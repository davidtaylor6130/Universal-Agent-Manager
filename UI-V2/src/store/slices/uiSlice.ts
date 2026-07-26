import { sendToCEF, isCefContext, createRequestId } from '../../ipc/cefBridge'
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
}) {
  if (typeof window === 'undefined') return
  try {
    window.localStorage.setItem(appShellLayoutStorageKey, JSON.stringify({
      sidebarCollapsed: layout.sidebarCollapsed,
      commitPanelOpen: layout.commitPanelOpen,
      sidebarWidthPx: clampSidebarWidthPx(layout.sidebarWidthPx),
      commitPanelWidthPx: clampCommitPanelWidthPx(layout.commitPanelWidthPx),
      workingDisplayMode: layout.workingDisplayMode,
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

  return {
    theme: readDocumentTheme(),
    customThemes: [] as CustomTheme[],
    workingDisplayMode: storedShellLayout.workingDisplayMode,
    showProviderIconsInSidebar: true,
    showWorktreePathInSidebar: true,
    isNewChatModalOpen: false,
    newChatFolderId: null as string | null,
    isSettingsOpen: false,
    sidebarCollapsed: storedShellLayout.sidebarCollapsed,
    commitPanelOpen: storedShellLayout.commitPanelOpen,
    sidebarWidthPx: storedShellLayout.sidebarWidthPx,
    commitPanelWidthPx: storedShellLayout.commitPanelWidthPx,
    streamingMessageId: null as string | null,
    pushChannelStatus: (inCef ? 'no-push-yet' : 'connected') as AppState['pushChannelStatus'],
    pushChannelError: '',
    lastPushAtMs: null as number | null,
    uiBuildId: UI_RUNTIME_BUILD_MARKER,

    setTheme: (theme: StoredTheme) => {
      const previousTheme = get().theme
      persistTheme(theme, get().customThemes)
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

          persistTheme(previousTheme, get().customThemes)
          set({ theme: previousTheme })
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

    setNewChatModalOpen: (open: boolean, folderId?: string | null) => set({
      isNewChatModalOpen: open,
      newChatFolderId: open ? (folderId ?? null) : null,
    }),
    setSettingsOpen: (open: boolean) => set({ isSettingsOpen: open }),
    setWorkingDisplayMode: (workingDisplayMode: 'compact' | 'verbose') => set((state: AppState) => {
      writeStoredAppShellLayout({ ...state, workingDisplayMode })
      return { workingDisplayMode }
    }),
    setSidebarCollapsed: (collapsed: boolean) => set((state: AppState) => {
      const next = {
        sidebarCollapsed: collapsed,
        commitPanelOpen: state.commitPanelOpen,
        sidebarWidthPx: state.sidebarWidthPx,
        commitPanelWidthPx: state.commitPanelWidthPx,
        workingDisplayMode: state.workingDisplayMode,
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
      }
      writeStoredAppShellLayout(next)
      return { commitPanelWidthPx }
    }),
  }
}
