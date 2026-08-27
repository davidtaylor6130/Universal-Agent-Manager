import { memo, useCallback, useEffect, useRef, useState, useMemo } from 'react'
import type { MouseEvent as ReactMouseEvent, ReactNode, RefObject } from 'react'
import { Check, Plus, Folder as FolderIcon, FolderOpen as FolderOpenIcon, FolderSync, X, MoreHorizontal, MessageSquarePlus, Brain, Pencil, Trash2, TriangleAlert, ChevronRight, Library, SearchX, RefreshCw } from 'lucide-react'
import type { LucideIcon } from 'lucide-react'
import { useAppStore } from '../../store/useAppStore'
import { useShallow } from 'zustand/react/shallow'
import { SessionItem } from './SessionItem'
import { Button, IconButton, Tooltip, ViewportMenu } from '../ui'
import {
  type ChatSearchFilters,
  buildChatSearchIndex,
  buildChatSearchModelFromGroups,
  buildChatSearchSessionGroups,
  displayedChatStatus,
  tokenizeChatSearchQuery,
} from './chatSearch'
import type { Folder, Session, WorkspaceFolderRecoveryPreview } from '../../types/session'
import { chatGridLeaves, chatPaneColors, readChatGridLayout, subscribeChatGridLayout } from '../../utils/chatGridStorage'
import { CollectionMenuItems, moveFolderToCollection } from './CollectionMenuItems'
import type { ResourceCollection } from '../../types/resourceCollection'

interface FolderTreeProps {
  searchQuery: string
  deepSearchSessionIds?: string[]
  filters?: ChatSearchFilters
}

const VISIBLE_SESSION_LIMIT = 5
const EMPTY_SEARCH_INDEX = {}
type FolderDropEdge = 'before' | 'after'

function moveId(ids: string[], sourceId: string, targetId: string, edge: FolderDropEdge) {
  const reordered = ids.filter((id) => id !== sourceId)
  const targetIndex = reordered.indexOf(targetId)
  if (targetIndex < 0 || reordered.length === ids.length) return ids
  reordered.splice(targetIndex + (edge === 'after' ? 1 : 0), 0, sourceId)
  return reordered
}

function reorderCollectionFolderReferences(
  collection: ResourceCollection,
  sourceFolderId: string,
  targetFolderId: string,
  edge: FolderDropEdge,
) {
  const folderReferences = collection.references.filter((reference) => reference.type === 'workspace-folder')
  const sourceReference = folderReferences.find((reference) => reference.target === sourceFolderId)
  const targetReference = folderReferences.find((reference) => reference.target === targetFolderId)
  if (!sourceReference || !targetReference) return null

  const reorderedFolderIds = moveId(
    folderReferences.map((reference) => reference.id),
    sourceReference.id,
    targetReference.id,
    edge,
  )
  let folderIndex = 0
  return collection.references.map((reference) =>
    reference.type === 'workspace-folder' ? reorderedFolderIds[folderIndex++] : reference.id
  )
}

