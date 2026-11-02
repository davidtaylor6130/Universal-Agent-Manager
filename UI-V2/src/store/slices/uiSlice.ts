import { sendToCEF, isCefContext, isCompanionContext, createRequestId } from '../../ipc/cefBridge'
import {
  applyDocumentTheme,
  normalizeCustomTheme,
  normalizeStoredTheme,
  readStoredTheme,
  writeStoredTheme,
  type StoredTheme,
  type CustomTheme,
} from '../../utils/themeStorage'
import { pendingRequestIdsByKey } from '../push/pushBuffers'
import {
  clearPendingRequest,
  isLatestPendingRequest,
  latestOptimisticRollback,
  rememberOptimisticFields,
  rememberPendingRequest,
} from '../cpp/reconcile'
import { sanitizeVcsStorageSettings, sanitizeProviderAvailability, sanitizeVoiceInputSettings } from '../cpp/sanitizers'
import type { NewChatWorktreeContext, VcsSettingsRequest, StorageSettingsRequest, ProviderAvailabilityRequest, VoiceInputSettingsRequest } from '../cpp/types'
import type { AppState, ZustandSet, ZustandGet } from '../storeTypes'

export function readDocumentTheme(): StoredTheme {
  const stored = readStoredTheme()
  if (stored) return stored
  if (typeof document === 'undefined') return 'focus'
  return normalizeStoredTheme(document.documentElement.getAttribute('data-theme')) ?? 'focus'
}

export function persistTheme(theme: StoredTheme, customThemes: CustomTheme[] = []): void {
  applyDocumentTheme(theme, customThemes)
  writeStoredTheme(theme)
}

const appShellLayoutStorageKey = 'uam-app-shell-layout-v2'
const legacyAppShellLayoutStorageKey = 'uam-app-shell-layout-v1'
const defaultAppShellLayout = {
  sidebarCollapsed: false,
  commitPanelOpen: false,
  sidebarWidthPx: 320,
  commitPanelWidthPx: 420,
  workingDisplayMode: 'compact' as const,
  expandWorkTraces: true,
  collapsibleWorkSections: true,
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
        workingDisplayMode: parsed.workingDisplayMode === 'verbose' ? 'verbose' as const : 'compact' as const,
        expandWorkTraces: parsed.expandWorkTraces !== false,
        collapsibleWorkSections: parsed.collapsibleWorkSections !== false,
      }
    }

    const legacyRaw = window.localStorage.getItem(legacyAppShellLayoutStorageKey)
    const parsed = legacyRaw ? JSON.parse(legacyRaw) : {}
    return {
      sidebarCollapsed: Boolean(parsed.sidebarCollapsed),
      commitPanelOpen: Boolean(parsed.commitPanelOpen),
      sidebarWidthPx: defaultAppShellLayout.sidebarWidthPx,
      commitPanelWidthPx: legacyCommitPanelPercentToPx(parsed.commitPanelWidth),
      workingDisplayMode: defaultAppShellLayout.workingDisplayMode,
      expandWorkTraces: defaultAppShellLayout.expandWorkTraces,
      collapsibleWorkSections: defaultAppShellLayout.collapsibleWorkSections,
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
  workingDisplayMode: 'compact' | 'verbose'
  expandWorkTraces: boolean
  collapsibleWorkSections: boolean
}) {
  if (typeof window === 'undefined') return
  try {
    window.localStorage.setItem(appShellLayoutStorageKey, JSON.stringify({
      sidebarCollapsed: layout.sidebarCollapsed,
      commitPanelOpen: layout.commitPanelOpen,
      sidebarWidthPx: clampSidebarWidthPx(layout.sidebarWidthPx),
      commitPanelWidthPx: clampCommitPanelWidthPx(layout.commitPanelWidthPx),
      workingDisplayMode: layout.workingDisplayMode,
      expandWorkTraces: layout.expandWorkTraces,
      collapsibleWorkSections: layout.collapsibleWorkSections,
    }))
  } catch {
    // Ignore storage failures; the in-memory store still tracks the layout.
  }
}

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

