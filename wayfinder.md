# Wayfinder: Universal Agent Manager comprehensive review

## Destination

Produce an evidence-backed, read-only assessment of the current UAM product that is specific
enough for the owner to choose what to fix or build next. The route is clear when core journeys,
UX/design, stability, architecture, provider parity, and current competitor positioning have each
been evaluated; findings are deduplicated against existing plans; and every recommendation has a
priority, affected area/provider, rationale, importance rating, evidence, and stated confidence.

## Constraints

- Do not modify application code, configuration, user data, provider accounts, or external systems.
- Preserve all five provider implementations and the release slice in `AGENTS.md`.
- Treat `OverhallPlan.md`, `review_found_issues.md`, and `progress.md` as prior evidence, not proof
  that the current product behaves correctly.
- Prefer the packaged macOS application and an isolated `UAM_DATA_DIR` for live review.
- Do not claim Windows or signed-in provider behavior without direct evidence.
- Compare against current first-party product documentation where available.
- Leave this map uncommitted; it is an audit artifact, not a product change.

## Current frontier

- Route clear. Q-007 records the actionable ordering; the remaining uncertainty is explicit in Fog.

## Fog

- Windows-only behavior without a Windows runner.
- Live provider authentication, quota, model catalog, and protocol drift without mutating accounts.
- Competitor behavior that is undocumented, staged, account-specific, or rapidly changing.
- Long-duration resource behavior beyond the practical observation window.

## Decisions

### Q-001 — Who is UAM for and what journeys must it win?

- Status: resolved
- Why now: Product intent determines whether density and advanced controls are strengths or defects.
- Blocked by: none
- Evidence: Owner context on 2026-08-27; `README.md` key features and universal-history promise;
  supported-provider and release-slice constraints in `AGENTS.md`.
- Decision: UAM is for provider-fluid builders, tinkerers, and open-source contributors who use
  several coding-agent providers, change subscriptions or preferred models, reject lock-in, and want
  one locally owned workspace, history, and capability model. Its must-win journey is
  `open one workspace -> find/resume any prior chat -> choose or switch provider -> retain context
  and UAM-level capabilities -> complete/recover the task without learning another interface`.
- Consequences: Reviews should tolerate expert density where it speeds repeat use, but first-run
  orientation, provider switching, capability truthfulness, recovery, and preservation of context
  are release-critical. UAM should normalize the working environment, not falsely imply that
  upstream providers or structured/terminal transports have identical guarantees.
- Revealed questions: Q-008.

### Q-002 — What does the current packaged product actually do?

- Status: resolved
- Why now: A real-product review cannot infer behavior from tests or screenshots.
- Blocked by: none
- Evidence: build identity, isolated launch, first-run and return-use journeys, logs, screenshots.
- Decision: The exact packaged macOS app is a functioning local agent manager, not a mock shell. In
  an isolated data root it enforced workspace selection, exposed all five providers, created a
  structured Codex chat, returned a real GPT-5.6-Sol response, opened a real Codex terminal, and
  recovered the workspace, selected chat, provider attribution, prompt, and response after a clean
  restart. It also exposed goals, skills, memory, provider/model controls, structured/terminal views,
  worktrees, diffs, checkpoints, voice, MCP, editor actions, and concurrent panes. The same package's
  update surface is completely nonfunctional because its renderer fetches violate its own CSP, and
  its default Gemini 0.38.1 failed against Google's current backend.
- Consequences: Preserve the compact shell and dual-path architecture, but treat provider readiness,
  compatibility, update recovery, and packaged-policy integration tests as core product behavior.
- Revealed questions: none.

### Q-003 — Where do the current UI and workflows help or hinder?

- Status: resolved
- Why now: The requested outcome prioritizes user experience and workflow clarity.
- Blocked by: none
- Evidence: live journeys, keyboard/resizing/state checks, React/CSS implementation.
- Decision: UAM's visual system is coherent, calm, and product-specific; provider attribution,
  structured/terminal boundaries, empty/error/retry states, slash discovery, and progressive controls
  are generally strong. The principal UX weakness is that readiness is hidden: every provider looks
  selectable before install/auth/version/transport checks settle, and raw protocol errors carry no
  direct repair action. The settings and Skills surfaces are powerful but dense. Default tertiary
  text fails normal-text contrast (approximately 3.45:1 dark and 2.45:1 light) while often rendered
  at 9–11 px. Confirmed interaction defects affect double-submitted goals, pending New Chat dismissal,
  Memory draft loss, mouse-only outer separators, and focus handling in portalled menus.
- Consequences: Improve the existing journey and components before adding another major surface:
  readiness in the provider picker, actionable failures, visible labels, contrast/type scale, and one
  shared popup/focus contract.
- Revealed questions: none.

### Q-004 — Which stability and architecture risks remain?