export function FolderTree({ searchQuery, deepSearchSessionIds, filters }: FolderTreeProps) {
  const folders = useAppStore(useShallow((s) => s.folders))
  const sessions = useAppStore(useShallow((s) => s.sessions))
  const resourceCollections = useAppStore(useShallow((s) => s.resourceCollections))
  const runtimeStatusSignature = useAppStore((s) => sessions.map(({ id }) => {
    const cli = s.cliBindingBySessionId[id]
    const acp = s.acpBindingBySessionId[id]
    return JSON.stringify([id, cli?.processing, cli?.lifecycleState, cli?.readySinceLastSelect,
      acp?.processing, acp?.lifecycleState, acp?.readySinceLastSelect, acp?.attentionKind])
  }).join('\n'))
  const runtimeBindings = useMemo(() => {
    const state = useAppStore.getState()
    return { cliBindingBySessionId: state.cliBindingBySessionId, acpBindingBySessionId: state.acpBindingBySessionId }
  }, [runtimeStatusSignature])
  const toggleFolder        = useAppStore((s) => s.toggleFolder)
  const addFolder           = useAppStore((s) => s.addFolder)
  const renameFolder        = useAppStore((s) => s.renameFolder)
  const deleteFolder        = useAppStore((s) => s.deleteFolder)
  const rescanFolderChats    = useAppStore((s) => s.rescanFolderChats)
  const browseFolderDirectory = useAppStore((s) => s.browseFolderDirectory)
  const setNewChatModalOpen = useAppStore((s) => s.setNewChatModalOpen)
  const openFolderMemoryLibrary = useAppStore((s) => s.openFolderMemoryLibrary)
  const reorderFolders = useAppStore((s) => s.reorderFolders)
  const reorderResourceReferences = useAppStore((s) => s.reorderResourceReferences)
  const createResourceCollection = useAppStore((s) => s.createResourceCollection)
  const deleteSessions = useAppStore((s) => s.deleteSessions)
  const previewUnsortedWorkspaceFolders = useAppStore((s) => s.previewUnsortedWorkspaceFolders)
  const rebuildUnsortedWorkspaceFolders = useAppStore((s) => s.rebuildUnsortedWorkspaceFolders)
  const workspaceFolderRecoveryError = useAppStore((s) => s.workspaceFolderRecoveryError)

  const [addingFolder, setAddingFolder] = useState(false)
  const [actionError, setActionError] = useState('')
  const [addingCollection, setAddingCollection] = useState(false)
  const [newCollectionName, setNewCollectionName] = useState('')
  const [newFolderName, setNewFolderName] = useState('')
  const [newFolderDirectory, setNewFolderDirectory] = useState('')
  const [editingFolderId, setEditingFolderId] = useState<string | null>(null)
  const [editFolderName, setEditFolderName] = useState('')
  const [editFolderDirectory, setEditFolderDirectory] = useState('')
  const [pendingDeleteFolderId, setPendingDeleteFolderId] = useState<string | null>(null)
  const [deleteFolderError, setDeleteFolderError] = useState('')
  const [deletingFolder, setDeletingFolder] = useState(false)
  const [unsortedCollapsed, setUnsortedCollapsed] = useState(false)
  const [selectedSessionIds, setSelectedSessionIds] = useState<Set<string>>(() => new Set())
  const [selectionAnchorId, setSelectionAnchorId] = useState<string | null>(null)
  const selectionAnchorRowRef = useRef<HTMLElement | null>(null)
  const [pendingBulkDeleteIds, setPendingBulkDeleteIds] = useState<string[] | null>(null)
  const [bulkDeleteFailed, setBulkDeleteFailed] = useState(false)
  const [unsortedMenuPoint, setUnsortedMenuPoint] = useState<{ x: number; y: number } | null>(null)
  const [recoveryDialogOpen, setRecoveryDialogOpen] = useState(false)
  const [recoveryPreview, setRecoveryPreview] = useState<WorkspaceFolderRecoveryPreview | null>(null)
  const [recoveryLoading, setRecoveryLoading] = useState(false)
  const [recoveryApplying, setRecoveryApplying] = useState(false)
  const [gridLayout, setGridLayout] = useState(readChatGridLayout)
  const [draggedFolderId, setDraggedFolderId] = useState<string | null>(null)
  const [folderDropTarget, setFolderDropTarget] = useState<{ id: string; edge: FolderDropEdge } | null>(null)
  const addControlsRef = useRef<HTMLDivElement>(null)
  const treeRef = useRef<HTMLDivElement>(null)
  const unsortedMenuTriggerRef = useRef<HTMLButtonElement>(null)
  const unsortedMenuRef = useRef<HTMLDivElement>(null)
  const recoveryDialogRef = useRef<HTMLDivElement>(null)
  const recoveryRequestRef = useRef(0)

  useEffect(() => subscribeChatGridLayout(setGridLayout), [])

  useEffect(() => {
    if (!unsortedMenuPoint) return
    unsortedMenuRef.current?.querySelector<HTMLButtonElement>('button')?.focus()
    const dismiss = (event: MouseEvent) => {
      const target = event.target as Node
      if (!unsortedMenuRef.current?.contains(target) && !unsortedMenuTriggerRef.current?.contains(target)) setUnsortedMenuPoint(null)
    }
    const closeOnEscape = (event: KeyboardEvent) => {
      if (event.key !== 'Escape') return
      setUnsortedMenuPoint(null)
      unsortedMenuTriggerRef.current?.focus()
    }
    document.addEventListener('mousedown', dismiss)
    document.addEventListener('keydown', closeOnEscape)
    return () => {
      document.removeEventListener('mousedown', dismiss)
      document.removeEventListener('keydown', closeOnEscape)
    }
  }, [unsortedMenuPoint])

  useEffect(() => {
    if (!recoveryDialogOpen) return
    recoveryDialogRef.current?.focus()
    const closeOnEscape = (event: KeyboardEvent) => {
      if (event.key !== 'Escape' || recoveryApplying) return
      recoveryRequestRef.current++
      setRecoveryDialogOpen(false)
      unsortedMenuTriggerRef.current?.focus()
    }
    window.addEventListener('keydown', closeOnEscape)
    return () => window.removeEventListener('keydown', closeOnEscape)
  }, [recoveryApplying, recoveryDialogOpen])

  useEffect(() => {
    if (addingFolder || addingCollection) {
      addControlsRef.current?.scrollIntoView?.({ block: 'nearest' })
    }
  }, [addingCollection, addingFolder])

  const paneColorsForFolders = (folderIds: Set<string>) => {
    const leaves = chatGridLeaves(gridLayout.root)
    if (leaves.length === 1) return []
    const folderSessionIds = new Set(sessions.filter((session) => session.folderId && folderIds.has(session.folderId)).map((session) => session.id))
    return leaves.flatMap((leaf, index) => folderSessionIds.has(leaf.sessionId) ? [chatPaneColors[index]] : [])
  }
  const paneColorsForFolder = (folderId: string) => paneColorsForFolders(new Set([folderId]))

  const searchTokens = useMemo(
    () => tokenizeChatSearchQuery(searchQuery),
    [searchQuery]
  )
  const searchIndex = useMemo(
    () => searchTokens.length > 0 ? buildChatSearchIndex(sessions) : EMPTY_SEARCH_INDEX,
    [sessions, searchTokens]
  )
  const deepSearchSet = useMemo(
    () => deepSearchSessionIds ? new Set(deepSearchSessionIds) : undefined,
    [deepSearchSessionIds]
  )
  const searchGroups = useMemo(
    () => buildChatSearchSessionGroups(
      sessions,
      searchIndex,
      searchTokens,
      deepSearchSet,
      filters,
      runtimeBindings
    ),
    [sessions, searchIndex, searchTokens, deepSearchSet, filters, runtimeBindings]
  )
  const searchModel = useMemo(
    () => buildChatSearchModelFromGroups(folders, searchGroups),
    [folders, searchGroups]
  )
  const sessionsById = useMemo(
    () => new Map(sessions.map((session) => [session.id, session])),
    [sessions]
  )
  const activeStatusCounts = useMemo(() => {
    const counts = { running: 0, attention: 0, done: 0 }
    for (const sessionId of searchModel.activeSessionIds) {
      const status = displayedChatStatus(
        [runtimeBindings.cliBindingBySessionId?.[sessionId]],
        [runtimeBindings.acpBindingBySessionId?.[sessionId]],
      )
      if (status?.type === 'processing') counts.running += 1
      else if (status?.type === 'attention') counts.attention += 1
      else if (status?.type === 'done') counts.done += 1
    }
    return counts
  }, [runtimeBindings, searchModel.activeSessionIds])
  const activeStatusSummary = [
    activeStatusCounts.running ? `${activeStatusCounts.running} running` : '',
    activeStatusCounts.attention ? `${activeStatusCounts.attention} attention` : '',
    activeStatusCounts.done ? `${activeStatusCounts.done} done` : '',
  ].filter(Boolean).join(' · ')
  const familySessionIdsByRootId = useMemo(() => {
    const families = new Map<string, string[]>()
    for (const session of sessions) {
      const rootId = session.branchRootChatId || session.parentChatId || session.id
      const family = families.get(rootId)
      if (family) family.push(session.id)
      else families.set(rootId, [session.id])
    }
    return families
  }, [sessions])
  const pendingDeleteFolder = useMemo(
    () => folders.find((folder) => folder.id === pendingDeleteFolderId) ?? null,
    [folders, pendingDeleteFolderId]
  )
  const pendingDeleteChatCount = useMemo(
    () => sessions.filter((session) => session.folderId === pendingDeleteFolderId).length,
    [sessions, pendingDeleteFolderId]
  )
  const unsortedExpanded = searchModel.isSearching || !unsortedCollapsed

  const visibleSessionRows = useCallback(() => Array.from(
    treeRef.current?.querySelectorAll<HTMLElement>('[data-session-id]') ?? []
  ).filter((row) => !row.closest('[inert]')), [])
  const visibleSessionIds = useCallback(() => visibleSessionRows()
    .map((row) => row.dataset.sessionId ?? '').filter(Boolean), [visibleSessionRows])

  const handleSessionClick = useCallback((sessionId: string, event: ReactMouseEvent<HTMLDivElement>) => {
    if (!event.shiftKey) {
      selectionAnchorRowRef.current = event.currentTarget
      setSelectionAnchorId(sessionId)
      setSelectedSessionIds(new Set())
      return false
    }

    event.preventDefault()
    const rows = visibleSessionRows()
    const anchorIndex = selectionAnchorId && selectionAnchorRowRef.current
      ? rows.indexOf(selectionAnchorRowRef.current)
      : -1
    const targetIndex = rows.indexOf(event.currentTarget)
    if (anchorIndex < 0 || targetIndex < 0) {
      selectionAnchorRowRef.current = event.currentTarget
      setSelectionAnchorId(sessionId)
      setSelectedSessionIds(new Set([sessionId]))
      return true
    }

    const start = Math.min(anchorIndex, targetIndex)
    const end = Math.max(anchorIndex, targetIndex)
    setSelectedSessionIds(new Set(rows.slice(start, end + 1).map((row) => row.dataset.sessionId ?? '').filter(Boolean)))
    return true
  }, [selectionAnchorId, visibleSessionRows])

  const clearBulkSelection = useCallback(() => {
    setSelectedSessionIds(new Set())
    setSelectionAnchorId(null)
    selectionAnchorRowRef.current = null
    setPendingBulkDeleteIds(null)
    setBulkDeleteFailed(false)
  }, [])

  const openWorkspaceRecovery = useCallback(() => {
    const requestId = ++recoveryRequestRef.current
    setUnsortedMenuPoint(null)
    setRecoveryDialogOpen(true)
    setRecoveryPreview(null)
    setRecoveryLoading(true)
    void previewUnsortedWorkspaceFolders().then((preview) => {
      if (recoveryRequestRef.current === requestId) setRecoveryPreview(preview)
    }).finally(() => {
      if (recoveryRequestRef.current === requestId) setRecoveryLoading(false)
    })
  }, [previewUnsortedWorkspaceFolders])

  const closeWorkspaceRecovery = useCallback(() => {
    if (recoveryApplying) return
    recoveryRequestRef.current++
    setRecoveryDialogOpen(false)
    unsortedMenuTriggerRef.current?.focus()
  }, [recoveryApplying])

  const applyWorkspaceRecovery = useCallback(() => {
    setRecoveryApplying(true)
    void rebuildUnsortedWorkspaceFolders().then((rebuilt) => {
      if (rebuilt) {
        setRecoveryDialogOpen(false)
        unsortedMenuTriggerRef.current?.focus()
      }
    }).finally(() => setRecoveryApplying(false))
  }, [rebuildUnsortedWorkspaceFolders])

  useEffect(() => {
    if (selectedSessionIds.size === 0) return
    const clearOnEscape = (event: KeyboardEvent) => {
      if (event.key === 'Escape') clearBulkSelection()
    }
    document.addEventListener('keydown', clearOnEscape)
    return () => document.removeEventListener('keydown', clearOnEscape)
  }, [clearBulkSelection, selectedSessionIds.size])

  useEffect(() => {
    const root = treeRef.current
    if (!root) return
    const keepVisibleSelection = () => {
      const visible = new Set(visibleSessionIds())
      setSelectedSessionIds((current) => {
        const next = new Set([...current].filter((id) => visible.has(id)))
        return next.size === current.size ? current : next
      })
      setSelectionAnchorId((current) => current && visible.has(current) ? current : null)
      if (selectionAnchorRowRef.current && !visibleSessionRows().includes(selectionAnchorRowRef.current)) {
        selectionAnchorRowRef.current = null
      }
    }
    const observer = new MutationObserver(keepVisibleSelection)
    observer.observe(root, { childList: true, subtree: true, attributes: true, attributeFilter: ['inert'] })
    keepVisibleSelection()
    return () => observer.disconnect()
  }, [visibleSessionIds, visibleSessionRows])

  useEffect(() => {
    if (pendingDeleteFolderId !== null && !pendingDeleteFolder) {
      setPendingDeleteFolderId(null)
      setDeleteFolderError('')
      setDeletingFolder(false)
    }
  }, [pendingDeleteFolder, pendingDeleteFolderId])

  const commitAddFolder = () => {
    const name = newFolderName.trim()
    const directory = newFolderDirectory.trim()

    if (!name || !directory) {
      return
    }

    setActionError('')
    void addFolder(name, null, directory).then((created) => {
      if (!created) {
        setActionError('The workspace could not be created. Check the directory and try again.')
        return
      }

      setNewFolderName('')
      setNewFolderDirectory('')
      setAddingFolder(false)
    })
  }

  const commitAddCollection = () => {
    const name = newCollectionName.trim()
    if (!name) return
    setActionError('')
    void createResourceCollection(name).then((created) => {
      if (!created) {
        setActionError('The collection could not be created. Try again.')
        return
      }
      setNewCollectionName('')
      setAddingCollection(false)
    })
  }

  const startRenameFolder = (folder: (typeof folders)[number]) => {
    setActionError('')
    setEditingFolderId(folder.id)
    setEditFolderName(folder.name)
    setEditFolderDirectory(folder.directory)
  }

  const commitRenameFolder = async (folderId: string) => {
    const name = editFolderName.trim()
    const directory = editFolderDirectory.trim()

    if (!name || !directory) {
      return
    }

    setActionError('')
    if (await renameFolder(folderId, name, directory)) {
      setEditingFolderId(null)
    } else {
      setActionError('The workspace could not be updated. Finish active work and try again.')
    }
  }

  const confirmDeleteFolder = async () => {
    if (!pendingDeleteFolder || deletingFolder) return
    setDeletingFolder(true)
    setDeleteFolderError('')
    try {
      if (await deleteFolder(pendingDeleteFolder.id)) {
        setPendingDeleteFolderId(null)
      } else {
        setDeleteFolderError('The workspace could not be deleted. Finish active work and try again.')
      }
    } catch {
      setDeleteFolderError('The workspace could not be deleted. Finish active work and try again.')
    } finally {
      setDeletingFolder(false)
    }
  }

  const chooseNewFolderDirectory = async () => {
    const selectedPath = await browseFolderDirectory(newFolderDirectory)
    if (selectedPath) {
      setNewFolderDirectory(selectedPath)
    }
  }

  const chooseEditFolderDirectory = async () => {
    const selectedPath = await browseFolderDirectory(editFolderDirectory)
    if (selectedPath) {
      setEditFolderDirectory(selectedPath)
    }
  }

  const folderRowsById = new Map(searchModel.folderRows.map((row) => [row.folder.id, row]))
  const groupedFolderIds = new Set<string>()
  const folderCollectionIds = new Map<string, string>()
  const collectionGroups = resourceCollections.map((collection) => ({
    collection,
    folderRows: collection.references.flatMap((reference) => {
      if (reference.type !== 'workspace-folder' || groupedFolderIds.has(reference.target)) return []
      const row = folderRowsById.get(reference.target)
      if (!row) return []
      groupedFolderIds.add(reference.target)
      folderCollectionIds.set(reference.target, collection.id)
      return [row]
    }),
  }))
  const ungroupedFolderRows = searchModel.folderRows.filter(({ folder }) => !groupedFolderIds.has(folder.id))

  const commitFolderMove = (sourceId: string, targetId: string, edge: FolderDropEdge) => {
    if (sourceId === targetId) return
    const sourceCollectionId = folderCollectionIds.get(sourceId)
    const targetCollectionId = folderCollectionIds.get(targetId)
    if (sourceCollectionId && sourceCollectionId === targetCollectionId) {
      const collection = resourceCollections.find((item) => item.id === sourceCollectionId)
      const referenceIds = collection
        ? reorderCollectionFolderReferences(collection, sourceId, targetId, edge)
        : null
      if (referenceIds) void reorderResourceReferences(sourceCollectionId, referenceIds)
      return
    }
    if (!sourceCollectionId && !targetCollectionId) {
      void reorderFolders(moveId(folders.map((folder) => folder.id), sourceId, targetId, edge))
    }
  }

  const moveFolderWithKeyboard = (sourceId: string, direction: -1 | 1) => {
    const collectionId = folderCollectionIds.get(sourceId)
    const rows = collectionId
      ? collectionGroups.find(({ collection }) => collection.id === collectionId)?.folderRows ?? []
      : ungroupedFolderRows
    const sourceIndex = rows.findIndex(({ folder }) => folder.id === sourceId)
    const target = rows[sourceIndex + direction]
    if (target) commitFolderMove(sourceId, target.folder.id, direction < 0 ? 'before' : 'after')
  }

  const renderFolderRow = ({ folder, sessionIds, shouldShowSessions }: (typeof searchModel.folderRows)[number]) => (
    <FolderRow
      key={folder.id}
      folder={folder}
      sessionIds={sessionIds}
      shouldShowSessions={shouldShowSessions}
      hiddenPaneColors={shouldShowSessions ? [] : paneColorsForFolder(folder.id)}
      isSearching={searchModel.isSearching}
      isEditing={editingFolderId === folder.id}
      editFolderName={editFolderName}
      editFolderDirectory={editFolderDirectory}
      onToggle={() => toggleFolder(folder.id)}
      onStartRename={() => startRenameFolder(folder)}
      onDelete={() => {
        setDeleteFolderError('')
        setPendingDeleteFolderId(folder.id)
      }}
      onRescan={() => {
        setActionError('')
        void rescanFolderChats(folder.id).then((rescanned) => {
          if (!rescanned) setActionError(`Could not rescan ${folder.name}. Try again.`)
        })
      }}
      onEditNameChange={setEditFolderName}
      onEditDirectoryChange={setEditFolderDirectory}
      onCommitRename={() => commitRenameFolder(folder.id)}
      onCancelEdit={() => setEditingFolderId(null)}
      onChooseDirectory={() => void chooseEditFolderDirectory()}
      onCreateChat={() => setNewChatModalOpen(true, folder.id)}
      onOpenMemory={() => void openFolderMemoryLibrary(folder.id)}
      sessionsById={sessionsById}
      familySessionIdsByRootId={familySessionIdsByRootId}
      selectedSessionIds={selectedSessionIds}
      onSessionClick={handleSessionClick}
      draggable={!searchModel.isSearching}
      dropEdge={folderDropTarget?.id === folder.id ? folderDropTarget.edge : null}
      onDragStart={() => setDraggedFolderId(folder.id)}
      onDragOver={(edge) => setFolderDropTarget({ id: folder.id, edge })}
      onDragEnd={() => { setDraggedFolderId(null); setFolderDropTarget(null) }}
      onMove={(direction) => moveFolderWithKeyboard(folder.id, direction)}
      onDrop={(edge) => {
        if (draggedFolderId) commitFolderMove(draggedFolderId, folder.id, edge)
        setDraggedFolderId(null)
        setFolderDropTarget(null)
      }}
    />
  )

  return (
    <div ref={treeRef} className="select-none">
      {selectedSessionIds.size > 0 && (
        <div className="mx-1 mb-1 flex items-center gap-2 rounded-md px-2.5 py-1" style={{ background: 'var(--surface-up)', border: '1px solid var(--border)' }}>
          <span className="min-w-0 flex-1 text-xs font-medium" style={{ color: 'var(--text-2)' }}>{selectedSessionIds.size} selected</span>
          <Button size="sm" variant="ghost" onClick={clearBulkSelection}>Clear</Button>
          <Button
            size="sm"
            variant="danger"
            aria-label={`Delete ${selectedSessionIds.size} selected chats`}
            onClick={() => {
              const visible = new Set(visibleSessionIds())
              const ids = [...selectedSessionIds].filter((id) => visible.has(id))
              if (ids.length > 0) {
                setBulkDeleteFailed(false)
                setPendingBulkDeleteIds(ids)
              }
            }}
          >
            Delete
          </Button>
        </div>
      )}
      {searchModel.activeSessionIds.length > 0 && (
        <div className="mb-0.5" data-testid="active-chats">
          <div className="flex flex-wrap items-center justify-between gap-x-2 px-2.5 py-0.5" style={{ color: 'var(--text-3)' }}>
            <span className="text-xs font-medium tracking-wider uppercase" style={{ letterSpacing: '0.08em', fontSize: 10 }}>
              Active chats
            </span>
            {activeStatusSummary && (
              <span aria-label="Active chat status counts" className="text-[10px]" title={activeStatusSummary}>
                {activeStatusSummary}
              </span>
            )}
          </div>
          {searchModel.activeSessionIds.map((id) => (
            <SessionItem key={id} sessionId={id} session={sessionsById.get(id)} familySessionIds={familySessionIdsByRootId.get(id)} selected={selectedSessionIds.has(id)} onSessionClick={handleSessionClick} />
          ))}
        </div>
      )}

      {searchModel.pinnedSessionIds.length > 0 && (
        <div className="mb-0.5">
          <div className="px-2.5 py-0.5" style={{ color: 'var(--text-3)' }}>
            <span className="text-xs font-medium tracking-wider uppercase" style={{ letterSpacing: '0.08em', fontSize: 10 }}>
              Pinned chats
            </span>
          </div>
          {searchModel.pinnedSessionIds.map((id) => (
            <SessionItem key={id} sessionId={id} session={sessionsById.get(id)} familySessionIds={familySessionIdsByRootId.get(id)} selected={selectedSessionIds.has(id)} onSessionClick={handleSessionClick} />
          ))}
        </div>
      )}

      {!searchModel.isSearching && (searchModel.folderRows.length > 0 || resourceCollections.length > 0) && (
        <div className="px-2.5 py-0" style={{ color: 'var(--text-3)' }}>
          <div style={{ display: 'flex', alignItems: 'center', gap: 10 }}>
            <span className="text-xs font-medium tracking-wider uppercase" style={{ letterSpacing: '0.08em', fontSize: 10, whiteSpace: 'nowrap' }}>
              All chats
            </span>
            <span style={{ height: 1, flex: 1, background: 'var(--border)' }} />
          </div>
        </div>
      )}

      {!searchModel.isSearching && collectionGroups.map(({ collection, folderRows }) => (
        <FolderCollection
          key={collection.id}
          collection={collection}
          folderCount={folderRows.length}
          hiddenPaneColors={paneColorsForFolders(new Set(folderRows.map(({ folder }) => folder.id)))}
          onFolderDrop={(folderId) => {
            const folder = folders.find((item) => item.id === folderId)
            if (folder) void moveFolderToCollection(collection.id, folder.id, folder.name)
          }}
        >
          {folderRows.map(renderFolderRow)}
        </FolderCollection>
      ))}

      {(searchModel.isSearching ? searchModel.folderRows : ungroupedFolderRows).map(renderFolderRow)}

      {/* Unfoldered sessions */}
      {searchModel.unfolderedSessionIds.length > 0 && (
        <div className="mt-0.5">
          <div
            className="mx-1 flex w-[calc(100%-0.5rem)] items-center rounded-md"
            onContextMenu={(event) => {
              event.preventDefault()
              setUnsortedMenuPoint({ x: event.clientX, y: event.clientY })
            }}
          >
            <button
              type="button"
              aria-label={`${unsortedExpanded ? 'Collapse' : 'Expand'} Unsorted`}
              aria-expanded={unsortedExpanded}
              aria-controls="unsorted-chat-list"
              className="flex min-w-0 flex-1 items-center gap-1.5 rounded-md px-2.5 py-0.5 text-left"
              style={{ background: 'transparent', border: 'none', color: 'var(--text-2)', cursor: 'pointer' }}
              onClick={() => setUnsortedCollapsed((collapsed) => !collapsed)}
            >
              <ChevronRight
                size={14}
                className="transition-transform duration-200 ease-out motion-reduce:transition-none"
                style={{ flexShrink: 0, color: 'var(--text-3)', transform: unsortedExpanded ? 'rotate(90deg)' : 'rotate(0deg)' }}
                aria-hidden
              />
              {unsortedExpanded
                ? <FolderOpenIcon size={14} style={{ flexShrink: 0, color: 'var(--text-3)', opacity: 0.85 }} aria-hidden />
                : <FolderIcon size={14} style={{ flexShrink: 0, color: 'var(--text-3)', opacity: 0.85 }} aria-hidden />}
              <span className="min-w-0 flex-1 truncate text-[13px] font-semibold">Unsorted</span>
              <span className="rounded px-1 text-[10px]" style={{ background: 'var(--surface-high)', color: 'var(--text-3)' }}>
                {searchModel.unfolderedSessionIds.length}
              </span>
            </button>
            <Tooltip label="Unsorted actions">
              <button
                ref={unsortedMenuTriggerRef}
                type="button"
                aria-label="Actions for Unsorted"
                aria-haspopup="menu"
                aria-expanded={unsortedMenuPoint !== null}
                className="mr-1 flex h-[22px] w-[22px] items-center justify-center rounded"
                style={{ background: 'transparent', border: 'none', color: 'var(--text-3)', cursor: 'pointer' }}
                onClick={(event) => {
                  if (unsortedMenuPoint) {
                    setUnsortedMenuPoint(null)
                    return
                  }
                  const rect = event.currentTarget.getBoundingClientRect()
                  setUnsortedMenuPoint({ x: rect.right, y: rect.bottom + 4 })
                }}
              >
                <MoreHorizontal size={14} aria-hidden />
              </button>
            </Tooltip>
          </div>
          {unsortedMenuPoint && (
            <ViewportMenu
              ref={unsortedMenuRef}
              point={unsortedMenuPoint}
              role="menu"
              aria-label="Unsorted actions"
              align="end"
              className="rounded-md py-1 animate-fade-in"
              style={{ minWidth: 220, background: 'var(--surface-up)', border: '1px solid var(--border-bright)', boxShadow: 'var(--elev-2)' }}
            >
              <button
                type="button"
                role="menuitem"
                className="uam-menu-select__option flex w-full items-center gap-2 px-3 py-1.5 text-left text-sm"
                style={{ color: 'var(--text-2)', border: 'none', fontFamily: 'inherit' }}
                onClick={openWorkspaceRecovery}
              >
                <FolderSync size={14} aria-hidden />
                Rebuild workspace folders…
              </button>
            </ViewportMenu>
          )}
          <div
            id="unsorted-chat-list"
            aria-hidden={!unsortedExpanded}
            {...(!unsortedExpanded ? { inert: '' } : {})}
            className={`grid transition-[grid-template-rows,opacity] duration-200 ease-out motion-reduce:transition-none ${unsortedExpanded ? 'grid-rows-[1fr] opacity-100' : 'grid-rows-[0fr] opacity-0'}`}
          >
            <div className="min-h-0 overflow-hidden">
              {searchModel.unfolderedSessionIds.map((id) => (
                <SessionItem key={id} sessionId={id} session={sessionsById.get(id)} familySessionIds={familySessionIdsByRootId.get(id)} selected={selectedSessionIds.has(id)} onSessionClick={handleSessionClick} />
              ))}
            </div>
          </div>
        </div>
      )}

      {searchModel.isSearching && !searchModel.hasMatches && (
        <div className="mx-3 my-6 flex flex-col items-center gap-1 text-center animate-fade-in" style={{ color: 'var(--text-3)' }}>
          <SearchX size={22} strokeWidth={1.5} aria-hidden />
          <div className="text-xs font-medium" style={{ color: 'var(--text-2)' }}>No chats match this search</div>
          <div className="text-[11px]">Try another term or clear the filters.</div>
        </div>
      )}

      {pendingBulkDeleteIds && (
        <div className="fixed inset-0 z-[70] flex items-center justify-center p-4 animate-fade-in" style={{ background: 'rgba(0,0,0,.5)' }} onClick={(event) => { if (event.target === event.currentTarget) setPendingBulkDeleteIds(null) }}>
          <div role="alertdialog" aria-modal="true" aria-label={`Delete ${pendingBulkDeleteIds.length} selected chats`} className="w-full max-w-md rounded-xl animate-slide-in" style={{ background: 'var(--surface)', border: '1px solid var(--border-bright)', boxShadow: 'var(--elev-3)' }}>
            <div className="px-5 py-4 text-sm font-semibold" style={{ color: 'var(--text)', borderBottom: '1px solid var(--border)' }}>Delete selected chats?</div>
            <div className="p-5 text-sm" style={{ color: 'var(--text-2)' }}>
              {pendingBulkDeleteIds.length} chats will be permanently deleted. This cannot be undone.
              {bulkDeleteFailed && <div className="mt-2" role="alert" style={{ color: 'var(--red)' }}>The chats could not be deleted. A chat may still be running.</div>}
            </div>
            <div className="flex justify-end gap-2 px-5 py-4" style={{ borderTop: '1px solid var(--border)' }}>
              <Button size="sm" onClick={() => setPendingBulkDeleteIds(null)}>Cancel</Button>
              <Button size="sm" variant="danger" onClick={() => {
                void deleteSessions(pendingBulkDeleteIds).then((deleted) => {
                  if (deleted) clearBulkSelection()
                  else setBulkDeleteFailed(true)
                })
              }}>Delete chats</Button>
            </div>
          </div>
        </div>
      )}

      {recoveryDialogOpen && (
        <WorkspaceFolderRecoveryModal
          dialogRef={recoveryDialogRef}
          preview={recoveryPreview}
          loading={recoveryLoading}
          applying={recoveryApplying}
          error={workspaceFolderRecoveryError}
          onCancel={closeWorkspaceRecovery}
          onApply={applyWorkspaceRecovery}
          onDeleteUnavailable={(ids) => {
            recoveryRequestRef.current++
            setRecoveryDialogOpen(false)
            setBulkDeleteFailed(false)
            setPendingBulkDeleteIds(ids)
          }}
        />
      )}

      {/* Add folder */}
      <div ref={addControlsRef} className="mt-1 px-2.5">
        {actionError && <div role="alert" className="mb-2 text-xs" style={{ color: 'var(--red)' }}>{actionError}</div>}
        {addingFolder ? (
          <div
            className="rounded-md p-2 space-y-2"
            style={{
              background: 'var(--surface-up)',
              border: '1px solid var(--border)',
            }}
          >
            <input
              autoFocus
              value={newFolderName}
              onChange={(e) => setNewFolderName(e.target.value)}
              onKeyDown={(e) => {
                if (e.key === 'Enter') commitAddFolder()
                if (e.key === 'Escape') {
                  setNewFolderName('')
                  setNewFolderDirectory('')
                  setAddingFolder(false)
                }
              }}
              placeholder="Folder name"
              className="w-full rounded px-2 py-1 text-xs outline-none"
              style={{
                background: 'var(--surface)',
                color: 'var(--text)',
                border: '1px solid var(--border)',
                fontFamily: 'inherit',
              }}
            />
            <div className="flex items-center gap-2">
              <input
                value={newFolderDirectory}
                onChange={(e) => setNewFolderDirectory(e.target.value)}
                onKeyDown={(e) => {
                  if (e.key === 'Enter') commitAddFolder()
                  if (e.key === 'Escape') {
                    setNewFolderName('')
                    setNewFolderDirectory('')
                    setAddingFolder(false)
                  }
                }}
                placeholder="Workspace directory"
                className="w-full flex-1 rounded px-2 py-1 text-xs outline-none"
                style={{
                  background: 'var(--surface)',
                  color: 'var(--text)',
                  border: '1px solid var(--border)',
                  fontFamily: 'inherit',
                }}
              />
              <Button
                variant="secondary"
                size="sm"
                onClick={() => { void chooseNewFolderDirectory() }}
              >
                Browse
              </Button>
            </div>
            <div className="flex items-center justify-end gap-2">
              <Button
                variant="ghost"
                size="sm"
                onClick={() => {
                  setNewFolderName('')
                  setNewFolderDirectory('')
                  setAddingFolder(false)
                }}
              >
                Cancel
              </Button>
              <Button
                variant="primary"
                size="sm"
                disabled={!newFolderName.trim() || !newFolderDirectory.trim()}
                onClick={commitAddFolder}
              >
                Create
              </Button>
            </div>
          </div>
        ) : addingCollection ? (
          <div className="flex items-center gap-1">
            <input
              autoFocus
              aria-label="Collection name"
              value={newCollectionName}
              onChange={(event) => setNewCollectionName(event.target.value)}
              onKeyDown={(event) => {
                if (event.key === 'Enter') commitAddCollection()
                if (event.key === 'Escape') { setNewCollectionName(''); setAddingCollection(false) }
              }}
              placeholder="Collection name"
              className="min-w-0 flex-1 rounded px-2 py-1 text-xs outline-none"
              style={{ background: 'var(--surface)', color: 'var(--text)', border: '1px solid var(--border)' }}
            />
            <IconButton
              icon={<Check size={14} />}
              label="Create collection"
              size="sm"
              disabled={!newCollectionName.trim()}
              onClick={commitAddCollection}
              style={{ background: 'var(--accent)', borderColor: 'var(--accent)', color: 'white' }}
            />
            <IconButton
              icon={<X size={14} />}
              label="Cancel new collection"
              variant="danger"
              size="sm"
              onClick={() => { setNewCollectionName(''); setAddingCollection(false) }}
              style={{ color: 'var(--error)' }}
            />
          </div>
        ) : (
          <div className="flex justify-center gap-4">
            <button
              onClick={() => { setActionError(''); setAddingFolder(true) }}
              className="flex items-center gap-1.5 text-xs transition-colors duration-100"
              style={{ color: 'var(--text-3)', background: 'transparent', border: 'none', cursor: 'pointer', fontFamily: 'inherit', padding: '2px 0' }}
            >
              <Plus size={14} aria-hidden />
              <span>New workspace</span>
            </button>
            <button
              onClick={() => { setActionError(''); setAddingCollection(true) }}
              className="flex items-center gap-1.5 text-xs transition-colors duration-100"
              style={{ color: 'var(--text-3)', background: 'transparent', border: 'none', cursor: 'pointer', fontFamily: 'inherit', padding: '2px 0' }}
            >
              <Plus size={14} aria-hidden />
              <span>New collection</span>
            </button>
          </div>
        )}
      </div>

      {pendingDeleteFolder && (
        <DeleteFolderModal
          folder={pendingDeleteFolder}
          chatCount={pendingDeleteChatCount}
          error={deleteFolderError}
          deleting={deletingFolder}
          onCancel={() => {
            if (deletingFolder) return
            setPendingDeleteFolderId(null)
            setDeleteFolderError('')
          }}
          onConfirm={() => { void confirmDeleteFolder() }}
        />
      )}
    </div>
  )
}

