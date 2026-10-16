"""Run every selected native case once, in bounded, isolated processes."""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
import os
import subprocess
import sys
import time


def run_tests(binary, jobs, requested=()):
    environment = os.environ.copy()
    if sys.platform == 'darwin':
        environment['TMPDIR'] = '/tmp'
    inventory = subprocess.run([str(binary), '--list-tests'], capture_output=True, text=True,
                               check=True, cwd=binary.parent, env=environment)
    names = inventory.stdout.splitlines()
    if not names or any(not name.isidentifier() for name in names) or len(set(names)) != len(names):
        raise ValueError('Native test inventory is empty, malformed or contains duplicate names.')
    selected = list(requested) if requested else names
    if len(set(selected)) != len(selected) or set(selected) - set(names):
        raise ValueError('Requested tests contain unknown or duplicate names.')
    started = time.perf_counter()
    passed = 0
    failed = False

    def execute(batch):
        return subprocess.run([str(binary), *batch], stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, text=True, timeout=600,
                              cwd=binary.parent, env=environment)

    # Small batches balance slow process/timeout fixtures without one process per tiny assertion.
    batches = [selected[index:index + 32] for index in range(0, len(selected), 32)]
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        pending = {pool.submit(execute, batch): batch for batch in batches}
        for future in as_completed(pending):
            batch = pending[future]
            try:
                result = future.result()
                print(result.stdout, end='', flush=True)
                actual = [line[len('[PASS] '):] for line in result.stdout.splitlines()
                          if line.startswith('[PASS] ')]
                counts = Counter(actual)
                passed += sum(counts[name] == 1 for name in batch)
                if result.returncode or counts != Counter(batch):
                    print(f'Batch failed or returned an incomplete inventory: {batch}', file=sys.stderr)
                    failed = True
            except (OSError, subprocess.TimeoutExpired) as error:
                print(f'Native test batch failed: {error}', file=sys.stderr)
                failed = True
    print(f'Verified {passed}/{len(selected)} native cases in '
          f'{time.perf_counter() - started:.2f}s with {jobs} processes.')
    return 1 if failed else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('names', nargs='*', help='Exact case names; omit to run the full inventory.')
    parser.add_argument('--jobs', type=int, default=2)
    args = parser.parse_intermixed_args()
    if args.jobs < 1:
        parser.error('--jobs must be at least 1')
    try:
        return run_tests(args.binary.resolve(strict=True), args.jobs, args.names)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f'Native tests could not run: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