- Status: resolved
- Why now: Prior remediation covered 86 findings, so this audit must verify rather than repeat it.
- Blocked by: none
- Evidence: runtime/service call paths, persistence boundaries, tests, builds, fault behavior.
- Decision: The baseline is materially strong: 513/513 frontend tests, the production frontend build,
  the signed Release app build, and 5/5 native CTest targets passed. Process ownership, bounded I/O,
  atomic replacement, lazy hydration, state revisions, stale-result rejection, and optimistic rollback
  are substantial strengths. Newly confirmed high risks remain: corrupt ACP streams can leak stale
  correlations into a later turn; transport noise can defeat inactivity recovery; internal callers can
  substitute the active provider for an explicitly unavailable one; unloaded metadata saves can destroy
  the last valid transcript backup; and a committed primary write can still be reported as failed.
  Secondary risks include an OpenCode `--auto` permission bypass, cross-provider environment-secret
  inheritance, false backup warnings, deferred tool-content reconciliation, CEF sandbox disablement,
  and missing current dependency-advisory evidence.
- Consequences: Fix shared execution and persistence boundaries before feature work. Do not accept
  green unit tests as proof of packaged network policy, fault-commit semantics, or signed-in provider
  compatibility.
- Revealed questions: none.

### Q-005 — How complete and honest is provider parity?

- Status: resolved
- Why now: Multi-provider consistency is UAM's core promise and largest integration surface.
- Blocked by: none
- Evidence: provider registry/runtime code, capability matrix, UI gating, fixtures, live checks where safe.
- Decision: Feature parity is unusually broad and the explicit UAM-managed/provider-managed capability
  boundary is the right design. All five providers have normalized history, structured and terminal
  paths, lifecycle controls, model/mode persistence, tool rendering, agents, goals, memory, and
  workspace support where the upstream protocol permits it. Parity is not yet operationally honest:
  Gemini's stale curated version fails live; working Codex 0.148 is labelled unsupported relative to
  0.124; the update logic intentionally presents that downgrade as an installable update; provider
  health is absent from New Chat; OpenCode `--auto` escapes the terminal safety filter; and internal
  execution can cross provider identities when one is unavailable.
- Consequences: Replace binary supported/unsupported claims with evidence-bearing states such as
  verified, untested-newer, known-incompatible, unavailable, and provider-managed. Never describe a
  downgrade as an update, and never substitute providers without an explicit user decision.
- Revealed questions: none.

### Q-006 — Where does UAM lead, lag, or differ from current competitors?

- Status: resolved
- Why now: Recommendations must improve UAM's own workflows, not blindly copy competitors.
- Blocked by: none
- Evidence: current first-party documentation for Codex, Claude Code, Gemini/Antigravity,
  OpenCode, and GitHub Copilot CLI and their GUI surfaces.
- Decision: UAM leads on local, provider-neutral continuity: one transcript store, mid-chat provider
  switching with message provenance, structured and raw CLI access, and common agents/goals/skills/
  memory/worktree controls. Codex, Claude Desktop, Antigravity, and the GitHub Copilot app currently
  lead on integrated change review, pane/task/subagent visibility, first-run readiness, and guided
  recovery; Copilot CLI also has unusually complete export/share flows. Gemini, Claude, Codex, OpenCode,
  and Copilot all continue to expand provider-native agents, skills, permissions, worktrees, and slash
  commands, making a static parity table insufficient without runtime discovery and dated verification.
- Consequences: Compete as the best local multi-provider manager, not as another IDE or cloud runner.
  Consolidate existing diff/checkpoint/commit controls, add local export/import and clearer activity,
  but defer remote execution, mobile control, cloud sync, IDE autocomplete, and full file editing until
  target-user evidence says they are necessary.
- Revealed questions: none.

### Q-007 — What should the owner act on first?

- Status: resolved
- Why now: This is the final synthesis requested by the owner.
- Blocked by: none
- Evidence: resolved decisions and deduplicated findings.
- Decision: First restore the compatibility/update truth loop and eliminate persistence/provider-
  attribution hazards. Next make provider readiness and recovery visible in New Chat and Settings.
  Then close accessibility/interaction gaps and consolidate review/activity/export workflows. Only
  after those should UAM consider broader competitor features.
- Consequences: The final report orders confirmed findings by user harm and leverage, not novelty.
- Revealed questions: none.

### Q-008 — Which capabilities belong in UAM's provider-neutral layer?

- Status: resolved
- Why now: The product strategy is to take the best features from each provider and make them
  available across providers, but upstream protocol differences can make literal parity impossible.
- Blocked by: none
- Evidence: Owner context; provider capability matrix; runtime/UI implementation; competitor review.
- Decision: UAM owns local chat identity/history/search/export, workspace selection, provider
  provenance, agents, goals, Skills, memory, permission presentation, worktree/checkpoint/review
  controls, activity/attention, diagnostics, and crash recovery. Providers own authentication, quota,
  models and modes, native slash commands, tool availability, wire-level permission semantics, and
  cloud/remote execution. UAM should discover those capabilities dynamically, translate them into one
  command/picker grammar, label who controls each capability, and retain the raw terminal escape hatch.
- Consequences: "Same support" means a consistent workflow and honest fallback—not fabricated
  semantics. A capability should be promoted into UAM only when UAM can own its state, safety, recovery,
  and cross-provider contract independently of a specific upstream wire format.
- Revealed questions: none.
