# Report: #984 CEF remote debugging switch

**Work branch:** `v5/fix-984-cef-remote-debugging`, pushed. The `v5/` name was accepted, so no `claude/` fallback was needed.
**Final SHA:** `b2a4b397f119757e3587f3f5b21c9fd612d66ca9`. Two commits on `origin/uam/v5.2-sync-main`:
- `f3c6530c`: the fix
- `b2a4b397`: review nits

The base did not already have the fix: `OnBeforeCommandLineProcessing` passed every switch to CEF unchanged.

## Review rounds
1 independent reviewer, 1 iteration.

- **Round 1 (independent fresh-context claude-opus-5-5, `agent-reports/review-R1`):** 2 suggested, 2 implemented, 0 blockers, 0 should-fixes. Verdict: approve.
  1. Nit, implemented: in a test run, only `--remote-debugging-address` is dropped. That case now logs "Ignored --remote-debugging-address; DevTools stays on 127.0.0.1." instead of "outside an explicit test run".
  2. Nit, implemented: `docs/security-enterprise.md` (the agreement page) now says a normal launch exposes no DevTools port.
- **No re-check round:** the follow-ups only change a log string and one doc clause, so they are not material.

## Issue previews
None. No new issues were found.

## PR preview
**Title:** `fix(cef): accept the remote debugging switch only in explicit test runs`

UAM passes its command line to CEF, so any launch with `--remote-debugging-port` opened a DevTools port that a local process could use to drive the UI.
- `UamCefApp::OnBeforeCommandLineProcessing` now drops every `--remote-debugging-*` switch and `--remote-allow-origins`, unless the test-only `UAM_TEST_ALLOW_REMOTE_DEBUGGING` is exactly `1`. It always drops `--remote-debugging-address`, so an allowed port stays on 127.0.0.1.
- `CefCommandLine` cannot remove a switch. The command line is rebuilt only when something is dropped, so normal launches are untouched. One diagnostic line names the dropped switches; values are never logged.
- The decision lives in `uam::cef::RemoteDebuggingSwitchesToDrop()` (`src/cef/uam_cef_command_line_config.*`).
- `scripts/perf_driven.py` sets the variable, so the performance suite's `driven` piece keeps its DevTools connection.
- `tests/html_view_isolation_browser_test.py` launches Chrome, not UAM, so it is unaffected.
- Regression case: `CefRemoteDebuggingSwitchesNeedExplicitTestRun`.
- Docs updated: `docs/security-enterprise.md`, `docs/reference/security-enterprise.md`, `docs/reference/performance-suite.md` and `docs/reference/architecture/build-test-release.md`.

**Proof:**
- `python3 scripts/check_docs.py` passed: 203 pages, 1510 links, 6 providers × 32 capabilities.
- The new test's 7 assertions, built with the helper as a standalone `g++ -std=c++20 -Wall -Wextra -Werror` driver on Linux, passed.
- The same driver against a mutant that keeps the switches failed on the first assertion.
- `python3 -m py_compile scripts/perf_driven.py` passed.
- **Not run:** the native build, `uam_core_tests`, CTest and `scripts/perf_suite.py driven`. CMake refuses Linux, and the CEF headers are not available here, so `uam_cef_app.cpp` was not compiled.
- **Not run:** manual acceptance on macOS and Windows:
  - A normal launch with `--remote-debugging-port` opens no port.
  - With the variable set, the port opens on 127.0.0.1 only.
  - `perf_suite.py driven` passes.

**Review:** 1 independent reviewer, 1 iteration. Independent claude-opus-5-5: 2 suggested, 2 implemented (both nits).

**Created with:** claude-opus-5-5 in Claude Code on the web (Claude Code remote session, scheduled task), on behalf of David Taylor.

Closes #984

## Screens needing Before/After capture
None. This is a native startup and script change with no UI change.
