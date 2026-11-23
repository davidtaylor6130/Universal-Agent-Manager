# #525 Disabled-provider names in native builds: report

Final SHA: `5fb05eb30149d60de6feca06f5fdef04bd135bc4` on `v5/feat-525-disabled-provider-names`, pushed. Base: `origin/uam/v5.2-sync-main`.

This covers part of the issue's V5.2 "what's missing" note:
- the SSH gap in the interface bundle is closed;
- the Windows native leak-gate rows now gate.

Computer-use state names in the interface, and the native residue for providers, SSH and computer use, remain. Nothing was built for macOS or Windows, and those builds are the real gate.

## What changed

- **Interface without SSH.** Routing state and both routing actions exist only in builds with SSH.
  - `executionRouting`, `setExecutionRoutingSettings` and `releaseExecutionRoutingReservation` are optional in `AppState`, so a new unguarded read fails to compile.
  - Session creation and the New Chat dialog treat automatic routing as unavailable.
  - The no-SSH bundle drops from 18 leak-gate matches to 1, the reviewed connection-error pattern. The `[eE]xecutionRouting` allow-list entry (budget 17) is removed.
- **Native without SSH.** `settings_frontend_json.h` no longer sends `executionRouting` to the interface. Stored routing stays unchanged on disk, because saved data uses `execution_routing` through `SettingsStore`. The two exact `executionRouting` residue entries (macOS app and Windows exe) are removed; that line was the string's only source.
- **CI.** The Windows native rows in `build-flag-leaks.yml` no longer use `continue-on-error`. The V5.1.0 release-candidate run 44641841609 passed all 19 Windows rows. No native rows have run on the V5.2 base since then.
- **Tests.** `UI-V2/src/store/noSsh.flags.test.tsx` adds 3 tests that mock SSH off:
  - the initial state, a patch and a full-state snapshot leave the store with no routing members;
  - automatic `addSession` returns false without a `createSession` request;
  - the New Chat dialog has no Automatic choice.
  All 3 fail with SSH on.
- **Docs.** In `docs/build-flags.md` the SSH interface row is Implemented, and the computer-use gap stays Partial. The build reference and the documentation review describe Windows gating.

## What remains for #525

- **Computer use:** its state-field names still appear 142 times in the no-computer-use interface bundle. They come from about 280 sites in 14 files, including session sync, which was too large to change safely in this run.
- **Native residue:** names of excluded providers, SSH and computer use remain in shared native code. This needs the per-provider adapter and state-schema work the issue describes.
- **CI trigger:** pushes only run the native rows on `uam/v5-improvements`. The V5.2 branch gets them only through the label or a manual run.
- **Before merge:** run the build-flags workflow on this branch, with the `build-flags` label or manually, and confirm all 38 native rows, including Windows (review R3 finding 3).

## Review rounds

