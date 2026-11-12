import type { ContextReference } from '../types/contextReference'
import type { WorkspaceFilePreview } from '../types/filePreview'
import type { StartupNativeImportStatus } from '../types/startupNativeImports'
import type { ExecutionRoutingSettings, RoutingReservation } from '../types/executionRouting'
import type { ChatSearchHit, ChatSearchNavigation } from '../types/chatSearch'
import type { OrganizationChange } from '../types/organization'
import type { CustomIconInput, CustomIconTarget } from '../types/customIcon'
import type { ComputerUseActionResult, ComputerUseBackend, ComputerUseControlState, ExecutionHost, RemoteDirectoryBrowseResult, Session, Folder, ViewMode, WorkspaceFolderRecoveryPreview } from '../types/session'
import type { Message, Attachment } from '../types/message'
import type { Provider } from '../types/provider'
import type { MemoryEntry, MemoryEntryDraft, MemoryLevel, MemoryScope, MemoryScanCandidate } from '../types/memory'
import type { MarkdownStoreConflictAction, MarkdownStoreDraft, MarkdownStoreEntry, MarkdownStoreImportCandidate, MarkdownStoreImportResult } from '../types/markdownStore'
import type { Goal, GoalStatus } from '../types/goal'
import type { CustomTheme, StoredTheme } from '../utils/themeStorage'
import type { ResourceCollection, ResourceReference, ResourceReferenceType } from '../types/resourceCollection'
import type {
  AcpBinding,
  ChatFileChangeReceiptsResponse,
  AcpUserInputAnswers,
  ChatAttachmentInput,
  CliBinding,
  CliTranscript,
  CliVersionManager,
  CppAppState,
  CppSettings,
  CppCliDebugState,
  CppDeletionProgress,
  EditorFileAssociation,
  GitWorktreeResult,
  GitWorktreeStatus,
  GitTurnCheckpointResult,
  MemoryActivity,
  MemoryWorkerBinding,
  McpServerConfiguration,
  CentralProviderConfiguration,
  ProviderChatDefaults,
  CreateSessionOptions,
  NewChatWorktreeContext,
  ProviderAvailabilityOverride,
  VoiceInputSettings,
  VcsStorageSettings,
  VcsSettingsRequest,
  StorageSettingsRequest,
  ProviderAvailabilityRequest,
  VoiceInputSettingsRequest,
  ProviderModelCatalog,
  ProviderAgentImportPreview,
  UamAgentCycleShortcut,
  UamAgentSummary,
  ShellAction,
  ComputePool,
  ComputePoolUsage,
  PullRequestWatch,
  PushChannelStatus,
  VcsCommitMessageSuggestion,
  VcsCommitResult,
  VcsCommitStatus,
  VcsHistoryPage,
  VcsOriginReply,
  VcsOriginView,
  VcsUpdateResult,
  VcsType,
  ComputerUseAllowedApplication,
  WindowsPowerShellPreference,
} from './cpp/types'
import type { CEFResponse } from '../ipc/cefBridge'
import type { StoreApi } from 'zustand'

/** Positive evidence of a guarded, immutable saved-history tail; never input authority. */
export type AcceptedHistoryPage = {
  chatId: string; birth: number; requestId: string; session: Session
  summaryDigest: string | undefined; summaryCount: number | undefined
  messagesDigest: string; startIndex: number; totalCount: number; messages: Message[]
}

export type MutationResult = { ok: boolean; error?: string }
export type CommandSafetyTierResult = MutationResult & { cancelled?: boolean }
export type GoalCreateResult = MutationResult & { goalId?: string }

/** Optional presentation observation only; action result, child controls and delivery authority remain separate. */
export type AcpPromptPresentationObserver = ((identity: { requestId: string; sessionId: string; createdAtMs: number }) => void) & {
  onInputRecorded?: (identity: { requestId: string; sessionId: string; createdAtMs: number; message: Message }) => void
  /** This attempt repeated a submission native had already admitted under the earlier attempt's request ID;
   *  `earlierRequestIds` are every earlier attempt of the same submission. */
  onDuplicateOf?: (originRequestId: string, earlierRequestIds: readonly string[]) => void
  /** Identifies one composer submission; the same token is passed only to retry that submission. */
  submissionToken?: string
}

