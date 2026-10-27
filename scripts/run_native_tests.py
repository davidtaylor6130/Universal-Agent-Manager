"""Run every selected native case once, in bounded, isolated processes."""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
import os
import subprocess
import sys
import time


def run_tests(binary, jobs, requested=(), retry_failed_once=False, quarantine=None, serial=frozenset()):
    environment = os.environ.copy()
    if sys.platform == 'darwin':
        environment['TMPDIR'] = '/tmp'
    # ccache base_dir compiles __FILE__ relative to the build tree, not to a multi-config Release/ subfolder.
    workdir = next((d for d in binary.resolve().parents if (d / 'CMakeCache.txt').exists()), binary.parent)
    inventory = subprocess.run([str(binary), '--list-tests'], capture_output=True, text=True,
                               check=True, cwd=workdir, env=environment)
    names = inventory.stdout.splitlines()
    if not names or any(not name.isidentifier() for name in names) or len(set(names)) != len(names):
        raise ValueError('Native test inventory is empty, malformed or contains duplicate names.')
    selected = list(requested) if requested else names
    if len(set(selected)) != len(selected) or set(selected) - set(names):
        raise ValueError('Requested tests contain unknown or duplicate names.')
    if serial - set(names):
        print(f'Serial cases not in this inventory: {" ".join(sorted(serial - set(names)))}')
    started = time.perf_counter()
    passed = 0
    passed_names = set()
    retried_passes = set()
    failed = False

    def execute(batch):
        return subprocess.run([str(binary), *batch], stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, text=True, timeout=600,
                              cwd=workdir, env=environment)

    # Small batches balance slow process/timeout fixtures without one process per tiny assertion. Cases with
    # tight wall-clock budgets then run one at a time, each in a fresh process with nothing else running.
    phases = [([name for name in selected if name not in serial], jobs, 32),
              ([name for name in selected if name in serial], 1, 1)]
    for phase_names, workers, size in phases:
        batches = [phase_names[index:index + size] for index in range(0, len(phase_names), size)]
        with ThreadPoolExecutor(max_workers=workers) as pool:
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
                    passed_names.update(name for name in batch if counts[name] == 1)
                    if result.returncode or counts != Counter(batch):
                        print(f'Batch failed or returned an incomplete inventory: {batch}', file=sys.stderr)
                        failed = True
                except (OSError, subprocess.TimeoutExpired) as error:
                    print(f'Native test batch failed: {error}', file=sys.stderr)
                    failed = True
    print(f'Verified {passed}/{len(selected)} native cases in '
          f'{time.perf_counter() - started:.2f}s with {jobs} processes '
          f'({len(phases[1][0])} run serially).')
    if failed and retry_failed_once:
        # CI-only flake tolerance: each failed case gets exactly one fresh, isolated rerun.
        retry = [name for name in selected if name not in passed_names]
        print(f'Retrying {len(retry)} failed native cases once: {" ".join(retry)}', flush=True)
        failed = False
        for name in retry:
            try:
                result = execute([name])
                print(result.stdout, end='', flush=True)
                if not result.returncode and result.stdout.splitlines().count(f'[PASS] {name}') == 1:
                    retried_passes.add(name)
                if result.returncode or result.stdout.splitlines().count(f'[PASS] {name}') != 1:
                    print(f'Native case failed again on retry: {name}', file=sys.stderr)
                    failed = True
            except (OSError, subprocess.TimeoutExpired) as error:
                print(f'Native case retry failed: {name}: {error}', file=sys.stderr)
                failed = True
        print(f'Retry {"failed" if failed else "passed"} for {len(retry)} native cases.')
    if quarantine:
        still_failing = [name for name in selected if name not in passed_names and name not in retried_passes]
        for name in selected:
            if name in quarantine and name in passed_names:
                print(f'QUARANTINE PASSED: {name} ({quarantine[name]}); consider removing it from the list.')
        for name in still_failing:
            if name in quarantine:
                print(f'QUARANTINED: {name} ({quarantine[name]})')
        failed = any(name not in quarantine for name in still_failing) or (failed and not still_failing)
    return 1 if failed else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('names', nargs='*', help='Exact case names; omit to run the full inventory.')
    parser.add_argument('--jobs', type=int, default=2)
    parser.add_argument('--quarantine', type=Path,
                        help='CI-only list of "<case> #<issue>" lines whose failures are reported, not fatal.')
    parser.add_argument('--serial', type=Path,
                        help='List of case names with tight wall-clock budgets; they run one at a time after the rest.')
    parser.add_argument('--retry-failed-once', action='store_true',
                        help='Rerun each failed case once in its own process; for CI timing flakes only.')
    args = parser.parse_intermixed_args()
    if args.jobs < 1:
        parser.error('--jobs must be at least 1')
    try:
        quarantine = None
        if args.quarantine:
            quarantine = {}
            for line in args.quarantine.read_text().splitlines():
                if line.strip() and not line.startswith('#'):
                    name, issue = line.split()
                    quarantine[name] = issue
        serial = frozenset()
        if args.serial:
            serial = frozenset(line.split()[0] for line in args.serial.read_text().splitlines()
                               if line.strip() and not line.strip().startswith('#'))
        return run_tests(args.binary.resolve(strict=True), args.jobs, args.names, args.retry_failed_once, quarantine, serial)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f'Native tests could not run: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
