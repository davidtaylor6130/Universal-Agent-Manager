"""Stale UAM code_sign_clone folders are removed; in-use, linked and foreign ones survive (#684)."""
from pathlib import Path
import os
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import prune_code_sign_clones  # noqa: E402


def make_clone(root, owner, name):
    executable = root / owner / name / 'universal_agent_manager.app.bundle/Contents/MacOS/universal_agent_manager'
    executable.parent.mkdir(parents=True)
    executable.write_bytes(b'x' * 4096)
    return executable


def failing_lsof():
    raise RuntimeError('lsof unavailable')


with tempfile.TemporaryDirectory(prefix='uam-clone-prune-') as directory:
    base = Path(directory)
    root = base / 'X'
    uam = 'com.universalagentmanager.desktop.code_sign_clone'
    stale = make_clone(root, uam, 'code_sign_clone.stale1')
    guitest = make_clone(root, 'com.universalagentmanager.desktop.guitest.code_sign_clone', 'code_sign_clone.stale2')
    running = make_clone(root, uam, 'code_sign_clone.running')
    # A launch still starting has nothing open; its original bundle still links the executable.
    starting = make_clone(root, uam, 'code_sign_clone.starting')
    os.link(starting, base / 'original_bundle_executable')
    foreign = make_clone(root, 'com.example.other.code_sign_clone', 'code_sign_clone.keep')
    unrelated = root / uam / 'not_a_clone'
    unrelated.mkdir()
    outside = make_clone(base, 'outside', 'code_sign_clone.target')
    os.symlink(base / 'outside', root / 'com.universalagentmanager.link.code_sign_clone')
    os.symlink(outside.parents[3], root / uam / 'code_sign_clone.symlinked')

    assert prune_code_sign_clones.prune(root, list_open=failing_lsof) == ([], 0)
    assert stale.exists() and guitest.exists(), 'clones removed although lsof failed'

    with running.open('rb'):  # A running app maps its clone executable.
        removed, freed = prune_code_sign_clones.prune(root, dry_run=True)
        assert {path.name for path in removed} == {'code_sign_clone.stale1', 'code_sign_clone.stale2'}, removed
        assert freed == 2 * 4096 and stale.exists(), freed
        removed, freed = prune_code_sign_clones.prune(root)

    assert not stale.parents[3].exists() and not guitest.parents[3].exists(), 'stale UAM clones remained'
    assert running.exists(), 'in-use clone was removed'
    assert starting.exists(), 'clone still linked to its original bundle was removed'
    assert foreign.exists() and unrelated.is_dir() and outside.exists(), 'non-UAM, non-clone or symlinked path was removed'
    assert (root / uam / 'code_sign_clone.symlinked').is_symlink()
    removed, _ = prune_code_sign_clones.prune(root, in_use=set())
    assert [path.name for path in removed] == ['code_sign_clone.running'] and not running.exists(), 'released clone was kept'
    (base / 'original_bundle_executable').unlink()
    removed, _ = prune_code_sign_clones.prune(root, in_use=set())
    assert [path.name for path in removed] == ['code_sign_clone.starting'], 'clone of a deleted bundle was kept'
    assert prune_code_sign_clones.prune(base / 'missing') == ([], 0)

platform = sys.platform
def unexpected_root():
    raise AssertionError('non-macOS default run resolved a clone root')
prune_code_sign_clones.default_root, sys.platform = unexpected_root, 'linux'
try:
    assert prune_code_sign_clones.prune() == ([], 0)
finally:
    sys.platform = platform
print('PASS: stale UAM clones removed; in-use, linked, foreign, non-clone and symlinked paths kept; lsof failure and non-macOS remove nothing')