export interface AppState extends VoiceInputSettings, VcsStorageSettings {
  providerAvailability: ProviderAvailabilityOverride[]
  startupNativeImports: StartupNativeImportStatus
  startupNativeImportReviewSequence: number
  startupNativeImportFolderId: string | null
  openStartupNativeImportReview: (folderId?: string) => void
  // Data
  folders: Folder[]
  resourceCollections: ResourceCollection[]
  organizationMutationSequence: number
  applyOrganizationChange: (change: OrganizationChange) => Promise<boolean>
  moveResourceToCollection: (collectionId: string | null, type: ResourceReferenceType, target: string, label: string) => Promise<boolean>
  sessions: Session[]
  activeSessionId: string | null
  lastAppliedStateRevision: number
  messages: Record<string, Message[]>
  historyStartIndexBySessionId: Record<string, number>
  chatSearchNavigation: ChatSearchNavigation | null
  jumpToSessionMessage: (chatId: string, messageIndex: number) => Promise<boolean>
  jumpToChatSearchHit: (hit: ChatSearchHit, queryRequestId: string) => Promise<boolean>
  consumeChatSearchJump: (chatId: string, generation: number) => boolean
  clearChatSearchNavigation: (queryRequestId?: string) => Promise<void>
  chatHistoryErrorBySessionId: Record<string, string>
  // Failed request context changes notice wording only, never native save authority.
  chatHistoryErrorWasRefreshBySessionId: Record<string, boolean>
  /** refreshNative marks an explicit native refresh, shown while it runs even over loaded messages. */
  chatHistoryReadBySessionId: Record<string, { requestId: string; birth: number; refreshNative?: boolean }>
  chatHistoryPageBySessionId: Record<string, AcceptedHistoryPage>
  goalsByChatId: Record<string, Goal[]>
  activeGoalIdByChatId: Record<string, string | null>
  goalModeByChatId: Record<string, boolean>

