#!/usr/bin/env python3
"""Fail when a build still mentions a provider or feature that its build flags disabled.

Scans the printable strings of the native executables, packaged resource files, file names and
UI dist/** for each disabled flag's identifiers. Matches are excused only by a reviewed entry in
scripts/build_flag_leak_allowlist.json (each entry carries a justification). Native executables are
excused only string by string: scripts/build_flag_native_residue.json lists, per executable and flag,
the exact text around each reviewed match and how often it occurs, so removing one match never makes
room for a new one.

  check_build_flag_leaks.py --build-dir Builds/flags-off            # flags read from CMakeCache.txt
  check_build_flag_leaks.py --dist Builds/ui-off/dist --disabled CODEX SSH
  check_build_flag_leaks.py --build-dir Builds/flags-off --record-residue   # add new native matches for review
"""
import argparse
import fnmatch
import json
import re
import sys
from pathlib import Path, PurePosixPath, PureWindowsPath

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_ALLOWLIST = ROOT / 'scripts' / 'build_flag_leak_allowlist.json'
DEFAULT_RESIDUE = ROOT / 'scripts' / 'build_flag_native_residue.json'
NATIVE_SCOPES = {'native', 'runner'}

# Short name -> (CMake option, identifiers). Identifiers are case-insensitive regular expressions
# covering names, ids such as codex-cli, logo file names, action names and settings keys.
FLAGS = {
    'GEMINI': ('UAM_ENABLE_RUNTIME_GEMINI_CLI', [r'gemini']),
    'CODEX': ('UAM_ENABLE_RUNTIME_CODEX_CLI', [r'codex']),
    'CLAUDE': ('UAM_ENABLE_RUNTIME_CLAUDE_CLI', [r'claude', r'anthropic']),
    'OPENCODE': ('UAM_ENABLE_RUNTIME_OPENCODE_CLI', [r'open-?code']),
    'COPILOT': ('UAM_ENABLE_RUNTIME_COPILOT_CLI', [r'copilot']),
    'ANTIGRAVITY': ('UAM_ENABLE_RUNTIME_ANTIGRAVITY_CLI', [r'antigravity', r'\bagy\b']),
    'SSH': ('UAM_ENABLE_SSH', [r'\bssh\b', r'remote[ _-]?host', r'uam-runner', r'execution[ _-]?routing']),
    'COMPUTER_USE': ('UAM_ENABLE_COMPUTER_USE', [r'computer[ _-]?use', r'softwarecursor']),
    'MOBILE_COMPANION': ('UAM_ENABLE_MOBILE_COMPANION', [r'companion', r'phone access']),
}
OPTION_TO_FLAG = {option: name for name, (option, _) in FLAGS.items()}
FALSE_VALUES = {'0', 'OFF', 'NO', 'FALSE', 'N', 'IGNORE', 'NOTFOUND', ''}
TEXT_SUFFIXES = {'.js', '.mjs', '.css', '.html', '.json', '.webmanifest', '.svg', '.txt', '.toml', '.md', '.uam', '.map', '.plist', '.sha256', '.version'}
# Third-party runtime files that UAM does not author; scanning them would only find Chromium text.
SKIP_NAMES = {'Chromium Embedded Framework.framework', 'locales', 'swiftshader', 'UI-V2'}
# Packaged copies of binaries that are scanned once in their own scope.
SKIP_FILES = {'uam-runner', 'uam-runner.exe', 'universal_agent_manager.exe'}
SKIP_SUFFIXES = {'.pak', '.dat', '.bin', '.dll', '.icns', '.png', '.woff', '.woff2', '.ttf', '.bmp', '.ico', '.lib'}
# A pattern entry is tested against the token around a match (up to whitespace, quotes, commas, semicolons or
# brackets), so text from a neighbouring statement in a minified bundle cannot excuse it.
TOKEN_BREAKS = set(' \t\r\n"\'`,;()[]')
PRINTABLE = re.compile(rb'[\x20-\x7e]{4,}')
# Windows wide string literals (L"...") are UTF-16LE: printable characters each followed by a zero byte.
PRINTABLE_UTF16 = re.compile(rb'(?:[\x20-\x7e]\x00){4,}')


def cmake_bool(value):
    value = value.strip().upper()
    return not (value in FALSE_VALUES or value.endswith('-NOTFOUND'))


def flag_name(value):
    value = value.strip().upper()
    if value in FLAGS:
        return value
    if value in OPTION_TO_FLAG:
        return OPTION_TO_FLAG[value]
    raise SystemExit(f'Unknown build flag: {value}')


