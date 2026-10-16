# Documentation review and decisions

This page records the outcome of the isolated documentation trial based on `3a7c8477` and updated through `uam/v5-improvements` at `e28ba717`. Expected behaviour comes from David’s instructions in this conversation and recorded repository decisions. Source inspection identifies implementation routes; no application was built, installed or exercised, and no provider was prompted. The [V5 log](<../V5 Improvement Change log.md>) remains the acceptance and ownership register.

## Resolved documentation conflicts

| Previous conflict | Documentation disposition |
|---|---|
| Missing provider files could mean unsupported or forgotten | Every provider has an explicit row for every required capability; omission is a documentation defect. |
| “Supported” implied full delivery despite qualified gaps | Separate delivery route, implementation status and acceptance; preserve concrete limits. |
| Parity/security prose said all structured requests always passed through UAM | Document native Claude auto and confirmed Codex auto-review alongside UAM review and residual user prompts. |
| Computer-use checklist asked for approval on every action, despite a target grant covering subsequent work | Retain the current target-grant model; review that grant and its revocation rather than claiming a new dialog per action. |
| Security notes said UAM exposed no network service | Optional Phone Access publishes a restricted authenticated browser interface; it is explicitly enabled and build-excludable. |
| Memory documents disagreed between `.codex/memories` and `.UAM` | Current source uses the effective workspace’s `.UAM` directory; update active guidance and mark the old statement obsolete. |
| Configurator documentation omitted the sixth CMake flag | Document all six flags and explicitly exclude Antigravity in the Codex-only CMake example; separately flag that the actual configurator still enumerates only five providers. |
| General UI settings pattern demanded subtitles while the later settings agreement removed them | Settings-specific rules take precedence: page title only, concise section labels and no repeated description. |
| Product decisions pending implementation appeared as unconditional descriptions of current UI | Preserve the notification/undo agreements and identify their incomplete implementation separately. |
| Previous agent guide claimed file-origin loading with disabled Chromium web security | Correct the retained reference to the trusted `uam://app/` scheme and enabled web security. |
| Long architecture pages accumulated debugging receipts | Restore short system overviews and retain useful details as labelled engineering reference. |

## Antigravity scope

**Resolved by David, 2026-10-16.** Antigravity is fully included in structured Chat and native CLI View, with the same shared provider requirements as the other five. Its existing structured adapter uses native stream JSON, not ACP on the wire. Remove the previous terminal-only exception from repository instructions, provider agreements and diagrams; keep actual capability gaps and unverified platform combinations explicit. This scope decision is not acceptance of every implementation. [Antigravity checklist](providers/antigravity/antigravity.md).

## Data root

**Resolved by David, 2026-10-16.** Keep existing data-storage behaviour. Startup uses an explicit `UAM_DATA_DIR` when set; otherwise `AppPaths::DefaultDataRootPath` selects OS app-data, then temporary storage, then relative `data` only if no temporary directory is available. Correct the old working-directory-first instructions rather than changing application routing or moving user data. [Persistence agreement](architecture/app-state-persistence.md).

## Environment scope

**Partial implementation.** The desired agreement includes every settings page whose values can differ by provider/environment, including chat approval defaults and computer-use policy where applicable; local OS permission status is a different scope. The inspected `SettingsModal.tsx` shows the scope bar on provider pages, Chat Defaults and MCP Servers. Computer use is local-only. [Environment scope](settings/environment-scope.md) records the target and current boundary; no backend or settings UI change is made here.

## Build exclusion

**Partial implementation; artifact verification needed.** Provider, phone, SSH and computer-use flags exist, but the stricter requirement is zero mention of excluded capabilities in the resulting application and shipped content. Source gating and hidden controls do not prove that. The browser configurator’s `PROVIDERS` list contains only five names and does not emit the Antigravity flag, leaving its CMake ON default intact for a supposedly restricted selection; this confirmed gap is recorded as Configurator Provider Exclusion in the V5 log without changing product source. Compatibility strings, generated resources and packaged artifacts require a restricted-build audit before making a compliance claim. [Build flags](build-flags.md) owns the agreement; future implementation must retain existing user data compatibility without resurfacing the excluded capability.

## Known gaps retained

| Area | Current limit | Record to consult |
|---|---|---|
| Terminal continuity | Codex initial native-session ownership can be ambiguous with concurrent unbound terminals | [Session continuity](providers/terminal-view/session-continuity.md) |
| Compaction | Manual routes vary by provider/host; OpenCode live and historical behaviour retains unresolved acceptance | [Provider reference](reference/provider-runtime-parity.md), V5 OpenCode Compaction State Sync |
| Child agents | OpenCode task launch restriction and partial addressable-child history prevent full parity | [Subagent history](providers/agent-history.md), [OpenCode checklist](providers/opencode/opencode.md) |
| Sending | Direct input/queue separation and reported Unsent flash retain outstanding acceptance | [Sending](chat/sending.md), V5 Send Flashes Unsent |
| Sidebar | Active Chats restoration was user-confirmed; other sidebar controls and wake-time overlap remain separately pending | [Active Chats](sidebar/active/active.md), V5 Restore Active Chats Section and Snoozed Wake Time Overlap |
| Notifications and undo | Agreed shared notification system and organization-undo removal remain pending in the base decision record | [Recorded decisions](architecture/decisions.md) |
| Account usage | Independent local reads exist for four providers; Gemini eligibility, OpenCode acquisition, custom commands, remote feeds and platform acceptance remain limited | [Allowances](usage/allowances.md), V5 Independent Account Allowance Snapshots |
| Cross-platform/SSH | Source routes and fixture receipts do not prove every real CLI, OS or SSH combination | [Provider checklists](providers/providers.md#provider-checklists) |

Do not reassign the owners of these existing V5 tasks or mark them fixed from this documentation trial. Statuses are conservative source-level assessments, not a replacement for running a provider implementation audit.

## Documentation validation

The local checker validates navigation, heading targets, reachability and one explicit row for each required capability in every provider checklist. The trial passes six negative scenarios: omitted capability, acceptance used as implementation status, duplicate capability, missing local target, missing heading and unreachable feature page. A manual reading check traced provider completeness, memory Off semantics, editor actions, waking chats and build exclusion to their owning agreements. This is documentation validation, not an independent user study or application acceptance.

## Decision status

Both questions raised by the trial are resolved above. David approved retaining the guide and requested its merge into `uam/v5-improvements` on 2026-10-17; see the [adoption record](<../V5 Improvement Change log.md#documentation-adoption-2026-10-17>). Independent documentation review remains pending; implementation gaps and runtime acceptance remain tracked separately.

## V5 synchronization

The trial merges `uam/v5-improvements` at `e28ba717`, including empty native-chat rejection and independent account allowances. Incoming application sources are retained without merge edits. New import, allowance, cost and accessibility rules are reflected in the short agreements and provider rows; detailed acquisition, cache and isolated-test-bundle guidance is retained in engineering reference. Upstream review receipts and unresolved native failures remain in the V5 log; this synchronization does not establish new runtime or user acceptance.

## Remaining scope and source mismatches

- [Existing VCS panel](architecture/decisions.md#existing-vcs-panel): older release exclusions conflict with the accessible Git/SVN commit panel. Its documentation preserves the existing surface without authorizing removal or expansion; David's scope disposition is pending.
- Antigravity availability copy in `SettingsModal.tsx` still says “Terminal sessions only.” The agreed scope is structured Chat and CLI View; this is a current implementation-copy mismatch, not a reason to change the agreement. Its [checklist](providers/antigravity/antigravity.md) and V5 log retain the follow-up.