function FolderCollection({ collection, folderCount, hiddenPaneColors, onFolderDrop, children }: { collection: ResourceCollection; folderCount: number; hiddenPaneColors: string[]; onFolderDrop: (folderId: string) => void; children: ReactNode }) {
  const toggle = useAppStore((state) => state.toggleResourceCollection)
  const rename = useAppStore((state) => state.renameResourceCollection)
  const remove = useAppStore((state) => state.deleteResourceCollection)
  const [editing, setEditing] = useState(false)
  const [name, setName] = useState(collection.name)
  const [menuOpen, setMenuOpen] = useState(false)
  const [menuPoint, setMenuPoint] = useState<{ x: number; y: number } | null>(null)
  const [dropTarget, setDropTarget] = useState(false)
  const [confirmingDelete, setConfirmingDelete] = useState(false)
  const activeSessionId = useAppStore((state) => state.activeSessionId)
  const menuTriggerRef = useRef<HTMLButtonElement>(null)
  const menuRef = useRef<HTMLDivElement>(null)

  useEffect(() => { setMenuOpen(false) }, [activeSessionId])

  useEffect(() => {
    if (!menuOpen) return
    menuRef.current?.querySelector<HTMLButtonElement>('button')?.focus()
    const onDown = (event: MouseEvent) => {
      const target = event.target as Node
      if (!menuRef.current?.contains(target) && !menuTriggerRef.current?.contains(target)) { setMenuOpen(false); setMenuPoint(null) }
    }
    const onKey = (event: KeyboardEvent) => {
      if (event.key !== 'Escape') return
      setMenuOpen(false)
      setMenuPoint(null)
      menuTriggerRef.current?.focus()
    }
    document.addEventListener('mousedown', onDown)
    document.addEventListener('keydown', onKey)
    return () => {
      document.removeEventListener('mousedown', onDown)
      document.removeEventListener('keydown', onKey)
    }
  }, [menuOpen])

  const commitName = () => {
    const trimmed = name.trim()
    if (trimmed && trimmed !== collection.name) void rename(collection.id, trimmed)
    else setName(collection.name)
    setEditing(false)
  }

  return (
    <div
      className="mb-0"
      data-testid={`folder-collection-${collection.id}`}
      onDragOver={(event) => {
        if (!event.dataTransfer.types.includes('text/x-uam-folder-resource-id')) return
        event.preventDefault()
        setDropTarget(true)
      }}
      onDragLeave={() => setDropTarget(false)}
      onDrop={(event) => {
        const folderId = event.dataTransfer.getData('text/x-uam-folder-resource-id')
        if (!folderId) return
        event.preventDefault()
        event.stopPropagation()
        setDropTarget(false)
        onFolderDrop(folderId)
      }}
    >
      <div
        data-testid={`collection-header-${collection.id}`}
        className="group relative mx-1 flex items-center gap-1.5 rounded-md px-2.5 py-0.5 transition-colors duration-150"
        style={{ color: 'var(--text-2)', background: dropTarget ? 'var(--sidebar-item-hover)' : 'transparent', border: '1px solid transparent', outline: dropTarget ? '1px solid var(--accent)' : 'none' }}
        onContextMenu={(event) => {
          event.preventDefault()
          event.stopPropagation()
          setMenuPoint({ x: event.clientX, y: event.clientY })
          setMenuOpen(true)
        }}
      >
        <Tooltip label={collection.collapsed ? `Expand ${collection.name}` : `Collapse ${collection.name}`}>
          <button
            type="button"
            aria-label={collection.collapsed ? `Expand ${collection.name}` : `Collapse ${collection.name}`}
            aria-expanded={!collection.collapsed}
            onClick={() => { void toggle(collection.id) }}
            style={{ display: 'flex', background: 'transparent', border: 'none', color: 'var(--text-3)', cursor: 'pointer', padding: 0 }}
          >
            <ChevronRight size={14} className="transition-transform duration-200 ease-out motion-reduce:transition-none" style={{ transform: collection.collapsed ? 'rotate(0deg)' : 'rotate(90deg)' }} />
          </button>
        </Tooltip>
        <PaneColorIcon
          Icon={Library}
          colors={collection.collapsed ? hiddenPaneColors : []}
          testId={`collection-icon-${collection.id}`}
        />
        {editing ? (
          <input
            autoFocus
            aria-label={`Rename ${collection.name}`}
            value={name}
            onChange={(event) => setName(event.target.value)}
            onBlur={commitName}
            onKeyDown={(event) => {
              if (event.key === 'Enter') commitName()
              if (event.key === 'Escape') { setName(collection.name); setEditing(false) }
            }}
            className="min-w-0 flex-1 bg-transparent text-xs outline-none"
            style={{ color: 'var(--text)', borderBottom: '1px solid var(--accent)' }}
          />
        ) : (
          <Tooltip label={collection.name}>
            <button type="button" className="min-w-0 flex-1 truncate text-left text-[13px] font-semibold" onClick={() => { void toggle(collection.id) }} style={{ background: 'transparent', border: 'none', color: 'inherit', cursor: 'pointer' }}>
              {collection.name}
            </button>
          </Tooltip>
        )}
        <span className="text-[10px]" style={{ color: 'var(--text-3)' }}>{folderCount}</span>
        <Tooltip label="Collection actions">
          <button ref={menuTriggerRef} type="button" aria-label={`Actions for ${collection.name}`} aria-haspopup="menu" aria-expanded={menuOpen} onClick={() => { setMenuPoint(null); setMenuOpen((open) => !open) }} style={{ background: 'transparent', border: 'none', color: 'var(--text-3)', cursor: 'pointer', padding: 0 }}>
            <MoreHorizontal size={14} />
          </button>
        </Tooltip>
      </div>
      {menuOpen && (
        <ViewportMenu ref={menuRef} {...(menuPoint ? { point: menuPoint } : { anchorRef: menuTriggerRef, align: 'end' as const })} role="menu" aria-label={`${collection.name} actions`} className="rounded-md py-1" style={{ minWidth: 140, background: 'var(--surface-up)', border: '1px solid var(--border)', boxShadow: 'var(--elev-2)' }}>
          <button type="button" role="menuitem" className="uam-menu-select__option flex w-full items-center gap-2 rounded px-2 py-1 text-xs" style={{ border: 'none', color: 'var(--text-2)' }} onClick={() => { setMenuOpen(false); setEditing(true) }}><Pencil size={12} />Rename</button>
          <button type="button" role="menuitem" className="uam-menu-select__option flex w-full items-center gap-2 rounded px-2 py-1 text-xs" style={{ border: 'none', color: 'var(--red)' }} onClick={() => { setMenuOpen(false); setConfirmingDelete(true) }}><Trash2 size={12} />Delete</button>
        </ViewportMenu>
      )}
      <div
        aria-hidden={collection.collapsed}
        {...(collection.collapsed ? { inert: '' } : {})}
        className={`grid transition-[grid-template-rows,opacity] duration-200 ease-out motion-reduce:transition-none ${collection.collapsed ? 'grid-rows-[0fr] opacity-0' : 'grid-rows-[1fr] opacity-100'}`}
      >
        <div className="min-h-0 overflow-hidden">
          <div
            data-testid={`collection-children-${collection.id}`}
            className="ml-3 mr-1 mt-0 py-0 pl-1"
            style={{
              background: 'transparent',
              boxShadow: 'none',
            }}
          >
            {children}
            {folderCount === 0 && <div className="px-3 py-2 text-xs" style={{ color: 'var(--text-3)' }}>Drag a workspace here</div>}
          </div>
        </div>
      </div>
      {confirmingDelete && (
        <div className="fixed inset-0 z-[70] flex items-center justify-center p-4 animate-fade-in" style={{ background: 'rgba(0,0,0,.5)' }} onClick={(event) => { if (event.target === event.currentTarget) setConfirmingDelete(false) }}>
          <div role="alertdialog" aria-modal="true" aria-label={`Delete ${collection.name} collection`} className="w-full max-w-md rounded-xl animate-slide-in" style={{ background: 'var(--surface)', border: '1px solid var(--border-bright)', boxShadow: 'var(--elev-3)' }}>
            <div className="px-5 py-4 text-sm font-semibold" style={{ color: 'var(--text)', borderBottom: '1px solid var(--border)' }}>Delete collection?</div>
            <div className="p-5 text-sm" style={{ color: 'var(--text-2)' }}>“{collection.name}” will be permanently deleted. Workspaces remain, but this collection cannot be restored.</div>
            <div className="flex justify-end gap-2 px-5 py-4" style={{ borderTop: '1px solid var(--border)' }}><Button size="sm" onClick={() => setConfirmingDelete(false)}>Cancel</Button><Button size="sm" variant="danger" onClick={() => { setConfirmingDelete(false); void remove(collection.id) }}>Delete collection</Button></div>
          </div>
        </div>
      )}
    </div>
  )
}

