# Report: v5/fix-custom-agent-cli-view-and-skills

Work branch `v5/fix-custom-agent-cli-view-and-skills` is at **`718b9faac7ef347409ec36e124e778b6fdb0e046`** (3 commits on `uam/v5.2-sync-main`: `6b4b4157`, `d3656ee1`, `718b9faa`). No PR, issue or comment was created.

## Review rounds

Both reviewers were fresh-context Claude Opus 5.5 subagents, given the issue, the diff against `origin/uam/v5.2-sync-main`, AGENTS.md, CLAUDE.md, the owning feature docs and `docs/engineering/testing.md`, and asked to hunt compile errors.

**Round 1: full review of the branch.** 11 suggested (0 blocker, 7 should-fix, 4 nits), 11 implemented, in `d3656ee1`:
- Codex: only UAM's own marked `developer_instructions` value is folded, so a user's TOML or raw value no longer fails the launch.
- New options go before a positional `--`.
- Central Setup gets the captured agent instructions.
- Antigravity got an `AGENTS.md` + `--add-dir` route instead of a refusal.
- The Codex size limit follows the executing host.
- The UI-thread agent check is no longer repeated on every poll.
- New async CLI View tests.
- Nits: a late Codex guard removed, error wording, a Claude docs caveat, and the synchronous wrapper's limit documented.

**Round 2: re-check of the round-1 fixes.** 9 suggested (1 blocker, 2 should-fix, 2 nits, 1 suspicion, 3 partly resolved round-1 items; one partial is the same issue as a new should-fix). 8 implemented (one as a docs correction only), 1 declined, in `718b9faa`:
- **Blocker:** a remote Antigravity custom agent failed silently, because the SSH helper does not admit Antigravity context. It is now refused with one line.
- `AGENTS.md` is now listed as an owned context file, so it is cleaned up.
- The Codex limit takes the controller's command line into account (Windows controller with a remote host) and measures the full argument.
- Apply refuses when a user's `developer_instructions` would replace the agent's.
- One shared insert helper.
- Apply's own options also go before `--`.
- **Docs only:** the "once per launch attempt" wording was corrected. The check still repeats while a CLI version check is pending; that is a rare, short window, so I did not add caching for it.
- **Declined:** the suspicion that Antigravity may get two `--add-dir` options. This can't be verified without the CLI, and the capability row already says "needs verification".

## Issue preview

**Title:** `bug(agents): custom agent instructions and skill limits are not applied in CLI View and native skills`

Two custom-agent settings are saved but not applied (reported by David Taylor):
1. CLI View starts the provider without the selected custom agent's instructions unless Central Setup is on.
2. An agent's skill allowlist only filters UAM's own skill tools. Provider-native skills stay available, including Claude's skill tool and OpenCode's `skill` tool through `"*": ask`.

**Expected:** The chosen agent follows the request through startup (`docs/providers/agent-selection.md`). The allowlist limits native skills where the provider supports it, and is documented as UAM-only elsewhere.
**Actual:** `BuildProviderInteractiveArgv` only forces Plan, and the terminal path never calls `PrepareRuntimeAdapter`. The skill checks live only in `uam_control_service.cpp` `SkillList`/`SkillRead`.

**Environment:** `uam/v5.2-sync-main` at `6f235eb9`, all six providers.
**Evidence:** Source inspection only.
**Reproduction status:** Not reproduced against live CLIs.

**Acceptance criteria:**
- [ ] CLI View carries a custom agent's instructions for each provider, or refuses with one line
- [ ] Native skill rules for OpenCode and Claude; UAM-only boundary documented for the rest
- [ ] Fixture tests for staged instructions per provider and for the OpenCode skill rules

**Review:** Self-triage only.
**Prepared by:** Claude Opus 5.5 in a Claude Code cloud session (Claude Code 2.1.330), on behalf of David Taylor.

## PR preview

**Title:** `fix(agents): carry custom agent roles into CLI View and limit native skills`

Without Central Setup, CLI View started a selected custom agent's provider without the agent's instructions. An agent's skill allowlist also only filtered UAM's own skill tools.

