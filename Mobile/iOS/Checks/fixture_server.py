#!/usr/bin/env python3
"""Isolated deterministic HTTP host for native UI journeys; never starts a provider."""
import argparse
import base64
import json
import threading
import time
import uuid
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

TOKEN = 'uam-isolated-ui-test-token'
NOW = datetime.now(timezone.utc).isoformat().replace('+00:00', 'Z')
LOCK = threading.RLock()
MESSAGES = {
    'mobile-root': [
        {'role': 'user', 'content': 'Improve the mobile experience.', 'createdAt': NOW},
        {'role': 'assistant', 'content': 'The phone connects to your main UAM computer.\n\nI am checking the native composer, history and agent controls while preserving your existing provider setup.\n\n```swift\nlet connection = try CompanionClient(baseURL: url, token: token)\n```', 'createdAt': NOW,
         'toolCalls': [{'id': 'tool-1', 'title': 'Read companion configuration', 'kind': 'read', 'status': 'completed', 'content': 'Configuration checked; no production data was read.'},
                       {'id': 'tool-child', 'title': 'Review attachment persistence', 'isSubAgent': True, 'subAgentId': 'mobile-child', 'subAgentTitle': 'Attachment review', 'currentActivity': 'Checking original bytes', 'status': 'running', 'content': ''}]}],
    'mobile-child': [{'role': 'assistant', 'content': 'Original files remain on the main computer. The phone loads bounded previews.\n\n| Check | Result |\n| :--- | :--- |\n| Original bytes | Preserved |\n| Mobile preview | Bounded |', 'createdAt': NOW}],
    'mobile-pinned': [{'role': 'assistant', 'content': 'A saved conversation ready to continue.', 'createdAt': NOW}],
    'mobile-snoozed': []}


def chat(chat_id, title, **extra):
    return {'id': chat_id, 'title': title, 'folderId': 'mobile-project', 'providerId': 'codex-cli', 'executionHostId': 'local', 'modelId': 'fixture-model', 'workspaceDirectory': '/isolated/mobile-project', 'workspaceBranchName': 'mobile/native-ios', 'createdAt': NOW, 'updatedAt': NOW, 'messageCount': len(MESSAGES.get(chat_id, [])), 'acpSession': {'processing': False, 'currentModelId': 'fixture-model', 'availableModels': [{'id': 'fixture-model', 'name': 'Fixture model'}], 'availableModes': [], 'configOptions': [], 'toolCalls': [], 'turnEvents': [], 'readySinceLastSelect': False}, **extra}


CHATS = [chat('mobile-root', 'Native mobile experience'), chat('mobile-child', 'Attachment review', parentChatId='mobile-root', branchRootChatId='mobile-root', branchFromMessageIndex=-1), chat('mobile-pinned', 'Pinned conversation', pinned=True), chat('mobile-snoozed', 'Snoozed conversation', snoozedUntilMs=int(time.time() * 1000) + 86400000)]
CHATS[0]['acpSession']['readySinceLastSelect'] = True
CHATS[1]['acpSession']['processing'] = True
REVISION = 1
TRANSFERS = {}
TOOL_CONTENTS = {}
MESSAGE_BRANCH_DELAY = 0.0
FOLDERS = [{'id': 'mobile-project', 'title': 'Universal Agent Manager', 'directory': '/isolated/mobile-project', 'executionHostId': 'local'}]
COLLECTIONS = None
EXPIRY_CHECK = False
EXPIRY_SNAPSHOT_SENT = False


