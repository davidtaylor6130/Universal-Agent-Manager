# #512 Manual compaction for Gemini and Antigravity: report

Final SHA: `1b3337fc8ab5960c794137d4ccd2129f2a44a55a` on `v5/feat-512-manual-compaction`, pushed. Base: `origin/uam/v5.2-sync-main`.

Commits:
- `d4da6e79`: docs re-check.
- `ff82c4d8`: implementation.
- `d82dad8c`: review round 1 fixes.
- `1b3337fc`: re-check fixes.

The `ff82c4d8` implementation was pushed before its review finished, because the session's stop hook asked for unpushed commits to be pushed. The review fixes came after it as separate commits.

## What changed (after David's 2026-11-23 answers)

- **Antigravity manual compaction (local chats).**
  - Compact is offered only when the checked `agy` on this computer is 1.3.5 or later and the chat has a saved conversation.
  - It sends native `/compact` on the stream input.
  - While the request is pending, the adapter reads only the checkpoint step: `ACTIVE` shows compacting; `DONE` followed by a `SUCCESS` result records a `context_compaction` marker with empty text. The transcript renders that marker as "Context compacted", with "The provider did not include a summary."
  - A no-op, a failure, or `/compact` answered as a message records nothing and gives one plain line.
  - A changed identity or a new process retires the request and any stale receipt.
  - SSH chats are not offered the action.
- **Typed `/compact <instructions>` for every provider.**
  - Chat routes `/compact` and `/compact <text>` to the compact action, never to an ordinary send.
  - Claude receives `/compact <instructions>`. Other providers run the plain compaction and Chat says, through `reportAppNotice`: "This provider compacts without instructions, so yours were not used."
  - Where compaction is unavailable (Gemini, OpenCode), the draft is kept and the existing unavailable warning is shown.
  - Claude instructions are capped at 4,000 bytes. A deferred request keeps its instructions, and a second deferred request with different text is refused.
- **Docs.**
  - `docs/providers/compaction.md` states the new rules.
  - All six `compaction` rows are updated.
  - The open decision in `docs/documentation-review.md` is closed as "Gemini and Antigravity manual compaction", resolved 2026-11-23 with both answers.
  - The reference section and the companion protocol entry are updated.
  - Gemini stays Partial: there is still no upstream ACP control, and no UAM fallback for it was agreed.
- **Tests.**
  - Native, in `tests/antigravity_tests.cpp`, `tests/manual_compaction_startup_tests.cpp` and `tests/claude_native_compaction_tests.cpp`:
    - the Antigravity request;
    - the marker outcome, and the no-op, error, answered and foreign-identity outcomes;
    - the stale receipt cleared by a new process;
    - Claude instructions;
    - instruction routing and dropping, and deferred instructions;
    - refusals for an old, unknown or remote version and for overlong text.
  - UI: the parser unit test, plus ChatView tests for routing with instructions used and unused, refusal when compaction is unavailable, and a backend refusal that keeps the draft.

## Open questions and limits (not decided here)

- **SSH Antigravity chats:** they are not offered compaction, because remote maintenance delivery and reconnect replay are not implemented for this stream. Should a later slice add it?
- **Automatic Antigravity compaction during an ordinary turn** is still not shown in the transcript.
- **History scans after compaction:** the native history reader refuses unknown step types, and the checkpoint step's type number is unknown. Native history refresh, import and folder scans may therefore report a compacted Antigravity conversation as unimported until someone captures a real checkpoint fixture. UAM's own saved chat history is unaffected.
- **Unverified agy behaviour.** Whether agy keeps or resets `num_turns` around a checkpoint is unverified. If it resets them, the next ordinary turn could be dropped and wait for the inactivity timeout. This has to be checked live.
- **Pending gates:** live verification against real `agy` and `claude`, Claude review of the React change (required by AGENTS.md), and your acceptance.

## Review rounds

Every round used a fresh-context `claude-opus-5-5` sub-agent.

