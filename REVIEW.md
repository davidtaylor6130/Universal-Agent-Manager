# Review of #923: CEF process sandbox on Windows (`origin/uam/v5.2-sync-main...origin/v5/feat-923-windows-cef-sandbox`, commit b4502836)

**Verdict: approve after fixes, but only with a green Windows run.** Two blockers stop it merging as it stands: the packaged smoke test will always fail, and nothing guards the DLL imports that sandboxed processes load. If Windows CI and the packaged smoke test can't pass before the V5.2 cut, it is too risky for V5.2, because it reroutes every Windows entry point.

This was a read-only review; nothing was built on Windows. I checked the approach against CEF branch 8078 (the CEF 156 branch): `cef_variables.cmake.in`, `cefsimple`, `bootstrap_win.cc`, `main_runner.cc` and `cef_sandbox_win.h`. The docs check passes, and so do the 8 build-flag leak gate tests.

## What checks out

- **`cef_sandbox.lib`:** not used. With the `bootstrap.exe` layout the sandbox lives in `bootstrap.exe`, so there is no library version to match.
- **Runtime library:** everything is static (`/MT`), with nothing mixed. That covers the CEF wrapper, the app, its shared objects, sqlite, the new resource tool and vcpkg (`x64-windows-static`). The base branch already linked all of these into one executable, so a mismatch would already fail to link. No runtime objects cross the boundary into the bootstrap.
- **Entry point:** `RunWinMain` matches the declaration in `cef_sandbox_win.h` exactly, including C linkage. The DLL must be named `universal_agent_manager.dll` because the bootstrap loads the DLL named after itself; that matches.
- **`sandbox_info`:** passed to both `CefExecuteProcess` (twice) and `CefInitialize`. `no_sandbox` is false on Windows and true on Linux.
- **CEF API version:** the defines copied from the wrapper match how CEF builds a bootstrap client. In the headers UAM includes, the only experimental APIs are `ax_viewport_collapse` and `SetAxViewportCollapse`, which UAM doesn't use. So switching Windows to the last released API version should compile.
- **Working directory:** the bootstrap really does move every process without `--type` into the app folder (`SetCwdForBrowserProcess`), as the change assumes.
- **macOS:** unaffected. Everything new is Windows-only; macOS still passes a null `sandbox_info` and keeps its helper path.
- **DevTools and remote debugging:** stay blocked, with no `--remote-debugging-port` and no sandbox-disabling switches anywhere.
- **`release.yml` and `ci.yml`:** need no changes. `Compress-Archive Builds/Release/*` picks up the DLL, and the import library and build tool are kept out of the package.

## Findings

1. **Blocker – `scripts/smoke_packaged_windows.ps1:141`.** `foreach ($process in $cefProcesses)` overwrites the app's `$process` with a CIM object. That object has `ProcessId` but no `Id` and no `WaitForExit`, so `Stop-Process -Id $process.Id` (line 145) throws, and so does the cleanup on line 148. Every successful run therefore ends in failure, and Windows CI and the release job go red.
   - **Fix:** rename the loop variable to `$cefProcess`.

2. **Blocker (must be proven before merge) – `CMakeLists.txt:891`, `CMakeLists.txt:910`, `src/main.cpp`.** All of UAM's code now loads into every renderer, GPU and utility process. The bootstrap source says these processes "may already be locked down" when the DLL loads. Any import that is not delay-loaded and pulls in user32 or gdi32 makes the DLL load fail, and every renderer dies. Any static initialiser that calls a blocked API does the same. Such imports can arrive directly or through the `windowsapp.lib` umbrella, which is linked before `gdi32`, `ole32` and `user32`. Nothing currently checks this.
   - **Fix:** add a build or CTest check that runs `dumpbin /imports` on the DLL. It should fail on any import that is not delay-loaded, apart from `kernel32`, `ntdll` and `api-ms-win-core-*`.
   - Move `windowsapp` after the classic libraries, or replace it with `runtimeobject.lib`.
   - Audit namespace-scope initialisers.

3. **Should-fix – `cef_sandbox_folder_access_windows.cpp:25` and `:61`.** Two problems with the startup folder-permission check:
   - It only accepts a single access entry that is inheritable by both files and folders and also applies to the folder itself. Folders under Program Files split this into two inherited entries, so the check fails even though access is granted. UAM then tries to add the grant, fails, and logs a false error on every start.
   - Where the grant does succeed, it rewrites the permissions of the whole tree. If the exe is run straight from Downloads or the Desktop, that is slow and grants read access far more widely than needed.
   - **Fix:** check folder access and inherited access separately, counting inherit-only entries, or do a real access check. Only grant when the folder holds `libcef.dll` and `universal_agent_manager.dll`.

4. **Should-fix – `platform_process_windows.cpp:588`.** The working directory is restored only for processes started through this one launcher. Three other paths still leave UAM in the app folder:
   - PTY launches (`platform_pty_windows.cpp:135`);
   - the other `CreateProcessW` call (`platform_process_windows.cpp:351`);
   - helpers that providers start themselves.

   This matters because `opencode_v2_bridge.cpp:351` sends `current_path()` as the session's working directory.
   - **Fix:** pass `--cwd` explicitly to the OpenCode V2 bridge and text worker, or put the injection in one shared helper that every launcher uses.

