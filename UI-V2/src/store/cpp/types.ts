// C++ state serialisation types (mirrors state_serializer.cpp output) plus the
// frontend-facing binding/push types. Extracted from useAppStore.ts (MO-1); the
// store re-exports everything here so existing imports keep working.

import type { Attachment, MessageBlock } from '../../types/message'
import type { GoalStatus } from '../../types/goal'
import type { StoredTheme } from '../../utils/themeStorage'
import type { ResourceCollection } from '../../types/resourceCollection'
import type { MemoryLevel } from '../../types/memory'
import type { ComputerUseBackend, ComputerUseEffectiveBackend, ComputerUseState, ExecutionHost } from '../../types/session'

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
  checkpointSha?: string
  checkpointParentSha?: string
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
  executionHostId?: string
  title: string
  folderId: string
  pinned?: boolean
  providerId: string
  parentChatId?: string
  branchRootChatId?: string
  branchFromMessageIndex?: number
  branchMessageEdited?: boolean
  modelId?: string
  reviewerModelId?: string
  reasoningEffort?: string
  serviceTier?: string
  serviceTierExplicit?: boolean
  approvalMode?: string
  uamAgentId?: string
  uamControlEnabled?: boolean
  commandSafetyTier?: 'off' | 'acceptEdits' | 'aiReview' | 'yolo'
  computerUseEnabled?: boolean
  computerUseBackend?: ComputerUseBackend
  computerUseEffectiveBackend?: ComputerUseEffectiveBackend
  computerUseProviderAvailable?: boolean
  computerUseTargetKind?: 'window' | 'screen'
  computerUseTargetId?: string
  computerUseTargetTitle?: string
  computerUseTargetInputMode?: 'background' | 'foreground'
  computerUse?: ComputerUseState
  memoryEnabled?: boolean
  memoryLevel?: MemoryLevel
  smallModelMode?: boolean
  memoryLastProcessedMessageCount?: number
  memoryLastProcessedAt?: string
  workspaceDirectory?: string
  workspaceIsolationKind?: string
  workspaceSourceDirectory?: string
  workspaceBaseRef?: string
  workspaceBranchName?: string
  workspaceWorktreeDirectory?: string
  importedReadOnly?: boolean
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
	workerModelId?: string
	reviewerModelId?: string
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

