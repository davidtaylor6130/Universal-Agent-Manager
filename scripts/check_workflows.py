#!/usr/bin/env python3
"""Check, without GitHub, that workflows parse and CMake jobs restore their fetched sources first."""
import pathlib
import re
import sys

import yaml

CONFIGURE = re.compile(r'\bcmake -S \. -B (\S+)')


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
                if uses.startswith('actions/cache') and not uses.startswith('actions/cache/save'):
                    cached.add(step['with']['path'])
                for build_dir in CONFIGURE.findall(step.get('run', '')):
                    if f'{build_dir}/_deps' not in cached:
                        found.append(f'{path}: job {name} configures {build_dir} before restoring {build_dir}/_deps')
    return found


def main():
    workflows = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else '.github/workflows')
    found = problems(workflows)
    print('\n'.join(found) or f'{workflows}: workflows parse; every CMake job restores its fetched sources')
    return 1 if found else 0


if __name__ == '__main__':
    sys.exit(main())
