# Agent report: remove dormant cold-retry scaffolding

**Work branch:** `v5/refactor-remove-cold-retry`
**Final SHA:** `69e98fae`, based on `uam/v5.2-sync-main` at `6f235eb9`

Commits:

- `d025d3d5`: the removal
- `5ea7af5c`: round-1 review fixes
- `69e98fae`: round-2 nit

The branch was pushed as a normal fast-forward. No PR, issue, comment, label or merge was made.

## Result

The dormant cold image Retry launch scaffolding is removed. The branch is unbuilt: no real build or test run was possible on Linux.

No Retry caller ever created the binding, so every removed branch was unreachable and behaviour is unchanged. The only output difference is that the `[remote-restore]` diagnostic drops its constant `cold_binding=0` field.

Removed:

- `ColdRetryBinding` and `PrepareColdRetryLaunchBinding`, with the helpers that served only them: `RevokeColdRetryBinding`, `PollColdRetryBinding`, `ColdRetryFinalWriteWitness` and `ObserveColdRetryFinalWrite`.
- The `AcpSessionState::cold_retry_binding` field and its forward declaration.
- The `cold_binding` parameter of `StartAcpProcessForChat`, its branches, and two call sites that passed `{}`.
- The unused `cold_request_id` parameter of `WriteCommittedRemotePrompt`.
- The cold-binding branches in prompt admission, staging, delivery and session setup.
- `acp_cold_retry_witness.h`.

The live "cold launch" path (`StageAcpColdLaunch`) is untouched. The OpenCode version-gate region (base lines 2555–2580) is byte-identical.

## Lines removed

The total is 834 lines removed and 50 added across 13 files in the first commit. Review fixes added about 13 lines across 4 files.

| File | Removed | Added |
|---|---|---|
| `acp_session_lifecycle.cpp` | 406 | 11 |
| `acp_cold_retry_witness.h` (deleted) | 207 | 0 |
| `tests/chat_birth_preservation_tests.cpp` | 170 | 20 |
| `acp_session_internal.h` | 29 | 2 |
| `app_state.h` / `acp_session_runtime.cpp` / `claude_preinput_observation.h` | 3 / 3 / 4 | 0 / 3 / 4 |

## Tests removed or rewritten

- **Deleted:** three cases that only tested the removed `cold_retry::Sequence` class:
  - `ColdWitnessSequenceRefusesCopiedIdentityReplacementStopAndExpiredCreators`
  - `ColdWitnessSequenceRequiresStartupHandoffAndExactFinalPublicationBeforeOneWrite`
  - `ColdWitnessSequenceSealsActualLaunchWithoutReplacementOrMetadataRebinding`
- **Rewritten:** `ColdWitnessSequenceUsesActualBindingAndPacketPublicationCallbacksWithoutProcesses` is now `OwnedPublicationChainsBindingAndPacketReceiptsWithoutProcesses`.
  - It tested the repository's live chained publication callbacks through `Sequence`, so the live checks stay and `Sequence` is gone.
  - Cases: `none`, `foreign-primary`, `owner-loss` and `newer-save`.
  - Every failing case asserts the fact stays Committed while the receipt is withheld.
- **Renamed only:** `ColdWitnessPublicationCancellation…` is now `OwnedPublicationCancellationPreservesOriginalAndNewerSaveOwners`, because it never used the removed code.
- **Kept:** `ColdRetryFixture` in `chat_icon_handler_tests.cpp`, which tests live cold-history Retry.

## Review rounds

Both rounds used an independent `claude-opus-5-5` reviewer with a fresh context.

**Round 1: 6 suggested, 5 implemented (0 blockers, 1 should-fix, 5 nits).** It found no compile errors, no broken call sites, no behaviour change and no edits to the OpenCode region.

1. **Should-fix, implemented:** the rewrite had dropped a case. In it, the owner gives up the save after the file is already written, so the fact still reports Committed but no receipt is returned. It is restored as the `owner-loss` case.
2. **Implemented:** the test asserts the receipt is withheld on every failing case.
3. **Implemented:** reworded a `provider-sessions.md` sentence that described the removed publication fence.
4. **Implemented:** the durability-inventory row names saved-image cold Retry again, as refused before launch.
5. **Implemented, adapted:** the removal is recorded as a resolved decision. It says "product owner", not David's name, because only the scheduled task names a product owner and that could not be confirmed as David.
6. **Declined:** amending the first commit to add the `Claude-Session:` trailer would need a force-push, which the task forbids. The later commits carry the trailer and end with `Co-Authored-By`.

