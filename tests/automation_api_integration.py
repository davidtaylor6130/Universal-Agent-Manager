"""Live HTTP checks of the automation API against an isolated UAM profile.
Usage: python3 tests/automation_api_integration.py <base-url> <token> <workspace-id> [other-workspace-id]
"""
import json
import os
import sys
import urllib.error
import urllib.request

base, token, workspace = sys.argv[1:4]
other = sys.argv[4] if len(sys.argv) > 4 else None
url = base.rstrip('/') + '/automation/chats'


def call(method, target, body=None, credential=token, origin=None):
    data = body.encode() if isinstance(body, str) else json.dumps(body).encode() if body is not None else None
    headers = {'Content-Type': 'application/json'}
    if credential is not None:
        headers['Authorization'] = 'Bearer ' + credential
    if origin:
        headers['Origin'] = origin
    request = urllib.request.Request(target, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(request, timeout=20) as response:
            return response.status, json.load(response)
    except urllib.error.HTTPError as error:
        return error.code, json.load(error)


valid = {'prompt': 'Automation integration check.', 'workspaceId': workspace, 'providerId': os.environ.get('UAM_AUTOMATION_TEST_PROVIDER', 'codex-cli'), 'maxIterations': 2, 'gitPermission': 'commit'}
assert call('POST', url, valid, credential='uamat_' + 'b' * 64)[0] == 401
assert call('POST', url, valid, credential='not-a-token')[0] == 401
assert call('POST', url, valid, credential=None)[0] == 401
assert call('POST', url, valid, origin='https://example.invalid')[0] == 403
assert call('POST', url, {k: v for k, v in valid.items() if k != 'maxIterations'})[0] == 400
assert call('POST', url, dict(valid, gitPermission='pr'))[0] == 403
assert call('POST', url, dict(valid, workspaceId='not-on-the-list'))[0] == 403
if other:
    assert call('POST', url, dict(valid, workspaceId=other))[0] == 403
assert call('POST', url, 'x' * (256 * 1024 + 1))[0] == 413
assert call('POST', url, '{')[0] == 400
assert call('GET', url + '/no-such-chat')[0] == 404
status, created = call('POST', url, valid)
assert status == 200 and created['status'] == 'awaitingApproval', (status, created)
status, polled = call('GET', url + '/' + created['chatId'])
assert status == 200 and polled['status'] == 'awaitingApproval' and polled['maxIterations'] == 2, (status, polled)
companion = os.environ.get('UAM_COMPANION_TEST_TOKEN')
if companion:
    # The phone's send route, including its initial-mention branch, must refuse a held chat.
    status, refused = call('POST', base.rstrip('/') + '/api', {'action': 'sendAcpPrompt', 'payload': {'chatId': created['chatId'], 'text': '@reviewer go anyway'}}, credential=companion)
    assert status == 409, (status, refused)
    assert call('POST', url, valid, credential=companion)[0] == 401
print('automation API integration: ok', created['chatId'])
