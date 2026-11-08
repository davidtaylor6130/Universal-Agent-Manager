#!/usr/bin/env python3
"""Opt-in real CEF check of the HTML view runtime check, with disposable data and a fake provider only.

Usage: python3 tests/html_view_render_check_cef.py Builds/agent/universal_agent_manager.app
Never launches an installed UAM, never sends a real model prompt, and stops only the process it started.
"""
import argparse
import atexit
import concurrent.futures
import http.server
import json
import os
import secrets
import shlex
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

SESSION = 'ses_render_check'
STUCK_PAGE = '<script>while(true){}</script>'

# Each page reports only what got through. Every line below must stay silent for the check to pass.
ESCAPE_PAGE = '''<!doctype html><body><script>
const report = (name) => console.error('ESCAPE:' + name);
const blocked = (name, attempt) => { try { attempt() && report(name) } catch (e) {} };
blocked('parent-document', () => parent.document.title !== undefined);
blocked('top-document', () => top.document.title !== undefined);
blocked('cookie', () => { document.cookie = 'a=1'; return document.cookie.includes('a=1') });
blocked('local-storage', () => { localStorage.setItem('a', '1'); return true });
blocked('session-storage', () => { sessionStorage.setItem('a', '1'); return true });
blocked('bridge', () => typeof window.cefQuery !== 'undefined' || typeof window.cefQueryCancel !== 'undefined');
blocked('parent-bridge', () => typeof parent.cefQuery !== 'undefined');
blocked('webrtc', () => typeof RTCPeerConnection !== 'undefined' || typeof webkitRTCPeerConnection !== 'undefined');
blocked('popup', () => window.open('__LISTENER__/popup') !== null);
fetch('__LISTENER__/fetch').then(() => report('fetch')).catch(() => {});
fetch('file:///etc/hosts').then(() => report('file-fetch')).catch(() => {});
new Image().src = '__LISTENER__/image';
const preconnect = document.createElement('link'); preconnect.rel = 'preconnect'; preconnect.href = '__LISTENER__'; document.head.append(preconnect);
const link = document.createElement('link'); link.rel = 'stylesheet'; link.href = '__LISTENER__/style'; document.head.append(link);
const script = document.createElement('script'); script.src = '__LISTENER__/script'; document.head.append(script);
const beacon = navigator.sendBeacon && navigator.sendBeacon('__LISTENER__/beacon');
const socket = new WebSocket('__LISTENER__'.replace('http', 'ws') + '/socket');
socket.onopen = () => report('websocket');
try { top.location.href = '__LISTENER__/top' } catch (e) {}
</script></body>'''

NAVIGATION_PAGE = '''<!doctype html><body><script>setTimeout(() => { location.href = '__LISTENER__/self' }, 100)</script></body>'''


def write_request(directory, name, html):
    """Mailbox requests dropped directly run at the same time; the tool server sends them one by one."""
    payload = {'requestId': name, 'method': 'html_view_preview', 'arguments': {'title': name, 'html': html}}
    temporary = Path(directory) / 'requests' / (name + '.tmp-write')
    temporary.write_text(json.dumps(payload))
    temporary.rename(Path(directory) / 'requests' / (name + '.json'))