  // Providers
  providers: Provider[]
  cliBindingBySessionId: Record<string, CliBinding>
  acpBindingBySessionId: Record<string, AcpBinding>
  providerModelCatalogs: ProviderModelCatalog[]
  cliTranscriptBySessionId: Record<string, CliTranscript>
  cliDebugState: CppCliDebugState | null
  memoryEnabledDefault: boolean
  memoryLevelDefault: MemoryLevel
  memoryIdleDelaySeconds: number
  memoryRecallBudgetBytes: number
  goalMaxLoopIterations: number
  goalTokenBudget: number
  acpSetupInactivityTimeoutSeconds: number
  activeTurnInactivityTimeoutSeconds: number
  toolCallTimeoutSeconds: number
  streamLoopRepeatLimit: number
  acpTurnOutputLimitMiB: number
  appVersion: string
  runnerProtocolVersion: number
  showProviderIconsInSidebar: boolean
  showWorktreePathInSidebar: boolean
  steerActiveTurnOnSend: boolean
  gitEnabled: boolean
  svnEnabled: boolean
  updateChecksEnabled: boolean
  automaticProviderUpdates: boolean
  updateLastCheckedAt: string
  dismissedUpdateVersions: Record<string, string>
  memoryLastStatus: string
  /** Local display metadata, never native saved values or permission authority. */
  memorySettingsUnconfirmedFields: string[]
  memoryWorkerBindings: Record<string, MemoryWorkerBinding>
  permissionReviewerProviderId: string
  permissionReviewerModelId: string
  memoryActivity: MemoryActivity
  cliVersionManager: CliVersionManager
  markdownStoreDirectory: string
  defaultNewChatProviderId: string
  providerChatDefaults: Record<string, ProviderChatDefaults>
  fileExplorerApplication: string
  windowsPowerShell: WindowsPowerShellPreference
  defaultEditorPresetId: string
  editorFileAssociations: EditorFileAssociation[]
  centralProviderConfiguration: CentralProviderConfiguration
  mcpServers: McpServerConfiguration[]
  executionRouting: ExecutionRoutingSettings
  setExecutionRoutingSettings: (settings: ExecutionRoutingSettings) => Promise<void>
  releaseExecutionRoutingReservation: (expected: RoutingReservation) => Promise<void>
  executionHosts: ExecutionHost[]
  favoriteUamAgentIds: string[]
  /** Native sessions whose title starts with one of these (ignoring case) are never offered for import. */
  nativeImportExcludedTitlePrefixes: string[]
  /** Native sessions in one of these folders, or below one, are never offered for import. */
  nativeImportExcludedDirectories: string[]
  /** Startup import leaves out sessions with no activity in this many days (0 = no limit); Chat Data discovery still lists them. */
  nativeImportStartupMaxAgeDays: number
  uamAgentCycleShortcut: UamAgentCycleShortcut
  computerUseEnabled?: boolean
  computerUseRevocationPending?: boolean
  /** Native publication witness for these two fields; never a consent grant. */
  computerUseSettingsStateRevision?: number
  /** Session-only global settings request, shared across both settings surfaces. */
  computerUseSettingsOperation?: 'check' | 'enable' | 'disable'
  computerUseSettingsError?: string
  /** Native observation current when the session-only failure was received. */
  computerUseSettingsErrorStateRevision?: number
  computerUseAllowlistEnabled: boolean
  computerUseAllowedApplications: ComputerUseAllowedApplication[]
  branchSwitching?: boolean
  /** Backend accepts scopeExecutionHostId on settings saves. Scope selector is disabled when absent. */
  hostScopedSettings?: boolean
  hostScopedSettingsValues: NonNullable<CppSettings['hostScopedSettingsValues']>
  /** Settings page scope: '*' for all environments, otherwise an SSH execution host id. Session only. */
  settingsScopeHostId: string
  uamAgentsBySessionId: Record<string, UamAgentSummary[]>
  shellActions: ShellAction[]
  computePools: ComputePool[]
  computePoolUsage: ComputePoolUsage
  /** Pull requests linked to chats, with their watch state; native-owned. */
  prWatches: PullRequestWatch[]
  shellActionNotification: string
  statusLine: string
  deletionProgress?: CppDeletionProgress
  workspaceFolderRecoveryError: string

  // UI
  theme: StoredTheme
  customThemes: CustomTheme[]
  workingDisplayMode: 'compact' | 'verbose'
  expandWorkTraces: boolean
  collapsibleWorkSections: boolean
  isNewChatModalOpen: boolean
  newChatFolderId: string | null
  newChatWorktreeContext: NewChatWorktreeContext | null
  isSettingsOpen: boolean
  memoryLibraryScope: MemoryScope | null
  memoryLibraryEntries: MemoryEntry[]
  memoryLibraryLoading: boolean
  memoryLibraryError: string
  isMemoryScanModalOpen: boolean
  memoryScanCandidates: MemoryScanCandidate[]
  selectedMemoryScanChatIds: string[]
  memoryScanLoading: boolean
  /** Why the eligible-chat list could not be read; distinct from a scan that did not run. */
  memoryScanLoadError: string
  memoryScanRunning: boolean
  memoryScanError: string
  isMarkdownStoreOpen: boolean
  markdownStoreEntries: MarkdownStoreEntry[]
  markdownStoreLoading: boolean
  markdownStoreError: string
  markdownStoreAttachedBySessionId: Record<string, MarkdownStoreEntry[]>
  sidebarCollapsed: boolean
  commitPanelOpen: boolean
  sidebarWidthPx: number
  commitPanelWidthPx: number
  streamingMessageId: string | null
  pushChannelStatus: PushChannelStatus
  pushChannelError: string
  bootstrapStateError: string
  bootstrapStateLoading: boolean
  retryBootstrapState: () => void
  lastPushAtMs: number | null
  uiBuildId: string
  repositoryReviewBySessionId: Record<string, VcsCommitStatus>

