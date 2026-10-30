import { EMPTY_STARTUP_NATIVE_IMPORT_STATUS } from '../types/startupNativeImports'
import { createExecutionRoutingSlice } from './slices/executionRoutingSlice'
import { DEFAULT_CENTRAL_PROVIDER_CONFIGURATION, type CentralProviderConfiguration } from './cpp/types'
import { create } from 'zustand'
import { Session, Folder } from '../types/session'
import { Message } from '../types/message'
import { Provider } from '../types/provider'
import type { MemoryLevel } from '../types/memory'
import { sendToCEF, isCefContext, isCompanionContext } from '../ipc/cefBridge'
import {
  CLI_TRANSCRIPT_FLUSH_DELAY_MS,
  STREAM_TOKEN_FLUSH_DELAY_MS,
  pendingCliTranscriptChunksBySessionId,
  pendingStreamTokensByChatId,
  pushFlushTimers,
  discardPendingPushesForChats,
} from './push/pushBuffers'
import {
  CLAUDE_CLI_PROVIDER_ID,
  CODEX_CLI_PROVIDER_ID,
  COPILOT_CLI_PROVIDER_ID,
  DEFAULT_PROVIDER_ID,
  GEMINI_CLI_PROVIDER_ID,
  OPENCODE_CLI_PROVIDER_ID,
  SUPPORTED_CLI_PROVIDER_IDS,
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
import type { WindowsPowerShellPreference, VoiceInputSettings, VcsStorageSettings, ProviderAvailabilityOverride, AcpBinding, CliBinding, CliTranscript, CliVersionManager, CppAppState, CppCliDebugState, CppStatePatch, EditorFileAssociation, MemoryActivity, MemoryWorkerBinding, McpServerConfiguration, ProviderChatDefaults, ProviderModelCatalog, ShellAction } from './cpp/types'
import { acpBindingFromCppChat, applyPendingCodexOptions, appendCliTranscriptChunk, cliBindingFromCppChat, cppPatchRevision, cppStateRevision, decodeCliChunk, folderFromCppFolder, normalizeCliTranscript, cliOutputMatchesBinding, providerFromCppProvider, reconcileCppMessages, reconcileHeldOutboxHistory, queuedPromptsEquivalent, sameArrayEntries, sameRecordEntries, sessionFromCppChat } from './cpp/reconcile'
import { pendingProviderChatDefaults, withoutDeletedKeys } from './slices/sessionsSlice'
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
].filter((provider) => SUPPORTED_CLI_PROVIDER_IDS.includes(provider.id))

let lastPushStatusUpdateAtMs = 0

function mergeAssistantText(durable: string, streamed: string): string {
  if (!streamed || durable.endsWith(streamed)) return durable
  if (!durable || streamed.startsWith(durable)) return streamed

  for (let overlap = Math.min(durable.length, streamed.length); overlap > 0; overlap -= 1) {
    if (durable.endsWith(streamed.slice(0, overlap))) {
      return durable + streamed.slice(overlap)
    }
  }
  return durable + streamed
}

