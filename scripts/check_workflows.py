#!/usr/bin/env python3
"""Check, without GitHub, that workflows parse, third-party actions are pinned and CMake jobs restore their fetched sources first."""
import pathlib
import re
import sys

import yaml

CONFIGURE = re.compile(r'\bcmake -S \. -B (\S+)')
PINNED = re.compile(r'@[0-9a-f]{40}$')


def problems(workflows):
    found = []
    files = sorted(workflows.glob('*.yml'))
    if not files:
        return [f'{workflows}: no workflow files']
    for path in files:
        try:
            workflow = yaml.safe_load(path.read_text(encoding='utf-8'))
        except yaml.YAMLError as error:
            found.append(f'{path}: {error}')
            continue
        for name, job in workflow['jobs'].items():
            cached = set()
            for step in job.get('steps', []):
                uses = step.get('uses', '')
                # GitHub's own actions/* may use tags; any other action runs only at a reviewed commit.
                if uses and not uses.startswith(('actions/', './')) and not PINNED.search(uses):
                    found.append(f'{path}: job {name} uses {uses} without a full commit SHA')
                if uses.startswith('actions/cache') and not uses.startswith('actions/cache/save'):
                    cached.add(step['with']['path'])
                for build_dir in CONFIGURE.findall(step.get('run', '')):
                    if f'{build_dir}/_deps' not in cached:
                        found.append(f'{path}: job {name} configures {build_dir} before restoring {build_dir}/_deps')
    return found


def main():
    workflows = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else '.github/workflows')
    found = problems(workflows)
    print('\n'.join(found) or f'{workflows}: workflows parse; third-party actions are pinned; every CMake job restores its fetched sources')
    return 1 if found else 0


if __name__ == '__main__':
    sys.exit(main())