  // Session actions
  acknowledgeChatAttention: (id: string, revision: string) => Promise<boolean>
  setChatSnoozedUntil: (id: string, untilMs: number) => Promise<{ ok: boolean; error?: string }>
  setChatSettled: (id: string, settled: boolean) => Promise<boolean>
  setActiveSession: (id: string | null, navigation?: { preserveSubagentRoute?: boolean; leafId?: string }) => void
  loadSessionMessages: (id: string, force?: boolean, refreshNative?: boolean) => Promise<void | false | AcceptedHistoryPage> | void
  loadOlderSessionMessages: (id: string) => Promise<boolean>
  /** Hover/focus intent: warm a cold local chat's first page without selecting it. */
  prefetchSessionMessages: (id: string) => void
  /** Unloads the oldest unopened warm chats beyond the retained cap, skipping any still processing. */
  evictWarmChats: () => void
  unloadSessionMessages: (id: string) => void
  addSession: (name: string, folderId: string | null, providerId?: string, modelId?: string, reasoningEffort?: string, viewMode?: ViewMode, executionHostId?: string, workspaceDirectory?: string, routingMode?: 'explicit' | 'automatic', options?: CreateSessionOptions) => Promise<boolean>
  branchFromMessage: (id: string, messageIndex: number, content?: string, retainedAttachmentIds?: string[], retainedMarkdownStoreFiles?: string[]) => Promise<string | null>
  retryFailedMessage: (id: string, messageIndex: number) => Promise<{ ok: boolean; error?: string }>
  discardUnsentMessage: (id: string, messageIndex: number, expectedMessageId: string) => Promise<boolean>
  editUnsentMessage: (id: string, messageIndex: number, expectedMessage: Message, text: string, retainedAttachments: Attachment[]) => Promise<{ ok: boolean; error?: string }>
  renameSession: (id: string, name: string) => void
  /** `quiet` leaves failure reporting to a caller that names each chat. */
  setSessionPinned: (id: string, pinned: boolean, options?: { quiet?: boolean }) => Promise<boolean>
  /** `onFailure` receives the native refusal reason, such as held queued input, when the bridge rejects the change. `allowUncertainDelivery` is the user's choice after the may-run-twice warning. */
  setSessionProvider: (id: string, providerId: string, onFailure?: (reason: string) => void, allowUncertainDelivery?: boolean) => Promise<boolean>
  /** Without `onFailure`, a refusal is reported through the shared app failure notice. */
  setSessionModel: (id: string, modelId: string, onFailure?: (reason: string) => void) => Promise<boolean>
  setSessionReviewerModel: (id: string, modelId: string) => Promise<boolean>
  setSessionApprovalMode: (id: string, modeId: string) => Promise<boolean>
  setSessionUamAgent: (id: string, agentId: string) => Promise<boolean>
  setSessionUamControlEnabled: (id: string, enabled: boolean, applyOnNextLaunch?: boolean) => Promise<boolean>
  refreshUamAgents: (id: string) => Promise<boolean>
  browseProviderAgentImport: (currentValue?: string) => Promise<string | null>
  previewProviderAgentImport: (providerId: string, sourcePath: string) => Promise<ProviderAgentImportPreview | null>
  importProviderAgent: (options: { chatId: string; providerId: string; sourcePath: string; canonicalId: string; workspaceAccess: 'read' | 'write'; workspaceScope: boolean; acknowledgeIgnoredFields: boolean }) => Promise<boolean>
  setSessionCommandSafetyTier: (id: string, tier: 'off' | 'acceptEdits' | 'aiReview' | 'yolo') => Promise<CommandSafetyTierResult>
	setSessionComputerUseEnabled: (id: string, enabled: boolean) => Promise<ComputerUseActionResult>
	setSessionComputerUseBackend: (id: string, backend: ComputerUseBackend) => Promise<ComputerUseActionResult>
	setSessionComputerUseControl: (id: string, state: ComputerUseControlState) => Promise<ComputerUseActionResult>
  setSessionMemoryEnabled: (id: string, enabled: boolean) => Promise<boolean>
  setSessionMemoryLevel: (id: string, level: MemoryLevel) => Promise<boolean>
  setSessionSmallModelMode: (id: string, enabled: boolean) => Promise<boolean>
  /** Internal display observation/overlay; never publication or native mutation authority. */
  reconcileMemorySettingsProjection: (projection: Partial<AppState>, suppliedNativeSettings?: Partial<CppSettings>) => Partial<AppState>
  setMemorySettings: (settings: Partial<Pick<AppState, 'memoryEnabledDefault' | 'memoryLevelDefault' | 'memoryIdleDelaySeconds' | 'memoryRecallBudgetBytes' | 'goalMaxLoopIterations' | 'goalTokenBudget' | 'acpSetupInactivityTimeoutSeconds' | 'activeTurnInactivityTimeoutSeconds' | 'toolCallTimeoutSeconds' | 'streamLoopRepeatLimit' | 'acpTurnOutputLimitMiB' | 'memoryWorkerBindings' | 'permissionReviewerProviderId' | 'permissionReviewerModelId'>>) => Promise<boolean>
  setUpdateSettings: (settings: Partial<Pick<AppState, 'automaticProviderUpdates' | 'updateChecksEnabled' | 'updateLastCheckedAt' | 'dismissedUpdateVersions'>>) => Promise<boolean>
  /** Without `onFailure`, a refusal is reported through the shared app failure notice. */
  setSessionCodexOptions: (id: string, options: { reasoningEffort?: string; serviceTier?: string; serviceTierExplicit?: boolean }, onFailure?: (reason: string) => void) => Promise<boolean>
  setProviderChatDefaults: (settings: { defaultNewChatProviderId?: string; providerChatDefaults?: Record<string, ProviderChatDefaults> }, scopeExecutionHostId?: string) => Promise<boolean>
  setEditorSettings: (settings: Pick<AppState, 'defaultEditorPresetId' | 'editorFileAssociations'> & Partial<Pick<AppState, 'fileExplorerApplication' | 'windowsPowerShell'>>) => Promise<boolean>
  setCentralProviderConfiguration: (configuration: CentralProviderConfiguration) => Promise<{ ok: boolean; error?: string }>
  setMcpServers: (servers: McpServerConfiguration[], scopeExecutionHostId?: string) => Promise<{ ok: boolean; error?: string }>
  setUamAgentPreferences: (settings: { favoriteUamAgentIds: string[]; uamAgentCycleShortcut: UamAgentCycleShortcut }) => Promise<boolean>
  setNativeImportFilters: (filters: { titlePrefixes: string[]; directories: string[]; startupMaxAgeDays: number }) => Promise<boolean>
  setShellActions: (actions: ShellAction[]) => Promise<boolean>
  setComputePools: (pools: ComputePool[]) => Promise<boolean>
  applyShellActions: () => Promise<boolean>
  dismissShellActionNotification: () => Promise<void>
  refreshCliProviderVersion: (providerId?: string, executionHostId?: string) => Promise<boolean>
  applyCliProviderVersion: (providerId: string, version: string, executionHostId?: string) => Promise<boolean>
  browseMarkdownStoreDirectory: (currentValue: string) => Promise<string | null>
  setMarkdownStoreDirectory: (directory: string) => Promise<boolean>
  openMarkdownStore: () => Promise<boolean>
  closeMarkdownStore: () => void
  clearMarkdownStoreError: () => void
  refreshMarkdownStore: () => Promise<boolean>
  createMarkdownStoreEntry: (draft: MarkdownStoreDraft) => Promise<boolean>
  updateMarkdownStoreEntry: (entry: MarkdownStoreEntry, draft: MarkdownStoreDraft) => Promise<boolean>
  setMarkdownStoreFavorite: (entry: MarkdownStoreEntry, favorite: boolean) => Promise<boolean>
  browseMarkdownStoreImport: (kind: 'file' | 'folder') => Promise<string | null>
  previewMarkdownStoreImports: (options: { includeProviders?: boolean; paths?: string[] }) => Promise<MarkdownStoreImportCandidate[]>
  importMarkdownStoreEntries: (imports: Array<{ sourceProvider: string; sourcePath: string; conflictAction: MarkdownStoreConflictAction }>) => Promise<MarkdownStoreImportResult[]>
  revealMarkdownStoreEntry: (entry: MarkdownStoreEntry) => Promise<boolean>
  editMarkdownStoreEntry: (entry: MarkdownStoreEntry) => Promise<boolean>
  attachMarkdownStoreEntry: (sessionId: string, entry: MarkdownStoreEntry) => void
  detachMarkdownStoreEntry: (sessionId: string, filePath: string) => void
  openSessionWorkspace: (id: string) => Promise<boolean>
  openSessionWorkspaceEditor: (id: string) => Promise<boolean>
  openSessionTerminal: (id: string) => Promise<boolean>
  openSubAgentSession: (sourceChatId: string, nativeSessionId: string, title?: string, selectChat?: boolean, onFailure?: (message: string) => void) => Promise<string | null>
  getChatWorktreeStatus: (id: string) => Promise<GitWorktreeStatus | null>
  createChatWorktree: (id: string) => Promise<GitWorktreeResult>
  discardChatWorktreeChanges: (id: string) => Promise<GitWorktreeResult>
  portChatWorktreeChanges: (id: string) => Promise<GitWorktreeResult>
  previewChatTurnRollback: (id: string, messageIndex: number) => Promise<GitTurnCheckpointResult | null>
  rollbackChatTurn: (id: string, messageIndex: number) => Promise<GitTurnCheckpointResult | null>
  getChatFileChangeReceipts: (id: string) => Promise<ChatFileChangeReceiptsResponse>
  getVcsCommitStatus: (id: string, vcsType?: VcsType, options?: { includeLineStats?: boolean; contextOnly?: boolean; requestId?: string; comparisonRef?: string }) => Promise<VcsCommitStatus | null>
  getWorkspaceFilePreview: (id: string, path: string) => Promise<WorkspaceFilePreview>
  getVcsFileDiff: (id: string, path: string, vcsType: VcsType, comparisonRef?: string) => Promise<string>
  commitVcsChanges: (id: string, vcsType: VcsType, message: string, files: string[]) => Promise<VcsCommitResult>
  /** One page of the workspace's revision history, newest first. Throws with the system's own error. */
  getVcsHistory: (id: string, vcsType: VcsType, options?: { before?: string; limit?: number; search?: string; requestId?: string }) => Promise<VcsHistoryPage>
  /** The patch one listed revision applied to one of its changed paths. */
  getVcsRevisionDiff: (id: string, vcsType: VcsType, revision: string, path: string, requestId?: string) => Promise<string>
  /** One part of how the workspace's branch or tag relates to the path it was copied from. Throws with the system's own error. */
  getVcsOrigin: (id: string, vcsType: VcsType, view: VcsOriginView, options?: { before?: string; limit?: number; revisions?: string[]; requestId?: string }) => Promise<VcsOriginReply>
  /** `requestId` lets cancelVcsRequest stop it; a stopped update may leave the working copy needing cleanup. */
  updateVcsWorkspace: (id: string, vcsType: VcsType, requestId?: string) => Promise<VcsUpdateResult>
  /** Stops a history or revision-diff request that is still running. */
  cancelVcsRequest: (requestId: string) => void
  generateVcsCommitMessage: (id: string, vcsType: VcsType, files: string[]) => Promise<VcsCommitMessageSuggestion | null>
  deleteSession: (id: string) => Promise<boolean>
  deleteSessions: (ids: string[]) => Promise<boolean>

