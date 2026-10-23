"""Run an owned app bundle without a browser, against synthetic provider state only.

Usage: python3 tests/management_node_integration.py Builds/management/universal_agent_manager.app
"""
import json
import os
from pathlib import Path
import secrets
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

import companion_integration

fixture = r'''
import json, os, sys
name = os.path.basename(sys.argv[0])
if os.environ.get('UAM_TEST_FIXTURE_LOG'):
    with open(os.environ['UAM_TEST_FIXTURE_LOG'], 'a') as log:
        log.write(name + ' ' + repr(sys.argv[1:]) + '\n')
if '--version' in sys.argv:
    print('v22.15.0' if name == 'node' else '1.18.15')
    sys.exit(0)
if 'acp' not in sys.argv:
    print('fixture/model-one')
    sys.exit(0)
pending = None
def send(value):
    print(json.dumps(dict(jsonrpc='2.0', **value)), flush=True)
def finish(text, reason='end_turn'):
    global pending
    if text:
        send(dict(method='session/update', params=dict(sessionId='ses_fixture', update=dict(sessionUpdate='agent_message_chunk', content=dict(type='text', text=text)))))
    if pending is not None:
        send(dict(id=pending, result=dict(stopReason=reason)))
        pending = None
for line in sys.stdin:
    request = json.loads(line)
    method = request.get('method', '')
    if method == 'initialize':
        send(dict(id=request['id'], result=dict(protocolVersion=1, agentInfo=dict(name='fixture', version='1.18.15'), agentCapabilities=dict(loadSession=True))))
    elif method in ('session/new', 'session/load'):
        send(dict(id=request['id'], result=dict(sessionId='ses_fixture', models=dict(currentModelId='fixture/model-one', availableModels=[dict(modelId='fixture/model-one', name='Fixture one'), dict(modelId='fixture/model-two', name='Fixture two')]))))
    elif method == 'session/prompt':
        pending = request['id']
        text = str(request['params'].get('prompt', []))
        if 'permission check' in text:
            send(dict(id=900, method='session/request_permission', params=dict(sessionId='ses_fixture', toolCall=dict(toolCallId='fixture-permission', title='Read fixture', kind='read', status='in_progress'), options=[dict(optionId='allow-once', name='Allow once', kind='allow_once'), dict(optionId='deny', name='Deny', kind='reject_once')])) )
        elif 'hold check' not in text:
            finish('Companion round trip.')
    elif method == 'session/cancel':
        finish('', 'cancelled')
    elif request.get('id') == 900 and 'result' in request:
        finish('Permission answered.')
    elif 'id' in request:
        send(dict(id=request['id'], result={}))
'''

