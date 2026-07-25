// C++ state serialisation types (mirrors state_serializer.cpp output) plus the
// frontend-facing binding/push types. Extracted from useAppStore.ts (MO-1); the
// store re-exports everything here so existing imports keep working.

import type { Attachment, MessageBlock } from '../../types/message'
import type { GoalStatus } from '../../types/goal'
import type { StoredTheme } from '../../utils/themeStorage'
import type { ResourceCollection } from '../../types/resourceCollection'
import type { MemoryLevel } from '../../types/memory'

export type CliLifecycleState = 'disabled' | 'stopped' | 'idle' | 'busy' | 'shuttingDown'
export type AcpLifecycleState =
  | 'stopped'
  | 'starting'
  | 'ready'
  | 'processing'
  | 'waitingPermission'
  | 'waitingUserInput'
  | 'error'

export type AcpAttentionKind =
  | 'question'
  | 'plan'
  | 'memory'
  | 'permission'
  | 'command'
  | 'file'
  | 'error'
  | 'generic'

export interface CppMessage {
  role: 'user' | 'assistant' | 'system'
  content: string
  providerId?: string
  thoughts?: string
  planSummary?: string
  planEntries?: AcpPlanEntry[]
  toolCalls?: AcpToolCall[]
  blocks?: MessageBlock[]
  markdownStoreFiles?: string[]
  attachments?: Attachment[]
  processingTimeMs?: number
  createdAt: string
}

export interface ChatAttachmentInput {
  id: string
  name: string
  kind: 'file' | 'image' | 'directory'
  mimeType?: string
  size?: number
  path?: string
  dataBase64?: string
}

export interface CppChat {
  id: string
  title: string
  folderId: string
  pinned?: boolean
  providerId: string
  parentChatId?: string
  branchRootChatId?: string
  branchFromMessageIndex?: number
  branchMessageEdited?: boolean
  modelId?: string
  reasoningEffort?: string
  serviceTier?: string
  approvalMode?: string
  autoApproveCommands?: boolean
  commandSafetyTier?: 'off' | 'acceptEdits' | 'low' | 'medium' | 'high' | 'yolo'
  memoryEnabled?: boolean
  memoryLevel?: MemoryLevel
  memoryLastProcessedMessageCount?: number
  memoryLastProcessedAt?: string
  workspaceDirectory?: string
  workspaceIsolationKind?: string
  workspaceSourceDirectory?: string
  workspaceBaseRef?: string
  workspaceBranchName?: string
  workspaceWorktreeDirectory?: string
  createdAt: string
  updatedAt: string
  lastOpenedAt?: string
  messageCount?: number
  messagesDigest?: string
  messages?: CppMessage[]
  cliTerminal?: {
    terminalId?: string
    frontendChatId?: string
    sourceChatId?: string
    running: boolean
    lifecycleState?: CliLifecycleState | string
    turnState?: 'idle' | 'busy' | string
    processing?: boolean
    readySinceLastSelect?: boolean
    active?: boolean
    pendingSteer?: boolean
    lastError: string
  }
  acpSession?: CppAcpSession
  activeGoalId?: string | null
  goals?: CppGoal[]
  goalMode?: boolean
}

export interface CppGoal {
  id: string
  objective: string
  status: GoalStatus
  tokenBudget?: number
  tokensUsed?: number
  blockedTurnCount?: number
  lastBlocker?: string
  lastDiagnostic?: string
  completedItems?: string[]
  remainingItems?: string[]
  currentStep?: string
  lastVerification?: string
  lastNextPrompt?: string
  sameNextPromptCount?: number
  loopCount?: number
  createdAt: string
  updatedAt: string
	executionOwner?: 'uam' | 'provider'
	providerCommand?: string
}

export interface GitWorktreeStatus {
  isGitRepository: boolean
  isSvnWorkspace: boolean
	managedRepository: boolean
  isolated: boolean
  sourceDirty: boolean
  worktreeDirty: boolean
  worktreeMissing: boolean
  sourceDirectory: string
  worktreeDirectory: string
  branchName: string
  baseRef: string
  warning: string
  error: string
}

export interface GitWorktreeResult {
  ok: boolean
  status?: GitWorktreeStatus
  message: string
  patchPath: string
}