**Docs phase (earlier):**
- Round 1: 14 suggested, 14 implemented.
- Re-check: 8 suggested, 6 implemented. Two were declined:
  - A pre-existing sentence that is correct in context.
  - The commit-trailer order, which your instruction fixes.

**Implementation, round 1:** 0 blockers, 5 should-fix, 5 nits; 10 suggested, 9 implemented (two of them partly).
- **Declined:** passing instructions as an `OnAcpBuildCompact` parameter instead of a session field. Keeping the field leaves the hook signature unchanged across six adapters and their tests. The field is set and cleared around the single call, and a test asserts it is cleared.
- **Partial, tests:** there is no Antigravity serializer-availability test, because the existing one needs a real owned-process fixture that only runs on macOS. The shared version-gate helper that the serializer calls is unit-tested instead. There is also no late-result test, because the matching guard was declined (next item).
- **Partial, turn counter:** I stopped advancing the turn counter from the compaction result, but declined a `num_turns` duplicate guard. With agy's `num_turns` behaviour unknown, that guard could leave a compaction stuck. The limit is documented.

**Implementation, re-check (fixes were material):** 0 blockers, 2 should-fix, 3 nits; 5 suggested, 5 implemented.

**Total:** 37 suggested, 34 implemented.

## Checks run

- `npx vitest run` (UI-V2): 240 files and 4,222 tests passed. `npx tsc --noEmit -p .`: clean. `npm run build`: succeeded.
- `python3 scripts/check_docs.py`: passed (203 pages, 1,518 links, 6 providers × 32 capabilities).
- Runner build (`uam_runner`): passes. sqlite.org is blocked here, so for this scratch build only I pointed FetchContent at a SQLite 3.54.1 amalgamation from an npm package; the pinned version is 3.54.0. Nothing about this was committed.
- **Native code was not compiled or run.** The desktop app and native tests refuse Linux, and CEF downloads are blocked. The changed native sources and the three test files were syntax-checked with g++ against stub CEF headers: no new errors, only the known stub noise in CEF bridge headers.
- No provider CLI was run, and no token test was done.

## Issue previews

**Comment on #512 (preview, not posted):**

> Implemented on `v5/feat-512-manual-compaction` (head `1b3337fc`), following David's answers of 2026-11-23.
>
> - **Antigravity:** for chats on this computer whose checked `agy` is 1.3.5 or later, Compact sends native `/compact`. The adapter maps the `checkpoint` step to compacting and then completed, and the chat records a "Context compacted" marker that says the provider gave no summary. A no-op, failure, or `/compact` answered as a message records nothing and gives one plain line. SSH chats are not offered it.
> - **Typed instructions:** `/compact <instructions>` now always takes the compact action. Claude receives the instructions natively. Other providers run the plain compaction, and Chat says in one line that the instructions were not used. Gemini and OpenCode keep the draft and say compaction is unavailable.
> - **Gemini:** re-checked through 0.71.0-nightly.20261123. There is still no ACP compaction control, so the row stays Partial.
> - **Still open:**
>   - Live checks with real `agy` and `claude`.
>   - Whether `agy` resets `num_turns` after a checkpoint.
>   - A real checkpoint fixture for the native history reader; until then, scans may report a compacted conversation as unimported.
>   - SSH support for Antigravity compaction.
>   - Claude review of the React change, and David's acceptance.
> - **Proof:** UI tests, typecheck and build pass, and the docs check passes. Native code was syntax-checked only, because the native tests cannot be built on Linux.
>
> **Prepared by:** Claude Opus 5.5 (`claude-opus-5-5`) in Claude Code on the web (Claude Agent SDK), on behalf of David Taylor.

**New issue (preview, not filed):**

