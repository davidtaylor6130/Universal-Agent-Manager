# Report: v5/refactor-acp-antigravity-dedup

## Result

- **Work branch:** `v5/refactor-acp-antigravity-dedup`, pushed. Final SHA `7913f0248bf479b78eff7973f4d409e097cac325`, based on `uam/v5.2-sync-main` at `6f235eb9`.
- **No PR, issue, comment or label was created.** None of the files on the do-not-touch list was changed.
- **Screens needing Before/After capture:** none. This is a native-code refactor with no UI or visible behaviour change.

| SHA | Commit | Files | Lines added | Lines removed |
|---|---|---|---|---|
| `09f53512` | `refactor(acp): share tool-call restore from saved chat messages` | 4 | 40 | 67 |
| `34802dfc` | `refactor(antigravity): share one protobuf wire reader` | 3 | 121 | 72 |
| `78836b86` | `refactor(acp): remove unused SendCodexUserInputResponse` | 2 | 0 | 6 |
| `7913f024` | `refactor(antigravity): take the wire error factory by value` (review fixes) | 2 | 6 | 3 |

The new `src/common/utils/protobuf_wire.h` accounts for 87 of the added lines in `34802dfc`.

## Deviations from the audit

- **UUID check not replaced.** The history file's own `Uuid()` accepts only lowercase hex, while `uam::uuid::IsCanonicalUuid` also accepts uppercase. Swapping them would let history discovery accept more than it does now.
- **One usage error message can differ.** When a message has more than 4,096 fields and the extra tag is also cut short, accounting now reports "Invalid fields" instead of "Truncated varint". Every accounting caller turns any error into nullopt/false, so nothing observable changes.

## Review loop

**Round 1** (one independent reviewer, fresh context, `claude-opus-5-5`): 4 suggested (0 blockers, 1 should-fix, 3 nits), 2 implemented.

- **Implemented, nit 2:** `ReadVarint` and `ReadFields` now take the error factory by value. Passing a plain function to the old `const MakeError&` deduced a function type, so the `const` meant nothing and MSVC can warn C4180.
- **Implemented, nit 3:** the accounting error mapping lists every `Error` value instead of using `default:`, so a new value draws a `-Wswitch` warning.
- **Declined, should-fix 1:** no CI job compiles this branch with MSVC before merge, because `select_native_tests.py` marks it as not Windows-relevant. The fix is a `build-flags` label or a manual Windows run, and this task forbids labels. Someone with that permission needs to do it before merge.
- **Declined, nit 4:** the reviewer wanted a `Claude-Session:` trailer on each commit. Adding it to pushed commits needs a rewrite and force-push, which the task forbids, and the requested trailer was the Co-Authored-By line only.

No second review was run. The fixes only change how one argument is passed and list the switch cases explicitly, and messages and behaviour are unchanged.

## Verification (Linux only)

- `python3 scripts/check_docs.py` passes.
- **Compile:** every changed source file compiles with `g++ -std=c++20 -fsyntax-only -Wall`, all provider flags on, against CEF headers at the pinned commit `57389f04`. I confirmed the check catches a deliberately broken call. The accounting file also compiles with Antigravity turned off. The reviewer also syntax-checked the four ACP files with clang++.
- **Old vs new reader comparison:** the pre-refactor readers against the new `ReadFields`, run with each caller's settings, on about 3.2 million generated inputs (the reviewer ran a further 3 million).
  - History: no differences in fields or error messages.
  - Accounting: no outcome differences; only the message change above.
- **`antigravity_accounting_tests`:** 10 of 10 pass on both base and branch, using a small stand-in test runner.
- **`tests/antigravity_history`:** 24 of 27 fixtures pass on both base and branch. The same three image fixtures fail at lines 222, 282 and 446 on both, so they are not caused by this change. Two local workarounds were needed:
  - System SQLite 3.45 stood in for the pinned 3.54, which couldn't be downloaded.
  - The three build-feature defines (`UAM_ENABLE_COMPUTER_USE`, `UAM_ENABLE_SSH`, `UAM_ENABLE_MOBILE_COMPANION`) were passed by hand. The test target's build file already lacks them on the base branch.

**Not verified:**

- `uam_core_tests`, including the Claude/Copilot ACP and subagent cases and `antigravity_native_import_tests`.
- A `tests/antigravity_history` run with the pinned SQLite.
- The full macOS/Windows native, CTest and build-flag leak gates.
- Any MSVC compile.
- David's acceptance.

