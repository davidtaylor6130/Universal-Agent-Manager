#!/usr/bin/env python3
"""Opt-in real CEF companion integration with disposable data and fake CLI providers only.

Usage: python3 tests/companion_native_smoke.py Builds/agent/universal_agent_manager.app
Retains bounded logs/fixtures under .UAM/AINotes; never launches installed UAM or sends a real model prompt.
"""
import argparse
import base64
import hashlib
import json
import os
import plistlib
from pathlib import Path
import secrets
import shlex
import socket
import subprocess
import struct
import sys
import tempfile
import time
import urllib.error
import urllib.request
import zlib


def fake_provider():
    """Minimal deterministic JSON-RPC ACP peer; no networking or external commands."""
    args = sys.argv[2:]
    if os.environ.get('UAM_FAKE_PROVIDER_LOG'):
        with open(os.environ['UAM_FAKE_PROVIDER_LOG'], 'a') as log:
            log.write(json.dumps({'started': time.time(), 'operation': args[0] if args else ''}) + '\n')
    if '--version' in args:
        print('1.18.36'); return
    if 'models' in args:
        print('fixture/model-one\nfixture/model-two'); return
    if 'acp' not in args and '--acp' not in args:
        print('[]' if 'list' in args else '{}'); return
    session_id = 'ses_companion_fixture'
    held = None
    waiting_permission = None

    def emit(value):
        print(json.dumps(value), flush=True)

    def result(identity, value):
        emit({'jsonrpc': '2.0', 'id': identity, 'result': value})

    def answer(identity, text, reason='end_turn'):
        if text:
            emit({'jsonrpc': '2.0', 'method': 'session/update', 'params': {
                'sessionId': session_id, 'update': {'sessionUpdate': 'agent_message_chunk',
                'content': {'type': 'text', 'text': text}}}})
        result(identity, {'stopReason': reason})

    for line in sys.stdin:
        request = json.loads(line)
        method, identity = request.get('method'), request.get('id')
        params = request.get('params', {})
        if method is None:
            if identity == 9001 and waiting_permission is not None:
                answer(waiting_permission, 'Permission answered.')
                waiting_permission = None
            continue
        if method == 'initialize':
            result(identity, {'protocolVersion': 1, 'agentInfo': {'name': 'isolated-fixture', 'version': '1.18.36'},
                              'agentCapabilities': {'loadSession': True, 'promptCapabilities': {'image': True, 'embeddedContext': True}}})
        elif method in ('session/new', 'session/load', 'session/resume'):
            session_id = params.get('sessionId', session_id)
            result(identity, {'sessionId': session_id, 'models': {
                'currentModelId': 'fixture/model-one', 'availableModels': [
                    {'modelId': 'fixture/model-one', 'name': 'Fixture one'},
                    {'modelId': 'fixture/model-two', 'name': 'Fixture two'}]},
                'modes': {'currentModeId': 'build', 'availableModes': [{'id': 'build', 'name': 'Build'}]}})
        elif method in ('session/set_model', 'session/set_mode', 'session/set_config_option'):
            result(identity, {})
        elif method == 'session/prompt':
            text = '\n'.join(item.get('text', '') for item in params.get('prompt', []) if isinstance(item, dict))
            # Log only disposable fixture input, never inherited environment or host credentials.
            with open(os.environ['UAM_FAKE_PROVIDER_LOG'], 'a') as log:
                log.write(json.dumps({'method': method, 'text': text, 'sessionId': session_id}) + '\n')
            if 'permission check' in text:
                waiting_permission = identity
                emit({'jsonrpc': '2.0', 'id': 9001, 'method': 'session/request_permission', 'params': {
                    'sessionId': session_id, 'toolCall': {'toolCallId': 'fixture-permission', 'title': 'doom_loop', 'kind': 'other', 'status': 'pending'},
                    'options': [{'optionId': 'allow-once', 'name': 'Allow once', 'kind': 'allow_once'}, {'optionId': 'deny', 'name': 'Deny', 'kind': 'reject_once'}]}})
            elif 'hold check' in text:
                held = identity
                emit({'jsonrpc': '2.0', 'method': 'session/update', 'params': {'sessionId': session_id, 'update': {'sessionUpdate': 'agent_message_chunk', 'content': {'type': 'text', 'text': 'Holding fixture turn.'}}}})
            else:
                answer(identity, 'Companion round trip.')
        elif method == 'session/cancel':
            if held is not None:
                answer(held, '', 'cancelled'); held = None
            if identity is not None: result(identity, {})
        elif identity is not None:
            result(identity, {})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('bundle', type=Path)
    parser.add_argument('--skip-protocol', action='store_true', help='Use only while verifying an earlier binary before protocol changes are rebuilt')
    parser.add_argument('--evidence-root', type=Path, default=Path('.UAM/AINotes'))
    parser.add_argument('--native-ui', type=Path, metavar='XCTESTRUN', help='Run the opt-in native iOS round trip using an existing build-for-testing product')
    parser.add_argument('--simulator-id', help='Explicit simulator UDID required with --native-ui')
    args = parser.parse_args()
    if args.native_ui and not args.simulator_id:
        parser.error('--native-ui requires --simulator-id')
    bundle = args.bundle.resolve()
    executable = bundle / 'Contents/MacOS/universal_agent_manager'
    assert executable.is_file() and bundle.is_relative_to(Path(__file__).resolve().parent.parent / 'Builds'), 'Only this worktree task build inside Builds is allowed'
    args.evidence_root.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='companion-native-', dir=args.evidence_root.resolve()))
    (root / 'bin').mkdir(); (root / 'chats').mkdir(); (root / 'workspace').mkdir()
    token_file = root / 'token'
    token_file.write_text(secrets.token_hex(32)); token_file.chmod(0o600)
    with socket.socket() as connection:
        connection.bind(('127.0.0.1', 0)); port = connection.getsockname()[1]
    script = Path(__file__).resolve()
    wrapper = '#!/bin/sh\nexec ' + shlex.quote(sys.executable) + ' ' + shlex.quote(str(script)) + ' --fake-provider "$@"\n'
    # Every supported provider executable resolves to this fake process before normal PATH.
    for name in ('opencode', 'codex', 'gemini', 'claude', 'copilot', 'agy', 'antigravity'):
        shim = root / 'bin' / name; shim.write_text(wrapper); shim.chmod(0o700)
    settings = '\n'.join(['settings_format_version=1', 'active_provider_id=opencode-cli',
        'default_new_chat_provider_id=opencode-cli', 'remember_last_chat=1', 'last_selected_chat_id=desktop-sentinel',
        'git_enabled=0', 'svn_enabled=0', 'git_snapshot_plain_folders=0', 'memory_enabled_default=0',
        'memory_level_default=off', 'update_checks_enabled=0', 'settings_complete=1', ''])
    (root / 'settings.txt').write_text(settings)
    (root / 'folders.txt').write_text('folders_format_version=1\n\n[folder]\nid=phone-workspace\ntitle=Phone workspace\ndirectory=' + str(root / 'workspace') + '\ncollapsed=0\nexecution_host_id=local\n')
    for identity, title in [('desktop-sentinel', 'Isolated desktop sentinel')]:
        (root / 'chats' / (identity + '.json')).write_text(json.dumps({
            'id': identity, 'title': title, 'provider_id': 'opencode-cli', 'model_id': 'fixture/model-one',
            'folder_id': 'phone-workspace', 'workspace_directory': str(root / 'workspace'),
            'execution_host_id': 'local', 'memory_enabled': False, 'memory_level': 'off', 'uamControlEnabled': False,
            'created_at': '2026-10-17T00:00:00Z', 'updated_at': '2026-10-17T00:00:00Z',
            'messages': [{'role': 'user', 'provider': 'opencode-cli', 'content': 'Seed fixture prompt'},
                         {'role': 'assistant', 'provider': 'opencode-cli', 'content': 'Seed fixture answer'}]}))
    environment = {key: value for key, value in os.environ.items() if not key.startswith('UAM_COMPANION_') and not any(secret in key.upper() for secret in ('TOKEN', 'API_KEY', 'SECRET', 'PASSWORD'))}
    environment.update({'UAM_DATA_DIR': str(root), 'UAM_COMPANION_TOKEN_FILE': str(token_file),
        'UAM_COMPANION_PORT': str(port), 'PATH': str(root / 'bin') + ':/usr/bin:/bin:/usr/sbin:/sbin',
        'UAM_FAKE_PROVIDER_LOG': str(root / 'fake-provider.jsonl'), 'UAM_OPENCODE_ZEN_MODELS_PATH': str(root / 'zen-models.json')})
    (root / 'zen-models.json').write_text('{}')
    origin = f'http://127.0.0.1:{port}'
    token = token_file.read_text().strip()

    def call(action, payload=None):
        request = urllib.request.Request(origin + '/api', data=json.dumps({'action': action, 'payload': payload or {}}).encode(),
            headers={'Authorization': 'Bearer ' + token, 'Content-Type': 'application/json'})
        started = time.monotonic()
        try:
            with urllib.request.urlopen(request, timeout=30) as response:
                result = json.load(response)
        finally:
            elapsed = time.monotonic() - started
            if elapsed >= 1: print(f'HTTP {action}: {elapsed:.3f}s', flush=True)
        if 'uamTransfer' in result:
            transfer = result['uamTransfer']; data = bytearray()
            while len(data) < transfer['totalBytes']:
                chunk = call('getCompanionResponseChunk', {'id': transfer['id'], 'offset': len(data)})
                data.extend(base64.b64decode(chunk['base64'], validate=True))
                assert chunk['nextOffset'] == len(data)
            result = json.loads(data)
        return result

    def wait(predicate, label, seconds=30):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            value = predicate()
            if value: return value
            if process.poll() is not None: raise AssertionError('Isolated UAM exited: ' + str(process.returncode))
            time.sleep(.2)
        raise AssertionError('Timed out: ' + label)

    def selection():
        state = call('getInitialState')
        fixture = next(c for c in state['chats'] if c['id'] == fixture_id)
        assert fixture['title'] == 'Companion integration fixture', 'Refusing to mutate non-fixture'
        return state.get('selectedChatId')

    receipt = {'root': str(root), 'port': port, 'bundle': str(bundle), 'checks': []}
    print('Isolated evidence: ' + str(root), flush=True)
    with (root / 'app.log').open('wb') as log:
        process = subprocess.Popen([str(executable)], cwd=root, env=environment, stdin=subprocess.DEVNULL, stdout=log, stderr=log)
    receipt['pid'] = process.pid
    try:
        def ready():
            try: return call('getInitialState')
            except (OSError, urllib.error.URLError): return None
        initial = wait(ready, 'companion startup', 60)
        time.sleep(8)  # Allow isolated startup model-catalog probes to settle before mutation checks.
        limits = call('getCompanionCapabilities')['limits']
        expected_limits = {'maxInFlightRequests': 8, 'inFlightRequestTotalBytes': 32 * 1024 * 1024,
                           'maxResponseBytes': 32 * 1024 * 1024, 'maxResponseTransfers': 16,
                           'responseTransferTotalBytes': 32 * 1024 * 1024}
        assert all(limits.get(key) == value for key, value in expected_limits.items()), 'Incorrect live request/response budgets'
        receipt['capabilityLimits'] = {key: limits[key] for key in expected_limits}
        receipt['checks'].append('live advertised in-flight and retained-response budgets')
        fixture_id = call('createSession', {'title': 'Companion integration fixture', 'folderId': 'phone-workspace', 'providerId': 'opencode-cli', 'defaults': {'modelId': 'fixture/model-one'}, 'selectChat': False})['chatId']
        environment['UAM_COMPANION_FIXTURE_ID'] = fixture_id
        before = selection()
        assert before == 'desktop-sentinel', ('Unexpected desktop fixture selection', before)
        for test in (('companion_integration.py',) if args.skip_protocol else ('companion_protocol_http.py', 'companion_integration.py')):
            command = [sys.executable, str(script.parent / test), str(token_file)]
            command.extend([origin] if test == 'companion_protocol_http.py' else [str(port), str(bundle)])
            started = time.monotonic()
            completed = subprocess.run(command, capture_output=True, text=True, timeout=180, env=environment)
            (root / (test + '.log')).write_text(completed.stdout + completed.stderr)
            assert completed.returncode == 0, (test, completed.stdout, completed.stderr)
            print(completed.stdout.strip(), flush=True); receipt['checks'].append({'test': test, 'elapsedSeconds': round(time.monotonic() - started, 3)})
        assert selection() == before, 'Integration moved desktop selection'
        raw = secrets.token_bytes(112 * 1024)
        def png_chunk(kind, data):
            return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
        image = b'\x89PNG\r\n\x1a\n' + png_chunk(b'IHDR', struct.pack('>IIBBBBB', 128, 128, 8, 2, 0, 0, 0))
        image += png_chunk(b'IDAT', zlib.compress(b''.join(b'\0' + secrets.token_bytes(128 * 3) for _ in range(128)))) + png_chunk(b'IEND', b'')
        envelope = json.dumps({'action': 'stageChatAttachments', 'payload': {'chatId': fixture_id, 'items': [
            {'name': 'fixture-original.bin', 'kind': 'file', 'mimeType': 'application/octet-stream', 'dataBase64': base64.b64encode(raw).decode()},
            {'name': 'fixture-original.png', 'kind': 'image', 'mimeType': 'image/png', 'dataBase64': base64.b64encode(image).decode()}]}}).encode()
        assert len(envelope) > 65536
        transfer = call('beginCompanionRequestTransfer', {'totalBytes': len(envelope)})
        for offset in range(0, len(envelope), transfer['chunkBytes']):
            data = envelope[offset:offset + transfer['chunkBytes']]
            result = call('appendCompanionRequestChunk', {'id': transfer['id'], 'offset': offset, 'base64': base64.b64encode(data).decode()})
            assert result['nextOffset'] == offset + len(data)
        staged = call('commitCompanionRequestTransfer', {'id': transfer['id']})
        attachments = staged['attachments']
        assert len(attachments) == 2
        staged_files = []
        for attachment, original in zip(attachments, (raw, image)):
            staged_file = Path(attachment['path'])
            if not staged_file.is_absolute(): staged_file = root / 'workspace' / staged_file
            assert staged_file.is_relative_to(root / 'workspace'), 'Staging escaped isolated workspace'
            assert staged_file.read_bytes() == original, 'Original upload bytes changed'
            staged_files.append(staged_file)
        receipt['checks'].append('large byte/file/image staging preserves exact originals')

        call('sendAcpPrompt', {'chatId': fixture_id, 'text': 'Large attachment fixture', 'attachments': attachments})
        wait(lambda: any(m.get('content') == 'Large attachment fixture' and m.get('attachments') for m in call('getChatMessages', {'chatId': fixture_id})['messages']), 'attachment message persistence')
        wait(lambda: not next(c for c in call('getInitialState')['chats'] if c['id'] == fixture_id)['acpSession']['processing'], 'attachment turn completion')
        messages = call('getChatMessages', {'chatId': fixture_id})['messages']
        message_index = next(index for index, message in enumerate(messages) if message.get('content') == 'Large attachment fixture')
        preview = call('getAttachmentImagePreview', {'chatId': fixture_id, 'messageIndex': message_index, 'attachmentId': attachments[1]['id']})
        assert preview['available'] is True and preview['mimeType'] == 'image/png'
        assert preview['width'] > 0 and preview['height'] > 0
        assert base64.b64decode(preview['dataBase64'], validate=True).startswith(b'\x89PNG')
        assert staged_files[1].read_bytes() == image, 'Preview replaced the original image'
        receipt['checks'].append('attachment send persists image and bounded preview without replacing original')
        child = call('branchFromMessage', {'chatId': fixture_id, 'messageIndex': 0, 'operationId': 'native-smoke-branch', 'regenerate': False, 'selectChat': True})
        branch_id = child['chatId']
        assert branch_id != fixture_id and selection() == before, 'Branch moved desktop selection'
        created = call('createSession', {'title': 'Native smoke created fixture', 'folderId': 'phone-workspace', 'providerId': 'opencode-cli', 'selectChat': True})
        created_id = created['chatId']
        assert selection() == before, 'Create moved desktop selection'
        receipt['checks'].append('create and branch preserve desktop selection')
        for identity in (branch_id, created_id):
            call('deleteSession', {'chatId': identity})
            assert selection() == before, 'Delete moved unrelated desktop selection'
        if args.native_ui:
            # The test runner receives only this disposable host's credential. Keep the
            # xctestrun beside its existing products so __TESTROOT__ remains correct.
            original_run = args.native_ui.resolve()
            assert original_run.is_file() and original_run.is_relative_to(script.parent.parent / 'Builds/agent-ios'), 'Use this worktree iOS test products only'
            with original_run.open('rb') as source:
                run = plistlib.load(source)
            marker = 'native-real-cef-round-trip-' + secrets.token_hex(6)
            runners = []
            for configuration in run.get('TestConfigurations', []):
                runners.extend(configuration.get('TestTargets', []))
            if not runners:
                runners = [value for value in run.values() if isinstance(value, dict) and value.get('IsUITestBundle')]
            ui_targets = [target for target in runners if target.get('IsUITestBundle')]
            assert len(ui_targets) == 1, 'Expected exactly one native UI test target'
            ui_targets[0].setdefault('EnvironmentVariables', {}).update({
                'UAM_REAL_TEST_ENDPOINT': origin, 'UAM_REAL_TEST_TOKEN': token,
                'UAM_REAL_TEST_CHAT_ID': fixture_id, 'UAM_REAL_TEST_MARKER': marker})
            temporary_run = original_run.with_name('UAM-RealCompanion-' + secrets.token_hex(6) + '.xctestrun')
            try:
                with temporary_run.open('wb') as destination:
                    plistlib.dump(run, destination)
                temporary_run.chmod(0o600)
                command = ['xcodebuild', 'test-without-building', '-xctestrun', str(temporary_run),
                    '-destination', 'platform=iOS Simulator,id=' + args.simulator_id,
                    '-only-testing:UAMUITests/UAMUITests/testRealCompanionRoundTrip',
                    '-collect-test-diagnostics', 'never', '-resultBundlePath', str(root / 'NativeRoundTrip.xcresult')]
                with (root / 'native-ui.log').open('w') as ui_log:
                    completed = subprocess.run(command, stdout=ui_log, stderr=subprocess.STDOUT, timeout=240)
                assert completed.returncode == 0, 'Native UI round trip failed; see isolated native-ui.log'
            finally:
                temporary_run.unlink(missing_ok=True)
            wait(lambda: any(entry.get('method') == 'session/prompt' and marker in entry.get('text', '')
                 for entry in map(json.loads, (root / 'fake-provider.jsonl').read_text().splitlines())), 'native prompt reached fake ACP process')
            wait(lambda: any(message.get('content') == marker for message in call('getChatMessages', {'chatId': fixture_id})['messages']), 'native prompt persisted by real UAM')
            assert selection() == before, 'Native app moved desktop selection'
            receipt['checks'].append('native iOS UI, UAMKit, real CEF companion and fake ACP provider round trip without desktop selection changes')
        messages = call('getChatMessages', {'chatId': fixture_id})['messages']
        receipt['attachmentSha256'] = hashlib.sha256(raw).hexdigest()
        receipt['attachmentBytes'] = len(raw)
        receipt['imageSha256'] = hashlib.sha256(image).hexdigest()
        receipt['imageBytes'] = len(image)
        receipt['selection'] = before
        receipt['checks'] += ['large byte staging, exact original and attachment send', 'create/branch/delete preserve desktop selection']
        receipt['status'] = 'passed'
        print('PASS >64 KiB byte staging, exact original persistence, attachment send, create/branch/delete desktop selection preservation', flush=True)
    except Exception as error:
        receipt['status'] = 'failed'; receipt['error'] = str(error)
        raise
    finally:
        # Only this exact Popen child is terminated; never find/kill an installed app.
        process.terminate()
        try: process.wait(timeout=20)
        except subprocess.TimeoutExpired:
            process.kill(); process.wait(timeout=5)
        receipt['exitCode'] = process.returncode
        (root / 'receipt.json').write_text(json.dumps(receipt, indent=2))


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--fake-provider': fake_provider()
    else: main()
