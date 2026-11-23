#!/usr/bin/env python3
"""Scripted ACP agent for UAM's Custom ACP agent tests.

A real stdio process that speaks newline-delimited JSON-RPC 2.0 the way an
Agent Client Protocol agent does. It is deterministic: what it does depends only
on the prompt text and its own command-line arguments.

Prompt scripts (matched on the first word of the prompt, case-insensitive):
  hello       streams three agent_message_chunk updates, then ends the turn.
  permission  reports a pending tool call, asks session/request_permission and
              reports the outcome the client selected before ending the turn.
  wait        streams one chunk, then waits for session/cancel and ends the
              turn with stopReason "cancelled".
  argv        replies with its own arguments and the UAM_FIXTURE_ECHO
              environment value as JSON, so a test can check exact delivery.
Anything else is echoed back in one chunk.

Options:
  --name NAME         agentInfo.title reported by initialize.
  --models            report two models and two modes from session/new.
  --exit-code N       print one line on stderr and exit with N before reading.
Standard library only; run with `python3 -I` so nothing is imported from the
working directory.
"""
from __future__ import annotations

import json
import os
import sys
import threading

PROTOCOL_VERSION = 1


class Agent:
    def __init__(self, argv: list[str]) -> None:
        self.argv = argv
        self.title = 'Scripted ACP fixture'
        self.report_models = False
        self.exit_code = None
        index = 0
        while index < len(argv):
            arg = argv[index]
            if arg == '--name' and index + 1 < len(argv):
                self.title = argv[index + 1]
                index += 2
                continue
            if arg == '--models':
                self.report_models = True
            elif arg == '--exit-code' and index + 1 < len(argv):
                self.exit_code = int(argv[index + 1])
                index += 2
                continue
            index += 1
        self.write_lock = threading.Lock()
        self.sessions: set[str] = set()
        self.next_session = 1
        self.next_request = 1000
        self.cancelled = threading.Event()
        # Client responses to agent->client requests, keyed by request id.
        self.responses: dict[int, dict] = {}
        self.response_ready = threading.Condition()

    # -- transport -----------------------------------------------------------------
    def send(self, message: dict) -> None:
        line = json.dumps(message, separators=(',', ':'))
        with self.write_lock:
            sys.stdout.write(line + '\n')
            sys.stdout.flush()

    def result(self, request_id, value) -> None:
        self.send({'jsonrpc': '2.0', 'id': request_id, 'result': value})

    def error(self, request_id, code: int, message: str) -> None:
        self.send({'jsonrpc': '2.0', 'id': request_id, 'error': {'code': code, 'message': message}})

    def update(self, session_id: str, update: dict) -> None:
        self.send({'jsonrpc': '2.0', 'method': 'session/update', 'params': {'sessionId': session_id, 'update': update}})

    def chunk(self, session_id: str, text: str) -> None:
        self.update(session_id, {'sessionUpdate': 'agent_message_chunk', 'content': {'type': 'text', 'text': text}})

    def request(self, method: str, params: dict) -> dict:
        with self.response_ready:
            request_id = self.next_request
            self.next_request += 1
        self.send({'jsonrpc': '2.0', 'id': request_id, 'method': method, 'params': params})
        with self.response_ready:
            while request_id not in self.responses:
                self.response_ready.wait()
            return self.responses.pop(request_id)

    # -- protocol ------------------------------------------------------------------
    def initialize(self, request_id, params) -> None:
        if not isinstance(params, dict) or not isinstance(params.get('protocolVersion'), int):
            self.error(request_id, -32602, 'initialize requires an integer protocolVersion')
            return
        self.result(request_id, {
            'protocolVersion': PROTOCOL_VERSION,
            'agentCapabilities': {
                'loadSession': False,
                'promptCapabilities': {'image': False, 'audio': False, 'embeddedContext': False},
                'mcpCapabilities': {'http': False, 'sse': False},
            },
            'agentInfo': {'name': 'uam-scripted-acp-fixture', 'title': self.title, 'version': '1.0.0'},
            'authMethods': [],
        })

    def new_session(self, request_id, params) -> None:
        if not isinstance(params, dict) or not isinstance(params.get('cwd'), str) or not params['cwd']:
            self.error(request_id, -32602, 'session/new requires cwd')
            return
        if not isinstance(params.get('mcpServers'), list):
            self.error(request_id, -32602, 'session/new requires an mcpServers array')
            return
        session_id = f'fixture-session-{self.next_session}'
        self.next_session += 1
        self.sessions.add(session_id)
        result: dict = {'sessionId': session_id}
        if self.report_models:
            result['modes'] = {'currentModeId': 'ask', 'availableModes': [
                {'id': 'ask', 'name': 'Ask'}, {'id': 'code', 'name': 'Code'}]}
            result['models'] = {'currentModelId': 'fixture-small', 'availableModels': [
                {'modelId': 'fixture-small', 'name': 'Fixture small'},
                {'modelId': 'fixture-large', 'name': 'Fixture large'}]}
        self.result(request_id, result)

    def prompt(self, request_id, params) -> None:
        session_id = params.get('sessionId') if isinstance(params, dict) else None
        if session_id not in self.sessions:
            self.error(request_id, -32602, 'Unknown sessionId')
            return
        blocks = params.get('prompt')
        if not isinstance(blocks, list):
            self.error(request_id, -32602, 'session/prompt requires a prompt array')
            return
        text = ' '.join(block.get('text', '') for block in blocks if isinstance(block, dict) and block.get('type') == 'text').strip()
        self.cancelled.clear()
        # Prompts run on their own thread so session/cancel and permission responses keep arriving.
        threading.Thread(target=self.run_prompt, args=(request_id, session_id, text), daemon=True).start()

    def run_prompt(self, request_id, session_id: str, text: str) -> None:
        command = text.split()[0].lower() if text else ''
        if command == 'hello':
            for part in ('Hello', ' from the', ' scripted agent.'):
                self.chunk(session_id, part)
            self.result(request_id, {'stopReason': 'end_turn'})
        elif command == 'permission':
            tool_call_id = 'fixture-tool-1'
            tool_call = {'toolCallId': tool_call_id, 'title': 'Write notes.txt', 'kind': 'edit', 'status': 'pending'}
            self.update(session_id, {'sessionUpdate': 'tool_call', **tool_call})
            response = self.request('session/request_permission', {
                'sessionId': session_id,
                'toolCall': tool_call,
                'options': [
                    {'optionId': 'allow', 'name': 'Allow once', 'kind': 'allow_once'},
                    {'optionId': 'reject', 'name': 'Reject', 'kind': 'reject_once'},
                ],
            })
            outcome = (response.get('result') or {}).get('outcome') or {}
            selected = outcome.get('optionId') if outcome.get('outcome') == 'selected' else None
            if outcome.get('outcome') == 'cancelled' or self.cancelled.is_set():
                self.update(session_id, {'sessionUpdate': 'tool_call_update', 'toolCallId': tool_call_id, 'status': 'failed'})
                self.result(request_id, {'stopReason': 'cancelled'})
                return
            allowed = selected == 'allow'
            self.update(session_id, {'sessionUpdate': 'tool_call_update', 'toolCallId': tool_call_id,
                                     'status': 'completed' if allowed else 'failed'})
            self.chunk(session_id, 'Permission granted.' if allowed else 'Permission denied.')
            self.result(request_id, {'stopReason': 'end_turn'})
        elif command == 'wait':
            self.chunk(session_id, 'Working')
            self.cancelled.wait(timeout=30)
            self.result(request_id, {'stopReason': 'cancelled' if self.cancelled.is_set() else 'end_turn'})
        elif command == 'argv':
            self.chunk(session_id, json.dumps({'argv': self.argv, 'echo': os.environ.get('UAM_FIXTURE_ECHO')}))
            self.result(request_id, {'stopReason': 'end_turn'})
        else:
            self.chunk(session_id, text or '(empty prompt)')
            self.result(request_id, {'stopReason': 'end_turn'})

    def cancel(self, params) -> None:
        if isinstance(params, dict) and params.get('sessionId') in self.sessions:
            self.cancelled.set()

    def dispatch(self, message: dict) -> None:
        method = message.get('method')
        request_id = message.get('id')
        params = message.get('params')
        if method is None:
            # A response to one of the agent's own requests.
            if isinstance(request_id, int):
                with self.response_ready:
                    self.responses[request_id] = message
                    self.response_ready.notify_all()
            return
        if method == 'initialize':
            self.initialize(request_id, params)
        elif method == 'session/new':
            self.new_session(request_id, params)
        elif method == 'session/prompt':
            self.prompt(request_id, params)
        elif method == 'session/cancel':
            self.cancel(params)
        elif request_id is not None:
            self.error(request_id, -32601, f'Method not found: {method}')

    def run(self) -> int:
        if self.exit_code is not None:
            sys.stderr.write('scripted fixture: exiting before the handshake as requested\n')
            sys.stderr.flush()
            return self.exit_code
        for line in sys.stdin:
            line = line.strip()
            if not line:
                continue
            try:
                message = json.loads(line)
            except json.JSONDecodeError:
                self.send({'jsonrpc': '2.0', 'id': None, 'error': {'code': -32700, 'message': 'Parse error'}})
                continue
            if isinstance(message, dict):
                self.dispatch(message)
        return 0


if __name__ == '__main__':
    sys.exit(Agent(sys.argv[1:]).run())