def configure_organisation():
    """Replace only this process's disposable catalogue; no filesystem/provider access."""
    global COLLECTIONS
    FOLDERS[:] = [
        {'id': 'org-workspace', 'title': 'Product Workspace', 'directory': '/isolated/product', 'executionHostId': 'local', 'collapsed': True},
        {'id': 'org-empty-workspace', 'title': 'Empty Workspace', 'directory': '/isolated/empty', 'executionHostId': 'local', 'collapsed': False},
        {'id': 'org-ungrouped', 'title': 'Personal Workspace', 'directory': '/isolated/personal', 'executionHostId': 'local', 'collapsed': False}]
    COLLECTIONS = [
        {'id': 'org-collection', 'name': 'Work Collection', 'collapsed': False, 'references': [
            {'id': 'org-reference-product', 'type': 'workspace-folder', 'target': 'org-workspace', 'label': 'Product Workspace'},
            {'id': 'org-reference-empty', 'type': 'workspace-folder', 'target': 'org-empty-workspace', 'label': 'Empty Workspace'}]},
        {'id': 'org-empty-collection', 'name': 'Empty Collection', 'collapsed': True, 'references': []}]
    MESSAGES.clear()
    for identity, content in [
        ('org-root', 'Root conversation fixture.'),
        ('org-child', 'Working child conversation fixture.'),
        ('org-pinned', 'Pinned conversation fixture.'),
        ('org-snoozed', 'Snoozed conversation fixture.'),
        ('org-opencode', 'OpenCode conversation fixture.'),
        ('org-unsorted', 'Unsorted conversation fixture.')]:
        MESSAGES[identity] = [{'role': 'assistant', 'content': content, 'createdAt': NOW}]
    CHATS[:] = [
        chat('org-root', 'Working family', folderId='org-workspace', interactionAt=NOW, settledAt=NOW),
        chat('org-child', 'Needle child review', folderId='org-workspace', parentChatId='org-root', branchRootChatId='org-root'),
        chat('org-pinned', 'Pinned workspace chat', folderId='org-workspace', pinned=True, interactionAt=NOW, settledAt=NOW),
        chat('org-snoozed', 'Snoozed workspace chat', folderId='org-workspace', snoozedUntilMs=int(time.time() * 1000) + 86400000, interactionAt=NOW, settledAt=NOW),
        chat('org-opencode', 'OpenCode workspace chat', folderId='org-ungrouped', providerId='opencode-cli', modelId='normal', interactionAt=NOW, settledAt=NOW),
        chat('org-unsorted', 'Unsorted conversation', folderId='missing-workspace', interactionAt=NOW, settledAt=NOW)]
    directories = {folder['id']: folder['directory'] for folder in FOLDERS}
    for row in CHATS: row['workspaceDirectory'] = directories.get(row['folderId'], '/isolated/unknown')
    CHATS[1]['acpSession']['processing'] = True
    CHATS[4]['acpSession']['currentModelId'] = 'normal'
    CHATS[4]['acpSession']['availableModels'] = [{'id': 'normal', 'name': 'normal'}]


def configure_expiry_check():
    """Start the deadline only when the UI requests its first snapshot."""
    global EXPIRY_CHECK, EXPIRY_SNAPSHOT_SENT
    configure_organisation()
    EXPIRY_CHECK = True
    EXPIRY_SNAPSHOT_SENT = False


def configure_message_branches():
    """Saved prompt alternatives and real serializer-shaped execute tool payloads."""
    global COLLECTIONS
    FOLDERS[:] = [{'id': 'mb-workspace', 'title': 'Branch Workspace', 'directory': '/isolated/message-branches', 'executionHostId': 'local', 'collapsed': False}]
    COLLECTIONS = []
    def message(role, content, **extra):
        return {'role': role, 'content': content, 'createdAt': NOW, **extra}
    prompt = message('user', 'Review this exact saved prompt.')
    later_prompt = message('user', 'Inspect the alternate change.')
    tools = []
    for identity, command, result, deferred in [
        ('mb-status', 'git status --short', 'fixture working tree clean', False),
        ('mb-diff', 'git diff --stat', 'fixture diff summary', False),
        ('mb-deferred', 'git log -1 --oneline', 'fixture full deferred output marker', True)]:
        content = 'Arguments:\n' + json.dumps({'command': command}) + '\n\nResult:\n' + result
        TOOL_CONTENTS[identity] = content
        tool = {'id': identity, 'title': '', 'subAgentTitle': '', 'subAgentId': '', 'kind': 'execute', 'status': 'completed', 'approvalStatus': '', 'isSubAgent': False, 'contentDeferred': deferred}
        if deferred: tool['contentDigest'] = 'fixture-' + identity
        else: tool['content'] = content
        tools.append(tool)
    MESSAGES.clear()
    MESSAGES.update({
        'mb-root': [prompt, message('assistant', 'Original root response.', toolCalls=tools)],
        'mb-one': [dict(prompt), message('assistant', 'First branch response.'), later_prompt, message('assistant', 'First branch follow-up.')],
        'mb-two': [dict(prompt), message('assistant', 'Second branch response.')],
        'mb-deep': [dict(prompt), message('assistant', 'First branch response.'), dict(later_prompt), message('assistant', 'Deeper branch response.')]})
    MESSAGES['mb-two'].extend(message('assistant', 'Saved long branch reply ' + str(index)) for index in range(140))
    CHATS[:] = [
        chat('mb-root', 'Pinned message branches', folderId='mb-workspace', workspaceDirectory='/isolated/message-branches', pinned=True, interactionAt=NOW, settledAt=NOW),
        chat('mb-one', 'First prompt alternative', folderId='mb-workspace', parentChatId='mb-root', branchRootChatId='mb-root', branchFromMessageIndex=0, branchMessageEdited=False, interactionAt=NOW, settledAt=NOW),
        chat('mb-two', 'Second prompt alternative', folderId='mb-workspace', parentChatId='mb-root', branchRootChatId='mb-root', branchFromMessageIndex=0, branchMessageEdited=True, interactionAt=NOW, settledAt=NOW),
        chat('mb-deep', 'Deeper prompt alternative', folderId='mb-workspace', parentChatId='mb-one', branchRootChatId='mb-root', branchFromMessageIndex=2, branchMessageEdited=False, interactionAt=NOW, settledAt=NOW)]


