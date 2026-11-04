#!/usr/bin/env python3
"""Remove stale macOS code_sign_clone copies of UAM app bundles.

Launching a signed bundle makes macOS keep a copy under the per-user
`X/<bundle-id>.code_sign_clone/code_sign_clone.*` folder. Copies of killed or
rebuilt UAM bundles are never removed and keep about 0.5 GB each. Only
`com.universalagentmanager.*` clone folders are considered. A clone whose
executable is still mapped by a running process, or still hard-linked to an
existing original bundle, is kept.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

BUNDLE_PREFIX = 'com.universalagentmanager.'


def default_root():
    """Per-user clone root, or None when it cannot be resolved safely."""
    try:
        temp = subprocess.run(['getconf', 'DARWIN_USER_TEMP_DIR'], capture_output=True, text=True, check=True).stdout.strip()
    except (subprocess.CalledProcessError, OSError):
        return None
    return Path(temp).parent / 'X' if os.path.isabs(temp) else None


def open_inodes():
    # Clone executables are hard links, so lsof may name either path: match inodes.
    result = subprocess.run(['lsof', '-n', '-F', 'i'], capture_output=True, text=True)
    if result.returncode not in (0, 1) or not result.stdout:
        raise RuntimeError('lsof failed; refusing to remove clones that may be in use')
    return {int(line[1:]) for line in result.stdout.splitlines() if line.startswith('i') and line[1:].isdigit()}


def executables(clone):
    return [path.stat() for path in clone.glob('*/Contents/MacOS/*') if path.is_file() and not path.is_symlink()]


def tree_bytes(path):
    total = 0
    for directory, _, files in os.walk(path):
        for name in files:
            try:
                total += os.lstat(os.path.join(directory, name)).st_size
            except OSError:
                pass
    return total


def prune(root=None, in_use=None, dry_run=False, list_open=open_inodes):
    """Return (removed clone paths, bytes) for stale UAM clones under root."""
    if root is None and sys.platform != 'darwin':
        return [], 0
    root = Path(root) if root else default_root()
    if root is None or not root.is_dir():
        return [], 0
    candidates = [clone for owner in sorted(root.glob(BUNDLE_PREFIX + '*.code_sign_clone'))
                  if owner.is_dir() and not owner.is_symlink()
                  for clone in sorted(owner.glob('code_sign_clone.*'))
                  if clone.is_dir() and not clone.is_symlink() and clone.lstat().st_uid == os.getuid()]
    if not candidates:
        return [], 0
    if in_use is None:
        try:
            in_use = list_open()
        except (RuntimeError, OSError) as error:
            print(f'Kept all code_sign_clone folders: {error}', file=sys.stderr)
            return [], 0
    removed, freed = [], 0
    for clone in candidates:
        try:
            stats = executables(clone)
            # A launch still starting has nothing open yet, and helpers may outlive the main
            # process; while the original bundle exists the clone is a hard link costing no space.
            if any(stat.st_nlink > 1 or stat.st_ino in in_use for stat in stats):
                continue
            size = tree_bytes(clone)
            if not dry_run:
                shutil.rmtree(clone)
        except OSError as error:
            print(f'Could not remove {clone}: {error}', file=sys.stderr)
            continue
        freed += size
        removed.append(clone)
    return removed, freed


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--root', type=Path, help='Clone root; defaults to the per-user X folder beside DARWIN_USER_TEMP_DIR')
    parser.add_argument('--dry-run', action='store_true')
    args = parser.parse_args()
    removed, freed = prune(args.root, dry_run=args.dry_run)
    verb = 'Would remove' if args.dry_run else 'Removed'
    print(f'{verb} {len(removed)} stale UAM code_sign_clone folder(s), {freed / 1e6:.0f} MB.')


if __name__ == '__main__':
    main()