export interface GitTurnCheckpointResult {
  message: string
  diff: string
  checkpointSha: string
  parentSha: string
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

export interface AcpCommand {
  name: string
  description: string
  inputHint: string
}

export interface AcpModel {
  id: string
  name: string
  description: string
  defaultReasoningEffort?: string
  supportedReasoningEfforts?: string[]
  additionalSpeedTiers?: string[]
}

export interface AcpConfigOptionChoice {
  value: string
  name: string
  description: string
}

export interface AcpConfigOption {
  id: string
  name: string
  description: string
  category: string
  currentValue: string
  options: AcpConfigOptionChoice[]
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
  uamAgentId: string
  markdownStoreFiles: string[]
  attachments: Attachment[]
  goalMode: boolean
  goalId: string
  computerUseMode?: boolean
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

export interface AcpTokenUsageBreakdown {
  inputTokens: number
  cachedInputTokens: number
  cacheWriteInputTokens: number
  outputTokens: number
  reasoningOutputTokens: number
  totalTokens: number
}

export interface AcpTokenUsage {
  updatedAt: number
  total: AcpTokenUsageBreakdown
  last: AcpTokenUsageBreakdown
  modelContextWindow: number | null
}

export interface AcpRateLimitWindow {
  usedPercent: number
  resetsAt: number | null
  windowDurationMinutes: number | null
}

export interface AcpCredits {
  hasCredits: boolean
  unlimited: boolean
  balance: string | null
}

export interface AcpSpendControlLimit {
  limit: string
  used: string
  remainingPercent: number
  resetsAt: number
}

export interface AcpRateLimits {
  updatedAt: number
  limitId: string
  limitName: string
  primary: AcpRateLimitWindow | null
  secondary: AcpRateLimitWindow | null
  credits: AcpCredits | null
  individualLimit: AcpSpendControlLimit | null
  spendControlReached: boolean | null
  planType: string
  rateLimitReachedType: string
}

export interface AcpProviderUsage {
  tokenUsage: AcpTokenUsage | null
  rateLimits: AcpRateLimits | null
}

export interface CppAcpSession {
  sessionId?: string
  providerId?: string
	/** Exact UAM execution path: provider-native config/plugin or UAM prompt injection. */
	uamAgentExecutionCapability?: string
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
  availableCommands?: AcpCommand[]
  availableModes?: AcpMode[]
  currentModeId?: string
  availableModels?: AcpModel[]
  configOptions?: AcpConfigOption[]
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
  providerUsage?: AcpProviderUsage | null
}

export interface ProviderModelCatalog {
  providerId: string
  workspaceDirectory: string
	executionHostId: string
  availableModels: AcpModel[]
	configOptions?: AcpConfigOption[]
  currentModelId: string
  modelsLoading: boolean
  modelRefreshError: string
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
  executionHostId: string
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
  structuredPermissionControl?: 'uam' | 'provider'
  terminalPermissionControl?: 'provider'
  npmPackageName?: string
	nativeGoalCommand?: string
}

export interface MemoryWorkerBinding {
  workerProviderId: string
  workerModelId: string
}

export interface ProviderChatDefaults {
  modelId: string
  reviewerModelId?: string
  featurePreference?: 'uam' | 'provider'
  approvalMode: string
  commandSafetyTier: 'off' | 'acceptEdits' | 'aiReview' | 'yolo'
  memoryEnabled: boolean
  memoryLevel?: MemoryLevel
  smallModelMode?: boolean
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
  verifiedVersion?: string
  verifiedAt?: string
  status: 'unknown' | 'checking' | 'installing' | 'verified' | 'untested' | 'untested-newer' | 'known-incompatible' | 'unavailable' | 'provider-managed'
  message: string
  running: boolean
  installMethod?: 'npm' | 'homebrew-formula' | 'homebrew-cask' | 'winget'
  lastInstallStatus?: 'none' | 'running' | 'succeeded' | 'failed'
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
  groupPath: string[]
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
  acpSetupInactivityTimeoutSeconds?: number
  acpTurnOutputLimitMiB?: number
  updateChecksEnabled: boolean
  updateLastCheckedAt: string
  dismissedUpdateVersions: Record<string, string>
  memoryLastStatus: string
  memoryWorkerBindings: Record<string, MemoryWorkerBinding>
  permissionReviewerProviderId?: string
  permissionReviewerModelId?: string
  defaultNewChatProviderId?: string
  providerChatDefaults?: Record<string, ProviderChatDefaults>
  markdownStoreDirectory?: string
  defaultEditorPresetId?: string
  editorFileAssociations?: EditorFileAssociation[]
  mcpServers?: McpServerConfiguration[]
  executionHosts?: ExecutionHost[]
  favoriteUamAgentIds?: string[]
  uamAgentCycleShortcut?: UamAgentCycleShortcut
}

export type UamAgentCycleShortcut = 'shift+tab' | 'control+shift+tab' | 'alt+shift+tab' | 'meta+shift+tab' | 'disabled'

export interface UamAgentSummary {
  id: string
  description: string
  builtIn: boolean
}

export interface ProviderAgentImportPreview {
  providerId: string
  sourcePath: string
  suggestedId: string
  description: string
  mode: string
  securityFields: string[]
  ignoredFields: string[]
  error: string
  supported: boolean
}

export interface McpSecretReference {
  name: string
  environmentVariable: string
}

export interface McpServerConfiguration {
  id: string
  name: string
  workspaceDirectory: string
  transport: 'stdio' | 'http' | 'sse'
  command: string
  args: string[]
  url: string
  environment: McpSecretReference[]
  headers: McpSecretReference[]
  enabled: boolean
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
  contentFingerprint?: string
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
  providerModelCatalogs?: ProviderModelCatalog[]
  settings: CppSettings
  memoryActivity?: MemoryActivity
  cliVersionManager?: CliVersionManager
  shellActions?: ShellAction[]
  shellActionNotification?: string
  statusLine?: string
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
  providerModelCatalogs?: ProviderModelCatalog[]
  settings?: CppSettings
  memoryActivity?: MemoryActivity
  cliVersionManager?: CliVersionManager
  shellActions?: ShellAction[]
  shellActionNotification?: string
  statusLine?: string
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
  availableCommands?: AcpCommand[]
  availableModes: AcpMode[]
  currentModeId: string
  availableModels: AcpModel[]
  configOptions?: AcpConfigOption[]
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
  providerUsage?: AcpProviderUsage | null
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