def fake_provider():
    """Minimal ACP peer: a prompt publishes the listed pages through the real UAM tool server."""
    args = sys.argv[2:]
    if '--version' in args:
        print('99.0.0'); return
    if 'models' in args:
        print('fixture/model-one'); return
    if 'acp' not in args and '--acp' not in args:
        print('[]' if 'list' in args else '{}'); return
    log = Path(os.environ['UAM_FAKE_PROVIDER_LOG'])
    server_spec = None
    server = None
    directory = None
    prompts = 0
    held = None

    def emit(value):
        print(json.dumps(value), flush=True)

    def result(identity, value):
        emit({'jsonrpc': '2.0', 'id': identity, 'result': value})

    def tool_call(server, identity, name, arguments):
        server.stdin.write(json.dumps({'jsonrpc': '2.0', 'id': identity, 'method': 'tools/call', 'params': {'name': name, 'arguments': arguments}}) + '\n')
        server.stdin.flush()
        return json.loads(server.stdout.readline())

    for line in sys.stdin:
        request = json.loads(line)
        method, identity = request.get('method'), request.get('id')
        params = request.get('params', {})
        if method == 'initialize':
            result(identity, {'protocolVersion': 1, 'agentInfo': {'name': 'render-check-fixture', 'version': '99.0.0'},
                              'agentCapabilities': {'loadSession': True, 'promptCapabilities': {'image': True, 'embeddedContext': True}}})
        elif method in ('session/new', 'session/load', 'session/resume'):
            server_spec = next((server for server in params.get('mcpServers', []) if server.get('name') == 'uam-control'), server_spec)  # Model discovery sessions carry none.
            if server_spec and server is None:
                # As a real provider does: UAM waits for this MCP handshake before it sends the first prompt.
                environment = {**os.environ, **{entry['name']: entry['value'] for entry in server_spec['env']}}
                directory = Path(environment['UAM_CONTROL_CAPABILITY_DIR'])
                server = subprocess.Popen([server_spec['command'], *server_spec['args']], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, env=environment)
                server.stdin.write(json.dumps({'jsonrpc': '2.0', 'id': 1, 'method': 'initialize', 'params': {'protocolVersion': '2024-11-05', 'capabilities': {}, 'clientInfo': {'name': 'fixture', 'version': '1'}}}) + '\n')
                server.stdin.flush()
                server.stdout.readline()
                server.stdin.write(json.dumps({'jsonrpc': '2.0', 'method': 'notifications/initialized'}) + '\n')
                server.stdin.flush()
            result(identity, {'sessionId': SESSION, 'models': {'currentModelId': 'fixture/model-one', 'availableModels': [{'modelId': 'fixture/model-one', 'name': 'Fixture'}]},
                              'modes': {'currentModeId': 'build', 'availableModes': [{'id': 'build', 'name': 'Build'}]}})
        elif method == 'session/prompt':
            prompts += 1
            if prompts == 2:
                # A stuck check in a turn the test then cancels.
                write_request(directory, 'at-cancel', STUCK_PAGE)
                held = identity
                log.with_suffix('.held').write_text('held')
                continue
            plan = json.loads(Path(os.environ['UAM_RENDER_CHECK_PLAN']).read_text())
            records = []
            for index, step in enumerate(plan['steps'], start=2):
                started = time.monotonic()
                reply = tool_call(server, index, 'html_view_publish', {'title': step['name'], 'html': step['html']})
                records.append({'name': step['name'], 'seconds': round(time.monotonic() - started, 2), 'reply': reply})
            names = [f'burst-{secrets.token_hex(4)}' for _ in range(plan['burst'])]
            for name in names:
                write_request(directory, name, STUCK_PAGE)
            bursts = []
            deadline = time.monotonic() + 15
            while len(bursts) < len(names) and time.monotonic() < deadline:
                for name in names:
                    path = directory / 'responses' / (name + '.json')
                    if name not in [item['name'] for item in bursts] and path.is_file():
                        bursts.append({'name': name, 'reply': json.loads(path.read_text())})
                time.sleep(.05)
            log.write_text(json.dumps({'steps': records, 'bursts': bursts, 'directory': str(directory)}))
            result(identity, {'stopReason': 'end_turn'})
        elif method == 'session/cancel':
            if held is not None:
                result(held, {'stopReason': 'cancelled'}); held = None
            if identity is not None: result(identity, {})
        elif identity is not None:
            result(identity, {})


class Listener(http.server.BaseHTTPRequestHandler):
    hits = []

    def do_GET(self):
        Listener.hits.append(self.path)
        self.send_response(200); self.send_header('Content-Length', '0'); self.end_headers()

    def do_POST(self):
        self.do_GET()

    def log_message(self, *args):
        pass


class CountingServer(http.server.ThreadingHTTPServer):
    """Counts every accepted connection, so a bare preconnect with no request still shows."""
    connections = 0

    def verify_request(self, request, client_address):
        CountingServer.connections += 1
        return True


def tool_payload(record):
    """The JSON object the tool server returned as its text content."""
    reply = record['reply']
    assert 'error' not in reply, reply
    assert not reply['result'].get('isError'), reply
    return json.loads(reply['result']['content'][0]['text'])


def renderers(bundle):
    """(pid, %CPU) for this bundle's renderer processes. Read-only: nothing here signals a process."""
    listing = subprocess.run(['ps', '-axo', 'pid=,pcpu=,args='], capture_output=True, text=True, check=True).stdout
    rows = [line.split(None, 2) for line in listing.splitlines()]
    return [(int(row[0]), float(row[1])) for row in rows if len(row) == 3 and str(bundle) in row[2] and '--type=renderer' in row[2]]


def spinning(bundle):
    return [pid for pid, cpu in renderers(bundle) if cpu > 50]


