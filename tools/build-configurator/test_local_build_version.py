from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import subprocess
import tempfile
import unittest

ALLOCATOR = Path(__file__).resolve().parents[2] / 'cmake/local_build_version.cmake'


class LocalBuildVersionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.source = Path(self.temp.name)
        self.state = self.source / 'Builds/local-versions'
        self.release('4.9.0')

    def release(self, version):
        (self.source / 'CMakeLists.txt').write_text(f'set(UAM_PRODUCT_VERSION "{version}")\n')

    def allocate(self, minimum=None, source=None, shared=False):
        args = ['cmake', f'-DUAM_SOURCE_DIR={source or self.source}']
        if not shared:
            args.append(f'-DUAM_LOCAL_VERSION_STATE_DIR={self.state}')
        if minimum is not None:
            args.append(f'-DUAM_LOCAL_VERSION_MINIMUM={minimum}')
        return subprocess.run([*args, '-P', str(ALLOCATOR)], capture_output=True, text=True)

    def test_monotonic_historical_floor_and_independent_release(self):
        self.assertEqual(self.allocate().stdout.strip(), '4.9.0-alpha-18')
        self.assertEqual(self.allocate(0).stdout.strip(), '4.9.0-alpha-19')
        self.release('5.0.0')
        self.assertEqual(self.allocate().stdout.strip(), '5.0.0-alpha-1')
        self.assertEqual((self.state / '4.9.0.counter').read_text(), '19\n')

    def test_concurrent_reservations_never_reuse_a_number(self):
        with ThreadPoolExecutor(max_workers=4) as executor:
            results = list(executor.map(lambda _: self.allocate(0), range(8)))
        self.assertTrue(all(result.returncode == 0 for result in results))
        self.assertEqual(sorted(int(result.stdout.strip().rsplit('-', 1)[1]) for result in results), list(range(1, 9)))
        self.assertEqual((self.state / '4.9.0.counter').read_text(), '8\n')

    def test_corrupt_or_exhausted_state_is_preserved_and_rejected(self):
        self.state.mkdir(parents=True)
        counter = self.state / '4.9.0.counter'
        for invalid in ['', 'not-a-number\n', '018\n', '-1\n', '2147483647\n', '9' * 33]:
            with self.subTest(invalid=invalid):
                counter.write_text(invalid)
                result = self.allocate()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('Invalid local version counter', result.stderr)
                self.assertEqual(counter.read_text(), invalid)
                self.assertFalse(counter.with_suffix('.counter.tmp').exists())

    def test_invalid_minimum_does_not_create_counter(self):
        for minimum in ['-1', '018', 'invalid', '2147483647']:
            self.assertNotEqual(self.allocate(minimum).returncode, 0)
        self.assertFalse((self.state / '4.9.0.counter').exists())

    def test_linked_worktree_and_main_checkout_share_one_counter(self):
        subprocess.run(['git', 'init', '-q', str(self.source)], check=True)
        linked = self.source / 'linked'
        linked.mkdir()
        (linked / 'CMakeLists.txt').write_text('set(UAM_PRODUCT_VERSION "4.9.0")\n')
        metadata = self.source / '.git/worktrees/linked'
        metadata.mkdir(parents=True)
        (linked / '.git').write_text(f'gitdir: {metadata}\n')
        (metadata / 'commondir').write_text('../..\n')
        (metadata / 'gitdir').write_text(f'{linked / ".git"}\n')
        (metadata / 'HEAD').write_text('ref: refs/heads/main\n')
        self.assertEqual(self.allocate(source=self.source, shared=True).stdout.strip(), '4.9.0-alpha-18')
        self.assertEqual(self.allocate(source=linked, shared=True).stdout.strip(), '4.9.0-alpha-19')
        self.assertEqual((self.state / '4.9.0.counter').read_text(), '19\n')
        self.assertFalse((linked / 'Builds').exists())

    def test_presentation_override_accepts_only_matching_release_alpha(self):
        script = self.source / 'presentation.cmake'
        for version, expected in [('', '4.9.0'), ('4.9.0-alpha-18', '4.9.0-alpha-18'), ('4.10.0-alpha-18', None), ('4.9.0-alpha-0', None), ('4.9.0-alpha-018', None), ('4.9.0', None)]:
            with self.subTest(version=version):
                script.write_text(f'include("{ALLOCATOR.as_posix()}")\nuam_local_build_presentation("4.9.0" "{version}" presentation)\nfile(WRITE "{(self.source / "presentation.txt").as_posix()}" "${{presentation}}")\n')
                result = subprocess.run(['cmake', '-P', str(script)], capture_output=True, text=True)
                if expected is None:
                    self.assertNotEqual(result.returncode, 0)
                else:
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual((self.source / 'presentation.txt').read_text(), expected)


if __name__ == '__main__':
    unittest.main()
