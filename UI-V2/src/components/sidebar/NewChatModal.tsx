import { useState, useEffect, useRef } from 'react'
import { useAppStore } from '../../store/useAppStore'

export function NewChatModal() {
  const { addSession, setNewChatModalOpen, folders } = useAppStore()
  const [name, setName] = useState('')
  const [folderId, setFolderId] = useState<string | null>(folders[0]?.id ?? null)
  const nameRef = useRef<HTMLInputElement>(null)

  useEffect(() => {
    nameRef.current?.focus()
  }, [])

  // Close on Escape
  useEffect(() => {
    const handler = (e: KeyboardEvent) => {
      if (e.key === 'Escape') setNewChatModalOpen(false)
    }
    window.addEventListener('keydown', handler)
    return () => window.removeEventListener('keydown', handler)
  }, [setNewChatModalOpen])

  const handleCreate = () => {
    const n = name.trim() || 'New Session'
    addSession(n, 'cli', folderId)
  }

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
              <select
                value={folderId ?? ''}
                onChange={(e) => setFolderId(e.target.value || null)}
                className="w-full rounded-md px-3 py-2 text-xs outline-none appearance-none"
                style={{
                  background: 'var(--surface-up)',
                  border: '1px solid var(--border)',
                  color: 'var(--text)',
                  fontFamily: 'inherit',
                  cursor: 'pointer',
                }}
              >
                <option value="">No folder</option>
                {folders.map((f) => (
                  <option key={f.id} value={f.id}>
                    {f.name}
                  </option>
                ))}
              </select>
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
