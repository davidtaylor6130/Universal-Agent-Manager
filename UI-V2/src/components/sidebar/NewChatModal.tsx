import { useState, useEffect, useRef } from 'react'
import { FolderPlus, TriangleAlert, X } from 'lucide-react'
import { useAppStore } from '../../store/useAppStore'
import { useShallow } from 'zustand/react/shallow'
import { ProviderLogo } from '../shared/ProviderLogo'
import { COPILOT_CLI_PROVIDER_ID, DEFAULT_PROVIDER_ID, providerCapabilities, providerRuntimeDescription } from '../../utils/providerMetadata'
import { Button, IconButton, MenuSelect } from '../ui'
import { buildCodexReasoningOptions, buildModelOptions, modelOptionFor, reasoningEffortForModel, selectedRuntimeModel } from '../chat/modelOptions'

export function NewChatModal() {
  const addSession = useAppStore((s) => s.addSession)
  const setNewChatModalOpen = useAppStore((s) => s.setNewChatModalOpen)
  const folders = useAppStore(useShallow((s) => s.folders))
  const providers = useAppStore(useShallow((s) => s.providers))
  const defaultNewChatProviderId = useAppStore((s) => s.defaultNewChatProviderId)
  const providerChatDefaults = useAppStore(useShallow((s) => s.providerChatDefaults))
	const sessions = useAppStore(useShallow((s) => s.sessions))
	const activeSessionId = useAppStore((s) => s.activeSessionId)
	const acpBindingBySessionId = useAppStore(useShallow((s) => s.acpBindingBySessionId))
  const discoverProviderModels = useAppStore((s) => s.discoverProviderModels)
  const addFolder = useAppStore((s) => s.addFolder)
  const browseFolderDirectory = useAppStore((s) => s.browseFolderDirectory)
  const newChatFolderId = useAppStore((s) => s.newChatFolderId)
  const activeFolderId = sessions.find((session) => session.id === activeSessionId)?.folderId ?? null
  const initialFolderId = [newChatFolderId, activeFolderId, folders[0]?.id]
    .find((candidate) => candidate !== null && candidate !== undefined && folders.some((folder) => folder.id === candidate)) ?? null
  const [name, setName] = useState('')
  const [folderId, setFolderId] = useState<string | null>(initialFolderId)
  const initialProviderId =
    providers.some((provider) => provider.id === defaultNewChatProviderId)
      ? defaultNewChatProviderId
      : providers[0]?.id ?? DEFAULT_PROVIDER_ID
  const [providerId, setProviderId] = useState(initialProviderId)
  const [modelId, setModelId] = useState(providerChatDefaults[initialProviderId]?.modelId ?? '')
  const [reasoningEffort, setReasoningEffort] = useState(providerChatDefaults[initialProviderId]?.reasoningEffort ?? '')
  const [creatingChat, setCreatingChat] = useState(false)
  const [creatingWorkspace, setCreatingWorkspace] = useState(false)
  const [workspaceError, setWorkspaceError] = useState('')
  const nameRef = useRef<HTMLInputElement>(null)
  const creatingChatRef = useRef(false)
  const discoveryRequestedRef = useRef(new Set<string>())

  useEffect(() => {
    nameRef.current?.focus()
  }, [])

  useEffect(() => {
    if (folders.length === 0) {
      if (folderId !== null) setFolderId(null)
      return
    }

    const folderExists = folderId !== null && folders.some((f) => f.id === folderId)
    if (!folderExists) {
      setFolderId(folders[0].id)
    }
  }, [folders, folderId])

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
	  setReasoningEffort(providerChatDefaults[nextProviderId]?.reasoningEffort ?? '')
    }
  }, [providers, providerId, defaultNewChatProviderId, providerChatDefaults])

  // Close on Escape
  useEffect(() => {
    const handler = (e: KeyboardEvent) => {
      if (e.key === 'Escape') {
        if (e.defaultPrevented || (e.target instanceof Element && e.target.closest('[role="listbox"]'))) return

        setNewChatModalOpen(false)
      }
    }
    window.addEventListener('keydown', handler)
    return () => window.removeEventListener('keydown', handler)
  }, [setNewChatModalOpen])

  const handleCreate = async () => {
    if (creatingChatRef.current || folderId === null || !folders.some((folder) => folder.id === folderId)) {
      return
    }

    creatingChatRef.current = true
    setCreatingChat(true)
    const n = name.trim() || 'New chat'
	const created = await addSession(n, folderId, providerId, selectedModelId, reasoningEffortForModel(cachedAcp, selectedModelId, reasoningEffort, providerId === COPILOT_CLI_PROVIDER_ID))
    if (!created) {
      creatingChatRef.current = false
      setCreatingChat(false)
    }
  }

  const selectedFolder =
    (folderId !== null ? folders.find((f) => f.id === folderId) : null) ?? null
  const selectedProvider = providers.find((provider) => provider.id === providerId) ?? providers[0] ?? null
	const providerSessions = sessions.filter((session) => session.providerId === providerId)
	const discoverySession = providerSessions[0]
	const cachedAcp = providerSessions.map((session) => acpBindingBySessionId[session.id]).find((binding) => (binding?.availableModels.length ?? 0) > 0)
	  ?? (discoverySession ? acpBindingBySessionId[discoverySession.id] : undefined)
	const modelOptions = buildModelOptions(cachedAcp, modelId, selectedProvider ?? undefined, providerId, true)
	const selectedModelId = modelOptionFor(modelOptions, modelId).id
	const runtimeSupportsReasoning = (selectedRuntimeModel(cachedAcp, selectedModelId)?.supportedReasoningEfforts?.length ?? 0) > 0
	const capabilities = providerCapabilities(providerId, selectedProvider ?? undefined)
	const reasoningOptions = capabilities.hasReasoningEffort || runtimeSupportsReasoning
	  ? buildCodexReasoningOptions(
	      cachedAcp,
	      selectedModelId,
	      reasoningEffort,
	      providerId === COPILOT_CLI_PROVIDER_ID ? capabilities.reasoningOptions.map((option) => option.id) : undefined
	    )
	  : []
  useEffect(() => {
    if (!discoverySession || cachedAcp?.modelsLoading || discoveryRequestedRef.current.has(providerId)) return
    discoveryRequestedRef.current.add(providerId)
    void discoverProviderModels(discoverySession.id)
  }, [cachedAcp?.availableModels.length, cachedAcp?.modelsLoading, discoverProviderModels, discoverySession, providerId])
  const canCreate = selectedFolder !== null

  const createWorkspace = async () => {
    setCreatingWorkspace(true)
    setWorkspaceError('')
    const directory = await browseFolderDirectory('')
    const name = directory?.replace(/[\\/]+$/, '').split(/[\\/]/).pop() || 'Workspace'
    const created = directory ? await addFolder(name, null, directory) : false
    setCreatingWorkspace(false)
    if (directory && !created) setWorkspaceError('The workspace could not be created. Choose another folder.')
  }

  return (
    <div
      className="fixed inset-0 z-50 flex items-center justify-center animate-fade-in"
      style={{ background: 'rgba(0,0,0,0.48)' }}
      onClick={(e) => {
        if (e.target === e.currentTarget) setNewChatModalOpen(false)
      }}
    >
      <div
        role="dialog"
        aria-modal="true"
        aria-label="New chat"
        tabIndex={-1}
        className="flex max-h-[calc(100vh-32px)] w-full max-w-lg flex-col overflow-hidden mx-4 animate-slide-in"
        style={{
          background: 'var(--surface)',
          borderRadius: 10,
          border: '1px solid var(--border-bright)',
          boxShadow: 'var(--elev-3)',
        }}
      >
        {/* Header */}
        <div
          className="flex shrink-0 items-center justify-between px-5 py-4"
          style={{ borderBottom: '1px solid var(--border)' }}
        >
          <span className="text-sm font-semibold" style={{ color: 'var(--text)' }}>
            New chat
          </span>
          <IconButton icon={<X size={16} />} label="Close new chat" onClick={() => setNewChatModalOpen(false)} />
        </div>

        <div className="min-h-0 flex-1 overflow-y-auto p-5 space-y-5">
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
              onKeyDown={(e) => {
                if (e.key === 'Enter') void handleCreate()
              }}
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
              <MenuSelect
                label="Folder"
                value={folderId ?? ''}
                options={folders.map((folder) => ({ value: folder.id, label: folder.name, description: folder.directory }))}
                onChange={setFolderId}
              />
            </div>
          )}

          {!selectedFolder && (
            <div role="alert" className="rounded-lg p-3 animate-fade-in" style={{ background: 'color-mix(in srgb, var(--yellow) 12%, transparent)', border: '1px solid color-mix(in srgb, var(--yellow) 35%, var(--border))' }}>
              <div className="flex items-start gap-2">
                <TriangleAlert size={16} aria-hidden style={{ color: 'var(--yellow)', flexShrink: 0 }} />
                <div className="min-w-0 flex-1">
                  <div className="text-sm font-medium" style={{ color: 'var(--text)' }}>A workspace is required</div>
                  <p className="mt-1 text-xs" style={{ color: 'var(--text-2)' }}>Agents cannot start a chat without a valid workspace folder.</p>
                  {workspaceError && <p className="mt-1 text-xs" style={{ color: 'var(--red)' }}>{workspaceError}</p>}
                  <Button className="mt-3" variant="secondary" size="sm" leadingIcon={<FolderPlus size={14} />} disabled={creatingWorkspace} onClick={() => void createWorkspace()}>
                    {creatingWorkspace ? 'Choosing…' : 'Create workspace'}
                  </Button>
                </div>
              </div>
            </div>
          )}

          {providers.length > 1 && (
            <div>
              <label className="text-xs font-medium mb-1.5 block" style={{ color: 'var(--text-2)' }}>
                Provider
              </label>
              <MenuSelect
                label="Provider"
                value={providerId}
                options={providers.map((provider) => ({
                  value: provider.id,
                  label: provider.shortName || provider.name,
                  description: providerRuntimeDescription(provider, provider.id),
                  icon: <ProviderLogo providerId={provider.id} />,
                }))}
                onChange={(nextProviderId) => {
                  setProviderId(nextProviderId)
                  setModelId(providerChatDefaults[nextProviderId]?.modelId ?? '')
				  setReasoningEffort(providerChatDefaults[nextProviderId]?.reasoningEffort ?? '')
                }}
              />
            </div>
          )}

          <div>
            <div className="mb-1.5 text-xs font-medium" style={{ color: 'var(--text-2)' }}>Model{cachedAcp?.modelsLoading ? ' · discovering…' : ''}</div>
            <MenuSelect
              label="Model"
              value={selectedModelId}
              options={modelOptions.map((option) => ({
                value: option.id,
                label: option.label,
                description: option.detail,
              }))}
			  onChange={(nextModelId) => {
				setModelId(nextModelId)
				setReasoningEffort(reasoningEffortForModel(cachedAcp, nextModelId, reasoningEffort, providerId === COPILOT_CLI_PROVIDER_ID))
			  }}
            />
          </div>
		  {reasoningOptions.length > 0 && (
			<div>
			  <div className="mb-1.5 text-xs font-medium" style={{ color: 'var(--text-2)' }}>Reasoning effort</div>
			  <MenuSelect
				label="Reasoning effort"
				value={reasoningEffortForModel(cachedAcp, selectedModelId, reasoningEffort, providerId === COPILOT_CLI_PROVIDER_ID)}
				options={reasoningOptions.map((option) => ({ value: option.id, label: option.label, description: option.detail }))}
				onChange={setReasoningEffort}
			  />
			</div>
		  )}
        </div>

        {/* Footer */}
        <div
          className="flex shrink-0 items-center justify-end gap-2 px-5 py-4"
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
            onClick={() => void handleCreate()}
            disabled={!canCreate}
            loading={creatingChat}
          >
            Create chat
          </Button>
        </div>
      </div>
    </div>
  )
}
