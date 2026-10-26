import type { StartupNativeImportStatus } from '../../types/startupNativeImports'
import type { ExecutionRoutingSettings } from '../../types/executionRouting'
import type { CustomIcon } from '../../types/customIcon'
// C++ state serialisation types (mirrors state_serializer.cpp output) plus the
// frontend-facing binding/push types. Extracted from useAppStore.ts (MO-1); the
// store re-exports everything here so existing imports keep working.

import type { Attachment, MessageBlock, MessageToolCall } from '../../types/message'
import type { GoalStatus } from '../../types/goal'
import type { StoredTheme } from '../../utils/themeStorage'
import type { ResourceCollection } from '../../types/resourceCollection'
import type { MemoryLevel } from '../../types/memory'
import type { ComputerUseBackend, ComputerUseEffectiveBackend, ComputerUseState, ExecutionHost } from '../../types/session'

export type CliLifecycleState = 'disabled' | 'starting' | 'stopped' | 'idle' | 'busy' | 'shuttingDown' | 'unknown'
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
  /** Positive historical local OS-sink write fact only; absent/false is unknown, never retry authority. */
  inputWriteCompleted?: boolean
  /** Inert per-input provenance; never admission/delivery/control proof. */
  nativeInputId?: string
  originRequestId?: string
  /** Output-only correlation; scoped native list alone supplies child authority. */
  managedOriginRunId?: string
  role: 'user' | 'assistant' | 'system'
  content: string
  providerId?: string
  modelId?: string
  thoughts?: string
  planSummary?: string
  planEntries?: AcpPlanEntry[]
  toolCalls?: AcpToolCall[]
  blocks?: MessageBlock[]
  markdownStoreFiles?: string[]
  attachments?: Attachment[]
  processingTimeMs?: number
	interrupted?: boolean
  stopReason?: string
	acpPromptNotSent?: boolean
	acpPromptDiscarded?: boolean
	prioritySteer?: boolean
	continuesTurn?: boolean
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

export interface NewChatWorktreeContext {
  sourceChatId: string
  expectedWorkspaceDirectory: string
  expectedExecutionHostId: string
}

export interface CreateSessionOptions extends Partial<NewChatWorktreeContext> {
  gitBaseBranch?: string
}

export interface CppChat {
  gitBaseBranch?: string | null
  routingMode?: 'explicit' | 'automatic'
  routingSourceFolderId?: string
  routingSourceDirectory?: string
  routingReservationPending?: boolean
  routingAssignedConnectionIdentity?: string
  routingAssignedProviderConfigurationIdentity?: string
  routingWaitReason?: string
  routingUnresolvedReservation?: boolean

