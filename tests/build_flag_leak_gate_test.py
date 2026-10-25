"""Exercise scripts/check_build_flag_leaks.py against synthetic build outputs."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GATE = ROOT / 'scripts' / 'check_build_flag_leaks.py'


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
            allow.write_text(json.dumps({'entries': [{'flag': 'COMPUTER_USE', 'scope': 'ui', 'match': 'computerUse'}]}))
            code, output = run('--dist', dist, '--disabled', 'COMPUTER_USE', '--allowlist', allow)
            self.assertEqual(code, 1, 'Identifier entries never excuse visible text')
            self.assertIn('Computer Use', output)
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


if __name__ == '__main__':
    unittest.main()
