# Review Changes: Small-Model Workflow

Date: 2026-07-25
Branch: `codex/exhaustive-bug-audit`
Merged from: `codex/small-model-optimization`
Integration worktree: `/private/tmp/uam-exhaustive-small-model-merge`

## Outcome

UAM now has an opt-in small-model workflow for structured ACP chats. It is designed for
27B-class local models, including the intended Qwen 3.6 27B/OpenCode setup, without
changing normal chat behavior when the switch is off.

Enable it either:

1. Per provider in **Settings → New Chat Defaults → OpenCode → Small-model workflow**.
2. Per existing chat in the composer **Options → Small-model workflow** control.

With the workflow enabled, an ordinary user request automatically becomes a durable
UAM-managed goal and follows this controller sequence:

1. Read-only planning turn: inspect the actual project and produce 3–8 atomic,
   verifiable steps without editing.
2. Reviewer turn: convert that plan into durable completed/remaining/current-step
   state and select exactly one next step.
3. Worker turn: execute only that step, run one focused verification, report, and stop.
4. Reviewer turn: accept evidence, update durable progress, and choose the next atomic
   step.
5. Repeat until the reviewer proves the complete objective or records a real blocker.

The existing token budget, maximum-loop limit, repeated-output detection, stalled-loop
watchdog, pause/resume behavior, and persisted goal state remain in force.

## Features Added

- Provider defaults and per-chat overrides for `smallModelMode`.
- Backward-compatible settings and chat-file persistence.
- Mode propagation when creating chats, switching providers, and branching chats.
- Automatic durable goal creation for ordinary prompts in small-model chats.
- Mandatory planning-only first turn.
- One atomic implementation step per worker turn.
- Durable objective, completed work, remaining work, current step, and last
  verification injected into later turns.
- Small-model progress context is bounded to recent completed work and upcoming steps;
  the full durable state remains stored on disk.
- Small-model queues preserve each user message as a separate FIFO item instead of
  merging or draining the full queue into one oversized prompt.
- Memory recall is ranked by deterministic lexical relevance to the current request
  before the existing byte budget is applied.
- Visible active-mode chip and chat-settings status.
- Provider-default and per-chat controls use the existing optimistic save/rollback
  behavior.

## Issues Fixed

### Malformed reviewer output could falsely complete unfinished work

Previously, an unreadable reviewer response took the
`review_json_invalid_default_complete` path and marked the goal complete. UAM now makes
one strict JSON repair attempt. If that also fails, the goal is blocked with
`goal_blocked_invalid_review`; it is never treated as complete.

### Internally queued worker turns were not reviewed

Worker continuations do not create visible user messages, so their user-message index is
`-1`. The scheduler previously rejected those turns as missing an index, which could
leave the loop relying on the stalled-loop watchdog instead of reviewing the completed
step. UAM now retains the internal worker prompt and schedules its reviewer normally.

### Queued prompts could overwhelm a small model

Normal UAM behavior merges compatible queued prompts and can drain the queue as one
batch. Small-model chats now keep and drain one prompt at a time. Standard chats retain
their existing batching behavior.

### Durable progress was absent from worker continuation prompts

Goal progress was persisted and shown to reviewers but not included in ordinary worker
continuations. Small-model worker prompts now receive the objective, current assignment,
recent completed work, upcoming work, and last verification.

### Memory recall ignored the current prompt

`BuildRecallPreface` accepted the prompt but did not use it. Small-model chats now rank
memory previews by request overlap before enforcing the existing recall budget. Standard
chat ordering is unchanged.

### Source-branch UI audit tests did not compile in the production build

The source branch added Node-based Vitest files while the production TypeScript build
compiled every file under `src` without Node type declarations. Production type-checking
now excludes `*.test.ts`/`*.test.tsx`; those files still run through Vitest. Two DOM test
helpers were also made ES2020-safe and explicitly typed. No dependency or production
behavior changed.

## Main Review Areas

- Controller prompts and progress shaping:
  `src/app/goal_service.cpp`
- Goal review, repair, and continuation loop:
  `src/common/runtime/acp/acp_goal_loop.cpp`
- Automatic goal creation and atomic prompt queues:
  `src/common/runtime/acp/acp_session_runtime.cpp`
- Internal worker prompt retention:
  `src/common/runtime/acp/acp_session_lifecycle.cpp`
- Relevant memory selection:
  `src/app/memory_service.cpp`
- Chat/provider persistence:
  `src/common/models/app_models.h`,
  `src/common/config/settings_store.cpp`,
  `src/common/chat/chat_repository.cpp`
- User controls:
  `UI-V2/src/components/settings/SettingsModal.tsx`,
  `UI-V2/src/components/chat/Composer.tsx`

## Verification

- `npm --prefix UI-V2 ci`
- `npm --prefix UI-V2 run test`
  - 32 test files passed after source-branch integration.
  - 369 tests passed after source-branch integration.
- `npm --prefix UI-V2 run build`
  - TypeScript and production Vite build passed.
- `cmake -S . -B Builds/tests -DUAM_BUILD_TESTS=ON`
- `cmake --build Builds/tests --config Debug --parallel 8`
  - Native tests and signed macOS app bundle built successfully.
- `ctest --test-dir Builds/tests -C Debug --output-on-failure`
  - `uam_core_tests` passed.
  - `uam_macos_application_tests` passed.
  - `uam_platform_ifdef_guard` passed.

Regression coverage now includes:

- planning and one-step worker prompt contracts;
- provider-default and per-chat persistence;
- automatic small-model goal creation;
- non-merged FIFO prompt queues;
- reviewer repair followed by safe blocking;
- review scheduling after internal worker continuations;
- prompt-relevant memory ordering;
- provider-default UI and live per-chat toggle behavior.

## Deliberate Boundaries

- No live Infidec/OpenCode/Qwen run was performed because Infidec is being overhauled.
  The next live check should select the Qwen 3.6 27B model in an OpenCode structured
  chat, enable the workflow, and run a multi-file task through at least three reviewed
  worker steps.
- The controller applies to structured ACP chat sessions. The xterm.js terminal
  fallback remains unchanged.
- The reviewer intentionally uses the same selected local model; no paid or hidden
  frontier-model dependency was added.
- Memory ranking is local lexical matching, with no embeddings, network service, or new
  dependency.
- No provider implementation was removed or given provider-specific prompt behavior.
  The controller is shared across Gemini CLI, Codex CLI, OpenCode CLI, Claude Code CLI,
  and GitHub Copilot CLI structured sessions.