  id: string
  executionHostId?: string
  remoteRecoveryEnabled?: boolean
  remoteRecoveryState?: string
  customIcon?: CustomIcon
  title: string
  folderId: string
  pinned?: boolean
  providerId: string
  temporaryParentChatId?: string
  sideCleanupRequested?: boolean
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
  interactionAt?: string
  lastOpenedAt?: string
  attentionRevision?: string
  settledAt?: string
  snoozedUntilMs?: number
  messageCount?: number
  /** Display-only native lineage; never provider resume or control authority. */
  subagentParentChatId?: string
  subagentParentCreatedAt?: string
  managedSubagentCount?: number
  managedSubagentRevision?: number
  messagesDigest?: string
  messages?: CppMessage[]
  cliTerminal?: {
    terminalId?: string
    startupTime?: number
    frontendChatId?: string
    sourceChatId?: string
    idleCountdownStartsAtMs?: number
    idleShutdownAtMs?: number
    idleShutdownTimeoutSeconds?: number
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
	lastBlockerKind?: string
  lastDiagnostic?: string
  pendingContinuation?: boolean
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

export type AcpToolCall = MessageToolCall

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
  contextWindowTokens?: number | null
  maxOutputTokens?: number | null
}

/** Native observations preserve omitted versus explicitly nullable staged values. */
export type NativeOptionPresence<T> = { present: false } | { present: true; value: T }
export type NativeOptionStatus = 'unknown' | 'ready' | 'invalid' | 'unsupported'

export interface NativeModelOptionOwnership {
  chatId: string
  nativeSessionId: string
  executionHostId: string
  /** Public process token; never the adapter's private lifetime nonce. */
  lifetime: string
}

export interface CopilotAutoTierDescriptor {
  id: string
  displayName: string
  description: string
  type: string
  status: { enabled: boolean; reason?: string; message?: string }
}

export interface CopilotAutoRoutingProjection {
  ownership?: NativeModelOptionOwnership
  catalogStatus: NativeOptionStatus
  stateStatus: NativeOptionStatus
  eligibleTiers: NativeOptionPresence<CopilotAutoTierDescriptor[]>
  providerDefault: NativeOptionPresence<string>
  currentModel: NativeOptionPresence<string>
  effective: NativeOptionPresence<string>
  pending: NativeOptionPresence<string | null>
  activating: NativeOptionPresence<string | null>
  requestPending: boolean
  available: boolean
  disabledCode: string
  disabledReason: string
  lastAckStatus: '' | 'pending' | 'unchanged'
  lastError: string
}

export interface NativeModelOptions {
  autoRouting?: CopilotAutoRoutingProjection
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
  | { type: 'context_compaction_summary'; text: string; toolCallId?: string; requestId?: string }
  | { type: 'thought'; text: string; toolCallId?: string; requestId?: string }
  | { type: 'context_compaction' | 'context_compaction_interrupted'; text: string; toolCallId?: string; requestId?: string; tokensBefore?: number; tokensAfter?: number }
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
  isMultiple?: boolean
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
  /** Inert per-input provenance; never admission/delivery/control proof. */
  nativeInputId?: string
  originRequestId?: string
  id?: string
  revision?: number
  prepared?: boolean
  existingUserMessageIndex?: number
  /** Native-validated current raw-row reference; not durable identity or delivery authority. */
  referencedMessageCreatedAt?: string
  retryAllowed?: boolean
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
  last: AcpTokenUsageBreakdown | null
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

export interface AcpContextUsage {
  updatedAt: number
  usedTokens: number
  windowTokens: number
}

export interface AcpProviderUsage {
  contextUsage?: AcpContextUsage | null
  tokenUsage: AcpTokenUsage | null
  rateLimits: AcpRateLimits | null
}

/** Advisory native projection; missing or malformed support is unavailable. */
export interface ManualCompactionAvailability {
  available: boolean
  providerId: string
  modelId: string
  nativeSessionId: string
}

export interface CppAcpSession {
  lastStopReason?: string
  idleCountdownStartsAtMs?: number
  idleShutdownAtMs?: number
  idleShutdownTimeoutSeconds?: number

