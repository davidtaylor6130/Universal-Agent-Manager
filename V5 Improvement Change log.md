# V5 Improvement Change log

This is the cumulative four-day pass included in alpha82, covering this chat, the earlier subagents and the separately merged feature agent—not just the changes since alpha80.

Packaged source: `3ed93c813b66611e72167dae505362234b7dccf6`.

## Agent maintenance instructions

This tracked root document is the V5 master change log. Every agent making a V5 feature, graphical, performance, code-quality or bug-fix change must update this log in the same commit as its implementation.

- Read the existing rows first and update a matching entry when extending or correcting the same change; append a new row for distinct work.
- Keep the columns **Group | Type | Change | Summary | Status** and reuse an existing group where appropriate.
- Use only **Performance**, **Graphical**, **Code Only**, **Bugfix** or **Misc** as the Type.
- Limit Change names to five words and write Summary as one factual sentence describing the resulting behavior.
- Describe actual implementation separately from acceptance, and explicitly label blocked, partial or unverified work under the remaining-work groups.
- Preserve existing history and verification limits, resolve overlapping agent edits without dropping rows, and do not replace this document with a log in ignored working notes.
- Keep detailed test receipts in `.UAM/AINotes/` and avoid claiming live or platform acceptance from source checks alone.

- Status flags are cumulative: use **Un-Reviewed** for work without documented review and **Not User Tested** until David has explicitly confirmed testing the specific change.
- Mark every React/frontend UI or UX code change **Claude Review**, regardless of Type or which agent implemented it; this also applies to affected UI workflows awaiting Claude validation.
- Remove **Claude Review** only after recording a completed Claude review and resolving its findings; completed automation or a different agent's review does not satisfy that flag.
- Set **Complete** only when David explicitly approves that specific change as all good, all required review is satisfied, and the approval reference is recorded with the entry or linked evidence.
- **Claude UI Reviewed** records that Claude completed the visual, UX and motion review of that change's interface and resolved its findings (or confirmed it has no visual surface); it does not cover code correctness, which remains under **Un-Reviewed** until a code review is recorded.
- Never infer user approval from passed tests, successful builds, installation, a restart, silence or a general request to continue.

## Changes

Initial statuses are deliberately unsigned: no entry has been marked Complete based on build or automated test results, and earlier user comments are not treated as blanket approval of these cumulative changes.