native_discovery = '--native-discovery-local-only' in sys.argv[2:]
bundle = Path(sys.argv[1]).resolve()
executable = bundle / 'Contents/MacOS/universal_agent_manager'
assert executable.is_file()
with tempfile.TemporaryDirectory(prefix='uam-management-check-') as temporary:
    root = Path(temporary)
    data = root / 'data'
    (data / 'chats').mkdir(parents=True)
    workspace = root / 'workspace'
    workspace.mkdir()
    (data / 'chats/companion-fixture.json').write_text(json.dumps(dict(
        id='companion-fixture', title='Companion integration fixture', provider_id='opencode-cli',
        execution_host_id='local', model_id='fixture/model-one', workspace_directory=str(workspace),
        memory_enabled=False, messages=[])))
    token_file = root / 'token'
    token_file.write_text(secrets.token_hex(32))
    token_file.chmod(0o600)
    binary = root / 'bin'
    binary.mkdir()
    for name in ('opencode', 'gemini', 'codex', 'claude', 'copilot', 'node'):
        path = binary / name
        path.write_text('#!' + sys.executable + '\n' + fixture)
        path.chmod(0o700)
    with socket.socket() as reservation:
        reservation.bind(('127.0.0.1', 0))
        port = reservation.getsockname()[1]
    environment = os.environ.copy()
    environment.update(UAM_DATA_DIR=str(data), UAM_COMPANION_TOKEN_FILE=str(token_file),
                       UAM_COMPANION_PORT=str(port), UAM_DISABLE_OPENCODE_ZEN_REFRESH='1',
                       UAM_TEST_FIXTURE_LOG=str(root / 'fixture.log'),
                       PATH=str(binary) + os.pathsep + '/usr/bin:/bin',
                       CODEX_HOME=str(root / 'codex'), GEMINI_CLI_HOME=str(root / 'gemini'),
                       CLAUDE_CONFIG_DIR=str(root / 'claude'), COPILOT_HOME=str(root / 'copilot'),
                       XDG_CONFIG_HOME=str(root / 'config'), XDG_DATA_HOME=str(root / 'share'),
                       OPENCODE_CONFIG_DIR=str(root / 'opencode'))
    announcement = 'uam-manager-check-' + str(os.getpid())
    discovery_origin = 'https://' + announcement + '.local'
    if native_discovery:
        environment['UAM_COMPANION_ORIGIN'] = discovery_origin
    def discovered():
        response = subprocess.run([str(executable), '--discover-management-nodes', '--discovery-local-only'],
                                  cwd=root, env=environment, capture_output=True, timeout=10)
        assert response.returncode == 0, response.stdout
        result = json.loads(response.stdout)
        assert not result['error'] and all(node['trusted'] is False for node in result['nodes'])
        return any(node['origin'] == discovery_origin for node in result['nodes'])
    url = f'http://127.0.0.1:{port}/api'
    def state():
        request = urllib.request.Request(url, data=b'{"action":"getInitialState"}',
            headers={'Authorization': 'Bearer ' + token_file.read_text(), 'Content-Type': 'application/json'})
        with urllib.request.urlopen(request, timeout=2) as response:
            return json.load(response)
    def start():
        arguments = [str(executable), '--management-node']
        if native_discovery: arguments += ['--advertise-management-node', announcement, '--discovery-local-only']
        process = subprocess.Popen(arguments, cwd=root, env=environment,
                                   stdout=subprocess.DEVNULL, stderr=log)
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline and process.poll() is None:
            try:
                state()
                return process
            except (OSError, ValueError):
                time.sleep(.1)
        process.terminate()
        process.wait(timeout=15)
        raise AssertionError('Management node did not serve authenticated state:\n' + (root / 'runtime.log').read_text()[-4000:])
    with (root / 'runtime.log').open('w') as log:
        process = start()
        try:
            if native_discovery: assert discovered()
            collision = subprocess.run([str(executable), '--management-node'], cwd=root, env=environment,
                                       capture_output=True, timeout=15)
            assert collision.returncode != 0 and b'data root' in collision.stderr.lower()
            def refused(overrides, expected, arguments=None, occupied_port=False):
                child_root = root / ('refusal-' + secrets.token_hex(4))
                child_root.mkdir()
                child_token = child_root / 'token'
                child_token.write_text(secrets.token_hex(32))
                child_token.chmod(0o600)
                child_environment = environment.copy()
                child_environment['UAM_DATA_DIR'] = str(child_root / 'data')
                child_environment['UAM_COMPANION_TOKEN_FILE'] = str(child_token)
                if not occupied_port:
                    with socket.socket() as reservation:
                        reservation.bind(('127.0.0.1', 0))
                        child_environment['UAM_COMPANION_PORT'] = str(reservation.getsockname()[1])
                child_environment.update(overrides)
                response = subprocess.run([str(executable), '--management-node'] + (arguments or []),
                                          cwd=root, env=child_environment, capture_output=True, timeout=15)
                diagnostic = response.stderr.decode(errors='replace')
                child_log = child_root / 'data/uam.log'
                if child_log.exists(): diagnostic += child_log.read_text()
                assert response.returncode != 0 and expected.lower() in diagnostic.lower(), diagnostic[-2000:]
            refused({'UAM_COMPANION_TOKEN_FILE': ''}, 'requires a configured authenticated')
            refused({}, 'failed to bind', occupied_port=True)
            advertised_arguments = ['--advertise-management-node', announcement, '--discovery-local-only']
            refused({'UAM_COMPANION_ORIGIN': ''}, 'explicit HTTPS UAM_COMPANION_ORIGIN', advertised_arguments)
            refused({'UAM_COMPANION_ORIGIN': 'http://untrusted.local'}, 'origin must be HTTPS', advertised_arguments)
            sys.argv = ['companion_integration', str(token_file), str(port), str(bundle)]
            try:
                companion_integration.main()
            except Exception:
                print((root / 'runtime.log').read_text()[-4000:], file=sys.stderr)
                for path in (root / 'fixture.log', data / 'uam.log'):
                    if path.exists(): print(path.read_text()[-6000:], file=sys.stderr)
                raise
            assert {provider['id'] for provider in state()['providers']} == {'gemini-cli', 'codex-cli', 'opencode-cli', 'claude-cli', 'copilot-cli', 'antigravity-cli'}
            with socket.create_connection(('127.0.0.1', port), timeout=2) as websocket:
                websocket.sendall((f'GET / HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nUpgrade: websocket\r\n'
                    'Connection: Upgrade\r\nSec-WebSocket-Version: 13\r\n'
                    'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n'
                    f'Authorization: Bearer {token_file.read_text()}\r\n\r\n').encode())
                assert not websocket.recv(1024).startswith(b'HTTP/1.1 101')
            process.send_signal(signal.SIGTERM)
            assert process.wait(timeout=15) == 0
            if native_discovery: assert not discovered()
            process = start()
            if native_discovery: assert discovered()
            chats = state()['chats']
            assert next(chat for chat in chats if chat['id'] == 'companion-fixture')['modelId'] == 'fixture/model-two'
            persisted = json.loads((data / 'chats/companion-fixture.json').read_text())
            assert any(message.get('content') == 'Permission answered.' for message in persisted['messages'])
        finally:
            if process.poll() is None:
                process.send_signal(signal.SIGTERM)
                process.wait(timeout=15)
    if native_discovery: print('PASS actual manager native local-only announcement, JSON untrusted hint, removal and restart.')
    print('Passed: headless management dispatch/polling/auth/assets/permissions, six provider profiles, exclusive data lock, startup refusal, graceful termination and restart persistence.')