function deserializeState(
  cpp: CppAppState,
  existing: VoiceInputSettings & VcsStorageSettings & {
    providerAvailability?: ProviderAvailabilityOverride[]
    sessions: Session[]
    folders: Folder[]
    resourceCollections: ResourceCollection[]
    messages: Record<string, Message[]>
    historyStartIndexBySessionId: AppState['historyStartIndexBySessionId']
    chatSearchNavigation: AppState['chatSearchNavigation']
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
    gitEnabled?: boolean
    svnEnabled?: boolean
    updateChecksEnabled: boolean
  automaticProviderUpdates: boolean
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
    fileExplorerApplication: string
    windowsPowerShell: WindowsPowerShellPreference
    defaultEditorPresetId: string
    editorFileAssociations: EditorFileAssociation[]
    centralProviderConfiguration: CentralProviderConfiguration
    mcpServers: McpServerConfiguration[]
    executionRouting: AppState['executionRouting']
    executionHosts: AppState['executionHosts']
    favoriteUamAgentIds: string[]
    nativeImportExcludedTitlePrefixes: string[]
    nativeImportExcludedDirectories: string[]
    uamAgentCycleShortcut: AppState['uamAgentCycleShortcut']
    computerUseEnabled?: boolean
    computerUseAllowlistEnabled: boolean
    computerUseAllowedApplications: AppState['computerUseAllowedApplications']
    branchSwitching?: boolean
    hostScopedSettings?: boolean
    hostScopedSettingsValues: AppState['hostScopedSettingsValues']
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
  const historyStartIndexBySessionId = { ...existing.historyStartIndexBySessionId }
  for (const chat of cpp.chats) {
    const existingMessages = existing.messages[chat.id]
    if (!Array.isArray(chat.messages)) {
      const retained = reconcileHeldOutboxHistory(chat, existingMessages, historyStartIndexBySessionId[chat.id] ?? 0, existing.acpBindingBySessionId[chat.id], existingSessionsById[chat.id])
      if (retained !== undefined) nextMessages[chat.id] = retained
      else if (retained !== existingMessages) delete historyStartIndexBySessionId[chat.id]
      continue
    }

    if (existing.chatSearchNavigation?.chatId === chat.id && existingMessages) {
      nextMessages[chat.id] = existingMessages
      continue
    }
    historyStartIndexBySessionId[chat.id] = 0
    nextMessages[chat.id] = reconcileCppMessages(chat.id, existingMessages, chat.messages, true)
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
        cliBindingBySessionId[session.id]?.terminalId ?? '',
        cliBindingBySessionId[session.id]?.startupTime
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
        lastBlockerKind: cppGoal.lastBlockerKind,
        lastDiagnostic: cppGoal.lastDiagnostic,
        completedItems: cppGoal.completedItems,
        remainingItems: cppGoal.remainingItems,
        pendingContinuation: cppGoal.pendingContinuation,
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
    historyStartIndexBySessionId,
    goalsByChatId,
    activeGoalIdByChatId,
    providers,
    activeSessionId: effectiveActiveSessionId,
    lastAppliedStateRevision: cppStateRevision(cpp),
    theme: cpp.settings.theme,
    cliBindingBySessionId,
    acpBindingBySessionId,
    providerModelCatalogs: cpp.providerModelCatalogs ?? existing.providerModelCatalogs,
    startupNativeImports: cpp.startupNativeImports ?? EMPTY_STARTUP_NATIVE_IMPORT_STATUS,
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
    gitEnabled: typeof cpp.settings.gitEnabled === 'boolean' ? cpp.settings.gitEnabled : true,
    svnEnabled: typeof cpp.settings.svnEnabled === 'boolean' ? cpp.settings.svnEnabled : true,
    updateChecksEnabled: cpp.settings.updateChecksEnabled,
    automaticProviderUpdates: cpp.settings.automaticProviderUpdates,
    updateLastCheckedAt: cpp.settings.updateLastCheckedAt,
    dismissedUpdateVersions: cpp.settings.dismissedUpdateVersions,
    memoryLastStatus: cpp.settings.memoryLastStatus,
    memoryWorkerBindings: cpp.settings.memoryWorkerBindings,
    permissionReviewerProviderId: cpp.settings.permissionReviewerProviderId ?? '',
    permissionReviewerModelId: cpp.settings.permissionReviewerModelId ?? '',
    memoryActivity: cpp.memoryActivity ?? sanitizeMemoryActivity(undefined, cpp.settings.memoryLastStatus),
    cliVersionManager: cpp.cliVersionManager ?? existing.cliVersionManager,
    markdownStoreDirectory: cpp.settings.markdownStoreDirectory ?? '',
    defaultNewChatProviderId: pendingProviderChatDefaults?.defaultNewChatProviderId ?? cpp.settings.defaultNewChatProviderId ?? cpp.settings.activeProviderId ?? DEFAULT_PROVIDER_ID,
    providerChatDefaults: pendingProviderChatDefaults?.providerChatDefaults ?? cpp.settings.providerChatDefaults ?? {},
    fileExplorerApplication: cpp.settings.fileExplorerApplication ?? '',
    windowsPowerShell: cpp.settings.windowsPowerShell ?? 'auto',
    defaultEditorPresetId: cpp.settings.defaultEditorPresetId ?? 'vscode',
    editorFileAssociations: cpp.settings.editorFileAssociations ?? defaultEditorFileAssociations(),
    voiceInputMode: cpp.settings.voiceInputMode ?? existing.voiceInputMode,
    voiceInputServerBaseUrl: cpp.settings.voiceInputServerBaseUrl ?? existing.voiceInputServerBaseUrl,
    voiceInputServerEndpoint: cpp.settings.voiceInputServerEndpoint ?? existing.voiceInputServerEndpoint,
    voiceInputServerModel: cpp.settings.voiceInputServerModel ?? existing.voiceInputServerModel,
    voiceInputApiKeyEnv: cpp.settings.voiceInputApiKeyEnv ?? existing.voiceInputApiKeyEnv,
    voiceInputCapabilities: cpp.settings.voiceInputCapabilities ?? existing.voiceInputCapabilities,
    providerAvailability: cpp.settings.providerAvailability ?? existing.providerAvailability ?? [],
    gitAutoWorktree: cpp.settings.gitAutoWorktree ?? existing.gitAutoWorktree,
    gitSnapshotSvn: cpp.settings.gitSnapshotSvn ?? existing.gitSnapshotSvn,
    gitSnapshotPlainFolders: cpp.settings.gitSnapshotPlainFolders ?? existing.gitSnapshotPlainFolders,
    gitDefaultBaseBranch: cpp.settings.gitDefaultBaseBranch ?? existing.gitDefaultBaseBranch,
    gitPullBeforeChat: cpp.settings.gitPullBeforeChat ?? existing.gitPullBeforeChat,
    svnUpdateBeforeChat: cpp.settings.svnUpdateBeforeChat ?? existing.svnUpdateBeforeChat,
    gitPortMethod: cpp.settings.gitPortMethod ?? existing.gitPortMethod,
    worktreeCleanupEnabled: cpp.settings.worktreeCleanupEnabled ?? existing.worktreeCleanupEnabled,
    worktreeRetentionDays: cpp.settings.worktreeRetentionDays ?? existing.worktreeRetentionDays,
    backupLogRetentionDays: cpp.settings.backupLogRetentionDays ?? existing.backupLogRetentionDays,
    httpCacheClearIntervalDays: cpp.settings.httpCacheClearIntervalDays ?? existing.httpCacheClearIntervalDays,
    centralProviderConfiguration: cpp.settings.centralProviderConfiguration ?? DEFAULT_CENTRAL_PROVIDER_CONFIGURATION,
    mcpServers: cpp.settings.mcpServers ?? [],
    executionRouting: cpp.settings.executionRouting ?? existing.executionRouting,
    executionHosts: cpp.settings.executionHosts ?? existing.executionHosts,
    favoriteUamAgentIds: cpp.settings.favoriteUamAgentIds ?? [],
    nativeImportExcludedTitlePrefixes: cpp.settings.nativeImportExcludedTitlePrefixes ?? [],
    nativeImportExcludedDirectories: cpp.settings.nativeImportExcludedDirectories ?? [],
    uamAgentCycleShortcut: cpp.settings.uamAgentCycleShortcut ?? 'shift+tab',
    computerUseEnabled: cpp.settings.computerUseGloballyEnabled,
    computerUseRevocationPending: cpp.settings.computerUseRevocationPending,
    computerUseSettingsStateRevision: cppStateRevision(cpp),
    computerUseAllowlistEnabled: cpp.settings.computerUseAllowlistEnabled ?? false,
    computerUseAllowedApplications: cpp.settings.computerUseAllowedApplications ?? [],
    branchSwitching: cpp.settings.branchSwitching === true,
    hostScopedSettings: cpp.settings.hostScopedSettings,
    hostScopedSettingsValues: cpp.settings.hostScopedSettingsValues ?? {},
    uamAgentsBySessionId: existing.uamAgentsBySessionId,
    shellActions: cpp.shellActions ?? existing.shellActions,
    shellActionNotification: cpp.shellActionNotification ?? existing.shellActionNotification,
    statusLine: cpp.statusLine ?? existing.statusLine,
    // Full-state only: absent means no deletion is running, so do not keep the previous value.
    deletionProgress: cpp.deletionProgress,
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
    : [{ id: DEFAULT_PROVIDER_ID }]

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
  const historyStartIndexBySessionId = { ...current.historyStartIndexBySessionId }
  for (const chatId of removedChatIds) {
    delete messages[chatId]
    delete historyStartIndexBySessionId[chatId]
  }
  for (const chat of patch.chats ?? []) {
    const retained = reconcileHeldOutboxHistory(chat, messages[chat.id], historyStartIndexBySessionId[chat.id] ?? 0, current.acpBindingBySessionId[chat.id], existingSessionsById[chat.id])
    if (retained !== undefined) messages[chat.id] = retained
    else if (messages[chat.id] !== undefined) {
      delete messages[chat.id]
      delete historyStartIndexBySessionId[chat.id]
    }
  }
  for (const [chatId, cppMessages] of Object.entries(patch.messagesByChatId ?? {})) {
    if (current.chatSearchNavigation?.chatId === chatId) continue
    historyStartIndexBySessionId[chatId] = 0
    messages[chatId] = reconcileCppMessages(chatId, current.messages[chatId], cppMessages, true)
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
        nextCli.terminalId,
        nextCli.startupTime
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
    goalsByChatId[chat.id] = (chat.goals ?? []).map((cppGoal) => ({
        id: cppGoal.id,
        chatId: chat.id,
        objective: cppGoal.objective,
        status: cppGoal.status,
        tokenBudget: cppGoal.tokenBudget,
        tokensUsed: cppGoal.tokensUsed,
        blockedTurnCount: cppGoal.blockedTurnCount,
        lastBlocker: cppGoal.lastBlocker,
        lastBlockerKind: cppGoal.lastBlockerKind,
        lastDiagnostic: cppGoal.lastDiagnostic,
        completedItems: cppGoal.completedItems,
        remainingItems: cppGoal.remainingItems,
        pendingContinuation: cppGoal.pendingContinuation,
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
  const goalModeByChatId = withoutDeletedKeys(current.goalModeByChatId, removedChatIds)
  const defaultGoalTokenBudgetByChatId = withoutDeletedKeys(current.defaultGoalTokenBudgetByChatId, removedChatIds)
  const markdownStoreAttachedBySessionId = withoutDeletedKeys(current.markdownStoreAttachedBySessionId, removedChatIds)
  const repositoryReviewBySessionId = withoutDeletedKeys(current.repositoryReviewBySessionId, removedChatIds)
  const uamAgentsBySessionId = withoutDeletedKeys(current.uamAgentsBySessionId, removedChatIds)

  return {
    folders,
    resourceCollections,
    sessions: sameArrayEntries(current.sessions, sessionsWithPendingCodexOptions) ? current.sessions : sessionsWithPendingCodexOptions,
    messages: sameRecordEntries(current.messages, messages) ? current.messages : messages,
    historyStartIndexBySessionId,
    chatSearchNavigation: current.chatSearchNavigation && removedChatIds.has(current.chatSearchNavigation.chatId) ? null : current.chatSearchNavigation,
    goalsByChatId: sameRecordEntries(current.goalsByChatId, goalsByChatId) ? current.goalsByChatId : goalsByChatId,
    activeGoalIdByChatId: sameRecordEntries(current.activeGoalIdByChatId, activeGoalIdByChatId) ? current.activeGoalIdByChatId : activeGoalIdByChatId,
    goalModeByChatId: sameRecordEntries(current.goalModeByChatId, goalModeByChatId) ? current.goalModeByChatId : goalModeByChatId,
    defaultGoalTokenBudgetByChatId: sameRecordEntries(current.defaultGoalTokenBudgetByChatId, defaultGoalTokenBudgetByChatId) ? current.defaultGoalTokenBudgetByChatId : defaultGoalTokenBudgetByChatId,
    markdownStoreAttachedBySessionId: sameRecordEntries(current.markdownStoreAttachedBySessionId, markdownStoreAttachedBySessionId) ? current.markdownStoreAttachedBySessionId : markdownStoreAttachedBySessionId,
    repositoryReviewBySessionId: sameRecordEntries(current.repositoryReviewBySessionId, repositoryReviewBySessionId) ? current.repositoryReviewBySessionId : repositoryReviewBySessionId,
    providers,
    providerModelCatalogs: patch.providerModelCatalogs ?? current.providerModelCatalogs,
    startupNativeImports: patch.startupNativeImports ?? current.startupNativeImports,
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
    gitEnabled: typeof patch.settings?.gitEnabled === 'boolean' ? patch.settings.gitEnabled : current.gitEnabled,
    svnEnabled: typeof patch.settings?.svnEnabled === 'boolean' ? patch.settings.svnEnabled : current.svnEnabled,
    updateChecksEnabled: patch.settings?.updateChecksEnabled ?? current.updateChecksEnabled,
    automaticProviderUpdates: patch.settings?.automaticProviderUpdates ?? current.automaticProviderUpdates,
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
    fileExplorerApplication: patch.settings?.fileExplorerApplication ?? current.fileExplorerApplication,
    windowsPowerShell: patch.settings?.windowsPowerShell ?? current.windowsPowerShell,
    defaultEditorPresetId: patch.settings?.defaultEditorPresetId ?? current.defaultEditorPresetId,
    editorFileAssociations: patch.settings?.editorFileAssociations ?? current.editorFileAssociations,
    voiceInputMode: patch.settings?.voiceInputMode ?? current.voiceInputMode,
    voiceInputServerBaseUrl: patch.settings?.voiceInputServerBaseUrl ?? current.voiceInputServerBaseUrl,
    voiceInputServerEndpoint: patch.settings?.voiceInputServerEndpoint ?? current.voiceInputServerEndpoint,
    voiceInputServerModel: patch.settings?.voiceInputServerModel ?? current.voiceInputServerModel,
    voiceInputApiKeyEnv: patch.settings?.voiceInputApiKeyEnv ?? current.voiceInputApiKeyEnv,
    voiceInputCapabilities: patch.settings?.voiceInputCapabilities ? { ...current.voiceInputCapabilities, ...patch.settings.voiceInputCapabilities } : current.voiceInputCapabilities,
    providerAvailability: patch.settings?.providerAvailability ?? current.providerAvailability,
    gitAutoWorktree: patch.settings?.gitAutoWorktree ?? current.gitAutoWorktree,
    gitSnapshotSvn: patch.settings?.gitSnapshotSvn ?? current.gitSnapshotSvn,
    gitSnapshotPlainFolders: patch.settings?.gitSnapshotPlainFolders ?? current.gitSnapshotPlainFolders,
    gitDefaultBaseBranch: patch.settings?.gitDefaultBaseBranch ?? current.gitDefaultBaseBranch,
    gitPullBeforeChat: patch.settings?.gitPullBeforeChat ?? current.gitPullBeforeChat,
    svnUpdateBeforeChat: patch.settings?.svnUpdateBeforeChat ?? current.svnUpdateBeforeChat,
    gitPortMethod: patch.settings?.gitPortMethod ?? current.gitPortMethod,
    worktreeCleanupEnabled: patch.settings?.worktreeCleanupEnabled ?? current.worktreeCleanupEnabled,
    worktreeRetentionDays: patch.settings?.worktreeRetentionDays ?? current.worktreeRetentionDays,
    backupLogRetentionDays: patch.settings?.backupLogRetentionDays ?? current.backupLogRetentionDays,
    httpCacheClearIntervalDays: patch.settings?.httpCacheClearIntervalDays ?? current.httpCacheClearIntervalDays,
    centralProviderConfiguration: patch.settings?.centralProviderConfiguration ?? current.centralProviderConfiguration,
    mcpServers: patch.settings?.mcpServers ?? current.mcpServers,
    executionRouting: patch.settings?.executionRouting ?? current.executionRouting,
    executionHosts: patch.settings?.executionHosts ?? current.executionHosts,
    favoriteUamAgentIds: patch.settings?.favoriteUamAgentIds ?? current.favoriteUamAgentIds,
    nativeImportExcludedTitlePrefixes: patch.settings?.nativeImportExcludedTitlePrefixes ?? current.nativeImportExcludedTitlePrefixes,
    nativeImportExcludedDirectories: patch.settings?.nativeImportExcludedDirectories ?? current.nativeImportExcludedDirectories,
    uamAgentCycleShortcut: patch.settings?.uamAgentCycleShortcut ?? current.uamAgentCycleShortcut,
    computerUseEnabled: patch.settings?.computerUseGloballyEnabled ?? current.computerUseEnabled,
    computerUseRevocationPending: patch.settings?.computerUseRevocationPending ?? current.computerUseRevocationPending,
    computerUseSettingsStateRevision: typeof patch.settings?.computerUseGloballyEnabled === 'boolean' || typeof patch.settings?.computerUseRevocationPending === 'boolean'
      ? nextRevision : current.computerUseSettingsStateRevision,
    computerUseAllowlistEnabled: patch.settings?.computerUseAllowlistEnabled ?? current.computerUseAllowlistEnabled,
    computerUseAllowedApplications: patch.settings?.computerUseAllowedApplications ?? current.computerUseAllowedApplications,
    branchSwitching: patch.settings?.branchSwitching ?? current.branchSwitching,
    hostScopedSettings: patch.settings?.hostScopedSettings ?? current.hostScopedSettings,
    hostScopedSettingsValues: patch.settings?.hostScopedSettingsValues ?? current.hostScopedSettingsValues,
    uamAgentsBySessionId: sameRecordEntries(current.uamAgentsBySessionId, uamAgentsBySessionId) ? current.uamAgentsBySessionId : uamAgentsBySessionId,
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
      const liveSessionIds = new Set(state.sessions.map((session) => session.id))

      for (const [sessionId, pending] of entries) {
        if (!liveSessionIds.has(sessionId)) continue
        const chunk = pending.chunks.join('')
        if (!chunk) continue
        if (!cliOutputMatchesBinding(state.cliBindingBySessionId[sessionId], pending.terminalId, pending.startupTime)) {
          continue
        }

        if (nextTranscripts === state.cliTranscriptBySessionId) {
          nextTranscripts = { ...state.cliTranscriptBySessionId }
        }

        nextTranscripts[sessionId] = appendCliTranscriptChunk(
          nextTranscripts[sessionId],
          pending.terminalId,
          chunk,
          pending.startupTime
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
      const liveSessionIds = new Set(state.sessions.map((session) => session.id))

      for (const [chatId, pending] of entries) {
        if (!liveSessionIds.has(chatId)) continue
        if (state.chatSearchNavigation?.chatId === chatId) continue
        const existingMessages = nextMessages[chatId] ?? []
        let lastMessage = existingMessages[existingMessages.length - 1]
        const token = pending.token
        const currentAssistantIndex = pending.messageIndex ?? state.acpBindingBySessionId[chatId]?.turnAssistantMessageIndex ?? -1
        const historyStartIndex = state.historyStartIndexBySessionId[chatId] ?? 0
        const lastAbsoluteIndex = historyStartIndex + existingMessages.length - 1
        // A bounded historical page cannot absorb tokens from another part of the transcript.
        if (historyStartIndex > 0 && (currentAssistantIndex < lastAbsoluteIndex || currentAssistantIndex > lastAbsoluteIndex + 1 ||
            (currentAssistantIndex === lastAbsoluteIndex + 1 && lastAbsoluteIndex + 1 < (state.sessions.find((session) => session.id === chatId)?.messageCount ?? 0)))) continue
        const lastMessageIsCurrentAssistant = lastMessage?.streamMessageIndex === undefined
          ? currentAssistantIndex === lastAbsoluteIndex
          : lastMessage.streamMessageIndex === currentAssistantIndex
        const lastMessageIsPlaceholder = lastMessage?.isStreaming && lastMessage.id.startsWith(`stream-${chatId}-`) &&
          (pending.messageIndex === undefined || lastMessage.streamMessageIndex === pending.messageIndex)

        if (lastMessage?.role === 'assistant' && !lastMessage.isStreaming && !state.acpBindingBySessionId[chatId]?.processing) continue

        if (!lastMessage || lastMessage.role !== 'assistant' || (!lastMessageIsPlaceholder && !lastMessageIsCurrentAssistant)) {
          const placeholder: Message = {
            id: `stream-${chatId}-${Date.now()}-${existingMessages.length}`,
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
            streamMessageIndex: pending.messageIndex,
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

  // CompanionShell bootstraps after authentication and preserves phone-local selection.
  // Desktop state arrives via uamPush later too.
  if (isCefContext() && !isCompanionContext()) {
    sendToCEF<CppAppState>({ action: 'getInitialState', payload: { summaryOnly: true } }).then((resp) => {
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
            historyStartIndexBySessionId: current.historyStartIndexBySessionId,
            chatSearchNavigation: current.chatSearchNavigation,
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
        gitEnabled: current.gitEnabled,
        svnEnabled: current.svnEnabled,
            updateChecksEnabled: current.updateChecksEnabled,
            automaticProviderUpdates: current.automaticProviderUpdates,
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
            voiceInputMode: current.voiceInputMode,
            voiceInputServerBaseUrl: current.voiceInputServerBaseUrl,
            voiceInputServerEndpoint: current.voiceInputServerEndpoint,
            voiceInputServerModel: current.voiceInputServerModel,
            voiceInputApiKeyEnv: current.voiceInputApiKeyEnv,
            voiceInputCapabilities: current.voiceInputCapabilities,
            providerAvailability: current.providerAvailability,
            gitAutoWorktree: current.gitAutoWorktree,
            gitSnapshotSvn: current.gitSnapshotSvn,
            gitSnapshotPlainFolders: current.gitSnapshotPlainFolders,
            gitDefaultBaseBranch: current.gitDefaultBaseBranch,
            gitPullBeforeChat: current.gitPullBeforeChat,
            svnUpdateBeforeChat: current.svnUpdateBeforeChat,
            gitPortMethod: current.gitPortMethod,
            worktreeCleanupEnabled: current.worktreeCleanupEnabled,
            worktreeRetentionDays: current.worktreeRetentionDays,
            backupLogRetentionDays: current.backupLogRetentionDays,
            httpCacheClearIntervalDays: current.httpCacheClearIntervalDays,
            fileExplorerApplication: current.fileExplorerApplication,
            windowsPowerShell: current.windowsPowerShell,
        defaultEditorPresetId: current.defaultEditorPresetId,
            editorFileAssociations: current.editorFileAssociations,
            centralProviderConfiguration: current.centralProviderConfiguration,
            mcpServers: current.mcpServers,
            executionRouting: current.executionRouting,
            executionHosts: current.executionHosts,
            favoriteUamAgentIds: current.favoriteUamAgentIds,
            nativeImportExcludedTitlePrefixes: current.nativeImportExcludedTitlePrefixes,
            nativeImportExcludedDirectories: current.nativeImportExcludedDirectories,
            uamAgentCycleShortcut: current.uamAgentCycleShortcut,
            computerUseEnabled: current.computerUseEnabled,
            computerUseAllowlistEnabled: current.computerUseAllowlistEnabled,
            computerUseAllowedApplications: current.computerUseAllowedApplications,
            branchSwitching: current.branchSwitching,
            hostScopedSettings: current.hostScopedSettings,
            hostScopedSettingsValues: current.hostScopedSettingsValues,
            uamAgentsBySessionId: current.uamAgentsBySessionId,
            shellActions: current.shellActions,
            shellActionNotification: current.shellActionNotification,
            statusLine: current.statusLine,
          })
          set(current.reconcileMemorySettingsProjection(deserialized, sanitized.settings))
          // Sync theme to DOM
          if (deserialized.theme) {
            persistTheme(deserialized.theme, get().customThemes)
          }
        } else if (
          (sanitized.appVersion && sanitized.appVersion !== current.appVersion) ||
          (sanitized.runnerProtocolVersion && sanitized.runnerProtocolVersion !== current.runnerProtocolVersion)
        ) {
          // A state patch can overtake bootstrap; its revision must win, but it lacks runtime metadata.
          set({
            appVersion: sanitized.appVersion || current.appVersion,
            runnerProtocolVersion: sanitized.runnerProtocolVersion || current.runnerProtocolVersion,
          })
        }
      }
    })
  }

  // Register window.uamPush SYNCHRONOUSLY at module load time (before React mounts)
  // so that C++'s OnLoadEnd push is never missed.  Using getState() avoids stale closures.
  if (isCefContext() && typeof window !== 'undefined') {
    const handlePush = (payload: unknown) => {
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
          if (!isNewerStateRevision(cppStateRevision(msg.data), store.lastAppliedStateRevision)) break
          // Full state only hydrates the selected transcript. Keep buffered
          // background deltas unless this update contains that chat's messages.
          for (const chatId of pendingStreamTokensByChatId.keys()) {
            const chat = msg.data.chats.find((candidate) => candidate.id === chatId)
            if (!chat || Array.isArray(chat.messages)) {
              pendingStreamTokensByChatId.delete(chatId)
            }
          }
          store.loadFromCef(msg.data)
          break
        case 'statePatch':
          {
            if (!isNewerStateRevision(cppPatchRevision(msg.data), store.lastAppliedStateRevision)) break
            // A patch that replaces a chat's messages carries the streamed text
            // authoritatively; buffered deltas for those chats must not be
            // re-applied on top.
            if (msg.data && typeof msg.data === 'object' && msg.data.messagesByChatId && typeof msg.data.messagesByChatId === 'object') {
              for (const chatId of Object.keys(msg.data.messagesByChatId)) {
                pendingStreamTokensByChatId.delete(chatId)
              }
            }
            discardPendingPushesForChats(msg.data.removedChatIds ?? [])
            const current = get()
            const activeChatId = current.activeSessionId
            const activeBindingBefore = activeChatId ? current.acpBindingBySessionId[activeChatId] : undefined
            const next = applyStatePatch(msg.data, current)
            if (next) {
              set(current.reconcileMemorySettingsProjection(next, msg.data.settings))
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
              const appendedTurnMessages = Boolean(
                activeBindingBefore?.processing && activeBindingAfter?.processing &&
                (get().sessions.find((session) => session.id === activeChatId)?.messageCount ?? 0) >
                  (current.sessions.find((session) => session.id === activeChatId)?.messageCount ?? 0)
              )
              const activeSessionBefore = current.sessions.find((session) => session.id === activeChatId)
              const activeSessionAfter = get().sessions.find((session) => session.id === activeChatId)
              // External history imports publish summaries. Refresh an already loaded idle
              // transcript when its count or content digest changes, just as turn completion does.
              const refreshedIdleHistory = Boolean(
                activeChatId && current.messages[activeChatId] !== undefined &&
                activeSessionBefore && activeSessionAfter &&
                !activeBindingBefore?.processing && !activeBindingAfter?.processing &&
                !current.cliBindingBySessionId[activeChatId]?.processing &&
                !get().cliBindingBySessionId[activeChatId]?.processing &&
                !current.messages[activeChatId].some((message) => message.isStreaming) &&
                !msg.data.messagesByChatId?.[activeChatId] &&
                ((activeSessionBefore.messageCount ?? 0) !== (activeSessionAfter.messageCount ?? 0) ||
                  (activeSessionBefore.messagesDigest ?? '') !== (activeSessionAfter.messagesDigest ?? ''))
              )
              const newlyHeldOutboxMismatch = Boolean(activeChatId && activeBindingAfter?.queueHeld &&
                activeBindingAfter.queuedPrompts?.length &&
                (!activeBindingBefore?.queueHeld || !queuedPromptsEquivalent(activeBindingBefore.queuedPrompts, activeBindingAfter.queuedPrompts)) &&
                Number.isSafeInteger(activeSessionAfter?.messageCount) && activeSessionAfter?.messagesDigest &&
                (get().historyStartIndexBySessionId[activeChatId] ?? 0) + (get().messages[activeChatId]?.length ?? 0) > activeSessionAfter!.messageCount!)
              if (
                activeChatId &&
                activeChatId === get().activeSessionId &&
                (completedActiveTurn || advancedContinuousTurn || appendedTurnMessages || refreshedIdleHistory || newlyHeldOutboxMismatch)
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
              if (!cliOutputMatchesBinding(currentBinding, terminalId, msg.startupTime)) break
              const boundChatId = msg.sourceChatId ?? currentBinding?.boundChatId ?? sessionId
              const existingPending = pendingCliTranscriptChunksBySessionId.get(sessionId)
              if (existingPending && existingPending.terminalId === terminalId && existingPending.startupTime === msg.startupTime) {
                existingPending.chunks.push(decodedData)
              } else {
                pendingCliTranscriptChunksBySessionId.set(sessionId, {
                  terminalId,
                  startupTime: msg.startupTime,
                  chunks: [decodedData],
                })
              }
              scheduleCliTranscriptFlush()
            }

            window.dispatchEvent(
              new CustomEvent('uam-cli-output', {
                detail: {
                  sessionId: msg.sessionId ?? '',
                  sourceChatId: msg.sourceChatId ?? '',
                  terminalId: msg.terminalId ?? '',
                  startupTime: msg.startupTime,
                  data: decodedData,
                },
              })
            )
          }
          break
        case 'streamToken':
          {
            const { chatId, token, messageIndex } = msg
            const session = get().sessions.find((s) => s.id === chatId)
            if (!session) break

            const pending = pendingStreamTokensByChatId.get(chatId)
            if (pending && pending.messageIndex !== messageIndex) {
              if (pushFlushTimers.streamToken !== null) window.clearTimeout(pushFlushTimers.streamToken)
              flushPendingStreamTokens()
            }
            pendingStreamTokensByChatId.set(chatId, { token: (pendingStreamTokensByChatId.get(chatId)?.token ?? '') + token, messageIndex })
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
              const binding = get().acpBindingBySessionId[chatId]
              const currentAssistantIndex = binding?.turnAssistantMessageIndex ?? -1
              const currentAssistant = existingMessages[currentAssistantIndex]
              const turnAssistantText = (binding?.turnEvents ?? []).reduce(
                (text, event) => event.type === 'assistant_text' ? text + event.text : text,
                ''
              )
              const duplicatePlaceholder = currentAssistantIndex === existingMessages.length - 2 &&
                currentAssistant?.role === 'assistant' &&
                (currentAssistant.content === lastMessage.content || turnAssistantText.length > 0)
              const updatedMessages = duplicatePlaceholder ? existingMessages.slice(0, -1) : [...existingMessages]
              if (duplicatePlaceholder) {
                updatedMessages[currentAssistantIndex] = {
                  ...currentAssistant,
                  content: mergeAssistantText(
                    currentAssistant.content,
                    turnAssistantText || lastMessage.content
                  ),
                  isStreaming: false,
                }
              } else {
                updatedMessages[updatedMessages.length - 1] = {
                  ...lastMessage,
                  isStreaming: false,
                }
              }
              set((state) => ({
                messages: {
                  ...state.messages,
                  [chatId]: updatedMessages,
                },
              }))
            }
            // Completion settles the final token mutation; hydrate durable structure afterwards.
            if (get().messages[chatId] !== undefined) void get().loadSessionMessages(chatId, true)
          }
          break
        case 'dictation':
          window.dispatchEvent(new CustomEvent('uam-dictation', { detail: msg }))
          break
      }
    }
    // A failing handler must not throw into native code; report it and let the next push recover the status.
    window.uamPush = (payload: unknown) => {
      try {
        handlePush(payload)
      } catch (error) {
        set({ pushChannelStatus: 'invalid-message', pushChannelError: `Push handling failed: ${error instanceof Error ? error.message : String(error)}` })
        console.error('[uamPush] Handler failed:', error)
      }
    }
  }

  const inCef = isCefContext()

  return {
    startupNativeImports: EMPTY_STARTUP_NATIVE_IMPORT_STATUS,
    startupNativeImportReviewSequence: 0,
    startupNativeImportFolderId: null,
    openStartupNativeImportReview: (folderId) => set((state) => ({ startupNativeImportReviewSequence: state.startupNativeImportReviewSequence + 1, startupNativeImportFolderId: folderId ?? null })),
    computerUseAllowlistEnabled: false,
    computerUseAllowedApplications: [],
    ...createSessionsSlice(set, get, inCef),
    ...createExecutionRoutingSlice(get),
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
        historyStartIndexBySessionId: current.historyStartIndexBySessionId,
        chatSearchNavigation: current.chatSearchNavigation,
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
        gitEnabled: current.gitEnabled,
        svnEnabled: current.svnEnabled,
        updateChecksEnabled: current.updateChecksEnabled,
            automaticProviderUpdates: current.automaticProviderUpdates,
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
        voiceInputMode: current.voiceInputMode,
        voiceInputServerBaseUrl: current.voiceInputServerBaseUrl,
        voiceInputServerEndpoint: current.voiceInputServerEndpoint,
        voiceInputServerModel: current.voiceInputServerModel,
        voiceInputApiKeyEnv: current.voiceInputApiKeyEnv,
        voiceInputCapabilities: current.voiceInputCapabilities,
        providerAvailability: current.providerAvailability,
        gitAutoWorktree: current.gitAutoWorktree,
        gitSnapshotSvn: current.gitSnapshotSvn,
        gitSnapshotPlainFolders: current.gitSnapshotPlainFolders,
        gitDefaultBaseBranch: current.gitDefaultBaseBranch,
        gitPullBeforeChat: current.gitPullBeforeChat,
        svnUpdateBeforeChat: current.svnUpdateBeforeChat,
        gitPortMethod: current.gitPortMethod,
        worktreeCleanupEnabled: current.worktreeCleanupEnabled,
        worktreeRetentionDays: current.worktreeRetentionDays,
        backupLogRetentionDays: current.backupLogRetentionDays,
        httpCacheClearIntervalDays: current.httpCacheClearIntervalDays,
        fileExplorerApplication: current.fileExplorerApplication,
        windowsPowerShell: current.windowsPowerShell,
        defaultEditorPresetId: current.defaultEditorPresetId,
        editorFileAssociations: current.editorFileAssociations,
        centralProviderConfiguration: current.centralProviderConfiguration,
            mcpServers: current.mcpServers,
        executionRouting: current.executionRouting,
        executionHosts: current.executionHosts,
        favoriteUamAgentIds: current.favoriteUamAgentIds,
        nativeImportExcludedTitlePrefixes: current.nativeImportExcludedTitlePrefixes,
        nativeImportExcludedDirectories: current.nativeImportExcludedDirectories,
        uamAgentCycleShortcut: current.uamAgentCycleShortcut,
        computerUseEnabled: current.computerUseEnabled,
        computerUseAllowlistEnabled: current.computerUseAllowlistEnabled,
        computerUseAllowedApplications: current.computerUseAllowedApplications,
        branchSwitching: current.branchSwitching,
        hostScopedSettings: current.hostScopedSettings,
        hostScopedSettingsValues: current.hostScopedSettingsValues,
        uamAgentsBySessionId: current.uamAgentsBySessionId,
        shellActions: current.shellActions,
        shellActionNotification: current.shellActionNotification,
        statusLine: current.statusLine,
      })
      set(current.reconcileMemorySettingsProjection(deserialized, sanitized.settings))
      if (deserialized.theme) {
        persistTheme(deserialized.theme, get().customThemes)
      }
    },
  }
})