  // Goal actions
	setGoal: (chatId: string, objective: string, tokenBudget?: number, executionOwner?: 'uam' | 'provider') => Promise<GoalCreateResult>
  updateGoalStatus: (chatId: string, goalId: string, status: GoalStatus) => Promise<MutationResult>
  updateGoalObjective: (chatId: string, goalId: string, objective: string) => Promise<MutationResult>
  removeGoal: (chatId: string, goalId: string) => Promise<MutationResult>
  resumeGoal: (chatId: string, goalId: string, restart?: boolean) => Promise<MutationResult>
  setGoalMode: (chatId: string, active: boolean) => void
  clearActiveGoal: (chatId: string) => Promise<MutationResult>

  // Folder actions
  addFolder: (name: string, parentId: string | null, directory: string, executionHostId?: string) => Promise<boolean>
  toggleFolder: (id: string) => void
  reorderFolders: (folderIds: string[]) => Promise<boolean>
  rescanFolderChats: (id: string) => Promise<boolean>
  previewUnsortedWorkspaceFolders: () => Promise<WorkspaceFolderRecoveryPreview | null>
  rebuildUnsortedWorkspaceFolders: () => Promise<boolean>
  renameFolder: (id: string, name: string, directory: string) => Promise<boolean>
  workspaceDeletionError: string
  deleteFolder: (id: string) => Promise<boolean>
  deleteFolders: (ids: string[]) => Promise<boolean>
  browseFolderDirectory: (currentValue: string) => Promise<string | null>
  listRemoteDirectories: (executionHostId: string, directory: string) => Promise<RemoteDirectoryBrowseResult>
  setCustomIcon: (targetType: CustomIconTarget, targetId: string, icon: CustomIconInput) => Promise<boolean>
  createResourceCollection: (name: string, parentId?: string | null) => Promise<ResourceCollection | null>
  /** Nests a collection (with its references and children) under parentId, or moves it to the top level for null. */
  moveResourceCollection: (collectionId: string, parentId: string | null) => Promise<boolean>
  renameResourceCollection: (collectionId: string, name: string) => Promise<boolean>
  /** Child collections move up a level unless deleteChildren removes them too; referenced targets are never deleted. */
  deleteResourceCollection: (collectionId: string, deleteChildren?: boolean) => Promise<boolean>
  toggleResourceCollection: (collectionId: string) => Promise<boolean>
  reorderResourceCollections: (collectionIds: string[]) => Promise<boolean>
  addResourceReference: (collectionId: string, type: ResourceReferenceType, target: string, label?: string) => Promise<ResourceReference | null>
  removeResourceReference: (collectionId: string, referenceId: string) => Promise<boolean>
  reorderResourceReferences: (collectionId: string, referenceIds: string[]) => Promise<boolean>
  openAllMemoryLibrary: () => Promise<boolean>
  openGlobalMemoryLibrary: () => Promise<boolean>
  openFolderMemoryLibrary: (folderId: string) => Promise<boolean>
  closeMemoryLibrary: () => void
  refreshMemoryLibrary: () => Promise<boolean>
  createMemoryEntry: (draft: MemoryEntryDraft) => Promise<boolean>
  deleteMemoryEntry: (entryId: string) => Promise<boolean>
  deleteMemoryEntries: (entryIds: string[]) => Promise<boolean>
  openMemoryRoot: () => Promise<boolean>
  revealMemoryEntry: (entryId: string) => Promise<boolean>
  readMemoryEntry: (request: Pick<MemoryScope, 'scopeType' | 'folderId'> & { entryId: string }) => Promise<CEFResponse<{ content: string }>>
  openMemoryScanModal: () => Promise<boolean>
  closeMemoryScanModal: () => void
  toggleMemoryScanChat: (chatId: string) => void
  selectAllMemoryScanChats: () => void
  selectNoMemoryScanChats: () => void
  startMemoryScan: () => Promise<boolean>