| Group | Type | Change | Summary | Status |
|---|---|---|---|---|
| Providers and review | Bugfix | Review Queue Task Ownership | Completed review tasks leave the vector before advancing queues, retaining newly started workers for the next poll. | Claude Review; Un-Reviewed; Not User Tested |
| Performance | Bugfix | Failed Save Admission Revocation | Failed save receipts retire undelivered input and record its failure before reporting rejection. | Claude Review; Un-Reviewed; Not User Tested |
| Performance | Bugfix | Asynchronous Save State Feedback | Save receipts report changed processing and errors to the conditional frontend state update. | Claude Review; Un-Reviewed; Not User Tested |
| Providers and review | Code Only | Host Scoped Provider Availability | Persisted native provider switches block new host-scoped work while preserving accepted queues and existing runtime attachment. | Partial Implementation; Un-Reviewed; Claude Review; Not User Tested |
| Providers and MCP | Code Only | Explicit Configuration Cache Refresh | OpenCode configuration remains cached until restart or explicit background refresh, retaining valid cached data on refresh failure. | Un-Reviewed; Claude Review; Not User Tested |
| Core correctness | Bugfix | Recovery State Reconciliation | Recovery-only changes now reach the frontend without requiring another chat field to change. | Claude UI Reviewed; Not User Tested |
| Providers and subagents | Bugfix | Claude Parent Envelope Deduplication | Repeated native parent assistant envelope IDs no longer duplicate narration while distinct IDs and identity-free output remain separate. | Un-Reviewed; Claude Review; Not User Tested |
| Providers and review | Misc | Claude Native Permission Translation | Structured Claude sessions use native auto/edit permissions while preserving Plan, avoiding duplicate parent review and retaining native residual prompts for user decisions. | Un-Reviewed; Claude Review; Not User Tested |
| Providers and review | Misc | Codex Native Permission Translation | Codex thread and turn requests use native reviewer settings with read-only review bounds, scoped effective-state tracking and user-owned residual prompts. | Un-Reviewed; Claude Review; Not User Tested |
| Providers and review | Bugfix | Native Reviewer Warning Diagnostics | Codex native warnings become thread-scoped diagnostics with sensitive content withheld and unchanged processing or permission state. | Un-Reviewed; Claude Review; Not User Tested |
| Core correctness | Bugfix | Diagnostic UTF8 Boundary Safety | Diagnostic caps preserve complete UTF8 code points so clipped provider text remains valid for strict JSON serialization. | Un-Reviewed; Claude Review; Not User Tested |
| Providers and review | Bugfix | Complete Permission Review Input | Parent review preserves complete normalized and native request data and leaves oversized, malformed or sensitive input for manual decisions. | Un-Reviewed; Claude Review; Not User Tested |
| Core correctness | Bugfix | Memory Request Ownership | Older memory requests can no longer overwrite the currently selected scope or its errors. | Claude UI Reviewed; Not User Tested |
| Core correctness | Bugfix | Pending Attachment Preservation | Send completion preserves skills added or reattached while the request was pending. | Claude UI Reviewed; Not User Tested |
| Core correctness | Bugfix | Cancellation Persistence | Failed cancellation saves retain cancellation intent and prevent queued runs from relaunching. | Un-Reviewed; Not User Tested |
| Core correctness | Code Only | Shared Provider Catalogue | Provider choices use shared metadata instead of duplicated provider lists. | Claude UI Reviewed; Not User Tested |
| Core correctness | Code Only | Tooltip Ownership Cleanup | Redundant tooltip timing ownership was removed. | Claude UI Reviewed; Not User Tested |
| Composer and queue | Graphical | Composer Message Editing | Editing a sent message now uses the existing composer instead of a separate editing textarea. | Claude UI Reviewed; Not User Tested |
| Composer and queue | Bugfix | Edit Draft Restoration | Cancelling an edit restores the unsent draft and preserves its attachments. | Claude UI Reviewed; Not User Tested |
| Composer and queue | Bugfix | Editable Pending Composer | The composer remains editable while prompt admission is pending without allowing duplicate submissions. | Claude UI Reviewed; Not User Tested |
| Composer and queue | Bugfix | Separate FIFO Turns | Queued prompts dispatch as separate durable turns instead of being concatenated into one prompt. | Un-Reviewed; Not User Tested |
| Composer and queue | Graphical | Queue Management Controls | Guarded controls expose queue editing, reordering, removal and explicit resume. | Claude UI Reviewed; Not User Tested |
| Composer and queue | Bugfix | Queue Revision Guards | Stale queue edits and reorders are rejected without losing captured attachments or execution options. | Claude UI Reviewed; Not User Tested |
| Composer and queue | Bugfix | Restart Queue Holding | Restored queued intent remains held until explicitly resumed. | Claude UI Reviewed; Not User Tested |
| Composer and queue | Performance | Queue Persistence Journal | Ordinary queue changes use a bounded durable journal instead of rewriting the entire transcript. | Un-Reviewed; Not User Tested |
| Composer and queue | Bugfix | Queue Crash Recovery | Journal generations and consumption markers prevent stale saves or crashes from resurrecting dispatched input. | Un-Reviewed; Not User Tested |
| Composer and queue | Bugfix | Queue Attachment Editing | Queued prompts support attachment removal and attachment-only edits while preserving retained attachments. | Claude UI Reviewed; Not User Tested |
| Composer and queue | Graphical | Queued Attachment Names | Queued messages display their captured file names. | Claude UI Reviewed; Not User Tested |
| Composer and queue | Bugfix | Secondary Control Submission | Secondary composer buttons no longer accidentally submit the draft. | Claude UI Reviewed; Not User Tested |
| Composer and queue | Bugfix | Editor Keyboard Ownership | Queue-editor shortcuts remain inside the editor rather than triggering composer actions. | Claude UI Reviewed; Not User Tested |
| Composer and queue | Bugfix | Goal Request Ownership | Goal composer requests retain correct ownership through React effect replay. | Claude UI Reviewed; Not User Tested |
| Composer and queue | Graphical | Composer Goal Controls | Goal actions were moved into the composer controls with guarded asynchronous ownership. | Claude UI Reviewed; Not User Tested |
| Composer and queue | Bugfix | Goal Popup Escape | Escape closes the goals popup from the composer. | Claude UI Reviewed; Not User Tested |
| Composer and queue | Bugfix | Checkpoint Queue Preservation | Checkpoint finalization cannot overwrite newer queued intent or a replacement turn. | Un-Reviewed; Not User Tested |
| Composer and queue | Performance | Prompt Settings Capture | Provider prompt construction captures required settings without copying the complete loaded transcript. | Un-Reviewed; Not User Tested |
| Composer and queue | Performance | Redundant Delivery Saves | Unchanged local delivery state is no longer rewritten after its durable delivery-intent save. | Un-Reviewed; Not User Tested |
| History and performance | Performance | Retry Selector Caching | Composer typing avoids repeated retry-history scans. | Claude UI Reviewed; Not User Tested |
| History and performance | Performance | Timeline Render Reuse | Unchanged transcript rendering is reused while the composer updates. | Claude UI Reviewed; Not User Tested |
| History and performance | Performance | Timestamp Render Reuse | Unchanged turn timestamps are no longer repeatedly formatted during typing and streaming. | Claude UI Reviewed; Not User Tested |
| History and performance | Performance | Incremental History Digests | Exact unchanged message prefixes reuse their hashes when history appends or shrinks. | Un-Reviewed; Not User Tested |
| History and performance | Bugfix | Earlier History Corrections | Digest checks still detect earlier content, thought and tool-result changes. | Un-Reviewed; Not User Tested |
| History and performance | Performance | Bounded Frontend Summaries | Catalogue updates avoid transporting complete transcript bodies unnecessarily. | Un-Reviewed; Not User Tested |
| History and performance | Performance | Cold Startup Histories | Startup leaves chat histories unloaded until a bounded page is requested. | Un-Reviewed; Not User Tested |
| History and performance | Graphical | Desktop History Paging | Large transcripts load in pages while retaining absolute message positions. | Claude UI Reviewed; Not User Tested |
| History and performance | Bugfix | Detached History Pages | Paging an unloaded chat no longer unnecessarily hydrates its complete native state. | Un-Reviewed; Not User Tested |
| History and performance | Bugfix | Paged Native Tokens | Native history identity tokens survive paged loading. | Claude UI Reviewed; Not User Tested |
| History and performance | Performance | Recent History Cache | Validated recent-message caches avoid repeated complete reads of large saved histories. | Un-Reviewed; Not User Tested |
| History and performance | Performance | Bounded Complete Tails | Oversized recent pages cache fewer complete messages rather than abandoning caching or truncating messages. | Un-Reviewed; Not User Tested |
| History and performance | Performance | Priority History Loading | Opening chat history no longer waits behind background VCS subprocess checks. | Un-Reviewed; Not User Tested |
| History and performance | Performance | Priority Cold Sending | Cold send preparation runs outside the background VCS queue. | Un-Reviewed; Not User Tested |
| History and performance | Performance | Background Buffer Disposal | Large detached history buffers are released away from the GUI thread. | Un-Reviewed; Not User Tested |
| History and performance | Performance | Background Cache Publication | Revision-guarded history cache publication runs on the background worker. | Un-Reviewed; Not User Tested |
| History and performance | Performance | Durable Transcript Retention | Only saved inactive histories are released while visible, active and unsaved work stays protected. | Un-Reviewed; Not User Tested |
| History and performance | Performance | Paused History Release | Durable hidden histories can be released for paused and blocked goals. | Un-Reviewed; Not User Tested |
| History and performance | Bugfix | Reselected History Preservation | Idle transitions cannot discard history that the user has reselected. | Claude UI Reviewed; Not User Tested |
| History and performance | Bugfix | Worktree History Preservation | Worktree operations preserve valid cached chat history. | Un-Reviewed; Not User Tested |
| History and performance | Bugfix | Missing Timestamp Hydration | Saved chats with missing timestamps hydrate correctly. | Un-Reviewed; Not User Tested |
| History and performance | Performance | Deferred Tool Loading | Deferred tool output loads on background workers. | Un-Reviewed; Not User Tested |
| History and performance | Performance | Bounded Tool Pages | Tool-output paging materializes only the requested bounded UTF-8 range. | Un-Reviewed; Not User Tested |
| History and performance | Performance | JSON Serialization Spans | Bulk string copying reduced the private large-save benchmark median from 835.7 to 498.5 milliseconds. | Un-Reviewed; Not User Tested |
| History and performance | Performance | JSON Parsing Spans | Bulk string parsing reduced the private large-parse benchmark median from 277.3 to 151.9 milliseconds. | Un-Reviewed; Not User Tested |
| History and performance | Performance | Workspace Resolution Reuse | State serialization reuses workspace resolution within each serialization pass. | Un-Reviewed; Not User Tested |
| History and performance | Performance | Background Workspace Probes | Workspace availability is probed on workers and published only to matching owners. | Un-Reviewed; Not User Tested |
| History and performance | Performance | Background Pending Saves | Ordinary pending saves prepare durable files on workers before guarded publication. | Un-Reviewed; Not User Tested |
| History and performance | Performance | Faster Selection Acknowledgement | Chat selection acknowledges before the heavier full-state publication. | Un-Reviewed; Not User Tested |
| History and performance | Performance | State Baseline Reuse | Patch-baseline resets reuse existing complete state summaries. | Un-Reviewed; Not User Tested |
| History and performance | Performance | Idle Terminal Scan Avoidance | Idle terminal polling skips unnecessary catalogue scans. | Un-Reviewed; Not User Tested |
| History and performance | Misc | Polling Stage Diagnostics | Slow Computer Use, shell, workspace and UI polling stages now have scoped diagnostic timing. | Un-Reviewed; Not User Tested |
| Memory processing | Performance | Minimal Extraction Snapshots | Memory extraction captures required roles and text without copying unused tools, thoughts or attachments. | Un-Reviewed; Not User Tested |
| Memory processing | Performance | Requested Range Capture | Memory workers capture only the requested unprocessed transcript range. | Un-Reviewed; Not User Tested |
| Memory processing | Performance | Durable Progress Journal | Memory progress updates avoid rewriting otherwise unchanged transcripts. | Un-Reviewed; Not User Tested |
| Memory processing | Performance | Background Entry Preparation | Memory entries are prepared on workers before guarded publication. | Un-Reviewed; Not User Tested |
| Memory processing | Performance | Background First Recall | First-send memory recall runs under cancellable worker ownership. | Un-Reviewed; Not User Tested |
| Memory processing | Performance | Bounded Recall Previews | Recall previews read bounded content instead of complete memory bodies. | Un-Reviewed; Not User Tested |
| Memory processing | Performance | Recall Budget Cutoff | Ordinary recall scanning stops when its prompt budget is filled. | Un-Reviewed; Not User Tested |
| Memory processing | Performance | Lightweight Memory Counts | Memory counts and timestamps avoid loading complete entry bodies. | Un-Reviewed; Not User Tested |
| Memory processing | Performance | Memory Title Index | Memory titles are indexed once per completed extraction. | Un-Reviewed; Not User Tested |
| Memory processing | Performance | Redundant Layout Checks | Repeated memory layout checks and title-body reads were removed. | Un-Reviewed; Not User Tested |
| Memory processing | Performance | Bounded Signal Scanning | Automatic admission scans avoid transcript-sized scratch allocations while retaining the existing policy. | Un-Reviewed; Not User Tested |
| Memory processing | Performance | Permissive Admission Shortcut | Permissive memory admission avoids unnecessary transcript folding. | Un-Reviewed; Not User Tested |
| Memory processing | Performance | Idle Classification Deferral | Accepted idle waits no longer repeatedly classify the same memory input. | Un-Reviewed; Not User Tested |
| Memory processing | Bugfix | Pending Save Coordination | Memory scans and progress publication wait for pending transcript publication to finish. | Un-Reviewed; Not User Tested |
| Memory processing | Bugfix | Data Root Ownership | Memory completion is rejected after its original data root changes. | Un-Reviewed; Not User Tested |
| Memory processing | Bugfix | Workspace Ownership Guards | Memory results cannot publish after their workspace ownership changes. | Un-Reviewed; Not User Tested |
| Memory processing | Bugfix | Remote Memory Cancellation | Stale remote memory preparation is cancelled before startup waits or publication. | Un-Reviewed; Not User Tested |
| Memory processing | Bugfix | Gemini History Protection | Worker cleanup preserves Gemini history that it does not own. | Un-Reviewed; Not User Tested |
| Memory processing | Bugfix | Gemini Worker Policies | Generated deny-all Gemini policies now match the installed policy schema. | Un-Reviewed; Not User Tested |
| Memory processing | Bugfix | OpenCode Worker Isolation | OpenCode text workers use isolated agent settings rather than inheriting unsuitable workspace agents. | Un-Reviewed; Not User Tested |
| Memory processing | Bugfix | Remote Path Remapping | Shared path remapping handles roots, descendants and platform separators without prefix collisions. | Un-Reviewed; Not User Tested |
| Providers and MCP | Bugfix | Scoped MCP Configuration | Provider and host MCP overrides are honoured during session launch. | Un-Reviewed; Not User Tested |
| Providers and MCP | Graphical | Consolidated Provider Settings | Provider settings expose scoped switches and distinguish configured setup from verified connection status. | Claude UI Reviewed; Not User Tested |
| Providers and MCP | Bugfix | MCP Opt Out | Explicit opt-out suppresses warnings and prevents launch overrides from falsely reporting a connection. | Claude UI Reviewed; Not User Tested |
| Providers and MCP | Bugfix | MCP Warning Ownership | Warning dismissals reset across foreground and background runtime restarts. | Claude UI Reviewed; Not User Tested |
| Providers and MCP | Bugfix | Unknown Connection States | Unknown provider connections and startup failures remain distinguishable from successful connections. | Claude UI Reviewed; Not User Tested |
| Providers and MCP | Bugfix | MCP Import Preservation | MCP import preserves existing settings and reports actionable source errors. | Claude UI Reviewed; Not User Tested |
| Providers and MCP | Bugfix | MCP Editor Identity | MCP editing retains identity and rejects reserved external definitions. | Claude UI Reviewed; Not User Tested |
| Providers and MCP | Graphical | Multiline MCP Editing | Imported MCP configuration remains editable in a multiline editor. | Claude UI Reviewed; Not User Tested |
| Providers and MCP | Bugfix | OpenCode Import Warnings | OpenCode import warnings remain visible through review and save. | Claude UI Reviewed; Not User Tested |
| Providers and MCP | Misc | Native MCP Answer Verification | Question-answer delivery was exercised through the native runner relay. | Un-Reviewed; Not User Tested |
| Providers and MCP | Bugfix | Late Permission Cancellation | Permission requests arriving during cancellation receive a cancelled response. | Un-Reviewed; Not User Tested |
| Providers and MCP | Bugfix | Model Catalogue Ownership | Model catalogues are bound to the provider connection that produced them. | Un-Reviewed; Not User Tested |
| Providers and MCP | Bugfix | Disabled Model Enforcement | Disabled models are rejected in chat, worker and reviewer selectors. | Claude UI Reviewed; Not User Tested |
| Providers and MCP | Performance | Cached Model Discovery | Cached model discovery avoids interrupting existing conversations. | Claude UI Reviewed; Not User Tested |
| Providers and MCP | Bugfix | Startup Model Isolation | Startup model discovery does not take over user conversations. | Un-Reviewed; Not User Tested |
| Providers and MCP | Bugfix | Codex Instruction Transport | Complete Codex instructions travel through native session setup instead of exceeding OS command-line limits. | Un-Reviewed; Not User Tested |
| Providers and MCP | Bugfix | Duplicate Workspace Instructions | Codex startup avoids duplicating workspace instructions and preserves setup errors. | Un-Reviewed; Not User Tested |
| Providers and MCP | Bugfix | Completed Codex Decisions | Suggestions, memory extraction and permission reviews use completed Codex output. | Un-Reviewed; Not User Tested |
| Providers and MCP | Bugfix | Empty Remote Arguments | Remote launch transport preserves intentionally empty provider option values. | Un-Reviewed; Not User Tested |
| Providers and MCP | Bugfix | POSIX Argument Preservation | Quoted backslashes and line continuations retain their intended values. | Un-Reviewed; Not User Tested |
| Providers and MCP | Bugfix | Remote Workspace Backslashes | Literal backslashes in POSIX workspace paths survive model and provider operations. | Un-Reviewed; Not User Tested |
| Providers and MCP | Misc | Antigravity Terminal Support | Antigravity terminal launch, saved-ID resume, installation and authentication were added and verified. | Claude UI Reviewed; Not User Tested |
| Providers and MCP | Misc | Antigravity Structured Support | Antigravity native streaming uses isolated configuration with conversation identity and resume handling. | Claude UI Reviewed; Not User Tested |
| Providers and MCP | Bugfix | Antigravity Replay Filtering | Resumed Antigravity answers are preserved when they repeat earlier text. | Un-Reviewed; Not User Tested |
| Claude and orchestration | Bugfix | Claude Tool History | Claude tool history and native task ownership remain attached to the correct conversation. | Claude UI Reviewed; Not User Tested |
| Claude and orchestration | Bugfix | Native Child Settlement | Child status settles after confirmed exits while unconfirmed remote exits retain ownership. | Claude UI Reviewed; Not User Tested |
| Claude and orchestration | Bugfix | Stop Output Preservation | Stop drains final output before settling children or closing process handles. | Un-Reviewed; Not User Tested |
| Claude and orchestration | Bugfix | Shutdown Queue Preservation | Application shutdown preserves held future prompts. | Un-Reviewed; Not User Tested |
| Claude and orchestration | Bugfix | Replacement Launch Protection | Replacement launches cannot erase pending child ownership before termination settles. | Un-Reviewed; Not User Tested |
| Claude and orchestration | Bugfix | Idle Proxy Child Recovery | Live Claude background tasks remain owned during idle proxy recovery. | Un-Reviewed; Not User Tested |
| Claude and orchestration | Performance | Deferred Claude Activity Saves | Streaming child updates defer routine saves while retaining terminal durability barriers. | Un-Reviewed; Not User Tested |
| Claude and orchestration | Performance | Deferred Codex Activity Saves | Codex streaming activity uses guarded background persistence. | Un-Reviewed; Not User Tested |
| Claude and orchestration | Bugfix | Durable Quota Continuation | Structured five-hour quota failures persist a continuation owner with a trustworthy reset time. | Un-Reviewed; Not User Tested |
| Claude and orchestration | Bugfix | Safe Native Continuation | Quota recovery sends one new instruction in the same native session without replaying acknowledged prompts or tools. | Un-Reviewed; Not User Tested |
| Claude and orchestration | Bugfix | Ambiguous Recovery Holding | Uncertain deliveries, repeated quota failures and unresolved tool work remain held rather than automatically replayed. | Un-Reviewed; Not User Tested |
| Claude and orchestration | Bugfix | Continuation Context Binding | Quota continuation retains its accepted agent definition, execution context and future queue. | Un-Reviewed; Not User Tested |
| Claude and orchestration | Performance | Cancellable Cold Startup | Cold prompt history preparation runs after cancellable admission. | Un-Reviewed; Not User Tested |
| Claude and orchestration | Performance | Cancellable Provider Handoff | Cold provider handoff history is prepared on workers with identity and storage validation. | Un-Reviewed; Not User Tested |
| Claude and orchestration | Performance | Cancellable Child Preparation | Native-child history preparation avoids synchronous cold reads and repair writes. | Un-Reviewed; Not User Tested |
| Claude and orchestration | Performance | Cancellable Branch Preparation | Cold branch and edit preparation retains ownership through Stop and stale completions. | Un-Reviewed; Not User Tested |
| Claude and orchestration | Bugfix | Managed Resume History | Managed-run resume preserves cold history and native identity. | Un-Reviewed; Not User Tested |
| Claude and orchestration | Performance | Managed Transcript Reads | Settled managed transcripts are read outside the GUI thread. | Un-Reviewed; Not User Tested |
| Claude and orchestration | Bugfix | Delegated Agent Definitions | Delegated worktrees retain the parent workspace’s agent definitions. | Un-Reviewed; Not User Tested |
| Claude and orchestration | Bugfix | Child Model Inheritance | Model options inherit only when parent and child use the same provider. | Un-Reviewed; Not User Tested |
| Claude and orchestration | Graphical | Eligible Subagent Providers | Managed subagent choices expose only scoped eligible providers. | Claude UI Reviewed; Not User Tested |
| Review and checkpoints | Bugfix | Unfinished Review Gating | Review changes stays hidden while parent, child, goal or history-loading work remains unfinished. | Claude UI Reviewed; Not User Tested |
| Review and checkpoints | Bugfix | Stale Diff Ownership | Old diff metadata cannot be paired with a newly selected chat. | Claude UI Reviewed; Not User Tested |
| Review and checkpoints | Misc | Recorded Edit Receipts | Immutable provider-reported edit receipts support chat-specific review without attributing unrelated workspace changes. | Un-Reviewed; Not User Tested |
| Review and checkpoints | Graphical | Recorded Patch Review | Recorded chat edits have review controls that preserve newer commit drafts. | Claude UI Reviewed; Not User Tested |
| Review and checkpoints | Bugfix | User Staging Protection | Automatic checkpoints preserve user staging instead of consuming files staged during agent work. | Un-Reviewed; Not User Tested |
| Review and checkpoints | Bugfix | Cold Checkpoint Rollback | Checkpointed turns can be rolled back from cold saved chats. | Un-Reviewed; Not User Tested |
| Review and checkpoints | Graphical | Bounded Diff Rendering | Large diffs render within bounds with source line gutters. | Claude UI Reviewed; Not User Tested |
| Review and checkpoints | Graphical | Virtualized Split Diffs | The feature agent added a virtualized split layout with a remembered view preference. | Claude UI Reviewed; Not User Tested |
| Review and checkpoints | Graphical | Scoped File Inspection | The feature agent added bounded source and diff navigation scoped to the selected workspace. | Claude UI Reviewed; Not User Tested |
| Review and checkpoints | Misc | Git SVN Switches | Git and SVN integration can be enabled or disabled persistently. | Claude UI Reviewed; Not User Tested |
| Workspaces and deletion | Misc | Automatic Worktree Isolation | Readable isolated worktrees are prepared before first provider dispatch. | Claude UI Reviewed; Not User Tested |
| Workspaces and deletion | Bugfix | Worktree Collision Detection | Collision checks work across Windows shell formatting and existing branch names. | Un-Reviewed; Not User Tested |
| Workspaces and deletion | Bugfix | UTF8 Worktree Paths | Readable worktree paths preserve UTF-8 names. | Un-Reviewed; Not User Tested |
| Workspaces and deletion | Bugfix | Canonical Workspace Ownership | Duplicate canonical directory owners are rejected. | Un-Reviewed; Not User Tested |
| Workspaces and deletion | Graphical | Original Workspace Labels | Worktree rows identify their original workspace. | Claude UI Reviewed; Not User Tested |
| Workspaces and deletion | Performance | Branch Snapshot Reduction | Branch creation avoids copying unnecessary full source transcripts. | Un-Reviewed; Not User Tested |
| Workspaces and deletion | Performance | Background Cold Deletion | Cold deletion preparation and staging run outside the GUI thread. | Un-Reviewed; Not User Tested |
| Workspaces and deletion | Bugfix | Shared Native Ownership | Deletion preserves native history still owned by surviving chat aliases. | Un-Reviewed; Not User Tested |
| Workspaces and deletion | Bugfix | Durable Deletion Recovery | Shared ownership protection survives interrupted staging and commit recovery. | Un-Reviewed; Not User Tested |
| Workspaces and deletion | Bugfix | Deletion Failure Guards | Backup collisions, identity collisions and pending imports cannot silently weaken deletion ownership. | Un-Reviewed; Not User Tested |
| Workspaces and deletion | Bugfix | Selected Workspace Deletion | Context-menu deletion respects the selected workspace scope. | Claude UI Reviewed; Not User Tested |
| Workspaces and deletion | Performance | Deletion Path Caching | Canonical ownership normalization is reused within deletion preparation. | Un-Reviewed; Not User Tested |
| Workspaces and deletion | Performance | Survivor Buffer Preservation | Folder commits avoid copying unrelated surviving transcript bodies. | Un-Reviewed; Not User Tested |
| Workspaces and deletion | Graphical | Deletion Progress Feedback | Workspace deletion exposes progress and useful native failure details. | Claude UI Reviewed; Not User Tested |
| Workspaces and deletion | Misc | Orphan Worktree Inspection | Old owned worktrees are inspected before retention cleanup. | Un-Reviewed; Not User Tested |
| Workspaces and deletion | Bugfix | Leased Worktree Cleanup | Orphan worktree cleanup respects workspace leases and stored agent references. | Un-Reviewed; Not User Tested |
| Import and attachments | Graphical | Native Import Review | Cached startup histories can be reviewed and selectively imported. | Claude UI Reviewed; Not User Tested |
| Import and attachments | Graphical | Scoped Import Selection | Shift selection and importing all native-history pages remain scoped to their workspace. | Claude UI Reviewed; Not User Tested |
| Import and attachments | Bugfix | Cross Workspace Import Guards | Import decisions cannot publish into a different workspace. | Claude UI Reviewed; Not User Tested |
| Import and attachments | Bugfix | Native Duplicate Prevention | Legacy remote folder aliases cannot create duplicate native imports. | Un-Reviewed; Not User Tested |
| Import and attachments | Bugfix | Side Chat Retention | Import retains side-chat transcripts without assuming source-cleanup authority. | Un-Reviewed; Not User Tested |
| Import and attachments | Bugfix | Discarded Import Reporting | Selected histories discarded before publication are reported rather than silently lost. | Claude UI Reviewed; Not User Tested |
| Import and attachments | Bugfix | Import Request Recovery | Stale request guards are released through effect replay and completed review ownership. | Claude UI Reviewed; Not User Tested |
| Import and attachments | Graphical | Lazy Image Previews | Saved image previews load lazily using persisted attachment identity. | Claude UI Reviewed; Not User Tested |
| Import and attachments | Performance | Bounded Image Cache | Native saved-image previews use bounded caching. | Un-Reviewed; Not User Tested |
| Import and attachments | Performance | Cold Attachment Previews | Saved attachments can be previewed without hydrating the complete chat. | Un-Reviewed; Not User Tested |
| Import and attachments | Performance | Background Claude Images | Claude image delivery is prepared asynchronously within transport limits. | Un-Reviewed; Not User Tested |
| Import and attachments | Graphical | Image Collection Display | Image collections and attachment-retention choices are displayed in chat editing. | Claude UI Reviewed; Not User Tested |
| Import and attachments | Bugfix | Attachment Path Labels | Image fallback names and full-path tooltips retain correct attachment identity. | Un-Reviewed; Not User Tested |
| Navigation and interface | Graphical | Active Chat Pinning | Running chats support context-menu pinning without changing the selection. | Claude UI Reviewed; Not User Tested |
| Navigation and interface | Graphical | Next Attention Navigation | Navigation can select the next chat needing an answer, approval or failure response. | Claude UI Reviewed; Not User Tested |
| Navigation and interface | Graphical | Organization Undo | Guarded reversible collection changes are exposed through undo controls. | Claude UI Reviewed; Not User Tested |
| Navigation and interface | Bugfix | Delayed Rename Preservation | Delayed collection rename replies cannot overwrite newer names. | Claude UI Reviewed; Not User Tested |
| Navigation and interface | Graphical | Bounded Conversation Search | Search returns bounded content hits with guarded transcript provenance and message navigation. | Claude UI Reviewed; Not User Tested |
| Navigation and interface | Performance | Background Search Loading | Search workers load one transcript at a time rather than copying the entire catalogue. | Un-Reviewed; Not User Tested |
| Navigation and interface | Graphical | Workspace Picker Search | The new-chat picker searches grouped workspaces. | Claude UI Reviewed; Not User Tested |
| Navigation and interface | Graphical | Composer Focus Shortcut | A scoped shortcut focuses the active composer or terminal. | Claude UI Reviewed; Not User Tested |
| Navigation and interface | Bugfix | Modal Focus Ownership | Only the top visible modal owns keyboard handling and focus restoration. | Claude UI Reviewed; Not User Tested |
| Navigation and interface | Bugfix | Hidden Modal Exclusion | Hidden or closed dialogs no longer steal chat-input focus. | Claude UI Reviewed; Not User Tested |
| Navigation and interface | Bugfix | Branch Family Selection | Branch families restore their last successfully selected chat. | Claude UI Reviewed; Not User Tested |
| Navigation and interface | Graphical | Persistent Chat Snooze | The feature agent added persisted snoozing with guarded background metadata saves. | Claude UI Reviewed; Not User Tested |
| Navigation and interface | Graphical | Chat Title Regeneration | Chat titles can be regenerated on guarded workers, including imported transcripts. | Claude UI Reviewed; Not User Tested |
| Navigation and interface | Graphical | Chat Copy Actions | Sidebar menus expose chat-name and chat-ID copying. | Claude UI Reviewed; Not User Tested |
| Navigation and interface | Graphical | Message Position Navigation | The feature agent added navigation through large histories by absolute message position. | Claude UI Reviewed; Not User Tested |
| Navigation and interface | Graphical | User Message Rail | The message rail targets user messages with corrected spacing, hover areas and final-marker alignment. | Claude UI Reviewed; Not User Tested |
| Navigation and interface | Graphical | Searchable Model Picker | The feature agent added keyboard-accessible model search, a provider sidebar and favourites. | Claude UI Reviewed; Not User Tested |
| Navigation and interface | Graphical | Version Control Settings | Persisted Git and SVN switches independently enable or disable their supported workflows. | Claude UI Reviewed; Not User Tested |
| Navigation and interface | Graphical | Typography Settings | The feature agent added persisted typography controls and live terminal-font updates. | Claude UI Reviewed; Not User Tested |
| Navigation and interface | Graphical | Parent Waiting Feedback | Chat activity distinguishes a parent waiting for subagents and subagent calls display their latest reported activity. | Claude UI Reviewed; Not User Tested |
| Navigation and interface | Graphical | Native Compaction Feedback | Native compaction displays its in-progress state and available metrics. | Claude UI Reviewed; Not User Tested |
| Navigation and interface | Graphical | Working Indicator Motion | The blue working indicator has reduced-motion support without implying that overall animation smoothness is accepted. | Claude UI Reviewed; Not User Tested |
| Navigation and interface | Graphical | Provider Branding | Official provider marks and unknown-provider fallbacks were aligned across the interface. | Claude UI Reviewed; Not User Tested |
| Navigation and interface | Graphical | Main Application Icon | The selected full-tile app icon and accepted CEF title-bar icon are packaged. | Claude Review; Not User Tested |
| Navigation and interface | Graphical | Active Host Labels | Active chat rows display their owning host. | Claude UI Reviewed; Not User Tested |
| Navigation and interface | Graphical | Scoped Branch Labels | Activity branch labels remain bound to the correct workspace and load near the viewport. | Claude UI Reviewed; Not User Tested |
| Navigation and interface | Performance | Collapsed Row Unmounting | Collapsed sidebar sections stop mounting hidden chat rows and their background hooks. | Un-Reviewed; Claude UI Reviewed; Not User Tested |
| Navigation and interface | Bugfix | Loading Draft Preservation | History-loading and empty states preserve the composer draft. | Claude UI Reviewed; Not User Tested |
| Navigation and interface | Bugfix | Startup Recovery Visibility | Startup recovery remains hidden while the armed interface is loading. | Claude UI Reviewed; Not User Tested |
| Navigation and interface | Bugfix | Empty Assistant Rows | Contiguous assistant text no longer produces empty message-action rows. | Claude UI Reviewed; Not User Tested |
| Navigation and interface | Graphical | Actionable Workspace Errors | Workspace creation and missing-initial-commit failures use clearer scoped feedback. | Claude UI Reviewed; Not User Tested |
| Usage and accounting | Code Only | Lifetime Collection And Peers | Background collection backfills available histories and exports scoped lifetime receipts while legacy peers remain rolling-only. | Partial Implementation; Un-Reviewed; Claude Review; Not User Tested |
| Usage and accounting | Code Only | Lifetime Accounting Scope | Bounded lifetime receipt reads feed an explicitly scoped ledger codec while legacy rolling snapshots reject lifetime queries. | Partial Implementation; Un-Reviewed; Claude Review; Not User Tested |
| Usage and accounting | Code Only | Archived Accounting Receipts | Versioned SQLite archives old canonical receipts atomically while preserving mutable costs and linked Antigravity evidence. | Partial Implementation; Un-Reviewed; Claude Review; Not User Tested |
| Usage and accounting | Misc | Persistent Usage Ledger | A persistent background ledger caches the rolling 24 hours and older provider usage by UTC day. | Claude UI Reviewed; Not User Tested |
| Usage and accounting | Misc | Historical Usage Normalization | Historical usage is normalized and scanned incrementally. | Un-Reviewed; Not User Tested |
| Usage and accounting | Misc | Usage Dashboard | The feature agent added a usage workspace with explicit cost coverage and model prices. | Claude UI Reviewed; Not User Tested |
| Usage and accounting | Misc | Provider Period Filters | Usage totals and chart periods can be filtered by provider. | Claude UI Reviewed; Not User Tested |
| Usage and accounting | Misc | Cached Price Estimates | Cached reference prices support equivalent hosted Qwen estimates for local OpenCode models while preserving reported charges and unknown rates. | Claude UI Reviewed; Not User Tested |
| Usage and accounting | Bugfix | Deleted Usage Entries | Removed transcript files are dropped from the usage-history cache. | Un-Reviewed; Not User Tested |
| Usage and accounting | Misc | Remote Usage Export | SSH runners export cached usage accounting. | Un-Reviewed; Not User Tested |
| Usage and accounting | Bugfix | Machine Total Deduplication | Remote machine totals avoid duplicate accounting across copied histories. | Claude UI Reviewed; Not User Tested |
| Usage and accounting | Misc | Claude Live Usage | Scoped Claude snapshots expose live consumed-token accounting. | Claude UI Reviewed; Not User Tested |
| Usage and accounting | Misc | Gemini Saved Usage | Gemini saved-session usage is read through bounded snapshot scans. | Un-Reviewed; Not User Tested |
| Usage and accounting | Misc | Gemini Prompt Usage | Gemini prompt replies contribute validated observed input, cache and reasoning counts. | Claude UI Reviewed; Not User Tested |
| Usage and accounting | Misc | OpenCode SQLite Usage | OpenCode SQLite histories provide token accounting without counting copied fork usage twice. | Un-Reviewed; Not User Tested |
| Usage and accounting | Misc | OpenCode Prompt Usage | OpenCode prompt accounting includes cache and reasoning across tool steps. | Claude UI Reviewed; Not User Tested |
| Usage and accounting | Misc | Copilot Durable Usage | Copilot durable accounting avoids counting repeated snapshots. | Claude UI Reviewed; Not User Tested |
| Usage and accounting | Misc | Antigravity Usage Accounting | Antigravity usage participates in cached background accounting. | Un-Reviewed; Not User Tested |
| Usage and accounting | Misc | Provider Context Usage | Provider-neutral ACP context-usage updates are handled. | Claude UI Reviewed; Not User Tested |
| Usage and accounting | Bugfix | Fresh Login Allowances | Session account allowances clear appropriately after a fresh Codex login. | Claude UI Reviewed; Not User Tested |
| Remote execution | Bugfix | Installer Lock Inheritance | Helper descendants no longer inherit the Linux installer transaction lock. | Un-Reviewed; Not User Tested |
| Remote execution | Bugfix | Provider Blocking Diagnostics | Remote CLI update refusals identify validated blocking provider PIDs. | Un-Reviewed; Not User Tested |
| Remote execution | Bugfix | Update Activity Rechecks | CLI updates recheck current activity and respect the identified package-manager target. | Claude UI Reviewed; Not User Tested |
| Remote execution | Misc | Opt In Linux Elevation | Linux npm updates can use persisted per-host sudo selection instead of automatic elevation. | Claude UI Reviewed; Not User Tested |
| Remote execution | Bugfix | Host Option Preservation | Failed startup configuration no longer resets unrelated saved host options. | Un-Reviewed; Not User Tested |
| Remote execution | Bugfix | Selected Host Actions | Provider settings and updates stay scoped to the selected host. | Claude UI Reviewed; Not User Tested |
| Remote execution | Bugfix | Repeated Update Admission | Repeated clicks cannot launch overlapping manual provider updates. | Claude UI Reviewed; Not User Tested |
| Remote execution | Bugfix | Startup Error Preservation | Remote configuration and rejected-input errors remain visible before turn creation. | Un-Reviewed; Not User Tested |
| Remote execution | Bugfix | Accepted Input Acknowledgement | Accepted remote input remains acknowledged even when the provider subsequently exits. | Un-Reviewed; Not User Tested |
| Remote execution | Bugfix | Exit Interaction Settlement | Authenticated provider exits settle their remote interactions consistently. | Un-Reviewed; Not User Tested |
| Remote execution | Bugfix | Consistent Runner Snapshots | Completion and output snapshots remain consistent. | Un-Reviewed; Not User Tested |
| Remote execution | Bugfix | Final Output Draining | Remote Stop and shutdown retain final provider lines without artificial drain delays. | Un-Reviewed; Not User Tested |
| Remote execution | Bugfix | Stop Confirmation Timing | Remote Stop has time to confirm termination before timeout. | Un-Reviewed; Not User Tested |
| Remote execution | Bugfix | Missing Process Settlement | Confirmed missing remote processes settle through the shutdown path. | Un-Reviewed; Not User Tested |
| Remote execution | Bugfix | Replacement Output Isolation | Pending Stop output cannot contaminate a replacement session. | Un-Reviewed; Not User Tested |
| Remote execution | Bugfix | Reconnect Coalescing | Pending terminal reads coalesce duplicate reconnect requests. | Claude UI Reviewed; Not User Tested |
| Remote execution | Bugfix | Terminal Launch Identity | Reused terminal IDs reject output from older launches. | Claude UI Reviewed; Not User Tested |
| Remote execution | Bugfix | Remote Path Traversal | Rooted remote context and memory paths reject traversal outside their owner. | Un-Reviewed; Not User Tested |
| Remote execution | Misc | Atomic Host Admission | Explicit SSH pools use atomic admission and durable assignment ownership. | Un-Reviewed; Not User Tested |
| Remote execution | Misc | Routing Ownership Controls | Routing controls cover owner pinning, capacity waits, token refusal and authorized release. | Un-Reviewed; Not User Tested |
| Remote execution | Misc | Helper Source Manifests | Locally built helper artifacts include source fingerprints, protocol versions and checksums. | Un-Reviewed; Not User Tested |
| Remote execution | Misc | Forced Helper Repair | An earlier authorized repair replaced verified Linux and Windows helper trees and confirmed production handshakes. | Un-Reviewed; Not User Tested |
| Remote execution | Misc | Obsolete Helper Retirement | Later guarded cleanup retired 20 obsolete Linux services while preserving refusals and current ownership. | Un-Reviewed; Not User Tested |
| Remote execution | Misc | Two Host Provider Acceptance | Isolated Claude, Codex and OpenCode delivery checks passed on both Linux and Windows hosts. | Un-Reviewed; Not User Tested |
| Platform and diagnostics | Bugfix | Windows Long Path Writes | Windows atomic file operations and cleanup handle paths beyond legacy limits. | Un-Reviewed; Not User Tested |
| Platform and diagnostics | Bugfix | Windows Worktree Lengths | Windows worktree paths are bounded without changing branch names. | Un-Reviewed; Not User Tested |
| Platform and diagnostics | Bugfix | Windows History Paths | Long Claude history directories and runner output spools are supported. | Un-Reviewed; Not User Tested |
| Platform and diagnostics | Bugfix | Windows Runner Framing | Direct Windows runner binary framing preserves transport bytes. | Un-Reviewed; Not User Tested |
| Platform and diagnostics | Bugfix | Windows SSH State Cleanup | Replacing Windows stdio discards inherited OpenSSH state. | Un-Reviewed; Not User Tested |
| Platform and diagnostics | Bugfix | Windows Usage Reads | Windows usage reads and native acceptance fixtures were corrected for platform behavior. | Un-Reviewed; Not User Tested |
| Platform and diagnostics | Code Only | SQLite Helper Integration | SQLite is included in headless SSH helper builds. | Un-Reviewed; Not User Tested |
| Platform and diagnostics | Bugfix | MSVC SQLite Compatibility | SQLite integration builds correctly with MSVC. | Un-Reviewed; Not User Tested |
| Platform and diagnostics | Bugfix | ES2020 Sidebar Compatibility | Sidebar code remains compatible with the configured JavaScript target. | Claude UI Reviewed; Not User Tested |
| Platform and diagnostics | Bugfix | Fresh Ninja Builds | Fresh Ninja UI builds preserve the expected output layout. | Un-Reviewed; Not User Tested |
| Platform and diagnostics | Bugfix | macOS Quit Routing | Quit exits an owned picker modal loop before deferring CEF closure, including ignored cancellation and repeated requests. | Claude Review; Un-Reviewed; Not User Tested |
| Platform and diagnostics | Bugfix | Caps Lock Quit | Command-Q works with Caps Lock enabled. | Un-Reviewed; Not User Tested |
| Platform and diagnostics | Bugfix | Reaped Exit Status | macOS retains process exit status while remaining output is drained. | Un-Reviewed; Not User Tested |
| Platform and diagnostics | Performance | Background Diagnostic Logging | Diagnostic logging uses a bounded background worker. | Un-Reviewed; Not User Tested |
| Platform and diagnostics | Bugfix | Oversized Log Recovery | Startup safely recovers oversized diagnostic logs. | Un-Reviewed; Not User Tested |
| Platform and diagnostics | Bugfix | Special Log Path Rejection | Startup rejects special diagnostic paths before opening them. | Un-Reviewed; Not User Tested |
| Platform and diagnostics | Code Only | CEF Task Ownership | Metadata work remains on CEF-owned background tasks. | Un-Reviewed; Not User Tested |
| Platform and diagnostics | Performance | Background Computer Scans | Computer Use scan copies and cleanup run away from the GUI thread. | Un-Reviewed; Not User Tested |
| Platform and diagnostics | Performance | Indexed Scan Publication | Computer Use publication indexes chat identities instead of repeatedly searching the catalogue. | Un-Reviewed; Not User Tested |
| Platform and diagnostics | Performance | Changed Scan Entries | Unchanged Computer Use scans avoid unnecessary GUI-thread state publication. | Un-Reviewed; Not User Tested |
| Build and evidence | Code Only | Interaction Timing Tests | Real-clock tests cover settings, selection, composer input, local send feedback and bounded history code paths with agreed budgets while startup currently covers only the initial-state-summary service. | Un-Reviewed; Claude Review; Not User Tested |
| Build and evidence | Misc | Exact Test Inventories | Native compiled registrations and frontend inventories are checked against reported passing tests. | Un-Reviewed; Not User Tested |
| Build and evidence | Misc | Windows Native Verification | Earlier pinned source passed full MSVC builds, 1,481 native tests and all 18 Windows gates. | Un-Reviewed; Not User Tested |
| Build and evidence | Misc | Alpha82 Combined Verification | Alpha82 passed 1,540 native tests, 1,450 frontend tests and all 19 CTest gates. | Un-Reviewed; Not User Tested |
| Build and evidence | Misc | Matching Helper Packaging | Alpha82 includes four checksum-verified helpers sharing the packaged source fingerprint. | Un-Reviewed; Not User Tested |
| Build and evidence | Misc | Signed Package Verification | Alpha82 passed signing, asset, version, runner-validation and isolated macOS application checks. | Un-Reviewed; Not User Tested |
| Build and evidence | Misc | Main Application Replacement | Alpha82 replaced the main user Applications app after normal exit and preserved alpha80 for rollback. | Un-Reviewed; Not User Tested |
| Build and evidence | Misc | Audit Evidence Reconciliation | Historical audit claims were checked against current code rather than treated as current defects automatically. | Un-Reviewed; Not User Tested |
| Build and evidence | Misc | Copied Data Rehearsal | Duplicate histories and folder aliases were reviewed on copies without consolidating live user data. | Un-Reviewed; Not User Tested |
| Build and evidence | Misc | Performance Benchmark Evidence | Queue, memory, history, parsing and serialization measurements retain native-fixture limits rather than claiming live UI acceptance. | Un-Reviewed; Not User Tested |
| Remaining acceptance | Misc | Live Performance Acceptance | The reported opening target, periodic typing stalls and overall animation smoothness still need current live acceptance. | Claude Review; Not User Tested |
| Remaining acceptance | Misc | Remote Ownership Refusals | Some remote services still refuse guarded Stop because they report owned processes. | Un-Reviewed; Not User Tested |
| Remaining acceptance | Misc | Full Provider Acceptance | Remaining provider-policy, account, reviewer, physical-device and startup-session gates remain documented. | Un-Reviewed; Not User Tested |
| Permissions and review | Performance | Local Response Save Barriers | Measure local permission choices and question replies against250ms and assess their post-response timestamp save barriers. | Open; Claude Review; Un-Reviewed; Not User Tested | Unassigned |
| Permissions and review | Bugfix | Permission Review Receipt Persistence | Review and user-test durable owned verdicts after the initial permission waiting snapshot finishes. | Implementation Verified; Claude Review; Un-Reviewed; Not User Tested | chat-1791755529536-1b146e |
| Permissions and review | Performance | Permission Waiting Save Barrier | Review and user-test background waiting timestamps and prompt permission feedback under storage contention. | Implementation Verified; Claude Review; Un-Reviewed; Not User Tested | chat-1791755529536-1b146e |
| Permissions and review | Performance | Background Permission Feedback | Native permission waiting timestamps and owned reviewer results save in the background while isolated decisions retain one-time and manual-only gates. | Partial Implementation; Claude Review; Un-Reviewed; Not User Tested |
| Runtime durability | Bugfix | Queue Control Save Acknowledgement | Native queue-control handlers defer success until owned save completion and report failed persistence without interrupting delivered input. | Partial Implementation; Claude Review; Un-Reviewed; Not User Tested |
| Runtime durability | Performance | Idle Local Admission | Explicit idle local sends use complete background queue admission while compatibility checks or automatic routing are pending. | Partial Implementation; Claude Review; Un-Reviewed; Not User Tested |
| Runtime durability | Performance | Pending First Prompt Queue | Local follow-up input uses a complete background admission snapshot until the first provider request is delivered. | Partial Implementation; Claude Review; Un-Reviewed; Not User Tested |
| Runtime durability | Performance | Owned Same Session Retry | Healthy local retries stage the original message and owned save acknowledgements before provider delivery. | Partial Implementation; Claude Review; Un-Reviewed; Not User Tested |
| Runtime durability | Performance | Atomic Local Queue Dispatch | Ordinary local dispatch retires its queued entry with the unsent user message before asynchronous staged delivery. | Partial Implementation; Claude Review; Un-Reviewed; Not User Tested |
| Performance | Performance | Busy Queue Admission Durability | Ordinary local busy-turn queue admission saves asynchronously and holds failed intent without interrupting delivered output. | Partial Implementation; Claude Review; Un-Reviewed; Not User Tested |
| Remaining performance | Performance | Asynchronous Send Durability | Ordinary local admission, delivery, queue dispatch, retries and pending-first-prompt follow-ups save asynchronously while steering, remote and force barriers remain synchronous. | Partial Implementation; Claude Review; Un-Reviewed; Not User Tested |

