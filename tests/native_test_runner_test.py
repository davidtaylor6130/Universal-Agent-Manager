import contextlib
import importlib.util
import io
from pathlib import Path
import subprocess
import threading
import time
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('native_runner', Path(__file__).resolve().parents[1] / 'scripts/run_native_tests.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class NativeRunnerTests(unittest.TestCase):
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

    def test_empty_and_duplicate_inventory_are_rejected(self):
        for inventory in ['', 'Case1\nCase1', '[PASS] Case1']:
            result = subprocess.CompletedProcess([], 0, inventory)
            with patch.object(runner.subprocess, 'run', return_value=result), self.assertRaises(ValueError):
                runner.run_tests(Path('/fixture'), 2)


if __name__ == '__main__':
    unittest.main()
