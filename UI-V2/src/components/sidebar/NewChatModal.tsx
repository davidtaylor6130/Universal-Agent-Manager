import { useState, useEffect, useRef, useCallback } from 'react'
import { Download, FolderPlus, RefreshCw, SquareTerminal, TriangleAlert, X } from 'lucide-react'
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
  const providerModelCatalogs = useAppStore(useShallow((s) => s.providerModelCatalogs))
  const discoverProviderModels = useAppStore((s) => s.discoverProviderModels)
  const cliVersionManager = useAppStore(useShallow((s) => s.cliVersionManager))
  const refreshCliProviderVersion = useAppStore((s) => s.refreshCliProviderVersion)
  const applyCliProviderVersion = useAppStore((s) => s.applyCliProviderVersion)
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
  const [chatError, setChatError] = useState('')
  const [creatingWorkspace, setCreatingWorkspace] = useState(false)
  const [workspaceError, setWorkspaceError] = useState('')
  const nameRef = useRef<HTMLInputElement>(null)
  const creatingChatRef = useRef(false)
  const discoveryRequestedRef = useRef(new Set<string>())

  const requestClose = useCallback(() => {
    if (creatingChatRef.current) return
    setNewChatModalOpen(false)
  }, [setNewChatModalOpen])

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

        requestClose()
      }
    }
    window.addEventListener('keydown', handler)
    return () => window.removeEventListener('keydown', handler)
  }, [requestClose])

  const selectedProviderReadiness = cliVersionManager.providers.find((provider) => provider.providerId === providerId)
  const readinessStatus = selectedProviderReadiness?.status ?? 'unknown'
  const structuredCreationBlocked = readinessStatus === 'checking' ||
    readinessStatus === 'installing' ||
    readinessStatus === 'known-incompatible' ||
    readinessStatus === 'unavailable'
  const terminalCreationBlocked = readinessStatus === 'unavailable' ||
    (providerId === COPILOT_CLI_PROVIDER_ID && readinessStatus === 'known-incompatible')
  const supportedInstallVersion = selectedProviderReadiness?.preferredVersion ||
    selectedProviderReadiness?.availableVersions.find((version) => version.preferred)?.version ||
    selectedProviderReadiness?.selectedVersion || ''
  const readinessMessage = selectedProviderReadiness?.message || ({
    unknown: 'Provider readiness has not been checked yet.',
    checking: 'Checking whether this provider CLI can start structured chats…',
    installing: 'Installing a supported provider CLI version…',
    verified: 'This provider CLI is verified for structured chat.',
    untested: 'This provider CLI version is untested. Structured chat may still work.',
    'untested-newer': 'This provider CLI is newer than the verified range. Structured chat may still work.',
    'known-incompatible': 'This provider CLI version is known to be incompatible with structured chat.',
    unavailable: 'This provider CLI is unavailable for structured chat.',
    'provider-managed': 'CLI compatibility is managed by this provider.',
  } as const)[readinessStatus]

  const handleCreate = async (terminalFallback = false) => {
    if (creatingChatRef.current || folderId === null || !folders.some((folder) => folder.id === folderId)) {
      return
    }
    if ((!terminalFallback && structuredCreationBlocked) || (terminalFallback && terminalCreationBlocked)) return

    creatingChatRef.current = true
    setCreatingChat(true)
    setChatError('')
    const n = name.trim() || 'New chat'
	const created = await addSession(n, folderId, providerId, selectedModelId, reasoningEffortForModel(cachedAcp, selectedModelId, reasoningEffort, providerId === COPILOT_CLI_PROVIDER_ID), terminalFallback ? 'cli' : 'chat')
    if (!created) {
      creatingChatRef.current = false
      setCreatingChat(false)
      setChatError('The chat could not be created. Check the workspace and provider, then try again.')
    }
  }

  const selectedFolder =
    (folderId !== null ? folders.find((f) => f.id === folderId) : null) ?? null
  const selectedProvider = providers.find((provider) => provider.id === providerId) ?? providers[0] ?? null
	const providerSessions = sessions.filter((session) => session.providerId === providerId)
	const workspaceKey = (value: string | undefined) => (value ?? '').trim().replace(/\\/g, '/').replace(/\/+$/, '').toLowerCase()
	const selectedWorkspace = selectedFolder?.directory ?? ''
	const discoverySession = providerSessions.find((session) => workspaceKey(session.workspaceDirectory) === workspaceKey(selectedWorkspace))
	const scopedCatalog = providerModelCatalogs.find((catalog) => catalog.providerId === providerId && workspaceKey(catalog.workspaceDirectory) === workspaceKey(selectedWorkspace))
	const cachedAcp = (discoverySession ? acpBindingBySessionId[discoverySession.id] : undefined) ?? scopedCatalog
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
  const requestModelDiscovery = () => {
	if (!selectedWorkspace) return
	const discoveryKey = `${providerId}\n${workspaceKey(selectedWorkspace)}`
	discoveryRequestedRef.current.add(discoveryKey)
	void discoverProviderModels(discoverySession?.id ?? '', providerId, selectedWorkspace)
  }
  useEffect(() => {
	const discoveryKey = `${providerId}\n${workspaceKey(selectedWorkspace)}`
	if (structuredCreationBlocked || !selectedWorkspace || cachedAcp?.modelsLoading || discoveryRequestedRef.current.has(discoveryKey)) return
	requestModelDiscovery()
  }, [cachedAcp?.availableModels.length, cachedAcp?.modelsLoading, discoverProviderModels, discoverySession, providerId, selectedWorkspace, structuredCreationBlocked])
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
        if (e.target === e.currentTarget) requestClose()
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
          <IconButton icon={<X size={16} />} label="Close new chat" disabled={creatingChat} onClick={requestClose} />
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
                if (e.key === 'Enter') void handleCreate(false)
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

          <div
            role={structuredCreationBlocked ? 'alert' : 'status'}
            className="rounded-lg p-3"
            style={{
              background: structuredCreationBlocked
                ? 'color-mix(in srgb, var(--yellow) 10%, var(--surface))'
                : 'var(--surface-up)',
              border: `1px solid ${structuredCreationBlocked ? 'color-mix(in srgb, var(--yellow) 35%, var(--border))' : 'var(--border)'}`,
            }}
          >
            <div className="flex items-start gap-2">
              {structuredCreationBlocked && <TriangleAlert size={15} aria-hidden style={{ color: 'var(--yellow)', flexShrink: 0, marginTop: 1 }} />}
              <div className="min-w-0 flex-1">
                <div className="text-xs font-semibold" style={{ color: 'var(--text)' }}>Provider readiness</div>
                <p className="mt-1 text-xs" style={{ color: 'var(--text-2)' }}>{readinessMessage}</p>
                {structuredCreationBlocked && (
                  <p className="mt-1 text-xs" style={{ color: 'var(--text-2)' }}>
                    {terminalCreationBlocked
                      ? 'Terminal fallback also requires an installed, compatible provider CLI.'
                      : 'Terminal fallback remains available and does not require structured compatibility.'}
                  </p>
                )}
              </div>
            </div>
            <div className="mt-3 flex flex-wrap gap-2">
              {readinessStatus !== 'checking' && readinessStatus !== 'installing' && (
                <Button
                  variant="secondary"
                  size="sm"
                  leadingIcon={<RefreshCw size={13} />}
                  disabled={Boolean(selectedProviderReadiness?.running)}
                  onClick={() => void refreshCliProviderVersion(providerId)}
                >
                  Check again
                </Button>
              )}
              {(readinessStatus === 'known-incompatible' || readinessStatus === 'unavailable') && supportedInstallVersion && (
                <Button
                  variant="secondary"
                  size="sm"
                  leadingIcon={<Download size={13} />}
                  disabled={Boolean(selectedProviderReadiness?.running)}
                  onClick={() => void applyCliProviderVersion(providerId, supportedInstallVersion)}
                >
                  Install supported version
                </Button>
              )}
            </div>
          </div>

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
            {cachedAcp?.modelRefreshError && (
              <div role="alert" className="mt-2 flex items-center justify-between gap-2 text-xs" style={{ color: 'var(--red)' }}>
                <span>{cachedAcp.modelRefreshError}</span>
                <Button variant="secondary" size="sm" onClick={() => {
                  discoveryRequestedRef.current.delete(providerId)
                  requestModelDiscovery()
                }}>Retry</Button>
              </div>
            )}
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
        {chatError && <div role="alert" className="px-5 pt-3 text-xs" style={{ color: 'var(--red)' }}>{chatError}</div>}
        <div
          className="flex shrink-0 items-center justify-end gap-2 px-5 py-4"
          style={{ borderTop: '1px solid var(--border)' }}
        >
          <Button
            variant="ghost"
            size="md"
            onClick={requestClose}
            disabled={creatingChat}
          >
            Cancel
          </Button>
          <Button
            variant="secondary"
            size="md"
            leadingIcon={<SquareTerminal size={15} />}
            onClick={() => void handleCreate(true)}
            disabled={!canCreate || terminalCreationBlocked}
            loading={creatingChat}
          >
            Create terminal chat
          </Button>
          <Button
            variant="primary"
            size="md"
            onClick={() => void handleCreate(false)}
            disabled={!canCreate || structuredCreationBlocked}
            loading={creatingChat}
          >
            Create structured chat
          </Button>
        </div>
      </div>
    </div>
  )
}
