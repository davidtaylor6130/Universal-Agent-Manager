export interface DataFolderSettings {
  currentFolder: string
  pendingFolder: string
  revision: string
  lastOperationId: string
  transitionState: 'none' | 'awaitingMigration' | 'publicationUnknown'
  externalOverride: boolean
  canChange: boolean
  restartReady: boolean
  message: string
}

/** Require native state; the preview stub and incomplete replies cannot imply saved settings. */
export function parseDataFolderSettings(value: unknown): DataFolderSettings | null {
  if (!value || typeof value !== 'object') return null
  const state = value as Record<string, unknown>
  if (typeof state.currentFolder !== 'string' || !state.currentFolder || typeof state.pendingFolder !== 'string'
    || typeof state.revision !== 'string' || !state.revision || typeof state.lastOperationId !== 'string' || typeof state.externalOverride !== 'boolean'
    || typeof state.canChange !== 'boolean' || typeof state.restartReady !== 'boolean' || typeof state.message !== 'string'
    || typeof state.transitionState !== 'string' || !['none', 'awaitingMigration', 'publicationUnknown'].includes(state.transitionState)) return null
  if (state.transitionState === 'none' && state.pendingFolder) return null
  if (state.transitionState === 'awaitingMigration' && !state.pendingFolder) return null
  if ((state.externalOverride || state.transitionState === 'publicationUnknown') && state.canChange) return null
  if (state.restartReady && (!state.canChange || state.externalOverride || state.transitionState !== 'awaitingMigration' || !state.pendingFolder)) return null
  return state as unknown as DataFolderSettings
}

export interface UnconfirmedFolderChoice {
  action: 'chooseDataFolder' | 'cancelDataFolderChoice'
  folder: string
  expectedRevision: string
  operationId: string
}

// Session-only draft, never a root preference or authority. Retained across Settings navigation/remount.
let unconfirmed: UnconfirmedFolderChoice | null = null
export function readUnconfirmedFolderChoice() { return unconfirmed }
export function retainUnconfirmedFolderChoice(value: UnconfirmedFolderChoice | null) { unconfirmed = value }

// A restart reply cannot establish completed migration. Keep uncertain requests held through navigation.
let restartAttempt: { revision: string; folder: string } | null = null
export function readUnconfirmedFolderRestart() { return restartAttempt }
export function retainUnconfirmedFolderRestart(value: typeof restartAttempt) { restartAttempt = value }

export function observesFolderChoice(settings: DataFolderSettings, draft: UnconfirmedFolderChoice): boolean {
  return !settings.externalOverride && settings.transitionState !== 'publicationUnknown'
    && settings.revision !== draft.expectedRevision && settings.lastOperationId === draft.operationId
    && (draft.action === 'chooseDataFolder'
      ? settings.transitionState === 'awaitingMigration' && Boolean(settings.pendingFolder)
      : settings.transitionState === 'none' && !settings.pendingFolder)
}
