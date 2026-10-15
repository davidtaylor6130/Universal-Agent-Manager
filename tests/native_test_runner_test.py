import contextlib
import importlib.util
import io
from pathlib import Path
import subprocess
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

    def test_incomplete_duplicate_and_failed_batches_fail(self):
        for mode in ['missing', 'duplicate', 'crash']:
            with self.subTest(mode=mode):
                self.assertEqual(self.exercise(mode)[0], 1)
        for names in [['Unknown'], ['Case3', 'Case3']]:
            with self.assertRaises(ValueError):
                self.exercise(requested=names)

    def test_empty_and_duplicate_inventory_are_rejected(self):
        for inventory in ['', 'Case1\nCase1', '[PASS] Case1']:
            result = subprocess.CompletedProcess([], 0, inventory)
            with patch.object(runner.subprocess, 'run', return_value=result), self.assertRaises(ValueError):
                runner.run_tests(Path('/fixture'), 2)


if __name__ == '__main__':
    unittest.main()
