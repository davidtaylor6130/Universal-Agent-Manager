// Sanitiser and normaliser functions for raw C++ state payloads.
// Extracted from useAppStore.ts (MO-1). Pure functions — no side effects.

import type {
  AcpAgentInfo,
  AcpAttentionKind,
  AcpCommand,
  AcpConfigOption,
  AcpDiagnosticEntry,
  AcpMode,
  AcpModel,
  AcpPendingPermission,
  AcpPendingUserInput,
  AcpPermissionOption,
  AcpPlanEntry,
  AcpProviderUsage,
  AcpQueuedPrompt,
  AcpToolCall,
  AcpTurnEvent,
  AcpUserInputOption,
  AcpUserInputQuestion,
  CliVersionManager,
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
  McpServerConfiguration,
  CppStatePatch,
  EditorFileAssociation,
  GitWorktreeResult,
  GitWorktreeStatus,
  MemoryActivity,
  MemoryWorkerBinding,
  ProviderChatDefaults,
  ProviderModelCatalog,
  ShellAction,
  VcsCommitResult,
  VcsCommitStatus,
  VcsType,
} from './types'
import type { Attachment, MessageBlock } from '../../types/message'
import type { GoalStatus } from '../../types/goal'
import type { MemoryLevel } from '../../types/memory'
import type { ResourceCollection, ResourceReferenceType } from '../../types/resourceCollection'
import { normalizeStoredTheme } from '../../utils/themeStorage'
import {
  DEFAULT_PROVIDER_ID as GEMINI_CLI_PROVIDER_ID,
  normalizeCliProviderIdAlias,
  providerCapabilities,
} from '../../utils/providerMetadata'

// ---------------------------------------------------------------------------
// Memory settings constants (re-exported so useAppStore.ts and slices can use them)
// ---------------------------------------------------------------------------

export const DEFAULT_MEMORY_IDLE_DELAY_SECONDS = 60
export const MIN_MEMORY_IDLE_DELAY_SECONDS = 30
export const MAX_MEMORY_IDLE_DELAY_SECONDS = 3600
export const DEFAULT_MEMORY_RECALL_BUDGET_BYTES = 2048
export const MIN_MEMORY_RECALL_BUDGET_BYTES = 512
export const MAX_MEMORY_RECALL_BUDGET_BYTES = 8192
export const DEFAULT_GOAL_MAX_LOOP_ITERATIONS = 200
export const MIN_GOAL_MAX_LOOP_ITERATIONS = 1
export const MAX_GOAL_MAX_LOOP_ITERATIONS = 200
export const DEFAULT_ACP_SETUP_INACTIVITY_TIMEOUT_SECONDS = 600
export const MIN_ACP_SETUP_INACTIVITY_TIMEOUT_SECONDS = 60
export const MAX_ACP_SETUP_INACTIVITY_TIMEOUT_SECONDS = 3600
export const DEFAULT_ACP_TURN_OUTPUT_LIMIT_MIB = 1024
export const MIN_ACP_TURN_OUTPUT_LIMIT_MIB = 256
export const MAX_ACP_TURN_OUTPUT_LIMIT_MIB = 4096

export function normalizeGoalMaxLoopIterations(value: unknown): number {
  const parsed = Math.floor(finiteNumberOr(value, DEFAULT_GOAL_MAX_LOOP_ITERATIONS))
  return parsed <= 0 ? DEFAULT_GOAL_MAX_LOOP_ITERATIONS : Math.min(MAX_GOAL_MAX_LOOP_ITERATIONS, parsed)
}

export function normalizeAcpTurnOutputLimitMiB(value: unknown): number {
  const parsed = Math.floor(finiteNumberOr(value, DEFAULT_ACP_TURN_OUTPUT_LIMIT_MIB))
  return parsed <= 0
    ? DEFAULT_ACP_TURN_OUTPUT_LIMIT_MIB
    : Math.min(MAX_ACP_TURN_OUTPUT_LIMIT_MIB, Math.max(MIN_ACP_TURN_OUTPUT_LIMIT_MIB, parsed))
}

export function normalizeAcpSetupInactivityTimeoutSeconds(value: unknown): number {
  return clampedFiniteNumberOr(value, DEFAULT_ACP_SETUP_INACTIVITY_TIMEOUT_SECONDS, MIN_ACP_SETUP_INACTIVITY_TIMEOUT_SECONDS, MAX_ACP_SETUP_INACTIVITY_TIMEOUT_SECONDS)
}

export function normalizeMemoryLevel(value: unknown, legacyEnabled = true): MemoryLevel {
  if (!legacyEnabled) return 'off'
  const normalized = typeof value === 'string' ? value.trim().toLowerCase() : ''
  return normalized === 'off' || normalized === 'strict' || normalized === 'balanced' || normalized === 'open'
    ? normalized
    : 'strict'
}

// ---------------------------------------------------------------------------
// Primitive helpers
// ---------------------------------------------------------------------------

export function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null
}

export function isString(value: unknown): value is string {
  return typeof value === 'string'
}

export function stringOr(value: unknown, fallback = ''): string {
  return isString(value) ? value : fallback
}

export function finiteNumberOr(value: unknown, fallback: number): number {
  return typeof value === 'number' && Number.isFinite(value) ? value : fallback
}

export function clampedFiniteNumberOr(value: unknown, fallback: number, min: number, max: number): number {
  return Math.min(max, Math.max(min, finiteNumberOr(value, fallback)))
}

export function booleanOr(value: unknown, fallback = false): boolean {
  return typeof value === 'boolean' ? value : fallback
}

