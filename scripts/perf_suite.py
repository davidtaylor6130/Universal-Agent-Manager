"""UAM performance suite: twelve user-facing metrics from existing tests and the packaged app, reported against a baseline.

Pieces run separately and merge into one JSON file (--out), so a change can rerun only the metric it touches:
  python3 scripts/perf_suite.py fixture DIR                      synthetic data root (2,000 chats, one 2,000-message chat)
  python3 scripts/perf_suite.py --out R.json native BIN          timed native cases and request-overhead sizes (uam_core_tests)
  python3 scripts/perf_suite.py --out R.json ui                  frontend performance suite timings (fails when it fails)
  python3 scripts/perf_suite.py --out R.json app APP             packaged app on the fixture: cold start, relaunch, RAM, idle CPU, slow polls
  python3 scripts/perf_suite.py report R.json [MORE.json ...]    markdown table against the baseline and reference figures
Report-only: figures are evidence for before/after comparisons, never a pass/fail gate (budgets stay in the tests).
"""
import argparse
import json
import os
import platform
import plistlib
import re
import shutil
import signal
import statistics
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path

NUMBER = r'[\d.]+(?:e[-+]?\d+)?'  # std::cout prints very small doubles in scientific notation
ROOT = Path(__file__).resolve().parent.parent
BASELINE = ROOT / 'tests' / 'performance_baseline.json'
BRIDGE_MARKER = '[frontend] [UI startup] Initial state loaded through the native bridge.'

# The twelve user-facing metrics: (key, label, unit, T3 Code 2.74 reference, leaders' reference). None = no published figure.
# Reference figures are the published ADEmark results (github.com/zqkra/ademark), used only for comparison.
METRICS = [
    ('cold_start_ms', 'Cold start', 'ms', 2740, 1800),
    ('relaunch_ms', 'Relaunch', 'ms', None, None),
    ('new_thread_ms', 'New thread', 'ms', None, None),
    ('first_word_ms', 'First word', 'ms', 1148, 256),
    ('typing_lag_p95_ms', 'Typing lag p95 (5 agents streaming)', 'ms', 48, 50),
    ('long_thread_ms', 'Long thread (~2,000 messages)', 'ms', None, None),
    ('ram_at_rest_mb', 'RAM at rest', 'MB', 1356, 134),
    ('idle_cpu_percent', 'Idle CPU', '% of one core', 2.7, 0.2),
    ('ram_under_load_mb', 'RAM under load', 'MB', None, None),
    ('per_thread_mb', 'Per-thread memory', 'MB', None, None),
    ('hello_tokens', '"hello" tokens', 'tokens', 38200, None),
    ('real_prompt_tokens', 'Real-prompt tokens', 'tokens', 168300, None),
]
REFERENCE_NOTE = ('References: published ADEmark figures for T3 Code 2.74 and the leading apps; token references are the bare Claude Code CLI. '
                  'Rows marked * are in-process component figures (no CEF, IPC or paint), not the end-to-end measurement; '
                  '† is only what UAM adds to the request (UAM-Control tool schemas and prompt text, bytes / 4), not the provider total.')