def read_cache(build_dir):
    values = {}
    cache = build_dir / 'CMakeCache.txt'
    for line in cache.read_text(errors='replace').splitlines():
        match = re.match(r'^(UAM_[A-Z_]+):[A-Z]+=(.*)$', line)
        if match:
            values[match.group(1)] = match.group(2)
    return values


def file_strings(path):
    data = path.read_bytes()
    if path.suffix.lower() in TEXT_SUFFIXES:
        return data.decode('utf-8', errors='replace').splitlines()
    return [chunk.decode('ascii') for chunk in PRINTABLE.findall(data)] + \
        [chunk.decode('utf-16-le') for chunk in PRINTABLE_UTF16.findall(data)]


def walk(root):
    for path in sorted(root.rglob('*')):
        relative = path.relative_to(root)
        if any(part in SKIP_NAMES for part in relative.parts):
            continue
        yield path, relative.as_posix()


def collect(artifacts):
    """Yield (scope, artifact path, extracted string) for every scanned file and file name."""
    for scope, root in artifacts:
        if root.is_file():
            for text in file_strings(root):
                yield scope, root.name, text
            continue
        for path, relative in walk(root):
            yield scope, relative, relative
            if path.is_file() and path.suffix.lower() not in SKIP_SUFFIXES and path.name not in SKIP_FILES:
                for text in file_strings(path):
                    yield scope, relative, text


def local_path_pattern(folders):
    """Source and build folders embedded by __FILE__ belong to the machine, not the product: a build folder named
    after a provider must not count as a leak, and recorded residue must not depend on where it was built."""
    spellings = set()
    for folder in folders:
        for form in {str(folder), folder.as_posix()}:
            spellings.update({form, form.replace('\\', '/'), form.replace('/', '\\')})
    spellings = sorted((form for form in spellings if len(form) > 3), key=len, reverse=True)
    return re.compile('|'.join(map(re.escape, spellings)), re.IGNORECASE) if spellings else None


def find_leaks(disabled, artifacts, local_folders=(), foreign_folders=()):
    patterns = {name: re.compile('|'.join(FLAGS[name][1]), re.IGNORECASE) for name in disabled}
    local = local_path_pattern([form for folder in local_folders for form in (Path(folder).absolute(), Path(folder).resolve())] +
                               [PureWindowsPath(folder) if '\\' in folder or ':' in folder else PurePosixPath(folder) for folder in foreign_folders])
    leaks = []
    any_flag = re.compile('|'.join(pattern.pattern for pattern in patterns.values()), re.IGNORECASE) if patterns else None
    for scope, path, text in collect(artifacts):
        if any_flag is None or not any_flag.search(text):
            continue
        if local is not None:
            text = local.sub('<local>', text)
        for name, pattern in patterns.items():
            for match in pattern.finditer(text):
                start = max(0, match.start() - 60)
                token_start, token_end = match.start(), match.end()
                while token_start > 0 and text[token_start - 1] not in TOKEN_BREAKS:
                    token_start -= 1
                while token_end < len(text) and text[token_end] not in TOKEN_BREAKS:
                    token_end += 1
                leaks.append({'flag': name, 'scope': scope, 'path': path, 'match': match.group(0), 'token': text[token_start:token_end],
                              'text': text[start:match.end() + 60]})
    return leaks


def entry_flags(entry):
    flags = entry['flag'] if isinstance(entry['flag'], list) else [entry['flag']]
    return {flag_name(flag) for flag in flags}


def apply_allowlist(leaks, entries, disabled):
    """Return (failures, unused budget notes). An entry excuses a match by artifact path glob ('path'), a
    case-insensitive regex over the token around the match ('pattern') and/or a case-sensitive full match of the
    matched identifier ('match'); 'max' caps how many matches it may excuse."""
    used = [0] * len(entries)
    failures = []
    for leak in leaks:
        for index, entry in enumerate(entries):
            if leak['flag'] not in entry_flags(entry) or entry.get('scope', leak['scope']) != leak['scope']:
                continue
            if 'path' in entry and not fnmatch.fnmatch(leak['path'], entry['path']):
                continue
            if 'pattern' in entry and not re.search(entry['pattern'], leak['token'], re.IGNORECASE):
                continue
            if 'match' in entry and not re.fullmatch(entry['match'], leak['match']):
                continue
            if 'max' in entry and used[index] >= entry['max']:
                continue
            used[index] += 1
            break
        else:
            failures.append(leak)
    notes = [f"{entry['flag']} {entry.get('scope', '*')}: budget {entry['max']} but only {used[index]} used; lower it."
             for index, entry in enumerate(entries) if 'max' in entry and used[index] < entry['max'] and entry_flags(entry) & set(disabled)]
    return failures, notes


