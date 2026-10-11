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
- Never infer user approval from passed tests, successful builds, installation, a restart, silence or a general request to continue.

## Changes

Initial statuses are deliberately unsigned: no entry has been marked Complete based on build or automated test results, and earlier user comments are not treated as blanket approval of these cumulative changes.


| Group | Type | Change | Summary | Status |
|---|---|---|---|---|
| Core correctness | Bugfix | Recovery State Reconciliation | Recovery-only changes now reach the frontend without requiring another chat field to change. | Claude Review; Not User Tested |
| Core correctness | Bugfix | Memory Request Ownership | Older memory requests can no longer overwrite the currently selected scope or its errors. | Claude Review; Not User Tested |
| Core correctness | Bugfix | Pending Attachment Preservation | Send completion preserves skills added or reattached while the request was pending. | Claude Review; Not User Tested |
| Core correctness | Bugfix | Cancellation Persistence | Failed cancellation saves retain cancellation intent and prevent queued runs from relaunching. | Un-Reviewed; Not User Tested |
| Core correctness | Code Only | Shared Provider Catalogue | Provider choices use shared metadata instead of duplicated provider lists. | Claude Review; Not User Tested |
| Core correctness | Code Only | Tooltip Ownership Cleanup | Redundant tooltip timing ownership was removed. | Claude Review; Not User Tested |
| Composer and queue | Graphical | Composer Message Editing | Editing a sent message now uses the existing composer instead of a separate editing textarea. | Claude Review; Not User Tested |
| Composer and queue | Bugfix | Edit Draft Restoration | Cancelling an edit restores the unsent draft and preserves its attachments. | Claude Review; Not User Tested |
| Composer and queue | Bugfix | Editable Pending Composer | The composer remains editable while prompt admission is pending without allowing duplicate submissions. | Claude Review; Not User Tested |
| Composer and queue | Bugfix | Separate FIFO Turns | Queued prompts dispatch as separate durable turns instead of being concatenated into one prompt. | Un-Reviewed; Not User Tested |
| Composer and queue | Graphical | Queue Management Controls | Guarded controls expose queue editing, reordering, removal and explicit resume. | Claude Review; Not User Tested |
| Composer and queue | Bugfix | Queue Revision Guards | Stale queue edits and reorders are rejected without losing captured attachments or execution options. | Claude Review; Not User Tested |
| Composer and queue | Bugfix | Restart Queue Holding | Restored queued intent remains held until explicitly resumed. | Claude Review; Not User Tested |
| Composer and queue | Performance | Queue Persistence Journal | Ordinary queue changes use a bounded durable journal instead of rewriting the entire transcript. | Un-Reviewed; Not User Tested |
| Composer and queue | Bugfix | Queue Crash Recovery | Journal generations and consumption markers prevent stale saves or crashes from resurrecting dispatched input. | Un-Reviewed; Not User Tested |
| Composer and queue | Bugfix | Queue Attachment Editing | Queued prompts support attachment removal and attachment-only edits while preserving retained attachments. | Claude Review; Not User Tested |
| Composer and queue | Graphical | Queued Attachment Names | Queued messages display their captured file names. | Claude Review; Not User Tested |
| Composer and queue | Bugfix | Secondary Control Submission | Secondary composer buttons no longer accidentally submit the draft. | Claude Review; Not User Tested |
| Composer and queue | Bugfix | Editor Keyboard Ownership | Queue-editor shortcuts remain inside the editor rather than triggering composer actions. | Claude Review; Not User Tested |
| Composer and queue | Bugfix | Goal Request Ownership | Goal composer requests retain correct ownership through React effect replay. | Claude Review; Not User Tested |
| Composer and queue | Graphical | Composer Goal Controls | Goal actions were moved into the composer controls with guarded asynchronous ownership. | Claude Review; Not User Tested |
| Composer and queue | Bugfix | Goal Popup Escape | Escape closes the goals popup from the composer. | Claude Review; Not User Tested |
| Composer and queue | Bugfix | Checkpoint Queue Preservation | Checkpoint finalization cannot overwrite newer queued intent or a replacement turn. | Un-Reviewed; Not User Tested |
| Composer and queue | Performance | Prompt Settings Capture | Provider prompt construction captures required settings without copying the complete loaded transcript. | Un-Reviewed; Not User Tested |
| Composer and queue | Performance | Redundant Delivery Saves | Unchanged local delivery state is no longer rewritten after its durable delivery-intent save. | Un-Reviewed; Not User Tested |
| History and performance | Performance | Retry Selector Caching | Composer typing avoids repeated retry-history scans. | Claude Review; Not User Tested |
| History and performance | Performance | Timeline Render Reuse | Unchanged transcript rendering is reused while the composer updates. | Claude Review; Not User Tested |
| History and performance | Performance | Timestamp Render Reuse | Unchanged turn timestamps are no longer repeatedly formatted during typing and streaming. | Claude Review; Not User Tested |
| History and performance | Performance | Incremental History Digests | Exact unchanged message prefixes reuse their hashes when history appends or shrinks. | Un-Reviewed; Not User Tested |
| History and performance | Bugfix | Earlier History Corrections | Digest checks still detect earlier content, thought and tool-result changes. | Un-Reviewed; Not User Tested |
| History and performance | Performance | Bounded Frontend Summaries | Catalogue updates avoid transporting complete transcript bodies unnecessarily. | Un-Reviewed; Not User Tested |
| History and performance | Performance | Cold Startup Histories | Startup leaves chat histories unloaded until a bounded page is requested. | Un-Reviewed; Not User Tested |
| History and performance | Graphical | Desktop History Paging | Large transcripts load in pages while retaining absolute message positions. | Claude Review; Not User Tested |
| History and performance | Bugfix | Detached History Pages | Paging an unloaded chat no longer unnecessarily hydrates its complete native state. | Un-Reviewed; Not User Tested |
| History and performance | Bugfix | Paged Native Tokens | Native history identity tokens survive paged loading. | Claude Review; Not User Tested |
| History and performance | Performance | Recent History Cache | Validated recent-message caches avoid repeated complete reads of large saved histories. | Un-Reviewed; Not User Tested |
| History and performance | Performance | Bounded Complete Tails | Oversized recent pages cache fewer complete messages rather than abandoning caching or truncating messages. | Un-Reviewed; Not User Tested |
| History and performance | Performance | Priority History Loading | Opening chat history no longer waits behind background VCS subprocess checks. | Un-Reviewed; Not User Tested |
| History and performance | Performance | Priority Cold Sending | Cold send preparation runs outside the background VCS queue. | Un-Reviewed; Not User Tested |
| History and performance | Performance | Background Buffer Disposal | Large detached history buffers are released away from the GUI thread. | Un-Reviewed; Not User Tested |
| History and performance | Performance | Background Cache Publication | Revision-guarded history cache publication runs on the background worker. | Un-Reviewed; Not User Tested |
| History and performance | Performance | Durable Transcript Retention | Only saved inactive histories are released while visible, active and unsaved work stays protected. | Un-Reviewed; Not User Tested |
| History and performance | Performance | Paused History Release | Durable hidden histories can be released for paused and blocked goals. | Un-Reviewed; Not User Tested |
| History and performance | Bugfix | Reselected History Preservation | Idle transitions cannot discard history that the user has reselected. | Claude Review; Not User Tested |
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
| Providers and MCP | Graphical | Consolidated Provider Settings | Provider settings expose scoped switches and distinguish configured setup from verified connection status. | Claude Review; Not User Tested |
| Providers and MCP | Bugfix | MCP Opt Out | Explicit opt-out suppresses warnings and prevents launch overrides from falsely reporting a connection. | Claude Review; Not User Tested |
| Providers and MCP | Bugfix | MCP Warning Ownership | Warning dismissals reset across foreground and background runtime restarts. | Claude Review; Not User Tested |
| Providers and MCP | Bugfix | Unknown Connection States | Unknown provider connections and startup failures remain distinguishable from successful connections. | Claude Review; Not User Tested |
| Providers and MCP | Bugfix | MCP Import Preservation | MCP import preserves existing settings and reports actionable source errors. | Claude Review; Not User Tested |
| Providers and MCP | Bugfix | MCP Editor Identity | MCP editing retains identity and rejects reserved external definitions. | Claude Review; Not User Tested |
| Providers and MCP | Graphical | Multiline MCP Editing | Imported MCP configuration remains editable in a multiline editor. | Claude Review; Not User Tested |
| Providers and MCP | Bugfix | OpenCode Import Warnings | OpenCode import warnings remain visible through review and save. | Claude Review; Not User Tested |
| Providers and MCP | Misc | Native MCP Answer Verification | Question-answer delivery was exercised through the native runner relay. | Un-Reviewed; Not User Tested |
| Providers and MCP | Bugfix | Late Permission Cancellation | Permission requests arriving during cancellation receive a cancelled response. | Un-Reviewed; Not User Tested |
| Providers and MCP | Bugfix | Model Catalogue Ownership | Model catalogues are bound to the provider connection that produced them. | Un-Reviewed; Not User Tested |
| Providers and MCP | Bugfix | Disabled Model Enforcement | Disabled models are rejected in chat, worker and reviewer selectors. | Claude Review; Not User Tested |
| Providers and MCP | Performance | Cached Model Discovery | Cached model discovery avoids interrupting existing conversations. | Claude Review; Not User Tested |
| Providers and MCP | Bugfix | Startup Model Isolation | Startup model discovery does not take over user conversations. | Un-Reviewed; Not User Tested |
| Providers and MCP | Bugfix | Codex Instruction Transport | Complete Codex instructions travel through native session setup instead of exceeding OS command-line limits. | Un-Reviewed; Not User Tested |
| Providers and MCP | Bugfix | Duplicate Workspace Instructions | Codex startup avoids duplicating workspace instructions and preserves setup errors. | Un-Reviewed; Not User Tested |
| Providers and MCP | Bugfix | Completed Codex Decisions | Suggestions, memory extraction and permission reviews use completed Codex output. | Un-Reviewed; Not User Tested |
| Providers and MCP | Bugfix | Empty Remote Arguments | Remote launch transport preserves intentionally empty provider option values. | Un-Reviewed; Not User Tested |
| Providers and MCP | Bugfix | POSIX Argument Preservation | Quoted backslashes and line continuations retain their intended values. | Un-Reviewed; Not User Tested |
| Providers and MCP | Bugfix | Remote Workspace Backslashes | Literal backslashes in POSIX workspace paths survive model and provider operations. | Un-Reviewed; Not User Tested |
| Providers and MCP | Misc | Antigravity Terminal Support | Antigravity terminal launch, saved-ID resume, installation and authentication were added and verified. | Claude Review; Not User Tested |
| Providers and MCP | Misc | Antigravity Structured Support | Antigravity native streaming uses isolated configuration with conversation identity and resume handling. | Claude Review; Not User Tested |
| Providers and MCP | Bugfix | Antigravity Replay Filtering | Resumed Antigravity answers are preserved when they repeat earlier text. | Un-Reviewed; Not User Tested |
| Claude and orchestration | Bugfix | Claude Tool History | Claude tool history and native task ownership remain attached to the correct conversation. | Claude Review; Not User Tested |
| Claude and orchestration | Bugfix | Native Child Settlement | Child status settles after confirmed exits while unconfirmed remote exits retain ownership. | Claude Review; Not User Tested |
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
| Claude and orchestration | Graphical | Eligible Subagent Providers | Managed subagent choices expose only scoped eligible providers. | Claude Review; Not User Tested |
| Review and checkpoints | Bugfix | Unfinished Review Gating | Review changes stays hidden while parent, child, goal or history-loading work remains unfinished. | Claude Review; Not User Tested |
| Review and checkpoints | Bugfix | Stale Diff Ownership | Old diff metadata cannot be paired with a newly selected chat. | Claude Review; Not User Tested |
| Review and checkpoints | Misc | Recorded Edit Receipts | Immutable provider-reported edit receipts support chat-specific review without attributing unrelated workspace changes. | Un-Reviewed; Not User Tested |
| Review and checkpoints | Graphical | Recorded Patch Review | Recorded chat edits have review controls that preserve newer commit drafts. | Claude Review; Not User Tested |
| Review and checkpoints | Bugfix | User Staging Protection | Automatic checkpoints preserve user staging instead of consuming files staged during agent work. | Un-Reviewed; Not User Tested |
| Review and checkpoints | Bugfix | Cold Checkpoint Rollback | Checkpointed turns can be rolled back from cold saved chats. | Un-Reviewed; Not User Tested |
| Review and checkpoints | Graphical | Bounded Diff Rendering | Large diffs render within bounds with source line gutters. | Claude Review; Not User Tested |
| Review and checkpoints | Graphical | Virtualized Split Diffs | The feature agent added a virtualized split layout with a remembered view preference. | Claude Review; Not User Tested |
| Review and checkpoints | Graphical | Scoped File Inspection | The feature agent added bounded source and diff navigation scoped to the selected workspace. | Claude Review; Not User Tested |
| Review and checkpoints | Misc | Git SVN Switches | Git and SVN integration can be enabled or disabled persistently. | Claude Review; Not User Tested |
| Workspaces and deletion | Misc | Automatic Worktree Isolation | Readable isolated worktrees are prepared before first provider dispatch. | Claude Review; Not User Tested |
| Workspaces and deletion | Bugfix | Worktree Collision Detection | Collision checks work across Windows shell formatting and existing branch names. | Un-Reviewed; Not User Tested |
| Workspaces and deletion | Bugfix | UTF8 Worktree Paths | Readable worktree paths preserve UTF-8 names. | Un-Reviewed; Not User Tested |
| Workspaces and deletion | Bugfix | Canonical Workspace Ownership | Duplicate canonical directory owners are rejected. | Un-Reviewed; Not User Tested |
| Workspaces and deletion | Graphical | Original Workspace Labels | Worktree rows identify their original workspace. | Claude Review; Not User Tested |
| Workspaces and deletion | Performance | Branch Snapshot Reduction | Branch creation avoids copying unnecessary full source transcripts. | Un-Reviewed; Not User Tested |
| Workspaces and deletion | Performance | Background Cold Deletion | Cold deletion preparation and staging run outside the GUI thread. | Un-Reviewed; Not User Tested |
| Workspaces and deletion | Bugfix | Shared Native Ownership | Deletion preserves native history still owned by surviving chat aliases. | Un-Reviewed; Not User Tested |
| Workspaces and deletion | Bugfix | Durable Deletion Recovery | Shared ownership protection survives interrupted staging and commit recovery. | Un-Reviewed; Not User Tested |
| Workspaces and deletion | Bugfix | Deletion Failure Guards | Backup collisions, identity collisions and pending imports cannot silently weaken deletion ownership. | Un-Reviewed; Not User Tested |
| Workspaces and deletion | Bugfix | Selected Workspace Deletion | Context-menu deletion respects the selected workspace scope. | Claude Review; Not User Tested |
| Workspaces and deletion | Performance | Deletion Path Caching | Canonical ownership normalization is reused within deletion preparation. | Un-Reviewed; Not User Tested |
| Workspaces and deletion | Performance | Survivor Buffer Preservation | Folder commits avoid copying unrelated surviving transcript bodies. | Un-Reviewed; Not User Tested |
| Workspaces and deletion | Graphical | Deletion Progress Feedback | Workspace deletion exposes progress and useful native failure details. | Claude Review; Not User Tested |
| Workspaces and deletion | Misc | Orphan Worktree Inspection | Old owned worktrees are inspected before retention cleanup. | Un-Reviewed; Not User Tested |
| Workspaces and deletion | Bugfix | Leased Worktree Cleanup | Orphan worktree cleanup respects workspace leases and stored agent references. | Un-Reviewed; Not User Tested |
| Import and attachments | Graphical | Native Import Review | Cached startup histories can be reviewed and selectively imported. | Claude Review; Not User Tested |
| Import and attachments | Graphical | Scoped Import Selection | Shift selection and importing all native-history pages remain scoped to their workspace. | Claude Review; Not User Tested |
| Import and attachments | Bugfix | Cross Workspace Import Guards | Import decisions cannot publish into a different workspace. | Claude Review; Not User Tested |
| Import and attachments | Bugfix | Native Duplicate Prevention | Legacy remote folder aliases cannot create duplicate native imports. | Un-Reviewed; Not User Tested |
| Import and attachments | Bugfix | Side Chat Retention | Import retains side-chat transcripts without assuming source-cleanup authority. | Un-Reviewed; Not User Tested |
| Import and attachments | Bugfix | Discarded Import Reporting | Selected histories discarded before publication are reported rather than silently lost. | Claude Review; Not User Tested |
| Import and attachments | Bugfix | Import Request Recovery | Stale request guards are released through effect replay and completed review ownership. | Claude Review; Not User Tested |
| Import and attachments | Graphical | Lazy Image Previews | Saved image previews load lazily using persisted attachment identity. | Claude Review; Not User Tested |
| Import and attachments | Performance | Bounded Image Cache | Native saved-image previews use bounded caching. | Un-Reviewed; Not User Tested |
| Import and attachments | Performance | Cold Attachment Previews | Saved attachments can be previewed without hydrating the complete chat. | Un-Reviewed; Not User Tested |
| Import and attachments | Performance | Background Claude Images | Claude image delivery is prepared asynchronously within transport limits. | Un-Reviewed; Not User Tested |
| Import and attachments | Graphical | Image Collection Display | Image collections and attachment-retention choices are displayed in chat editing. | Claude Review; Not User Tested |
| Import and attachments | Bugfix | Attachment Path Labels | Image fallback names and full-path tooltips retain correct attachment identity. | Un-Reviewed; Not User Tested |
| Navigation and interface | Graphical | Active Chat Pinning | Running chats support context-menu pinning without changing the selection. | Claude Review; Not User Tested |
| Navigation and interface | Graphical | Next Attention Navigation | Navigation can select the next chat needing an answer, approval or failure response. | Claude Review; Not User Tested |
| Navigation and interface | Graphical | Organization Undo | Guarded reversible collection changes are exposed through undo controls. | Claude Review; Not User Tested |
| Navigation and interface | Bugfix | Delayed Rename Preservation | Delayed collection rename replies cannot overwrite newer names. | Claude Review; Not User Tested |
| Navigation and interface | Graphical | Bounded Conversation Search | Search returns bounded content hits with guarded transcript provenance and message navigation. | Claude Review; Not User Tested |
| Navigation and interface | Performance | Background Search Loading | Search workers load one transcript at a time rather than copying the entire catalogue. | Un-Reviewed; Not User Tested |
| Navigation and interface | Graphical | Workspace Picker Search | The new-chat picker searches grouped workspaces. | Claude Review; Not User Tested |
| Navigation and interface | Graphical | Composer Focus Shortcut | A scoped shortcut focuses the active composer or terminal. | Claude Review; Not User Tested |
| Navigation and interface | Bugfix | Modal Focus Ownership | Only the top visible modal owns keyboard handling and focus restoration. | Claude Review; Not User Tested |
| Navigation and interface | Bugfix | Hidden Modal Exclusion | Hidden or closed dialogs no longer steal chat-input focus. | Claude Review; Not User Tested |
| Navigation and interface | Bugfix | Branch Family Selection | Branch families restore their last successfully selected chat. | Claude Review; Not User Tested |
| Navigation and interface | Graphical | Persistent Chat Snooze | The feature agent added persisted snoozing with guarded background metadata saves. | Claude Review; Not User Tested |
| Navigation and interface | Graphical | Chat Title Regeneration | Chat titles can be regenerated on guarded workers, including imported transcripts. | Claude Review; Not User Tested |
| Navigation and interface | Graphical | Chat Copy Actions | Sidebar menus expose chat-name and chat-ID copying. | Claude Review; Not User Tested |
| Navigation and interface | Graphical | Message Position Navigation | The feature agent added navigation through large histories by absolute message position. | Claude Review; Not User Tested |
| Navigation and interface | Graphical | User Message Rail | The message rail targets user messages with corrected spacing, hover areas and final-marker alignment. | Claude Review; Not User Tested |
| Navigation and interface | Graphical | Searchable Model Picker | The feature agent added keyboard-accessible model search, a provider sidebar and favourites. | Claude Review; Not User Tested |
| Navigation and interface | Graphical | Version Control Settings | Persisted Git and SVN switches independently enable or disable their supported workflows. | Claude Review; Not User Tested |
| Navigation and interface | Graphical | Typography Settings | The feature agent added persisted typography controls and live terminal-font updates. | Claude Review; Not User Tested |
| Navigation and interface | Graphical | Parent Waiting Feedback | Chat activity distinguishes a parent waiting for subagents and subagent calls display their latest reported activity. | Claude Review; Not User Tested |
| Navigation and interface | Graphical | Native Compaction Feedback | Native compaction displays its in-progress state and available metrics. | Claude Review; Not User Tested |
| Navigation and interface | Graphical | Working Indicator Motion | The blue working indicator has reduced-motion support without implying that overall animation smoothness is accepted. | Claude Review; Not User Tested |
| Navigation and interface | Graphical | Provider Branding | Official provider marks and unknown-provider fallbacks were aligned across the interface. | Claude Review; Not User Tested |
| Navigation and interface | Graphical | Main Application Icon | The selected full-tile app icon and accepted CEF title-bar icon are packaged. | Claude Review; Not User Tested |
| Navigation and interface | Graphical | Active Host Labels | Active chat rows display their owning host. | Claude Review; Not User Tested |
| Navigation and interface | Graphical | Scoped Branch Labels | Activity branch labels remain bound to the correct workspace and load near the viewport. | Claude Review; Not User Tested |
| Navigation and interface | Performance | Collapsed Row Unmounting | Collapsed sidebar sections stop mounting hidden chat rows and their background hooks. | Un-Reviewed; Claude Review; Not User Tested |
| Navigation and interface | Bugfix | Loading Draft Preservation | History-loading and empty states preserve the composer draft. | Claude Review; Not User Tested |
| Navigation and interface | Bugfix | Startup Recovery Visibility | Startup recovery remains hidden while the armed interface is loading. | Claude Review; Not User Tested |
| Navigation and interface | Bugfix | Empty Assistant Rows | Contiguous assistant text no longer produces empty message-action rows. | Claude Review; Not User Tested |
| Navigation and interface | Graphical | Actionable Workspace Errors | Workspace creation and missing-initial-commit failures use clearer scoped feedback. | Claude Review; Not User Tested |
| Usage and accounting | Misc | Persistent Usage Ledger | A persistent background ledger caches the rolling 24 hours and older provider usage by UTC day. | Claude Review; Not User Tested |
| Usage and accounting | Misc | Historical Usage Normalization | Historical usage is normalized and scanned incrementally. | Un-Reviewed; Not User Tested |
| Usage and accounting | Misc | Usage Dashboard | The feature agent added a usage workspace with explicit cost coverage and model prices. | Claude Review; Not User Tested |
| Usage and accounting | Misc | Provider Period Filters | Usage totals and chart periods can be filtered by provider. | Claude Review; Not User Tested |
| Usage and accounting | Misc | Cached Price Estimates | Cached reference prices support equivalent hosted Qwen estimates for local OpenCode models while preserving reported charges and unknown rates. | Claude Review; Not User Tested |
| Usage and accounting | Bugfix | Deleted Usage Entries | Removed transcript files are dropped from the usage-history cache. | Un-Reviewed; Not User Tested |
| Usage and accounting | Misc | Remote Usage Export | SSH runners export cached usage accounting. | Un-Reviewed; Not User Tested |
| Usage and accounting | Bugfix | Machine Total Deduplication | Remote machine totals avoid duplicate accounting across copied histories. | Claude Review; Not User Tested |
| Usage and accounting | Misc | Claude Live Usage | Scoped Claude snapshots expose live consumed-token accounting. | Claude Review; Not User Tested |
| Usage and accounting | Misc | Gemini Saved Usage | Gemini saved-session usage is read through bounded snapshot scans. | Un-Reviewed; Not User Tested |
| Usage and accounting | Misc | Gemini Prompt Usage | Gemini prompt replies contribute validated observed input, cache and reasoning counts. | Claude Review; Not User Tested |
| Usage and accounting | Misc | OpenCode SQLite Usage | OpenCode SQLite histories provide token accounting without counting copied fork usage twice. | Un-Reviewed; Not User Tested |
| Usage and accounting | Misc | OpenCode Prompt Usage | OpenCode prompt accounting includes cache and reasoning across tool steps. | Claude Review; Not User Tested |
| Usage and accounting | Misc | Copilot Durable Usage | Copilot durable accounting avoids counting repeated snapshots. | Claude Review; Not User Tested |
| Usage and accounting | Misc | Antigravity Usage Accounting | Antigravity usage participates in cached background accounting. | Un-Reviewed; Not User Tested |
| Usage and accounting | Misc | Provider Context Usage | Provider-neutral ACP context-usage updates are handled. | Claude Review; Not User Tested |
| Usage and accounting | Bugfix | Fresh Login Allowances | Session account allowances clear appropriately after a fresh Codex login. | Claude Review; Not User Tested |
| Remote execution | Bugfix | Installer Lock Inheritance | Helper descendants no longer inherit the Linux installer transaction lock. | Un-Reviewed; Not User Tested |
| Remote execution | Bugfix | Provider Blocking Diagnostics | Remote CLI update refusals identify validated blocking provider PIDs. | Un-Reviewed; Not User Tested |
| Remote execution | Bugfix | Update Activity Rechecks | CLI updates recheck current activity and respect the identified package-manager target. | Claude Review; Not User Tested |
| Remote execution | Misc | Opt In Linux Elevation | Linux npm updates can use persisted per-host sudo selection instead of automatic elevation. | Claude Review; Not User Tested |
| Remote execution | Bugfix | Host Option Preservation | Failed startup configuration no longer resets unrelated saved host options. | Un-Reviewed; Not User Tested |
| Remote execution | Bugfix | Selected Host Actions | Provider settings and updates stay scoped to the selected host. | Claude Review; Not User Tested |
| Remote execution | Bugfix | Repeated Update Admission | Repeated clicks cannot launch overlapping manual provider updates. | Claude Review; Not User Tested |
| Remote execution | Bugfix | Startup Error Preservation | Remote configuration and rejected-input errors remain visible before turn creation. | Un-Reviewed; Not User Tested |
| Remote execution | Bugfix | Accepted Input Acknowledgement | Accepted remote input remains acknowledged even when the provider subsequently exits. | Un-Reviewed; Not User Tested |
| Remote execution | Bugfix | Exit Interaction Settlement | Authenticated provider exits settle their remote interactions consistently. | Un-Reviewed; Not User Tested |
| Remote execution | Bugfix | Consistent Runner Snapshots | Completion and output snapshots remain consistent. | Un-Reviewed; Not User Tested |
| Remote execution | Bugfix | Final Output Draining | Remote Stop and shutdown retain final provider lines without artificial drain delays. | Un-Reviewed; Not User Tested |
| Remote execution | Bugfix | Stop Confirmation Timing | Remote Stop has time to confirm termination before timeout. | Un-Reviewed; Not User Tested |
| Remote execution | Bugfix | Missing Process Settlement | Confirmed missing remote processes settle through the shutdown path. | Un-Reviewed; Not User Tested |
| Remote execution | Bugfix | Replacement Output Isolation | Pending Stop output cannot contaminate a replacement session. | Un-Reviewed; Not User Tested |
| Remote execution | Bugfix | Reconnect Coalescing | Pending terminal reads coalesce duplicate reconnect requests. | Claude Review; Not User Tested |
| Remote execution | Bugfix | Terminal Launch Identity | Reused terminal IDs reject output from older launches. | Claude Review; Not User Tested |
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
| Platform and diagnostics | Bugfix | ES2020 Sidebar Compatibility | Sidebar code remains compatible with the configured JavaScript target. | Claude Review; Not User Tested |
| Platform and diagnostics | Bugfix | Fresh Ninja Builds | Fresh Ninja UI builds preserve the expected output layout. | Un-Reviewed; Not User Tested |
| Platform and diagnostics | Bugfix | macOS Quit Routing | Application Quit runs through CEF shutdown and handles picker cancellation and repeated requests. | Un-Reviewed; Not User Tested |
| Platform and diagnostics | Bugfix | Caps Lock Quit | Command-Q works with Caps Lock enabled. | Un-Reviewed; Not User Tested |
| Platform and diagnostics | Bugfix | Reaped Exit Status | macOS retains process exit status while remaining output is drained. | Un-Reviewed; Not User Tested |
| Platform and diagnostics | Performance | Background Diagnostic Logging | Diagnostic logging uses a bounded background worker. | Un-Reviewed; Not User Tested |
| Platform and diagnostics | Bugfix | Oversized Log Recovery | Startup safely recovers oversized diagnostic logs. | Un-Reviewed; Not User Tested |
| Platform and diagnostics | Bugfix | Special Log Path Rejection | Startup rejects special diagnostic paths before opening them. | Un-Reviewed; Not User Tested |
| Platform and diagnostics | Code Only | CEF Task Ownership | Metadata work remains on CEF-owned background tasks. | Un-Reviewed; Not User Tested |
| Platform and diagnostics | Performance | Background Computer Scans | Computer Use scan copies and cleanup run away from the GUI thread. | Un-Reviewed; Not User Tested |
| Platform and diagnostics | Performance | Indexed Scan Publication | Computer Use publication indexes chat identities instead of repeatedly searching the catalogue. | Un-Reviewed; Not User Tested |
| Platform and diagnostics | Performance | Changed Scan Entries | Unchanged Computer Use scans avoid unnecessary GUI-thread state publication. | Un-Reviewed; Not User Tested |
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
| Remaining performance | Performance | Asynchronous Send Durability | Moving the complete pre-dispatch durable save off the GUI thread remains unfinished despite faster serialization. | Un-Reviewed; Not User Tested |

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
| Performance | Performance | Asynchronous Send Durability | Move the remaining pre-dispatch durable save off the GUI thread while preserving admission, ordering and crash recovery. | Open; Un-Reviewed; Not User Tested | Unassigned |
| Performance | Performance | Large Chat Opening Acceptance | Measure live CEF opening of the reported huge chat against David’s one-to-two-second target, verify history-loading feedback and fix remaining bottlenecks. | Open; Claude Review; Not User Tested | Unassigned |
| Performance | Performance | Typing And Sending Acceptance | Measure native typing stalls, sending, scrolling and concurrent streaming in the current app and correct remaining interaction delays. | Open; Claude Review; Not User Tested | Unassigned |
| Graphical review | Graphical | Animation Smoothness Acceptance | Review and verify current transition smoothness without introducing continuously repainting decorative animations. | Open; Claude Review; Not User Tested | Unassigned |
| Graphical review | Graphical | Claude Interface Review | Complete Claude review of every flagged React or UX change and resolve its findings before removing review flags. | Open; Claude Review; Not User Tested | Unassigned |
| Remote maintenance | Bugfix | Remote Ownership Refusals | Investigate helpers that still refuse guarded Stop and retire verified idle ownership without terminating unrelated live work. | Open; Un-Reviewed; Not User Tested | Unassigned |
| Remote maintenance | Misc | Current Helper Activation | Verify safe activation of the latest packaged helpers on both remote hosts and sending Codex skill prompts without the OS launch-limit failure after resolving ownership guards. | Open; Un-Reviewed; Not User Tested | Unassigned |
| Remote maintenance | Misc | Elevated Provider Update Acceptance | Verify saved per-host elevation choices update the identified provider installations without resetting settings or granting chat sessions privileges. | Open; Un-Reviewed; Not User Tested | Unassigned |
| Remote maintenance | Misc | Interactive Sudo Update Support | Resolve the unimplemented password-authenticated update path for hosts requiring interactive sudo without placing passwords in chat or settings. | Open; Un-Reviewed; Not User Tested | Unassigned |
| Remote startup | Misc | Runner Startup Acceptance | Verify real Linux user-session and Windows login startup behavior while preserving independently saved host options on failure. | Open; Un-Reviewed; Not User Tested | Unassigned |
| Remote routing | Misc | Concurrent Two Host Acceptance | Run the production-linked two-host lifecycle packet with independent review and verify pinning, token refusal, authorized release and scoped cleanup. | Blocked; Un-Reviewed; Not User Tested | Unassigned |
| Remote evidence | Misc | Missing Incident Evidence | Keep the failed r4 run and missing historical startup or Stop traces explicit until equivalent fresh evidence can be captured. | Blocked; Un-Reviewed; Not User Tested | Unassigned |
| Provider acceptance | Misc | Six Provider Live Matrix | Complete fresh authenticated acceptance for all six providers across applicable local and remote launch, send, resume, cancellation and token/cost accounting paths with unsupported usage explicit. | Open; Un-Reviewed; Not User Tested | Unassigned |
| Provider acceptance | Misc | Managed Action Acceptance | Verify real question, permission, model-setting and MCP invocation flows rather than treating tool discovery as successful execution. | Open; Claude Review; Not User Tested | Unassigned |
| Provider acceptance | Misc | Handoff And Review Acceptance | Verify current provider handoffs and recorded-edit review preserve the selected chat’s context and ownership in real sessions. | Open; Claude Review; Not User Tested | Unassigned |
| Provider acceptance | Misc | Queue Restart Acceptance | Verify queue editing, individual delivery and restart recovery through the current interface without replaying acknowledged work. | Open; Claude Review; Not User Tested | Unassigned |
| Provider acceptance | Misc | Claude Lifecycle Acceptance | Verify authenticated Claude child ownership, Stop, shutdown and Review changes suppression after the isolated lifecycle fixes. | Open; Claude Review; Not User Tested | Unassigned |
| Provider acceptance | Misc | Quota Continuation Acceptance | Verify trusted five-hour reset continuation in an authenticated native session without replaying accepted prompts or tools. | Open; Un-Reviewed; Not User Tested | Unassigned |
| Provider resources | Misc | Provider Policy Gates | Resolve compatible Gemini account or client access and Antigravity headless tool-policy gates without bypassing provider restrictions. | Blocked; Un-Reviewed; Not User Tested | Unassigned |
| Provider resources | Misc | Independent Reviewer Availability | Restore the permitted UAM reviewer connection and obtain hash-bound independent verdicts for work still awaiting that review. | Blocked; Un-Reviewed; Not User Tested | Unassigned |
| Data ownership | Performance | Copied Deletion Timing | Measure bounded deletion phases on isolated copied data with private provider homes and survivor-byte checks after confirming capacity. | Open; Un-Reviewed; Not User Tested | Unassigned |
| Data ownership | Misc | Live Folder Consolidation | Obtain David’s explicit mapping approval before consolidating the conflicting histories and folder aliases identified by the copied-data rehearsal. | Blocked; Un-Reviewed; Not User Tested | Unassigned |
| Platform acceptance | Misc | Current Windows Application Acceptance | Verify the latest source with full Windows application builds and live GUI checks instead of relying on older pinned builds or helper cross-compilation. | Open; Claude Review; Not User Tested | Unassigned |
| Platform acceptance | Graphical | macOS Dock Icon Acceptance | Verify the selected full-tile artwork renders at the intended size without cropping in the actual macOS Dock. | Open; Claude Review; Not User Tested | Unassigned |
| Platform acceptance | Misc | Physical Mobile Acceptance | Verify physical iPhone paging, authentication persistence, reconnect and installed icon behavior when the designated device is available. | Blocked; Claude Review; Not User Tested | Unassigned |
| Platform acceptance | Misc | Image And Cleanup Acceptance | Complete applicable live image, transcript-cleanup and account-allowance checks while retaining provider and platform limits. | Open; Un-Reviewed; Not User Tested | Unassigned |
| User acceptance | Misc | Explicit Change Signoff | Collect David’s item-specific testing and approval references before marking any cumulative change Complete. | Open; Not User Tested | Unassigned |
| AI performance | Misc | Context Pollution Report | Prepare the low-priority report on memory recall, resumed history, skills, instructions and handoff context without attributing model latency to unmeasured context growth. | Open; Un-Reviewed; Not User Tested | Unassigned |
| Deferred ideas | Misc | T3 PR Workflow | Retain T3-style PR handling and auto-settling as a record-only idea at the end of the list with no implementation requested. | Open; Claude Review; Not User Tested | Unassigned |

The historical commit inventory below preserves the source/test/build entries, including follow-up corrections and checkpoints.



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