> **Title:** `bug(antigravity): native history may refuse conversations after /compact`
>
> UAM's Antigravity history reader (`antigravity_native_history.cpp`) refuses any step type it does not recognise. Compaction writes a checkpoint step whose type number is not known to UAM. After a UAM Compact, native history refresh, import and folder history scans may report that conversation as unimported.
>
> **Expected:** a compacted conversation imports, with the checkpoint shown as a compaction marker.
> **Actual:** not observed. This is a hypothesis drawn from the reader's refuse-unknown rule.
>
> **Evidence:** read from source. A third-party report (getpaseo/paseo#10527) shows a `CHECKPOINT` row with status 3 in `conversations/<id>.db`. **Reproduction status:** not attempted; it needs a real `agy` 1.3.5+ conversation fixture.
>
> **Acceptance criteria:**
> - [ ] A captured checkpoint row is decoded or skipped safely, and a compacted conversation imports.
>
> **Review:** self-triage only. **Prepared by:** Claude Opus 5.5 (`claude-opus-5-5`) in Claude Code on the web (Claude Agent SDK), on behalf of David Taylor.

## PR preview (not opened)

**Title:** `feat(compaction): add Antigravity manual compaction and route /compact instructions`

> Adds manual compaction for Antigravity and routes typed `/compact <instructions>` through the compact action for every provider, as David decided on 2026-11-23.
>
> - **Antigravity (local chats, checked `agy` 1.3.5 or later):** Compact sends native `/compact`. The checkpoint step drives the compacting state and then a "Context compacted" marker without summary text. No-ops, failures and answered-as-message outcomes record nothing and give one plain line.
> - **Typed instructions:** Claude receives them natively. Other providers run the plain compaction and say in one line that the instructions were unused. Unavailable providers keep the draft.
> - **Docs:** the compaction agreement, all six compaction rows, the closed decision and the reference detail are updated.
> - **Limits:** Gemini stays Partial (no upstream control). SSH Antigravity chats are not offered compaction. Automatic Antigravity compaction is not shown, and native history scans may report compacted Antigravity conversations as unimported.
>
> Related to #512
>
> **Proof:**
> - UI-V2: `npx vitest run` 240 files / 4,222 tests pass, with new routing and parser tests; `npx tsc --noEmit -p .` is clean; `npm run build` succeeds.
> - `python3 scripts/check_docs.py` passes.
> - `uam_runner` builds. The scratch build used a local SQLite 3.54.1 because sqlite.org was blocked; the pinned version is 3.54.0.
> - New native tests cover the Antigravity adapter, Claude instructions and `CompactAcpSession` routing. They were syntax-checked against stub CEF headers but **not compiled or run**: the native targets need macOS or Windows.
> - No live provider CLI was run.
>
> **Review:** 1 independent reviewer, 4 iterations. Independent claude-opus-5-5: 37 suggested, 34 implemented.
> - Declined: the commit-trailer order (your instruction); a pre-existing sentence that is correct in context; and passing instructions as a hook parameter (it would change the hook signature across six adapters).
> - Two suggestions were done in part: a serializer-availability test, which needs a macOS-only owned-process fixture; and the `num_turns` duplicate guard, which was declined to avoid a stuck compaction.
>
> **Created with:** Claude Opus 5.5 (`claude-opus-5-5`) in Claude Code on the web (Claude Agent SDK), on behalf of David Taylor.

## Screens needing Before/After capture

1. **Claude chat:** type `/compact keep the API notes` and Send. Before: sent as an ordinary prompt. After: the compacting divider, then the completed marker, with no extra notice.
2. **Codex or Copilot chat:** type `/compact keep the API notes` and Send. After: compaction runs and the toast "This provider compacts without instructions, so yours were not used." appears.
3. **Gemini or OpenCode chat:** type `/compact keep the API notes` and Send. Before: sent as a prompt. After: the warning "Manual compaction is unavailable for this chat." and the draft is kept.
4. **Antigravity local chat (agy 1.3.5+):**
   - the slash palette now lists `/compact`;
   - the compacting divider;
   - the completed marker expanded, showing "The provider did not include a summary.";
   - the one-line notice after a no-op ("…It may have nothing to compact yet.").
5. **Antigravity SSH chat, and a local chat with agy older than 1.3.5:** `/compact` is not offered in the palette.
