#!/usr/bin/env python3
"""Remove stale macOS code_sign_clone copies of UAM app bundles.

Launching a signed bundle makes macOS keep a copy under the per-user
`X/<bundle-id>.code_sign_clone/code_sign_clone.*` folder. Copies of killed or
rebuilt UAM bundles are never removed and keep about 0.5 GB each. Only
`com.universalagentmanager.*` clone folders are considered, and a clone whose
executable is still mapped by a running process is kept.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

BUNDLE_PREFIX = 'com.universalagentmanager.'


def default_root():
    temp = subprocess.run(['getconf', 'DARWIN_USER_TEMP_DIR'], capture_output=True, text=True, check=True).stdout.strip()
    return Path(temp).parent / 'X'


def open_inodes():
    # Clone executables are hard links, so lsof may name either path: match inodes.
    result = subprocess.run(['lsof', '-n', '-F', 'i'], capture_output=True, text=True)
    if result.returncode not in (0, 1) or not result.stdout:
        raise RuntimeError('lsof failed; refusing to remove clones that may be in use')
    return {int(line[1:]) for line in result.stdout.splitlines() if line.startswith('i') and line[1:].isdigit()}


def clone_inodes(clone):
    return {path.stat().st_ino for path in clone.glob('*/Contents/MacOS/*') if path.is_file() and not path.is_symlink()}


def tree_bytes(path):
    total = 0
    for directory, _, files in os.walk(path):
        for name in files:
            try:
                total += os.lstat(os.path.join(directory, name)).st_size
            except OSError:
                pass
    return total


def prune(root=None, in_use=None, dry_run=False):
    """Return (removed clone paths, bytes) for stale UAM clones under root."""
    root = Path(root) if root else default_root()
    if not root.is_dir():
        return [], 0
    candidates = [clone for owner in sorted(root.glob(BUNDLE_PREFIX + '*.code_sign_clone'))
                  if owner.is_dir() and not owner.is_symlink()
                  for clone in sorted(owner.glob('code_sign_clone.*'))
                  if clone.is_dir() and not clone.is_symlink() and clone.lstat().st_uid == os.getuid()]
    if not candidates:
        return [], 0
    in_use = open_inodes() if in_use is None else in_use
    removed, freed = [], 0
    for clone in candidates:
        if clone_inodes(clone) & in_use:
            continue
        freed += tree_bytes(clone)
        if not dry_run:
            shutil.rmtree(clone)
        removed.append(clone)
    return removed, freed


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--root', type=Path, help='Clone root; defaults to the per-user X folder beside DARWIN_USER_TEMP_DIR')
    parser.add_argument('--dry-run', action='store_true')
    args = parser.parse_args()
    if sys.platform != 'darwin' and not args.root:
        return
    removed, freed = prune(args.root, dry_run=args.dry_run)
    verb = 'Would remove' if args.dry_run else 'Removed'
    print(f'{verb} {len(removed)} stale UAM code_sign_clone folder(s), {freed / 1e6:.0f} MB.')


if __name__ == '__main__':
    main()
