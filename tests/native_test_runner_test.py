import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('native_runner', Path(__file__).resolve().parents[1] / 'scripts/run_native_tests.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class NativeRunnerTests(unittest.TestCase):
    def setUp(self):
        # Keep the fake flaky cases out of the real job summary and annotations when this runs in CI.
        kept = {key: value for key, value in os.environ.items() if key not in ('GITHUB_ACTIONS', 'GITHUB_STEP_SUMMARY')}
        environment = patch.dict(os.environ, kept, clear=True)
        environment.start()
        self.addCleanup(environment.stop)

    def exercise(self, mode='pass', requested=()):
        names = [f'Case{index}' for index in range(75)]
        calls = []
        def execute(command, **kwargs):
            if command[1] == '--list-tests':
                return subprocess.CompletedProcess(command, 0, '\n'.join(names))
            batch = command[1:]
            calls.extend(batch)
            if mode == 'crash':
                return subprocess.CompletedProcess(command, 1, '')
            if mode == 'missing':
                batch = batch[:-1]
            if mode == 'duplicate':
                batch = [*batch, batch[0]]
            return subprocess.CompletedProcess(command, 0, ''.join(f'[PASS] {name}\n' for name in batch))
        output = io.StringIO()
        with patch.object(runner.subprocess, 'run', side_effect=execute), contextlib.redirect_stdout(output), contextlib.redirect_stderr(io.StringIO()):
            result = runner.run_tests(Path('/fixture'), 2, requested)
        if not requested:
            expected = 75 if mode == 'pass' else 0 if mode == 'crash' else 72
            self.assertIn(f'Verified {expected}/75 native cases', output.getvalue())
        return result, calls, names

    def test_full_inventory_and_exact_selection(self):
        result, calls, names = self.exercise()
        self.assertEqual(result, 0)
        self.assertCountEqual(calls, names)
        result, calls, _ = self.exercise(requested=['Case3', 'Case7'])
        self.assertEqual(result, 0)
        self.assertCountEqual(calls, ['Case3', 'Case7'])

    def test_shards_partition_the_selection(self):
        names = [f'Case{index}' for index in range(75)]
        serial = {'Case4', 'Case5', 'Case6', 'Case70'}
        shards = []
        for index in range(1, 5):
            batches = []
            def execute(command, **kwargs):
                if command[1] == '--list-tests':
                    return subprocess.CompletedProcess(command, 0, '\n'.join(names))
                batches.append(command[1:])
                return subprocess.CompletedProcess(command, 0, ''.join(f'[PASS] {name}\n' for name in command[1:]))
            with patch.object(runner.subprocess, 'run', side_effect=execute), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(runner.run_tests(Path('/fixture'), 2, serial=serial, shard=(index, 4)), 0)
            ran = [name for batch in batches for name in batch]
            # Serial cases still run alone, after the shard's parallel cases.
            alone = [batch[0] for batch in batches if len(batch) == 1 and batch[0] in serial]
            self.assertEqual(alone, [name for name in ran if name in serial])
            self.assertEqual(ran[len(ran) - len(alone):], alone)
            shards.append(ran)
        self.assertCountEqual([name for ran in shards for name in ran], names)
        self.assertEqual(len({name for ran in shards for name in ran}), len(names))
        self.assertTrue(all(18 <= len(ran) <= 19 for ran in shards))

    def test_serial_cases_run_alone_after_parallel_batches(self):
        names = [f'Case{index}' for index in range(75)]
        batches, running, overlapped, lock = [], [0], [], threading.Lock()
        def execute(command, **kwargs):
            if command[1] == '--list-tests':
                return subprocess.CompletedProcess(command, 0, '\n'.join(names))
            with lock:
                running[0] += 1
                overlapped.append((command[1:], running[0]))
            time.sleep(0.01)
            with lock:
                batches.append(command[1:])
                running[0] -= 1
            return subprocess.CompletedProcess(command, 0, ''.join(f'[PASS] {name}\n' for name in command[1:]))
        with patch.object(runner.subprocess, 'run', side_effect=execute), contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(runner.run_tests(Path('/fixture'), 3, serial={'Case4', 'Case70', 'Absent'}), 0)
        self.assertIn('Serial cases not in this inventory: Absent', output.getvalue())
        self.assertEqual(batches[-2:], [['Case4'], ['Case70']])
        self.assertCountEqual([name for batch in batches[:-2] for name in batch], set(names) - {'Case4', 'Case70'})
        self.assertEqual([depth for batch, depth in overlapped if batch in (['Case4'], ['Case70'])], [1, 1])

    def test_incomplete_duplicate_and_failed_batches_fail(self):
        for mode in ['missing', 'duplicate', 'crash']:
            with self.subTest(mode=mode):
                self.assertEqual(self.exercise(mode)[0], 1)
        for names in [['Unknown'], ['Case3', 'Case3']]:
            with self.assertRaises(ValueError):
                self.exercise(requested=names)

    def test_retry_failed_once_reruns_only_failed_cases(self):
        attempts = {}
        def execute(command, **kwargs):
            if command[1] == '--list-tests':
                return subprocess.CompletedProcess(command, 0, 'Flaky\nSteady\nBroken')
            output = ''
            for name in command[1:]:
                attempts[name] = attempts.get(name, 0) + 1
                if name == 'Steady' or (name == 'Flaky' and attempts[name] > 1):
                    output += f'[PASS] {name}\n'
            return subprocess.CompletedProcess(command, 1 if 'Broken' in command or attempts.get('Flaky') == 1 else 0, output)
        for requested, expected in [(['Flaky', 'Steady'], 0), (['Flaky', 'Broken'], 1)]:
            attempts.clear()
            with self.subTest(requested=requested), patch.object(runner.subprocess, 'run', side_effect=execute), \
                    contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(runner.run_tests(Path('/fixture'), 1, requested, retry_failed_once=True), expected)
                self.assertEqual(attempts.get('Steady', 0), 1 if 'Steady' in requested else 0)
                self.assertEqual(attempts['Flaky'], 2)
        with patch.object(runner.subprocess, 'run', side_effect=execute), contextlib.redirect_stdout(io.StringIO()), \
                contextlib.redirect_stderr(io.StringIO()):
            attempts.clear()
            self.assertEqual(runner.run_tests(Path('/fixture'), 1, ['Flaky']), 1)

    def test_quarantine_reports_only_listed_failures(self):
        def execute(command, **kwargs):
            if command[1] == '--list-tests':
                return subprocess.CompletedProcess(command, 0, 'Known\nOther\nSteady')
            output = ''.join(f'[PASS] {name}\n' for name in command[1:] if name == 'Steady')
            return subprocess.CompletedProcess(command, 0 if output and len(command) == 2 else 1, output)
        for requested, expected in [(['Known', 'Steady'], 0), (['Known', 'Other'], 1)]:
            with self.subTest(requested=requested), patch.object(runner.subprocess, 'run', side_effect=execute), \
                    contextlib.redirect_stdout(io.StringIO()) as output, contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(runner.run_tests(Path('/fixture'), 1, requested, True, {'Known': '#1'}), expected)
                self.assertIn('QUARANTINED: Known (#1)', output.getvalue())

    def run_flaky_fixture(self, inventory, first_output, returncode=1):
        attempted = set()
        def execute(command, **kwargs):
            if command[1] == '--list-tests':
                return subprocess.CompletedProcess(command, 0, inventory)
            if len(command) > 2 and not attempted.intersection(command[1:]):
                attempted.update(command[1:])
                return subprocess.CompletedProcess(command, returncode, first_output)
            passes = [name for name in command[1:] if name != 'Broken']
            return subprocess.CompletedProcess(command, 0 if passes else 1, ''.join(f'[PASS] {name}\n' for name in passes))
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'binary').touch()
            (root / 'quarantine.txt').write_text('Known #12\n')
            environment = {'GITHUB_ACTIONS': 'true', 'GITHUB_STEP_SUMMARY': str(root / 'summary.md')}
            argv = ['run_native_tests.py', str(root / 'binary'), '--retry-failed-once', '--quarantine', str(root / 'quarantine.txt'),
                    '--flaky-report', str(root / 'nested/flaky.json')]
            with patch.object(runner.subprocess, 'run', side_effect=execute), patch.object(sys, 'argv', argv), patch.dict(os.environ, environment), \
                    contextlib.redirect_stdout(io.StringIO()) as output, contextlib.redirect_stderr(io.StringIO()):
                result = runner.main()
            report = root / 'nested/flaky.json'
            summary = root / 'summary.md'
            return (result, output.getvalue(), json.loads(report.read_text()) if report.exists() else None,
                    summary.read_text() if summary.exists() else None)

    def test_pass_on_retry_stays_green_and_is_reported(self):
        first = '[PASS] Steady\n[FAIL] Flaky: budget missed\n[FAIL] Flaky2: other\n[FAIL] Known: lease\n'
        result, output, report, summary = self.run_flaky_fixture('Flaky\nFlaky2\nKnown\nCrashed\nSteady', first)
        self.assertEqual(result, 0)
        self.assertEqual(report, [
            {'case': 'Flaky', 'firstFailure': ['[FAIL] Flaky: budget missed'], 'quarantine': None},
            {'case': 'Flaky2', 'firstFailure': ['[FAIL] Flaky2: other'], 'quarantine': None},
            {'case': 'Known', 'firstFailure': ['[FAIL] Known: lease'], 'quarantine': '#12'},
            {'case': 'Crashed', 'firstFailure': ['No [FAIL] line for this case; its batch exited with code 1.'], 'quarantine': None}])
        block = output[output.index('FLAKY NATIVE TESTS: 4 failed, then passed on retry'):]
        self.assertIn('Known (quarantined #12)\n    [FAIL] Known: lease', block)
        self.assertIn('::warning title=Flaky native test::Crashed\n', block)
        self.assertNotIn('Steady', block)
        self.assertIn('- `Known` (quarantined #12)', summary)
        self.assertIn('  [FAIL] Flaky: budget missed', summary)

    def test_flaky_case_is_still_reported_when_another_case_fails(self):
        result, output, report, _ = self.run_flaky_fixture('Flaky\nBroken', '[FAIL] Flaky: first\n[FAIL] Broken: always\n')
        self.assertEqual(result, 1)
        self.assertEqual([entry['case'] for entry in report], ['Flaky'])
        self.assertIn('::warning title=Flaky native test::Flaky', output)

    def test_no_flaky_output_when_nothing_flakes(self):
        result, output, report, summary = self.run_flaky_fixture('One\nTwo', '[PASS] One\n[PASS] Two\n', returncode=0)
        self.assertEqual((result, report, summary), (0, None, None))
        self.assertNotIn('FLAKY', output)
        self.assertNotIn('::warning', output)

    def test_empty_and_duplicate_inventory_are_rejected(self):
        for inventory in ['', 'Case1\nCase1', '[PASS] Case1']:
            result = subprocess.CompletedProcess([], 0, inventory)
            with patch.object(runner.subprocess, 'run', return_value=result), self.assertRaises(ValueError):
                runner.run_tests(Path('/fixture'), 2)


if __name__ == '__main__':
    unittest.main()
