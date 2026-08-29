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
  AcpUserInputAnswers,
  ChatAttachmentInput,
  CliBinding,
  CliTranscript,
  CliVersionManager,
  CppAppState,
  CppCliDebugState,
  EditorFileAssociation,
  GitWorktreeResult,
  GitWorktreeStatus,
  GitTurnCheckpointResult,
  MemoryActivity,
  MemoryWorkerBinding,
  McpServerConfiguration,
  ProviderChatDefaults,
  ProviderModelCatalog,
  ProviderAgentImportPreview,
  UamAgentCycleShortcut,
  UamAgentSummary,
  ShellAction,
  PushChannelStatus,
  VcsCommitMessageSuggestion,
  VcsCommitResult,
  VcsCommitStatus,
  VcsType,
} from './cpp/types'
import type { StoreApi } from 'zustand'

export type MutationResult = { ok: boolean; error?: string }
export type CommandSafetyTierResult = MutationResult & { cancelled?: boolean }
export type GoalCreateResult = MutationResult & { goalId?: string }

export interface AppState {
  // Data
  folders: Folder[]
  resourceCollections: ResourceCollection[]
  sessions: Session[]
  activeSessionId: string | null
  lastAppliedStateRevision: number
  messages: Record<string, Message[]>
  goalsByChatId: Record<string, Goal[]>
  activeGoalIdByChatId: Record<string, string | null>
  goalModeByChatId: Record<string, boolean>
  defaultGoalTokenBudgetByChatId: Record<string, number>

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
  acpSetupInactivityTimeoutSeconds: number
  acpTurnOutputLimitMiB: number
  appVersion: string
  runnerProtocolVersion: number
  showProviderIconsInSidebar: boolean
  showWorktreePathInSidebar: boolean
  updateChecksEnabled: boolean
  updateLastCheckedAt: string
  dismissedUpdateVersions: Record<string, string>
  memoryLastStatus: string
  memoryWorkerBindings: Record<string, MemoryWorkerBinding>
  permissionReviewerProviderId: string
  permissionReviewerModelId: string
  memoryActivity: MemoryActivity
  cliVersionManager: CliVersionManager
  markdownStoreDirectory: string
  defaultNewChatProviderId: string
  providerChatDefaults: Record<string, ProviderChatDefaults>
  defaultEditorPresetId: string
  editorFileAssociations: EditorFileAssociation[]
  mcpServers: McpServerConfiguration[]
  executionHosts: ExecutionHost[]
  favoriteUamAgentIds: string[]
  uamAgentCycleShortcut: UamAgentCycleShortcut
  uamAgentsBySessionId: Record<string, UamAgentSummary[]>
  shellActions: ShellAction[]
  shellActionNotification: string
  statusLine: string
  workspaceFolderRecoveryError: string

  // UI
  theme: StoredTheme
  customThemes: CustomTheme[]
  workingDisplayMode: 'compact' | 'verbose'
  isNewChatModalOpen: boolean
  newChatFolderId: string | null
  isSettingsOpen: boolean
  memoryLibraryScope: MemoryScope | null
  memoryLibraryEntries: MemoryEntry[]
  memoryLibraryLoading: boolean
  memoryLibraryError: string
  isMemoryScanModalOpen: boolean
  memoryScanCandidates: MemoryScanCandidate[]
  selectedMemoryScanChatIds: string[]
  memoryScanLoading: boolean
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
  lastPushAtMs: number | null
  uiBuildId: string
  repositoryReviewBySessionId: Record<string, VcsCommitStatus>

