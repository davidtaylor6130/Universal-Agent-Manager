import { memo, useEffect, useRef, useState, useMemo } from 'react'
import type { ReactNode } from 'react'
import { Plus, Folder as FolderIcon, FolderOpen as FolderOpenIcon, X, MoreHorizontal, MessageSquarePlus, Brain, Pencil, Trash2 } from 'lucide-react'
import { useAppStore } from '../../store/useAppStore'
import { useShallow } from 'zustand/react/shallow'
import { SessionItem } from './SessionItem'
import { Button, Tooltip } from '../ui'
import {
  type ChatSearchFilters,
  buildChatSearchIndex,
  buildChatSearchModelFromGroups,
  buildChatSearchSessionGroups,
  tokenizeChatSearchQuery,
} from './chatSearch'
import type { Folder, Session } from '../../types/session'

interface FolderTreeProps {
  searchQuery: string
  deepSearchSessionIds?: string[]
  filters?: ChatSearchFilters
}

const VISIBLE_SESSION_LIMIT = 5
const EMPTY_SEARCH_INDEX = {}

export function FolderTree({ searchQuery, deepSearchSessionIds, filters }: FolderTreeProps) {
  const folders = useAppStore(useShallow((s) => s.folders))
  const sessions = useAppStore(useShallow((s) => s.sessions))
  const cliBindingBySessionId = useAppStore(useShallow((s) => s.cliBindingBySessionId))
  const acpBindingBySessionId = useAppStore(useShallow((s) => s.acpBindingBySessionId))
  const toggleFolder        = useAppStore((s) => s.toggleFolder)
  const addFolder           = useAppStore((s) => s.addFolder)
  const renameFolder        = useAppStore((s) => s.renameFolder)
  const deleteFolder        = useAppStore((s) => s.deleteFolder)
  const browseFolderDirectory = useAppStore((s) => s.browseFolderDirectory)
  const setNewChatModalOpen = useAppStore((s) => s.setNewChatModalOpen)
  const openFolderMemoryLibrary = useAppStore((s) => s.openFolderMemoryLibrary)

  const [addingFolder, setAddingFolder] = useState(false)
  const [newFolderName, setNewFolderName] = useState('')
  const [newFolderDirectory, setNewFolderDirectory] = useState('')
  const [editingFolderId, setEditingFolderId] = useState<string | null>(null)
  const [editFolderName, setEditFolderName] = useState('')
  const [editFolderDirectory, setEditFolderDirectory] = useState('')
  const [pendingDeleteFolderId, setPendingDeleteFolderId] = useState<string | null>(null)

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

  return (
    <div className="select-none">
      {searchModel.pinnedSessionIds.length > 0 && (
        <div className="mb-2">
          <div className="px-3 py-1" style={{ color: 'var(--text-3)' }}>
            <span className="text-xs font-medium tracking-wider uppercase" style={{ letterSpacing: '0.08em', fontSize: 10 }}>
              Pinned chats
            </span>
          </div>
          {searchModel.pinnedSessionIds.map((id) => (
            <SessionItem key={id} sessionId={id} session={sessionsById.get(id)} forceShowPin={true} />
          ))}
        </div>
      )}

      {!searchModel.isSearching && searchModel.folderRows.length > 0 && (
        <div className="px-3 pb-1 pt-1" style={{ color: 'var(--text-3)' }}>
          <div style={{ display: 'flex', alignItems: 'center', gap: 10 }}>
            <span className="text-xs font-medium tracking-wider uppercase" style={{ letterSpacing: '0.08em', fontSize: 10, whiteSpace: 'nowrap' }}>
              All chats
            </span>
            <span style={{ height: 1, flex: 1, background: 'var(--border)' }} />
          </div>
        </div>
      )}

      {searchModel.folderRows.map(({ folder, sessionIds, shouldShowSessions }) => (
        <FolderRow
          key={folder.id}
          folder={folder}
          sessionIds={sessionIds}
          shouldShowSessions={shouldShowSessions}
          isSearching={searchModel.isSearching}
          isEditing={editingFolderId === folder.id}
          editFolderName={editFolderName}
          editFolderDirectory={editFolderDirectory}
          onToggle={() => toggleFolder(folder.id)}
          onStartRename={() => startRenameFolder(folder)}
          onDelete={() => setPendingDeleteFolderId(folder.id)}
          onEditNameChange={setEditFolderName}
          onEditDirectoryChange={setEditFolderDirectory}
          onCommitRename={() => commitRenameFolder(folder.id)}
          onCancelEdit={() => setEditingFolderId(null)}
          onChooseDirectory={() => void chooseEditFolderDirectory()}
          onCreateChat={() => setNewChatModalOpen(true, folder.id)}
          onOpenMemory={() => void openFolderMemoryLibrary(folder.id)}
          sessionsById={sessionsById}
        />
      ))}

      {/* Unfoldered sessions */}
      {searchModel.unfolderedSessionIds.length > 0 && (
        <div className="mt-1">
          <div className="px-3 py-1" style={{ color: 'var(--text-3)' }}>
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
        <div className="px-3 py-2 text-xs" style={{ color: 'var(--text-3)', fontSize: 11 }}>
          No matching chats
        </div>
      )}

      {/* Add folder */}
      <div className="mt-2 px-3">
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
        ) : (
          <button
            onClick={() => setAddingFolder(true)}
            className="flex items-center gap-1.5 text-xs transition-colors duration-100"
            style={{
              color: 'var(--text-3)',
              background: 'transparent',
              border: 'none',
              cursor: 'pointer',
              fontFamily: 'inherit',
              padding: '2px 0',
            }}
            onMouseEnter={(e) => e.currentTarget.style.color = 'var(--text-2)'}
            onMouseLeave={(e) => e.currentTarget.style.color = 'var(--text-3)'}
          >
            <Plus size={14} aria-hidden />
            <span>New folder</span>
          </button>
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
          <Tooltip label="Close">
            <button
              type="button"
              onClick={onCancel}
              className="flex items-center justify-center rounded transition-colors duration-100"
              style={{
                background: 'transparent',
                color: 'var(--text-3)',
                border: 'none',
                cursor: 'pointer',
                padding: 0,
                width: 20,
                height: 20,
                fontFamily: 'inherit',
              }}
            >
              <X size={16} aria-hidden />
            </button>
          </Tooltip>
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
      className="flex w-full items-center gap-2 px-3 py-1.5 text-sm text-left transition-colors duration-100"
      style={{ background: 'transparent', color: danger ? 'var(--red)' : 'var(--text-2)', border: 'none', cursor: 'pointer', fontFamily: 'inherit' }}
      onMouseEnter={(e) => { e.currentTarget.style.background = 'var(--sidebar-item-hover)' }}
      onMouseLeave={(e) => { e.currentTarget.style.background = 'transparent' }}
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
  isSearching: boolean
  isEditing: boolean
  editFolderName: string
  editFolderDirectory: string
  onToggle: () => void
  onStartRename: () => void
  onDelete: () => void
  onEditNameChange: (v: string) => void
  onEditDirectoryChange: (v: string) => void
  onCommitRename: () => void
  onCancelEdit: () => void
  onChooseDirectory: () => void
  onCreateChat: () => void
  onOpenMemory: () => void
  sessionsById: Map<string, Session>
}

const FolderRow = memo(function FolderRow({
  folder,
  sessionIds,
  shouldShowSessions,
  isSearching,
  isEditing,
  editFolderName,
  editFolderDirectory,
  onToggle,
  onStartRename,
  onDelete,
  onEditNameChange,
  onEditDirectoryChange,
  onCommitRename,
  onCancelEdit,
  onChooseDirectory,
  onCreateChat,
  onOpenMemory,
  sessionsById,
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
      if (menuRef.current && !menuRef.current.contains(e.target as Node)) setMenuPos(null)
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
    <div className="mb-2">
      {/* Folder header */}
      <div
        className="relative flex items-center gap-2 px-3 py-1.5 cursor-pointer group rounded-md mx-1"
        style={{
          background: 'transparent',
          color: 'var(--text-2)',
        }}
        onClick={onToggle}
      >
        {shouldShowSessions ? (
          <FolderOpenIcon size={14} style={{ flexShrink: 0, color: 'var(--accent)', opacity: 0.85 }} aria-hidden />
        ) : (
          <FolderIcon size={14} style={{ flexShrink: 0, color: 'var(--accent)', opacity: 0.85 }} aria-hidden />
        )}
        <span className="font-semibold truncate flex-1" style={{ fontSize: 13 }}>
          {folder.name}
        </span>
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
        <div
          ref={menuRef}
          className="fixed z-50 rounded-md py-1 animate-fade-in"
          style={{
            left: Math.max(8, Math.min(menuPos.x, window.innerWidth - 176)),
            top: Math.min(menuPos.y, window.innerHeight - 150),
            minWidth: 168,
            background: 'var(--surface-up)',
            border: '1px solid var(--border-bright)',
            boxShadow: 'var(--elev-2)',
          }}
          onClick={(e) => e.stopPropagation()}
        >
          <FolderMenuItem icon={<MessageSquarePlus size={14} aria-hidden />} label="New chat" onClick={() => { setMenuPos(null); onCreateChat() }} />
          <FolderMenuItem icon={<Brain size={14} aria-hidden />} label="Project memory" onClick={() => { setMenuPos(null); onOpenMemory() }} />
          <FolderMenuItem icon={<Pencil size={14} aria-hidden />} label="Rename folder" onClick={() => { setMenuPos(null); onStartRename() }} />
          <FolderMenuItem icon={<Trash2 size={14} aria-hidden />} label="Delete folder" danger onClick={() => { setMenuPos(null); onDelete() }} />
        </div>
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
        <div>
          {sessionIds.length === 0 ? (
            <div className="px-6 py-1 text-xs" style={{ color: 'var(--text-3)', opacity: 0.5, fontSize: 11 }}>
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
              className="mx-5 mt-1 flex w-[calc(100%-2.5rem)] items-center justify-between gap-2 rounded-md px-2 py-1.5 text-xs transition-colors duration-100"
              style={{
                background: 'transparent',
                color: 'var(--text-3)',
                border: '1px solid var(--border)',
                cursor: 'pointer',
                fontFamily: 'inherit',
              }}
              onMouseEnter={(e) => {
                e.currentTarget.style.color = 'var(--text-2)'
                e.currentTarget.style.borderColor = 'var(--border-bright)'
              }}
              onMouseLeave={(e) => {
                e.currentTarget.style.color = 'var(--text-3)'
                e.currentTarget.style.borderColor = 'var(--border)'
              }}
            >
              <span>{showAllSessions ? 'Show less' : 'See more'}</span>
              {!showAllSessions && (
                <span className="text-[10px]" style={{ color: 'var(--text-3)' }}>
                  +{hiddenSessionCount}
                </span>
              )}
            </button>
          )}
          {folder.isExpanded && (
            <button
              type="button"
              onClick={onCreateChat}
              className="mx-5 mt-1 flex w-[calc(100%-2.5rem)] items-center gap-1.5 rounded-md px-2 py-1.5 text-xs transition-colors duration-100"
              style={{
                background: 'var(--surface-up)',
                color: 'var(--text-3)',
                border: '1px solid var(--border)',
                cursor: 'pointer',
                fontFamily: 'inherit',
              }}
              onMouseEnter={(e) => {
                e.currentTarget.style.color = 'var(--text-2)'
                e.currentTarget.style.borderColor = 'var(--border-bright)'
              }}
              onMouseLeave={(e) => {
                e.currentTarget.style.color = 'var(--text-3)'
                e.currentTarget.style.borderColor = 'var(--border)'
              }}
            >
              <Plus size={14} aria-hidden />
              <span>New chat</span>
            </button>
          )}
        </div>
      )}
    </div>
  )
})
