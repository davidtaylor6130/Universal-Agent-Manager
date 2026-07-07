import type { Session, Folder } from '../types/session'
import type { Message, Attachment } from '../types/message'
import type { Provider } from '../types/provider'
import type { MemoryEntry, MemoryEntryDraft, MemoryScope, MemoryScanCandidate } from '../types/memory'
import type { MarkdownStoreDraft, MarkdownStoreEntry } from '../types/markdownStore'
import type { Goal, GoalStatus } from '../types/goal'
import type { StoredTheme } from '../utils/themeStorage'
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
  MemoryActivity,
  MemoryWorkerBinding,
  ProviderChatDefaults,
  PushChannelStatus,
  VcsCommitMessageSuggestion,
  VcsCommitResult,
  VcsCommitStatus,
  VcsType,
} from './cpp/types'
import type { StoreApi } from 'zustand'

export interface AppState {
  // Data
  folders: Folder[]
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
  cliTranscriptBySessionId: Record<string, CliTranscript>
  cliDebugState: CppCliDebugState | null
  memoryEnabledDefault: boolean
  memoryIdleDelaySeconds: number
  memoryRecallBudgetBytes: number
  memoryLastStatus: string
  memoryWorkerBindings: Record<string, MemoryWorkerBinding>
  memoryActivity: MemoryActivity
  cliVersionManager: CliVersionManager
  markdownStoreDirectory: string
  defaultNewChatProviderId: string
  providerChatDefaults: Record<string, ProviderChatDefaults>
  defaultEditorPresetId: string
  editorFileAssociations: EditorFileAssociation[]

  // UI
  theme: StoredTheme
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

  // Session actions
  setActiveSession: (id: string) => void
  addSession: (name: string, folderId: string | null, providerId?: string) => void
  renameSession: (id: string, name: string) => void
  setSessionPinned: (id: string, pinned: boolean) => Promise<boolean>
  setSessionProvider: (id: string, providerId: string) => Promise<boolean>
  setSessionModel: (id: string, modelId: string) => Promise<boolean>
  setSessionApprovalMode: (id: string, modeId: string) => Promise<boolean>
  setSessionAutoApproveCommands: (id: string, enabled: boolean) => Promise<boolean>
  setSessionMemoryEnabled: (id: string, enabled: boolean) => Promise<boolean>
  setMemorySettings: (settings: Partial<Pick<AppState, 'memoryEnabledDefault' | 'memoryIdleDelaySeconds' | 'memoryRecallBudgetBytes' | 'memoryWorkerBindings'>>) => Promise<boolean>
  setSessionCodexOptions: (id: string, options: { reasoningEffort?: string; serviceTier?: string }) => Promise<boolean>
  setProviderChatDefaults: (settings: { defaultNewChatProviderId?: string; providerChatDefaults?: Record<string, ProviderChatDefaults> }) => Promise<boolean>
  setEditorSettings: (settings: Pick<AppState, 'defaultEditorPresetId' | 'editorFileAssociations'>) => Promise<boolean>
  refreshCliProviderVersion: (providerId?: string) => Promise<boolean>
  applyCliProviderVersion: (providerId: string, version: string) => Promise<boolean>
  browseMarkdownStoreDirectory: (currentValue: string) => Promise<string | null>
  setMarkdownStoreDirectory: (directory: string) => Promise<boolean>
  openMarkdownStore: () => Promise<boolean>
  closeMarkdownStore: () => void
  refreshMarkdownStore: () => Promise<boolean>
  createMarkdownStoreEntry: (draft: MarkdownStoreDraft) => Promise<boolean>
  revealMarkdownStoreEntry: (entry: MarkdownStoreEntry) => Promise<boolean>
  editMarkdownStoreEntry: (entry: MarkdownStoreEntry) => Promise<boolean>
  attachMarkdownStoreEntry: (sessionId: string, entry: MarkdownStoreEntry) => void
  detachMarkdownStoreEntry: (sessionId: string, filePath: string) => void
  openSessionWorkspace: (id: string) => Promise<boolean>
  openSessionWorkspaceEditor: (id: string) => Promise<boolean>
  openSessionTerminal: (id: string) => Promise<boolean>
  openSubAgentSession: (sourceChatId: string, nativeSessionId: string, title?: string) => Promise<boolean>
  getChatWorktreeStatus: (id: string) => Promise<GitWorktreeStatus | null>
  createChatWorktree: (id: string) => Promise<GitWorktreeResult>
  discardChatWorktreeChanges: (id: string) => Promise<GitWorktreeResult>
  portChatWorktreeChanges: (id: string) => Promise<GitWorktreeResult>
  getVcsCommitStatus: (id: string, vcsType?: VcsType, options?: { includeLineStats?: boolean; requestId?: string }) => Promise<VcsCommitStatus | null>
  getVcsFileDiff: (id: string, path: string, vcsType: VcsType) => Promise<string>
  commitVcsChanges: (id: string, vcsType: VcsType, message: string, files: string[]) => Promise<VcsCommitResult>
  generateVcsCommitMessage: (id: string, vcsType: VcsType, files: string[]) => Promise<VcsCommitMessageSuggestion | null>
  deleteSession: (id: string) => void

  // Goal actions
  setGoal: (chatId: string, objective: string, tokenBudget?: number) => Promise<string | null>
  updateGoalStatus: (goalId: string, status: GoalStatus) => Promise<boolean>
  removeGoal: (goalId: string) => Promise<boolean>
  resumeGoal: (chatId: string, goalId: string) => Promise<boolean>
  setGoalMode: (chatId: string, active: boolean) => void
  setDefaultGoalTokenBudget: (chatId: string, tokenBudget: number) => void
  clearActiveGoal: (chatId: string) => Promise<boolean>

  // Folder actions
  addFolder: (name: string, parentId: string | null, directory: string) => Promise<boolean>
  toggleFolder: (id: string) => void
  renameFolder: (id: string, name: string, directory: string) => void
  deleteFolder: (id: string) => void
  browseFolderDirectory: (currentValue: string) => Promise<string | null>
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
  sendAcpPrompt: (sessionId: string, text: string, attachments?: Attachment[]) => Promise<boolean>
  cancelAcpTurn: (sessionId: string) => Promise<boolean>
  resolveAcpPermission: (sessionId: string, requestId: string, optionId: string | 'cancelled') => Promise<boolean>
  resolveAcpUserInput: (sessionId: string, requestId: string, answers: AcpUserInputAnswers) => Promise<boolean>
  stopAcpSession: (sessionId: string) => Promise<boolean>

  // UI actions
  setTheme: (theme: StoredTheme) => void
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
