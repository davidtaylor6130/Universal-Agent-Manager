# #923 Windows CEF process sandbox: report

Final SHA: `18357aaefe76ab227fbebff377326e869f159e01` on `v5/feat-923-windows-cef-sandbox`, pushed. Base: `origin/uam/v5.2-sync-main`.

The Windows sandbox is implemented through CEF's `bootstrap.exe` layout. Nothing has been built or run on Windows: this environment is Linux and cannot reach the CEF CDN. It is therefore **implemented, awaiting Windows verification**, and the docs keep it under Known Gaps until the Windows checks below pass.

## What changed

- **Layout.** CEF 156 ships no `cef_sandbox.lib` for Windows, so the sandbox lives in CEF's `bootstrap.exe`.
  - UAM builds as `universal_agent_manager.dll` exporting `RunWinMain`.
  - `bootstrap.exe` is copied beside it as `universal_agent_manager.exe`, so the file users run keeps its name.
  - A build tool (`tools/windows-exe-resources`) copies UAM's icon into the exe and removes the bootstrap's "CEF Bootstrap Application" version text.
  - `mt.exe` merges the long-path manifest; the build fails if `longPathAware` or the Windows 10 entry is missing.
  - `icacls` grants the output folder `*S-1-15-2-2:(OI)(CI)(RX)` (CEF's LPAC requirement).
- **CMake.** The `USE_SANDBOX OFF` override is gone.
  - The wrapper's `CEF_API_VERSION`/`CEF_USE_BOOTSTRAP` defines become its usage requirements, so exactly the CEF-using targets get them.
  - The DLL uses CEF's delay-load list; `kernel32` and `windowsapp` link last.
  - A post-link `dumpbin /imports` check fails unless only `kernel32`, `ntdll` and `api-ms-win-core-*` are imported at load time.
- **Entry point.** `--type=` command lines go straight to `CefExecuteProcess` with the sandbox info, before any delay-loaded library. All other entry points keep their order: OpenCode V2 commands, discovery CLI, computer-use MCP, crash supervisor, control server, management node, GUI. `CefInitialize` gets the sandbox info and refuses to start without it.
- **Working directory.** The bootstrap moves non-CEF processes into the app folder. The stdio and terminal launchers pass `UAM_LAUNCH_WORKING_DIRECTORY` for UAM's own helpers, and the entry point restores it.
- **Folder grant at startup.** ZIP extraction loses it, so the app re-checks it. Split and inherit-only entries are counted, as Program Files uses. The grant is added only to a folder holding `libcef.dll` and the DLL, outside the profile, Desktop, Documents and Downloads folders; otherwise UAM logs why.
- **Smoke test** (`scripts/smoke_packaged_windows.ps1`) fails when:
  - the DLL or `chrome_elf.dll` is missing;
  - a CEF process has `--no-sandbox`, or `--disable-gpu-sandbox` outside the info-collection GPU process;
  - a renderer runs at medium integrity or above;
  - the network service lacks `--service-sandbox-type=network` or runs at medium integrity or above.
- **Leak gate** scans `universal_agent_manager.dll`; its residue key is renamed.
- **Tests.** New `tests/core_windows_cef_bootstrap_tests.cpp` (Windows only, in `uam_core_tests`) has 4 cases: subprocess detection, the working-directory variable, ACL evaluation and the end-to-end grant.
- **Docs.** Security (both pages), building, build-test-release, HTML views, documentation review, code map and Visual Studio solution are updated.

## Review rounds

- **Round 1** (independent fresh-context Opus review, `agent-reports/review-R4`, #923 section): **12 suggested (2 blockers, 6 should-fix, 4 nits), 10 implemented.**
  - Implemented 1–10:
    - Blockers: the smoke-script `$process` variable clash; the `dumpbin /imports` check and `windowsapp` link order.
    - Should-fixes: folder-grant evaluation and scope; a shared working-directory helper; GPU and network-service smoke checks; focused core tests; refusing to start without sandbox info; docs moved back to Known Gaps.
    - Nits: one `CEF_USE_BOOTSTRAP` guard; defines scoped to the wrapper.
  - Declined:
    - 11 (move the icon tool to a follow-up): the orchestrator directed keeping it, so Task Manager doesn't show "CEF Bootstrap Application".
    - 12 (measure per-launch bootstrap cost): it needs a Windows machine, so it is listed in the Windows verification steps instead.
- **Round 2** (re-check because the round-1 fixes were material; fresh-context Opus sub-agent): **8 suggested (0 blockers, 3 should-fix, 5 nits), 5 implemented.**
  - Implemented:
    - The import check's parse sentinel is the section header instead of `kernel32.dll`, and `kernel32` is named before `windowsapp`.
    - Chromium's info-collection GPU process (`--use-gl=disabled`) is exempt from the `--disable-gpu-sandbox` rule.
    - The static-initialiser search is recorded in the security reference.
    - Import names of any extension are checked, so `.drv` counts.
    - Docs wording for CEF's silent unsandboxed fallback is corrected.
  - Declined:
    - The `lld-link /dump` fallback: clang-cl is not a supported Windows toolchain; MSVC is the documented requirement.
    - Backslash-escaped quotes in the subprocess scanner: Chromium-generated child command lines never put escaped quotes before `--type=`, UAM helper launches put their arguments after `--`, and the behaviour predates the review.
    - Deny entries for S-1-15-2-2, and "error 0" if SID conversion fails: neither occurs in practice.
- **Total: 20 suggested, 15 implemented.**

## Checks run (Linux only)

- `python3 scripts/check_docs.py`: passed (203 pages, 1509 local links, 6 providers × 32 capabilities).
- `python3 tests/build_flag_leak_gate_test.py`: 8/8 passed.
- `python3 tests/app_icon_resource_test.py`: passed. This checks the icon file only; the exe check runs on Windows.
- `python3 tests/management_discovery_windows_compile.py`: passed.
- CMake contract scripts: 5/5 passed (Windows process containment, platform ifdefs, provider terminal boundary, computer-use permission, macOS spawn).
- MinGW `-Werror` compiles passed for `cef_sandbox_folder_access_windows.cpp`, `cef_bootstrap_launch_windows.cpp` and the resource tool.
- Syntax-only MinGW compiles passed, with stand-in headers (CEF is unavailable), for the `main.cpp` Windows block and the new test file.
- `check_windows_dll_imports.cmake` against simulated `dumpbin` output: 4 cases behaved as expected (clean passes; `USER32.dll`, `winspool.drv` and unparseable output fail).
- `windows_bootstrap_executable.cmake` with stand-in `mt` and `icacls`: passed.
- Cross-configure of the Windows CMake path (MinGW, stand-in CEF, SQLite and zlib) passed, for both the full build and the data-folder-only build.

## Windows verification still required before merge

1. Build with Ninja Multi-Config (CI) and the Visual Studio generator (release workflow). The configure checks, the `dumpbin` import check and the manifest merge must pass.
2. `ctest -C Release`, including `uam_app_icon_resource` on the exe, plus the core shards. The 4 new Windows cases must pass.
3. `scripts/smoke_packaged_windows.ps1` against the CI and release zips.
4. Desktop run from a fresh folder:
   - the folder grant is added and nothing is logged on the second start;
   - Process Explorer shows renderer, GPU and network-service integrity and sandboxing;
   - `window.cefQuery` and HTML view cards work;
   - voice recording and GPU compositing work.
5. Non-GUI entry points from `cmd` in another directory:
   - the crash supervisor restarts after a crash;
   - the stdio control and computer-use MCP servers answer a round trip;
   - the management node starts;
   - discovery prints to the console;
   - an OpenCode V2 chat and text worker run in the workspace.
6. Signing: all unsigned starts; exe, DLL and `chrome_elf.dll` signed with one certificate starts; mixed signing is refused.
7. Startup time of one command-line helper, and the `perf_suite` app check, before and after.

## Issue previews

None. No new issue is needed; everything outstanding is Windows verification of #923 itself.

## PR preview (not opened)

**Title:** `feat(build): enable the CEF process sandbox on Windows`

> Enables the Chromium process sandbox on Windows through CEF's `bootstrap.exe` layout. CEF 156 ships no `cef_sandbox.lib` for Windows.
>
> - **Layout:** UAM builds as `universal_agent_manager.dll` exporting `RunWinMain`. CEF's `bootstrap.exe` ships as `universal_agent_manager.exe`, with UAM's icon, no CEF version text, the long-path manifest merged and the LPAC folder grant.
> - **Imports:** the wrapper's API and bootstrap defines are its usage requirements, and the DLL is delay-loaded like CEF's own clients. A post-link `dumpbin /imports` check rejects load-time imports that sandboxed processes can't load.
> - **Entry points:** CEF subprocesses reach `CefExecuteProcess` with the sandbox info first. Every other entry point keeps its order. `CefInitialize` refuses to start without sandbox info.
> - **Working directory:** UAM's own helpers get back the directory the bootstrap moves them out of.
> - **Folder grant:** the app adds the LPAC grant when it's missing, only to its own folder.
> - **Smoke test:** checks that renderer and network-service processes are sandboxed. The leak gate scans the DLL.
> - **Docs:** the Windows sandbox stays under Known Gaps as implemented, awaiting Windows verification.
>
> Closes #923 once Windows verification passes.
>
> **Proof:**
> - `python3 scripts/check_docs.py` passes (203 pages, 1509 local links, 6 providers × 32 capabilities).
> - `tests/build_flag_leak_gate_test.py` 8/8, `tests/app_icon_resource_test.py` and `tests/management_discovery_windows_compile.py` pass.
> - 5/5 CMake contract scripts pass.
> - The new Windows sources and the resource tool compile with MinGW (`-Werror`).
> - The import-check and bootstrap-exe CMake scripts behave as expected against simulated tool output.
> - The Windows CMake path cross-configures with stand-in dependencies.
> - Not built or run on Windows: the Windows build, CTest, core shards and packaged smoke test have not run.
>
> **Review:** 1 independent reviewer, 2 iterations. Independent claude-opus-5-5: 20 suggested, 15 implemented. Declined:
> - the icon-tool split, kept as directed;
> - the per-launch cost measurement, which needs Windows;
> - a clang-cl `lld-link` fallback, an unsupported toolchain;
> - escaped-quote handling in the subprocess scanner, which Chromium's child command lines never need;
> - deny-entry and "error 0" edge cases in the folder grant, which don't occur in practice.
>
> **Created with:** Claude Opus 5.5 (`claude-opus-5-5`) in Claude Code on the web (Claude Agent SDK), on behalf of David Taylor.

## Screens needing Before/After capture

No React or settings UI changed. These native Windows views change and need Before/After captures on Windows:

1. Explorer: the `universal_agent_manager.exe` icon, which should still be UAM's. Properties → Details should show no "CEF Bootstrap Application" description.
2. Task Manager → Processes: the UAM entry name and icon.
3. Process Explorer with the Integrity column: the UAM process tree. Before, the renderer, GPU and utility processes are Medium. After, they should be Untrusted or Low, or AppContainer.
