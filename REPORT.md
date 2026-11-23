# Antigravity unknown-field refusals: final report

**Work branch:** `v5/fix-antigravity-unknown-field` at `a5111cf3b8b1fc847a8069333beebf742382fdae`. The branch started from `uam/v5.2-sync-main`, which is already in `uam/v5.2-performance`, and it merges into `uam/v5.2-performance` with no conflicts. There are three commits: `cc98fd92` (the fix), `720a20b2` (review round 1) and `a5111cf3` (review round 2). No PR, issue or comment was created.

**Still unverified:** the application test `SettingsAntigravityUnknownFieldRefusalIsReportedOnceAtStartupUntilTheFileChanges` has not been run. It only builds on macOS or Windows, and this session ran on Linux. Both independent reviews traced it by reading the code and found that every expectation holds.

## What changed

**What's now reported**
- **For the user:** each refused conversation shows one plain line: `Antigravity conversation <id>: Antigravity added a history field UAM can't read yet.`
- **In the log:** one detailed line replaces the plain one. It names the reader version, protobuf message kind, field number and wire type, and never includes field content.
- **Startup reports once:** UAM still tries to read the conversation on every scan, but the automatic startup scan reports it only once. It reports again when the reader version (`kReaderVersion`, now 2) or that history file changes.
- **Chat Data always lists it:** an explicit Chat Data scan lists the refusal every time.
- **Where "already reported" is remembered:** the existing `native-import-tombstones.json` record holds the notes, under `format-refusal|` keys kept separate from the deletion keys. Nothing that reads deletions sees them.
- **Detail caps:** refusals sit outside the reader's and the review list's 20-item detail caps, so other discovery issues are never counted as omitted because of them.

**What's now supported**
- I compared the reader's field lists with the agy-reader protobuf definitions for Antigravity CLI releases 1.3.6 to 1.5.14. Two new plain-text fields are now read:
  - `CortexStepUserInput.client_message_id` (field 19, added in 1.5.1). It's kept as background information only, never as a UAM input ID or write receipt.
  - `CortexStepRunCommand.environment_id` (field 38, added in 1.5.13).
- These stay refused, because they carry media or structured results the reader can't show yet: planner `media` (20) and generic-result `result_struct`/`result_value` (10, 11).

**Docs:** `native-imports.md`, the Antigravity history decision page, the imports architecture note and the Antigravity `native-import` capability row are updated.

## Review rounds

One fresh-context Opus reviewer per round, two rounds (`claude-opus-5-5`).

### Round 1 (review of `cc98fd92`)
7 suggested (0 blockers, 2 should-fix, 5 nits); 5 implemented in `720a20b2`.

Implemented:
- **Should-fix:** the one report could be used up by a startup scan the user never sees. Only the startup scan now uses the notes, and Chat Data always lists refusals.
- **Should-fix:** refusals took up the reader's 20-detail cap. They are now outside it.
- **Nit:** the first report wrote two log lines. It now writes only the detailed one.
- **Nit:** the code comment and the docs gave different rules for raising `kReaderVersion`. Both now say "whenever a field table changes".
- **Nit:** a build from before this change reads the notes as deletion keys. The doc option was taken: the downgrade effect is now documented.

Declined:
- **Notes are never pruned.** At most one note per refused conversation is kept, so the record stays small; this is documented.
- **A wrong-wire `environment_id` gets a misleading reason.** The conversation is still correctly refused; only the wording of the reason is off.

### Round 2 (fresh re-check of `720a20b2`)
3 suggested (0 blockers, 1 should-fix, 2 nits); 3 implemented in `a5111cf3`.

- **Should-fix:** the review list's own 20-line cap still let refusals push other issues into the omitted count. Refusals are now added outside that cap.
- **Nit:** the capability row now says the report-once rule applies to the automatic startup scan.
- **Nit:** a refusal could be recorded as reported when the user only saw it inside the omitted count. The round-2 fix above also resolves this.

## Tests (Linux only)

- **Parser fixtures:** I built `tests/antigravity_history` against the system SQLite 3.45, because the proxy blocks the pinned SQLite download. The project also needed the three build-feature flags passed on the command line. 27 of 30 cases pass, including the three new ones:
  - `UnknownFieldRefusalNamesKindFieldAndWireWithoutContent`
  - `ReadsNewerClientMessageIdAndRunCommandEnvironmentFields`
  - `UnknownFieldRefusalsDoNotUseTheDetailCap`
