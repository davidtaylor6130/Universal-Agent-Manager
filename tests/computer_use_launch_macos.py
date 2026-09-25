"""Exercise the packaged macOS Computer Use relay without requesting permissions or input."""
import json
import pathlib
import subprocess
import sys
import tempfile
import os

app = pathlib.Path(sys.argv[1]).resolve()
executable = app / 'Contents/MacOS/universal_agent_manager'
helper = app / 'Contents/Frameworks/UAM Computer Use.app'
assert helper.is_dir()
with tempfile.TemporaryDirectory(prefix='uam-cu-test-') as root:
    env = {**os.environ, 'UAM_DATA_DIR': root}
    def run(arguments, data=''):
        result = subprocess.run([str(executable), '--uam-computer-use-mcp', *arguments],
                                input=data, text=True, capture_output=True, env=env, timeout=60)
        assert result.returncode == 0, result.stderr
        return [json.loads(line) for line in result.stdout.splitlines() if line]

    status = run(['--settings-action', 'check'])[0]
    assert set(status) == {'screenRecording', 'accessibility'}
    requests = [
        {'jsonrpc': '2.0', 'id': 1, 'method': 'initialize', 'params': {
            'protocolVersion': '2024-11-05', 'capabilities': {},
            'clientInfo': {'name': 'relay-test', 'version': '1'},
            'padding': 'x' * 200000}},
        {'jsonrpc': '2.0', 'id': 2, 'method': 'tools/list', 'params': {}},
    ]
    replies = run(['--chat-id', 'isolated-relay-check'],
                  ''.join(json.dumps(request) + '\n' for request in requests))
    by_id = {reply['id']: reply for reply in replies}
    assert 'result' in by_id[1]
    assert {tool['name'] for tool in by_id[2]['result']['tools']} == {'computer_observe', 'computer_action'}
print('PASS: independent settings check, MCP round trip, large input and EOF drain')
