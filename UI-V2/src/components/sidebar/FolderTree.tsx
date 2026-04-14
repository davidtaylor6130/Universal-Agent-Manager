import { useState } from 'react'
import { useAppStore } from '../../store/useAppStore'
import { SessionItem } from './SessionItem'

export function FolderTree() {
  const {
    folders,
    sessions,
    toggleFolder,
    addFolder,
    renameFolder,
    deleteFolder,
    browseFolderDirectory,
  } = useAppStore()
  const [addingFolder, setAddingFolder] = useState(false)
  const [newFolderName, setNewFolderName] = useState('')
  const [newFolderDirectory, setNewFolderDirectory] = useState('')
  const [editingFolderId, setEditingFolderId] = useState<string | null>(null)
  const [editFolderName, setEditFolderName] = useState('')
  const [editFolderDirectory, setEditFolderDirectory] = useState('')

  const rootFolders = folders.filter((f) => f.parentId === null)

  const commitAddFolder = () => {
    const name = newFolderName.trim()
    const directory = newFolderDirectory.trim()

    if (!name || !directory) {
      return
    }

    addFolder(name, null, directory)
    setNewFolderName('')
    setNewFolderDirectory('')
    setAddingFolder(false)
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
      {rootFolders.map((folder) => {
        const folderSessions = sessions.filter((s) => s.folderId === folder.id)
        const isEditing = editingFolderId === folder.id

        return (
          <div key={folder.id} className="mb-1 rounded-md">
            {/* Folder header */}
            <div
              className="flex items-center gap-1.5 px-3 py-1 cursor-pointer group"
              style={{ color: 'var(--text-3)' }}
              onClick={() => toggleFolder(folder.id)}
            >
              <span
                className="transition-transform duration-150 text-xs"
                style={{
                  display: 'inline-block',
                  transform: folder.isExpanded ? 'rotate(90deg)' : 'rotate(0deg)',
                  fontSize: 9,
                }}
              >
                ▶
              </span>
              <span className="text-xs font-medium tracking-wider uppercase" style={{ letterSpacing: '0.08em', fontSize: 10 }}>
                {folder.name}
              </span>
              <span className="text-xs ml-auto opacity-50" style={{ fontSize: 10 }}>
                {folderSessions.length}
              </span>
              <div
                className="flex items-center gap-1 opacity-0 group-hover:opacity-100 transition-opacity duration-100"
                onClick={(e) => e.stopPropagation()}
              >
                <button
                  type="button"
                  className="text-[10px] px-1.5 py-0.5 rounded"
                  style={{
                    background: 'transparent',
                    color: 'var(--text-3)',
                    border: '1px solid var(--border)',
                    cursor: 'pointer',
                    fontFamily: 'inherit',
                  }}
                  onClick={() => startRenameFolder(folder)}
                >
                  Rename
                </button>
                <button
                  type="button"
                  className="text-[10px] px-1.5 py-0.5 rounded"
                  style={{
                    background: 'transparent',
                    color: 'var(--red)',
                    border: '1px solid var(--border)',
                    cursor: 'pointer',
                    fontFamily: 'inherit',
                  }}
                  onClick={() => {
                    if (window.confirm(`Delete folder "${folder.name}" and move its chats to General?`)) {
                      deleteFolder(folder.id)
                    }
                  }}
                >
                  Delete
                </button>
              </div>
            </div>

            <div className="px-3 pb-1 text-[10px] truncate" style={{ color: 'var(--text-3)', opacity: 0.65 }}>
              {folder.directory}
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
                  onChange={(e) => setEditFolderName(e.target.value)}
                  onKeyDown={(e) => {
                    if (e.key === 'Enter') commitRenameFolder(folder.id)
                    if (e.key === 'Escape') setEditingFolderId(null)
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
                    onChange={(e) => setEditFolderDirectory(e.target.value)}
                    onKeyDown={(e) => {
                      if (e.key === 'Enter') commitRenameFolder(folder.id)
                      if (e.key === 'Escape') setEditingFolderId(null)
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
                    onClick={() => {
                      void chooseEditFolderDirectory()
                    }}
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
                    onClick={() => setEditingFolderId(null)}
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
                    onClick={() => commitRenameFolder(folder.id)}
                  >
                    Save
                  </button>
                </div>
              </div>
            )}

            {/* Sessions */}
            {folder.isExpanded && (
              <div>
                {folderSessions.length === 0 ? (
                  <div className="px-6 py-1 text-xs" style={{ color: 'var(--text-3)', opacity: 0.5, fontSize: 11 }}>
                    Empty
                  </div>
                ) : (
                  folderSessions.map((session) => (
                    <SessionItem key={session.id} session={session} />
                  ))
                )}
              </div>
            )}
          </div>
        )
      })}

      {/* Unfoldered sessions */}
      {sessions.filter((s) => s.folderId === null).length > 0 && (
        <div className="mt-1">
          <div className="px-3 py-1" style={{ color: 'var(--text-3)' }}>
            <span className="text-xs font-medium tracking-wider uppercase" style={{ letterSpacing: '0.08em', fontSize: 10 }}>
              Unsorted
            </span>
          </div>
          {sessions
            .filter((s) => s.folderId === null)
            .map((session) => (
              <SessionItem key={session.id} session={session} />
            ))}
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
                onClick={() => {
                  void chooseNewFolderDirectory()
                }}
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
