// Equivalence checkers, binding builders, message reconciler, and request-clear
// helpers. Extracted from useAppStore.ts (MO-1). Module-level mutable state
// (pendingProviderChatDefaults, lastPushStatusUpdateAtMs) is exported for use
// by the push deserializer.

import type { Session, Folder } from '../../types/session'
import type { Attachment, Message, MessageBlock } from '../../types/message'
import type { Provider } from '../../types/provider'
import {
  DEFAULT_PROVIDER_ID as GEMINI_CLI_PROVIDER_ID,
  CODEX_CLI_PROVIDER_ID,
  COPILOT_CLI_PROVIDER_ID,
  normalizeCliProviderIdAlias,
} from '../../utils/providerMetadata'
import {
  messageAttachments,
  normalizeAcpApprovalMode,
  normalizeCommandSafetyTier,
  normalizeAcpModelId,
  normalizeCodexReasoningEffort,
  normalizeCodexServiceTier,
  normalizeMemoryLevel,
} from './sanitizers'
import {
  pendingCodexOptionsByChatId,
  pendingModelByChatId,
  pendingRequestIdsByKey,
} from '../push/pushBuffers'
import type {
  AcpAgentInfo,
  AcpBinding,
  AcpCommand,
  AcpDiagnosticEntry,
  AcpLifecycleState,
  AcpMode,
  AcpModel,
  AcpPendingPermission,
  AcpPendingUserInput,
  AcpPlanEntry,
  AcpToolCall,
  AcpTurnEvent,
  CliBinding,
  CliLifecycleState,
  CliTranscript,
  CppAppState,
  CppChat,
  CppFolder,
  CppMessage,
  CppProvider,
  CppStatePatch,
} from './types'

export function cppStateRevision(state: CppAppState): number {
  return typeof state.stateRevision === 'number' && Number.isFinite(state.stateRevision)
    ? state.stateRevision
    : 0
}

export function cppPatchRevision(patch: CppStatePatch): number {
  return typeof patch.stateRevision === 'number' && Number.isFinite(patch.stateRevision)
    ? patch.stateRevision
    : 0
}

export function normalizeProviderIdForVisibleProviders(
  providerId: string | undefined,
  providers: Array<{ id: string }>
): string {
  const requestedProviderId = normalizeCliProviderIdAlias(providerId ?? '') || GEMINI_CLI_PROVIDER_ID
  if (providers.some((provider) => provider.id === requestedProviderId)) {
    return requestedProviderId
  }
  return providers[0]?.id ?? GEMINI_CLI_PROVIDER_ID
}

export function foldersEquivalent(previous: Folder, next: Folder): boolean {
  return previous.name === next.name &&
    previous.directory === next.directory &&
    previous.isExpanded === next.isExpanded &&
    previous.missing === next.missing
}

export function folderFromCppFolder(folder: CppFolder, previous: Folder | undefined): Folder {
  const nextFolder: Folder = {
    id: folder.id,
    name: folder.title,
    parentId: null,
    directory: folder.directory ?? '',
    isExpanded: !folder.collapsed,
    missing: folder.missing,
    createdAt: previous?.createdAt ?? new Date(),
  }

  return previous && foldersEquivalent(previous, nextFolder) ? previous : nextFolder
}

export function providersEquivalent(previous: Provider, next: Provider): boolean {
  return previous.id === next.id &&
    previous.name === next.name &&
    previous.shortName === next.shortName &&
    previous.color === next.color &&
    previous.description === next.description &&
    previous.outputMode === next.outputMode &&
    previous.supportsCli === next.supportsCli &&
    previous.supportsStructured === next.supportsStructured &&
    previous.structuredProtocol === next.structuredProtocol &&
	previous.npmPackageName === next.npmPackageName &&
	previous.nativeGoalCommand === next.nativeGoalCommand
}

export function providerFromCppProvider(provider: CppProvider, previous: Provider | undefined): Provider {
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
	  nativeGoalCommand: provider.nativeGoalCommand,
  }

  if (previous) {
    return providersEquivalent(previous, nextProvider) ? previous : nextProvider
  }

  return nextProvider
}

