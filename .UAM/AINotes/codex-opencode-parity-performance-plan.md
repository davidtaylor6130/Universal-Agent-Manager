# UAM Alpha-2 performance and parity plan

## Summary

UAM can reach Codex-like responsiveness without replacing CEF. The separate Alpha-3 performance
work already measured 8.9 ms input-to-paint p95 under streaming, 23.0 MiB renderer heap, and
368.2 MiB process-tree RSS. The shortest route is to land that work, then close four product gaps:
native Codex steering, provider-aware review, side chat, and dependable browser control.

OpenCode cannot currently match Codex's native in-turn steering. UAM should expose Queue and
Interrupt & send for OpenCode, not label interrupt plus restart as steering.

## Baseline and version correction

| Item | Finding |
| --- | --- |
| Daily-driver build | Installed `4.9.0-alpha-2` |
| Newest evidenced local candidate | Signed performance build `4.9.0-alpha-3` |
| Current dirty source labels | `4.8.0-alpha-6` in several files |
| Alpha-9 | Not found locally or in git history |

Before feature work, make one root version value drive the native binary, React metadata, About UI,
packaging, runner compatibility, and CI assertions. Do not guess Alpha-9. If Alpha-3 is the last real
local build, the next local build is Alpha-4.

## Performance comparison

The old live sample is useful for diagnosis, not release acceptance, because the two apps had
different uptime and open work.

| Live observation | UAM Alpha-2 | Codex | Meaning |
| --- | ---: | ---: | --- |
| Shell CPU | 204.7 to 245.8% | 37.6 to 47.8% | UAM's own UI process was the primary CPU problem. |
| Shell memory | 748.0 to 1154.7 MiB | 524.5 to 833.4 MiB | Long-lived UAM renderer growth needs a soak test. |
| Whole-tree processes | 70 | 29 | UAM retains many chat-specific provider processes. |
| Whole-tree memory | 1190.6 to 1616.5 MiB | 924.6 to 1295.4 MiB | Fix renderer churn first, then remeasure provider lifetime. |

CEF is about 75% of UAM's application bundle, but Codex also bundles Chromium and has a larger total
application. Bundle size and UI responsiveness are separate problems.

The measured performance worktree already contains the changes worth keeping:

- `27600b8b`: stop continuous status-dot repaint. Idle tree CPU fell from 33.57% to 0.25% median.
- `9bbd93d5`: cache parsed chat view modes. Renderer benchmark improved 97.87% overall.
- `064df80f`: skip empty attachment conversion. Append work improved 36.53%.
- `f7678cd7`: fast-path active stream reconciliation. Streaming work improved 90.58%.
- `8c200c7a` and `3e114516`: omit empty collections. Message heap improved 37.15% and sanitizer peak heap improved 50.76%.
- `84f02234`: spare-renderer experiment. Do not land it. `bb102fd4` reverted it as unstable.

Review and cherry-pick only the proven commits after the performance worktree owner finishes. Preserve
their small commit boundaries and do not copy uncommitted files from that worktree.

## Required changes

### P0: Version truth and repeatable measurement

1. Add one canonical version value and derive all build and CI checks from it.
2. Keep an Alpha-2 result fixture for 1, 4, and 9 panes, a 2,000-message chat, idle, eight stream
   chunks, and a 30-minute soak.
3. Measure fresh launches with the same data, pane count, uptime, and provider state. Record UI
   latency separately from provider network latency.

Acceptance: no conflicting product versions, input-to-paint p95 at or below 16.7 ms, settled idle CPU
at or below 1%, renderer heap at or below 30 MiB under the fixed stream case, and no post-warmup RSS
growth above 10% in the soak.

### P0: Complete the existing performance integration

Land the measured Alpha-3 commits first. Also review the owner worktree's still-uncommitted draft
debounce and turn-identity work after it is finished. Measure each logical commit. Remove or revert a
change that misses its gate.

Do not add process pooling. If provider processes remain material after the UI fix, retire only idle,
invisible sessions after a small fixed timeout and resume them through the existing session ID.

### P0: Native steering with honest provider controls

Codex:

- Add the App Server `turn/steer` request and response path.
- Send `threadId`, new input, and `expectedTurnId` while the matching turn is active.
- Do not send `turn/interrupt` when steering succeeds.
- If the turn ended or the ID is stale, queue the input as the next normal turn. Never cancel a newer turn.

OpenCode:

- Replace the misleading Steer now action with Queue and Interrupt & send.
- Keep generic interrupt plus send as an explicit user choice.
- Add native steering later only when OpenCode exposes a documented protocol operation.

