# #523 first slice: Custom ACP agent — report

**Branch:** `v5/feat-523-custom-acp-agent` (based on `origin/uam/v5.2-sync-main` at `c252f9a9`)
**Final SHA:** `d5da6b68896f8d067126275655f57c22b83a4cd9`. There are 3 commits: `77eb79eb` adds the feature, then `284b1de0` and `d5da6b68` fix the review findings. Nothing was merged, and no PR, issue or comment was created.

## What was built

The first slice is exactly what David decided on 2026-11-23. The decision is recorded in `docs/documentation-review.md#custom-acp-agent`.

- **Build flag.** One opt-in, best-effort Custom ACP agent sits behind `UAM_ENABLE_RUNTIME_CUSTOM_ACP`, which is **off by default**.
  - Leaving it off doesn't make a build "restricted". It never counts as a required provider and is never the default.
  - In the frontend, the flag is `UAM_FEATURE_CUSTOM_ACP`, also off when unset. The default bundle contains none of its code; I checked the built `dist`.
- **Configuration.** It comes only from a Settings form (name, command, arguments one per line, `NAME=value` environment lines).
  - The form is stored in `<data-root>/custom-acp-agent.json`. Its only writers are three desktop bridge actions: `getCustomAcpAgent`, `setCustomAcpAgent` and `allowCustomAcpAgent`. The phone companion cannot reach them, and nothing in a repo or chat can.
- **Launch.**
  - **No shell.** The agent starts from an argument array and never through a shell. On Windows, `.cmd`/`.bat` and other non-`.exe` targets are refused, and a full path must name the `.exe`, so the platform's `cmd.exe` wrapper can never run.
  - **Exact command shown.** "Command to run" shows the exact command, the program file it resolves to and the names of the environment variables it sets.
  - **Allow step.** The agent can't launch until the user presses **Allow this command**. Any change to the command, arguments or environment, or a different program now being found on PATH, needs a new approval.
  - **Bad command.** It produces one plain line in the chat, e.g. "Custom ACP agent command not found: X. Use the program's full path in Settings."
  - **Settings errors.** Save, allow and load errors go through the notification system, as one line each.
- **Runtime.** It uses only the shared ACP defaults, so features follow what `initialize` and `session/new` report.
  - CLI View, text workers, native import, usage, compaction, central setup, the UAM tool connection (uam-control) and SSH are unavailable. SSH is refused with one line.
  - Diagnostics record the program and the number of arguments, never their values.
  - New runtime capabilities replace provider-name checks: `UsesCentralLaunchConfiguration`, `LaunchArgumentsArePrivate` and `SupportsUamControl`. The platform process service gains `ExecutableSearchDirectories`.
- **UI.** A "Best effort" label appears in Settings and in Chat's model picker.
  - The Terminal switch is disabled for providers with no CLI.
  - The provider overview shows UAM MCP and Shared setup as "not applicable" for this provider.
  - The page warns about unsaved changes before you leave it.
- **Tests.**
  - `tests/fixtures/custom_acp_agent.py` is a scripted ACP agent run as a real stdio process.
  - `tests/custom_acp_agent_tests.cpp` covers initialize, `session/new` with models and modes, a streamed prompt, permission allow and reject, cancel, exact delivery of arguments and environment (including shell metacharacters), early exit, validation, approval and resolved-program checks, and Windows path rules.
  - It also builds on Linux with `UAM_CUSTOM_ACP_TESTS_ONLY=ON`, and a new `custom-acp` CI job runs it.
- **Docs.**
  - New checklist `docs/providers/custom-acp/custom-acp.md`, with all 32 rows filled honestly: mostly Needs verification / Not implemented, nothing Implemented.
  - Updated `providers.md`, `provider-boundaries.md`, `settings/providers.md`, `build-flags.md`, `building.md`, `testing.md`, the source map, the README and the decision record.
  - `check_docs.py` now checks 7 checklists.

