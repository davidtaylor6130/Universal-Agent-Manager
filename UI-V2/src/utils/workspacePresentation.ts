import type { Folder, Session } from '../types/session'

function pathIdentity(path: string): string {
  const normalized = path.replace(/\\/g, '/').replace(/\/+$/, '')
  return /^[a-z]:/i.test(normalized) ? normalized.toLowerCase() : normalized
}

/** Display organisation does not change the directory used for workspace actions. */
export function composerWorkspaceName(session: Session, folders: Folder[], workspaceDirectory: string): string {
  const source = session.workspaceSourceDirectory?.trim() || workspaceDirectory
  const folder = folders.find((candidate) => (candidate.executionHostId || 'local') === (session.executionHostId || 'local')
    && pathIdentity(candidate.directory) === pathIdentity(source))
  return folder?.name || source.replace(/\\/g, '/').split('/').filter(Boolean).pop() || 'No workspace'
}
