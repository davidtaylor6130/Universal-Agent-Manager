"""Run only against an isolated UAM profile containing Companion integration fixture.
Usage: python3 tests/companion_integration.py <token-file> [port] [app-bundle] [https-origin]
"""
import json
import os
import pathlib
import sys
import time
import urllib.error
import urllib.request


def main():
    token = pathlib.Path(sys.argv[1]).read_text().strip()
    url = os.environ.get('UAM_COMPANION_TEST_URL', 'http://127.0.0.1:' + (sys.argv[2] if len(sys.argv) > 2 else '58948')) + '/api'

    def request(body, credential=token, origin=None):
        headers = {'Content-Type': 'application/json', 'Authorization': 'Bearer ' + credential}
        if origin:
            headers['Origin'] = origin
        req = urllib.request.Request(url, data=body.encode(), headers=headers)
        try:
            with urllib.request.urlopen(req, timeout=10) as response:
                return response.status, json.load(response)
        except urllib.error.HTTPError as error:
            return error.code, json.load(error)

    def call(action, payload=None):
        status, result = request(json.dumps({'action': action, 'payload': payload or {}}))
        assert status == 200, (status, result)
        return result

    envelope = json.dumps({'action': 'getInitialState'})
    base = url.removesuffix('/api')
    for route in ('/companion', '/companion/'):
        with urllib.request.urlopen(base + route, timeout=10) as response:
            html = response.read().decode()
            assert response.status == 200 and 'type="module"' in html
    import re
    asset = re.search(r'src="(\./assets/[^"]+)"', html).group(1)
    for route in ('/' + asset.removeprefix('./'), '/companion/' + asset.removeprefix('./')):
        with urllib.request.urlopen(base + route, timeout=10) as response:
            assert response.status == 200 and 'javascript' in response.headers['Content-Type']
            response.read()  # Detect truncated asset responses, including large lazy chunks.
    if len(sys.argv) > 3:
        bundle = pathlib.Path(sys.argv[3])
        for asset_file in (bundle / 'Contents/Resources/UI-V2/dist/assets').glob('*.js'):
            with urllib.request.urlopen(base + '/assets/' + asset_file.name, timeout=10) as response:
                assert response.read() == asset_file.read_bytes(), asset_file.name
    with urllib.request.urlopen(base + '/companion.webmanifest', timeout=10) as response:
        manifest = json.load(response)
        assert manifest['start_url'] == '/companion' and manifest['display'] == 'standalone'
    if len(sys.argv) > 4:
        assert request(envelope, origin=sys.argv[4])[0] == 200
        assert request(envelope, origin=sys.argv[4] + '.invalid')[0] == 401
    assert request(envelope, origin=base)[0] == 200
    assert request(envelope, credential='wrong')[0] == 401
    assert request(envelope, origin='https://example.invalid')[0] == 401
    assert request('{')[0] == 400
    assert request(json.dumps({'action': 'deleteSession'}))[0] == 403
    assert request('x' * 65537)[0] == 413
    state = call('getInitialState')
    chat = next(c for c in state['chats'] if c['id'] == 'companion-fixture')
    assert chat['title'] == 'Companion integration fixture', 'Refusing to mutate a non-fixture chat'
    # The companion may open a subagent/native transcript, but the server must force
    # selectChat=false so this authenticated action cannot mutate desktop selection.
    open_status, open_result = request(json.dumps({
        'action': 'openNativeSessionChat',
        'payload': {'chatId': chat['id'], 'nativeSessionId': 'missing-companion-session', 'selectChat': True},
    }))
    assert open_status in (404, 502), (open_status, open_result)
    assert call('getInitialState').get('selectedChatId') == state.get('selectedChatId')
    call('setChatModel', {'chatId': chat['id'], 'modelId': 'fixture/model-two'})
    updated = next(c for c in call('getInitialState')['chats'] if c['id'] == chat['id'])
    assert updated['modelId'] == 'fixture/model-two'
    before = call('getChatMessages', {'chatId': chat['id']})['messages']
    text = 'Companion integration check ' + str(time.time_ns())
    call('sendAcpPrompt', {'chatId': chat['id'], 'text': text})
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        messages = call('getChatMessages', {'chatId': chat['id']})['messages']
        if any(m.get('content') == 'Companion round trip.' for m in messages[len(before):]):
            assert sum(m.get('content') == text for m in messages) == 1
            break
        time.sleep(.2)
    else:
        raise AssertionError('OpenCode fixture did not complete the round trip')

    def wait_runtime(predicate):
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            runtime = next(c for c in call('getInitialState')['chats'] if c['id'] == chat['id'])['acpSession']
            if runtime and predicate(runtime):
                return runtime
            time.sleep(.1)
        raise AssertionError('Fixture runtime did not reach expected state')

    wait_runtime(lambda runtime: not runtime['processing'])
    call('sendAcpPrompt', {'chatId': chat['id'], 'text': 'permission check'})
    permission = wait_runtime(lambda runtime: runtime.get('pendingPermission'))['pendingPermission']
    call('resolveAcpPermission', {'chatId': chat['id'], 'requestId': permission['requestId'], 'optionId': 'allow-once'})
    wait_runtime(lambda runtime: not runtime['processing'] and not runtime.get('pendingPermission'))
    assert any(m.get('content') == 'Permission answered.' for m in call('getChatMessages', {'chatId': chat['id']})['messages'])
    call('sendAcpPrompt', {'chatId': chat['id'], 'text': 'hold check'})
    wait_runtime(lambda runtime: runtime['processing'])
    call('cancelAcpTurn', {'chatId': chat['id']})
    wait_runtime(lambda runtime: not runtime['processing'])
    print('PASS authentication, origin rejection, malformed/oversized input, action allowlist, transcript, OpenCode send/receive, permission response and cancellation')


if __name__ == '__main__':
    main()
