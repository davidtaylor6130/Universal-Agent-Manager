# #310 Chat repository changes reviewable with file diffs: report

Final SHA: `eda2c1e75084d3fe84aa9b5d7caa410ab0f77447` on `v5/feat-310-repository-diffs`, pushed. Base: `origin/uam/v5.2-sync-main`. Commits: `43e02ce6`, `0d0663f1`, `eda2c1e7`.

V5.1.0 (#735) already delivered the Git slice of #310: the per-chat baseline, the **Changes from this chat** label, per-file diffs, and the loading, empty, binary and error states. The V5.2 close-out note left three gaps: no SVN baseline, an SSH drop that silently hides the summary, and touch devices unverified. This branch fixes the SSH drop. The other two remain.

## What changed

- **Native detection** (`src/app/vcs_commit_service.cpp`, `src/app/vcs_provider.h`).
  - `DetectRemote` now records whether a probe could not answer (`RemoteProbeFailure`: `TimedOut` or `LostConnection`). Before, a dropped host read as "No Git or SVN repository detected", which hid the summary.
  - The summary shows one fixed sentence: "Reading the repository on the remote host timed out." or "Lost the connection to the remote host while reading the repository."
  - A runner that only refused a command over a live connection (for example, `svn` not installed) still reads as "no repository".
  - If the requested system could not be read, the read returns the error even when another system was found after a reconnect. A dropped Git read no longer falls back to SVN whole-working-copy rows.
  - Failures are logged once per read, with the host id and failure kind only.
- **Stale-read refusal** (`src/cef/vcs_handlers.cpp`).
  - The 409 "workspace changed while loading" refusal now applies only when the host appears, vanishes or changes connection during the read.
  - A host that was already missing when the read started reaches the worker, which reports it with Retry.
- **Frontend** (`sessionsSlice.ts`, `storeTypes.ts`, `types.ts`, `ChatView.tsx`).
  - `getVcsCommitStatus({ reportFailure: true })` turns a failed or malformed bridge reply into an error status instead of `null`. The chat summary uses it, so it stays visible with the error and Retry.
  - The stale-context 409 is marked `contextChanged`. The summary reads once more per completed turn. If that read is refused too, it clears the earlier result instead of showing the race as an error.
- **Docs:** `docs/changes/changes.md` records the failure behaviour and that touch review is not device-verified. No provider capability rows change, because change review works the same for every provider.

## What remains for #310

1. An SVN chat baseline. SVN workspaces still show **Working copy changes** for the whole working copy.
2. Touch verification on a real device. Browser mode is blocked by the native-shell guard, so touch could not be emulated here.

## Review rounds

- **Round 1: independent fresh-context Opus review (R3, #310 section). 4 suggested (2 should-fix, 2 nits), 3 implemented, 1 partly.**
  - Implemented:
    - 1: separate timeout and lost-connection messages, with raw runner text kept out of the interface.
    - 2: no fallback to SVN when the Git read failed.
    - 3: the stale-context 409 is retried once, not shown.
  - Partly, 4 (process): the full frontend test counts and the production build are recorded below. Compiling and running the native cases on macOS was handed to the orchestrator, as directed.
- **Round 2: fresh-context Opus re-check of the round-1 fixes, run because they were material. 9 suggested (0 blocker, 4 should-fix, 5 nits), 5 implemented, 1 partly, 3 declined.**
  - Implemented:
    - A deleted host no longer gets the race 409.
    - A dropped repeat clears stale rows.
    - The log line is reduced to host id and failure kind.
    - The doc no longer claims every host failure text goes to the log.
    - The doc states what happens on a repeated refusal.
  - Partly: native tests. A mixed Git+SVN checkout case was added. A timeout case was declined, because the remote timeout is a fixed 120 s with no test seam, and adding one is new scope.
  - Declined:
    - `timed_out` is set only when no stderr was written: this predates the change and is outside this slice.
    - A manual Retry after the automatic retry hits the race again: it now clears rather than shows stale rows, and there is no loop.
    - A structured reason code for the 409: it needs a native bridge contract change, which is out of scope.
- **Total: 13 suggested, 8 implemented, 2 partly, 3 declined.**

## Checks run

- `npm --prefix UI-V2 run test`: unit suite 233 files, 4188 tests, all passing. Performance suite 7 files, 33 tests, all passing.
- `npm --prefix UI-V2 run build`: succeeds. `npx tsc --noEmit -p .`: passes.
- `python3 scripts/check_docs.py`: passes (203 pages, 1508 local links, 6 providers × 32 capabilities).
- Native, syntax only: `src/app/vcs_commit_service.cpp` compiles with `g++ -fsyntax-only` (C++20, all feature flags on, a local stub for one CEF header).
- Native, not run here:
  - `src/cef/vcs_handlers.cpp` and `tests/vcs_provider_tests.cpp` need the full CEF headers, so they are unchecked.
  - CMake refuses to configure on Linux, so no native target was built or run.
  - Three new cases are pending a macOS run: `RemoteVcsStatusReportsALostConnectionInsteadOfNoRepository`, `RemoteVcsStatusDoesNotFallBackToSvnWhenTheGitProbeLosesItsConnection` and `RemoteVcsStatusStillReportsAPlainFolderAsHavingNoRepository`.
- No provider CLI was run, and no live SSH host was used.

## Issue previews

**Comment on #310 (preview, not posted):**

> The V5.2 follow-up for the SSH gap is on `v5/feat-310-repository-diffs` (`eda2c1e7`).
>
> - **Dropped host:** an SSH host that drops or times out while its repository is detected now keeps the **Changes from this chat** summary visible. It shows one fixed sentence and Retry, instead of hiding the summary as "no repository". A failed bridge reply is shown the same way.
> - **Mixed checkouts:** a Git read that could not answer no longer falls back to SVN rows.
> - **Stale-read race:** when the chat's workspace or host changes during a read, the summary reads once more and never shows the race as an error.
>
> Still open: an SVN chat baseline, and touch verification on a device.
>
> **Proof:**
> - UI-V2 unit tests 4188/4188 and performance tests 33/33 pass. The production build and `tsc` pass.
> - `vcs_commit_service.cpp` passes a syntax-only compile.
> - Three native cases are pending a macOS run.
>
> **Prepared by:** Claude Opus 5.5 (`claude-opus-5-5`) in Claude Code on the web (Claude Agent SDK), on behalf of David Taylor.

No new issue is proposed. The SVN baseline and touch verification are already tracked on #310.

## PR preview (not opened)

**Title:** `fix(vcs): keep the chat changes summary visible when an SSH host drops`

> Fixes the V5.2 SSH gap noted on #310. The **Changes from this chat** summary no longer disappears when a remote host drops or times out while its repository is read.
>
> - **Native detection:** tells a probe that could not answer (timed out or lost connection) apart from "not a repository". The summary shows one fixed sentence for each.
>   - When the requested system could not be read, there is no fallback to another system's rows.
>   - The diagnostic log gets one line with the host id and failure kind.
> - **Stale-read refusal:** the "workspace changed while loading" refusal now applies only when the host changes during the read. A host that was already missing reports its own error with Retry.
> - **Frontend:** `getVcsCommitStatus({ reportFailure: true })` returns a failed read as an error. The chat summary shows it with Retry.
>   - A stale-context refusal is retried once per completed turn. A repeat clears the earlier result instead of showing it as current.
> - **Docs:** `docs/changes/changes.md` is updated. No provider capability rows change.
>
> Related to #310 (stays open for the SVN baseline and touch verification).
>
> **Proof:**
> - `npm --prefix UI-V2 run test`: 4188/4188 unit tests (233 files) and 33/33 performance tests (7 files) pass.
> - `npm --prefix UI-V2 run build` and `npx tsc --noEmit -p .` pass.
> - `python3 scripts/check_docs.py` passes.
> - `src/app/vcs_commit_service.cpp` passes a syntax-only `g++` compile.
> - Native code was not built on Linux. Three new native cases in `tests/vcs_provider_tests.cpp` are pending a macOS run. No live SSH host or provider CLI was used.
>
> **Review:** 1 independent reviewer, 2 iterations. Independent claude-opus-5-5: 13 suggested, 8 implemented, 2 partly. The three declined items are an older `timed_out` quirk outside this slice, a manual Retry that hits the race again (it clears rather than loops), and a structured 409 reason that needs a native bridge contract change.
>
> **Created with:** Claude Opus 5.5 (`claude-opus-5-5`) in Claude Code on the web (Claude Agent SDK), on behalf of David Taylor.

## Screens needing Before/After capture

- The end-of-transcript **Changes from this chat** summary in a chat on an SSH host whose connection drops after a turn.
  - Before: the summary is absent.
  - After: the summary shows "Lost the connection to the remote host while reading the repository." with Retry, then the recovered file list after Retry.
  - Capture it on desktop and on the narrow phone companion.