  sessionId?: string
  providerId?: string
	/** Exact UAM execution path: provider-native config/plugin or UAM prompt injection. */
	uamAgentExecutionCapability?: string
  protocolKind?: string
  threadId?: string
  running?: boolean
  processing?: boolean
  compacting?: boolean
  manualCompaction?: ManualCompactionAvailability
  backgroundTaskCount?: number
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
  nativeModelOptions?: NativeModelOptions
  modelsLoading?: boolean
  modelRefreshError?: string
  currentModelId?: string
  turnEvents?: AcpTurnEvent[]
  turnUserMessageIndex?: number
  turnAssistantMessageIndex?: number
  turnSerial?: number
  queueHeld?: boolean
  unsentMessageIndices?: number[]
  recoverableUnsentMessageIndices?: number[]
  queuedPrompts?: AcpQueuedPrompt[]
  pendingDirectPrompts?: AcpQueuedPrompt[]
  unsentDirectPrompts?: AcpQueuedPrompt[]
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
  customIcon?: CustomIcon
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

export interface HostScopedSettingsValues {
  defaultNewChatProviderId: string
  providerChatDefaults: Record<string, ProviderChatDefaults>
  mcpServers: McpServerConfiguration[]
}

export interface ProviderChatDefaults {
  hiddenModelIds?: string[]
  hiddenProviderIds?: string[]
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
  canInstall?: boolean
  routingConnectionIdentity?: string
  routingProviderConfigurationIdentity?: string
  providerId: string
  blockingChatIds?: string[]
  executionHostId?: string
  executionHostName?: string
  installedVersion: string
  selectedVersion: string
  availableVersions: CliVersionOption[]
  preferredVersion: string
  verifiedVersion?: string
  verifiedAt?: string
  status: 'unknown' | 'checking' | 'installing' | 'verified' | 'untested' | 'untested-newer' | 'known-incompatible' | 'unavailable' | 'provider-managed'
  message: string
  checkError?: string
  running: boolean
  installMethod?: 'npm' | 'homebrew-formula' | 'homebrew-cask' | 'winget' | 'unknown'
  lastInstallStatus?: 'none' | 'running' | 'awaiting-verification' | 'succeeded' | 'failed'
  lastCommand: string
  lastOutput: string
}

export interface CliVersionManager {
  providers: CliVersionProviderState[]
  remoteProviders?: CliVersionProviderState[]
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

export interface ProviderAvailabilityOverride {
  providerId: string
  executionHostId: string
  enabled: boolean
}

export type ProviderAvailabilityRequest = ProviderAvailabilityOverride
export interface VoiceInputSettingsRequest {
  mode: VoiceInputMode
  serverBaseUrl?: string
  serverEndpoint?: string
  serverModel?: string
  apiKeyEnv?: string
}

export type VoiceInputMode = 'system' | 'local' | 'server'
export type VoiceInputCapabilities = Partial<Record<VoiceInputMode, { supported: boolean; reason: string }>>

export interface VoiceInputSettings {
  voiceInputMode?: VoiceInputMode
  voiceInputServerBaseUrl?: string
  voiceInputServerEndpoint?: string
  voiceInputServerModel?: string
  voiceInputApiKeyEnv?: string
  voiceInputCapabilities?: VoiceInputCapabilities
}

export interface VcsStorageSettings {
  gitAutoWorktree: boolean
  gitSnapshotSvn: boolean
  gitSnapshotPlainFolders: boolean
  gitDefaultBaseBranch: string
  gitPullBeforeChat: boolean
  svnUpdateBeforeChat: boolean
  gitPortMethod: 'patch' | 'merge' | 'squash'
  worktreeCleanupEnabled: boolean
  worktreeRetentionDays: number
  backupLogRetentionDays: number
  httpCacheClearIntervalDays: number
}
export type VcsSettingsRequest = { gitEnabled: boolean; svnEnabled: boolean } & Partial<Omit<VcsStorageSettings, 'backupLogRetentionDays' | 'httpCacheClearIntervalDays'>>
export type StorageSettingsRequest = Pick<VcsStorageSettings, 'backupLogRetentionDays' | 'httpCacheClearIntervalDays'>

export interface CppSettings extends VoiceInputSettings, Partial<VcsStorageSettings> {
  providerAvailability?: ProviderAvailabilityOverride[]
  activeProviderId: string
  theme: StoredTheme
  showProviderIconsInSidebar?: boolean
  showWorktreePathInSidebar?: boolean
  gitEnabled?: boolean
  svnEnabled?: boolean
  memoryEnabledDefault: boolean
  memoryLevelDefault?: MemoryLevel
  memoryIdleDelaySeconds: number
  memoryRecallBudgetBytes: number
  goalMaxLoopIterations: number
  acpSetupInactivityTimeoutSeconds?: number
  acpTurnOutputLimitMiB?: number
  updateChecksEnabled: boolean
  automaticProviderUpdates: boolean
  updateLastCheckedAt: string
  dismissedUpdateVersions: Record<string, string>
  memoryLastStatus: string
  memoryWorkerBindings: Record<string, MemoryWorkerBinding>
  permissionReviewerProviderId?: string
  permissionReviewerModelId?: string
  defaultNewChatProviderId?: string
  providerChatDefaults?: Record<string, ProviderChatDefaults>
  markdownStoreDirectory?: string
  fileExplorerApplication?: string
  defaultEditorPresetId?: string
  editorFileAssociations?: EditorFileAssociation[]
  centralProviderConfiguration?: CentralProviderConfiguration
  mcpServers?: McpServerConfiguration[]
  executionRouting?: ExecutionRoutingSettings
  executionHosts?: ExecutionHost[]
  favoriteUamAgentIds?: string[]
  uamAgentCycleShortcut?: UamAgentCycleShortcut
  /** Saved global Computer Use consent. Unknown when absent; does not grant a target. */
  computerUseGloballyEnabled?: boolean
  /** Native owned-resource cleanup remains incomplete. Unknown when absent. */
  computerUseRevocationPending?: boolean
  computerUseAllowlistEnabled?: boolean
  computerUseAllowedApplications?: ComputerUseAllowedApplication[]

