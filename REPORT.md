# #512 Manual compaction for Gemini and Antigravity: report

Final SHA: `d4da6e79bcb7870bce99365467d93dfc59fc4ffe` on `v5/feat-512-manual-compaction`, pushed. Base: `origin/uam/v5.2-sync-main`.

I made a docs-only change. The scoping slice allows an implementation only if a summary can be read, and none can. Gemini still has no ACP compaction control. Antigravity has a native trigger, but there is no summary anywhere a client can read it. No provider CLI was run, and no account or token was used. The official antigravity.google docs host is blocked from this environment. I used first-party sources instead: the Antigravity CLI changelog in Google's GitHub repo, Google's SDK proto, and gemini-cli source and npm packages. Third-party issue reports fill in the stream evidence.

## Findings

- **Gemini.** It is still blocked through 0.70.0, 0.71.0-preview.0 and 0.71.0-nightly.20261123.
  - The ACP command handler registers only Memory, Extensions, Init, Restore, About and Help. The source file is identical at both tags, and the bundles match.
  - There is no extension method and no compression event.
- **Antigravity: trigger.**
  - The changelog dates `/compact` to 1.3.5. Print-mode slash expansion arrived in 1.1.9, with `--disable-slash-commands` to opt out, and stream input in 1.1.15.
  - UAM does not pass `--disable-slash-commands`.
  - Third-party captures on 1.3.5–1.4.12 show the stream then reports a `checkpoint` step at `DONE` and an empty `SUCCESS` result, with no summary text.
