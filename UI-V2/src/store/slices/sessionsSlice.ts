import type { ComputerUseActionResult, ComputerUseBackend, ComputerUseControlState, Session, ViewMode } from '../../types/session'
import type { Attachment, Message } from '../../types/message'
import type { Provider } from '../../types/provider'
import type { MemoryLevel } from '../../types/memory'
import type { McpServerConfiguration } from '../cpp/types'
import { sendWhenRemoteStopSettles, sendToCEF, isCefContext, createRequestId } from '../../ipc/cefBridge'
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
import {
  AGENT_MODE_IDS,
  cefPayloadOrRawResponse,
  clampedFiniteNumberOr,
  DEFAULT_GOAL_MAX_LOOP_ITERATIONS,
  DEFAULT_ACP_SETUP_INACTIVITY_TIMEOUT_SECONDS,
  DEFAULT_ACP_TURN_OUTPUT_LIMIT_MIB,
  DEFAULT_MEMORY_IDLE_DELAY_SECONDS,
  DEFAULT_MEMORY_RECALL_BUDGET_BYTES,
  defaultEditorFileAssociations,
  emptyCliVersionManager,
  emptyMemoryActivity,
  finiteNumberOr,
  failedGitWorktreeResult,
  isAllowedAcpModelId,
  isRecord,
  MAX_MEMORY_IDLE_DELAY_SECONDS,
  MAX_MEMORY_RECALL_BUDGET_BYTES,
  MIN_MEMORY_IDLE_DELAY_SECONDS,
  MIN_MEMORY_RECALL_BUDGET_BYTES,
  normalizeAcpApprovalMode,
  normalizeCodexReasoningEffort,
  normalizeCodexServiceTier,
  normalizeMemoryLevel,
  normalizeGoalMaxLoopIterations,
  normalizeAcpSetupInactivityTimeoutSeconds,
  normalizeAcpTurnOutputLimitMiB,
  providerChatDefaultsForNewChat,
  sanitizeAttachment,
  sanitizeEditorFileAssociations,
  sanitizeEditorPresetId,
  sanitizeGitWorktreeResult,
  sanitizeGitWorktreeStatus,
  sanitizeProviderChatDefaults,
  sanitizeProviderChatDefaultsMap,
  sanitizeVcsCommitResult,
  sanitizeVcsCommitStatus,
  stringOr,
} from '../cpp/sanitizers'
import {
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

import type { AppState, ZustandSet, ZustandGet } from '../storeTypes'
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

export function withoutDeletedKeys<T>(values: Record<string, T>, deletedIds: Set<string>): Record<string, T> {
  return Object.fromEntries(Object.entries(values).filter(([id]) => !deletedIds.has(id)))
}

export function deleteSessionsFromState(state: AppState, deletedIds: Set<string>, selectedChatId?: string | null): Partial<AppState> {
  const sessions = state.sessions.filter((session) => !deletedIds.has(session.id))
  const requestedSelection = selectedChatId !== undefined ? selectedChatId : state.activeSessionId
  return {
    sessions,
    messages: withoutDeletedKeys(state.messages, deletedIds),
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
  let intentionalSelectionRevision = 0
  const loadedMessagesDigestByChatId = new Map<string, string>()
  const messageHydrationGenerationByChatId = new Map<string, number>()

  const requestChatMessagesFromCef = (chatId: string, force = false) => {
    if (!isCefContext() || !chatId) return
    const current = get()
    const messagesAtRequestStart = current.messages[chatId]
    const requestKey = `getChatMessages:${chatId}`
    const requestId = createRequestId('getChatMessages')
    const hydrationGeneration = messageHydrationGenerationByChatId.get(chatId) ?? 0
    rememberPendingRequest(requestKey, requestId)
    void sendToCEF<ChatMessagesResponse>({
      action: 'getChatMessages',
      payload: {
        chatId,
        messagesDigest: force || current.messages[chatId] === undefined
          ? ''
          : loadedMessagesDigestByChatId.get(chatId) ?? '',
      },
      requestId,
    }).then((response) => {
      if (!isLatestPendingRequest(requestKey, response.requestId)) return
      clearPendingRequest(requestKey, response.requestId)
      if ((messageHydrationGenerationByChatId.get(chatId) ?? 0) !== hydrationGeneration ||
          !get().sessions.some((session) => session.id === chatId)) return
      if (!response.ok || !response.data) {
        const chatName = current.sessions.find((session) => session.id === chatId)?.name?.trim() || chatId
        set({ statusLine: `Failed to load chat history for ${chatName}: ${response.error ?? 'The chat history response was empty.'}` })
        return
      }

      const data = response.data
      if (data.chatId && data.chatId !== chatId) return
      if (data.unchanged) return
      if (!Array.isArray(data.messages)) return

      set((state) => {
        if (force && state.messages[chatId] !== messagesAtRequestStart) return state
        const nextMessages = reconcileCppMessages(chatId, state.messages[chatId], data.messages ?? [], force)
        if (nextMessages.length === data.messages!.length && !nextMessages.some((message) => message.isStreaming)) {
          loadedMessagesDigestByChatId.set(chatId, data.messagesDigest ?? '')
        }
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

  return {
    folders: inCef ? [] : initialFolders,
    sessions: inCef ? [] : initialSessions,
    activeSessionId: (inCef ? null : 's1') as string | null,
    lastAppliedStateRevision: -1,
    messages: {} as Record<string, Message[]>,
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
    appVersion: 'V4.9.0-alpha-11',
    runnerProtocolVersion: 0,
    updateChecksEnabled: true,
    updateLastCheckedAt: '',
    dismissedUpdateVersions: {} as Record<string, string>,
    memoryLastStatus: '',
    memoryWorkerBindings: {} as Record<string, MemoryWorkerBinding>,
    permissionReviewerProviderId: '',
    permissionReviewerModelId: '',
    memoryActivity: { ...emptyMemoryActivity } as MemoryActivity,
    cliVersionManager: { ...emptyCliVersionManager } as CliVersionManager,
    defaultNewChatProviderId: GEMINI_CLI_PROVIDER_ID,
    providerChatDefaults: {} as Record<string, ProviderChatDefaults>,
    defaultEditorPresetId: 'vscode',
    editorFileAssociations: defaultEditorFileAssociations() as EditorFileAssociation[],
    mcpServers: [] as McpServerConfiguration[],
    executionHosts: [{
      id: 'local', label: 'This computer', transport: 'local', sshAlias: '',
      runnerStatus: 'ready', runnerVersion: '', platform: '', architecture: '', lastSeenAt: '',
    }] as AppState['executionHosts'],
    favoriteUamAgentIds: [] as string[],
    uamAgentCycleShortcut: 'shift+tab' as UamAgentCycleShortcut,
    uamAgentsBySessionId: {} as Record<string, UamAgentSummary[]>,
    statusLine: '',

    setActiveSession: (id: string | null) => {
      if (get().activeSessionId === id) return
      intentionalSelectionRevision += 1
      if (isCefContext()) {
        const previousActiveSessionId = get().activeSessionId
        const requestKey = 'selectSession'
        const requestId = createRequestId('selectSession')
        rememberPendingRequest(requestKey, requestId)
        const openedAt = new Date()
        const previousSession = id ? get().sessions.find((s) => s.id === id) : undefined
        set((state) => ({
          activeSessionId: id,
          sessions: state.sessions.map((s) =>
            id && s.id === id ? { ...s, lastOpenedAt: openedAt } : s
          ),
        }))
        sendToCEF({ action: 'selectSession', payload: { chatId: id ?? '' }, requestId }).then((resp) => {
          if (resp.ok) {
			if (!isLatestPendingRequest(requestKey, resp.requestId)) return
            clearPendingRequest(requestKey, resp.requestId)
            if (id) requestChatMessagesFromCef(id)
            return
          }

          if (!isLatestPendingRequest(requestKey, resp.requestId)) {
            return
          }

          set((state) => ({
            activeSessionId: previousActiveSessionId,
            sessions: previousSession
              ? state.sessions.map((s) =>
                  id && s.id === id ? { ...s, lastOpenedAt: previousSession.lastOpenedAt } : s
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
            id && s.id === id ? { ...s, lastOpenedAt: openedAt } : s
          ),
        }
      })
    },

    loadSessionMessages: requestChatMessagesFromCef,

    unloadSessionMessages: (id: string) => {
      const current = get()
      if (current.acpBindingBySessionId[id]?.processing || current.cliBindingBySessionId[id]?.processing) return
      messageHydrationGenerationByChatId.set(id, (messageHydrationGenerationByChatId.get(id) ?? 0) + 1)
      discardPendingTranscriptPushesForChat(id)
      if (current.messages[id] === undefined && current.cliTranscriptBySessionId[id] === undefined) return
      loadedMessagesDigestByChatId.delete(id)
      const messages = { ...current.messages }
      const cliTranscriptBySessionId = { ...current.cliTranscriptBySessionId }
      delete messages[id]
      delete cliTranscriptBySessionId[id]
      set({ messages, cliTranscriptBySessionId })
    },

    addSession: async (name: string, folderId: string | null, providerId = GEMINI_CLI_PROVIDER_ID, modelId?: string, reasoningEffort?: string, viewMode: ViewMode = 'chat', executionHostId = 'local', workspaceDirectory = '') => {
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
      const normalizedProviderId = normalizeCliProviderIdAlias(providerId)
      const normalizedDefaultProviderId = normalizeCliProviderIdAlias(current.defaultNewChatProviderId)
      const requestedProviderId = current.providers.some((provider) => provider.id === normalizedProviderId)
        ? normalizedProviderId
        : current.providers.some((provider) => provider.id === normalizedDefaultProviderId)
          ? normalizedDefaultProviderId
          : GEMINI_CLI_PROVIDER_ID
      const defaults = providerChatDefaultsForNewChat(current, requestedProviderId)
      if (modelId !== undefined) defaults.modelId = modelId.trim()
	  if (reasoningEffort !== undefined) defaults.reasoningEffort = normalizeCodexReasoningEffort(reasoningEffort)

      if (isCefContext()) {
        const resp = await sendToCEF<{ chatId?: string }>({
          action: 'createSession',
          payload: {
            title: name,
            folderId: selectedFolderId ?? '',
            providerId: requestedProviderId,
            defaults,
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
        memoryLevel: defaults.memoryLevel,
        memoryEnabled: defaults.memoryEnabled,
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

    branchFromMessage: async (id: string, messageIndex: number, content?: string): Promise<string | null> => {
      if (isCefContext()) {
        const response = await sendToCEF<{ chatId?: string }>({
          action: 'branchFromMessage',
          payload: {
            chatId: id,
            messageIndex,
            ...(content === undefined ? {} : { content }),
          },
        })
        if (!response.ok) {
          console.error('[CEF] branchFromMessage failed:', response.error)
          return null
        }
        return response.data?.chatId?.trim() || null
      }

      const source = get().sessions.find((session) => session.id === id)
      const sourceMessages = get().messages[id] ?? []
      const sourceMessage = sourceMessages[messageIndex]
      if (!source || sourceMessage?.role !== 'user' || (content !== undefined && !content.trim())) return null

      sessionCounter++
      const branchId = makeId('branch', sessionCounter)
      const now = new Date()
      const branchMessages = sourceMessages.slice(0, messageIndex + 1).map((message, index) => ({
        ...message,
        sessionId: branchId,
        ...(index === messageIndex && content !== undefined ? { content } : {}),
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
        const response = await sendToCEF<OpenNativeSessionChatResponse>({
          action: 'openNativeSessionChat',
          payload: {
            chatId: sourceChatId,
            nativeSessionId,
            title,
            selectChat,
          },
        })

        if (!response.ok) {
          console.error('[CEF] openNativeSessionChat failed:', response.error)
          return null
        }

        const chatId = response.data?.chatId?.trim() ?? ''
        if (chatId) requestChatMessagesFromCef(chatId)
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

    getVcsCommitStatus: async (id: string, vcsType: VcsType = 'git', options: { includeLineStats?: boolean; requestId?: string; comparisonRef?: string } = {}): Promise<VcsCommitStatus | null> => {
      if (isCefContext()) {
        const response = await sendToCEF<VcsCommitStatus>({
          action: 'getVcsCommitStatus',
          payload: {
            chatId: id,
            vcsType,
            includeLineStats: options.includeLineStats ?? true,
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
                name: previousSession.name,
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
            sessions: state.sessions.map((s) =>
              s.id === id ? { ...s, isPinned: previousSession.isPinned } : s
            ),
          }))
          pendingRequestIdsByKey.delete(requestKey)
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
              memoryLevel: defaults.memoryLevel,
              memoryEnabled: defaults.memoryEnabled,
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

    setSessionUamControlEnabled: async (id: string, enabled: boolean): Promise<boolean> => {
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
        payload: { chatId: id, enabled },
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
      await get().refreshUamAgents(options.chatId)
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
      if ((session.computerUseEnabled ?? false) === enabled) return computerUseSuccess
	  if (enabled && (session.computerUseEffectiveBackend ?? 'uam') === 'uam') {
		return computerUseFailure('Ask the AI to use Computer Use. UAM will ask you once to approve its chosen target.')
      }
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

    setMemorySettings: async (settings: Partial<Pick<AppState, 'memoryEnabledDefault' | 'memoryLevelDefault' | 'memoryIdleDelaySeconds' | 'memoryRecallBudgetBytes' | 'goalMaxLoopIterations' | 'acpSetupInactivityTimeoutSeconds' | 'acpTurnOutputLimitMiB' | 'memoryWorkerBindings' | 'permissionReviewerProviderId' | 'permissionReviewerModelId'>>): Promise<boolean> => {
      const previous = {
        memoryEnabledDefault: get().memoryEnabledDefault,
        memoryLevelDefault: get().memoryLevelDefault,
        memoryIdleDelaySeconds: get().memoryIdleDelaySeconds,
        memoryRecallBudgetBytes: get().memoryRecallBudgetBytes,
        goalMaxLoopIterations: get().goalMaxLoopIterations,
        acpSetupInactivityTimeoutSeconds: get().acpSetupInactivityTimeoutSeconds,
        acpTurnOutputLimitMiB: get().acpTurnOutputLimitMiB,
        memoryWorkerBindings: get().memoryWorkerBindings,
        permissionReviewerProviderId: get().permissionReviewerProviderId,
        permissionReviewerModelId: get().permissionReviewerModelId,
      }
      const requestedDefaultLevel = settings.memoryLevelDefault
        ?? (settings.memoryEnabledDefault === undefined ? previous.memoryLevelDefault : settings.memoryEnabledDefault ? 'strict' : 'off')
      const next = {
        memoryLevelDefault: normalizeMemoryLevel(requestedDefaultLevel),
        memoryEnabledDefault: normalizeMemoryLevel(requestedDefaultLevel) !== 'off',
        memoryIdleDelaySeconds: clampedFiniteNumberOr(settings.memoryIdleDelaySeconds, previous.memoryIdleDelaySeconds, MIN_MEMORY_IDLE_DELAY_SECONDS, MAX_MEMORY_IDLE_DELAY_SECONDS),
        memoryRecallBudgetBytes: clampedFiniteNumberOr(settings.memoryRecallBudgetBytes, previous.memoryRecallBudgetBytes, MIN_MEMORY_RECALL_BUDGET_BYTES, MAX_MEMORY_RECALL_BUDGET_BYTES),
        goalMaxLoopIterations: settings.goalMaxLoopIterations === undefined
          ? previous.goalMaxLoopIterations
          : normalizeGoalMaxLoopIterations(settings.goalMaxLoopIterations),
        acpSetupInactivityTimeoutSeconds: settings.acpSetupInactivityTimeoutSeconds === undefined
          ? previous.acpSetupInactivityTimeoutSeconds
          : normalizeAcpSetupInactivityTimeoutSeconds(settings.acpSetupInactivityTimeoutSeconds),
        acpTurnOutputLimitMiB: settings.acpTurnOutputLimitMiB === undefined
          ? previous.acpTurnOutputLimitMiB
          : normalizeAcpTurnOutputLimitMiB(settings.acpTurnOutputLimitMiB),
        memoryWorkerBindings: settings.memoryWorkerBindings ?? previous.memoryWorkerBindings,
        permissionReviewerProviderId: settings.permissionReviewerProviderId ?? previous.permissionReviewerProviderId,
        permissionReviewerModelId: settings.permissionReviewerModelId ?? previous.permissionReviewerModelId,
      }
      const ownedFields = Object.keys(settings)
      if (settings.memoryEnabledDefault !== undefined || settings.memoryLevelDefault !== undefined) {
        ownedFields.push('memoryEnabledDefault', 'memoryLevelDefault')
      }
      const optimisticRevision = rememberOptimisticFields(ownedFields)
      const applySettings = () => set(next)

      if (isCefContext()) {
        const requestId = createRequestId('setMemorySettings')
        applySettings()
        const response = await sendToCEF({
          action: 'setMemorySettings',
          payload: {
            enabledDefault: next.memoryEnabledDefault,
            levelDefault: next.memoryLevelDefault,
            idleDelaySeconds: next.memoryIdleDelaySeconds,
            recallBudgetBytes: next.memoryRecallBudgetBytes,
            goalMaxLoopIterations: next.goalMaxLoopIterations,
            acpSetupInactivityTimeoutSeconds: next.acpSetupInactivityTimeoutSeconds,
            acpTurnOutputLimitMiB: next.acpTurnOutputLimitMiB,
            workerBindings: next.memoryWorkerBindings,
            permissionReviewerProviderId: next.permissionReviewerProviderId,
            permissionReviewerModelId: next.permissionReviewerModelId,
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

    setUpdateSettings: async (settings: Partial<Pick<AppState, 'updateChecksEnabled' | 'updateLastCheckedAt' | 'dismissedUpdateVersions'>>): Promise<boolean> => {
      const previous = {
        updateChecksEnabled: get().updateChecksEnabled,
        updateLastCheckedAt: get().updateLastCheckedAt,
        dismissedUpdateVersions: get().dismissedUpdateVersions,
      }
      const next = {
        updateChecksEnabled: settings.updateChecksEnabled ?? previous.updateChecksEnabled,
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
          lastCheckedAt: next.updateLastCheckedAt,
          dismissedVersions: next.dismissedUpdateVersions,
        },
        requestId: createRequestId('setUpdateSettings'),
      })
      if (!response.ok) set(latestOptimisticRollback(previous, optimisticRevision))
      return response.ok
    },

    setEditorSettings: async (settings: Pick<AppState, 'defaultEditorPresetId' | 'editorFileAssociations'>): Promise<boolean> => {
      const previous = {
        defaultEditorPresetId: get().defaultEditorPresetId,
        editorFileAssociations: get().editorFileAssociations,
      }
      const next = {
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

    setMcpServers: async (servers: McpServerConfiguration[]): Promise<{ ok: boolean; error?: string }> => {
      const previous = get().mcpServers
      const optimisticRevision = rememberOptimisticFields(['mcpServers'])
      set({ mcpServers: servers })
      if (!isCefContext()) return { ok: true }

      const response = await sendToCEF({
        action: 'setMcpServers',
        payload: { servers },
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

    setProviderChatDefaults: async (settings: { defaultNewChatProviderId?: string; providerChatDefaults?: Record<string, ProviderChatDefaults> }): Promise<boolean> => {
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
          payload: {
            defaultProviderId: next.defaultNewChatProviderId,
            defaults: next.providerChatDefaults,
          },
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

    refreshCliProviderVersion: async (providerId?: string): Promise<boolean> => {
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

      return false
    },

    applyCliProviderVersion: async (providerId: string, version: string): Promise<boolean> => {
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
        if (!response.ok) return false
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
              pendingSteer: binding.pendingSteer ?? existingBinding?.pendingSteer ?? false,
              lastError: binding.lastError ?? existingBinding?.lastError ?? '',
            },
          },
          cliTranscriptBySessionId: nextTranscripts,
        }
      }),

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

    sendAcpPrompt: async (sessionId: string, text: string, attachments: Attachment[] = [], steerNow = false): Promise<boolean> => {
      const prompt = text.trim()
      if (!prompt) {
        return false
      }
      const markdownStoreFiles = (get().markdownStoreAttachedBySessionId[sessionId] ?? []).map((entry) => entry.filePath)
      const state = get()
      const goalId = state.activeGoalIdByChatId[sessionId] ?? null
      const appendUserMessage = !state.acpBindingBySessionId[sessionId]?.processing

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
            computerUseMode: Boolean(state.sessions.find((session) => session.id === sessionId)?.computerUseEnabled),
            steerNow,
          },
        })
		if (!get().sessions.some((session) => session.id === sessionId)) return false
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
        set((state) => {
          const existing = state.messages[sessionId] ?? []
          const alreadyHydrated = existing[existing.length - 1]?.role === 'user' && existing[existing.length - 1]?.content === prompt
          const messages = appendUserMessage && !alreadyHydrated
            ? {
                ...state.messages,
                [sessionId]: [...existing, {
                  id: `sent-user-${Date.now()}`,
                  sessionId,
                  role: 'user' as const,
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
                  createdAt: new Date(),
                }],
              }
            : state.messages
          return {
            messages,
            markdownStoreAttachedBySessionId: {
              ...state.markdownStoreAttachedBySessionId,
              [sessionId]: [],
            },
          }
        })
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
            lastError: 'Structured chat requires the desktop app.',
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

    removeQueuedAcpPrompt: async (sessionId: string, index: number): Promise<boolean> => {
      if (isCefContext()) {
        const response = await sendToCEF({
          action: 'manageQueuedAcpPrompt',
          payload: { chatId: sessionId, operation: 'remove', index },
        })
        return response.ok
      }
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

    steerQueuedAcpPrompt: async (sessionId: string, index: number): Promise<boolean> => {
      if (isCefContext()) {
        const response = await sendToCEF({
          action: 'manageQueuedAcpPrompt',
          payload: { chatId: sessionId, operation: 'steer', index },
        })
        return response.ok
      }
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

    discoverProviderModels: async (sessionId: string, providerId = '', workspaceDirectory = '', executionHostId = ''): Promise<boolean> => {
	  if (!isCefContext()) return false
	  if (sessionId) set((state) => ({
		acpBindingBySessionId: {
          ...state.acpBindingBySessionId,
          [sessionId]: { ...state.acpBindingBySessionId[sessionId], modelRefreshError: '' },
        },
      }))
	  const response = await sendToCEF<{ started?: boolean; pending?: boolean }>({
		action: 'discoverProviderModels',
		payload: { chatId: sessionId, ...(providerId ? { providerId } : {}), ...(workspaceDirectory ? { workspaceDirectory } : {}), ...(executionHostId ? { executionHostId } : {}) },
	  })
	  if (!response.ok) {
		if (sessionId) set((state) => ({
          acpBindingBySessionId: {
            ...state.acpBindingBySessionId,
            [sessionId]: { ...state.acpBindingBySessionId[sessionId], modelsLoading: false, modelRefreshError: response.error ?? 'Model discovery failed.' },
          },
        }))
        return false
      }
	  if (sessionId && response.data?.pending) set((state) => ({
        acpBindingBySessionId: {
          ...state.acpBindingBySessionId,
          [sessionId]: { ...state.acpBindingBySessionId[sessionId], modelsLoading: true },
        },
      }))
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

    stopAcpSession: async (sessionId: string): Promise<boolean> => {
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
  }
}
