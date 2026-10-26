import { useAppStore } from '../store/useAppStore'

/** A projection witness for rejecting stale captures; native services still resolve and fence their own sources. */
export function contextReferenceOwner(chatId: string) {
  const state = useAppStore.getState()
  const chat = state.sessions.find((session) => session.id === chatId)
  if (!chat || chat.importedReadOnly || !Number.isFinite(chat.createdAt.getTime())) return null
  const folder = state.folders.find((item) => item.id === chat.folderId)
  const host = chat.executionHostId?.trim() || 'local'
  const isolated = chat.workspaceIsolationKind?.trim() === 'gitWorktree'
  const worktree = isolated ? chat.workspaceWorktreeDirectory?.trim() || '' : ''
  // Local native roots fall back from an empty worktree to the chat directory, then its folder.
  // Remote isolated roots require their configured worktree; this witness cannot invent one.
  if (isolated && !worktree && host !== 'local') return null
  const folderWorkspace = host === 'local' || folder?.executionHostId === chat.executionHostId ? folder?.directory?.trim() || '' : ''
  const workspace = worktree || chat.workspaceDirectory?.trim() || folderWorkspace
  const birth = chat.createdAt.getTime()
  // Delivery keeps the original snapshot when display/association metadata refreshes for the same target.
  // Capture still uses the stricter association witness below; native services retain source authority.
  return { workspace, host, birth, deliveryKey: JSON.stringify([chatId, birth, host, workspace, chat.providerId]), key: JSON.stringify([chatId, birth, chat.workspaceDirectory, chat.workspaceWorktreeDirectory,
    host, chat.workspaceIsolationKind, chat.workspaceSourceDirectory, chat.workspaceBranchName, chat.workspaceBaseRef,
    chat.folderId, folder?.directory, folder?.executionHostId]) }
}
