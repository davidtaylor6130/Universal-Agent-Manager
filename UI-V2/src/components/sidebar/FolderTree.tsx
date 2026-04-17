import { memo, useState, useMemo } from 'react'
import { useAppStore } from '../../store/useAppStore'
import { useShallow } from 'zustand/react/shallow'
import { SessionItem } from './SessionItem'
import {
  buildChatSearchIndex,
  buildChatSearchModel,
  tokenizeChatSearchQuery,
} from './chatSearch'
import type { Folder } from '../../types/session'

interface FolderTreeProps {
  searchQuery: string
}

export function FolderTree({ searchQuery }: FolderTreeProps) {
  const folders  = useAppStore(useShallow((s) => s.folders))
  const sessions = useAppStore(useShallow((s) => s.sessions))
  const messages = useAppStore(useShallow((s) => s.messages))
  const toggleFolder        = useAppStore((s) => s.toggleFolder)
  const addFolder           = useAppStore((s) => s.addFolder)
  const renameFolder        = useAppStore((s) => s.renameFolder)
  const deleteFolder        = useAppStore((s) => s.deleteFolder)
  const browseFolderDirectory = useAppStore((s) => s.browseFolderDirectory)

  const [addingFolder, setAddingFolder] = useState(false)
  const [newFolderName, setNewFolderName] = useState('')
  const [newFolderDirectory, setNewFolderDirectory] = useState('')
  const [editingFolderId, setEditingFolderId] = useState<string | null>(null)
  const [editFolderName, setEditFolderName] = useState('')
  const [editFolderDirectory, setEditFolderDirectory] = useState('')

  const searchIndex = useMemo(
    () => buildChatSearchIndex(sessions, messages),
    [sessions, messages]
  )
  const searchTokens = useMemo(
    () => tokenizeChatSearchQuery(searchQuery),
    [searchQuery]
  )
  const searchModel = useMemo(
    () => buildChatSearchModel(folders, sessions, searchIndex, searchTokens),
    [folders, sessions, searchIndex, searchTokens]
  )

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
      {searchModel.folderRows.map(({ folder, sessionIds, shouldShowSessions }) => (
        <FolderRow
          key={folder.id}
          folder={folder}
          sessionIds={sessionIds}
          shouldShowSessions={shouldShowSessions}
          isEditing={editingFolderId === folder.id}
          editFolderName={editFolderName}
          editFolderDirectory={editFolderDirectory}
          onToggle={() => toggleFolder(folder.id)}
          onStartRename={() => startRenameFolder(folder)}
          onDelete={() => {
            if (window.confirm(`Delete folder "${folder.name}" and move its chats to General?`)) {
              deleteFolder(folder.id)
            }
          }}
          onEditNameChange={setEditFolderName}
          onEditDirectoryChange={setEditFolderDirectory}
          onCommitRename={() => commitRenameFolder(folder.id)}
          onCancelEdit={() => setEditingFolderId(null)}
          onChooseDirectory={() => void chooseEditFolderDirectory()}
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
            <SessionItem key={id} sessionId={id} />
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
              <button
                type="button"
                className="px-2 py-1 rounded text-[10px] whitespace-nowrap"
                style={{
                  background: 'transparent',
                  color: 'var(--text-3)',
                  border: '1px solid var(--border)',
                  cursor: 'pointer',
                  fontFamily: 'inherit',
                }}
                onClick={() => { void chooseNewFolderDirectory() }}
              >
                Browse
              </button>
            </div>
            <div className="flex items-center justify-end gap-2">
              <button
                type="button"
                className="px-2 py-1 rounded text-[10px]"
                style={{
                  background: 'transparent',
                  color: 'var(--text-3)',
                  border: '1px solid var(--border)',
                  cursor: 'pointer',
                  fontFamily: 'inherit',
                }}
                onClick={() => {
                  setNewFolderName('')
                  setNewFolderDirectory('')
                  setAddingFolder(false)
                }}
              >
                Cancel
              </button>
              <button
                type="button"
                className="px-2 py-1 rounded text-[10px]"
                style={{
                  background: 'var(--accent)',
                  color: '#fff',
                  border: 'none',
                  cursor: 'pointer',
                  fontFamily: 'inherit',
                }}
                disabled={!newFolderName.trim() || !newFolderDirectory.trim()}
                onClick={commitAddFolder}
              >
                Create
              </button>
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
            <span style={{ fontSize: 11 }}>+</span>
            <span>New folder</span>
          </button>
        )}
      </div>
    </div>
  )
}