export function createUiSlice(set: ZustandSet, get: ZustandGet, inCef: boolean) {
  const storedShellLayout = readStoredAppShellLayout()
  let confirmedTheme = readDocumentTheme()
  let themeRequestSerial = 0
  let confirmedThemeSerial = 0

  return {
    theme: readDocumentTheme(),
    customThemes: [] as CustomTheme[],
    workingDisplayMode: storedShellLayout.workingDisplayMode,
    expandWorkTraces: storedShellLayout.expandWorkTraces,
    collapsibleWorkSections: storedShellLayout.collapsibleWorkSections,
    showProviderIconsInSidebar: true,
    showWorktreePathInSidebar: true,
    nativeImportExcludedTitlePrefixes: [] as string[],
    nativeImportExcludedDirectories: [] as string[],
    gitEnabled: true,
    svnEnabled: true,
    isNewChatModalOpen: false,
    newChatFolderId: null as string | null,
    newChatWorktreeContext: null as NewChatWorktreeContext | null,
    isSettingsOpen: false,
    settingsScopeHostId: '*',
    sidebarCollapsed: storedShellLayout.sidebarCollapsed,
    commitPanelOpen: storedShellLayout.commitPanelOpen,
    sidebarWidthPx: storedShellLayout.sidebarWidthPx,
    commitPanelWidthPx: storedShellLayout.commitPanelWidthPx,
    streamingMessageId: null as string | null,
    pushChannelStatus: (inCef ? 'no-push-yet' : 'connected') as AppState['pushChannelStatus'],
    pushChannelError: '',
    lastPushAtMs: null as number | null,
    uiBuildId: UI_RUNTIME_BUILD_MARKER,
    repositoryReviewBySessionId: {} as AppState['repositoryReviewBySessionId'],

    setTheme: (theme: StoredTheme) => {
      const requestKey = 'setTheme'
      // Roll back only to the newest theme native code accepted, never to an earlier pending or rejected one.
      if (!pendingRequestIdsByKey.has(requestKey)) { confirmedTheme = get().theme; confirmedThemeSerial = themeRequestSerial }
      persistTheme(theme, get().customThemes)
      set({ theme })
      if (isCefContext() && !isCompanionContext()) {
        const serial = ++themeRequestSerial
        const requestId = createRequestId('setTheme')
        rememberPendingRequest(requestKey, requestId)
        sendToCEF({ action: 'setTheme', payload: { theme }, requestId }).then((resp) => {
          if (resp.ok) {
            if (serial > confirmedThemeSerial) { confirmedThemeSerial = serial; confirmedTheme = theme }
            clearPendingRequest(requestKey, resp.requestId)
            return
          }

          if (!isLatestPendingRequest(requestKey, resp.requestId)) {
            return
          }

          persistTheme(confirmedTheme, get().customThemes)
          set({ theme: confirmedTheme })
          pendingRequestIdsByKey.delete(requestKey)
        })
      }
    },

    refreshCustomThemes: async () => {
      if (!isCefContext()) return true
      const response = await sendToCEF<{ themes?: unknown[] }>({ action: 'listThemes' })
      if (!response.ok) return false
      const customThemes = (response.data?.themes ?? []).flatMap((theme) => {
        const normalized = normalizeCustomTheme(theme)
        return normalized ? [normalized] : []
      })
      set({ customThemes })
      persistTheme(get().theme, customThemes)
      return true
    },

    saveCustomTheme: async (theme: CustomTheme) => {
      const normalizedInput = normalizeCustomTheme(theme)
      if (!normalizedInput) return null
      const response = await sendToCEF<{ theme?: unknown }>({ action: 'saveTheme', payload: { theme: normalizedInput } })
      if (!response.ok) return null
      const saved = normalizeCustomTheme(response.data?.theme)
      if (!saved) return null
      const customThemes = [...get().customThemes.filter((candidate) => candidate.id !== saved.id), saved]
        .sort((a, b) => a.name.localeCompare(b.name))
      set({ customThemes })
      if (get().theme === saved.id) persistTheme(saved.id, customThemes)
      return saved
    },

    deleteCustomTheme: async (id: string) => {
      if (!id.startsWith('custom:')) return false
      const response = await sendToCEF({ action: 'deleteTheme', payload: { id } })
      if (!response.ok) return false
      const customThemes = get().customThemes.filter((candidate) => candidate.id !== id)
      const nextTheme = get().theme === id ? 'focus' : get().theme
      set({ customThemes, theme: nextTheme })
      persistTheme(nextTheme, customThemes)
      return true
    },

    setVcsSettings: async (settings: VcsSettingsRequest) => {
      if (typeof settings.gitEnabled !== 'boolean' || typeof settings.svnEnabled !== 'boolean') return false
      const optionalKeys = ['gitAutoWorktree', 'gitSnapshotSvn', 'gitSnapshotPlainFolders', 'gitDefaultBaseBranch', 'gitPullBeforeChat', 'svnUpdateBeforeChat', 'gitPortMethod', 'worktreeCleanupEnabled', 'worktreeRetentionDays']
      const parsed = sanitizeVcsStorageSettings(settings)
      const raw = settings as unknown as Record<string, unknown>
      if (Object.keys(settings).some((key) => key !== 'gitEnabled' && key !== 'svnEnabled' &&
        (raw[key] !== undefined && (!optionalKeys.includes(key) || !Object.prototype.hasOwnProperty.call(parsed, key))))) return false
      const payload = { gitEnabled: settings.gitEnabled, svnEnabled: settings.svnEnabled, ...parsed }
      const previous = Object.fromEntries(Object.keys(payload).map((key) => [key, get()[key as keyof AppState]])) as Partial<AppState>
      const optimisticRevision = rememberOptimisticFields(Object.keys(payload))
      set(payload)
      if (!isCefContext()) return true
      try {
        const response = await sendToCEF({ action: 'setVcsSettings', payload }, false)
        if (!response.ok) set(latestOptimisticRollback(previous, optimisticRevision))
        return response.ok
      } catch {
        set(latestOptimisticRollback(previous, optimisticRevision))
        return false
      }
    },

    setProviderAvailability: async (settings: ProviderAvailabilityRequest) => {
      if (Object.keys(settings).some((key) => !['providerId', 'executionHostId', 'enabled'].includes(key))) return false
      const payload = sanitizeProviderAvailability([settings])?.[0]
      if (!payload || !get().executionHosts.some((host) => host.id === payload.executionHostId) || !isCefContext()) return false
      const matchesTarget = (entry: ProviderAvailabilityRequest) => entry.providerId === payload.providerId && entry.executionHostId === payload.executionHostId
      const previousEnabled = get().providerAvailability.find(matchesTarget)?.enabled ?? true
      const previousRevision = get().lastAppliedStateRevision
      const key = `providerAvailability:${JSON.stringify([payload.providerId, payload.executionHostId])}`
      const requestId = createRequestId('setProviderAvailability')
      rememberPendingRequest(key, requestId)
      try {
        const response = await sendToCEF({ action: 'setProviderAvailability', payload, requestId }, false)
        if (response.ok && isLatestPendingRequest(key, requestId) && get().lastAppliedStateRevision === previousRevision &&
          (get().providerAvailability.find(matchesTarget)?.enabled ?? true) === previousEnabled &&
          get().executionHosts.some((host) => host.id === payload.executionHostId)) {
          const retained = get().providerAvailability.filter((entry) => !matchesTarget(entry))
          set({ providerAvailability: payload.enabled ? retained : [...retained, payload] })
        }
        return response.ok
      } catch { return false }
      finally { clearPendingRequest(key, requestId) }
    },

    setVoiceInputSettings: async (settings: VoiceInputSettingsRequest) => {
      const mapping = { mode: 'voiceInputMode', serverBaseUrl: 'voiceInputServerBaseUrl', serverEndpoint: 'voiceInputServerEndpoint', serverModel: 'voiceInputServerModel', apiKeyEnv: 'voiceInputApiKeyEnv' } as const
      if (Object.keys(settings).some((key) => !Object.prototype.hasOwnProperty.call(mapping, key)) || !isCefContext()) return false
      const raw = settings as unknown as Record<string, unknown>
      const projected = Object.fromEntries(Object.entries(mapping).filter(([key]) => raw[key] !== undefined).map(([key, field]) => [field, raw[key]]))
      const parsed = sanitizeVoiceInputSettings(projected)
      if (!parsed.voiceInputMode || Object.keys(projected).some((field) => !Object.prototype.hasOwnProperty.call(parsed, field))) return false
      const previous = Object.fromEntries(Object.values(mapping).map((field) => [field, get()[field]]))
      const effective = { ...previous, ...parsed }
      if (parsed.voiceInputMode === 'server') {
        const validated = sanitizeVoiceInputSettings(effective)
        if (['voiceInputServerBaseUrl', 'voiceInputServerEndpoint', 'voiceInputServerModel', 'voiceInputApiKeyEnv'].some((field) =>
          !Object.prototype.hasOwnProperty.call(validated, field))) return false
      }
      const payload = Object.fromEntries(Object.entries(mapping).filter(([, field]) => Object.prototype.hasOwnProperty.call(parsed, field)).map(([key, field]) => [key, parsed[field as keyof typeof parsed]]))
      const key = 'voiceInputSettings'
      const requestId = createRequestId('setVoiceInputSettings')
      rememberPendingRequest(key, requestId)
      try {
        const response = await sendToCEF({ action: 'setVoiceInputSettings', payload, requestId }, false)
        if (response.ok && isLatestPendingRequest(key, requestId) && Object.values(mapping).every((field) => get()[field] === previous[field])) set(parsed)
        return response.ok
      } catch { return false }
      finally { clearPendingRequest(key, requestId) }
    },

    setStorageSettings: async (settings: StorageSettingsRequest) => {
      const parsed = sanitizeVcsStorageSettings(settings)
      if (parsed.backupLogRetentionDays === undefined || parsed.httpCacheClearIntervalDays === undefined || !isCefContext()) return false
      const payload = { backupLogRetentionDays: parsed.backupLogRetentionDays, httpCacheClearIntervalDays: parsed.httpCacheClearIntervalDays }
      const previous = { backupLogRetentionDays: get().backupLogRetentionDays, httpCacheClearIntervalDays: get().httpCacheClearIntervalDays }
      try {
        const response = await sendToCEF({ action: 'setStorageSettings', payload }, false)
        if (response.ok && get().backupLogRetentionDays === previous.backupLogRetentionDays &&
          get().httpCacheClearIntervalDays === previous.httpCacheClearIntervalDays) set(payload)
        return response.ok
      } catch {
        return false
      }
    },

    setSidebarSettings: async (settings: Pick<AppState, 'showProviderIconsInSidebar' | 'showWorktreePathInSidebar'>) => {
      const previous = {
        showProviderIconsInSidebar: get().showProviderIconsInSidebar,
        showWorktreePathInSidebar: get().showWorktreePathInSidebar,
      }
      const optimisticRevision = rememberOptimisticFields(Object.keys(settings))
      set(settings)
      if (!isCefContext()) return true
      const response = await sendToCEF({ action: 'setSidebarSettings', payload: settings, requestId: createRequestId('setSidebarSettings') })
      if (!response.ok) set(latestOptimisticRollback(previous, optimisticRevision))
      return response.ok
    },

    setNativeImportFilters: async (filters: { titlePrefixes: string[]; directories: string[] }) => {
      const clean = (rules: string[]) => Array.from(new Set(rules.map((rule) => rule.trim()).filter((rule) => rule && rule.length <= 4096))).slice(0, 100)
      const next = { nativeImportExcludedTitlePrefixes: clean(filters.titlePrefixes), nativeImportExcludedDirectories: clean(filters.directories) }
      const previous = {
        nativeImportExcludedTitlePrefixes: get().nativeImportExcludedTitlePrefixes,
        nativeImportExcludedDirectories: get().nativeImportExcludedDirectories,
      }
      const optimisticRevision = rememberOptimisticFields(Object.keys(next))
      set(next)
      if (!isCefContext()) return true
      const response = await sendToCEF({
        action: 'setNativeImportFilters',
        payload: { titlePrefixes: next.nativeImportExcludedTitlePrefixes, directories: next.nativeImportExcludedDirectories },
        requestId: createRequestId('setNativeImportFilters'),
      })
      if (!response.ok) set(latestOptimisticRollback(previous, optimisticRevision))
      return response.ok
    },

    setNewChatModalOpen: (open: boolean, folderId?: string | null, worktreeContext?: NewChatWorktreeContext | null) => set({
      isNewChatModalOpen: open,
      newChatFolderId: open ? (folderId ?? null) : null,
      newChatWorktreeContext: open ? (worktreeContext ?? null) : null,
    }),
    setSettingsScopeHostId: (hostId: string) => set({ settingsScopeHostId: hostId || '*' }),
    setSettingsOpen: (open: boolean) => {
      // Retire embedded memory before closing, so it cannot reappear as a standalone overlay.
      if (!open && get().isSettingsOpen) get().closeMemoryLibrary()
      set({ isSettingsOpen: open })
    },
    setWorkingDisplayMode: (workingDisplayMode: 'compact' | 'verbose') => set((state: AppState) => {
      writeStoredAppShellLayout({ ...state, workingDisplayMode })
      return { workingDisplayMode }
    }),
    setExpandWorkTraces: (expandWorkTraces: boolean) => set((state: AppState) => {
      writeStoredAppShellLayout({ ...state, expandWorkTraces })
      return { expandWorkTraces }
    }),
    setCollapsibleWorkSections: (collapsibleWorkSections: boolean) => set((state: AppState) => {
      writeStoredAppShellLayout({ ...state, collapsibleWorkSections })
      return { collapsibleWorkSections }
    }),
    setSidebarCollapsed: (collapsed: boolean) => set((state: AppState) => {
      const next = {
        sidebarCollapsed: collapsed,
        commitPanelOpen: state.commitPanelOpen,
        sidebarWidthPx: state.sidebarWidthPx,
        commitPanelWidthPx: state.commitPanelWidthPx,
        workingDisplayMode: state.workingDisplayMode,
        expandWorkTraces: state.expandWorkTraces,
        collapsibleWorkSections: state.collapsibleWorkSections,
      }
      writeStoredAppShellLayout(next)
      return { sidebarCollapsed: collapsed }
    }),
    setCommitPanelOpen: (open: boolean) => set((state: AppState) => {
      const next = {
        sidebarCollapsed: state.sidebarCollapsed,
        commitPanelOpen: open,
        sidebarWidthPx: state.sidebarWidthPx,
        commitPanelWidthPx: state.commitPanelWidthPx,
        workingDisplayMode: state.workingDisplayMode,
        expandWorkTraces: state.expandWorkTraces,
        collapsibleWorkSections: state.collapsibleWorkSections,
      }
      writeStoredAppShellLayout(next)
      return { commitPanelOpen: open }
    }),
    setSidebarWidthPx: (width: number) => set((state: AppState) => {
      const sidebarWidthPx = clampSidebarWidthPx(width)
      if (sidebarWidthPx === state.sidebarWidthPx) {
        return state
      }
      const next = {
        sidebarCollapsed: state.sidebarCollapsed,
        commitPanelOpen: state.commitPanelOpen,
        sidebarWidthPx,
        commitPanelWidthPx: state.commitPanelWidthPx,
        workingDisplayMode: state.workingDisplayMode,
        expandWorkTraces: state.expandWorkTraces,
        collapsibleWorkSections: state.collapsibleWorkSections,
      }
      writeStoredAppShellLayout(next)
      return { sidebarWidthPx }
    }),
    setCommitPanelWidthPx: (width: number) => set((state: AppState) => {
      const commitPanelWidthPx = clampCommitPanelWidthPx(width)
      if (commitPanelWidthPx === state.commitPanelWidthPx) {
        return state
      }
      const next = {
        sidebarCollapsed: state.sidebarCollapsed,
        commitPanelOpen: state.commitPanelOpen,
        sidebarWidthPx: state.sidebarWidthPx,
        commitPanelWidthPx,
        workingDisplayMode: state.workingDisplayMode,
        expandWorkTraces: state.expandWorkTraces,
        collapsibleWorkSections: state.collapsibleWorkSections,
      }
      writeStoredAppShellLayout(next)
      return { commitPanelWidthPx }
    }),
  }
}