export interface AcpToolCall {
  id: string
  title: string
  kind: string
  status: string
  content: string
  contentDeferred?: boolean
  isSubAgent?: boolean
  subAgentId?: string
  subAgentTitle?: string
}

export interface AcpPlanEntry {
  content: string
  priority: string
  status: string
}

export interface AcpMode {
  id: string
  name: string
  description: string
}

export interface AcpModel {
  id: string
  name: string
  description: string
  defaultReasoningEffort?: string
  supportedReasoningEfforts?: string[]
  additionalSpeedTiers?: string[]
}

export type AcpTurnEvent =
  | { type: 'assistant_text'; text: string; toolCallId?: string; requestId?: string }
  | { type: 'thought'; text: string; toolCallId?: string; requestId?: string }
  | { type: 'plan'; text?: string; toolCallId?: string; requestId?: string }
  | { type: 'tool_call'; toolCallId: string; text?: string; requestId?: string }
  | { type: 'permission_request'; requestId: string; toolCallId?: string; text?: string }
  | { type: 'user_input_request'; requestId: string; toolCallId?: string; text?: string }

export interface AcpPermissionOption {
  id: string
  name: string
  kind: string
}

export interface AcpPendingPermission {
  requestId: string
  toolCallId: string
  title: string
  kind: string
  status: string
  content: string
  safetyRisk?: 'allowed' | 'warn' | 'warn_high'
  safetyTier?: 'low' | 'medium' | 'high'
  safetyRequiresApproval?: boolean
  options: AcpPermissionOption[]
}

export interface AcpUserInputOption {
  label: string
  description: string
}

export interface AcpUserInputQuestion {
  id: string
  header: string
  question: string
  isOther: boolean
  isSecret: boolean
  options: AcpUserInputOption[]
}

export interface AcpPendingUserInput {
  requestId: string
  itemId: string
  status: string
  attentionKind?: AcpAttentionKind
  questions: AcpUserInputQuestion[]
}

export type AcpUserInputAnswers = Record<string, string[]>

export interface AcpAgentInfo {
  name: string
  title: string
  version: string
}

export interface AcpQueuedPrompt {
  text: string
  markdownStoreFiles: string[]
  attachments: Attachment[]
  goalMode: boolean
  goalId: string
  prioritySteer?: boolean
}

export interface AcpDiagnosticEntry {
  time: string
  event: string
  reason: string
  method: string
  requestId: string
  code: number | null
  message: string
  detail: string
  lifecycleState: string
}

export interface CppAcpSession {
  sessionId?: string
  providerId?: string
  protocolKind?: string
  threadId?: string
  running?: boolean
  processing?: boolean
  readySinceLastSelect?: boolean
  attentionKind?: AcpAttentionKind | null
  lifecycleState?: AcpLifecycleState | string
  lastError?: string
  recentStderr?: string
  lastExitCode?: number | null
  diagnostics?: AcpDiagnosticEntry[]
  agentInfo?: Partial<AcpAgentInfo>
  toolCalls?: AcpToolCall[]
  planSummary?: string
  planEntries?: AcpPlanEntry[]
  availableModes?: AcpMode[]
  currentModeId?: string
  availableModels?: AcpModel[]
  modelsLoading?: boolean
  modelRefreshError?: string
  currentModelId?: string
  turnEvents?: AcpTurnEvent[]
  turnUserMessageIndex?: number
  turnAssistantMessageIndex?: number
  turnSerial?: number
  queuedPrompts?: AcpQueuedPrompt[]
  waitIsStale?: boolean
  waitStaleReason?: string
  waitSeconds?: number
  pendingPermission?: AcpPendingPermission | null
  pendingUserInput?: AcpPendingUserInput | null
}

export interface CppCliDebugTerminal {
  terminalId: string
  frontendChatId: string
  sourceChatId: string
  attachedSessionId: string
  providerId: string
  nativeSessionId: string
  processId: string
  running: boolean
  uiAttached: boolean
  lifecycleState?: CliLifecycleState | string
  turnState: 'idle' | 'busy' | string
  inputReady: boolean
  generationInProgress: boolean
  lastUserInputAt: number
  lastAiOutputAt: number
  lastPolledAt: number
  lastError: string
}

