# Wayfinder: non-SSH implementation slice

## Destination

Implement and verify every accepted improvement that is independent of remote execution while the
owner beta-tests the current alpha: provider-handoff confirmation and honest task/subtask
presentation. Preserve the existing alpha changes and leave every SSH-dependent schema, runtime,
packaging, and companion decision untouched.

## Constraints

- Do not implement `ExecutionHost`, SSH bootstrap, `uam-runner`, remote journals, remote workspaces,
  host-aware activity, runner updates, remote file/Git operations, or the companion surface.
- Do not create an interim local-only workspace-profile schema that would need another migration when
  hosts are implemented.
- Preserve `wayfinder.md`, `wayfinder-implementation.md`, and
  `wayfinder-remote-execution-plan.md` as prior decisions.
- Append implementation and verification evidence to `progress.md`; do not rewrite it.
- Preserve all five provider implementations. Never launch Gemini CLI locally.
- Reuse the existing provider selector, modal patterns, sub-agent panels, transcript loader, test
  harnesses, and packaged-app workflow.
- Do not commit, publish, release, install software, authenticate providers, or change network state.

## Current frontier

- Route clear. Implement Q-N001, then Q-N002, then execute Q-N003.

## Fog

- Some providers do not expose child session identifiers. This is an upstream capability limit and
  must remain visibly distinct from a failed UAM transcript load.
- Signed-in provider-specific handoff behavior is outside this safe local test run.

## Decisions

### Q-N001 — Which provider-handoff behavior can land without remote architecture?

- Status: resolved
- Why now: It is the accepted P0 and changes no runtime architecture.
- Blocked by: none
- Evidence: `SwitchChatProvider` in `src/app/chat_lifecycle_service.cpp` and the optimistic destination
  defaults in `UI-V2/src/store/slices/sessionsSlice.ts`.
- Decision: Intercept a real provider change in the existing Chat View provider selector and show an
  accessible confirmation dialog. State precisely that recorded UAM messages and tool results,
  title, and workspace remain; the native session and provider-specific live state reset; destination
  model, reasoning/speed, permission/safety, and memory defaults apply; the selected UAM agent and
  goals remain. Cancel makes no store
  or backend request; confirm calls the existing transactional switch exactly once. Same-provider
  selection remains a no-op. Do not add a preview endpoint while the existing mutation contract is
  stable and fully testable.
- Consequences: The user receives the missing warning without changing persistence or provider
  lifecycle code under alpha testing.
- Revealed questions: none

### Q-N002 — What task/subtask improvement is truthful and alpha-safe?

- Status: resolved
- Why now: The existing panel already contains the correct protocol boundary but does not consistently
  explain whether an item is a transcript-capable subtask or an opaque provider event.
- Blocked by: none
- Evidence: `SubAgentHistory` in `UI-V2/src/components/chat/MessageBlocks.tsx` and
  `SubAgentRunningPanel` in `UI-V2/src/components/chat/ToolCallViews.tsx`.
- Decision: Keep the current chat as the task and refine the existing sub-agent panel only. Label a
  child with a stable `subAgentId` as a subtask and expose provider, status, and transcript availability.
  Label a child without an ID as a provider sub-agent event and state that its provider did not expose
  a separate transcript. Distinguish that case from a real load failure. Do not add a second chat tree,
  recursive hierarchy, polling architecture, or fabricated child messages.
- Consequences: The UI is more understandable without changing ACP, chat persistence, or provider
  implementations.
- Revealed questions: none

### Q-N003 — What evidence is required before this slice is complete?

- Status: resolved
- Why now: The owner requested Computer Use proof and the repository contains substantial uncommitted
  alpha work that must not regress.
- Blocked by: Q-N001, Q-N002
- Evidence: existing Vitest component tests, native CTest suites, packaged macOS smoke workflow, and
  the Computer Use skill.
- Decision: Require focused tests for dialog cancel/confirm/same-provider and subtask-ID/no-ID/load-
  failure states; the full frontend test suite and production build; native CTest regression; packaged
  macOS smoke; and a packaged-app Computer Use journey with an isolated `UAM_DATA_DIR`. Computer Use
  must re-read accessibility state after every action and may use fixture data only. Gemini CLI is
  never invoked. Record exact passed, failed, or unexecuted evidence in `progress.md`.
- Consequences: Completion is based on independent code, build, package, and rendered-product evidence.
- Revealed questions: none

## ROUTE_CLEAR

The non-SSH scope, exclusions, implementation points, and verification gates are explicit. The next
action is Q-N001 in the existing Chat View and its tests.