function PaneColorIcon({ Icon, colors, testId }: { Icon: LucideIcon; colors: string[]; testId: string }) {
  const paneNumbers = colors.flatMap((color) => {
    const index = chatPaneColors.findIndex((paneColor) => paneColor === color)
    return index < 0 ? [] : [index + 1]
  })
  const paneLabel = paneNumbers.length === 1
    ? `Shown in pane ${paneNumbers[0]}`
    : `Shown in panes ${paneNumbers.slice(0, -1).join(', ')} and ${paneNumbers[paneNumbers.length - 1]}`
  return (
    <span
      {...(colors.length > 0 ? { role: 'img', 'aria-label': paneLabel } : {})}
      className="inline-flex shrink-0 items-center gap-1"
    >
      <Icon
        data-testid={testId}
        size={14}
        style={{ flexShrink: 0, color: 'var(--text-3)', filter: 'none', opacity: 0.85 }}
        aria-hidden
      />
      {colors.map((color, index) => (
        <span
          key={`${color}-${index}`}
          data-testid={`${testId}-marker`}
          className="h-1.5 w-1.5 rounded-full"
          style={{ background: color }}
          aria-hidden
        />
      ))}
    </span>
  )
}

interface DeleteFolderModalProps {
  folder: Folder
  chatCount: number
  error: string
  deleting: boolean
  onCancel: () => void
  onConfirm: () => void
}