export interface CppCliDebugState {
  selectedChatId: string | null
  terminalCount: number
  runningTerminalCount: number
  busyTerminalCount: number
  terminals: CppCliDebugTerminal[]
}

export interface CppFolder {
  id: string
  title: string
  directory: string
  collapsed: boolean
  missing?: boolean
}

export interface CppProvider {
  id: string
  name: string
  shortName: string
  outputMode?: 'structured' | 'cli' | string
  supportsCli?: boolean
  supportsStructured?: boolean
  structuredProtocol?: string
  npmPackageName?: string
	nativeGoalCommand?: string
}

export interface MemoryWorkerBinding {
  workerProviderId: string
  workerModelId: string
}

export interface ProviderChatDefaults {
  modelId: string
  approvalMode: string
  autoApproveCommands: boolean
  memoryEnabled: boolean
  memoryLevel?: MemoryLevel
  reasoningEffort: string
  serviceTier: string
}

export interface MemoryActivity {
  entryCount: number
  lastCreatedAt: string
  lastCreatedCount: number
  runningCount: number
  lastStatus: string
  lastWorkerChatId?: string
  lastWorkerProviderId?: string
  lastWorkerUpdatedAt?: string
  lastWorkerStatus?: string
  lastWorkerOutput?: string
  lastWorkerError?: string
  lastWorkerTimedOut?: boolean
  lastWorkerCanceled?: boolean
  lastWorkerHasExitCode?: boolean
  lastWorkerExitCode?: number
}

export interface CliVersionOption {
  version: string
  preferred: boolean
}

export interface CliVersionProviderState {
  providerId: string
  installedVersion: string
  selectedVersion: string
  availableVersions: CliVersionOption[]
  preferredVersion: string
  status: 'unknown' | 'checking' | 'installing' | 'supported' | 'unsupported'
  message: string
  running: boolean
  lastCommand: string
  lastOutput: string
}

export interface CliVersionManager {
  providers: CliVersionProviderState[]
}

export interface ShellAction {
  id: string
  label: string
  skillPath: string
  providerId: string
  modelId: string
  acceptsFiles: boolean
  acceptsFolders: boolean
  enabled: boolean
  openWorkspace: boolean
}

export interface CppSettings {
  activeProviderId: string
  theme: StoredTheme
  showProviderIconsInSidebar?: boolean
  showWorktreePathInSidebar?: boolean
  memoryEnabledDefault: boolean
  memoryLevelDefault?: MemoryLevel
  memoryIdleDelaySeconds: number
  memoryRecallBudgetBytes: number
  goalMaxLoopIterations: number
  updateChecksEnabled: boolean
  updateLastCheckedAt: string
  dismissedUpdateVersions: Record<string, string>
  memoryLastStatus: string
  memoryWorkerBindings: Record<string, MemoryWorkerBinding>
  defaultNewChatProviderId?: string
  providerChatDefaults?: Record<string, ProviderChatDefaults>
  markdownStoreDirectory?: string
  voiceInputMode?: VoiceInputMode
  voiceInputServerBaseUrl?: string
  voiceInputServerEndpoint?: string
  voiceInputServerModel?: string
  voiceInputApiKeyEnv?: string
  voiceInputCapabilities?: VoiceInputCapabilities
  defaultEditorPresetId?: string
  editorFileAssociations?: EditorFileAssociation[]
}

export type VoiceInputMode = 'system' | 'local' | 'server'
export interface VoiceInputCapability { supported: boolean; reason: string }
export interface VoiceInputCapabilities {
  system: VoiceInputCapability
  local: VoiceInputCapability
  server: VoiceInputCapability
}

export interface EditorFileAssociation {
  id: string
  name: string
  extensions: string[]
  editorPresetId: string
}

export type VcsType = 'git' | 'svn'

export interface VcsChangedFile {
  path: string
  status: string
  staged: boolean
  additions: number
  deletions: number
  binary: boolean
}

export interface VcsCommitStatus {
  available: boolean
  vcsTypes: VcsType[]
  activeVcsType: VcsType
  workspaceDirectory: string
  branchOrRevision: string
  changedFiles: VcsChangedFile[]
  lineStatsReady: boolean
  warning: string
  error: string
}