def native_artifacts(build_dir, values):
    found = []
    bundle = build_dir / 'universal_agent_manager.app'
    if bundle.is_dir():
        found.append(('native', bundle / 'Contents' / 'MacOS' / 'universal_agent_manager'))
        found.append(('resources', bundle / 'Contents' / 'Resources'))  # UI-V2 inside is skipped; dist is scanned as 'ui'
        found.append(('resources', bundle / 'Contents' / 'Info.plist'))
        frameworks = bundle / 'Contents' / 'Frameworks'
        if frameworks.is_dir():
            # Nested apps (CEF helpers, UAM Computer Use) run copies of the main executable, which is scanned once as 'native'.
            for app in frameworks.iterdir():
                if app.name in SKIP_NAMES or app.name.startswith('universal_agent_manager Helper'):
                    continue
                for part in (app / 'Contents' / 'Resources', app / 'Contents' / 'Info.plist'):
                    if part.exists():
                        found.append(('resources', part))
    for candidate in (build_dir / 'Release' / 'universal_agent_manager.exe', build_dir / 'universal_agent_manager.exe'):
        if candidate.is_file():
            found.append(('native', candidate))
            # The executable's folder is the Windows package: scan it as one resource tree (paths such as
            # markdown-store/... match their allowlist globs) without rescanning the executables.
            found.append(('resources', candidate.parent))
    for runner in (build_dir / 'uam-runner', build_dir / 'Release' / 'uam-runner.exe', build_dir / 'Release' / 'uam-runner'):
        if runner.is_file() and cmake_bool(values.get('UAM_ENABLE_SSH', 'ON')):
            found.append(('runner', runner))
    return found


def apply_residue(leaks, residue):
    """Excuse native matches whose exact surrounding text is recorded for that executable and flag, at most as often
    as recorded. Returns the remaining matches."""
    remaining = {}
    left = []
    for leak in leaks:
        if leak['scope'] not in NATIVE_SCOPES:
            left.append(leak)
            continue
        key = (leak['path'], leak['flag'], leak['text'])
        if key not in remaining:
            remaining[key] = residue.get('artifacts', {}).get(leak['path'], {}).get(leak['flag'], {}).get(leak['text'], 0)
        if remaining[key] > 0:
            remaining[key] -= 1
        else:
            left.append(leak)
    return left


def residue_failures(residue):
    reasons = residue.get('reasons', {})
    flags = {flag for per_flag in residue.get('artifacts', {}).values() for flag in per_flag}
    return [f'Native residue for {flag} has no reviewed reason in {DEFAULT_RESIDUE.name}.'
            for flag in sorted(flags) if not reasons.get(flag, '').strip() or reasons[flag].startswith('TODO')]


def record_residue(path, residue, leaks):
    """Merge unexcused native matches into the residue file; their flags need a reviewed reason before the gate passes."""
    counts = {}
    for leak in leaks:
        if leak['scope'] in NATIVE_SCOPES:
            key = (leak['path'], leak['flag'], leak['text'])
            counts[key] = counts.get(key, 0) + 1
    artifacts = residue.setdefault('artifacts', {})
    for (artifact, flag, text), count in counts.items():
        strings = artifacts.setdefault(artifact, {}).setdefault(flag, {})
        strings[text] = max(strings.get(text, 0), count)
        residue.setdefault('reasons', {}).setdefault(flag, 'TODO: review these strings and explain why they remain.')
    for per_flag in artifacts.values():
        for flag, strings in per_flag.items():
            per_flag[flag] = dict(sorted(strings.items()))
    path.write_text(json.dumps(residue, indent=2, ensure_ascii=False) + '\n')
    print(f'Recorded {sum(counts.values())} native matches ({len(counts)} distinct) in {path}.')