## Incomplete Work

This is the shared outstanding-work register; the remaining-work rows above are summaries, and ownership is maintained here. These entries include unfinished implementation, unresolved incidents and acceptance gates, not claims that every item is a reproduced bug. Historical resource observations must be rechecked before treating them as current blockers.

### Discovery, ownership and handoff

- Every agent must read this section before starting work and add newly discovered fixes or missing acceptance here, updating an existing matching row instead of creating duplicates.
- Keep **Group | Type | Change | Summary | Status | Owner**, with the same Type, five-word Change and one-sentence Summary rules as the main table.
- New items start with **Owner = Unassigned**; before implementation or investigation, claim the row using the agent’s exact **UAM chat ID** and set **In Progress** while preserving pending review and user-test flags.
- Never invent an ID or use a display name, PID or branch name as the Owner; if the chat ID is unavailable, record that limitation and leave the item Unassigned until it can be obtained.
- An existing owner must be contacted through the available UAM communication tools before overlapping work or reassignment; if those tools are unavailable, record the coordination gate and avoid conflicting edits.
- Use **Open**, **In Progress** or **Blocked** for work state and record specific blockers or handoffs in linked `.UAM/AINotes/` evidence; release or transfer ownership explicitly, preserving the prior chat ID in the handoff record.
- When implementation finishes, update or append its main Changes entry and retain any remaining acceptance here; remove a fully resolved task only with a recorded disposition and owner handoff, never by silently dropping it.
- **Claude Review**, **Un-Reviewed** and **Not User Tested** remain acceptance flags; only David’s explicit approval of that specific change, with required review satisfied and evidence recorded, permits **Complete**.

| Group | Type | Change | Summary | Status | Owner |
|---|---|---|---|---|---|
| Platform and diagnostics | Bugfix | macOS Quit Routing | Review and user-test native picker Quit cancellation, including early and repeated requests. | Implementation Verified; Claude Review; Un-Reviewed; Not User Tested | chat-1791755529536-1b146e |
| Performance | Bugfix | Failed Save Admission Revocation | Review and user-test rejected admission and unsent retry intent after failed synchronous or background saves. | Implementation Verified; Claude Review; Un-Reviewed; Not User Tested | chat-1791755529536-1b146e |
| Performance | Bugfix | Asynchronous Save State Feedback | Review and user-test frontend error and processing updates after asynchronous save completion. | Implementation Verified; Claude Review; Un-Reviewed; Not User Tested | chat-1791755529536-1b146e |
| Runtime durability | Bugfix | Queue Control Save Acknowledgement | Review and user-test durable queue-control acknowledgements and failed storage while a delivered turn continues. | Implementation Verified; Claude Review; Un-Reviewed; Not User Tested | chat-1791755529536-1b146e |
| Runtime durability | Bugfix | Unadmitted Dispatch Marker | Review and user-test atomic local admission and queue retirement after crash or storage failure. | Implementation Verified; Claude Review; Un-Reviewed; Not User Tested | chat-1791755529536-1b146e |
| Performance | Performance | Asynchronous Send Durability | Move the remaining pre-dispatch durable save off the GUI thread while preserving admission, ordering and crash recovery. | In Progress; Claude Review; Un-Reviewed; Not User Tested | chat-1791755529536-1b146e |
| Performance | Performance | Large Chat Opening Acceptance | Measure live CEF opening of the reported huge chat against David’s one-to-two-second target, verify history-loading feedback and fix remaining bottlenecks. | In Progress; Claude Review; Not User Tested | chat-1791755529536-1b146e |
| Performance | Performance | Typing And Sending Acceptance | Measure native typing stalls, sending, scrolling and concurrent streaming in the current app and correct remaining interaction delays. | In Progress; Claude Review; Not User Tested | chat-1791755529536-1b146e |
| Graphical review | Graphical | Animation Smoothness Acceptance | Compare T3 motion quality and implement fluid physical transitions, including the settings selection moving between tabs, while respecting reduced motion and avoiding idle repaint loops. | Open; Claude Review; Not User Tested | Unassigned |
| Graphical review | Graphical | Claude Interface Review | Complete Claude review of every flagged React or UX change and resolve its findings before removing review flags. | Open; Claude Review; Not User Tested | Unassigned |
| Remote maintenance | Bugfix | Remote Ownership Refusals | Investigate helpers that still refuse guarded Stop and retire verified idle ownership without terminating unrelated live work. | In Progress; Un-Reviewed; Not User Tested | chat-1791755529536-1b146e |
| Remote maintenance | Misc | Current Helper Activation | Verify safe activation of the latest packaged helpers on both remote hosts and sending Codex skill prompts without the OS launch-limit failure after resolving ownership guards. | In Progress; Un-Reviewed; Not User Tested | chat-1791755529536-1b146e |
| Remote maintenance | Misc | Elevated Provider Update Acceptance | Verify saved per-host elevation choices update the identified provider installations without resetting settings or granting chat sessions privileges. | In Progress; Un-Reviewed; Not User Tested | chat-1791755529536-1b146e |
| Remote maintenance | Misc | Interactive Sudo Update Support | Resolve the unimplemented password-authenticated update path for hosts requiring interactive sudo without placing passwords in chat or settings. | In Progress; Un-Reviewed; Not User Tested | chat-1791755529536-1b146e |
| Remote startup | Misc | Runner Startup Acceptance | Verify real Linux user-session and Windows login startup behavior while preserving independently saved host options on failure. | In Progress; Un-Reviewed; Not User Tested | chat-1791755529536-1b146e |
| Remote routing | Misc | Concurrent Two Host Acceptance | Run the production-linked two-host lifecycle packet with independent review and verify pinning, token refusal, authorized release and scoped cleanup. | Blocked; Assigned for coordination; Un-Reviewed; Not User Tested | chat-1791755529536-1b146e |
| Remote evidence | Misc | Missing Incident Evidence | Keep the failed r4 run and missing historical startup or Stop traces explicit until equivalent fresh evidence can be captured. | Blocked; Assigned for coordination; Un-Reviewed; Not User Tested | chat-1791755529536-1b146e |
| Provider acceptance | Misc | Six Provider Live Matrix | Complete fresh authenticated acceptance for all six providers across applicable local and remote launch, send, resume, cancellation and token/cost accounting paths with unsupported usage explicit. | In Progress; Un-Reviewed; Not User Tested | chat-1791755529536-1b146e |
| Provider acceptance | Misc | Managed Action Acceptance | Verify real question, permission, model-setting and MCP invocation flows rather than treating tool discovery as successful execution. | In Progress; Claude Review; Not User Tested | chat-1791755529536-1b146e |
| Provider acceptance | Misc | Handoff And Review Acceptance | Verify current provider handoffs and recorded-edit review preserve the selected chat’s context and ownership in real sessions. | In Progress; Claude Review; Not User Tested | chat-1791755529536-1b146e |
| Provider acceptance | Misc | Queue Restart Acceptance | Verify queue editing, individual delivery and restart recovery through the current interface without replaying acknowledged work. | In Progress; Claude Review; Not User Tested | chat-1791755529536-1b146e |
| Provider acceptance | Misc | Claude Lifecycle Acceptance | Verify authenticated Claude child ownership, Stop, shutdown and Review changes suppression after the isolated lifecycle fixes. | In Progress; Claude Review; Not User Tested | chat-1791755529536-1b146e |
| Provider acceptance | Misc | Quota Continuation Acceptance | Verify trusted five-hour reset continuation in an authenticated native session without replaying accepted prompts or tools. | In Progress; Un-Reviewed; Not User Tested | chat-1791755529536-1b146e |
| Provider resources | Misc | Provider Policy Gates | Resolve compatible Gemini account or client access and Antigravity headless tool-policy gates without bypassing provider restrictions. | Blocked; Assigned for coordination; Un-Reviewed; Not User Tested | chat-1791755529536-1b146e |
| Provider resources | Misc | Independent Reviewer Availability | Restore the permitted UAM reviewer connection and obtain hash-bound independent verdicts for work still awaiting that review. | Blocked; Assigned for coordination; Un-Reviewed; Not User Tested | chat-1791755529536-1b146e |
| Data ownership | Performance | Copied Deletion Timing | Measure bounded deletion phases on isolated copied data with private provider homes and survivor-byte checks after confirming capacity. | In Progress; Un-Reviewed; Not User Tested | chat-1791755529536-1b146e |
| Data ownership | Misc | Live Folder Consolidation | Obtain David’s explicit mapping approval before consolidating the conflicting histories and folder aliases identified by the copied-data rehearsal. | Blocked; Assigned for coordination; Un-Reviewed; Not User Tested | chat-1791755529536-1b146e |
| Platform acceptance | Misc | Current Windows Application Acceptance | Verify the latest source with full Windows application builds and live GUI checks instead of relying on older pinned builds or helper cross-compilation. | In Progress; Claude Review; Not User Tested | chat-1791755529536-1b146e |
| Platform acceptance | Graphical | macOS Dock Icon Acceptance | Verify the selected full-tile artwork renders at the intended size without cropping in the actual macOS Dock. | Open; Claude Review; Not User Tested | Unassigned |
| Platform acceptance | Misc | Physical Mobile Acceptance | Verify physical iPhone paging, authentication persistence, reconnect and installed icon behavior when the designated device is available. | Blocked; Claude Review; Not User Tested | Unassigned |
| Platform acceptance | Misc | Image And Cleanup Acceptance | Complete applicable live image, transcript-cleanup and account-allowance checks while retaining provider and platform limits. | In Progress; Un-Reviewed; Not User Tested | chat-1791755529536-1b146e |
| User acceptance | Misc | Explicit Change Signoff | Collect David’s item-specific testing and approval references before marking any cumulative change Complete. | Open; Not User Tested | Unassigned |
| Context and navigation | Graphical | Compaction Before After Values | Show compaction in progress and the available context usage before and after it as X to Y, following T3 without inventing unavailable provider metrics. | In Progress; Un-Reviewed; Claude Review; Not User Tested | chat-1791755529536-1b146e |
| Context and navigation | Graphical | Referenced Context Chips | Use inspectable chips for referenced files, branches and other context with T3-inspired interactions and UAM themes. | Open; Un-Reviewed; Claude Review; Not User Tested | Unassigned |
| Providers and review | Bugfix | Broken AI Review | Investigate the reported broken AI review flow and restore reliable request, result and error handling without treating historical fixes as current acceptance. | In Progress; Un-Reviewed; Claude Review; Not User Tested | chat-1791755529536-1b146e |
| Providers and review | Misc | Native Automatic Mode Translation | Claude and Codex native translation, warning capture and residual-prompt protection pass isolated verification, with other provider translations and live acceptance still pending. | In Progress; Un-Reviewed; Claude Review; Not User Tested | chat-1791755529536-1b146e |
| Providers and review | Misc | OpenCode Automatic AI Review | The OpenCode permission-frame to isolated-reviewer and durable one-time verdict path passes private fixtures, with native capability and authenticated live acceptance pending. | In Progress; Un-Reviewed; Claude Review; Not User Tested | chat-1791755529536-1b146e |
| Providers and review | Graphical | Provider Enable Disable Controls | Native host-scoped switches and guards pass isolated verification, with frontend controls and acceptance still pending. | In Progress; Un-Reviewed; Claude Review; Not User Tested | chat-1791755529536-1b146e |
| VCS and worktrees | Bugfix | Deepest Worktree Selection Regression | Reproduce and restore selection of the intended furthest descendant worktree while preserving explicit user selection. | In Progress; Un-Reviewed; Claude Review; Not User Tested | chat-1791755529536-1b146e |
| VCS and worktrees | Graphical | Composer Workspace Branch Controls | Match the sidebar workspace-then-branch presentation and allow branch selection in the composer and new-chat flow using each VCS system’s supported branch equivalent. | In Progress; Un-Reviewed; Claude Review; Not User Tested | chat-1791755529536-1b146e |
| VCS and worktrees | Graphical | Expanded VCS Defaults | Extend basic switches with configurable default base branch, automatic worktree creation, opt-in UAM-managed local Git features, merge method and pulling before new chats. | In Progress; Un-Reviewed; Claude Review; Not User Tested | chat-1791755529536-1b146e |
| Navigation and interface | Graphical | Message Rail T3 Parity | Compare the reported scroll control with Codex and T3 for dimensions, mark spacing, sizing and interactions, confirming whether the message rail is the affected control. | Open; Un-Reviewed; Claude Review; Not User Tested | Unassigned |
| Usage and accounting | Graphical | Allowances Limits Claude Review | Have Claude fully review and redesign allowance and limit displays for hierarchy, clarity and usability. | Open; Un-Reviewed; Claude Review; Not User Tested | Unassigned |
| Settings and motion | Graphical | Configurable Panel Motion | Add T3-inspired motion controls for panel opening and closing speeds and base interaction performance measurements on default animation settings. | Open; Un-Reviewed; Claude Review; Not User Tested | Unassigned |
| Remote settings | Graphical | Remote Host Settings Review | Review Remote Hosts and Configured Hosts for consistent scope, clear capabilities and usable configuration flows. | Open; Un-Reviewed; Claude Review; Not User Tested | Unassigned |
| Providers and MCP | Bugfix | MCP Connection Secret Exposure | Raw connection URLs and launch arguments still render in Claude-owned MCP summaries, with implementation awaiting UI coordination and no available UAM message tool. | Blocked; Assigned for coordination; Un-Reviewed; Claude Review; Not User Tested | chat-1791755529536-1b146e |
| Remote settings | Bugfix | Remote Provider Capability Sharing | Investigate blank configured-provider capability rows and ensure scoped provider configuration and supported capability information reach SSH helpers. | In Progress; Un-Reviewed; Claude Review; Not User Tested | chat-1791755529536-1b146e |
| Providers and MCP | Graphical | Provider Specific Settings Redesign | Redesign provider-specific settings while retaining the well-received T3-inspired searched-model selector. | Open; Un-Reviewed; Claude Review; Not User Tested | Unassigned |
| Providers and MCP | Performance | Startup Model Catalogue Refresh | Refresh provider and model metadata in the background at main-app startup, reuse it across chats and refresh again only on restart or an explicit settings action. | In Progress; Un-Reviewed; Claude Review; Not User Tested | chat-1791755529536-1b146e |
| Providers and MCP | Graphical | Unavailable Model Restart Warning | When a cached model becomes inaccessible, show an actionable warning advising app restart or the manual model refresh. | In Progress; Un-Reviewed; Claude Review; Not User Tested | chat-1791755529536-1b146e |
| Import and attachments | Graphical | Import Experience Review | Review the import popup and its paging, shift selection and import-all behavior for a clear mass-import workflow. | Open; Un-Reviewed; Claude Review; Not User Tested | Unassigned |
| Memory and skills | Bugfix | Unexpected Memory Store Window | Investigate and prevent the memory store opening outside its intended settings interaction. | In Progress; Un-Reviewed; Claude Review; Not User Tested | chat-1791755529536-1b146e |
| Memory and skills | Graphical | Skills Memory Settings Review | Redesign the settings skills experience and assess locating memory settings under providers with scope made clear. | Open; Un-Reviewed; Claude Review; Not User Tested | Unassigned |
| Navigation and interface | Graphical | Collapsed Shell Actions | Show shell actions collapsed by default with an explicit expansion control. | Open; Un-Reviewed; Claude Review; Not User Tested | Unassigned |
| Side chats | Misc | Temporary Side Chat Context | Verify side chats use temporary conversation context for a quick message rather than silently creating Git branches or worktrees. | In Progress; Un-Reviewed; Claude Review; Not User Tested | chat-1791755529536-1b146e |
| Side chats | Bugfix | Chat History Error Investigation | Inspect the reported chat-history errors and preserve useful scoped diagnostics and recovery behavior. | In Progress; Un-Reviewed; Claude Review; Not User Tested | chat-1791755529536-1b146e |
| Side chats | Graphical | Side Chat Composer Controls | Replace the top banner with a small right-composer side-chat and return control opposite VCS controls and provide a dismiss action that ends the temporary interaction. | Open; Un-Reviewed; Claude Review; Not User Tested | Unassigned |
| Usage and accounting | Graphical | Usage Experience Improvements | Review usage totals, provider coverage, estimated cost presentation and cached background behavior against the requested T3-inspired experience. | Open; Un-Reviewed; Claude Review; Not User Tested | Unassigned |
| Memory and skills | Graphical | Memory Policy Dropdown | Offer Off, Read Only, Strict Creation, Balanced Creation, Remember Everything and Default with creation and prompt insertion honoring the selected policy. | In Progress; Un-Reviewed; Claude Review; Not User Tested | chat-1791755529536-1b146e |
| Goals and providers | Misc | Native Goal Delegation | Investigate provider-native goal support, delegate where supported and remove the UAM goal token-budget control without claiming unsupported native equivalents. | In Progress; Un-Reviewed; Claude Review; Not User Tested | chat-1791755529536-1b146e |
| Voice input | Bugfix | Dictation Mode Settings Regression | Restore the previously available on-device and remote-server dictation settings with their scoped configuration. | In Progress; Un-Reviewed; Claude Review; Not User Tested | chat-1791755529536-1b146e |
| Performance | Performance | Strict Interaction Performance Budgets | The first 19 frontend and eight native timing regressions are implemented, with full app startup, remaining interaction categories and workload expansion still pending without performance optimizations. | In Progress; Un-Reviewed; Claude Review; Not User Tested | chat-1791755529536-1b146e |
| Performance | Performance | Input Timing Regression Investigation | Parallel runs measured 134-ms and 114-ms input responses while the corrected serial timing stage passes without changing the 100-ms limit, with scheduling noise retained as an explicit investigation limit. | In Progress; Un-Reviewed; Claude Review; Not User Tested | chat-1791755529536-1b146e |
| Core correctness | Bugfix | Diagnostic UTF8 Boundary Safety | The shared byte-cap fix passes Unicode boundary and native-warning regressions plus full verification, with live and review acceptance pending. | In Progress; Un-Reviewed; Claude Review; Not User Tested | chat-1791755529536-1b146e |
| Build and evidence | Misc | Native Build Capacity Failure | The timing-test rebuild recovered from transient disk exhaustion using two compiler jobs without deleting live data, with both builds, 1548 core tests and all 19 CTest gates subsequently passing while longer-term capacity remains a separate concern. | In Progress; Un-Reviewed; Not User Tested | chat-1791755529536-1b146e |
| Usage and accounting | Graphical | All Time Usage Range | Coordinate frontend All Time selection and historical coverage with Claude, then verify local and remote lifetime behavior in the live app. | In Progress; Un-Reviewed; Claude Review; Not User Tested | chat-1791755529536-1b146e |
| Appearance | Graphical | Appearance Contrast Diff Width | Add contrast and specific diff-colour controls plus Wide, Full and default Comfortable chat-width options with accessible themes. | Open; Un-Reviewed; Claude Review; Not User Tested | Unassigned |
| Storage and retention | Graphical | Configurable Storage Cleanup | Add settings for worktrees of deleted threads, inactive, merged and unchanged worktrees, old browser artifacts and logs while protecting active work and unmerged changes. | In Progress; Un-Reviewed; Claude Review; Not User Tested | chat-1791755529536-1b146e |
| Composer and queue | Graphical | Composer Draft In Progress | Add a composer indicator showing that a prompt is currently being drafted without implying that its unfinished contents have been sent to the AI. | Open; Un-Reviewed; Claude Review; Not User Tested | Unassigned |
| Navigation and interface | Graphical | Persistent Processing Status | Keep orange waiting-on-subagents and compacting status text visible while work is active even when no new output arrives, clearing it only when the underlying state settles. | In Progress; Un-Reviewed; Claude Review; Not User Tested | chat-1791755529536-1b146e |
| Providers and subagents | Graphical | Provider Subagent Activity Visibility | Expose current child activity in chat and usable child-session views where supported, verify Codex and Claude native identity limitations, and complete OpenCode child-session integration without inventing session IDs. | In Progress; Un-Reviewed; Claude Review; Not User Tested | chat-1791755529536-1b146e |
| Claude and orchestration | Bugfix | Duplicated Claude Subagent History | A verified native identity guard prevents replayed parent assistant envelopes duplicating narration, with authenticated child-history acceptance still pending. | In Progress; Un-Reviewed; Claude Review; Not User Tested | chat-1791755529536-1b146e |
| Composer and queue | Graphical | Centered New Chat Composer | Require a Claude model to implement a composer centered in an empty new chat that animates to the bottom after the first message, with reduced-motion support and preserved focus and draft. | Open; Un-Reviewed; Claude Review; Not User Tested | Unassigned |
| Navigation and interface | Graphical | Chat Icon Picker | Allow selecting and persisting a chat icon from a broad, varied library and include an associated page icon or image when available with a fallback when it cannot load. | In Progress; Un-Reviewed; Claude Review; Not User Tested | chat-1791755529536-1b146e |
| AI performance | Misc | Context Pollution Report | Source-backed report records prompt insertion, memory/skill/handoff bounds and measurement gaps; no provider latency or token-growth cause is claimed. | In Progress; Un-Reviewed; Not User Tested | chat-1791755529536-1b146e |
| Usage and accounting | Graphical | Usage Chart Keyboard Access | Make usage chart columns keyboard focusable with readable values or provide an accessible table alternative. | Resolved in `0123b279`; Claude UI Reviewed; Not User Tested | Unassigned |
| Usage and accounting | Graphical | Usage Chart Token Split | Show the input, cache, output and reasoning split within each token chart column rather than only per provider. | Resolved in `0123b279`; Claude UI Reviewed; Not User Tested | Unassigned |
| Usage and accounting | Misc | Usage Test Timeout Flakiness | Stabilise AppShell usage tests that can exceed the default timeout under parallel load. | Open; Un-Reviewed; Not User Tested | Unassigned |
| Workspaces and deletion | Graphical | Determinate Deletion Progress | Report per-item deletion progress from the backend so the indeterminate deletion bar can show real completion. | Open; Un-Reviewed; Not User Tested | Unassigned |
| Remote settings | Bugfix | Per Host Busy State | Scope remote host busy and progress state to the host being changed instead of every configured host. | Open; Un-Reviewed; Not User Tested | Unassigned |
| Remote settings | Bugfix | Startup State Contradiction | Expose applied runner-startup state separately so the status line cannot contradict the saved switch. | Open; Un-Reviewed; Not User Tested | Unassigned |
| Providers and MCP | Graphical | Wide MCP Matrix Layout | Replace the horizontally scrolling provider-by-server matrix with a per-server or grouped layout when many MCP servers exist. | Resolved in `41331441`; Claude UI Reviewed; Not User Tested | Unassigned |
| Settings and motion | Graphical | Settings Page Exit Crossfade | Add an exit-aware page wrapper so settings pages cross-fade instead of only animating in. | Resolved in `f72a3af4`; Claude UI Reviewed; Not User Tested | Unassigned |
| Settings and motion | Graphical | Central Setup Visual Consistency | Align Central Setup native selects, file picker and unboxed sections with the shared settings field and section style. | Resolved in `034e8c67`; Claude UI Reviewed; Not User Tested | Unassigned |
| Appearance | Graphical | Theme Swatch Picker | Show theme choices as visual swatches instead of a text-only select. | Resolved in `f72a3af4`; Claude UI Reviewed; Not User Tested | Unassigned |
| Settings and motion | Graphical | Remaining Inline Modal Scrims | Move memory scan, memory library and markdown store modal scrims and remaining inline colours onto shared overlay tokens. | Resolved in `41331441`; Claude UI Reviewed; Not User Tested | Unassigned |
| Composer and queue | Graphical | Queue Drag Reordering | Add drag-to-reorder for queued prompts alongside the existing move controls. | Resolved in `33db0f3f`; Claude UI Reviewed; Not User Tested | Unassigned |
| Composer and queue | Graphical | Edit Strip Duplicate Cancel | Remove the duplicated Cancel control between the editing-message strip and the composer footer. | Resolved in `33db0f3f`; Claude UI Reviewed; Not User Tested | Unassigned |
| History and performance | Graphical | History Loading Shimmer Decision | Decide whether history-loading skeletons should use a one-shot shimmer given the no-idle-animation rule. | Open; Claude Review; Not User Tested | Unassigned |
| Graphical review | Misc | Harness Unreachable UI States | Live-check the undo toast, MCP warning banner, goal chip, goals popup, loading skeleton, subagent and compaction rows that the fake backend could not reach. | Open; Claude Review; Not User Tested | Unassigned |
| Build and evidence | Misc | Usage History Bridge Docs | Add the getUsageHistory bridge action to the CLAUDE.md action table. | Open; Un-Reviewed; Not User Tested | Unassigned |
| Memory and skills | Misc | Automatic Provider Agent Import | Let the Agents page import agents directly from a chosen provider's native configuration without asking for a Markdown file path. | Open; Un-Reviewed; Claude Review; Not User Tested | Unassigned |
| Providers and MCP | Bugfix | Computer Use Disable Setting | Add a persisted setting and action so built-in Computer Use can be turned off, then replace its status-only rows with a real switch. | Open; Un-Reviewed; Claude Review; Not User Tested | Unassigned |
| Providers and MCP | Misc | UAM MCP Always On Enforcement | Decide whether per-provider UAM MCP overrides and the JSON editor's enabled:false should remain now that the interface presents the service as always on. | Open; Un-Reviewed; Not User Tested | Unassigned |
| Settings and motion | Graphical | Remaining Settings Native Selects | Move typography, Computer Use backend and memory worker pickers onto the shared menu select and update their tests. | Resolved in `f72a3af4 / 41331441`; Claude UI Reviewed; Not User Tested | Unassigned |
| Settings and motion | Graphical | Settings Dialogs On Overlay | Move Central Setup agent editing, Shell Actions dialogs and Skills child dialogs onto the shared overlay with exit motion. | Resolved in `f72a3af4 / 41331441`; Claude UI Reviewed; Not User Tested | Unassigned |
| Memory and skills | Bugfix | Superseded Store Refresh Result | Return success for superseded markdown-store refreshes so callers cannot report a false load failure. | Open; Un-Reviewed; Not User Tested | Unassigned |
| Workspaces and deletion | Graphical | File Type Editor Labels | Show which editor each file-type group opens in directly on its row. | Resolved in `f72a3af4`; Claude UI Reviewed; Not User Tested | Unassigned |
| Notifications | Misc | Notification Timestamps | Add timestamps to notifications so the panel can group by Today/Earlier and show relative times. | Open; Un-Reviewed; Not User Tested | Unassigned |
| Notifications | Misc | Update Release Note Excerpts | Carry a release-note excerpt in the update catalogue so update cards can show what changed. | Open; Un-Reviewed; Not User Tested | Unassigned |
| Providers and MCP | Misc | Model Context And Size Metadata | Add context-window and size fields to model metadata so the model picker can show them as badges. | Open; Un-Reviewed; Not User Tested | Unassigned |
| Settings and motion | Code Only | Menu Select Id Support | Let the shared menu select accept an id so settings labels can reference it directly. | Open; Un-Reviewed; Not User Tested | Unassigned |
| Deferred ideas | Misc | T3 PR Workflow | Keep T3-inspired pull-request, branch and GitHub-provider workflows plus auto-settling at the very end of the backlog for later design and implementation. | Open; Claude Review; Not User Tested | Unassigned |

