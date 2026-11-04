"""Wait until this workflow run's remote helper jobs have succeeded, so native jobs can compile meanwhile.

Each "Remote runner" job uploads its artifact as its last step, so success means the artifact is available.
Exits 1 as soon as one of them ends any other way. Needs GH_TOKEN with actions: read.
"""
import json
import os
import subprocess
import sys
import time

PREFIX = 'Remote runner'


def state(jobs, expected):
    """Return 'ready', 'failed' or 'waiting' for the remote helper jobs in a run's job list."""
    helpers = [job for job in jobs if job['name'].startswith(PREFIX)]
    if any(job.get('conclusion') not in (None, 'success') for job in helpers):
        return 'failed'
    if len(helpers) >= expected and all(job.get('conclusion') == 'success' for job in helpers):
        return 'ready'
    return 'waiting'


def main():
    expected = int(sys.argv[1]) if len(sys.argv) > 1 else 3
    path = f"repos/{os.environ['GITHUB_REPOSITORY']}/actions/runs/{os.environ['GITHUB_RUN_ID']}/jobs?per_page=100"
    while True:
        result = subprocess.run(['gh', 'api', path], capture_output=True, text=True)
        try:
            jobs = json.loads(result.stdout)['jobs'] if result.returncode == 0 else None
        except (ValueError, KeyError):
            jobs = None
        if jobs is not None:
            current = state(jobs, expected)
            if current != 'waiting':
                for job in jobs:
                    if job['name'].startswith(PREFIX):
                        print(f"{job['name']}: {job.get('conclusion')}")
                return 0 if current == 'ready' else 1
        else:
            # Transient API errors and rate limits retry; the step's timeout bounds the wait.
            print(f'Job query failed, retrying: {result.stderr.strip()}', file=sys.stderr)
        time.sleep(20)


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--self-test':
        ok = {'name': 'Remote runner linux x86_64', 'conclusion': 'success'}
        running = {'name': 'Remote runner linux arm64', 'conclusion': None}
        other = {'name': 'Frontend', 'conclusion': 'failure'}
        assert state([ok, running, other], 3) == 'waiting'
        assert state([ok, ok, ok, other], 3) == 'ready'
        assert state([ok, ok], 3) == 'waiting'
        for conclusion in ('failure', 'cancelled', 'timed_out', 'skipped'):
            assert state([ok, running, {**ok, 'conclusion': conclusion}], 3) == 'failed'
        print('wait_for_remote_runners self-test passed')
        sys.exit(0)
    sys.exit(main())
