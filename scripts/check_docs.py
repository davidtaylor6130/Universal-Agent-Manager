#!/usr/bin/env python3
"""Check UAM's local documentation navigation and explicit provider coverage.

This validates documentation structure, not feature implementation or acceptance.
Run from any directory: python3 /path/to/repo/scripts/check_docs.py
"""
from __future__ import annotations

import argparse
from collections import Counter, deque
from pathlib import Path
import re
from urllib.parse import unquote, urlsplit

PROVIDERS = ('gemini', 'codex', 'claude-code', 'opencode', 'copilot', 'antigravity')
STATUSES = {'Implemented', 'Partial', 'Not implemented', 'Needs verification', 'Not applicable'}
DELIVERY = {'Provider', 'UAM', 'Both', 'None', 'Unverified'}
LINK = re.compile(r'(?<!!)\[[^\]\n]+\]\((<[^>\n]+>|[^\s)]+)(?:\s+"[^"\n]*")?\)')
IMAGE = re.compile(r'!\[[^\]\n]*\]\((<[^>\n]+>|[^\s)]+)\)')
HEADING = re.compile(r'^ {0,3}#{1,6}\s+(.+?)\s*#*\s*$', re.MULTILINE)
EXPLICIT_ID = re.compile(r'\bid=["\']([^"\']+)["\']')
CAPABILITY = re.compile(r'^\| `([^`]+)` \|', re.MULTILINE)


def prose(text: str) -> str:
    """Remove fenced examples; they can intentionally contain placeholder links."""
    return re.sub(r'^ {0,3}(`{3,}|~{3,})[^\n]*\n.*?^ {0,3}\1\s*$', '', text, flags=re.MULTILINE | re.DOTALL)


def anchors(text: str) -> set[str]:
    found = set(EXPLICIT_ID.findall(text))
    counts: Counter[str] = Counter()
    for heading in HEADING.findall(prose(text)):
        heading = re.sub(r'\[([^\]]+)\]\([^)]*\)', r'\1', heading)
        heading = re.sub(r'<[^>]+>', '', heading).replace('`', '').lower()
        slug = re.sub(r'[^\w\- ]', '', heading).replace(' ', '-')
        count = counts[slug]
        counts[slug] += 1
        found.add(slug if count == 0 else f'{slug}-{count}')
    return found


def check(root: Path) -> tuple[list[str], dict[str, int]]:
    root = root.resolve()
    errors: list[str] = []
    docs = root / 'docs'
    files = sorted(docs.rglob('*.md')) + [root / n for n in ('README.md', 'AGENTS.md', 'CLAUDE.md')]
    files.append(root / 'tools/build-configurator/README.md')
    graph: dict[Path, set[Path]] = {}
    checked_links = 0
    for path in files:
        if not path.is_file():
            errors.append(f'Missing documentation entry point: {path.relative_to(root)}')
            continue
        text = prose(path.read_text(encoding='utf-8'))
        graph[path.resolve()] = set()
        for match in list(LINK.finditer(text)) + list(IMAGE.finditer(text)):
            destination = match.group(1).strip('<>')
            split = urlsplit(destination)
            if split.scheme or split.netloc:
                continue
            # Historical ignored receipts are evidence, not portable navigation.
            if '.UAM/AINotes/' in split.path and 'reference' in path.relative_to(root).parts:
                continue
            checked_links += 1
            target = (path.parent / unquote(split.path)).resolve() if split.path else path.resolve()
            label = f'{path.relative_to(root)}:{text[:match.start()].count(chr(10)) + 1}'
            if not target.exists():
                errors.append(f'{label}: missing target {destination}')
                continue
            if target.suffix == '.md':
                graph[path.resolve()].add(target)
                if split.fragment and unquote(split.fragment) not in anchors(target.read_text(encoding='utf-8')):
                    errors.append(f'{label}: missing heading #{split.fragment} in {target}')

    catalog_path = docs / 'providers/providers.md'
    catalog = CAPABILITY.findall(catalog_path.read_text()) if catalog_path.is_file() else []
    if not catalog:
        errors.append('Provider capability catalogue is missing or empty.')
    if len(set(catalog)) != len(catalog):
        errors.append('Provider capability catalogue contains duplicate IDs.')
    required = set(catalog)
    for provider in PROVIDERS:
        path = docs / 'providers' / provider / (provider + '.md')
        if not path.is_file():
            errors.append(f'Missing provider checklist: {provider}')
            continue
        rows = []
        for line in path.read_text().splitlines():
            if not re.match(r'^\| `[^`]+` \|', line):
                continue
            fields = [value.strip() for value in line.strip('|').split('|')]
            if len(fields) != 4:
                errors.append(f'{provider}: capability row requires ID, delivery, implementation and limit.')
                continue
            capability, delivery, status, explanation = fields
            capability = capability.strip('`')
            rows.append(capability)
            if delivery not in DELIVERY:
                errors.append(f'{provider}/{capability}: invalid delivery {delivery!r}')
            if status not in STATUSES:
                errors.append(f'{provider}/{capability}: invalid implementation status {status!r}')
            if not explanation or (status == 'Not applicable' and len(explanation) < 20):
                errors.append(f'{provider}/{capability}: explain the route, gap or scope exception.')
        counts = Counter(rows)
        for capability in sorted(required - set(rows)):
            errors.append(f'{provider}: missing required capability {capability}')
        for capability in sorted(set(rows) - required):
            errors.append(f'{provider}: unknown capability {capability}')
        for capability, count in counts.items():
            if count > 1:
                errors.append(f'{provider}: duplicate capability {capability}')

    # The feature map must actually lead to every maintained feature page.
    seen: set[Path] = set()
    pending = deque([(docs / 'README.md').resolve()])
    while pending:
        node = pending.popleft()
        if node in seen:
            continue
        seen.add(node)
        pending.extend(graph.get(node, set()) - seen)
    for path in sorted(docs.rglob('*.md')):
        if path.resolve() not in seen:
            errors.append(f'Unreachable from docs/README.md: {path.relative_to(root)}')
    return errors, {'pages': len(files), 'local_links': checked_links,
                    'providers': len(PROVIDERS), 'capabilities_each': len(required)}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1],
                        help='Repository root (defaults to this script’s repository).')
    args = parser.parse_args()
    errors, stats = check(args.root.resolve())
    if errors:
        for error in errors:
            print('ERROR:', error)
        print(f'Documentation check failed: {len(errors)} problem(s).')
        return 1
    print(f"Documentation check passed: {stats['pages']} pages, {stats['local_links']} local links, "
          f"{stats['providers']} providers × {stats['capabilities_each']} explicit capabilities.")
    print('This checks documentation integrity; runtime verification and acceptance are separate.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