interface WorkspaceFolderRecoveryModalProps {
  dialogRef: RefObject<HTMLDivElement>
  preview: WorkspaceFolderRecoveryPreview | null
  loading: boolean
  applying: boolean
  error: string
  onCancel: () => void
  onApply: () => void
  onDeleteUnavailable: (ids: string[]) => void
}

function WorkspaceFolderRecoveryModal({
  dialogRef,
  preview,
  loading,
  applying,
  error,
  onCancel,
  onApply,
  onDeleteUnavailable,
}: WorkspaceFolderRecoveryModalProps) {
  const readyChatCount = preview?.groups.reduce((count, group) => count + group.chatIds.length, 0) ?? 0
  const newFolderCount = preview?.groups.filter((group) => !group.existingFolderId).length ?? 0
  const unavailableChats = [...(preview?.missing ?? []), ...(preview?.unavailable ?? [])]
  const unavailableIds = Array.from(new Set(unavailableChats.map((chat) => chat.id)))
  const attentionSections = preview ? [
    { title: 'Location not found', chats: preview.missing },
    { title: 'Location unavailable', chats: preview.unavailable },
    { title: 'No saved location', chats: preview.noLocation },
  ].filter((section) => section.chats.length > 0) : []
  const hasResults = (preview?.groups.length ?? 0) > 0 || attentionSections.length > 0

  return (
    <div
      className="fixed inset-0 z-[80] flex items-center justify-center p-4 animate-fade-in"
      style={{ background: 'rgba(0,0,0,.55)' }}
      onClick={(event) => { if (event.target === event.currentTarget && !applying) onCancel() }}
    >
      <div
        ref={dialogRef}
        role="dialog"
        aria-modal="true"
        aria-label="Rebuild workspace folders"
        tabIndex={-1}
        className="flex max-h-[calc(100vh-2rem)] w-full max-w-2xl flex-col overflow-hidden rounded-xl animate-slide-in"
        style={{ background: 'var(--surface)', border: '1px solid var(--border-bright)', boxShadow: 'var(--elev-3)' }}
      >
        <div className="flex items-start justify-between gap-4 px-5 py-4" style={{ borderBottom: '1px solid var(--border)' }}>
          <div>
            <div className="text-sm font-semibold" style={{ color: 'var(--text)' }}>Rebuild workspace folders</div>
            <div className="mt-1 text-xs" style={{ color: 'var(--text-3)' }}>Review what will change before organising Unsorted chats.</div>
          </div>
          <IconButton icon={<X size={16} />} label="Close workspace recovery" disabled={applying} onClick={onCancel} />
        </div>

        <div className="min-h-0 flex-1 overflow-y-auto p-5">
          {loading && (
            <div role="status" className="flex items-center gap-2 rounded-md p-3 text-sm" style={{ color: 'var(--text-2)', background: 'var(--surface-up)', border: '1px solid var(--border)' }}>
              <FolderSync size={16} className="animate-spin" aria-hidden />
              Checking saved workspace locations…
            </div>
          )}
          {error && <div role="alert" className="rounded-md p-3 text-sm" style={{ color: 'var(--red)', background: 'var(--surface-up)', border: '1px solid var(--border)' }}>{error}</div>}
          {!loading && preview && (
            <div className="grid gap-4">
              {preview.groups.length > 0 && (
                <section aria-label="Ready workspace folders">
                  <div className="mb-2 flex items-center gap-2">
                    <Check size={15} style={{ color: 'var(--green)' }} aria-hidden />
                    <h3 className="text-xs font-semibold" style={{ color: 'var(--text)' }}>Ready · {readyChatCount} chat{readyChatCount === 1 ? '' : 's'}</h3>
                  </div>
                  <div className="grid gap-2">
                    {preview.groups.map((group) => (
                      <div key={group.directory} className="rounded-md px-3 py-2" style={{ background: 'var(--surface-up)', border: '1px solid var(--border)' }}>
                        <div className="flex items-center justify-between gap-3 text-xs">
                          <strong className="truncate" style={{ color: 'var(--text)' }}>{group.title}</strong>
                          <span className="shrink-0" style={{ color: 'var(--text-3)' }}>{group.existingFolderId ? 'Use existing' : 'Create'} · {group.chatIds.length}</span>
                        </div>
                        <div className="mt-1 truncate text-[11px]" title={group.directory} style={{ color: 'var(--text-3)' }}>{group.directory}</div>
                      </div>
                    ))}
                  </div>
                </section>
              )}

              {attentionSections.length > 0 && (
                <section aria-label="Chats needing attention">
                  <div className="mb-2 flex items-center gap-2">
                    <TriangleAlert size={15} style={{ color: 'var(--yellow)' }} aria-hidden />
                    <h3 className="text-xs font-semibold" style={{ color: 'var(--text)' }}>Needs attention</h3>
                  </div>
                  <div className="grid gap-2">
                    {attentionSections.map((section) => (
                      <div key={section.title} className="rounded-md px-3 py-2" style={{ background: 'var(--surface-up)', border: '1px solid var(--border)' }}>
                        <div className="text-xs font-medium" style={{ color: 'var(--text-2)' }}>{section.title} · {section.chats.length}</div>
                        {section.chats.map((chat) => (
                          <div key={chat.id} className="mt-1 min-w-0 text-[11px]" style={{ color: 'var(--text-3)' }}>
                            <span style={{ color: 'var(--text-2)' }}>{chat.title}</span>
                            {chat.directory && <span className="block truncate" title={chat.directory}>{chat.directory}</span>}
                            <span className="block">{chat.reason}</span>
                          </div>
                        ))}
                      </div>
                    ))}
                  </div>
                  <p className="mt-2 text-xs" style={{ color: 'var(--text-3)' }}>These chats stay in Unsorted unless you explicitly delete the unavailable ones.</p>
                </section>
              )}

              {!hasResults && <div className="rounded-md p-4 text-center text-sm" style={{ color: 'var(--text-3)', background: 'var(--surface-up)', border: '1px solid var(--border)' }}>No Unsorted chats need workspace recovery.</div>}
            </div>
          )}
        </div>

        <div className="flex flex-wrap items-center justify-end gap-2 px-5 py-4" style={{ borderTop: '1px solid var(--border)' }}>
          {unavailableIds.length > 0 && (
            <Button variant="danger" size="sm" disabled={applying} onClick={() => onDeleteUnavailable(unavailableIds)}>
              Delete {unavailableIds.length} unavailable…
            </Button>
          )}
          <Button variant="ghost" size="sm" disabled={applying} onClick={onCancel}>Keep unchanged</Button>
          <Button variant="primary" size="sm" loading={applying} disabled={loading || readyChatCount === 0} onClick={onApply}>
            {newFolderCount > 0
              ? `Create ${newFolderCount} folder${newFolderCount === 1 ? '' : 's'} and organise ${readyChatCount} chat${readyChatCount === 1 ? '' : 's'}`
              : `Organise ${readyChatCount} chat${readyChatCount === 1 ? '' : 's'}`}
          </Button>
        </div>
      </div>
    </div>
  )
}

