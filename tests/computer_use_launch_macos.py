"""Exercise the packaged macOS Computer Use relay without requesting permissions or input."""
import atexit
import json
import pathlib
import subprocess
import sys
import tempfile
import time
import os

# macOS keeps a ~0.5 GB code_sign_clone copy of each launched bundle (#684).
atexit.register(subprocess.run, [sys.executable, str(pathlib.Path(__file__).resolve().parents[1] / 'scripts/prune_code_sign_clones.py')])

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

    # The helper is copied once; later starts reuse it and leave no staging copies.
    install = pathlib.Path(root) / 'Computer Use'
    staged = lambda: sorted(path.name for path in install.glob('.staging-*'))
    installed = install / 'UAM Computer Use.app'
    assert staged() == [], staged()
    assert (install / '.receipt').is_file()
    identity = installed.stat().st_ino
    run(['--settings-action', 'check'])
    assert installed.stat().st_ino == identity, 'an unchanged helper was copied again'

    # A copy left by a dead installer is removed by the next start; a live owner's copy is kept.
    (install / '.staging-999999999' / 'Contents').mkdir(parents=True)
    (install / '.staging-1' / 'Contents').mkdir(parents=True)
    run(['--settings-action', 'check'])
    assert staged() == ['.staging-1'], staged()
    (install / '.staging-1' / 'Contents').rmdir()
    (install / '.staging-1').rmdir()

    # A caller killed mid-install (a provider's MCP startup timeout) neither leaks a copy nor makes the next start copy again.
    (install / '.receipt').unlink()
    killed = subprocess.Popen([str(executable), '--uam-computer-use-mcp', '--settings-action', 'check'],
                              stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
    time.sleep(0.3)
    killed.kill()
    killed.wait()
    run(['--settings-action', 'check'])
    assert staged() == [], staged()
    assert (install / '.receipt').is_file()
print('PASS: independent settings check, MCP round trip, large input, EOF drain and single helper install')
