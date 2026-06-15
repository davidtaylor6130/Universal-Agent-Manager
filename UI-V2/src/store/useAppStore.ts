import { create } from 'zustand'
import { Session, Folder } from '../types/session'
import { Attachment, Message, MessageBlock } from '../types/message'
import { Provider } from '../types/provider'
import { MemoryEntry, MemoryEntryDraft, MemoryScope, MemoryScanCandidate } from '../types/memory'
import type { MarkdownStoreDraft, MarkdownStoreEntry } from '../types/markdownStore'
import type { Goal, GoalStatus } from '../types/goal'
import { sendToCEF, isCefContext, createRequestId } from '../ipc/cefBridge'
import { applyDocumentTheme, normalizeStoredTheme, readStoredTheme, writeStoredTheme, type StoredTheme } from '../utils/themeStorage'
import {
  CLI_TRANSCRIPT_FLUSH_DELAY_MS,
  STREAM_TOKEN_FLUSH_DELAY_MS,
  pendingCliTranscriptChunksBySessionId,
  pendingCodexOptionsByChatId,
  pendingRequestIdsByKey,
  pendingStreamTokensByChatId,
  pushFlushTimers,
} from './push/pushBuffers'
import {
  CLAUDE_CLI_PROVIDER_ID,
  CODEX_CLI_PROVIDER_ID,
  COPILOT_CLI_PROVIDER_ID,
  DEFAULT_PROVIDER_ID as GEMINI_CLI_PROVIDER_ID,
  OPENCODE_CLI_PROVIDER_ID,
  buildProviderCliInstallCommand,
  fallbackProviderForId,
  normalizeCliProviderIdAlias,
} from '../utils/providerMetadata'

const ACP_APPROVAL_MODE_IDS = ['default', 'acceptEdits', 'plan'] as const
export const DEFAULT_MEMORY_IDLE_DELAY_SECONDS = 60
export const MIN_MEMORY_IDLE_DELAY_SECONDS = 30
export const MAX_MEMORY_IDLE_DELAY_SECONDS = 3600
export const DEFAULT_MEMORY_RECALL_BUDGET_BYTES = 2048
export const MIN_MEMORY_RECALL_BUDGET_BYTES = 512
export const MAX_MEMORY_RECALL_BUDGET_BYTES = 8192

const initialFolders: Folder[] = [
  {
    id: 'default',
    name: 'General',
    parentId: null,
    directory: '',
    isExpanded: true,
    createdAt: new Date(),
  },
]
const initialSessions: Session[] = [
  {
    id: 's1',
    name: 'Gemini CLI',
    viewMode: 'chat',
    folderId: 'default',
    createdAt: new Date(),
    updatedAt: new Date(),
  },
]
function initialProvider(providerId: string, color: string): Provider {
  return {
    ...fallbackProviderForId(providerId),
    color,
  }
}

const initialProviders: Provider[] = [
  initialProvider(GEMINI_CLI_PROVIDER_ID, '#f97316'),
  initialProvider(CODEX_CLI_PROVIDER_ID, '#0ea5e9'),
  initialProvider(CLAUDE_CLI_PROVIDER_ID, '#7c3aed'),
  initialProvider(OPENCODE_CLI_PROVIDER_ID, '#14b8a6'),
  initialProvider(COPILOT_CLI_PROVIDER_ID, '#22c55e'),
]
const UI_RUNTIME_BUILD_MARKER = (() => {
  const env = (import.meta as unknown as { env?: Record<string, string | undefined> }).env
  const configured = env?.VITE_UAM_UI_BUILD_ID?.trim()
  if (configured) return configured

  try {
    const chunkName = new URL(import.meta.url).pathname.split('/').pop()?.trim()
    if (chunkName) return chunkName
  } catch {
    // no-op
  }

  return 'dev'
})()

// ---------------------------------------------------------------------------
// C++ state serialisation types (mirrors state_serializer.cpp output)
// ---------------------------------------------------------------------------
// Moved to ./cpp/types.ts (MO-1). Re-exported so existing imports keep working.

export * from './cpp/types'
import type {
  AcpAgentInfo,
  AcpBinding,
  AcpDiagnosticEntry,
  AcpMode,
  AcpModel,
  AcpPendingPermission,
  AcpPendingUserInput,
  AcpPermissionOption,
  AcpPlanEntry,
  AcpToolCall,
  AcpTurnEvent,
  AcpUserInputAnswers,
  AcpUserInputOption,
  AcpUserInputQuestion,
  AcpLifecycleState,
  AcpAttentionKind,
  ChatAttachmentInput,
  ChatMessagesResponse,
  CliBinding,
  CliLifecycleState,
  CliTranscript,
  CliVersionManager,
  CliVersionOption,
  CliVersionProviderState,
  CppAcpSession,
  CppAppState,
  CppChat,
  CppCliDebugState,
  CppCliDebugTerminal,
  CppFolder,
  CppGoal,
  CppMessage,
  CppProvider,
  CppSettings,
  CppStatePatch,
  EditorFileAssociation,
  GitWorktreeResult,
  GitWorktreeStatus,
  MemoryActivity,
  MemoryWorkerBinding,
  OpenWorkspaceEditorResponse,
  ParsedPushMessage,
  ParsedPushResult,
  ProviderChatDefaults,
  PushChannelStatus,
  VcsChangedFile,
  VcsCommitMessageSuggestion,
  VcsCommitResult,
  VcsCommitStatus,
  VcsFileDiffResponse,
  VcsType,
} from './cpp/types'

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null
}

function isString(value: unknown): value is string {
  return typeof value === 'string'
}

function stringOr(value: unknown, fallback = ''): string {
  return isString(value) ? value : fallback
}

function finiteNumberOr(value: unknown, fallback: number): number {
  return typeof value === 'number' && Number.isFinite(value) ? value : fallback
}

function clampedFiniteNumberOr(value: unknown, fallback: number, min: number, max: number): number {
  return Math.min(max, Math.max(min, finiteNumberOr(value, fallback)))
}

function booleanOr(value: unknown, fallback = false): boolean {
  return typeof value === 'boolean' ? value : fallback
}

const emptyMemoryActivity: MemoryActivity = {
  entryCount: 0,
  lastCreatedAt: '',
  lastCreatedCount: 0,
  runningCount: 0,
  lastStatus: '',
  lastWorkerChatId: '',
  lastWorkerProviderId: '',
  lastWorkerUpdatedAt: '',
  lastWorkerStatus: '',
  lastWorkerOutput: '',
  lastWorkerError: '',
  lastWorkerTimedOut: false,
  lastWorkerCanceled: false,
  lastWorkerHasExitCode: false,
  lastWorkerExitCode: 0,
}

function sanitizeMemoryActivity(value: unknown, fallbackStatus = ''): MemoryActivity {
  if (!isRecord(value)) {
    return { ...emptyMemoryActivity, lastStatus: fallbackStatus }
  }

  return {
    entryCount: Math.max(0, Math.floor(finiteNumberOr(value.entryCount, 0))),
    lastCreatedAt: stringOr(value.lastCreatedAt),
    lastCreatedCount: Math.max(0, Math.floor(finiteNumberOr(value.lastCreatedCount, 0))),
    runningCount: Math.max(0, Math.floor(finiteNumberOr(value.runningCount, 0))),
    lastStatus: stringOr(value.lastStatus, fallbackStatus),
    lastWorkerChatId: stringOr(value.lastWorkerChatId),
    lastWorkerProviderId: stringOr(value.lastWorkerProviderId),
    lastWorkerUpdatedAt: stringOr(value.lastWorkerUpdatedAt),
    lastWorkerStatus: stringOr(value.lastWorkerStatus),
    lastWorkerOutput: stringOr(value.lastWorkerOutput),
    lastWorkerError: stringOr(value.lastWorkerError),
    lastWorkerTimedOut: booleanOr(value.lastWorkerTimedOut),
    lastWorkerCanceled: booleanOr(value.lastWorkerCanceled),
    lastWorkerHasExitCode: booleanOr(value.lastWorkerHasExitCode),
    lastWorkerExitCode: Math.floor(finiteNumberOr(value.lastWorkerExitCode, 0)),
  }
}

function sanitizeAcpAttentionKind(value: unknown, fallback: AcpAttentionKind | null = null): AcpAttentionKind | null {
  if (
    value === 'question' ||
    value === 'plan' ||
    value === 'memory' ||
    value === 'permission' ||
    value === 'command' ||
    value === 'file' ||
    value === 'error' ||
    value === 'generic'
  ) {
    return value
  }
  return fallback
}

function normalizeAcpModelId(value: unknown): string {
  const modelId = stringOr(value).trim()
  return isAllowedAcpModelId(modelId) ? modelId : ''
}

function isAllowedAcpModelId(modelId: string): boolean {
  if (modelId === '') return true
  if (modelId.length > 160 || modelId.startsWith('-')) return false
  return /^[A-Za-z0-9._:/-]+$/.test(modelId)
}

function normalizeCodexReasoningEffort(value: unknown): string {
  const effort = stringOr(value).trim().toLowerCase()
  return ['none', 'minimal', 'low', 'medium', 'high', 'xhigh'].includes(effort) ? effort : ''
}

function normalizeCodexServiceTier(value: unknown): string {
  const tier = stringOr(value).trim().toLowerCase()
  return tier === 'fast' || tier === 'flex' ? tier : ''
}

function normalizeAcpApprovalMode(value: unknown): string {
  const modeId = stringOr(value).trim() || 'default'
  if (modeId === 'yolo') return 'default'
  if (modeId === 'auto_edit') return 'acceptEdits'
  return (ACP_APPROVAL_MODE_IDS as readonly string[]).includes(modeId) ? modeId : 'default'
}

function sanitizeCppMessage(value: unknown): CppMessage | null {
  if (!isRecord(value)) return null
  const role = value.role
  if (role !== 'user' && role !== 'assistant' && role !== 'system') return null
  if (!isString(value.content)) return null

  return {
    role,
    content: value.content,
    thoughts: isString(value.thoughts) ? value.thoughts : undefined,
    planSummary: isString(value.planSummary) ? value.planSummary : undefined,
    planEntries: Array.isArray(value.planEntries)
      ? value.planEntries.flatMap((entry) => {
          const sanitized = sanitizePlanEntry(entry)
          return sanitized ? [sanitized] : []
        })
      : [],
    toolCalls: Array.isArray(value.toolCalls)
      ? value.toolCalls.flatMap((toolCall) => {
          const sanitized = sanitizeToolCall(toolCall)
          return sanitized ? [sanitized] : []
        })
      : [],
    blocks: Array.isArray(value.blocks)
      ? value.blocks.flatMap((block) => {
          const sanitized = sanitizeTurnEvent(block)
          return sanitized ? [sanitized as MessageBlock] : []
        })
      : [],
    attachments: Array.isArray(value.attachments)
      ? value.attachments.flatMap((attachment) => {
          const sanitized = sanitizeAttachment(attachment)
          return sanitized ? [sanitized] : []
        })
      : [],
    createdAt: stringOr(value.createdAt),
  }
}

function sanitizeAttachment(value: unknown): Attachment | null {
  if (!isRecord(value)) return null
  const path = isString(value.path) ? value.path : ''
  const name = isString(value.name) ? value.name : path.split(/[\\/]/).pop() || ''
  const id = isString(value.id) ? value.id : path || name
  if (!id || !name) return null
  const type = isString(value.kind)
    ? value.kind
    : isString(value.type)
      ? value.type
      : 'file'
  return {
    id,
    name,
    type,
    size: finiteNumberOr(value.size, finiteNumberOr(value.sizeBytes, 0)),
    path,
  }
}

function messageAttachments(message: CppMessage): Attachment[] {
  const markdownAttachments = (message.markdownStoreFiles ?? []).map((filePath) => ({
    id: filePath,
    name: filePath.split(/[\\/]/).pop() || filePath,
    type: 'markdown-store',
    size: 0,
    path: filePath,
  }))
  return [...markdownAttachments, ...(message.attachments ?? [])]
}

function sanitizeCppCliTerminal(value: unknown): CppChat['cliTerminal'] | undefined {
  if (!isRecord(value)) return undefined

  return {
    terminalId: isString(value.terminalId) ? value.terminalId : undefined,
    frontendChatId: isString(value.frontendChatId) ? value.frontendChatId : undefined,
    sourceChatId: isString(value.sourceChatId) ? value.sourceChatId : undefined,
    running: booleanOr(value.running),
    lifecycleState: isString(value.lifecycleState) ? value.lifecycleState : undefined,
    turnState: isString(value.turnState) ? value.turnState : undefined,
    processing: typeof value.processing === 'boolean' ? value.processing : undefined,
    readySinceLastSelect: typeof value.readySinceLastSelect === 'boolean' ? value.readySinceLastSelect : undefined,
    active: typeof value.active === 'boolean' ? value.active : undefined,
    lastError: stringOr(value.lastError),
  }
}

function sanitizeDiagnostic(value: unknown): AcpDiagnosticEntry | null {
  if (!isRecord(value)) return null
  return {
    time: stringOr(value.time),
    event: stringOr(value.event),
    reason: stringOr(value.reason),
    method: stringOr(value.method),
    requestId: stringOr(value.requestId),
    code: typeof value.code === 'number' && Number.isFinite(value.code) ? value.code : null,
    message: stringOr(value.message),
    detail: stringOr(value.detail),
    lifecycleState: stringOr(value.lifecycleState),
  }
}

function sanitizeToolCall(value: unknown): AcpToolCall | null {
  if (!isRecord(value)) return null
  const id = stringOr(value.id)
  if (!id) return null
  return {
    id,
    title: stringOr(value.title),
    kind: stringOr(value.kind),
    status: stringOr(value.status),
    content: stringOr(value.content),
    isSubAgent: booleanOr(value.isSubAgent),
    subAgentId: stringOr(value.subAgentId),
    subAgentTitle: stringOr(value.subAgentTitle),
  }
}

function sanitizePlanEntry(value: unknown): AcpPlanEntry | null {
  if (!isRecord(value)) return null
  return {
    content: stringOr(value.content),
    priority: stringOr(value.priority),
    status: stringOr(value.status),
  }
}

function sanitizeAcpMode(value: unknown): AcpMode | null {
  if (!isRecord(value)) return null
  const rawId = stringOr(value.id).trim()
  if (!rawId) return null
  const id = normalizeAcpApprovalMode(rawId)
  if (id === 'default' && rawId !== 'default') return null
  return {
    id,
    name: stringOr(value.name, id),
    description: stringOr(value.description),
  }
}

function sanitizeAcpModel(value: unknown): AcpModel | null {
  if (!isRecord(value)) return null
  const id = normalizeAcpModelId(value.id)
  if (!id) return null
  const supportedReasoningEfforts = Array.isArray(value.supportedReasoningEfforts)
    ? Array.from(new Set(value.supportedReasoningEfforts.map(normalizeCodexReasoningEffort).filter(Boolean)))
    : []
  const additionalSpeedTiers = Array.isArray(value.additionalSpeedTiers)
    ? Array.from(new Set(value.additionalSpeedTiers.map(normalizeCodexServiceTier).filter(Boolean)))
    : []
  return {
    id,
    name: stringOr(value.name, id),
    description: stringOr(value.description),
    defaultReasoningEffort: normalizeCodexReasoningEffort(value.defaultReasoningEffort),
    supportedReasoningEfforts,
    additionalSpeedTiers,
  }
}

function sanitizeTurnEvent(value: unknown): AcpTurnEvent | null {
  if (!isRecord(value)) return null
  const type = value.type
  if (type === 'assistant_text' || type === 'thought') {
    return {
      type,
      text: stringOr(value.text),
      toolCallId: isString(value.toolCallId) ? value.toolCallId : undefined,
      requestId: isString(value.requestId) ? value.requestId : undefined,
    }
  }

  if (type === 'plan') {
    return {
      type,
      text: isString(value.text) ? value.text : undefined,
      toolCallId: isString(value.toolCallId) ? value.toolCallId : undefined,
      requestId: isString(value.requestId) ? value.requestId : undefined,
    }
  }

  if (type === 'tool_call') {
    const toolCallId = stringOr(value.toolCallId)
    if (!toolCallId) return null
    return {
      type,
      toolCallId,
      text: isString(value.text) ? value.text : undefined,
      requestId: isString(value.requestId) ? value.requestId : undefined,
    }
  }

  if (type === 'permission_request') {
    const requestId = stringOr(value.requestId)
    if (!requestId) return null
    return {
      type,
      requestId,
      toolCallId: isString(value.toolCallId) ? value.toolCallId : undefined,
      text: isString(value.text) ? value.text : undefined,
    }
  }

  if (type === 'user_input_request') {
    const requestId = stringOr(value.requestId)
    if (!requestId) return null
    return {
      type,
      requestId,
      toolCallId: isString(value.toolCallId) ? value.toolCallId : undefined,
      text: isString(value.text) ? value.text : undefined,
    }
  }

  return null
}

function sanitizePermissionOption(value: unknown): AcpPermissionOption | null {
  if (!isRecord(value)) return null
  const id = stringOr(value.id)
  if (!id) return null
  return {
    id,
    name: stringOr(value.name),
    kind: stringOr(value.kind),
  }
}

function sanitizePendingPermission(value: unknown): AcpPendingPermission | null {
  if (value == null) return null
  if (!isRecord(value)) return null
  const requestId = stringOr(value.requestId)
  if (!requestId) return null
  return {
    requestId,
    toolCallId: stringOr(value.toolCallId),
    title: stringOr(value.title),
    kind: stringOr(value.kind),
    status: stringOr(value.status),
    content: stringOr(value.content),
    options: Array.isArray(value.options)
      ? value.options.flatMap((option) => {
          const sanitized = sanitizePermissionOption(option)
          return sanitized ? [sanitized] : []
        })
      : [],
  }
}

function sanitizeUserInputOption(value: unknown): AcpUserInputOption | null {
  if (!isRecord(value)) return null
  return {
    label: stringOr(value.label),
    description: stringOr(value.description),
  }
}

function sanitizeUserInputQuestion(value: unknown): AcpUserInputQuestion | null {
  if (!isRecord(value)) return null
  const id = stringOr(value.id)
  if (!id) return null
  return {
    id,
    header: stringOr(value.header),
    question: stringOr(value.question),
    isOther: booleanOr(value.isOther),
    isSecret: booleanOr(value.isSecret),
    options: Array.isArray(value.options)
      ? value.options.flatMap((option) => {
          const sanitized = sanitizeUserInputOption(option)
          return sanitized ? [sanitized] : []
        })
      : [],
  }
}

function sanitizePendingUserInput(value: unknown): AcpPendingUserInput | null {
  if (value == null) return null
  if (!isRecord(value)) return null
  const requestId = stringOr(value.requestId)
  if (!requestId) return null
  return {
    requestId,
    itemId: stringOr(value.itemId),
    status: stringOr(value.status),
    attentionKind: sanitizeAcpAttentionKind(value.attentionKind, 'question') ?? 'question',
    questions: Array.isArray(value.questions)
      ? value.questions.flatMap((question) => {
          const sanitized = sanitizeUserInputQuestion(question)
          return sanitized ? [sanitized] : []
        })
      : [],
  }
}

function sanitizeAgentInfo(value: unknown): Partial<AcpAgentInfo> | undefined {
  if (!isRecord(value)) return undefined
  return {
    name: stringOr(value.name),
    title: stringOr(value.title),
    version: stringOr(value.version),
  }
}

function sanitizeCppAcpSession(value: unknown): CppAcpSession | undefined {
  if (!isRecord(value)) return undefined

  return {
    sessionId: isString(value.sessionId) ? value.sessionId : undefined,
    providerId: isString(value.providerId) ? value.providerId : undefined,
    protocolKind: isString(value.protocolKind) ? value.protocolKind : undefined,
    threadId: isString(value.threadId) ? value.threadId : undefined,
    running: typeof value.running === 'boolean' ? value.running : undefined,
    processing: typeof value.processing === 'boolean' ? value.processing : undefined,
    readySinceLastSelect: typeof value.readySinceLastSelect === 'boolean' ? value.readySinceLastSelect : undefined,
    attentionKind: sanitizeAcpAttentionKind(value.attentionKind),
    lifecycleState: isString(value.lifecycleState) ? value.lifecycleState : undefined,
    lastError: isString(value.lastError) ? value.lastError : undefined,
    recentStderr: isString(value.recentStderr) ? value.recentStderr : undefined,
    lastExitCode: typeof value.lastExitCode === 'number' && Number.isFinite(value.lastExitCode) ? value.lastExitCode : null,
    diagnostics: Array.isArray(value.diagnostics)
      ? value.diagnostics.flatMap((entry) => {
          const sanitized = sanitizeDiagnostic(entry)
          return sanitized ? [sanitized] : []
        })
      : [],
    agentInfo: sanitizeAgentInfo(value.agentInfo),
    toolCalls: Array.isArray(value.toolCalls)
      ? value.toolCalls.flatMap((toolCall) => {
          const sanitized = sanitizeToolCall(toolCall)
          return sanitized ? [sanitized] : []
        })
      : [],
    planSummary: stringOr(value.planSummary),
    planEntries: Array.isArray(value.planEntries)
      ? value.planEntries.flatMap((entry) => {
          const sanitized = sanitizePlanEntry(entry)
          return sanitized ? [sanitized] : []
        })
      : [],
    availableModes: Array.isArray(value.availableModes)
      ? value.availableModes.flatMap((mode) => {
          const sanitized = sanitizeAcpMode(mode)
          return sanitized ? [sanitized] : []
        })
      : [],
    currentModeId: normalizeAcpApprovalMode(value.currentModeId),
    availableModels: Array.isArray(value.availableModels)
      ? value.availableModels.flatMap((model) => {
          const sanitized = sanitizeAcpModel(model)
          return sanitized ? [sanitized] : []
        })
      : [],
    currentModelId: normalizeAcpModelId(value.currentModelId),
    turnEvents: Array.isArray(value.turnEvents)
      ? value.turnEvents.flatMap((event) => {
          const sanitized = sanitizeTurnEvent(event)
          return sanitized ? [sanitized] : []
        })
      : [],
    turnUserMessageIndex: finiteNumberOr(value.turnUserMessageIndex, -1),
    turnAssistantMessageIndex: finiteNumberOr(value.turnAssistantMessageIndex, -1),
    turnSerial: finiteNumberOr(value.turnSerial, 0),
    waitIsStale: Boolean(value.waitIsStale),
    waitStaleReason: stringOr(value.waitStaleReason),
    waitSeconds: finiteNumberOr(value.waitSeconds, 0),
    pendingPermission: sanitizePendingPermission(value.pendingPermission),
    pendingUserInput: sanitizePendingUserInput(value.pendingUserInput),
  }
}

function sanitizeCppFolder(value: unknown): CppFolder | null {
  if (!isRecord(value)) return null
  const id = stringOr(value.id).trim()
  if (!id) return null
  return {
    id,
    title: stringOr(value.title, 'Untitled'),
    directory: stringOr(value.directory),
    collapsed: booleanOr(value.collapsed),
  }
}

