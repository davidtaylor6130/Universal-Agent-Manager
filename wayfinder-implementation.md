# Wayfinder: Universal Agent Manager audit implementation

## Destination

Every confirmed P0-P2 recommendation in the 2026-08-27 Universal Agent Manager audit is either
implemented at its smallest shared boundary and validated in the packaged product, or rejected by a
resolved evidence-backed decision showing that the proposed change would make the product less safe.
The route is clear when compatibility truth, runtime recovery, persistence, UI/accessibility,
provider-neutral workflows, security, and release gates have no unresolved implementation decision.

## Constraints

- Preserve all five providers and the release slice in `AGENTS.md`.
- Reuse current services, components, data formats, and test harnesses before adding abstractions.
- Add the smallest regression for every non-trivial correctness fix.
- Append implementation and validation evidence to `progress.md`; never rewrite its history.
- Preserve the pre-existing untracked root `node_modules/` directory and `wayfinder.md`.
- Do not push, publish, release, create remote issues, or mutate provider accounts.
- Keep Windows behavior explicit when this macOS host cannot execute it.

## Current frontier

- None. All implementation decisions are resolved; Windows execution remains an explicit external
  release gate rather than an unresolved product decision.

## Fog

- Signed-in behavior, quota, and account-specific provider flags that cannot be exercised without
  changing user/provider state.
- Native Windows runtime behavior beyond compile-time and platform-contract coverage on this host.

## Decisions

### Q-I001 — What is the smallest truthful provider compatibility model?

- Status: resolved
- Why now: It blocks the P0 Gemini, Codex, update, and New Chat fixes.
- Blocked by: none
- Evidence: `src/common/runtime/provider_cli_compatibility_service.cpp`,
  `src/core/gemini_cli_compat.cpp`, `UI-V2/src/services/updateCatalog.ts`, live packaged-app evidence.
- Decision: Installed/available is distinct from compatibility. Every provider reports one of
  `unknown`, `checking`, `installing`, `verified`, `untested`, `untested-newer`,
  `known-incompatible`, `unavailable`, or `provider-managed`. A dated verified version is evidence,
  not an upper bound. Only a documented minimum creates a known-incompatible state. Untested safe
  versions remain launchable with an honest label. The preferred install channel is `latest`; a
  dated verified build may remain available as an explicit recovery choice. Update notices compare
  the installed version only with the current package catalog and can never select an older curated
  build.
- Consequences: Gemini below the current verified/minimum baseline is repair-blocking; current and
  newer safe builds are truthful rather than silently blessed. Codex no longer rejects every build
  except two pinned versions. Settings can show last-verified evidence without presenting a rollback
  as an update.
- Revealed questions: Q-I003.

### Q-I002 — What write outcome can callers safely act on?

- Status: resolved
- Why now: Current boolean failure can mean either no commit or a committed primary with degraded backup.
- Blocked by: none
- Evidence: `src/common/io_utils.h`, `src/app/chat_repository.cpp`, persistence caller tests.
- Decision: The shared atomic-write result exposes primary commit and backup degradation separately.
  Existing boolean callers mean “the requested primary content is durable”; detailed callers may
  additionally surface degraded backup maintenance. Metadata-only chat writes must validate both
  generations, preserve any recoverable transcript, and refuse mutation when neither generation can
  supply the unloaded content.
- Consequences: A committed primary never causes an in-memory rollback, backup warnings describe
  actual recovery/degradation rather than an ordinary previous generation, and unloaded metadata
  edits cannot erase the last valid transcript.
- Revealed questions: Q-I006.

### Q-I003 — How should provider readiness gate chat creation?

- Status: resolved
- Why now: Readiness must be visible without preventing the terminal escape hatch or advanced use.
- Blocked by: Q-I001
- Evidence: provider discovery state, `NewChatModal.tsx`, structured and terminal creation paths.
- Decision: New Chat presents the native compatibility taxonomy and its evidence before creation.
  Structured chat is blocked only while checking/installing or when the provider is known incompatible
  or unavailable. Verified, provider-managed, and honestly labelled untested versions remain usable.
  Blocked states expose Check again and supported install/recovery actions. Terminal fallback opens
  directly in the terminal pane when the provider can launch; it is disabled when the provider is
  unavailable or its interactive contract is known incompatible.
- Consequences: UAM does not silently bless unknown binaries or trap advanced users behind stale
  compatibility data. A provider problem remains attributable and recoverable without removing the
  cross-provider terminal escape hatch.
- Revealed questions: Q-I008.

### Q-I004 — Can CEF sandboxing be enabled in the current package?