Scope recorded 2026-10-11: these are David’s requested work and reported regressions, pending investigation rather than newly verified defects or completed changes. Existing implementation records remain intact. Mobile overhaul and mobile chat-width work are deferred at David’s request; the historical physical-device acceptance gate remains recorded. SSH computer use stays disabled as the current security policy, with any future support outside this change request. Performance limits require explicit values before acceptance and must include application processing and default animation completion.

### V5 swarm allocation and active execution

Allocation recorded 2026-10-11. David authorized execution on 2026-10-11 after confirming the timing-test scope and limits; the internal orchestration goal is active. Existing Changes entries marked Un-Reviewed are excluded from the swarm review scope; the separate review pass follows the final V5 feature set. Claude Review is retained on mixed work and means Claude owns its UI implementation and review, not that its backend is excluded.

- Coordinator: `chat-1791755529536-1b146e`, branch `build/v5-bug-fix-swarm`, worktree `worktrees/96305b429aa4db15/build/v5-bug-fix-swarm` under the UAM application-support directory.
- 48 original rows are reserved to the coordinator, with additional discoveries claimed in this register; 20 remain outside this reservation for Claude UI, David sign-off or explicitly deferred work. In Progress denotes coordinator ownership; per-packet evidence distinguishes active work from queued work. Existing Blocked states and acceptance flags are preserved.
- Claude's active UI chat is `chat-1791749306179-8f5743`, worktree `worktrees/96305b429aa4db15/build/fix-new-gui-elelements`, checked-out Git branch `claude/v5-ui-ux-sweep`. This is verified chat/worktree metadata, not a fabricated worker assignment. Its 20 excluded rows are listed below; no ownership handoff or completed review is inferred. No UAM chat-message tool is available, so mixed frontend edits require coordination through David before touching Claude's files.
- Proposed workers use `gpt-6.1-sol` through UAM only. Never use Astra. Size and split packets by logical dependencies and exclusive file ownership, not an artificial three-worker cap or token-saving target; David permits as many or as few workers as the work needs. Actual host/tool concurrency limits still apply and must be reported honestly. Each worker uses a separate worktree based on the latest integrated V5 tip; use read-only discovery first and explicitly divided file ownership for fixes.
- Worker chat IDs and actual worktree paths do not exist yet. The coordinator retains ownership until a permitted UAM delegate returns verified identity and worktree metadata; publish that handoff to this register and `uam/v5-improvements` before the worker edits. No invented IDs or planned paths count as ownership.
- UAM currently reports readOnly mutation policy, mutationToolsEnabled=false, and only ui-consistency-review with workspaceAccess=read. Implementation delegation is blocked until permitted build agents and mutation capability are available; do not bypass this using another agent-launch mechanism.
- Shared files, especially app models, state serialization, ACP runtime, settings, CMake, core_tests.cpp and frontend stores, are edited by one owner at a time. Concrete file lists are fixed after discovery and published before implementation. Workers do not spawn workers or independently edit the master log; the coordinator integrates their tested changes and records their attribution.

### David's allocation corrections, 2026-10-11

- David approved the logical packet grouping and removed the proposed three-worker ceiling; the number of agents follows independent work and real tool capacity.
- GPT workers may implement new UI where needed, following the current UAM visual design and existing components. The standing pure-black background preference does not apply to this project. Claude's active file ownership must still be respected; permission to implement UI is not permission to duplicate another agent's task.
- **Every user-visible change requires Claude Review**, including new features, controls, loading states, notifications, errors, warnings, messages and user-facing fallback behavior even if it appears only in rare circumstances. A purely internal bug fix with no user-visible surface change is the only exception. This rule is broader than React file changes and applies to backend-generated text and runtime responses too. Add or retain Claude Review in the relevant Changes and Incomplete Work rows in the same implementation commit; retain it until Claude's review and resolved findings are documented.
- Keep the master log current at assignment, discovery, scope change, handoff, implementation, verification and integration. Recheck current owners and branch tips before assigning work; publish verified worker chat IDs, worktrees and exclusive file scopes before edits. Shared dependencies are coordinated through the orchestrator and available UAM communication, not concurrent overlapping changes.
- **Performance test scope and limits confirmed by David, 2026-10-11:** this timing packet adds executable regression tests, not performance optimizations, a Markdown-only rule or a skill. General user interactions must complete within 250 ms including local processing and default animation completion; settings and panels should aim for 100 ms or less. Cold and warm startup are capped at 10 seconds to a usable app. New-chat composer and send feedback must appear immediately without waiting for provider or other background work. Initial visible history pages for 20/50/200-message and huge chats have a 1-second ceiling and a preferred 500-ms target, independent of total transcript size; subsequent scrolling and controls retain the interaction budget. Provider time to first response is excluded from local feedback timing. No typing interruption, stutter, frozen frame or macOS beach ball is acceptable. Favor bounded paging and caches over retaining all history in RAM. Cached provider information remains visible while applicable background refresh runs; verify startup/use/manual refresh paths according to the agreed metadata policy. David confirmed a 100-ms upper limit for input-to-response tests, the machine executing the tests as the reference, and time-only scope with no RAM ceiling for this pass; no arbitrary fixed concurrency assumption is added.
- Existing P1 optimization rows remain recorded and queued separately; this performance-test packet does not implement optimizations. Do not fix pre-existing slowness silently to make the new tests pass, weaken thresholds or hide failing counts; report failures separately for a later optimization decision.

| Packet | Proposed model | Reserved incomplete entries | File boundary and sequence |
|---|---|---|---|
| P1 Performance and durability | gpt-6.1-sol | Asynchronous Send Durability; Large Chat Opening Acceptance; Typing And Sending Acceptance; Strict Interaction Performance Budgets | Persistence, history workers and timing harness; serialize ACP/service overlaps with P3 and retention overlaps with P5; Claude owns visible loading and motion. |
| P2 Permissions and MCP | gpt-6.1-sol | Managed Action Acceptance; Broken AI Review; Native Automatic Mode Translation; OpenCode Automatic AI Review; MCP Connection Secret Exposure | Permission review, native policy translation and redacted MCP transport; serialize ACP overlaps with P3 and configuration overlaps with P4; Claude owns summaries and dialogs. |
| P3 Provider and child lifecycle | gpt-6.1-sol | Handoff And Review Acceptance; Queue Restart Acceptance; Claude Lifecycle Acceptance; Quota Continuation Acceptance; Temporary Side Chat Context; Chat History Error Investigation; Native Goal Delegation; Persistent Processing Status; Provider Subagent Activity Visibility; Duplicated Claude Subagent History | Provider runtime, replay identity, queue/session ownership and side-chat context; execute one bounded provider fix at a time; Claude owns activity and composer UI. |
| P4 Provider configuration and memory | gpt-6.1-sol | Provider Enable Disable Controls; Remote Provider Capability Sharing; Startup Model Catalogue Refresh; Unavailable Model Restart Warning; Unexpected Memory Store Window; Memory Policy Dropdown; Dictation Mode Settings Regression; Context Pollution Report | Catalogue caching, capability transport, persisted provider switches, memory policy, dictation configuration and context report; Claude owns settings components; report is last. |
| P5 Workspaces and safe retention | gpt-6.1-sol | Copied Deletion Timing; Live Folder Consolidation; Deepest Worktree Selection Regression; Composer Workspace Branch Controls; Expanded VCS Defaults; Configurable Storage Cleanup | Worktree ancestry, base-branch/default settings and cleanup safety; serialize lifecycle/model overlaps with P3/P4; live consolidation stays blocked without mapping approval. |
| P6 Usage and persisted presentation | gpt-6.1-sol | Compaction Before After Values; All Time Usage Range; Chat Icon Picker | Daily accounting, honest compaction metrics and persisted icon metadata; Claude owns charts, picker, chips and navigation components. |
| P7 Remote runtime and maintenance | gpt-6.1-sol | Remote Ownership Refusals; Current Helper Activation; Elevated Provider Update Acceptance; Interactive Sudo Update Support; Runner Startup Acceptance; Concurrent Two Host Acceptance; Missing Incident Evidence | Remote runner, shutdown ownership, update and startup services; isolated tests first; production helper activation and live remote maintenance require explicit target authorization. |
| P8 Platform and provider acceptance | gpt-6.1-sol | Six Provider Live Matrix; Provider Policy Gates; Independent Reviewer Availability; Current Windows Application Acceptance; Image And Cleanup Acceptance | Isolated provider matrix, Windows build and runtime receipts, image/cleanup acceptance and resource gates; runs after affected packets integrate; no historical Un-Reviewed audit. |

Outside the 48-row coordinator reservation:

- **Animation Smoothness Acceptance**: Claude UI implementation/review.
- **Claude Interface Review**: Claude UI implementation/review.
- **macOS Dock Icon Acceptance**: Claude UI implementation/review.
- **Physical Mobile Acceptance**: Deferred physical-device acceptance; hardware gate preserved.
- **Explicit Change Signoff**: David-specific sign-off; cannot be delegated.
- **Referenced Context Chips**: Claude UI implementation/review.
- **Message Rail T3 Parity**: Claude UI implementation/review.
- **Allowances Limits Claude Review**: Claude UI implementation/review.
- **Configurable Panel Motion**: Claude UI implementation/review.
- **Remote Host Settings Review**: Claude UI implementation/review.
- **Provider Specific Settings Redesign**: Claude UI implementation/review.
- **Import Experience Review**: Claude UI implementation/review.
- **Skills Memory Settings Review**: Claude UI implementation/review.
- **Collapsed Shell Actions**: Claude UI implementation/review.
- **Side Chat Composer Controls**: Claude UI implementation/review.
- **Usage Experience Improvements**: Claude UI implementation/review.
- **Appearance Contrast Diff Width**: Claude UI implementation/review.
- **Composer Draft In Progress**: Claude UI implementation/review.
- **Centered New Chat Composer**: Claude UI implementation/review.
- **T3 PR Workflow**: Explicitly deferred, keep last in backlog.

