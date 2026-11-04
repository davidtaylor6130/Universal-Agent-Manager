# Isolated bootstrap validation

This target compiles the production bootstrap, runner client and memory-library services with an injected process service. It never launches a process, links CEF, builds a helper or reads an app profile. All six provider build definitions remain enabled. Exact native cases cover manifest admission, lease/phase sequencing and competition, failure/lost-ack rollback, cancellation and Windows loaded-image replacement planning. The normal UAM_BUILD_TESTS configuration also registers this target and its safe filesystem controls; the standalone configuration below avoids building CEF or the full native suite.

Configure and build only inside `Builds/task-1085`, under the shared validation lease when required by the active task instructions:

```sh
cmake -S tests/bootstrap_metadata -B Builds/task-1085 -DCMAKE_BUILD_TYPE=Release -DUAM_PACKAGE_REMOTE_RUNNERS=OFF
cmake --build Builds/task-1085 --target uam_bootstrap_metadata_tests --parallel 2
ctest --test-dir Builds/task-1085 --output-on-failure
Builds/task-1085/uam_bootstrap_metadata_tests --dump-scripts > /path/to/owned/native-scripts.json
python3 scripts/test_bootstrap_metadata.py --native-script-bundle /path/to/owned/native-scripts.json
python3 scripts/test_bootstrap_metadata_linux.py --native-script-bundle /path/to/owned/native-scripts.json
```

The script controls execute these compiled commands only against temporary files and injected helper commands. Linux uses fixture shell scripts; Windows uses an existing PowerShell installation and .NET file operations. macOS execution does not establish Windows mapped-image, Windows PowerShell 5 or real OpenSSH acceptance. The normal core tests in `tests/remote_runner_tests.cpp` remain separate from this focused target.

Additional review controls cover explicit commit acknowledgement versus unknown delivery and cleanup failure, partial journal creation before atomic publication, protocol-2 busy recovery, and Windows guard expiry during publication. The Windows expiry control uses an isolated PowerShell runspace, real disposable file locks and synchronization events; helper invocations remain injected. The CEF caller's final status persistence is source-reviewed separately and is not compiled by this two-translation-unit target.

`NativeRecoveryAdmission` exercises structured rollback outcomes and the shared host authority policy, including JSON save/reload, health promotion refusal and preservation of ordinary transient-error reconnect. A Linux disposable control pauses activation after its marker, kills only that fixture guard, confirms successor refusal while the mutation child survives, and then resumes and recovers. Both lock descriptors are checked absent in fixture helper starts. This target never opens a product data root or a live provider connection.

The pre-recovery fence controls exercise lock acquisition without mutation, rejected nonce/identity acknowledgements, controller/completion loss across recovery/activation/publication and settings-file reload. They compile the actual saved-host client, workspace-directory validation worker and memory-library list/create/delete paths and assert zero bridge launches or requests for an unresolved host, while an ordinary transient-error host completes an injected fresh handshake. Delayed-success/refused-rollback and disconnected-stream controls exercise operation correlation and stream invalidation without shortening production deadlines. CEF callback orchestration and original full-core cases remain separate uncompiled acceptance gates.

The two Windows generator assertion blocks are shared with the normal core cases through `windows_command_assertions.h`. The isolated routes `WindowsBootstrapLockFitsOpenSshCommandLimit` and `RemoteRunnerBootstrapSelectsAndHardensTheWindowsArtifact` run those same assertions against the production guard and injected captured transaction commands. They check acquisition without journal recovery, separate acknowledged recovery, stop-before-restoration, replacement and command budgets. The first route also includes the existing replacement-plan checks; these route names do not claim execution of the original core fake-SSH transport fixture.