function DeleteFolderModal({
  folder,
  chatCount,
  error,
  deleting,
  onCancel,
  onConfirm,
}: DeleteFolderModalProps) {
  const chatLabel = chatCount === 1 ? '1 chat' : `${chatCount} chats`

  useEffect(() => {
    const handler = (e: KeyboardEvent) => {
      if (e.key === 'Escape' && !deleting) {
        onCancel()
      }
    }

    window.addEventListener('keydown', handler)
    return () => window.removeEventListener('keydown', handler)
  }, [deleting, onCancel])

  return (
    <div
      className="fixed inset-0 z-50 flex items-center justify-center animate-fade-in"
      style={{ background: 'rgba(0,0,0,0.55)', backdropFilter: 'blur(4px)' }}
      onClick={(e) => {
        if (e.target === e.currentTarget && !deleting) onCancel()
      }}
    >
      <div
        role="dialog"
        aria-modal="true"
        aria-label="Delete folder and chats"
        tabIndex={-1}
        className="rounded-xl shadow-2xl w-full max-w-md mx-4 animate-slide-in"
        style={{
          background: 'var(--surface)',
          border: '1px solid var(--border-bright)',
        }}
      >
        <div
          className="flex items-center justify-between px-5 py-4"
          style={{ borderBottom: '1px solid var(--border)' }}
        >
          <span className="text-sm font-semibold" style={{ color: 'var(--text)' }}>
            Delete folder and chats?
          </span>
          <IconButton icon={<X size={16} />} label="Close delete folder dialog" disabled={deleting} onClick={onCancel} />
        </div>

        <div className="p-5 space-y-4">
          <p className="text-sm" style={{ color: 'var(--text)' }}>
            This removes "{folder.name}" from Universal Agent Manager and deletes {chatLabel} inside it.
            This will not delete the actual workspace directory from your computer.
          </p>

          {folder.directory && (
            <div
              className="rounded-md px-3 py-2"
              style={{
                background: 'var(--surface-up)',
                border: '1px solid var(--border)',
              }}
            >
              <div className="text-[10px] uppercase" style={{ color: 'var(--text-3)', letterSpacing: '0.08em' }}>
                Directory stays on disk
              </div>
              <div className="truncate text-xs mt-1" style={{ color: 'var(--text-2)' }}>
                {folder.directory}
              </div>
            </div>
          )}

          {chatCount > 0 && (
            <p className="text-xs" style={{ color: 'var(--red)' }}>
              Deleted chats cannot be restored from this app.
            </p>
          )}
          {error && <p role="alert" className="text-xs" style={{ color: 'var(--red)' }}>{error}</p>}
        </div>

        <div
          className="flex items-center justify-end gap-2 px-5 py-4"
          style={{ borderTop: '1px solid var(--border)' }}
        >
          <Button
            variant="ghost"
            size="md"
            disabled={deleting}
            onClick={onCancel}
          >
            Cancel
          </Button>
          <Button
            variant="danger"
            size="md"
            loading={deleting}
            onClick={onConfirm}
          >
            Delete Folder
          </Button>
        </div>
      </div>
    </div>
  )
}

