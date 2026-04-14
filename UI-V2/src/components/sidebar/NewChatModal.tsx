import { useState, useEffect, useRef } from 'react'
import { useAppStore } from '../../store/useAppStore'

export function NewChatModal() {
  const { addSession, setNewChatModalOpen, folders } = useAppStore()
  const [name, setName] = useState('')
  const [folderId, setFolderId] = useState<string | null>(folders[0]?.id ?? null)
  const [folderMenuOpen, setFolderMenuOpen] = useState(false)
  const nameRef = useRef<HTMLInputElement>(null)
  const folderMenuRef = useRef<HTMLDivElement>(null)

  useEffect(() => {
    nameRef.current?.focus()
  }, [])

  useEffect(() => {
    if (folders.length === 0) {
      if (folderId !== null) setFolderId(null)
      if (folderMenuOpen) setFolderMenuOpen(false)
      return
    }

    const folderExists = folderId !== null && folders.some((f) => f.id === folderId)
    if (!folderExists) {
      setFolderId(folders[0]?.id ?? null)
    }
  }, [folders, folderId, folderMenuOpen])

  // Close on Escape
  useEffect(() => {
    const handler = (e: KeyboardEvent) => {
      if (e.key === 'Escape') {
        if (folderMenuOpen) {
          setFolderMenuOpen(false)
          return
        }

        setNewChatModalOpen(false)
      }
    }
    window.addEventListener('keydown', handler)
    return () => window.removeEventListener('keydown', handler)
  }, [folderMenuOpen, setNewChatModalOpen])

  useEffect(() => {
    const handlePointerDown = (event: MouseEvent) => {
      if (!folderMenuOpen) return

      if (folderMenuRef.current && event.target instanceof Node && !folderMenuRef.current.contains(event.target)) {
        setFolderMenuOpen(false)
      }
    }

    window.addEventListener('mousedown', handlePointerDown)
    return () => window.removeEventListener('mousedown', handlePointerDown)
  }, [folderMenuOpen])

  const handleCreate = () => {
    const n = name.trim() || 'New Session'
    const selectedFolderId = folderId ?? folders[0]?.id ?? null
    addSession(n, 'cli', selectedFolderId)
  }

  const selectedFolder =
    (folderId !== null ? folders.find((f) => f.id === folderId) : null) ?? folders[0] ?? null

  return (
    <div
      className="fixed inset-0 z-50 flex items-center justify-center animate-fade-in"
      style={{ background: 'rgba(0,0,0,0.55)', backdropFilter: 'blur(4px)' }}
      onClick={(e) => {
        if (e.target === e.currentTarget) setNewChatModalOpen(false)
      }}
    >
      <div
        className="rounded-xl shadow-2xl w-full max-w-md mx-4 animate-slide-in"
        style={{
          background: 'var(--surface)',
          border: '1px solid var(--border-bright)',
        }}
      >
        {/* Header */}
        <div
          className="flex items-center justify-between px-5 py-4"
          style={{ borderBottom: '1px solid var(--border)' }}
        >
          <span className="text-sm font-semibold" style={{ color: 'var(--text)' }}>
            New Session
          </span>
          <button
            onClick={() => setNewChatModalOpen(false)}
            className="text-xs rounded transition-colors duration-100"
            style={{
              background: 'transparent',
              color: 'var(--text-3)',
              border: 'none',
              cursor: 'pointer',
              padding: '2px 4px',
              fontFamily: 'inherit',
            }}
          >
            ✕
          </button>
        </div>

        <div className="p-5 space-y-5">
          {/* Name */}
          <div>
            <label className="text-xs font-medium mb-1.5 block" style={{ color: 'var(--text-2)' }}>
              Session name
            </label>
            <input
              ref={nameRef}
              type="text"
              value={name}
              onChange={(e) => setName(e.target.value)}
              onKeyDown={(e) => e.key === 'Enter' && handleCreate()}
              placeholder="e.g. API Design Review"
              className="w-full rounded-md px-3 py-2 text-sm outline-none transition-all duration-150"
              style={{
                background: 'var(--surface-up)',
                border: '1px solid var(--border)',
                color: 'var(--text)',
                fontFamily: 'inherit',
              }}
              onFocus={(e) => { e.target.style.borderColor = 'var(--accent)' }}
              onBlur={(e) => { e.target.style.borderColor = 'var(--border)' }}
            />
          </div>

          {/* Folder */}
          {folders.length > 0 && (
            <div>
              <label className="text-xs font-medium mb-1.5 block" style={{ color: 'var(--text-2)' }}>
                Folder
              </label>
              <div className="relative" ref={folderMenuRef}>
                <button
                  type="button"
                  onClick={() => setFolderMenuOpen((open) => !open)}
                  className="w-full rounded-md px-3 py-2 text-left transition-colors duration-100"
                  style={{
                    background: 'var(--surface-up)',
                    border: '1px solid var(--border)',
                    color: 'var(--text)',
                    cursor: 'pointer',
                    fontFamily: 'inherit',
                  }}
                >
                  <div className="flex items-center justify-between gap-3">
                    <div className="min-w-0">
                      <div className="text-xs">
                        {selectedFolder ? selectedFolder.name : 'Choose a folder'}
                      </div>
                      {selectedFolder?.directory && (
                        <div className="truncate text-[10px]" style={{ color: 'var(--text-3)' }}>
                          {selectedFolder.directory}
                        </div>
                      )}
                    </div>
                    <span className="text-[10px]" style={{ color: 'var(--text-3)' }}>
                      {folderMenuOpen ? '▲' : '▼'}
                    </span>
                  </div>
                </button>

                {folderMenuOpen && (
                  <div
                    className="absolute left-0 right-0 top-full z-10 mt-1 max-h-52 overflow-y-auto rounded-md p-1 shadow-2xl"
                    style={{
                      background: 'var(--surface)',
                      border: '1px solid var(--border-bright)',
                    }}
                  >
                    {folders.map((f) => {
                      const isSelected = selectedFolder?.id === f.id

                      return (
                        <button
                          key={f.id}
                          type="button"
                          onClick={() => {
                            setFolderId(f.id)
                            setFolderMenuOpen(false)
                          }}
                          className="w-full rounded-md px-2 py-2 text-left transition-colors duration-100"
                          style={{
                            background: isSelected ? 'color-mix(in srgb, var(--accent) 16%, transparent)' : 'transparent',
                            border: 'none',
                            color: 'var(--text)',
                            cursor: 'pointer',
                            fontFamily: 'inherit',
                          }}
                        >
                          <div className="text-xs">{f.name}</div>
                          {f.directory && (
                            <div className="truncate text-[10px]" style={{ color: 'var(--text-3)' }}>
                              {f.directory}
                            </div>
                          )}
                        </button>
                      )
                    })}
                  </div>
                )}
              </div>
            </div>
          )}
        </div>

        {/* Footer */}
        <div
          className="flex items-center justify-end gap-2 px-5 py-4"
          style={{ borderTop: '1px solid var(--border)' }}
        >
          <button
            onClick={() => setNewChatModalOpen(false)}
            className="px-4 py-1.5 rounded-md text-xs transition-colors duration-150"
            style={{
              background: 'transparent',
              color: 'var(--text-2)',
              border: '1px solid var(--border)',
              cursor: 'pointer',
              fontFamily: 'inherit',
            }}
            onMouseEnter={(e) => e.currentTarget.style.borderColor = 'var(--border-bright)'}
            onMouseLeave={(e) => e.currentTarget.style.borderColor = 'var(--border)'}
          >
            Cancel
          </button>
          <button
            onClick={handleCreate}
            className="px-4 py-1.5 rounded-md text-xs font-medium transition-opacity duration-150"
            style={{
              background: 'var(--accent)',
              color: '#fff',
              border: 'none',
              cursor: 'pointer',
              fontFamily: 'inherit',
            }}
            onMouseEnter={(e) => { e.currentTarget.style.opacity = '0.88' }}
            onMouseLeave={(e) => { e.currentTarget.style.opacity = '1' }}
          >
            Create Session
          </button>
        </div>
      </div>
    </div>
  )
}