// ---------------------------------------------------------------------------
// FolderRow - memoized so it only re-renders when its own folder/session IDs change
// ---------------------------------------------------------------------------

interface FolderRowProps {
  folder: Folder
  sessionIds: string[]
  shouldShowSessions: boolean
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
}

const FolderRow = memo(function FolderRow({
  folder,
  sessionIds,
  shouldShowSessions,
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
}: FolderRowProps) {
  return (
    <div className="mb-2">
      {/* Folder header */}
      <div
        className="relative flex items-center gap-2 px-2.5 py-1.5 cursor-pointer group rounded-md mx-1"
        style={{
          background: 'var(--surface-up)',
          color: 'var(--text-2)',
        }}
        onClick={onToggle}
      >
        <svg
          width="13"
          height="13"
          viewBox="0 0 16 16"
          fill="currentColor"
          style={{ flexShrink: 0, color: 'var(--accent)', opacity: 0.85 }}
        >
          {shouldShowSessions ? (
            <>
              <path d="M1 5.5A1.5 1.5 0 012.5 4H6l1.5 1.5H14A1.5 1.5 0 0115.5 7v.5H.5V5.5A1 1 0 011 5.5z" />
              <path d="M.5 8h15l-1.5 5.5H2L.5 8z" opacity="0.85" />
            </>
          ) : (
            <path d="M1 5.5A1.5 1.5 0 012.5 4H6l1.5 1.5H13.5A1.5 1.5 0 0115 7v5.5A1.5 1.5 0 0113.5 14h-11A1.5 1.5 0 011 12.5v-7z" />
          )}
        </svg>
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
        {/* Action buttons */}
        <div
          className="absolute right-2 flex items-center gap-1 opacity-0 group-hover:opacity-100 transition-opacity duration-100"
          onClick={(e) => e.stopPropagation()}
        >
          <button
            type="button"
            className="text-[10px] px-1.5 py-0.5 rounded"
            style={{
              background: 'var(--surface-up)',
              color: 'var(--text-3)',
              border: '1px solid var(--border)',
              cursor: 'pointer',
              fontFamily: 'inherit',
            }}
            onClick={onStartRename}
          >
            Rename
          </button>
          <button
            type="button"
            className="text-[10px] px-1.5 py-0.5 rounded"
            style={{
              background: 'var(--surface-up)',
              color: 'var(--red)',
              border: '1px solid var(--border)',
              cursor: 'pointer',
              fontFamily: 'inherit',
            }}
            onClick={onDelete}
          >
            Delete
          </button>
        </div>
      </div>

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
            <button
              type="button"
              className="px-2 py-1 rounded text-[10px] whitespace-nowrap"
              style={{
                background: 'transparent',
                color: 'var(--text-3)',
                border: '1px solid var(--border)',
                cursor: 'pointer',
                fontFamily: 'inherit',
              }}
              onClick={onChooseDirectory}
            >
              Browse
            </button>
          </div>
          <div className="flex items-center justify-end gap-2">
            <button
              type="button"
              className="px-2 py-1 rounded text-[10px]"
              style={{
                background: 'transparent',
                color: 'var(--text-3)',
                border: '1px solid var(--border)',
                cursor: 'pointer',
                fontFamily: 'inherit',
              }}
              onClick={onCancelEdit}
            >
              Cancel
            </button>
            <button
              type="button"
              className="px-2 py-1 rounded text-[10px]"
              style={{
                background: 'var(--accent)',
                color: '#fff',
                border: 'none',
                cursor: 'pointer',
                fontFamily: 'inherit',
              }}
              disabled={!editFolderName.trim() || !editFolderDirectory.trim()}
              onClick={onCommitRename}
            >
              Save
            </button>
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
            sessionIds.map((id) => (
              <SessionItem key={id} sessionId={id} />
            ))
          )}
        </div>
      )}
    </div>
  )
})