  /** Backend supports listing and switching a chat workspace's git branch. Tab stays display-only when absent or false. */
  branchSwitching?: boolean
  /** Backend accepts scopeExecutionHostId on settings saves. Hidden/disabled when absent or false. */
  hostScopedSettings?: boolean
  hostScopedSettingsValues?: Record<string, HostScopedSettingsValues>
}

export interface ComputerUseAllowedApplication {
  identityKind: 'bundleId' | 'executablePath'
  identity: string
}

export type UamAgentCycleShortcut = 'shift+tab' | 'control+shift+tab' | 'alt+shift+tab' | 'meta+shift+tab' | 'disabled'

export interface UamAgentSummary {
  id: string
  description: string
  builtIn: boolean
}

export interface ProviderAgentCandidate {
  id: string
  name: string
  description?: string
  sourcePath: string
  alreadyImported?: boolean
}

export interface ProviderAgentImportResult {
  imported: number
  failed?: Array<{ id: string; error: string }>
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
  /** Literal value stored in settings; present means "use this value" instead of a host variable. */
  value?: string
}

export interface McpServerConfiguration {
  executionHostId?: string
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
  unsentMessageIndices?: number[]
  recoverableUnsentMessageIndices?: number[]
  userMessageIndices?: number[] | null
  chatId?: string
  messagesDigest?: string
  unchanged?: boolean
  messages?: CppMessage[]
  startIndex?: number
  totalCount?: number
}

export interface OpenNativeSessionChatResponse {
  chatId?: string
}

export interface VcsCommitMessageSuggestion {
  title: string
  description: string
}

export interface CppAppState {
  startupNativeImports?: StartupNativeImportStatus
  unchanged?: false
  stateRevision?: number
  bootId?: string
  appVersion?: string
  runnerProtocolVersion?: number
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
  deletionProgress?: CppDeletionProgress
}

export interface CppDeletionProgress {
  requestId?: string
  completed: number
  total: number
  currentLabel?: string
}

export interface CppCompanionUnchangedState {
  unchanged: true
  bootId: string
  stateRevision: number
}

export interface CppStatePatch {
  startupNativeImports?: StartupNativeImportStatus
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
  settings?: Partial<CppSettings>
  memoryActivity?: MemoryActivity
  cliVersionManager?: CliVersionManager
  shellActions?: ShellAction[]
  shellActionNotification?: string
  statusLine?: string
}

export interface CliBinding {
  startupTime?: number
  idleCountdownStartsAtMs?: number
  idleShutdownAtMs?: number
  idleShutdownTimeoutSeconds?: number

  terminalId: string
  boundChatId: string
  running: boolean
  lifecycleState: CliLifecycleState
  turnState: 'idle' | 'busy' | 'unknown'
  processing: boolean
  readySinceLastSelect: boolean
  active: boolean
  pendingSteer?: boolean
  lastError: string
}

export interface AcpBinding {
  lastStopReason?: string
  idleCountdownStartsAtMs?: number
  idleShutdownAtMs?: number
  idleShutdownTimeoutSeconds?: number