# Native cases and how their existing [TIMING]/[SIZE] lines map to metrics.
NATIVE = [
    ('PerformanceIdleInstallPushTick', r'idle install 2000 chats: slices=([\d.]+) ms digests=([\d.]+) ms; push tick p50 unchanged=([\d.]+) ms, '
     r'one chat changed=([\d.]+) ms, large loaded chat changed=([\d.]+) ms',
     ['push_2000_slices_ms', 'push_2000_digests_ms', 'push_2000_unchanged_ms', 'push_2000_one_changed_ms', 'push_2000_large_changed_ms']),
    ('PerformanceLargeCatalogStateFingerprint', r'large catalog 5000 chats: fingerprint p50=([\d.]+) ms p95=[\d.]+ ms; unchanged push tick p50=([\d.]+) ms',
     ['fingerprint_5000_ms', 'push_5000_unchanged_ms']),
    ('PerformanceStreamingStatePushTick', r'per-rebuild p50=([\d.]+) ms; UI-thread push cost=([\d.]+) ms/s',
     ['streaming_push_rebuild_ms', 'streaming_push_ms_per_s']),
    ('PerformanceMemoryPollOnLargeInstall', r'memory poll, 2000 chats, \d+ queued: p50=([\d.]+) ms', ['memory_poll_2000_ms']),
    ('PerformanceColdHistoryPage10000Messages', r'cold initial history page, 10000 messages: ([\d.]+) ms', ['cold_history_10000_ms']),
    ('PerformanceColdLocalSummaryCatalogue', r'cold 1000-chat summary catalogue with cache regeneration: ([\d.]+) ms', ['cold_catalogue_1000_ms']),
    ('PerformanceSuiteUamRequestOverhead', r'uam-control MCP tool schemas: (\d+) bytes', ['uam_control_tools_bytes']),
    ('PerformanceSuiteUamRequestOverhead', r'computer-use MCP tool schemas: (\d+) bytes', ['computer_use_tools_bytes']),
    ('PerformanceSuiteUamRequestOverhead', r'claude-cli hello prompt text added: (\d+) bytes', ['claude_hello_prompt_added_bytes']),
    ('PerformanceSuiteUamRequestOverhead', r'codex-cli hello prompt text added: (\d+) bytes', ['codex_hello_prompt_added_bytes']),
]
# Frontend labels (digits folded to N) that stand in for end-to-end rows until the packaged app is driven.
UI_PROXIES = {'new_thread_ms': ('empty chat to usable composer', 'empty chat to usable composer', 'p50'),
              'typing_lag_p95_ms': ('input N to committed draft', 'input to committed draft, one chat streaming at most', 'p95'),
              'long_thread_ms': ('history response to committed initial page (2000 messages)', 'first 50-message page of 2,000', 'p50')}
UI_COMPONENTS = {'ui_send_feedback_ms': 'send to local pending feedback', 'ui_stream_delta_p95_ms': 'streaming delta N to committed text',
                 'ui_sidebar_selection_ms': 'large sidebar selection to committed state'}


