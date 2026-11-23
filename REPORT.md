# Agent import UX: report for `v5/fix-agent-import-visibility`

**Final work-branch SHA:** `d078858d54a5ca13c68c661fed8e1f2158eea720`

The branch has three commits on `origin/uam/v5.2-sync-main`:
- `448c0f99` is the change.
- `0cfacfa1` fixes the first review round.
- `d078858d` fixes the re-check.

Every commit ends with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Nothing is open: no PR, issue or comment.

## Review rounds

Both reviewers were Opus sub-agents with a fresh context (`opus` alias, serving `claude-opus-5-5`). I wrote neither review myself.

- **Round 1** reviewed the full diff: 14 suggested (0 blockers, 4 should-fix, 10 nits), 13 implemented.
  - **Declined:** nit #10. It asked to move the merge of global and workspace agents out of the bridge handler and into the agent service. That file belongs to the parser branch (`v5/fix-agent-import-parser`), so I recorded it as a follow-up in `docs/documentation-review.md`.
  - **Done another way:** nit #7 (namesake agents are not listed side by side). I corrected the docs and comment wording instead of sorting the list.
- **Round 2** re-checked only the round-1 fixes: all 7 confirmed, plus 4 new findings (2 should-fix, 2 nits). All 4 implemented.
  - To check the native test code, this reviewer compiled it on its own against nlohmann/json 3.11.3 with stub CEF types, using g++ and clang++ (`-std=c++20 -Wall -Wextra`). It compiled cleanly and its checks passed.

**What the review changed:**
- The Replace confirmation names each change it would make: the agent becoming delegate-only or no longer usable by other agents, gaining or losing write access, and losing skills or delegates.
- Preview stays busy until saved agents are listed, so Import can't turn into Replace under the pointer.
- Navigation happens only from the Agents page, and the highlight is set only if the section really changes.
- When a global and a workspace agent share a name, their action, dialog and delete-confirmation names include the scope. Read-only dialogs are named "View".
- A delegate-only favourite shows a status saying so.
- Connected native handler cases were added: `ChatAgentListsAddDelegateOnlyAndWorkspaceAgentsOnRequest` and `ChatAgentImportReplacesOnlyWhenRequested`.
- Smaller fixes:
  - Badge lost its unused tone.
  - There is now one helper for raw bridge errors.
  - The docs say the delegate-only listing is desktop only, and record that the phone pickers still list chat agents only.
  - The Codex and Antigravity checklist rows now state what UAM does, not what the provider can do.
  - The review status is kept in the known-gaps row.

## Issue preview

**Title:** `bug(agents): custom agent import fails silently or lands where no list shows it`

Importing a provider agent in Settings › Agents often looks like it did nothing. A failure shows a generic "Import failed… The source or target may have changed". A success says "Imported X", but the agent appears in no list. Source: David's report of the import UX.

**Reproduction:** Preview and import a Claude Code agent file with the default scope, then look in Central Setup and the composer agent picker. Re-import the same file.
**Expected:** a success shows where the agent went; a failure says why; a re-import can replace.
**Actual:** the agent isn't listed anywhere, and a re-import always fails with the generic message.

**Environment:** `uam/v5.2-sync-main` at `6f235eb9`.

**Evidence (from the source, not seen in the running app):**
- `sessionsSlice.ts` returns `false` on a refusal and drops `response.error`.
- Neither the UI nor `HandleImportProviderAgent` passes `replaceExisting`.
- The install scope defaults to `workspace`, but `HandleListAgentDefinitions` loads with an empty workspace root.
- `HandleListUamAgents` and the store both drop sub-agents.
- `PreviewProviderAgentImport` sets every non-OpenCode import to `subagent`.

**Reproduction status:** not reproduced in the running app; confirmed by reading the code.

**Acceptance criteria:**
- [ ] A refused import shows the backend reason in one notice line.
- [ ] Re-importing an existing ID offers a Replace that asks for confirmation.
- [ ] The install scope defaults to All workspaces.
- [ ] Central Setup lists the active workspace's agents with a scope badge.
- [ ] Sub-agents are listed with a Delegate-only badge.
- [ ] A successful import highlights the agent where it now appears.

**Review:** self-triage only.
**Prepared by:** claude-opus-5-5 in a Claude Code cloud session (host app not recorded; Claude Code 2.1.330), on behalf of David Taylor.

