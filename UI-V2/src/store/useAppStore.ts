import { create } from 'zustand'
import { Session, Folder } from '../types/session'
import { Message } from '../types/message'
import { Provider } from '../types/provider'
import type { MemoryLevel } from '../types/memory'
import { sendToCEF, isCefContext, createRequestId } from '../ipc/cefBridge'
import {
  CLI_TRANSCRIPT_FLUSH_DELAY_MS,
  STREAM_TOKEN_FLUSH_DELAY_MS,
  pendingCliTranscriptChunksBySessionId,
  pendingStreamTokensByChatId,
  pushFlushTimers,
} from './push/pushBuffers'
import {
  CLAUDE_CLI_PROVIDER_ID,
  CODEX_CLI_PROVIDER_ID,
  COPILOT_CLI_PROVIDER_ID,
  DEFAULT_PROVIDER_ID as GEMINI_CLI_PROVIDER_ID,
  OPENCODE_CLI_PROVIDER_ID,
  fallbackProviderForId,
} from '../utils/providerMetadata'
import {
  sanitizeCppAppState,
  sanitizeMemoryActivity,
  defaultEditorFileAssociations,
} from './cpp/sanitizers'
export {
  DEFAULT_MEMORY_IDLE_DELAY_SECONDS,
  MIN_MEMORY_IDLE_DELAY_SECONDS,
  MAX_MEMORY_IDLE_DELAY_SECONDS,
  DEFAULT_MEMORY_RECALL_BUDGET_BYTES,
  MIN_MEMORY_RECALL_BUDGET_BYTES,
  MAX_MEMORY_RECALL_BUDGET_BYTES,
} from './cpp/sanitizers'

// ---------------------------------------------------------------------------
// C++ state serialisation types (mirrors state_serializer.cpp output)
// ---------------------------------------------------------------------------
// Moved to ./cpp/types.ts (MO-1). Re-exported so existing imports keep working.

export * from './cpp/types'
import type {
  AcpBinding,
  AcpLifecycleState,
  ChatMessagesResponse,
  CliBinding,
  CliTranscript,
  CliVersionManager,
  CppAppState,
  CppCliDebugState,
  CppStatePatch,
  EditorFileAssociation,
  MemoryActivity,
  MemoryWorkerBinding,
  McpServerConfiguration,
  ProviderChatDefaults,
  ProviderModelCatalog,
  ShellAction,
} from './cpp/types'

import {
  acpBindingFromCppChat,
  applyPendingCodexOptions,
  appendCliTranscriptChunk,
  cliBindingFromCppChat,
  clampCliTranscript,
  cppPatchRevision,
  cppStateRevision,
  decodeCliChunk,
  folderFromCppFolder,
  normalizeCliTranscript,
  normalizeProviderIdForVisibleProviders,
  providerFromCppProvider,
  reconcileCppMessages,
  sameArrayEntries,
  sameRecordEntries,
  sessionFromCppChat,
} from './cpp/reconcile'

import { pendingProviderChatDefaults } from './slices/sessionsSlice'

import { parseUamPushPayload, cliDebugSignature, isNewerStateRevision } from './push/uamPush'
import { persistTheme } from './slices/uiSlice'
import { createSessionsSlice } from './slices/sessionsSlice'
import { createFoldersSlice } from './slices/foldersSlice'
import { createGoalsSlice } from './slices/goalsSlice'
import { createUiSlice } from './slices/uiSlice'
import { createResourceCollectionsSlice } from './slices/resourceCollectionsSlice'
import type { ResourceCollection } from '../types/resourceCollection'

import type { AppState } from './storeTypes'
export type { AppState }

// ---------------------------------------------------------------------------
// Deserialiser — convert C++ format → React store format
// ---------------------------------------------------------------------------

