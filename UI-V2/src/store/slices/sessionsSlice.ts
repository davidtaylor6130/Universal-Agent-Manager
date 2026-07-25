import type { Session } from '../../types/session'
import type { Attachment, Message } from '../../types/message'
import type { Provider } from '../../types/provider'
import type { MemoryLevel } from '../../types/memory'
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
  AGENT_MODE_IDS,
  cefPayloadOrRawResponse,
  clampedFiniteNumberOr,
  DEFAULT_GOAL_MAX_LOOP_ITERATIONS,
  DEFAULT_MEMORY_IDLE_DELAY_SECONDS,
  DEFAULT_MEMORY_RECALL_BUDGET_BYTES,
  defaultEditorFileAssociations,
  emptyCliVersionManager,
  emptyCliVersionProviderState,
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
  latestOptimisticRollback,
  normalizeCliLifecycleState,
  reconcileCppMessages,
  rememberOptimisticFields,
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
  OpenNativeSessionChatResponse,
  OpenWorkspaceEditorResponse,
  ProviderChatDefaults,
  VcsCommitMessageSuggestion,
  VcsCommitResult,
  VcsCommitStatus,
  VcsFileDiffResponse,
  VcsType,
  VoiceInputCapabilities,
  VoiceInputMode,
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
  let intentionalSelectionRevision = 0

  const requestChatMessagesFromCef = (chatId: string) => {
    if (!isCefContext() || !chatId) return
    const current = get()
    const session = current.sessions.find((candidate) => candidate.id === chatId)
    const requestKey = `getChatMessages:${chatId}`
    const requestId = createRequestId('getChatMessages')
    rememberPendingRequest(requestKey, requestId)
    void sendToCEF<ChatMessagesResponse>({
      action: 'getChatMessages',
      payload: {
        chatId,
        messagesDigest: session?.messagesDigest ?? '',
      },
      requestId,
    }).then((response) => {
      if (!isLatestPendingRequest(requestKey, response.requestId)) return
      clearPendingRequest(requestKey, response.requestId)
      if (!response.ok || !response.data) return

      const data = response.data
      if (data.chatId && data.chatId !== chatId) return
      if (data.unchanged) return
      if (!Array.isArray(data.messages)) return

      set((state) => {
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
    memoryLevelDefault: 'strict' as MemoryLevel,
    memoryIdleDelaySeconds: DEFAULT_MEMORY_IDLE_DELAY_SECONDS,
    memoryRecallBudgetBytes: DEFAULT_MEMORY_RECALL_BUDGET_BYTES,
    goalMaxLoopIterations: DEFAULT_GOAL_MAX_LOOP_ITERATIONS,
    appVersion: 'V4.4.2',
    updateChecksEnabled: true,
    updateLastCheckedAt: '',
    dismissedUpdateVersions: {} as Record<string, string>,
    memoryLastStatus: '',
    memoryWorkerBindings: {} as Record<string, MemoryWorkerBinding>,
    memoryActivity: { ...emptyMemoryActivity } as MemoryActivity,
    cliVersionManager: { ...emptyCliVersionManager } as CliVersionManager,
    defaultNewChatProviderId: GEMINI_CLI_PROVIDER_ID,
    providerChatDefaults: {} as Record<string, ProviderChatDefaults>,
    defaultEditorPresetId: 'vscode',
    editorFileAssociations: defaultEditorFileAssociations() as EditorFileAssociation[],
    voiceInputMode: 'system' as VoiceInputMode,
    voiceInputServerBaseUrl: '',
    voiceInputServerEndpoint: '/v1/audio/transcriptions',
    voiceInputServerModel: 'whisper-1',
    voiceInputApiKeyEnv: 'OPENAI_API_KEY',
    voiceInputCapabilities: {
      system: { supported: true, reason: '' },
      local: { supported: false, reason: 'Coming soon.' },
      server: { supported: false, reason: 'Unavailable.' },
    } as VoiceInputCapabilities,

    setActiveSession: (id: string) => {
      intentionalSelectionRevision += 1
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

    loadSessionMessages: requestChatMessagesFromCef,

    addSession: async (name: string, folderId: string | null, providerId = GEMINI_CLI_PROVIDER_ID, modelId?: string, reasoningEffort?: string) => {
      const current = get()
      const selectedFolderId = folderId && current.folders.some((folder) => folder.id === folderId)
        ? folderId
        : null
      if (!selectedFolderId) {
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
        const resp = await sendToCEF({
          action: 'createSession',
          payload: { title: name, folderId: selectedFolderId, providerId: requestedProviderId, defaults },
        })
        if (!resp.ok) {
          console.error('[CEF] createSession failed:', resp.error)
          return false
        }

        set({ isNewChatModalOpen: false, newChatFolderId: null })
        return true
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
        approvalMode: defaults.approvalMode === 'acceptEdits' ? 'default' : defaults.approvalMode,
        commandSafetyTier: defaults.approvalMode === 'acceptEdits' ? 'acceptEdits' : 'medium',
        autoApproveCommands: defaults.autoApproveCommands,
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
              reasoningEffort: defaults.reasoningEffort,
              serviceTier: providerCapabilities(requestedProviderId).hasServiceTier ? defaults.serviceTier : '',
              approvalMode: defaults.approvalMode === 'acceptEdits' ? 'default' : defaults.approvalMode,
              commandSafetyTier: defaults.approvalMode === 'acceptEdits' ? 'acceptEdits' : s.commandSafetyTier,
              autoApproveCommands: defaults.autoApproveCommands,
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
              reasoningEffort: previousSession.reasoningEffort,
              serviceTier: previousSession.serviceTier,
              approvalMode: previousSession.approvalMode,
              commandSafetyTier: previousSession.commandSafetyTier,
              autoApproveCommands: previousSession.autoApproveCommands,
              memoryLevel: previousSession.memoryLevel,
              memoryEnabled: previousSession.memoryEnabled,
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

      const applyModel = () => {
        set((state) => ({
          sessions: state.sessions.map((s) =>
			s.id === id ? { ...s, modelId: requestedModelId, updatedAt: optimisticUpdatedAt } : s
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
            sessions: state.sessions.map((s) => s.id === id ? {
              ...s,
              modelId: previousSession.modelId,
              updatedAt: s.updatedAt === optimisticUpdatedAt ? previousSession.updatedAt : s.updatedAt,
            } : s),
          }))
          pendingRequestIdsByKey.delete(requestKey)
        }

        return false
      }

      applyModel()
      return true
    },

    setSessionCodexOptions: async (id: string, options: { reasoningEffort?: string; serviceTier?: string }): Promise<boolean> => {
      const previousSession = get().sessions.find((s) => s.id === id)
	  if (!previousSession) {
        return false
      }
	  const codexProvider = (previousSession.providerId ?? GEMINI_CLI_PROVIDER_ID) === CODEX_CLI_PROVIDER_ID
	  const runtimeModel = get().acpBindingBySessionId[id]?.availableModels.find((model) => model.id === (previousSession.modelId ?? ''))
	  const supportedEfforts = runtimeModel?.supportedReasoningEfforts ?? []
	  if (!codexProvider && supportedEfforts.length === 0) return false
	  const normalizedEffort = options.reasoningEffort === undefined ? previousSession.reasoningEffort ?? '' : normalizeCodexReasoningEffort(options.reasoningEffort)
	  const requestedReasoningEffort = options.reasoningEffort === undefined
	    ? normalizedEffort
	    : supportedEfforts.length > 0
	    ? supportedEfforts.includes(normalizedEffort) ? normalizedEffort : supportedEfforts[supportedEfforts.length - 1]
	    : normalizedEffort
	  const requestedServiceTier = options.serviceTier === undefined ? previousSession.serviceTier ?? '' : codexProvider ? normalizeCodexServiceTier(options.serviceTier) : ''
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
          return true
        }

        if (isLatestPendingRequest(requestKey, response.requestId)) {
          set((state) => ({
            sessions: state.sessions.map((s) => (s.id === id ? {
              ...s,
              reasoningEffort: previousSession.reasoningEffort,
              serviceTier: previousSession.serviceTier,
            } : s)),
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
            sessions: state.sessions.map((s) => (s.id === id ? {
              ...s,
              approvalMode: previousSession.approvalMode,
            } : s)),
            acpBindingBySessionId: previousBinding && state.acpBindingBySessionId[id]
              ? {
                  ...state.acpBindingBySessionId,
                  [id]: {
                    ...state.acpBindingBySessionId[id],
                    currentModeId: previousBinding.currentModeId,
                  },
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
            sessions: state.sessions.map((s) => (s.id === id ? {
              ...s,
              autoApproveCommands: previousSession.autoApproveCommands,
            } : s)),
          }))
          pendingRequestIdsByKey.delete(requestKey)
        }

        return false
      }

      applyAutoApprove()
      return true
    },

    setSessionCommandSafetyTier: async (id: string, tier: 'off' | 'acceptEdits' | 'low' | 'medium' | 'high' | 'yolo'): Promise<boolean> => {
      const previousSession = get().sessions.find((session) => session.id === id)
      if (!previousSession || (previousSession.commandSafetyTier ?? 'medium') === tier) return Boolean(previousSession)
      const applyTier = () => set((state) => ({
        sessions: state.sessions.map((session) => session.id === id ? { ...session, commandSafetyTier: tier, updatedAt: new Date() } : session),
      }))
      if (!isCefContext()) {
        applyTier()
        return true
      }

	  const requestKey = `setSessionCommandSafetyTier:${id}`
	  const requestId = createRequestId('setSessionCommandSafetyTier')
	  rememberPendingRequest(requestKey, requestId)
      applyTier()
      const response = await sendToCEF({
        action: 'setChatCommandSafetyTier',
        payload: { chatId: id, commandSafetyTier: tier },
        requestId,
      })
	  if (response.ok) {
		clearPendingRequest(requestKey, response.requestId)
		return true
	  }
	  if (isLatestPendingRequest(requestKey, response.requestId)) {
		set((state) => ({
		  sessions: state.sessions.map((session) => session.id === id ? {
		    ...session,
		    commandSafetyTier: previousSession.commandSafetyTier,
		  } : session),
		}))
		pendingRequestIdsByKey.delete(requestKey)
	  }
      return false
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

    setMemorySettings: async (settings: Partial<Pick<AppState, 'memoryEnabledDefault' | 'memoryLevelDefault' | 'memoryIdleDelaySeconds' | 'memoryRecallBudgetBytes' | 'goalMaxLoopIterations' | 'memoryWorkerBindings'>>): Promise<boolean> => {
      const previous = {
        memoryEnabledDefault: get().memoryEnabledDefault,
        memoryLevelDefault: get().memoryLevelDefault,
        memoryIdleDelaySeconds: get().memoryIdleDelaySeconds,
        memoryRecallBudgetBytes: get().memoryRecallBudgetBytes,
        goalMaxLoopIterations: get().goalMaxLoopIterations,
        memoryWorkerBindings: get().memoryWorkerBindings,
      }
      const requestedDefaultLevel = settings.memoryLevelDefault
        ?? (settings.memoryEnabledDefault === undefined ? previous.memoryLevelDefault : settings.memoryEnabledDefault ? 'strict' : 'off')
      const next = {
        memoryLevelDefault: normalizeMemoryLevel(requestedDefaultLevel),
        memoryEnabledDefault: normalizeMemoryLevel(requestedDefaultLevel) !== 'off',
        memoryIdleDelaySeconds: clampedFiniteNumberOr(settings.memoryIdleDelaySeconds, previous.memoryIdleDelaySeconds, MIN_MEMORY_IDLE_DELAY_SECONDS, MAX_MEMORY_IDLE_DELAY_SECONDS),
        memoryRecallBudgetBytes: clampedFiniteNumberOr(settings.memoryRecallBudgetBytes, previous.memoryRecallBudgetBytes, MIN_MEMORY_RECALL_BUDGET_BYTES, MAX_MEMORY_RECALL_BUDGET_BYTES),
        goalMaxLoopIterations: Math.max(0, Math.floor(finiteNumberOr(settings.goalMaxLoopIterations, previous.goalMaxLoopIterations))),
        memoryWorkerBindings: settings.memoryWorkerBindings ?? previous.memoryWorkerBindings,
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
            workerBindings: next.memoryWorkerBindings,
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

    setVoiceInputSettings: async (settings: Pick<AppState, 'voiceInputMode' | 'voiceInputServerBaseUrl' | 'voiceInputServerEndpoint' | 'voiceInputServerModel' | 'voiceInputApiKeyEnv'>): Promise<boolean> => {
      const previous = {
        voiceInputMode: get().voiceInputMode,
        voiceInputServerBaseUrl: get().voiceInputServerBaseUrl,
        voiceInputServerEndpoint: get().voiceInputServerEndpoint,
        voiceInputServerModel: get().voiceInputServerModel,
        voiceInputApiKeyEnv: get().voiceInputApiKeyEnv,
      }
      const optimisticRevision = rememberOptimisticFields(Object.keys(settings))
      set(settings)
      if (!isCefContext()) return true
      const response = await sendToCEF({
        action: 'setVoiceInputSettings',
        payload: {
          mode: settings.voiceInputMode,
          serverBaseUrl: settings.voiceInputServerBaseUrl,
          serverEndpoint: settings.voiceInputServerEndpoint,
          serverModel: settings.voiceInputServerModel,
          apiKeyEnv: settings.voiceInputApiKeyEnv,
        },
      })
      if (response.ok) return true
      set(latestOptimisticRollback(previous, optimisticRevision))
      return false
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
        const optimisticActiveSessionId = get().activeSessionId
        const optimisticSelectionRevision = intentionalSelectionRevision

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
                ...(!Object.prototype.hasOwnProperty.call(state.messages, id) ? { [id]: deletedMessages } : {}),
              },
              cliBindingBySessionId: deletedBinding && !Object.prototype.hasOwnProperty.call(state.cliBindingBySessionId, id)
                ? { ...state.cliBindingBySessionId, [id]: deletedBinding }
                : state.cliBindingBySessionId,
              acpBindingBySessionId: deletedAcpBinding && !Object.prototype.hasOwnProperty.call(state.acpBindingBySessionId, id)
                ? { ...state.acpBindingBySessionId, [id]: deletedAcpBinding }
                : state.acpBindingBySessionId,
              cliTranscriptBySessionId: deletedTranscript && !Object.prototype.hasOwnProperty.call(state.cliTranscriptBySessionId, id)
                ? { ...state.cliTranscriptBySessionId, [id]: deletedTranscript }
                : state.cliTranscriptBySessionId,
              activeSessionId:
                state.activeSessionId === optimisticActiveSessionId &&
                intentionalSelectionRevision === optimisticSelectionRevision
                  ? previousActiveSessionId
                  : state.activeSessionId,
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
            steerNow,
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
              providerId: state.sessions.find((session) => session.id === sessionId)?.providerId,
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

    discoverProviderModels: async (sessionId: string): Promise<boolean> => {
      if (!isCefContext()) return false
      const response = await sendToCEF<{ started?: boolean; pending?: boolean }>({ action: 'discoverProviderModels', payload: { chatId: sessionId } })
      if (!response.ok) return false
      if (response.data?.pending) set((state) => ({
        acpBindingBySessionId: {
          ...state.acpBindingBySessionId,
          [sessionId]: { ...state.acpBindingBySessionId[sessionId], modelsLoading: true },
        },
      }))
      return Boolean(response.data?.started || response.data?.pending)
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