- **Round 1 (independent fresh-context Opus review, R3, #525 section): 4 suggested (3 should-fix, 1 nit), 3 implemented.**
  - Finding 1 implemented: routing members are optional in the store types.
  - Finding 2 implemented: the no-SSH Vitest.
  - Finding 4 implemented: native no longer sends `executionRouting` without SSH, and the residue entries are removed.
  - Finding 3 not implemented: it needs a CI run on this branch, which this environment can't start. It is listed as a pre-merge step.
- **Round 2 (fresh-context Opus re-check of the round-1 fixes, because they were material): 3 suggested (0 blocker, 0 should-fix, 3 nits), 2 implemented.**
  - Implemented: spy on `window.dispatchEvent` instead of assigning it, and `vi.waitFor` the initial state instead of a single timer tick.
  - Declined: reusing a frozen `NO_EXECUTION_ROUTING` in the slice's initial state. The slice keeps its own literal so store state never shares a module-level object.
- **Total: 7 suggested, 5 implemented.**

## Checks run

- **UI (Vitest):** full suite at `7a7c531b`: 4,185 unit tests in 233 files and 33 performance tests in 7 files pass. After the round-2 test nits at `5fb05eb3`: `noSsh.flags.test.tsx` 3/3 and `tsc -b` clean.
- **Leak gate on interface bundles:**
  - At `23af176b`: all 15 UI profiles CI builds pass (9 single-flag, 6 single-provider).
  - At `7a7c531b`: the no-SSH bundle (1 reviewed SSH match, 0 unexcused) and the Antigravity-only bundle pass. The all-on bundle still contains the routing code.
- **Gate scripts:** `tests/build_flag_leak_gate_test.py` 8 pass, `tests/ui_build_incremental_test.py` 1 pass, `scripts/check_docs.py` passes.
- **Native on Linux:** only the remote runner builds here.
  - `uam_runner` built with all providers, and with OpenCode and Antigravity off. A local SQLite source was substituted because sqlite.org is blocked; nothing about that is committed.
  - All-on passes the packaging checks.
  - The OpenCode/Antigravity-off runner shows 40 unrecorded Linux (GCC) symbol names from unchanged shared code. Linux is not a gated platform.
  - `settings_frontend_json.h` passes a syntax-only `g++` compile with SSH off and on.
- **Not run:** the app's native build (macOS and Windows only) and the CI native rows.

## Issue previews

**Comment on #525 (preview, not posted):**

> Progress on the V5.2 "what's missing" note, on branch `v5/feat-525-disabled-provider-names` (`5fb05eb3`, not merged):
> - **SSH in the interface: done.** Builds without SSH carry no routing state or actions in the store, and native no longer sends routing settings. The no-SSH bundle's only leak-gate match is the reviewed connection-error pattern, and the `executionRouting` allow-list entry is gone. Stored routing stays on disk unchanged.
> - **Windows gate: done, pending a run.** Windows native rows no longer use `continue-on-error`. The V5.1.0 RC run passed all 19, but nothing has run on the V5.2 base yet.
> - **Still open:** computer-use state names in the interface (142 matches), and native residue for providers, SSH and computer use, which need the per-provider adapter and state-schema work.
>
> **Prepared by:** Claude Opus 5.5 (`claude-opus-5-5`) in Claude Code on the web (Claude Agent SDK), on behalf of David Taylor.

## PR preview (not opened)

**Title:** `feat(build): drop SSH routing state from no-SSH builds and gate Windows leak rows`

> Part of #525. Closes the SSH gap in the interface bundle and makes the Windows native leak-gate rows gate.
>
> - **Interface:** builds without SSH contain no routing state or actions. They are optional in the store's types, and automatic routing is unavailable. The no-SSH bundle has 1 reviewed leak-gate match instead of 18, and the `executionRouting` allow-list entry is removed.
> - **Native:** builds without SSH no longer send routing settings to the interface, and stored routing is unchanged on disk. Two exact residue entries are removed.
> - **CI:** Windows native rows no longer use `continue-on-error`. The V5.1.0 RC run (44641841609) passed all 19.
> - **Docs:** the SSH interface row is Implemented. The build reference and documentation review describe Windows gating.
> - **Remaining for #525:** computer-use state names in the interface, and native residue for providers, SSH and computer use.
>
> Related to #525
>
> **Before merge:** run the build-flags workflow on this branch (`build-flags` label or manual) and confirm all 38 native rows, including Windows.
>
> **Proof:**
> - **UI:** Vitest 4,185 unit and 33 performance tests pass, including 3 new no-SSH tests that fail with SSH on. `tsc -b` is clean.
> - **Leak gate:** all 15 UI profiles pass, and the no-SSH bundle has 1 reviewed match. Gate self-test 8 pass. `check_docs.py` passes.
> - **Linux:** the runner builds with all providers, and with OpenCode and Antigravity off. `settings_frontend_json.h` compiles syntax-only with SSH off and on.
> - **Not verified:** no macOS or Windows binary was built, and the CI native rows have not run on this branch.
>
> **Review:** 2 independent reviewers, 2 iterations. Independent Opus review R3: 4 suggested, 3 implemented (the fourth needs a CI run, listed above). Fresh-context claude-opus-5-5 re-check: 3 suggested, 2 implemented (one nit declined to keep slice state off a shared module constant).
>
> **Created with:** Claude Opus 5.5 (`claude-opus-5-5`) in Claude Code on the web (Claude Agent SDK), on behalf of David Taylor.

## Screens needing Before/After capture

- **New Chat dialog, routing control.** In a no-SSH build, the Automatic routing choice should be absent. In a full build, it should be unchanged.