Checks: request-shape unit test, same-turn state test, stale-turn race test, no-interrupt test, and UI
capability-label test.

### P1: Browser use

First, pass UAM's enabled MCP server configuration into the Codex process using scoped command-line
configuration. Do not write to the user's global Codex configuration. Pin the Playwright MCP package
version instead of using `@latest`.

Then create three static browser-pane mocks and stop for selection. After selection, build one
disposable prototype to prove navigation, screenshot capture, focus, clipboard, localhost access,
and authenticated tool control in a second isolated CEF context. Do not enable global unauthenticated
CDP access.

The MVP needs URL controls, back, forward, reload, localhost shortcuts, screenshot, and Attach to
chat. It does not need browser tabs, bookmarks, extensions, history sync, or a download manager.

### P1: Side chat

Create three static mocks and stop for selection. The implementation should reuse an ordinary child
chat with `parent_chat_id`, existing provider and workspace inheritance, and the current split-pane
system. Closing the side pane must not delete the chat. The child must survive restart and be openable
as a normal pane.

Do not add a second chat store, transcript format, or orchestration layer.

### P1: AI code review

Add one Review with provider action to the existing Review changes card.

- Codex uses native `review/start` on the current thread, starting with uncommitted changes.
- OpenCode uses one stored, provider-neutral review prompt over UAM's existing diff data.
- Findings render as a normal assistant turn and link path and line references to the existing diff dialog.

Do not build a new VCS panel or detached review workflow. Checks cover no working-tree mutation,
correct path and line navigation, cancellation, retry, and provider preservation.

### P1 dependency: Computer Use reliability

The existing bug-swarm owns implementation. This plan depends on its findings:

1. Give the helper a stable signing identity so macOS TCC approval survives rebuilds.
2. Report OS-ready only after the accessibility permission preflight succeeds.
3. Replace the full CEF-linked companion with a minimal dedicated helper.
4. Surface exact bridge failures in the current modal.
5. Correct the app named in denial instructions.
6. Validate the helper independently in CI.
7. Bound every `window.cefQuery` wait and make the developer launcher identity explicit.

Browser MCP work can proceed independently, but general Computer Use parity is not complete until
these P1 and P2 defects are closed. Source: [Computer Use bug-swarm report](./computer-use-bug-swarm/report.md).

## Safe implementation sequence

Each row is one small local commit unless two files are inseparable. Run the focused check before
committing and stage explicit paths only.

| Order | Commit intent | Minimum check |
| ---: | --- | --- |
| 1 | Make product version single-source | Version contract test and both builds |
| 2 | Freeze the controlled Alpha-2 benchmark | Benchmark self-check |
| 3 | Land proven performance commits in their existing order | Frontend tests, native focused tests, benchmark gate |
| 4 | Complete Computer Use fixes in its owner chat | Helper signing, permission, bridge, and packaging checks |
| 5 | Add Codex `turn/steer` protocol support | Focused ACP tests |
| 6 | Add stale-turn fallback and provider-specific labels | ACP and React tests |
| 7 | Inject pinned browser MCP config into Codex launch | Launch-argument tests |
| 8 | Add Codex and OpenCode review actions | Review and no-mutation tests |
| 9 | Produce side-chat mocks, then implement the selected reuse path | Persistence and pane tests |
| 10 | Produce browser mocks and control prototype, then implement the selected pane | Focus, permission, screenshot, and local-page checks |
| 11 | Run macOS and Windows release-candidate benchmarks | Full test, build, soak, and signed-package checks |

## Explicitly deferred

- Renderer migration. Reconsider only for a hard package-size target or a requirement to remove CEF
  patching and sandbox debt. Prototype WKWebView on macOS and WebView2 on Windows before any rewrite.
- Shared provider server or process pool. Add only if post-fix profiling proves idle retirement is
  insufficient.
- OpenCode native steer emulation. Interrupt plus restart is not steering.
- Five-provider handoff and attempt comparison, as requested.

## Protocol references

- [Codex App Server](https://learn.chatgpt.com/docs/app-server)
- [Codex MCP configuration](https://learn.chatgpt.com/docs/extend/mcp)
- [Codex advanced configuration](https://learn.chatgpt.com/docs/config-file/config-advanced)
- [OpenCode server API](https://opencode.ai/docs/server/)
- [OpenCode queue and steer request](https://github.com/anomalyco/opencode/issues/32157)
- [Microsoft WebView2 distribution](https://learn.microsoft.com/microsoft-edge/webview2/concepts/distribution)
- [Apple WKWebView](https://developer.apple.com/documentation/webkit/wkwebview)
