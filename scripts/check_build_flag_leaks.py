#!/usr/bin/env python3
"""Fail when a build still mentions a provider or feature that its build flags disabled.

Scans the printable strings of the native executables, packaged resource files, file names and
UI dist/** for each disabled flag's identifiers. Matches are excused only by a reviewed entry in
scripts/build_flag_leak_allowlist.json (each entry carries a justification).

  check_build_flag_leaks.py --build-dir Builds/flags-off            # flags read from CMakeCache.txt
  check_build_flag_leaks.py --dist Builds/ui-off/dist --disabled CODEX SSH
"""
import argparse
import fnmatch
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_ALLOWLIST = ROOT / 'scripts' / 'build_flag_leak_allowlist.json'

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
SKIP_SUFFIXES = {'.pak', '.dat', '.bin', '.dll', '.icns', '.png', '.woff', '.woff2', '.ttf', '.bmp', '.ico', '.lib'}
PRINTABLE = re.compile(rb'[\x20-\x7e]{4,}')


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
    return [chunk.decode('ascii') for chunk in PRINTABLE.findall(data)]


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
            if path.is_file() and path.suffix.lower() not in SKIP_SUFFIXES:
                for text in file_strings(path):
                    yield scope, relative, text


def find_leaks(disabled, artifacts):
    patterns = {name: re.compile('|'.join(FLAGS[name][1]), re.IGNORECASE) for name in disabled}
    leaks = []
    for scope, path, text in collect(artifacts):
        for name, pattern in patterns.items():
            for match in pattern.finditer(text):
                start = max(0, match.start() - 60)
                leaks.append({'flag': name, 'scope': scope, 'path': path, 'match': match.group(0), 'text': text[start:match.end() + 60]})
    return leaks


def entry_flags(entry):
    flags = entry['flag'] if isinstance(entry['flag'], list) else [entry['flag']]
    return {flag_name(flag) for flag in flags}


def apply_allowlist(leaks, entries, disabled):
    """Return (failures, unused budget notes). An entry excuses a match by artifact path glob ('path'), a
    case-insensitive regex over the surrounding text ('pattern') and/or a case-sensitive full match of the
    matched identifier ('match'); 'max' caps how many matches it may excuse."""
    used = [0] * len(entries)
    failures = []
    for leak in leaks:
        for index, entry in enumerate(entries):
            if leak['flag'] not in entry_flags(entry) or entry.get('scope', leak['scope']) != leak['scope']:
                continue
            if 'path' in entry and not fnmatch.fnmatch(leak['path'], entry['path']):
                continue
            if 'pattern' in entry and not re.search(entry['pattern'], leak['text'], re.IGNORECASE):
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
            found.extend(('resources', path) for path in frameworks.iterdir() if path.name not in SKIP_NAMES and not path.name.startswith('universal_agent_manager Helper'))
    for candidate in (build_dir / 'Release' / 'universal_agent_manager.exe', build_dir / 'universal_agent_manager.exe'):
        if candidate.is_file():
            found.append(('native', candidate))
            found.extend(('resources', path) for path in candidate.parent.iterdir()
                         if path != candidate and path.name not in SKIP_NAMES and path.suffix.lower() not in SKIP_SUFFIXES and path.name != 'UI-V2')
    for runner in (build_dir / 'uam-runner', build_dir / 'Release' / 'uam-runner.exe', build_dir / 'Release' / 'uam-runner'):
        if runner.is_file() and cmake_bool(values.get('UAM_ENABLE_SSH', 'ON')):
            found.append(('runner', runner))
    return found


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
    parser.add_argument('--ui-only', action='store_true', help='Scan only the UI dist even when a build folder is given')
    args = parser.parse_args(argv)
    if not args.build_dir and not args.dist:
        parser.error('Pass --build-dir or --dist.')

    values = read_cache(args.build_dir) if args.build_dir else {}
    if args.disabled is not None:
        disabled = sorted({flag_name(value) for value in args.disabled})
    else:
        disabled = sorted(name for name, (option, _) in FLAGS.items() if not cmake_bool(values.get(option, 'ON')))
    if not disabled:
        print('No provider or feature flag is disabled; nothing to check.')
        return 0

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
        artifacts.extend(native)
        failures.extend(structural_failures(args.build_dir, values))
    if not artifacts:
        parser.error('No artifacts to scan.')

    entries = json.loads(args.allowlist.read_text()).get('entries', []) if args.allowlist.exists() else []
    for entry in entries:
        if not entry.get('reason', '').strip():
            failures.append(f'Allowlist entry without a justification: {entry}')
    leaks = find_leaks(disabled, artifacts)
    leak_failures, notes = apply_allowlist(leaks, entries, disabled)

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