def main():
    # macOS keeps a ~0.5 GB code_sign_clone copy of each launched bundle (#684). Only here: the fake provider's stdout is parsed.
    atexit.register(subprocess.run, [sys.executable, str(Path(__file__).resolve().parents[1] / 'scripts/prune_code_sign_clones.py')])
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('bundle', type=Path)
    args = parser.parse_args()
    bundle = args.bundle.resolve()
    executable = bundle / 'Contents/MacOS/universal_agent_manager'
    assert executable.is_file() and bundle.is_relative_to(Path(__file__).resolve().parent.parent / 'Builds'), 'Only this worktree task build inside Builds is allowed'
    server = CountingServer(('127.0.0.1', 0), Listener)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    listener = f'http://127.0.0.1:{server.server_address[1]}'
    root = Path(tempfile.mkdtemp(prefix='uam-render-check-', dir='/tmp'))
    (root / 'bin').mkdir(); (root / 'chats').mkdir(); (root / 'workspace').mkdir()
    token_file = root / 'token'
    token_file.write_text(secrets.token_hex(32)); token_file.chmod(0o600)
    token = token_file.read_text().strip()
    with socket.socket() as connection:
        connection.bind(('127.0.0.1', 0)); port = connection.getsockname()[1]
    wrapper = '#!/bin/sh\nexec ' + shlex.quote(sys.executable) + ' ' + shlex.quote(str(Path(__file__).resolve())) + ' --fake-provider "$@"\n'
    for name in ('opencode', 'codex', 'gemini', 'claude', 'copilot', 'agy', 'antigravity'):
        shim = root / 'bin' / name; shim.write_text(wrapper); shim.chmod(0o700)
    (root / 'settings.txt').write_text('\n'.join(['settings_format_version=1', 'active_provider_id=gemini-cli', 'default_new_chat_provider_id=gemini-cli',
        'git_enabled=0', 'svn_enabled=0', 'git_snapshot_plain_folders=0', 'memory_enabled_default=0', 'memory_level_default=off', 'update_checks_enabled=0', 'settings_complete=1', 'remember_last_chat=1', 'last_selected_chat_id=card-seed', '']))
    # A card on screen for the whole run: stuck hidden pages must not hold or freeze the renderer it uses.
    (root / 'chats' / 'card-seed.json').write_text(json.dumps({'id': 'card-seed', 'title': 'Card fixture', 'provider_id': 'gemini-cli', 'folder_id': 'fixture',
        'workspace_directory': str(root / 'workspace'), 'execution_host_id': 'local', 'memory_enabled': False, 'memory_level': 'off', 'uamControlEnabled': False,
        'created_at': '2026-11-08T00:00:00Z', 'updated_at': '2026-11-08T00:00:00Z',
        'messages': [{'role': 'user', 'provider': 'gemini-cli', 'content': 'Seed'},
                     {'role': 'assistant', 'provider': 'gemini-cli', 'content': 'Seed answer', 'blocks': [
                         {'type': 'html_view', 'title': 'Seed card', 'text': '<!doctype html><body><p>Seed card</p><script>setInterval(() => console.error("seed-card-tick"), 500)</script></body>', 'request_id': 'html-view:seed'},
                         {'type': 'assistant_text', 'text': 'Seed answer'}]}]}))
    (root / 'folders.txt').write_text('folders_format_version=1\n\n[folder]\nid=fixture\ntitle=Fixture\ndirectory=' + str(root / 'workspace') + '\ncollapsed=0\nexecution_host_id=local\n')
    (root / 'zen-models.json').write_text('{}')
    log = root / 'fake-provider.json'
    environment = {key: value for key, value in os.environ.items() if not key.startswith('UAM_COMPANION_') and not any(secret in key.upper() for secret in ('TOKEN', 'API_KEY', 'SECRET', 'PASSWORD'))}
    environment.update({'UAM_DATA_DIR': str(root), 'UAM_COMPANION_TOKEN_FILE': str(token_file), 'UAM_COMPANION_PORT': str(port),
        'PATH': str(root / 'bin') + ':/usr/bin:/bin:/usr/sbin:/sbin', 'UAM_FAKE_PROVIDER_LOG': str(log), 'UAM_RENDER_CHECK_PLAN': str(root / 'plan.json'), 'UAM_OPENCODE_ZEN_MODELS_PATH': str(root / 'zen-models.json')})
    origin = f'http://127.0.0.1:{port}'

    def call(action, payload=None):
        request = urllib.request.Request(origin + '/api', data=json.dumps({'action': action, 'payload': payload or {}}).encode(),
            headers={'Authorization': 'Bearer ' + token, 'Content-Type': 'application/json'})
        with urllib.request.urlopen(request, timeout=30) as response:
            return json.load(response)

    def wait(predicate, label, seconds=45):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            value = predicate()
            if value: return value
            assert process.poll() is None, 'Isolated UAM exited: ' + str(process.returncode)
            time.sleep(.2)
        raise AssertionError('Timed out: ' + label)

    def ready():
        try: return call('getInitialState')
        except (OSError, urllib.error.URLError): return None

    steps = [
        {'name': 'Thrown error', 'html': '<!doctype html><body><script>throw new Error("render-check-boom")</script></body>'},
        {'name': 'Rejected promise', 'html': '<!doctype html><body><script>Promise.reject(new Error("render-check-rejected"))</script></body>'},
        {'name': 'Console error after a timer', 'html': '<!doctype html><body><script>setTimeout(() => console.error("render-check-later"), 300)</script></body>'},
        {'name': 'Clean page', 'html': '<!doctype html><body><h1>Clean</h1><script>document.body.dataset.ok = "1"</script></body>'},
        {'name': 'Sandbox escapes', 'html': ESCAPE_PAGE.replace('__LISTENER__', listener)},
        {'name': 'Navigates itself', 'html': NAVIGATION_PAGE.replace('__LISTENER__', listener)},
        {'name': 'Floods the console', 'html': '<!doctype html><body><script>for (;;) console.log("flood")</script></body>'},
        {'name': 'Never finishes', 'html': '<!doctype html><body><script>addEventListener("beforeunload", () => {}); while (true) {}</script></body>'},
    ]
    plan_path = root / 'plan.json'
    plan_path.write_text(json.dumps({'steps': steps, 'burst': 4}))

    with (root / 'app.log').open('wb') as app_log:
        process = subprocess.Popen([str(executable)], cwd=root, env=environment, stdin=subprocess.DEVNULL, stdout=app_log, stderr=app_log)
    try:
        wait(ready, 'companion startup', 60)
        time.sleep(4)
        ui_renderer = max(renderers(bundle), key=lambda row: row[1])[0]  # The window's renderer has done the startup work.
        print(f'Renderers with the seed card on screen: {renderers(bundle)}', flush=True)
        chat_id = call('createSession', {'title': 'Render check fixture', 'folderId': 'fixture', 'providerId': 'gemini-cli', 'selectChat': False})['chatId']
        call('sendAcpPrompt', {'chatId': chat_id, 'text': 'Publish the fixture pages.'})

        # The UI thread keeps answering while pages run, including the one that never finishes.
        latencies = []
        spinning_seen = set()
        finished = threading.Event()
        def sample():
            while not finished.is_set() and not log.exists() and process.poll() is None:
                started = time.monotonic()
                try: call('getCompanionCapabilities')
                except (OSError, urllib.error.URLError): pass
                latencies.append(time.monotonic() - started)
                spinning_seen.update(spinning(bundle))
                time.sleep(.1)
        with concurrent.futures.ThreadPoolExecutor(1) as executor:
            sampling = executor.submit(sample)
            try: wait(log.exists, 'fake provider tool results', 90)
            finally: finished.set()
            sampling.result(timeout=40)
        records = json.loads(log.read_text())
        by_name = {record['name']: tool_payload(record) for record in records['steps']}
        print(json.dumps({name: payload['runtimeCheck'] for name, payload in by_name.items()}, indent=1), flush=True)

        for name, payload in by_name.items():
            assert payload.get('published') is True, (name, payload)
        thrown = by_name['Thrown error']['runtimeCheck']
        assert thrown['completed'] is True and any('render-check-boom' in error['message'] and error['message'].startswith('Uncaught') for error in thrown['errors']), thrown
        assert thrown['errors'][0]['line'] == 1, thrown
        rejected = by_name['Rejected promise']['runtimeCheck']
        assert rejected['completed'] is True and any('render-check-rejected' in error['message'] for error in rejected['errors']), rejected
        later = by_name['Console error after a timer']['runtimeCheck']
        assert later['completed'] is True and any('render-check-later' in error['message'] for error in later['errors']), later
        clean = by_name['Clean page']['runtimeCheck']
        assert clean['completed'] is True and clean['errors'] == [] and 'no runtime errors' in clean['summary'], clean
        hung = by_name['Never finishes']['runtimeCheck']
        assert hung['status'] == 'timed_out' and hung['completed'] is False and 'did not complete' in hung['summary'], hung
        hung_record = next(record for record in records['steps'] if record['name'] == 'Never finishes')
        assert 4 <= hung_record['seconds'] <= 9, hung_record
        escapes = by_name['Sandbox escapes']['runtimeCheck']
        assert escapes['completed'] is True, escapes
        assert not [error for error in escapes['errors'] if 'ESCAPE:' in error['message']], escapes
        assert Listener.hits == [] and CountingServer.connections == 0, (Listener.hits, CountingServer.connections)
        navigation = by_name['Navigates itself']['runtimeCheck']
        assert navigation['status'] == 'timed_out' and navigation['completed'] is False, navigation
        flood = by_name['Floods the console']['runtimeCheck']
        flood_record = next(record for record in records['steps'] if record['name'] == 'Floods the console')
        assert flood['status'] == 'not_run' and 'too many console messages' in flood['summary'] and flood_record['seconds'] < 5, (flood, flood_record)
        assert spinning_seen and ui_renderer not in spinning_seen, (ui_renderer, spinning_seen)
        print(f'PASS stuck pages ran outside the window renderer {ui_renderer}: {sorted(spinning_seen)}', flush=True)
        ticks = [time.mktime(time.strptime(line[:19], '%Y-%m-%dT%H:%M:%S')) for line in (root / 'uam.log').read_text().splitlines() if 'seed-card-tick' in line]
        gaps = [later - earlier for earlier, later in zip(ticks, ticks[1:])]
        assert len(ticks) >= 20 and max(gaps) <= 3, (len(ticks), max(gaps or [0]))
        print(f'PASS the card on screen kept running during the stuck pages: {len(ticks)} ticks, longest gap {max(gaps):.0f}s', flush=True)
        print('PASS thrown error, rejected promise, later console error, clean page, timeout without failing the publish, sandbox escapes', flush=True)

        replies = [item['reply'] for item in records['bursts']]
        assert len(replies) == 4, records['bursts']
        statuses = sorted(reply['result']['runtimeCheck']['status'] for reply in replies)
        assert statuses == ['not_run', 'timed_out', 'timed_out', 'timed_out'], statuses
        busy = next(reply['result']['runtimeCheck'] for reply in replies if reply['result']['runtimeCheck']['status'] == 'not_run')
        assert 'too many pages' in busy['summary'] and busy['completed'] is False, busy
        print('PASS four concurrent checks: three ran, the fourth said it did not run', flush=True)

        assert latencies and max(latencies) < 2.5, max(latencies)
        print(f'PASS app stayed responsive: slowest bridge call {max(latencies):.2f}s of {len(latencies)}', flush=True)

        started = time.monotonic()
        wait(lambda: not spinning(bundle) and not (time.sleep(2) or spinning(bundle)), 'stuck renderers to stop', 60)
        print(f'PASS no renderer still spinning {time.monotonic() - started:.1f}s after the last check; renderers left: {len(renderers(bundle))}', flush=True)

        # A turn stopped during a check ends that check at once and destroys its browser.
        directory = Path(records['directory'])
        held_marker = log.with_suffix('.held')
        call('sendAcpPrompt', {'chatId': chat_id, 'text': 'Hold a stuck check.'})
        wait(held_marker.exists, 'the held turn', 30)
        time.sleep(1.5)
        started = time.monotonic()
        call('cancelAcpTurn', {'chatId': chat_id})
        response = directory / 'responses' / 'at-cancel.json'
        wait(response.exists, 'the cancelled check reply', 10)
        cancelled = json.loads(response.read_text())['result']['runtimeCheck']
        assert cancelled['status'] == 'not_run' and 'cancelled' in cancelled['summary'], cancelled
        assert time.monotonic() - started < 3.5, 'Cancel waited for the timeout'
        wait(lambda: not spinning(bundle) and not (time.sleep(2) or spinning(bundle)), 'the cancelled page to stop', 60)
        print('PASS cancelling the turn ended the check and its page stopped', flush=True)

        # A check still running at quit must not hold the app open. The eight second fallback is what a browser that never closes would hit.
        write_request(directory, 'at-quit', STUCK_PAGE)
        time.sleep(1.5)
        quit_request = subprocess.run(['osascript', '-e', f'tell application "{bundle}" to quit'], capture_output=True, text=True, timeout=30)
        assert quit_request.returncode == 0, quit_request.stderr
        started = time.monotonic()
        code = process.wait(timeout=40)
        elapsed = time.monotonic() - started
        assert code == 0 and elapsed < 7, (code, elapsed)
        assert 'did not close before shutdown' not in (root / 'uam.log').read_text()
        print(f'PASS quit with a check running: exit code {code} after {elapsed:.1f}s', flush=True)
    finally:
        # Only this exact Popen child is terminated; never find/kill an installed app.
        process.terminate()
        try: process.wait(timeout=20)
        except subprocess.TimeoutExpired:
            process.kill(); process.wait(timeout=5)
        server.shutdown()
        print('Evidence: ' + str(root), flush=True)


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--fake-provider': fake_provider()
    else: main()