SETTINGS = {'executionHosts': [{'id': 'local', 'label': 'Test Mac', 'platform': 'macos', 'runnerStatus': 'ready'}], 'defaultNewChatProviderId': 'codex-cli', 'providerChatDefaults': {}, 'memoryEnabledDefault': True, 'memoryLevelDefault': 'strict', 'memoryIdleDelaySeconds': 120, 'memoryRecallBudgetBytes': 32768, 'backupLogRetentionDays': 30, 'httpCacheClearIntervalDays': 7, 'favoriteUamAgentIds': []}
ALLOWED = ['getChatRuntimeMetrics', 'getInitialState', 'getChatMessages', 'getToolCallContent', 'getProviderAllowances', 'getUsageHistory', 'createSession', 'stageChatAttachments', 'sendAcpPrompt', 'setChatPinned', 'renameSession', 'deleteSession', 'setChatSnoozedUntil', 'listUamAgents', 'setChatUamAgent', 'setChatUamControlEnabled', 'setChatMemoryEnabled', 'setChatProvider', 'setChatModel', 'cancelAcpTurn', 'resolveAcpPermission', 'resolveAcpUserInput', 'getCompanionCapabilities', 'beginCompanionRequestTransfer', 'appendCompanionRequestChunk', 'commitCompanionRequestTransfer']
ALLOWED += ['setProviderChatDefaults', 'setMemorySettings', 'setUamAgentPreferences', 'setStorageSettings', 'setVcsSettings', 'setProviderAvailability', 'getProviderMcpStatus', 'setProviderMcpConfiguration', 'openNativeSessionChat', 'acknowledgeChatAttention']


