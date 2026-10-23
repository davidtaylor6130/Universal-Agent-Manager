"""Opt-in read-only protocol smoke test against an isolated running companion.

Usage: python3 tests/companion_protocol_http.py TOKEN_FILE http://127.0.0.1:58948
Uses explicit single requests, with no retries or prompt/attachment submission.
"""

import base64
import json
import pathlib
import sys
import urllib.error
import urllib.request


def main():
    token = pathlib.Path(sys.argv[1]).read_text().strip()
    origin = sys.argv[2].rstrip('/')

    def request(action, payload=None, credential=token, request_origin=None):
        headers = {'Authorization': 'Bearer ' + credential, 'Content-Type': 'application/json'}
        if request_origin is not None:
            headers['Origin'] = request_origin
        body = json.dumps({'action': action, 'payload': payload or {}}).encode()
        req = urllib.request.Request(origin + '/api', data=body, headers=headers)
        try:
            with urllib.request.urlopen(req, timeout=15) as response:
                return response.status, json.load(response)
        except urllib.error.HTTPError as error:
            return error.code, json.load(error)

    def call(action, payload=None):
        status, result = request(action, payload)
        assert status == 200, (action, status)
        return result

    for action in ('getCompanionCapabilities', 'getCompanionSettings',
                   'beginCompanionRequestTransfer', 'appendCompanionRequestChunk',
                   'commitCompanionRequestTransfer', 'getCompanionResponseChunk'):
        assert request(action, credential='wrong')[0] == 401, action
        assert request(action, request_origin='https://untrusted.invalid')[0] == 401, action

    capabilities = call('getCompanionCapabilities')
    assert capabilities['protocolVersion'] == 1
    assert capabilities['mainHost']['id'] == 'local'
    assert capabilities['mainHost']['instanceId']
    assert capabilities['restrictions']['desktopSelectionReadOnly'] is True
    assert capabilities['restrictions']['automaticReplay'] is False
    assert capabilities['features']['byteAttachmentStaging'] is True
    settings = call('getCompanionSettings')
    assert settings['readOnly'] is False
    assert capabilities['restrictions']['settingsReadOnly'] is False
    assert capabilities['restrictions']['configurationDefinitionsReadOnly'] is True
    assert capabilities['restrictions']['connectionAdministrationReadOnly'] is True
    assert set(capabilities['writableSettingsActions']) == {
        'setProviderChatDefaults', 'setMemorySettings', 'setUamAgentPreferences', 'setStorageSettings',
        'setVcsSettings', 'setProviderMcpConfiguration', 'setProviderAvailability'}
    mcp = call('getProviderMcpStatus', {'executionHostId': 'local'})
    assert mcp['executionHostId'] == 'local'
    assert len(mcp['providers']) == 6
    for provider in mcp['providers']:
        assert set(provider) <= {'providerId', 'uamControlEnabled', 'sharedSetupEnabled', 'status', 'detail',
                                 'activeSessionCount', 'configuredSessionCount', 'connectedSessionCount',
                                 'additionalServerCount', 'providerSessionFailed', 'mcpServers'}
        assert all(set(server) <= {'id', 'name', 'enabled'} for server in provider['mcpServers'])
    for action, payload, expected in (
            ('setMemorySettings', {'recallBudgetBytes': -1}, 400),
            ('setStorageSettings', {'backupLogRetentionDays': 1.5}, 400),
            ('setVcsSettings', {'gitEnabled': False}, 403),
            ('getProviderMcpStatus', {'executionHostId': 'local', 'workspaceDirectory': '/arbitrary'}, 403),
            ('setProviderMcpConfiguration', {'executionHostId': 'local', 'providerId': 'codex-cli',
                                             'serverId': 'existing', 'enabled': True, 'headers': {}}, 403)):
        assert request(action, payload)[0] == expected, action
    assert not {'mcpServers', 'centralProviderConfiguration', 'voiceInputServerBaseUrl'} & settings['settings'].keys()
    for action in ('getCompanionToken', 'setCompanionEnabled', 'selectSession', 'deleteSessions'):
        assert request(action, {'chatId': 'not-a-real-chat'})[0] == 403, action

    for action in ('deleteSession', 'branchFromMessage'):
        assert request(action, {'chatId': 'not-a-real-chat', **({'messageIndex': 0} if action == 'branchFromMessage' else {})})[0] == 404, action

    assert request('beginCompanionRequestTransfer', {'totalBytes': 32 * 1024 * 1024 + 1})[0] == 413
    assert request('beginCompanionRequestTransfer', {'totalBytes': -1})[0] == 400
    assert request('beginCompanionRequestTransfer', {'totalBytes': 1.5})[0] == 400

    def upload(envelope):
        data = json.dumps(envelope).encode()
        transfer = call('beginCompanionRequestTransfer', {'totalBytes': len(data)})
        transfer_id = transfer['id']
        assert request('appendCompanionRequestChunk', {'id': transfer_id, 'offset': 1, 'base64': 'eA=='})[0] == 409
        assert request('appendCompanionRequestChunk', {'id': transfer_id, 'offset': -1, 'base64': 'eA=='})[0] == 400
        assert request('appendCompanionRequestChunk', {'id': transfer_id, 'offset': 0, 'base64': 'eB=='})[0] == 400
        for offset in range(0, len(data), transfer['chunkBytes']):
            chunk = data[offset:offset + transfer['chunkBytes']]
            appended = call('appendCompanionRequestChunk', {
                'id': transfer_id, 'offset': offset, 'base64': base64.b64encode(chunk).decode(),
            })
            assert appended['nextOffset'] == offset + len(chunk)
        result = request('commitCompanionRequestTransfer', {'id': transfer_id})
        assert request('commitCompanionRequestTransfer', {'id': transfer_id})[0] == 404
        return result

    status, result = upload({'action': 'getCompanionCapabilities', 'payload': {'padding': 'x' * (90 * 1024)}})
    assert status == 200 and result['protocolVersion'] == 1
    assert upload({'action': 'getCompanionToken'})[0] == 403
    assert upload({'action': 'beginCompanionRequestTransfer', 'payload': {'totalBytes': 1}})[0] == 403
    assert upload({'action': 'sendAcpPrompt', 'payload': []})[0] == 400
    assert request('stageChatAttachments', {'chatId': 'not-a-real-chat', 'items': [{
        'name': 'secret', 'kind': 'file', 'path': '/private/secret', 'dataBase64': 'eA==',
    }]})[0] == 403

    print('PASS auth on every API, origin rejection, capabilities/settings, strict transfer validation, large envelope, commit once, final allowlist, bytes-only staging')


if __name__ == '__main__':
    main()
