import { useState, useEffect, useRef } from 'react'
import { useAppStore } from '../../store/useAppStore'
import { ProviderLogo } from '../shared/ProviderLogo'

export function NewChatModal() {
  const { addSession, setNewChatModalOpen, folders, providers, newChatFolderId } = useAppStore()
  const initialFolderId =
    newChatFolderId !== null && folders.some((folder) => folder.id === newChatFolderId)
      ? newChatFolderId
      : null
  const [name, setName] = useState('')
  const [folderId, setFolderId] = useState<string | null>(initialFolderId)
  const [providerId, setProviderId] = useState<string>(providers[0]?.id ?? 'gemini-cli')
  const [folderMenuOpen, setFolderMenuOpen] = useState(false)
  const [providerMenuOpen, setProviderMenuOpen] = useState(false)
  const nameRef = useRef<HTMLInputElement>(null)
  const folderMenuRef = useRef<HTMLDivElement>(null)
  const providerMenuRef = useRef<HTMLDivElement>(null)

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
      setFolderId(null)
    }
  }, [folders, folderId, folderMenuOpen])

  useEffect(() => {
    if (newChatFolderId === null) return
    if (folders.some((folder) => folder.id === newChatFolderId)) {
      setFolderId(newChatFolderId)
    }
  }, [folders, newChatFolderId])

  useEffect(() => {
    if (providers.length === 0) return
    if (!providers.some((provider) => provider.id === providerId)) {
      setProviderId(providers[0].id)
    }
  }, [providers, providerId])

  // Close on Escape
  useEffect(() => {
    const handler = (e: KeyboardEvent) => {
      if (e.key === 'Escape') {
        if (providerMenuOpen) {
          setProviderMenuOpen(false)
          return
        }

        if (folderMenuOpen) {
          setFolderMenuOpen(false)
          return
        }

        setNewChatModalOpen(false)
      }
    }
    window.addEventListener('keydown', handler)
    return () => window.removeEventListener('keydown', handler)
  }, [folderMenuOpen, providerMenuOpen, setNewChatModalOpen])

  useEffect(() => {
    const handlePointerDown = (event: MouseEvent) => {
      if (folderMenuOpen && folderMenuRef.current && event.target instanceof Node && !folderMenuRef.current.contains(event.target)) {
        setFolderMenuOpen(false)
      }

      if (providerMenuOpen && providerMenuRef.current && event.target instanceof Node && !providerMenuRef.current.contains(event.target)) {
        setProviderMenuOpen(false)
      }
    }

    window.addEventListener('mousedown', handlePointerDown)
    return () => window.removeEventListener('mousedown', handlePointerDown)
  }, [folderMenuOpen, providerMenuOpen])

  const handleCreate = () => {
    if (folderId === null || !folders.some((folder) => folder.id === folderId)) {
      return
    }

    const n = name.trim() || 'New Session'
    addSession(n, folderId, providerId)
  }

  const selectedFolder =
    (folderId !== null ? folders.find((f) => f.id === folderId) : null) ?? null
  const selectedProvider = providers.find((provider) => provider.id === providerId) ?? providers[0] ?? null
  const canCreate = selectedFolder !== null

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

          {providers.length > 1 && (
            <div>
              <label className="text-xs font-medium mb-1.5 block" style={{ color: 'var(--text-2)' }}>
                Provider
              </label>
              <div className="relative" ref={providerMenuRef}>
                <button
                  type="button"
                  onClick={() => setProviderMenuOpen((open) => !open)}
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
                    <span className="min-w-0 flex items-center gap-2 text-xs">
                      <ProviderLogo providerId={selectedProvider?.id} />
                      <span className="truncate">{selectedProvider?.shortName ?? selectedProvider?.name ?? 'Provider'}</span>
                    </span>
                    <span className="text-[10px]" style={{ color: 'var(--text-3)' }}>
                      {providerMenuOpen ? '▲' : '▼'}
                    </span>
                  </div>
                </button>

                {providerMenuOpen && (
                  <div
                    className="absolute left-0 right-0 top-full z-10 mt-1 max-h-52 overflow-y-auto rounded-md p-1 shadow-2xl"
                    style={{
                      background: 'var(--surface)',
                      border: '1px solid var(--border-bright)',
                    }}
                  >
                    {providers.map((provider) => {
                      const isSelected = selectedProvider?.id === provider.id

                      return (
                        <button
                          key={provider.id}
                          type="button"
                          onClick={() => {
                            setProviderId(provider.id)
                            setProviderMenuOpen(false)
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
                          <div className="flex items-center gap-2 text-xs">
                            <ProviderLogo providerId={provider.id} />
                            <span>{provider.shortName || provider.name}</span>
                          </div>
                          <div className="truncate text-[10px]" style={{ color: 'var(--text-3)' }}>
                            {provider.structuredProtocol === 'codex-app-server'
                              ? 'Codex app-server + CLI'
                              : provider.structuredProtocol === 'claude-code-stream-json'
                                ? 'Claude stream + CLI'
                                : 'Gemini ACP + CLI'}
                          </div>
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
            disabled={!canCreate}
            className="px-4 py-1.5 rounded-md text-xs font-medium transition-opacity duration-150"
            style={{
              background: 'var(--accent)',
              color: '#fff',
              border: 'none',
              cursor: canCreate ? 'pointer' : 'not-allowed',
              fontFamily: 'inherit',
              opacity: canCreate ? 1 : 0.45,
            }}
            onMouseEnter={(e) => { if (canCreate) e.currentTarget.style.opacity = '0.88' }}
            onMouseLeave={(e) => { e.currentTarget.style.opacity = canCreate ? '1' : '0.45' }}
          >
            Create Session
          </button>
        </div>
      </div>
    </div>
  )
}