  // Session actions
  setActiveSession: (id: string | null) => void
  loadSessionMessages: (id: string, force?: boolean) => void
  addSession: (name: string, folderId: string | null, providerId?: string, modelId?: string, reasoningEffort?: string, viewMode?: ViewMode, executionHostId?: string, workspaceDirectory?: string) => Promise<boolean>
  branchFromMessage: (id: string, messageIndex: number, content?: string) => Promise<string | null>
  renameSession: (id: string, name: string) => void
  setSessionPinned: (id: string, pinned: boolean) => Promise<boolean>
  setSessionProvider: (id: string, providerId: string) => Promise<boolean>
  setSessionModel: (id: string, modelId: string) => Promise<boolean>
  setSessionReviewerModel: (id: string, modelId: string) => Promise<boolean>
  setSessionApprovalMode: (id: string, modeId: string) => Promise<boolean>
  setSessionUamAgent: (id: string, agentId: string) => Promise<boolean>
  setSessionUamControlEnabled: (id: string, enabled: boolean) => Promise<boolean>
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
  setMemorySettings: (settings: Partial<Pick<AppState, 'memoryEnabledDefault' | 'memoryLevelDefault' | 'memoryIdleDelaySeconds' | 'memoryRecallBudgetBytes' | 'goalMaxLoopIterations' | 'acpSetupInactivityTimeoutSeconds' | 'acpTurnOutputLimitMiB' | 'memoryWorkerBindings' | 'permissionReviewerProviderId' | 'permissionReviewerModelId'>>) => Promise<boolean>
  setUpdateSettings: (settings: Partial<Pick<AppState, 'updateChecksEnabled' | 'updateLastCheckedAt' | 'dismissedUpdateVersions'>>) => Promise<boolean>
  setSessionCodexOptions: (id: string, options: { reasoningEffort?: string; serviceTier?: string; serviceTierExplicit?: boolean }) => Promise<boolean>
  setProviderChatDefaults: (settings: { defaultNewChatProviderId?: string; providerChatDefaults?: Record<string, ProviderChatDefaults> }) => Promise<boolean>
  setEditorSettings: (settings: Pick<AppState, 'defaultEditorPresetId' | 'editorFileAssociations'>) => Promise<boolean>
  setMcpServers: (servers: McpServerConfiguration[]) => Promise<{ ok: boolean; error?: string }>
  setUamAgentPreferences: (settings: { favoriteUamAgentIds: string[]; uamAgentCycleShortcut: UamAgentCycleShortcut }) => Promise<boolean>
  setShellActions: (actions: ShellAction[]) => Promise<boolean>
  applyShellActions: () => Promise<boolean>
  dismissShellActionNotification: () => Promise<void>
  refreshCliProviderVersion: (providerId?: string) => Promise<boolean>
  applyCliProviderVersion: (providerId: string, version: string) => Promise<boolean>
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
  openSubAgentSession: (sourceChatId: string, nativeSessionId: string, title?: string, selectChat?: boolean) => Promise<string | null>
  getChatWorktreeStatus: (id: string) => Promise<GitWorktreeStatus | null>
  createChatWorktree: (id: string) => Promise<GitWorktreeResult>
  discardChatWorktreeChanges: (id: string) => Promise<GitWorktreeResult>
  portChatWorktreeChanges: (id: string) => Promise<GitWorktreeResult>
  previewChatTurnRollback: (id: string, messageIndex: number) => Promise<GitTurnCheckpointResult | null>
  rollbackChatTurn: (id: string, messageIndex: number) => Promise<GitTurnCheckpointResult | null>
  getVcsCommitStatus: (id: string, vcsType?: VcsType, options?: { includeLineStats?: boolean; requestId?: string; comparisonRef?: string }) => Promise<VcsCommitStatus | null>
  getVcsFileDiff: (id: string, path: string, vcsType: VcsType, comparisonRef?: string) => Promise<string>
  commitVcsChanges: (id: string, vcsType: VcsType, message: string, files: string[]) => Promise<VcsCommitResult>
  generateVcsCommitMessage: (id: string, vcsType: VcsType, files: string[]) => Promise<VcsCommitMessageSuggestion | null>
  deleteSession: (id: string) => Promise<boolean>
  deleteSessions: (ids: string[]) => Promise<boolean>