## PR preview

**Title:** `fix(agents): show why an agent import failed and where it landed`
**Base ← head:** `uam/v5.2-sync-main` ← `v5/fix-agent-import-visibility`

**Failures:** a refused import shows the backend's reason in one notice line, for example "Agent reviewer was not imported. The imported OpenCode permission policy requires read access." The form stays filled.

**Replace:** if the chosen scope already has that ID, Import becomes **Replace agent…**. Its confirmation names every change: becoming delegate-only, gaining or losing write access, losing skills or delegates. Only that confirmation sends `replaceExisting`. The import handler now reads it, and the service still refuses a replacement that would drop or change a saved permission policy.

**Scope and lists:**
- The install scope defaults to All workspaces.
- Central Setup lists the open chat's workspace agents with All workspaces / This workspace badges. Workspace agents are view-only there.
- Sub-agents carry a Delegate-only badge in Central Setup.
- The desktop chat pickers list sub-agents as options that can't be selected. The cycle and favourites skip them, and the backend still refuses them as a chat agent.
- The phone apps don't send the new `includeDelegateOnly` flag, so their pickers are unchanged.

**After a successful import:** if the user is still on the Agents page, Settings opens Central Setup with the agent's row scrolled into view, focused and highlighted for 4 seconds.

**Scope of native changes:** limited to the two list handlers and the import handler's request and reply. The importer (`agent_definition_service.cpp`) is unchanged.

*Before/After captures from the real app are still needed:* see the last section.

**Proof:**
- **Vitest:** the full suite passed at `0cfacfa1` (240 files, 4,233 tests). After the round-2 change, its two affected files passed (147 tests).
- **Regression proof:** run against the original `SettingsModal.tsx`, 6 of the 7 original import tests fail. The seventh is a guard case that should pass either way. All of them pass with the fix.
- **Type-check:** `npx tsc --noEmit -p tsconfig.app.json` is clean. The requested `npx tsc --noEmit -p .` also exits 0, but it checks nothing, because the root `tsconfig.json` has `files: []`.
- **Build:** `npm run build` passed. It first caught a type-name clash, which I fixed.
- **Docs:** `python3 scripts/check_docs.py` passed.
- **Visual check:** the real components on a temporary page with mocked bridge replies, in Chromium, in the default dark and light themes. That is not product evidence.
- **Not run:**
  - the native desktop build and `uam_core_tests` (CMake refuses Linux, and there are no CEF headers);
  - the connected handler program, including the two new cases (macOS CI only);
  - the real app, macOS or Windows, and any provider CLI.

**Review:** 2 independent reviewers over 2 iterations.
- Agent 1: claude-opus-5-5 (fresh context, full diff), 14 suggested, 13 implemented. Declined: moving the list merge into the agent service, because the parser branch owns that file; recorded as a follow-up.
- Agent 2: claude-opus-5-5 (fresh context, re-check of the fixes), 4 suggested, 4 implemented.
- David's acceptance is pending.

**Created with:** claude-opus-5-5 in a Claude Code cloud session (host app not recorded; Claude Code 2.1.330), on behalf of David Taylor.

## Screens needing Before/After capture

1. **Settings › Agents › Import from a provider:** the install scope default; an existing ID showing its hint and the **Replace agent…** button.
2. **The Replace confirmation dialog**, with its list of changes.
3. **The failure notice toast** after a refused import.
4. **Settings › Central setup › Agent store:** scope and Delegate-only badges; the highlighted row right after an import; a global and a workspace agent that share a name.
5. **The composer agent picker**, both the inline prefix and the toolbar menu, with a Delegate-only option.
6. **Settings › Agents › Agent cycle:** a favourite that is delegate-only, showing its status line.

## Coordination

- **Parser branch:** if it stops forcing sub-agent mode, update the implementation note in `docs/settings/central-setup/agents.md` and the Claude Code, Gemini and Copilot `agent-selection` rows.
- **Notifications branch:** that branch moves backend text into the notice detail. The import notice deliberately keeps the reason in its one-line message, as asked. Notification components are untouched.
- **Type name:** a `ProviderAgentImportResult` interface already existed and is unused, probably the discovery work. Mine is named `ProviderAgentImportOutcome` to avoid colliding with it.