def handle(action, payload):
    global REVISION, EXPIRY_SNAPSHOT_SENT
    if action == 'getCompanionCapabilities':
        return {'protocolVersion': 1, 'actions': ALLOWED, 'features': {'requestChunks': True}, 'limits': {'maxRequestBytes': 32 * 1024 * 1024, 'requestChunkBytes': 32768}}
    if action == 'setProviderChatDefaults':
        if 'defaultProviderId' in payload: SETTINGS['defaultNewChatProviderId'] = payload['defaultProviderId']
        for provider, patch in payload.get('defaults', {}).items(): SETTINGS['providerChatDefaults'].setdefault(provider, {}).update(patch)
        REVISION += 1; return {}
    if action == 'setMemorySettings':
        names = {'enabledDefault': 'memoryEnabledDefault', 'levelDefault': 'memoryLevelDefault', 'idleDelaySeconds': 'memoryIdleDelaySeconds', 'recallBudgetBytes': 'memoryRecallBudgetBytes', 'workerBindings': 'memoryWorkerBindings'}
        for key, value in payload.items(): SETTINGS[names.get(key, key)] = value
        REVISION += 1; return {}
    if action in ['setStorageSettings', 'setUamAgentPreferences', 'setVcsSettings']:
        SETTINGS.update(payload); REVISION += 1; return {}
    if action == 'getProviderMcpStatus': return {'executionHostId': 'local', 'providers': []}
    if action == 'openNativeSessionChat': return {'chatId': 'mobile-child'}
    if action == 'acknowledgeChatAttention': return {}
    if action == 'getInitialState':
        if EXPIRY_CHECK:
            if EXPIRY_SNAPSHOT_SENT:
                raise ValueError('The isolated expiry fixture is intentionally unavailable after its first snapshot.')
            next(row for row in CHATS if row['id'] == 'org-snoozed')['snoozedUntilMs'] = int(time.time() * 1000) + 12000
            EXPIRY_SNAPSHOT_SENT = True
        return {'bootId': 'fixture-boot', 'stateRevision': REVISION, 'appVersion': 'Isolated test host', 'chats': CHATS, 'folders': FOLDERS, **({'resourceCollections': COLLECTIONS} if COLLECTIONS is not None else {}), 'providers': [{'id': x, 'name': {'codex-cli':'Codex','claude-cli':'Claude','gemini-cli':'Gemini','opencode-cli':'OpenCode','copilot-cli':'GitHub Copilot','antigravity-cli':'Antigravity'}[x], 'supportsStructured': True} for x in ['codex-cli', 'claude-cli', 'gemini-cli', 'opencode-cli', 'copilot-cli', 'antigravity-cli']], 'providerModelCatalogs': [{'providerId': 'codex-cli', 'executionHostId': 'local', 'availableModels': [{'id': 'fixture-model', 'name': 'Fixture model'}]}], 'settings': SETTINGS, 'selectedChatId': None}
    if action == 'getChatRuntimeMetrics':
        return {'chatId': payload['chatId'], 'processing': True, 'elapsedMs': 127000, 'turnSerial': 1}
    if action == 'getChatMessages':
        if MESSAGE_BRANCH_DELAY > 0 and payload.get('chatId') == 'mb-two' and 'before' in payload:
            time.sleep(MESSAGE_BRANCH_DELAY)
        rows = MESSAGES.get(payload['chatId'], [])
        end = min(payload.get('before', len(rows)), len(rows)); start = max(0, end - payload.get('limit', 50))
        return {'chatId': payload['chatId'], 'messages': rows[start:end], 'startIndex': start, 'totalCount': len(rows), 'messagesDigest': str(REVISION), 'unchanged': False}
    if action == 'getToolCallContent':
        if payload.get('toolCallId') in TOOL_CONTENTS:
            content = TOOL_CONTENTS[payload['toolCallId']]
            start = max(0, payload.get('offset', 0)); end = min(len(content), start + 32768)
            return {'content': content[start:end], 'offset': start, 'nextOffset': end, 'hasMore': end < len(content)}
        return {'content': 'Verified fixture tool output. No real provider ran.', 'offset': 0, 'nextOffset': 52, 'hasMore': False}
    if action == 'listUamAgents':
        return {'agents': [{'id': 'build', 'description': 'Build and edit'}, {'id': 'read-only', 'description': 'Inspect without editing'}], 'errors': []}
    if action == 'createSession':
        identity = 'created-' + uuid.uuid4().hex[:8]
        MESSAGES[identity] = []
        CHATS.insert(0, chat(identity, 'New mobile chat', providerId=payload.get('providerId', 'codex-cli')))
        REVISION += 1
        return {'chatId': identity}
    if action == 'stageChatAttachments':
        result = []
        for item in payload['items']:
            data = base64.b64decode(item['dataBase64'], validate=True)
            identity = uuid.uuid4().hex
            result.append({'id': identity, 'name': item['name'], 'type': item['kind'], 'mimeType': item['mimeType'], 'size': len(data), 'path': '.UAM/attachments/' + identity})
        return {'attachments': result}
    if action == 'sendAcpPrompt':
        rows = MESSAGES[payload['chatId']]
        rows.extend([{'role': 'user', 'content': payload['text'], 'attachments': payload.get('attachments', []), 'createdAt': NOW}, {'role': 'assistant', 'content': 'The isolated host received your message.', 'createdAt': NOW, 'processingTimeMs': 1234}])
        REVISION += 1
        return {}
    if action == 'beginCompanionRequestTransfer':
        identity = uuid.uuid4().hex; TRANSFERS[identity] = {'total': payload['totalBytes'], 'data': bytearray()}
        return {'id': identity, 'totalBytes': payload['totalBytes'], 'chunkBytes': 32768, 'nextOffset': 0}
    if action == 'appendCompanionRequestChunk':
        transfer = TRANSFERS[payload['id']]
        if payload['offset'] != len(transfer['data']): raise ValueError('Invalid offset')
        transfer['data'].extend(base64.b64decode(payload['base64'], validate=True))
        return {'id': payload['id'], 'nextOffset': len(transfer['data']), 'done': len(transfer['data']) == transfer['total']}
    if action == 'commitCompanionRequestTransfer':
        transfer = TRANSFERS.pop(payload['id'])
        if len(transfer['data']) != transfer['total']: raise ValueError('Incomplete upload')
        request = json.loads(transfer['data']); return handle(request['action'], request.get('payload', {}))
    if action == 'getProviderAllowances':
        return {'executionHostId': 'local', 'refreshing': False, 'refreshAfterMs': 0, 'providers': [{'providerId': 'codex-cli', 'status': 'ready', 'windows': [{'id': 'weekly', 'label': 'Weekly', 'usedPercent': 58, 'resetsAt': int(time.time()) + 86400}], 'limits': {'updatedAt': int(time.time() * 1000)}}]}
    if action == 'getUsageHistory':
        total = {'totalTokens': 1200000, 'reportedCostUsd': 1.5, 'estimatedCostUsd': 2.75, 'unpricedTokens': 0}
        return {'version': 3, 'totals': total, 'complete': True, 'collecting': False, 'providers': [{'providerId': 'codex-cli', **total}], 'days': [{'startMs': int(time.time() * 1000) - i * 86400000, 'reportedCostUsd': 0.1 * i, 'estimatedCostUsd': 0.1, 'totalTokens': i * 10000} for i in range(7)]}
    if action == 'deleteSession':
        CHATS[:] = [row for row in CHATS if row['id'] != payload['chatId']]; REVISION += 1; return {}
    if action.startswith('setChat') or action in ['renameSession', 'cancelAcpTurn', 'resolveAcpPermission', 'resolveAcpUserInput']:
        row = next(row for row in CHATS if row['id'] == payload['chatId'])
        for key in ['pinned', 'title', 'snoozedUntilMs', 'modelId', 'providerId']:
            if key in payload: row[key] = payload[key]
        if action == 'setChatSnoozedUntil': row['snoozedUntilMs'] = payload.get('untilMs', payload.get('snoozedUntilMs', 0))
        if action == 'setChatUamControlEnabled': row['uamControlEnabled'] = payload['enabled']
        if action == 'setChatMemoryEnabled': row['memoryEnabled'] = payload['enabled']
        if action == 'setChatUamAgent': row['uamAgentId'] = payload['agentId']
        if action == 'cancelAcpTurn': row['acpSession']['processing'] = False
        REVISION += 1; return {}
    raise ValueError('Unsupported fixture action: ' + action)


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        if self.path != '/api' or self.headers.get('Authorization') != 'Bearer ' + TOKEN:
            self.send_error(401); return
        try:
            length = int(self.headers.get('Content-Length', '0'))
            if not 0 < length <= 65536: raise ValueError('Request wire limit')
            value = json.loads(self.rfile.read(length))
            with LOCK: result = handle(value['action'], value.get('payload', {}))
            data = json.dumps(result).encode()
            self.send_response(200); self.send_header('Content-Type', 'application/json'); self.send_header('Content-Length', str(len(data))); self.end_headers(); self.wfile.write(data)
        except Exception as error:
            data = json.dumps({'error': str(error)}).encode()
            self.send_response(400); self.send_header('Content-Type', 'application/json'); self.send_header('Content-Length', str(len(data))); self.end_headers(); self.wfile.write(data)

    def log_message(self, fmt, *args):
        pass


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', type=int, default=None)
    parser.add_argument('--organisation', action='store_true', help='Serve isolated collection/workspace organisation journeys (default port 58966).')
    parser.add_argument('--expiry-check', action='store_true', help='Serve one organisation snapshot with a 12-second snooze, then fail snapshot reads (default port 58967).')
    parser.add_argument('--message-branches', action='store_true', help='Serve saved prompt branches and command tools (default port 58968).')
    parser.add_argument('--message-branch-delay', type=float, default=0.0, help='Delay older mb-two history pages by 0–10 seconds for cancellation checks.')
    parser.add_argument('--catalogue-size', type=int, default=None)
    args = parser.parse_args()
    if not 0 <= args.message_branch_delay <= 10: parser.error('message-branch-delay must be between 0 and 10 seconds')
    if args.message_branch_delay and not args.message_branches: parser.error('message-branch-delay requires --message-branches')
    MESSAGE_BRANCH_DELAY = args.message_branch_delay
    if args.message_branches: configure_message_branches()
    elif args.expiry_check: configure_expiry_check()
    elif args.organisation: configure_organisation()
    if args.port is None: args.port = 58968 if args.message_branches else 58967 if args.expiry_check else 58966 if args.organisation else 58962
    if args.catalogue_size is None: args.catalogue_size = len(CHATS)
    if not len(CHATS) <= args.catalogue_size <= 10000:
        parser.error('catalogue-size must be between the standard fixture size and 10000')
    for index in range(len(CHATS), args.catalogue_size):
        CHATS.append(chat('quiet-' + str(index), 'Settled conversation ' + str(index),
                          interactionAt=NOW, settledAt=NOW, attentionRevision=''))
    print('Isolated UAM fixture listening on loopback port', args.port, flush=True)
    ThreadingHTTPServer(('127.0.0.1', args.port), Handler).serve_forever()