// ---------------------------------------------------------------------------
// FolderRow - memoized so it only re-renders when its own folder/session IDs change
// ---------------------------------------------------------------------------

function FolderMenuItem({ icon, label, onClick, danger }: { icon: ReactNode; label: string; onClick: () => void; danger?: boolean }) {
  return (
    <button
      type="button"
      role="menuitem"
      className="uam-menu-select__option flex w-full items-center gap-2 px-3 py-1.5 text-sm text-left"
      style={{ color: danger ? 'var(--red)' : 'var(--text-2)', border: 'none', fontFamily: 'inherit' }}
      onClick={onClick}
    >
      {icon}
      {label}
    </button>
  )
}

interface FolderRowProps {
  folder: Folder
  sessionIds: string[]
  shouldShowSessions: boolean
  hiddenPaneColors: string[]
  isSearching: boolean
  isEditing: boolean
  editFolderName: string
  editFolderDirectory: string
  onToggle: () => void
  onStartRename: () => void
  onDelete: () => void
  onRescan: () => void
  onEditNameChange: (v: string) => void
  onEditDirectoryChange: (v: string) => void
  onCommitRename: () => void
  onCancelEdit: () => void
  onChooseDirectory: () => void
  onCreateChat: () => void
  onOpenMemory: () => void
  sessionsById: Map<string, Session>
  familySessionIdsByRootId: Map<string, string[]>
  selectedSessionIds: Set<string>
  onSessionClick: (sessionId: string, event: ReactMouseEvent<HTMLDivElement>) => boolean
  draggable: boolean
  dropEdge: FolderDropEdge | null
  onDragStart: () => void
  onDragOver: (edge: FolderDropEdge) => void
  onDragEnd: () => void
  onMove: (direction: -1 | 1) => void
  onDrop: (edge: FolderDropEdge) => void
}