const initialProviders: Provider[] = [
  { ...fallbackProviderForId(GEMINI_CLI_PROVIDER_ID), color: '#f97316' },
  { ...fallbackProviderForId(CODEX_CLI_PROVIDER_ID), color: '#0ea5e9' },
  { ...fallbackProviderForId(CLAUDE_CLI_PROVIDER_ID), color: '#7c3aed' },
  { ...fallbackProviderForId(OPENCODE_CLI_PROVIDER_ID), color: '#14b8a6' },
  { ...fallbackProviderForId(COPILOT_CLI_PROVIDER_ID), color: '#22c55e' },
]

let lastPushStatusUpdateAtMs = 0

function deserializeState(
  cpp: CppAppState,
  existing: {
    sessions: Session[]
    folders: Folder[]
    resourceCollections: ResourceCollection[]
    messages: Record<string, Message[]>
    providers: Provider[]
    activeSessionId: string | null
    cliTranscriptBySessionId: Record<string, CliTranscript>
    cliBindingBySessionId: Record<string, CliBinding>
    acpBindingBySessionId: Record<string, AcpBinding>
    providerModelCatalogs: ProviderModelCatalog[]
    cliDebugState: CppCliDebugState | null
    memoryEnabledDefault: boolean
    memoryLevelDefault: MemoryLevel
    memoryIdleDelaySeconds: number
    memoryRecallBudgetBytes: number
    goalMaxLoopIterations: number
    acpSetupInactivityTimeoutSeconds: number
    acpTurnOutputLimitMiB: number
    appVersion: string
    runnerProtocolVersion: number
    showProviderIconsInSidebar: boolean
    showWorktreePathInSidebar: boolean
    updateChecksEnabled: boolean
    updateLastCheckedAt: string
    dismissedUpdateVersions: Record<string, string>
    memoryLastStatus: string
    memoryWorkerBindings: Record<string, MemoryWorkerBinding>
    permissionReviewerProviderId: string
    permissionReviewerModelId: string
    memoryActivity: MemoryActivity
    cliVersionManager: CliVersionManager
    markdownStoreDirectory: string
    defaultNewChatProviderId: string
    providerChatDefaults: Record<string, ProviderChatDefaults>
    defaultEditorPresetId: string
    editorFileAssociations: EditorFileAssociation[]
    mcpServers: McpServerConfiguration[]
    executionHosts: AppState['executionHosts']
    favoriteUamAgentIds: string[]
    uamAgentCycleShortcut: AppState['uamAgentCycleShortcut']
    uamAgentsBySessionId: AppState['uamAgentsBySessionId']
    shellActions: ShellAction[]
    shellActionNotification: string
    statusLine: string
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
    if (!Array.isArray(chat.messages) || (chat.messages.length === 0 && existingMessages?.length)) {
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
  const effectiveActiveSessionId = cpp.selectedChatId === null
    ? null
    : selectedByBackend ?? selectedFromCurrent ?? sessions[0]?.id ?? null
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

  const goalsByChatId: Record<string, import('../types/goal').Goal[]> = {}
  const activeGoalIdByChatId: Record<string, string | null> = {}
  for (const chat of cpp.chats) {
    if (chat.activeGoalId !== undefined) {
      activeGoalIdByChatId[chat.id] = chat.activeGoalId
    }
    if (Array.isArray(chat.goals) && chat.goals.length > 0) {
      const goalObjects: import('../types/goal').Goal[] = chat.goals.map((cppGoal) => ({
        id: cppGoal.id,
        chatId: chat.id,
        objective: cppGoal.objective,
        status: cppGoal.status,
        tokenBudget: cppGoal.tokenBudget,
        tokensUsed: cppGoal.tokensUsed,
        blockedTurnCount: cppGoal.blockedTurnCount,
        lastBlocker: cppGoal.lastBlocker,
        lastDiagnostic: cppGoal.lastDiagnostic,
        completedItems: cppGoal.completedItems,
        remainingItems: cppGoal.remainingItems,
        currentStep: cppGoal.currentStep,
        lastVerification: cppGoal.lastVerification,
        lastNextPrompt: cppGoal.lastNextPrompt,
        sameNextPromptCount: cppGoal.sameNextPromptCount,
        loopCount: cppGoal.loopCount,
        createdAt: new Date(cppGoal.createdAt || Date.now()),
        updatedAt: new Date(cppGoal.updatedAt || Date.now()),
		executionOwner: cppGoal.executionOwner ?? 'uam',
		providerCommand: cppGoal.providerCommand ?? '',
		workerModelId: cppGoal.workerModelId ?? '',
		reviewerModelId: cppGoal.reviewerModelId ?? '',
      }))
      goalsByChatId[chat.id] = goalObjects
    }
  }

  return {
    folders,
    resourceCollections: cpp.resourceCollections ?? existing.resourceCollections,
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
    providerModelCatalogs: cpp.providerModelCatalogs ?? existing.providerModelCatalogs,
    cliTranscriptBySessionId,
    cliDebugState,
    memoryEnabledDefault: cpp.settings.memoryEnabledDefault,
    memoryLevelDefault: cpp.settings.memoryLevelDefault ?? (cpp.settings.memoryEnabledDefault ? 'strict' : 'off'),
    memoryIdleDelaySeconds: cpp.settings.memoryIdleDelaySeconds,
    memoryRecallBudgetBytes: cpp.settings.memoryRecallBudgetBytes,
    goalMaxLoopIterations: cpp.settings.goalMaxLoopIterations,
    acpSetupInactivityTimeoutSeconds: cpp.settings.acpSetupInactivityTimeoutSeconds ?? 600,
    acpTurnOutputLimitMiB: cpp.settings.acpTurnOutputLimitMiB ?? 1024,
    appVersion: cpp.appVersion || existing.appVersion,
    runnerProtocolVersion: cpp.runnerProtocolVersion || existing.runnerProtocolVersion,
    showProviderIconsInSidebar: cpp.settings.showProviderIconsInSidebar ?? true,
    showWorktreePathInSidebar: cpp.settings.showWorktreePathInSidebar ?? true,
    updateChecksEnabled: cpp.settings.updateChecksEnabled,
    updateLastCheckedAt: cpp.settings.updateLastCheckedAt,
    dismissedUpdateVersions: cpp.settings.dismissedUpdateVersions,
    memoryLastStatus: cpp.settings.memoryLastStatus,
    memoryWorkerBindings: cpp.settings.memoryWorkerBindings,
    permissionReviewerProviderId: cpp.settings.permissionReviewerProviderId ?? '',
    permissionReviewerModelId: cpp.settings.permissionReviewerModelId ?? '',
    memoryActivity: cpp.memoryActivity ?? sanitizeMemoryActivity(undefined, cpp.settings.memoryLastStatus),
    cliVersionManager: cpp.cliVersionManager ?? existing.cliVersionManager,
    markdownStoreDirectory: cpp.settings.markdownStoreDirectory ?? '',
    defaultNewChatProviderId: pendingProviderChatDefaults?.defaultNewChatProviderId ?? cpp.settings.defaultNewChatProviderId ?? cpp.settings.activeProviderId ?? GEMINI_CLI_PROVIDER_ID,
    providerChatDefaults: pendingProviderChatDefaults?.providerChatDefaults ?? cpp.settings.providerChatDefaults ?? {},
    defaultEditorPresetId: cpp.settings.defaultEditorPresetId ?? 'vscode',
    editorFileAssociations: cpp.settings.editorFileAssociations ?? defaultEditorFileAssociations(),
    mcpServers: cpp.settings.mcpServers ?? [],
    executionHosts: cpp.settings.executionHosts ?? existing.executionHosts,
    favoriteUamAgentIds: cpp.settings.favoriteUamAgentIds ?? [],
    uamAgentCycleShortcut: cpp.settings.uamAgentCycleShortcut ?? 'shift+tab',
    uamAgentsBySessionId: existing.uamAgentsBySessionId,
    shellActions: cpp.shellActions ?? existing.shellActions,
    shellActionNotification: cpp.shellActionNotification ?? existing.shellActionNotification,
    statusLine: cpp.statusLine ?? existing.statusLine,
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
  const resourceCollections = patch.resourceCollections ?? current.resourceCollections

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
      const nextTranscript = normalizeCliTranscript(
        cliTranscriptBySessionId[chat.id],
        nextCli.terminalId
      )
      if (nextTranscript) cliTranscriptBySessionId[chat.id] = nextTranscript
      else delete cliTranscriptBySessionId[chat.id]
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
        lastDiagnostic: cppGoal.lastDiagnostic,
        completedItems: cppGoal.completedItems,
        remainingItems: cppGoal.remainingItems,
        currentStep: cppGoal.currentStep,
        lastVerification: cppGoal.lastVerification,
        lastNextPrompt: cppGoal.lastNextPrompt,
        sameNextPromptCount: cppGoal.sameNextPromptCount,
        loopCount: cppGoal.loopCount,
        createdAt: new Date(cppGoal.createdAt || Date.now()),
        updatedAt: new Date(cppGoal.updatedAt || Date.now()),
        executionOwner: cppGoal.executionOwner ?? 'uam',
        providerCommand: cppGoal.providerCommand ?? '',
        workerModelId: cppGoal.workerModelId ?? '',
        reviewerModelId: cppGoal.reviewerModelId ?? '',
      }))
    }
  }

  const activeSessionId =
    patch.selectedChatId !== undefined
      ? patch.selectedChatId && sessions.some((session) => session.id === patch.selectedChatId)
        ? patch.selectedChatId
        : null
      : current.activeSessionId === null
        ? null
        : sessions.some((session) => session.id === current.activeSessionId)
          ? current.activeSessionId
          : sessions[0]?.id ?? null

  const sessionsWithPendingCodexOptions = applyPendingCodexOptions(sessions)

  return {
    folders,
    resourceCollections,
    sessions: sameArrayEntries(current.sessions, sessionsWithPendingCodexOptions) ? current.sessions : sessionsWithPendingCodexOptions,
    messages: sameRecordEntries(current.messages, messages) ? current.messages : messages,
    goalsByChatId: sameRecordEntries(current.goalsByChatId, goalsByChatId) ? current.goalsByChatId : goalsByChatId,
    activeGoalIdByChatId: sameRecordEntries(current.activeGoalIdByChatId, activeGoalIdByChatId) ? current.activeGoalIdByChatId : activeGoalIdByChatId,
    providers,
    providerModelCatalogs: patch.providerModelCatalogs ?? current.providerModelCatalogs,
    activeSessionId,
    lastAppliedStateRevision: nextRevision,
    theme: patch.settings?.theme ?? current.theme,
    cliBindingBySessionId: sameRecordEntries(current.cliBindingBySessionId, cliBindingBySessionId) ? current.cliBindingBySessionId : cliBindingBySessionId,
    acpBindingBySessionId: sameRecordEntries(current.acpBindingBySessionId, acpBindingBySessionId) ? current.acpBindingBySessionId : acpBindingBySessionId,
    cliTranscriptBySessionId: sameRecordEntries(current.cliTranscriptBySessionId, cliTranscriptBySessionId) ? current.cliTranscriptBySessionId : cliTranscriptBySessionId,
    memoryEnabledDefault: patch.settings?.memoryEnabledDefault ?? current.memoryEnabledDefault,
    memoryLevelDefault: patch.settings?.memoryLevelDefault ?? (patch.settings?.memoryEnabledDefault === undefined ? current.memoryLevelDefault : patch.settings.memoryEnabledDefault ? 'strict' : 'off'),
    memoryIdleDelaySeconds: patch.settings?.memoryIdleDelaySeconds ?? current.memoryIdleDelaySeconds,
    memoryRecallBudgetBytes: patch.settings?.memoryRecallBudgetBytes ?? current.memoryRecallBudgetBytes,
    goalMaxLoopIterations: patch.settings?.goalMaxLoopIterations ?? current.goalMaxLoopIterations,
    acpSetupInactivityTimeoutSeconds: patch.settings?.acpSetupInactivityTimeoutSeconds ?? current.acpSetupInactivityTimeoutSeconds,
    acpTurnOutputLimitMiB: patch.settings?.acpTurnOutputLimitMiB ?? current.acpTurnOutputLimitMiB,
    showProviderIconsInSidebar: patch.settings?.showProviderIconsInSidebar ?? current.showProviderIconsInSidebar,
    showWorktreePathInSidebar: patch.settings?.showWorktreePathInSidebar ?? current.showWorktreePathInSidebar,
    updateChecksEnabled: patch.settings?.updateChecksEnabled ?? current.updateChecksEnabled,
    updateLastCheckedAt: patch.settings?.updateLastCheckedAt ?? current.updateLastCheckedAt,
    dismissedUpdateVersions: patch.settings?.dismissedUpdateVersions ?? current.dismissedUpdateVersions,
    memoryLastStatus: patch.settings?.memoryLastStatus ?? current.memoryLastStatus,
    memoryWorkerBindings: patch.settings?.memoryWorkerBindings ?? current.memoryWorkerBindings,
    permissionReviewerProviderId: patch.settings?.permissionReviewerProviderId ?? current.permissionReviewerProviderId,
    permissionReviewerModelId: patch.settings?.permissionReviewerModelId ?? current.permissionReviewerModelId,
    memoryActivity: patch.memoryActivity ?? current.memoryActivity,
    cliVersionManager: patch.cliVersionManager ?? current.cliVersionManager,
    markdownStoreDirectory: patch.settings?.markdownStoreDirectory ?? current.markdownStoreDirectory,
    defaultNewChatProviderId: pendingProviderChatDefaults?.defaultNewChatProviderId ?? patch.settings?.defaultNewChatProviderId ?? current.defaultNewChatProviderId,
    providerChatDefaults: pendingProviderChatDefaults?.providerChatDefaults ?? patch.settings?.providerChatDefaults ?? current.providerChatDefaults,
    defaultEditorPresetId: patch.settings?.defaultEditorPresetId ?? current.defaultEditorPresetId,
    editorFileAssociations: patch.settings?.editorFileAssociations ?? current.editorFileAssociations,
    mcpServers: patch.settings?.mcpServers ?? current.mcpServers,
    executionHosts: patch.settings?.executionHosts ?? current.executionHosts,
    favoriteUamAgentIds: patch.settings?.favoriteUamAgentIds ?? current.favoriteUamAgentIds,
    uamAgentCycleShortcut: patch.settings?.uamAgentCycleShortcut ?? current.uamAgentCycleShortcut,
    uamAgentsBySessionId: current.uamAgentsBySessionId,
    shellActions: patch.shellActions ?? current.shellActions,
    shellActionNotification: patch.shellActionNotification ?? current.shellActionNotification,
    statusLine: patch.statusLine ?? current.statusLine,
  }
}