5. **Should-fix – smoke coverage.** Only renderers are checked. The fragile part, the network service's restricted sandbox (which needs the folder permission), isn't checked at all.
   - **Fix:** fail on `--disable-gpu-sandbox`.
   - Require the network service utility process to carry `--service-sandbox-type=network` and run below medium integrity.

6. **Should-fix – tests (`docs/engineering/testing.md`).** There are no focused cases for:
   - `IsCefSubprocessCommandLine` (`main.cpp:132`): quoting, the `--` terminator, and a user argument containing `--type=`;
   - the `UAM_LAUNCH_WORKING_DIRECTORY` injection;
   - the folder-permission check.

   Add them to `uam_core_tests`, which Windows CI runs.

7. **Should-fix – `application.cpp:1084` and `:1156`.** If `sandbox_info` is null, CEF quietly sets `no_sandbox = true` (`main_runner.cc:285-288`), so a future entry-point refactor could switch the sandbox off without anyone noticing.
   - **Fix:** on Windows, when `UAM_CEF_SANDBOX` is set and `windows_sandbox_info` is null, log loudly or fail.

8. **Should-fix – docs (`docs/reference/security-enterprise.md:28`, `docs/security-enterprise.md`, `docs/chat/html-views.md`).** Windows is listed under "Security controls present" as if it were in place, while every page also says it is unverified. Keep Windows under Known Gaps, marked "implemented, awaiting Windows verification", until the smoke test passes. Separately, the html-views sentence "(Windows not yet verified there…)" reads awkwardly.

9. **Nit – `main.cpp:99` plus the matching CMake `FATAL_ERROR`.** The `CEF_USE_BOOTSTRAP` guard is duplicated; keep one.

10. **Nit – `CMakeLists.txt:288`.** `add_compile_definitions` applies the CEF API and sandbox defines to every later target in the project, including `uam-runner`, sqlite and the tools. Scope them to an INTERFACE target linked only by code that uses CEF.

11. **Nit – size (727 lines is largely justified).** The removable part is the icon and version-text tool, about 215 lines with its CMake and docs (`tools/windows-exe-resources`). It could move to a follow-up, at the cost of "CEF Bootstrap Application" showing in Task Manager meanwhile. The `mt.exe` discovery block (`CMakeLists.txt` around lines 1252–1271) can also shrink to a single `find_program` with hints. Everything else is needed: the DLL, entry point, folder permission, manifest merge, sandbox check and the `.exe` → `.dll` rename in the leak scanner.

12. **Nit – per-launch cost.** Every command-line, MCP, stdio and OpenCode bridge launch now goes through the bootstrap. On signed builds that means certificate verification each time, which CEF itself puts at about 30–300 ms. Measure it for helpers that are started on every turn.

**Distinct suggestions: 12** (2 blockers, 6 should-fix, 4 nits).

## Windows verification required before merge

1. Build on a clean Windows runner with both generators: the CI one (Ninja Multi-Config with `x64-windows-static`) and the release one (Visual Studio). Confirm the configure-time `bootstrap.exe` and delay-load checks pass.
2. Run `dumpbin /imports` and `dumpbin /dependents` on `universal_agent_manager.dll`. Only kernel32, ntdll and `api-ms-win-core-*` may be imported without delay-loading; user32, gdi32, shell32, ole32 and `ext-ms-win-*` must all be delay-loaded. Check the merged manifest with `mt -inputresource:universal_agent_manager.exe;#1`.
3. Run `ctest -C Release` and the core test shards.
4. Run `scripts/smoke_packaged_windows.ps1` against the CI zip and the release zip, after fixing finding 1. Every CEF process must be free of `--no-sandbox`. Renderers must be at untrusted or low integrity. With finding 5 in place, the GPU and network processes must also show as sandboxed.
5. On a real signed-in desktop, run the extracted zip from a fresh folder:
   - permissions get granted and nothing is logged on the second start;
   - with Process Explorer, check each process's integrity level and that the network service runs in its restricted sandbox;
   - `window.cefQuery` and the HTML view card work;
   - HTML view rendering, voice recording (audio service) and GPU compositing work.
6. Check every non-GUI entry point from `cmd` started in a different working directory:
   - the crash supervisor restarts after the renderer is killed;
   - the stdio control server and computer-use MCP server answer a round trip;
   - the management node starts;
   - discovery prints to the console;
   - an OpenCode V2 chat opens its session in the workspace, not the app folder.
7. Signing: an all-unsigned package starts. Signing the `.exe`, `.dll` and `chrome_elf.dll` with one certificate starts. Signing only the `.exe`, or using a mixed certificate, is refused, as the docs say.
8. Run the `perf_suite` app check before and after the change, plus the startup time of a single command-line helper.
