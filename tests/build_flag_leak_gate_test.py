"""Exercise scripts/check_build_flag_leaks.py against synthetic build outputs."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GATE = ROOT / 'scripts' / 'check_build_flag_leaks.py'
WORKFLOW = ROOT / '.github' / 'workflows' / 'build-flag-leaks.yml'


def run(*args):
    result = subprocess.run([sys.executable, str(GATE), *map(str, args)], capture_output=True, text=True)
    return result.returncode, result.stdout + result.stderr


class BuildFlagLeakGateTests(unittest.TestCase):
    def test_disabled_names_fail_unless_a_reviewed_entry_excuses_them(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            dist = root / 'dist'
            (dist / 'assets').mkdir(parents=True)
            (dist / 'assets/app.js').write_text('const a="claude-cli";const b={computerUseEnabled:!1};')
            (dist / 'assets/codex-123.svg').write_text('<svg/>')
            empty = root / 'empty.json'
            empty.write_text('{"entries": []}')
            code, output = run('--dist', dist, '--disabled', 'CODEX', '--allowlist', empty)
            self.assertEqual(code, 1, output)
            self.assertIn('assets/codex-123.svg', output, 'Asset file names are scanned')
            code, output = run('--dist', dist, '--disabled', 'UAM_ENABLE_RUNTIME_CLAUDE_CLI', '--allowlist', empty)
            self.assertEqual(code, 1, output)
            self.assertIn('claude-cli', output)
            allow = root / 'allow.json'
            allow.write_text(json.dumps({'entries': [{'flag': 'COMPUTER_USE', 'scope': 'ui', 'match': 'computerUse', 'max': 1, 'reason': 'protocol field'}]}))
            code, output = run('--dist', dist, '--disabled', 'COMPUTER_USE', '--allowlist', allow)
            self.assertEqual(code, 0, output)
            (dist / 'assets/more.js').write_text('label("Computer Use");x.computerUseTargetId')
            code, output = run('--dist', dist, '--disabled', 'COMPUTER_USE', '--allowlist', allow)
            self.assertEqual(code, 1, 'A visible label and an extra field exceed the reviewed budget')
            allow.write_text(json.dumps({'entries': [{'flag': 'COMPUTER_USE', 'scope': 'ui', 'match': 'computerUse', 'max': 2, 'reason': 'field'}]}))
            code, output = run('--dist', dist, '--disabled', 'COMPUTER_USE', '--allowlist', allow)
            self.assertEqual(code, 1, 'Identifier entries never excuse visible text')
            allow.write_text(json.dumps({'entries': [{'flag': 'COMPUTER_USE', 'scope': 'ui', 'match': 'computerUse', 'reason': 'field'}]}))
            self.assertIn('without a budget', run('--dist', dist, '--disabled', 'COMPUTER_USE', '--allowlist', allow)[1])
            self.assertIn('Computer Use', output)
            (dist / 'assets/near.js').write_text('a="zodiac-gemini";b="gemini-cli"')
            allow.write_text(json.dumps({'entries': [{'flag': 'GEMINI', 'scope': 'ui', 'pattern': 'zodiac-gemini', 'max': 1, 'reason': 'icon name'}]}))
            code, output = run('--dist', dist, '--disabled', 'GEMINI', '--allowlist', allow)
            self.assertEqual(code, 1, 'A pattern is tested against the matched token, not neighbouring text')
            self.assertIn('gemini-cli', output)
            (dist / 'assets/near.js').unlink()
            allow.write_text(json.dumps({'entries': [{'flag': 'CODEX', 'pattern': 'codex'}]}))
            code, output = run('--dist', dist, '--disabled', 'CODEX', '--allowlist', allow)
            self.assertEqual(code, 1, 'Entries without a reason are rejected')

    def test_build_folder_flags_come_from_the_cache_and_packaging_is_checked(self):
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary) / 'Builds' / 'off'
            bundle = build / 'universal_agent_manager.app' / 'Contents'
            (bundle / 'MacOS').mkdir(parents=True)
            (bundle / 'Resources/remote/linux-x86_64').mkdir(parents=True)
            (bundle / 'MacOS/universal_agent_manager').write_bytes(b'\x00\x01binary text gemini-cli\x00')
            (build / 'UI-V2/dist').mkdir(parents=True)
            (build / 'CMakeCache.txt').write_text('UAM_ENABLE_RUNTIME_GEMINI_CLI:BOOL=0\nUAM_ENABLE_SSH:BOOL=ON\nUAM_PACKAGE_REMOTE_RUNNERS:BOOL=OFF\n')
            empty = Path(temporary) / 'empty.json'
            empty.write_text('{"entries": []}')
            code, output = run('--build-dir', build, '--allowlist', empty)
            self.assertEqual(code, 1, output)
            self.assertIn('Disabled: GEMINI', output, '-D...=0 normalises like CMake')
            self.assertIn('gemini-cli', output, 'Native strings are scanned')
            self.assertIn('linux-x86_64', output, 'Stale cross-platform helpers fail the gate')

    def test_native_residue_is_excused_string_by_string(self):
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary) / 'gemini-off'
            exe = build / 'universal_agent_manager.app/Contents/MacOS/universal_agent_manager'
            exe.parent.mkdir(parents=True)
            (build / 'CMakeCache.txt').write_text('UAM_ENABLE_RUNTIME_GEMINI_CLI:BOOL=OFF\nUAM_ENABLE_SSH:BOOL=OFF\n')
            (build / 'UI-V2/dist').mkdir(parents=True)
            empty = Path(temporary) / 'empty.json'
            empty.write_text('{"entries": []}')
            residue = Path(temporary) / 'residue.json'
            residue.write_text(json.dumps({'reasons': {'GEMINI': 'shared settings key'},
                                           'artifacts': {'universal_agent_manager': {'GEMINI': {'geminiThinkingLevel': 1}}}}))
            gate = ('--build-dir', build, '--allowlist', empty, '--residue', residue)
            exe.write_bytes(b'\x00geminiThinkingLevel\x00' + str(build / '_deps' / 'cef.h').encode() + b'\x00')
            code, output = run(*gate)
            self.assertEqual(code, 0, f'Build-folder paths are not product text: {output}')
            exe.write_bytes(b'\x00geminiThinkingLevel\x00geminiThinkingLevel\x00')
            self.assertEqual(run(*gate)[0], 1, 'A recorded string may not occur more often than reviewed')
            exe.write_bytes('\x00Sign in to Gemini\x00'.encode('utf-16-le') + '\x00Gemini wide\x00'.encode('utf-32-le'))
            code, output = run(*gate)
            self.assertEqual(code, 1, 'Removing a reviewed string never makes room for a new one, and wide strings are scanned')
            self.assertIn('Sign in to Gemini', output)
            self.assertIn('Gemini wide', output, 'UTF-32 wide strings are scanned')
            self.assertEqual(run(*gate, '--local-path', '/src')[0], 2, 'A shallow local path is rejected')
            code, output = run(*gate, '--record-residue')
            self.assertEqual(code, 0, output)
            recorded = json.loads(residue.read_text())
            self.assertEqual(recorded['artifacts']['universal_agent_manager']['GEMINI']['Sign in to Gemini'], 1)
            recorded['reasons']['GEMINI'] = 'TODO: review'
            residue.write_text(json.dumps(recorded))
            self.assertEqual(run(*gate)[0], 1, 'Recorded strings need a reviewed reason')

    def test_packaging_profiles_without_disabled_names_are_still_checked(self):
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary) / 'build'
            remote = build / 'universal_agent_manager.app/Contents/Resources/remote/linux-x86_64'
            remote.mkdir(parents=True)
            (build / 'universal_agent_manager.app/Contents/MacOS').mkdir(parents=True)
            (build / 'universal_agent_manager.app/Contents/MacOS/universal_agent_manager').write_bytes(b'\x00')
            (build / 'CMakeCache.txt').write_text('UAM_PACKAGE_REMOTE_RUNNERS:BOOL=OFF\n')
            code, output = run('--build-dir', build)
            self.assertEqual(code, 1, output)
            self.assertIn('linux-x86_64', output)
            self.assertIn('SSH is on but no runner', output)
            self.assertIn('No interface bundle', output)
            remote.rmdir()
            (build / 'uam-runner').write_bytes(b'\x00')
            (build / 'UI-V2/dist').mkdir(parents=True)
            code, output = run('--build-dir', build)
            self.assertEqual(code, 0, output)
            runner_only = Path(temporary) / 'runner-only'
            runner_only.mkdir()
            (runner_only / 'CMakeCache.txt').write_text('UAM_RUNNER_ONLY:BOOL=ON\n')
            code, output = run('--build-dir', runner_only)
            self.assertEqual(code, 1, output)
            self.assertIn('No runner executable', output)
            (runner_only / 'uam-runner').write_bytes(b'\x00')
            self.assertEqual(run('--build-dir', runner_only)[0], 0)

    def test_windows_rows_are_recorded_per_executable_and_merged_without_touching_macos_rows(self):
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary) / 'windows-off'
            (build / 'Release').mkdir(parents=True)
            (build / 'UI-V2/dist').mkdir(parents=True)
            (build / 'CMakeCache.txt').write_text('UAM_ENABLE_RUNTIME_GEMINI_CLI:BOOL=OFF\n')
            (build / 'Release/universal_agent_manager.exe').write_bytes('\x00Sign in to Gemini\x00'.encode('utf-16-le'))
            (build / 'Release/uam-runner.exe').write_bytes(b'\x00geminiRunnerKey\x00')
            empty = Path(temporary) / 'empty.json'
            empty.write_text('{"entries": []}')
            base = {'reasons': {'GEMINI': 'shared settings key'},
                    'artifacts': {'universal_agent_manager': {'GEMINI': {'macOnly': 2}}}}
            residue = Path(temporary) / 'residue.json'
            residue.write_text(json.dumps(base))
            first = Path(temporary) / 'first.json'
            first.write_text(json.dumps(base))
            self.assertEqual(run('--build-dir', build, '--allowlist', empty, '--residue', first, '--record-residue')[0], 0)
            second = Path(temporary) / 'second.json'
            second.write_text(json.dumps({'reasons': {'GEMINI': 'TODO: replaced text'},
                                          'artifacts': {'universal_agent_manager.exe': {'GEMINI': {'Sign in to Gemini': 3}}}}))
            code, output = run('--merge-residue', first, second, '--residue', residue)
            self.assertEqual(code, 0, output)
            merged = json.loads(residue.read_text())
            self.assertEqual(merged['artifacts']['universal_agent_manager'], base['artifacts']['universal_agent_manager'], 'macOS rows are unchanged')
            self.assertEqual(merged['artifacts']['universal_agent_manager.exe']['GEMINI']['Sign in to Gemini'], 3, 'The highest count wins')
            self.assertEqual(merged['artifacts']['uam-runner.exe']['GEMINI']['geminiRunnerKey'], 1, 'The Windows runner has its own rows')
            self.assertEqual(merged['reasons']['GEMINI'], 'shared settings key', 'A reviewed reason is kept')
            self.assertEqual(run('--build-dir', build, '--allowlist', empty, '--residue', residue)[0], 0, 'Merged rows pass the gate')
            third = Path(temporary) / 'third.json'
            third.write_text(json.dumps({'reasons': {'SSH': 'TODO: review'}, 'artifacts': {'uam-runner.exe': {'SSH': {'ssh key': 1}}}}))
            unreviewed = Path(temporary) / 'unreviewed.json'
            unreviewed.write_text(residue.read_text())
            self.assertEqual(run('--merge-residue', third, '--residue', unreviewed)[0], 0)
            code, output = run('--build-dir', build, '--allowlist', empty, '--residue', unreviewed)
            self.assertEqual(code, 1, output)
            self.assertIn('Native residue for SSH has no reviewed reason', output)

    def test_recording_counts_matches_beyond_the_recorded_count(self):
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary) / 'windows-off'
            (build / 'Release').mkdir(parents=True)
            (build / 'UI-V2/dist').mkdir(parents=True)
            (build / 'CMakeCache.txt').write_text('UAM_ENABLE_RUNTIME_GEMINI_CLI:BOOL=OFF\nUAM_ENABLE_SSH:BOOL=OFF\n')
            (build / 'Release/universal_agent_manager.exe').write_bytes(b'\x00geminiKey\x00geminiKey\x00geminiKey\x00')
            empty = Path(temporary) / 'empty.json'
            empty.write_text('{"entries": []}')
            residue = Path(temporary) / 'residue.json'
            residue.write_text(json.dumps({'reasons': {'GEMINI': 'shared key'}, 'artifacts': {'universal_agent_manager.exe': {'GEMINI': {'geminiKey': 2}}}}))
            gate = ('--build-dir', build, '--allowlist', empty, '--residue', residue)
            self.assertEqual(run(*gate)[0], 1, 'A third occurrence is not reviewed yet')
            self.assertEqual(run(*gate, '--record-residue')[0], 0)
            self.assertEqual(json.loads(residue.read_text())['artifacts']['universal_agent_manager.exe']['GEMINI']['geminiKey'], 3)
            self.assertEqual(run(*gate)[0], 0, 'A recorded row passes on its own build')

    def test_native_rows_cover_windows_only_when_requested(self):
        workflow = WORKFLOW.read_text()
        native = workflow[workflow.index('\n  native:\n'):]
        self.assertIn('os: [macos-latest, windows-latest]', native)
        self.assertIn('runs-on: ${{ matrix.os }}', native)
        self.assertIn("if: (github.event_name != 'pull_request' || contains(github.event.pull_request.labels.*.name, 'build-flags'))", native,
                      'Unlabelled pull requests never start native rows')
        self.assertIn("native-residue-${{ matrix.os == 'windows-latest' && 'windows-' || '' }}${{ strategy.job-index }}", native,
                      'Windows candidates can be downloaded on their own')
        self.assertIn('if: failure()', native, 'A failing row uploads its candidates')


if __name__ == '__main__':
    unittest.main()