function sanitizeCppChat(value: unknown): CppChat | null {
  if (!isRecord(value)) return null
  const id = stringOr(value.id).trim()
  if (!id) return null
  return {
    id,
    title: stringOr(value.title, 'Untitled'),
    folderId: stringOr(value.folderId),
    pinned: booleanOr(value.pinned),
    providerId: stringOr(value.providerId, GEMINI_CLI_PROVIDER_ID),
    modelId: normalizeAcpModelId(value.modelId),
    approvalMode: normalizeAcpApprovalMode(value.approvalMode),
    autoApproveCommands: booleanOr(value.autoApproveCommands, stringOr(value.approvalMode).trim() === 'yolo'),
    memoryEnabled: booleanOr(value.memoryEnabled, true),
	    memoryLastProcessedMessageCount: finiteNumberOr(value.memoryLastProcessedMessageCount, 0),
	    memoryLastProcessedAt: isString(value.memoryLastProcessedAt) ? value.memoryLastProcessedAt : undefined,
	    workspaceDirectory: isString(value.workspaceDirectory) ? value.workspaceDirectory : undefined,
	    workspaceIsolationKind: isString(value.workspaceIsolationKind) ? value.workspaceIsolationKind : undefined,
	    workspaceSourceDirectory: isString(value.workspaceSourceDirectory) ? value.workspaceSourceDirectory : undefined,
	    workspaceBaseRef: isString(value.workspaceBaseRef) ? value.workspaceBaseRef : undefined,
	    workspaceBranchName: isString(value.workspaceBranchName) ? value.workspaceBranchName : undefined,
	    workspaceWorktreeDirectory: isString(value.workspaceWorktreeDirectory) ? value.workspaceWorktreeDirectory : undefined,
	    createdAt: stringOr(value.createdAt),
    updatedAt: stringOr(value.updatedAt),
    lastOpenedAt: isString(value.lastOpenedAt) ? value.lastOpenedAt : undefined,
    messageCount: finiteNumberOr(value.messageCount, 0),
    messagesDigest: isString(value.messagesDigest) ? value.messagesDigest : undefined,
    messages: Array.isArray(value.messages)
      ? value.messages.flatMap((message) => {
          const sanitized = sanitizeCppMessage(message)
          return sanitized ? [sanitized] : []
        })
      : undefined,
    cliTerminal: sanitizeCppCliTerminal(value.cliTerminal),
    acpSession: sanitizeCppAcpSession(value.acpSession),
    activeGoalId: isString(value.activeGoalId) ? value.activeGoalId : null,
    goals: Array.isArray(value.goals)
      ? value.goals.flatMap((goal) => {
          const sanitized = sanitizeCppGoal(goal)
          return sanitized ? [sanitized] : []
        })
      : undefined,
	  }
	}

function sanitizeGoalStatus(value: unknown): GoalStatus {
  if (value === 'active' || value === 'complete' || value === 'blocked' || value === 'paused') return value
  return 'active'
}

function sanitizeCppGoal(value: unknown): CppGoal | null {
  if (!isRecord(value)) return null
  const id = stringOr(value.id).trim()
  if (!id) return null
  return {
    id,
    objective: stringOr(value.objective),
    status: sanitizeGoalStatus(value.status),
    tokenBudget: finiteNumberOr(value.tokenBudget, 0),
    tokensUsed: finiteNumberOr(value.tokensUsed, 0),
    blockedTurnCount: finiteNumberOr(value.blockedTurnCount, 0),
    lastBlocker: isString(value.lastBlocker) ? value.lastBlocker : undefined,
    completedItems: Array.isArray(value.completedItems) ? value.completedItems.filter(isString) : undefined,
    remainingItems: Array.isArray(value.remainingItems) ? value.remainingItems.filter(isString) : undefined,
    currentStep: isString(value.currentStep) ? value.currentStep : undefined,
    lastVerification: isString(value.lastVerification) ? value.lastVerification : undefined,
    lastNextPrompt: isString(value.lastNextPrompt) ? value.lastNextPrompt : undefined,
    sameNextPromptCount: finiteNumberOr(value.sameNextPromptCount, 0),
    loopCount: finiteNumberOr(value.loopCount, 0),
    createdAt: stringOr(value.createdAt),
    updatedAt: stringOr(value.updatedAt),
  }
}

function sanitizeGitWorktreeStatus(value: unknown): GitWorktreeStatus | null {
  if (!isRecord(value)) return null
  return {
    isGitRepository: booleanOr(value.isGitRepository),
    isSvnWorkspace: booleanOr(value.isSvnWorkspace),
    isolated: booleanOr(value.isolated),
    sourceDirty: booleanOr(value.sourceDirty),
    worktreeDirty: booleanOr(value.worktreeDirty),
    worktreeMissing: booleanOr(value.worktreeMissing),
    sourceDirectory: stringOr(value.sourceDirectory),
    worktreeDirectory: stringOr(value.worktreeDirectory),
    branchName: stringOr(value.branchName),
    baseRef: stringOr(value.baseRef),
    warning: stringOr(value.warning),
    error: stringOr(value.error),
  }
}

function sanitizeGitWorktreeResult(value: unknown): GitWorktreeResult {
  if (!isRecord(value)) {
    return { ok: false, message: 'Invalid worktree response.', patchPath: '' }
  }
  return {
    ok: booleanOr(value.ok, true),
    status: sanitizeGitWorktreeStatus(value.status) ?? undefined,
    message: stringOr(value.message),
    patchPath: stringOr(value.patchPath),
  }
}

function cefPayloadOrRawResponse<T>(response: { data?: T }): unknown {
  return response.data ?? response
}

function failedGitWorktreeResult(message: string): GitWorktreeResult {
  return { ok: false, message, patchPath: '' }
}

function sanitizeVcsType(value: unknown): VcsType {
  return value === 'svn' ? 'svn' : 'git'
}

function sanitizeVcsCommitStatus(value: unknown): VcsCommitStatus | null {
  if (!isRecord(value)) return null
  return {
    available: booleanOr(value.available),
    vcsTypes: Array.isArray(value.vcsTypes)
      ? value.vcsTypes.map(sanitizeVcsType).filter((candidate, index, all) => all.indexOf(candidate) === index)
      : [],
    activeVcsType: sanitizeVcsType(value.activeVcsType),
    workspaceDirectory: stringOr(value.workspaceDirectory),
    branchOrRevision: stringOr(value.branchOrRevision),
    changedFiles: Array.isArray(value.changedFiles)
      ? value.changedFiles.flatMap((file) => {
          if (!isRecord(file)) return []
          const path = stringOr(file.path)
          return path
            ? [{
                path,
                status: stringOr(file.status),
                staged: booleanOr(file.staged),
                additions: finiteNumberOr(file.additions, 0),
                deletions: finiteNumberOr(file.deletions, 0),
                binary: booleanOr(file.binary),
              }]
            : []
        })
      : [],
    lineStatsReady: typeof value.lineStatsReady === 'boolean' ? value.lineStatsReady : true,
    warning: stringOr(value.warning),
    error: stringOr(value.error),
  }
}

function sanitizeVcsCommitResult(value: unknown): VcsCommitResult {
  if (!isRecord(value)) {
    return { ok: false, message: '', error: 'Invalid VCS commit response.' }
  }
  return {
    ok: booleanOr(value.ok),
    status: sanitizeVcsCommitStatus(value.status) ?? undefined,
    message: stringOr(value.message),
    error: stringOr(value.error),
  }
}

function sanitizeCppProvider(value: unknown): CppProvider | null {
  if (!isRecord(value)) return null
  const id = stringOr(value.id).trim()
  if (!id) return null
  return {
    id,
    name: stringOr(value.name, id),
    shortName: stringOr(value.shortName, stringOr(value.name, id)),
    outputMode: isString(value.outputMode) ? value.outputMode : undefined,
    supportsCli: typeof value.supportsCli === 'boolean' ? value.supportsCli : undefined,
    supportsStructured: typeof value.supportsStructured === 'boolean' ? value.supportsStructured : undefined,
    structuredProtocol: isString(value.structuredProtocol) ? value.structuredProtocol : undefined,
  }
}

function sanitizeCliDebugTerminal(value: unknown): CppCliDebugTerminal | null {
  if (!isRecord(value)) return null
  const terminalId = stringOr(value.terminalId)
  if (!terminalId) return null
  return {
    terminalId,
    frontendChatId: stringOr(value.frontendChatId),
    sourceChatId: stringOr(value.sourceChatId),
    attachedSessionId: stringOr(value.attachedSessionId),
    providerId: stringOr(value.providerId),
    nativeSessionId: stringOr(value.nativeSessionId),
    processId: stringOr(value.processId),
    running: booleanOr(value.running),
    uiAttached: booleanOr(value.uiAttached),
    lifecycleState: isString(value.lifecycleState) ? value.lifecycleState : undefined,
    turnState: isString(value.turnState) ? value.turnState : 'idle',
    inputReady: booleanOr(value.inputReady),
    generationInProgress: booleanOr(value.generationInProgress),
    lastUserInputAt: finiteNumberOr(value.lastUserInputAt, 0),
    lastAiOutputAt: finiteNumberOr(value.lastAiOutputAt, 0),
    lastPolledAt: finiteNumberOr(value.lastPolledAt, 0),
    lastError: stringOr(value.lastError),
  }
}

function sanitizeCliDebugState(value: unknown): CppCliDebugState | undefined {
  if (!isRecord(value)) return undefined
  const terminals = Array.isArray(value.terminals)
    ? value.terminals.flatMap((terminal) => {
        const sanitized = sanitizeCliDebugTerminal(terminal)
        return sanitized ? [sanitized] : []
      })
    : []

  return {
    selectedChatId: isString(value.selectedChatId) ? value.selectedChatId : null,
    terminalCount: finiteNumberOr(value.terminalCount, terminals.length),
    runningTerminalCount: finiteNumberOr(value.runningTerminalCount, terminals.filter((terminal) => terminal.running).length),
    busyTerminalCount: finiteNumberOr(value.busyTerminalCount, terminals.filter((terminal) => terminal.turnState === 'busy').length),
    terminals,
  }
}

const emptyCliVersionProviderState: CliVersionProviderState = {
  providerId: GEMINI_CLI_PROVIDER_ID,
  installedVersion: '',
  selectedVersion: '',
  availableVersions: [],
  preferredVersion: '',
  status: 'unknown',
  message: '',
  running: false,
  lastCommand: '',
  lastOutput: '',
}

const emptyCliVersionManager: CliVersionManager = {
  providers: [],
}

function sanitizeCliVersionProviderState(value: unknown): CliVersionProviderState | null {
  if (!isRecord(value)) return null
  const providerId = stringOr(value.providerId, GEMINI_CLI_PROVIDER_ID).trim()
  if (!providerId) return null
  const status = stringOr(value.status)
  const normalizedStatus: CliVersionProviderState['status'] =
    status === 'checking' ||
    status === 'installing' ||
    status === 'supported' ||
    status === 'unsupported' ||
    status === 'unknown'
      ? status
      : 'unknown'

  return {
    providerId,
    installedVersion: stringOr(value.installedVersion),
    selectedVersion: stringOr(value.selectedVersion),
    availableVersions: Array.isArray(value.availableVersions)
      ? value.availableVersions.flatMap((option) => {
          if (!isRecord(option)) return []
          const version = stringOr(option.version).trim()
          return version ? [{ version, preferred: booleanOr(option.preferred) }] : []
        })
      : [],
    preferredVersion: stringOr(value.preferredVersion),
    status: normalizedStatus,
    message: stringOr(value.message),
    running: booleanOr(value.running),
    lastCommand: stringOr(value.lastCommand),
    lastOutput: stringOr(value.lastOutput),
  }
}

function sanitizeCliVersionManager(value: unknown): CliVersionManager | undefined {
  if (!isRecord(value)) return undefined
  const providers = Array.isArray(value.providers)
    ? value.providers.flatMap((provider) => {
        const sanitized = sanitizeCliVersionProviderState(provider)
        return sanitized ? [sanitized] : []
      })
    : []
  if (providers.length > 0) {
    return { providers }
  }

  const legacy = sanitizeCliVersionProviderState(value)
  return { providers: legacy ? [legacy] : [] }
}

function upsertCliVersionProviderState(
  providers: CliVersionProviderState[],
  next: CliVersionProviderState
): CliVersionProviderState[] {
  const found = providers.some((provider) => provider.providerId === next.providerId)
  if (!found) return [...providers, next]
  return providers.map((provider) => provider.providerId === next.providerId ? next : provider)
}

const DEFAULT_EDITOR_FILE_ASSOCIATIONS: EditorFileAssociation[] = [
  {
    id: 'cpp',
    name: 'C++',
    extensions: ['.c', '.cc', '.cpp', '.cxx', '.h', '.hh', '.hpp', '.hxx'],
    editorPresetId: 'clion',
  },
  {
    id: 'csharp',
    name: 'C#',
    extensions: ['.cs', '.csx', '.csproj', '.sln'],
    editorPresetId: 'rider',
  },
  {
    id: 'python',
    name: 'Python',
    extensions: ['.py', '.pyw', '.ipynb'],
    editorPresetId: 'pycharm',
  },
  {
    id: 'javascript',
    name: 'JavaScript',
    extensions: ['.js', '.mjs', '.cjs'],
    editorPresetId: 'webstorm',
  },
  {
    id: 'react-typescript',
    name: 'React / TypeScript',
    extensions: ['.jsx', '.ts', '.tsx', '.mts', '.cts'],
    editorPresetId: 'webstorm',
  },
  {
    id: 'rust',
    name: 'Rust',
    extensions: ['.rs'],
    editorPresetId: 'rustrover',
  },
  {
    id: 'go',
    name: 'Go',
    extensions: ['.go'],
    editorPresetId: 'goland',
  },
  {
    id: 'java-kotlin',
    name: 'Java / Kotlin',
    extensions: ['.java', '.kt', '.kts'],
    editorPresetId: 'idea',
  },
  {
    id: 'swift-apple',
    name: 'Swift / Apple',
    extensions: ['.swift'],
    editorPresetId: 'xcode',
  },
  {
    id: 'powershell',
    name: 'PowerShell',
    extensions: ['.ps1', '.psm1', '.psd1'],
    editorPresetId: 'vscode',
  },
  {
    id: 'shell',
    name: 'Bash / Shell',
    extensions: ['.sh', '.bash', '.zsh', '.fish'],
    editorPresetId: 'vscode',
  },
  {
    id: 'web-styles',
    name: 'Web Styles / Templates',
    extensions: ['.html', '.css', '.scss', '.sass', '.less'],
    editorPresetId: 'webstorm',
  },
]

const EDITOR_PRESET_IDS = [
  'vscode',
  'xcode',
  'visualstudio',
  'clion',
  'rider',
  'webstorm',
  'pycharm',
  'idea',
  'goland',
  'rustrover',
]

function defaultEditorFileAssociations() {
  return DEFAULT_EDITOR_FILE_ASSOCIATIONS.map((association) => ({
    ...association,
    extensions: [...association.extensions],
  }))
}

function normalizeEditorExtension(value: string) {
  const trimmed = value.trim().toLowerCase()
  if (!trimmed) return ''
  return trimmed.startsWith('.') ? trimmed : `.${trimmed}`
}

function sanitizeEditorPresetId(value: unknown) {
  const preset = stringOr(value, 'vscode').trim()
  return EDITOR_PRESET_IDS.includes(preset) ? preset : 'vscode'
}

function sanitizeEditorFileAssociations(value: unknown): EditorFileAssociation[] {
  if (!Array.isArray(value)) return defaultEditorFileAssociations()
  const associations = value.flatMap((item, index) => {
    if (!isRecord(item)) return []
    const extensions = Array.isArray(item.extensions)
      ? Array.from(new Set(item.extensions.map((extension) => normalizeEditorExtension(stringOr(extension))).filter(Boolean)))
      : []
    const name = stringOr(item.name).trim()
    if (!name || extensions.length === 0) return []
    return [{
      id: stringOr(item.id, `editor-group-${index + 1}`).trim() || `editor-group-${index + 1}`,
      name,
      extensions,
      editorPresetId: sanitizeEditorPresetId(item.editorPresetId),
    }]
  })
  return associations.length > 0 ? associations : defaultEditorFileAssociations()
}

function sanitizeProviderChatDefaults(value: unknown): ProviderChatDefaults {
  if (!isRecord(value)) {
    return {
      modelId: '',
      approvalMode: 'default',
      autoApproveCommands: false,
      memoryEnabled: true,
      reasoningEffort: '',
      serviceTier: '',
    }
  }
  return {
    modelId: normalizeAcpModelId(value.modelId),
    approvalMode: normalizeAcpApprovalMode(value.approvalMode),
    autoApproveCommands: booleanOr(value.autoApproveCommands),
    memoryEnabled: booleanOr(value.memoryEnabled, true),
    reasoningEffort: normalizeCodexReasoningEffort(value.reasoningEffort),
    serviceTier: normalizeCodexServiceTier(value.serviceTier),
  }
}

function sanitizeProviderChatDefaultsMap(value: unknown): Record<string, ProviderChatDefaults> {
  const defaults: Record<string, ProviderChatDefaults> = {}
  if (!isRecord(value)) return defaults
  for (const [providerId, providerDefaults] of Object.entries(value)) {
    const id = normalizeCliProviderIdAlias(providerId)
    if (!id) continue
    defaults[id] = sanitizeProviderChatDefaults(providerDefaults)
  }
  return defaults
}

function providerChatDefaultsForNewChat(
  state: {
    providerChatDefaults: Record<string, ProviderChatDefaults>
    memoryEnabledDefault: boolean
  },
  providerId: string
): ProviderChatDefaults {
  const saved = state.providerChatDefaults[providerId]
  const defaults = sanitizeProviderChatDefaults(saved ?? null)
  if (!saved) {
    defaults.memoryEnabled = state.memoryEnabledDefault
  }
  if (providerId !== CODEX_CLI_PROVIDER_ID) {
    defaults.reasoningEffort = ''
    defaults.serviceTier = ''
  }
  return defaults
}

function sanitizeCppSettings(value: unknown): CppSettings {
  if (!isRecord(value)) {
    return {
      activeProviderId: GEMINI_CLI_PROVIDER_ID,
      theme: 'dark',
      memoryEnabledDefault: true,
      memoryIdleDelaySeconds: DEFAULT_MEMORY_IDLE_DELAY_SECONDS,
      memoryRecallBudgetBytes: DEFAULT_MEMORY_RECALL_BUDGET_BYTES,
      memoryLastStatus: '',
      memoryWorkerBindings: {},
      defaultNewChatProviderId: GEMINI_CLI_PROVIDER_ID,
      providerChatDefaults: {},
      markdownStoreDirectory: '',
      defaultEditorPresetId: 'vscode',
      editorFileAssociations: defaultEditorFileAssociations(),
    }
  }

  const theme = normalizeStoredTheme(value.theme) ?? 'dark'
  const bindings: Record<string, MemoryWorkerBinding> = {}
  if (isRecord(value.memoryWorkerBindings)) {
    for (const [providerId, binding] of Object.entries(value.memoryWorkerBindings)) {
      if (!isRecord(binding)) continue
      bindings[providerId] = {
        workerProviderId: stringOr(binding.workerProviderId),
        workerModelId: stringOr(binding.workerModelId),
      }
    }
  }
  return {
    activeProviderId: stringOr(value.activeProviderId, GEMINI_CLI_PROVIDER_ID),
    theme,
    memoryEnabledDefault: booleanOr(value.memoryEnabledDefault, true),
    memoryIdleDelaySeconds: clampedFiniteNumberOr(value.memoryIdleDelaySeconds, DEFAULT_MEMORY_IDLE_DELAY_SECONDS, MIN_MEMORY_IDLE_DELAY_SECONDS, MAX_MEMORY_IDLE_DELAY_SECONDS),
    memoryRecallBudgetBytes: clampedFiniteNumberOr(value.memoryRecallBudgetBytes, DEFAULT_MEMORY_RECALL_BUDGET_BYTES, MIN_MEMORY_RECALL_BUDGET_BYTES, MAX_MEMORY_RECALL_BUDGET_BYTES),
    memoryLastStatus: stringOr(value.memoryLastStatus),
    memoryWorkerBindings: bindings,
    defaultNewChatProviderId: stringOr(value.defaultNewChatProviderId, stringOr(value.activeProviderId, GEMINI_CLI_PROVIDER_ID)),
    providerChatDefaults: sanitizeProviderChatDefaultsMap(value.providerChatDefaults),
    markdownStoreDirectory: stringOr(value.markdownStoreDirectory),
    defaultEditorPresetId: sanitizeEditorPresetId(value.defaultEditorPresetId),
    editorFileAssociations: sanitizeEditorFileAssociations(value.editorFileAssociations),
  }
}

function sanitizeCppAppState(value: unknown): CppAppState | null {
  if (!isRecord(value)) return null

  const folders = Array.isArray(value.folders)
    ? value.folders.flatMap((folder) => {
        const sanitized = sanitizeCppFolder(folder)
        return sanitized ? [sanitized] : []
      })
    : []
  const chats = Array.isArray(value.chats)
    ? value.chats.flatMap((chat) => {
        const sanitized = sanitizeCppChat(chat)
        return sanitized ? [sanitized] : []
      })
    : []
  const providers = Array.isArray(value.providers)
    ? value.providers.flatMap((provider) => {
        const sanitized = sanitizeCppProvider(provider)
        return sanitized ? [sanitized] : []
      })
    : []

  const selectedChatId =
    isString(value.selectedChatId)
      ? value.selectedChatId
      : typeof value.selectedChatIndex === 'number' &&
          Number.isInteger(value.selectedChatIndex) &&
          value.selectedChatIndex >= 0 &&
          value.selectedChatIndex < chats.length
        ? chats[value.selectedChatIndex].id
        : null

  const settings = sanitizeCppSettings(value.settings)

  return {
    stateRevision: finiteNumberOr(value.stateRevision, 0),
    folders,
    chats,
    cliDebug: sanitizeCliDebugState(value.cliDebug),
    selectedChatId,
    selectedChatIndex: finiteNumberOr(value.selectedChatIndex, -1),
    providers,
    settings,
    memoryActivity: sanitizeMemoryActivity(value.memoryActivity, settings.memoryLastStatus),
    cliVersionManager: sanitizeCliVersionManager(value.cliVersionManager),
  }
}

function sanitizeMessagesByChatId(value: unknown): Record<string, CppMessage[]> | undefined {
  if (!isRecord(value)) return undefined
  const messagesByChatId: Record<string, CppMessage[]> = {}
  for (const [chatId, messages] of Object.entries(value)) {
    if (!Array.isArray(messages)) continue
    messagesByChatId[chatId] = messages.flatMap((message) => {
      const sanitized = sanitizeCppMessage(message)
      return sanitized ? [sanitized] : []
    })
  }
  return messagesByChatId
}

function sanitizeCppStatePatch(value: unknown): CppStatePatch | null {
  if (!isRecord(value)) return null

  return {
    stateRevision: finiteNumberOr(value.stateRevision, 0),
    folders: Array.isArray(value.folders)
      ? value.folders.flatMap((folder) => {
          const sanitized = sanitizeCppFolder(folder)
          return sanitized ? [sanitized] : []
        })
      : undefined,
    chats: Array.isArray(value.chats)
      ? value.chats.flatMap((chat) => {
          const sanitized = sanitizeCppChat(chat)
          return sanitized ? [sanitized] : []
        })
      : undefined,
    removedChatIds: Array.isArray(value.removedChatIds)
      ? value.removedChatIds.filter(isString)
      : undefined,
    chatOrder: Array.isArray(value.chatOrder)
      ? value.chatOrder.filter(isString)
      : undefined,
    messagesByChatId: sanitizeMessagesByChatId(value.messagesByChatId),
    selectedChatId:
      isString(value.selectedChatId)
        ? value.selectedChatId
        : value.selectedChatId === null
          ? null
          : undefined,
    providers: Array.isArray(value.providers)
      ? value.providers.flatMap((provider) => {
          const sanitized = sanitizeCppProvider(provider)
          return sanitized ? [sanitized] : []
        })
      : undefined,
    settings: isRecord(value.settings) ? sanitizeCppSettings(value.settings) : undefined,
    memoryActivity: value.memoryActivity !== undefined ? sanitizeMemoryActivity(value.memoryActivity) : undefined,
    cliVersionManager: value.cliVersionManager !== undefined ? sanitizeCliVersionManager(value.cliVersionManager) : undefined,
  }
}

function cppStateRevision(state: CppAppState): number {
  return typeof state.stateRevision === 'number' && Number.isFinite(state.stateRevision)
    ? state.stateRevision
    : 0
}

function cppPatchRevision(patch: CppStatePatch): number {
  return typeof patch.stateRevision === 'number' && Number.isFinite(patch.stateRevision)
    ? patch.stateRevision
    : 0
}

function normalizeProviderIdForVisibleProviders(
  providerId: string | undefined,
  providers: Array<{ id: string }>
): string {
  const requestedProviderId = normalizeCliProviderIdAlias(providerId ?? '') || GEMINI_CLI_PROVIDER_ID
  if (providers.some((provider) => provider.id === requestedProviderId)) {
    return requestedProviderId
  }
  return providers[0]?.id ?? GEMINI_CLI_PROVIDER_ID
}