const FolderRow = memo(function FolderRow({
  folder,
  sessionIds,
  shouldShowSessions,
  hiddenPaneColors,
  isSearching,
  isEditing,
  editFolderName,
  editFolderDirectory,
  onToggle,
  onStartRename,
  onDelete,
  onRescan,
  onEditNameChange,
  onEditDirectoryChange,
  onCommitRename,
  onCancelEdit,
  onChooseDirectory,
  onCreateChat,
  onOpenMemory,
  sessionsById,
  familySessionIdsByRootId,
  selectedSessionIds,
  onSessionClick,
  draggable,
  dropEdge,
  onDragStart,
  onDragOver,
  onDragEnd,
  onMove,
  onDrop,
}: FolderRowProps) {
  const [showAllSessions, setShowAllSessions] = useState(false)
  const [menuPos, setMenuPos] = useState<{ x: number; y: number } | null>(null)
  const menuRef = useRef<HTMLDivElement>(null)
  const menuTriggerRef = useRef<HTMLButtonElement>(null)
  const shouldLimitSessions = !isSearching && sessionIds.length > VISIBLE_SESSION_LIMIT
  const visibleSessionIds = shouldLimitSessions && !showAllSessions
    ? sessionIds.slice(0, VISIBLE_SESSION_LIMIT)
    : sessionIds
  const hiddenSessionCount = sessionIds.length - visibleSessionIds.length

  useEffect(() => {
    if (sessionIds.length <= VISIBLE_SESSION_LIMIT && showAllSessions) {
      setShowAllSessions(false)
    }
  }, [sessionIds.length, showAllSessions])

  useEffect(() => {
    if (!menuPos) return
    const onDown = (e: MouseEvent) => {
      const target = e.target as Node
      if (target instanceof Element && target.closest('[data-viewport-menu]')) return
      if (menuRef.current && !menuRef.current.contains(target)) setMenuPos(null)
    }
    const onKey = (e: KeyboardEvent) => { if (e.key === 'Escape') setMenuPos(null) }
    document.addEventListener('mousedown', onDown)
    document.addEventListener('keydown', onKey)
    return () => {
      document.removeEventListener('mousedown', onDown)
      document.removeEventListener('keydown', onKey)
    }
  }, [menuPos])

  return (
    <div
      className="mb-0"
      data-testid={`folder-row-${folder.id}`}
      draggable={draggable}
      onDragStart={(event) => {
        event.dataTransfer.effectAllowed = 'move'
        event.dataTransfer.setData('text/x-uam-folder-id', folder.id)
        event.dataTransfer.setData('text/x-uam-folder-resource-id', folder.id)
        onDragStart()
      }}
      onDragOver={(event) => {
        if (!draggable) return
        event.preventDefault()
        const rect = event.currentTarget.getBoundingClientRect()
        onDragOver(event.clientY < rect.top + rect.height / 2 ? 'before' : 'after')
      }}
      onDrop={(event) => {
        event.preventDefault()
        const rect = event.currentTarget.getBoundingClientRect()
        onDrop(event.clientY < rect.top + rect.height / 2 ? 'before' : 'after')
      }}
      onDragEnd={onDragEnd}
      style={{
        boxShadow:
          dropEdge === 'before'
            ? 'inset 0 2px var(--accent)'
            : dropEdge === 'after'
              ? 'inset 0 -2px var(--accent)'
              : 'none',
      }}
    >
      {/* Folder header */}
      <div
        data-testid={`folder-header-${folder.id}`}
        tabIndex={draggable ? 0 : -1}
        aria-keyshortcuts="ArrowUp ArrowDown"
        className="relative flex items-center gap-1.5 px-2.5 py-0.5 cursor-pointer group rounded-md mx-1 focus-visible:outline focus-visible:outline-1 focus-visible:outline-[var(--accent)]"
        style={{
          background: 'transparent',
          color: 'var(--text-2)',
        }}
        onClick={onToggle}
        onKeyDown={(event) => {
          if (event.currentTarget !== event.target || !draggable || (event.key !== 'ArrowUp' && event.key !== 'ArrowDown')) return
          event.preventDefault()
          onMove(event.key === 'ArrowUp' ? -1 : 1)
        }}
        onContextMenu={(event) => {
          event.preventDefault()
          event.stopPropagation()
          setMenuPos({ x: event.clientX, y: event.clientY })
        }}
      >
        {shouldShowSessions ? (
          <FolderOpenIcon data-testid={`folder-icon-${folder.id}`} size={14} style={{ flexShrink: 0, color: 'var(--text-3)', opacity: 0.85 }} aria-hidden />
        ) : (
          <PaneColorIcon Icon={FolderIcon} colors={hiddenPaneColors} testId={`folder-icon-${folder.id}`} />
        )}
        <span className="font-semibold truncate flex-1" style={{ fontSize: 13 }}>
          {folder.name}
        </span>
        {folder.missing && (
          <span
            role="img"
            aria-label={`Workspace folder missing: ${folder.directory}`}
            title={`Workspace folder missing: ${folder.directory}`}
            data-testid={`folder-missing-${folder.id}`}
            style={{ color: 'var(--yellow)', flexShrink: 0 }}
          >
            <TriangleAlert size={14} aria-hidden />
          </span>
        )}
        {/* Count */}
        <span
          className="text-xs flex-shrink-0 rounded px-1 group-hover:opacity-0 transition-opacity duration-100"
          style={{ fontSize: 10, background: 'var(--surface-high)', color: 'var(--text-3)' }}
        >
          {sessionIds.length}
        </span>
        {/* Overflow menu trigger */}
        <div
          className={`absolute right-2 transition-opacity duration-100 ${menuPos ? 'opacity-100' : 'opacity-0 group-hover:opacity-100'}`}
          onClick={(e) => e.stopPropagation()}
        >
          <Tooltip label="Folder actions" side="top">
            <button
              ref={menuTriggerRef}
              type="button"
              aria-label="Folder actions"
              aria-haspopup="menu"
              aria-expanded={Boolean(menuPos)}
              className="flex items-center justify-center rounded"
              style={{ width: 22, height: 22, background: 'var(--surface-up)', color: 'var(--text-3)', border: '1px solid var(--border)', cursor: 'pointer' }}
              onClick={(e) => {
                e.stopPropagation()
                if (menuPos) { setMenuPos(null); return }
                const r = e.currentTarget.getBoundingClientRect()
                setMenuPos({ x: r.right - 168, y: r.bottom + 4 })
              }}
            >
              <MoreHorizontal size={14} aria-hidden />
            </button>
          </Tooltip>
        </div>
      </div>

      {menuPos && (
        <ViewportMenu
          ref={menuRef}
          anchorRef={menuTriggerRef}
          point={menuPos}
          role="menu"
          aria-label={`${folder.name} actions`}
          onRequestClose={() => setMenuPos(null)}
          className="fixed z-50 rounded-md py-1 animate-fade-in"
          style={{
            minWidth: 168,
            background: 'var(--surface-up)',
            border: '1px solid var(--border-bright)',
            boxShadow: 'var(--elev-2)',
          }}
          onClick={(e) => e.stopPropagation()}
        >
          <FolderMenuItem icon={<MessageSquarePlus size={14} aria-hidden />} label="New chat" onClick={() => { setMenuPos(null); onCreateChat() }} />
          <FolderMenuItem icon={<Brain size={14} aria-hidden />} label="Project memory" onClick={() => { setMenuPos(null); onOpenMemory() }} />
          <CollectionMenuItems target={folder.id} label={folder.name} onAdded={() => setMenuPos(null)} />
          <FolderMenuItem icon={<RefreshCw size={14} aria-hidden />} label="Rescan chats" onClick={() => { setMenuPos(null); onRescan() }} />
          <FolderMenuItem icon={<Pencil size={14} aria-hidden />} label="Rename folder" onClick={() => { setMenuPos(null); onStartRename() }} />
          <FolderMenuItem icon={<Trash2 size={14} aria-hidden />} label="Delete folder" danger onClick={() => { setMenuPos(null); onDelete() }} />
        </ViewportMenu>
      )}

      {isEditing && (
        <div
          className="mx-3 mb-2 rounded-md p-2 space-y-2"
          style={{
            background: 'var(--surface-up)',
            border: '1px solid var(--border)',
          }}
        >
          <input
            autoFocus
            value={editFolderName}
            onChange={(e) => onEditNameChange(e.target.value)}
            onKeyDown={(e) => {
              if (e.key === 'Enter') onCommitRename()
              if (e.key === 'Escape') onCancelEdit()
            }}
            placeholder="Folder name"
            className="w-full rounded px-2 py-1 text-xs outline-none"
            style={{
              background: 'var(--surface)',
              border: '1px solid var(--border)',
              color: 'var(--text)',
              fontFamily: 'inherit',
            }}
          />
          <div className="flex items-center gap-2">
            <input
              value={editFolderDirectory}
              onChange={(e) => onEditDirectoryChange(e.target.value)}
              onKeyDown={(e) => {
                if (e.key === 'Enter') onCommitRename()
                if (e.key === 'Escape') onCancelEdit()
              }}
              placeholder="Workspace directory"
              className="w-full flex-1 rounded px-2 py-1 text-xs outline-none"
              style={{
                background: 'var(--surface)',
                border: '1px solid var(--border)',
                color: 'var(--text)',
                fontFamily: 'inherit',
              }}
            />
            <Button
              variant="secondary"
              size="sm"
              onClick={onChooseDirectory}
            >
              Browse
            </Button>
          </div>
          <div className="flex items-center justify-end gap-2">
            <Button
              variant="ghost"
              size="sm"
              onClick={onCancelEdit}
            >
              Cancel
            </Button>
            <Button
              variant="primary"
              size="sm"
              disabled={!editFolderName.trim() || !editFolderDirectory.trim()}
              onClick={onCommitRename}
            >
              Save
            </Button>
          </div>
        </div>
      )}

      {/* Sessions */}
      {shouldShowSessions && (
        <div className="pl-3.5">
          {sessionIds.length === 0 ? (
            <div className="px-4 py-0.5 text-xs" style={{ color: 'var(--text-3)', opacity: 0.5, fontSize: 11 }}>
              Empty
            </div>
          ) : (
            visibleSessionIds.map((id) => (
              <SessionItem key={id} sessionId={id} session={sessionsById.get(id)} familySessionIds={familySessionIdsByRootId.get(id)} selected={selectedSessionIds.has(id)} onSessionClick={onSessionClick} />
            ))
          )}
          {shouldLimitSessions && (
            <button
              type="button"
              onClick={() => setShowAllSessions((value) => !value)}
              className="mx-3 mt-0.5 inline-flex items-center px-2 py-0.5 text-xs transition-colors duration-100"
              style={{
                background: 'transparent',
                color: 'var(--text-3)',
                border: 'none',
                cursor: 'pointer',
                fontFamily: 'inherit',
              }}
              onMouseEnter={(e) => {
                e.currentTarget.style.color = 'var(--text-2)'
              }}
              onMouseLeave={(e) => {
                e.currentTarget.style.color = 'var(--text-3)'
              }}
            >
              {showAllSessions ? 'Show less' : `Show ${hiddenSessionCount} more`}
            </button>
          )}
        </div>
      )}
    </div>
  )
})
