import { createMemorySettingsWrites } from '../memorySettingsWrites'
import { canExecuteNativeSubagent, hasNativeSubagentLineage, subagentParentFor, openSubagentView, returnToSubagentParent, rememberSubagentParent } from '../../utils/subagentNavigation'
import type { ContextReference } from '../../types/contextReference'
import { sanitizeContextReferences, CONTEXT_REFERENCE_MAX_COUNT } from '../../utils/contextReferences'
import { readChatComposerDraft, writeChatComposerDraft } from '../../utils/composerDraftStorage'
import { filePreviewResponse } from '../../types/filePreview'
import { reportAppFailure } from '../../utils/appFailure'
import { canRouteAutomatically } from '../../types/executionRouting'
import type { ChatSearchHit, ChatSearchNavigation } from '../../types/chatSearch'
import { chatSearchPageRequest, sanitizeChatSearchHits } from '../../utils/chatSearchResults'
import { setPreferredFamilyChat } from '../../utils/branchPreferenceStorage'
import { DEFAULT_CENTRAL_PROVIDER_CONFIGURATION, type CentralProviderConfiguration } from '../cpp/types'
import type { ComputerUseActionResult, ComputerUseBackend, ComputerUseControlState, Session, ViewMode } from '../../types/session'
import { version as packageVersion } from '../../../package.json'
import { defaultsForExecutionHost, withSettingsScope } from '../../utils/settingsScope'
import type { Attachment, Message } from '../../types/message'
import type { Provider } from '../../types/provider'
import type { MemoryLevel } from '../../types/memory'
import type { CreateSessionOptions, VoiceInputMode, VcsStorageSettings, ChatFileChangeReceiptsResponse, McpServerConfiguration } from '../cpp/types'
import type { CEFResponse } from '../../ipc/cefBridge'
import { sendWhenRemoteStopSettles, sendToCEF, isCefContext, isCompanionContext, createRequestId } from '../../ipc/cefBridge'
import {
  CLAUDE_CLI_PROVIDER_ID,
  CODEX_CLI_PROVIDER_ID,
  COPILOT_CLI_PROVIDER_ID,
  DEFAULT_PROVIDER_ID as GEMINI_CLI_PROVIDER_ID,
  OPENCODE_CLI_PROVIDER_ID,
  fallbackProviderForId,
  normalizeCliProviderIdAlias,
  providerCapabilities,
} from '../../utils/providerMetadata'
import { AGENT_MODE_IDS, cefPayloadOrRawResponse, clampedFiniteNumberOr, DEFAULT_GOAL_MAX_LOOP_ITERATIONS, DEFAULT_ACP_SETUP_INACTIVITY_TIMEOUT_SECONDS, DEFAULT_ACP_TURN_OUTPUT_LIMIT_MIB, DEFAULT_MEMORY_IDLE_DELAY_SECONDS, DEFAULT_MEMORY_RECALL_BUDGET_BYTES, defaultEditorFileAssociations, emptyCliVersionManager, emptyMemoryActivity, failedGitWorktreeResult, isAllowedAcpModelId, isRecord, messageAttachments, MAX_MEMORY_IDLE_DELAY_SECONDS, MAX_MEMORY_RECALL_BUDGET_BYTES, MIN_MEMORY_IDLE_DELAY_SECONDS, MIN_MEMORY_RECALL_BUDGET_BYTES, normalizeAcpApprovalMode, normalizeCodexReasoningEffort, normalizeCodexServiceTier, normalizeMemoryLevel, normalizeGoalMaxLoopIterations, normalizeAcpSetupInactivityTimeoutSeconds, normalizeAcpTurnOutputLimitMiB, providerChatDefaultsForNewChat, sanitizeAttachment, sanitizeCppMessage, sanitizeManagedOriginRunId, sanitizeEditorFileAssociations, sanitizeEditorPresetId, sanitizeGitBaseBranch, sanitizeGitWorktreeResult, sanitizeGitWorktreeStatus, sanitizeProviderChatDefaults, sanitizeProviderChatDefaultsMap, sanitizeVcsCommitResult, sanitizeVcsCommitStatus, stringOr } from '../cpp/sanitizers'
import {
  acpBindingsEquivalent,
  attachmentsEquivalent,
  buildMessageFromCpp,
  clearPendingRequest,
  cliLifecycleIsProcessing,
  isLatestPendingRequest,
  latestOptimisticRollback,
  normalizeCliLifecycleState,
  reconcileCppMessages,
  rememberOptimisticFields,
  rememberPendingRequest,
} from '../cpp/reconcile'
import { pendingModelByChatId, pendingRequestIdsByKey, pendingViewModeBySessionId } from '../push/pushBuffers'
import type {
  AcpBinding,
  AcpLifecycleState,
  AcpUserInputAnswers,
  ChatAttachmentInput,
  ChatMessagesResponse,
  CliBinding,
  CliTranscript,
  CliVersionManager,
  CppCliDebugState,
  EditorFileAssociation,
  GitWorktreeResult,
  GitWorktreeStatus,
  GitTurnCheckpointResult,
  MemoryActivity,
  MemoryWorkerBinding,
  UamAgentCycleShortcut,
  UamAgentSummary,
  OpenNativeSessionChatResponse,
  OpenWorkspaceEditorResponse,
  ProviderChatDefaults,
  ProviderAgentImportPreview,
  ProviderModelCatalog,
  VcsCommitMessageSuggestion,
  VcsCommitResult,
  VcsCommitStatus,
  VcsFileDiffResponse,
  VcsType,
} from '../cpp/types'
import type { AppState, AcceptedHistoryPage, AcpPromptPresentationObserver, ZustandSet, ZustandGet } from '../storeTypes'
import { removeChatsFromGrid, writeChatViewMode } from '../../utils/chatGridStorage'
import { removeComposerDrafts } from '../../utils/composerDraftStorage'
import { discardPendingPushesForChats, discardPendingTranscriptPushesForChat } from '../push/pushBuffers'