  sessionId: string
  providerId: string
  protocolKind: string
  threadId: string
  running: boolean
  lifecycleState: AcpLifecycleState
  processing: boolean
  compacting?: boolean
  manualCompaction?: ManualCompactionAvailability
  backgroundTaskCount?: number
  readySinceLastSelect: boolean
  attentionKind?: AcpAttentionKind | null
  processingStartedAtMs: number | null
  /** Local explicit-action failure; native snapshots do not own this value. */
  promptActionError?: { id: string; message: string }
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
  nativeModelOptions?: NativeModelOptions
  modelsLoading?: boolean
  modelRefreshError?: string
  currentModelId: string
  turnEvents: AcpTurnEvent[]
  turnUserMessageIndex: number
  turnAssistantMessageIndex: number
  turnSerial: number
  queueHeld?: boolean
  unsentMessageIndices?: number[]
  recoverableUnsentMessageIndices?: number[]
  queuedPrompts?: AcpQueuedPrompt[]
  pendingDirectPrompts?: AcpQueuedPrompt[]
  unsentDirectPrompts?: AcpQueuedPrompt[]
  waitIsStale?: boolean
  waitStaleReason?: string
  waitSeconds?: number
  pendingPermission: AcpPendingPermission | null
  pendingUserInput: AcpPendingUserInput | null
  agentInfo: AcpAgentInfo | null
  providerUsage?: AcpProviderUsage | null
}

export interface CliTranscript {
  startupTime?: number
  terminalId: string
  content: string
}

export type PushChannelStatus = 'no-push-yet' | 'connected' | 'parse-error' | 'invalid-message'

export type DictationPushMessage =
  | { type: 'dictation'; dictationId: string; event: 'interim' | 'final'; text: string }
  | { type: 'dictation'; dictationId: string; event: 'error'; message: string }
  | { type: 'dictation'; dictationId: string; event: 'end' }

export type ParsedPushMessage =
  | { type: 'stateUpdate'; data: CppAppState }
  | { type: 'statePatch'; data: CppStatePatch }
  | {
      type: 'cliOutput'
      data: string
      sessionId?: string
      sourceChatId?: string
      terminalId?: string
      startupTime?: number
    }
  | { type: 'streamToken'; chatId: string; token: string; messageIndex?: number }
  | { type: 'streamDone'; chatId: string }
  | DictationPushMessage

export type ParsedPushResult =
  | { ok: true; message: ParsedPushMessage }
  | { ok: false; status: Exclude<PushChannelStatus, 'connected' | 'no-push-yet'>; error: string }

export interface CentralProviderConfiguration {
  mcpOverrides?: Array<{ providerId: string; executionHostId: string; serverId: string; enabled: boolean; kind?: 'preference' }>
  enabled: boolean
  instructions: string
  instructionFiles: string[]
  skillDirectories: string[]
  defaultSkills?: string[]
  defaultAgents?: string[]
  defaultAgentId: string
  uamControlEnabled: boolean
}

export const DEFAULT_CENTRAL_PROVIDER_CONFIGURATION: CentralProviderConfiguration = {
  enabled: false, instructions: '', instructionFiles: [], skillDirectories: [], defaultSkills: [], defaultAgentId: 'build', uamControlEnabled: true,
}

/** Immutable provider edit evidence, separate from current workspace changes. */
export interface ChatFileChangeReceipt {
  chatId: string
  messageIndex: number
  messageCreatedAt: string
  providerId: string
  toolCallId: string
  reportedPath: string
  path: string
  destinationPath: string
  kind: string | Record<string, unknown>
  patch: string
}
export interface ChatFileChangeReceiptsResponse {
  chatId: string
  scope: 'recorded-provider-edits'
  receipts: ChatFileChangeReceipt[]
  omittedCount: number
}