// ---------------------------------------------------------------------------
// Store
// ---------------------------------------------------------------------------

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
        const activeTerminalId = state.cliBindingBySessionId[sessionId]?.terminalId ?? ''
        if (pending.terminalId && activeTerminalId && pending.terminalId !== activeTerminalId) {
          continue
        }

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
        let lastMessage = existingMessages[existingMessages.length - 1]
        const currentAssistantIndex = state.acpBindingBySessionId[chatId]?.turnAssistantMessageIndex ?? -1
        const lastMessageIsCurrentAssistant = currentAssistantIndex === existingMessages.length - 1

        if (lastMessage?.role === 'assistant' && !lastMessage.isStreaming && !state.acpBindingBySessionId[chatId]?.processing) continue

        if (!lastMessage || lastMessage.role !== 'assistant' || (!lastMessage.isStreaming && !lastMessageIsCurrentAssistant)) {
          const placeholder: Message = {
            id: `stream-${chatId}-${Date.now()}`,
            sessionId: chatId,
            role: 'assistant',
            content: '',
            providerId: state.sessions.find((session) => session.id === chatId)?.providerId,
            thoughts: '',
            planSummary: '',
            planEntries: [],
            toolCalls: [],
            blocks: [],
            attachments: [],
            createdAt: new Date(),
            isStreaming: true,
          }
          lastMessage = placeholder
          const updatedMessages = [...existingMessages, placeholder]
          if (nextMessages === state.messages) {
            nextMessages = { ...state.messages }
          }
          nextMessages[chatId] = updatedMessages
        }

        const updatedMessages = [...(nextMessages[chatId] ?? existingMessages)]
        updatedMessages[updatedMessages.length - 1] = {
          ...lastMessage,
          content: (lastMessage as Message).content + token,
          isStreaming: true,
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
            resourceCollections: current.resourceCollections,
            messages: current.messages,
            providers: current.providers,
            activeSessionId: current.activeSessionId,
            cliTranscriptBySessionId: current.cliTranscriptBySessionId,
            cliBindingBySessionId: current.cliBindingBySessionId,
            acpBindingBySessionId: current.acpBindingBySessionId,
            providerModelCatalogs: current.providerModelCatalogs,
            cliDebugState: current.cliDebugState,
            memoryEnabledDefault: current.memoryEnabledDefault,
            memoryLevelDefault: current.memoryLevelDefault,
            memoryIdleDelaySeconds: current.memoryIdleDelaySeconds,
            memoryRecallBudgetBytes: current.memoryRecallBudgetBytes,
            goalMaxLoopIterations: current.goalMaxLoopIterations,
            acpSetupInactivityTimeoutSeconds: current.acpSetupInactivityTimeoutSeconds,
            acpTurnOutputLimitMiB: current.acpTurnOutputLimitMiB,
            appVersion: current.appVersion,
            runnerProtocolVersion: current.runnerProtocolVersion,
            showProviderIconsInSidebar: current.showProviderIconsInSidebar,
            showWorktreePathInSidebar: current.showWorktreePathInSidebar,
            updateChecksEnabled: current.updateChecksEnabled,
            updateLastCheckedAt: current.updateLastCheckedAt,
            dismissedUpdateVersions: current.dismissedUpdateVersions,
            memoryLastStatus: current.memoryLastStatus,
            memoryWorkerBindings: current.memoryWorkerBindings,
            permissionReviewerProviderId: current.permissionReviewerProviderId,
            permissionReviewerModelId: current.permissionReviewerModelId,
            memoryActivity: current.memoryActivity,
            cliVersionManager: current.cliVersionManager,
            markdownStoreDirectory: current.markdownStoreDirectory,
            defaultNewChatProviderId: current.defaultNewChatProviderId,
            providerChatDefaults: current.providerChatDefaults,
            defaultEditorPresetId: current.defaultEditorPresetId,
            editorFileAssociations: current.editorFileAssociations,
            mcpServers: current.mcpServers,
            executionHosts: current.executionHosts,
            favoriteUamAgentIds: current.favoriteUamAgentIds,
            uamAgentCycleShortcut: current.uamAgentCycleShortcut,
            uamAgentsBySessionId: current.uamAgentsBySessionId,
            shellActions: current.shellActions,
            shellActionNotification: current.shellActionNotification,
            statusLine: current.statusLine,
          })
          set(deserialized)
          // Sync theme to DOM
          if (deserialized.theme) {
            persistTheme(deserialized.theme, get().customThemes)
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
            const current = get()
            const activeChatId = current.activeSessionId
            const activeBindingBefore = activeChatId ? current.acpBindingBySessionId[activeChatId] : undefined
            const next = applyStatePatch(msg.data, current)
            if (next) {
              set(next)
              if (next.theme) {
                persistTheme(next.theme, get().customThemes)
              }
              const activeBindingAfter = activeChatId ? get().acpBindingBySessionId[activeChatId] : undefined
              const completedActiveTurn = Boolean(activeBindingBefore?.processing && !activeBindingAfter?.processing)
              const advancedContinuousTurn = Boolean(
                activeBindingBefore?.processing &&
                activeBindingAfter?.processing &&
                (activeBindingAfter.turnSerial ?? 0) > (activeBindingBefore.turnSerial ?? 0)
              )
              if (
                activeChatId &&
                activeChatId === get().activeSessionId &&
                (completedActiveTurn || advancedContinuousTurn)
              ) {
                get().loadSessionMessages(activeChatId, true)
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
        case 'dictation':
          window.dispatchEvent(new CustomEvent('uam-dictation', { detail: msg }))
          break
        }
      }
    }

  const inCef = isCefContext()

  return {
    ...createSessionsSlice(set, get, inCef),
    ...createFoldersSlice(set, get),
    ...createResourceCollectionsSlice(set, get),
    ...createGoalsSlice(set, get),
    ...createUiSlice(set, get, inCef),

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
        resourceCollections: current.resourceCollections,
        messages: current.messages,
        providers: current.providers,
        activeSessionId: current.activeSessionId,
        cliTranscriptBySessionId: current.cliTranscriptBySessionId,
        cliBindingBySessionId: current.cliBindingBySessionId,
        acpBindingBySessionId: current.acpBindingBySessionId,
        providerModelCatalogs: current.providerModelCatalogs,
        cliDebugState: current.cliDebugState,
        memoryEnabledDefault: current.memoryEnabledDefault,
        memoryLevelDefault: current.memoryLevelDefault,
        memoryIdleDelaySeconds: current.memoryIdleDelaySeconds,
        memoryRecallBudgetBytes: current.memoryRecallBudgetBytes,
        goalMaxLoopIterations: current.goalMaxLoopIterations,
        acpSetupInactivityTimeoutSeconds: current.acpSetupInactivityTimeoutSeconds,
        acpTurnOutputLimitMiB: current.acpTurnOutputLimitMiB,
        appVersion: current.appVersion,
        runnerProtocolVersion: current.runnerProtocolVersion,
        showProviderIconsInSidebar: current.showProviderIconsInSidebar,
        showWorktreePathInSidebar: current.showWorktreePathInSidebar,
        updateChecksEnabled: current.updateChecksEnabled,
        updateLastCheckedAt: current.updateLastCheckedAt,
        dismissedUpdateVersions: current.dismissedUpdateVersions,
        memoryLastStatus: current.memoryLastStatus,
        memoryWorkerBindings: current.memoryWorkerBindings,
        permissionReviewerProviderId: current.permissionReviewerProviderId,
        permissionReviewerModelId: current.permissionReviewerModelId,
        memoryActivity: current.memoryActivity,
        cliVersionManager: current.cliVersionManager,
        markdownStoreDirectory: current.markdownStoreDirectory,
        defaultNewChatProviderId: current.defaultNewChatProviderId,
        providerChatDefaults: current.providerChatDefaults,
        defaultEditorPresetId: current.defaultEditorPresetId,
        editorFileAssociations: current.editorFileAssociations,
        mcpServers: current.mcpServers,
        executionHosts: current.executionHosts,
        favoriteUamAgentIds: current.favoriteUamAgentIds,
        uamAgentCycleShortcut: current.uamAgentCycleShortcut,
        uamAgentsBySessionId: current.uamAgentsBySessionId,
        shellActions: current.shellActions,
        shellActionNotification: current.shellActionNotification,
        statusLine: current.statusLine,
      })
      set(deserialized)
      if (deserialized.theme) {
        persistTheme(deserialized.theme, get().customThemes)
      }
    },
  }
})