const initialFolders = [
  {
    id: 'default',
    name: 'General',
    parentId: null as string | null,
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
  return { ...fallbackProviderForId(providerId), color }
}

const initialProviders: Provider[] = [
  initialProvider(GEMINI_CLI_PROVIDER_ID, '#f97316'),
  initialProvider(CODEX_CLI_PROVIDER_ID, '#0ea5e9'),
  initialProvider(CLAUDE_CLI_PROVIDER_ID, '#7c3aed'),
  initialProvider(OPENCODE_CLI_PROVIDER_ID, '#14b8a6'),
  initialProvider(COPILOT_CLI_PROVIDER_ID, '#22c55e'),
]

let sessionCounter = 10

function makeId(prefix: string, counter: number) {
  return `${prefix}-${counter}`
}

const computerUseSuccess: ComputerUseActionResult = { ok: true }

function computerUseFailure(error?: string): ComputerUseActionResult {
  return error ? { ok: false, error } : { ok: false }
}

/** Validate the optional bounded navigation index from current and legacy bridges. */
export function userMessageIndicesFromPage(data: ChatMessagesResponse): number[] | undefined {
  const indices = data.userMessageIndices
  if (!Array.isArray(indices) || indices.length > 32768 || !Number.isSafeInteger(data.totalCount)) return undefined
  return indices.every((index, position) => {
    const localIndex = index - (data.startIndex ?? 0)
    const loaded = data.messages?.[localIndex]
    return Number.isSafeInteger(index) && index >= 0 && index < data.totalCount! &&
      (position === 0 || index > indices[position - 1]) && (!loaded || loaded.role === 'user')
  }) ? indices : undefined
}

/** Native cold-history adoption publishes display/retry authority with the same guarded page. */
function unsentIndicesFromPage(data: ChatMessagesResponse) {
  const valid = (indices: unknown): indices is number[] => Array.isArray(indices) && Number.isSafeInteger(data.totalCount) && indices.every((index, position) =>
    Number.isSafeInteger(index) && index >= 0 && index < data.totalCount! && (position === 0 || index > indices[position - 1]))
  return valid(data.unsentMessageIndices) && valid(data.recoverableUnsentMessageIndices)
    ? { unsentMessageIndices: data.unsentMessageIndices, recoverableUnsentMessageIndices: data.recoverableUnsentMessageIndices }
    : undefined
}

function unsentProjectionFromPage(state: AppState, chatId: string, data: ChatMessagesResponse, observedBinding: AppState['acpBindingBySessionId'][string] | undefined) {
  const indices = unsentIndicesFromPage(data)
  const binding = state.acpBindingBySessionId[chatId]
  return indices && binding && binding === observedBinding
    ? { acpBindingBySessionId: { ...state.acpBindingBySessionId, [chatId]: { ...binding, ...indices } } } : undefined
}

function historyChangedDuringLoad(state: AppState, chatId: string) {
  return { chatHistoryErrorBySessionId: { ...state.chatHistoryErrorBySessionId,
    [chatId]: 'Chat history changed while loading. Retry to refresh it.' } }
}

export function withoutDeletedKeys<T>(values: Record<string, T>, deletedIds: Set<string>): Record<string, T> {
  return Object.fromEntries(Object.entries(values).filter(([id]) => !deletedIds.has(id)))
}

export function deleteSessionsFromState(state: AppState, deletedIds: Set<string>, selectedChatId?: string | null): Partial<AppState> {
  const sessions = state.sessions.filter((session) => !deletedIds.has(session.id))
  const requestedSelection = selectedChatId !== undefined ? selectedChatId : state.activeSessionId
  return {
    sessions,
    messages: withoutDeletedKeys(state.messages, deletedIds),
    historyStartIndexBySessionId: withoutDeletedKeys(state.historyStartIndexBySessionId, deletedIds),
    chatHistoryErrorBySessionId: withoutDeletedKeys(state.chatHistoryErrorBySessionId, deletedIds),
    chatHistoryReadBySessionId: withoutDeletedKeys(state.chatHistoryReadBySessionId ?? {}, deletedIds),
    chatHistoryPageBySessionId: withoutDeletedKeys(state.chatHistoryPageBySessionId ?? {}, deletedIds),
    goalsByChatId: withoutDeletedKeys(state.goalsByChatId, deletedIds),
    activeGoalIdByChatId: withoutDeletedKeys(state.activeGoalIdByChatId, deletedIds),
    goalModeByChatId: withoutDeletedKeys(state.goalModeByChatId, deletedIds),
    defaultGoalTokenBudgetByChatId: withoutDeletedKeys(state.defaultGoalTokenBudgetByChatId, deletedIds),
    cliBindingBySessionId: withoutDeletedKeys(state.cliBindingBySessionId, deletedIds),
    acpBindingBySessionId: withoutDeletedKeys(state.acpBindingBySessionId, deletedIds),
    cliTranscriptBySessionId: withoutDeletedKeys(state.cliTranscriptBySessionId, deletedIds),
    markdownStoreAttachedBySessionId: withoutDeletedKeys(state.markdownStoreAttachedBySessionId, deletedIds),
    repositoryReviewBySessionId: withoutDeletedKeys(state.repositoryReviewBySessionId, deletedIds),
    uamAgentsBySessionId: withoutDeletedKeys(state.uamAgentsBySessionId, deletedIds),
    activeSessionId: requestedSelection === null || sessions.some((session) => session.id === requestedSelection)
      ? requestedSelection
      : (sessions[0]?.id ?? null),
  }
}

export let pendingProviderChatDefaults: {
  requestId: string
  defaultNewChatProviderId: string
  providerChatDefaults: Record<string, ProviderChatDefaults>
} | null = null

function clearPendingProviderChatDefaults(requestId?: string) {
  if (pendingProviderChatDefaults && pendingProviderChatDefaults.requestId === requestId) {
    pendingProviderChatDefaults = null
  }
}

export function createSessionsSlice(set: ZustandSet, get: ZustandGet, inCef: boolean) {
  // Overlapping pin writes share the last acknowledged value, never another optimistic value.
  const pendingPinChanges = new Map<string, {
    confirmedPinned: Session['isPinned']
    confirmedSequence: number
    latestSequence: number
    pendingCount: number
  }>()
  const memorySettingsWrites = createMemorySettingsWrites({
    readValues: () => get(),
    applyValues: (values) => set(values),
    publishUnconfirmedFields: (fields) => set({ memorySettingsUnconfirmedFields: fields }),
    isNative: isCefContext,
    request: sendToCEF,
    requestId: () => createRequestId('memory-settings'),
    reportFailure: (outcome) => reportAppFailure(
      outcome === 'unknown' ? 'Could not confirm the memory settings save.'
        : outcome === 'not-sent' ? 'Could not send the memory settings save.' : 'Memory settings were not saved.',
      outcome === 'unknown'
        ? 'Your requested values are kept. Check the current saved values before trying again.'
        : outcome === 'not-sent'
          ? 'No save request was sent. Previous displayed values were restored; other pending edits were kept.'
          : 'Current saved values were restored. Check them before trying again.',
    ),
  })

  let intentionalSelectionRevision = 0
  let searchJumpGeneration = 0
  const boundedTailReloads = new Map<string, number>()
  const loadedMessagesDigestByChatId = new Map<string, string>()
  const messageHydrationGenerationByChatId = new Map<string, number>()

  const requestChatMessagesFromCef = (chatId: string, force = false, refreshNative = false, presentationRetry = 0): Promise<void | false | AcceptedHistoryPage> | undefined => {
    if (!isCefContext() || !chatId) return
    if (get().chatSearchNavigation?.chatId === chatId || boundedTailReloads.has(chatId)) return
    const current = get()
    const historyStartAtRequestStart = current.historyStartIndexBySessionId[chatId] ?? 0
    const messagesAtRequestStart = current.messages[chatId]
    const bindingAtRequestStart = current.acpBindingBySessionId[chatId]
    const sessionAtRequestStart = current.sessions.find((session) => session.id === chatId)
    const chatBirthAtRequestStart = sessionAtRequestStart?.createdAt.getTime()
    if (!sessionAtRequestStart || !Number.isFinite(chatBirthAtRequestStart)) return
    const cliBindingAtRequestStart = current.cliBindingBySessionId[chatId]
    // A held native summary can predate an opaque optimistic echo, including while another chat was selected.
    force ||= Boolean(bindingAtRequestStart?.queueHeld && bindingAtRequestStart.queuedPrompts?.length &&
      bindingAtRequestStart.queuedPrompts.every((entry) => entry.id && Number.isSafeInteger(entry.revision) && entry.revision! > 0) &&
      Number.isSafeInteger(sessionAtRequestStart?.messageCount) && sessionAtRequestStart?.messagesDigest &&
      historyStartAtRequestStart + (messagesAtRequestStart?.length ?? 0) > sessionAtRequestStart!.messageCount!)
    const requestKey = `getChatMessages:${chatId}`
    const requestId = createRequestId('getChatMessages')
    const hydrationGeneration = messageHydrationGenerationByChatId.get(chatId) ?? 0
    rememberPendingRequest(requestKey, requestId)
    set(state => ({ chatHistoryReadBySessionId: { ...state.chatHistoryReadBySessionId,
      [chatId]: { requestId, birth: chatBirthAtRequestStart! } } }))
    let timedOut = false
    let deadline: ReturnType<typeof setTimeout>
    const timeout = new Promise<CEFResponse<ChatMessagesResponse>>(resolve => {
      deadline = setTimeout(() => {
        timedOut = true
        resolve({ ok: false, requestId,
          error: 'Reading saved chat history timed out. Retry to refresh it; retained messages have not been resent.' })
      }, 30_000)
    })
    const requestedDigest = force || current.messages[chatId] === undefined ? '' : loadedMessagesDigestByChatId.get(chatId) ?? ''
    const read = sendToCEF<ChatMessagesResponse>({
      action: 'getChatMessages',
      payload: {
        chatId,
        limit: 50,
        deferToolCallContent: true,
        ...(refreshNative ? { refreshNative: true } : {}),
        messagesDigest: requestedDigest,
      },
      requestId,
    })
    return Promise.race([read, timeout]).then((response) => {
      if (!isLatestPendingRequest(requestKey, response.requestId)) return
      clearPendingRequest(requestKey, response.requestId)
      const stateAtResponse = get()
      const sessionAtResponse = stateAtResponse.sessions.find((session) => session.id === chatId)
      if ((messageHydrationGenerationByChatId.get(chatId) ?? 0) !== hydrationGeneration ||
          !sessionAtResponse || sessionAtResponse.createdAt.getTime() !== chatBirthAtRequestStart) return
      const bindingAtResponse = stateAtResponse.acpBindingBySessionId[chatId]
      const sameHistoryOwner = sessionAtRequestStart &&
        sessionAtResponse.providerId === sessionAtRequestStart.providerId &&
        sessionAtResponse.executionHostId === sessionAtRequestStart.executionHostId &&
        sessionAtResponse.workspaceDirectory === sessionAtRequestStart.workspaceDirectory
      if (!sameHistoryOwner || (timedOut && stateAtResponse.messages[chatId] !== messagesAtRequestStart)) return false as const
      const sameBindingAuthority = bindingAtResponse === bindingAtRequestStart ||
        Boolean(bindingAtRequestStart && bindingAtResponse && acpBindingsEquivalent(bindingAtRequestStart, { ...bindingAtResponse,
          idleCountdownStartsAtMs: bindingAtRequestStart.idleCountdownStartsAtMs,
          idleShutdownAtMs: bindingAtRequestStart.idleShutdownAtMs,
          diagnostics: bindingAtRequestStart.diagnostics }))
      const mayReadColdHistoryOnce = presentationRetry === 0 && !refreshNative &&
        messagesAtRequestStart === undefined && stateAtResponse.messages[chatId] === undefined &&
        Number.isFinite(chatBirthAtRequestStart) && sameHistoryOwner && sameBindingAuthority &&
        stateAtResponse.cliBindingBySessionId[chatId] === cliBindingAtRequestStart &&
        (stateAtResponse.historyStartIndexBySessionId[chatId] ?? 0) === historyStartAtRequestStart &&
        stateAtResponse.chatSearchNavigation?.chatId !== chatId && !boundedTailReloads.has(chatId) &&
        !pendingRequestIdsByKey.has(`sendAcpPrompt:${chatId}`)
      if (!response.ok || !response.data) {
        if (!response.ok && response.error === 'Chat history changed while loading. Retry to refresh it.' && mayReadColdHistoryOnce) {
          // A refusal contains no page to adopt. Share the presentation reread's single budget.
          const fresh = requestChatMessagesFromCef(chatId, true, false, presentationRetry + 1)
          if (fresh) return fresh
        }
        const chatName = current.sessions.find((session) => session.id === chatId)?.name?.trim() || chatId
        set((state) => ({
          chatHistoryErrorBySessionId: {
            ...state.chatHistoryErrorBySessionId,
            [chatId]: response.error ?? `Failed to load chat history for ${chatName}.`,
          },
        }))
        return false as const
      }

      const data = response.data
      let accepted: AcceptedHistoryPage | undefined
      const attest = (state: AppState, messages: Message[], startIndex: number, totalCount: number, digest: string) => {
        if (!digest || messages.some(message => message.isStreaming) || messages.length > 50 ||
            startIndex + messages.length !== totalCount || state.messages[chatId] !== messagesAtRequestStart) return
        accepted = { chatId, birth: chatBirthAtRequestStart!, requestId, session: sessionAtRequestStart,
          summaryDigest: sessionAtRequestStart.messagesDigest, summaryCount: sessionAtRequestStart.messageCount,
          messagesDigest: digest, startIndex, totalCount, messages }
      }
      const acceptedCurrent = () => {
        const state = get(), live = state.sessions.find(session => session.id === chatId)
        return accepted && live?.createdAt.getTime() === accepted.birth &&
          live.providerId === sessionAtRequestStart.providerId && live.executionHostId === sessionAtRequestStart.executionHostId &&
          live.workspaceDirectory === sessionAtRequestStart.workspaceDirectory &&
          (messageHydrationGenerationByChatId.get(chatId) ?? 0) === hydrationGeneration &&
          state.messages[chatId] === accepted.messages &&
          (state.historyStartIndexBySessionId[chatId] ?? 0) === accepted.startIndex ? accepted : undefined
      }
      if (data.chatId && data.chatId !== chatId) return
      if (mayReadColdHistoryOnce &&
          unsentIndicesFromPage(data) && bindingAtRequestStart && bindingAtResponse && bindingAtResponse !== bindingAtRequestStart &&
          acpBindingsEquivalent(bindingAtRequestStart, { ...bindingAtResponse,
            idleCountdownStartsAtMs: bindingAtRequestStart.idleCountdownStartsAtMs,
            idleShutdownAtMs: bindingAtRequestStart.idleShutdownAtMs,
            diagnostics: bindingAtRequestStart.diagnostics })) {
        // Discard this page; presentation changes only permit one new read, never stale unsent adoption.
        const fresh = requestChatMessagesFromCef(chatId, true, false, presentationRetry + 1)
        if (fresh) return fresh
      }
      if (data.unchanged) {
        set((state) => {
          if (unsentIndicesFromPage(data) && state.acpBindingBySessionId[chatId] !== bindingAtRequestStart) return historyChangedDuringLoad(state, chatId)
          const projection = state.messages[chatId] === messagesAtRequestStart ? unsentProjectionFromPage(state, chatId, data, bindingAtRequestStart) : undefined
          const cachedDigest = loadedMessagesDigestByChatId.get(chatId)
          if (requestedDigest && cachedDigest === requestedDigest && data.messagesDigest === requestedDigest && state.messages[chatId] === messagesAtRequestStart &&
              (state.historyStartIndexBySessionId[chatId] ?? 0) === historyStartAtRequestStart &&
              data.startIndex === historyStartAtRequestStart && data.totalCount === historyStartAtRequestStart + (messagesAtRequestStart?.length ?? 0)) {
            attest(state, messagesAtRequestStart!, historyStartAtRequestStart, data.totalCount, cachedDigest)
          }
          const evidence = accepted ? { chatHistoryPageBySessionId: { ...state.chatHistoryPageBySessionId, [chatId]: accepted } } : undefined
          if (!state.chatHistoryErrorBySessionId[chatId]) return evidence || projection ? { ...evidence, ...projection } : state
          const chatHistoryErrorBySessionId = { ...state.chatHistoryErrorBySessionId }
          delete chatHistoryErrorBySessionId[chatId]
          return { ...evidence, ...projection, chatHistoryErrorBySessionId }
        })
        return acceptedCurrent()
      }
      if (!Array.isArray(data.messages)) return

      const pageStart = data.startIndex ?? 0
      const totalCount = data.totalCount ?? data.messages.length
      if (!Number.isSafeInteger(pageStart) || pageStart < 0 || !Number.isSafeInteger(totalCount) ||
          pageStart + data.messages.length !== totalCount) return false as const

      set((state) => {
        if (unsentIndicesFromPage(data) && state.acpBindingBySessionId[chatId] !== bindingAtRequestStart) return historyChangedDuringLoad(state, chatId)
        let streamedTail: Message | undefined
        if ((force || data.startIndex !== undefined) && state.messages[chatId] !== messagesAtRequestStart) {
          const existing = state.messages[chatId] ?? []
          const start = messagesAtRequestStart ?? []
          const binding = state.acpBindingBySessionId[chatId]
          const tail = existing[existing.length - 1]
          const startTail = start[start.length - 1]
          const assistantIndex = binding?.turnAssistantMessageIndex ?? -1
          const nativeAssistant = data.messages?.[assistantIndex - pageStart]
          const sameTurn = binding && bindingAtRequestStart &&
            binding.turnSerial === bindingAtRequestStart.turnSerial &&
            binding.sessionId === bindingAtRequestStart.sessionId &&
            binding.threadId === bindingAtRequestStart.threadId &&
            binding.providerId === bindingAtRequestStart.providerId
          const unchangedPrefix = existing.length === start.length
            ? existing.slice(0, -1).every((message, index) => message === start[index]) &&
              (startTail?.isStreaming || assistantIndex === historyStartAtRequestStart + existing.length - 1)
            : existing.length === start.length + 1 && start.every((message, index) => message === existing[index])
          // Only streaming tail updates may coexist with authoritative missing message positions.
          // Edits, new turns and unrelated replacements retain the strict stale-response guard.
          if ((state.historyStartIndexBySessionId[chatId] ?? 0) !== historyStartAtRequestStart || !sameTurn || !unchangedPrefix || tail?.role !== 'assistant' ||
              !(tail.isStreaming || startTail?.isStreaming) || nativeAssistant?.role !== 'assistant' ||
              assistantIndex !== totalCount - 1 ||
              !(tail.content.startsWith(nativeAssistant.content) || nativeAssistant.content.startsWith(tail.content))) return state
          streamedTail = tail
        }
        const existingStart = state.historyStartIndexBySessionId[chatId] ?? 0
        const existingPage = existingStart === pageStart ? state.messages[chatId]
          : existingStart < pageStart ? state.messages[chatId]?.slice(pageStart - existingStart) : undefined
        let nextMessages = reconcileCppMessages(chatId, existingPage, data.messages ?? [], force, pageStart)
        if (streamedTail) {
          nextMessages = [...nextMessages]
          const index = nextMessages.length - 1
          const nativeAssistant = nextMessages[index]
          nextMessages[index] = { ...nativeAssistant,
            content: streamedTail.content.length > nativeAssistant.content.length ? streamedTail.content : nativeAssistant.content,
            isStreaming: streamedTail.isStreaming,
          }
        }
        if (!streamedTail && nextMessages.length === data.messages!.length && !nextMessages.some((message) => message.isStreaming)) {
          loadedMessagesDigestByChatId.set(chatId, data.messagesDigest ?? '')
        }
        if (!streamedTail) attest(state, nextMessages, pageStart, totalCount, data.messagesDigest ?? '')
        const messagesChanged = nextMessages !== state.messages[chatId]
        // Native summary tokens and authoritative page hashes have different contracts.
        const sessions = !streamedTail
          ? state.sessions.map((candidate) =>
              candidate.id === chatId && (candidate.messageCount !== totalCount || data.userMessageIndices !== undefined)
                ? { ...candidate, messageCount: totalCount, userMessageIndices: userMessageIndicesFromPage(data) }
                : candidate
            )
          : state.sessions
        const sessionsChanged = sessions !== state.sessions && sessions.some((candidate, index) => candidate !== state.sessions[index])
        const chatHistoryErrorBySessionId = state.chatHistoryErrorBySessionId[chatId]
          ? (() => {
              const next = { ...state.chatHistoryErrorBySessionId }
              delete next[chatId]
              return next
            })()
          : state.chatHistoryErrorBySessionId
        const historyErrorChanged = chatHistoryErrorBySessionId !== state.chatHistoryErrorBySessionId

        const historyStartChanged = existingStart !== pageStart
        const projection = !streamedTail ? unsentProjectionFromPage(state, chatId, data, bindingAtRequestStart) : undefined
        if (!accepted && !messagesChanged && !sessionsChanged && !historyErrorChanged && !historyStartChanged && !projection) return state
        return {
          ...projection,
          ...(accepted ? { chatHistoryPageBySessionId: { ...state.chatHistoryPageBySessionId, [chatId]: accepted } } : {}),
          ...(messagesChanged ? {
            messages: {
              ...state.messages,
              [chatId]: nextMessages,
            },
          } : {}),
          ...(historyStartChanged ? { historyStartIndexBySessionId: { ...state.historyStartIndexBySessionId, [chatId]: pageStart } } : {}),
          ...(sessionsChanged ? { sessions } : {}),
          ...(historyErrorChanged ? { chatHistoryErrorBySessionId } : {}),
        }
      })
      return acceptedCurrent()
    }).catch(() => {
      const state = get()
      if (state.chatHistoryReadBySessionId[chatId]?.requestId !== requestId ||
          (messageHydrationGenerationByChatId.get(chatId) ?? 0) !== hydrationGeneration ||
          state.sessions.find(session => session.id === chatId)?.createdAt.getTime() !== chatBirthAtRequestStart) return
      set({ chatHistoryErrorBySessionId: { ...state.chatHistoryErrorBySessionId,
        [chatId]: 'Saved chat history could not be read. Retry to refresh it.' } })
      return false as const
    }).finally(() => {
      clearTimeout(deadline)
      const state = get()
      if (state.chatHistoryReadBySessionId[chatId]?.requestId !== requestId) return
      clearPendingRequest(requestKey, requestId)
      const chatHistoryReadBySessionId = { ...state.chatHistoryReadBySessionId }
      delete chatHistoryReadBySessionId[chatId]
      set({ chatHistoryReadBySessionId })
    })
  }

  return {
    folders: inCef ? [] : initialFolders,
    sessions: inCef ? [] : initialSessions,
    activeSessionId: (inCef ? null : 's1') as string | null,
    lastAppliedStateRevision: -1,
    messages: {} as Record<string, Message[]>,
    historyStartIndexBySessionId: {} as Record<string, number>,
    chatSearchNavigation: null as ChatSearchNavigation | null,
    chatHistoryErrorBySessionId: {} as Record<string, string>,
    chatHistoryReadBySessionId: {} as Record<string, { requestId: string; birth: number }>,
    chatHistoryPageBySessionId: {} as Record<string, AcceptedHistoryPage>,
    providers: inCef ? [] : initialProviders,
    cliBindingBySessionId: {} as Record<string, CliBinding>,
    acpBindingBySessionId: {} as Record<string, AcpBinding>,
    providerModelCatalogs: [] as ProviderModelCatalog[],
    cliTranscriptBySessionId: {} as Record<string, CliTranscript>,
    cliDebugState: null as CppCliDebugState | null,
    memoryEnabledDefault: true,
    memoryLevelDefault: 'strict' as MemoryLevel,
    memoryIdleDelaySeconds: DEFAULT_MEMORY_IDLE_DELAY_SECONDS,
    memoryRecallBudgetBytes: DEFAULT_MEMORY_RECALL_BUDGET_BYTES,
    goalMaxLoopIterations: DEFAULT_GOAL_MAX_LOOP_ITERATIONS,
    acpSetupInactivityTimeoutSeconds: DEFAULT_ACP_SETUP_INACTIVITY_TIMEOUT_SECONDS,
    acpTurnOutputLimitMiB: DEFAULT_ACP_TURN_OUTPUT_LIMIT_MIB,
    appVersion: `V${packageVersion}`,
    runnerProtocolVersion: 0,
    updateChecksEnabled: true,
    automaticProviderUpdates: false,
    updateLastCheckedAt: '',
    dismissedUpdateVersions: {} as Record<string, string>,
    memoryLastStatus: '',
    memorySettingsUnconfirmedFields: [] as string[],
    memoryWorkerBindings: {} as Record<string, MemoryWorkerBinding>,
    permissionReviewerProviderId: '',
    permissionReviewerModelId: '',
    memoryActivity: { ...emptyMemoryActivity } as MemoryActivity,
    cliVersionManager: { ...emptyCliVersionManager } as CliVersionManager,
    defaultNewChatProviderId: GEMINI_CLI_PROVIDER_ID,
    providerChatDefaults: {} as Record<string, ProviderChatDefaults>,
    fileExplorerApplication: '',
    defaultEditorPresetId: 'vscode',
    editorFileAssociations: defaultEditorFileAssociations() as EditorFileAssociation[],
    providerAvailability: [],
    voiceInputMode: 'system' as VoiceInputMode,
    gitAutoWorktree: true,
    gitSnapshotSvn: true,
    gitSnapshotPlainFolders: true,
    gitDefaultBaseBranch: '',
    gitPullBeforeChat: false,
    svnUpdateBeforeChat: false,
    gitPortMethod: 'patch' as VcsStorageSettings['gitPortMethod'],
    worktreeCleanupEnabled: true,
    worktreeRetentionDays: 14,
    backupLogRetentionDays: 14,
    httpCacheClearIntervalDays: 0,
    centralProviderConfiguration: DEFAULT_CENTRAL_PROVIDER_CONFIGURATION,
    mcpServers: [] as McpServerConfiguration[],
    hostScopedSettingsValues: {},
    executionHosts: [{
      id: 'local', label: 'This computer', transport: 'local', sshAlias: '',
      runnerStatus: 'ready', runnerVersion: '', platform: '', architecture: '', lastSeenAt: '',
    }] as AppState['executionHosts'],
    favoriteUamAgentIds: [] as string[],
    uamAgentCycleShortcut: 'shift+tab' as UamAgentCycleShortcut,
    uamAgentsBySessionId: {} as Record<string, UamAgentSummary[]>,
    statusLine: '',

    acknowledgeChatAttention: async (id: string, revision: string) => {
      const response = await sendToCEF({ action: 'acknowledgeChatAttention', payload: { chatId: id, attentionRevision: revision } })
      return response.ok
    },

    setChatSnoozedUntil: async (id: string, untilMs: number) => {
      if (!isCefContext()) return { ok: false, error: 'Snoozing requires the desktop app.' }
      return sendToCEF({ action: 'setChatSnoozedUntil', payload: { chatId: id, untilMs } }, false)
    },

    setChatSettled: async (id: string, settled: boolean) => {
      const response = await sendToCEF({ action: 'setChatSettled', payload: { chatId: id, settled } })
      return response.ok
    },

    setActiveSession: (id: string | null, navigation?: { preserveSubagentRoute?: boolean; leafId?: string }) => {
      const selected = get().sessions.find(candidate => candidate.id === id)
      const parent = selected && subagentParentFor(selected, get().sessions)
      if (parent && id) {
        if (!canExecuteNativeSubagent(selected!)) {
          get().setActiveSession(parent.id, { preserveSubagentRoute: true, leafId: navigation?.leafId })
          openSubagentView(parent, { kind: 'native', chatId: id }, navigation?.leafId)
          return
        }
        openSubagentView(parent, { kind: 'native', chatId: id }, navigation?.leafId)
      }
      if (selected && hasNativeSubagentLineage(selected) && !canExecuteNativeSubagent(selected)) {
        // An orphan is a passive history destination, never an ordinary runtime selection.
        if (!navigation?.preserveSubagentRoute) returnToSubagentParent(selected.id, navigation?.leafId)
        intentionalSelectionRevision += 1
        set({ activeSessionId: selected.id, chatSearchNavigation: null })
        return
      }
      if (id && !navigation?.preserveSubagentRoute) returnToSubagentParent(id, navigation?.leafId)
      intentionalSelectionRevision += 1
      if (get().chatSearchNavigation && get().chatSearchNavigation!.chatId !== id) set({ chatSearchNavigation: null })
      const persistSelection = () => {
        if (!id) return
        const selected = get().sessions.find((session) => session.id === id)
        if (selected) setPreferredFamilyChat(selected.branchRootChatId || selected.parentChatId || selected.id, selected.id)
      }
      if (get().activeSessionId === id) {
        persistSelection()
        return
      }
      if (isCefContext()) {
        const previousActiveSessionId = get().activeSessionId
        const requestKey = 'selectSession'
        const requestId = createRequestId('selectSession')
        rememberPendingRequest(requestKey, requestId)
        const waitsForHistory = Boolean(id && selected && Number.isFinite(selected.createdAt.getTime()) &&
          get().messages[id] === undefined && (selected.messageCount ?? 0) > 0 && get().chatSearchNavigation?.chatId !== id)
        let selectionHistoryExpired = false
        let selectionHistoryDeadline: ReturnType<typeof setTimeout> | undefined
        const finishHistorySelection = (expired = false) => {
          clearTimeout(selectionHistoryDeadline)
          if (!id) return
          const state = get()
          if (state.chatHistoryReadBySessionId[id]?.requestId !== requestId) return
          const chatHistoryReadBySessionId = { ...state.chatHistoryReadBySessionId }
          delete chatHistoryReadBySessionId[id]
          const live = state.sessions.find(session => session.id === id)
          const sameOwner = selected && live && live.createdAt.getTime() === selected.createdAt.getTime() &&
            live.providerId === selected.providerId && live.executionHostId === selected.executionHostId &&
            live.workspaceDirectory === selected.workspaceDirectory
          // This only expires history presentation. A slow selection's mutation outcome stays unknown.
          const fail = expired && sameOwner && state.activeSessionId === id && state.messages[id] === undefined &&
            state.chatSearchNavigation?.chatId !== id
          set({ chatHistoryReadBySessionId, ...(fail ? { chatHistoryErrorBySessionId: {
            ...state.chatHistoryErrorBySessionId, [id]: 'Opening saved chat history timed out. Retry to refresh it; retained messages have not been resent.' } } : {}) })
        }
        set({ activeSessionId: id, ...(waitsForHistory ? { chatHistoryReadBySessionId: {
          ...get().chatHistoryReadBySessionId, [id!]: { requestId, birth: selected!.createdAt.getTime() } } } : {}) })
        if (waitsForHistory) selectionHistoryDeadline = setTimeout(() => {
          selectionHistoryExpired = true
          finishHistorySelection(true)
        }, 30_000)
        sendToCEF({ action: 'selectSession', payload: { chatId: id ?? '' }, requestId }).then((resp) => {
          if (!isLatestPendingRequest(requestKey, resp.requestId)) {
            finishHistorySelection()
            return
          }
          clearPendingRequest(requestKey, resp.requestId)
          if (resp.ok) {
            if (get().activeSessionId === id) persistSelection()
            const pendingRead = id ? get().chatHistoryReadBySessionId[id] : undefined
            if (id && !selectionHistoryExpired && (!pendingRead || pendingRead.requestId === requestId) &&
                (!waitsForHistory || get().messages[id] === undefined)) requestChatMessagesFromCef(id)
            finishHistorySelection()
            return
          }

          set({ activeSessionId: previousActiveSessionId })
          finishHistorySelection()
        })
        return
      }

      set((state) => {
        const openedAt = new Date()
        return {
          activeSessionId: id,
          sessions: state.sessions.map((s) =>
            id && s.id === id ? { ...s, lastOpenedAt: openedAt } : s
          ),
        }
      })
      persistSelection()
    },

    // Reuse the digest-checked, bounded history jump for the message rail.
    jumpToSessionMessage: async (chatId: string, messageIndex: number) => {
      const session = get().sessions.find((candidate) => candidate.id === chatId)
      if (!session?.messagesDigest || !Number.isSafeInteger(messageIndex) || messageIndex < 0 || messageIndex >= (session.messageCount ?? 0)) return false
      return get().jumpToChatSearchHit({ chatId, messageIndex, messagesDigest: session.messagesDigest, field: 'content', excerpt: '' }, `message-rail:${chatId}`)
    },

    jumpToChatSearchHit: async (input: ChatSearchHit, queryRequestId: string) => {
      if (!isCefContext() || !queryRequestId) return false
      const hit = sanitizeChatSearchHits([input], new Set(get().sessions.map((session) => session.id)))[0]
      if (!hit) return false
      const generation = ++searchJumpGeneration
      const hydrationGeneration = (messageHydrationGenerationByChatId.get(hit.chatId) ?? 0) + 1
      messageHydrationGenerationByChatId.set(hit.chatId, hydrationGeneration)
      const target: ChatSearchNavigation = { generation, queryRequestId, chatId: hit.chatId,
        messageIndex: hit.messageIndex, messagesDigest: hit.messagesDigest, phase: 'loading', anchored: false }
      set({ chatSearchNavigation: target })
      get().setActiveSession(hit.chatId)
      const selectionRevision = intentionalSelectionRevision
      const observedBinding = get().acpBindingBySessionId[hit.chatId]
      const response = await sendToCEF<ChatMessagesResponse>({ action: 'getChatMessages', payload: chatSearchPageRequest(hit) })
      const current = get()
      if (current.chatSearchNavigation?.generation !== generation) return false
      if (current.activeSessionId !== hit.chatId || intentionalSelectionRevision !== selectionRevision ||
          messageHydrationGenerationByChatId.get(hit.chatId) !== hydrationGeneration ||
          !current.sessions.some((session) => session.id === hit.chatId)) {
        set({ chatSearchNavigation: null })
        return false
      }
      const data = response.data
      if (!response.ok || !data || data.chatId !== hit.chatId || data.messagesDigest !== hit.messagesDigest ||
          !Array.isArray(data.messages) || !Number.isSafeInteger(data.startIndex) || data.startIndex === undefined ||
          data.startIndex < 0 || !Number.isSafeInteger(data.totalCount) || data.totalCount === undefined ||
          data.startIndex + data.messages.length > data.totalCount || data.messages.length > 50 ||
          hit.messageIndex < data.startIndex || hit.messageIndex >= data.startIndex + data.messages.length) {
        set((state) => ({ chatSearchNavigation: null, chatHistoryErrorBySessionId: { ...state.chatHistoryErrorBySessionId,
          [hit.chatId]: response.error || 'Chat history changed. Try opening the message again.' } }))
        return false
      }
      if (unsentIndicesFromPage(data) && current.acpBindingBySessionId[hit.chatId] !== observedBinding) {
        set({ chatSearchNavigation: null, ...historyChangedDuringLoad(current, hit.chatId) })
        return false
      }
      const page = data.messages.map((message, index) => buildMessageFromCpp(hit.chatId, message, data.startIndex! + index))
      loadedMessagesDigestByChatId.set(hit.chatId, data.messagesDigest)
      set((state) => ({
        ...unsentProjectionFromPage(state, hit.chatId, data, observedBinding),
        chatSearchNavigation: { ...target, phase: 'ready' },
        messages: { ...state.messages, [hit.chatId]: page },
        historyStartIndexBySessionId: { ...state.historyStartIndexBySessionId, [hit.chatId]: data.startIndex! },
        sessions: state.sessions.map((session) => session.id === hit.chatId
          ? { ...session, messageCount: data.totalCount, userMessageIndices: userMessageIndicesFromPage(data) } : session),
        chatHistoryErrorBySessionId: Object.fromEntries(Object.entries(state.chatHistoryErrorBySessionId).filter(([id]) => id !== hit.chatId)),
      }))
      return true
    },

    consumeChatSearchJump: (chatId: string, generation: number) => {
      const target = get().chatSearchNavigation
      if (!target || get().activeSessionId !== chatId || target.chatId !== chatId || target.generation !== generation || target.phase !== 'ready' || target.anchored) return false
      set({ chatSearchNavigation: { ...target, anchored: true } })
      return true
    },

    clearChatSearchNavigation: async (queryRequestId?: string) => {
      const target = get().chatSearchNavigation
      if (!target || (queryRequestId && target.queryRequestId !== queryRequestId)) return
      ++searchJumpGeneration
      const chatId = target.chatId
      const reloadGeneration = searchJumpGeneration
      boundedTailReloads.set(chatId, reloadGeneration)
      set({ chatSearchNavigation: null })
      try {
        if (!isCefContext() || get().activeSessionId !== chatId) return
        const hydrationGeneration = (messageHydrationGenerationByChatId.get(chatId) ?? 0) + 1
        messageHydrationGenerationByChatId.set(chatId, hydrationGeneration)
        const selectionRevision = intentionalSelectionRevision
        const observedBinding = get().acpBindingBySessionId[chatId]
        const response = await sendToCEF<ChatMessagesResponse>({ action: 'getChatMessages', payload: { chatId, limit: 50, deferToolCallContent: true } })
        if (get().chatSearchNavigation || get().activeSessionId !== chatId || intentionalSelectionRevision !== selectionRevision ||
            messageHydrationGenerationByChatId.get(chatId) !== hydrationGeneration || !get().sessions.some((session) => session.id === chatId)) return
        const data = response.data
        if (!response.ok || !data || data.chatId !== chatId || !Array.isArray(data.messages) || data.messages.length > 50 ||
            !Number.isSafeInteger(data.startIndex) || data.startIndex === undefined || data.startIndex < 0 ||
            !Number.isSafeInteger(data.totalCount) || data.totalCount === undefined || data.startIndex + data.messages.length !== data.totalCount) {
          set((state) => ({ chatHistoryErrorBySessionId: { ...state.chatHistoryErrorBySessionId,
            [chatId]: response.error || 'Could not return to the latest messages. Retry chat history.' } }))
          return
        }
        if (unsentIndicesFromPage(data) && get().acpBindingBySessionId[chatId] !== observedBinding) {
          set((state) => historyChangedDuringLoad(state, chatId))
          return
        }
        loadedMessagesDigestByChatId.set(chatId, data.messagesDigest ?? '')
        set((state) => ({ ...unsentProjectionFromPage(state, chatId, data, observedBinding), messages: { ...state.messages, [chatId]: data.messages!.map((message, index) => buildMessageFromCpp(chatId, message, data.startIndex! + index)) },
          historyStartIndexBySessionId: { ...state.historyStartIndexBySessionId, [chatId]: data.startIndex! },
          sessions: state.sessions.map((session) => session.id === chatId ? { ...session, messageCount: data.totalCount, userMessageIndices: userMessageIndicesFromPage(data) } : session),
        }))
      } finally { if (boundedTailReloads.get(chatId) === reloadGeneration) boundedTailReloads.delete(chatId) }
    },

    loadSessionMessages: requestChatMessagesFromCef,

    loadOlderSessionMessages: async (id: string) => {
      if (!isCefContext()) return false
      const hydrationGeneration = messageHydrationGenerationByChatId.get(id) ?? 0
      const before = get().historyStartIndexBySessionId[id] ?? 0
      const observedBinding = get().acpBindingBySessionId[id]
      if (before <= 0 || !get().messages[id]) return false
      const fail = (error: string) => {
        set((state) => ({ chatHistoryErrorBySessionId: {
          ...state.chatHistoryErrorBySessionId, [id]: error,
        } }))
        return false
      }
      const response = await sendToCEF<ChatMessagesResponse>({
        action: 'getChatMessages',
        payload: { chatId: id, limit: 100, before, deferToolCallContent: true },
      })
      if ((messageHydrationGenerationByChatId.get(id) ?? 0) !== hydrationGeneration ||
          !get().sessions.some((session) => session.id === id) ||
          get().historyStartIndexBySessionId[id] !== before || get().chatSearchNavigation?.chatId === id) return false
      const data = response.data
      if (!response.ok) return fail(response.error || 'Could not load earlier messages.')
      if (!data || (data.chatId && data.chatId !== id) || !Array.isArray(data.messages) || data.messages.length === 0 ||
          !Number.isSafeInteger(data.startIndex) || data.startIndex === undefined ||
          data.startIndex < 0 || data.startIndex + data.messages.length !== before) return fail('The earlier message page was invalid.')
      if (data.messagesDigest !== loadedMessagesDigestByChatId.get(id)) return fail('Chat history changed while loading. Retry to refresh it.')
      if (unsentIndicesFromPage(data) && get().acpBindingBySessionId[id] !== observedBinding) return fail('Chat history changed while loading. Retry to refresh it.')
      const page = data.messages.map((message, index) => buildMessageFromCpp(id, message, data.startIndex! + index))
      let added = false
      set((state) => {
        if (state.historyStartIndexBySessionId[id] !== before || !state.messages[id]) return state
        added = true
        return {
          ...unsentProjectionFromPage(state, id, data, observedBinding),
          messages: { ...state.messages, [id]: [...page, ...state.messages[id]] },
          historyStartIndexBySessionId: { ...state.historyStartIndexBySessionId, [id]: data.startIndex! },
          chatHistoryErrorBySessionId: Object.fromEntries(
            Object.entries(state.chatHistoryErrorBySessionId).filter(([chatId]) => chatId !== id)
          ),
        }
      })
      return added
    },

    unloadSessionMessages: (id: string) => {
      const current = get()
      if (current.acpBindingBySessionId[id]?.processing || current.cliBindingBySessionId[id]?.processing) return
      messageHydrationGenerationByChatId.set(id, (messageHydrationGenerationByChatId.get(id) ?? 0) + 1)
      discardPendingTranscriptPushesForChat(id)
      if (current.messages[id] === undefined && current.cliTranscriptBySessionId[id] === undefined && !current.chatHistoryErrorBySessionId[id] && !current.chatHistoryReadBySessionId[id] && !current.chatHistoryPageBySessionId?.[id]) return
      loadedMessagesDigestByChatId.delete(id)
      const messages = { ...current.messages }
      const historyStartIndexBySessionId = { ...current.historyStartIndexBySessionId }
      const cliTranscriptBySessionId = { ...current.cliTranscriptBySessionId }
      const chatHistoryErrorBySessionId = { ...current.chatHistoryErrorBySessionId }
      const chatHistoryReadBySessionId = { ...current.chatHistoryReadBySessionId }
      const chatHistoryPageBySessionId = { ...current.chatHistoryPageBySessionId }
      delete messages[id]
      delete historyStartIndexBySessionId[id]
      delete cliTranscriptBySessionId[id]
      delete chatHistoryErrorBySessionId[id]
      delete chatHistoryReadBySessionId[id]
      delete chatHistoryPageBySessionId[id]
      set({ messages, historyStartIndexBySessionId, cliTranscriptBySessionId, chatHistoryErrorBySessionId, chatHistoryReadBySessionId, chatHistoryPageBySessionId })
      if (isCefContext() && !isCompanionContext()) void sendToCEF({ action: 'releaseChatMessages', payload: { chatId: id } })
    },

    addSession: async (name: string, folderId: string | null, providerId = '', modelId?: string, reasoningEffort?: string, viewMode: ViewMode = 'chat', executionHostId = 'local', workspaceDirectory = '', routingMode: 'explicit' | 'automatic' = 'explicit', options: CreateSessionOptions = {}) => {
      const current = get()
      const selectedFolderId = folderId && current.folders.some((folder) => folder.id === folderId)
        ? folderId
        : null
      const isRemote = executionHostId !== 'local'
	  const selectedFolder = current.folders.find((folder) => folder.id === selectedFolderId)
	  if (selectedFolder && (selectedFolder.executionHostId || 'local') !== executionHostId) {
		console.error('[UAM] createSession workspace belongs to a different computer')
		return false
	  }
      if (!selectedFolderId && !isRemote) {
        console.error('[UAM] createSession requires a workspace folder')
        return false
      }
      if (!isRecord(options) || Object.keys(options).some((key) => !['gitBaseBranch', 'sourceChatId', 'expectedWorkspaceDirectory', 'expectedExecutionHostId'].includes(key))) return false
      const hasWorktreeContext = ['sourceChatId', 'expectedWorkspaceDirectory', 'expectedExecutionHostId'].some((key) => options[key as keyof CreateSessionOptions] !== undefined)
      if (hasWorktreeContext) {
        const source = current.sessions.find((session) => session.id === options.sourceChatId)
        const bounded = (value: unknown, maximum: number) => typeof value === 'string' && value.length > 0 && !/[\u0000-\u001f\u007f]/.test(value) && new TextEncoder().encode(value).length <= maximum
        if (!bounded(options.sourceChatId, 512) || !bounded(options.expectedWorkspaceDirectory, 16384) ||
          typeof options.expectedExecutionHostId !== 'string' || options.expectedExecutionHostId !== 'local' ||
          !isCefContext() || isRemote || routingMode !== 'explicit' || options.gitBaseBranch !== undefined ||
          !source || source.importedReadOnly || source.workspaceIsolationKind !== 'gitWorktree' || source.folderId !== selectedFolderId ||
          (source.executionHostId || 'local') !== options.expectedExecutionHostId || source.workspaceWorktreeDirectory !== options.expectedWorkspaceDirectory) return false
      }
      const hasBaseOverride = options.gitBaseBranch !== undefined
      const gitBaseBranch = hasBaseOverride ? sanitizeGitBaseBranch(options.gitBaseBranch) : undefined
      if (hasBaseOverride && (gitBaseBranch === undefined || routingMode !== 'explicit' || isRemote ||
        !selectedFolder?.directory || !current.gitEnabled || !current.gitAutoWorktree || !isCefContext())) return false
      const normalizedProviderId = normalizeCliProviderIdAlias(providerId)
      const effectiveDefaults = routingMode === 'explicit' ? defaultsForExecutionHost(current, executionHostId) : current
      const normalizedDefaultProviderId = normalizeCliProviderIdAlias(effectiveDefaults.defaultNewChatProviderId)
      const requestedProviderId = current.providers.some((provider) => provider.id === normalizedProviderId)
        ? normalizedProviderId
        : current.providers.some((provider) => provider.id === normalizedDefaultProviderId)
          ? normalizedDefaultProviderId
          : GEMINI_CLI_PROVIDER_ID
      if (routingMode === 'automatic' && !canRouteAutomatically(current.executionRouting, selectedFolder, requestedProviderId)) return false
      const defaults = providerChatDefaultsForNewChat({ ...current, providerChatDefaults: effectiveDefaults.providerChatDefaults }, requestedProviderId)
      if (modelId !== undefined) defaults.modelId = modelId.trim()
	  if (reasoningEffort !== undefined) defaults.reasoningEffort = normalizeCodexReasoningEffort(reasoningEffort)

      if (routingMode === 'automatic') { defaults.modelId = ''; defaults.reviewerModelId = ''; defaults.reasoningEffort = ''; defaults.serviceTier = '' }
      const requestDefaults = routingMode === 'automatic'
        ? Object.fromEntries(Object.entries(defaults).filter(([key]) => !['modelId', 'reviewerModelId', 'reasoningEffort', 'serviceTier'].includes(key)))
        : defaults
      if (isCefContext()) {
        const resp = await sendToCEF<{ chatId?: string }>({
          action: 'createSession',
          payload: {
            ...(hasBaseOverride ? { gitBaseBranch } : {}),
            // A branch was discovered for this directory; native creation rejects a remapped folder.
            ...(hasBaseOverride ? { workspaceDirectory: selectedFolder!.directory } : {}),
            ...(hasWorktreeContext ? { sourceChatId: options.sourceChatId, expectedWorkspaceDirectory: options.expectedWorkspaceDirectory, expectedExecutionHostId: options.expectedExecutionHostId } : {}),
            title: name,
            ...(routingMode === 'automatic' ? { routingMode } : {}),
            folderId: selectedFolderId ?? '',
            providerId: requestedProviderId,
            defaults: requestDefaults,
            ...(executionHostId === 'local' ? {} : { executionHostId, workspaceDirectory }),
          },
        })
        if (!resp.ok) {
          console.error('[CEF] createSession failed:', resp.error)
          return false
        }

        const chatId = resp.data?.chatId ?? ''
		if (chatId) writeChatViewMode(chatId, viewMode)
        let appliedViewMode = viewMode === 'chat'
        set((state) => ({
          ...(isCompanionContext() && chatId ? { activeSessionId: chatId } : {}),
          isNewChatModalOpen: false,
          newChatFolderId: null,
          sessions: viewMode === 'chat' ? state.sessions : state.sessions.map((session) => {
            if (session.id !== chatId) return session
            appliedViewMode = true
            return { ...session, viewMode }
          }),
        }))
        if (!appliedViewMode && chatId) pendingViewModeBySessionId.set(chatId, viewMode)
        return true
      }

      // Dev/mock path
      sessionCounter++
      const id = makeId('s', sessionCounter)
      const now = new Date()
      const session: Session = {
        id,
        routingMode,
        executionHostId,
        name,
        viewMode,
        folderId: selectedFolderId,
        providerId: requestedProviderId,
		...(isRemote ? { workspaceDirectory } : {}),
        modelId: defaults.modelId,
        reviewerModelId: defaults.reviewerModelId,
        reasoningEffort: defaults.reasoningEffort,
        serviceTier: defaults.serviceTier,
        serviceTierExplicit: defaults.serviceTier !== '',
        approvalMode: defaults.approvalMode,
        commandSafetyTier: defaults.commandSafetyTier,
        memoryLevel: 'default',
        memoryEnabled: true,
        smallModelMode: defaults.smallModelMode,
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
      return true
    },

    discardUnsentMessage: async (id: string, messageIndex: number, expectedMessageId: string) => {
      const state = get()
      const chat = state.sessions.find((session) => session.id === id)
      const binding = state.acpBindingBySessionId[id]
      const message = state.messages[id]?.[messageIndex - (state.historyStartIndexBySessionId[id] ?? 0)]
      if (!isCefContext() || isCompanionContext() || !chat || chat.importedReadOnly || !expectedMessageId || message?.id !== expectedMessageId ||
          !Number.isSafeInteger(messageIndex) || messageIndex < 0 || !binding?.unsentMessageIndices?.includes(messageIndex) ||
          message.role !== 'user' || !message.acpPromptNotSent || message.acpPromptDiscarded) return false
      // Probe the clicked row without replacing a paged or search-anchored transcript with its tail.
      const page = await sendToCEF<ChatMessagesResponse>({ action: 'getChatMessages', payload: {
        chatId: id, limit: 1, before: messageIndex + 1, deferToolCallContent: true, messagesDigest: '',
      } })
      const data = page.data
      const current = get()
      if (!page.ok || !data?.messagesDigest || (data.chatId && data.chatId !== id) || data.startIndex !== messageIndex ||
          !Number.isSafeInteger(data.totalCount) || data.totalCount! <= messageIndex || data.messages?.length !== 1 ||
          reconcileCppMessages(id, [message], data.messages, true, messageIndex)[0] !== message ||
          current.messages[id]?.[messageIndex - (current.historyStartIndexBySessionId[id] ?? 0)] !== message ||
          !current.sessions.some((session) => session.id === id && !session.importedReadOnly) ||
          !current.acpBindingBySessionId[id]?.unsentMessageIndices?.includes(messageIndex)) return false
      const response = await sendToCEF({ action: 'manageQueuedAcpPrompt', payload: {
        chatId: id, operation: 'discardUnsent', messageIndex, expectedMessagesDigest: data.messagesDigest,
      } })
      if (!response.ok) return false
      loadedMessagesDigestByChatId.delete(id)
      set((state) => {
        const index = messageIndex - (state.historyStartIndexBySessionId[id] ?? 0)
        if (state.messages[id]?.[index] !== message) return state
        const messages = [...state.messages[id]]
        messages[index] = { ...message, acpPromptDiscarded: true }
        return { messages: { ...state.messages, [id]: messages } }
      })
      return true
    },

    editUnsentMessage: async (id: string, messageIndex: number, expectedMessage: Message, text: string, retainedAttachments: Attachment[]) => {
      const changed = () => ({ ok: false, error: 'The unsent message changed. Your edit is kept; reopen it before saving.' })
      if (!isCefContext() || isCompanionContext()) return { ok: false, error: 'Editing unsent messages requires the desktop runtime.' }
      const initialChat = get().sessions.find((session) => session.id === id)
      const ownsRow = (state: AppState, message = expectedMessage) => {
        if (!isCefContext() || isCompanionContext()) return false
        const chat = state.sessions.find((session) => session.id === id)
        const binding = state.acpBindingBySessionId[id]
        const cli = state.cliBindingBySessionId[id]
        const index = messageIndex - (state.historyStartIndexBySessionId[id] ?? 0)
        const rows = state.messages[id] ?? []
        if (!chat || chat.importedReadOnly || !initialChat || chat.providerId !== initialChat.providerId ||
            chat.createdAt.getTime() !== initialChat.createdAt.getTime() || chat.executionHostId !== initialChat.executionHostId || !Number.isSafeInteger(messageIndex) || messageIndex < 0 ||
            rows[index] !== message || message.nativeMessageIndex !== messageIndex ||
            message.role !== 'user' || !message.acpPromptNotSent || message.acpPromptDiscarded ||
            !binding?.unsentMessageIndices?.includes(messageIndex) || binding.processing || binding.pendingPermission || binding.pendingUserInput ||
            binding.queuedPrompts?.length || binding.pendingDirectPrompts?.length || binding.unsentDirectPrompts?.length ||
            cli?.processing || cli?.turnState === 'busy') return false
        const providerId = normalizeCliProviderIdAlias(chat.providerId || binding.providerId)
        if (normalizeCliProviderIdAlias(binding.providerId) !== providerId ||
            (message.providerId && normalizeCliProviderIdAlias(message.providerId) !== providerId)) return false
        // An allowlist cannot override substantive output already visible after the input.
        for (let next = index + 1; next < rows.length && rows[next].role !== 'user'; next += 1) {
          const output = rows[next]
          if (output.role !== 'assistant' || output.content.trim() || output.thoughts?.trim() || output.planSummary?.trim() ||
              output.planEntries?.length || output.toolCalls?.length || output.blocks?.length || output.attachments?.length) return false
        }
        return true
      }
      if (!ownsRow(get())) return changed()
      const originalAttachments = expectedMessage.attachments ?? []
      const key = (attachment: Attachment) => `${attachment.type === 'markdown-store' ? 'skill' : 'file'}:${attachment.id}`
      const selected = new Set(retainedAttachments.map(key))
      if (selected.size !== retainedAttachments.length || retainedAttachments.some((attachment) => !attachment.id ||
          !originalAttachments.some((original) => key(original) === key(attachment) && attachmentsEquivalent([original], [attachment])))) {
        return { ok: false, error: 'The retained files or skills changed. Your edit is kept.' }
      }
      const retained = originalAttachments.filter((attachment) => selected.has(key(attachment)))
      // Match native strings::Trim; JavaScript trim would also remove authored non-ASCII edge characters.
      const normalizedText = text.replace(/^[ \t\r\n\f\v]+|[ \t\r\n\f\v]+$/g, '')
      if ((!normalizedText && retained.length === 0) || new TextEncoder().encode(normalizedText).length > 1024 * 1024) {
        return { ok: false, error: 'Enter a message or retain context, within the message size limit.' }
      }
      const validPage = (data: ChatMessagesResponse | undefined) => Boolean(data && data.chatId === id && data.startIndex === messageIndex &&
        Number.isSafeInteger(data.totalCount) && data.totalCount! > messageIndex && /^[a-f0-9]{16}$/i.test(data.messagesDigest ?? '') &&
        data.unchanged !== true && Array.isArray(data.messages) && data.messages.length === 1)
      try {
        const probe = await sendToCEF<ChatMessagesResponse>({ action: 'getChatMessages', payload: {
          chatId: id, limit: 1, before: messageIndex + 1, deferToolCallContent: true, messagesDigest: '',
        } })
        if (!probe.ok) return { ok: false, error: probe.error || 'Could not check the unsent message. Your edit is kept.' }
        if (!validPage(probe.data) || !ownsRow(get()) ||
            reconcileCppMessages(id, [expectedMessage], probe.data!.messages!, true, messageIndex)[0] !== expectedMessage) return changed()
        const observedChat = get().sessions.find((session) => session.id === id)
        const response = await sendToCEF<ChatMessagesResponse>({ action: 'manageQueuedAcpPrompt', payload: {
          chatId: id, operation: 'editUnsent', messageIndex, expectedMessagesDigest: probe.data!.messagesDigest,
          text, retainedAttachmentIds: retained.filter((attachment) => attachment.type !== 'markdown-store').map((attachment) => attachment.id),
          retainedMarkdownStoreFiles: retained.filter((attachment) => attachment.type === 'markdown-store').map((attachment) => attachment.id),
        } })
        if (!response.ok) return { ok: false, error: response.error || 'Could not save the unsent message. Your edit is kept.' }
        const data = response.data
        const expectedEdited = { ...expectedMessage, content: normalizedText, attachments: retained }
        // A generic ACK, another row, changed immutable data or omitted retained context is not a committed edit page.
        if (!validPage(data) || data!.unchanged !== false || data!.totalCount !== probe.data!.totalCount ||
            reconcileCppMessages(id, [expectedEdited], data!.messages!, true, messageIndex)[0] !== expectedEdited) {
          return { ok: false, error: 'The saved message could not be confirmed. Your edit is kept; check the chat before retrying.' }
        }
        loadedMessagesDigestByChatId.delete(id)
        let applied = false
        set((state) => {
          const index = messageIndex - (state.historyStartIndexBySessionId[id] ?? 0)
          if (!ownsRow(state)) {
            // The native commit may push its exact row before this ACK. Confirm it without rewriting a newer projection.
            const pushed = state.messages[id]?.[index]
            const currentChat = state.sessions.find((session) => session.id === id)
            applied = Boolean(pushed && currentChat && ownsRow(state, pushed) && currentChat.messagesDigest === data!.messagesDigest &&
              currentChat.messageCount === data!.totalCount && reconcileCppMessages(id, [pushed], data!.messages!, true, messageIndex)[0] === pushed)
            return state
          }
          const messages = [...state.messages[id]]
          messages[index] = reconcileCppMessages(id, [expectedMessage], data!.messages!, true, messageIndex)[0]
          applied = true
          return { messages: { ...state.messages, [id]: messages },
            sessions: state.sessions.map((session) => session === observedChat
              ? { ...session, messagesDigest: data!.messagesDigest, messageCount: data!.totalCount } : session) }
        })
        return applied ? { ok: true } : changed()
      } catch {
        return { ok: false, error: 'Could not save the unsent message. Your edit is kept; check the chat before retrying.' }
      }
    },

    retryFailedMessage: async (id: string, messageIndex: number) => {
      if (!isCefContext()) return { ok: false, error: 'Retry requires the desktop runtime.' }
      const response = await sendToCEF({ action: 'retryFailedMessage', payload: { chatId: id, messageIndex } })
      if (response.ok) await requestChatMessagesFromCef(id, true)
      return { ok: response.ok, error: response.error }
    },

    branchFromMessage: async (id: string, messageIndex: number, content?: string, retainedAttachmentIds?: string[], retainedMarkdownStoreFiles?: string[]): Promise<string | null> => {
      const nativeSelection = retainedAttachmentIds === undefined ? undefined : Object.freeze([...retainedAttachmentIds])
      const skillSelection = retainedMarkdownStoreFiles === undefined ? undefined : Object.freeze([...retainedMarkdownStoreFiles])
      if (isCefContext()) {
        const operationId = createRequestId('branch')
        const payload = Object.freeze({
          chatId: id,
          messageIndex,
          ...(content === undefined ? {} : { content }),
          ...(nativeSelection === undefined ? {} : { retainedAttachmentIds: nativeSelection }),
          ...(skillSelection === undefined ? {} : { retainedMarkdownStoreFiles: skillSelection }),
          ...(operationId ? { operationId } : {}),
        })
        let response = await sendToCEF<{ chatId?: string; warning?: string }>({
          action: 'branchFromMessage',
          payload,
        })
        if (!response.ok && operationId && /connection lost|action may have reached|timed out/i.test(response.error ?? '')) {
          response = await sendToCEF<{ chatId?: string; warning?: string }>({ action: 'branchFromMessage', payload })
        }
        if (!response.ok) {
          console.error('[CEF] branchFromMessage failed:', response.error)
          throw new Error(response.error || 'Could not create the message branch.')
        }
        if (response.data?.warning) set({ statusLine: response.data.warning })
        return response.data?.chatId?.trim() || null
      }

      const source = get().sessions.find((session) => session.id === id)
      const sourceMessages = get().messages[id] ?? []
      const sourceMessage = sourceMessages[messageIndex]
      if (!source || sourceMessage?.role !== 'user') return null
      const attachments = sourceMessage.attachments ?? []
      const validSelection = (selection: readonly string[] | undefined, markdownStore: boolean) => selection === undefined ||
        (new Set(selection).size === selection.length && selection.every((id) => id && attachments.filter((attachment) =>
          (attachment.type === 'markdown-store') === markdownStore && attachment.id === id).length === 1))
      if (!validSelection(nativeSelection, false) || !validSelection(skillSelection, true)) return null
      const retainedAttachments = attachments.filter((attachment) => {
        const selection = attachment.type === 'markdown-store' ? skillSelection : nativeSelection
        return selection === undefined || selection.includes(attachment.id)
      })
      const hasContext = retainedAttachments.some((attachment) => attachment.type !== 'markdown-store') ||
        (skillSelection !== undefined && retainedAttachments.some((attachment) => attachment.type === 'markdown-store'))
      if (content === undefined) {
        if (!sourceMessage.content.trim() && retainedAttachments.length === 0) return null
      } else if (!content.trim() && !hasContext) return null

      sessionCounter++
      const branchId = makeId('branch', sessionCounter)
      const now = new Date()
      const branchMessages = sourceMessages.slice(0, messageIndex + 1).map((message, index) => ({
        ...message,
        sessionId: branchId,
        ...(index === messageIndex && content !== undefined ? { content } : {}),
        ...(index === messageIndex && (nativeSelection !== undefined || skillSelection !== undefined) ? { attachments: retainedAttachments } : {}),
      }))
      const branch: Session = {
        ...source,
        id: branchId,
        name: `Branch: ${(content ?? sourceMessage.content).trim().slice(0, 40)}`,
        parentChatId: source.id,
        branchRootChatId: source.branchRootChatId || source.id,
        branchFromMessageIndex: messageIndex,
        branchMessageEdited: content !== undefined,
        messageCount: branchMessages.length,
        messagesDigest: '',
        createdAt: now,
        updatedAt: now,
        lastOpenedAt: now,
        ...(source.workspaceIsolationKind === 'gitWorktree' ? {
          workspaceDirectory: source.workspaceSourceDirectory ?? '',
          workspaceIsolationKind: '',
          workspaceSourceDirectory: '',
          workspaceBaseRef: '',
          workspaceBranchName: '',
          workspaceWorktreeDirectory: '',
        } : {}),
      }
      set((state) => ({
        sessions: [branch, ...state.sessions],
        messages: { ...state.messages, [branchId]: branchMessages },
        activeSessionId: branchId,
      }))
      return branchId
    },

    openSessionWorkspace: async (id: string) => {
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

    openSessionWorkspaceEditor: async (id: string) => {
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

    openSessionTerminal: async (id: string) => {
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

    openSubAgentSession: async (sourceChatId: string, nativeSessionId: string, title = '', selectChat = true) => {
      if (isCefContext()) {
        const source = get().sessions.find(candidate => candidate.id === sourceChatId)
        const selectionRevision = selectChat ? ++intentionalSelectionRevision : intentionalSelectionRevision
        const response = await sendToCEF<OpenNativeSessionChatResponse>({
          action: 'openNativeSessionChat',
          payload: {
            chatId: sourceChatId,
            nativeSessionId,
            title,
            selectChat: false,
            refreshHistory: !selectChat,
          },
        })

        if (!response.ok) {
          console.error('[CEF] openNativeSessionChat failed:', response.error)
          return null
        }

        const chatId = response.data?.chatId?.trim() ?? ''
        if (selectChat && intentionalSelectionRevision !== selectionRevision) return null
        const currentSource = get().sessions.find(candidate => candidate.id === sourceChatId)
        if (!source || !currentSource || source.createdAt.getTime() !== currentSource.createdAt.getTime() ||
            source.providerId !== currentSource.providerId || source.executionHostId !== currentSource.executionHostId ||
            source.workspaceDirectory !== currentSource.workspaceDirectory || source.workspaceWorktreeDirectory !== currentSource.workspaceWorktreeDirectory || source.folderId !== currentSource.folderId) return null
        if (chatId) rememberSubagentParent(source, chatId)
        if (chatId && selectChat) {
          const child = get().sessions.find(candidate => candidate.id === chatId)
          if (child && canExecuteNativeSubagent(child)) get().setActiveSession(chatId)
          else {
            get().setActiveSession(source.id, { preserveSubagentRoute: true })
            openSubagentView(source, { kind: 'native', chatId })
          }
        }
        else if (chatId) requestChatMessagesFromCef(chatId)
        return chatId || null
      }

      const sourceChat = get().sessions.find((candidate) => candidate.id === sourceChatId)
      return sourceChat && nativeSessionId.trim() ? sourceChatId : null
    },

    getChatWorktreeStatus: async (id: string): Promise<GitWorktreeStatus | null> => {
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
		managedRepository: false,
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

    createChatWorktree: async (id: string): Promise<GitWorktreeResult> => {
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

    discardChatWorktreeChanges: async (id: string): Promise<GitWorktreeResult> => {
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

    portChatWorktreeChanges: async (id: string): Promise<GitWorktreeResult> => {
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

    previewChatTurnRollback: async (id: string, messageIndex: number): Promise<GitTurnCheckpointResult | null> => {
      if (!isCefContext()) return null
      const response = await sendToCEF<GitTurnCheckpointResult>({
        action: 'previewChatTurnRollback',
        payload: { chatId: id, messageIndex },
      })
      if (!response.ok) {
        console.error('[CEF] previewChatTurnRollback failed:', response.error)
        return null
      }
      return response.data ?? null
    },

    rollbackChatTurn: async (id: string, messageIndex: number): Promise<GitTurnCheckpointResult | null> => {
      if (!isCefContext()) return null
      const response = await sendToCEF<GitTurnCheckpointResult>({
        action: 'rollbackChatTurn',
        payload: { chatId: id, messageIndex },
      })
      if (!response.ok) {
        console.error('[CEF] rollbackChatTurn failed:', response.error)
        return null
      }
      return response.data ?? null
    },

    getChatFileChangeReceipts: async (id: string): Promise<ChatFileChangeReceiptsResponse> => {
      const response = await sendToCEF<ChatFileChangeReceiptsResponse>({ action: 'getChatFileChangeReceipts', payload: { chatId: id } })
      if (!response.ok || !response.data) throw new Error(response.error || 'Could not load chat file changes.')
      return response.data
    },

    getVcsCommitStatus: async (id: string, vcsType: VcsType = 'git', options: { includeLineStats?: boolean; contextOnly?: boolean; requestId?: string; comparisonRef?: string } = {}): Promise<VcsCommitStatus | null> => {
      if (isCefContext()) {
        const response = await sendToCEF<VcsCommitStatus>({
          action: 'getVcsCommitStatus',
          payload: {
            chatId: id,
            vcsType,
            includeLineStats: options.includeLineStats ?? true,
            contextOnly: options.contextOnly ?? false,
            requestId: options.requestId,
            comparisonRef: options.comparisonRef,
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

    getWorkspaceFilePreview: async (id: string, path: string) => {
      if (!isCefContext()) throw new Error('File inspection requires the desktop app or its connected companion.')
      const response = await sendToCEF({ action: 'getWorkspaceFilePreview', payload: { chatId: id, path } }, false)
      if (!response.ok) throw new Error(response.error || 'Could not load this file.')
      const file = filePreviewResponse(response.data)
      if (!file) throw new Error('The runner returned an invalid file preview.')
      return file
    },

    getVcsFileDiff: async (id: string, path: string, vcsType: VcsType, comparisonRef?: string): Promise<string> => {
      if (isCefContext()) {
        const response = await sendToCEF<VcsFileDiffResponse>({
          action: 'getVcsFileDiff',
          payload: { chatId: id, path, vcsType, comparisonRef },
        })
        if (!response.ok) {
          console.error('[CEF] getVcsFileDiff failed:', response.error)
          throw new Error(response.error || 'Failed to load this diff.')
        }
        return typeof response.data?.diff === 'string' ? response.data.diff : ''
      }

      return `diff -- ${path}\n`
    },

    commitVcsChanges: async (id: string, vcsType: VcsType, message: string, files: string[]): Promise<VcsCommitResult> => {
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

    generateVcsCommitMessage: async (id: string, vcsType: VcsType, files: string[]): Promise<VcsCommitMessageSuggestion | null> => {
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

    renameSession: (id: string, name: string) => {
      if (isCefContext()) {
        const previousSession = get().sessions.find((s) => s.id === id)
        if (!previousSession) {
          return
        }

        const requestKey = `renameSession:${id}`
        const requestId = createRequestId('renameSession')
        const optimisticUpdatedAt = new Date()
        rememberPendingRequest(requestKey, requestId)
        set((state) => ({
          sessions: state.sessions.map((s) =>
            s.id === id ? { ...s, name, updatedAt: optimisticUpdatedAt } : s
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
              sessions: state.sessions.map((s) => (s.id === id ? {
                ...s,
                name: s.name === name ? previousSession.name : s.name,
                updatedAt: s.updatedAt === optimisticUpdatedAt ? previousSession.updatedAt : s.updatedAt,
              } : s)),
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

    setSessionPinned: async (id: string, pinned: boolean): Promise<boolean> => {
      const previousSession = get().sessions.find((s) => s.id === id)
      if (!previousSession) {
        return false
      }

      if ((previousSession.isPinned ?? false) === pinned) {
        return true
      }

      set({ organizationMutationSequence: get().organizationMutationSequence + 1 })
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
        const pinChanges = pendingPinChanges.get(id) ?? {
          confirmedPinned: previousSession.isPinned,
          confirmedSequence: 0,
          latestSequence: 0,
          pendingCount: 0,
        }
        const sequence = ++pinChanges.latestSequence
        pinChanges.pendingCount++
        pendingPinChanges.set(id, pinChanges)
        rememberPendingRequest(requestKey, requestId)
        applyPinned()
        const response = await sendToCEF({
          action: 'setChatPinned',
          payload: { chatId: id, pinned },
          requestId,
        })

        const isLatest = isLatestPendingRequest(requestKey, response.requestId)
        if (--pinChanges.pendingCount === 0) {
          pendingPinChanges.delete(id)
        }

        if (response.ok) {
          if (sequence > pinChanges.confirmedSequence) {
            pinChanges.confirmedPinned = pinned
            pinChanges.confirmedSequence = sequence
            // A later rejection may already have removed the optimistic value.
            if (!isLatest && !pendingRequestIdsByKey.has(requestKey)) {
              applyPinned()
            }
          }
          clearPendingRequest(requestKey, response.requestId)
          return true
        }

        if (isLatest) {
          clearPendingRequest(requestKey, response.requestId)
          set((state) => ({
            sessions: state.sessions.map((s) =>
              s.id === id ? { ...s, isPinned: pinChanges.confirmedPinned } : s
            ),
          }))
          reportAppFailure('Could not save the pin change.', 'The chat was returned to its previous state. Try again.')
        }

        return false
      }

      applyPinned()
      return true
    },

    setSessionProvider: async (id: string, providerId: string): Promise<boolean> => {
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
      const cli = current.cliBindingBySessionId[id]
      if (acp?.processing || (acp?.queuedPrompts?.length ?? 0) > 0 || acp?.pendingPermission || acp?.pendingUserInput || cli?.processing || cli?.turnState === 'busy') {
        return false
      }
      const defaults = sanitizeProviderChatDefaults(current.providerChatDefaults[requestedProviderId] ?? null)
      const optimisticUpdatedAt = new Date()

      const applyProvider = () => {
        set((state) => ({
          sessions: state.sessions.map((s) =>
            s.id === id ? {
              ...s,
              providerId: requestedProviderId,
              modelId: defaults.modelId,
              reviewerModelId: defaults.reviewerModelId,
              reasoningEffort: defaults.reasoningEffort,
              serviceTier: providerCapabilities(requestedProviderId).hasServiceTier ? defaults.serviceTier : '',
              serviceTierExplicit: providerCapabilities(requestedProviderId).hasServiceTier && defaults.serviceTier !== '',
              approvalMode: defaults.approvalMode,
              commandSafetyTier: defaults.commandSafetyTier,
              smallModelMode: defaults.smallModelMode,
              updatedAt: optimisticUpdatedAt,
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
            sessions: state.sessions.map((s) => s.id === id ? {
              ...s,
              providerId: previousSession.providerId,
              modelId: previousSession.modelId,
              reviewerModelId: previousSession.reviewerModelId,
              reasoningEffort: previousSession.reasoningEffort,
              serviceTier: previousSession.serviceTier,
              serviceTierExplicit: previousSession.serviceTierExplicit,
              approvalMode: previousSession.approvalMode,
              commandSafetyTier: previousSession.commandSafetyTier,
              memoryLevel: previousSession.memoryLevel,
              memoryEnabled: previousSession.memoryEnabled,
              smallModelMode: previousSession.smallModelMode,
              updatedAt: s.updatedAt === optimisticUpdatedAt ? previousSession.updatedAt : s.updatedAt,
            } : s),
          }))
          pendingRequestIdsByKey.delete(requestKey)
        }

        return false
      }

      applyProvider()
      return true
    },

    setSessionModel: async (id: string, modelId: string): Promise<boolean> => {
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
      const optimisticUpdatedAt = new Date()
      const targetModel = get().acpBindingBySessionId[id]?.availableModels.find((model) => model.id === requestedModelId)
      const targetReasoningEfforts = targetModel?.supportedReasoningEfforts ?? []
      const targetSpeedTiers = targetModel?.additionalSpeedTiers ?? []
      const currentReasoningEffort = previousSession.reasoningEffort ?? ''
      const targetDefaultReasoningEffort = targetModel?.defaultReasoningEffort ?? ''
      const requestedReasoningEffort = targetModel && currentReasoningEffort && !targetReasoningEfforts.includes(currentReasoningEffort)
        ? targetReasoningEfforts.includes(targetDefaultReasoningEffort)
          ? targetDefaultReasoningEffort
          : targetReasoningEfforts[0] ?? ''
        : currentReasoningEffort
      const currentServiceTier = previousSession.serviceTier ?? ''
      const requestedServiceTier = targetModel && currentServiceTier && !targetSpeedTiers.includes(currentServiceTier)
        ? ''
        : currentServiceTier
	  const requestedServiceTierExplicit = previousSession.serviceTierExplicit ?? currentServiceTier !== ''

      const applyModel = () => {
        set((state) => ({
          sessions: state.sessions.map((s) =>
			s.id === id ? {
			  ...s,
			  modelId: requestedModelId,
			  reasoningEffort: requestedReasoningEffort,
			  serviceTier: requestedServiceTier,
			  serviceTierExplicit: requestedServiceTierExplicit,
			  updatedAt: optimisticUpdatedAt,
			} : s
          ),
        }))
      }

      if (isCefContext()) {
        const requestKey = `setSessionModel:${id}`
        const requestId = createRequestId('setSessionModel')
        rememberPendingRequest(requestKey, requestId)
        pendingModelByChatId.set(id, {
          requestId,
          modelId: requestedModelId,
          reasoningEffort: requestedReasoningEffort,
          serviceTier: requestedServiceTier,
          serviceTierExplicit: requestedServiceTierExplicit,
        })
        applyModel()
        const response = await sendToCEF<{ modelId?: string; reasoningEffort?: string; serviceTier?: string; serviceTierExplicit?: boolean }>({
          action: 'setChatModel',
          payload: { chatId: id, modelId: requestedModelId },
          requestId,
        })

        if (response.ok) {
          if (isLatestPendingRequest(requestKey, response.requestId)) {
            const canonicalModelId = response.data?.modelId
            const canonicalReasoningEffort = response.data?.reasoningEffort
            const canonicalServiceTier = response.data?.serviceTier
			const canonicalServiceTierExplicit = response.data?.serviceTierExplicit
            if (typeof canonicalModelId === 'string' && typeof canonicalReasoningEffort === 'string' && typeof canonicalServiceTier === 'string') {
              set((state) => ({
                sessions: state.sessions.map((session) => session.id === id
                  ? { ...session, modelId: canonicalModelId, reasoningEffort: canonicalReasoningEffort, serviceTier: canonicalServiceTier, serviceTierExplicit: typeof canonicalServiceTierExplicit === 'boolean' ? canonicalServiceTierExplicit : requestedServiceTierExplicit }
                  : session),
              }))
              pendingModelByChatId.delete(id)
            }
            clearPendingRequest(requestKey, response.requestId)
          }
          return true
        }

        if (isLatestPendingRequest(requestKey, response.requestId)) {
          set((state) => ({
            sessions: state.sessions.map((s) => s.id === id ? {
              ...s,
              modelId: previousSession.modelId,
              reasoningEffort: previousSession.reasoningEffort,
              serviceTier: previousSession.serviceTier,
              serviceTierExplicit: previousSession.serviceTierExplicit,
              updatedAt: s.updatedAt === optimisticUpdatedAt ? previousSession.updatedAt : s.updatedAt,
            } : s),
          }))
          pendingRequestIdsByKey.delete(requestKey)
          if (pendingModelByChatId.get(id)?.requestId === response.requestId) pendingModelByChatId.delete(id)
        }

        return false
      }

      applyModel()
      return true
    },

    setSessionReviewerModel: async (id: string, modelId: string): Promise<boolean> => {
      const requestedModelId = modelId.trim()
      if (!isAllowedAcpModelId(requestedModelId) || !get().sessions.some((session) => session.id === id)) return false
      if (!isCefContext()) {
        set((state) => ({ sessions: state.sessions.map((session) => session.id === id ? { ...session, reviewerModelId: requestedModelId } : session) }))
        return true
      }

      const requestKey = `setSessionReviewerModel:${id}`
      const requestId = createRequestId('setSessionReviewerModel')
      rememberPendingRequest(requestKey, requestId)
      const response = await sendToCEF<{ reviewerModelId?: string }>({
        action: 'setChatModel',
        payload: { chatId: id, modelId: requestedModelId, modelRole: 'reviewer' },
        requestId,
      })
      if (!isLatestPendingRequest(requestKey, response.requestId)) return response.ok
      clearPendingRequest(requestKey, response.requestId)
      if (!response.ok || typeof response.data?.reviewerModelId !== 'string') return false
      set((state) => ({ sessions: state.sessions.map((session) => session.id === id ? { ...session, reviewerModelId: response.data!.reviewerModelId } : session) }))
      return true
    },

    setSessionCodexOptions: async (id: string, options: { reasoningEffort?: string; serviceTier?: string; serviceTierExplicit?: boolean }): Promise<boolean> => {
      const previousSession = get().sessions.find((s) => s.id === id)
	  if (!previousSession) {
        return false
      }
	  const acp = get().acpBindingBySessionId[id]
	  if (acp?.processing || acp?.lifecycleState === 'waitingPermission' || acp?.lifecycleState === 'waitingUserInput') return false
	  const providerId = previousSession.providerId ?? GEMINI_CLI_PROVIDER_ID
	  const codexProvider = providerId === CODEX_CLI_PROVIDER_ID
	  const copilotProvider = providerId === COPILOT_CLI_PROVIDER_ID
	  const runtimeModel = get().acpBindingBySessionId[id]?.availableModels.find((model) => model.id === (previousSession.modelId ?? ''))
	  const supportedEfforts = runtimeModel?.supportedReasoningEfforts ?? []
	  const supportedServiceTiers = runtimeModel?.additionalSpeedTiers ?? []
	  if (!codexProvider && !copilotProvider && supportedEfforts.length === 0) return false
	  const normalizedEffort = options.reasoningEffort === undefined ? previousSession.reasoningEffort ?? '' : normalizeCodexReasoningEffort(options.reasoningEffort)
	  const requestedReasoningEffort = options.reasoningEffort === undefined
	    ? normalizedEffort
	    : supportedEfforts.length > 0
	    ? supportedEfforts.includes(normalizedEffort) ? normalizedEffort : supportedEfforts[supportedEfforts.length - 1]
	    : normalizedEffort
	  const normalizedServiceTier = options.serviceTier === undefined ? previousSession.serviceTier ?? '' : codexProvider ? normalizeCodexServiceTier(options.serviceTier) : ''
	  const requestedServiceTier = runtimeModel && normalizedServiceTier && !supportedServiceTiers.includes(normalizedServiceTier) ? '' : normalizedServiceTier
	  const requestedServiceTierExplicit = codexProvider && (options.serviceTierExplicit ?? (options.serviceTier === undefined ? previousSession.serviceTierExplicit ?? normalizedServiceTier !== '' : true))
      if ((previousSession.reasoningEffort ?? '') === requestedReasoningEffort && (previousSession.serviceTier ?? '') === requestedServiceTier && (previousSession.serviceTierExplicit ?? (previousSession.serviceTier ?? '') !== '') === requestedServiceTierExplicit) {
        return true
      }

      const applyOptions = () => {
        set((state) => ({
          sessions: state.sessions.map((s) =>
            s.id === id ? { ...s, reasoningEffort: requestedReasoningEffort, serviceTier: requestedServiceTier, serviceTierExplicit: requestedServiceTierExplicit, updatedAt: new Date() } : s
          ),
        }))
      }

      if (isCefContext()) {
        const requestKey = `setSessionCodexOptions:${id}`
        const requestId = createRequestId('setSessionCodexOptions')
        rememberPendingRequest(requestKey, requestId)
        const response = await sendToCEF<{ reasoningEffort?: string; serviceTier?: string; serviceTierExplicit?: boolean }>({
          action: 'setChatCodexOptions',
          payload: { chatId: id, reasoningEffort: requestedReasoningEffort, serviceTier: requestedServiceTier, serviceTierExplicit: requestedServiceTierExplicit },
          requestId,
        })

        if (response.ok) {
          if (isLatestPendingRequest(requestKey, response.requestId)) {
			const canonicalReasoningEffort = typeof response.data?.reasoningEffort === 'string' ? response.data.reasoningEffort : requestedReasoningEffort
			const canonicalServiceTier = typeof response.data?.serviceTier === 'string' ? response.data.serviceTier : requestedServiceTier
			const canonicalServiceTierExplicit = typeof response.data?.serviceTierExplicit === 'boolean' ? response.data.serviceTierExplicit : requestedServiceTierExplicit
			set((state) => ({
			  sessions: state.sessions.map((session) => session.id === id
				? { ...session, reasoningEffort: canonicalReasoningEffort, serviceTier: canonicalServiceTier, serviceTierExplicit: canonicalServiceTierExplicit }
				: session),
			}))
            clearPendingRequest(requestKey, response.requestId)
          }
          return true
        }

        if (isLatestPendingRequest(requestKey, response.requestId)) pendingRequestIdsByKey.delete(requestKey)
        return false
      }

      applyOptions()
      return true
    },

    setSessionApprovalMode: async (id: string, modeId: string): Promise<boolean> => {
      const requestedModeId = modeId.trim() || 'default'
      if (!(AGENT_MODE_IDS as readonly string[]).includes(requestedModeId)) {
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

	  const applyMode = (confirmedModeId: string, confirmedRuntimeModeId?: string) => {
        set((state) => ({
          sessions: state.sessions.map((s) =>
			s.id === id ? { ...s, approvalMode: confirmedModeId, updatedAt: new Date() } : s
          ),
		  acpBindingBySessionId: state.acpBindingBySessionId[id] && confirmedRuntimeModeId
            ? {
                ...state.acpBindingBySessionId,
                [id]: {
                  ...state.acpBindingBySessionId[id],
				  currentModeId: confirmedRuntimeModeId,
                },
              }
            : state.acpBindingBySessionId,
        }))
      }

      if (isCefContext()) {
        const requestKey = `setSessionApprovalMode:${id}`
        const requestId = createRequestId('setSessionApprovalMode')
        rememberPendingRequest(requestKey, requestId)
		const response = await sendToCEF<{ approvalMode?: string; currentModeId?: string }>({
          action: 'setChatApprovalMode',
          payload: { chatId: id, modeId: requestedModeId },
          requestId,
        })

		if (response.ok) {
		  if (isLatestPendingRequest(requestKey, response.requestId)) {
			const confirmedModeId = normalizeAcpApprovalMode(response.data?.approvalMode ?? requestedModeId)
			const confirmedRuntimeModeId = typeof response.data?.currentModeId === 'string' ? response.data.currentModeId : undefined
			applyMode(confirmedModeId, confirmedRuntimeModeId)
		  }
		  clearPendingRequest(requestKey, response.requestId)
		  return true
		}

		if (isLatestPendingRequest(requestKey, response.requestId)) {
		  pendingRequestIdsByKey.delete(requestKey)
        }

        return false
      }

	  applyMode(requestedModeId, requestedModeId)
      return true
    },

    setSessionUamAgent: async (id: string, agentId: string): Promise<boolean> => {
	  const requestedAgentId = agentId.trim() || 'build'
	  const previousSession = get().sessions.find((session) => session.id === id)
	  if (!previousSession) return false
	  if ((previousSession.uamAgentId ?? 'build') === requestedAgentId) return true
	  const applyAgent = (confirmedAgentId: string) => set((state) => ({
		sessions: state.sessions.map((session) => session.id === id
		  ? { ...session, uamAgentId: confirmedAgentId, updatedAt: new Date() }
		  : session),
	  }))
	  if (!isCefContext()) {
		applyAgent(requestedAgentId)
		return true
	  }
	  const requestKey = `setSessionUamAgent:${id}`
	  const requestId = createRequestId('setSessionUamAgent')
	  rememberPendingRequest(requestKey, requestId)
	  const response = await sendToCEF<{ uamAgentId?: string }>({
		action: 'setChatUamAgent',
		payload: { chatId: id, agentId: requestedAgentId },
		requestId,
	  })
	  if (response.ok) {
		if (isLatestPendingRequest(requestKey, response.requestId)) {
		  applyAgent(response.data?.uamAgentId?.trim() || requestedAgentId)
		}
		clearPendingRequest(requestKey, response.requestId)
		return true
	  }
	  if (isLatestPendingRequest(requestKey, response.requestId)) pendingRequestIdsByKey.delete(requestKey)
	  return false
    },

    setSessionUamControlEnabled: async (id: string, enabled: boolean, applyOnNextLaunch = false): Promise<boolean> => {
      const previousSession = get().sessions.find((session) => session.id === id)
      if (!previousSession || (previousSession.uamControlEnabled ?? false) === enabled) return Boolean(previousSession)
      const apply = (confirmed: boolean) => set((state) => ({
        sessions: state.sessions.map((session) => session.id === id
          ? { ...session, uamControlEnabled: confirmed, updatedAt: new Date() }
          : session),
      }))
      if (!isCefContext()) {
        apply(enabled)
        return true
      }
      const requestKey = `setSessionUamControlEnabled:${id}`
      const requestId = createRequestId('setSessionUamControlEnabled')
      rememberPendingRequest(requestKey, requestId)
      const response = await sendToCEF<{ enabled?: boolean }>({
        action: 'setChatUamControlEnabled',
        payload: { chatId: id, enabled, ...(applyOnNextLaunch ? { applyOnNextLaunch: true } : {}) },
        requestId,
      })
      if (response.ok) {
        apply(typeof response.data?.enabled === 'boolean' ? response.data.enabled : enabled)
        clearPendingRequest(requestKey, response.requestId)
        return true
      }
      if (isLatestPendingRequest(requestKey, response.requestId)) pendingRequestIdsByKey.delete(requestKey)
      return false
    },

    refreshUamAgents: async (id: string): Promise<boolean> => {
      const builtIns: UamAgentSummary[] = [
        { id: 'build', description: 'Implement changes while obeying the current UAM permission policy.', builtIn: true },
        { id: 'plan', description: 'Inspect and plan under a hard read-only workspace ceiling.', builtIn: true },
      ]
      if (!isCefContext()) {
        set((state) => state.sessions.some((session) => session.id === id)
          ? { uamAgentsBySessionId: { ...state.uamAgentsBySessionId, [id]: builtIns } }
          : state)
        return true
      }
      const requestKey = `listUamAgents:${id}`
      const requestId = createRequestId('listUamAgents')
      rememberPendingRequest(requestKey, requestId)
      const response = await sendToCEF<{ agents?: unknown[] }>({
        action: 'listUamAgents',
        payload: { chatId: id },
        requestId,
      })
      if (!response.ok) {
        if (isLatestPendingRequest(requestKey, response.requestId)) pendingRequestIdsByKey.delete(requestKey)
        return false
      }
      const agents = Array.isArray(response.data?.agents)
        ? response.data.agents.flatMap((entry): UamAgentSummary[] => {
            if (!isRecord(entry)) return []
            const agentId = stringOr(entry.id).trim().toLowerCase()
            const mode = stringOr(entry.mode)
            if (!agentId || mode === 'subagent') return []
            return [{ id: agentId, description: stringOr(entry.description), builtIn: Boolean(entry.builtIn) }]
          })
        : builtIns
      if (isLatestPendingRequest(requestKey, response.requestId)) {
        set((state) => state.sessions.some((session) => session.id === id)
          ? { uamAgentsBySessionId: { ...state.uamAgentsBySessionId, [id]: agents } }
          : state)
      }
      clearPendingRequest(requestKey, response.requestId)
      return true
    },

    browseProviderAgentImport: async (currentValue = '') => {
      if (!isCefContext()) return null
      const response = await sendToCEF<{ selectedPath?: string }>({
        action: 'browseProviderAgentImport',
        payload: { currentValue },
      })
      const selected = response.ok ? response.data?.selectedPath?.trim() ?? '' : ''
      return selected || null
    },

    previewProviderAgentImport: async (providerId: string, sourcePath: string) => {
      if (!isCefContext()) return null
      const response = await sendToCEF<ProviderAgentImportPreview>({
        action: 'previewProviderAgentImport',
        payload: { providerId, sourcePath },
      })
      return response.ok ? response.data ?? null : null
    },

    importProviderAgent: async (options: { chatId: string; providerId: string; sourcePath: string; canonicalId: string; workspaceAccess: 'read' | 'write'; workspaceScope: boolean; acknowledgeIgnoredFields: boolean }) => {
      if (!isCefContext()) return false
      const response = await sendToCEF<{ id?: string }>({
        action: 'importProviderAgent',
        payload: options,
      })
      if (!response.ok) return false
      if (options.chatId) await get().refreshUamAgents(options.chatId)
      return true
    },

    setSessionCommandSafetyTier: async (id: string, tier: 'off' | 'acceptEdits' | 'aiReview' | 'yolo') => {
      const previousSession = get().sessions.find((session) => session.id === id)
	  if (!previousSession) return { ok: false, error: 'Chat not found.' }
	  const sameTier = (previousSession.commandSafetyTier ?? 'off') === tier
	  const applyTier = (confirmedTier: 'off' | 'acceptEdits' | 'aiReview' | 'yolo') => set((state) => ({
		sessions: state.sessions.map((session) => session.id === id ? { ...session, commandSafetyTier: confirmedTier, updatedAt: new Date() } : session),
	  }))
	  if (!isCefContext()) {
		if (!sameTier) applyTier(tier)
        return { ok: true }
      }

	  const requestKey = `setSessionCommandSafetyTier:${id}`
	  const requestId = createRequestId('setSessionCommandSafetyTier')
	  rememberPendingRequest(requestKey, requestId)
	  const response = await sendToCEF<{ commandSafetyTier?: string }>({
        action: 'setChatCommandSafetyTier',
        payload: { chatId: id, commandSafetyTier: tier },
        requestId,
      })
	  if (!isLatestPendingRequest(requestKey, response.requestId)) return { ok: false, cancelled: true }
	  clearPendingRequest(requestKey, response.requestId)
	  if (response.ok) {
		const confirmed = response.data?.commandSafetyTier
		applyTier(confirmed === 'off' || confirmed === 'acceptEdits' || confirmed === 'aiReview' || confirmed === 'yolo' ? confirmed : tier)
		return { ok: true }
	  }
      return { ok: false, error: response.error ?? 'Failed to change permission mode.' }
    },

    setSessionComputerUseBackend: async (id: string, backend: ComputerUseBackend): Promise<ComputerUseActionResult> => {
      const session = get().sessions.find((candidate) => candidate.id === id)
      if (!session) return computerUseFailure('Chat not found.')
      if (session.computerUseEnabled) return computerUseFailure('Turn off computer use before changing its control method.')
      if ((session.computerUseBackend ?? 'auto') === backend) return computerUseSuccess
      if (isCefContext()) {
        const response = await sendToCEF({ action: 'setChatComputerUseBackend', payload: { chatId: id, backend } })
        if (!response.ok) return computerUseFailure(response.error)
      }
      set((state) => ({
        sessions: state.sessions.map((candidate) => candidate.id === id ? {
          ...candidate,
          computerUseBackend: backend,
          computerUseTargetId: '',
          computerUseTargetTitle: '',
          computerUseTargetKind: 'window',
          computerUseTargetInputMode: 'foreground',
          updatedAt: new Date(),
        } : candidate),
      }))
      return computerUseSuccess
    },

	setSessionComputerUseEnabled: async (id: string, enabled: boolean): Promise<ComputerUseActionResult> => {
      const session = get().sessions.find((candidate) => candidate.id === id)
      if (!session) return computerUseFailure('Chat not found.')
      if (enabled && (session.computerUseEnabled ?? false)) return computerUseSuccess
      if (isCefContext()) {
        const response = await sendToCEF({ action: 'setChatComputerUseEnabled', payload: { chatId: id, enabled } })
        if (!response.ok) return computerUseFailure(response.error)
      }
      set((state) => ({
        sessions: state.sessions.map((candidate) => candidate.id === id ? {
          ...candidate,
          computerUseEnabled: enabled,
          computerUse: {
            enabled,
            state: enabled ? 'running' : 'stopped',
            history: candidate.computerUse?.history ?? [],
          },
          updatedAt: new Date(),
        } : candidate),
      }))
      return computerUseSuccess
    },

    setSessionComputerUseControl: async (id: string, controlState: ComputerUseControlState): Promise<ComputerUseActionResult> => {
      const session = get().sessions.find((candidate) => candidate.id === id)
      if (!session) return computerUseFailure('Chat not found.')
      if (!session.computerUseEnabled || (session.computerUseEffectiveBackend ?? 'uam') !== 'uam') return computerUseFailure()
      if (session.computerUse?.state === controlState) return computerUseSuccess
      if (isCefContext()) {
        const response = await sendToCEF({ action: 'setComputerUseControl', payload: { chatId: id, state: controlState } })
        if (!response.ok) return computerUseFailure(response.error)
      }
      set((state) => ({
        sessions: state.sessions.map((candidate) => candidate.id === id ? {
          ...candidate,
          computerUse: { enabled: true, state: controlState, history: candidate.computerUse?.history ?? [] },
        } : candidate),
      }))
      return computerUseSuccess
    },

    setSessionMemoryEnabled: async (id: string, enabled: boolean): Promise<boolean> => {
      return get().setSessionMemoryLevel(id, enabled ? 'strict' : 'off')
    },

    setSessionSmallModelMode: async (id: string, enabled: boolean): Promise<boolean> => {
      const previousSession = get().sessions.find((s) => s.id === id)
      if (!previousSession || (previousSession.smallModelMode ?? false) === enabled) {
        return Boolean(previousSession)
      }

      const applyMode = () => {
        set((state) => ({
          sessions: state.sessions.map((s) =>
            s.id === id ? { ...s, smallModelMode: enabled, updatedAt: new Date() } : s
          ),
        }))
      }

      if (isCefContext()) {
        const requestKey = `setSessionSmallModelMode:${id}`
        const requestId = createRequestId('setSessionSmallModelMode')
        rememberPendingRequest(requestKey, requestId)
        applyMode()
        const response = await sendToCEF({
          action: 'setChatSmallModelMode',
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

      applyMode()
      return true
    },

    setSessionMemoryLevel: async (id: string, level: MemoryLevel): Promise<boolean> => {
      const previousSession = get().sessions.find((s) => s.id === id)
      if (!previousSession) {
        return false
      }
      const requestedLevel = normalizeMemoryLevel(level)
      const enabled = requestedLevel !== 'off'
      if ((previousSession.memoryLevel ?? ((previousSession.memoryEnabled ?? true) ? 'strict' : 'off')) === requestedLevel) {
        return true
      }

      const applyMemory = () => {
        set((state) => ({
          sessions: state.sessions.map((s) =>
            s.id === id ? { ...s, memoryLevel: requestedLevel, memoryEnabled: enabled, updatedAt: new Date() } : s
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
          payload: { chatId: id, enabled, memoryLevel: requestedLevel },
          requestId,
        })

        if (response.ok) {
          clearPendingRequest(requestKey, response.requestId)
          return true
        }

        if (isLatestPendingRequest(requestKey, response.requestId)) {
          set((state) => ({
            sessions: state.sessions.map((s) => (s.id === id ? {
              ...s,
              memoryLevel: previousSession.memoryLevel,
              memoryEnabled: previousSession.memoryEnabled,
            } : s)),
          }))
          pendingRequestIdsByKey.delete(requestKey)
        }
        return false
      }

      applyMemory()
      return true
    },

    // Only actually supplied native fields update fallback display values; never commit authority.
    reconcileMemorySettingsProjection: (projection: Partial<AppState>, suppliedNativeSettings?: Parameters<AppState['reconcileMemorySettingsProjection']>[1]): Partial<AppState> => {
      memorySettingsWrites.observeProjection(suppliedNativeSettings)
      return memorySettingsWrites.overlay(projection)
    },
    setMemorySettings: memorySettingsWrites.save,

    setUpdateSettings: async (settings: Partial<Pick<AppState, 'automaticProviderUpdates' | 'updateChecksEnabled' | 'updateLastCheckedAt' | 'dismissedUpdateVersions'>>): Promise<boolean> => {
      const previous = {
        updateChecksEnabled: get().updateChecksEnabled,
        automaticProviderUpdates: get().automaticProviderUpdates,
        updateLastCheckedAt: get().updateLastCheckedAt,
        dismissedUpdateVersions: get().dismissedUpdateVersions,
      }
      const next = {
        updateChecksEnabled: settings.updateChecksEnabled ?? previous.updateChecksEnabled,
        automaticProviderUpdates: settings.automaticProviderUpdates ?? previous.automaticProviderUpdates,
        updateLastCheckedAt: settings.updateLastCheckedAt ?? previous.updateLastCheckedAt,
        dismissedUpdateVersions: settings.dismissedUpdateVersions ?? previous.dismissedUpdateVersions,
      }
      const optimisticRevision = rememberOptimisticFields(Object.keys(settings))
      set(next)
      if (!isCefContext()) return true

      const response = await sendToCEF({
        action: 'setUpdateSettings',
        payload: {
          enabled: next.updateChecksEnabled,
          automaticProviderUpdates: next.automaticProviderUpdates,
          lastCheckedAt: next.updateLastCheckedAt,
          dismissedVersions: next.dismissedUpdateVersions,
        },
        requestId: createRequestId('setUpdateSettings'),
      })
      if (!response.ok) set(latestOptimisticRollback(previous, optimisticRevision))
      return response.ok
    },

    setEditorSettings: async (settings: Pick<AppState, 'defaultEditorPresetId' | 'editorFileAssociations'> & Partial<Pick<AppState, 'fileExplorerApplication'>>): Promise<boolean> => {
      const previous = {
        ...(settings.fileExplorerApplication !== undefined ? { fileExplorerApplication: get().fileExplorerApplication } : {}),
        defaultEditorPresetId: get().defaultEditorPresetId,
        editorFileAssociations: get().editorFileAssociations,
      }
      const next = {
        ...(settings.fileExplorerApplication !== undefined ? { fileExplorerApplication: settings.fileExplorerApplication } : {}),
        defaultEditorPresetId: sanitizeEditorPresetId(settings.defaultEditorPresetId),
        editorFileAssociations: sanitizeEditorFileAssociations(settings.editorFileAssociations),
      }
      const optimisticRevision = rememberOptimisticFields(Object.keys(settings))
      const applySettings = () => set(next)

      if (isCefContext()) {
        const requestId = createRequestId('setEditorSettings')
        applySettings()
        const response = await sendToCEF({
          action: 'setEditorSettings',
          payload: {
            ...(next.fileExplorerApplication !== undefined ? { fileExplorerApplication: next.fileExplorerApplication } : {}),
            defaultEditorPresetId: next.defaultEditorPresetId,
            fileAssociations: next.editorFileAssociations,
          },
          requestId,
        })
        if (!response.ok) {
          set(latestOptimisticRollback(previous, optimisticRevision))
          return false
        }
        return true
      }

      applySettings()
      return true
    },

    setCentralProviderConfiguration: async (configuration: CentralProviderConfiguration): Promise<{ ok: boolean; error?: string }> => {
      const previous = get().centralProviderConfiguration
      const revision = rememberOptimisticFields(['centralProviderConfiguration'])
      set({ centralProviderConfiguration: configuration })
      if (!isCefContext()) return { ok: true }
      const response = await sendToCEF({ action: 'setCentralProviderConfiguration', payload: configuration, requestId: createRequestId('setCentralProviderConfiguration') })
      if (!response.ok) set(latestOptimisticRollback({ centralProviderConfiguration: previous }, revision))
      return { ok: response.ok, error: response.error }
    },

    setMcpServers: async (servers: McpServerConfiguration[], scopeExecutionHostId?: string): Promise<{ ok: boolean; error?: string }> => {
      const scope = scopeExecutionHostId?.trim()
      if (scope && scope !== '*') {
        const requestKey = `setMcpServers:${scope}`
        const requestId = createRequestId('setMcpServers')
        rememberPendingRequest(requestKey, requestId)
        const response = isCefContext() ? await sendToCEF<{ servers?: McpServerConfiguration[] }>({ action: 'setMcpServers', payload: { servers, scopeExecutionHostId: scope }, requestId }) : { ok: true, requestId, data: { servers } }
        if (response.ok && isLatestPendingRequest(requestKey, requestId)) {
          const current = get()
          const previous = current.hostScopedSettingsValues[scope] ?? { defaultNewChatProviderId: current.defaultNewChatProviderId, providerChatDefaults: current.providerChatDefaults, mcpServers: current.mcpServers }
          set({ hostScopedSettingsValues: { ...current.hostScopedSettingsValues, [scope]: { ...previous, mcpServers: response.data?.servers ?? servers } } })
        }
        clearPendingRequest(requestKey, requestId)
        return { ok: response.ok, error: 'error' in response ? response.error : undefined }
      }
      const previous = get().mcpServers
      const optimisticRevision = rememberOptimisticFields(['mcpServers'])
      set({ mcpServers: servers })
      if (!isCefContext()) return { ok: true }

      const response = await sendToCEF({
        action: 'setMcpServers',
        payload: withSettingsScope({ servers }, scopeExecutionHostId),
        requestId: createRequestId('setMcpServers'),
      })
      if (!response.ok) set(latestOptimisticRollback({ mcpServers: previous }, optimisticRevision))
      return { ok: response.ok, error: response.error }
    },

    setUamAgentPreferences: async (settings: { favoriteUamAgentIds: string[]; uamAgentCycleShortcut: UamAgentCycleShortcut }): Promise<boolean> => {
      const previous = {
        favoriteUamAgentIds: get().favoriteUamAgentIds,
        uamAgentCycleShortcut: get().uamAgentCycleShortcut,
      }
      const favoriteUamAgentIds = Array.from(new Set(settings.favoriteUamAgentIds.map((id) => id.trim().toLowerCase())))
        .filter((id) => id !== 'build' && id !== 'plan')
        .slice(0, 64)
      const next = { favoriteUamAgentIds, uamAgentCycleShortcut: settings.uamAgentCycleShortcut }
      const optimisticRevision = rememberOptimisticFields(Object.keys(next))
      set(next)
      if (!isCefContext()) return true

      const requestKey = 'setUamAgentPreferences'
      const requestId = createRequestId('setUamAgentPreferences')
      rememberPendingRequest(requestKey, requestId)
      const response = await sendToCEF<typeof next>({
        action: 'setUamAgentPreferences',
        payload: next,
        requestId,
      })
      if (!response.ok) {
        if (isLatestPendingRequest(requestKey, response.requestId)) {
          pendingRequestIdsByKey.delete(requestKey)
          set(latestOptimisticRollback(previous, optimisticRevision))
        }
        return false
      }
      if (isLatestPendingRequest(requestKey, response.requestId)) {
        set({
          favoriteUamAgentIds: response.data?.favoriteUamAgentIds ?? favoriteUamAgentIds,
          uamAgentCycleShortcut: response.data?.uamAgentCycleShortcut ?? settings.uamAgentCycleShortcut,
        })
      }
      clearPendingRequest(requestKey, response.requestId)
      return true
    },

    setProviderChatDefaults: async (settings: { defaultNewChatProviderId?: string; providerChatDefaults?: Record<string, ProviderChatDefaults> }, scopeExecutionHostId?: string): Promise<boolean> => {
      const scope = scopeExecutionHostId?.trim()
      if (scope && scope !== '*') {
        const current = get()
        const providerId = settings.defaultNewChatProviderId === undefined ? undefined : normalizeCliProviderIdAlias(settings.defaultNewChatProviderId)
        if (providerId !== undefined && !current.providers.some(provider => provider.id === providerId)) return false
        const defaults = settings.providerChatDefaults === undefined ? undefined : sanitizeProviderChatDefaultsMap(settings.providerChatDefaults)
        for (const [id, value] of Object.entries(defaults ?? {})) {
          if (!providerCapabilities(id).hasServiceTier) value.serviceTier = ''
        }
        const requestKey = `setProviderChatDefaults:${scope}`
        const requestId = createRequestId('setProviderChatDefaults')
        rememberPendingRequest(requestKey, requestId)
        const response = isCefContext() ? await sendToCEF({ action: 'setProviderChatDefaults', payload: { scopeExecutionHostId: scope, ...(providerId !== undefined ? { defaultProviderId: providerId } : {}), ...(defaults !== undefined ? { defaults } : {}) }, requestId }) : { ok: true, requestId }
        if (response.ok && isLatestPendingRequest(requestKey, requestId)) {
          const latest = get()
          const previous = latest.hostScopedSettingsValues[scope] ?? { defaultNewChatProviderId: latest.defaultNewChatProviderId, providerChatDefaults: latest.providerChatDefaults, mcpServers: latest.mcpServers }
          set({ hostScopedSettingsValues: { ...latest.hostScopedSettingsValues, [scope]: { ...previous, ...(providerId !== undefined ? { defaultNewChatProviderId: providerId } : {}), providerChatDefaults: { ...previous.providerChatDefaults, ...defaults } } } })
        }
        clearPendingRequest(requestKey, requestId)
        return response.ok
      }
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
      const current = get()
      for (const [providerId, defaults] of Object.entries(nextDefaults)) {
        const sanitized = sanitizeProviderChatDefaults(defaults)
        const caps = providerCapabilities(providerId)
        const providerSession = current.sessions.find((session) => session.providerId === providerId)
        const models = providerSession ? current.acpBindingBySessionId[providerSession.id]?.availableModels ?? [] : []
        const selectedModel = models.find((model) => model.id === sanitized.modelId) ?? (!sanitized.modelId ? models[0] : undefined)
        if (!caps.hasReasoningEffort && !(selectedModel?.supportedReasoningEfforts?.length)) {
          sanitized.reasoningEffort = ''
        }
        if (!caps.hasServiceTier) {
          sanitized.serviceTier = ''
        }
        nextDefaults[providerId] = sanitized
      }
      const next = {
        defaultNewChatProviderId: requestedDefaultProviderId,
        providerChatDefaults: nextDefaults,
      }
      const optimisticRevision = rememberOptimisticFields(Object.keys(settings))
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
          payload: withSettingsScope({
            defaultProviderId: next.defaultNewChatProviderId,
            defaults: next.providerChatDefaults,
          }, scopeExecutionHostId),
          requestId,
        })
        if (!response.ok) {
          clearPendingProviderChatDefaults(response.requestId)
          set(latestOptimisticRollback(previous, optimisticRevision))
          return false
        }
        clearPendingProviderChatDefaults(response.requestId)
        return true
      }

      applySettings()
      return true
    },

    refreshCliProviderVersion: async (providerId?: string, executionHostId?: string): Promise<boolean> => {
      const targetProviderId = normalizeCliProviderIdAlias(providerId ?? get().cliVersionManager.providers[0]?.providerId ?? GEMINI_CLI_PROVIDER_ID)
      if (!targetProviderId) return false

      if (isCefContext()) {
        const response = await sendToCEF({
          action: 'refreshCliProviderVersion',
          payload: { providerId: targetProviderId, ...(executionHostId?.trim() ? { executionHostId: executionHostId.trim() } : {}) },
          requestId: createRequestId('refreshCliProviderVersion'),
        })
        return response.ok
      }

      return false
    },

    applyCliProviderVersion: async (providerId: string, version: string, executionHostId?: string): Promise<boolean> => {
      const targetProviderId = normalizeCliProviderIdAlias(providerId)
      const targetVersion = version.trim()
      if (!targetProviderId || !targetVersion) return false

      if (isCefContext()) {
        const response = await sendToCEF({
          action: 'applyCliProviderVersion',
          payload: { providerId: targetProviderId, version: targetVersion, allowInteractiveElevation: true, ...(executionHostId?.trim() ? { executionHostId: executionHostId.trim() } : {}) },
          requestId: createRequestId('applyCliProviderVersion'),
        })
        return response.ok
      }

      return false
    },

    deleteSession: (id: string) => get().deleteSessions([id]),

    deleteSessions: async (ids: string[]) => {
      const existingIds = new Set(get().sessions.map((session) => session.id))
      const chatIds = [...new Set(ids.map((id) => id.trim()).filter((id) => id && existingIds.has(id)))]
      if (chatIds.length === 0) return false
	  const selectionRevisionAtStart = intentionalSelectionRevision

      let selectedChatId: string | null | undefined
      if (isCefContext()) {
        const response = await sendWhenRemoteStopSettles<{ selectedChatId?: string | null; deletedChatIds?: string[] }>({
          action: 'deleteSessions',
          payload: { chatIds },
        })
        if (!response.ok) {
          reportAppFailure('Could not confirm chat deletion.', response.error || 'Check the chat state before trying again.')
          return false
        }
		if (intentionalSelectionRevision === selectionRevisionAtStart) {
		  selectedChatId = response.data?.selectedChatId
		}
		const authoritativeIds = response.data?.deletedChatIds
		if (Array.isArray(authoritativeIds)) {
		  chatIds.splice(0, chatIds.length, ...authoritativeIds.filter((id) => typeof id === 'string' && id.trim()).map((id) => id.trim()))
		}
      }

      const deletedIds = new Set(chatIds)
      for (const id of deletedIds) {
        messageHydrationGenerationByChatId.set(id, (messageHydrationGenerationByChatId.get(id) ?? 0) + 1)
      }
      discardPendingPushesForChats(deletedIds)
      set((state) => deleteSessionsFromState(state, deletedIds, selectedChatId))
      removeChatsFromGrid(deletedIds)
      removeComposerDrafts(deletedIds)
      return true
    },

    setCliBinding: (sessionId: string, binding: Partial<CliBinding>) =>
      set((state: AppState) => {
        const existingBinding = state.cliBindingBySessionId[sessionId]
        const resolvedTerminalId = binding.terminalId ?? existingBinding?.terminalId ?? ''
        const startupTime = binding.startupTime ?? (resolvedTerminalId === existingBinding?.terminalId ? existingBinding?.startupTime : undefined)
        const running = binding.running ?? existingBinding?.running ?? false
        const turnState = binding.turnState ?? existingBinding?.turnState ?? 'idle'
        const processing = binding.processing ?? existingBinding?.processing ?? false
        const lifecycleState =
          binding.lifecycleState ??
          (binding.turnState === 'unknown' ? 'unknown' : existingBinding?.lifecycleState) ??
          normalizeCliLifecycleState(undefined, running, turnState, processing)
        let nextTranscripts = state.cliTranscriptBySessionId
        const existingTranscript = state.cliTranscriptBySessionId[sessionId]

        if (existingTranscript && resolvedTerminalId) {
          if ((existingTranscript.terminalId && existingTranscript.terminalId !== resolvedTerminalId) ||
              (startupTime !== undefined && existingTranscript.startupTime !== startupTime)) {
            nextTranscripts = {
              ...state.cliTranscriptBySessionId,
              [sessionId]: {
                terminalId: resolvedTerminalId,
                startupTime,
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
              startupTime,
              boundChatId: binding.boundChatId ?? existingBinding?.boundChatId ?? sessionId,
              running,
              lifecycleState,
              turnState: lifecycleState === 'unknown' ? 'unknown' : cliLifecycleIsProcessing(lifecycleState) ? 'busy' : turnState,
              processing: lifecycleState !== 'unknown' && (processing || cliLifecycleIsProcessing(lifecycleState)),
              readySinceLastSelect: binding.readySinceLastSelect ?? existingBinding?.readySinceLastSelect ?? false,
              active: lifecycleState !== 'unknown' && (binding.active ?? existingBinding?.active ?? false),
              pendingSteer: binding.pendingSteer ?? existingBinding?.pendingSteer ?? false,
              lastError: binding.lastError ?? existingBinding?.lastError ?? '',
            },
          },
          cliTranscriptBySessionId: nextTranscripts,
        }
      }),

    stageChatContextReference: (sessionId: string, reference: ContextReference): boolean => {
      if (!get().sessions.some((session) => session.id === sessionId)) return false
      const validated = sanitizeContextReferences([reference])[0]
      const source = get().sessions.find((session) => session.id === reference.sourceChatId)
      if (!validated || !source || (source.executionHostId || 'local') !== validated.executionHostId ||
        (source.workspaceWorktreeDirectory || source.workspaceDirectory || '') !== validated.workspaceDirectory) return false
      if (validated.kind === 'message') {
        const index = validated.sourceMessageIndex! - (get().historyStartIndexBySessionId[source.id] ?? 0)
        const message = get().messages[source.id]?.[index]
        if (!message || message.isStreaming || (validated.sourceMessageId && message.id !== validated.sourceMessageId) || !message.content.includes(validated.text)) return false
      }
      if (validated.kind === 'terminal' && get().cliBindingBySessionId[source.id]?.terminalId !== validated.terminalId) return false
      const draft = readChatComposerDraft(sessionId)
      const references = draft.references ?? []
      if (references.length >= CONTEXT_REFERENCE_MAX_COUNT || references.some((item) => item.id === validated.id)) return false
      writeChatComposerDraft(sessionId, { ...draft, references: [...references, validated] })
      return true
    },

    stageChatAttachments: async (sessionId: string, items: ChatAttachmentInput[]): Promise<Attachment[]> => {
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

    sendAcpPrompt: async (sessionId: string, text: string, attachments: Attachment[] = [], steerNow = false, onStarted?: AcpPromptPresentationObserver): Promise<boolean> => {
      const prompt = text.trim()
      if (!prompt && attachments.length === 0) {
        return false
      }
      const actionErrorId = createRequestId('sendAcpPrompt')
      set((state) => {
        const binding = state.acpBindingBySessionId[sessionId]
        return binding?.promptActionError ? { acpBindingBySessionId: {
          ...state.acpBindingBySessionId,
          [sessionId]: { ...binding, promptActionError: undefined },
        } } : state
      })
      const sentMarkdownEntries = new Set(get().markdownStoreAttachedBySessionId[sessionId] ?? [])
      const markdownStoreFiles = [...sentMarkdownEntries].map((entry) => entry.filePath)
      const state = get()
      const stateAtAdmission = state
      const goalId = state.activeGoalIdByChatId[sessionId] ?? null

      // Synchronous presentation only: publish the store-minted ID before IPC may synchronously push.
      // An observer failure must not turn a requested send into a cancelled or falsely failed input.
      const presentationOwner = stateAtAdmission.sessions.find((session) => session.id === sessionId)
      if (presentationOwner && onStarted) {
        try { onStarted({ requestId: actionErrorId, sessionId, createdAtMs: presentationOwner.createdAt.getTime() }) }
        catch { /* Presentation is optional and cannot cancel or change the native send outcome. */ }
      }

      if (isCefContext()) {
        const requestKey = `sendAcpPrompt:${sessionId}`
        rememberPendingRequest(requestKey, actionErrorId)
        const response = await sendToCEF({
          action: 'sendAcpPrompt',
          requestId: actionErrorId,
          payload: {
            chatId: sessionId,
            text: prompt,
            markdownStoreFiles,
            attachments,
            goalMode: goalId != null,
            goalId,
            computerUseMode: Boolean(state.sessions.find((session) => session.id === sessionId)?.computerUseEnabled),
            steerNow,
          },
        })
        const ownsAttempt = isLatestPendingRequest(requestKey, actionErrorId)
        clearPendingRequest(requestKey, actionErrorId)
		if (!get().sessions.some((session) => session.id === sessionId)) return false
        const recordPromptError = (message: string, inputRecorded = false) => {
          if (!ownsAttempt) return
          set((state) => ({
            acpBindingBySessionId: {
              ...state.acpBindingBySessionId,
              [sessionId]: {
                ...(state.acpBindingBySessionId[sessionId] ?? {
                  sessionId: '',
                  providerId: state.sessions.find((session) => session.id === sessionId)?.providerId ?? GEMINI_CLI_PROVIDER_ID,
                  protocolKind: inputRecorded ? '' : 'gemini-acp',
                  threadId: '',
                  running: false,
                  lifecycleState: (inputRecorded ? 'stopped' : 'error') as AcpLifecycleState,
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
                promptActionError: { id: actionErrorId, message: message },
              },
            },
          }))
        }
        const hasManagedReceipt = isRecord(response.data) && Object.prototype.hasOwnProperty.call(response.data, 'managedMention')
        const receipt = hasManagedReceipt && isRecord(response.data) && isRecord(response.data.managedMention)
          ? response.data.managedMention : null
        const sentAttachments = messageAttachments({ markdownStoreFiles, attachments })
        const parentMessage = receipt?.inputRecorded === true ? sanitizeCppMessage(receipt.parentMessage) : null
        const delegationState = receipt?.delegationState
        const validState = delegationState === 'committed' || delegationState === 'rejected' || delegationState === 'uncertain'
        const validIds = delegationState === 'committed'
          ? typeof receipt?.runId === 'string' && receipt.runId.length > 0 && typeof receipt.transcriptChatId === 'string' && receipt.transcriptChatId.length > 0
          : receipt?.runId === undefined && receipt?.transcriptChatId === undefined
        const validReceipt = receipt !== null && validState && validIds && typeof receipt.inputRecorded === 'boolean' && (
          receipt.inputRecorded
            ? parentMessage?.role === 'user' && parentMessage.content === prompt &&
              typeof receipt.parentMessageIndex === 'number' && Number.isSafeInteger(receipt.parentMessageIndex) && receipt.parentMessageIndex >= 0 &&
              Boolean(parentMessage.createdAt) && Number.isFinite(Date.parse(parentMessage.createdAt)) &&
              attachmentsEquivalent(messageAttachments(parentMessage), sentAttachments)
            : delegationState !== 'committed' && receipt.parentMessage === undefined && receipt.parentMessageIndex === undefined)
        if (!response.ok || (hasManagedReceipt && (!validReceipt || !receipt?.inputRecorded))) {
          recordPromptError(!response.ok ? response.error || 'Failed to send ACP prompt.'
            : !validReceipt ? 'The saved request could not be confirmed. Check the chat before retrying.'
              : typeof receipt?.error === 'string' && receipt.error ? receipt.error.slice(0, 4096) : 'The parent request was not confirmed as saved. Check the saved chat before retrying.')
          return false
        }
        if (hasManagedReceipt && receipt && parentMessage) {
          const admitted = stateAtAdmission.sessions.find((session) => session.id === sessionId)
          const current = get().sessions.find((session) => session.id === sessionId)
          if (!admitted || !current || admitted.createdAt.getTime() !== current.createdAt.getTime()) return false
          const canProject = admitted.providerId === current.providerId && admitted.executionHostId === current.executionHostId &&
            admitted.workspaceDirectory === current.workspaceDirectory && admitted.folderId === current.folderId &&
            admitted.workspaceWorktreeDirectory === current.workspaceWorktreeDirectory
          const incoming = buildMessageFromCpp(sessionId, parentMessage, receipt.parentMessageIndex as number)
          const recordedProjection: { message?: Message } = {}
          set((state) => {
            const existing = state.messages[sessionId] ?? []
            const localIndex = (receipt.parentMessageIndex as number) - (state.historyStartIndexBySessionId[sessionId] ?? 0)
            // A receipt owns one recorded row; it cannot replace a newer window or unrelated history.
            const append = canProject && localIndex === existing.length
            if (append) recordedProjection.message = incoming
            else if (canProject && localIndex >= 0 && localIndex < existing.length &&
                reconcileCppMessages(sessionId, [existing[localIndex]], [parentMessage], true, receipt.parentMessageIndex as number)[0] === existing[localIndex])
              recordedProjection.message = existing[localIndex]
            // The coordinate above comes from this validated action-local native receipt. No arbitrary row search.
            return {
              messages: append ? { ...state.messages, [sessionId]: [...existing, incoming] } : state.messages,
              sessions: !canProject ? state.sessions : state.sessions.map((session) => session.id === sessionId && (session.messageCount ?? 0) < (receipt.parentMessageIndex as number) + 1
                ? { ...session, messageCount: (receipt.parentMessageIndex as number) + 1 } : session),
              markdownStoreAttachedBySessionId: {
                ...state.markdownStoreAttachedBySessionId,
                [sessionId]: (state.markdownStoreAttachedBySessionId[sessionId] ?? []).filter((entry) => !sentMarkdownEntries.has(entry)),
              },
            }
          })
          const projectedRecordedInput = recordedProjection.message
          const afterProjection = get()
          const projectedOwner = afterProjection.sessions.find((session) => session.id === sessionId)
          if (projectedRecordedInput && sanitizeManagedOriginRunId(projectedRecordedInput.managedOriginRunId) && afterProjection.messages[sessionId]?.includes(projectedRecordedInput) && projectedOwner &&
              projectedOwner.createdAt.getTime() === admitted.createdAt.getTime() && projectedOwner.providerId === admitted.providerId &&
              projectedOwner.executionHostId === admitted.executionHostId && projectedOwner.workspaceDirectory === admitted.workspaceDirectory &&
              projectedOwner.folderId === admitted.folderId && projectedOwner.workspaceWorktreeDirectory === admitted.workspaceWorktreeDirectory) {
            try { onStarted?.onInputRecorded?.({ requestId: actionErrorId, sessionId, createdAtMs: admitted.createdAt.getTime(), message: projectedRecordedInput }) }
            catch { /* Optional observation cannot alter publication, saved input, draft ownership or child outcome. */ }
          }
          if (delegationState !== 'committed') recordPromptError(typeof receipt.error === 'string' && receipt.error
            ? receipt.error.slice(0, 4096) : 'The parent request was saved, but child creation was not confirmed.', true)
          return true
        }
        // A generic success acknowledges admission only. Native history/typed producer facts own real rows;
        // ChatView's exact submitted-input presentation retains immediate feedback without fabricating messages[].
        set((state) => ({
          markdownStoreAttachedBySessionId: {
            ...state.markdownStoreAttachedBySessionId,
            [sessionId]: (state.markdownStoreAttachedBySessionId[sessionId] ?? []).filter((entry) => !sentMarkdownEntries.has(entry)),
          },
        }))
        return true
      }

      set((state) => ({
        acpBindingBySessionId: {
          ...state.acpBindingBySessionId,
          [sessionId]: {
            ...(state.acpBindingBySessionId[sessionId] ?? {}),
            sessionId: state.acpBindingBySessionId[sessionId]?.sessionId ?? '',
            providerId: state.sessions.find((session) => session.id === sessionId)?.providerId ?? GEMINI_CLI_PROVIDER_ID,
            protocolKind: 'gemini-acp',
            threadId: state.acpBindingBySessionId[sessionId]?.threadId ?? '',
            running: false,
            lifecycleState: 'error',
            processing: false,
            readySinceLastSelect: false,
            processingStartedAtMs: null,
            promptActionError: { id: actionErrorId, message: 'Structured chat requires the desktop app.' },
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
            turnSerial: state.acpBindingBySessionId[sessionId]?.turnSerial ?? 0,
            waitIsStale: false,
            waitStaleReason: '',
            waitSeconds: 0,
            pendingPermission: null,
            pendingUserInput: null,
            agentInfo: state.acpBindingBySessionId[sessionId]?.agentInfo ?? null,
          },
        },
      }))
      return false
    },

    removeQueuedAcpPrompt: async (sessionId: string, index: number, expected?: { id: string; revision: number }): Promise<boolean> => {
      if (!Number.isSafeInteger(index) || index < 0 || (expected && (!expected.id || !Number.isSafeInteger(expected.revision) || expected.revision < 1))) return false
      if (isCefContext()) {
        const response = await sendToCEF({
          action: 'manageQueuedAcpPrompt',
          payload: { chatId: sessionId, operation: 'remove', index, ...(expected ? { expected } : {}) },
        })
        if (!response.ok && response.error) throw new Error(response.error)
        return response.ok
      }
      const observed = get().acpBindingBySessionId[sessionId]?.queuedPrompts?.[index]
      if (!observed || observed.prepared || (expected && (observed.id !== expected.id || observed.revision !== expected.revision))) return false
      set((state) => ({
        acpBindingBySessionId: {
          ...state.acpBindingBySessionId,
          [sessionId]: {
            ...state.acpBindingBySessionId[sessionId],
            queuedPrompts: (state.acpBindingBySessionId[sessionId]?.queuedPrompts ?? []).filter((_, queuedIndex) => queuedIndex !== index),
          },
        },
      }))
      return true
    },

    retryUnsentAcpPrompt: async (sessionId: string, expected: { id: string; revision: number }): Promise<boolean> => {
      if (!isCefContext() || isCompanionContext() || !expected.id || !Number.isSafeInteger(expected.revision) || expected.revision < 1) return false
      const state = get()
      const session = state.sessions.find(entry => entry.id === sessionId)
      const binding = state.acpBindingBySessionId[sessionId]
      const matches = binding?.unsentDirectPrompts?.filter(prompt => prompt.id === expected.id && prompt.revision === expected.revision) ?? []
      // Native retryAllowed alone authorizes a row-less receipt (a held, failed pre-launch baseline).
      if (!session || session.importedReadOnly || matches.length !== 1 || matches[0].prepared || (matches[0].retryAllowed !== true &&
          (!Number.isSafeInteger(matches[0].existingUserMessageIndex) || matches[0].existingUserMessageIndex! < 0))) return false
      // This asks native recovery to check the exact receipt. It neither duplicates history nor confirms delivery.
      const response = await sendToCEF({ action: 'manageQueuedAcpPrompt', payload: {
        chatId: sessionId, operation: 'retry', index: 0, expected: { id: expected.id, revision: expected.revision },
      } })
      if (!response.ok && response.error) throw new Error(response.error)
      return response.ok
    },

    steerQueuedAcpPrompt: async (sessionId: string, index: number, expected?: { id: string; revision: number }): Promise<boolean> => {
      if (get().acpBindingBySessionId[sessionId]?.queueHeld) return false
      if (!Number.isSafeInteger(index) || index < 0 || (expected && (!expected.id || !Number.isSafeInteger(expected.revision) || expected.revision < 1))) return false
      if (isCefContext()) {
        const response = await sendToCEF({
          action: 'manageQueuedAcpPrompt',
          payload: { chatId: sessionId, operation: 'steer', index, ...(expected ? { expected } : {}) },
        })
        return response.ok
      }
      const observed = get().acpBindingBySessionId[sessionId]?.queuedPrompts?.[index]
      if (!observed || observed.prepared || (expected && (observed.id !== expected.id || observed.revision !== expected.revision))) return false
      set((state) => {
        const queuedPrompts = [...(state.acpBindingBySessionId[sessionId]?.queuedPrompts ?? [])]
        const [prompt] = queuedPrompts.splice(index, 1)
        if (!prompt) return state
        return {
          acpBindingBySessionId: {
            ...state.acpBindingBySessionId,
            [sessionId]: {
              ...state.acpBindingBySessionId[sessionId],
              queuedPrompts: [prompt, ...queuedPrompts],
            },
          },
        }
      })
      return true
    },

    resumeQueuedAcpPrompts: async (sessionId: string, expected: { id: string; revision: number }[]): Promise<boolean> => {
      const binding = get().acpBindingBySessionId[sessionId]
      const queue = binding?.queuedPrompts ?? []
      if (!binding?.queueHeld || expected.length === 0 || expected.length > 32 || queue.length !== expected.length ||
          new Set(expected.map((entry) => entry.id)).size !== expected.length ||
          expected.some((entry, index) => !entry.id || !Number.isSafeInteger(entry.revision) || entry.revision < 1 ||
            queue[index].id !== entry.id || queue[index].revision !== entry.revision || queue[index].prepared)) return false
      if (isCefContext()) {
        const response = await sendToCEF({ action: 'resumeQueuedAcpPrompts', payload: { chatId: sessionId, expected } })
        return response.ok
      }
      set((state) => ({ acpBindingBySessionId: { ...state.acpBindingBySessionId, [sessionId]: { ...state.acpBindingBySessionId[sessionId], queueHeld: false } } }))
      return true
    },

    editQueuedAcpPrompt: async (sessionId: string, promptId: string, expectedRevision: number, text: string, retainedAttachmentIds?: string[]): Promise<boolean> => {
      if (!promptId || !Number.isSafeInteger(expectedRevision) || expectedRevision < 1 ||
          (retainedAttachmentIds && (retainedAttachmentIds.length > 128 || new Set(retainedAttachmentIds).size !== retainedAttachmentIds.length ||
            retainedAttachmentIds.some((id) => typeof id !== 'string' || !id || id.length > 128)))) return false
      if (isCefContext()) {
        const response = await sendToCEF({ action: 'manageQueuedAcpPrompt', payload: { chatId: sessionId, operation: 'edit', promptId, expectedRevision, text, ...(retainedAttachmentIds === undefined ? {} : { retainedAttachmentIds }) } })
        return response.ok
      }
      const queue = get().acpBindingBySessionId[sessionId]?.queuedPrompts ?? []
      const index = queue.findIndex((prompt) => prompt.id === promptId && prompt.revision === expectedRevision)
      if (index < 0) return false
      const original = queue[index]
      if (original.prepared || (retainedAttachmentIds && retainedAttachmentIds.some((id) => original.attachments.filter((attachment) => attachment.id === id).length !== 1))) return false
      const attachments = retainedAttachmentIds === undefined
        ? original.attachments
        : original.attachments.filter((attachment) => retainedAttachmentIds.includes(attachment.id))
      if (!text.trim() && attachments.length === 0) return false
      const next = queue.map((prompt, position) => position === index ? { ...prompt, text, attachments, revision: expectedRevision + 1 } : prompt)
      set((state) => ({ acpBindingBySessionId: { ...state.acpBindingBySessionId, [sessionId]: { ...state.acpBindingBySessionId[sessionId], queuedPrompts: next } } }))
      return true
    },

    reorderQueuedAcpPrompts: async (sessionId: string, expected: { id: string; revision: number }[], orderedIds: string[]): Promise<boolean> => {
      if (expected.length !== orderedIds.length || expected.length > 32 || new Set(orderedIds).size !== orderedIds.length ||
        expected.some((entry) => !entry.id || !Number.isSafeInteger(entry.revision) || entry.revision < 1) ||
        orderedIds.some((id) => !expected.some((entry) => entry.id === id))) return false
      if (isCefContext()) {
        const response = await sendToCEF({ action: 'manageQueuedAcpPrompt', payload: { chatId: sessionId, operation: 'reorder', expected, orderedIds } })
        return response.ok
      }
      const queue = get().acpBindingBySessionId[sessionId]?.queuedPrompts ?? []
      if (queue.length !== expected.length || queue.some((prompt, index) => prompt.id !== expected[index].id || prompt.revision !== expected[index].revision)) return false
      const next = orderedIds.map((id) => queue.find((prompt) => prompt.id === id)!)
      set((state) => ({ acpBindingBySessionId: { ...state.acpBindingBySessionId, [sessionId]: { ...state.acpBindingBySessionId[sessionId], queuedPrompts: next } } }))
      return true
    },

    discoverProviderModels: async (sessionId: string, providerId = '', workspaceDirectory = '', executionHostId = ''): Promise<boolean> => {
      if (!isCefContext()) return false
      const initial = get()
      const session = sessionId ? initial.sessions.find((entry) => entry.id === sessionId) : undefined
      const initialBinding = initial.acpBindingBySessionId[sessionId]
      // These witnesses fence selector UI only; the transport outcome never grants input authority.
      const owner = session ? {
        birth: session.createdAt.getTime(), providerId: session.providerId, host: session.executionHostId,
        workspace: session.workspaceDirectory, worktree: session.workspaceWorktreeDirectory,
        folderId: session.folderId,
        folderDirectory: initial.folders.find((entry) => entry.id === session.folderId)?.directory,
      } : undefined
      const native = {
        sessionId: initialBinding?.sessionId, threadId: initialBinding?.threadId,
        providerId: initialBinding?.providerId, protocol: initialBinding?.protocolKind,
      }
      const stillOwnsSelector = (state: AppState) => {
        const current = state.sessions.find((entry) => entry.id === sessionId)
        const binding = state.acpBindingBySessionId[sessionId]
        if (owner) {
          if (!Number.isFinite(owner.birth) || !current || current.createdAt.getTime() !== owner.birth ||
            current.providerId !== owner.providerId || current.executionHostId !== owner.host ||
            current.workspaceDirectory !== owner.workspace || current.workspaceWorktreeDirectory !== owner.worktree ||
            current.folderId !== owner.folderId ||
            state.folders.find((entry) => entry.id === current.folderId)?.directory !== owner.folderDirectory) return false
        } else if (current || !initialBinding) return false
        return binding?.sessionId === native.sessionId && binding?.threadId === native.threadId &&
          binding?.providerId === native.providerId && binding?.protocolKind === native.protocol
      }
      if (sessionId) set((state) => stillOwnsSelector(state) ? ({
        acpBindingBySessionId: {
          ...state.acpBindingBySessionId,
          [sessionId]: { ...state.acpBindingBySessionId[sessionId], modelRefreshError: '' },
        },
      }) : state)
      const preparedBinding = sessionId ? get().acpBindingBySessionId[sessionId] : undefined
      const mayProjectReply = (state: AppState) => Boolean(preparedBinding && stillOwnsSelector(state) &&
        // A binding-only or identity-less selector must remain the exact object prepared by this call.
        ((owner && (native.sessionId || native.threadId)) || state.acpBindingBySessionId[sessionId] === preparedBinding))
      const response = await sendToCEF<{ started?: boolean; pending?: boolean }>({
        action: 'discoverProviderModels',
        payload: { chatId: sessionId, ...(providerId ? { providerId } : {}), ...(workspaceDirectory ? { workspaceDirectory } : {}), ...(executionHostId ? { executionHostId } : {}) },
      })
      if (!response.ok) {
        if (sessionId) set((state) => mayProjectReply(state) ? ({
          acpBindingBySessionId: {
            ...state.acpBindingBySessionId,
            [sessionId]: { ...state.acpBindingBySessionId[sessionId], modelsLoading: false, modelRefreshError: response.error ?? 'Model discovery failed.' },
          },
        }) : state)
        return false
      }
      if (sessionId && response.data?.pending) set((state) => mayProjectReply(state) ? ({
        acpBindingBySessionId: {
          ...state.acpBindingBySessionId,
          [sessionId]: { ...state.acpBindingBySessionId[sessionId], modelsLoading: true },
        },
      }) : state)
      return Boolean(response.data?.started || response.data?.pending)
    },

    setAcpConfigOption: async (sessionId: string, configId: string, value: string): Promise<boolean> => {
      const option = get().acpBindingBySessionId[sessionId]?.configOptions?.find((candidate) => candidate.id === configId)
      if (!option || !option.options.some((choice) => choice.value === value)) return false
      if (!isCefContext()) return false
      const response = await sendToCEF({
        action: 'setAcpConfigOption',
        payload: { chatId: sessionId, configId, value },
      })
      return response.ok
    },

    cancelAcpTurn: async (sessionId: string): Promise<boolean> => {
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

    resolveAcpPermission: async (sessionId: string, requestId: string, optionId: string | 'cancelled'): Promise<boolean> => {
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

    resolveAcpUserInput: async (sessionId: string, requestId: string, answers: AcpUserInputAnswers): Promise<boolean> => {
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

    stopAcpSession: async (sessionId: string, purpose: 'interrupt' | 'timeout' = 'interrupt'): Promise<boolean> => {
      if (isCefContext()) {
        const response = await sendToCEF({
          action: 'stopAcpSession',
          payload: purpose === 'timeout' ? { chatId: sessionId, purpose } : { chatId: sessionId },
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
  }
}
