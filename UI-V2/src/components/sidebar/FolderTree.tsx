import { memo, useEffect, useRef, useState, useMemo } from 'react'
import type { ReactNode } from 'react'
import { Check, Plus, Folder as FolderIcon, FolderOpen as FolderOpenIcon, X, MoreHorizontal, MessageSquarePlus, Brain, Pencil, Trash2, TriangleAlert, ChevronRight, Library, SearchX, RefreshCw } from 'lucide-react'
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
  tokenizeChatSearchQuery,
} from './chatSearch'
import type { Folder, Session } from '../../types/session'
import { chatPaneColors, readChatGridLayout, subscribeChatGridLayout } from '../../utils/chatGridStorage'
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
  const cliBindingBySessionId = useAppStore(useShallow((s) => s.cliBindingBySessionId))
  const acpBindingBySessionId = useAppStore(useShallow((s) => s.acpBindingBySessionId))
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

  const [addingFolder, setAddingFolder] = useState(false)
  const [addingCollection, setAddingCollection] = useState(false)
  const [newCollectionName, setNewCollectionName] = useState('')
  const [newFolderName, setNewFolderName] = useState('')
  const [newFolderDirectory, setNewFolderDirectory] = useState('')
  const [editingFolderId, setEditingFolderId] = useState<string | null>(null)
  const [editFolderName, setEditFolderName] = useState('')
  const [editFolderDirectory, setEditFolderDirectory] = useState('')
  const [pendingDeleteFolderId, setPendingDeleteFolderId] = useState<string | null>(null)
  const [gridLayout, setGridLayout] = useState(readChatGridLayout)
  const [draggedFolderId, setDraggedFolderId] = useState<string | null>(null)
  const [folderDropTarget, setFolderDropTarget] = useState<{ id: string; edge: FolderDropEdge } | null>(null)
  const addControlsRef = useRef<HTMLDivElement>(null)

  useEffect(() => subscribeChatGridLayout(setGridLayout), [])

  useEffect(() => {
    if (addingFolder || addingCollection) {
      addControlsRef.current?.scrollIntoView?.({ block: 'nearest' })
    }
  }, [addingCollection, addingFolder])

  const paneColorsForFolders = (folderIds: Set<string>) => {
    if (gridLayout.paneCount === 1) return []
    const folderSessionIds = new Set(sessions.filter((session) => session.folderId && folderIds.has(session.folderId)).map((session) => session.id))
    return gridLayout.sessionIds.slice(0, gridLayout.paneCount).flatMap((id, index) => folderSessionIds.has(id) ? [chatPaneColors[index]] : [])
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
      { cliBindingBySessionId, acpBindingBySessionId }
    ),
    [sessions, searchIndex, searchTokens, deepSearchSet, filters, cliBindingBySessionId, acpBindingBySessionId]
  )
  const searchModel = useMemo(
    () => buildChatSearchModelFromGroups(folders, searchGroups),
    [folders, searchGroups]
  )
  const sessionsById = useMemo(
    () => new Map(sessions.map((session) => [session.id, session])),
    [sessions]
  )
  const pendingDeleteFolder = useMemo(
    () => folders.find((folder) => folder.id === pendingDeleteFolderId) ?? null,
    [folders, pendingDeleteFolderId]
  )
  const pendingDeleteChatCount = useMemo(
    () => sessions.filter((session) => session.folderId === pendingDeleteFolderId).length,
    [sessions, pendingDeleteFolderId]
  )

  useEffect(() => {
    if (pendingDeleteFolderId !== null && !pendingDeleteFolder) {
      setPendingDeleteFolderId(null)
    }
  }, [pendingDeleteFolder, pendingDeleteFolderId])

  const commitAddFolder = () => {
    const name = newFolderName.trim()
    const directory = newFolderDirectory.trim()

    if (!name || !directory) {
      return
    }

    void addFolder(name, null, directory).then((created) => {
      if (!created) {
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
    void createResourceCollection(name).then((created) => {
      if (!created) return
      setNewCollectionName('')
      setAddingCollection(false)
    })
  }

  const startRenameFolder = (folder: (typeof folders)[number]) => {
    setEditingFolderId(folder.id)
    setEditFolderName(folder.name)
    setEditFolderDirectory(folder.directory)
  }

  const commitRenameFolder = (folderId: string) => {
    const name = editFolderName.trim()
    const directory = editFolderDirectory.trim()

    if (!name || !directory) {
      return
    }

    renameFolder(folderId, name, directory)
    setEditingFolderId(null)
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
      onDelete={() => setPendingDeleteFolderId(folder.id)}
      onRescan={() => void rescanFolderChats(folder.id)}
      onEditNameChange={setEditFolderName}
      onEditDirectoryChange={setEditFolderDirectory}
      onCommitRename={() => commitRenameFolder(folder.id)}
      onCancelEdit={() => setEditingFolderId(null)}
      onChooseDirectory={() => void chooseEditFolderDirectory()}
      onCreateChat={() => setNewChatModalOpen(true, folder.id)}
      onOpenMemory={() => void openFolderMemoryLibrary(folder.id)}
      sessionsById={sessionsById}
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
    <div className="select-none">
      {searchModel.pinnedSessionIds.length > 0 && (
        <div className="mb-0.5">
          <div className="px-2.5 py-0.5" style={{ color: 'var(--text-3)' }}>
            <span className="text-xs font-medium tracking-wider uppercase" style={{ letterSpacing: '0.08em', fontSize: 10 }}>
              Pinned chats
            </span>
          </div>
          {searchModel.pinnedSessionIds.map((id) => (
            <SessionItem key={id} sessionId={id} session={sessionsById.get(id)} />
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
          <div className="px-2.5 py-0.5" style={{ color: 'var(--text-3)' }}>
            <span className="text-xs font-medium tracking-wider uppercase" style={{ letterSpacing: '0.08em', fontSize: 10 }}>
              Unsorted
            </span>
          </div>
          {searchModel.unfolderedSessionIds.map((id) => (
            <SessionItem key={id} sessionId={id} session={sessionsById.get(id)} />
          ))}
        </div>
      )}

      {searchModel.isSearching && !searchModel.hasMatches && (
        <div className="mx-3 my-6 flex flex-col items-center gap-1 text-center animate-fade-in" style={{ color: 'var(--text-3)' }}>
          <SearchX size={22} strokeWidth={1.5} aria-hidden />
          <div className="text-xs font-medium" style={{ color: 'var(--text-2)' }}>No chats match this search</div>
          <div className="text-[11px]">Try another term or clear the filters.</div>
        </div>
      )}

      {/* Add folder */}
      <div ref={addControlsRef} className="mt-1 px-2.5">
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
              onClick={() => setAddingFolder(true)}
              className="flex items-center gap-1.5 text-xs transition-colors duration-100"
              style={{ color: 'var(--text-3)', background: 'transparent', border: 'none', cursor: 'pointer', fontFamily: 'inherit', padding: '2px 0' }}
            >
              <Plus size={14} aria-hidden />
              <span>New workspace</span>
            </button>
            <button
              onClick={() => setAddingCollection(true)}
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
          onCancel={() => setPendingDeleteFolderId(null)}
          onConfirm={() => {
            deleteFolder(pendingDeleteFolder.id)
            setPendingDeleteFolderId(null)
          }}
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
  onCancel: () => void
  onConfirm: () => void
}

function DeleteFolderModal({
  folder,
  chatCount,
  onCancel,
  onConfirm,
}: DeleteFolderModalProps) {
  const chatLabel = chatCount === 1 ? '1 chat' : `${chatCount} chats`

  useEffect(() => {
    const handler = (e: KeyboardEvent) => {
      if (e.key === 'Escape') {
        onCancel()
      }
    }

    window.addEventListener('keydown', handler)
    return () => window.removeEventListener('keydown', handler)
  }, [onCancel])

  return (
    <div
      className="fixed inset-0 z-50 flex items-center justify-center animate-fade-in"
      style={{ background: 'rgba(0,0,0,0.55)', backdropFilter: 'blur(4px)' }}
      onClick={(e) => {
        if (e.target === e.currentTarget) onCancel()
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
          <IconButton icon={<X size={16} />} label="Close delete folder dialog" onClick={onCancel} />
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
        </div>

        <div
          className="flex items-center justify-end gap-2 px-5 py-4"
          style={{ borderTop: '1px solid var(--border)' }}
        >
          <Button
            variant="ghost"
            size="md"
            onClick={onCancel}
          >
            Cancel
          </Button>
          <Button
            variant="danger"
            size="md"
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
              type="button"
              aria-label="Folder actions"
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
          point={menuPos}
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
              <SessionItem key={id} sessionId={id} session={sessionsById.get(id)} />
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