export const emptyMemoryActivity: MemoryActivity = {
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

// ---------------------------------------------------------------------------
// Sanitisers
// ---------------------------------------------------------------------------

export function sanitizeMemoryActivity(value: unknown, fallbackStatus = ''): MemoryActivity {
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

export function sanitizeAcpAttentionKind(value: unknown, fallback: AcpAttentionKind | null = null): AcpAttentionKind | null {
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

export function normalizeAcpModelId(value: unknown): string {
  const modelId = stringOr(value).trim()
  return isAllowedAcpModelId(modelId) ? modelId : ''
}

export function isAllowedAcpModelId(modelId: string): boolean {
  if (modelId === '') return true
  if (modelId.length > 160 || modelId.startsWith('-')) return false
  return /^[A-Za-z0-9._:/-]+$/.test(modelId)
}

export function normalizeCodexReasoningEffort(value: unknown): string {
  const effort = stringOr(value).trim().toLowerCase()
  return ['none', 'minimal', 'low', 'medium', 'high', 'xhigh', 'max', 'ultra'].includes(effort) ? effort : ''
}

export function normalizeCodexServiceTier(value: unknown): string {
  const tier = stringOr(value).trim().toLowerCase()
  return tier === 'fast' || tier === 'flex' ? tier : ''
}

export function normalizeAcpApprovalMode(value: unknown): string {
  const modeId = stringOr(value).trim() || 'default'
  if (modeId === 'yolo') return 'default'
  if (modeId === 'auto_edit') return 'acceptEdits'
  return (ACP_APPROVAL_MODE_IDS as readonly string[]).includes(modeId) ? modeId : 'default'
}

export function normalizeAgentMode(value: unknown): string {
  const modeId = stringOr(value).trim()
  return (AGENT_MODE_IDS as readonly string[]).includes(modeId) ? modeId : 'default'
}

export function normalizeCommandSafetyTier(value: unknown): 'off' | 'acceptEdits' | 'aiReview' | 'yolo' {
  const tier = stringOr(value).trim().toLowerCase()
  if (tier === 'acceptedits') return 'acceptEdits'
  if (tier === 'aireview' || tier === 'low' || tier === 'medium' || tier === 'high') return 'aiReview'
  return tier === 'yolo' ? 'yolo' : 'off'
}

export function sanitizePlanEntry(value: unknown): AcpPlanEntry | null {
  if (!isRecord(value)) return null
  return {
    content: stringOr(value.content),
    priority: stringOr(value.priority),
    status: stringOr(value.status),
  }
}

export function sanitizeToolCall(value: unknown): AcpToolCall | null {
  if (!isRecord(value)) return null
  const id = stringOr(value.id)
  if (!id) return null
  return {
    id,
    title: stringOr(value.title),
    kind: stringOr(value.kind),
    status: stringOr(value.status),
    content: stringOr(value.content),
    contentDeferred: booleanOr(value.contentDeferred),
    isSubAgent: booleanOr(value.isSubAgent),
    subAgentId: stringOr(value.subAgentId),
    subAgentTitle: stringOr(value.subAgentTitle),
  }
}

export function sanitizeTurnEvent(value: unknown): AcpTurnEvent | null {
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

export function sanitizeAttachment(value: unknown): Attachment | null {
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

export function messageAttachments(message: CppMessage): Attachment[] {
  const markdownStoreFiles = message.markdownStoreFiles ?? []
  if (markdownStoreFiles.length === 0) return message.attachments ?? []
  const markdownAttachments = markdownStoreFiles.map((filePath) => ({
    id: filePath,
    name: filePath.split(/[\\/]/).pop() || filePath,
    type: 'markdown-store',
    size: 0,
    path: filePath,
  }))
  return [...markdownAttachments, ...(message.attachments ?? [])]
}

export function sanitizeQueuedPrompt(value: unknown): AcpQueuedPrompt | null {
  if (!isRecord(value)) return null
  const text = stringOr(value.text).trim()
  if (!text) return null
  return {
    text,
    uamAgentId: stringOr(value.uamAgentId).trim() || 'build',
    markdownStoreFiles: Array.isArray(value.markdownStoreFiles)
      ? value.markdownStoreFiles.filter(isString)
      : [],
    attachments: Array.isArray(value.attachments)
      ? value.attachments.flatMap((attachment) => {
          const sanitized = sanitizeAttachment(attachment)
          return sanitized ? [sanitized] : []
        })
      : [],
    goalMode: Boolean(value.goalMode),
    goalId: stringOr(value.goalId),
    ...(value.computerUseMode === true ? { computerUseMode: true } : {}),
    prioritySteer: typeof value.prioritySteer === 'boolean' ? value.prioritySteer : undefined,
  }
}

export function sanitizeCppMessage(value: unknown): CppMessage | null {
  if (!isRecord(value)) return null
  const role = value.role
  if (role !== 'user' && role !== 'assistant' && role !== 'system') return null
  if (!isString(value.content)) return null

  return {
    role,
    content: value.content,
    modelId: isString(value.modelId) ? value.modelId : undefined,
    providerId: isString(value.providerId) ? value.providerId : undefined,
    thoughts: isString(value.thoughts) ? value.thoughts : undefined,
    planSummary: isString(value.planSummary) ? value.planSummary : undefined,
    planEntries: Array.isArray(value.planEntries) && value.planEntries.length > 0
      ? value.planEntries.flatMap((entry) => {
          const sanitized = sanitizePlanEntry(entry)
          return sanitized ? [sanitized] : []
        })
      : undefined,
    toolCalls: Array.isArray(value.toolCalls) && value.toolCalls.length > 0
      ? value.toolCalls.flatMap((toolCall) => {
          const sanitized = sanitizeToolCall(toolCall)
          return sanitized ? [sanitized] : []
        })
      : undefined,
    blocks: Array.isArray(value.blocks) && value.blocks.length > 0
      ? value.blocks.flatMap((block) => {
          const sanitized = sanitizeTurnEvent(block)
          return sanitized ? [sanitized as MessageBlock] : []
        })
      : undefined,
    markdownStoreFiles: Array.isArray(value.markdownStoreFiles) && value.markdownStoreFiles.length > 0
      ? value.markdownStoreFiles.filter(isString)
      : undefined,
    attachments: Array.isArray(value.attachments) && value.attachments.length > 0
      ? value.attachments.flatMap((attachment) => {
          const sanitized = sanitizeAttachment(attachment)
          return sanitized ? [sanitized] : []
      })
      : undefined,
    processingTimeMs: Math.max(0, finiteNumberOr(value.processingTimeMs, 0)),
		interrupted: booleanOr(value.interrupted),
		prioritySteer: booleanOr(value.prioritySteer),
    checkpointSha: isString(value.checkpointSha) ? value.checkpointSha : undefined,
    checkpointParentSha: isString(value.checkpointParentSha) ? value.checkpointParentSha : undefined,
    createdAt: stringOr(value.createdAt),
  }
}

export function sanitizeCppCliTerminal(value: unknown): CppChat['cliTerminal'] | undefined {
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
    pendingSteer: typeof value.pendingSteer === 'boolean' ? value.pendingSteer : undefined,
    lastError: stringOr(value.lastError),
  }
}

export function sanitizeDiagnostic(value: unknown): AcpDiagnosticEntry | null {
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

export function sanitizePermissionOption(value: unknown): AcpPermissionOption | null {
  if (!isRecord(value)) return null
  const id = stringOr(value.id)
  if (!id) return null
  return {
    id,
    name: stringOr(value.name),
    kind: stringOr(value.kind),
  }
}

export function sanitizePendingPermission(value: unknown): AcpPendingPermission | null {
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
    safetyRisk: value.safetyRisk === 'allowed' || value.safetyRisk === 'warn' || value.safetyRisk === 'warn_high' ? value.safetyRisk : undefined,
    safetyTier: value.safetyTier === 'low' || value.safetyTier === 'medium' || value.safetyTier === 'high' ? value.safetyTier : undefined,
    safetyRequiresApproval: booleanOr(value.safetyRequiresApproval),
    options: Array.isArray(value.options)
      ? value.options.flatMap((option) => {
          const sanitized = sanitizePermissionOption(option)
          return sanitized ? [sanitized] : []
        })
      : [],
  }
}

export function sanitizeUserInputOption(value: unknown): AcpUserInputOption | null {
  if (!isRecord(value)) return null
  return {
    label: stringOr(value.label),
    description: stringOr(value.description),
  }
}

export function sanitizeUserInputQuestion(value: unknown): AcpUserInputQuestion | null {
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

export function sanitizePendingUserInput(value: unknown): AcpPendingUserInput | null {
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

export function sanitizeAgentInfo(value: unknown): Partial<AcpAgentInfo> | undefined {
  if (!isRecord(value)) return undefined
  return {
    name: stringOr(value.name),
    title: stringOr(value.title),
    version: stringOr(value.version),
  }
}

export function sanitizeAcpMode(value: unknown): AcpMode | null {
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

export function sanitizeAcpCommand(value: unknown): AcpCommand | null {
  if (!isRecord(value)) return null
  const name = stringOr(value.name).trim().replace(/^\/+/, '')
  if (!name) return null
  return {
    name,
    description: stringOr(value.description).trim(),
    inputHint: stringOr(value.inputHint).trim(),
  }
}

export function sanitizeAcpModel(value: unknown): AcpModel | null {
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

export function sanitizeAcpConfigOption(value: unknown): AcpConfigOption | null {
  if (!isRecord(value)) return null
  const id = stringOr(value.id).trim().slice(0, 256)
  if (!id) return null
  return {
    id,
    name: stringOr(value.name, id).slice(0, 512),
    description: stringOr(value.description).slice(0, 1024),
    category: stringOr(value.category).slice(0, 256),
    currentValue: stringOr(value.currentValue).trim().slice(0, 512),
    options: Array.isArray(value.options)
      ? value.options.slice(0, 128).flatMap((choice) => {
          if (!isRecord(choice)) return []
          const choiceValue = stringOr(choice.value).trim().slice(0, 512)
          if (!choiceValue) return []
          return [{
            value: choiceValue,
            name: stringOr(choice.name, choiceValue).slice(0, 512),
            description: stringOr(choice.description).slice(0, 1024),
          }]
        })
      : [],
  }
}

function nonNegativeInteger(value: unknown): number | null {
  return typeof value === 'number' && Number.isSafeInteger(value) && value >= 0 ? value : null
}

function sanitizeTokenUsageBreakdown(value: unknown) {
  if (!isRecord(value)) return null
  const totalTokens = nonNegativeInteger(value.totalTokens)
  if (totalTokens === null) return null
  return {
    inputTokens: nonNegativeInteger(value.inputTokens) ?? 0,
    cachedInputTokens: nonNegativeInteger(value.cachedInputTokens) ?? 0,
    cacheWriteInputTokens: nonNegativeInteger(value.cacheWriteInputTokens) ?? 0,
    outputTokens: nonNegativeInteger(value.outputTokens) ?? 0,
    reasoningOutputTokens: nonNegativeInteger(value.reasoningOutputTokens) ?? 0,
    totalTokens,
  }
}

function sanitizeRateLimitWindow(value: unknown) {
  if (!isRecord(value)) return null
  const usedPercent = nonNegativeInteger(value.usedPercent)
  if (usedPercent === null || usedPercent > 100) return null
  return {
    usedPercent,
    resetsAt: nonNegativeInteger(value.resetsAt),
    windowDurationMinutes: nonNegativeInteger(value.windowDurationMinutes),
  }
}

function sanitizeCredits(value: unknown) {
  if (!isRecord(value) || typeof value.hasCredits !== 'boolean' || typeof value.unlimited !== 'boolean') return null
  return {
    hasCredits: value.hasCredits,
    unlimited: value.unlimited,
    balance: typeof value.balance === 'string' ? value.balance : null,
  }
}

function sanitizeSpendControlLimit(value: unknown) {
  if (!isRecord(value)) return null
  const remainingPercent = nonNegativeInteger(value.remainingPercent)
  const resetsAt = nonNegativeInteger(value.resetsAt)
  if (typeof value.limit !== 'string' || typeof value.used !== 'string' || remainingPercent === null || remainingPercent > 100 || resetsAt === null) return null
  return { limit: value.limit, used: value.used, remainingPercent, resetsAt }
}

function sanitizeProviderUsage(value: unknown): AcpProviderUsage | null {
  if (!isRecord(value)) return null

  let tokenUsage = null
  if (isRecord(value.tokenUsage)) {
    const updatedAt = nonNegativeInteger(value.tokenUsage.updatedAt)
    const total = sanitizeTokenUsageBreakdown(value.tokenUsage.total)
    const last = sanitizeTokenUsageBreakdown(value.tokenUsage.last)
    if (updatedAt !== null && total && last) {
      tokenUsage = {
        updatedAt,
        total,
        last,
        modelContextWindow: nonNegativeInteger(value.tokenUsage.modelContextWindow),
      }
    }
  }

  let rateLimits = null
  if (isRecord(value.rateLimits)) {
    const updatedAt = nonNegativeInteger(value.rateLimits.updatedAt)
    const limitId = stringOr(value.rateLimits.limitId)
    const limitName = stringOr(value.rateLimits.limitName)
    const primary = sanitizeRateLimitWindow(value.rateLimits.primary)
    const secondary = sanitizeRateLimitWindow(value.rateLimits.secondary)
    const credits = sanitizeCredits(value.rateLimits.credits)
    const individualLimit = sanitizeSpendControlLimit(value.rateLimits.individualLimit)
    const spendControlReached = typeof value.rateLimits.spendControlReached === 'boolean' ? value.rateLimits.spendControlReached : null
    const planType = stringOr(value.rateLimits.planType)
    const rateLimitReachedType = stringOr(value.rateLimits.rateLimitReachedType)
    if (updatedAt !== null && (limitId || limitName || primary || secondary || credits || individualLimit || spendControlReached !== null || planType || rateLimitReachedType)) {
      rateLimits = { updatedAt, limitId, limitName, primary, secondary, credits, individualLimit, spendControlReached, planType, rateLimitReachedType }
    }
  }

  return tokenUsage || rateLimits ? { tokenUsage, rateLimits } : null
}

export function sanitizeCppAcpSession(value: unknown): CppAcpSession | undefined {
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
    availableCommands: Array.isArray(value.availableCommands)
      ? value.availableCommands.flatMap((command) => {
          const sanitized = sanitizeAcpCommand(command)
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
    configOptions: Array.isArray(value.configOptions)
      ? value.configOptions.slice(0, 64).flatMap((option) => {
          const sanitized = sanitizeAcpConfigOption(option)
          return sanitized ? [sanitized] : []
        })
      : [],
    modelsLoading: booleanOr(value.modelsLoading),
    modelRefreshError: stringOr(value.modelRefreshError),
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
    queuedPrompts: Array.isArray(value.queuedPrompts)
      ? value.queuedPrompts.flatMap((prompt) => {
          const sanitized = sanitizeQueuedPrompt(prompt)
          return sanitized ? [sanitized] : []
        })
      : [],
    waitIsStale: Boolean(value.waitIsStale),
    waitStaleReason: stringOr(value.waitStaleReason),
    waitSeconds: finiteNumberOr(value.waitSeconds, 0),
    pendingPermission: sanitizePendingPermission(value.pendingPermission),
    pendingUserInput: sanitizePendingUserInput(value.pendingUserInput),
    providerUsage: sanitizeProviderUsage(value.providerUsage),
  }
}

export function sanitizeGoalStatus(value: unknown): GoalStatus {
  if (value === 'active' || value === 'complete' || value === 'blocked' || value === 'paused') return value
  return 'active'
}

export function sanitizeCppGoal(value: unknown): CppGoal | null {
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
		lastBlockerKind: isString(value.lastBlockerKind) ? value.lastBlockerKind : undefined,
    lastDiagnostic: isString(value.lastDiagnostic) ? value.lastDiagnostic : undefined,
    completedItems: Array.isArray(value.completedItems) ? value.completedItems.filter(isString) : undefined,
    remainingItems: Array.isArray(value.remainingItems) ? value.remainingItems.filter(isString) : undefined,
    currentStep: isString(value.currentStep) ? value.currentStep : undefined,
    lastVerification: isString(value.lastVerification) ? value.lastVerification : undefined,
    lastNextPrompt: isString(value.lastNextPrompt) ? value.lastNextPrompt : undefined,
    sameNextPromptCount: finiteNumberOr(value.sameNextPromptCount, 0),
    loopCount: finiteNumberOr(value.loopCount, 0),
    createdAt: stringOr(value.createdAt),
    updatedAt: stringOr(value.updatedAt),
	executionOwner: value.executionOwner === 'provider' ? 'provider' : 'uam',
	workerModelId: isString(value.workerModelId) ? value.workerModelId : '',
	reviewerModelId: isString(value.reviewerModelId) ? value.reviewerModelId : '',
	providerCommand: isString(value.providerCommand) ? value.providerCommand : '',
  }
}

export function sanitizeCppFolder(value: unknown): CppFolder | null {
  if (!isRecord(value)) return null
  const id = stringOr(value.id).trim()
  if (!id) return null
  return {
    id,
    title: stringOr(value.title, 'Untitled'),
    directory: stringOr(value.directory),
    collapsed: booleanOr(value.collapsed),
    executionHostId: stringOr(value.executionHostId).trim() || 'local',
    missing: booleanOr(value.missing),
  }
}

export function sanitizeCppChat(value: unknown): CppChat | null {
  if (!isRecord(value)) return null
  const id = stringOr(value.id).trim()
  if (!id) return null
  return {
    id,
    executionHostId: stringOr(value.executionHostId).trim() || 'local',
    title: stringOr(value.title, 'Untitled'),
    folderId: stringOr(value.folderId),
    pinned: booleanOr(value.pinned),
    providerId: stringOr(value.providerId, GEMINI_CLI_PROVIDER_ID),
    parentChatId: isString(value.parentChatId) ? value.parentChatId : undefined,
    branchRootChatId: isString(value.branchRootChatId) ? value.branchRootChatId : undefined,
    branchFromMessageIndex: Math.trunc(finiteNumberOr(value.branchFromMessageIndex, -1)),
    branchMessageEdited: booleanOr(value.branchMessageEdited),
    modelId: normalizeAcpModelId(value.modelId),
    reviewerModelId: normalizeAcpModelId(value.reviewerModelId),
    reasoningEffort: normalizeCodexReasoningEffort(value.reasoningEffort),
    serviceTier: normalizeCodexServiceTier(value.serviceTier),
    serviceTierExplicit: booleanOr(value.serviceTierExplicit, normalizeCodexServiceTier(value.serviceTier) !== ''),
    approvalMode: normalizeAgentMode(value.approvalMode),
    uamAgentId: stringOr(value.uamAgentId).trim() || 'build',
    uamControlEnabled: booleanOr(value.uamControlEnabled),
    commandSafetyTier: normalizeCommandSafetyTier(
      isString(value.commandSafetyTier)
        ? value.commandSafetyTier
        : booleanOr(value.autoApproveCommands) || stringOr(value.approvalMode).trim() === 'yolo' ? 'yolo' : 'off'
    ),
    computerUseEnabled: booleanOr(value.computerUseEnabled),
    computerUseBackend: value.computerUseBackend === 'provider' || value.computerUseBackend === 'uam' ? value.computerUseBackend : 'auto',
    computerUseEffectiveBackend: value.computerUseEffectiveBackend === 'provider' ? 'provider' : 'uam',
    computerUseProviderAvailable: booleanOr(value.computerUseProviderAvailable),
    computerUseTargetKind: value.computerUseTargetKind === 'screen' ? 'screen' : 'window',
    computerUseTargetId: stringOr(value.computerUseTargetId),
    computerUseTargetTitle: stringOr(value.computerUseTargetTitle),
    computerUseTargetInputMode: value.computerUseTargetInputMode === 'background' ? 'background' : 'foreground',
    computerUse: isRecord(value.computerUse)
      ? {
          enabled: booleanOr(value.computerUse.enabled),
          state: value.computerUse.state === 'paused' || value.computerUse.state === 'stopped' ? value.computerUse.state : 'running',
          history: Array.isArray(value.computerUse.history)
            ? value.computerUse.history.flatMap((entry) => isRecord(entry) ? [{
                time: stringOr(entry.time),
                action: stringOr(entry.action),
                status: stringOr(entry.status),
                detail: stringOr(entry.detail),
              }] : [])
            : [],
        }
      : undefined,
    memoryEnabled: normalizeMemoryLevel(value.memoryLevel, booleanOr(value.memoryEnabled, true)) !== 'off',
    memoryLevel: normalizeMemoryLevel(value.memoryLevel, booleanOr(value.memoryEnabled, true)),
    smallModelMode: booleanOr(value.smallModelMode),
    memoryLastProcessedMessageCount: finiteNumberOr(value.memoryLastProcessedMessageCount, 0),
    memoryLastProcessedAt: isString(value.memoryLastProcessedAt) ? value.memoryLastProcessedAt : undefined,
    workspaceDirectory: isString(value.workspaceDirectory) ? value.workspaceDirectory : undefined,
    workspaceIsolationKind: isString(value.workspaceIsolationKind) ? value.workspaceIsolationKind : undefined,
    workspaceSourceDirectory: isString(value.workspaceSourceDirectory) ? value.workspaceSourceDirectory : undefined,
    workspaceBaseRef: isString(value.workspaceBaseRef) ? value.workspaceBaseRef : undefined,
    workspaceBranchName: isString(value.workspaceBranchName) ? value.workspaceBranchName : undefined,
    workspaceWorktreeDirectory: isString(value.workspaceWorktreeDirectory) ? value.workspaceWorktreeDirectory : undefined,
    importedReadOnly: booleanOr(value.importedReadOnly),
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

export function sanitizeCppProvider(value: unknown): CppProvider | null {
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
    structuredPermissionControl: value.structuredPermissionControl === 'uam' ? 'uam' : 'provider',
    terminalPermissionControl: 'provider',
    npmPackageName: isString(value.npmPackageName) ? value.npmPackageName.trim() : undefined,
	nativeGoalCommand: isString(value.nativeGoalCommand) ? value.nativeGoalCommand.trim() : undefined,
  }
}

export function sanitizeCliDebugTerminal(value: unknown): CppCliDebugTerminal | null {
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

export function sanitizeCliDebugState(value: unknown): CppCliDebugState | undefined {
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

export function sanitizeGitWorktreeStatus(value: unknown): GitWorktreeStatus | null {
  if (!isRecord(value)) return null
  return {
    isGitRepository: booleanOr(value.isGitRepository),
    isSvnWorkspace: booleanOr(value.isSvnWorkspace),
	managedRepository: booleanOr(value.managedRepository),
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

export function sanitizeGitWorktreeResult(value: unknown): GitWorktreeResult {
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

export function failedGitWorktreeResult(message: string): GitWorktreeResult {
  return { ok: false, message, patchPath: '' }
}

export function cefPayloadOrRawResponse<T>(response: { data?: T }): unknown {
  return response.data ?? response
}

export const ACP_APPROVAL_MODE_IDS = ['default', 'acceptEdits', 'plan'] as const
export const AGENT_MODE_IDS = ['default', 'plan'] as const

export function sanitizeVcsType(value: unknown): VcsType {
  return value === 'svn' ? 'svn' : 'git'
}

export function sanitizeVcsCommitStatus(value: unknown): VcsCommitStatus | null {
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
                contentFingerprint: stringOr(file.contentFingerprint),
              }]
            : []
        })
      : [],
    lineStatsReady: typeof value.lineStatsReady === 'boolean' ? value.lineStatsReady : true,
    warning: stringOr(value.warning),
    error: stringOr(value.error),
  }
}

export function sanitizeVcsCommitResult(value: unknown): VcsCommitResult {
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

// ---------------------------------------------------------------------------
// CLI version manager sanitisers
// ---------------------------------------------------------------------------

export const emptyCliVersionProviderState: CliVersionProviderState = {
  providerId: GEMINI_CLI_PROVIDER_ID,
  installedVersion: '',
  selectedVersion: '',
  availableVersions: [],
  preferredVersion: '',
  status: 'unknown',
  message: '',
  running: false,
  installMethod: 'npm',
  lastInstallStatus: 'none',
  lastCommand: '',
  lastOutput: '',
}

export const emptyCliVersionManager: CliVersionManager = {
  providers: [],
}

export function sanitizeCliVersionProviderState(value: unknown): CliVersionProviderState | null {
  if (!isRecord(value)) return null
  const providerId = stringOr(value.providerId, GEMINI_CLI_PROVIDER_ID).trim()
  if (!providerId) return null
  const status = stringOr(value.status)
  const normalizedStatus: CliVersionProviderState['status'] =
    status === 'checking' ||
    status === 'installing' ||
    status === 'verified' ||
    status === 'untested' ||
    status === 'untested-newer' ||
    status === 'known-incompatible' ||
    status === 'unavailable' ||
    status === 'provider-managed' ||
    status === 'unknown'
      ? status
      : 'unknown'
  const installMethod = stringOr(value.installMethod)
  const normalizedInstallMethod: NonNullable<CliVersionProviderState['installMethod']> =
    installMethod === 'homebrew-formula' || installMethod === 'homebrew-cask' || installMethod === 'winget'
      ? installMethod
      : 'npm'
  const lastInstallStatus = stringOr(value.lastInstallStatus)
  const normalizedLastInstallStatus: NonNullable<CliVersionProviderState['lastInstallStatus']> =
    lastInstallStatus === 'running' ||
    lastInstallStatus === 'succeeded' ||
    lastInstallStatus === 'failed'
      ? lastInstallStatus
      : 'none'

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
    verifiedVersion: stringOr(value.verifiedVersion),
    verifiedAt: stringOr(value.verifiedAt),
    status: normalizedStatus,
    message: stringOr(value.message),
    running: booleanOr(value.running),
    installMethod: normalizedInstallMethod,
    lastInstallStatus: normalizedLastInstallStatus,
    lastCommand: stringOr(value.lastCommand),
    lastOutput: stringOr(value.lastOutput),
  }
}

export function sanitizeCliVersionManager(value: unknown): CliVersionManager | undefined {
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

export function upsertCliVersionProviderState(
  providers: CliVersionProviderState[],
  next: CliVersionProviderState
): CliVersionProviderState[] {
  const found = providers.some((provider) => provider.providerId === next.providerId)
  if (!found) return [...providers, next]
  return providers.map((provider) => provider.providerId === next.providerId ? next : provider)
}

// ---------------------------------------------------------------------------
// Editor settings sanitisers
// ---------------------------------------------------------------------------

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

export function defaultEditorFileAssociations(): EditorFileAssociation[] {
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

export function sanitizeEditorPresetId(value: unknown) {
  const preset = stringOr(value, 'vscode').trim()
  return EDITOR_PRESET_IDS.includes(preset) ? preset : 'vscode'
}

export function sanitizeEditorFileAssociations(value: unknown): EditorFileAssociation[] {
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

// ---------------------------------------------------------------------------
// Provider chat defaults sanitisers
// ---------------------------------------------------------------------------

export function sanitizeProviderChatDefaults(value: unknown): ProviderChatDefaults {
  if (!isRecord(value)) {
    return {
      modelId: '',
      approvalMode: 'default',
      commandSafetyTier: 'off',
      memoryEnabled: true,
      memoryLevel: 'strict',
      smallModelMode: false,
      reasoningEffort: '',
      serviceTier: '',
    }
  }
  return {
    modelId: normalizeAcpModelId(value.modelId),
    reviewerModelId: normalizeAcpModelId(value.reviewerModelId),
    featurePreference: value.featurePreference === 'provider' ? 'provider' : 'uam',
    approvalMode: normalizeAcpApprovalMode(value.approvalMode),
    commandSafetyTier: normalizeCommandSafetyTier(
      isString(value.commandSafetyTier) ? value.commandSafetyTier : booleanOr(value.autoApproveCommands) ? 'yolo' : 'off'
    ),
    memoryEnabled: normalizeMemoryLevel(value.memoryLevel, booleanOr(value.memoryEnabled, true)) !== 'off',
    memoryLevel: normalizeMemoryLevel(value.memoryLevel, booleanOr(value.memoryEnabled, true)),
    smallModelMode: booleanOr(value.smallModelMode),
    reasoningEffort: normalizeCodexReasoningEffort(value.reasoningEffort),
    serviceTier: normalizeCodexServiceTier(value.serviceTier),
  }
}

export function sanitizeProviderChatDefaultsMap(value: unknown): Record<string, ProviderChatDefaults> {
  const defaults: Record<string, ProviderChatDefaults> = {}
  if (!isRecord(value)) return defaults
  for (const [providerId, providerDefaults] of Object.entries(value)) {
    const id = normalizeCliProviderIdAlias(providerId)
    if (!id) continue
    defaults[id] = sanitizeProviderChatDefaults(providerDefaults)
  }
  return defaults
}

export function providerChatDefaultsForNewChat(
  state: {
    providerChatDefaults: Record<string, ProviderChatDefaults>
    memoryEnabledDefault: boolean
    memoryLevelDefault: MemoryLevel
  },
  providerId: string
): ProviderChatDefaults {
  const saved = state.providerChatDefaults[providerId]
  const defaults = sanitizeProviderChatDefaults(saved ?? null)
  if (!saved) {
    defaults.memoryLevel = normalizeMemoryLevel(state.memoryLevelDefault, state.memoryEnabledDefault)
    defaults.memoryEnabled = defaults.memoryLevel !== 'off'
  }
  const caps = providerCapabilities(providerId)
  if (!caps.hasReasoningEffort) {
    defaults.reasoningEffort = ''
  }
  if (!caps.hasServiceTier) {
    defaults.serviceTier = ''
  }
  return defaults
}

// ---------------------------------------------------------------------------
// Settings sanitiser
// ---------------------------------------------------------------------------

export function sanitizeCppSettings(value: unknown): CppSettings {
  if (!isRecord(value)) {
    return {
      activeProviderId: GEMINI_CLI_PROVIDER_ID,
      theme: 'focus',
      showProviderIconsInSidebar: true,
      showWorktreePathInSidebar: true,
      memoryEnabledDefault: true,
      memoryLevelDefault: 'strict',
      memoryIdleDelaySeconds: DEFAULT_MEMORY_IDLE_DELAY_SECONDS,
      memoryRecallBudgetBytes: DEFAULT_MEMORY_RECALL_BUDGET_BYTES,
      goalMaxLoopIterations: DEFAULT_GOAL_MAX_LOOP_ITERATIONS,
      acpSetupInactivityTimeoutSeconds: DEFAULT_ACP_SETUP_INACTIVITY_TIMEOUT_SECONDS,
      acpTurnOutputLimitMiB: DEFAULT_ACP_TURN_OUTPUT_LIMIT_MIB,
      updateChecksEnabled: true,
      updateLastCheckedAt: '',
      dismissedUpdateVersions: {},
      memoryLastStatus: '',
      memoryWorkerBindings: {},
      permissionReviewerProviderId: '',
      permissionReviewerModelId: '',
      defaultNewChatProviderId: GEMINI_CLI_PROVIDER_ID,
      providerChatDefaults: {},
      markdownStoreDirectory: '',
      defaultEditorPresetId: 'vscode',
      editorFileAssociations: defaultEditorFileAssociations(),
      mcpServers: [],
      executionHosts: [{
        id: 'local', label: 'This computer', transport: 'local', sshAlias: '',
        runnerStatus: 'ready', runnerVersion: '', platform: '', architecture: '', lastSeenAt: '',
      }],
      favoriteUamAgentIds: [],
      uamAgentCycleShortcut: 'shift+tab',
    }
  }

  const theme = normalizeStoredTheme(value.theme) ?? 'focus'
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
  const dismissedUpdateVersions: Record<string, string> = {}
  if (isRecord(value.dismissedUpdateVersions)) {
    for (const [id, version] of Object.entries(value.dismissedUpdateVersions)) {
      if (typeof version === 'string' && id.trim() && version.trim()) {
        dismissedUpdateVersions[id.trim()] = version.trim()
      }
    }
  }
  const mcpServers: McpServerConfiguration[] = Array.isArray(value.mcpServers)
    ? value.mcpServers.flatMap((entry) => {
        if (!isRecord(entry)) return []
        const transport = stringOr(entry.transport)
        if (transport !== 'stdio' && transport !== 'http' && transport !== 'sse') return []
        const references = (input: unknown) => Array.isArray(input)
          ? input.flatMap((reference) => isRecord(reference)
            ? [{ name: stringOr(reference.name), environmentVariable: stringOr(reference.environmentVariable) }]
            : [])
          : []
        return [{
          id: stringOr(entry.id),
          name: stringOr(entry.name),
          workspaceDirectory: stringOr(entry.workspaceDirectory),
          transport,
          command: stringOr(entry.command),
		  args: Array.isArray(entry.args) ? entry.args.filter((arg): arg is string => typeof arg === 'string') : [],
          url: stringOr(entry.url),
          environment: references(entry.environment),
          headers: references(entry.headers),
          enabled: booleanOr(entry.enabled, true),
        }]
      })
    : []
  const favoriteUamAgentIds = Array.isArray(value.favoriteUamAgentIds)
    ? Array.from(new Set(value.favoriteUamAgentIds
        .filter((id): id is string => typeof id === 'string')
        .map((id) => id.trim().toLowerCase())
        .filter((id) => id !== 'build' && id !== 'plan' && /^[a-z0-9](?:[a-z0-9-]{0,62}[a-z0-9])?$/.test(id))))
        .slice(0, 64)
    : []
  const executionHosts: NonNullable<CppSettings['executionHosts']> = [{
    id: 'local', label: 'This computer', transport: 'local' as const, sshAlias: '',
    runnerStatus: 'ready' as const, runnerVersion: '', platform: '', architecture: '', lastSeenAt: '',
  }]
  const seenExecutionHostIds = new Set(['local'])
  if (Array.isArray(value.executionHosts)) {
    for (const entry of value.executionHosts) {
      if (!isRecord(entry)) continue
      const id = stringOr(entry.id).trim()
      const sshAlias = stringOr(entry.sshAlias).trim()
      if (!/^[A-Za-z0-9_-]{1,64}$/.test(id) || id === 'local' ||
          !/^[A-Za-z0-9][A-Za-z0-9._-]{0,254}$/.test(sshAlias) || seenExecutionHostIds.has(id)) continue
      seenExecutionHostIds.add(id)
      const runnerStatus = ['uninstalled', 'installing', 'ready', 'offline', 'error'].includes(stringOr(entry.runnerStatus))
        ? stringOr(entry.runnerStatus) as 'uninstalled' | 'installing' | 'ready' | 'offline' | 'error'
        : 'uninstalled'
      executionHosts.push({
        id,
        label: stringOr(entry.label).trim() || sshAlias,
        transport: 'ssh',
        sshAlias,
        runnerStatus,
        runnerVersion: stringOr(entry.runnerVersion),
        platform: stringOr(entry.platform),
        architecture: stringOr(entry.architecture),
        lastSeenAt: stringOr(entry.lastSeenAt),
        runnerDirectory: stringOr(entry.runnerDirectory),
        runnerProtocolVersion: finiteNumberOr(entry.runnerProtocolVersion, 0),
      })
    }
  }
  const requestedUamAgentCycleShortcut = stringOr(value.uamAgentCycleShortcut).toLowerCase()
  const uamAgentCycleShortcut = ['shift+tab', 'control+shift+tab', 'alt+shift+tab', 'meta+shift+tab', 'disabled'].includes(requestedUamAgentCycleShortcut)
    ? requestedUamAgentCycleShortcut as CppSettings['uamAgentCycleShortcut']
    : 'shift+tab'
  return {
    activeProviderId: stringOr(value.activeProviderId, GEMINI_CLI_PROVIDER_ID),
    theme,
    showProviderIconsInSidebar: booleanOr(value.showProviderIconsInSidebar, true),
    showWorktreePathInSidebar: booleanOr(value.showWorktreePathInSidebar, true),
    memoryEnabledDefault: normalizeMemoryLevel(value.memoryLevelDefault, booleanOr(value.memoryEnabledDefault, true)) !== 'off',
    memoryLevelDefault: normalizeMemoryLevel(value.memoryLevelDefault, booleanOr(value.memoryEnabledDefault, true)),
    memoryIdleDelaySeconds: clampedFiniteNumberOr(value.memoryIdleDelaySeconds, DEFAULT_MEMORY_IDLE_DELAY_SECONDS, MIN_MEMORY_IDLE_DELAY_SECONDS, MAX_MEMORY_IDLE_DELAY_SECONDS),
    memoryRecallBudgetBytes: clampedFiniteNumberOr(value.memoryRecallBudgetBytes, DEFAULT_MEMORY_RECALL_BUDGET_BYTES, MIN_MEMORY_RECALL_BUDGET_BYTES, MAX_MEMORY_RECALL_BUDGET_BYTES),
    goalMaxLoopIterations: normalizeGoalMaxLoopIterations(value.goalMaxLoopIterations),
    acpSetupInactivityTimeoutSeconds: normalizeAcpSetupInactivityTimeoutSeconds(value.acpSetupInactivityTimeoutSeconds),
    acpTurnOutputLimitMiB: normalizeAcpTurnOutputLimitMiB(value.acpTurnOutputLimitMiB),
    updateChecksEnabled: booleanOr(value.updateChecksEnabled, true),
    updateLastCheckedAt: stringOr(value.updateLastCheckedAt),
    dismissedUpdateVersions,
    memoryLastStatus: stringOr(value.memoryLastStatus),
    memoryWorkerBindings: bindings,
    permissionReviewerProviderId: stringOr(value.permissionReviewerProviderId),
    permissionReviewerModelId: stringOr(value.permissionReviewerModelId),
    defaultNewChatProviderId: stringOr(value.defaultNewChatProviderId, stringOr(value.activeProviderId, GEMINI_CLI_PROVIDER_ID)),
    providerChatDefaults: sanitizeProviderChatDefaultsMap(value.providerChatDefaults),
    markdownStoreDirectory: stringOr(value.markdownStoreDirectory),
    defaultEditorPresetId: sanitizeEditorPresetId(value.defaultEditorPresetId),
    editorFileAssociations: sanitizeEditorFileAssociations(value.editorFileAssociations),
    mcpServers,
    executionHosts,
    favoriteUamAgentIds,
    uamAgentCycleShortcut,
  }
}

// ---------------------------------------------------------------------------
// Top-level app-state and patch sanitisers
// ---------------------------------------------------------------------------

export function sanitizeCppAppState(value: unknown): CppAppState | null {
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
  const resourceCollections = sanitizeResourceCollections(value.resourceCollections)
	const providerModelCatalogs = sanitizeProviderModelCatalogs(value.providerModelCatalogs)

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
  const shellActions = sanitizeShellActions(value.shellActions)

  return {
    stateRevision: finiteNumberOr(value.stateRevision, 0),
    appVersion: stringOr(value.appVersion),
    runnerProtocolVersion: finiteNumberOr(value.runnerProtocolVersion, 0),
    folders,
    resourceCollections,
    chats,
    cliDebug: sanitizeCliDebugState(value.cliDebug),
    selectedChatId,
    selectedChatIndex: finiteNumberOr(value.selectedChatIndex, -1),
    providers,
    providerModelCatalogs,
    settings,
    memoryActivity: sanitizeMemoryActivity(value.memoryActivity, settings.memoryLastStatus),
    cliVersionManager: sanitizeCliVersionManager(value.cliVersionManager),
    shellActions,
    shellActionNotification: stringOr(value.shellActionNotification),
    statusLine: stringOr(value.statusLine),
  }
}

function sanitizeProviderModelCatalogs(value: unknown): ProviderModelCatalog[] {
  if (!Array.isArray(value)) return []
  return value.slice(0, 512).flatMap((entry) => {
    if (!isRecord(entry)) return []
    const providerId = normalizeCliProviderIdAlias(stringOr(entry.providerId))
    const workspaceDirectory = stringOr(entry.workspaceDirectory).trim()
    if (!providerId || !workspaceDirectory) return []
    return [{
      providerId,
      workspaceDirectory,
	  executionHostId: stringOr(entry.executionHostId, 'local').trim() || 'local',
      availableModels: Array.isArray(entry.availableModels)
        ? entry.availableModels.flatMap((model) => {
            const sanitized = sanitizeAcpModel(model)
            return sanitized ? [sanitized] : []
          })
        : [],
	  configOptions: Array.isArray(entry.configOptions)
		? entry.configOptions.flatMap((option) => {
			const sanitized = sanitizeAcpConfigOption(option)
			return sanitized ? [sanitized] : []
		  })
		: [],
      currentModelId: normalizeAcpModelId(entry.currentModelId),
      modelsLoading: booleanOr(entry.modelsLoading),
      modelRefreshError: stringOr(entry.modelRefreshError),
    }]
  })
}

function sanitizeShellActions(value: unknown): ShellAction[] {
  if (!Array.isArray(value)) return []
  return value.flatMap((entry) => {
    if (!isRecord(entry)) return []
    const id = stringOr(entry.id).trim()
    const label = stringOr(entry.label).trim()
    if (!id || !label) return []
    return [{
      id,
      label,
      skillPath: stringOr(entry.skillPath),
      providerId: normalizeCliProviderIdAlias(stringOr(entry.providerId)),
      modelId: normalizeAcpModelId(entry.modelId),
      groupPath: Array.isArray(entry.groupPath)
        ? entry.groupPath.map((segment) => stringOr(segment).trim()).filter(Boolean)
        : [],
      acceptsFiles: booleanOr(entry.acceptsFiles, true),
      acceptsFolders: booleanOr(entry.acceptsFolders, true),
      enabled: booleanOr(entry.enabled, true),
      openWorkspace: booleanOr(entry.openWorkspace, false),
    }]
  })
}

export function sanitizeMessagesByChatId(value: unknown): Record<string, CppMessage[]> | undefined {
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

const RESOURCE_REFERENCE_TYPES = new Set<ResourceReferenceType>([
  'workspace-folder', 'chat', 'file', 'website', 'desktop-app',
])

function sanitizeResourceCollections(value: unknown): ResourceCollection[] {
  if (!Array.isArray(value)) return []
  return value.flatMap((entry) => {
    if (!isRecord(entry)) return []
    const id = stringOr(entry.id).trim()
    const name = stringOr(entry.name).trim()
    if (!id || !name) return []
    const references = Array.isArray(entry.references)
      ? entry.references.flatMap((item) => {
          if (!isRecord(item)) return []
          const referenceId = stringOr(item.id).trim()
          const type = stringOr(item.type).trim() as ResourceReferenceType
          const target = stringOr(item.target).trim()
          if (!referenceId || !target || !RESOURCE_REFERENCE_TYPES.has(type)) return []
          return [{ id: referenceId, type, target, label: stringOr(item.label).trim() }]
        })
      : []
    return [{ id, name, collapsed: booleanOr(entry.collapsed, false), references }]
  })
}

export function sanitizeCppStatePatch(value: unknown): CppStatePatch | null {
  if (!isRecord(value)) return null

  return {
    stateRevision: finiteNumberOr(value.stateRevision, 0),
    folders: Array.isArray(value.folders)
      ? value.folders.flatMap((folder) => {
          const sanitized = sanitizeCppFolder(folder)
          return sanitized ? [sanitized] : []
        })
      : undefined,
    resourceCollections: value.resourceCollections !== undefined
      ? sanitizeResourceCollections(value.resourceCollections)
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
    providerModelCatalogs: value.providerModelCatalogs !== undefined
      ? sanitizeProviderModelCatalogs(value.providerModelCatalogs)
      : undefined,
    settings: isRecord(value.settings) ? sanitizeCppSettings(value.settings) : undefined,
    memoryActivity: value.memoryActivity !== undefined ? sanitizeMemoryActivity(value.memoryActivity) : undefined,
    cliVersionManager: value.cliVersionManager !== undefined ? sanitizeCliVersionManager(value.cliVersionManager) : undefined,
    shellActions: value.shellActions !== undefined ? sanitizeShellActions(value.shellActions) : undefined,
    shellActionNotification: value.shellActionNotification !== undefined ? stringOr(value.shellActionNotification) : undefined,
    statusLine: value.statusLine !== undefined ? stringOr(value.statusLine) : undefined,
  }
}