function foldersEquivalent(previous: Folder, next: Folder): boolean {
  return previous.name === next.name &&
    previous.directory === next.directory &&
    previous.isExpanded === next.isExpanded
}

function folderFromCppFolder(folder: CppFolder, previous: Folder | undefined): Folder {
  const nextFolder: Folder = {
    id: folder.id,
    name: folder.title,
    parentId: null,
    directory: folder.directory ?? '',
    isExpanded: !folder.collapsed,
    createdAt: previous?.createdAt ?? new Date(),
  }

  return previous && foldersEquivalent(previous, nextFolder) ? previous : nextFolder
}

function providersEquivalent(previous: Provider, next: Provider): boolean {
  return previous.id === next.id &&
    previous.name === next.name &&
    previous.shortName === next.shortName &&
    previous.color === next.color &&
    previous.description === next.description &&
    previous.outputMode === next.outputMode &&
    previous.supportsCli === next.supportsCli &&
    previous.supportsStructured === next.supportsStructured &&
    previous.structuredProtocol === next.structuredProtocol &&
    previous.npmPackageName === next.npmPackageName
}

function providerFromCppProvider(provider: CppProvider, previous: Provider | undefined): Provider {
  const nextProvider: Provider = {
    id: provider.id,
    name: provider.name,
    shortName: provider.shortName,
    color: previous?.color ?? '#f97316',
    description: previous?.description ?? '',
    outputMode: provider.outputMode,
    supportsCli: provider.supportsCli,
    supportsStructured: provider.supportsStructured,
    structuredProtocol: provider.structuredProtocol,
    npmPackageName: provider.npmPackageName,
  }

  if (previous) {
    return providersEquivalent(previous, nextProvider) ? previous : nextProvider
  }

  return nextProvider
}

function sessionsEquivalent(previous: Session, next: Session): boolean {
  return previous.name === next.name &&
    previous.folderId === next.folderId &&
    (previous.isPinned ?? false) === (next.isPinned ?? false) &&
    (previous.providerId ?? GEMINI_CLI_PROVIDER_ID) === next.providerId &&
    (previous.modelId ?? '') === (next.modelId ?? '') &&
    (previous.reasoningEffort ?? '') === (next.reasoningEffort ?? '') &&
    (previous.serviceTier ?? '') === (next.serviceTier ?? '') &&
    (previous.approvalMode ?? 'default') === next.approvalMode &&
    (previous.autoApproveCommands ?? false) === (next.autoApproveCommands ?? false) &&
    (previous.memoryEnabled ?? true) === next.memoryEnabled &&
    (previous.memoryLastProcessedMessageCount ?? 0) === next.memoryLastProcessedMessageCount &&
    (previous.memoryLastProcessedAt ?? '') === next.memoryLastProcessedAt &&
    (previous.workspaceDirectory ?? '') === (next.workspaceDirectory ?? '') &&
    (previous.workspaceIsolationKind ?? '') === (next.workspaceIsolationKind ?? '') &&
    (previous.workspaceSourceDirectory ?? '') === (next.workspaceSourceDirectory ?? '') &&
    (previous.workspaceBaseRef ?? '') === (next.workspaceBaseRef ?? '') &&
    (previous.workspaceBranchName ?? '') === (next.workspaceBranchName ?? '') &&
    (previous.workspaceWorktreeDirectory ?? '') === (next.workspaceWorktreeDirectory ?? '') &&
    (previous.messageCount ?? 0) === (next.messageCount ?? 0) &&
    (previous.messagesDigest ?? '') === (next.messagesDigest ?? '') &&
    previous.viewMode === next.viewMode &&
    previous.createdAt.getTime() === next.createdAt.getTime() &&
    previous.updatedAt.getTime() === next.updatedAt.getTime() &&
    (previous.lastOpenedAt ?? previous.updatedAt).getTime() === next.lastOpenedAt?.getTime()
}

function sessionFromCppChat(
  chat: CppChat,
  previous: Session | undefined,
  visibleProviders: Array<{ id: string }>
): Session {
  const createdAt = new Date(chat.createdAt || Date.now())
  const updatedAt = new Date(chat.updatedAt || Date.now())
  const lastOpenedAt = new Date(chat.lastOpenedAt || chat.updatedAt || chat.createdAt || Date.now())
  const nextSession: Session = {
    id: chat.id,
    name: chat.title || 'Untitled',
    viewMode: 'chat',
    folderId: chat.folderId || null,
    isPinned: chat.pinned ?? false,
    providerId: normalizeProviderIdForVisibleProviders(chat.providerId, visibleProviders),
    modelId: chat.modelId ?? '',
    reasoningEffort: normalizeCodexReasoningEffort(chat.reasoningEffort),
    serviceTier: normalizeCodexServiceTier(chat.serviceTier),
    approvalMode: normalizeAcpApprovalMode(chat.approvalMode),
    autoApproveCommands: chat.autoApproveCommands ?? false,
    memoryEnabled: chat.memoryEnabled ?? true,
    memoryLastProcessedMessageCount: chat.memoryLastProcessedMessageCount ?? 0,
    memoryLastProcessedAt: chat.memoryLastProcessedAt ?? '',
    workspaceDirectory: chat.workspaceDirectory ?? '',
    workspaceIsolationKind: chat.workspaceIsolationKind ?? '',
    workspaceSourceDirectory: chat.workspaceSourceDirectory ?? '',
    workspaceBaseRef: chat.workspaceBaseRef ?? '',
    workspaceBranchName: chat.workspaceBranchName ?? '',
    workspaceWorktreeDirectory: chat.workspaceWorktreeDirectory ?? '',
    messageCount: chat.messageCount ?? 0,
    messagesDigest: chat.messagesDigest ?? '',
    createdAt,
    updatedAt,
    lastOpenedAt,
  }

  return previous && sessionsEquivalent(previous, nextSession) ? previous : nextSession
}

const MAX_CLI_TRANSCRIPT_BYTES = 1024 * 1024

function decodeCliChunk(encoded: string): string {
  try {
    return atob(encoded)
  } catch {
    return encoded
  }
}

function clampCliTranscript(content: string): string {
  return content.length > MAX_CLI_TRANSCRIPT_BYTES
    ? content.slice(content.length - MAX_CLI_TRANSCRIPT_BYTES)
    : content
}

function appendCliTranscriptChunk(
  existing: CliTranscript | undefined,
  terminalId: string,
  chunk: string
): CliTranscript {
  const nextTerminalId = terminalId || existing?.terminalId || ''
  const resetTranscript =
    Boolean(existing?.terminalId) && Boolean(terminalId) && existing?.terminalId !== terminalId
  const nextContent = clampCliTranscript((resetTranscript ? '' : existing?.content ?? '') + chunk)

  return {
    terminalId: nextTerminalId,
    content: nextContent,
  }
}

function normalizeCliLifecycleState(
  value: unknown,
  running: boolean,
  turnState?: string,
  processing?: boolean
): CliLifecycleState {
  if (
    value === 'disabled' ||
    value === 'stopped' ||
    value === 'idle' ||
    value === 'busy' ||
    value === 'shuttingDown'
  ) {
    return value
  }

  if (!running) {
    return 'stopped'
  }

  if (turnState === 'busy' || processing) {
    return 'busy'
  }

  return 'idle'
}

function cliLifecycleIsProcessing(lifecycleState: CliLifecycleState): boolean {
  return lifecycleState === 'busy' || lifecycleState === 'shuttingDown'
}

function normalizeAcpLifecycleState(value: unknown, running: boolean, processing: boolean): AcpLifecycleState {
  if (
    value === 'stopped' ||
    value === 'starting' ||
    value === 'ready' ||
    value === 'processing' ||
    value === 'waitingPermission' ||
    value === 'waitingUserInput' ||
    value === 'error'
  ) {
    return value
  }

  if (processing) return 'processing'
  if (running) return 'ready'
  return 'stopped'
}

function diagnosticsEquivalent(existing: AcpDiagnosticEntry[], next: AcpDiagnosticEntry[]) {
  if (existing.length !== next.length) return false
  return existing.every((entry, index) => {
    const other = next[index]
    return (
      entry.time === other.time &&
      entry.event === other.event &&
      entry.reason === other.reason &&
      entry.method === other.method &&
      entry.requestId === other.requestId &&
      entry.code === other.code &&
      entry.message === other.message &&
      entry.detail === other.detail &&
      entry.lifecycleState === other.lifecycleState
    )
  })
}

function modesEquivalent(existing: AcpMode[], next: AcpMode[]) {
  if (existing.length !== next.length) return false
  return existing.every((mode, index) => {
    const other = next[index]
    return mode.id === other.id && mode.name === other.name && mode.description === other.description
  })
}

function modelsEquivalent(existing: AcpModel[], next: AcpModel[]) {
  if (existing.length !== next.length) return false
  return existing.every((model, index) => {
    const other = next[index]
    return model.id === other.id &&
      model.name === other.name &&
      model.description === other.description &&
      (model.defaultReasoningEffort ?? '') === (other.defaultReasoningEffort ?? '') &&
      (model.supportedReasoningEfforts ?? []).join('|') === (other.supportedReasoningEfforts ?? []).join('|') &&
      (model.additionalSpeedTiers ?? []).join('|') === (other.additionalSpeedTiers ?? []).join('|')
  })
}

function turnEventsEquivalent(existing: AcpTurnEvent[], next: AcpTurnEvent[]) {
  if (existing.length !== next.length) return false
  return existing.every((event, index) => {
    const other = next[index]
    if (event.type !== other.type) return false
    if ('text' in event || 'text' in other) {
      if ((event as { text?: string }).text !== (other as { text?: string }).text) return false
    }
    return (
      (event as { toolCallId?: string }).toolCallId === (other as { toolCallId?: string }).toolCallId &&
      (event as { requestId?: string }).requestId === (other as { requestId?: string }).requestId
    )
  })
}

function agentInfoEquivalent(existing: AcpAgentInfo | null, next: AcpAgentInfo | null) {
  if (existing === next) return true
  if (!existing || !next) return false
  return existing.name === next.name && existing.title === next.title && existing.version === next.version
}

function pendingPermissionEquivalent(existing: AcpPendingPermission | null, next: AcpPendingPermission | null) {
  if (existing === next) return true
  if (!existing || !next) return false
  return (
    existing.requestId === next.requestId &&
    existing.toolCallId === next.toolCallId &&
    existing.title === next.title &&
    existing.kind === next.kind &&
    existing.status === next.status &&
    existing.content === next.content &&
    existing.options.length === next.options.length &&
    existing.options.every((option, index) => {
      const other = next.options[index]
      return option.id === other.id && option.name === other.name && option.kind === other.kind
    })
  )
}

function pendingUserInputEquivalent(existing: AcpPendingUserInput | null, next: AcpPendingUserInput | null) {
  if (existing === next) return true
  if (!existing || !next) return false
  return (
    existing.requestId === next.requestId &&
    existing.itemId === next.itemId &&
    existing.status === next.status &&
    existing.attentionKind === next.attentionKind &&
    existing.questions.length === next.questions.length &&
    existing.questions.every((question, index) => {
      const other = next.questions[index]
      return (
        question.id === other.id &&
        question.header === other.header &&
        question.question === other.question &&
        question.isOther === other.isOther &&
        question.isSecret === other.isSecret &&
        question.options.length === other.options.length &&
        question.options.every((option, optionIndex) => {
          const otherOption = other.options[optionIndex]
          return option.label === otherOption.label && option.description === otherOption.description
        })
      )
    })
  )
}

function acpBindingsEquivalent(existing: AcpBinding | undefined, next: AcpBinding) {
  if (!existing) return false
  return (
    existing.sessionId === next.sessionId &&
    existing.providerId === next.providerId &&
    existing.protocolKind === next.protocolKind &&
    existing.threadId === next.threadId &&
    existing.running === next.running &&
    existing.lifecycleState === next.lifecycleState &&
    existing.processing === next.processing &&
    existing.readySinceLastSelect === next.readySinceLastSelect &&
    existing.attentionKind === next.attentionKind &&
    existing.processingStartedAtMs === next.processingStartedAtMs &&
    existing.lastError === next.lastError &&
    existing.recentStderr === next.recentStderr &&
    existing.lastExitCode === next.lastExitCode &&
    diagnosticsEquivalent(existing.diagnostics, next.diagnostics) &&
    toolCallsEquivalent(existing.toolCalls, next.toolCalls) &&
    existing.planSummary === next.planSummary &&
    planEntriesEquivalent(existing.planEntries, next.planEntries) &&
    modesEquivalent(existing.availableModes, next.availableModes) &&
    existing.currentModeId === next.currentModeId &&
    modelsEquivalent(existing.availableModels, next.availableModels) &&
    existing.currentModelId === next.currentModelId &&
    turnEventsEquivalent(existing.turnEvents, next.turnEvents) &&
    existing.turnUserMessageIndex === next.turnUserMessageIndex &&
    existing.turnAssistantMessageIndex === next.turnAssistantMessageIndex &&
    existing.turnSerial === next.turnSerial &&
    existing.waitIsStale === next.waitIsStale &&
    existing.waitStaleReason === next.waitStaleReason &&
    existing.waitSeconds === next.waitSeconds &&
    pendingPermissionEquivalent(existing.pendingPermission, next.pendingPermission) &&
    pendingUserInputEquivalent(existing.pendingUserInput, next.pendingUserInput) &&
    agentInfoEquivalent(existing.agentInfo, next.agentInfo)
  )
}

function cliBindingsEquivalent(existing: CliBinding | undefined, next: CliBinding) {
  if (!existing) return false
  return (
    existing.terminalId === next.terminalId &&
    existing.boundChatId === next.boundChatId &&
    existing.running === next.running &&
    existing.lifecycleState === next.lifecycleState &&
    existing.turnState === next.turnState &&
    existing.processing === next.processing &&
    existing.readySinceLastSelect === next.readySinceLastSelect &&
    existing.active === next.active &&
    existing.lastError === next.lastError
  )
}

function cliBindingFromCppChat(chat: CppChat, previous: CliBinding | undefined): CliBinding | null {
  if (!chat.cliTerminal) return null

  const running = Boolean(chat.cliTerminal.running)
  const lifecycleState = normalizeCliLifecycleState(
    chat.cliTerminal.lifecycleState,
    running,
    chat.cliTerminal.turnState,
    chat.cliTerminal.processing
  )
  const processing = Boolean(chat.cliTerminal.processing) || cliLifecycleIsProcessing(lifecycleState)
  const next: CliBinding = {
    terminalId: chat.cliTerminal.terminalId ?? '',
    boundChatId: chat.cliTerminal.sourceChatId ?? chat.id,
    running,
    lifecycleState,
    turnState: processing ? 'busy' : 'idle',
    processing,
    readySinceLastSelect: Boolean(chat.cliTerminal.readySinceLastSelect),
    active: lifecycleState === 'idle' && running,
    lastError: chat.cliTerminal.lastError ?? '',
  }

  return cliBindingsEquivalent(previous, next) ? previous! : next
}

function acpBindingFromCppChat(chat: CppChat, previous: AcpBinding | undefined): AcpBinding {
  const acp = chat.acpSession
  const running = Boolean(acp?.running)
  const processing = Boolean(acp?.processing)
  const lifecycleState = normalizeAcpLifecycleState(acp?.lifecycleState, running, processing)
  const effectiveProcessing =
    lifecycleState === 'error'
      ? false
      : processing ||
        lifecycleState === 'processing' ||
        lifecycleState === 'waitingPermission' ||
        lifecycleState === 'waitingUserInput'
  const next: AcpBinding = {
    sessionId: acp?.sessionId ?? '',
    providerId: acp?.providerId ?? chat.providerId ?? GEMINI_CLI_PROVIDER_ID,
    protocolKind: acp?.protocolKind ?? '',
    threadId: acp?.threadId ?? '',
    running,
    lifecycleState,
    processing: effectiveProcessing,
    readySinceLastSelect: Boolean(acp?.readySinceLastSelect),
    attentionKind: acp?.attentionKind ?? null,
    processingStartedAtMs: effectiveProcessing
      ? previous?.processing
        ? previous.processingStartedAtMs ?? Date.now()
        : Date.now()
      : null,
    lastError: acp?.lastError ?? '',
    recentStderr: acp?.recentStderr ?? '',
    lastExitCode: typeof acp?.lastExitCode === 'number' ? acp.lastExitCode : null,
    diagnostics: Array.isArray(acp?.diagnostics) ? acp!.diagnostics : [],
    toolCalls: Array.isArray(acp?.toolCalls) ? acp!.toolCalls : [],
    planSummary: acp?.planSummary ?? '',
    planEntries: Array.isArray(acp?.planEntries) ? acp!.planEntries : [],
    availableModes: Array.isArray(acp?.availableModes) ? acp!.availableModes : [],
    currentModeId: normalizeAcpApprovalMode(acp?.currentModeId ?? chat.approvalMode),
    availableModels: Array.isArray(acp?.availableModels) ? acp!.availableModels : [],
    currentModelId: normalizeAcpModelId(acp?.currentModelId ?? chat.modelId),
    turnEvents: Array.isArray(acp?.turnEvents) ? acp!.turnEvents : [],
    turnUserMessageIndex: typeof acp?.turnUserMessageIndex === 'number' ? acp.turnUserMessageIndex : -1,
    turnAssistantMessageIndex: typeof acp?.turnAssistantMessageIndex === 'number' ? acp.turnAssistantMessageIndex : -1,
    turnSerial: typeof acp?.turnSerial === 'number' ? acp.turnSerial : 0,
    waitIsStale: Boolean(acp?.waitIsStale),
    waitStaleReason: typeof acp?.waitStaleReason === 'string' ? acp.waitStaleReason : '',
    waitSeconds: typeof acp?.waitSeconds === 'number' ? acp.waitSeconds : 0,
    pendingPermission: acp?.pendingPermission ?? null,
    pendingUserInput: acp?.pendingUserInput ?? null,
    agentInfo: acp?.agentInfo
      ? {
          name: acp.agentInfo.name ?? '',
          title: acp.agentInfo.title ?? '',
          version: acp.agentInfo.version ?? '',
        }
      : null,
  }

  return acpBindingsEquivalent(previous, next) ? previous! : next
}

function normalizeCliTranscript(
  existing: CliTranscript | undefined,
  terminalId: string
): CliTranscript | undefined {
  if (!existing) {
    return undefined
  }

  if (existing.terminalId && terminalId && existing.terminalId !== terminalId) {
    return undefined
  }

  const nextTranscript: CliTranscript = {
    terminalId: terminalId || existing.terminalId || '',
    content: clampCliTranscript(existing.content),
  }

  if (
    existing.terminalId === nextTranscript.terminalId &&
    existing.content === nextTranscript.content
  ) {
    return existing
  }

  return nextTranscript
}

let pendingProviderChatDefaults: {
  requestId: string
  defaultNewChatProviderId: string
  providerChatDefaults: Record<string, ProviderChatDefaults>
} | null = null
let lastPushStatusUpdateAtMs = 0

function rememberPendingRequest(key: string, requestId: string) {
  pendingRequestIdsByKey.set(key, requestId)
}

function isLatestPendingRequest(key: string, requestId?: string) {
  return typeof requestId === 'string' && pendingRequestIdsByKey.get(key) === requestId
}

function sameRecordEntries<T>(existing: Record<string, T>, next: Record<string, T>) {
  const existingKeys = Object.keys(existing)
  const nextKeys = Object.keys(next)

  if (existingKeys.length !== nextKeys.length) {
    return false
  }

  for (const key of nextKeys) {
    if (!Object.prototype.hasOwnProperty.call(existing, key)) {
      return false
    }

    if (!Object.is(existing[key], next[key])) {
      return false
    }
  }

  return true
}

function sameArrayEntries<T>(existing: T[], next: T[]) {
  if (existing.length !== next.length) {
    return false
  }

  for (let i = 0; i < next.length; i++) {
    if (!Object.is(existing[i], next[i])) {
      return false
    }
  }

  return true
}

function cppMessageCreatedAtMillis(message: CppMessage) {
  if (!message.createdAt) {
    return Date.now()
  }

  const timestamp = Date.parse(message.createdAt)
  return Number.isFinite(timestamp) ? timestamp : Date.now()
}

function cppMessagesEquivalent(existing: Message, next: CppMessage) {
  return (
    existing.role === next.role &&
    existing.content === next.content &&
    (existing.thoughts ?? '') === (next.thoughts ?? '') &&
    (existing.planSummary ?? '') === (next.planSummary ?? '') &&
    planEntriesEquivalent(existing.planEntries ?? [], next.planEntries ?? []) &&
    toolCallsEquivalent(existing.toolCalls ?? [], next.toolCalls ?? []) &&
    messageBlocksEquivalent(existing.blocks ?? [], next.blocks ?? []) &&
    existing.createdAt.getTime() === cppMessageCreatedAtMillis(next)
  )
}

function buildMessageFromCpp(chatId: string, message: CppMessage, index: number): Message {
  const createdAtMillis = cppMessageCreatedAtMillis(message)
  return {
    id: `cef-m-${chatId}-${createdAtMillis}-${index}-${message.role}`,
    sessionId: chatId,
    role: message.role,
    content: message.content,
    thoughts: message.thoughts ?? '',
    planSummary: message.planSummary ?? '',
    planEntries: message.planEntries ?? [],
    toolCalls: message.toolCalls ?? [],
    blocks: message.blocks ?? [],
    attachments: messageAttachments(message),
    createdAt: new Date(createdAtMillis),
  }
}

function reconcileCppMessages(
  chatId: string,
  existingMessages: Message[] | undefined,
  cppMessages: CppMessage[]
): Message[] {
  const existing = existingMessages ?? []
  const existingRealMessages = existing.filter((message) => !message.isStreaming)
  const hasStreamingPlaceholder = existing.some((message) => message.isStreaming)

  if (hasStreamingPlaceholder && cppMessages.length <= existingRealMessages.length) {
    return existing
  }

  let prefixLength = 0
  while (
    prefixLength < existingRealMessages.length &&
    prefixLength < cppMessages.length &&
    cppMessagesEquivalent(existingRealMessages[prefixLength], cppMessages[prefixLength])
  ) {
    prefixLength++
  }

  if (!hasStreamingPlaceholder && prefixLength === existingRealMessages.length && prefixLength === cppMessages.length) {
    return existing
  }

  const reconciled = existingRealMessages.slice(0, prefixLength)
  for (let index = prefixLength; index < cppMessages.length; index += 1) {
    reconciled.push(buildMessageFromCpp(chatId, cppMessages[index], index))
  }
  return reconciled
}

function planEntriesEquivalent(existing: AcpPlanEntry[], next: AcpPlanEntry[]) {
  if (existing.length !== next.length) return false
  return existing.every((entry, index) => {
    const other = next[index]
    return entry.content === other.content && entry.priority === other.priority && entry.status === other.status
  })
}

function toolCallsEquivalent(existing: AcpToolCall[], next: AcpToolCall[]) {
  if (existing.length !== next.length) return false
  return existing.every((tool, index) => {
    const other = next[index]
    return (
      tool.id === other.id &&
      tool.title === other.title &&
      tool.kind === other.kind &&
      tool.status === other.status &&
      tool.content === other.content &&
      Boolean(tool.isSubAgent) === Boolean(other.isSubAgent) &&
      (tool.subAgentId ?? '') === (other.subAgentId ?? '') &&
      (tool.subAgentTitle ?? '') === (other.subAgentTitle ?? '')
    )
  })
}

function messageBlocksEquivalent(existing: MessageBlock[], next: MessageBlock[]) {
  if (existing.length !== next.length) return false
  return existing.every((block, index) => {
    const other = next[index]
    return (
      block.type === other.type &&
      (block.text ?? '') === (other.text ?? '') &&
      (block.toolCallId ?? '') === (other.toolCallId ?? '') &&
      (block.requestId ?? '') === (other.requestId ?? '')
    )
  })
}

function clearPendingRequest(key: string, requestId?: string) {
  if (typeof requestId === 'string' && pendingRequestIdsByKey.get(key) === requestId) {
    pendingRequestIdsByKey.delete(key)
  }
}

function clearPendingCodexOptions(chatId: string, requestId?: string) {
  const pending = pendingCodexOptionsByChatId.get(chatId)
  if (pending && pending.requestId === requestId) {
    pendingCodexOptionsByChatId.delete(chatId)
  }
}