def structural_failures(build_dir, values):
    """Packaging rules that are about folders rather than strings."""
    failures = []
    for remote in (build_dir / 'universal_agent_manager.app/Contents/Resources/remote', build_dir / 'Release/remote'):
        if not remote.exists():
            continue
        if not cmake_bool(values.get('UAM_ENABLE_SSH', 'ON')):
            failures.append(f'{remote} exists although SSH is disabled.')
        elif not cmake_bool(values.get('UAM_PACKAGE_REMOTE_RUNNERS', 'ON')):
            stale = sorted(path.name for path in remote.iterdir() if path.is_dir())
            if stale:
                failures.append(f'{remote} keeps cross-platform helper folders although UAM_PACKAGE_REMOTE_RUNNERS is OFF: {", ".join(stale)}')
    return failures


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--build-dir', type=Path, help='CMake build folder; flags come from its CMakeCache.txt')
    parser.add_argument('--dist', type=Path, help='UI dist folder (defaults to <build-dir>/UI-V2/dist)')
    parser.add_argument('--disabled', nargs='*', default=None, help='Disabled flags, short (CODEX) or CMake (UAM_ENABLE_SSH) names')
    parser.add_argument('--allowlist', type=Path, default=DEFAULT_ALLOWLIST)
    parser.add_argument('--residue', type=Path, default=DEFAULT_RESIDUE, help='Reviewed exact native strings per executable')
    parser.add_argument('--record-residue', action='store_true', help='Add unexcused native matches to the residue file for review')
    parser.add_argument('--local-path', action='append', default=[], help='Extra machine-local folder whose spelling in __FILE__ paths is ignored (for outputs copied from another machine)')
    parser.add_argument('--ui-only', action='store_true', help='Scan only the UI dist even when a build folder is given')
    args = parser.parse_args(argv)
    if not args.build_dir and not args.dist:
        parser.error('Pass --build-dir or --dist.')

    values = read_cache(args.build_dir) if args.build_dir else {}
    if args.disabled is not None:
        disabled = sorted({flag_name(value) for value in args.disabled})
    else:
        disabled = sorted(name for name, (option, _) in FLAGS.items() if not cmake_bool(values.get(option, 'ON')))
    artifacts = []
    dist = args.dist or (args.build_dir / 'UI-V2' / 'dist')
    if dist.is_dir():
        artifacts.append(('ui', dist))
    elif args.dist:
        parser.error(f'{dist} is not a directory.')
    failures = []
    if args.build_dir and not args.ui_only:
        native = native_artifacts(args.build_dir, values)
        expected = 'runner' if cmake_bool(values.get('UAM_RUNNER_ONLY', 'OFF')) else 'native'
        if not any(scope == expected for scope, _ in native):
            failures.append(f'No {"runner" if expected == "runner" else "application"} executable found in {args.build_dir}.')
        if expected == 'native':
            if cmake_bool(values.get('UAM_ENABLE_SSH', 'ON')) and not any(scope == 'runner' for scope, _ in native):
                failures.append(f'SSH is on but no runner executable was found in {args.build_dir}.')
            if not dist.is_dir():
                failures.append(f'No interface bundle found at {dist}.')
        artifacts.extend(native)
        failures.extend(structural_failures(args.build_dir, values))
    if not artifacts and not failures:
        parser.error('No artifacts to scan.')
    if not disabled:
        # Packaging-only profiles (UAM_PACKAGE_REMOTE_RUNNERS=OFF, or UAM_RUNNER_ONLY=ON with every provider on) still
        # need the expected executable and folder rules; there are no names to search for.
        for failure in failures:
            print(f'FAIL: {failure}')
        if failures:
            print(f'Build flag leak gate failed: {len(failures)} packaging failures.')
            return 1
        print('No provider or feature flag is disabled; packaging checks passed.')
        return 0

    entries = json.loads(args.allowlist.read_text()).get('entries', []) if args.allowlist.exists() else []
    for entry in entries:
        if not entry.get('reason', '').strip():
            failures.append(f'Allowlist entry without a justification: {entry}')
        if 'max' not in entry and 'path' not in entry:
            failures.append(f'Allowlist entry without a budget: {entry}')
    residue = json.loads(args.residue.read_text()) if args.residue.exists() else {}
    leaks = find_leaks(disabled, artifacts, [ROOT] + ([args.build_dir] if args.build_dir else []), args.local_path)
    leak_failures, notes = apply_allowlist(apply_residue(leaks, residue), entries, disabled)
    if args.record_residue:
        record_residue(args.residue, residue, leak_failures)
        leak_failures = [leak for leak in leak_failures if leak['scope'] not in NATIVE_SCOPES]
    failures.extend(residue_failures(residue))

    print(f"Disabled: {', '.join(disabled)}; scanned: {', '.join(f'{scope}:{path}' for scope, path in artifacts)}")
    summary = {}
    for leak in leaks:
        key = (leak['flag'], leak['scope'])
        summary[key] = summary.get(key, 0) + 1
    for (name, scope), count in sorted(summary.items()):
        unexcused = sum(1 for leak in leak_failures if leak['flag'] == name and leak['scope'] == scope)
        print(f'  {name:<16} {scope:<9} matches={count:<5} unexcused={unexcused}')
    for note in notes:
        print(f'note: {note}')
    for failure in failures:
        print(f'FAIL: {failure}')
    for leak in leak_failures[:200]:
        print(f"LEAK {leak['flag']} {leak['scope']} {leak['path']}: {leak['text']!r}")
    if len(leak_failures) > 200:
        print(f'... {len(leak_failures) - 200} more unexcused matches')
    if failures or leak_failures:
        print(f'Build flag leak gate failed: {len(leak_failures)} unexcused matches, {len(failures)} packaging failures.')
        return 1
    print('Build flag leak gate passed.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