export function sessionsEquivalent(previous: Session, next: Session): boolean {
  return previous.name === next.name &&
    previous.folderId === next.folderId &&
    (previous.isPinned ?? false) === (next.isPinned ?? false) &&
    (previous.providerId ?? GEMINI_CLI_PROVIDER_ID) === next.providerId &&
    (previous.parentChatId ?? '') === (next.parentChatId ?? '') &&
    (previous.branchRootChatId ?? previous.id) === (next.branchRootChatId ?? next.id) &&
    (previous.branchFromMessageIndex ?? -1) === (next.branchFromMessageIndex ?? -1) &&
    (previous.branchMessageEdited ?? false) === (next.branchMessageEdited ?? false) &&
    (previous.modelId ?? '') === (next.modelId ?? '') &&
    (previous.reasoningEffort ?? '') === (next.reasoningEffort ?? '') &&
    (previous.serviceTier ?? '') === (next.serviceTier ?? '') &&
    (previous.approvalMode ?? 'default') === next.approvalMode &&
    (previous.autoApproveCommands ?? false) === (next.autoApproveCommands ?? false) &&
    (previous.commandSafetyTier ?? 'medium') === (next.commandSafetyTier ?? 'medium') &&
    (previous.memoryEnabled ?? true) === next.memoryEnabled &&
    (previous.memoryLevel ?? ((previous.memoryEnabled ?? true) ? 'strict' : 'off')) === next.memoryLevel &&
    (previous.smallModelMode ?? false) === (next.smallModelMode ?? false) &&
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

export function sessionFromCppChat(
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
    parentChatId: chat.parentChatId ?? '',
    branchRootChatId: chat.branchRootChatId || chat.id,
    branchFromMessageIndex: chat.branchFromMessageIndex ?? -1,
    branchMessageEdited: chat.branchMessageEdited ?? false,
    modelId: chat.modelId ?? '',
    reasoningEffort: normalizeCodexReasoningEffort(chat.reasoningEffort),
    serviceTier: normalizeCodexServiceTier(chat.serviceTier),
    approvalMode: normalizeAcpApprovalMode(chat.approvalMode),
    autoApproveCommands: chat.autoApproveCommands ?? false,
    commandSafetyTier: normalizeCommandSafetyTier(chat.commandSafetyTier),
    memoryLevel: normalizeMemoryLevel(chat.memoryLevel, chat.memoryEnabled ?? true),
    memoryEnabled: normalizeMemoryLevel(chat.memoryLevel, chat.memoryEnabled ?? true) !== 'off',
    smallModelMode: chat.smallModelMode ?? false,
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

export const MAX_CLI_TRANSCRIPT_BYTES = 1024 * 1024

export function decodeCliChunk(encoded: string): string {
  try {
    return atob(encoded)
  } catch {
    return encoded
  }
}

export function clampCliTranscript(content: string): string {
  return content.length > MAX_CLI_TRANSCRIPT_BYTES
    ? content.slice(content.length - MAX_CLI_TRANSCRIPT_BYTES)
    : content
}

export function appendCliTranscriptChunk(
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

export function normalizeCliLifecycleState(
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

export function cliLifecycleIsProcessing(lifecycleState: CliLifecycleState): boolean {
  return lifecycleState === 'busy' || lifecycleState === 'shuttingDown'
}

export function normalizeAcpLifecycleState(value: unknown, running: boolean, processing: boolean): AcpLifecycleState {
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

function commandsEquivalent(existing: AcpCommand[], next: AcpCommand[]) {
  if (existing.length !== next.length) return false
  return existing.every((command, index) => {
    const other = next[index]
    return command.name === other.name && command.description === other.description && command.inputHint === other.inputHint
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
    existing.safetyRisk === next.safetyRisk &&
    existing.safetyTier === next.safetyTier &&
    existing.safetyRequiresApproval === next.safetyRequiresApproval &&
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

export function acpBindingsEquivalent(existing: AcpBinding | undefined, next: AcpBinding) {
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
    commandsEquivalent(existing.availableCommands ?? [], next.availableCommands ?? []) &&
    modesEquivalent(existing.availableModes, next.availableModes) &&
    existing.currentModeId === next.currentModeId &&
    modelsEquivalent(existing.availableModels, next.availableModels) &&
    existing.modelsLoading === next.modelsLoading &&
    existing.modelRefreshError === next.modelRefreshError &&
    existing.currentModelId === next.currentModelId &&
    turnEventsEquivalent(existing.turnEvents, next.turnEvents) &&
    existing.turnUserMessageIndex === next.turnUserMessageIndex &&
    existing.turnAssistantMessageIndex === next.turnAssistantMessageIndex &&
    existing.turnSerial === next.turnSerial &&
    queuedPromptsEquivalent(existing.queuedPrompts ?? [], next.queuedPrompts ?? []) &&
    existing.waitIsStale === next.waitIsStale &&
    existing.waitStaleReason === next.waitStaleReason &&
    existing.waitSeconds === next.waitSeconds &&
    pendingPermissionEquivalent(existing.pendingPermission, next.pendingPermission) &&
    pendingUserInputEquivalent(existing.pendingUserInput, next.pendingUserInput) &&
    agentInfoEquivalent(existing.agentInfo, next.agentInfo)
  )
}

export function cliBindingsEquivalent(existing: CliBinding | undefined, next: CliBinding) {
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
    existing.pendingSteer === next.pendingSteer &&
    existing.lastError === next.lastError
  )
}

export function cliBindingFromCppChat(chat: CppChat, previous: CliBinding | undefined): CliBinding | null {
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
    pendingSteer: Boolean(chat.cliTerminal.pendingSteer),
    lastError: chat.cliTerminal.lastError ?? '',
  }

  return cliBindingsEquivalent(previous, next) ? previous! : next
}

export function acpBindingFromCppChat(chat: CppChat, previous: AcpBinding | undefined): AcpBinding {
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
    availableCommands: Array.isArray(acp?.availableCommands) ? acp!.availableCommands : [],
    availableModes: Array.isArray(acp?.availableModes) ? acp!.availableModes : [],
    currentModeId: normalizeAcpApprovalMode(acp?.currentModeId ?? chat.approvalMode),
    availableModels: Array.isArray(acp?.availableModels) ? acp!.availableModels : [],
    modelsLoading: Boolean(acp?.modelsLoading),
    modelRefreshError: acp?.modelRefreshError ?? '',
    currentModelId: normalizeAcpModelId(acp?.currentModelId ?? chat.modelId),
    turnEvents: Array.isArray(acp?.turnEvents) ? acp!.turnEvents : [],
    turnUserMessageIndex: typeof acp?.turnUserMessageIndex === 'number' ? acp.turnUserMessageIndex : -1,
    turnAssistantMessageIndex: typeof acp?.turnAssistantMessageIndex === 'number' ? acp.turnAssistantMessageIndex : -1,
    turnSerial: typeof acp?.turnSerial === 'number' ? acp.turnSerial : 0,
    queuedPrompts: acp?.queuedPrompts ?? [],
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

export function normalizeCliTranscript(
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

export function rememberPendingRequest(key: string, requestId: string) {
  pendingRequestIdsByKey.set(key, requestId)
}

let optimisticFieldRevision = 0
const optimisticFieldRevisions = new Map<string, number>()

export function rememberOptimisticFields(fields: readonly string[]) {
  const revision = ++optimisticFieldRevision
  fields.forEach((field) => optimisticFieldRevisions.set(field, revision))
  return revision
}

export function latestOptimisticRollback<T extends object>(previous: T, revision: number): Partial<T> {
  return Object.fromEntries(
    Object.entries(previous).filter(([field]) => optimisticFieldRevisions.get(field) === revision)
  ) as Partial<T>
}

export function isLatestPendingRequest(key: string, requestId?: string) {
  return typeof requestId === 'string' && pendingRequestIdsByKey.get(key) === requestId
}

export function sameRecordEntries<T>(existing: Record<string, T>, next: Record<string, T>) {
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

export function sameArrayEntries<T>(existing: T[], next: T[]) {
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
    (existing.providerId ?? '') === (next.providerId ?? '') &&
    (existing.thoughts ?? '') === (next.thoughts ?? '') &&
    (existing.planSummary ?? '') === (next.planSummary ?? '') &&
    planEntriesEquivalent(existing.planEntries ?? [], next.planEntries ?? []) &&
    toolCallsEquivalent(existing.toolCalls ?? [], next.toolCalls ?? []) &&
    messageBlocksEquivalent(existing.blocks ?? [], next.blocks ?? []) &&
    attachmentsEquivalent(existing.attachments ?? [], messageAttachments(next)) &&
    (existing.processingTimeMs ?? 0) === (next.processingTimeMs ?? 0) &&
    existing.createdAt.getTime() === cppMessageCreatedAtMillis(next)
  )
}

export function buildMessageFromCpp(chatId: string, message: CppMessage, index: number): Message {
  const createdAtMillis = cppMessageCreatedAtMillis(message)
  return {
    id: `cef-m-${chatId}-${createdAtMillis}-${index}-${message.role}`,
    sessionId: chatId,
    role: message.role,
    content: message.content,
    providerId: message.providerId,
    thoughts: message.thoughts ?? '',
    planSummary: message.planSummary ?? '',
    planEntries: message.planEntries ?? [],
    toolCalls: message.toolCalls ?? [],
    blocks: message.blocks ?? [],
    attachments: messageAttachments(message),
    processingTimeMs: message.processingTimeMs ?? 0,
    createdAt: new Date(createdAtMillis),
  }
}

export function reconcileCppMessages(
  chatId: string,
  existingMessages: Message[] | undefined,
  cppMessages: CppMessage[],
  authoritative = false
): Message[] {
  const existing = existingMessages ?? []
  const existingRealMessages = existing.filter((message) => !message.isStreaming)
  const hasStreamingPlaceholder = existing.some((message) => message.isStreaming)

  if (!authoritative && hasStreamingPlaceholder && cppMessages.length <= existingRealMessages.length) {
    return existing
  }

  if (!authoritative && cppMessages.length < existingRealMessages.length) {
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

export function planEntriesEquivalent(existing: AcpPlanEntry[], next: AcpPlanEntry[]) {
  if (existing.length !== next.length) return false
  return existing.every((entry, index) => {
    const other = next[index]
    return entry.content === other.content && entry.priority === other.priority && entry.status === other.status
  })
}

export function toolCallsEquivalent(existing: AcpToolCall[], next: AcpToolCall[]) {
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

export function messageBlocksEquivalent(existing: MessageBlock[], next: MessageBlock[]) {
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

export function attachmentsEquivalent(existing: Attachment[], next: Attachment[]) {
  if (existing.length !== next.length) return false
  return existing.every((attachment, index) => {
    const other = next[index]
    return attachment.id === other.id &&
      attachment.name === other.name &&
      attachment.type === other.type &&
      attachment.size === other.size &&
      (attachment.path ?? '') === (other.path ?? '')
  })
}

export function queuedPromptsEquivalent(existing: AcpBinding['queuedPrompts'], next: AcpBinding['queuedPrompts']) {
  const previous = existing ?? []
  const incoming = next ?? []
  if (previous.length !== incoming.length) return false
  return previous.every((prompt, index) => {
    const other = incoming[index]
    return prompt.text === other.text &&
      prompt.goalMode === other.goalMode &&
      prompt.goalId === other.goalId &&
      Boolean(prompt.prioritySteer) === Boolean(other.prioritySteer) &&
      sameArrayEntries(prompt.markdownStoreFiles, other.markdownStoreFiles) &&
      attachmentsEquivalent(prompt.attachments, other.attachments)
  })
}

export function clearPendingRequest(key: string, requestId?: string) {
  if (typeof requestId === 'string' && pendingRequestIdsByKey.get(key) === requestId) {
    pendingRequestIdsByKey.delete(key)
  }
}

export function clearPendingCodexOptions(chatId: string, requestId?: string) {
  const pending = pendingCodexOptionsByChatId.get(chatId)
  if (pending && pending.requestId === requestId) {
    pendingCodexOptionsByChatId.delete(chatId)
  }
}

export function applyPendingCodexOptions(sessions: Session[]): Session[] {
  if (pendingCodexOptionsByChatId.size === 0 && pendingModelByChatId.size === 0) return sessions
  let changed = false
  const nextSessions = sessions.map((session) => {
    let nextSession = session
    const pendingModel = pendingModelByChatId.get(session.id)
    if (pendingModel) {
      if (
        (session.modelId ?? '') === pendingModel.modelId &&
        (session.reasoningEffort ?? '') === pendingModel.reasoningEffort &&
        (session.serviceTier ?? '') === pendingModel.serviceTier
      ) {
        pendingModelByChatId.delete(session.id)
      } else {
        changed = true
        nextSession = {
          ...nextSession,
          modelId: pendingModel.modelId,
          reasoningEffort: pendingModel.reasoningEffort,
          serviceTier: pendingModel.serviceTier,
        }
      }
    }
    const pending = pendingCodexOptionsByChatId.get(session.id)
    const providerId = session.providerId ?? GEMINI_CLI_PROVIDER_ID
    if (!pending || (providerId !== CODEX_CLI_PROVIDER_ID && providerId !== COPILOT_CLI_PROVIDER_ID)) {
      return nextSession
    }
    if ((session.reasoningEffort ?? '') === pending.reasoningEffort && (session.serviceTier ?? '') === pending.serviceTier) {
      pendingCodexOptionsByChatId.delete(session.id)
      return nextSession
    }
    changed = true
    return {
      ...nextSession,
      reasoningEffort: pending.reasoningEffort,
      serviceTier: pending.serviceTier,
    }
  })
  return changed ? nextSessions : sessions
}
