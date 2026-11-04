import importlib.util
from pathlib import Path
import subprocess
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('select_native_tests', ROOT / 'scripts/select_native_tests.py')
selector = importlib.util.module_from_spec(spec)
spec.loader.exec_module(selector)
FILES = subprocess.run(['git', 'ls-files', 'src', 'tests'], cwd=ROOT, capture_output=True, text=True, check=True).stdout.split()
CASES = {path: selector.CASE.findall((ROOT / path).read_text(errors='replace'))
         for path in FILES if path.startswith('tests/') and path.endswith(selector.SOURCE)}
INVENTORY = [name for names in CASES.values() for name in names]


class SelectNativeTests(unittest.TestCase):
    def test_classify(self):
        self.assertEqual(selector.classify(['docs/building.md', 'README.md']), (False, False))
        self.assertEqual(selector.classify(['UI-V2/src/App.tsx']), (True, False))
        # A native case reads this script, so it is not documentation-only.
        self.assertEqual(selector.classify(['docs/reference/history/run_performance_test.ps1'])[0], True)
        for path in ['tests/bootstrap_metadata/CMakeLists.txt', 'assets/app_icon.ico', 'tests/app_icon_resource_test.py']:
            self.assertEqual(selector.classify([path])[1], True, path)
        self.assertEqual(selector.classify([]), (True, False))
        for path in ['CMakeLists.txt', 'src/common/platform/platform_paths_windows.cpp', 'build.ps1', 'src/gone_windows.cpp']:
            self.assertEqual(selector.classify([path]), (True, True), path)

    def test_shared_inputs_and_unmapped_sources_run_everything(self):
        for path in ['CMakeLists.txt', 'tests/test_harness.h', 'scripts/run_native_tests.py', 'src/never/existed.h', 'src/uam_long_paths.manifest',
                     'markdown-store/bundled/example.uam']:
            self.assertIsNone(selector.select([path], INVENTORY, FILES), path)

    def test_changed_test_file_and_its_includers_are_selected(self):
        test = next(path for path, names in CASES.items() if path.endswith('.cpp') and names)
        selected = selector.select([test], INVENTORY, FILES)
        self.assertTrue(set(CASES[test]) <= set(selected))
        self.assertLess(len(selected), len(INVENTORY))

    def test_header_selects_only_reaching_test_files(self):
        header = 'src/app/chat_title_service.h'
        selected = set(selector.select([header], INVENTORY, FILES))
        including = [path for path in CASES if CASES[path] and '"app/chat_title_service.h"' in (ROOT / path).read_text(errors='replace')]
        self.assertTrue(including)
        for path in including:
            self.assertTrue(set(CASES[path]) <= selected, path)
        self.assertLess(len(selected), len(INVENTORY))

    def test_source_scan_cases_follow_quoted_paths(self):
        selected = selector.select(['src/cef/remote_host_install_guard.h'], INVENTORY, FILES)
        self.assertIn('RemoteHelperUpdateBlocksEveryRemoteCleanupState', selected)

    def test_precompiled_harness_includes_run_everything(self):
        # Every core test sees the harness's include set, so tests can use these without including them.
        for path in ['src/common/config/custom_icon.h', 'src/app/shell_action_service.cpp']:
            self.assertIsNone(selector.select([path], INVENTORY, FILES), path)

    def test_platform_code_on_either_side_selects_windows(self):
        with patch.object(selector, 'read', return_value='int shared();\n'):
            self.assertEqual(selector.classify(['src/app/shared.cpp']), (True, False))
            # Removing a Windows block is a Windows change.
            with patch.object(selector, 'read_base', return_value='#ifdef _WIN32\n#endif\n'):
                self.assertEqual(selector.classify(['src/app/shared.cpp'], 'HEAD^1'), (True, True))
        # The app builds only for macOS and Windows, so an __APPLE__ #else branch is Windows code.
        with patch.object(selector, 'read', return_value='#if defined(__APPLE__)\n#else\n#endif\n'):
            self.assertEqual(selector.classify(['src/app/shared.cpp']), (True, True))

    def test_non_native_changes_select_nothing_and_unknown_cases_always_run(self):
        self.assertEqual(selector.select(['UI-V2/src/App.tsx'], INVENTORY, FILES), [])
        self.assertEqual(selector.select(['UI-V2/src/App.tsx'], [*INVENTORY, 'GeneratedCase'], FILES), ['GeneratedCase'])


if __name__ == '__main__':
    unittest.main()