**Known limits (documented):**
- The integration test drives the fixture with its own small stdio client, not UAM's in-app ACP runtime.
- The stdio case runs on macOS and Linux only.
- The desktop app with the flag on has not been built or launched (the native desktop build refuses Linux).
- In full builds without the flag, the agent's saved chats are not yet hidden.
- The leak gate does not cover the new flag.

## Review rounds

- **Round 1** (fresh-context claude-opus-5-5): **17 suggested, 14 implemented.** It found 3 blockers (UAM Control offered to the agent; a Windows extensionless path reaching `cmd.exe`; a test path that isn't absolute on Windows), 7 should-fix and 7 nits. Declined:
  - **#10** Flag-on side effects in the usage and account-allowance loops. The feature is opt-in only, the usage effect is documented, and the allowance worker sits idle. Verifying it needs a flag-on macOS build.
  - **#11** A test for the one default-build behaviour change (`IsRetainedExcludedProviderId("custom-acp")`). No CEF-free target builds the full set without the flag; the behaviour is documented.
  - **#17** UAM's `PAGER`, `GIT_PAGER` and `GIT_CONFIG_*` overriding the user's values. This is intentional for every structured process; documented in round 2.
- **Round 2** (one fresh-context claude-opus-5-5 re-check, because the fixes were material): **9 suggested, 9 implemented.** It found 0 blockers, 2 should-fix (native MCP status still reported UAM MCP; platform-internal headers included outside the platform layer) and 7 nits. Two were only partly addressed:
  - **#4** The character check was extended and the shown command is direction-isolated, rather than checking full Unicode categories.
  - **#8** The Windows path rules now run on every platform. The Allow handler and the platform search still have no native test.
- **Total: 26 suggested, 23 implemented.**

## New issue previews (not filed)

**1. `feature(providers): run the Custom ACP agent through UAM's ACP runtime in a native test`**
> The #523 first slice tests the scripted ACP agent with a standalone stdio client. UAM's own route (`StartAcpProcessForChat`, `CustomAcpProviderRuntime::PrepareStructuredLaunch`, permission cards, the generic start-failure message and the Settings Allow handler) is only syntax-checked on Linux.
> **Desired outcome:** a flag-on core test target on macOS (and Windows where the stdio case can run) that drives a chat against `tests/fixtures/custom_acp_agent.py` through the real runtime.
> **Acceptance criteria:**
> - [ ] initialize, a new session, a streamed prompt, a permission card round trip and cancel pass through `AcpSessionRuntime`.
> - [ ] A bad command and an unapproved launch each give their one-line chat error.
> - [ ] A flag-on app build on macOS and on Windows.
> **Review:** self-triaged from the #523 review rounds. **Prepared by:** claude-opus-5-5 in a Claude Code remote session (Claude Agent SDK), on behalf of David Taylor.

**2. `feature(build): bring the Custom ACP flag under build-exclusion rules`**
> With `UAM_ENABLE_RUNTIME_CUSTOM_ACP` off, a full build keeps the agent's saved settings inert. It still shows its saved chats, and the leak gate has no CUSTOM_ACP entry. With the flag on, usage caches from a build without it are not reused, and an idle allowance worker starts for the agent.
> **Acceptance criteria:**
> - [ ] Full builds without the flag hide the agent's chats, as for excluded providers.
> - [ ] `check_build_flag_leaks.py` and the configurator know the flag.
> - [ ] The usage and allowance loops skip providers that have no usage or allowance route.
> **Review:** self-triaged. **Prepared by:** claude-opus-5-5 in a Claude Code remote session (Claude Agent SDK), on behalf of David Taylor.

## PR preview (not opened)

**Title:** `feat(providers): add an opt-in best-effort Custom ACP agent`

UAM can now drive one user-configured local agent that speaks ACP over stdio, as a best-effort provider. It sits behind `UAM_ENABLE_RUNTIME_CUSTOM_ACP`, which is off by default; this is the first slice of #523.
- **Configuration.** The agent's command, arguments, environment and name come only from a Settings form.
- **Launch.** UAM starts it from an argument array with no shell. Windows batch scripts and extensionless paths are refused.
- **Approval.** Settings shows the exact command and the program it resolves to, and a new or changed launch must be allowed there first.
- **Capabilities.** Features follow what the agent reports. CLI View, text workers, import, usage, compaction, central setup, UAM tools and SSH are unavailable and shown as such.
- **Labels and checklist.** It is labelled "Best effort" in Settings and in the model picker, and has its own seventh checklist page.
- **Default builds.** Without the flag, the only change is that this agent's saved settings stay inert in full builds.

Related to #523; SSH, several agents, an icon and a real third-party agent remain.

<Visual evidence needed: see the screen list below. Every screen needs a flag-on build.>

**Proof:**
- **Focused native test** (`uam_custom_acp_agent_tests`, isolated `UAM_CUSTOM_ACP_TESTS_ONLY` build on Linux): passes.
  - It starts the scripted fixture as a real stdio process. Deliberately broken fixture copies (a wrong permission reply; a cancel answered as `end_turn`) made it fail.
- **Remote runner** (`uam_runner`, Linux): builds with the flag off and with it on.
- **Native app code:** syntax-checked only, with `g++ -fsyntax-only` against stub CEF headers, with the flag on and off.
  - Covered: the custom ACP sources, registry, profile, ACP launch and lifecycle, attachments, uam_control_service and the bridge handler.
  - Not covered: `application.cpp`, `uam_query_handler.cpp`, and the macOS/Windows platform files, which were reviewed by reading only.
  - Not built or run: the desktop app, `uam_core_tests` and the Windows/macOS stdio case.
- **UI-V2:**
  - `npx vitest run`: 241 files, 4224 tests, all pass.
  - `npx tsc --noEmit -p .`: clean.
  - `npm run build`: passes. The default bundle contains no custom ACP code.
  - A build with `UAM_FEATURE_CUSTOM_ACP=1` also passes, and the provider-metadata, usage and featureDefines suites pass with the flag on.
- **Docs:** `python3 scripts/check_docs.py` passes (204 pages, 7 providers × 32 capabilities).
- No live provider or token tests.

**Review:** 1 independent reviewer, 2 iterations. Independent claude-opus-5-5: 26 suggested, 23 implemented. Declined:
- Flag-on usage and allowance loops: documented, and they need a flag-on macOS build to verify.
- A test for the default-build retained-id change: no CEF-free full-provider target.
- UAM's pager and Git variables taking precedence: intentional, and now documented.

**Created with:** claude-opus-5-5 in a Claude Code remote session (Claude Agent SDK), on behalf of David Taylor.

Required before merge: Claude review of the React changes, and David's specific approval (AGENTS.md).

## Screens that need Before/After capture

All of these need an app built with `-DUAM_ENABLE_RUNTIME_CUSTOM_ACP=ON`.

1. **Settings → Providers → Custom ACP agent page** (new, so After only). Capture three states:
   - empty;
   - filled but not allowed, with "Allow this command";
   - allowed.
2. **"Command to run" section** after a changed command, showing "Save your changes to review the new command", and after a refused Allow, showing the new "Runs …" program.
3. **Settings → Providers overview table:** the Custom ACP row, with the UAM MCP and Shared setup cells shown as not applicable.
4. **Chat model picker:** the "Custom ACP" rail button tooltip "(best effort)", and the group header "· Best effort".
5. **Chat header view switch:** Terminal disabled, with the tooltip "This provider has no CLI View", for a Custom ACP chat. For comparison, Before is a normal provider chat.
6. **New Chat dialog** with the Custom ACP agent selected. There is no "does not support terminal chat" message; Before showed it.
7. **Chat error banner** for a bad command ("Custom ACP agent command not found: …") and for an unapproved launch.
8. **Unsaved-changes prompt** when leaving the Custom ACP page with edits.