export interface VcsCommitResult {
  ok: boolean
  status?: VcsCommitStatus
  message: string
  error: string
}

export interface VcsFileDiffResponse {
  diff?: string
}

export interface OpenWorkspaceEditorResponse {
  editorPresetId?: string
}

export interface ChatMessagesResponse {
  chatId?: string
  messagesDigest?: string
  unchanged?: boolean
  messages?: CppMessage[]
}

export interface OpenNativeSessionChatResponse {
  chatId?: string
}

export interface VcsCommitMessageSuggestion {
  title: string
  description: string
}

export interface CppAppState {
  stateRevision?: number
  appVersion?: string
  folders: CppFolder[]
  resourceCollections?: ResourceCollection[]
  chats: CppChat[]
  cliDebug?: CppCliDebugState
  selectedChatId: string | null
  selectedChatIndex?: number
  providers: CppProvider[]
  settings: CppSettings
  memoryActivity?: MemoryActivity
  cliVersionManager?: CliVersionManager
  shellActions?: ShellAction[]
  shellActionNotification?: string
}

export interface CppStatePatch {
  stateRevision?: number
  folders?: CppFolder[]
  resourceCollections?: ResourceCollection[]
  chats?: CppChat[]
  chatOrder?: string[]
  removedChatIds?: string[]
  messagesByChatId?: Record<string, CppMessage[]>
  selectedChatId?: string | null
  providers?: CppProvider[]
  settings?: CppSettings
  memoryActivity?: MemoryActivity
  cliVersionManager?: CliVersionManager
  shellActions?: ShellAction[]
  shellActionNotification?: string
}

export interface CliBinding {
  terminalId: string
  boundChatId: string
  running: boolean
  lifecycleState: CliLifecycleState
  turnState: 'idle' | 'busy'
  processing: boolean
  readySinceLastSelect: boolean
  active: boolean
  pendingSteer?: boolean
  lastError: string
}

export interface AcpBinding {
  sessionId: string
  providerId: string
  protocolKind: string
  threadId: string
  running: boolean
  lifecycleState: AcpLifecycleState
  processing: boolean
  readySinceLastSelect: boolean
  attentionKind?: AcpAttentionKind | null
  processingStartedAtMs: number | null
  lastError: string
  recentStderr: string
  lastExitCode: number | null
  diagnostics: AcpDiagnosticEntry[]
  toolCalls: AcpToolCall[]
  planSummary?: string
  planEntries: AcpPlanEntry[]
  availableModes: AcpMode[]
  currentModeId: string
  availableModels: AcpModel[]
  modelsLoading?: boolean
  modelRefreshError?: string
  currentModelId: string
  turnEvents: AcpTurnEvent[]
  turnUserMessageIndex: number
  turnAssistantMessageIndex: number
  turnSerial: number
  queuedPrompts?: AcpQueuedPrompt[]
  waitIsStale?: boolean
  waitStaleReason?: string
  waitSeconds?: number
  pendingPermission: AcpPendingPermission | null
  pendingUserInput: AcpPendingUserInput | null
  agentInfo: AcpAgentInfo | null
}

export interface CliTranscript {
  terminalId: string
  content: string
}

export type PushChannelStatus = 'no-push-yet' | 'connected' | 'parse-error' | 'invalid-message'

export type DictationPushMessage =
  | { type: 'dictation'; event: 'interim' | 'final'; text: string }
  | { type: 'dictation'; event: 'error'; message: string }
  | { type: 'dictation'; event: 'end' }

export type ParsedPushMessage =
  | { type: 'stateUpdate'; data: CppAppState }
  | { type: 'statePatch'; data: CppStatePatch }
  | {
      type: 'cliOutput'
      data: string
      sessionId?: string
      sourceChatId?: string
      terminalId?: string
    }
  | { type: 'streamToken'; chatId: string; token: string }
  | { type: 'streamDone'; chatId: string }
  | DictationPushMessage

export type ParsedPushResult =
  | { ok: true; message: ParsedPushMessage }
  | { ok: false; status: Exclude<PushChannelStatus, 'connected' | 'no-push-yet'>; error: string }
