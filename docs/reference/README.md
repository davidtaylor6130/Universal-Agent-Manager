# Engineering reference

These pages preserve useful source-level details from the trial base, outside the short feature agreements. They can include historical verification and old implementation states. The [feature guide](../README.md) and [documented corrections](../documentation-review.md) take precedence; these references are not proof of current full support or user acceptance. Follow source and tests when changing implementation rather than copying historical receipts into feature paragraphs.

| Reference | Purpose |
|---|---|
| [Previous agent code map](agent-code-map.md) | Retained bridge/actions and source detail, not current instructions |
| [Source map](source-map.md) | Entry points for checking implementation routes |
| [Queue setting admission audit](nongeneric-queue-setting-admission.md) | Per-adapter mode, model and reasoning routes with isolated corrections, scoped fixtures and remaining verification |
| [Provider parity evidence](provider-runtime-parity.md) | Existing provider limits and investigation evidence |
| [Antigravity native history](antigravity-native-history-decision.md) | Pinned primary storage evidence, read-only reader, completeness limits and remaining upstream export dependency |
| [Antigravity runtime and goals](antigravity-runtime-goals.md) | CLI-conversation UAM goal lifecycle and all 26 capability source boundaries |
| [OpenCode V2 protocol assessment](opencode-v2-protocol-assessment.md) | Pinned V2.1.2 native adapter contracts, session/history differences, permission guard and verification limits |
| [Provider sessions](architecture/provider-sessions.md) | Native identities, recovery and compaction details |
| [Chat/workspace services](architecture/chats-imports-workspaces.md) | Imports, ownership, worktrees and deletion |
| [Persistence](architecture/app-state-persistence.md) | Storage/cache relationships |
| [Data-folder transition](data-folder-transition.md) | Preference and stopped handoff/rollback implemented in source; live activation gated |
| [React/CEF](architecture/react-cef-transport.md) | Bridge and history transport |
| [Companion protocol](companion-protocol.md) | Restricted shared HTTP API for browser and native phone clients |
| [Execution hosts](architecture/execution-hosts.md) | Helper and routing details |
| [Memory](architecture/durable-memory.md) | Recall/extraction policy and file delivery |
| [Permissions](architecture/permissions-notifications.md) | Native/UAM approval mapping and imports |
| [Usage](architecture/usage-history.md) | Collection and paged snapshots |
| [Build/release](architecture/build-test-release.md) | Incremental builds, tests and signing |
| [Central setup](central-configuration.md) | Resource delivery and launch constraints |
| [Computer use](computer-use.md) | Controller, trust boundaries and platform packaging |
| [Security](security-enterprise.md) | Existing security controls and deployment checks |
| [Settings history](settings-history.md) | Recorded layout choices and implementation notes |
| [Visual Studio](visual-studio-solution.md) | Platform-specific developer setup |
| [Duplication audit](codebase-dedup-report.md) | Prior code investigation, not product behaviour |
| [Archived performance script](history/run_performance_test.ps1) | Manual Windows 500-chat load fixture; still writes the removed `rag_enabled` field |

Exact current acceptance and assigned owners remain in the [V5 log](<../../V5 Improvement Change log.md>).

## Previous entry points

These short compatibility pages preserve existing repository links: [provider parity](../provider-runtime-parity.md), [central configuration](../central-configuration.md), [duplication audit](../codebase-dedup-report.md) and [Visual Studio](../visual-studio-solution.md).
