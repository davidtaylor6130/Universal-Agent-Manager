"""Checks scripts/perf_suite.py parsing without a build: python3 tests/perf_suite_test.py"""
import json
import re
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'scripts'))
import perf_suite  # noqa: E402

# Every native case the suite runs still exists, so a rename cannot silently empty the report.
sources = (perf_suite.ROOT / 'tests' / 'interaction_performance_tests.cpp').read_text(encoding='utf-8')
for case, _, _ in perf_suite.NATIVE:
    assert f'UAM_TEST({case})' in sources, case

# The printed formats of the timed cases map to their metrics.
lines = {
    'PerformanceIdleInstallPushTick': '[TIMING] idle install 2000 chats: slices=1.5 ms digests=2 ms; push tick p50 unchanged=3.25 ms, one chat changed=4 ms, large loaded chat changed=5 ms',
    'PerformanceStreamingStatePushTick': '[TIMING] streaming push fixture: fingerprint p50=1 ms; ticks=62 calls=35 rebuilds=9 pushes=9 per-rebuild p50=2.5 ms; UI-thread push cost=40.1 ms/s, cheapest of 3 windows',
    'PerformanceSuiteUamRequestOverhead': '[SIZE] uam-control MCP tool schemas: 4000 bytes, 20 tools',
}
lines['PerformanceMemoryPollOnLargeInstall'] = '[TIMING] memory poll, 2000 chats, 1000 queued: p50=5e-05 ms'
for case, line in lines.items():
    assert any(name == case and re.search(pattern.replace(r'[\d.]+', perf_suite.NUMBER), line) for name, pattern, _ in perf_suite.NATIVE), case

summary = perf_suite.summarize_ui(['\x1b[22m\x1b[39m[TIMING] input 3 to committed draft: 2 ms\r\n', '[TIMING] input 4 to committed draft: 4 ms',
                                   '[TIMING] history response to committed initial page (2000 messages): 9 ms', 'noise'])
assert summary['input N to committed draft'] == {'count': 2, 'p50': 3.0, 'p95': 4.0, 'max': 4.0}, summary
assert 'history response to committed initial page (2000 messages)' in summary

log = ('[performance] PollTick took 812 ms; stages=runtimes:700,terminals:1,push:100,awake:0 ms.\n'
       '[performance] PollTick took 60 ms; stages=runtimes:5,terminals:1,push:50,awake:0 ms.\n'
       '[performance] statePatch serialization took 160 ms, bytes=10, changedChats=1\n')
polls = perf_suite.slow_poll_metrics(log)
assert polls['slow_poll_count'] == 2 and polls['slow_poll_max_ms'] == 812 and polls['slow_poll_stage_runtimes_max_ms'] == 700, polls
assert polls['slow_patch_max_ms'] == 160 and polls['slow_poll_stage_push_max_ms'] == 100, polls

with tempfile.TemporaryDirectory() as folder:
    run, baseline = Path(folder) / 'run.json', Path(folder) / 'baseline.json'
    run.write_text(json.dumps({'meta': {'revision': 'abc'}, 'metrics': {'cold_start_ms': 3000, 'uam_control_tools_bytes': 4000,
                                                                        'ui_proxy_new_thread_ms': 20, 'push_2000_unchanged_ms': 12}}))
    baseline.write_text(json.dumps({'metrics': {'cold_start_ms': 2000, 'push_2000_unchanged_ms': 24}}))
    table = perf_suite.report([run], baseline)
    assert '| Cold start | ms | 3,000 | 2,000 | +50% | 2,740 | 1,800 |' in table, table
    assert '| New thread * (empty chat to usable composer) | ms | 20 |' in table and '| "hello" tokens † | tokens | 1,000 |' in table, table
    assert '| `push_2000_unchanged_ms` | 12 | 24 | -50% |' in table, table
    run.write_text(json.dumps({'metrics': {'windows_session': 0}}))
    assert 'session 0' in perf_suite.report([run], baseline)
    # The fixture turns every provider off and the app environment carries no credentials or real home.
    root = perf_suite.write_fixture(Path(folder) / 'data', chats=3, long_messages=5)
    assert 'provider_availability=' in (root / 'settings.txt').read_text() and '"enabled":true' not in (root / 'settings.txt').read_text()
    assert len(json.loads((root / 'chats' / 'perf-chat-0000.json').read_text())['messages']) == 5
    import os
    os.environ['GH_TOKEN'] = os.environ['ANTHROPIC_API_KEY'] = 'secret'
    environment = perf_suite.hermetic_environment(root)
    assert 'GH_TOKEN' not in environment and 'ANTHROPIC_API_KEY' not in environment and environment['UAM_DATA_DIR'] == str(root)
    assert environment['HOME'] != os.path.expanduser('~') and 'CLAUDE_CODE_OAUTH_TOKEN' not in environment
    os.environ['SYSTEMROOT'] = 'C:\\Windows'  # how Python on Windows spells it
    assert perf_suite.hermetic_environment(root).get('SYSTEMROOT') == 'C:\\Windows'
    # Empty rows (no Windows session on macOS) are hidden; a driven run without a desktop is flagged.
    run.write_text(json.dumps({'meta': {'driven': {'interactive': False}}, 'metrics': {'windows_session': None, 'first_word_ms': 300}}))
    table = perf_suite.report([run], baseline)
    assert 'windows_session' not in table and '| First word | ms | 300 |' in table and 'no interactive desktop' in table, table
    # The driven fixture turns on only OpenCode, whose executable (like every provider's) is the fake, and pins its two chats.
    import perf_driven
    root = perf_driven.driven_fixture(Path(folder) / 'driven', chats=3, long_messages=5)
    settings = (root / 'settings.txt').read_text()
    assert settings.count('"enabled":true') == 1 and '"providerId":"opencode-cli","executionHostId":"local","enabled":true' in settings
    assert all(json.loads((root / 'chats' / f'{chat}.json').read_text())['pinned'] for chat in (perf_driven.LONG_CHAT, perf_driven.SHORT_CHAT))
    shims = perf_driven.write_shims(Path(folder) / 'bin')
    assert all(any(shims.glob(name + '*')) for name in ('claude', 'codex', 'opencode')) and str(perf_driven.FAKE) in next(shims.iterdir()).read_text()
    assert perf_driven.percentile([5, 1, 3, 2, 4], 0.95) == 5 and perf_driven.percentile([], 0.95) is None
print('perf suite checks: ok')