- **Pre-existing failures:** the 3 inline-image cases fail on the unchanged base too, because that image path only has macOS and Windows code.
- **App code compile:** the `uam_native_import_source_checks` objects for `runtime_orchestration_services.cpp`, `startup_native_import_service.cpp` and both test files compile with GCC. In the same target, `state_serializer.cpp` fails to compile on the unchanged base too (`AddFrontendAsset`).
- **Docs:** `python3 scripts/check_docs.py` passes.
- **Not run:** the full `uam_core_tests` and `ctest` suites, macOS/Windows builds, and any real Antigravity history. Which field the 13 reported conversations use is still unknown; the new log line will name it.

## Screens needing Before/After capture

These are text-only changes, with no layout change:
- the startup history-import review: the error line for a refused Antigravity conversation;
- Chat Data → Native provider histories: the discovery issues list.

## Issue preview

**Title:** `bug(antigravity): unknown-field history refusals are unnamed and repeat every scan`

History import refuses 13 Antigravity conversations on every startup scan with "Antigravity history uses an unrecognised content field; retry with a supported reader." The message doesn't name the field, so it can't be diagnosed, and the same conversations are reported at every scan. This is separate from the "ambiguous original workspaces" refusals.

**Expected:** a refusal names the message kind, field number and wire type in the log (never content), shows one plain line to the user, and is reported once until the reader version or the file changes.
**Actual:** the same unnamed refusal for the same conversations at every scan.

**Environment:** `uam/v5.2-sync-main` at `6f235eb9`; `src/common/provider/antigravity/cli/antigravity_native_history.cpp`, `Proto::Known()` (~151–156).

**Evidence:** User-reported: 52 log lines covering 13 conversation IDs across 4 startup scans. From reading the code, `Known()` throws one fixed string. Hypothesis: newer agy releases add fields such as `CortexStepUserInput.client_message_id` (agy 1.5.1).
**Reproduction status:** reproduced with synthetic fixtures only; no real store was inspected.

**Acceptance criteria:**
- [ ] The log names the message kind, field number and wire type, with no content.
- [ ] The user sees one plain line per refused conversation.
- [ ] A refusal is reported once until the reader version or the file changes.
- [ ] Clearly identified newer fields are supported with synthetic fixtures; unknown fields stay refused.

**Review:** Self-triaged; no independent review of this report.

**Prepared by:** claude-opus-5-5 in Claude Code on the web (scheduled cloud session, Claude Code 2.1.330), on behalf of David Taylor.

## PR preview

**Title:** `fix(antigravity): name unknown history fields and report each refusal once`

Antigravity history refusals for an unknown field now name the protobuf message kind, field number and wire type in the log, never content, and show the user one plain line: "Antigravity added a history field UAM can't read yet." Discovery still retries each conversation every scan. The automatic startup scan reports a refusal once, using a separate entry in the existing native-import tombstone record that deletion handling doesn't read. A new reader version or any file change reports it again. Explicit Chat Data discovery always lists refused conversations. Refusals sit outside the 20-item detail caps. Compared with the agy 1.3.6–1.5.14 public protobuf definitions, the reader now reads `CortexStepUserInput.client_message_id` (19) and `CortexStepRunCommand.environment_id` (38); planner media and the newer generic-result fields stay refused. Changes stay inside `Known()`, the field tables and the `Discover` catch, so they should merge cleanly with the shared protobuf reader work.

**Proof:** On Linux, I built the standalone `tests/antigravity_history` fixture project against the system SQLite 3.45, because the pinned download was blocked. 27 of 30 cases pass, including the 3 new ones. The 3 inline-image cases also fail on the unchanged base under Linux. The `uam_native_import_source_checks` objects for the two changed app files and both test files compile with GCC; `state_serializer.cpp` in that target fails on the unchanged base too. `python3 scripts/check_docs.py` passes. **Not run:** `SettingsAntigravityUnknownFieldRefusalIsReportedOnceAtStartupUntilTheFileChanges`, the full `uam_core_tests`/`ctest` suites, macOS/Windows builds, and any real Antigravity store.

**Review:** 1 independent reviewer, 2 iterations.

- Independent claude-opus-5-5: 10 suggested; 8 implemented. Declined: pruning notes (at most one note per conversation, documented) and the reason wording for a wrong-wire field 38 (the conversation is still refused).

**Created with:** claude-opus-5-5 in Claude Code on the web (scheduled cloud session, Claude Code 2.1.330), on behalf of David Taylor.