def merge_out(path, metrics, meta_extra=None, raw=None):
    """Adds this piece's metrics to the run file, so separate pieces form one result."""
    result = json.loads(path.read_text()) if path and path.exists() else {}
    result.setdefault('meta', {}).update({
        'revision': git_revision(), 'machine': platform.node(), 'platform': f'{platform.system()} {platform.release()} {platform.machine()}',
        'cpus': os.cpu_count(), 'date': datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'), **(meta_extra or {})})
    result.setdefault('metrics', {}).update(metrics)
    if raw:
        result.setdefault('raw', {}).update(raw)
    text = json.dumps(result, indent=2) + '\n'
    if path:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
    print(json.dumps(metrics, indent=2))
    return result


def git_revision():
    try:
        return subprocess.run(['git', 'rev-parse', '--short=9', 'HEAD'], cwd=ROOT, capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return os.environ.get('GITHUB_SHA', 'unknown')[:9]


# ---------------------------------------------------------------------------- fixture

def write_fixture(root, chats=2000, long_messages=2000):
    """Synthetic data root in the on-disk chat format the cold-catalogue test writes; no private content."""
    root = Path(root)
    (root / 'chats').mkdir(parents=True, exist_ok=True)
    providers = ['claude-cli', 'codex-cli', 'gemini-cli', 'opencode-cli', 'copilot-cli', 'antigravity-cli']
    folders = ['folders_format_version=1', '']
    for index in range(20):
        workspace = root / 'workspaces' / f'folder-{index}'
        workspace.mkdir(parents=True, exist_ok=True)
        folders += ['[folder]', f'id=perf-folder-{index}', f'title=Workspace {index}', f'directory={workspace.as_posix()}',
                    'collapsed=0', 'execution_host_id=local', '']
    (root / 'folders.txt').write_text('\n'.join(folders + ['folders_complete=1', '']), encoding='utf-8')
    # Every provider off on this computer: chats and history still load and show, but no account allowance read,
    # version sweep or CLI probe starts, so a run never reads the machine's real provider credentials.
    availability = [{'providerId': provider, 'executionHostId': 'local', 'enabled': False} for provider in providers]
    (root / 'settings.txt').write_text(f'provider_availability={json.dumps(availability, separators=(",", ":"))}\n', encoding='utf-8')
    body = 'Synthetic transcript text for the performance fixture. ' * 8
    for index in range(chats):
        count = long_messages if index == 0 else 12
        chat = {'id': f'perf-chat-{index:04d}', 'title': f'Fixture chat {index}', 'provider_id': providers[index % len(providers)],
                'folder_id': f'perf-folder-{index % 20}', 'created_at': '2026-10-01 10:00:00',
                'updated_at': f'2026-10-{1 + index % 28:02d} 10:00:00',
                'messages': [{'role': 'user' if m % 2 == 0 else 'assistant', 'content': f'Message {m}: {body}'} for m in range(count)]}
        (root / 'chats' / f'{chat["id"]}.json').write_text(json.dumps(chat), encoding='utf-8')
    return root


# ---------------------------------------------------------------------------- native

def run_native(binary, out):
    binary = Path(binary).resolve()
    environment = os.environ.copy()
    if sys.platform == 'darwin':
        environment['TMPDIR'] = '/tmp'
    # Same working folder rule as scripts/run_native_tests.py: cases resolve sources from the build tree.
    workdir = next((d for d in binary.parents if (d / 'CMakeCache.txt').exists()), binary.parent)
    metrics, raw, failed = {}, {}, []
    for case in dict.fromkeys(name for name, _, _ in NATIVE):
        # One case per process, nothing else running, like the CI serial phase.
        try:
            result = subprocess.run([str(binary), case], cwd=workdir, env=environment, capture_output=True, text=True, timeout=900)
        except subprocess.TimeoutExpired:
            failed.append(case)
            continue
        lines = [line for line in result.stdout.splitlines() if line.startswith(('[TIMING]', '[SIZE]'))]
        raw[case] = lines
        if result.returncode or f'[PASS] {case}' not in result.stdout:
            failed.append(case)
        for name, pattern, keys in NATIVE:
            if name != case:
                continue
            match = next((m for m in (re.search(pattern.replace(r'[\d.]+', NUMBER), line) for line in lines) if m), None)
            for key, value in zip(keys, match.groups() if match else []):
                metrics[key] = float(value)
    if failed:
        print(f'Native cases failed (figures kept where printed): {" ".join(failed)}', file=sys.stderr)
    merge_out(out, metrics, raw={'native': raw})
    return 1 if failed else 0


# ---------------------------------------------------------------------------- ui

def summarize_ui(lines):
    groups = {}
    for line in lines:
        # search, not match: the reporter can colour the start of a test's first output line.
        match = re.search(r'\[TIMING\] (.+): ([\d.]+) ms$', line.strip())
        if match:
            # Loop indices fold to N; a fixture size such as (2000 messages) stays part of the label.
            groups.setdefault(re.sub(r'\b\d+\b(?! messages)', 'N', match.group(1)), []).append(float(match.group(2)))
    summary = {label: {'count': len(values), 'p50': round(statistics.median(values), 2),
                       'p95': round(sorted(values)[max(0, int(len(values) * 0.95 + 0.5) - 1)], 2), 'max': round(max(values), 2)}
               for label, values in groups.items()}
    return summary


def run_ui(out):
    environment = dict(os.environ, VITE_UAM_PERF_TIMINGS='1', NO_COLOR='1')
    npm = shutil.which('npm') or 'npm'
    # The explicit reporter keeps console output when Vitest would otherwise pick a quieter one (agent sessions).
    process = subprocess.Popen([npm, '--prefix', str(ROOT / 'UI-V2'), 'run', 'test:performance', '--', '--reporter=default'],
                               env=environment, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding='utf-8', errors='replace')
    lines = []
    for line in process.stdout:
        print(line, end='', flush=True)  # the Frontend gate keeps its live output
        lines.append(line)
    summary = summarize_ui(lines)
    metrics = {f'ui_proxy_{key}': summary[label][stat] for key, (label, _, stat) in UI_PROXIES.items() if label in summary}
    metrics.update({key: summary[label]['p95'] for key, label in UI_COMPONENTS.items() if label in summary})
    merge_out(out, metrics, raw={'ui': summary})
    return process.wait()


# ---------------------------------------------------------------------------- packaged app

def app_executable(app):
    app = Path(app)
    if app.suffix == '.app':
        # Local test copies wrap the app in a launcher that replaces UAM_DATA_DIR, so start the app binary itself.
        if (app / 'Contents' / 'MacOS' / 'universal_agent_manager').exists():
            return app / 'Contents' / 'MacOS' / 'universal_agent_manager'
        with open(app / 'Contents' / 'Info.plist', 'rb') as info:
            return app / 'Contents' / 'MacOS' / plistlib.load(info)['CFBundleExecutable']
    return app


def hermetic_environment(data_root):
    """Allowlisted environment: fixture data root and home, system-only PATH, no tokens, API keys or provider config paths.

    The fixture also turns every provider off (write_fixture), which is what keeps keychain and credential-manager items
    unread; these variables are the second line of defence."""
    home = data_root.parent / 'home'
    home.mkdir(exist_ok=True)
    keep = {'LANG', 'LC_ALL', 'TMPDIR', 'TEMP', 'TMP', 'SystemRoot', 'windir', 'SystemDrive', 'ComSpec', 'PATHEXT', 'OS',
            'NUMBER_OF_PROCESSORS', 'PROCESSOR_ARCHITECTURE', 'ProgramFiles', 'ProgramFiles(x86)', 'ProgramData', 'CommonProgramFiles'}
    environment = {name: value for name, value in os.environ.items() if name in keep}
    environment.update(UAM_DATA_DIR=str(data_root), HOME=str(home), USER='uam-perf', LOGNAME='uam-perf',
                       CLAUDE_CONFIG_DIR=str(home / '.claude'), CODEX_HOME=str(home / '.codex'), XDG_CONFIG_HOME=str(home / '.config'))
    if os.name == 'nt':
        for name in ('USERPROFILE', 'APPDATA', 'LOCALAPPDATA'):
            (home / name).mkdir(exist_ok=True)
            environment[name] = str(home / name)
        environment['PATH'] = os.path.join(os.environ.get('SystemRoot', r'C:\Windows'), 'System32')
    else:
        environment['PATH'] = '/usr/bin:/bin:/usr/sbin:/sbin'
    return environment


def process_table():
    """{pid: (ppid, cpu seconds)} for every process."""
    table = {}
    if os.name == 'nt':
        script = ('Get-CimInstance Win32_Process | ForEach-Object { "$($_.ProcessId) $($_.ParentProcessId) '
                  '$([uint64]$_.KernelModeTime + [uint64]$_.UserModeTime)" }')
        output = subprocess.run(['powershell', '-NoProfile', '-Command', script], capture_output=True, text=True).stdout
        for line in output.splitlines():
            parts = line.split()
            if len(parts) == 3:
                table[int(parts[0])] = (int(parts[1]), int(parts[2]) / 1e7)
        return table
    output = subprocess.run(['ps', '-axo', 'pid=,ppid=,time='], capture_output=True, text=True).stdout
    for line in output.splitlines():
        pid, ppid, cpu = line.split()
        days, _, cpu = cpu.rpartition('-')
        seconds = float(days or 0) * 86400
        clock = 0.0
        for part in cpu.split(':'):
            clock = clock * 60 + float(part)
        seconds += clock
        table[int(pid)] = (int(ppid), seconds)
    return table


def tree(root_pid, table):
    pids, frontier = {root_pid}, [root_pid]
    while frontier:
        parent = frontier.pop()
        for pid, (ppid, _) in table.items():
            if ppid == parent and pid not in pids:
                pids.add(pid)
                frontier.append(pid)
    return pids & set(table)


def memory_mb(pids):
    """macOS phys_footprint or Windows private working set, summed over the app's process tree."""
    if not pids:
        return 0.0
    if os.name == 'nt':
        script = ('Get-CimInstance Win32_PerfRawData_PerfProc_Process | Where-Object { @(' + ','.join(map(str, pids)) +
                  ') -contains $_.IDProcess } | ForEach-Object { $_.WorkingSetPrivate }')
        output = subprocess.run(['powershell', '-NoProfile', '-Command', script], capture_output=True, text=True).stdout
        return sum(int(value) for value in output.split()) / 2**20
    arguments = ['footprint', '-f', 'bytes', '--noCategories']
    for pid in pids:
        arguments += ['-p', str(pid)]
    output = subprocess.run(arguments, capture_output=True, text=True).stdout
    return sum(int(value) for value in re.findall(r'phys_footprint: (\d+) B', output)) / 2**20


def launch(executable, environment, log, timeout):
    """Starts the app and returns (process, ms until the UI's first native bridge reply is logged)."""
    seen = log.read_text(errors='replace').count(BRIDGE_MARKER) if log.exists() else 0
    started = time.perf_counter()
    process = subprocess.Popen([str(executable)], env=environment, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    while time.perf_counter() - started < timeout:
        if log.exists() and log.read_text(errors='replace').count(BRIDGE_MARKER) > seen:
            return process, (time.perf_counter() - started) * 1000
        if process.poll() is not None:
            raise RuntimeError(f'The app exited during startup with code {process.returncode}.')
        time.sleep(0.02)
    stop(process)
    raise RuntimeError('The UI did not log its first native bridge reply in time.')


def stop(process):
    """Ends the app and waits for every process it had started. A forced stop on Windows; SIGTERM on macOS, where the
    crash supervisor exits and its GUI child follows, so relaunch follows a stop rather than a clean quit."""
    pids = tree(process.pid, process_table())
    if os.name == 'nt':
        subprocess.run(['taskkill', '/PID', str(process.pid), '/T', '/F'], capture_output=True)
    else:
        process.send_signal(signal.SIGTERM)
    try:
        process.wait(timeout=20)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
    # The children outlive the root (reparented on macOS), so wait on the saved set, not the dead root's tree.
    deadline = time.time() + 20
    while time.time() < deadline and pids & set(process_table()):
        time.sleep(0.25)
    for pid in pids & set(process_table()):
        try:
            os.kill(pid, signal.SIGTERM if os.name == 'nt' else signal.SIGKILL)
        except OSError:
            pass


def windows_session():
    """This process's Windows session; its children share it. Session 0 (SSH, services, Win32_Process Create) has no
    interactive desktop, so CEF window, GPU and compositing timings there do not represent a user's machine."""
    if os.name != 'nt':
        return None
    output = subprocess.run(['powershell', '-NoProfile', '-Command', '(Get-Process -Id ' + str(os.getpid()) + ').SessionId'],
                            capture_output=True, text=True).stdout.strip()
    return int(output) if output.isdigit() else None


def slow_poll_metrics(log_text):
    """Existing rate-limited [performance] lines: slow poll ticks (>= 50 ms) and slow state-patch serialization."""
    polls = [(int(ms), stages) for ms, stages in re.findall(r'\[performance\] PollTick took (\d+) ms; stages=(\S+)', log_text)]
    patches = [int(ms) for ms in re.findall(r'\[performance\] statePatch serialization took (\d+) ms', log_text)]
    metrics = {'slow_poll_count': len(polls), 'slow_poll_max_ms': max((ms for ms, _ in polls), default=0),
               'slow_patch_count': len(patches), 'slow_patch_max_ms': max(patches, default=0)}
    for _, stages in polls:
        for name, ms in re.findall(r'(\w+):(\d+)', stages):
            key = f'slow_poll_stage_{name}_max_ms'
            metrics[key] = max(metrics.get(key, 0), int(ms))
    return metrics


def run_app(app, out, settle, idle, relaunches, timeout, keep):
    executable = app_executable(app)
    if not executable.exists():
        raise SystemExit(f'No app executable at {executable}.')
    work = Path(tempfile.mkdtemp(prefix='uam-perf-'))
    data_root = write_fixture(work / 'data')
    environment = hermetic_environment(data_root)
    log = data_root / 'uam.log'
    try:
        # Cold: the first launch on this data root, which builds its summary caches.
        process, cold = launch(executable, environment, log, timeout)
        try:
            time.sleep(settle)
            before = process_table()
            time.sleep(idle)
            after = process_table()
            pids = tree(process.pid, after)
            cpu = sum(after[pid][1] - before.get(pid, (0, 0.0))[1] for pid in pids)
            ram = memory_mb(pids)
        finally:
            stop(process)
        # Kept before relaunching, so a failed relaunch does not lose them.
        merge_out(out, {'windows_session': windows_session(), 'cold_start_ms': round(cold), 'ram_at_rest_mb': round(ram, 1), 'idle_cpu_percent': round(cpu / idle * 100, 2),
                        'app_processes': len(pids), **slow_poll_metrics(log.read_text(errors='replace'))},
                  meta_extra={'app': {'settle_s': settle, 'idle_window_s': idle}})
        relaunch = []
        for _ in range(relaunches):
            process, elapsed = launch(executable, environment, log, timeout)
            try:
                relaunch.append(elapsed)
                time.sleep(2)
            finally:
                stop(process)
        if relaunch:
            merge_out(out, {'relaunch_ms': round(statistics.median(relaunch))}, meta_extra={'relaunches_ms': [round(v) for v in relaunch]})
    finally:
        if keep:
            print(f'Kept the run folder: {work}')
        else:
            shutil.rmtree(work, ignore_errors=True)
    return 0


# ---------------------------------------------------------------------------- report

def fmt(value):
    if value is None:
        return '—'
    return f'{value:,.0f}' if abs(value) >= 100 else f'{value:,.2f}'.rstrip('0').rstrip('.')


def delta(value, base):
    if value is None or base in (None, 0):
        return ''
    return f'{(value - base) / base * 100:+.0f}%'


def uam_added_tokens(metrics):
    """UAM's own fixed additions to a Claude "hello" request: UAM-Control tool schemas plus prompt text, at ~4 bytes per token."""
    if 'uam_control_tools_bytes' not in metrics:
        return None
    return round((metrics['uam_control_tools_bytes'] + metrics.get('claude_hello_prompt_added_bytes', 0)) / 4)


def report(files, baseline_path):
    metrics, meta = {}, {}
    notes = []
    for file in files:
        if not Path(file).exists():
            notes.append(f'No result file at `{file}`; its piece did not run or failed early.')
            continue
        data = json.loads(Path(file).read_text())
        metrics.update(data.get('metrics', {}))
        for key, value in data.get('meta', {}).items():
            meta.setdefault(key, value)  # the first file names the machine
    baseline = json.loads(baseline_path.read_text()) if baseline_path and baseline_path.exists() else {}
    base = baseline.get('metrics', {})
    if base:
        notes.append(f'Baseline: {baseline.get("meta", {}).get("machine", "unknown machine")}, revision '
                     f'{baseline.get("meta", {}).get("production_revision", "?")}. Δ across different machines is only indicative.')
    if metrics.get('windows_session') == 0:
        notes.append('The packaged app ran in Windows session 0 (no interactive desktop); its startup, memory and CPU figures '
                     'do not represent a user\'s machine.')
    if not base:
        notes.append(f'No baseline at `{baseline_path}`; the Baseline and Δ columns are empty.')
    lines = [f'### UAM performance suite ({meta.get("revision", "?")}, {meta.get("platform", "?")})', '', *notes, *([''] if notes else []),
             '| Metric | Unit | This run | Baseline | Δ | T3 Code | Leaders |', '|---|---|---:|---:|---:|---:|---:|']
    for key, label, unit, t3, leaders in METRICS:
        value, base_value, mark = metrics.get(key), base.get(key), ''
        if value is None and f'ui_proxy_{key}' in metrics:
            value, base_value, mark = metrics[f'ui_proxy_{key}'], base.get(f'ui_proxy_{key}'), f' * ({UI_PROXIES[key][1]})'
        if value is None and key == 'hello_tokens' and 'uam_control_tools_bytes' in metrics:
            value, base_value, mark = uam_added_tokens(metrics), uam_added_tokens(base), ' †'
        lines.append(f'| {label}{mark} | {unit} | {fmt(value)} | {fmt(base_value)} | {delta(value, base_value)} | {fmt(t3)} | {fmt(leaders)} |')
    lines += ['', REFERENCE_NOTE, '', '| Component metric | This run | Baseline | Δ |', '|---|---:|---:|---:|']
    row_keys = {key for key, *_ in METRICS} | {f'ui_proxy_{key}' for key in UI_PROXIES}
    for key in sorted(set(metrics) - row_keys):
        lines.append(f'| `{key}` | {fmt(metrics[key])} | {fmt(base.get(key))} | {delta(metrics[key], base.get(key))} |')
    lines += ['', 'Units: `_ms` milliseconds, `_mb` MiB, `_bytes` bytes (~4 bytes per token), `_ms_per_s` UI-thread ms per second. '
              'Report-only: shared runners vary by tens of percent between runs, so compare repeated runs on one machine.']
    return '\n'.join(lines) + '\n'


def main():
    # Windows consoles default to a legacy code page that cannot print Vitest's marks or the report's symbols.
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--out', type=Path, help='run file this piece merges its figures into')
    pieces = parser.add_subparsers(dest='piece', required=True)
    fixture = pieces.add_parser('fixture')
    fixture.add_argument('directory', type=Path)
    native = pieces.add_parser('native')
    native.add_argument('binary', type=Path)
    pieces.add_parser('ui')
    app = pieces.add_parser('app')
    app.add_argument('app', type=Path, help='.app bundle (macOS) or universal_agent_manager.exe (Windows)')
    app.add_argument('--settle', type=float, default=30, help='seconds after startup before measuring rest')
    app.add_argument('--idle', type=float, default=60, help='idle CPU window in seconds')
    app.add_argument('--relaunches', type=int, default=3)
    app.add_argument('--timeout', type=float, default=120)
    app.add_argument('--keep', action='store_true', help='keep the fixture data root and log for inspection')
    reporter = pieces.add_parser('report')
    reporter.add_argument('files', nargs='+', type=Path)
    reporter.add_argument('--baseline', type=Path, default=BASELINE)
    args = parser.parse_args()
    if args.piece == 'fixture':
        print(write_fixture(args.directory))
        return 0
    if args.piece == 'native':
        return run_native(args.binary, args.out)
    if args.piece == 'ui':
        return run_ui(args.out)
    if args.piece == 'app':
        return run_app(args.app, args.out, args.settle, args.idle, args.relaunches, args.timeout, args.keep)
    sys.stdout.write(report(args.files, args.baseline))
    return 0


if __name__ == '__main__':
    sys.exit(main())