function clearPendingProviderChatDefaults(requestId?: string) {
  if (pendingProviderChatDefaults && pendingProviderChatDefaults.requestId === requestId) {
    pendingProviderChatDefaults = null
  }
}

function applyPendingCodexOptions(sessions: Session[]): Session[] {
  if (pendingCodexOptionsByChatId.size === 0) return sessions
  let changed = false
  const nextSessions = sessions.map((session) => {
    const pending = pendingCodexOptionsByChatId.get(session.id)
    if (!pending || (session.providerId ?? GEMINI_CLI_PROVIDER_ID) !== CODEX_CLI_PROVIDER_ID) {
      return session
    }
    if ((session.reasoningEffort ?? '') === pending.reasoningEffort && (session.serviceTier ?? '') === pending.serviceTier) {
      return session
    }
    changed = true
    return {
      ...session,
      reasoningEffort: pending.reasoningEffort,
      serviceTier: pending.serviceTier,
    }
  })
  return changed ? nextSessions : sessions
}

function parseUamPushPayload(payload: unknown): ParsedPushResult {
  let raw: unknown = payload

  if (typeof raw === 'string') {
    try {
      raw = JSON.parse(raw)
    } catch (err) {
      const reason = err instanceof Error ? err.message : 'unknown parse failure'
      return { ok: false, status: 'parse-error', error: `JSON parse failed: ${reason}` }
    }
  }

  if (!isRecord(raw)) {
    return { ok: false, status: 'invalid-message', error: 'Payload is not an object.' }
  }

  const type = raw.type
  if (!isString(type) || type.trim().length === 0) {
    return { ok: false, status: 'invalid-message', error: 'Missing message type.' }
  }

  if (type === 'stateUpdate') {
    const sanitized = sanitizeCppAppState(raw.data)
    if (!sanitized) {
      return { ok: false, status: 'invalid-message', error: 'stateUpdate.data does not match CppAppState shape.' }
    }
    return { ok: true, message: { type, data: sanitized } }
  }

  if (type === 'statePatch') {
    const sanitized = sanitizeCppStatePatch(raw.data)
    if (!sanitized) {
      return { ok: false, status: 'invalid-message', error: 'statePatch.data does not match CppStatePatch shape.' }
    }
    return { ok: true, message: { type, data: sanitized } }
  }

  if (type === 'cliOutput') {
    if (!isString(raw.data)) {
      return { ok: false, status: 'invalid-message', error: 'cliOutput requires string data.' }
    }

    const sessionId = isString(raw.sessionId) ? raw.sessionId : isString(raw.chatId) ? raw.chatId : undefined
    const sourceChatId = isString(raw.sourceChatId) ? raw.sourceChatId : isString(raw.chatId) ? raw.chatId : undefined
    const terminalId = isString(raw.terminalId) ? raw.terminalId : undefined

    return {
      ok: true,
      message: { type, data: raw.data, sessionId, sourceChatId, terminalId },
    }
  }

  if (type === 'streamToken') {
    if (!isString(raw.chatId) || !isString(raw.token)) {
      return { ok: false, status: 'invalid-message', error: 'streamToken requires chatId and token.' }
    }
    return { ok: true, message: { type, chatId: raw.chatId, token: raw.token } }
  }

  if (type === 'streamDone') {
    if (!isString(raw.chatId)) {
      return { ok: false, status: 'invalid-message', error: 'streamDone requires chatId.' }
    }
    return { ok: true, message: { type, chatId: raw.chatId } }
  }

  return { ok: false, status: 'invalid-message', error: `Unsupported push message type: ${type}` }
}

// ---------------------------------------------------------------------------
// Deserialiser — convert C++ format → React store format
// ---------------------------------------------------------------------------

function deserializeState(
  cpp: CppAppState,
  existing: {
    sessions: Session[]
    folders: Folder[]
    messages: Record<string, Message[]>
    providers: Provider[]
    activeSessionId: string | null
    cliTranscriptBySessionId: Record<string, CliTranscript>
    cliBindingBySessionId: Record<string, CliBinding>
    acpBindingBySessionId: Record<string, AcpBinding>
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
  }
) {
  // Build lookup maps for reference-identity preservation
  const existingSessionsById = Object.fromEntries(existing.sessions.map((s) => [s.id, s]))
  const existingFoldersById = Object.fromEntries(existing.folders.map((f) => [f.id, f]))
  const existingProvidersById = Object.fromEntries(existing.providers.map((p) => [p.id, p]))

  const newFolders: Folder[] = cpp.folders.map((folder) =>
    folderFromCppFolder(folder, existingFoldersById[folder.id])
  )
  const folders: Folder[] = newFolders.length === existing.folders.length && newFolders.every((f) => f === existingFoldersById[f.id])
    ? existing.folders
    : newFolders

  const visibleProviders = cpp.providers.length > 0
    ? cpp.providers
    : initialProviders
  const newSessions: Session[] = cpp.chats.map((chat) =>
    sessionFromCppChat(chat, existingSessionsById[chat.id], visibleProviders)
  )
  // Reuse array reference if all elements are identical
  const sessions: Session[] = newSessions.length === existing.sessions.length && newSessions.every((s, i) => s === existing.sessions[i])
    ? existing.sessions
    : newSessions

  const nextMessages: Record<string, Message[]> = {}
  for (const chat of cpp.chats) {
    const existingMessages = existing.messages[chat.id]
    if (!Array.isArray(chat.messages)) {
      if (existingMessages?.length) {
        nextMessages[chat.id] = existingMessages
      }
      continue
    }

    nextMessages[chat.id] = reconcileCppMessages(chat.id, existingMessages, chat.messages)
  }
  const messages = sameRecordEntries(existing.messages, nextMessages) ? existing.messages : nextMessages

  const nextProviders: Provider[] = visibleProviders.map((provider) =>
    providerFromCppProvider(provider, existingProvidersById[provider.id])
  )
  const providers = sameArrayEntries(existing.providers, nextProviders) ? existing.providers : nextProviders

  const selectedByBackend =
    typeof cpp.selectedChatId === 'string' && sessions.some((s) => s.id === cpp.selectedChatId)
      ? cpp.selectedChatId
      : null
  const selectedFromCurrent =
    existing.activeSessionId && sessions.some((s) => s.id === existing.activeSessionId)
      ? existing.activeSessionId
      : null
  const effectiveActiveSessionId = selectedByBackend ?? selectedFromCurrent ?? sessions[0]?.id ?? null
  const existingBindings = existing.cliBindingBySessionId
  const nextCliBindingBySessionId = Object.fromEntries(
    cpp.chats.flatMap((chat) => {
      const binding = cliBindingFromCppChat(chat, existingBindings[chat.id])
      return binding ? [[chat.id, binding]] : []
    })
  ) as Record<string, CliBinding>
  const cliBindingBySessionId = sameRecordEntries(existingBindings, nextCliBindingBySessionId)
    ? existingBindings
    : nextCliBindingBySessionId

  const existingAcpBindings = existing.acpBindingBySessionId
  const nextAcpBindingBySessionId = Object.fromEntries(
    cpp.chats.map((chat) => [
      chat.id,
      acpBindingFromCppChat(chat, existingAcpBindings[chat.id]),
    ])
  ) as Record<string, AcpBinding>
  const acpBindingBySessionId = sameRecordEntries(existingAcpBindings, nextAcpBindingBySessionId)
    ? existingAcpBindings
    : nextAcpBindingBySessionId

  const nextCliTranscriptBySessionId = Object.fromEntries(
    sessions.flatMap((session) => {
      const transcript = normalizeCliTranscript(
        existing.cliTranscriptBySessionId[session.id],
        cliBindingBySessionId[session.id]?.terminalId ?? ''
      )

      return transcript ? [[session.id, transcript]] : []
    })
  ) as Record<string, CliTranscript>
  const cliTranscriptBySessionId = sameRecordEntries(
    existing.cliTranscriptBySessionId,
    nextCliTranscriptBySessionId
  )
    ? existing.cliTranscriptBySessionId
    : nextCliTranscriptBySessionId

  const nextCliDebugState = cpp.cliDebug ?? null
  const cliDebugState =
    cliDebugSignature(existing.cliDebugState) === cliDebugSignature(nextCliDebugState)
      ? existing.cliDebugState
      : nextCliDebugState

  const sessionsWithPendingCodexOptions = applyPendingCodexOptions(sessions)

  const goalsByChatId: Record<string, Goal[]> = {}
  const activeGoalIdByChatId: Record<string, string | null> = {}
  for (const chat of cpp.chats) {
    if (chat.activeGoalId !== undefined) {
      activeGoalIdByChatId[chat.id] = chat.activeGoalId
    }
    if (Array.isArray(chat.goals) && chat.goals.length > 0) {
      const goalObjects: Goal[] = chat.goals.map((cppGoal) => ({
        id: cppGoal.id,
        chatId: chat.id,
        objective: cppGoal.objective,
        status: cppGoal.status,
        tokenBudget: cppGoal.tokenBudget,
        tokensUsed: cppGoal.tokensUsed,
        blockedTurnCount: cppGoal.blockedTurnCount,
        lastBlocker: cppGoal.lastBlocker,
        completedItems: cppGoal.completedItems,
        remainingItems: cppGoal.remainingItems,
        currentStep: cppGoal.currentStep,
        lastVerification: cppGoal.lastVerification,
        lastNextPrompt: cppGoal.lastNextPrompt,
        sameNextPromptCount: cppGoal.sameNextPromptCount,
        loopCount: cppGoal.loopCount,
        createdAt: new Date(cppGoal.createdAt || Date.now()),
        updatedAt: new Date(cppGoal.updatedAt || Date.now()),
      }))
      goalsByChatId[chat.id] = goalObjects
    }
  }

  return {
    folders,
    sessions: sessionsWithPendingCodexOptions,
    messages,
    goalsByChatId,
    activeGoalIdByChatId,
    providers,
    activeSessionId: effectiveActiveSessionId,
    lastAppliedStateRevision: cppStateRevision(cpp),
    theme: cpp.settings.theme,
    cliBindingBySessionId,
    acpBindingBySessionId,
    cliTranscriptBySessionId,
    cliDebugState,
    memoryEnabledDefault: cpp.settings.memoryEnabledDefault,
    memoryIdleDelaySeconds: cpp.settings.memoryIdleDelaySeconds,
    memoryRecallBudgetBytes: cpp.settings.memoryRecallBudgetBytes,
    memoryLastStatus: cpp.settings.memoryLastStatus,
    memoryWorkerBindings: cpp.settings.memoryWorkerBindings,
    memoryActivity: cpp.memoryActivity ?? sanitizeMemoryActivity(undefined, cpp.settings.memoryLastStatus),
    cliVersionManager: cpp.cliVersionManager ?? existing.cliVersionManager,
    markdownStoreDirectory: cpp.settings.markdownStoreDirectory ?? '',
    defaultNewChatProviderId: pendingProviderChatDefaults?.defaultNewChatProviderId ?? cpp.settings.defaultNewChatProviderId ?? cpp.settings.activeProviderId ?? GEMINI_CLI_PROVIDER_ID,
    providerChatDefaults: pendingProviderChatDefaults?.providerChatDefaults ?? cpp.settings.providerChatDefaults ?? {},
    defaultEditorPresetId: cpp.settings.defaultEditorPresetId ?? 'vscode',
    editorFileAssociations: cpp.settings.editorFileAssociations ?? defaultEditorFileAssociations(),
  }
}

function applyStatePatch(patch: CppStatePatch, current: AppState): Partial<AppState> | null {
  const nextRevision = cppPatchRevision(patch)
  if (!isNewerStateRevision(nextRevision, current.lastAppliedStateRevision)) return null

  const removedChatIds = new Set(patch.removedChatIds ?? [])
  const existingFoldersById = Object.fromEntries(current.folders.map((folder) => [folder.id, folder]))
  const existingSessionsById = Object.fromEntries(current.sessions.map((session) => [session.id, session]))
  const existingProvidersById = Object.fromEntries(current.providers.map((provider) => [provider.id, provider]))

  const folders = patch.folders
    ? patch.folders.map((folder) => folderFromCppFolder(folder, existingFoldersById[folder.id]))
    : current.folders

  const providers = patch.providers
    ? patch.providers.map((provider) => providerFromCppProvider(provider, existingProvidersById[provider.id]))
    : current.providers
  const visibleProviders = providers.length > 0
    ? providers
    : [{ id: GEMINI_CLI_PROVIDER_ID }]

  const patchedSessionsById = new Map(current.sessions.filter((session) => !removedChatIds.has(session.id)).map((session) => [session.id, session]))
  for (const chat of patch.chats ?? []) {
    patchedSessionsById.set(chat.id, sessionFromCppChat(chat, existingSessionsById[chat.id], visibleProviders))
  }
  let sessions = current.sessions
    .filter((session) => patchedSessionsById.has(session.id))
    .map((session) => patchedSessionsById.get(session.id)!)
  for (const chat of patch.chats ?? []) {
    if (!current.sessions.some((session) => session.id === chat.id)) {
      sessions.push(patchedSessionsById.get(chat.id)!)
    }
  }
  if (patch.chatOrder) {
    const order = new Map(patch.chatOrder.map((chatId, index) => [chatId, index]))
    sessions = sessions
      .filter((session) => order.has(session.id))
      .sort((a, b) => (order.get(a.id) ?? Number.MAX_SAFE_INTEGER) - (order.get(b.id) ?? Number.MAX_SAFE_INTEGER))
  }

  const messages = { ...current.messages }
  for (const chatId of removedChatIds) {
    delete messages[chatId]
  }
  for (const [chatId, cppMessages] of Object.entries(patch.messagesByChatId ?? {})) {
    messages[chatId] = reconcileCppMessages(chatId, current.messages[chatId], cppMessages)
  }

  const cliBindingBySessionId = { ...current.cliBindingBySessionId }
  const acpBindingBySessionId = { ...current.acpBindingBySessionId }
  const cliTranscriptBySessionId = { ...current.cliTranscriptBySessionId }
  for (const chatId of removedChatIds) {
    delete cliBindingBySessionId[chatId]
    delete acpBindingBySessionId[chatId]
    delete cliTranscriptBySessionId[chatId]
  }

  for (const chat of patch.chats ?? []) {
    const nextCli = cliBindingFromCppChat(chat, cliBindingBySessionId[chat.id])
    if (nextCli) {
      cliBindingBySessionId[chat.id] = nextCli
    }

    if (chat.acpSession) {
      acpBindingBySessionId[chat.id] = acpBindingFromCppChat(chat, acpBindingBySessionId[chat.id])
    }
  }

  // Patch goal data
  const goalsByChatId = { ...current.goalsByChatId }
  const activeGoalIdByChatId = { ...current.activeGoalIdByChatId }
  for (const chatId of removedChatIds) {
    delete goalsByChatId[chatId]
    delete activeGoalIdByChatId[chatId]
  }
  for (const chat of patch.chats ?? []) {
    if (chat.activeGoalId !== undefined) {
      activeGoalIdByChatId[chat.id] = chat.activeGoalId
    }
    if (Array.isArray(chat.goals)) {
      goalsByChatId[chat.id] = chat.goals.map((cppGoal) => ({
        id: cppGoal.id,
        chatId: chat.id,
        objective: cppGoal.objective,
        status: cppGoal.status,
        tokenBudget: cppGoal.tokenBudget,
        tokensUsed: cppGoal.tokensUsed,
        blockedTurnCount: cppGoal.blockedTurnCount,
        lastBlocker: cppGoal.lastBlocker,
        completedItems: cppGoal.completedItems,
        remainingItems: cppGoal.remainingItems,
        currentStep: cppGoal.currentStep,
        lastVerification: cppGoal.lastVerification,
        lastNextPrompt: cppGoal.lastNextPrompt,
        sameNextPromptCount: cppGoal.sameNextPromptCount,
        loopCount: cppGoal.loopCount,
        createdAt: new Date(cppGoal.createdAt || Date.now()),
        updatedAt: new Date(cppGoal.updatedAt || Date.now()),
      }))
    }
  }

  const activeSessionId =
    patch.selectedChatId !== undefined
      ? patch.selectedChatId && sessions.some((session) => session.id === patch.selectedChatId)
        ? patch.selectedChatId
        : sessions[0]?.id ?? null
      : current.activeSessionId && sessions.some((session) => session.id === current.activeSessionId)
        ? current.activeSessionId
        : sessions[0]?.id ?? null

  const sessionsWithPendingCodexOptions = applyPendingCodexOptions(sessions)

  return {
    folders,
    sessions: sessionsWithPendingCodexOptions,
    messages: sameRecordEntries(current.messages, messages) ? current.messages : messages,
    goalsByChatId,
    activeGoalIdByChatId,
    providers,
    activeSessionId,
    lastAppliedStateRevision: nextRevision,
    theme: patch.settings?.theme ?? current.theme,
    cliBindingBySessionId: sameRecordEntries(current.cliBindingBySessionId, cliBindingBySessionId) ? current.cliBindingBySessionId : cliBindingBySessionId,
    acpBindingBySessionId: sameRecordEntries(current.acpBindingBySessionId, acpBindingBySessionId) ? current.acpBindingBySessionId : acpBindingBySessionId,
    cliTranscriptBySessionId: sameRecordEntries(current.cliTranscriptBySessionId, cliTranscriptBySessionId) ? current.cliTranscriptBySessionId : cliTranscriptBySessionId,
    memoryEnabledDefault: patch.settings?.memoryEnabledDefault ?? current.memoryEnabledDefault,
    memoryIdleDelaySeconds: patch.settings?.memoryIdleDelaySeconds ?? current.memoryIdleDelaySeconds,
    memoryRecallBudgetBytes: patch.settings?.memoryRecallBudgetBytes ?? current.memoryRecallBudgetBytes,
    memoryLastStatus: patch.settings?.memoryLastStatus ?? current.memoryLastStatus,
    memoryWorkerBindings: patch.settings?.memoryWorkerBindings ?? current.memoryWorkerBindings,
    memoryActivity: patch.memoryActivity ?? current.memoryActivity,
    cliVersionManager: patch.cliVersionManager ?? current.cliVersionManager,
    markdownStoreDirectory: patch.settings?.markdownStoreDirectory ?? current.markdownStoreDirectory,
    defaultNewChatProviderId: pendingProviderChatDefaults?.defaultNewChatProviderId ?? patch.settings?.defaultNewChatProviderId ?? current.defaultNewChatProviderId,
    providerChatDefaults: pendingProviderChatDefaults?.providerChatDefaults ?? patch.settings?.providerChatDefaults ?? current.providerChatDefaults,
    defaultEditorPresetId: patch.settings?.defaultEditorPresetId ?? current.defaultEditorPresetId,
    editorFileAssociations: patch.settings?.editorFileAssociations ?? current.editorFileAssociations,
  }
}

function cliDebugSignature(debug: CppCliDebugState | null | undefined) {
  if (!debug) return 'none'
  // Intentionally excludes volatile timestamps to avoid churn on stateUpdate pushes.
  return JSON.stringify({
    selectedChatId: debug.selectedChatId,
    terminalCount: debug.terminalCount,
    runningTerminalCount: debug.runningTerminalCount,
    busyTerminalCount: debug.busyTerminalCount,
    terminals: debug.terminals.map((terminal) => ({
      terminalId: terminal.terminalId,
      frontendChatId: terminal.frontendChatId,
      sourceChatId: terminal.sourceChatId,
      attachedSessionId: terminal.attachedSessionId,
      providerId: terminal.providerId,
      nativeSessionId: terminal.nativeSessionId,
      processId: terminal.processId,
      running: terminal.running,
      uiAttached: terminal.uiAttached,
      turnState: terminal.turnState,
      lifecycleState: terminal.lifecycleState,
      inputReady: terminal.inputReady,
      generationInProgress: terminal.generationInProgress,
      lastError: terminal.lastError,
    })),
  })
}

function isNewerStateRevision(nextRevision: number, currentRevision: number) {
  return nextRevision > currentRevision
}

let sessionCounter = 10
let folderCounter = 10

function makeId(prefix: string, counter: number) {
  return `${prefix}-${counter}`
}

// ---------------------------------------------------------------------------
// Store interface
// ---------------------------------------------------------------------------