---

## Issue preview

**Title:** `refactor(acp, antigravity): remove duplicated tool restore, protobuf reader and dead Codex helper`

Three bits of native code are duplicated or never called, which makes them more costly to maintain. This comes from a read-only audit of `uam/v5.2-sync-main`.

**Desired outcome:**

- Claude and Copilot share one function that restores a tool call's state from its saved chat message.
- The Antigravity usage and history readers share one protobuf reader. Each keeps its own rules: history refuses non-canonical numbers and duplicate fields; usage takes the first match and throws `runtime_error`.
- The unused `SendCodexUserInputResponse` is deleted.
- Behaviour stays the same.

**Environment:** `uam/v5.2-sync-main` at `6f235eb9`.

**Evidence:**

- `claude_acp_message_handlers.cpp` `RestoreClaudeTool` and `copilot_native_protocol.cpp` `TrackedTool` have identical bodies.
- `antigravity_accounting.cpp` and `antigravity_native_history.cpp` each parse protobuf numbers, tags and lengths themselves.
- `SendCodexUserInputResponse` has no callers in the repo.
- The history file's own UUID check accepts only lowercase, but `uam::uuid::IsCanonicalUuid` also accepts uppercase, so it is not a drop-in replacement.

**Reproduction status:** These are code-structure findings, not a runtime defect, so there is nothing to reproduce.

**Acceptance criteria:**

- [ ] One shared restore function, used by Claude and Copilot
- [ ] One shared protobuf reader, with the history and usage rules unchanged
- [ ] The dead helper is removed
- [ ] The Claude/Copilot ACP and subagent cases, `antigravity_accounting_tests`, `antigravity_native_import_tests` and `tests/antigravity_history` pass on macOS

**Review:** Self-triage only.

**Prepared by:** Claude Opus 5.5 (`claude-opus-5-5`) in a Claude Code cloud session started by a scheduled task (Claude Code harness), on behalf of David Taylor.

---

## PR preview

**Title:** `refactor(acp): share tool restore and Antigravity protobuf reading`

**Base / head:** `uam/v5.2-sync-main` ← `v5/refactor-acp-antigravity-dedup` (`7913f024`)

This removes three duplicated or unused pieces of C++ without changing behaviour.

- **Tool restore:** Claude and Copilot now share `RestoreToolCallFromChat`, next to `UpsertToolCall`.
- **Protobuf reading:** Antigravity usage and history now use the shared reader in `src/common/utils/protobuf_wire.h`. Each keeps its own field limit, number-encoding rule and error type. History still refuses duplicate fields, and usage still takes the first match.
- **Dead helper:** the unused `SendCodexUserInputResponse` is deleted.
- **One message change:** in usage, a message over the field limit whose extra tag is also cut short now reports "Invalid fields" instead of "Truncated varint". Every caller turns either error into "no result", so nothing observable changes.
- **UUID check kept:** history keeps its own lowercase-only check, because `IsCanonicalUuid` also accepts uppercase.
- **Docs:** no documentation describes these functions, so no doc change is needed.

Related to #<issue>

**Proof:** These are Linux checks only.

- `python3 scripts/check_docs.py` passes.
- The changed files compile with g++ syntax-only checks against the pinned CEF headers. The reviewer also syntax-checked the four ACP files with clang++.
- A comparison of the old and new readers on about 3.2 million generated inputs found no history differences and no usage outcome differences.
- `antigravity_accounting_tests` passes 10 of 10, using a small stand-in test runner.
- `tests/antigravity_history` passes 24 of 27 with system SQLite 3.45 in place of the pinned version. The same three image fixtures fail on the base branch.
- **Not yet run:** `uam_core_tests` (including the Claude/Copilot ACP and subagent cases and `antigravity_native_import_tests`), the full macOS and Windows gates, and any MSVC compile. CI won't compile this on Windows unless the `build-flags` label is added or a manual run is started.

**Review:** 1 independent reviewer, 1 iteration. Independent `claude-opus-5-5`: 4 suggested, 2 implemented. The Windows CI run needs a label this task can't add, and the commit-trailer fix would need a force-push.

**Created with:** Claude Opus 5.5 (`claude-opus-5-5`) in a Claude Code cloud session started by a scheduled task (Claude Code harness), on behalf of David Taylor.
