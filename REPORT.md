# Report: v5/fix-uam-control-hidden-tools-secrets

The work branch `v5/fix-uam-control-hidden-tools-secrets` is pushed. Final SHA: `cc4f5b854b0c157757bcd00bd45e7ec6499e35fa`.

Commits on top of `origin/uam/v5.2-sync-main`:

- `ecab05c8`: the implementation.
- `31c05056`: fixes from review round 1.
- `cc4f5b85`: doc nits from round 2.

Every commit ends with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. No PRs, issues or comments were created.

## Review rounds

Two fresh-context `claude-opus-5-5` agents reviewed the branch over 2 iterations: 12 findings suggested, 10 implemented.

### Round 1: full independent review

9 suggested (1 blocker, 1 should-fix, 7 nits), 8 implemented.

- **Blocker (implemented).** A goal reviewer's hidden-tool list was stored unsorted, so the manager's `binary_search` could miss hidden tools. The main fix did not work for reviewers.
  - The list is now sorted in `UnusableTools`, and sorted and deduplicated wherever it is stored.
- **Should-fix (implemented).** Gemini CLI View had been moved to an unverified `${VAR}` expansion route, which could leave every UAM tool call unsigned. A live check was not possible here.
  - Gemini CLI View now gets the literal key in its own settings file again, recorded as a remaining file-based route.
  - Claude Code keeps the reference route. Its executable (2.1.330, in this container) shows both expansion and environment pass-through.
- **Nits implemented:**
  - The reviewer-ceiling test comment.
  - The `uam-tools.md` wording about the structured setup route.
  - The `uam-tools.md` per-provider detail, moved into an implementation note.
  - The Claude Code row claim, now "observed in executable".
  - The secret card notice, which now names the agent.
  - The logging wording, removed from the docs.
- **Declined (1).** The index format does not survive a downgrade.
  - Reason: downgrading is not a supported path. The Keychain items survive. At worst an older build stops showing "saved" until the user saves again.

### Round 2: fresh-context re-check of the fixes

3 suggested (3 nits), 2 implemented. All 8 round-1 items were confirmed resolved.

- **Implemented (2):** the Copilot SDK setup route is now named in `uam-tools.md`, and the key-delivery sentence was reworded.
- **Declined (1):** the duplicate sort in `UnusableTools`.
  - Reason: it keeps that function's returned list ordered for `HideTools`, and the sort where the list is stored keeps the invariant in one place. Harmless.

## Verification actually run

- `npm --prefix UI-V2 run test` passed: 4183 unit and 33 performance tests. `npm --prefix UI-V2 run build` passed. After round 1, the focused `SecretRequestCard` tests passed again (15/15).
- `python3 scripts/check_docs.py` passed.
- The `uam_runner` build (Linux, Release) passed after each round. It compiles `provider_setup.h`.
  - sqlite.org is blocked by the proxy, so it used the sqlite 3.54.1 amalgamation from the `better-sqlite3` npm package instead of the pinned 3.54.0.
- g++ 13 `-std=c++20 -fsyntax-only` checks:
  - `secret_request_service.cpp` compiled clean.
  - `uam_control_service.cpp` compiled clean against stand-in CEF headers (CEF downloads are blocked).
  - The changed test files have no errors in their own code; the remaining errors come only from the stand-in CEF headers.
- Not run:
  - Native tests (the test build refuses Linux).
  - macOS or Windows builds.
  - Keychain.
  - Live providers.
  - A visual check of the card.
  - `src/cef/secret_handlers.cpp`, a small change, was not compile-checked.

## Screens needing Before/After capture

- **Secret request card, saved state.** New line: "Saved for <provider> (<agent> agent) on <date>. Other agents are not offered it." under Use saved secret / Forget saved.
- **Secret request card, older-value state.** New info Notice: "An older saved value isn't offered." with a **Forget it** action.
- Capture both in light and dark themes, on macOS (saving is macOS-only).

## Issue preview 1

`bug(uam-control): hidden-tool restrictions are enforced only by the session's tool server`

