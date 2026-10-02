import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('configurator', Path(__file__).with_name('configurator.py'))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class BuildConfiguratorTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.source = Path(self.temp.name)
        (self.source / 'CMakeLists.txt').touch()
        (self.source / 'UI-V2').mkdir()
        (self.source / 'UI-V2/package-lock.json').touch()

    def test_selected_configuration_builds_only_selected_providers_and_features(self):
        config = module.configuration({'source': str(self.source), 'providers': ['CODEX'], 'features': ['SSH']})
        prepare = module.commands(*config)[1][0]
        self.assertIn('-DUAM_ENABLE_RUNTIME_CODEX_CLI=ON', prepare)
        self.assertIn('-DUAM_ENABLE_RUNTIME_GEMINI_CLI=OFF', prepare)
        self.assertIn('-DUAM_ENABLE_COMPUTER_USE=OFF', prepare)
        self.assertIn('-DUAM_ENABLE_SSH=ON', prepare)
        self.assertEqual(config[1], (self.source / 'Builds/configured').resolve())

    def test_refuses_arbitrary_output_existing_builds_and_symlink_escape(self):
        for folder in [str(self.source.parent / 'outside'), 'Builds', '../outside']:
            with self.assertRaises(ValueError):
                module.configuration({'source': str(self.source), 'build': folder})
        live = self.source / 'Builds/live'
        live.mkdir(parents=True)
        (live / 'CMakeCache.txt').touch()
        with self.assertRaises(ValueError):
            module.configuration({'source': str(self.source), 'build': 'Builds/live'})
        (self.source / 'Builds/link').symlink_to(self.source.parent, target_is_directory=True)
        with self.assertRaises(ValueError):
            module.configuration({'source': str(self.source), 'build': 'Builds/link/output'})

    def test_rejects_empty_unknown_or_malformed_options(self):
        for options in [{'providers': []}, {'providers': ['UNKNOWN']}, {'features': ['SHELL_COMMAND']}, {'source': []}, {'providers': 'CODEX'}]:
            with self.assertRaises(ValueError):
                module.configuration({'source': str(self.source), **options})

    def test_missing_prerequisites_offer_recovery(self):
        with patch.object(module.shutil, 'which', return_value=None), patch.object(module.platform, 'system', return_value='Linux'):
            errors = module.prerequisites()
            self.assertTrue(any('Node.js' in error for error in errors))
            self.assertTrue(any('CMake' in error for error in errors))
            self.assertTrue(any('macOS and Windows' in error for error in errors))

    def test_old_tools_fail_preflight_with_specific_versions(self):
        from types import SimpleNamespace
        def version(argv, **_):
            return SimpleNamespace(returncode=0, stdout='v20.0.0' if argv[0] == 'node' else 'cmake version 3.19.0')
        with patch.object(module.shutil, 'which', return_value='/fixture/tool'), patch.object(module.platform, 'system', return_value='Linux'), patch.object(module.subprocess, 'run', side_effect=version):
            errors = module.prerequisites()
            self.assertTrue(any('20.19+' in error for error in errors))
            self.assertTrue(any('CMake 3.20' in error for error in errors))

    def test_build_failure_does_not_claim_artifact_or_launch(self):
        config = module.configuration({'source': str(self.source)})
        module.STATE.update(running=True, artifact='', output='')
        with patch.object(module.subprocess, 'Popen', side_effect=OSError('fixture unavailable')):
            module.build(config)
        self.assertFalse(module.STATE['running'])
        self.assertEqual(module.STATE['artifact'], '')
        self.assertIn('fixture unavailable', module.STATE['message'])

    def test_launch_uses_direct_artifact_and_its_own_data_folder(self):
        artifact = module.artifact_path(self.source / 'Builds/configured')
        data = self.source / 'Builds/configured/data'
        with patch.object(module.subprocess, 'Popen') as launch:
            module.launch_artifact(str(artifact), str(data))
        args, kwargs = launch.call_args
        self.assertEqual(kwargs['env']['UAM_DATA_DIR'], str(data))
        self.assertEqual(kwargs['cwd'], data.parent)
        self.assertNotEqual(args[0][0], 'open')
        self.assertTrue(args[0][0].endswith('universal_agent_manager') or args[0][0].endswith('universal_agent_manager.exe'))

    def test_origin_validation_rejects_foreign_websites_and_dns_rebinding(self):
        handler = object.__new__(module.Handler)
        handler.server = type('Server', (), {'server_port': 1234})()
        handler.headers = {'Host': '127.0.0.1:1234', 'Origin': 'https://attacker.example'}
        self.assertFalse(handler.trusted())
        handler.headers = {'Host': 'attacker.example:1234'}
        self.assertFalse(handler.trusted())
        handler.headers = {'Host': '127.0.0.1:1234', 'Origin': 'http://127.0.0.1:1234'}
        self.assertTrue(handler.trusted())


if __name__ == '__main__':
    unittest.main()
