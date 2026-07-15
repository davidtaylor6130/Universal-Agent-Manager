import { useState, useEffect, useRef } from 'react'
import { X, ChevronDown, ChevronUp } from 'lucide-react'
import { useAppStore } from '../../store/useAppStore'
import { useShallow } from 'zustand/react/shallow'
import { ProviderLogo } from '../shared/ProviderLogo'
import { DEFAULT_PROVIDER_ID, providerRuntimeDescription } from '../../utils/providerMetadata'
import { Button, IconButton, MenuSelect } from '../ui'
import { buildModelOptions } from '../chat/modelOptions'

export function NewChatModal() {
  const addSession = useAppStore((s) => s.addSession)
  const setNewChatModalOpen = useAppStore((s) => s.setNewChatModalOpen)
  const folders = useAppStore(useShallow((s) => s.folders))
  const providers = useAppStore(useShallow((s) => s.providers))
  const defaultNewChatProviderId = useAppStore((s) => s.defaultNewChatProviderId)
  const providerChatDefaults = useAppStore(useShallow((s) => s.providerChatDefaults))
  const newChatFolderId = useAppStore((s) => s.newChatFolderId)
  const initialFolderId =
    newChatFolderId !== null && folders.some((folder) => folder.id === newChatFolderId)
      ? newChatFolderId
      : null
  const [name, setName] = useState('')
  const [folderId, setFolderId] = useState<string | null>(initialFolderId)
  const initialProviderId =
    providers.some((provider) => provider.id === defaultNewChatProviderId)
      ? defaultNewChatProviderId
      : providers[0]?.id ?? DEFAULT_PROVIDER_ID
  const [providerId, setProviderId] = useState(initialProviderId)
  const [modelId, setModelId] = useState(providerChatDefaults[initialProviderId]?.modelId ?? '')
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
      const nextProviderId = providers.some((provider) => provider.id === defaultNewChatProviderId) ? defaultNewChatProviderId : providers[0].id
      setProviderId(nextProviderId)
      setModelId(providerChatDefaults[nextProviderId]?.modelId ?? '')
    }
  }, [providers, providerId, defaultNewChatProviderId, providerChatDefaults])

  // Close on Escape
  useEffect(() => {
    const handler = (e: KeyboardEvent) => {
      if (e.key === 'Escape') {
        if (e.defaultPrevented || (e.target instanceof Element && e.target.closest('[role="listbox"]'))) return

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

    const n = name.trim() || 'New chat'
    addSession(n, folderId, providerId, modelId)
  }

  const selectedFolder =
    (folderId !== null ? folders.find((f) => f.id === folderId) : null) ?? null
  const selectedProvider = providers.find((provider) => provider.id === providerId) ?? providers[0] ?? null
  const modelOptions = buildModelOptions(undefined, modelId, selectedProvider ?? undefined, providerId)
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
        role="dialog"
        aria-modal="true"
        aria-label="New chat"
        tabIndex={-1}
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
            New chat
          </span>
          <IconButton icon={<X size={16} />} label="Close new chat" onClick={() => setNewChatModalOpen(false)} />
        </div>

        <div className="p-5 space-y-5">
          {/* Name */}
          <div>
            <label className="text-xs font-medium mb-1.5 block" style={{ color: 'var(--text-2)' }}>
              Chat name
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
                  aria-label="Folder"
                  aria-haspopup="listbox"
                  aria-expanded={folderMenuOpen}
                  onClick={() => setFolderMenuOpen((open) => !open)}
                  className="uam-menu-select__trigger w-full rounded-md px-3 py-2 text-left"
                  style={{
                    color: 'var(--text)',
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
                    <span style={{ color: 'var(--text-3)', display: 'flex', alignItems: 'center' }}>
                      {folderMenuOpen ? <ChevronUp size={14} aria-hidden /> : <ChevronDown size={14} aria-hidden />}
                    </span>
                  </div>
                </button>

                {folderMenuOpen && (
                  <div
                    role="listbox"
                    aria-label="Folder"
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
                          role="option"
                          aria-selected={isSelected}
                          onClick={() => {
                            setFolderId(f.id)
                            setFolderMenuOpen(false)
                          }}
                          className={`uam-menu-select__option w-full rounded-md px-2 py-2 text-left${isSelected ? ' is-selected' : ''}`}
                          style={{
                            border: 'none',
                            color: 'var(--text)',
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
                  aria-label="Provider"
                  aria-haspopup="listbox"
                  aria-expanded={providerMenuOpen}
                  onClick={() => setProviderMenuOpen((open) => !open)}
                  className="uam-menu-select__trigger w-full rounded-md px-3 py-2 text-left"
                  style={{
                    color: 'var(--text)',
                    fontFamily: 'inherit',
                  }}
                >
                  <div className="flex items-center justify-between gap-3">
                    <span className="min-w-0 flex items-center gap-2 text-xs">
                      <ProviderLogo providerId={selectedProvider?.id} />
                      <span className="truncate">{selectedProvider?.shortName ?? selectedProvider?.name ?? 'Provider'}</span>
                    </span>
                    <span style={{ color: 'var(--text-3)', display: 'flex', alignItems: 'center' }}>
                      {providerMenuOpen ? <ChevronUp size={14} aria-hidden /> : <ChevronDown size={14} aria-hidden />}
                    </span>
                  </div>
                </button>

                {providerMenuOpen && (
                  <div
                    role="listbox"
                    aria-label="Provider"
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
                          role="option"
                          aria-selected={isSelected}
                          onClick={() => {
                            setProviderId(provider.id)
                            setModelId(providerChatDefaults[provider.id]?.modelId ?? '')
                            setProviderMenuOpen(false)
                          }}
                          className={`uam-menu-select__option w-full rounded-md px-2 py-2 text-left${isSelected ? ' is-selected' : ''}`}
                          style={{
                            border: 'none',
                            color: 'var(--text)',
                            fontFamily: 'inherit',
                          }}
                        >
                          <div className="flex items-center gap-2 text-xs">
                            <ProviderLogo providerId={provider.id} />
                            <span>{provider.shortName || provider.name}</span>
                          </div>
                          <div className="truncate text-[10px]" style={{ color: 'var(--text-3)' }}>
                            {providerRuntimeDescription(provider, provider.id)}
                          </div>
                        </button>
                      )
                    })}
                  </div>
                )}
              </div>
            </div>
          )}

          <div>
            <div className="mb-1.5 text-xs font-medium" style={{ color: 'var(--text-2)' }}>Model</div>
            <MenuSelect
              label="Model"
              value={modelId}
              options={modelOptions.map((option) => ({
                value: option.id,
                label: option.label,
                description: option.detail,
              }))}
              onChange={setModelId}
            />
          </div>
        </div>

        {/* Footer */}
        <div
          className="flex items-center justify-end gap-2 px-5 py-4"
          style={{ borderTop: '1px solid var(--border)' }}
        >
          <Button
            variant="ghost"
            size="md"
            onClick={() => setNewChatModalOpen(false)}
          >
            Cancel
          </Button>
          <Button
            variant="primary"
            size="md"
            onClick={handleCreate}
            disabled={!canCreate}
          >
            Create chat
          </Button>
        </div>
      </div>
    </div>
  )
}