- **Antigravity: no summary source.**
  - The SDK schema's step-update compaction message is empty. The summary exists only in the SDK `OnCompaction` hook, and the CLI has no compaction hook.
  - The native store row is undocumented.
  - `agy acp` sends no compaction notification through 1.5.13 (antigravity-cli#1509, open).
- **Known gap (recorded, not fixed).** Chat intercepts only an exact `/compact` draft.
  - `/compact <message>` falls through as ordinary input for every provider.
  - For Antigravity, a resulting native compaction on the reported versions is not shown, because the adapter counts checkpoint usage but does not display or record the step.
  - This conflicts with `docs/providers/compaction.md`. The remedy is part of open question 2 below.

## Files changed

- `docs/providers/gemini/gemini.md` and `docs/providers/antigravity/antigravity.md`: the `compaction` rows stay Partial with an updated reason.
- `docs/reference/architecture/provider-sessions.md`: new section "Gemini and Antigravity manual compaction checks" holding the evidence.
- `docs/documentation-review.md`: the open-decision row carries the questions, and the 2026-10-23 stream-input note gets a dated follow-up.
- `docs/reference/antigravity-runtime-goals.md` and `docs/reference/provider-runtime-parity.md`: short rows linking to the section. I also removed a stale "native history import unavailable" clause that contradicted the Partial `native-import` row.

## Open questions for David (left out of the change)

1. Is Antigravity's native completion step without summary text enough to meet "records the summary"? If yes, the next slice would do three things:
   - send `/compact` from the compact action;
   - map `checkpoint` to the compacting and completed states;
   - record a marker without summary text.
   Claude's action sends the same text, but Claude also returns a compact boundary and a summary.
2. Should a `/compact <message>` draft be refused, or routed through the compact action? This applies to all providers.

## Review rounds

Both rounds used a fresh-context `claude-opus-5-5` sub-agent.

- **Round 1: 14 suggested (1 blocker, 5 should-fix, 8 nits), 14 implemented.**
  - Blocker: the new claim conflicted with the 2026-10-23 stream-input note.
  - Should-fix:
    - The stream claim was attributed to the changelog instead of the third-party captures.
    - The leading-text caveat for batched or prefixed prompts was missing.
    - The `/compact <message>` fall-through was not recorded against the compaction agreement.
    - The reference described the wrong reply for an exact `/compact`.
    - The same facts were repeated across five files.
  - Finding 4 offered two fixes: a code guard, or recording the gap. I recorded the gap. A guard would pre-empt open question 2, and a global guard would also change Claude, whose CLI may expand `/compact <message>` natively.
- **Round 2 (re-check, because round-1 fixes were material): 8 suggested (0 blocker, 2 should-fix, 6 nits), 6 implemented.** The should-fixes were narrowing "1.3.5 and later" to the reported 1.3.5–1.4.12, and qualifying the Claude comparison. Two were declined:
  - Old sentence at `provider-sessions.md:31`: it predates this change, and in context it describes Codex's idle-start compaction route, so it is not wrong here.
  - Trailer order: your instruction says the commit message must end with the `Co-Authored-By` line, so `Claude-Session` stays above it.
- **Total: 22 suggested, 20 implemented.**

## Checks run

- `python3 scripts/check_docs.py`: passes (203 pages, 1518 local links, 6 providers × 32 capabilities).
- No native or UI code changed, so I did not run the runner build, vitest, tsc or npm build.

## Issue previews

**Comment on #512 (preview, not posted):**

> Re-check on 2026-11-23, from public sources and published packages only. No provider CLI was run and no account or token was used, so nothing here is live-verified.
>
> **Gemini: still blocked.**
> - The ACP command handler registers only Memory, Extensions, Init, Restore, About and Help ([`acpCommandHandler.ts` at v0.70.0](https://github.com/google-gemini/gemini-cli/blob/8468962efadfbcdfd988a8fd04af4110a68c886e/packages/cli/src/acp/acpCommandHandler.ts#L23-L31)).
> - That file is unchanged at the `v0.71.0-nightly.20261123` tag. The published 0.70.0, 0.71.0-preview.0 and 0.71.0-nightly.20261123 bundles contain the same registry.
> - The ACP agent has no extension method, and the prompt loop handles no compression event.
> - Sending `/compress` as text would be an ordinary model prompt, which the open decision rules out.
>
> **Antigravity: a native trigger exists, but there is no summary.**
> - **Changelog.** The first-party [CLI changelog](https://github.com/google-antigravity/antigravity-cli/blob/1eedbb78b37c8acde91394fc7fef3ed33409328d/CHANGELOG.md) shows:
>   - 1.1.9 added slash expansion in print mode, with `--disable-slash-commands` to opt out.
>   - 1.1.15 added stream-json input.
>   - 1.3.5 added `/compact`. On its own it summarises and ends the turn; `/compact <message>` summarises and then answers. The 1.3.5 entry describes the terminal UI.
> - **Stream input.** Two third-party reports show `/compact` also running natively from stream input:
>   - On 1.4.12, [codex-host#746](https://github.com/BytePioneer-AI/codex-host/issues/746) logged `expanded slash command "compact"`. The stream then showed a `checkpoint` step reaching `DONE`, followed by a `SUCCESS` result with an empty response.
>   - On 1.3.5–1.4.0, [untether#1114](https://github.com/littlebearapps/untether/issues/1114) reported the same outcome, with input tokens falling afterwards.
>   - Neither report saw summary text. UAM does not pass `--disable-slash-commands`.
>   - These reports supersede, for `/compact` only, the 2026-10-23 recheck that said stream input rejects CLI-handled slash commands.
> - **No summary anywhere.**
>   - Google's SDK schema has an empty `ActionCompaction` on the step update. The summary appears only in the SDK's `OnCompaction` hook ([`localharness.proto`](https://github.com/google-antigravity/antigravity-sdk-python/blob/97c5c53d0741c601ea8c2401497cad3b5ee8dc98/google/antigravity/proto/localharness.proto#L424)).
>   - The CLI reports no compaction hook.
>   - The native store's checkpoint row is an undocumented internal format.
>   - [antigravity-cli#1509](https://github.com/google-antigravity/antigravity-cli/issues/1509) (open) reports that `agy acp` sends no compaction notification through 1.5.13.
>
> **Known gap.** Chat intercepts only an exact `/compact` draft. UAM's Antigravity adapter counts a checkpoint step's usage but does not display or record the step.
>
> As a result, when `/compact <message>` is the leading text of the delivered prompt, it reaches `agy`. On the reported versions (1.3.5–1.4.12), `agy` then compacts without UAM showing it. For other providers the same draft goes out as ordinary input. The compaction agreement already says typing the command must not fall through, so this is recorded as a known gap. It is not fixed here because the remedy is part of question 2.
>
> **Recorded in docs** (branch `v5/feat-512-manual-compaction`, commit `d4da6e79`):
> - Both `compaction` rows stay Partial.
> - The open decision links to a new "Gemini and Antigravity manual compaction checks" reference section.
> - The older stream-input note now points to the newer evidence.
>
> No code changed. `scripts/check_docs.py` passes; that checks links and rows, not runtime behaviour.
>
> **Decision needed from David:**
> 1. Is Antigravity's native completion step without summary text enough to meet "records the summary"? Claude's compact action also sends `/compact` as stream user content. Claude then reports a compact boundary and a summary record, so only the trigger is comparable. If yes, the next slice would:
>    - send the native `/compact` line from the compact action;
>    - map the `checkpoint` step to the compacting and completed states;
>    - record a compaction marker with no summary text.
> 2. Should a `/compact <message>` draft be refused, or routed through the compact action? This applies to every provider, because Chat intercepts only an exact `/compact`.
>
> **Prepared by:** Claude Opus 5.5 (`claude-opus-5-5`) in Claude Code on the web (Claude Agent SDK), on behalf of David Taylor.

**New issue (optional; preview, not filed).** File this only if you want the fall-through tracked separately from #512's question 2.

> **Title:** `bug(chat): /compact with a message falls through to the provider as input`
>
> Chat intercepts a draft only when it is exactly `/compact` (`UI-V2/src/components/views/ChatView.tsx`, `submit`). A draft such as `/compact keep the API notes` goes out as ordinary input for every provider. This breaks the [compaction agreement](docs/providers/compaction.md), which says typing the command must not fall through to an ordinary prompt.
>
> For Antigravity, `agy` 1.3.5–1.4.12 is reported to expand it as native compaction when it is the leading text. UAM neither shows nor records that compaction, because the adapter does not display checkpoint steps.
>
> **Expected:** the draft is kept and refused with one plain notice, or it is routed through the provider's compact action where one exists, as David decides.
> **Actual:** the draft is sent as input.
>
> **Evidence:** read from source and recorded in `docs/reference/architecture/provider-sessions.md` ("Gemini and Antigravity manual compaction checks"). **Reproduction status:** not attempted live.
>
> **Acceptance criteria:**
> - [ ] A `/compact <message>` draft never reaches a provider as ordinary input.
> - [ ] The draft and attachments are preserved when it is refused.
>
> **Review:** self-triage only. **Prepared by:** Claude Opus 5.5 (`claude-opus-5-5`) in Claude Code on the web (Claude Agent SDK), on behalf of David Taylor.

## PR preview (not opened)

**Title:** `docs(providers): record manual compaction re-check for Gemini and Antigravity`

> Records the #512 re-check of manual compaction. The change is docs only.
>
> - **Gemini:** still has no ACP compaction control through 0.71.0-nightly.20261123.
> - **Antigravity:** runs `/compact` natively from stream input on the reported 1.3.5–1.4.12. The stream reports only a checkpoint step with no summary text, and no documented store or CLI hook provides one.
> - **Status:** both `compaction` rows stay Partial, and the open decision now carries the evidence and two questions for David.
> - **Known gap:** a `/compact <message>` draft falls through as input. This is recorded against the compaction agreement and not fixed, because its remedy is one of those questions.
> - **Evidence:** the detail sits in a new "Gemini and Antigravity manual compaction checks" section of the provider-sessions reference.
>
> Related to #512
>
> **Proof:**
> - `python3 scripts/check_docs.py` passes (203 pages, 1518 local links, 6 providers × 32 capabilities). This checks documentation integrity, not runtime behaviour.
> - Evidence was read from gemini-cli source at the v0.70.0 and 0.71.0-nightly.20261123 tags, and from the published 0.70.0, 0.71.0-preview.0 and nightly npm bundles. The Antigravity CLI changelog and SDK proto were read at pinned commits. Third-party issue reports were read as published.
> - No provider CLI was run and nothing was live-verified. The antigravity.google docs host was unreachable from the build environment. No code changed, so no native or UI build or test was run.
>
> **Review:** 1 independent reviewer, 2 iterations. Independent claude-opus-5-5: 22 suggested, 20 implemented. The two declined nits were a pre-existing sentence that is correct in context, and the commit-trailer order, which your instruction fixes.
>
> **Created with:** Claude Opus 5.5 (`claude-opus-5-5`) in Claude Code on the web (Claude Agent SDK), on behalf of David Taylor.

## Screens needing Before/After capture

None. The change is docs only, with no UI or interaction change.