  // CLI actions
  setCliBinding: (sessionId: string, binding: Partial<CliBinding>) => void

  // ACP actions
  stageChatContextReference: (sessionId: string, reference: ContextReference) => boolean
  stageChatAttachments: (sessionId: string, items: ChatAttachmentInput[]) => Promise<Attachment[]>
  sendAcpPrompt: (sessionId: string, text: string, attachments?: Attachment[], steerNow?: boolean, onStarted?: AcpPromptPresentationObserver) => Promise<boolean>
  removeQueuedAcpPrompt: (sessionId: string, index: number, expected?: { id: string; revision: number }) => Promise<boolean>
  retryUnsentAcpPrompt: (sessionId: string, expected: { id: string; revision: number }) => Promise<boolean>
  steerQueuedAcpPrompt: (sessionId: string, index: number, expected?: { id: string; revision: number }) => Promise<boolean>
  resumeQueuedAcpPrompts: (sessionId: string, expected: { id: string; revision: number }[]) => Promise<boolean>
  editQueuedAcpPrompt: (sessionId: string, promptId: string, expectedRevision: number, text: string, retainedAttachmentIds?: string[]) => Promise<boolean>
  reorderQueuedAcpPrompts: (sessionId: string, expected: { id: string; revision: number }[], orderedIds: string[]) => Promise<boolean>
  /** automatic: asked by the UI itself rather than by Refresh or Retry; the backend skips it while its cached models are current. */
  discoverProviderModels: (sessionId: string, providerId?: string, workspaceDirectory?: string, executionHostId?: string, automatic?: boolean) => Promise<boolean>
  setAcpConfigOption: (sessionId: string, configId: string, value: string) => Promise<boolean>
  cancelAcpTurn: (sessionId: string) => Promise<boolean>
  resolveAcpPermission: (sessionId: string, requestId: string, optionId: string | 'cancelled') => Promise<boolean>
  resolveAcpUserInput: (sessionId: string, requestId: string, answers: AcpUserInputAnswers) => Promise<boolean>
  stopAcpSession: (sessionId: string, purpose?: 'interrupt' | 'timeout') => Promise<boolean>
  /** The user's override of a remote stop the host never confirmed; a process may still be running there. */
  treatRemoteStopAsStopped: (sessionId: string) => Promise<boolean>