interface AppState {
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

// ---------------------------------------------------------------------------
// Store
// ---------------------------------------------------------------------------

function readDocumentTheme(): StoredTheme {
  const stored = readStoredTheme()
  if (stored) return stored
  if (typeof document === 'undefined') return 'dark'
  return normalizeStoredTheme(document.documentElement.getAttribute('data-theme')) ?? 'dark'
}

function persistTheme(theme: StoredTheme): void {
  applyDocumentTheme(theme)
  writeStoredTheme(theme)
}

const appShellLayoutStorageKey = 'uam-app-shell-layout-v2'
const legacyAppShellLayoutStorageKey = 'uam-app-shell-layout-v1'
const defaultAppShellLayout = {
  sidebarCollapsed: false,
  commitPanelOpen: false,
  sidebarWidthPx: 320,
  commitPanelWidthPx: 420,
}

function clampSidebarWidthPx(width: number): number {
  return Math.min(520, Math.max(260, Math.round(width)))
}

function clampCommitPanelWidthPx(width: number): number {
  return Math.min(680, Math.max(320, Math.round(width)))
}

function legacyCommitPanelPercentToPx(value: unknown): number {
  const legacyPercent = typeof value === 'number' && Number.isFinite(value) ? value : 30
  return clampCommitPanelWidthPx(legacyPercent * 14)
}

function readStoredAppShellLayout() {
  if (typeof window === 'undefined') {
    return defaultAppShellLayout
  }
  try {
    const raw = window.localStorage.getItem(appShellLayoutStorageKey)
    if (raw) {
      const parsed = JSON.parse(raw)
      return {
        sidebarCollapsed: Boolean(parsed.sidebarCollapsed),
        commitPanelOpen: Boolean(parsed.commitPanelOpen),
        sidebarWidthPx: clampSidebarWidthPx(Number(parsed.sidebarWidthPx) || defaultAppShellLayout.sidebarWidthPx),
        commitPanelWidthPx: clampCommitPanelWidthPx(Number(parsed.commitPanelWidthPx) || defaultAppShellLayout.commitPanelWidthPx),
      }
    }

    const legacyRaw = window.localStorage.getItem(legacyAppShellLayoutStorageKey)
    const parsed = legacyRaw ? JSON.parse(legacyRaw) : {}
    return {
      sidebarCollapsed: Boolean(parsed.sidebarCollapsed),
      commitPanelOpen: Boolean(parsed.commitPanelOpen),
      sidebarWidthPx: defaultAppShellLayout.sidebarWidthPx,
      commitPanelWidthPx: legacyCommitPanelPercentToPx(parsed.commitPanelWidth),
    }
  } catch {
    return defaultAppShellLayout
  }
}

function writeStoredAppShellLayout(layout: {
  sidebarCollapsed: boolean
  commitPanelOpen: boolean
  sidebarWidthPx: number
  commitPanelWidthPx: number
}) {
  if (typeof window === 'undefined') return
  try {
    window.localStorage.setItem(appShellLayoutStorageKey, JSON.stringify({
      sidebarCollapsed: layout.sidebarCollapsed,
      commitPanelOpen: layout.commitPanelOpen,
      sidebarWidthPx: clampSidebarWidthPx(layout.sidebarWidthPx),
      commitPanelWidthPx: clampCommitPanelWidthPx(layout.commitPanelWidthPx),
    }))
  } catch {
    // Ignore storage failures; the in-memory store still tracks the layout.
  }
}

export const useAppStore = create<AppState>((set, get) => {
  const flushPendingCliTranscriptChunks = () => {
    pushFlushTimers.cliTranscript = null
    if (pendingCliTranscriptChunksBySessionId.size === 0) return

    const entries = Array.from(pendingCliTranscriptChunksBySessionId.entries())
    pendingCliTranscriptChunksBySessionId.clear()

    set((state) => {
      let nextTranscripts = state.cliTranscriptBySessionId

      for (const [sessionId, pending] of entries) {
        const chunk = pending.chunks.join('')
        if (!chunk) continue

        if (nextTranscripts === state.cliTranscriptBySessionId) {
          nextTranscripts = { ...state.cliTranscriptBySessionId }
        }

        nextTranscripts[sessionId] = appendCliTranscriptChunk(
          nextTranscripts[sessionId],
          pending.terminalId,
          chunk
        )
      }

      return nextTranscripts === state.cliTranscriptBySessionId
        ? state
        : { cliTranscriptBySessionId: nextTranscripts }
    })
  }

  const scheduleCliTranscriptFlush = () => {
    if (typeof window === 'undefined' || pushFlushTimers.cliTranscript !== null) return
    pushFlushTimers.cliTranscript = window.setTimeout(flushPendingCliTranscriptChunks, CLI_TRANSCRIPT_FLUSH_DELAY_MS)
  }

  const flushPendingStreamTokens = () => {
    pushFlushTimers.streamToken = null
    if (pendingStreamTokensByChatId.size === 0) return

    const entries = Array.from(pendingStreamTokensByChatId.entries())
    pendingStreamTokensByChatId.clear()

    set((state) => {
      let nextMessages = state.messages

      for (const [chatId, token] of entries) {
        const existingMessages = nextMessages[chatId] ?? []
        const lastMessage = existingMessages[existingMessages.length - 1]
        if (!lastMessage || lastMessage.role !== 'assistant' || !lastMessage.isStreaming) continue

        const updatedMessages = [...existingMessages]
        updatedMessages[updatedMessages.length - 1] = {
          ...lastMessage,
          content: lastMessage.content + token,
        }
        if (nextMessages === state.messages) {
          nextMessages = { ...state.messages }
        }
        nextMessages[chatId] = updatedMessages
      }

      return nextMessages === state.messages ? state : { messages: nextMessages }
    })
  }

  const scheduleStreamTokenFlush = () => {
    if (typeof window === 'undefined' || pushFlushTimers.streamToken !== null) return
    pushFlushTimers.streamToken = window.setTimeout(flushPendingStreamTokens, STREAM_TOKEN_FLUSH_DELAY_MS)
  }

  const requestChatMessagesFromCef = (chatId: string) => {
    if (!isCefContext() || !chatId) return
    const current = get()
    const session = current.sessions.find((candidate) => candidate.id === chatId)
    if (!session) return

    const requestKey = `getChatMessages:${chatId}`
    const requestId = createRequestId('getChatMessages')
    rememberPendingRequest(requestKey, requestId)
    void sendToCEF<ChatMessagesResponse>({
      action: 'getChatMessages',
      payload: {
        chatId,
        messagesDigest: session.messagesDigest ?? '',
      },
      requestId,
    }).then((response) => {
      if (!isLatestPendingRequest(requestKey, response.requestId)) return
      clearPendingRequest(requestKey, response.requestId)
      if (!response.ok || !response.data) return

      const data = response.data
      if (data.chatId && data.chatId !== chatId) return
      if (get().activeSessionId !== chatId) return
      if (data.unchanged) return
      if (!Array.isArray(data.messages)) return

      set((state) => {
        if (state.activeSessionId !== chatId) return state
        const nextMessages = reconcileCppMessages(chatId, state.messages[chatId], data.messages ?? [])
        const messagesChanged = nextMessages !== state.messages[chatId]
        const nextDigest = data.messagesDigest ?? ''
        const sessions = nextDigest
          ? state.sessions.map((candidate) =>
              candidate.id === chatId && (candidate.messagesDigest ?? '') !== nextDigest
                ? { ...candidate, messagesDigest: nextDigest, messageCount: data.messages?.length ?? candidate.messageCount ?? 0 }
                : candidate
            )
          : state.sessions
        const sessionsChanged = sessions !== state.sessions && sessions.some((candidate, index) => candidate !== state.sessions[index])

        if (!messagesChanged && !sessionsChanged) return state
        return {
          ...(messagesChanged ? {
            messages: {
              ...state.messages,
              [chatId]: nextMessages,
            },
          } : {}),
          ...(sessionsChanged ? { sessions } : {}),
        }
      })
    })
  }

  // Bootstrap from CEF if available (non-blocking — state arrives via uamPush later too)
	  if (isCefContext()) {
	    sendToCEF<CppAppState>({ action: 'getInitialState' }).then((resp) => {
	      // resp.data is the raw CppAppState object. Sanitize before deserializing.
	      const sanitized = resp.ok ? sanitizeCppAppState(resp.data) : null
	      if (sanitized) {
	        const current = get()
	        const nextRevision = cppStateRevision(sanitized)
	        if (isNewerStateRevision(nextRevision, current.lastAppliedStateRevision)) {
	          const deserialized = deserializeState(sanitized, {
	            sessions: current.sessions,
	            folders: current.folders,
	            messages: current.messages,
	            providers: current.providers,
	            activeSessionId: current.activeSessionId,
	            cliTranscriptBySessionId: current.cliTranscriptBySessionId,
	            cliBindingBySessionId: current.cliBindingBySessionId,
	            acpBindingBySessionId: current.acpBindingBySessionId,
	            cliDebugState: current.cliDebugState,
	            memoryEnabledDefault: current.memoryEnabledDefault,
	            memoryIdleDelaySeconds: current.memoryIdleDelaySeconds,
	            memoryRecallBudgetBytes: current.memoryRecallBudgetBytes,
	            memoryLastStatus: current.memoryLastStatus,
	            memoryWorkerBindings: current.memoryWorkerBindings,
	            memoryActivity: current.memoryActivity,
	            cliVersionManager: current.cliVersionManager,
	            markdownStoreDirectory: current.markdownStoreDirectory,
	            defaultNewChatProviderId: current.defaultNewChatProviderId,
	            providerChatDefaults: current.providerChatDefaults,
	            defaultEditorPresetId: current.defaultEditorPresetId,
	            editorFileAssociations: current.editorFileAssociations,
	          })
          set(deserialized)
          // Sync theme to DOM
          if (deserialized.theme) {
            persistTheme(deserialized.theme)
          }
        }
      }
    })
  }

  // Register window.uamPush SYNCHRONOUSLY at module load time (before React mounts)
  // so that C++'s OnLoadEnd push is never missed.  Using getState() avoids stale closures.
  if (isCefContext() && typeof window !== 'undefined') {
    window.uamPush = (payload: unknown) => {
      const parsed = parseUamPushPayload(payload)
      if (!parsed.ok) {
        set((state) => ({
          pushChannelStatus: parsed.status,
          pushChannelError: parsed.error,
          lastPushAtMs: state.lastPushAtMs,
        }))
        console.error('[uamPush] Rejected payload:', parsed.error, payload)
        return
      }

      const now = Date.now()
      const currentPushState = get()
      if (
        currentPushState.pushChannelStatus !== 'connected' ||
        currentPushState.pushChannelError !== '' ||
        currentPushState.lastPushAtMs === null ||
        now - lastPushStatusUpdateAtMs >= 1000
      ) {
        lastPushStatusUpdateAtMs = now
        set({
          pushChannelStatus: 'connected',
          pushChannelError: '',
          lastPushAtMs: now,
        })
      }

      const store = get()
      const msg = parsed.message

      switch (msg.type) {
        case 'stateUpdate':
          // Drop buffered deltas: the full state already contains the streamed
          // text, so applying them afterwards would duplicate content.
          pendingStreamTokensByChatId.clear()
          store.loadFromCef(msg.data)
          break
        case 'statePatch':
          {
            // A patch that replaces a chat's messages carries the streamed text
            // authoritatively; buffered deltas for those chats must not be
            // re-applied on top.
            if (msg.data && typeof msg.data === 'object' && msg.data.messagesByChatId && typeof msg.data.messagesByChatId === 'object') {
              for (const chatId of Object.keys(msg.data.messagesByChatId)) {
                pendingStreamTokensByChatId.delete(chatId)
              }
            }
            const next = applyStatePatch(msg.data, get())
            if (next) {
              set(next)
              if (next.theme) {
                persistTheme(next.theme)
              }
            }
          }
          break
        case 'cliOutput':
          {
            const decodedData = decodeCliChunk(msg.data)
            const sessionId = msg.sessionId ?? msg.sourceChatId ?? ''

            if (sessionId) {
              const currentBinding = get().cliBindingBySessionId[sessionId]
              const terminalId = msg.terminalId ?? currentBinding?.terminalId ?? ''
              const boundChatId = msg.sourceChatId ?? currentBinding?.boundChatId ?? sessionId
              const existingPending = pendingCliTranscriptChunksBySessionId.get(sessionId)
              if (existingPending && existingPending.terminalId === terminalId) {
                existingPending.chunks.push(decodedData)
              } else {
                pendingCliTranscriptChunksBySessionId.set(sessionId, {
                  terminalId,
                  chunks: [decodedData],
                })
              }
              scheduleCliTranscriptFlush()

              if (
                currentBinding &&
                (currentBinding.terminalId !== terminalId ||
                  currentBinding.boundChatId !== boundChatId)
              ) {
                set((state) => ({
                  cliBindingBySessionId: {
                    ...state.cliBindingBySessionId,
                    [sessionId]: {
                      ...currentBinding,
                      terminalId,
                      boundChatId,
                    },
                  },
                }))
              }
            }

            window.dispatchEvent(
              new CustomEvent('uam-cli-output', {
                detail: {
                  sessionId: msg.sessionId ?? '',
                  sourceChatId: msg.sourceChatId ?? '',
                  terminalId: msg.terminalId ?? '',
                  data: decodedData,
                },
              })
            )
          }
          break
        case 'streamToken':
          {
            const { chatId, token } = msg
            const session = get().sessions.find((s) => s.id === chatId)
            if (!session) break

            pendingStreamTokensByChatId.set(chatId, (pendingStreamTokensByChatId.get(chatId) ?? '') + token)
            scheduleStreamTokenFlush()
          }
          break
        case 'streamDone':
          {
            const { chatId } = msg
            const session = get().sessions.find((s) => s.id === chatId)
            if (!session) break

            flushPendingStreamTokens()
            const existingMessages = get().messages[chatId] ?? []
            const lastMessage = existingMessages[existingMessages.length - 1]

            if (lastMessage && lastMessage.role === 'assistant' && lastMessage.isStreaming) {
              const updatedMessages = [...existingMessages]
              updatedMessages[updatedMessages.length - 1] = {
                ...lastMessage,
                isStreaming: false,
              }
              set((state) => ({
                messages: {
                  ...state.messages,
                  [chatId]: updatedMessages,
                },
              }))
            }
          }
          break
        }
      }
    }
  // In CEF context, start with empty state — real data arrives via getInitialState above.
  // In dev/browser mode, use mock data so the UI is immediately interactive.
  const inCef = isCefContext()
  const storedShellLayout = readStoredAppShellLayout()

  return {
    folders: inCef ? [] : initialFolders,
    sessions: inCef ? [] : initialSessions,
    activeSessionId: inCef ? null : 's1',
    lastAppliedStateRevision: -1,
    messages: {},
    goalsByChatId: {},
    activeGoalIdByChatId: {},
    goalModeByChatId: {},
    defaultGoalTokenBudgetByChatId: {},

    providers: inCef ? [] : initialProviders,
    cliBindingBySessionId: {},
    acpBindingBySessionId: {},
    cliTranscriptBySessionId: {},
    cliDebugState: null,
    memoryEnabledDefault: true,
    memoryIdleDelaySeconds: DEFAULT_MEMORY_IDLE_DELAY_SECONDS,
    memoryRecallBudgetBytes: DEFAULT_MEMORY_RECALL_BUDGET_BYTES,
    memoryLastStatus: '',
    memoryWorkerBindings: {},
    memoryActivity: { ...emptyMemoryActivity },
    cliVersionManager: { ...emptyCliVersionManager },
    markdownStoreDirectory: '',
    defaultNewChatProviderId: GEMINI_CLI_PROVIDER_ID,
    providerChatDefaults: {},
    defaultEditorPresetId: 'vscode',
    editorFileAssociations: defaultEditorFileAssociations(),

    theme: readDocumentTheme(),
    isNewChatModalOpen: false,
    newChatFolderId: null,
    isSettingsOpen: false,
    memoryLibraryScope: null,
    memoryLibraryEntries: [],
    memoryLibraryLoading: false,
    memoryLibraryError: '',
    isMemoryScanModalOpen: false,
    memoryScanCandidates: [],
    selectedMemoryScanChatIds: [],
    memoryScanLoading: false,
    memoryScanRunning: false,
    memoryScanError: '',
    isMarkdownStoreOpen: false,
    markdownStoreEntries: [],
    markdownStoreLoading: false,
    markdownStoreError: '',
    markdownStoreAttachedBySessionId: {},
    sidebarCollapsed: storedShellLayout.sidebarCollapsed,
    commitPanelOpen: storedShellLayout.commitPanelOpen,
    sidebarWidthPx: storedShellLayout.sidebarWidthPx,
    commitPanelWidthPx: storedShellLayout.commitPanelWidthPx,
    streamingMessageId: null,
    pushChannelStatus: inCef ? 'no-push-yet' : 'connected',
    pushChannelError: '',
    lastPushAtMs: null,
    uiBuildId: UI_RUNTIME_BUILD_MARKER,

    // ---- CEF bootstrap ----

	    loadFromCef: (cppState) => {
	      const sanitized = sanitizeCppAppState(cppState)
	      if (!sanitized) return
	      const current = get()
	      const nextRevision = cppStateRevision(sanitized)
	      if (!isNewerStateRevision(nextRevision, current.lastAppliedStateRevision)) return
	      const deserialized = deserializeState(sanitized, {
	        sessions: current.sessions,
        folders: current.folders,
        messages: current.messages,
        providers: current.providers,
        activeSessionId: current.activeSessionId,
        cliTranscriptBySessionId: current.cliTranscriptBySessionId,
        cliBindingBySessionId: current.cliBindingBySessionId,
        acpBindingBySessionId: current.acpBindingBySessionId,
        cliDebugState: current.cliDebugState,
        memoryEnabledDefault: current.memoryEnabledDefault,
        memoryIdleDelaySeconds: current.memoryIdleDelaySeconds,
        memoryRecallBudgetBytes: current.memoryRecallBudgetBytes,
        memoryLastStatus: current.memoryLastStatus,
        memoryWorkerBindings: current.memoryWorkerBindings,
        memoryActivity: current.memoryActivity,
        cliVersionManager: current.cliVersionManager,
        markdownStoreDirectory: current.markdownStoreDirectory,
        defaultNewChatProviderId: current.defaultNewChatProviderId,
        providerChatDefaults: current.providerChatDefaults,
        defaultEditorPresetId: current.defaultEditorPresetId,
        editorFileAssociations: current.editorFileAssociations,
      })
      set(deserialized)
      if (deserialized.theme) {
        persistTheme(deserialized.theme)
      }
    },

    // ---- Session actions ----

    setActiveSession: (id) => {
      if (isCefContext()) {
        const previousActiveSessionId = get().activeSessionId
        const requestKey = 'selectSession'
        const requestId = createRequestId('selectSession')
        rememberPendingRequest(requestKey, requestId)
        const openedAt = new Date()
        const previousSession = get().sessions.find((s) => s.id === id)
        set((state) => ({
          activeSessionId: id,
          sessions: state.sessions.map((s) =>
            s.id === id ? { ...s, lastOpenedAt: openedAt } : s
          ),
        }))
        sendToCEF({ action: 'selectSession', payload: { chatId: id }, requestId }).then((resp) => {
          if (resp.ok) {
            clearPendingRequest(requestKey, resp.requestId)
            requestChatMessagesFromCef(id)
            return
          }

          if (!isLatestPendingRequest(requestKey, resp.requestId)) {
            return
          }

          set((state) => ({
            activeSessionId: previousActiveSessionId,
            sessions: previousSession
              ? state.sessions.map((s) =>
                  s.id === id ? { ...s, lastOpenedAt: previousSession.lastOpenedAt } : s
                )
              : state.sessions,
          }))
          pendingRequestIdsByKey.delete(requestKey)
        })
        return
      }

      set((state) => {
        const openedAt = new Date()
        return {
          activeSessionId: id,
          sessions: state.sessions.map((s) =>
            s.id === id ? { ...s, lastOpenedAt: openedAt } : s
          ),
        }
      })
    },

    addSession: (name, folderId, providerId = GEMINI_CLI_PROVIDER_ID) => {
      const current = get()
      const selectedFolderId = folderId && current.folders.some((folder) => folder.id === folderId)
        ? folderId
        : null
      if (!selectedFolderId) {
        console.error('[UAM] createSession requires a workspace folder')
        return
      }
      const normalizedProviderId = normalizeCliProviderIdAlias(providerId)
      const normalizedDefaultProviderId = normalizeCliProviderIdAlias(current.defaultNewChatProviderId)
      const requestedProviderId = current.providers.some((provider) => provider.id === normalizedProviderId)
        ? normalizedProviderId
        : current.providers.some((provider) => provider.id === normalizedDefaultProviderId)
          ? normalizedDefaultProviderId
          : GEMINI_CLI_PROVIDER_ID
      const defaults = providerChatDefaultsForNewChat(current, requestedProviderId)

      if (isCefContext()) {
        sendToCEF({
          action: 'createSession',
          payload: { title: name, folderId: selectedFolderId, providerId: requestedProviderId, defaults },
        }).then((resp) => {
          if (!resp.ok) {
            console.error('[CEF] createSession failed:', resp.error)
            return
          }

          set({ isNewChatModalOpen: false, newChatFolderId: null })
        })
        return
      }

      // Dev/mock path
      sessionCounter++
      const id = makeId('s', sessionCounter)
      const now = new Date()
      const session: Session = {
        id,
        name,
        viewMode: 'chat',
        folderId: selectedFolderId,
        providerId: requestedProviderId,
        modelId: defaults.modelId,
        reasoningEffort: defaults.reasoningEffort,
        serviceTier: defaults.serviceTier,
        approvalMode: defaults.approvalMode,
        autoApproveCommands: defaults.autoApproveCommands,
        memoryEnabled: defaults.memoryEnabled,
        createdAt: now,
        updatedAt: now,
        lastOpenedAt: now,
      }
      set((state) => ({
        sessions: [...state.sessions, session],
        messages: { ...state.messages, [id]: [] },
        activeSessionId: id,
        isNewChatModalOpen: false,
        newChatFolderId: null,
      }))
    },

    openSessionWorkspace: async (id) => {
      if (isCefContext()) {
        const response = await sendToCEF({
          action: 'openWorkspaceDirectory',
          payload: { chatId: id },
        })

        if (!response.ok) {
          console.error('[CEF] openWorkspaceDirectory failed:', response.error)
          return false
        }

        return true
      }

      const session = get().sessions.find((candidate) => candidate.id === id)
      const folderDirectory = session?.folderId
        ? get().folders.find((folder) => folder.id === session.folderId)?.directory ?? ''
        : ''
      return Boolean(session?.workspaceDirectory?.trim() || folderDirectory.trim())
    },

    openSessionWorkspaceEditor: async (id) => {
      if (isCefContext()) {
        const response = await sendToCEF<OpenWorkspaceEditorResponse>({
          action: 'openWorkspaceEditor',
          payload: { chatId: id },
        })

        if (!response.ok) {
          console.error('[CEF] openWorkspaceEditor failed:', response.error)
          return false
        }

        return true
      }

      const session = get().sessions.find((candidate) => candidate.id === id)
      const folderDirectory = session?.folderId
        ? get().folders.find((folder) => folder.id === session.folderId)?.directory ?? ''
        : ''
      return Boolean(session?.workspaceDirectory?.trim() || folderDirectory.trim())
    },

    openSessionTerminal: async (id) => {
      if (isCefContext()) {
        const response = await sendToCEF({
          action: 'openWorkspaceTerminal',
          payload: { chatId: id },
        })

        if (!response.ok) {
          console.error('[CEF] openWorkspaceTerminal failed:', response.error)
          return false
        }

        return true
      }

      const session = get().sessions.find((candidate) => candidate.id === id)
      const folderDirectory = session?.folderId
        ? get().folders.find((folder) => folder.id === session.folderId)?.directory ?? ''
        : ''
      return Boolean(session?.workspaceDirectory?.trim() || folderDirectory.trim())
    },

    openSubAgentSession: async (sourceChatId, nativeSessionId, title = '') => {
      if (isCefContext()) {
        const response = await sendToCEF({
          action: 'openNativeSessionChat',
          payload: {
            chatId: sourceChatId,
            nativeSessionId,
            title,
          },
        })

        if (!response.ok) {
          console.error('[CEF] openNativeSessionChat failed:', response.error)
          return false
        }

        return true
      }

      const sourceChat = get().sessions.find((candidate) => candidate.id === sourceChatId)
      return Boolean(sourceChat && nativeSessionId.trim())
    },

    getChatWorktreeStatus: async (id) => {
      if (isCefContext()) {
        const response = await sendToCEF<GitWorktreeStatus>({
          action: 'getChatWorktreeStatus',
          payload: { chatId: id },
        })
        if (!response.ok) {
          console.error('[CEF] getChatWorktreeStatus failed:', response.error)
          return null
        }
        return sanitizeGitWorktreeStatus(response.data)
      }

      const session = get().sessions.find((candidate) => candidate.id === id)
      return {
        isGitRepository: true,
        isSvnWorkspace: false,
        isolated: session?.workspaceIsolationKind === 'gitWorktree',
        sourceDirty: false,
        worktreeDirty: false,
        worktreeMissing: false,
        sourceDirectory: session?.workspaceSourceDirectory ?? session?.workspaceDirectory ?? '',
        worktreeDirectory: session?.workspaceWorktreeDirectory ?? '',
        branchName: session?.workspaceBranchName ?? '',
        baseRef: session?.workspaceBaseRef ?? '',
        warning: '',
        error: '',
      }
    },

    createChatWorktree: async (id) => {
      if (isCefContext()) {
        const response = await sendToCEF<GitWorktreeResult>({
          action: 'createChatWorktree',
          payload: { chatId: id },
        })
        if (!response.ok) {
          console.error('[CEF] createChatWorktree failed:', response.error)
          return failedGitWorktreeResult(response.error || 'Failed to create isolated Git worktree.')
        }
        return sanitizeGitWorktreeResult(cefPayloadOrRawResponse(response))
      }

      return failedGitWorktreeResult('Git worktree actions require the desktop runtime.')
    },

    discardChatWorktreeChanges: async (id) => {
      if (isCefContext()) {
        const response = await sendToCEF<GitWorktreeResult>({
          action: 'discardChatWorktreeChanges',
          payload: { chatId: id },
        })
        if (!response.ok) {
          console.error('[CEF] discardChatWorktreeChanges failed:', response.error)
          return failedGitWorktreeResult(response.error || 'Failed to discard worktree changes.')
        }
        return sanitizeGitWorktreeResult(cefPayloadOrRawResponse(response))
      }

      return failedGitWorktreeResult('Git worktree actions require the desktop runtime.')
    },

    portChatWorktreeChanges: async (id) => {
      if (isCefContext()) {
        const response = await sendToCEF<GitWorktreeResult>({
          action: 'portChatWorktreeChanges',
          payload: { chatId: id },
        })
        if (!response.ok) {
          console.error('[CEF] portChatWorktreeChanges failed:', response.error)
          return failedGitWorktreeResult(response.error || 'Failed to port worktree changes.')
        }
        return sanitizeGitWorktreeResult(cefPayloadOrRawResponse(response))
      }

      return failedGitWorktreeResult('Git worktree actions require the desktop runtime.')
    },

    getVcsCommitStatus: async (id, vcsType = 'git', options = {}) => {
      if (isCefContext()) {
        const response = await sendToCEF<VcsCommitStatus>({
          action: 'getVcsCommitStatus',
          payload: {
            chatId: id,
            vcsType,
            includeLineStats: options.includeLineStats ?? true,
            requestId: options.requestId,
          },
        })
        if (!response.ok) {
          console.error('[CEF] getVcsCommitStatus failed:', response.error)
          return null
        }
        return sanitizeVcsCommitStatus(response.data)
      }

      const session = get().sessions.find((candidate) => candidate.id === id)
      if (!session?.workspaceDirectory?.trim()) {
        return {
          available: false,
          vcsTypes: [],
          activeVcsType: 'git',
          workspaceDirectory: '',
          branchOrRevision: '',
          changedFiles: [],
          lineStatsReady: true,
          warning: 'No Git or SVN repository detected for this workspace.',
          error: '',
        }
      }
      return {
        available: true,
        vcsTypes: ['git'],
        activeVcsType: vcsType,
        workspaceDirectory: session.workspaceDirectory,
        branchOrRevision: 'main',
        changedFiles: [
          { path: 'src/example.ts', status: ' M', staged: false, additions: 12, deletions: 3, binary: false },
          { path: 'README.md', status: '??', staged: false, additions: 8, deletions: 0, binary: false },
        ],
        lineStatsReady: true,
        warning: '',
        error: '',
      }
    },

    getVcsFileDiff: async (id, path, vcsType) => {
      if (isCefContext()) {
        const response = await sendToCEF<VcsFileDiffResponse>({
          action: 'getVcsFileDiff',
          payload: { chatId: id, path, vcsType },
        })
        if (!response.ok) {
          console.error('[CEF] getVcsFileDiff failed:', response.error)
          return response.error || ''
        }
        return typeof response.data?.diff === 'string' ? response.data.diff : ''
      }

      return `diff -- ${path}\n`
    },

    commitVcsChanges: async (id, vcsType, message, files) => {
      if (isCefContext()) {
        const response = await sendToCEF<VcsCommitResult>({
          action: 'commitVcsChanges',
          payload: { chatId: id, vcsType, message, files },
        })
        if (!response.ok) {
          console.error('[CEF] commitVcsChanges failed:', response.error)
          return { ok: false, message: '', error: response.error || 'Failed to commit changes.' }
        }
        return sanitizeVcsCommitResult(cefPayloadOrRawResponse(response))
      }

      return {
        ok: true,
        message: `${vcsType.toUpperCase()} commit created for ${files.length} file${files.length === 1 ? '' : 's'}.`,
        error: '',
        status: await get().getVcsCommitStatus(id, vcsType) ?? undefined,
      }
    },

    generateVcsCommitMessage: async (id, vcsType, files) => {
      if (isCefContext()) {
        const response = await sendToCEF<VcsCommitMessageSuggestion>({
          action: 'generateVcsCommitMessage',
          payload: { chatId: id, vcsType, files },
        })
        if (!response.ok) {
          console.error('[CEF] generateVcsCommitMessage failed:', response.error)
          return null
        }
        if (!isRecord(response.data)) {
          return null
        }
        const title = stringOr(response.data.title)
        return title ? { title, description: stringOr(response.data.description) } : null
      }

      const status = await get().getVcsCommitStatus(id, vcsType)
      const selected = status?.changedFiles.filter((file) => files.includes(file.path)) ?? []
      if (selected.length === 0) return null
      const primary = selected[0]?.path.split(/[\\/]/).pop() || 'files'
      return {
        title: `Update ${primary}`,
        description: selected.map((file) => `- ${file.status.trim() || 'Changed'} ${file.path}`).join('\n'),
      }
    },

    renameSession: (id, name) => {
      if (isCefContext()) {
        const previousSession = get().sessions.find((s) => s.id === id)
        if (!previousSession) {
          return
        }

        const requestKey = `renameSession:${id}`
        const requestId = createRequestId('renameSession')
        rememberPendingRequest(requestKey, requestId)
        set((state) => ({
          sessions: state.sessions.map((s) =>
            s.id === id ? { ...s, name, updatedAt: new Date() } : s
          ),
        }))
        sendToCEF({ action: 'renameSession', payload: { chatId: id, title: name }, requestId }).then(
          (resp) => {
            if (resp.ok) {
              clearPendingRequest(requestKey, resp.requestId)
              return
            }

            if (!isLatestPendingRequest(requestKey, resp.requestId)) {
              return
            }

            set((state) => ({
              sessions: state.sessions.map((s) => (s.id === id ? previousSession : s)),
            }))
            pendingRequestIdsByKey.delete(requestKey)
          }
        )
        return
      }

      set((state) => ({
        sessions: state.sessions.map((s) =>
          s.id === id ? { ...s, name, updatedAt: new Date() } : s
        ),
      }))
    },

    setSessionPinned: async (id, pinned) => {
      const previousSession = get().sessions.find((s) => s.id === id)
      if (!previousSession) {
        return false
      }

      if ((previousSession.isPinned ?? false) === pinned) {
        return true
      }

      const applyPinned = () => {
        set((state) => ({
          sessions: state.sessions.map((s) =>
            s.id === id ? { ...s, isPinned: pinned } : s
          ),
        }))
      }

      if (isCefContext()) {
        const requestKey = `setSessionPinned:${id}`
        const requestId = createRequestId('setSessionPinned')
        rememberPendingRequest(requestKey, requestId)
        applyPinned()
        const response = await sendToCEF({
          action: 'setChatPinned',
          payload: { chatId: id, pinned },
          requestId,
        })

        if (response.ok) {
          clearPendingRequest(requestKey, response.requestId)
          return true
        }

        if (isLatestPendingRequest(requestKey, response.requestId)) {
          set((state) => ({
            sessions: state.sessions.map((s) => (s.id === id ? previousSession : s)),
          }))
          pendingRequestIdsByKey.delete(requestKey)
        }

        return false
      }

      applyPinned()
      return true
    },

	    setSessionProvider: async (id, providerId) => {
	      const requestedProviderId = normalizeCliProviderIdAlias(providerId)
	      const current = get()
	      if (!requestedProviderId || !current.providers.some((provider) => provider.id === requestedProviderId)) {
	        return false
	      }

	      const previousSession = current.sessions.find((s) => s.id === id)
	      if (!previousSession) {
	        return false
	      }

	      if ((previousSession.providerId ?? GEMINI_CLI_PROVIDER_ID) === requestedProviderId) {
	        return true
	      }

	      const acp = current.acpBindingBySessionId[id]
	      const messages = current.messages[id] ?? []
	      if (messages.length > 0 || acp?.running || acp?.processing) {
	        return false
	      }
	      const defaults = current.providerChatDefaults[requestedProviderId] ?? sanitizeProviderChatDefaults(null)

	      const applyProvider = () => {
	        set((state) => ({
	          sessions: state.sessions.map((s) =>
	            s.id === id ? {
                ...s,
                providerId: requestedProviderId,
                modelId: defaults.modelId,
                reasoningEffort: requestedProviderId === CODEX_CLI_PROVIDER_ID ? defaults.reasoningEffort : '',
                serviceTier: requestedProviderId === CODEX_CLI_PROVIDER_ID ? defaults.serviceTier : '',
                approvalMode: defaults.approvalMode,
                autoApproveCommands: defaults.autoApproveCommands,
                memoryEnabled: defaults.memoryEnabled,
                updatedAt: new Date(),
              } : s
	          ),
	        }))
	      }

	      if (isCefContext()) {
	        const requestKey = `setSessionProvider:${id}`
	        const requestId = createRequestId('setSessionProvider')
	        rememberPendingRequest(requestKey, requestId)
	        applyProvider()
	        const response = await sendToCEF({
	          action: 'setChatProvider',
	          payload: { chatId: id, providerId: requestedProviderId },
	          requestId,
	        })

	        if (response.ok) {
	          clearPendingRequest(requestKey, response.requestId)
	          return true
	        }

	        if (isLatestPendingRequest(requestKey, response.requestId)) {
	          set((state) => ({
	            sessions: state.sessions.map((s) => (s.id === id ? previousSession : s)),
	          }))
	          pendingRequestIdsByKey.delete(requestKey)
	        }

	        return false
	      }

	      applyProvider()
	      return true
	    },

	    setSessionModel: async (id, modelId) => {
	      const requestedModelId = modelId.trim()
	      if (!isAllowedAcpModelId(requestedModelId)) {
	        return false
	      }

      const previousSession = get().sessions.find((s) => s.id === id)
      if (!previousSession) {
        return false
      }

      if ((previousSession.modelId ?? '') === requestedModelId) {
        return true
      }

      const applyModel = () => {
        set((state) => ({
          sessions: state.sessions.map((s) =>
            s.id === id ? { ...s, modelId: requestedModelId, updatedAt: new Date() } : s
          ),
        }))
      }

      if (isCefContext()) {
        const requestKey = `setSessionModel:${id}`
        const requestId = createRequestId('setSessionModel')
        rememberPendingRequest(requestKey, requestId)
        applyModel()
        const response = await sendToCEF({
          action: 'setChatModel',
          payload: { chatId: id, modelId: requestedModelId },
          requestId,
        })

        if (response.ok) {
          clearPendingRequest(requestKey, response.requestId)
          return true
        }

        if (isLatestPendingRequest(requestKey, response.requestId)) {
          set((state) => ({
            sessions: state.sessions.map((s) => (s.id === id ? previousSession : s)),
          }))
          pendingRequestIdsByKey.delete(requestKey)
        }

        return false
      }

	      applyModel()
	      return true
	    },

	    setSessionCodexOptions: async (id, options) => {
	      const requestedReasoningEffort = normalizeCodexReasoningEffort(options.reasoningEffort)
	      const requestedServiceTier = normalizeCodexServiceTier(options.serviceTier)
	      const previousSession = get().sessions.find((s) => s.id === id)
	      if (!previousSession || (previousSession.providerId ?? GEMINI_CLI_PROVIDER_ID) !== CODEX_CLI_PROVIDER_ID) {
	        return false
	      }
	      if ((previousSession.reasoningEffort ?? '') === requestedReasoningEffort && (previousSession.serviceTier ?? '') === requestedServiceTier) {
	        return true
	      }

	      const applyOptions = () => {
	        set((state) => ({
	          sessions: state.sessions.map((s) =>
	            s.id === id ? { ...s, reasoningEffort: requestedReasoningEffort, serviceTier: requestedServiceTier, updatedAt: new Date() } : s
	          ),
	        }))
	      }

	      if (isCefContext()) {
	        const requestKey = `setSessionCodexOptions:${id}`
	        const requestId = createRequestId('setSessionCodexOptions')
	        rememberPendingRequest(requestKey, requestId)
	        pendingCodexOptionsByChatId.set(id, {
	          requestId,
	          reasoningEffort: requestedReasoningEffort,
	          serviceTier: requestedServiceTier,
	        })
	        applyOptions()
	        const response = await sendToCEF({
	          action: 'setChatCodexOptions',
	          payload: { chatId: id, reasoningEffort: requestedReasoningEffort, serviceTier: requestedServiceTier },
	          requestId,
	        })

	        if (response.ok) {
	          clearPendingRequest(requestKey, response.requestId)
	          clearPendingCodexOptions(id, response.requestId)
	          return true
	        }

	        if (isLatestPendingRequest(requestKey, response.requestId)) {
	          set((state) => ({
	            sessions: state.sessions.map((s) => (s.id === id ? previousSession : s)),
	          }))
	          pendingRequestIdsByKey.delete(requestKey)
	          clearPendingCodexOptions(id, response.requestId)
	        }
	        return false
	      }

	      applyOptions()
	      return true
	    },

	    setSessionApprovalMode: async (id, modeId) => {
	      const requestedModeId = modeId.trim() || 'default'
	      if (!(ACP_APPROVAL_MODE_IDS as readonly string[]).includes(requestedModeId)) {
	        return false
	      }

	      const previousSession = get().sessions.find((s) => s.id === id)
	      if (!previousSession) {
	        return false
	      }

	      const previousBinding = get().acpBindingBySessionId[id]
	      const previousSessionModeId = normalizeAcpApprovalMode(previousSession.approvalMode)
	      const previousRuntimeModeId = previousBinding ? normalizeAcpApprovalMode(previousBinding.currentModeId) : previousSessionModeId
	      if (previousSessionModeId === requestedModeId && previousRuntimeModeId === requestedModeId) {
	        return true
	      }

	      const applyMode = () => {
	        set((state) => ({
	          sessions: state.sessions.map((s) =>
	            s.id === id ? { ...s, approvalMode: requestedModeId, updatedAt: new Date() } : s
	          ),
	          acpBindingBySessionId: state.acpBindingBySessionId[id]
	            ? {
	                ...state.acpBindingBySessionId,
	                [id]: {
	                  ...state.acpBindingBySessionId[id],
	                  currentModeId: requestedModeId,
	                },
	              }
	            : state.acpBindingBySessionId,
	        }))
	      }

	      if (isCefContext()) {
	        const requestKey = `setSessionApprovalMode:${id}`
	        const requestId = createRequestId('setSessionApprovalMode')
	        rememberPendingRequest(requestKey, requestId)
	        applyMode()
	        const response = await sendToCEF({
	          action: 'setChatApprovalMode',
	          payload: { chatId: id, modeId: requestedModeId },
	          requestId,
	        })

	        if (response.ok) {
	          clearPendingRequest(requestKey, response.requestId)
	          return true
	        }

	        if (isLatestPendingRequest(requestKey, response.requestId)) {
	          set((state) => ({
	            sessions: state.sessions.map((s) => (s.id === id ? previousSession : s)),
	            acpBindingBySessionId: previousBinding
	              ? {
	                  ...state.acpBindingBySessionId,
	                  [id]: previousBinding,
	                }
	              : state.acpBindingBySessionId,
	          }))
	          pendingRequestIdsByKey.delete(requestKey)
	        }

	        return false
	      }

	      applyMode()
	      return true
	    },

	    setSessionAutoApproveCommands: async (id, enabled) => {
	      const previousSession = get().sessions.find((s) => s.id === id)
	      if (!previousSession) {
	        return false
	      }
	      if ((previousSession.autoApproveCommands ?? false) === enabled) {
	        return true
	      }

	      const applyAutoApprove = () => {
	        set((state) => ({
	          sessions: state.sessions.map((s) =>
	            s.id === id ? { ...s, autoApproveCommands: enabled, updatedAt: new Date() } : s
	          ),
	        }))
	      }

	      if (isCefContext()) {
	        const requestKey = `setSessionAutoApproveCommands:${id}`
	        const requestId = createRequestId('setSessionAutoApproveCommands')
	        rememberPendingRequest(requestKey, requestId)
	        applyAutoApprove()
	        const response = await sendToCEF({
	          action: 'setChatAutoApproveCommands',
	          payload: { chatId: id, enabled },
	          requestId,
	        })

	        if (response.ok) {
	          clearPendingRequest(requestKey, response.requestId)
	          return true
	        }

	        if (isLatestPendingRequest(requestKey, response.requestId)) {
	          set((state) => ({
	            sessions: state.sessions.map((s) => (s.id === id ? previousSession : s)),
	          }))
	          pendingRequestIdsByKey.delete(requestKey)
	        }

	        return false
	      }

	      applyAutoApprove()
	      return true
	    },

	    setSessionMemoryEnabled: async (id, enabled) => {
	      const previousSession = get().sessions.find((s) => s.id === id)
	      if (!previousSession) {
	        return false
	      }
	      if ((previousSession.memoryEnabled ?? true) === enabled) {
	        return true
	      }

	      const applyMemory = () => {
	        set((state) => ({
	          sessions: state.sessions.map((s) =>
	            s.id === id ? { ...s, memoryEnabled: enabled, updatedAt: new Date() } : s
	          ),
	        }))
	      }

	      if (isCefContext()) {
	        const requestKey = `setSessionMemoryEnabled:${id}`
	        const requestId = createRequestId('setSessionMemoryEnabled')
	        rememberPendingRequest(requestKey, requestId)
	        applyMemory()
	        const response = await sendToCEF({
	          action: 'setChatMemoryEnabled',
	          payload: { chatId: id, enabled },
	          requestId,
	        })

	        if (response.ok) {
	          clearPendingRequest(requestKey, response.requestId)
	          return true
	        }

	        if (isLatestPendingRequest(requestKey, response.requestId)) {
	          set((state) => ({
	            sessions: state.sessions.map((s) => (s.id === id ? previousSession : s)),
	          }))
	          pendingRequestIdsByKey.delete(requestKey)
	        }
	        return false
	      }

	      applyMemory()
	      return true
	    },

	    setMemorySettings: async (settings) => {
	      const previous = {
	        memoryEnabledDefault: get().memoryEnabledDefault,
	        memoryIdleDelaySeconds: get().memoryIdleDelaySeconds,
	        memoryRecallBudgetBytes: get().memoryRecallBudgetBytes,
	        memoryWorkerBindings: get().memoryWorkerBindings,
	      }
	      const next = {
	        memoryEnabledDefault: settings.memoryEnabledDefault ?? previous.memoryEnabledDefault,
	        memoryIdleDelaySeconds: clampedFiniteNumberOr(settings.memoryIdleDelaySeconds, previous.memoryIdleDelaySeconds, MIN_MEMORY_IDLE_DELAY_SECONDS, MAX_MEMORY_IDLE_DELAY_SECONDS),
	        memoryRecallBudgetBytes: clampedFiniteNumberOr(settings.memoryRecallBudgetBytes, previous.memoryRecallBudgetBytes, MIN_MEMORY_RECALL_BUDGET_BYTES, MAX_MEMORY_RECALL_BUDGET_BYTES),
	        memoryWorkerBindings: settings.memoryWorkerBindings ?? previous.memoryWorkerBindings,
	      }
	      const applySettings = () => set(next)

	      if (isCefContext()) {
	        const requestId = createRequestId('setMemorySettings')
	        applySettings()
	        const response = await sendToCEF({
	          action: 'setMemorySettings',
	          payload: {
	            enabledDefault: next.memoryEnabledDefault,
	            idleDelaySeconds: next.memoryIdleDelaySeconds,
	            recallBudgetBytes: next.memoryRecallBudgetBytes,
	            workerBindings: next.memoryWorkerBindings,
	          },
	          requestId,
	        })
	        if (!response.ok) {
	          set(previous)
	          return false
	        }
	        return true
	      }

	      applySettings()
	      return true
	    },

	    setEditorSettings: async (settings) => {
	      const previous = {
	        defaultEditorPresetId: get().defaultEditorPresetId,
	        editorFileAssociations: get().editorFileAssociations,
	      }
	      const next = {
	        defaultEditorPresetId: sanitizeEditorPresetId(settings.defaultEditorPresetId),
	        editorFileAssociations: sanitizeEditorFileAssociations(settings.editorFileAssociations),
	      }
	      const applySettings = () => set(next)

	      if (isCefContext()) {
	        const requestId = createRequestId('setEditorSettings')
	        applySettings()
	        const response = await sendToCEF({
	          action: 'setEditorSettings',
	          payload: {
	            defaultEditorPresetId: next.defaultEditorPresetId,
	            fileAssociations: next.editorFileAssociations,
	          },
	          requestId,
	        })
	        if (!response.ok) {
	          set(previous)
	          return false
	        }
	        return true
	      }

	      applySettings()
	      return true
	    },

	    setProviderChatDefaults: async (settings) => {
	      const previous = {
	        defaultNewChatProviderId: get().defaultNewChatProviderId,
	        providerChatDefaults: get().providerChatDefaults,
	      }
	      const requestedDefaultProviderId = normalizeCliProviderIdAlias(settings.defaultNewChatProviderId ?? '') || previous.defaultNewChatProviderId
	      if (!get().providers.some((provider) => provider.id === requestedDefaultProviderId)) {
	        return false
	      }
	      const nextDefaults: Record<string, ProviderChatDefaults> = {
	        ...previous.providerChatDefaults,
	        ...sanitizeProviderChatDefaultsMap(settings.providerChatDefaults),
	      }
	      for (const [providerId, defaults] of Object.entries(nextDefaults)) {
	        const sanitized = sanitizeProviderChatDefaults(defaults)
	        if (providerId !== CODEX_CLI_PROVIDER_ID) {
	          sanitized.reasoningEffort = ''
	          sanitized.serviceTier = ''
	        }
	        nextDefaults[providerId] = sanitized
	      }
	      const next = {
	        defaultNewChatProviderId: requestedDefaultProviderId,
	        providerChatDefaults: nextDefaults,
	      }
	      const applySettings = () => set(next)

	      if (isCefContext()) {
	        const requestId = createRequestId('setProviderChatDefaults')
	        pendingProviderChatDefaults = {
	          requestId,
	          defaultNewChatProviderId: next.defaultNewChatProviderId,
	          providerChatDefaults: next.providerChatDefaults,
	        }
	        applySettings()
	        const response = await sendToCEF({
	          action: 'setProviderChatDefaults',
	          payload: {
	            defaultProviderId: next.defaultNewChatProviderId,
	            defaults: next.providerChatDefaults,
	          },
	          requestId,
	        })
	        if (!response.ok) {
	          clearPendingProviderChatDefaults(response.requestId)
	          set(previous)
	          return false
	        }
	        clearPendingProviderChatDefaults(response.requestId)
	        return true
	      }

	      applySettings()
	      return true
	    },

	    refreshCliProviderVersion: async (providerId) => {
	      const targetProviderId = normalizeCliProviderIdAlias(providerId ?? get().cliVersionManager.providers[0]?.providerId ?? '') || GEMINI_CLI_PROVIDER_ID
	      if (!targetProviderId) return false

	      if (isCefContext()) {
	        const response = await sendToCEF({
	          action: 'refreshCliProviderVersion',
	          payload: { providerId: targetProviderId },
	          requestId: createRequestId('refreshCliProviderVersion'),
	        })
	        return response.ok
	      }

	      set((state) => ({
	        cliVersionManager: {
	          providers: upsertCliVersionProviderState(state.cliVersionManager.providers, {
	            ...emptyCliVersionProviderState,
	            providerId: targetProviderId,
	            status: 'supported',
	            message: 'Provider CLI version is supported.',
	            running: false,
	          }),
	        },
	      }))
	      return true
	    },

	    applyCliProviderVersion: async (providerId, version) => {
	      const targetProviderId = normalizeCliProviderIdAlias(providerId)
	      const targetVersion = version.trim()
	      if (!targetProviderId || !targetVersion) return false

	      if (isCefContext()) {
	        const response = await sendToCEF({
	          action: 'applyCliProviderVersion',
	          payload: { providerId: targetProviderId, version: targetVersion },
	          requestId: createRequestId('applyCliProviderVersion'),
	        })
	        return response.ok
	      }

	      set((state) => ({
	        cliVersionManager: {
	          providers: upsertCliVersionProviderState(state.cliVersionManager.providers, {
	            ...emptyCliVersionProviderState,
	            ...(state.cliVersionManager.providers.find((provider) => provider.providerId === targetProviderId) ?? {}),
	            providerId: targetProviderId,
	            selectedVersion: targetVersion,
	            installedVersion: targetVersion,
	            status: 'supported',
	            message: 'Provider CLI version is supported.',
	            running: false,
	            lastCommand: buildProviderCliInstallCommand(targetProviderId, targetVersion),
	            lastOutput: 'Dev mode install simulated.',
	          }),
	        },
	      }))
	      return true
	    },

	    deleteSession: (id) => {
      if (isCefContext()) {
        const current = get()
        const deletedSession = current.sessions.find((s) => s.id === id)
        if (!deletedSession) {
          return
        }

        const deletedIndex = current.sessions.findIndex((s) => s.id === id)
        const deletedMessages = current.messages[id] ?? []
        const deletedBinding = current.cliBindingBySessionId[id]
        const deletedAcpBinding = current.acpBindingBySessionId[id]
        const deletedTranscript = current.cliTranscriptBySessionId[id]
        const previousActiveSessionId = current.activeSessionId
        const requestKey = `deleteSession:${id}`
        const requestId = createRequestId('deleteSession')
        rememberPendingRequest(requestKey, requestId)
        set((state) => {
          const remaining = state.sessions.filter((s) => s.id !== id)
          const { [id]: _, ...msgs } = state.messages
          const { [id]: __, ...bindings } = state.cliBindingBySessionId
          const { [id]: ___, ...acpBindings } = state.acpBindingBySessionId
          const { [id]: ____, ...transcripts } = state.cliTranscriptBySessionId
          return {
            sessions: remaining,
            messages: msgs,
            cliBindingBySessionId: bindings,
            acpBindingBySessionId: acpBindings,
            cliTranscriptBySessionId: transcripts,
            activeSessionId:
              state.activeSessionId === id ? (remaining[0]?.id ?? null) : state.activeSessionId,
          }
        })

        sendToCEF({ action: 'deleteSession', payload: { chatId: id }, requestId }).then((resp) => {
          if (resp.ok) {
            clearPendingRequest(requestKey, resp.requestId)
            return
          }

          if (!isLatestPendingRequest(requestKey, resp.requestId)) {
            return
          }

          set((state) => {
            const sessions = state.sessions.some((s) => s.id === id)
              ? state.sessions
              : [
                  ...state.sessions.slice(0, Math.min(deletedIndex, state.sessions.length)),
                  deletedSession,
                  ...state.sessions.slice(Math.min(deletedIndex, state.sessions.length)),
                ]

            return {
              sessions,
              messages: {
                ...state.messages,
                [id]: deletedMessages,
              },
              cliBindingBySessionId: deletedBinding
                ? { ...state.cliBindingBySessionId, [id]: deletedBinding }
                : state.cliBindingBySessionId,
              acpBindingBySessionId: deletedAcpBinding
                ? { ...state.acpBindingBySessionId, [id]: deletedAcpBinding }
                : state.acpBindingBySessionId,
              cliTranscriptBySessionId: deletedTranscript
                ? { ...state.cliTranscriptBySessionId, [id]: deletedTranscript }
                : state.cliTranscriptBySessionId,
              activeSessionId: previousActiveSessionId,
            }
          })
          pendingRequestIdsByKey.delete(requestKey)
        })
        return
      }

      set((state) => {
        const remaining = state.sessions.filter((s) => s.id !== id)
        const { [id]: _, ...msgs } = state.messages
        const { [id]: __, ...bindings } = state.cliBindingBySessionId
        const { [id]: ___, ...acpBindings } = state.acpBindingBySessionId
        const { [id]: ____, ...transcripts } = state.cliTranscriptBySessionId
        return {
          sessions: remaining,
          messages: msgs,
          cliBindingBySessionId: bindings,
          acpBindingBySessionId: acpBindings,
          cliTranscriptBySessionId: transcripts,
          activeSessionId:
            state.activeSessionId === id ? (remaining[0]?.id ?? null) : state.activeSessionId,
        }
      })
    },

    // ---- Folder actions ----

    addFolder: (name, _parentId, directory) => {
      if (isCefContext()) {
        return sendToCEF<CppFolder>({ action: 'createFolder', payload: { title: name, directory } }).then((resp) => {
          if (!resp.ok || !resp.data?.id) {
            if (!resp.ok) console.error('[CEF] createFolder failed:', resp.error)
            return false
          }

          set((state) => {
            if (state.folders.some((folder) => folder.id === resp.data!.id)) {
              return {}
            }

            const createdFolder: Folder = {
              id: resp.data!.id,
              name: resp.data!.title,
              parentId: null,
              directory: resp.data!.directory ?? '',
              isExpanded: !resp.data!.collapsed,
              createdAt: new Date(),
            }

            return {
              folders: [...state.folders, createdFolder],
            }
          })
          return true
        })
      }

      folderCounter++
      const folder: Folder = {
        id: makeId('f', folderCounter),
        name,
        parentId: _parentId,
        directory,
        isExpanded: true,
        createdAt: new Date(),
      }
      set((state) => ({ folders: [...state.folders, folder] }))
      return Promise.resolve(true)
    },

	    toggleFolder: (id) => {
	      if (isCefContext()) {
	        const currentFolder = get().folders.find((folder) => folder.id === id)
	        if (!currentFolder) {
	          return
	        }

	        const requestKey = `toggleFolder:${id}`
	        const requestId = createRequestId('toggleFolder')
	        rememberPendingRequest(requestKey, requestId)
	        const previousExpanded = currentFolder.isExpanded
	        set((state) => ({
	          folders: state.folders.map((f) =>
	            f.id === id ? { ...f, isExpanded: !f.isExpanded } : f
	          ),
	        }))
	        sendToCEF({ action: 'toggleFolder', payload: { folderId: id }, requestId }).then((resp) => {
	          if (resp.ok) {
	            clearPendingRequest(requestKey, resp.requestId)
	            return
	          }

	          if (!isLatestPendingRequest(requestKey, resp.requestId)) {
	            return
	          }

	          set((state) => ({
	            folders: state.folders.map((f) =>
	              f.id === id ? { ...f, isExpanded: previousExpanded } : f
	            ),
	          }))
	          pendingRequestIdsByKey.delete(requestKey)
	        })
	        return
	      }

	      set((state) => ({
	        folders: state.folders.map((f) =>
	          f.id === id ? { ...f, isExpanded: !f.isExpanded } : f
	        ),
	      }))
	    },

	    renameFolder: (id, name, directory) => {
	      if (isCefContext()) {
	        const previousFolder = get().folders.find((folder) => folder.id === id)
	        if (!previousFolder) {
	          return
	        }

	        const requestKey = `renameFolder:${id}`
	        const requestId = createRequestId('renameFolder')
	        rememberPendingRequest(requestKey, requestId)
	        set((state) => ({
	          folders: state.folders.map((folder) =>
	            folder.id === id ? { ...folder, name, directory } : folder
	          ),
	        }))
	        sendToCEF({ action: 'renameFolder', payload: { folderId: id, title: name, directory }, requestId }).then(
	          (resp) => {
	            if (resp.ok) {
	              clearPendingRequest(requestKey, resp.requestId)
	              return
	            }

	            if (!isLatestPendingRequest(requestKey, resp.requestId)) {
	              return
	            }

	            set((state) => ({
	              folders: state.folders.map((folder) => (folder.id === id ? previousFolder : folder)),
	            }))
	            pendingRequestIdsByKey.delete(requestKey)
	          }
	        )
	        return
	      }

      set((state) => ({
        folders: state.folders.map((f) => (f.id === id ? { ...f, name, directory } : f)),
      }))
    },

	    deleteFolder: (id) => {
	      if (isCefContext()) {
	        const deletedFolder = get().folders.find((folder) => folder.id === id)
	        if (!deletedFolder) {
	          return
	        }

	        const requestId = createRequestId('deleteFolder')
	        sendToCEF({ action: 'deleteFolder', payload: { folderId: id }, requestId }).then((resp) => {
	          if (!resp.ok) {
	            console.error('[CEF] deleteFolder failed:', resp.error)
	          }
	        })
	        return
	      }

      set((state) => {
        const deletedSessionIds = new Set(
          state.sessions.filter((session) => session.folderId === id).map((session) => session.id)
        )
        const remainingFolders = state.folders.filter((f) => f.id !== id)
        const sessions = state.sessions.filter((session) => !deletedSessionIds.has(session.id))
        const messages = { ...state.messages }
        const cliBindingBySessionId = { ...state.cliBindingBySessionId }
        const acpBindingBySessionId = { ...state.acpBindingBySessionId }
        const cliTranscriptBySessionId = { ...state.cliTranscriptBySessionId }

        deletedSessionIds.forEach((sessionId) => {
          delete messages[sessionId]
          delete cliBindingBySessionId[sessionId]
          delete acpBindingBySessionId[sessionId]
          delete cliTranscriptBySessionId[sessionId]
        })

        return {
          folders: remainingFolders,
          sessions,
          messages,
          cliBindingBySessionId,
          acpBindingBySessionId,
          cliTranscriptBySessionId,
          activeSessionId:
            state.activeSessionId !== null && deletedSessionIds.has(state.activeSessionId)
              ? (sessions[0]?.id ?? null)
              : state.activeSessionId,
        }
      })
    },

    browseFolderDirectory: async (currentValue) => {
      if (!isCefContext()) {
        return null
      }

      const response = await sendToCEF<{ selectedPath?: string }>({
        action: 'browseFolderDirectory',
        payload: { currentValue },
      })

      const selectedPath = response.ok ? response.data?.selectedPath?.trim() ?? '' : ''
      return selectedPath.length > 0 ? selectedPath : null
    },

    browseMarkdownStoreDirectory: async (currentValue) => {
      if (!isCefContext()) {
        return null
      }

      const response = await sendToCEF<{ selectedPath?: string }>({
        action: 'browseMarkdownStoreDirectory',
        payload: { currentValue },
      })

      const selectedPath = response.ok ? response.data?.selectedPath?.trim() ?? '' : ''
      return selectedPath.length > 0 ? selectedPath : null
    },

    setMarkdownStoreDirectory: async (directory) => {
      const trimmed = directory.trim()
      if (isCefContext()) {
        const response = await sendToCEF<{ directory?: string }>({
          action: 'setMarkdownStoreDirectory',
          payload: { directory: trimmed },
        })
        if (!response.ok) {
          set({ markdownStoreError: response.error ?? 'Failed to save Markdown Store directory.' })
          return false
        }
        set({
          markdownStoreDirectory: response.data?.directory ?? trimmed,
          markdownStoreError: '',
        })
        return true
      }

      set({ markdownStoreDirectory: trimmed, markdownStoreError: '' })
      return true
    },

    openMarkdownStore: async () => {
      set({ isMarkdownStoreOpen: true, markdownStoreLoading: true, markdownStoreError: '' })
      return get().refreshMarkdownStore()
    },

    closeMarkdownStore: () => set({
      isMarkdownStoreOpen: false,
      markdownStoreEntries: [],
      markdownStoreLoading: false,
      markdownStoreError: '',
    }),

    refreshMarkdownStore: async () => {
      if (isCefContext()) {
        const requestKey = 'listMarkdownStoreEntries'
        const requestId = createRequestId(requestKey)
        rememberPendingRequest(requestKey, requestId)
        const response = await sendToCEF<{ directory?: string; entries?: MarkdownStoreEntry[] }>({
          action: 'listMarkdownStoreEntries',
          payload: { requestId },
          requestId,
        })
        if (!isLatestPendingRequest(requestKey, response.requestId)) return false
        clearPendingRequest(requestKey, response.requestId)
        if (!response.ok) {
          set({
            markdownStoreLoading: false,
            markdownStoreError: response.error ?? 'Failed to load Markdown Store.',
          })
          return false
        }
        set({
          markdownStoreDirectory: response.data?.directory ?? get().markdownStoreDirectory,
          markdownStoreEntries: response.data?.entries ?? [],
          markdownStoreLoading: false,
          markdownStoreError: '',
        })
        return true
      }

      set({ markdownStoreLoading: false, markdownStoreError: '' })
      return true
    },

    createMarkdownStoreEntry: async (draft) => {
      if (isCefContext()) {
        const response = await sendToCEF<MarkdownStoreEntry>({
          action: 'createMarkdownStoreEntry',
          payload: draft,
        })
        if (!response.ok) {
          set({ markdownStoreError: response.error ?? 'Failed to publish Markdown Store entry.' })
          return false
        }
        return get().refreshMarkdownStore()
      }

      const synthetic: MarkdownStoreEntry = {
        id: `${draft.title || Date.now()}.uam`,
        title: draft.title,
        maker: draft.maker,
        review: draft.review,
        dateCreated: new Date().toISOString(),
        dateUpdated: new Date().toISOString(),
        preview: draft.body,
        filePath: `${get().markdownStoreDirectory}/${draft.title || Date.now()}.uam`,
      }
      set((state) => ({ markdownStoreEntries: [...state.markdownStoreEntries, synthetic] }))
      return true
    },

    revealMarkdownStoreEntry: async (entry) => {
      if (isCefContext()) {
        const response = await sendToCEF({
          action: 'revealMarkdownStoreEntry',
          payload: { filePath: entry.filePath },
        })
        if (!response.ok) {
          set({ markdownStoreError: response.error ?? 'Failed to reveal Markdown Store entry.' })
          return false
        }
      }
      return true
    },

    attachMarkdownStoreEntry: (sessionId, entry) => set((state) => {
      const current = state.markdownStoreAttachedBySessionId[sessionId] ?? []
      if (current.some((candidate) => candidate.filePath === entry.filePath)) {
        return state
      }
      return {
        markdownStoreAttachedBySessionId: {
          ...state.markdownStoreAttachedBySessionId,
          [sessionId]: [...current, entry],
        },
      }
    }),

    detachMarkdownStoreEntry: (sessionId, filePath) => set((state) => ({
      markdownStoreAttachedBySessionId: {
        ...state.markdownStoreAttachedBySessionId,
        [sessionId]: (state.markdownStoreAttachedBySessionId[sessionId] ?? []).filter((entry) => entry.filePath !== filePath),
      },
    })),

    openAllMemoryLibrary: async () => {
      set({ memoryLibraryLoading: true, memoryLibraryError: '' })

      if (isCefContext()) {
        const requestKey = 'listMemoryEntries:all'
        const requestId = createRequestId(requestKey)
        rememberPendingRequest(requestKey, requestId)
        const response = await sendToCEF<{ scope?: MemoryScope; entries?: MemoryEntry[] }>({
          action: 'listMemoryEntries',
          payload: { scopeType: 'all', requestId },
          requestId,
        })
        if (!isLatestPendingRequest(requestKey, response.requestId)) return false
        clearPendingRequest(requestKey, response.requestId)

        if (!response.ok || !response.data?.scope) {
          set({
            memoryLibraryLoading: false,
            memoryLibraryError: response.error ?? 'Failed to load memory.',
          })
          return false
        }

        set({
          memoryLibraryScope: response.data.scope,
          memoryLibraryEntries: response.data.entries ?? [],
          memoryLibraryLoading: false,
          memoryLibraryError: '',
        })
        return true
      }

      set({
        memoryLibraryScope: {
          scopeType: 'all',
          folderId: '',
          label: 'All memory',
          rootPath: 'Global and project memory roots',
          rootCount: 0,
        },
        memoryLibraryEntries: [],
        memoryLibraryLoading: false,
        memoryLibraryError: '',
      })
      return true
    },

    openGlobalMemoryLibrary: async () => {
      set({ memoryLibraryLoading: true, memoryLibraryError: '' })

      if (isCefContext()) {
        const requestKey = 'listMemoryEntries:global'
        const requestId = createRequestId(requestKey)
        rememberPendingRequest(requestKey, requestId)
        const response = await sendToCEF<{ scope?: MemoryScope; entries?: MemoryEntry[] }>({
          action: 'listMemoryEntries',
          payload: { scopeType: 'global', requestId },
          requestId,
        })
        if (!isLatestPendingRequest(requestKey, response.requestId)) return false
        clearPendingRequest(requestKey, response.requestId)

        if (!response.ok || !response.data?.scope) {
          set({
            memoryLibraryLoading: false,
            memoryLibraryError: response.error ?? 'Failed to load global memory.',
          })
          return false
        }

        set({
          memoryLibraryScope: response.data.scope,
          memoryLibraryEntries: response.data.entries ?? [],
          memoryLibraryLoading: false,
          memoryLibraryError: '',
        })
        return true
      }

      set({
        memoryLibraryScope: {
          scopeType: 'global',
          folderId: '',
          label: 'Global memory',
          rootPath: '/tmp/uam-memory',
        },
        memoryLibraryEntries: [],
        memoryLibraryLoading: false,
        memoryLibraryError: '',
      })
      return true
    },

    openFolderMemoryLibrary: async (folderId) => {
      set({ memoryLibraryLoading: true, memoryLibraryError: '' })

      if (isCefContext()) {
        const requestKey = `listMemoryEntries:folder:${folderId}`
        const requestId = createRequestId('listMemoryEntries')
        rememberPendingRequest(requestKey, requestId)
        const response = await sendToCEF<{ scope?: MemoryScope; entries?: MemoryEntry[] }>({
          action: 'listMemoryEntries',
          payload: { scopeType: 'folder', folderId, requestId },
          requestId,
        })
        if (!isLatestPendingRequest(requestKey, response.requestId)) return false
        clearPendingRequest(requestKey, response.requestId)

        if (!response.ok || !response.data?.scope) {
          set({
            memoryLibraryLoading: false,
            memoryLibraryError: response.error ?? 'Failed to load project memory.',
          })
          return false
        }

        set({
          memoryLibraryScope: response.data.scope,
          memoryLibraryEntries: response.data.entries ?? [],
          memoryLibraryLoading: false,
          memoryLibraryError: '',
        })
        return true
      }

      const folder = get().folders.find((candidate) => candidate.id === folderId)
      if (!folder) {
        set({
          memoryLibraryLoading: false,
          memoryLibraryError: `Folder not found: ${folderId}`,
        })
        return false
      }

      set({
        memoryLibraryScope: {
          scopeType: 'folder',
          folderId,
          label: folder.name,
          rootPath: `${folder.directory}/.UAM`,
        },
        memoryLibraryEntries: [],
        memoryLibraryLoading: false,
        memoryLibraryError: '',
      })
      return true
    },

    closeMemoryLibrary: () => set({
      memoryLibraryScope: null,
      memoryLibraryEntries: [],
      memoryLibraryLoading: false,
      memoryLibraryError: '',
    }),

    refreshMemoryLibrary: async () => {
      const scope = get().memoryLibraryScope
      if (!scope) {
        return false
      }

      set({ memoryLibraryLoading: true, memoryLibraryError: '' })

      if (isCefContext()) {
        const requestKey = `listMemoryEntries:refresh:${scope.scopeType}:${scope.folderId ?? ''}`
        const requestId = createRequestId('listMemoryEntries')
        rememberPendingRequest(requestKey, requestId)
        const response = await sendToCEF<{ scope?: MemoryScope; entries?: MemoryEntry[] }>({
          action: 'listMemoryEntries',
          payload: { scopeType: scope.scopeType, folderId: scope.folderId, requestId },
          requestId,
        })
        if (!isLatestPendingRequest(requestKey, response.requestId)) return false
        clearPendingRequest(requestKey, response.requestId)

        if (!response.ok || !response.data?.scope) {
          set({
            memoryLibraryLoading: false,
            memoryLibraryError: response.error ?? 'Failed to refresh memory library.',
          })
          return false
        }

        set({
          memoryLibraryScope: response.data.scope,
          memoryLibraryEntries: response.data.entries ?? [],
          memoryLibraryLoading: false,
          memoryLibraryError: '',
        })
        return true
      }

      set({ memoryLibraryLoading: false })
      return true
    },

    createMemoryEntry: async (draft) => {
      const scope = get().memoryLibraryScope
      if (!scope) {
        return false
      }

      if (isCefContext()) {
        const payload: Record<string, string> = {
          scopeType: scope.scopeType,
          folderId: scope.folderId,
          category: draft.category,
          title: draft.title,
          memory: draft.memory,
          evidence: draft.evidence,
          confidence: draft.confidence,
          sourceChatId: draft.sourceChatId,
        }

        if (scope.scopeType === 'all') {
          payload.targetScopeType = draft.targetScopeType ?? 'global'
          payload.targetFolderId = draft.targetFolderId ?? ''
        }

        const response = await sendToCEF({
          action: 'createMemoryEntry',
          payload,
        })

        if (!response.ok) {
          set({ memoryLibraryError: response.error ?? 'Failed to create memory entry.' })
          return false
        }

        return get().refreshMemoryLibrary()
      }

      const syntheticEntry: MemoryEntry = {
        id: `memory-${Date.now()}.md`,
        title: draft.title,
        category: draft.category,
        scope: (scope.scopeType === 'global' || (scope.scopeType === 'all' && draft.targetScopeType === 'global')) ? 'global' : 'local',
        confidence: draft.confidence,
        sourceChatId: draft.sourceChatId,
        lastObserved: new Date().toISOString(),
        occurrenceCount: 1,
        preview: draft.memory,
        filePath: `${scope.rootPath}/${draft.category}/${draft.title}.md`,
        scopeType: scope.scopeType === 'all' ? (draft.targetScopeType ?? 'global') : (scope.scopeType === 'global' ? 'global' : 'folder'),
        folderId: scope.scopeType === 'all' ? draft.targetFolderId : scope.folderId,
        scopeLabel: scope.scopeType === 'all' && draft.targetScopeType === 'folder'
          ? (get().folders.find((folder) => folder.id === draft.targetFolderId)?.name ?? 'Project memory')
          : scope.label,
        rootPath: scope.rootPath,
      }
      set((state) => ({
        memoryLibraryEntries: [...state.memoryLibraryEntries, syntheticEntry],
        memoryLibraryError: '',
      }))
      return true
    },

    deleteMemoryEntry: async (entryId) => {
      const scope = get().memoryLibraryScope
      if (!scope) {
        return false
      }

      if (isCefContext()) {
        const response = await sendToCEF({
          action: 'deleteMemoryEntry',
          payload: {
            scopeType: scope.scopeType,
            folderId: scope.folderId,
            entryId,
          },
        })

        if (!response.ok) {
          set({ memoryLibraryError: response.error ?? 'Failed to delete memory entry.' })
          return false
        }

        return get().refreshMemoryLibrary()
      }

      set((state) => ({
        memoryLibraryEntries: state.memoryLibraryEntries.filter((entry) => entry.id !== entryId),
      }))
      return true
    },

    deleteMemoryEntries: async (entryIds) => {
      const scope = get().memoryLibraryScope
      const uniqueEntryIds = Array.from(new Set(entryIds.filter((entryId) => entryId.trim().length > 0)))
      if (!scope || uniqueEntryIds.length === 0) {
        return false
      }

      if (isCefContext()) {
        for (const entryId of uniqueEntryIds) {
          const response = await sendToCEF({
            action: 'deleteMemoryEntry',
            payload: {
              scopeType: scope.scopeType,
              folderId: scope.folderId,
              entryId,
            },
          })

          if (!response.ok) {
            set({ memoryLibraryError: response.error ?? 'Failed to delete memory entries.' })
            await get().refreshMemoryLibrary()
            return false
          }
        }

        return get().refreshMemoryLibrary()
      }

      set((state) => ({
        memoryLibraryEntries: state.memoryLibraryEntries.filter((entry) => !uniqueEntryIds.includes(entry.id)),
        memoryLibraryError: '',
      }))
      return true
    },

    openMemoryRoot: async () => {
      const scope = get().memoryLibraryScope
      if (!scope) {
        return false
      }

      if (isCefContext()) {
        const response = await sendToCEF({
          action: 'openMemoryRoot',
          payload: { scopeType: scope.scopeType, folderId: scope.folderId },
        })
        if (!response.ok) {
          set({ memoryLibraryError: response.error ?? 'Failed to open memory root.' })
          return false
        }
      }

      return true
    },

    revealMemoryEntry: async (entryId) => {
      const scope = get().memoryLibraryScope
      if (!scope) {
        return false
      }

      if (isCefContext()) {
        const response = await sendToCEF({
          action: 'revealMemoryEntry',
          payload: { scopeType: scope.scopeType, folderId: scope.folderId, entryId },
        })
        if (!response.ok) {
          set({ memoryLibraryError: response.error ?? 'Failed to reveal memory file.' })
          return false
        }
      }

      return true
    },

    openMemoryScanModal: async () => {
      set({
        isMemoryScanModalOpen: true,
        memoryScanLoading: true,
        memoryScanError: '',
        memoryScanCandidates: [],
        selectedMemoryScanChatIds: [],
      })

      if (isCefContext()) {
        const response = await sendToCEF<{ candidates?: MemoryScanCandidate[] }>({
          action: 'listMemoryScanCandidates',
        })
        if (!response.ok) {
          set({
            memoryScanLoading: false,
            memoryScanError: response.error ?? 'Failed to load chats for memory scan.',
          })
          return false
        }

        const candidates = response.data?.candidates ?? []
        set({
          memoryScanCandidates: candidates,
          selectedMemoryScanChatIds: candidates.map((candidate) => candidate.chatId),
          memoryScanLoading: false,
          memoryScanError: '',
        })
        return true
      }

      const sessions = get().sessions
        .filter((session) => (session.memoryEnabled ?? true) && (get().messages[session.id]?.length ?? 0) > 0)
        .map((session) => ({
          chatId: session.id,
          title: session.name,
          folderId: session.folderId ?? '',
          folderTitle: session.folderId ? (get().folders.find((folder) => folder.id === session.folderId)?.name ?? '') : '',
          providerId: session.providerId ?? GEMINI_CLI_PROVIDER_ID,
          messageCount: get().messages[session.id]?.length ?? 0,
          memoryEnabled: session.memoryEnabled ?? true,
          memoryLastProcessedAt: session.memoryLastProcessedAt ?? '',
          alreadyFullyProcessed: false,
        }))
      set({
        memoryScanCandidates: sessions,
        selectedMemoryScanChatIds: sessions.map((candidate) => candidate.chatId),
        memoryScanLoading: false,
        memoryScanError: '',
      })
      return true
    },

    closeMemoryScanModal: () => set({
      isMemoryScanModalOpen: false,
      memoryScanCandidates: [],
      selectedMemoryScanChatIds: [],
      memoryScanLoading: false,
      memoryScanRunning: false,
      memoryScanError: '',
    }),

    toggleMemoryScanChat: (chatId) => set((state) => ({
      selectedMemoryScanChatIds: state.selectedMemoryScanChatIds.includes(chatId)
        ? state.selectedMemoryScanChatIds.filter((id) => id !== chatId)
        : [...state.selectedMemoryScanChatIds, chatId],
    })),

    selectAllMemoryScanChats: () => set((state) => ({
      selectedMemoryScanChatIds: state.memoryScanCandidates.map((candidate) => candidate.chatId),
    })),

    selectNoMemoryScanChats: () => set({ selectedMemoryScanChatIds: [] }),

    startMemoryScan: async () => {
      const selectedChatIds = get().selectedMemoryScanChatIds
      if (selectedChatIds.length === 0) {
        set({ memoryScanError: 'Select at least one chat to scan.' })
        return false
      }

      set({ memoryScanRunning: true, memoryScanError: '' })

      if (isCefContext()) {
        const response = await sendToCEF<{ queuedCount?: number }>({
          action: 'scanCurrentChats',
          payload: { chatIds: selectedChatIds },
        })

        if (!response.ok) {
          set({
            memoryScanRunning: false,
            memoryScanError: response.error ?? 'Failed to queue memory scan.',
          })
          return false
        }

        set({
          isMemoryScanModalOpen: false,
          memoryScanCandidates: [],
          selectedMemoryScanChatIds: [],
          memoryScanLoading: false,
          memoryScanRunning: false,
          memoryScanError: '',
        })
        return true
      }

      set({
        isMemoryScanModalOpen: false,
        memoryScanCandidates: [],
        selectedMemoryScanChatIds: [],
        memoryScanLoading: false,
        memoryScanRunning: false,
        memoryScanError: '',
      })
      return true
    },

    setCliBinding: (sessionId, binding) =>
      set((state) => {
        const existingBinding = state.cliBindingBySessionId[sessionId]
        const resolvedTerminalId = binding.terminalId ?? existingBinding?.terminalId ?? ''
        const running = binding.running ?? existingBinding?.running ?? false
        const turnState = binding.turnState ?? existingBinding?.turnState ?? 'idle'
        const processing = binding.processing ?? existingBinding?.processing ?? false
        const lifecycleState =
          binding.lifecycleState ??
          existingBinding?.lifecycleState ??
          normalizeCliLifecycleState(undefined, running, turnState, processing)
        let nextTranscripts = state.cliTranscriptBySessionId
        const existingTranscript = state.cliTranscriptBySessionId[sessionId]

        if (existingTranscript && resolvedTerminalId) {
          if (existingTranscript.terminalId && existingTranscript.terminalId !== resolvedTerminalId) {
            nextTranscripts = {
              ...state.cliTranscriptBySessionId,
              [sessionId]: {
                terminalId: resolvedTerminalId,
                content: '',
              },
            }
          } else if (existingTranscript.terminalId !== resolvedTerminalId) {
            nextTranscripts = {
              ...state.cliTranscriptBySessionId,
              [sessionId]: {
                ...existingTranscript,
                terminalId: resolvedTerminalId,
              },
            }
          }
        }

        return {
          cliBindingBySessionId: {
            ...state.cliBindingBySessionId,
            [sessionId]: {
              terminalId: resolvedTerminalId,
              boundChatId: binding.boundChatId ?? existingBinding?.boundChatId ?? sessionId,
              running,
              lifecycleState,
              turnState: cliLifecycleIsProcessing(lifecycleState) ? 'busy' : turnState,
              processing: processing || cliLifecycleIsProcessing(lifecycleState),
              readySinceLastSelect: binding.readySinceLastSelect ?? existingBinding?.readySinceLastSelect ?? false,
              active: binding.active ?? existingBinding?.active ?? false,
              lastError: binding.lastError ?? existingBinding?.lastError ?? '',
            },
          },
          cliTranscriptBySessionId: nextTranscripts,
        }
      }),

    stageChatAttachments: async (sessionId, items) => {
      if (items.length === 0) return []
      if (isCefContext()) {
        const response = await sendToCEF<{ attachments?: Attachment[] }>({
          action: 'stageChatAttachments',
          payload: { chatId: sessionId, items },
        })
        if (!response.ok) {
          throw new Error(response.error ?? 'Failed to stage attachments.')
        }
        return (response.data?.attachments ?? []).flatMap((attachment) => {
          const sanitized = sanitizeAttachment(attachment)
          return sanitized ? [sanitized] : []
        })
      }

      return items.map((item) => ({
        id: item.id,
        name: item.name,
        type: item.kind,
        size: item.size ?? 0,
        path: item.path || item.name,
      }))
    },

    sendAcpPrompt: async (sessionId, text, attachments = []) => {
      const prompt = text.trim()
      if (!prompt) {
        return false
      }
      const markdownStoreFiles = (get().markdownStoreAttachedBySessionId[sessionId] ?? []).map((entry) => entry.filePath)
      const state = get()
      const goalId = state.activeGoalIdByChatId[sessionId] ?? null

      if (isCefContext()) {
        const response = await sendToCEF({
          action: 'sendAcpPrompt',
          payload: {
            chatId: sessionId,
            text: prompt,
            markdownStoreFiles,
            attachments,
            goalMode: goalId != null,
            goalId,
          },
        })
        if (!response.ok) {
          set((state) => ({
            acpBindingBySessionId: {
              ...state.acpBindingBySessionId,
              [sessionId]: {
                ...(state.acpBindingBySessionId[sessionId] ?? {
                  sessionId: '',
                  providerId: state.sessions.find((session) => session.id === sessionId)?.providerId ?? GEMINI_CLI_PROVIDER_ID,
                  protocolKind: 'gemini-acp',
                  threadId: '',
                  running: false,
                  lifecycleState: 'error' as AcpLifecycleState,
                  processing: false,
                  readySinceLastSelect: false,
	                  processingStartedAtMs: null,
	                  lastError: '',
	                  recentStderr: '',
	                  lastExitCode: null,
	                  diagnostics: [],
	                  toolCalls: [],
                  planSummary: '',
                  planEntries: [],
                  availableModes: [],
                  currentModeId: 'default',
                  availableModels: [],
                  currentModelId: '',
                  turnEvents: [],
                  turnUserMessageIndex: -1,
                  turnAssistantMessageIndex: -1,
                  turnSerial: 0,
                  waitIsStale: false,
                  waitStaleReason: '',
                  waitSeconds: 0,
                  pendingPermission: null,
                  pendingUserInput: null,
                  agentInfo: null,
                }),
                lifecycleState: 'error',
                processing: false,
                processingStartedAtMs: null,
                lastError: response.error ?? 'Failed to send ACP prompt.',
              },
            },
          }))
          return false
        }
        set((state) => ({
          markdownStoreAttachedBySessionId: {
            ...state.markdownStoreAttachedBySessionId,
            [sessionId]: [],
          },
        }))
        return true
      }

      const now = new Date()
      set((state) => ({
        messages: {
          ...state.messages,
          [sessionId]: [
            ...(state.messages[sessionId] ?? []),
            {
              id: `dev-user-${Date.now()}`,
              sessionId,
              role: 'user',
              content: prompt,
              attachments: [
                ...markdownStoreFiles.map((filePath) => ({
                id: filePath,
                name: filePath.split(/[\\/]/).pop() || filePath,
                type: 'markdown-store',
                size: 0,
                path: filePath,
                })),
                ...attachments,
              ],
              createdAt: now,
            },
            {
              id: `dev-assistant-${Date.now()}`,
              sessionId,
              role: 'assistant',
              content: 'ACP dev mode response placeholder.',
              createdAt: now,
            },
          ],
        },
        markdownStoreAttachedBySessionId: {
          ...state.markdownStoreAttachedBySessionId,
          [sessionId]: [],
        },
        acpBindingBySessionId: {
          ...state.acpBindingBySessionId,
          [sessionId]: {
            sessionId: 'dev-acp-session',
            providerId: state.sessions.find((session) => session.id === sessionId)?.providerId ?? GEMINI_CLI_PROVIDER_ID,
            protocolKind: 'gemini-acp',
            threadId: '',
            running: true,
            lifecycleState: 'ready',
            processing: false,
            readySinceLastSelect: false,
	            processingStartedAtMs: null,
	            lastError: '',
	            recentStderr: '',
	            lastExitCode: null,
	            diagnostics: [],
	            toolCalls: [],
            planSummary: '',
            planEntries: [],
            availableModes: [],
            currentModeId: state.sessions.find((session) => session.id === sessionId)?.approvalMode ?? 'default',
            availableModels: [],
            currentModelId: state.sessions.find((session) => session.id === sessionId)?.modelId ?? '',
            turnEvents: [],
            turnUserMessageIndex: -1,
            turnAssistantMessageIndex: -1,
            turnSerial: (state.acpBindingBySessionId[sessionId]?.turnSerial ?? 0) + 1,
            waitIsStale: false,
            waitStaleReason: '',
            waitSeconds: 0,
            pendingPermission: null,
            pendingUserInput: null,
            agentInfo: { name: 'dev', title: 'Dev ACP', version: 'local' },
          },
        },
      }))
      return true
    },

    cancelAcpTurn: async (sessionId) => {
      if (isCefContext()) {
        const response = await sendToCEF({
          action: 'cancelAcpTurn',
          payload: { chatId: sessionId },
        })
        return response.ok
      }

      set((state) => ({
        acpBindingBySessionId: {
          ...state.acpBindingBySessionId,
          [sessionId]: {
            ...(state.acpBindingBySessionId[sessionId] ?? {
              sessionId: 'dev-acp-session',
              running: true,
              lifecycleState: 'ready' as AcpLifecycleState,
              processing: false,
              readySinceLastSelect: false,
	              processingStartedAtMs: null,
	              lastError: '',
	              recentStderr: '',
	              lastExitCode: null,
	              diagnostics: [],
	              toolCalls: [],
              planSummary: '',
              planEntries: [],
              availableModes: [],
              currentModeId: state.sessions.find((session) => session.id === sessionId)?.approvalMode ?? 'default',
              availableModels: [],
              currentModelId: state.sessions.find((session) => session.id === sessionId)?.modelId ?? '',
              turnEvents: [],
              turnUserMessageIndex: -1,
              turnAssistantMessageIndex: -1,
              turnSerial: 0,
              waitIsStale: false,
              waitStaleReason: '',
              waitSeconds: 0,
              pendingPermission: null,
              pendingUserInput: null,
              agentInfo: null,
            }),
            lifecycleState: 'ready',
            processing: false,
            processingStartedAtMs: null,
            attentionKind: null,
            pendingPermission: null,
            pendingUserInput: null,
          },
        },
      }))
      return true
    },

    resolveAcpPermission: async (sessionId, requestId, optionId) => {
      if (isCefContext()) {
        const response = await sendToCEF({
          action: 'resolveAcpPermission',
          payload: {
            chatId: sessionId,
            requestId,
            optionId,
            cancelled: optionId === 'cancelled',
          },
        })
        return response.ok
      }

      set((state) => ({
        acpBindingBySessionId: {
          ...state.acpBindingBySessionId,
          [sessionId]: {
            ...(state.acpBindingBySessionId[sessionId] ?? {
              sessionId: 'dev-acp-session',
              running: true,
              lifecycleState: 'ready' as AcpLifecycleState,
              processing: false,
              readySinceLastSelect: false,
	              processingStartedAtMs: null,
	              lastError: '',
	              recentStderr: '',
	              lastExitCode: null,
	              diagnostics: [],
	              toolCalls: [],
              planSummary: '',
              planEntries: [],
              availableModes: [],
              currentModeId: state.sessions.find((session) => session.id === sessionId)?.approvalMode ?? 'default',
              availableModels: [],
              currentModelId: state.sessions.find((session) => session.id === sessionId)?.modelId ?? '',
              turnEvents: [],
              turnUserMessageIndex: -1,
              turnAssistantMessageIndex: -1,
              turnSerial: 0,
              waitIsStale: false,
              waitStaleReason: '',
              waitSeconds: 0,
              pendingPermission: null,
              pendingUserInput: null,
              agentInfo: null,
            }),
            lifecycleState: 'processing',
            attentionKind: null,
            pendingPermission: null,
            pendingUserInput: null,
          },
        },
      }))
      return true
    },

    resolveAcpUserInput: async (sessionId, requestId, answers) => {
      if (isCefContext()) {
        const response = await sendToCEF({
          action: 'resolveAcpUserInput',
          payload: {
            chatId: sessionId,
            requestId,
            answers,
          },
        })
        return response.ok
      }

      set((state) => ({
        acpBindingBySessionId: {
          ...state.acpBindingBySessionId,
          [sessionId]: {
            ...(state.acpBindingBySessionId[sessionId] ?? {
              sessionId: 'dev-acp-session',
              providerId: state.sessions.find((session) => session.id === sessionId)?.providerId ?? GEMINI_CLI_PROVIDER_ID,
              protocolKind: 'gemini-acp',
              threadId: '',
              running: true,
              lifecycleState: 'ready' as AcpLifecycleState,
              processing: false,
              readySinceLastSelect: false,
              processingStartedAtMs: null,
              lastError: '',
              recentStderr: '',
              lastExitCode: null,
              diagnostics: [],
              toolCalls: [],
              planSummary: '',
              planEntries: [],
              availableModes: [],
              currentModeId: state.sessions.find((session) => session.id === sessionId)?.approvalMode ?? 'default',
              availableModels: [],
              currentModelId: state.sessions.find((session) => session.id === sessionId)?.modelId ?? '',
              turnEvents: [],
              turnUserMessageIndex: -1,
              turnAssistantMessageIndex: -1,
              turnSerial: 0,
              waitIsStale: false,
              waitStaleReason: '',
              waitSeconds: 0,
              pendingPermission: null,
              pendingUserInput: null,
              agentInfo: null,
            }),
            lifecycleState: 'processing',
            processing: true,
            attentionKind: null,
            pendingUserInput: null,
          },
        },
      }))
      return true
    },

    stopAcpSession: async (sessionId) => {
      if (isCefContext()) {
        const response = await sendToCEF({
          action: 'stopAcpSession',
          payload: { chatId: sessionId },
        })
        return response.ok
      }

      set((state) => ({
        acpBindingBySessionId: {
          ...state.acpBindingBySessionId,
          [sessionId]: {
            ...(state.acpBindingBySessionId[sessionId] ?? {
              sessionId: '',
              running: false,
              lifecycleState: 'stopped' as AcpLifecycleState,
              processing: false,
              readySinceLastSelect: false,
	              processingStartedAtMs: null,
	              lastError: '',
	              recentStderr: '',
	              lastExitCode: null,
	              diagnostics: [],
	              toolCalls: [],
              planSummary: '',
              planEntries: [],
              availableModes: [],
              currentModeId: state.sessions.find((session) => session.id === sessionId)?.approvalMode ?? 'default',
              availableModels: [],
              currentModelId: state.sessions.find((session) => session.id === sessionId)?.modelId ?? '',
              turnEvents: [],
              turnUserMessageIndex: -1,
              turnAssistantMessageIndex: -1,
              turnSerial: 0,
              waitIsStale: false,
              waitStaleReason: '',
              waitSeconds: 0,
              pendingPermission: null,
              pendingUserInput: null,
              agentInfo: null,
            }),
            running: false,
            lifecycleState: 'stopped',
            processing: false,
            readySinceLastSelect: false,
            processingStartedAtMs: null,
            attentionKind: null,
            pendingPermission: null,
            pendingUserInput: null,
          },
        },
      }))
      return true
    },

    // ---- Goal actions ----

    setGoal: async (chatId: string, objective: string, tokenBudget = 0): Promise<string | null> => {
      if (isCefContext()) {
        const response = await sendToCEF<{ goalId: string }>({
          action: 'setGoal',
          payload: { chatId, objective, tokenBudget },
          requestId: createRequestId('setGoal'),
        })
        if (response.ok && response.data?.goalId) {
          return response.data.goalId
        }
        return null
      }

      // Mock: create a local goal
      const goalId = `goal-${Date.now()}`
      const now = new Date()
      const newGoal: Goal = {
        id: goalId,
        chatId,
        objective,
        status: 'active',
        tokenBudget: tokenBudget || undefined,
        tokensUsed: 0,
        blockedTurnCount: 0,
        completedItems: [],
        remainingItems: [],
        currentStep: '',
        lastVerification: '',
        lastNextPrompt: '',
        sameNextPromptCount: 0,
        loopCount: 0,
        createdAt: now,
        updatedAt: now,
      }
      set((state: AppState) => ({
        goalsByChatId: {
          ...state.goalsByChatId,
          [chatId]: [...(state.goalsByChatId[chatId] ?? []), newGoal],
        },
        activeGoalIdByChatId: {
          ...state.activeGoalIdByChatId,
          [chatId]: goalId,
        },
      }))
      return goalId
    },

    updateGoalStatus: async (goalId: string, status: GoalStatus): Promise<boolean> => {
      if (isCefContext()) {
        const response = await sendToCEF({
          action: 'updateGoalStatus',
          payload: { goalId, status },
          requestId: createRequestId('updateGoalStatus'),
        })
        return response.ok
      }

      set((state: AppState) => {
        const nextActive: Record<string, string | null> = {}
        const nextGoals: Record<string, Goal[]> = {}
        const entries = Object.entries(state.goalsByChatId) as [string, Goal[]][]
        for (const [chatId, goals] of entries) {
          const updated = goals.map((g: Goal) =>
            g.id === goalId ? { ...g, status, updatedAt: new Date() } : g
          )
          if (updated !== goals) {
            nextGoals[chatId] = updated
            if (status === 'complete' || status === 'blocked' || status === 'paused') {
              if (state.activeGoalIdByChatId[chatId] === goalId) {
                nextActive[chatId] = null
              }
            }
          }
        }
        if (Object.keys(nextGoals).length === 0) return state
        return {
          goalsByChatId: { ...state.goalsByChatId, ...nextGoals },
          activeGoalIdByChatId: { ...state.activeGoalIdByChatId, ...nextActive },
        }
      })
      return true
    },

    removeGoal: async (goalId: string): Promise<boolean> => {
      if (isCefContext()) {
        const response = await sendToCEF({
          action: 'removeGoal',
          payload: { goalId },
          requestId: createRequestId('removeGoal'),
        })
        return response.ok
      }

      set((state: AppState) => {
        const nextActive: Record<string, string | null> = {}
        const nextGoals: Record<string, Goal[]> = {}
        const entries = Object.entries(state.goalsByChatId) as [string, Goal[]][]
        for (const [chatId, goals] of entries) {
          const filtered = goals.filter((g: Goal) => g.id !== goalId)
          if (filtered.length !== goals.length) {
            nextGoals[chatId] = filtered
            if (state.activeGoalIdByChatId[chatId] === goalId) {
              nextActive[chatId] = null
            }
          }
        }
        if (Object.keys(nextGoals).length === 0) return state
        return {
          goalsByChatId: { ...state.goalsByChatId, ...nextGoals },
          activeGoalIdByChatId: { ...state.activeGoalIdByChatId, ...nextActive },
        }
      })
      return true
    },

    resumeGoal: async (chatId: string, goalId: string): Promise<boolean> => {
      if (!goalId) return false
      if (isCefContext()) {
        const response = await sendToCEF({
          action: 'setActiveGoal',
          payload: { chatId, goalId },
          requestId: createRequestId('setActiveGoal'),
        })
        return response.ok
      }

      set((state: AppState) => {
        const goals = state.goalsByChatId[chatId] ?? []
        return {
          goalsByChatId: {
            ...state.goalsByChatId,
            [chatId]: goals.map((goal) =>
              goal.id === goalId
                ? { ...goal, status: 'active', blockedTurnCount: 0, lastBlocker: '', updatedAt: new Date() }
                : goal
            ),
          },
          activeGoalIdByChatId: {
            ...state.activeGoalIdByChatId,
            [chatId]: goalId,
          },
        }
      })
      return true
    },

    clearActiveGoal: async (chatId: string): Promise<boolean> => {
      // Clear active goal by setting with empty goalId
      if (isCefContext()) {
        const current = get()
        const currentActiveGoalId = current.activeGoalIdByChatId[chatId]
        if (!currentActiveGoalId) return true
        const response = await sendToCEF({
          action: 'setActiveGoal',
          payload: { chatId, goalId: '' },
          requestId: createRequestId('setActiveGoal'),
        })
        return response.ok
      }

      set((state: AppState) => ({
        activeGoalIdByChatId: {
          ...state.activeGoalIdByChatId,
          [chatId]: null,
        },
      }))
      return true
    },

    setGoalMode: (chatId: string, active: boolean) => {
      set((state: AppState) => ({
        goalModeByChatId: {
          ...state.goalModeByChatId,
          [chatId]: active,
        },
      }))
    },

    setDefaultGoalTokenBudget: (chatId: string, tokenBudget: number) => {
      set((state: AppState) => ({
        defaultGoalTokenBudgetByChatId: {
          ...state.defaultGoalTokenBudgetByChatId,
          [chatId]: Math.max(0, Math.floor(Number.isFinite(tokenBudget) ? tokenBudget : 0)),
        },
      }))
    },

    // ---- UI actions ----

    setTheme: (theme) => {
      const previousTheme = get().theme
      persistTheme(theme)
      set({ theme })
      if (isCefContext()) {
        const requestKey = 'setTheme'
        const requestId = createRequestId('setTheme')
        rememberPendingRequest(requestKey, requestId)
        sendToCEF({ action: 'setTheme', payload: { theme }, requestId }).then((resp) => {
          if (resp.ok) {
            clearPendingRequest(requestKey, resp.requestId)
            return
          }

          if (!isLatestPendingRequest(requestKey, resp.requestId)) {
            return
          }

          persistTheme(previousTheme)
          set({ theme: previousTheme })
          pendingRequestIdsByKey.delete(requestKey)
        })
      }
    },

    setNewChatModalOpen: (open, folderId) => set({
      isNewChatModalOpen: open,
      newChatFolderId: open ? (folderId ?? null) : null,
    }),
    setSettingsOpen: (open) => set({ isSettingsOpen: open }),
    setSidebarCollapsed: (collapsed) => set((state) => {
      const next = {
        sidebarCollapsed: collapsed,
        commitPanelOpen: state.commitPanelOpen,
        sidebarWidthPx: state.sidebarWidthPx,
        commitPanelWidthPx: state.commitPanelWidthPx,
      }
      writeStoredAppShellLayout(next)
      return { sidebarCollapsed: collapsed }
    }),
    setCommitPanelOpen: (open) => set((state) => {
      const next = {
        sidebarCollapsed: state.sidebarCollapsed,
        commitPanelOpen: open,
        sidebarWidthPx: state.sidebarWidthPx,
        commitPanelWidthPx: state.commitPanelWidthPx,
      }
      writeStoredAppShellLayout(next)
      return { commitPanelOpen: open }
    }),
    setSidebarWidthPx: (width) => set((state) => {
      const sidebarWidthPx = clampSidebarWidthPx(width)
      if (sidebarWidthPx === state.sidebarWidthPx) {
        return state
      }
      const next = {
        sidebarCollapsed: state.sidebarCollapsed,
        commitPanelOpen: state.commitPanelOpen,
        sidebarWidthPx,
        commitPanelWidthPx: state.commitPanelWidthPx,
      }
      writeStoredAppShellLayout(next)
      return { sidebarWidthPx }
    }),
    setCommitPanelWidthPx: (width) => set((state) => {
      const commitPanelWidthPx = clampCommitPanelWidthPx(width)
      if (commitPanelWidthPx === state.commitPanelWidthPx) {
        return state
      }
      const next = {
        sidebarCollapsed: state.sidebarCollapsed,
        commitPanelOpen: state.commitPanelOpen,
        sidebarWidthPx: state.sidebarWidthPx,
        commitPanelWidthPx,
      }
      writeStoredAppShellLayout(next)
      return { commitPanelWidthPx }
    }),
  }
})