  // Goal actions
	setGoal: (chatId: string, objective: string, tokenBudget?: number, executionOwner?: 'uam' | 'provider') => Promise<GoalCreateResult>
  updateGoalStatus: (chatId: string, goalId: string, status: GoalStatus) => Promise<MutationResult>
  updateGoalObjective: (chatId: string, goalId: string, objective: string) => Promise<MutationResult>
  removeGoal: (chatId: string, goalId: string) => Promise<MutationResult>
  resumeGoal: (chatId: string, goalId: string) => Promise<MutationResult>
  setGoalMode: (chatId: string, active: boolean) => void
  setDefaultGoalTokenBudget: (chatId: string, tokenBudget: number) => void
  clearActiveGoal: (chatId: string) => Promise<MutationResult>

  // Folder actions
  addFolder: (name: string, parentId: string | null, directory: string, executionHostId?: string) => Promise<boolean>
  toggleFolder: (id: string) => void
  reorderFolders: (folderIds: string[]) => Promise<boolean>
  rescanFolderChats: (id: string) => Promise<boolean>
  previewUnsortedWorkspaceFolders: () => Promise<WorkspaceFolderRecoveryPreview | null>
  rebuildUnsortedWorkspaceFolders: () => Promise<boolean>
  renameFolder: (id: string, name: string, directory: string) => Promise<boolean>
  deleteFolder: (id: string) => Promise<boolean>
  browseFolderDirectory: (currentValue: string) => Promise<string | null>
  listRemoteDirectories: (executionHostId: string, directory: string) => Promise<RemoteDirectoryBrowseResult>
  createResourceCollection: (name: string) => Promise<ResourceCollection | null>
  renameResourceCollection: (collectionId: string, name: string) => Promise<boolean>
  deleteResourceCollection: (collectionId: string) => Promise<boolean>
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
  openMemoryScanModal: () => Promise<boolean>
  closeMemoryScanModal: () => void
  toggleMemoryScanChat: (chatId: string) => void
  selectAllMemoryScanChats: () => void
  selectNoMemoryScanChats: () => void
  startMemoryScan: () => Promise<boolean>

  // CLI actions
  setCliBinding: (sessionId: string, binding: Partial<CliBinding>) => void

  // ACP actions
  stageChatAttachments: (sessionId: string, items: ChatAttachmentInput[]) => Promise<Attachment[]>
  sendAcpPrompt: (sessionId: string, text: string, attachments?: Attachment[], steerNow?: boolean) => Promise<boolean>
  removeQueuedAcpPrompt: (sessionId: string, index: number) => Promise<boolean>
  steerQueuedAcpPrompt: (sessionId: string, index: number) => Promise<boolean>
  discoverProviderModels: (sessionId: string, providerId?: string, workspaceDirectory?: string, executionHostId?: string) => Promise<boolean>
  setAcpConfigOption: (sessionId: string, configId: string, value: string) => Promise<boolean>
  cancelAcpTurn: (sessionId: string) => Promise<boolean>
  resolveAcpPermission: (sessionId: string, requestId: string, optionId: string | 'cancelled') => Promise<boolean>
  resolveAcpUserInput: (sessionId: string, requestId: string, answers: AcpUserInputAnswers) => Promise<boolean>
  stopAcpSession: (sessionId: string) => Promise<boolean>

  // UI actions
  setTheme: (theme: StoredTheme) => void
  setWorkingDisplayMode: (mode: 'compact' | 'verbose') => void
  refreshCustomThemes: () => Promise<boolean>
  saveCustomTheme: (theme: CustomTheme) => Promise<CustomTheme | null>
  deleteCustomTheme: (id: string) => Promise<boolean>
  setSidebarSettings: (settings: Pick<AppState, 'showProviderIconsInSidebar' | 'showWorktreePathInSidebar'>) => Promise<boolean>
  setNewChatModalOpen: (open: boolean, folderId?: string | null) => void
  setSettingsOpen: (open: boolean) => void
  setSidebarCollapsed: (collapsed: boolean) => void
  setCommitPanelOpen: (open: boolean) => void
  setSidebarWidthPx: (width: number) => void
  setCommitPanelWidthPx: (width: number) => void

  // CEF bootstrap
  loadFromCef: (state: CppAppState) => void
}

export type ZustandSet = StoreApi<AppState>['setState']
export type ZustandGet = () => AppState