  // UI actions
  setTheme: (theme: StoredTheme) => void
  setWorkingDisplayMode: (mode: 'compact' | 'verbose') => void
  setExpandWorkTraces: (expanded: boolean) => void
  setCollapsibleWorkSections: (enabled: boolean) => void
  refreshCustomThemes: () => Promise<boolean>
  saveCustomTheme: (theme: CustomTheme) => Promise<CustomTheme | null>
  deleteCustomTheme: (id: string) => Promise<boolean>
  setVcsSettings: (settings: VcsSettingsRequest) => Promise<boolean>
  setProviderAvailability: (settings: ProviderAvailabilityRequest) => Promise<boolean>
  setVoiceInputSettings: (settings: VoiceInputSettingsRequest) => Promise<boolean>
  setStorageSettings: (settings: StorageSettingsRequest) => Promise<boolean>
  setSidebarSettings: (settings: Pick<AppState, 'showProviderIconsInSidebar' | 'showWorktreePathInSidebar'>) => Promise<boolean>
  /** Saves what composer Send does while a turn runs: steer it (true) or queue for the next turn (false). */
  setSteerActiveTurnOnSend: (steer: boolean) => Promise<boolean>
  setNewChatModalOpen: (open: boolean, folderId?: string | null, worktreeContext?: NewChatWorktreeContext | null) => void
  setSettingsOpen: (open: boolean) => void
  setSettingsScopeHostId: (hostId: string) => void
  setSidebarCollapsed: (collapsed: boolean) => void
  setCommitPanelOpen: (open: boolean) => void
  setSidebarWidthPx: (width: number) => void
  setCommitPanelWidthPx: (width: number) => void

  // CEF bootstrap
  loadFromCef: (state: CppAppState) => void
}

export type ZustandSet = StoreApi<AppState>['setState']
export type ZustandGet = () => AppState