### Performance regression coverage proposed for David's check

Executable coverage must measure actual completion rather than handler return: event to painted usable UI and default motion settled; startup to an interactive composer; history selection to a correctly rendered first page; input to painted text/caret; send to visible local pending feedback. Fake timers and DOM-only service tests cannot establish real rendering or native responsiveness. Use focused unit tests for blocking/caching/state invariants, rendered interaction tests for frontend timing and native integration measurements for CEF, filesystem, startup and responsiveness. A delayed provider or remote reply must not prevent local acknowledgement or cached state from displaying.

| Interaction category | Representative regression journeys | Ceiling or agreement gate |
|---|---|---|
| Input and editing | Type, paste, select text, move caret, undo, edit drafts and queued prompts, use IME while history loads and chats stream. | Input code-path response at most 100 ms; rendering/frame-gap assertions are supplementary and do not replace code-path regression tests. |
| Chat navigation | Select/create chats, switch branch or workspace, return from side chat, jump to search hits or message-rail positions. | 250 ms for UI; first visible history page at most 1 second, target 500 ms. |
| Panels and dialogs | Open/close settings, memory, usage, review/diff panels, menus, pickers and confirmation dialogs; keyboard and Escape equivalents. | 250 ms including default motion; target 100 ms. |
| Settings and cached metadata | Switch settings tabs, change/save preferences, load cached models/providers/allowances, explicit refresh with delayed response. | 250 ms local response; cached content remains visible while refresh proceeds. |
| Send and runtime controls | Send, enqueue, steer, pause/resume, cancel/Stop, answer questions and permissions. | Immediate local feedback, at most 100 ms; provider/remote completion measured separately. |
| History and large output | First and subsequent history pages, large tool output, images, long lines and diffs, scroll while loading. | Initial page at most 1 second; local controls 250 ms; frame-gap ceiling pending. |
| Organization and VCS | Rename/pin/snooze, move chats, expand folders, branch/worktree selection, deletion confirmation and progress. | 250 ms feedback/control response; slow filesystem work stays asynchronous and separately measured. |
| Attachments and import | Choose/remove attachments, lazy previews, open imports, page/select/import-all and show progress. | 250 ms local response; external picker, decoding and import completion separated. |
| Errors and recovery | Unavailable model/provider, startup failure, stale request, reconnect, failed save and retry while keeping usable UI. | 250 ms local feedback after the app receives the result; every changed visible state requires Claude Review. |
| Startup and sustained workload | Cold/warm app launch, large catalogue and persisted history, idle/streaming/background refresh with repeated typing/navigation. | Startup at most 10 seconds; interaction limits remain in force; executing machine is the reference; no RAM ceiling in this pass. |

Run representative ordinary and heavy fixtures, including bounded pages from a huge history and long tool output. Record exact test inventory, per-operation timings and failing journeys. Hard ceilings do not become median-only acceptance; the agreed workload, repetition policy and handling of measurement/environment noise must be explicit. Performance failures remain failures and require separate optimization authorization. Startup does not wait for refreshed provider metadata; local completion and external/provider completion are distinct assertions. Do not claim no beach balls from a fast unit test.

Execution order: capture the exact baseline and compiled/frontend test inventory; discover P1, P2 and P3; integrate their bounded fixes; then P4, P5 and P6 as shared-file dependencies permit; run P7 only within authorized isolated targets; finish P8 and full integrated verification. Recheck Claude and V5 branch tips before every integration. Keep the five divergent main commits as a separate integration decision, not a blind merge that overwrites newer V5 or Claude code.

Each bug requires reproducible evidence, two upstream/downstream hops, a focused regression and the smallest shared fix. Each implemented feature requires its specific behavioral tests. Before local code commits and integration, focused checks must pass; final acceptance requires frontend tests/build, native application build, all CTest gates and exact expected pass/fail/skip totals. Unexpected missing tests, failures or timeouts block completion. Use isolated data and provider homes; never mutate daily-driver builds, live databases or production helpers on the strength of general swarm approval. Timing thresholds need David's values, policy/account gates need valid access, and actual Windows/iPhone acceptance needs the corresponding host/device. User approval and Claude Review flags cannot be cleared by automated success.

The historical commit inventory below preserves the source/test/build entries, including follow-up corrections and checkpoints.



## Claude UI/UX sweep (2026-10-11)

Branch `claude/v5-ui-ux-sweep` from `uam/v5-improvements` at `061b107e`. Scope: visual design, user experience and animation of the existing Claude Review rows only; behaviour, store logic and IPC were not changed, and code correctness remains for a separate review. Seven surface passes were delegated to Haiku subagents under a shared design brief and every result was reviewed, corrected and screenshot-checked by Claude in dark and light themes through a fake CEF backend. Frontend verification after the final commit: 97 files, 1,450 of 1,450 Vitest tests passing, `tsc --noEmit` clean and `npm run build` passing. No row is user tested.

| Commit | Surface | Summary |
|---|---|---|
| `b3bb50b9` | Sidebar | Compact two-line activity rows with inline workspace, branch, host and provider, single accent tint and pop-in menus. |
| `672e3414` | Composer and queue | Aligned 26/32px controls, compact queue rows, held-after-restart banner and goal budget meter. |
| `5e6ff972` | Transcript | Message rhythm, rail hover previews, compaction context meters, subagent status rows and an editing-message strip. |
| `c0813995` | Usage | Stat tiles, reported versus hatched estimated spend, stacked token bars, allowance cards led by threshold bars with relative reset times. |
| `49ed808a` | Providers and MCP | Overview matrix with host segmented control, status dots, read-only Computer Use availability and MCP server rows. |
| `7a42da1b` | Settings | Sliding nav indicator, remote host cards, typography live previews and tokenised colours. |
| `47f46436` | Review and import | Tinted split/unified diffs, per-file change bars and a selectable import list. |
| `737197fb` | Motion | Collapse and dialog exit animations through the shared presence primitive. |
| `df0969c8` | New chat and navigation | New chat hierarchy, workspace search highlighting, undo toast and compact selection tiles. |
| `6dac63b8` | Settings | Remote host switches, CLI update progress, Central Setup choices and page motion. |

**Main Application Icon** and **Live Performance Acceptance** keep **Claude Review** because they require the packaged Dock icon and live interaction timing. Larger UI findings from the sweep are recorded below as Unassigned incomplete work.


### Wave 3 — David's review feedback (2026-10-12)

Settings were rebuilt on one shared language (`SettingsPrimitives`: uppercase section label, bordered card, title/description rows with the control on the right, label-above fields, animated loading), modelled on the MCP Servers page. Pages are centred and fluid, the nav highlight glides as a whole pill, switches are neutral with accent reserved for important switches, Typography merged into Appearance, Editors and file explorer merged, provider pages are flat in install → defaults → MCP order, MCP add-server opens a dialog and the UAM MCP service is shown as always on. Outside settings: one-line native import rows with failure counts and live progress, usage estimated-only cost with fixed provider colours and no hatching, a status strip above the composer for timeout, compaction and subagent waits, a history skeleton matching the current layout, rail "not loaded yet" hints, provider filter icons, a hideable review panel and tooltips above every overlay. Frontend verification: 97 files, 1,452 of 1,452 Vitest tests, `tsc --noEmit` clean, `npm run build` passing.

| Commit | Surface | Summary |
|---|---|---|
| `4bd89456` | Mixed (UAM checkpoint) | UAM's automatic turn checkpoint captured in-progress wave 3 import, transcript, review, sidebar and settings-foundation edits. |
| `7ab62ebb` | Import | One-line rows, select-all count, single Import action, hidden single-page paging and failure counts. |
| `712da333` | Transcript | Status strip, matching history skeleton and rail load hints. |
| `20723015` | Usage | Provider chips and colours, estimated-only cost, inline token bars and readable empty limits. |
| `034e8c67` | Settings | Shared settings language, neutral switches, centred fluid pages, MCP add dialog and always-on UAM MCP. |
| `b8fd1d67` | Settings | General pages, provider pages, agents, remote hosts, skills and memory store in the shared language. |


**Merge into `uam/v5-improvements` (2026-10-12):** the sweep branch was merged with the 26 newer V5 commits from other agents. The change log was merged row by row: every row, heading and paragraph from both sides is retained (703 rows), the three shared core-correctness rows keep their Claude UI Reviewed status, and other agents' newer rows keep their own statuses. Merged-tree verification: `tsc --noEmit` clean and 1,473 of 1,474 Vitest tests passing in the full parallel run; the one failure was the new interaction performance budget (empty new-chat composer editable within 100ms, measured 203ms under full-suite load), which passed 11 of 11 in three isolated reruns. The `claude/v5-ui-ux-sweep` branch remains for further tweaks.

### Wave 4 — remaining UI and motion items (2026-10-12)

Finished the remaining presentation-only follow-ups: keyboard-navigable usage chart with token-type stacking, notifications and updates panels, refined model picker with a gliding provider rail, theme swatch grid with arrow-key navigation, every settings dialog on the shared animated overlay, settings page cross-fade, file-type editor labels, typography and Computer Use pickers on the shared menu select, modal Central Setup agent editor, collapsed shell action rows, Skills child-dialog motion and quiet pins, memory scan rows, a many-server MCP popover in the provider overview, queue drag reordering, goal status pills, recorded-edit rows on the changed-files grid and a single editing Cancel. Frontend verification: 97 files, 1,455 of 1,455 Vitest tests, strict `npm run build` passing.

| Commit | Surface | Summary |
|---|---|---|
| `0123b279` | Usage | Keyboard-navigable chart, token-type stacking and edge-aware tooltips. |
| `b1b17290` | Panels | Notifications and updates panels and refined model picker. |
| `f72a3af4` | Settings | Theme swatches, animated settings dialogs, page cross-fade and editor labels. |
| `33db0f3f` | Chat | Queue drag reorder, goal status pills, recorded edit rows and single edit cancel. |
| `41331441` | Settings | Collapsed shell actions, modal agent editor, MCP popover, quiet pins and memory scan rows. |

## Historical alpha82 commit inventory

These 278 entries are reachable from the pinned alpha82 source and include follow-up fixes, parallel commit variants, test changes and checkpoints rather than 278 independent features.