- Status: resolved — documented packaging change, not a safe toggle
- Why now: The application currently disables Chromium sandboxing in the main and helper processes.
- Blocked by: none
- Evidence: `src/app/application.cpp`, helper launch/build configuration, packaged launch behavior.
- Decision: Do not toggle `no_sandbox` in the current package. The pinned CEF contract requires macOS
  to stop directly linking the framework, dynamically load it, and initialize a scoped sandbox inside
  each helper before CEF loads. Windows additionally needs sandbox-library linkage and sandbox-info
  plumbing. The current build reuses a copy of the main executable for all helpers and this host cannot
  prove the Windows half. The exact limitation and required release proof are now documented; the
  existing trusted-main-frame bridge, private `uam://` origin, CSP, path containment, and packaged
  signature/resource smoke checks remain the actual controls.
- Consequences: UAM does not claim a sandbox it has not initialized. Enabling it remains a bounded
  packaging-architecture project that must land with both platform package matrices green, rather
  than a boolean change likely to break helpers or create a false control.
- Revealed questions: Q-I008.

### Q-I005 — Which parent environment values may provider processes inherit?

- Status: resolved
- Why now: Blanket inheritance exposes unrelated environment credentials to every provider child.
- Blocked by: none
- Evidence: macOS and Windows process/PTY launch code plus provider authentication requirements.
- Decision: Provider children inherit the normal process environment but the shared launch policy
  blanks conventional direct-provider API keys that do not belong to the selected provider. Codex,
  Claude, Gemini, and Copilot retain only their directly relevant keys; OpenCode remains exempt
  because it is intentionally multi-provider. `UAM_PRESERVE_PROVIDER_CHILD_SECRETS=1` is the explicit
  compatibility escape hatch. The same overrides apply to ACP, terminal, and text-worker paths.
- Consequences: Ordinary cross-provider credential leakage is removed without fabricating an
  exhaustive cloud-secret denylist or breaking OpenCode's core use case. Broader AWS/Azure and custom
  provider variables remain a documented ceiling for a future opt-in allowlist model.
- Revealed questions: none.

### Q-I006 — What is the minimum durable export format?

- Status: resolved and implemented
- Why now: Friendly local export/import is part of the provider-neutral ownership promise.
- Blocked by: Q-I002
- Evidence: current chat JSON schema, repository backup/recovery behavior, existing UI actions.
- Decision: Use a dependency-free versioned folder bundle containing `manifest.json` and canonical
  `chats/*.json`. Export all local chats to a new/empty chosen folder. Import validates the manifest
  and every chat before mutation, never overwrites an existing chat identity, and reports degraded or
  partial outcomes explicitly. Imported chats are passive read-only transcripts: local workspace,
  native-session, permission, agent, goal, attachment, and worker authority is stripped, and execution
  boundaries reject them. Manifests, chat counts, individual chat sizes, and aggregate data are bounded
  before canonical JSON is read; export enforces the same envelope so it cannot create a bundle this
  version refuses to import. Export discovery is read-only and bounded: it validates canonical files
  individually and reports any remaining legacy directories for migration rather than hydrating them.
  Zip, cloud sync, provider-native share links, and whole-data-root
  replacement are outside this local ownership path.
- Consequences: Users receive a readable, inspectable, cross-machine backup without a new archive
  dependency or a dangerous restore operation. The format can be zipped externally without changing
  its semantics.
- Revealed questions: none.

### Q-I007 — How far should the existing review surface be consolidated?

- Status: resolved
- Why now: UAM already has diffs, checkpoints, rollback, staging, and commit controls; duplicating an IDE is out of scope.
- Blocked by: none
- Evidence: current ChatView, diff modal, VCS panel, checkpoint actions, release-slice constraints.
- Decision: Consolidate discovery and navigation, not editing. One Chat View area now groups the
  existing changed-file/diff list, checkpoint context and rollback, and entry to the existing commit
  panel. The sidebar's existing Active chats and shared status model expose aggregate running,
  attention, and done counts. Do not add an editor, inline-comment store, or second activity system.
- Consequences: Builders can move from a completed turn to exact diff, rollback, staging, and commit
  without hunting across unrelated cards, while UAM remains an agent manager rather than an IDE.
- Revealed questions: none.

### Q-I008 — Which release gates can be proven on this host?

- Status: resolved
- Why now: The audit requires exact packaged checks while this machine can execute only macOS artifacts.
- Blocked by: Q-I001, Q-I003, Q-I004
- Evidence: CMake/CTest targets, frontend tests, packaged macOS harness, Windows containment tests.
- Decision: This host proves the complete frontend suite/build, complete native macOS suite, strict
  app-bundle signature verification, exact packaged UI/resources smoke checks, generated frontend
  CycloneDX SBOM validation, and isolated packaged-app journeys. Windows containment and packaging
  remain mandatory CI matrix gates and must not be represented as executed here. Dependency-audit
  network results are attached only when the package registry is reachable; this run records the
  sandbox/policy denial instead of inventing a clean result.
- Consequences: macOS evidence is exact and reproducible, Windows claims remain honest, and release
  automation cannot publish without its platform artifact/evidence graph. The remaining CEF sandbox
  architecture work stays disclosed rather than weakening or mislabelling the current package.
- Revealed questions: none.
