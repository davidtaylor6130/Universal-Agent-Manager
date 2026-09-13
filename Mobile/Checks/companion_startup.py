#!/usr/bin/env python3
"""Check saved companion startup, restart and proxy cleanup with an isolated data root."""
import argparse
import base64
import gzip
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request


def free_port():
    with socket.socket() as connection:
        connection.bind(('127.0.0.1', 0))
        return connection.getsockname()[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('bundle', type=Path)
    args = parser.parse_args()
    executable = args.bundle.resolve() / 'Contents/MacOS/universal_agent_manager'
    with tempfile.TemporaryDirectory(prefix='uam-companion-startup-') as directory:
        root = Path(directory)
        backend, proxy_port = free_port(), free_port()
        token = secrets.token_hex(32)
        (root / 'token').write_text(token)
        (root / 'token').chmod(0o600)
        (root / 'proxy.json').write_text(json.dumps({
            'admin': {'disabled': True},
            'apps': {'http': {'servers': {'test': {
                'listen': [f'127.0.0.1:{proxy_port}'],
                'routes': [{'handle': [{'handler': 'reverse_proxy',
                    'upstreams': [{'dial': f'127.0.0.1:{backend}'}]}]}]
            }}}}
        }))
        (root / 'companion.json').write_text(json.dumps({
            'enabled': True, 'token_file': str(root / 'token'), 'port': backend,
            'proxy_config': str(root / 'proxy.json')
        }))
        (root / 'folders.txt').write_text(f'folders_format_version=1\n\n[folder]\nid=phone-workspace\ntitle=Phone workspace\ndirectory={root}\ncollapsed=0\nexecution_host_id=local\n')
        (root / 'chats').mkdir()
        (root / 'chats/transport-check.json').write_text(json.dumps({
            'id': 'transport-check', 'title': 'Transport check', 'provider_id': 'opencode-cli',
            'messages': [{'role': 'assistant', 'provider': 'opencode-cli',
                          'content': base64.b64encode(os.urandom(1200000)).decode() + '🙂'}]
        }))
        environment = {key: value for key, value in os.environ.items()
                       if not key.startswith('UAM_COMPANION_')}
        environment['UAM_DATA_DIR'] = str(root)
        url = f'http://127.0.0.1:{proxy_port}'
        for attempt in range(2):
            with (root / 'launch.log').open('ab') as log:
                process = subprocess.Popen([str(executable)], cwd=root, env=environment,
                                           stdin=subprocess.DEVNULL, stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + 45
                while True:
                    try:
                        with urllib.request.urlopen(url + '/companion', timeout=1) as response:
                            assert response.status == 200
                        break
                    except (OSError, urllib.error.URLError):
                        if process.poll() is not None or time.monotonic() >= deadline:
                            raise AssertionError((root / 'launch.log').read_text()[-3000:])
                        time.sleep(0.2)
                for action, expected in [('getInitialState', 200), ('getChatMessages', 200), ('getCompanionToken', 403),
                                         ('setCompanionEnabled', 403)]:
                    request = urllib.request.Request(url + '/api',
                        data=json.dumps({'action': action, 'payload': {'chatId': 'transport-check'}}).encode(),
                        headers={'Authorization': 'Bearer ' + token,
                                 'Content-Type': 'application/json', 'Accept-Encoding': 'gzip'})
                    try:
                        with urllib.request.urlopen(request, timeout=20) as response:
                            status = response.status
                            body = response.read()
                            if response.headers.get('Content-Encoding') == 'gzip':
                                body = gzip.decompress(body)
                            if action == 'getChatMessages':
                                descriptor = json.loads(body)
                                assert 'uamTransfer' in descriptor, 'Incompressible fixture did not use chunks'
                                transfer = descriptor['uamTransfer']
                                assembled = bytearray()
                                while len(assembled) < transfer['totalBytes']:
                                    chunk_request = urllib.request.Request(url + '/api',
                                        data=json.dumps({'action': 'getCompanionResponseChunk', 'payload': {
                                            'id': transfer['id'], 'offset': len(assembled)}}).encode(),
                                        headers={'Authorization': 'Bearer ' + token, 'Content-Type': 'application/json'})
                                    with urllib.request.urlopen(chunk_request, timeout=20) as chunk_response:
                                        chunk = json.load(chunk_response)
                                    assembled.extend(base64.b64decode(chunk['base64'], validate=True))
                                    assert chunk['nextOffset'] == len(assembled)
                                    assert chunk['done'] == (len(assembled) == transfer['totalBytes'])
                                assert len(assembled) > 1048576
                                json.loads(assembled.decode('utf-8'))
                    except urllib.error.HTTPError as error:
                        status = error.code
                    assert status == expected, (action, status, expected)
                def api(action, payload=None):
                    request = urllib.request.Request(url + '/api',
                        data=json.dumps({'action': action, 'payload': payload or {}}).encode(),
                        headers={'Authorization': 'Bearer ' + token, 'Content-Type': 'application/json',
                                 'Accept-Encoding': 'gzip'})
                    with urllib.request.urlopen(request, timeout=20) as response:
                        body = response.read()
                        if response.headers.get('Content-Encoding') == 'gzip':
                            body = gzip.decompress(body)
                        return json.loads(body)
                before = api('getInitialState')
                created = api('createSession', {'title': 'Created on phone', 'folderId': 'phone-workspace',
                                               'providerId': 'opencode-cli', 'selectChat': True})
                assert created.get('chatId'), created
                api('setChatPinned', {'chatId': created['chatId'], 'pinned': True})
                after = api('getInitialState')
                assert after.get('selectedChatId') == before.get('selectedChatId'), 'Phone moved desktop selection'
                saved_chat = root / 'chats' / (created['chatId'] + '.json')
                assert saved_chat.exists(), 'New chat was not persisted'
                assert json.loads(saved_chat.read_text()).get('pinned') is True, 'Phone pin was not saved'
                try:
                    api('createSession', {'folderId': 'missing-workspace'})
                    raise AssertionError('Invalid workspace accepted')
                except urllib.error.HTTPError as error:
                    assert error.code == 400, error.code
                print(f'Launch {attempt + 1}: saved configuration, proxy, authenticated state and local-only settings passed', flush=True)
            finally:
                process.terminate()
                process.wait(timeout=15)
            deadline = time.monotonic() + 10
            while True:
                with socket.socket() as connection:
                    closed = connection.connect_ex(('127.0.0.1', proxy_port)) != 0
                if closed:
                    break
                assert time.monotonic() < deadline, 'Proxy survived its app process'
                time.sleep(0.1)
        print('Restart and proxy cleanup passed')


if __name__ == '__main__':
    main()
