import type { Session } from '../../types/session'
import type { Attachment, Message } from '../../types/message'
import type { Provider } from '../../types/provider'
import { sendToCEF, isCefContext, createRequestId } from '../../ipc/cefBridge'
import {
  CLAUDE_CLI_PROVIDER_ID,
  CODEX_CLI_PROVIDER_ID,
  COPILOT_CLI_PROVIDER_ID,
  DEFAULT_PROVIDER_ID as GEMINI_CLI_PROVIDER_ID,
  OPENCODE_CLI_PROVIDER_ID,
  buildProviderCliInstallCommand,
  fallbackProviderForId,
  normalizeCliProviderIdAlias,
  providerCapabilities,
} from '../../utils/providerMetadata'
import {
  ACP_APPROVAL_MODE_IDS,
  cefPayloadOrRawResponse,
  clampedFiniteNumberOr,
  DEFAULT_MEMORY_IDLE_DELAY_SECONDS,
  DEFAULT_MEMORY_RECALL_BUDGET_BYTES,
  defaultEditorFileAssociations,
  emptyCliVersionManager,
  emptyCliVersionProviderState,
  emptyMemoryActivity,
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
  upsertCliVersionProviderState,
} from '../cpp/sanitizers'
import {
  clearPendingCodexOptions,
  clearPendingRequest,
  cliLifecycleIsProcessing,
  isLatestPendingRequest,
  normalizeCliLifecycleState,
  reconcileCppMessages,
  rememberPendingRequest,
} from '../cpp/reconcile'
import { pendingCodexOptionsByChatId, pendingRequestIdsByKey } from '../push/pushBuffers'
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
  MemoryActivity,
  MemoryWorkerBinding,
  OpenWorkspaceEditorResponse,
  ProviderChatDefaults,
  VcsCommitMessageSuggestion,
  VcsCommitResult,
  VcsCommitStatus,
  VcsFileDiffResponse,
  VcsType,
} from '../cpp/types'
import type { AppState, ZustandSet, ZustandGet } from '../storeTypes'

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

  return {
    folders: inCef ? [] : initialFolders,
    sessions: inCef ? [] : initialSessions,
    activeSessionId: (inCef ? null : 's1') as string | null,
    lastAppliedStateRevision: -1,
    messages: {} as Record<string, Message[]>,
    providers: inCef ? [] : initialProviders,
    cliBindingBySessionId: {} as Record<string, CliBinding>,
    acpBindingBySessionId: {} as Record<string, AcpBinding>,
    cliTranscriptBySessionId: {} as Record<string, CliTranscript>,
    cliDebugState: null as CppCliDebugState | null,
    memoryEnabledDefault: true,
    memoryIdleDelaySeconds: DEFAULT_MEMORY_IDLE_DELAY_SECONDS,
    memoryRecallBudgetBytes: DEFAULT_MEMORY_RECALL_BUDGET_BYTES,
    memoryLastStatus: '',
    memoryWorkerBindings: {} as Record<string, MemoryWorkerBinding>,
    memoryActivity: { ...emptyMemoryActivity } as MemoryActivity,
    cliVersionManager: { ...emptyCliVersionManager } as CliVersionManager,
    defaultNewChatProviderId: GEMINI_CLI_PROVIDER_ID,
    providerChatDefaults: {} as Record<string, ProviderChatDefaults>,
    defaultEditorPresetId: 'vscode',
    editorFileAssociations: defaultEditorFileAssociations() as EditorFileAssociation[],

    setActiveSession: (id: string) => {
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

    addSession: (name: string, folderId: string | null, providerId = GEMINI_CLI_PROVIDER_ID) => {
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

    openSubAgentSession: async (sourceChatId: string, nativeSessionId: string, title = '') => {
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

    getVcsCommitStatus: async (id: string, vcsType: VcsType = 'git', options: { includeLineStats?: boolean; requestId?: string } = {}): Promise<VcsCommitStatus | null> => {
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

    getVcsFileDiff: async (id: string, path: string, vcsType: VcsType): Promise<string> => {
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
            sessions: state.sessions.map((s) => (s.id === id ? previousSession : s)),
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
              reasoningEffort: providerCapabilities(requestedProviderId).hasReasoningEffort ? defaults.reasoningEffort : '',
              serviceTier: providerCapabilities(requestedProviderId).hasServiceTier ? defaults.serviceTier : '',
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

    setSessionCodexOptions: async (id: string, options: { reasoningEffort?: string; serviceTier?: string }): Promise<boolean> => {
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

    setSessionApprovalMode: async (id: string, modeId: string): Promise<boolean> => {
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

    setSessionAutoApproveCommands: async (id: string, enabled: boolean): Promise<boolean> => {
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

    setSessionMemoryEnabled: async (id: string, enabled: boolean): Promise<boolean> => {
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

    setMemorySettings: async (settings: Partial<Pick<AppState, 'memoryEnabledDefault' | 'memoryIdleDelaySeconds' | 'memoryRecallBudgetBytes' | 'memoryWorkerBindings'>>): Promise<boolean> => {
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

    setEditorSettings: async (settings: Pick<AppState, 'defaultEditorPresetId' | 'editorFileAssociations'>): Promise<boolean> => {
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
      for (const [providerId, defaults] of Object.entries(nextDefaults)) {
        const sanitized = sanitizeProviderChatDefaults(defaults)
        const caps = providerCapabilities(providerId)
        if (!caps.hasReasoningEffort) {
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

    deleteSession: (id: string) => {
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

    sendAcpPrompt: async (sessionId: string, text: string, attachments: Attachment[] = []): Promise<boolean> => {
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
