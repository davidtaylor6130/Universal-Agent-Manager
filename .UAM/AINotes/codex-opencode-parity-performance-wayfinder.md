# Wayfinder: Codex and OpenCode parity with Alpha-2 performance

## North Star

Make UAM feel as responsive as Codex while retaining UAM's stronger split-pane workflow. Add the
Codex and OpenCode capabilities that are useful in daily work, describe provider limitations
honestly, and avoid speculative platform rewrites.

## Scope

- Baseline: the installed `4.9.0-alpha-2` application.
- Priority providers: Codex CLI and OpenCode CLI.
- Included: responsiveness, steering, browser use, side chat, code review, process lifecycle,
  Computer Use dependencies, version discipline, and renderer choice.
- Excluded: five-provider handoff and attempt comparison.
- Output: an incremental implementation route. This investigation does not modify product code.

## Resolved decisions

| Question | Evidence | Decision |
| --- | --- | --- |
| Which build is the baseline? | The installed app reports `4.9.0-alpha-2`. The current dirty source reports `4.8.0-alpha-6`. A separate performance worktree produced `4.9.0-alpha-3`. No Alpha-9 artifact was found. | Measure Alpha-2. Treat Alpha-3 as the newest evidenced local candidate. Fix version drift before another build. |
| Is Chromium the main cause of poor responsiveness? | UAM's live shell used roughly 205 to 246% CPU, but the measured Alpha-3 CEF build reached 8.9 ms input-to-paint p95 under stream pressure. Codex also bundles Chromium and its application is larger. | Keep CEF for this route. Renderer replacement is a separate size and security project, not a performance prerequisite. |
| What work already exists? | The performance worktree contains measured commits for view-mode caching, empty collection omission, stream reconciliation, attachment conversion, and continuous status repaint removal. | Review and land those commits first. Never recreate them or copy the worktree's dirty state. |
| Can UAM steer Codex without stopping the turn? | Codex App Server exposes `turn/steer` with `threadId`, input, and `expectedTurnId`. UAM currently implements steering as interrupt plus a priority prompt. | Add native Codex steering and fall back safely when no matching turn is active. |
| Can OpenCode provide the same steering behavior? | UAM uses `opencode acp`. Current OpenCode server and ACP surfaces provide prompt and abort behavior, but no native in-turn steer. The upstream queue and steer request remains open. | Do not fake parity. Show Queue and Interrupt & send for OpenCode. Reserve Steer now for Codex. |
| Does UAM need a new side-chat data model? | UAM already stores parent chat IDs, child sessions, independent chats, and multi-pane layout state. | Reuse an ordinary child chat in a side pane. Add no new persistence system. |
| Does UAM need a new review subsystem? | UAM already has changed files, diffs, commit UI, and chat rendering. Codex has native `review/start`. | Add a review action to the existing review card. Use native Codex review and a durable OpenCode review prompt. |
| Should browser support start as an embedded pane? | UAM already configures Playwright MCP for some providers, but Codex launch does not receive that configuration. An embedded, tool-controlled browser requires a safe CEF or CDP design. | Inject scoped MCP configuration into Codex first. Prototype the embedded browser after three static UI mocks are reviewed. |
| What blocks dependable Computer Use? | The existing bug-swarm report identifies unstable macOS signing, premature permission readiness, an oversized helper, hidden bridge errors, and unbounded bridge calls. | Treat its P1 and P2 items as a separate prerequisite stream. Do not duplicate that chat's implementation. |
| Should idle provider processes be pooled? | UAM keeps a provider process per structured session, but live CPU was dominated by the UAM shell. | Re-measure after the frontend fixes. Add idle retirement only if process RSS remains material. Do not build a shared server pool without profiling evidence. |

## Route

1. Establish one product version source and preserve the Alpha-2 benchmark.
2. Land the existing measured Alpha-3 performance commits with their original boundaries.
3. Complete the Computer Use P1 and P2 stream in its existing owner chat.
4. Add native Codex steering and honest OpenCode queue and interrupt controls.
5. Inject UAM's pinned browser MCP configuration into Codex without changing global Codex config.
6. Add provider-aware AI review to the existing review surface.
7. Produce three side-chat mocks, stop for selection, then reuse child chat and pane primitives.
8. Produce three browser-pane mocks, stop for selection, then run a disposable control prototype.
9. Re-run controlled benchmarks. Add idle session retirement only if the data still calls for it.

## Evidence gates

- UI input-to-paint p95 at or below 16.7 ms, with no task above 50 ms in the controlled scenario.
- Settled idle process-tree CPU at or below 1% for the isolated benchmark.
- Stream-pressure renderer heap at or below 30 MiB and process-tree RSS at or below 450 MiB for
  the 2,000-message fixture.
- Renderer RSS grows no more than 10% during a 30-minute post-warmup soak.
- Codex steering sends no interrupt when `turn/steer` succeeds.
- OpenCode labels never imply native steering.
- Browser control never enables unauthenticated global remote debugging.
- Computer Use never reports OS-ready until the permission preflight passes.

## Remaining evidence limits

- Alpha-9 was not present in this checkout, installed applications, git history, or local performance
  worktree. Its existence elsewhere is unverified.
- The earlier live UAM and Codex process samples had different uptimes and workloads. They are
  directional evidence only.
- Windows renderer and process measurements still need to be run when implementation reaches the
  release-candidate stage.

## Frontier

`ROUTE_CLEAR`

The next action is the version-source commit, followed by review and integration of the existing
measured performance commits. No further discovery is required before implementation begins.
