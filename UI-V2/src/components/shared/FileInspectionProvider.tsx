import { useCallback, useRef, useState, type ReactNode } from 'react'
import { useAppStore } from '../../store/useAppStore'
import type { WorkspaceFilePreview } from '../../types/filePreview'
import { workspaceRelativeFilePath, type FileReference } from '../../utils/fileReferences'
import type { VcsChangedFile, VcsType } from '../../store/useAppStore'
import { FileInspectionContext } from './fileInspectionContext'
import { FilePreviewDialog } from './FilePreviewDialog'
import { RepositoryDiffDialog } from './RepositoryDiffDialog'

export function FileInspectionProvider({ chatId, children }: { chatId: string; children: ReactNode }) {
  const [selection, setSelection] = useState<{ chatId: string; reference: FileReference } | null>(null)
  const [changes, setChanges] = useState<{ chatId: string; file: VcsChangedFile; type: VcsType; comparisonRef?: string; context: string } | null>(null)
  const context = useAppStore((state) => {
    const session = state.sessions.find((candidate) => candidate.id === chatId)
    return JSON.stringify([chatId, session?.workspaceDirectory, session?.workspaceWorktreeDirectory, session?.executionHostId, session?.workspaceBaseRef, session?.workspaceIsolationKind, session?.importedReadOnly,
      state.folders.find((folder) => folder.id === session?.folderId)?.directory])
  })
  const latestContext = useRef(context); latestContext.current = context
  const current = useRef(selection); current.current = selection
  const close = useCallback(() => { setSelection(null); setChanges(null) }, [])
  const open = useCallback((reference: FileReference) => { setSelection({ chatId, reference }); setChanges(null) }, [chatId])
  const showChanges = async (preview: WorkspaceFilePreview | null) => {
    const captured = current.current
    if (!captured || captured.chatId !== chatId) return
    const state = useAppStore.getState()
    const session = state.sessions.find((candidate) => candidate.id === chatId)
    const capturedContext = context
    const status = await state.getVcsCommitStatus(chatId, 'git', { contextOnly: true, includeLineStats: false })
    if (current.current !== captured || latestContext.current !== capturedContext) return
    if (!status?.available || status.error) throw new Error(status?.error || status?.warning || 'No repository is available for this file.')
    const path = workspaceRelativeFilePath(preview?.path ?? captured.reference.path, status.workspaceDirectory)
    if (!path) throw new Error('This file is outside the current repository.')
    const comparisonRef = status.activeVcsType === 'git' && session?.workspaceIsolationKind === 'gitWorktree' ? session.workspaceBaseRef || undefined : undefined
    setChanges({ chatId, context: capturedContext, type: status.activeVcsType, comparisonRef,
      file: { path, status: 'M', binary: false, staged: false, additions: 0, deletions: 0 } })
  }
  return <FileInspectionContext.Provider value={open}>
    {children}
    {selection?.chatId === chatId && <FilePreviewDialog chatId={chatId} reference={selection.reference} onClose={close} onShowChanges={showChanges} />}
    {changes?.chatId === chatId && changes.context === context && <RepositoryDiffDialog chatId={chatId} file={changes.file} vcsType={changes.type} comparisonRef={changes.comparisonRef}
      showFileStats={false} comparisonLabel={changes.comparisonRef ? `Against ${changes.comparisonRef}` : 'Working tree changes'} zIndex={1200}
      getDiff={useAppStore.getState().getVcsFileDiff} onClose={() => setChanges(null)} />}
  </FileInspectionContext.Provider>
}