**Round 2 (re-check of the fixes): 1 suggested, 1 implemented.** It traced the restored case through the live save code and confirmed its assertions should hold. Its one nit was to give the decision its own paragraph in `documentation-review.md`, which is done.

**Total: 7 suggested, 6 implemented.**

## What's unverified

- **Not run:** the native build, `uam_core_tests` and CTest. CMake refuses Linux and the CEF host is blocked here, so no new or rewritten test has run.
- **What did run:**
  - A syntax-only compile with GCC 13 and Clang 18 against stand-in CEF headers, for the changed source and test files, with the Claude runtime both on and off.
  - A search showing no removed name remains in source, tests or build files.
  - `python3 scripts/check_docs.py`, which passes.
- **Pending:** Claude review and David's approval.
- **Decision for David:** three things now have no production caller, because the removed code was their only user. They were left in place as out of scope:
  - `BeginOwnedChatPublicationAdmission` and its stopped-launch receipt chain.
  - `claude_preinput::Arm`, `CurrentWitness` and `NativeWitness`.

  They still have their own tests. Removing them would be a follow-up that needs David's approval.

## Screens needing Before/After capture

None. This change touches only native code, tests and docs, with no UI.

## Issue preview (not filed)

**refactor(acp): remove dormant cold-retry launch scaffolding**

The product owner decided on 2026-11-23 to remove the cold image Retry launch scaffolding. Git history keeps it for when the feature is scheduled. No Retry caller ever created the binding, and its own comment says "No current Retry admission caller creates this object." Behaviour must not change.

**Desired outcome:** remove `ColdRetryBinding`, `PrepareColdRetryLaunchBinding`, the `cold_binding` branches in `StartAcpProcessForChat`, and `acp_cold_retry_witness.h`. Simplify the call sites. Delete tests that only exercise the removed code, and rewrite any test of live behaviour that goes through it.

**Environment:** `uam/v5.2-sync-main` at `6f235eb9`.

**Evidence:** a read-only audit of the source. Every `StartAcpProcessForChat` caller passes an empty binding, and only that function sets `cold_retry_binding`.

**Acceptance criteria:**

- [ ] No removed symbol remains in source, headers, CMake files or tests.
- [ ] The macOS and Windows native builds and `uam_core_tests` pass.
- [ ] The docs record the removal and how to recover it.

**Review:** self-triage only.

**Prepared by:** claude-opus-5-5 in Claude Code on the web (Claude Code), on behalf of David Taylor.

## PR preview (not opened)

**refactor(acp): remove dormant cold-retry scaffolding**

Removes the cold image Retry launch scaffolding the product owner retired on 2026-11-23: the binding, its process-free transition sequence, the final-write witness and the `StartAcpProcessForChat` parameter, plus every branch that read them. The binding was always null, so behaviour is unchanged. The only output difference is that the `[remote-restore]` diagnostic drops its constant `cold_binding=0` field.

Three tests that only exercised `cold_retry::Sequence` are deleted. The test of the repository's chained publication callbacks is rewritten without `Sequence`. It keeps the foreign-primary, newer-save and owner-loss cases. The docs record the removal as a resolved decision and point to `6f235eb9` for recovery.

**Proof:**

- A syntax-only compile with GCC 13 and Clang 18 against stand-in CEF headers passes for the changed source and test files, with the Claude runtime both on and off.
- A search finds no remaining removed names in source, tests or build files.
- `scripts/check_docs.py` passes.
- Not run: the native build and `uam_core_tests`, which need macOS or Windows.

**Review:** 1 independent reviewer, 2 iterations. Independent claude-opus-5-5: 7 suggested, 6 implemented. One trailer change declined because it needs a force-push.

**Created with:** claude-opus-5-5 in Claude Code on the web (Claude Code), on behalf of David Taylor.
