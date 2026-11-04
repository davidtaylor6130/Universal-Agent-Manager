"""Choose which pull-request CI jobs and native core cases a change needs.

classify: prints GitHub output lines `native=` (false only for documentation-only changes) and `windows=`
(true when a change can behave differently on Windows). select: prints the native cases whose test files
reach a changed source through project includes (a header also reaches its same-named sources), `*` when the
change touches the harness's precompiled include set or cannot be mapped and every case must run, or nothing
when no native case is affected. The full suite still runs on every push to the base branch.
"""
import argparse
import os
from pathlib import Path
import posixpath
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
# Shared build, harness and runner inputs: any change here runs every case on every platform.
FULL = ('CMakeLists.txt', 'cmake/', 'tests/test_harness.h', 'tests/core_tests.cpp', 'tests/ci_serial.txt',
        'tests/ci_quarantine.txt', 'scripts/run_native_tests.py', 'scripts/select_native_tests.py',
        '.github/workflows/ci.yml')
# Only prose: some native cases read scripts kept under docs/.
DOCS = re.compile(r'(\.md$|^LICENSE$|^docs/.*\.mmd$)')
WINDOWS_PATH = re.compile(r'(windows|win32|^src/common/platform/|\.(ps1|bat|rc|manifest|ico)$)', re.I)
# Python checks that CTest also runs on Windows.
WINDOWS_SCRIPT = re.compile(r'(win32|os\.name|\.exe\b)', re.I)
# Native cases also read these at run time without an include edge.
DATA = ('markdown-store/', 'assets/')
SOURCE_LITERAL = re.compile(r'"((?:src|tests)/[^"\s]+)"')
# The app targets only macOS and Windows, so the #else beside an __APPLE__ test is Windows code too.
WINDOWS_CODE = re.compile(r'\b(_WIN32|_MSC_VER|WIN32|__APPLE__)\b')
SOURCE = ('.h', '.hpp', '.inl', '.cpp', '.mm')
INCLUDE = re.compile(r'^\s*#\s*include\s*"([^"]+)"', re.M)
CASE = re.compile(r'^\s*UAM_TEST\((\w+)\)', re.M)
PLATFORM_SUFFIX = re.compile(r'_(windows|macos|posix|linux)(_impl)?$')
HARNESS = 'tests/test_harness.h'


def read(path):
    try:
        return (ROOT / path).read_text(errors='replace')
    except OSError:
        return None


def is_full(path):
    return path.endswith(('CMakeLists.txt', '.cmake')) or \
        any(path == item or (item.endswith('/') and path.startswith(item)) for item in FULL)


def read_base(path, base):
    if not base:
        return ''
    result = subprocess.run(['git', 'show', f'{base}:{path}'], cwd=ROOT, capture_output=True, text=True, errors='replace')
    return result.stdout if result.returncode == 0 else ''


def classify(changed, base=None):
    native = not changed or not all(DOCS.search(path) for path in changed)
    # Both sides count, so removing a platform block is platform-relevant too.
    windows = any(is_full(path) or WINDOWS_PATH.search(path) or
                  (path.endswith(SOURCE) and (read(path) is None or WINDOWS_CODE.search(read(path) + read_base(path, base)))) or
                  (path.endswith('.py') and WINDOWS_SCRIPT.search((read(path) or '') + read_base(path, base)))
                  for path in changed)
    return native, windows


def select(changed, inventory, files):
    """Return the selected case names, or None when every case must run."""
    if any(is_full(path) for path in changed):
        return None
    sources = {path for path in files if path.startswith(('src/', 'tests/')) and path.endswith(SOURCE)}
    graph, cases = {}, {}
    for path in sorted(sources):
        text = read(path) or ''
        edges = set()
        for name in INCLUDE.findall(text):
            for base in (posixpath.dirname(path), 'src', 'src/common', 'tests'):
                candidate = posixpath.normpath(posixpath.join(base, name))
                if candidate in sources:
                    edges.add(candidate)
                    break
        if path.startswith('tests/'):
            # Source-scan cases read files by path rather than including them.
            edges.update(literal for literal in SOURCE_LITERAL.findall(text) if literal in sources)
        graph[path] = edges
        if path.startswith('tests/'):
            cases[path] = CASE.findall(text)
    for path in sources:
        # A header reaches its implementation, so a test also covers code its included headers call into.
        if path.endswith(('.cpp', '.mm')) and not cases.get(path):
            header = f'{posixpath.dirname(path)}/{PLATFORM_SUFFIX.sub("", Path(path).stem)}.h'
            if header in graph:
                graph[header].add(path)
    # Every core test sees the harness's precompiled include set and the code it reaches, so a change there runs everything.
    precompiled, stack = set(), [HARNESS]
    while stack:
        for child in graph.get(stack.pop(), set()) - precompiled:
            precompiled.add(child)
            stack.append(child)
    for path in sources:
        graph[path].discard(HARNESS)
    reach = {}
    for test in (path for path, names in cases.items() if names):
        seen, stack = {test}, [test]
        while stack:
            for child in graph[stack.pop()] - seen:
                seen.add(child)
                stack.append(child)
        reach[test] = seen
    chosen = set()
    for path in changed:
        if path.startswith(DATA):
            return None
        if not path.startswith(('src/', 'tests/')) or path.endswith('.py'):
            continue
        if not path.endswith(SOURCE):
            # Manifests, resources and fixtures can change native behaviour without an include edge.
            return None
        if cases.get(path):
            chosen.update(cases[path])
        header = f'{posixpath.dirname(path)}/{PLATFORM_SUFFIX.sub("", Path(path).stem)}.h'
        if path in precompiled or header in precompiled:
            return None
        tests = [test for test, seen in reach.items() if path in seen]
        if not cases.get(path) and not tests:
            return None
        for test in tests:
            chosen.update(cases[test])
    known = {name for names in cases.values() for name in names}
    selected = [name for name in inventory if name in chosen or name not in known]
    return None if len(selected) == len(inventory) else selected


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=['classify', 'select'])
    parser.add_argument('--changed', type=Path, required=True, help='File with one changed path per line.')
    parser.add_argument('--inventory', type=Path, help='Output of uam_core_tests --list-tests (select mode).')
    parser.add_argument('--base', help='Revision before the change, so removed platform code also counts (classify mode).')
    args = parser.parse_args()
    changed = [line.strip() for line in args.changed.read_text().splitlines() if line.strip()]
    if args.mode == 'classify':
        native, windows = classify(changed, args.base)
        print(f'native={str(native).lower()}\nwindows={str(windows).lower()}')
        return 0
    if not args.inventory:
        parser.error('select needs --inventory')
    inventory = [line.strip() for line in args.inventory.read_text().splitlines() if line.strip()]
    files = [str(path.relative_to(ROOT)).replace(os.sep, '/') for folder in ('src', 'tests')
             for path in (ROOT / folder).rglob('*') if path.is_file()]
    selected = select(changed, inventory, files)
    # Plain newlines on Windows too, so the workflow can compare the output with '*'.
    sys.stdout.reconfigure(newline='\n')
    if selected is None or selected:
        print('*' if selected is None else '\n'.join(selected))
    print(f'Selected {"all" if selected is None else len(selected)} of {len(inventory)} native cases.', file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main())
