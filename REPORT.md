# Agent import fix — report

**Work branch:** `v5/fix-agent-import-parser`, final SHA `3f5b97eeb194cf9a6981697a7197f0eefe555187`, base `origin/uam/v5.2-sync-main` (`6f235eb9`).

Commits, each ending with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`:

1. `8e36694a` fix(agents): import provider agents with nested rules, discovery and named errors
2. `94b19642` fix(opencode): translate imported agent denies into the V2 permission guard (separate V2 change)
3. `101fb311` fix(agents): read OpenCode rules in order and refuse fields hidden by bare CRs
4. `a78ecea7` fix(opencode): deny renamed V2 actions for wildcard rules that cover them (separate V2 change)
5. `75893c73` fix(agents): merge OpenCode configs strictly and keep read-only limits after import (round-1 fixes)
6. `3f5b97ee` fix(agents): merge same-named OpenCode agent entries and refuse linked prompt paths (round-2 fixes)

## Review rounds

**Before the requested loop:** one self-initiated adversarial subagent pass found 7 issues (3 high/medium widening cases, 4 lower). All were fixed in `101fb311` and `a78ecea7`. This pass is not counted below because it was not given the issue, diff and docs in a fresh context.

**Round 1: independent Opus, fresh context, given the issue, the full diff and the project docs.** 12 suggested (2 blockers, 6 should-fix, 4 nits), 11 implemented in `75893c73`.

- Blockers implemented:
  - Several config files were merged as one ordered list (fail-open).
  - A JSON entry in a project's root config skipped the project's `.opencode` config (fail-open).
- Should-fixes implemented:
  - A read-only limit was lost after import; it is now stored as `maxWorkspaceAccess: read`.
  - Linked or empty global configs refused every OpenCode import.
  - New provider-named strings in shared code would fail the build-flag leak gate.
  - Discovery and preview ran on the CEF UI thread.
  - A `{file:...}` prompt could read any file.
  - Some errors did not name their field.
- Nits implemented:
  - `Node` special members are now defined out of line, for portability.
  - Unknown tool names no longer raise access to write.
  - A stale busy flag ignored a newly picked agent.
  - A shadowed variable was renamed.
- Declined: the misleading OpenCode V2 launch-gate message (`opencode_cli_provider_runtime.cpp` ~846). That gate belongs to `v5/fix-opencode-unverified-v2-launch`.

**Round 2: independent Opus re-check of the round-1 fixes, fresh context.**

- Round-1 verdicts: 10 fixed, 2 partly fixed (#6 UI thread, #7 prompt files), #12 accepted as declined.
- New findings: 6 suggested (0 blockers, 2 should-fix, 4 nits), 5 implemented in `3f5b97ee`:
  - same-named `agent.<id>` entries in other configs, plus `OPENCODE_CONFIG`;
  - `{file:...}` escaping its folder through a linked folder (this completes round-1 #7);
  - functions left unused in OpenCode-off builds;
  - per-file config validation with the file named;
  - docs and a comment overclaiming order-independence.
  - Also closed: a recorded read-only limit is now kept when the stored file fails to parse.
- Declined: moving import itself off the UI thread, which is the remainder of round-1 #6. Its connected handler tests (`tests/chat_icon_handler_tests.cpp`) assert synchronously, and changing that needs CEF task pumping that cannot be built or run on Linux. It is recorded as a known gap in `docs/documentation-review.md`.

**Totals:** 1 independent reviewer, 2 iterations; 18 suggested, 16 implemented, 2 declined.

## What changed for users

- **Parsing:** import reads a closed YAML frontmatter subset (nested maps, lists, quoted and block text) and OpenCode JSON(C) `agent`/`agents` entries.
  - Anchors, aliases, tags, merge keys and bare carriage returns are refused.
  - Every refusal names its field.
- **OpenCode rules:**
  - Read in OpenCode's last-match order, together with every applicable global, custom and project config, including same-named agent entries.
  - Denies are kept; `allow` and enabled tools become `ask`, so UAM still asks.
  - A source that cannot edit imports only read-only, and keeps that limit through later edits.
  - Rules are tied to OpenCode only when they hold a deny that read-only access cannot express.
- **Claude Code, Gemini CLI, Copilot CLI:**
  - Tool lists, disallowed tools and `plan` permission modes set the read-only limit.
  - Finer tool detail, turn limits and MCP servers need acknowledgment.
  - Hooks, worktree isolation and remote Gemini agents are refused.
- **Descriptions:** folded to one line; up to 1,024 bytes are kept, and anything longer is shortened with a preview note.
- **Discovery:** "Find agents" lists each provider's global and workspace agent folders and OpenCode JSON(C) entries; a typed path still works. Codex CLI and Antigravity have no native agent files, and their capability rows say so.
- **OpenCode 2.x:**
  - Imported deny rules are translated into the V2 bridge's permission check: native session rules plus the process-local hook, with `bash` also as `shell`, `task` as `subagent`, edits as `edit`.
  - Local V2 launch of custom agents is still refused by its existing check, which belongs to the coordinating branch.
  - Preview refuses agents with deny rules beyond read-only, with a one-line message, while the checked local OpenCode is 2.x.
- **Behaviour changes for David to confirm:**
  - The old passing shape `edit: deny, bash: ask` is no longer tied to OpenCode, because read-only access expresses it.
  - Turn and time limits and per-tool lists now need acknowledgment instead of blocking the import.
  - A multi-line plain description whose next line looks like a key is refused, by name.
  - The agent editor disables "Read and write" for agents imported from read-only sources.

## Issue preview

**Title:** `bug(agents): provider agent import refuses most real agent files`

Importing custom agents refuses most real agent files. On a Mac, 5 of 7 OpenCode agents failed, and Claude Code, Gemini CLI and Copilot CLI agents fail on `tools` and on long or multi-line descriptions. Discovery doesn't exist, so the user has to type one file path.

**Reproduction:** Settings → Agents → Import from a provider → OpenCode → preview an agent with a `tools:` map, a permission map that denies every tool, `edit: allow` with a 330-byte description, or `permission: {edit: allow, bash: ask}` plus `tools`.

**Expected:** the agent imports with its restrictions kept and never widened, and any refusal names the field.

**Actual:** "does not support indented frontmatter outside its flat OpenCode permission mapping", or a generic blocked-fields error. Agents in `opencode.json` and in native agent folders are never offered.

**Environment:** UAM `uam/v5.2-sync-main` (V5.2 merged with 5.1.0); macOS; OpenCode, Claude Code, Gemini CLI and Copilot CLI agent files.

**Evidence:**
- Product-owner report: 5 of 7 OpenCode agents failed on a real Mac.
- Code inspection of `agent_definition_service.cpp`: a flat `permission` parser, `tools` on the blocked list, and a 240-byte description limit.
- Code inspection of `opencode_cli_provider_runtime.cpp` (~844): the V2 refusal at first send.

**Reproduction status:** the Mac failures are user-reported. The agent shapes were reproduced as synthetic fixtures, not on the user's files.

**Acceptance criteria:**
- [ ] The reported OpenCode shapes and common Claude, Gemini and Copilot agents import, and no import is looser than its source.
- [ ] Every refusal names its field.
- [ ] Native agent folders and OpenCode JSON(C) entries appear in a pick-list.
- [ ] Imported OpenCode deny rules either reach V2 or are refused at preview.
- [ ] Live import on macOS and Windows.

**Review:** not started.

**Prepared by:** Claude Opus 5.5 (Claude Code, cloud session) for David Taylor.

## PR preview

**Title:** `fix(agents): import provider agents with nested rules, discovery and named errors`

Related to #<issue>.

Provider agent import now reads a closed YAML frontmatter subset and OpenCode JSON(C) entries, and lists native agents from each provider's folders as a pick-list.

- **Restrictions:** resolved in OpenCode's last-match order, including every applicable global, custom and project config and same-named agent entries.
- **Never looser than the source:**
  - denies are kept and `allow` is still asked;
  - read-only sources stay read-only, through later edits too;
  - every refusal names its field;
  - provider names in shared native strings exist only in builds that include that provider.
- **Separate OpenCode V2 commits:** imported deny rules translate into the V2 bridge's permission check.
- **Still refused on 2.x:** local V2 launch of custom agents is still refused by its existing check (owned by `v5/fix-opencode-unverified-v2-launch`), so preview refuses deny-bound imports while local OpenCode is 2.x.

**Visual evidence:** needs Before/After capture on macOS:
- Settings → Agents → "Import from a provider" before and after "Find agents" with results, including the skipped-files warning;
- a preview showing the "How this agent maps" notes;
- the import workspace-access menu with "Read and write" disabled for a read-only source;
- a refusal that names its field;
- Settings → Central Setup agent editor with "Read and write" disabled for an agent imported read-only.

**Proof:**
- **Native app build:** the macOS/Windows build and `uam_core_tests` were not run, because the build refuses Linux.
- **Linux scratch harness:**
  - It linked the real `provider_agent_import.cpp` and `agent_definition_service.cpp` with a stub path service.
  - It ran all of `tests/provider_agent_import_tests.cpp`, plus the changed cases extracted from `tests/agent_target_metadata_tests.cpp` and `tests/core_agent_control_tests.cpp`.
  - Result: 31/31 pass under ASan/UBSan.
- **Compile checks:**
  - Both import sources compile under g++ with `-Wall -Wextra -Wshadow`, with all providers on and with each of OpenCode, Claude Code, Gemini CLI and Copilot CLI off, with no warnings attributable to the change.
  - clang++ with libc++ syntax checks are clean.
  - With OpenCode off, the object files gain no OpenCode-named strings compared with the base.
  - `chat_config_handlers.cpp`, `uam_query_handler.*` and `opencode_cli_provider_runtime.cpp` include CEF headers, so they were reviewed by hand and not compiled.
- **OpenCode V2:**
  - The `tests/opencode_v2` fixture project (V2 session and bridge transport) passes 33/33, up from a baseline of 32.
  - `tests/opencode_v2/permission_plugin.test.mjs` passes 22 boundaries.
- **UI-V2:**
  - `npm ci` and `npx tsc --noEmit -p .` are clean.
  - `npx vitest run` passes 239 files and 4,219 tests, and `npm run build` succeeds, after the last UI change (round 1).
  - A new regression test was confirmed to fail without its fix.
- **Docs:** `python3 scripts/check_docs.py` passes.

**Review:** 1 independent reviewer, 2 iterations. Independent claude-opus-5-5: 18 suggested, 16 implemented.
- Round 1: 12 suggested, 11 implemented. Declined: the OpenCode V2 launch-gate message, owned by `v5/fix-opencode-unverified-v2-launch`.
- Round 2: 6 suggested, 5 implemented. Declined: moving import off the UI thread, because its connected handler tests assert synchronously; recorded as a known gap.
- A self-initiated adversarial subagent pass before the loop found 7 issues, all fixed.
- Required Claude review of the React change and David's approval: pending.

**Created with:** Claude Opus 5.5 (Claude Code, cloud session).
