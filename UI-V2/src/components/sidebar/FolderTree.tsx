import { useState } from 'react'
import { useAppStore } from '../../store/useAppStore'
import { SessionItem } from './SessionItem'

export function FolderTree() {
  const { folders, sessions, toggleFolder, addFolder } = useAppStore()
  const [addingFolder, setAddingFolder] = useState(false)
  const [newFolderName, setNewFolderName] = useState('')

  const rootFolders = folders.filter((f) => f.parentId === null)

  const commitAddFolder = () => {
    const name = newFolderName.trim()
    if (name) addFolder(name, null)
    setNewFolderName('')
    setAddingFolder(false)
  }

  return (
    <div className="select-none">
      {rootFolders.map((folder) => {
        const folderSessions = sessions.filter((s) => s.folderId === folder.id)

        return (
          <div key={folder.id} className="mb-1">
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
            </div>

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
          <input
            autoFocus
            value={newFolderName}
            onChange={(e) => setNewFolderName(e.target.value)}
            onBlur={commitAddFolder}
            onKeyDown={(e) => {
              if (e.key === 'Enter') commitAddFolder()
              if (e.key === 'Escape') { setNewFolderName(''); setAddingFolder(false) }
            }}
            placeholder="Folder name…"
            className="w-full bg-transparent text-xs outline-none px-1 py-0.5 rounded"
            style={{
              color: 'var(--text)',
              border: '1px solid var(--border-bright)',
              fontFamily: 'inherit',
            }}
          />
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