| Commit | Date | Work |
|---|---|---|
| `3ed93c81` | 2026-10-11 | Parse unchanged JSON string spans in bulk |
| `6079c141` | 2026-10-11 | Avoid rewriting unchanged local prompt delivery state |
| `915302d6` | 2026-10-11 | Load cold send history outside the background VCS queue |
| `a4837bff` | 2026-10-11 | Serialize unchanged JSON string spans in bulk |
| `1e86386e` | 2026-10-11 | Build ACP prompts without copying loaded history |
| `f1d5b6cb` | 2026-10-11 | Publish only changed computer-use scan entries |
| `e1870564` | 2026-10-11 | Publish history tail caches on the background worker |
| `b6da62be` | 2026-10-11 | Dispose detached history buffers off the GUI thread |
| `658fac0e` | 2026-10-11 | Load activity branch labels only near the viewport |
| `3141db83` | 2026-10-11 | Prioritize chat history loading over background VCS checks |
| `fdb19c10` | 2026-10-11 | Unmount chat rows in collapsed sidebar sections |
| `257d8714` | 2026-10-11 | Cache bounded complete message tails for large histories |
| `5144865b` | 2026-10-11 | Track OpenCode prompt usage including cache and reasoning |
| `c1b3be0f` | 2026-10-11 | Track Gemini prompt tokens observed during attachment |
| `52af372b` | 2026-10-11 | Reuse unchanged history hashes when messages append or shrink |
| `7142880b` | 2026-10-11 | Dispatch queued prompts as separate durable FIFO turns |
| `65984293` | 2026-10-11 | Edit sent messages through the existing composer |
| `4d1c48ec` | 2026-10-11 | Hide startup recovery while the armed interface loads |
| `655793f4` | 2026-10-11 | Keep composer editable while prompt admission is pending |
| `e29db446` | 2026-10-11 | fix(macos): package the selected full-tile app icon |
| `f2d57d15` | 2026-10-11 | Hide completed-change review while chat history is loading |
| `66ad3195` | 2026-10-11 | fix(sidebar): scope activity branches to their workspace |
| `e74a6e59` | 2026-10-11 | Align the final user-message rail mark with its target |
| `6f8a38b7` | 2026-10-11 | Reuse workspace resolution within each state serialization |
| `33904a68` | 2026-10-11 | Move computer-use scan copies and cleanup off the GUI thread |
| `1176cf95` | 2026-10-11 | Identify slow computer-use, shell and workspace poll stages |
| `b820cbad` | 2026-10-11 | Provide an accepted 16-DIP CEF title-bar icon |
| `22fb7c46` | 2026-10-11 | Show live consumed tokens and publish scoped Claude usage snapshots |
| `908a6962` | 2026-10-11 | Defer memory progress behind pending transcript saves |
| `21ee4a2f` | 2026-10-11 | Index chat identities when publishing computer-use scan results |
| `e6e55d19` | 2026-10-11 | Skip idle terminal catalog scans and diagnose slow UI polling |
| `eec5f3c9` | 2026-10-11 | Fix Windows usage reads and isolate native acceptance fixtures |
| `c06d99f8` | 2026-10-11 | Verify MCP question answers through the native runner relay |
| `b11883f2` | 2026-10-11 | Collect Antigravity usage in cached background accounting |
| `1985cec4` | 2026-10-11 | Fix ES2020 sidebar and MSVC SQLite build compatibility |
| `69a1644a` | 2026-10-11 | Isolate OpenCode text workers from inherited agent settings |
| `25263716` | 2026-10-11 | Include SQLite in headless SSH helper builds |
| `c0a6a036` | 2026-10-11 | Collect cached remote usage and show deduplicated machine totals |
| `1b03886d` | 2026-10-11 | Generate valid deny-all Gemini worker policies |
| `6a3350d4` | 2026-10-11 | Defer automatic memory scans until transcript publication completes |
| `14822685` | 2026-10-11 | Export cached usage accounting through remote runners |
| `8f2d798d` | 2026-10-11 | Avoid repeated memory classification during accepted idle waits |
| `e2c2a3ad` | 2026-10-11 | Preserve stored agent workspace references during cleanup |
| `e712d9dd` | 2026-10-11 | Deliver complete Codex instructions through native session setup |
| `62799111` | 2026-10-11 | Reject special diagnostic log paths before startup opens them |
| `c40dc192` | 2026-10-11 | Preserve empty provider option values in remote process launches |
| `f30b53db` | 2026-10-11 | Bound automatic memory signal scans without changing admission policy |
| `11b2a63c` | 2026-10-11 | Clean up old orphan worktrees behind workspace leases |
| `c709c765` | 2026-10-11 | Avoid transcript folding for permissive memory admission |
| `25ea418f` | 2026-10-11 | Guard remote memory publication with current cancellable ownership |
| `5fae84df` | 2026-10-11 | Honor disabled models in worker and reviewer menu selectors |
| `5e334f5c` | 2026-10-11 | Defer Claude child streaming saves while retaining terminal barriers |
| `548b5777` | 2026-10-11 | Inspect owned orphan worktrees before retention cleanup |
| `c8bbb173` | 2026-10-11 | Prepare first-send memory recall on cancellable owned workers |
| `bd7a1935` | 2026-10-11 | Recover oversized diagnostic logs safely at startup |
| `24c6a905` | 2026-10-11 | Cancel stale remote memory context before startup waits |
| `19be5082` | 2026-10-11 | Move diagnostic logging to a bounded background worker |
| `3f96c1fa` | 2026-10-11 | Cache workspace ownership normalization within deletion preparation |
| `23b2e9c8` | 2026-10-11 | Add scoped file inspection with bounded source and diff navigation |
| `6440dcb7` | 2026-10-11 | Add virtualized split diff layout with remembered view preference |
| `2154718d` | 2026-10-11 | Defer Codex streaming activity saves to guarded background persistence |
| `d721588a` | 2026-10-11 | Add persistent chat snooze with guarded background metadata saves |
| `b0032ae7` | 2026-10-11 | Prepare local memory entries on workers before guarded publication |
| `9af2c690` | 2026-10-10 | Add persistent Git and SVN integration switches |
| `ecdf120a` | 2026-10-10 | Allow regenerated titles on imported transcripts |
| `1ee3a00c` | 2026-10-10 | Regenerate chat titles with guarded background workers |
| `627a1266` | 2026-10-10 | Read memory recall previews without loading whole bodies |
| `9e0be7a6` | 2026-10-10 | Add chat name and ID copy actions to sidebar menus |
| `3f696191` | 2026-10-10 | Include source manifests in locally built helper artifacts |
| `e78c62e1` | 2026-10-10 | Use the official Copilot mark and verify sidebar provider switching |
| `ba49fe88` | 2026-10-10 | Use the official Copilot mark and verify sidebar provider switching |
| `ea9b0d89` | 2026-10-10 | Stop ordinary memory recall scans when their prompt budget is full |
| `e86fc55e` | 2026-10-10 | Resolve usage estimates from cached reference model prices |
| `370f375d` | 2026-10-10 | Read memory count timestamps without loading entry bodies |
| `20a64852` | 2026-10-10 | Cache provider usage in a persistent background ledger |
| `169ce057` | 2026-10-10 | Capture only memory scan input text from its requested range |
| `0ef25d29` | 2026-10-10 | Avoid redundant memory layout checks and title body reads |
| `e4bbd45d` | 2026-10-10 | Guard reused terminal IDs with launch identity |
| `5af17820` | 2026-10-10 | Keep message rail marks tightly spaced in short chats |
| `caccc639` | 2026-10-10 | Drop removed transcript files from usage history cache |
| `85453bf2` | 2026-10-10 | Index memory titles once per completed extraction |
| `070704f0` | 2026-10-10 | Expand message rail hover marks around the orange click target |
| `41cd6b91` | 2026-10-10 | Add shift selection and scoped import of all native history pages |
| `bd2bdc4c` | 2026-10-10 | Read OpenCode SQLite token history and suppress copied fork usage |
| `97be6943` | 2026-10-10 | Bind memory scan completion to its original data root |
| `735cf9f0` | 2026-10-10 | Respect selected workspaces when deleting from the context menu |
| `b5f70910` | 2026-10-10 | Read Copilot durable token accounting without counting repeated snapshots |
| `dae51b1c` | 2026-10-10 | Handle provider-neutral ACP context usage updates |
| `4a97cade` | 2026-10-10 | Persist memory scan progress without rewriting clean transcripts |
| `467a1e96` | 2026-10-10 | Read Gemini saved-session token usage with bounded snapshot scans |
| `8a8b6426` | 2026-10-10 | Persist queued prompt edits without rewriting transcripts |
| `7ad036b6` | 2026-10-10 | Filter usage totals and chart periods by provider |
| `b15c6541` | 2026-10-10 | Add a T3-style usage workspace with explicit cost coverage and model prices |
| `3d10fc0c` | 2026-10-10 | Expose bounded local usage history through the background CEF bridge |
| `dd3c4bf6` | 2026-10-10 | UAM checkpoint: chat-1791644514970-b331a2 turn 238 |
| `5d8f56fd` | 2026-10-10 | Normalize historical usage and cache incremental transcript scans |
| `c930cee0` | 2026-10-10 | fix(remote): preserve host options when startup setup fails |
| `8394c812` | 2026-10-10 | perf(memory): capture extraction text without unused payloads |
| `2679fe7d` | 2026-10-10 | Avoid duplicate Codex workspace instructions and preserve startup errors |
| `b2431cd2` | 2026-10-10 | Show history loading and empty states without losing drafts |
| `fd468d15` | 2026-10-10 | Use a provider sidebar and favorites in the model picker |
| `ea554c77` | 2026-10-10 | fix(remote): make Linux npm update elevation opt-in |
| `4909419e` | 2026-10-10 | Show compaction metrics and agent activity with user-only message navigation |
| `ca32f194` | 2026-10-10 | feat(chat): show when the parent is waiting on subagents |
| `93ae7509` | 2026-10-10 | feat(chat): show native compaction while it is in progress |
| `803a84a0` | 2026-10-10 | fix(bridge): keep metadata work on CEF-owned background tasks |
| `cc71942f` | 2026-10-10 | feat(chat): navigate large histories by message position |
| `f592a03f` | 2026-10-10 | perf(runtime): bound read paths and offload memory counts |
| `85e77fb2` | 2026-10-10 | feat(review): add source line gutters and bound large diff rendering |
| `8fd1180e` | 2026-10-10 | fix(ui): reject stale terminal output and disabled model selections |
| `75f37f0a` | 2026-10-10 | feat(chat): add searchable keyboard-accessible model selection |
| `6b0c1fc3` | 2026-10-10 | feat(settings): add persisted typography controls and live terminal fonts |
| `0a5eab16` | 2026-10-10 | feat(ui): rotate the blue working indicator with reduced-motion support |
| `a1d4328b` | 2026-10-10 | perf(runtime): prepare polling, saves and recent history off the UI thread |
| `3986d497` | 2026-10-10 | perf(chat): reuse live timeline renders during composer typing |
| `9c757676` | 2026-10-10 | Prevent remote installer lock inheritance by helper processes |
| `5257d14e` | 2026-10-10 | Report resident provider PIDs blocking remote CLI updates |
| `b32c6f56` | 2026-10-10 | Honor provider and host MCP overrides during session launch |
| `d9a7cba9` | 2026-10-10 | UAM checkpoint: chat-1791323829636-0ead76 turn 193 |
| `0f2a3740` | 2026-10-10 | fix(chat): preserve native ownership through deletion recovery |
| `15a5f428` | 2026-10-10 | fix(claude): persist safe five-hour quota continuations |
| `d1b0195b` | 2026-10-10 | fix(claude): settle native children and gate unfinished reviews |
| `b6a39986` | 2026-10-10 | perf(chat): reuse unchanged turn timestamp rendering |
| `49947df9` | 2026-10-10 | fix(mcp): honor opt-out and reset warnings after runtime restart |
| `568c47bc` | 2026-10-10 | Prepare native child history asynchronously and reuse selected digests |
| `a62e3946` | 2026-10-10 | Move cold ACP prompt hydration behind cancellable admission |
| `52b974c4` | 2026-10-10 | fix(runtime): prepare cold CLI handoff history on workers |
| `556fd790` | 2026-10-10 | fix: own cold branch preparation through cancellation |
| `f1af37af` | 2026-10-10 | Avoid retry history scans while typing in chats |
| `239ec841` | 2026-10-10 | Align queued file integration assertion with named attachments |
| `f0194ba3` | 2026-10-10 | Preserve cold history when resuming managed runs |
| `621cd4f3` | 2026-10-10 | Show queued file names and clarify resume action |
| `7158ac0a` | 2026-10-10 | Close the goals popup on Escape from the composer |
| `f5eb4432` | 2026-10-10 | Ignore hidden and closed modals when focusing chat inputs |
| `e64dd5b4` | 2026-10-10 | Guard manual provider update admission against repeated clicks |
| `a71e134c` | 2026-10-10 | Verify image fallback filename and full-path tooltip |
| `04bbf937` | 2026-10-10 | Hide change reviews for active goals and clear unavailable VCS results |
| `7e672ebc` | 2026-10-10 | Allow attachment removal and attachment-only edits in queued prompts |
| `e80cf346` | 2026-10-10 | Release settled import review ownership and distinguish saving |
| `8c374422` | 2026-10-10 | Keep provider settings actions scoped to the selected host |
| `8dd484ce` | 2026-10-10 | Keep MCP import editor multiline and space workspace labels |
| `72f5b9bc` | 2026-10-10 | Load cold idempotent branch replay histories off the UI thread |
| `00359ff8` | 2026-10-10 | Reuse full state summaries when resetting patch baselines |
| `52582235` | 2026-10-10 | Read settled managed transcripts outside the UI thread |
| `4b3c6522` | 2026-10-10 | Avoid copying full source transcript when creating a branch |
| `46893bcb` | 2026-10-10 | Stage cold chat deletion outside the UI thread |
| `a16688b9` | 2026-10-10 | fix: load deferred tool output off the GUI thread |
| `dbf78ca4` | 2026-10-10 | fix: preview saved attachments without hydrating cold chats |
| `7683a880` | 2026-10-10 | fix: roll back checkpointed turns from cold saved chats |
| `48d793e3` | 2026-10-10 | fix: preserve native history tokens across paged loads |
| `0e6509b8` | 2026-10-10 | Support safe attachment retention when editing queued prompts |
| `83accfc9` | 2026-10-10 | Release saved hidden history for paused and blocked goals |
| `105c06a6` | 2026-10-10 | Page imports within their workspace and reject cross-workspace decisions |
| `308f030b` | 2026-10-10 | Keep child work visible and defer change review until all work finishes |
| `f5dd495a` | 2026-10-10 | Use shared workspace field styling and actionable chat creation errors |
| `98054205` | 2026-10-10 | Align recorded edit controls and unknown provider icons with shared UI |
| `578e7ba1` | 2026-10-10 | Keep OpenCode import warnings visible through review and save |
| `a1b3f5d6` | 2026-10-10 | Distinguish unknown provider connections and preserve setup errors |
| `0a66678c` | 2026-10-10 | Keep MCP warnings aligned with verified session availability |
| `7680c61f` | 2026-10-10 | Leave startup chat history cold until a bounded page is requested |
| `3f1d4c00` | 2026-10-10 | Preserve parent workspace agent definitions in delegated worktrees |
| `c931d616` | 2026-10-10 | Keep queued edit keyboard shortcuts inside the editor |
| `16263432` | 2026-10-10 | Restore the last successfully selected chat in each branch family |
| `930622eb` | 2026-10-09 | Keep paged cold chat history detached from native state |
| `d35d6869` | 2026-10-09 | Preserve MCP editor identity and reject reserved external definitions |
| `4ee06ca6` | 2026-10-09 | fix: page desktop chat history and retain absolute message positions |
| `97da888f` | 2026-10-09 | fix: show scoped MCP switches and live connection checks |
| `8799c8a5` | 2026-10-09 | fix: scope provider MCP setup and keep frontend state summaries bounded |
| `86d8a83b` | 2026-10-09 | fix: preserve MCP import settings and show actionable source errors |
| `6b0d2edf` | 2026-10-09 | Consolidate provider settings and expose verified MCP setup status |
| `4019e729` | 2026-10-09 | Prepare Claude image delivery asynchronously within transport limits |
| `7083b6d4` | 2026-10-09 | fix(chat): show image collections and edit attachment retention |
| `3967ee0c` | 2026-10-09 | fix(remote): preserve configuration startup failures before turn creation |
| `b86b296a` | 2026-10-09 | fix(updates): recheck remote activity and honor package manager targets |
| `bd917f49` | 2026-10-09 | fix(workspaces): show deletion progress and native failure details |
| `51a49c58` | 2026-10-09 | fix(sidebar): label worktrees with their original workspace |
| `f9072612` | 2026-10-09 | Handle long Claude native history directories on Windows |
| `9c076044` | 2026-10-09 | Fix Windows atomic writes and cleanup near path limits |
| `da5cf88c` | 2026-10-09 | Bound Windows worktree paths without changing branch names |
| `2ebb9e66` | 2026-10-09 | Render lazy image previews with persisted attachment identity |
| `b23c9234` | 2026-10-09 | Cache bounded native image previews for saved attachments |
| `376f25a8` | 2026-10-09 | Fix history hydration for chats with missing timestamps |
| `7c909b0d` | 2026-10-09 | Preserve POSIX quoted backslashes and line continuations |
| `a8c56656` | 2026-10-09 | Preserve cached chat history during worktree operations |
| `95c090ba` | 2026-10-09 | Preserve reselected chat history during idle transitions |
| `aafb2372` | 2026-10-09 | Fix Windows runner output spools beyond MAX_PATH |
| `0f7a2ac1` | 2026-10-09 | Discard inherited OpenSSH state when replacing Windows stdio |
| `60cdb917` | 2026-10-09 | Fix fresh Ninja UI builds and preserve flat output |
| `4c0deeea` | 2026-10-09 | Fix Windows atomic file I/O beyond MAX_PATH |
| `9f3528b7` | 2026-10-09 | Drain active remote shutdown pipes without artificial delay |
| `b2560949` | 2026-10-09 | Make provider and import fixtures portable on Windows |
| `2b77c86a` | 2026-10-09 | Fix Windows history import failure fixture |
| `4d280fbc` | 2026-10-08 | Fix Windows direct runner binary framing |
| `58ecf178` | 2026-10-08 | Allow remote Stop confirmation to finish before timing out |
| `e90a06c1` | 2026-10-08 | Use completed Codex text for suggestions and memories |
| `53fe4449` | 2026-10-08 | Read completed Codex decisions in permission reviews |
| `fdfad9b8` | 2026-10-08 | Fix rooted remote context and memory path traversal |
| `1b7f765f` | 2026-10-08 | test(macos): assert deferred Quit coalescing and close retries |
| `d40d91ce` | 2026-10-08 | fix(macos): defer repeated Quit until picker cancellation unwinds |
| `efa5426b` | 2026-10-08 | fix(macos): accept Cmd+Q with Caps Lock enabled |
| `1af6c064` | 2026-10-08 | fix(macos): complete Quit after native path picker cancellation |
| `ffd53772` | 2026-10-08 | fix(macos): route application Quit through CEF shutdown |
| `6e0988c0` | 2026-10-08 | fix(remote): drain idle shutdown stdout before confirming Stop |
| `35deb221` | 2026-10-08 | fix(remote): confirm idle Stop outcomes during shutdown |
| `2de39259` | 2026-10-08 | fix(remote): confirm missing-process stops during shutdown |
| `737af2d8` | 2026-10-08 | fix(macos): retain reaped stdio exit status while draining |
| `c2d03912` | 2026-10-08 | fix(remote): isolate pending Stop output from replacement sessions |
| `ca465188` | 2026-10-08 | fix(remote): retain the final provider line during Stop cleanup |
| `d724be5a` | 2026-10-08 | fix(import): retain side-chat transcripts without source cleanup authority |
| `85953c16` | 2026-10-08 | fix(collections): preserve newer names across delayed rename replies |
| `a8943270` | 2026-10-08 | Keep runner completion and output snapshots consistent |
| `43ea218f` | 2026-10-08 | Settle remote interactions after authenticated provider exit |
| `47c837ff` | 2026-10-08 | Bind model catalogs to provider connection ownership |
| `3fb5d44b` | 2026-10-08 | fix(memory): preserve unowned Gemini histories during worker cleanup |
| `97175660` | 2026-10-08 | fix(import): prevent native duplicates across legacy remote folder aliases |
| `27c6ccb8` | 2026-10-08 | fix(import): release stale request guard during effect replay |
| `b20a2b9b` | 2026-10-08 | fix(goals): restore composer request ownership after effect replay |
| `c75e5d85` | 2026-10-08 | fix(models): preserve literal backslashes in remote POSIX workspaces |
| `3e7b3cee` | 2026-10-08 | fix(workspaces): reject duplicate canonical directory owners |
| `c82f4bf5` | 2026-10-08 | fix(claude): retain live background tasks across idle proxy recovery |
| `be5f47ef` | 2026-10-08 | fix(import): report selected chats discarded before publication |
| `a5e385d3` | 2026-10-08 | Name cached native history review accurately in folder controls |
| `ed7fafb2` | 2026-10-08 | Review cached startup history before importing selected chats |
| `849cbf60` | 2026-10-08 | Preserve provider startup errors after rejected remote input |
| `9e6bd219` | 2026-10-08 | Keep routine CLI version notices out of new chat creation |
| `d0f20c39` | 2026-10-08 | Move chat goals into composer controls and guard async ownership |
| `8bf5679b` | 2026-10-08 | Keep host instruction checks from submitting options |
| `3f0eafb2` | 2026-10-08 | Support Antigravity native streaming with isolated configuration |
| `93e956c9` | 2026-10-08 | Expose scoped eligible providers for managed subagents |
| `1299fc5f` | 2026-10-08 | Expose guarded queue controls and SSH routing in chat views |
| `0571523c` | 2026-10-08 | Guard queued prompt edits and hold restored intent until resume |
| `1f27db6d` | 2026-10-08 | Wire organization undo and attention navigation shortcuts |
| `297f16bd` | 2026-10-08 | Coalesce terminal reconnect requests during pending reads |
| `858cfdf6` | 2026-10-08 | Review recorded chat edits and preserve newer commit drafts |
| `a7e71257` | 2026-10-08 | Wire guarded chat state actions and bounded search navigation |
| `5a312268` | 2026-10-08 | feat(routing): admit threads to explicitly configured SSH pools |
| `41068cfd` | 2026-10-08 | feat(navigation): choose the next chat needing attention |
| `871ffff0` | 2026-10-08 | feat(search): return bounded hits with guarded transcript provenance |
| `b2828153` | 2026-10-08 | perf(history): release only durable inactive transcripts |
| `77e008cf` | 2026-10-08 | feat(organization): apply guarded reversible collection changes |
| `2abf6b8a` | 2026-10-08 | feat(review): expose immutable provider-reported edit receipts |
| `34d1feeb` | 2026-10-08 | chore(version): align local source with public main 4.11.0 |
| `a0db11db` | 2026-10-08 | fix(chat): keep secondary composer controls from sending drafts |
| `9723ea76` | 2026-10-08 | Keep checkpoint finalization from overwriting queued intent or newer turns |
| `60836523` | 2026-10-08 | Show session account allowances and clear them for a fresh Codex login |
| `14196692` | 2026-10-08 | Reject memory scan results after their workspace ownership changes |
| `04638da5` | 2026-10-08 | Add atomic host admission with durable assignment ownership |
| `f9814b11` | 2026-10-07 | Keep modal keyboard and focus ownership on the top dialog |
| `d1fd87b7` | 2026-10-07 | Pin running chats from their context menu without changing selection |
| `b64547a0` | 2026-10-07 | Initialize browser storage independently for every store test |
| `2e604da7` | 2026-10-07 | Discover startup model scopes without taking over user conversations |
| `17a24055` | 2026-10-07 | Acknowledge accepted remote input after the provider exits |
| `236c65e8` | 2026-10-07 | Inherit model options only within the same child provider |
| `b6946af0` | 2026-10-07 | Keep native history imports under registered workspace ownership |
| `4f209a22` | 2026-10-07 | Focus the active composer or terminal with a scoped keyboard shortcut |
| `e307093b` | 2026-10-07 | Render contiguous assistant text without empty message action rows |
| `5a4bf126` | 2026-10-07 | Search grouped workspaces from the new-chat picker |
| `a6519e8b` | 2026-10-07 | Reuse cached models without interrupting existing chats |
| `6c2a9dfb` | 2026-10-07 | Use official OpenCode Gemini CLI and Antigravity marks |
| `5cf984aa` | 2026-10-07 | Show the owning host on Active chat rows |
| `4962bbf8` | 2026-10-07 | test(acp): ignore Windows echo fixture's trailing blank line |
| `2edc6a1a` | 2026-10-07 | fix(acp): settle permissions arriving during turn cancellation |
| `e5e51e91` | 2026-10-07 | feat: add terminal-only Antigravity CLI provider |
| `a84addc5` | 2026-10-07 | Read worktree collision refs without shell format escaping |
| `a2e96a6f` | 2026-10-07 | Preserve UTF-8 when constructing readable worktree paths |
| `fa8fb4c7` | 2026-10-07 | Keep compaction ownership through stale completions and streamed errors |
| `a0b34bbb` | 2026-10-07 | Add native manual context compaction for local Codex sessions |
| `0f070319` | 2026-10-07 | fix: bound provider handoff history while preserving recent context |
| `5a17842f` | 2026-10-07 | fix(chat): preserve Claude tool history and task ownership |
| `7b49968d` | 2026-10-07 | fix: retry failed agent cancellations without relaunching queued runs |
| `a67456a1` | 2026-10-07 | Route compact composer actions through native session control |
| `64224774` | 2026-10-07 | Prepare readable isolated worktrees before first chat dispatch (#401 #405) |
| `affdb43a` | 2026-10-07 | Use provider catalogue and remove redundant tooltip root |
| `fe456c3e` | 2026-10-07 | Preserve skill attachments staged during pending sends |
| `6dda7a5f` | 2026-10-07 | Keep memory library responses owned by the latest visible request |
| `0fc0b984` | 2026-10-07 | Fix reconciliation of remote recovery-only updates |
| `5f8fd137` | 2026-10-07 | Fix composer workspace actions and missing-commit worktree errors (#425) |
| `5ba483ba` | 2026-10-07 | UAM checkpoint: chat-1791323829636-0ead76 turn 13 |

## Feature branch coverage check

Checked 2026-10-11 against `uam/v5-improvements` at `5d5e40a3` and `codex/t3-inspired-features` at `6079c141`. The V5 branch contains the complete feature branch history, with zero missing commits. All 117 non-merge commits since the feature branch was created at `d9a7cba9` now appear in the historical inventory; merge commits remain excluded from that inventory. The original Copilot mark and helper-manifest commits supplement existing descriptions of the same work. The collapsed-sidebar entry retains its pending acceptance flags and now explicitly requires Claude Review. No implementation, packaged source pin, user sign-off or outstanding-work ownership changed during this check.

A follow-up work-summary audit made VCS settings and reported subagent activity explicit, clarified background usage retention and Qwen reference pricing, and retained native interaction, loading, provider accounting and SSH skill-prompt acceptance in their existing open rows. macOS Dock verification and the low-priority context-pollution report are now explicit remaining work; the PR workflow and auto-settling idea remains record-only at the end of the incomplete-work list. No product code, acceptance approval or existing owner changed.

### Execution receipt, 2026-10-11

David explicitly authorized the orchestration goal and implementation. Mock-event-to-first-response unit tests are an accepted primary mechanism for code-path regression budgets; graphics or native responsiveness claims require separate evidence. No optimizations are included in the timing-test packet, no RAM budget is introduced, and hardware is the machine running the test. Current UAM still exposes only a read-only UI reviewer and reports mutationToolsEnabled=false; no implementation agents have been launched or assigned fabricated identities. Serial coordinator work continues on the same allocated scope while delegation remains unavailable. New confirmed or suspected bugs are recorded in the Incomplete Work table with evidence and no unsupported fixed claim.

### Timing test implementation receipt, 2026-10-12

Coordinator ownership remains chat-1791755529536-1b146e in build/v5-bug-fix-swarm. This serial packet owns CMakeLists.txt, UI-V2/package.json, UI-V2/src/test/interactionBudgets.ts, UI-V2/src/store/interaction.performance.test.ts, UI-V2/src/components/views/ChatView.performance.test.tsx and tests/interaction_performance_tests.cpp. No existing product UI component or production service was changed. Normal npm test now runs the original parallel unit suite and then all .performance.test files with one worker, preserving both test inventories and unchanged limits. Fake timers are rejected by the timing helper. Every performance sample is a hard-ceiling check, not a median acceptance rule.

Baseline verification: 1450 frontend tests, 1540 compiled native tests and 19 CTest gates all passed; native fixtures require temporary socket and macOS IPC permissions outside the filesystem sandbox. Added coverage: 19 frontend cases and eight native cases, with the compiled native inventory matching 1548 names and zero baseline removals. Focused native measurements: 20/50/200-message cold-state page responses 0.8/0.9/2.6 ms, 10000-message response 118.2 ms, cached pages 0.7–0.9 ms, initial-state summary service 8.5 ms and settings save/load 6.4/0.2 ms. These measurements exclude fixture creation, do not represent cold OS disk caches or a complete application launch, and do not establish visual or platform acceptance. Final verification passed before the implementation commit: standard npm test ran 1450 ordinary cases plus 19 timing cases, the frontend and full native Release builds passed, all 1548 native names matched the compiled inventory without missing or failing tests, all 19 CTest gates passed in 256.09 seconds, and git diff --check passed.

Remaining timing scope stays assigned: actual application startup readiness, the remaining interaction categories in the coverage table, provider cache refresh/error journeys and more simultaneous-work fixtures. New findings and uncertainty remain in the Incomplete Work table; no entry is marked Complete and existing Claude review or user-test flags remain.

### All Time backend investigation, 2026-10-12

The existing All Time Usage Range row remains owned by chat-1791755529536-1b146e. The initial source trace found a 90-day boundary in Collector::Run admission, HistoryService window validation, Store::ReadRecords selection and destructive pruning, Ledger creation/decoding/query, MultiHostHistory peer validation and CEF/frontend request validation. At that baseline Store::ReadRecords deleted old ordinary facts before returning the recent window; merely widening the selector or ledger query would therefore produce incomplete totals. Implementation must preserve durable older daily accounting before that pruning, retain idempotent receipt and mutable-record accounting, preserve cross-host deduplication and Antigravity alias ownership, and keep bounded query/transfer behavior. Migration cannot recreate facts already deleted unless their original provider histories still exist; coverage must remain explicit. No usage data, provider histories or production services were changed during this investigation. The graphical selector remains a coordinated Claude handoff; no frontend files are claimed from Claude for this packet.

### Full-scope execution and parent replay, 2026-10-12

David reiterated that the goal covers all eight allocated packets, not just performance tests. The existing active internal goal already covers all 48 original allocated items and remains active. A fresh UAM check still exposes only ui-consistency-review with read workspace access and mutationToolsEnabled=false. No implementation delegates have run; the coordinator continues serially without bypassing UAM delegation restrictions. MCP secret exposure tracing identifies the raw connection summary in Claude-owned McpServerSetup.tsx, so its frontend fix remains a coordinated handoff.

P3 ownership for this bounded fix is chat-1791755529536-1b146e in build/v5-bug-fix-swarm: tests/claude_projection_tests.cpp, src/common/provider/claude/cli/claude_acp_message_handlers.cpp and src/common/state/app_state.h. ClaudeParentEnvelopeReplayDoesNotDuplicateNarration failed before the production change when one parent envelope was delivered twice. The guard uses native envelope identity, retaining identical text under a distinct identity and leaving envelopes without identity on their existing path. The expanded regression also verifies nonconsecutive replay. Full native Release application/test builds passed, both focused parent-replay and child-ownership regressions passed, all 1549 native cases matched the expected compiled inventory with zero removals, failures or missing names, and all 19 CTest targets passed. This validates the isolated duplicate-delivery fix, not an authenticated reproduction of David's entire incident or completion of P3. Claude Review and user acceptance remain required. UAM automatically checkpointed the initial unverified edits as a178ffdb; coordinator integration follows completed verification, without rewriting that external checkpoint.

### Native permission translation packet, 2026-10-12

P2 remains owned by chat-1791755529536-1b146e in build/v5-bug-fix-swarm. The bounded Claude implementation owns tests/core_tests.cpp, tests/claude_projection_tests.cpp, src/common/provider/claude/cli/claude_cli_provider_runtime.cpp, src/common/runtime/acp/acp_session_runtime.cpp and src/common/runtime/acp/acp_permission_handlers.cpp. Installed Claude Code 2.1.296 advertises auto and acceptEdits; current UAM structured launches force default except Plan. The [official permission-mode documentation](https://code.claude.com/docs/en/permission-modes) confirms default remains the SDK/config name for Manual and auto uses a native classifier with availability fallback. This is a requested translation feature, not a claim that default is invalid. Map UAM automatic and edit permission choices into supported native modes, preserve explicit Plan/read-only ceilings, avoid redundant UAM AI review only after native auto is observed, keep fallback prompts user-resolvable, and verify turn preparation does not restart an already-correct native auto session. Other provider translations, authenticated acceptance and Claude Review remain pending. No Claude-owned UI component is included.

The same owner additionally claims src/common/runtime/acp/acp_session_state_helpers.h for the shared mode-matching boundary used by both turn-preparation checks. Before production edits, the native-mode translation and redundant parent-review regressions failed as intended. A third regression confirmed the parent attempts an automatic response to an edit prompt that survives native auto/acceptEdits, which would bypass the native residual permission decision with an open transport. The guard now leaves those requests for the user after enforcing the existing read-only ceiling. This finding belongs to Native Automatic Mode Translation rather than a duplicate task. Four new regressions cover native mapping, observed-auto versus fallback review, residual edit prompts and restart-mode matching across Plan, Off and OpenCode. The existing ClaudeStructuredArgvIgnoresUamCommandSafetyTier test is intentionally renamed ClaudeStructuredArgvTranslatesUamEditTier and now tests the requested native edit behavior; its Plan assertion remains. The completed isolated verification is recorded below; live acceptance and whole-packet completion remain pending.

Claude translation verification: full native Release application/test build passed; six focused regressions passed; all 1553 native PASS names exactly matched the independently tracked compiled inventory, with four additions, one explicit rename and no removed cases; all 19 CTest targets passed in 233.79 seconds; git diff --check passed. This replaces the pending verification state above for the isolated Claude implementation, not live provider, graphical or whole-P2 acceptance. The existing frontend source is unchanged. Next Codex investigation uses installed CLI 0.162.1's generated schema: approvalsReviewer is supported on thread/start, thread/resume and turn/start, and thread/settings/updated reports the effective setting. UAM currently omits it and its per-turn prompt_chat also omits command_safety_tier; both producer and consumer need coverage before claiming native translation. No Codex production changes or actual agent delegations have occurred yet.

### Codex permission continuation ownership, 2026-10-12

Coordinator chat-1791755529536-1b146e in build/v5-bug-fix-swarm owns this P2 continuation: CMakeLists.txt, tests/codex_permission_tests.cpp, src/common/state/app_state.h, src/common/provider/codex/cli/codex_cli_provider_runtime.cpp, src/common/provider/codex/cli/codex_acp_message_handlers.cpp, src/common/runtime/acp/acp_session_lifecycle.cpp and src/common/runtime/acp/acp_permission_handlers.cpp. This is serial work after the verified Claude implementation, so shared state and permission files have one owner. Use the installed 0.162.1 schema and [official configuration reference](https://learn.chatgpt.com/docs/config-file/config-reference) for reviewer routing, preserve on-request sandbox enforcement, cover fresh/resumed thread and per-turn producers, restrict managed read-only review setup/turns, and use identity-scoped effective reviewer observations before suppressing parent review. Legacy or unconfirmed reviewer state retains UAM fallback handling; residual native prompts remain user-resolvable. No existing Claude UI file is claimed, no actual native reviewer or implementation delegate has been launched, and remaining providers and live acceptance stay open.

The coordinator also owns src/common/provider/codex/codex_options.h for normalization shared by setup responses and native settings notifications. Six regressions failed before production edits for missing fresh/resume and turn reviewer routing, omitted lifecycle safety-tier propagation, ignored thread-scoped reviewer observations, parent auto-grant of a residual native edit prompt and missing process-reset observation. The requested builders, lifecycle propagation, Plan/read-agent/goal-review sandbox bounds, effective reviewer parsing and residual-prompt guards are now implemented with final isolated verification recorded below. Invalid, unknown or unscoped reviewer observations retain parent fallback; the native legacy guardian_subagent enum normalizes to auto_review. No live model call, native reviewer, remote host or production data was used for this reproduction. Claude Review and other-provider/live acceptance remain pending.

The full native build and six focused Codex regressions passed. The first full run recorded 1558 passes and one failure across all 1559 expected cases, with 18/19 CTest targets passing. CodexAppServerRequestBuildersUseCodexProtocolMethods retained the old workspace-write expectation while constructing explicit Plan chats, including its YOLO-plus-Plan cases. The same coordinator additionally owns tests/core_tests.cpp for updating those four assertions to read-only and adding explicit user-reviewer assertions; protocol, model, history and collaboration checks remain. This is the intentional native Plan ceiling change, not removal of a regression or a green-suite claim. A full rebuild and rerun remained required before integration, with final evidence recorded below.

MCP summary coordination evidence: the coordinator worktree's McpServerSetup.tsx renders the raw URL or executable plus arguments in its row summary; Claude's current branch independently still computes launch from the same raw fields and renders it in uam-prov-detail. The configuration editor needs its real values to preserve editing and launch behavior, so deleting credentials from backend storage is not a summary fix. The component remains Claude-owned and no UAM chat-message tool is available. The existing exposure task is therefore Blocked for UI coordination, retaining coordinator ownership rather than making overlapping frontend edits or claiming this credential display is fixed. Backend packets continue independently.

Codex verification after the intentional Plan expectation update: full native Release application/test builds and the six new focused regressions passed, as did CodexAppServerRequestBuildersUseCodexProtocolMethods; the final full run matched all 1559 expected native PASS names with no missing, unexpected, duplicated or failed cases; all 19 CTest targets passed in 237.08 seconds. The earlier 1558-pass/one-failure receipt remains above. The frontend source is unchanged, no live provider/model review was launched, and Claude Review and authenticated acceptance remain required. The installed schema also defines guardianWarning notifications that this handler does not yet capture; native warning diagnostics remain explicit follow-up within Native Automatic Mode Translation rather than an unsupported completed claim.

Native warning continuation uses the same coordinator and worktree, owning tests/codex_permission_tests.cpp and src/common/provider/codex/cli/codex_acp_message_handlers.cpp. CodexNativeReviewerWarningsAreScopedAndDoNotLeakSensitiveContent failed before the handler change because current-thread guardianWarning was discarded. The handler now records bounded, thread-scoped diagnostics, withholds sensitive message content and leaves processing, error and pending-permission state unchanged; foreign or unscoped warnings cannot affect another chat. This follows the existing noncritical-warning logging behavior and adds no new GUI component or authored message copy. Final isolated verification is recorded below; Claude Review remains required.

New shared-cap finding is recorded as Diagnostic UTF8 Boundary Safety and claimed by coordinator chat-1791755529536-1b146e in the same worktree before production edits. Additional owned files are src/common/runtime/acp/acp_diagnostics.cpp and tests/core_tests.cpp. CapDiagnosticString currently cuts raw bytes, including inside multibyte code points; producers include provider errors and native warning text, and consumers include log sanitization and strict JSON bridge/state serialization. A regression exercises every boundary of two-, three- and four-byte code points, alongside the actual native-warning consumer. The reported historical chat-history incident is not assumed equivalent without live evidence. The initial finding was unverified; the red and final verification receipts follow below.

Both UTF8 regressions failed before the shared fix with strict JSON invalid-UTF8 exceptions. CapDiagnosticString now backs up from a cut inside continuation bytes and reports the actual omitted byte count, preserving the existing ASCII behavior and bounded prefix. Final build and integrated verification are recorded below; the native warning and shared UTF8 tasks retain review/user acceptance flags.

Native warning/shared cap verification: full native Release application/test build passed; both focused regressions passed; all 1561 native PASS names exactly matched the compiled inventory with two additions and no removed, missing, unexpected, duplicated or failed cases; all 19 CTest targets passed in 237.63 seconds; git diff --check passed. The pre-fix ignored-warning and two invalid-UTF8 failures remain recorded. This completes the isolated diagnostic implementation, while Claude Review, user/live acceptance and the remaining native-provider work remain open. The frontend source is unchanged.

### Parent AI review input boundary ownership, 2026-10-12

Coordinator chat-1791755529536-1b146e in build/v5-bug-fix-swarm owns the next P2 Broken AI Review continuation: src/common/runtime/acp/acp_permission_handlers.cpp, src/common/runtime/acp/acp_session_internal.h and tests/acp_session_tests.cpp. The current builder clips individual normalized fields and then clips the whole serialized prompt to 16 KiB, potentially omitting a request suffix or cutting encoded JSON; native provider input/approval payload fields are also absent from that prompt. Establish a directly testable builder boundary, reproduce incomplete/truncated review input, preserve complete normalized and original provider payloads when bounded, and retain manual decisions when a whole input cannot fit. This is request-boundary work, not a historical Un-Reviewed audit or a claim of authenticated review acceptance. No worker may be launched on unreviewable partial input; no live model calls are required for these regressions.

Two builder regressions failed before the behavior change for modified content and accepted truncated input. The bounded builder now preserves exact normalized content and original native provider JSON, rejects malformed/non-object original payloads and checks the final escaped prompt against the unchanged 16-KiB limit. Its caller rejects unreviewable input before selecting or starting a worker and scans the complete review prompt for existing sensitive markers. A third runtime regression exercises OpenCode fallback gating without a reviewer process: oversized and sensitive-original-input requests remain pending for the user. Final isolated verification is recorded below. Installed OpenCode 1.18.35 and the [official permission documentation](https://opencode.ai/docs/permissions/) identify --auto as unconditional approval of non-denied requests, so it is not substituted for an AI classifier. OpenCode retains the configured parent-review path; authenticated and full acceptance remain pending. The new fallback diagnostic retains Claude Review.

Complete-input verification: the full native Release application/test build and all three focused regressions passed; all 1564 native PASS names exactly matched the expected compiled inventory with three additions and no removed, missing, unexpected, duplicated or failed cases; all 19 CTest targets passed in 237.41 seconds; git diff --check passed. The pre-fix failures remain recorded. This completes the isolated input-boundary fix, not authenticated verdict delivery, whole Broken AI Review acceptance or the remaining provider matrix. The frontend source is unchanged and no live reviewer/model call was made.

### Pre-dispatch durability continuation, 2026-10-12

P1 Asynchronous Send Durability remains owned by coordinator chat-1791755529536-1b146e in build/v5-bug-fix-swarm. Source tracing identifies DrainNextQueuedAcpUserPrompt's full SaveChatQuietly barrier after recording acp_dispatched_queued_prompt_count and before StartAcpUserPromptBatch. The existing queue journal supports bounded durable outbox updates, while FlushPendingChatSaves already prepares transcript snapshots on a worker and publishes only matching current metadata/bodies. Merely replacing the dispatch barrier with a synchronous journal write would not fulfill the off-GUI requirement and could miss unsaved terminal history. The next implementation must preserve one-head admission, queue ordering, the dispatch marker and complete crash-recovery state, reject stale completion after cancellation/host or workspace changes, and serialize against later outbox/full saves before writing to the provider. No P1 production optimization was made during this trace. Shared native runtime, save/state and queue-test files remain coordinator-owned; no Claude UI file is included.

### Startup cache refresh continuation, 2026-10-12

Coordinator chat-1791755529536-1b146e owns src/app/model_discovery_service.{cpp,h} and tests/model_discovery_startup_tests.cpp in build/v5-bug-fix-swarm. The new restart regression failed because BeginDiscoveryIfStale suppressed a refresh for a recent persisted cache. Startup now uses BeginDiscovery after removing its scope from the once-only startup queue, retains cached choices during and after failed discovery, and skips any already pending explicit refresh. Five focused cases, the full native Release application/test build, all 1566/1566 expected core cases (two added, none removed or duplicated), and 19/19 CTest gates pass. Startup Model Catalogue Refresh remains In Progress with Claude Review, Un-Reviewed and Not User Tested. No real provider calls or Claude-owned UI edits were made. Per-second OpenCode config polling and broader startup/cache acceptance remain separate unfinished parts of the same packet.

Temporary Side Chat Context continuation lead: CreateTemporarySideChat inherits pendingGitWorktree from CreateNewChat; StartAcpUserPromptBatch and GitWorktreeService::PrepareForFirstLaunch exclude managed agents/goals but not temporary_parent_chat_id. The first-send path can therefore create an unwanted side-chat branch/worktree. This is tracked under the existing Temporary Side Chat Context row, not as a duplicate issue. Coordinator owns native chat-lifecycle/worktree/runtime and their isolated tests for this continuation; no production fix or reproduced test result is claimed yet.

### Temporary side-chat workspace continuation, 2026-10-12

Coordinator chat-1791755529536-1b146e owns src/app/chat_lifecycle_service.cpp, src/app/git_worktree_service.cpp, src/common/runtime/acp/acp_session_runtime.cpp and tests/vcs_settings_tests.cpp in build/v5-bug-fix-swarm. Both new regressions failed before the fix: newly created side chats retained pendingGitWorktree, and persisted temporary chats entered first-launch worktree preparation. Creation now clears the ordinary automatic-isolation request; shared preparation and its first-send caller exclude temporary chats, including persisted ones. The side chat keeps the parent's effective workspace without claiming ownership of the parent's worktree, branch or native session. Four focused cases, the full native Release application/test build, all 1568/1568 expected core cases (two added, none missing or duplicated), and 19/19 CTest gates (237.52 seconds) pass. Temporary Side Chat Context retains Claude Review, Un-Reviewed and Not User Tested; no UI files, real provider sessions or live workspace data were touched.

### Context pollution report, 2026-10-12

Owner: chat-1791755529536-1b146e, worktree build/v5-bug-fix-swarm. This is a source-backed report of UAM prompt assembly, not a latency benchmark or an audit of all historical changes.

| Source | Observed insertion and bound | Implication |
|---|---|---|
| Durable memory recall | BuildAcpBatchPrompt requests recall on the first user prompt; BuildRecallPreface defaults to 2,048 bytes, normalized configurable range 512–8,192 bytes, and returns empty when disabled or cancelled. | A long native conversation does not cause UAM to append the same memory preface on every ordinary turn. Provider-native recall is outside this UAM measurement. |
| Attached skills | BuildAcpPromptBody inserts each queued skill snapshot for the prompt that references it, with a shared 2-MiB attached-skill-content limit. Wrappers and the user prompt are additional bytes. | Skills can contribute much more text than default memory recall; the byte limit is not a token or total-provider-context ceiling. |
| Agent definition | BuildAcpBatchPrompt inserts a prompt-injected definition when the provider/definition hash differs from the last successfully delivered definition. Delivery stores the hash. | UAM avoids unchanged per-turn reinsertion on this path; native provider setup and files may add their own instructions. |
| Provider handoff | BuildProviderHandoffContext retains up to 32 KiB of recent conversation and summaries; BuildConversationHandoff rejects conversation snapshots over 256 KiB. BuildAcpBatchPrompt compares the recorded handoff session to the native session; delivery records the current session. | A genuine new destination session needs context again. Different handoff paths have different limits; these are not interchangeable total-context guarantees. |
| Temporary side chat | CreateTemporarySideChat captures at most 48 nonempty message excerpts within a 32-KiB context budget, with an 8-KiB suffix cap per oversized message. It disables UAM memory and uses a fresh native identity. | The temporary conversation has bounded reference context rather than a second native resume of the parent's complete history. New worktree prevention is verified separately above. |
| Goal continuation and review | BuildContinuationPrompt repeats the objective and applicable loop state; review uses separate review prompts. These are additional to conversation and skill context. | Long objectives or accumulated goal state can add repeated input. The current trace does not establish a performance defect or justify silently removing instructions. |

No measured provider time-to-first-token comparison, tokenizer inventory or provider-resident-context dump was collected. UAM's existing prompt_chars diagnostic measures assembled text length, not provider tokenization or complete resumed context. A useful next measurement would record only component byte counts and provider-reported input/cached tokens, with the same provider/model/workspace and prompt, across fresh, resumed, attached-skill, handoff and goal turns. Do not log prompt bodies or compare model latency without controlling provider/network variation. Context Pollution Report has its requested source report here; its acceptance flags remain until review and item-specific user acceptance.

### Parent-review completion continuation, 2026-10-12

Coordinator chat-1791755529536-1b146e owns src/common/runtime/acp/acp_permission_handlers.cpp and tests/acp_session_tests.cpp for the Broken AI Review continuation in build/v5-bug-fix-swarm. PollPermissionReviewTasks checks request ID, running state and AI Review tier but currently omits native ownership, provider-required user decisions and Plan/managed-read-only ceilings. The initial fixture used an incorrectly cased Plan value; after correcting it to plan, the old completion predicate again failed by sending an allow response. The restored shared launch/delivery predicate passes the corrected five-scenario regression plus three adjacent review tests, the full native Release application/test build, all 1569/1569 expected core cases (+1, zero missing/duplicate names), and 19/19 CTest gates in 238.37 seconds. The native completion predicate must be shared with review launch and rechecked before any response. Claude Review and user acceptance remain required. An adjacent iterator-invalidation lead exists because advancing a permission queue may append a review task while PollPermissionReviewTasks holds an iterator into that vector; this is a separate unreproduced lead under Broken AI Review, not a verified fix.

Memory Policy Dropdown continuation allocation: coordinator chat-1791755529536-1b146e owns src/common/memory/memory_levels.h, src/app/memory_service.cpp and tests/memory_tests.cpp for native Read Only semantics in build/v5-bug-fix-swarm. Read Only must preserve recall while excluding automatic/manual AI extraction, queued work and late worker publication; explicit library edits remain separate user actions. Current native normalization recognizes only Off/Strict/Balanced/Open. No new policy code is implemented yet. Frontend types/sanitizers and policy controls still need coordinated Claude UI work, including the requested Default selection; this native slice will not be presented as the complete six-option dropdown.

### Native Read Only memory continuation, 2026-10-12

The two corrected private-data regressions fail before the implementation: readOnly normalizes to Strict, and a Strict-prepared candidate still publishes after switching to readOnly. Fixture compile-type errors and an initial Strict-rejected seed were corrected before treating those results as evidence. Native normalization now preserves readOnly with recall enabled; AllowsCreation excludes Off and Read Only from extraction admission, shared parsed-output preparation and publication. This covers automatic/manual queues and late local/remote worker results without blocking explicit memory-library edits. Five focused cases, the full native Release application/test build, all 1571/1571 expected core cases (+2, none missing or duplicated), and 19/19 CTest gates in 237.99 seconds pass. The new diagnostic wording is "Memory creation is disabled for this chat." (humanizer pass: direct, concise technical wording). Memory Policy Dropdown retains Claude Review, Un-Reviewed and Not User Tested; frontend normalization/type plumbing, controls and Default inheritance remain unfinished and assigned for UI coordination. No provider model call or live memory write was made.

### Codex native child activity continuation allocation, 2026-10-12

Coordinator chat-1791755529536-1b146e owns src/common/state/app_state.h, src/common/provider/codex/cli/codex_acp_message_handlers.cpp, src/common/provider/codex/cli/codex_cli_provider_runtime.cpp, src/common/runtime/acp/acp_session_state_helpers.h, src/common/runtime/acp/acp_session_lifecycle.cpp, src/common/runtime/acp/acp_polling.cpp, src/cef/state_serializer.cpp and tests/acp_session_tests.cpp for Provider Subagent Activity Visibility/Persistent Processing Status continuation in build/v5-bug-fix-swarm. Installed Codex 0.162.1 experimental JSON schema documents receiverThreadIds, agentsStates with pendingInit/running/interrupted/completed/errored/shutdown/notFound, and thread/status/changed. Current projection records only the first linked receiver in a tool, while backgroundTaskCount and idle-shutdown guards omit native Codex child activity. The intended slice tracks only identities reported by the current native parent, consumes known-child status/close notifications without changing parent processing state, rejects foreign/unscoped activity, and clears ephemeral observations at process/thread ownership changes. No invented child IDs, persisted historical liveness, or pure UI-component edits are permitted. Red regressions precede production edits. Claude Review, Un-Reviewed and Not User Tested remain; complete child views and the orange in-chat status still require coordinated UI acceptance.

Codex child-activity progress: all three new scope/idle/reset regressions fail against the previous implementation because backgroundTaskCount remains zero. Ephemeral known/live UUID sets, parent-scoped agentsStates observations, known-child status/close notifications, serialization and blocking-work checks are implemented but not yet verified. Confirmed native source exit clears observations; proxy loss retains them, and fresh/thread ownership reset clears them. The shared blocking predicate also recognizes already-tracked native Claude work. A fourth private-process regression is being built for idle-parent cancellation, which currently returns success without a native cancel message when no parent turn ID exists. Coordinator additionally owns src/common/runtime/acp/acp_session_runtime.cpp for that scoped cancel/observed-stop/reconnect/shutdown continuation; this file was already coordinator-owned by P1/P3, and no worker or Claude component owns it. No runtime stop fix is claimed yet; all gates and Claude UI review remain pending.

Codex child-activity verification progress: the producer fixture initially sent collaboration output without admitting a parent prompt; it was corrected to set admitted processing and emit native turn-start/completion events, preserving the existing rejection of late output. The corrected three activity/scope/ownership cases pass. The fourth regression then reproduced idle-parent cancellation returning success without any observed owned-process stop; the scoped cancel path now uses the existing observed stop, preserves unrelated process fixtures, and retains child ownership until confirmation. Reconnect and shutdown paths recognize these native children too. Six focused cases, the full native Release application/test build, all 1575/1575 expected core cases (+4, none missing or duplicated), and 19/19 CTest gates in 237.25 seconds pass. Native child IDs and active observations stay ephemeral, UUID-validated and parent-scoped; old tool histories are not treated as liveness. No real Codex, SSH, model, daily-driver or Claude-component operation ran.

### Scoped provider availability allocation, 2026-10-12

Provider Enable Disable Controls remains owned by coordinator chat-1791755529536-1b146e in build/v5-bug-fix-swarm. Native ownership: src/common/models/app_models.h; new src/common/config/provider_availability.h; src/common/config/settings_store.cpp; src/common/config/settings_frontend_json.h; src/app/provider_resolution_service.cpp; src/app/model_discovery_service.cpp; src/app/provider_worker_command.cpp; src/app/uam_control_service.cpp; native ACP/terminal launch and queue handlers; src/cef/settings_handlers.cpp and query route declarations; new tests/provider_availability_tests.cpp and its CMake registration. Scope is independent provider/host overrides, default enabled, alias-normalized durable configuration, preventing new launch/discovery/submission/delivery when disabled while retaining active output, cached metadata, Stop and maintenance access. Optional user clarification about queued prompts is pending; absent steering after a reasonable response window, the chosen default is to hold accepted queue intent until re-enabled. All six implementations and histories remain supported. No UI component ownership is taken; frontend controls/types/state plumbing and every new visible refusal remain Claude Review. No implementation or passing regression is claimed yet.

Provider availability native implementation and verification are in progress; frontend controls remain gated on Claude coordination, with detailed evidence in `.UAM/AINotes/v5-swarm-plan.md`.

Startup catalogue continuation: coordinator `chat-1791755529536-1b146e` reserves `src/app/provider_model_catalog_service.{cpp,h}`, `src/cef/acp_handlers.cpp` and the matching native catalogue tests in `build/v5-bug-fix-swarm` to replace periodic local configuration checks with startup and explicit background refresh; the existing Startup Model Catalogue Refresh row retains all pending acceptance flags.

Native provider availability verification: Release application/test builds, seven focused cases, all 1582 expected native cases and 19 CTest gates pass; frontend controls, live acceptance and review remain open, with detailed receipts in `.UAM/AINotes/v5-swarm-plan.md`.

Startup catalogue verification: the Release app/test build, two focused regressions, all 1583 expected native cases and 19 CTest gates pass after removing periodic local config reads; broader startup timing, live and Claude acceptance remain pending, with receipts in `.UAM/AINotes/v5-swarm-plan.md`.

Asynchronous send continuation: coordinator `chat-1791755529536-1b146e` reserves the ACP send/queue/steer lifecycle and polling files, native chat persistence and deletion guards, any required durable-save service/state declarations, `tests/async_prompt_durability_tests.cpp` and CMake registration in `build/v5-bug-fix-swarm`; the existing Asynchronous Send Durability row remains In Progress with pending acceptance flags.

Claude integration receipt: this coordinator worktree fast-forwarded to local `uam/v5-improvements` at `25787be0`, retaining its uncommitted durability regression; all 1455 frontend unit and 19 serial timing tests, the production UI build and private native CEF app rebuild pass, with all existing acceptance flags preserved.

Asynchronous send implementation progress: the warm-send regression reproduced foreground transcript persistence and provider input before return; owned background publication, ordinary-send gating and durable CEF acknowledgement are verified, while delivery-marker, queue, retry, steering and recovery conversion remain unfinished.

Durability fixture continuation: coordinator `chat-1791755529536-1b146e` also owns `src/cef/acp_handlers.cpp`, the runtime acknowledgement header and the affected native compaction, branch-retry and Claude restart fixtures in `tests/core_tests.cpp`; acknowledgement/error timing requires Claude Review, and the full durability item remains unfinished.

Save-feedback continuation allocation: coordinator `chat-1791755529536-1b146e` owns `src/app/application.cpp` and the shared save receipt declarations/tests in `build/v5-bug-fix-swarm`; source tracing found that save completion after runtime polling can change errors and processing state without entering the conditional frontend state push.

Durability admission verification: Release app/test builds, all 22 affected cases, all 1589 expected native cases and 19 CTest gates pass, plus 227 frontend store cases; six new regressions cover pending feedback, complete unsaved history, deletion ownership, storage failure and reentrant receipts, while pending acceptance and remaining synchronous barriers are preserved in the Incomplete Work table.

Delivery-marker continuation allocation: coordinator `chat-1791755529536-1b146e` reserves the existing ACP lifecycle/state declarations and `tests/async_prompt_durability_tests.cpp` in `build/v5-bug-fix-swarm` for an immutable local request held across the asynchronous delivery-intent save; request-ID stability, cancellation, stale ownership and failure rollback must be verified before integration, and the original durability item retains In Progress and all acceptance flags.

Quota continuation fixture ownership: coordinator `chat-1791755529536-1b146e` owns the affected owner-poll loop in `tests/claude_projection_tests.cpp` for this delivery continuation; warm, cold, queued and goal cases must preserve the one-instruction and no-replay assertions.

Native picker verification allocation: coordinator `chat-1791755529536-1b146e` owns the narrow folder-picker termination failure in `tests/macos_application_tests.mm` and `src/common/platform/platform_application_macos.mm` in `build/v5-bug-fix-swarm`; the 1596 core cases pass, while the full-run native picker watchdog failure and its isolated passing rerun remain recorded for investigation.

Native picker continuation verification: the ignored-first-cancel fixture reproduced the original two-second watchdog failure; the modal-stop fallback passes all 14 picker scenarios and the Release app/test build, with pending Claude and user acceptance retained and detailed integration receipts in `.UAM/AINotes/v5-swarm-plan.md`.

Local delivery continuation verification: one immutable request remains held across its owned delivery snapshot, cancellation and changed turn, configuration or goal commands cannot release it, and failed synchronous acknowledgements retire admission before rejection; the Release app/test build, 29 affected cases plus the quota continuation fixture, all 1596 expected core cases and all 19 gates pass, while queue, retry, steering, remote, live crash and acceptance work remain open.

Permission queue continuation allocation: coordinator `chat-1791755529536-1b146e` owns `src/common/runtime/acp/acp_permission_handlers.cpp` and the private review-queue fixture in `tests/core_tests.cpp` in `build/v5-bug-fix-swarm`; the existing Broken AI Review row remains In Progress while task ownership is made stable across queue advancement, without real reviewer model calls.

Permission queue continuation verification: the private reallocation fixture reproduced a bus-error crash before the fix; owned task removal and a bounded initial poll pass nine focused cases, the Release application/test build, all 1597 expected core cases and 19 gates, while the broader Broken AI Review flow and all existing acceptance flags remain open.

All Time storage continuation allocation: coordinator `chat-1791755529536-1b146e` owns `src/common/usage/usage_store.{cpp,h}` and `tests/usage_store_tests.cpp` in `build/v5-bug-fix-swarm` for atomic archived accounting receipts and mutable identity protection before retention pruning; the original All Time Usage Range row remains In Progress and no selector or complete All Time claim is authorized by this storage slice.

All Time Usage Window disposition: the duplicate unassigned window task is folded into All Time Usage Range, owned by `chat-1791755529536-1b146e`; collector, ledger, local/remote bridge and selector acceptance remain in that active row. This removes duplicate allocation, not remaining work.

Archived accounting verification, 2026-10-12: the old-retention fixture first failed because archived_facts did not exist. Atomic schema-v2 archival, version-1 migration, canonical mutable updates and cancellation rollback now pass 21/21 focused storage/Antigravity cases, the full native Release application/test build with signing, all 1600/1600 expected compiled core registrations (+3, zero missing or duplicate names), and 19/19 CTest gates in 239.65 seconds. An existing Antigravity retention assertion now confirms archived evidence remains available when querying the original window, while expired groups remain absent from later windows. Evidence is in `.UAM/AINotes/v5-swarm-plan.md`; private logs use `/tmp/uam-v5-usage-archive-*`. All Time Usage Range remains In Progress: lifetime reads, collector backfill, ledger/peer/request support and Claude UI acceptance remain outstanding. Earlier deleted facts are not recreated by this migration.

Lifetime accounting continuation allocation: coordinator `chat-1791755529536-1b146e` owns `src/common/usage/usage_store.{cpp,h}`, `src/common/usage/usage_ledger.{cpp,h}`, `tests/usage_store_tests.cpp` and `tests/usage_ledger_tests.cpp` in `build/v5-bug-fix-swarm` for bounded lifetime receipt reads and explicitly scoped ledger serialization. All Time Usage Range remains In Progress with existing acceptance flags; no frontend files or Claude-owned selector are claimed.

All Time collection/peer continuation allocation: coordinator `chat-1791755529536-1b146e` owns `src/common/usage/usage_collector.{cpp,h}`, `src/common/usage/usage_history_service.{cpp,h}`, `src/common/usage/multi_host_history.{cpp,h}`, `src/cef/usage_handlers.cpp`, and their native tests in `tests/usage_collector_tests.cpp`, `tests/usage_history_service_tests.cpp` and `tests/multi_host_history_tests.cpp` in `build/v5-bug-fix-swarm`. The packet covers bounded available-history backfill, lifetime request/peer capability and explicit coverage; Claude retains the frontend selector and required user-visible review.

Lifetime accounting verification, 2026-10-12: 30/30 focused storage/ledger/Antigravity cases, the final full native Release application/test build with signing, all 1602/1602 expected compiled core registrations (+2, zero missing or duplicate names), 14/14 macOS picker scenarios and 19/19 CTest gates passed in 238.67 seconds. Query-plan inspection caught a parameter-controlled archive scan before commit; separate predicates retain indexed rolling reads and the final suite was rerun after that correction. Lifetime receipt reads and scoped v2 ledger serialization are implemented; collector backfill, peer/request wiring and Claude UI acceptance remain pending under All Time Usage Range. Evidence uses `.UAM/AINotes/v5-swarm-plan.md` and `/tmp/uam-v5-lifetime-ledger-*`.

All Time remote-client allocation amendment: coordinator `chat-1791755529536-1b146e` also owns the accounting snapshot validation in `src/remote/runner_client.cpp` and its matching fixture in `tests/remote_runner_tests.cpp` in `build/v5-bug-fix-swarm`; tracing the real runner transport found a second version-1-only guard beyond MultiHostHistory, so both guards must accept scoped v2 without accepting malformed or legacy-as-lifetime payloads.

Lifetime collection verification, 2026-10-12: 26/26 focused history/collector/peer/client cases, the full native Release application/test build with signing, all 1606/1606 expected compiled core registrations (+4, zero missing or duplicate names), 14/14 macOS picker scenarios and 19/19 CTest gates passed in 234.35 seconds. The actual runner/client transfer first failed at its legacy-only guard and now preserves a 180-day ordinary receipt through a multi-frame export; seven framed scope scenarios verify native v2, valid recent v1, malformed scope, old rolling receipts and duplicate identities. Available-history backfill and local/remote windowDays=0 backend queries are implemented within existing accounting limits. The frontend UsageWindowDays/selector and coverage presentation remain a Claude handoff under the coordinator-owned All Time Usage Range row, with direct UAM messaging unavailable and no ownership transfer performed; Claude Review, live local/remote acceptance and David sign-off remain pending. Evidence uses `.UAM/AINotes/v5-swarm-plan.md` and `/tmp/uam-v5-lifetime-collection-*`.

Queued admission durability continuation allocation: coordinator `chat-1791755529536-1b146e` owns `src/common/runtime/acp/acp_session_runtime.cpp`, `src/common/runtime/acp/acp_session_internal.h`, `tests/async_prompt_durability_tests.cpp` and affected native queue fixtures in `build/v5-bug-fix-swarm` under the existing Asynchronous Send Durability row. The first bounded packet covers ordinary local busy-turn queue admission, durable acknowledgement and dispatch gating while publication holds the repository mutex; remote cancellation, queue dispatch, retry and steering barriers remain separate follow-ups. No frontend or Claude-owned files are claimed.

Queued admission scope refinement: `chat-1791755529536-1b146e` also owns the existing save-acknowledgement declaration in `src/common/runtime/acp/acp_session_runtime.h`; queue admission is tracked on the pending save task so its dispatch gate does not delay unrelated routine metadata saves. The private contention fixture failed the agreed 100ms input budget while publication held the real repository mutex.

Queue streaming refinement allocation: `chat-1791755529536-1b146e` owns the guarded queue-journal publication API in `src/common/chat/chat_repository.{cpp,h}` and the synchronous-barrier integration in `src/common/runtime/acp/acp_session_lifecycle.cpp`, alongside the existing queue packet. A new private continuous-output regression failed because full-snapshot admission was repeatedly postponed by streaming updates; the uncommitted admission must use background journal publication before integration, with owned revision receipts and existing full-save fallback.

Busy queue journal verification, 2026-10-12: the contention regression first failed the 100ms input limit, and the uncommitted full-snapshot approach then failed the continuous-output receipt regression. Guarded background queue journals now pass 18/18 focused async/recall cases, the final full native Release application/test build with signing, all 1610/1610 expected compiled core registrations (+4, zero missing or duplicate names), 14/14 macOS picker scenarios and 19/19 CTest gates in 241.82 seconds. The earlier full run was 1608 pass/1 fail because a recall fixture assumed immediate queue durability; bounded owner polling now retains every FIFO and recall-once assertion. Busy-turn queue admission is implemented, including owned revision receipts and failure isolation; remaining dispatch, retry, steering, remote, idle-compatibility and force barriers stay under Asynchronous Send Durability. Claude Review and user acceptance remain pending. Evidence uses `.UAM/AINotes/v5-swarm-plan.md` and `/tmp/uam-v5-queue-*`.

Local queue dispatch continuation allocation: coordinator `chat-1791755529536-1b146e` owns `DrainNextQueuedAcpUserPrompt` in `src/common/runtime/acp/acp_session_runtime.cpp`, its existing staging interface, and `tests/async_prompt_durability_tests.cpp` plus affected native queue fixtures in `build/v5-bug-fix-swarm`. The current local path saves a dispatched-prefix count before admitting the user message, while restart skips that prefix; this creates a crash gap for unadmitted input. The bounded packet will retire the local outbox entry and admit its unsent user message in one owned asynchronous snapshot, then reuse staged local delivery; remote, quota and small-model legacy paths remain separate.

Atomic local queue dispatch verification, 2026-10-12: the final Release application build and signing passed, followed by 1612/1612 expected compiled core registrations (+2, no missing, extra or duplicate names), 14/14 macOS picker scenarios and 19/19 CTest gates in 242.01 seconds. All 25 focused cases passed, including existing FIFO, recall-once, agent-definition, retry rollback and quota continuation assertions. The previous six-failure run is retained in the evidence. Ordinary usable local dispatch stages complete admission and delivery off the input thread, and no local dispatch marker precedes message admission; known unusable local pipes, small-model, quota and remote paths retain their compatibility barriers. Retry, steering, remote and force-save work and Claude/user acceptance remain pending. Evidence: `.UAM/AINotes/v5-swarm-plan.md`, `/tmp/uam-v5-local-dispatch-final-*` and `/tmp/uam-v5-local-dispatch-inventory.json`.

Same-session retry durability allocation, 2026-10-12: coordinator `chat-1791755529536-1b146e` owns `src/common/runtime/acp/acp_session_runtime.cpp`, `src/cef/chat_handlers.cpp`, `tests/async_prompt_durability_tests.cpp` and affected native retry fixtures in `build/v5-bug-fix-swarm` under Asynchronous Send Durability. Ordinary healthy local retries will stage owned admission and delivery of the original message without appending another copy, preserve interrupted/unsent intent after failed persistence, and defer the CEF acknowledgement until the owned save receipt. Closed or missing input, remote and small-model compatibility barriers remain explicit. No frontend or Claude-owned file is claimed; visible acknowledgement/error behavior retains Claude Review.

Same-session retry verification, 2026-10-12: all three new cases reproduced failures before implementation, then all nine focused retry and 22 asynchronous durability cases passed. The signed Release application/test build and exact 1615/1615 expected compiled core registrations (+3, zero missing, extra or duplicates), 14/14 macOS picker scenarios and 19/19 CTest gates passed in 244.24 seconds. Healthy ordinary local retries reuse the original message, stage owned admission/delivery and defer CEF success until the durable receipt; failed saves and pre-delivery cancellation preserve one interrupted, unsent original message without provider input. Remote, small-model, quota and unusable transport paths retain synchronous compatibility. Broader send durability, Claude review and user acceptance remain open. Evidence: `.UAM/AINotes/v5-swarm-plan.md`, `/tmp/uam-v5-retry-*` and `/tmp/uam-v5-retry-inventory.json`.

Pending-first-prompt queue allocation, 2026-10-12: coordinator `chat-1791755529536-1b146e` owns `src/common/runtime/acp/acp_session_runtime.cpp`, `tests/async_prompt_durability_tests.cpp` and affected native queue fixtures in `build/v5-bug-fix-swarm` under Asynchronous Send Durability. Follow-up input while local prompt_request_id remains zero must return within100ms and use a full owned snapshot, preserving both first-message admission and queued intent before acknowledgement; delivered busy turns retain guarded queue journals. Verify pending admission, staged delivery, failed storage, FIFO and no duplicate replay. No frontend or Claude-owned files are claimed.

Queue control acknowledgement finding and allocation, 2026-10-12: coordinator `chat-1791755529536-1b146e` owns `src/cef/acp_handlers.cpp` and native queue-receipt fixtures in `tests/async_prompt_durability_tests.cpp` in `build/v5-bug-fix-swarm`. HandleManageQueuedAcpPrompt and HandleResumeQueuedAcpPrompts still acknowledge immediately after successful runtime mutation although delivered-turn queue journals now publish asynchronously. Reuse owned save-result callbacks, handler lifetime and data-root guards before success, preserving immediate local state feedback and synchronous compatibility responses. Failure, removed queue and active-turn isolation require regression coverage. No frontend files are claimed; visible late failure handling retains Claude Review. Implementation has not started.

Pending-first-prompt queue verification, 2026-10-12: the contention regression failed100ms before implementation, then all24async cases, two compatibility cases, six queue-control cases and five FIFO/recall/goal/image cases passed (37 focused cases total). The final signed Release application/test build and exact1617/1617 expected compiled core registrations (+2, zero missing, extra or duplicate names),14/14macOS picker scenarios and19/19CTest gates passed in246.19seconds. The first full run retained1609PASS+8FAIL and18/19gates; narrowed eligibility preserves all eight queue-only fixture assertions unchanged. Pending local first input requires full owned admission before follow-up success; delivered busy input retains guarded journals. Queue-control CEF acknowledgements, steering, remote, compatibility/force barriers and Claude/user acceptance remain open. Evidence: `.UAM/AINotes/v5-swarm-plan.md`, `/tmp/uam-v5-pending-queue-*` and `/tmp/uam-v5-pending-queue-inventory.json`.

OpenCode review and permission-feedback allocation, 2026-10-12: coordinator `chat-1791755529536-1b146e` owns `src/common/runtime/acp/acp_permission_handlers.cpp` and permission-flow cases in `tests/core_tests.cpp` in `build/v5-bug-fix-swarm` under OpenCode Automatic AI Review, Broken AI Review and the newly recorded Permission Waiting Save Barrier. AdvanceAcpPendingPermissionQueue synchronously saves a newer wait timestamp before launching the isolated reviewer; a private held-repository-mutex regression will establish whether this blocks the250ms feedback budget. Verify real OpenCode permission-frame dispatch through a private fake text reviewer, one-time decisions and manual repeated-call/sensitive-input gates without live provider calls or heuristic-only approvals. No Claude/frontend file is claimed and implementation has not started.

Queue control acknowledgement verification, 2026-10-12:25/25async cases, including eight edit/reorder/remove/resume storage outcomes, and227/227connected frontend store cases passed. The signed Release application/test build and exact1618/1618expected compiled core registrations (+1,no missing,extra or duplicate names),14/14macOS picker scenarios and19/19CTest gates passed in257.44seconds. Native manage/resume callbacks now defer success until owned save completion, retain local state feedback and report failed persistence under handler lifetime/data-root guards; delivered turns continue and retained queue intent is held on failure. Runtime receipts are tested and CEF glue is compiled; live GUI, Claude review and user acceptance remain pending. The next recorded permission-feedback allocation is unimplemented. Evidence: `.UAM/AINotes/v5-swarm-plan.md`, `/tmp/uam-v5-queue-controls-*` and `/tmp/uam-v5-queue-controls-inventory.json`.

Additional permission-flow finding: the same owner and native handler/core-test scope now includes Permission Review Receipt Persistence. PollPermissionReviewTasks updates tool review decision/reason but does not schedule a save after an owned worker result; PollAcpSessions consumes its changed flag before per-session polling. A private OpenCode fixture will finish the wait snapshot before consuming the result and then inspect on-disk review metadata. This is an unverified source finding until that regression runs; no complete claim or historical-incident equivalence is made.

Local interaction-response finding for future work, 2026-10-12: ResolveAcpPermission and ResolveAcpUserInput perform a full post-response timestamp save before returning to CEF. The new Local Response Save Barriers row remains Unassigned and unverified; future work should measure local clicks against250ms, preserve exactly one provider response and eventual UTC metadata, and retain remote pre-response/recovery barriers. No implementation or ownership allocation is published for this finding, and no failing timing test is added to the current packet. This preserves David’s timing-test scope and request to record newly found bugs for future agents/runs. No frontend or Claude-owned files are claimed.

Permission feedback and OpenCode review verification,2026-10-12: the250ms feedback test failed before the timestamp-save change, and the real-frame/fake-reviewer test delivered one approval but failed on-disk verdict inspection before result persistence. All11focused cases now pass, including approve/deny/uncertain/malformed output and manual doom-loop/sensitive-input gates. The signed Release app/test build and exact1620/1620expected compiled core registrations (+2,zero missing,extra or duplicate names),14/14macOS picker scenarios and19/19CTest gates pass in250.71seconds. Waiting timestamps and owned verdict diagnostics save in the background, preserving latest UTC metadata, one-time responses and ownership/read-only gates. OpenCode native capability verification, authenticated live review, Claude review and user acceptance remain open; the unrelated local response save finding is Open/Unassigned for future performance work. Evidence: `.UAM/AINotes/v5-swarm-plan.md`, `/tmp/uam-v5-permission-feedback-*`, `/tmp/uam-v5-review-receipt-*` and `/tmp/uam-v5-permission-feedback-inventory.json`.

Idle local admission continuation allocation,2026-10-12: coordinator `chat-1791755529536-1b146e` owns `src/common/runtime/acp/acp_session_runtime.cpp`, `tests/async_prompt_durability_tests.cpp` and affected native compatibility/routing fixtures in `build/v5-bug-fix-swarm` under Asynchronous Send Durability. Scope is explicit local send admission while idle compatibility checks or automatic routing are pending, using full owned snapshots before durable acknowledgement and dispatch while keeping queue-only compatibility/control contracts, remote and quota paths unchanged. Verify100ms contention, failure-held intent, complete history and dispatch gating. No frontend or Claude-owned file is claimed; implementation has not started.

Idle admission connected-fixture ownership: the same coordinator additionally owns `tests/acp_session_tests.cpp` and `tests/turn_finalization_ownership_tests.cpp` for bounded save/delivery polling before their existing resumed-turn and checkpoint assertions. The first full idle run is1620PASS+2FAIL with18/19gates in258.06seconds; preserve that evidence. Known unusable ready local transports must retain the existing synchronous rollback contract. No fixture assertion or gate will be weakened.