CLI View now asks the provider runtime to carry the agent:
- **Claude, Copilot, Gemini, OpenCode:** the text goes ahead of any handoff in the staged handoff files, locally and over SSH.
- **Antigravity:** staged as `AGENTS.md` in a private `--add-dir` directory locally. It is refused over SSH, where the helper does not admit its context.
- **Codex:** passed as a marked `developer_instructions` value. Setup folds it after host or central instructions. CLI View is refused when the agent exceeds the controller's or executing host's argument limit, or when a user's own value would replace it.
- **Central Setup on:** it already installs the captured instructions, so no second copy is staged.
- **Launch options** go before a positional `--` from extra flags.
- **Refusals** are one line and happen before any worktree, baseline or SSH work.

On skills:
- OpenCode's local Chat agent config (and the merged permission for imported agents) denies native skills outside the allowlist; approved skills ask.
- Claude Chat and CLI View remove the `Skill` and `SlashCommand` tools for custom agents.
- Other routes are documented as UAM-only, with a new open provider-parity decision.

Build and Plan are unchanged. **Coordinates with** `v5/refactor-provider-handoff-staging` (not on the remote yet). This PR adds two `IProviderRuntime` methods and only threads values through the existing staging, so it stays separable.

**Proof:** (Linux container; the native build refuses Linux)
- `python3 scripts/check_docs.py` passed: 203 pages, 6 providers × 32 capabilities.
- Syntax-only `g++ -std=c++20` compile of the changed sources passed (10 changed translation units plus `runner_proxy.cpp` and `runner_state.cpp`, which include the changed headers), and of the 4 changed test files, with the pinned nlohmann/json and a minimal CEF stub. This check caught and fixed two test compile errors in round 1.
- These 7 cases passed in a scratch driver linking the real `agent_definition_service.cpp` and `provider_setup.h`:
  - new: `OpenCodeAgentAdapterLimitsNativeSkillsToTheAgentAllowlist`, `OpenCodeNativeSkillRuleNeverWidensAnImportedPolicy`, `CodexSetupFoldsCliViewAgentDeveloperInstructionsInsteadOfReplacingThem`
  - existing: `OpenCodeImportedPolicyAdapterUsesCapturedRulesWithoutBroaderWildcardOverrides`, `ProviderAgentRuntimeAdaptersAreIsolatedExactAndPermissionMediated`, `CodexCentralSetupLeavesWorkspaceInstructionsToNativeDiscovery`, `CentralLaunchContractsCoverAllFiveProvidersAndPreserveOpenCodeConfig`
- **Not run:** `uam_core_tests`/CTest on macOS or Windows. This includes the never-executed `CliViewStagesCustomAgentInstructionsForEachProviderOrRefuses`, `CliViewRemovesClaudeNativeSkillsForCustomAgentsOnly`, `AsyncCliViewCarriesCustomAgentThroughEveryPreparationRoute` and the extended `ImportedOpenCodePolicyReachesActualChildEnvironment`. No live provider CLI or SSH runs; whether Gemini (folder trust) and Antigravity (`--add-dir`) natively load the staged instructions is unverified. No UI files were touched, so the frontend checks weren't run.

**Review:** 2 independent fresh-context reviewers, 2 iterations.
- Independent Claude Opus 5.5 (full review): 11 suggested, 11 implemented.
- Independent Claude Opus 5.5 (re-check of fixes): 9 suggested, 8 implemented (one as a docs correction), 1 declined (repeated `--add-dir` is unverifiable here and already recorded as needing verification).
- Required Claude review of the final candidate and David's approval are still pending.

**Created with:** Claude Opus 5.5 in a Claude Code cloud session (Claude Code 2.1.330), on behalf of David Taylor.

## Screens needing Before/After capture

- CLI View for a Claude chat with a custom agent: role absent before, role applied after.
- CLI View for a Codex chat with a custom agent: no developer instructions before, role applied after.
- CLI View for an Antigravity chat with a custom agent:
  - local: started without the role before, role staged after
  - SSH: launched without the role before, one-line refusal after
