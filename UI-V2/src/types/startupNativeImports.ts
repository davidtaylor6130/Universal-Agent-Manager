export interface StartupNativeImportStatus {
  phase: 'idle' | 'scanning' | 'ready' | 'importing' | 'complete' | 'cancelled'
  generation: string
  totalCount: number
  importedCount: number
}

export interface StartupNativeImportCandidate {
  candidateId: string
  providerId: string
  nativeSessionId: string
  title: string
  folderId: string
  workspaceDirectory: string
  executionHostId: string
  updatedAt: string
  workspaceTitle?: string
  newWorkspace?: boolean
}

export interface StartupNativeImportPage extends StartupNativeImportStatus {
  candidates: StartupNativeImportCandidate[]
  nextOffset: number
  errors: string[]
}

export const EMPTY_STARTUP_NATIVE_IMPORT_STATUS: StartupNativeImportStatus = {
  phase: 'idle', generation: '', totalCount: 0, importedCount: 0,
}

/** Accepts only bounded startup metadata; native transcript payloads never enter this state. */
export function sanitizeStartupNativeImportStatus(value: unknown): StartupNativeImportStatus {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return EMPTY_STARTUP_NATIVE_IMPORT_STATUS
  const record = value as Record<string, unknown>
  const phases: StartupNativeImportStatus['phase'][] = ['idle', 'scanning', 'ready', 'importing', 'complete', 'cancelled']
  if (!phases.includes(record.phase as StartupNativeImportStatus['phase']) || typeof record.generation !== 'string' || record.generation.length > 128) return EMPTY_STARTUP_NATIVE_IMPORT_STATUS
  const count = (input: unknown) => typeof input === 'number' && Number.isSafeInteger(input) && input >= 0 ? input : 0
  return { phase: record.phase as StartupNativeImportStatus['phase'], generation: record.generation, totalCount: count(record.totalCount), importedCount: count(record.importedCount) }
}

/** Validates cached metadata at the IPC boundary and drops unknown payload fields. */
export function parseStartupNativeImportPage(value: unknown): StartupNativeImportPage | null {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return null
  const record = value as Record<string, unknown>
  const count = (input: unknown): input is number => typeof input === 'number' && Number.isSafeInteger(input) && input >= 0
  const text = (input: unknown, limit: number): input is string => typeof input === 'string' && input.length <= limit
  const status = sanitizeStartupNativeImportStatus(record)
  if (!status.generation || status.phase !== record.phase || !count(record.totalCount) || !count(record.importedCount) || !count(record.nextOffset) || record.nextOffset > record.totalCount) return null
  if (!Array.isArray(record.candidates) || record.candidates.length > 200 || record.candidates.length > record.nextOffset || !Array.isArray(record.errors) || record.errors.length > 200 || !record.errors.every((error) => text(error, 4096))) return null
  const candidates: StartupNativeImportCandidate[] = []
  const ids = new Set<string>()
  for (const value of record.candidates) {
    if (!value || typeof value !== 'object' || Array.isArray(value)) return null
    const candidate = value as Record<string, unknown>
    if (!text(candidate.candidateId, 128) || !candidate.candidateId || ids.has(candidate.candidateId) ||
      !text(candidate.providerId, 128) || !text(candidate.nativeSessionId, 4096) || !text(candidate.title, 4096) ||
      !text(candidate.folderId, 128) || !text(candidate.workspaceDirectory, 32768) || !text(candidate.executionHostId, 128) || !text(candidate.updatedAt, 128) ||
      (candidate.workspaceTitle !== undefined && !text(candidate.workspaceTitle, 512)) || (candidate.newWorkspace !== undefined && typeof candidate.newWorkspace !== 'boolean')) return null
    ids.add(candidate.candidateId)
    candidates.push({ candidateId: candidate.candidateId, providerId: candidate.providerId, nativeSessionId: candidate.nativeSessionId, title: candidate.title, folderId: candidate.folderId, workspaceDirectory: candidate.workspaceDirectory, executionHostId: candidate.executionHostId, updatedAt: candidate.updatedAt,
      ...(candidate.workspaceTitle !== undefined ? { workspaceTitle: candidate.workspaceTitle as string } : {}), ...(candidate.newWorkspace !== undefined ? { newWorkspace: candidate.newWorkspace as boolean } : {}) })
  }
  return { ...status, candidates, nextOffset: record.nextOffset, errors: record.errors as string[] }
}
