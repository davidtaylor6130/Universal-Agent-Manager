#!/usr/bin/env python3
"""Run compiled bootstrap shell commands only against disposable fixture files.

No SSH, app or product runner is executed. The candidate/old "helpers" are shell
fixtures with deterministic identity, stop/start and injected refusal controls.
"""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    native_input = parser.add_mutually_exclusive_group(required=True)
    native_input.add_argument('--native-script-bundle', type=Path)
    native_input.add_argument('--native-binary', type=Path)
    args = parser.parse_args()
    bundle = json.loads(args.native_script_bundle.read_text() if args.native_script_bundle else
                        subprocess.check_output([str(args.native_binary.resolve()), '--dump-scripts'], text=True, timeout=30))
    commands = bundle['linux']
    protocol = str(bundle['protocol'])
    fingerprint = 'b' * 64
    passed = 0
    python = sys.executable
    with tempfile.TemporaryDirectory(prefix='uam-linux-metadata-') as temp:
        root = Path(temp)
        home = root / 'home'
        tools = root / 'bin'
        tools.mkdir()
        directory = home / 'r/4.5.7'
        binary = directory / 'uam-runner'
        candidate = directory / 'uam-runner.tmp-metadata'
        backup = directory / 'uam-runner.rollback-metadata'
        marker = directory / 'uam-runner.activation-metadata'
        metadata = directory / 'uam-runner.metadata-metadata'
        journal = home / 'r/install.transaction'
        prior_manifest = b'\x00\xff\r\nprior manifest'
        prior_checksum = b'\xfehistorical checksum\x00\r\n'
        old = b'#!/bin/sh\ncase "$1" in stop) test "$UAM_METADATA_MODE" != rollback-busy;; start) uam-check-restart;; esac\n'
        new = (f'''#!/bin/sh
identity_status() {{
  test "$UAM_METADATA_MODE" != "$1-failure" && {{ test "$UAM_METADATA_MODE" != "post-$1-failure" || test "${{0##*/}}" != uam-runner; }}
}}
case "$1" in
  stop) echo "$*" >> "$UAM_FIXTURE_HOME/stops"; if test "$UAM_METADATA_MODE" = legacy-busy; then test "$3" != "$UAM_FIXTURE_HOME/r/uam.sock"; else test "$UAM_METADATA_MODE" != busy && test "$UAM_METADATA_MODE" != rollback-busy; fi;;
  start) uam-check-no-locks || exit $?; if test "$UAM_METADATA_MODE" = pause-start; then uam-pause-start || exit $?; fi; test "$UAM_METADATA_MODE" != start-failure;;
  --version) if test "$UAM_METADATA_MODE" = version-mismatch; then echo wrong; else echo 4.5.7; fi; identity_status version;;
  --protocol-version) if test "$UAM_METADATA_MODE" = protocol-mismatch; then echo 0; else echo {protocol}; fi; identity_status protocol;;
  --source-fingerprint) if test "$UAM_METADATA_MODE" = source-mismatch; then echo wrong; else echo {fingerprint}; fi; identity_status source;;
esac
''').encode()
        digest = hashlib.sha256(new).hexdigest()
        def tool(name, text):
            path = tools / name
            path.write_text(text)
            path.chmod(0o700)
        tool('uam-check-no-locks', '#!' + python + '''
import os
for fd in (8,9):
 try: os.fstat(fd)
 except OSError: continue
 raise AssertionError('Resident inherited installation lock descriptor '+str(fd))
''')
        tool('uam-pause-start', '#!' + python + '''
import os,pathlib,time
home=pathlib.Path(os.environ['UAM_FIXTURE_HOME'])
(home/'paused').write_text('paused')
deadline=time.monotonic()+10
while not (home/'resume').exists():
 if time.monotonic()>=deadline: raise TimeoutError('Fixture start was never resumed')
 time.sleep(.01)
''')
        tool('uam-check-restart', '#!' + python + '\n' + f'''
import os,pathlib
root=pathlib.Path(os.environ['UAM_FIXTURE_HOME'])
for fd in (8,9):
 try: os.fstat(fd)
 except OSError: continue
 raise AssertionError('Prior resident inherited installation lock descriptor '+str(fd))
directory=root/'r/4.5.7'
for name,expected,prior in [('uam-runner.manifest.json','UAM_PRIOR_MANIFEST',{prior_manifest!r}),('uam-runner.sha256','UAM_PRIOR_CHECKSUM',{prior_checksum!r})]:
 path=directory/name
 assert path.read_bytes()==prior if os.environ[expected]=='1' else not path.exists()
assert (directory/'uam-runner.activation-metadata').exists()
with (root/'restarts').open('a') as output: output.write('restarted\\n')
''')
        tool('sha256sum', '#!' + python + '''
import hashlib,sys
for line in sys.stdin:
 expected,path=line.rstrip('\\n').split('  ',1)
 if hashlib.sha256(open(path,'rb').read()).hexdigest()!=expected: sys.exit(1)
''')
        tool('mv', '''#!/bin/sh
if test "$UAM_METADATA_MODE" = publication-failure; then
 for arg in "$@"; do case "$arg" in */uam-runner.sha256.new) exit 27;; esac; done
fi
if test "$UAM_METADATA_MODE" = restore-move-failure; then
 for arg in "$@"; do case "$arg" in */uam-runner.manifest.json.restore) exit 29;; esac; done
fi
exec /bin/mv "$@"
''')
        tool('cp', '''#!/bin/sh
if test "$UAM_METADATA_MODE" = restore-failure; then
 for arg in "$@"; do case "$arg" in */uam-runner.metadata-metadata/uam-runner.manifest.json) exit 28;; esac; done
fi
exec /bin/cp "$@"
''')
        tool('flock', '#!' + python + '''
import fcntl,sys
try: fcntl.flock(int(sys.argv[-1]),fcntl.LOCK_EX|fcntl.LOCK_NB)
except BlockingIOError: sys.exit(1)
''')
        tool('base64', '#!' + python + '''
import base64,sys
value=sys.stdin.buffer.read()
sys.stdout.buffer.write(base64.b64decode(value) if '-d' in sys.argv else base64.b64encode(value))
''')
        env = dict(os.environ, UAM_FIXTURE_HOME=str(home), PATH=str(tools) + ':/usr/bin:/bin', UAM_METADATA_MODE='normal')
        # The environment is private to these shell subprocesses, never the app/profile.
        raw = dict(commands)
        guard = raw['guard']
        common = raw['activate'][:raw['activate'].index('file=~/')]
        # Recovery is a finite owned operation, separate from lock acquisition.
        # Redirect only the generated remote-home references into the fixture;
        # never reassign the host process's HOME or use an installed helper path.
        scripts = {key: value.replace('a' * 64, digest).replace('$HOME', '$UAM_FIXTURE_HOME').replace('~/', '"$UAM_FIXTURE_HOME"/')
                   for key, value in raw.items()}
        def setup(first=False, manifest=True, checksum=True):
            if home.exists(): shutil.rmtree(home)
            directory.mkdir(parents=True)
            if not first:
                binary.write_bytes(old)
                binary.chmod(0o700)
            if manifest: (directory / 'uam-runner.manifest.json').write_bytes(prior_manifest)
            if checksum: (directory / 'uam-runner.sha256').write_bytes(prior_checksum)
            candidate.write_bytes(new)
            env['UAM_METADATA_MODE'] = 'normal'
            env['UAM_PRIOR_MANIFEST'] = '1' if manifest else '0'
            env['UAM_PRIOR_CHECKSUM'] = '1' if checksum else '0'
        def run(name, success=True):
            result = subprocess.run(['/bin/sh', '-c', scripts[name]], env=env, capture_output=True, text=True, timeout=20)
            assert (result.returncode == 0) == success, (name, result.returncode, result.stdout, result.stderr)
        def prior(first=False, manifest=True, checksum=True):
            assert (not binary.exists()) if first else binary.read_bytes() == old
            for name, exists, data in [('uam-runner.manifest.json', manifest, prior_manifest), ('uam-runner.sha256', checksum, prior_checksum)]:
                path = directory / name
                assert path.read_bytes() == data if exists else not path.exists(), name
        def published():
            assert binary.read_bytes() == new
            manifest = json.loads((directory / 'uam-runner.manifest.json').read_bytes())
            assert manifest == {'protocolVersion': int(protocol), 'sourceFingerprint': fingerprint}
            assert (directory / 'uam-runner.sha256').read_text() == digest + '\n'
        def clean():
            assert not marker.exists() and not backup.exists() and not metadata.exists()
        def case(name, test):
            nonlocal passed
            test()
            passed += 1
            print('PASS ' + name, flush=True)
        def commit():
            setup(); run('activate'); published(); assert marker.exists()
            run('commit'); run('rollback'); published(); clean()
        case('Linux commit publishes verified corresponding files', commit)
        def presence():
            for first in (False, True):
                for manifest in (False, True):
                    for checksum in (False, True):
                        setup(first, manifest, checksum); run('activate'); run('rollback')
                        prior(first, manifest, checksum); clean()
        case('Linux rollback preserves eight prior presence/absence combinations', presence)
        def refuse():
            for mode in ('source-mismatch', 'busy', 'version-mismatch', 'protocol-mismatch', 'start-failure', 'publication-failure'):
                setup(); env['UAM_METADATA_MODE'] = mode
                run('activate', False)
                if mode in ('source-mismatch', 'version-mismatch', 'protocol-mismatch'):
                    assert not (home / 'stops').exists(), 'Invalid candidate stopped a helper: ' + mode
                env['UAM_METADATA_MODE'] = 'normal'; run('rollback'); prior(); clean()
        case('Linux verification busy start and partial publication failures restore files', refuse)
        def query_failure():
            for query in ('source', 'version', 'protocol'):
                for prefix in ('', 'post-'):
                    setup(); env['UAM_METADATA_MODE'] = prefix + query + '-failure'
                    run('activate', False)
                    if not prefix:
                        assert not (home / 'stops').exists(), 'Failed query stopped a helper: ' + query
                    env['UAM_METADATA_MODE'] = 'normal'; run('rollback'); prior(); clean()
        case('Linux rejects failing identity queries despite expected output before and after activation', query_failure)
        def hash_refusal():
            setup(); candidate.write_bytes(b'changed bytes'); run('activate', False)
            assert not (home / 'stops').exists()
            run('rollback'); prior(); clean()
        case('Linux bad hash refuses before stop', hash_refusal)
        def lost_ack():
            setup(); run('activate'); published()
            run('rollback'); prior(); clean()
        case('Linux lost activation acknowledgement rolls back published metadata', lost_ack)
        def staged_restore():
            setup(); run('activate')
            with binary.open('rb') as previously_open_image:
                run('rollback')
                assert previously_open_image.read() == new, 'Rollback modified the previously opened installed inode.'
            prior(); clean()
        case('Linux binary rollback stages replacement without overwriting an open inode', staged_restore)
        def recovery():
            for first in (False, True):
                for manifest in (False, True):
                    for checksum in (False, True):
                        setup(first, manifest, checksum); run('activate'); published(); run('recover')
                        prior(first, manifest, checksum); clean()
                        assert not journal.exists()
                        assert (home / 'restarts').exists() == (not first)
        case('Linux interrupted journal restores eight prior states before any old helper restart', recovery)
        def legacy_recovery():
            setup()
            backup.write_bytes(old); backup.chmod(0o700)
            binary.write_bytes(new)
            marker.write_text('')
            journal.write_text('4.5.7\nmetadata\n' + protocol + '\n')
            assert not metadata.exists()
            run('recover'); prior(); clean()
            assert not journal.exists()
        case('Linux legacy binary-only recovery preserves existing sidecars without inventing metadata', legacy_recovery)
        def busy_recovery():
            setup(); run('activate'); env['UAM_METADATA_MODE'] = 'rollback-busy'
            run('rollback', False); published()
            assert marker.exists() and backup.exists() and metadata.exists()
            run('recover', False); published()
            env['UAM_METADATA_MODE'] = 'normal'; run('recover'); prior(); clean()
        case('Linux busy rollback and recovery refuse while preserving retry authority', busy_recovery)
        def restore_retry():
            setup(); run('activate'); env['UAM_METADATA_MODE'] = 'restore-failure'
            run('rollback', False)
            assert marker.exists() and metadata.exists() and backup.exists()
            env['UAM_METADATA_MODE'] = 'normal'; run('recover'); prior(); clean()
        case('Linux failed sidecar restoration retains evidence for retry', restore_retry)
        def readonly_restore_retry():
            setup()
            (directory / 'uam-runner.manifest.json').chmod(0o400)
            run('activate'); env['UAM_METADATA_MODE'] = 'restore-move-failure'
            run('rollback', False)
            scratch = metadata / 'uam-runner.manifest.json.restore'
            assert scratch.exists() and (scratch.stat().st_mode & 0o777) == 0o400
            assert marker.exists() and backup.exists()
            env['UAM_METADATA_MODE'] = 'normal'; run('recover'); prior(); clean()
        case('Linux readonly interrupted restore scratch can be retried safely', readonly_restore_retry)
        def abandoned():
            setup()
            with subprocess.Popen(['/bin/sh', '-c', scripts['guard']], env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) as owner:
                assert owner.stdout.readline().strip() == 'UAM_INSTALL_LOCK_READY:metadata'
                owner.stdin.write('UAM_INSTALL_AUTHORIZE:metadata\n' + '1:' + base64.b64encode(scripts['activate'].encode()).decode() + '\n')
                owner.stdin.flush()
                assert owner.stdout.readline().startswith('UAM_INSTALL_RESULT:1:0:')
                owner.stdin.close()
                owner.wait(timeout=20)
                assert owner.returncode == 0, owner.stderr.read()
            prior(); clean()
        case('Linux actual shell guard rolls back an abandoned leased transaction', abandoned)
        def before_fence_loss():
            setup(); run('activate')
            snapshot={p:p.read_bytes() for p in (binary,marker,backup,journal,directory/'uam-runner.manifest.json',directory/'uam-runner.sha256')}
            result=subprocess.run(['/bin/sh','-c',scripts['guard']],input='',env=env,capture_output=True,text=True,timeout=10)
            assert 'UAM_INSTALL_LOCK_READY:metadata' in result.stdout
            assert all(p.read_bytes()==data for p,data in snapshot.items()), 'Acquisition/controller loss recovered before durable fence'
            run('recover');prior();clean()
        case('Linux acquisition and controller loss leave prior transaction untouched before fence', before_fence_loss)

        def legacy_protocol_two():
            setup(); backup.write_bytes(old); backup.chmod(0o700); binary.write_bytes(new); binary.chmod(0o700)
            marker.write_text(''); journal.write_text('4.5.7\nmetadata\n2\n')
            env['UAM_METADATA_MODE'] = 'legacy-busy'
            run('recover', False)
            assert marker.exists() and backup.exists() and journal.exists()
            assert binary.read_bytes() == new
            assert (home / 'stops').read_text().strip().endswith('/r/uam.sock')
            env['UAM_METADATA_MODE'] = 'normal'; run('recover'); prior(); clean()
        case('Linux protocol2 recovery consults the legacy busy endpoint before mutation', legacy_protocol_two)
        def partial_journal():
            setup()
            saved = scripts['activate']
            start = saved.index("printf '%s\\n' 4.5.7 metadata ")
            end = saved.index('; mv -f "$backup.receipt"', start)
            scripts['activate'] = saved[:start] + "printf '%s' partial > \"$backup.receipt\"; exit 88" + saved[end:]
            try: run('activate', False)
            finally: scripts['activate'] = saved
            assert not journal.exists() and not marker.exists()
            run('rollback'); run('recover'); prior(); clean()
            candidate.write_bytes(new); run('activate'); run('commit'); published(); clean()
        case('Linux partial journal write cannot block guarded retry', partial_journal)
        def killed_guard_during_activation():
            setup(); env['UAM_METADATA_MODE'] = 'pause-start'
            def await_file(path):
                deadline=time.monotonic()+5
                while not path.exists():
                    assert time.monotonic()<deadline, 'Missing fixture synchronization file: '+str(path)
                    time.sleep(.01)
            with subprocess.Popen(['/bin/sh','-c',scripts['guard']], env=env, stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) as owner:
                try:
                    assert owner.stdout.readline().strip() == 'UAM_INSTALL_LOCK_READY:metadata'
                    command='('+scripts['activate']+'); printf done > "$UAM_FIXTURE_HOME/child-done"'
                    owner.stdin.write('UAM_INSTALL_AUTHORIZE:metadata\n'+'1:'+base64.b64encode(command.encode()).decode()+'\n');owner.stdin.flush()
                    await_file(home/'paused')
                    assert marker.exists() and backup.exists() and metadata.exists()
                    owner.kill();owner.wait(timeout=5)  # only this disposable guard, never its mutation child
                    competing=subprocess.run(['/bin/sh','-c',scripts['guard']],input='',env=env,
                                             capture_output=True,text=True,timeout=5)
                    assert competing.returncode==73,(competing.returncode,competing.stdout,competing.stderr)
                    assert marker.exists() and backup.exists() and metadata.exists()
                    assert binary.read_bytes()==new
                finally:
                    (home/'resume').write_text('resume')
                    await_file(home/'child-done')
                    if owner.poll() is None:owner.kill();owner.wait(timeout=5)
            # The surviving synchronous child must release the operation descriptor
            # before a successor performs actual guarded journal recovery.
            import fcntl
            with (home/'r/install.operation').open('r+') as operation:
                deadline=time.monotonic()+5
                while True:
                    try: fcntl.flock(operation,fcntl.LOCK_EX|fcntl.LOCK_NB);break
                    except BlockingIOError:
                        assert time.monotonic()<deadline,'Surviving child retained the operation lock'
                        time.sleep(.01)
                fcntl.flock(operation,fcntl.LOCK_UN)
            env['UAM_METADATA_MODE']='normal'
            recovery_request='UAM_INSTALL_AUTHORIZE:metadata\n1:'+base64.b64encode(scripts['recover'].encode()).decode()+'\n'
            recovered=subprocess.run(['/bin/sh','-c',scripts['guard']],input=recovery_request,env=env,
                                     capture_output=True,text=True,timeout=10)
            assert recovered.returncode==0,(recovered.stdout,recovered.stderr)
            assert 'UAM_INSTALL_LOCK_READY:metadata' in recovered.stdout
            prior();clean()
        case('Linux killed guard cannot release a surviving activation into successor recovery', killed_guard_during_activation)
        print(f'Compiled Linux metadata filesystem/injected-command cases: {passed} PASS', flush=True)


if __name__ == '__main__':
    main()