> Each UAM tool session gets a fixed list of tools it is not offered. Only that session's own tool server refused them; the manager did not re-check the list. A request that did not come through that server could reach tools the session was never offered. For example, a goal reviewer's session is offered no tools, but the manager would still serve `agent_delegate`, `user_question` and `html_view_*` for it. Separately, in CLI View the tool server's signing key was written into configuration files under UAM's private folder (Claude Code's `--mcp-config` file, Antigravity's `mcp_config.json` and others). The docs said the server refuses tools a session does not list.
>
> **Desired outcome:** the manager enforces each session's hidden-tool list itself. The signing key is passed through the environment where a provider supports it, and the docs describe any remaining exposure.
>
> **Evidence:** found by reading the source: `uam_control_service.cpp` (the tool server's check, and `HandleRequest` without one), `provider_setup.h` (config file writers) and `docs/providers/uam-tools.md`. **Reproduction status:** not reproduced at runtime.
>
> **Acceptance criteria:**
> - [ ] The manager refuses hidden tools for goal reviewer, read-only and CLI View sessions.
> - [ ] Generated config files carry no signing key where the provider can take it from its environment.
> - [ ] The residual exposure for the remaining providers is documented.
>
> **Review:** self-triage only.
>
> **Prepared by:** Claude Opus 5.5 (`claude-opus-5-5`) in a Claude Code cloud session (Claude Code 2.1.330; host app not recorded), on behalf of David Taylor.

## Issue preview 2

`bug(secrets): a saved Keychain secret is offered to any agent in the same workspace`

> A secret saved with **Save in Keychain** is named by workspace and label only. A later `secret_request` with the same label from any provider or agent in that workspace is offered **Use saved secret**, and the card does not say who saved the value.
>
> **Desired outcome:** a saved secret is offered only to the provider and agent that saved it, and the card shows who it is kept for and when. Entries saved under the old name are handled safely.
>
> **Evidence:** `secret_request_service.cpp` (`Target`) and `secret_handlers.cpp` (`use_saved`). **Reproduction status:** not reproduced at runtime.
>
> **Acceptance criteria:**
> - [ ] Saved secrets are named by workspace, provider, agent and label.
> - [ ] Older unscoped entries are never offered for use.
> - [ ] The card shows who the value is saved for and the save date.
>
> **Review:** self-triage only.
>
> **Prepared by:** Claude Opus 5.5 (`claude-opus-5-5`) in a Claude Code cloud session (Claude Code 2.1.330; host app not recorded), on behalf of David Taylor.

## PR preview

`fix(uam-control): enforce hidden tools in the manager and scope saved secrets`

> The manager now refuses UAM tools a session was never offered, using the hidden-tool list stored (sorted) on its capability at launch, including the CLI View additions. Before, only the session's own tool server enforced that list, so a goal reviewer could still reach `agent_delegate`, `user_question` and the HTML view tools.
>
> Generated config files no longer carry the tool server's signing key where the provider can take it from its environment. Claude Code's `--mcp-config` file holds a `${UAM_CONTROL_LAUNCH_KEY}` reference, and the tool server falls back to that variable if the reference arrives unexpanded. The shared `mcp.json` copy is stripped for every provider that does not read it. Codex and OpenCode were already environment-only. Gemini CLI View, Copilot CLI View and Antigravity still receive the key in a private file, documented as a known gap.
>
> Saved Keychain secrets are now scoped to workspace, provider, agent and label. The card shows who a value is saved for and when, and older unscoped entries are offered only for forgetting.
>
> Docs updated: `uam-tools.md` (agreement and implementation note), `secrets.md`, all six `mcp-connections` rows and the known-gaps table.
>
> Related to #<hidden-tools issue>, #<secrets issue>.
>
> **Visual evidence:** not captured. The saved-for line and older-value notice on the secret card still need Before/After screenshots, light and dark, on macOS.
>
> **Proof:**
> - Frontend: `npm run test` passed (4183 unit, 33 performance), including new card and sanitizer cases. `npm run build` passed.
> - Docs: `python3 scripts/check_docs.py` passed.
> - Native, compile only: on Linux, the `uam_runner` build passed with a substitute sqlite 3.54.1. Syntax-only g++ checks: `secret_request_service.cpp` clean, `uam_control_service.cpp` clean against stand-in CEF headers, no errors in the changed test files' own code.
> - New native cases (not run: the test build refuses Linux):
>   - `UamControlManagerRefusesToolsTheSessionWasNotOffered`
>   - `UamControlToolServerSignsWithTheProviderEnvironmentKeyWhenTheReferenceIsNotExpanded`
>   - `UamControlSigningKeyStaysOutOfConfigurationFilesWhereTheProviderCanTakeItFromItsEnvironment`
>   - `SecretRequestSavedSecretIsOfferedOnlyToTheSavingProviderAndAgent`
> - Three existing assertions now expect the hidden-tool refusal.
> - Not run: native test inventory, macOS/Windows, Keychain, live provider config expansion.
>
> **Review:** Reviewed by 2 fresh-context agents over 2 iterations.
>
> - Agent 1: claude-opus-5-5 (full independent review). Suggested 9; implemented 8. The downgrade-compatible index format was declined, because downgrading is not a supported path and Keychain items survive.
> - Agent 2: claude-opus-5-5 (re-check of the fixes). Suggested 3; implemented 2. The duplicate sort was kept as harmless.
>
> Required Claude review of the React change and David's approval are still pending.
>
> **Created with:** Claude Opus 5.5 (`claude-opus-5-5`) in a Claude Code cloud session (Claude Code 2.1.330; host app not recorded), on behalf of David Taylor.
