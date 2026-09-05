import { useState, useEffect, useRef, useCallback } from 'react'
import { FolderPlus, Monitor, RefreshCw, SquareTerminal, X } from 'lucide-react'
import { useAppStore } from '../../store/useAppStore'
import { useShallow } from 'zustand/react/shallow'
import type { Provider } from '../../types/provider'
import { SelectionGrid } from '../shared/SelectionGrid'
import { StatusIndicator } from '../shared/StatusIndicator'
import { ProviderLogo } from '../shared/ProviderLogo'
import { COPILOT_CLI_PROVIDER_ID, DEFAULT_PROVIDER_ID, providerCapabilities } from '../../utils/providerMetadata'
import { Button, IconButton, MenuSelect } from '../ui'
import { buildCodexReasoningOptions, buildModelOptions, modelOptionFor, reasoningEffortForModel, selectedRuntimeModel } from '../chat/modelOptions'
import { isAbsoluteRemoteWorkspace } from '../../utils/remoteWorkspace'
import { RemoteDirectoryBrowser } from './RemoteDirectoryBrowser'

export function NewChatModal() {
  const addSession = useAppStore((s) => s.addSession)
  const setNewChatModalOpen = useAppStore((s) => s.setNewChatModalOpen)
  const folders = useAppStore(useShallow((s) => s.folders))
  const providers = useAppStore(useShallow((s) => s.providers))
  const defaultNewChatProviderId = useAppStore((s) => s.defaultNewChatProviderId)
  const providerChatDefaults = useAppStore(useShallow((s) => s.providerChatDefaults))
	const executionHosts = useAppStore(useShallow((s) => s.executionHosts))
	const sessions = useAppStore(useShallow((s) => s.sessions))
	const activeSessionId = useAppStore((s) => s.activeSessionId)
	const acpBindingBySessionId = useAppStore(useShallow((s) => s.acpBindingBySessionId))
  const providerModelCatalogs = useAppStore(useShallow((s) => s.providerModelCatalogs))
  const discoverProviderModels = useAppStore((s) => s.discoverProviderModels)
  const cliVersionManager = useAppStore(useShallow((s) => s.cliVersionManager))
  const addFolder = useAppStore((s) => s.addFolder)
  const browseFolderDirectory = useAppStore((s) => s.browseFolderDirectory)
  const newChatFolderId = useAppStore((s) => s.newChatFolderId)
  const activeFolderId = sessions.find((session) => session.id === activeSessionId)?.folderId ?? null
  const initialFolderId = [newChatFolderId, activeFolderId, folders[0]?.id]
    .find((candidate) => candidate !== null && candidate !== undefined && folders.some((folder) => folder.id === candidate)) ?? null
  const initialFolder = folders.find((folder) => folder.id === initialFolderId) ?? null
  const [name, setName] = useState('')
  const [folderId, setFolderId] = useState<string | null>(initialFolderId)
  const initialIsRemote = Boolean(initialFolder?.executionHostId && initialFolder.executionHostId !== 'local')
  const initialProviderId =
    providers.some((provider) => provider.id === defaultNewChatProviderId)
      ? defaultNewChatProviderId
      : providers[0]?.id ?? DEFAULT_PROVIDER_ID
  const [providerId, setProviderId] = useState(initialProviderId)
	const [executionHostId, setExecutionHostId] = useState(initialFolder?.executionHostId || 'local')
	const [remoteWorkspace, setRemoteWorkspace] = useState(initialFolder?.executionHostId && initialFolder.executionHostId !== 'local' ? initialFolder.directory : '')
  const [modelId, setModelId] = useState(initialIsRemote ? '' : providerChatDefaults[initialProviderId]?.modelId ?? '')
  const [reasoningEffort, setReasoningEffort] = useState(initialIsRemote ? '' : providerChatDefaults[initialProviderId]?.reasoningEffort ?? '')
  const [creatingChat, setCreatingChat] = useState(false)
  const [chatError, setChatError] = useState('')
  const [creatingWorkspace, setCreatingWorkspace] = useState(false)
  const [workspaceError, setWorkspaceError] = useState('')
  const [remoteBrowserOpen, setRemoteBrowserOpen] = useState(false)
  const nameRef = useRef<HTMLInputElement>(null)
  const creatingChatRef = useRef(false)
  const discoveryRequestedRef = useRef(new Set<string>())
  const workspaceChoices = useRef<Record<string, { folderId: string | null; path: string }>>({})
  const providerChoices = useRef<Record<string, { modelId: string; effort: string }>>({})
  const requestedFolderRef = useRef(initialFolder?.id === newChatFolderId ? newChatFolderId : null)

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

    if (folderId === null && executionHostId !== 'local') return
    const folderExists = folderId !== null && folders.some((f) => f.id === folderId && (f.executionHostId || 'local') === executionHostId)
    if (!folderExists) {
	  const compatible = folders.find((folder) => (folder.executionHostId || 'local') === executionHostId)
	  setFolderId(compatible?.id ?? null)
	  if (compatible && executionHostId !== 'local') setRemoteWorkspace(compatible.directory)
    }
  }, [executionHostId, folders, folderId])

  useEffect(() => {
    if (newChatFolderId === null || requestedFolderRef.current === newChatFolderId) return
    const folder = folders.find((candidate) => candidate.id === newChatFolderId)
    if (folder) {
      requestedFolderRef.current = newChatFolderId
      setFolderId(newChatFolderId)
	  setExecutionHostId(folder.executionHostId || 'local')
	  if ((folder.executionHostId || 'local') !== 'local') setRemoteWorkspace(folder.directory)
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
        if (remoteBrowserOpen) {
          setRemoteBrowserOpen(false)
          return
        }

        requestClose()
      }
    }
    window.addEventListener('keydown', handler)
    return () => window.removeEventListener('keydown', handler)
  }, [remoteBrowserOpen, requestClose])

  const selectedFolder =
    (folderId !== null ? folders.find((f) => f.id === folderId && (f.executionHostId || 'local') === executionHostId) : null) ?? null
  const selectedProvider = providers.find((provider) => provider.id === providerId)
  const selectedExecutionHost = executionHosts.find((host) => host.id === executionHostId)
  const isRemote = executionHostId !== 'local'
  const hostIssue = !selectedExecutionHost ? 'This computer is no longer available. Choose another computer.'
    : isRemote && selectedExecutionHost.runnerStatus !== 'ready'
      ? `Remote runner is ${selectedExecutionHost.runnerStatus}. Open Settings to configure this computer.` : ''

  // Use the same availability rules for the grid and the Create action.
  const providerAvailability = (provider: Provider | undefined) => {
    const readiness = !isRemote ? cliVersionManager.providers.find((item) => item.providerId === provider?.id) : undefined
    const status = readiness?.status ?? 'unknown'
    const structuredBlocked = !provider || provider.supportsStructured === false ||
      (!isRemote && ['checking', 'installing', 'known-incompatible', 'unavailable'].includes(status))
    const terminalBlocked = !provider || provider.supportsCli === false ||
      (!isRemote && (status === 'unavailable' || (provider.id === COPILOT_CLI_PROVIDER_ID && status === 'known-incompatible')))
    const issues: string[] = []
    if (!provider) issues.push('No provider is available. Configure a provider in Settings.')
    else {
      if (provider.supportsStructured === false) issues.push('This provider does not support structured chat. Choose another provider or check Settings.')
      if (provider.supportsCli === false) issues.push('This provider does not support terminal chat. Choose another provider or check Settings.')
      if (!isRemote && !['verified', 'provider-managed'].includes(status)) {
        const message = readiness?.message || ({
          unknown: 'Provider readiness has not been checked.',
          checking: 'Checking provider compatibility.',
          installing: 'Installing the provider CLI.',
          untested: 'This provider version has not been verified for structured chat.',
          'untested-newer': 'This provider version is newer than the verified range.',
          'known-incompatible': 'This provider version is incompatible with structured chat.',
          unavailable: 'The provider CLI is unavailable.',
        } as Record<string, string>)[status]
        issues.push(`${message}${provider.id === COPILOT_CLI_PROVIDER_ID && status === 'known-incompatible' ? ' Terminal fallback is also unavailable.' : ''} Check Settings > CLI Version.`)
      }
    }
    return { structuredBlocked, terminalBlocked, issues }
  }
  const availability = providerAvailability(selectedProvider)
  const structuredCreationBlocked = Boolean(hostIssue) || availability.structuredBlocked
  const terminalCreationBlocked = Boolean(hostIssue) || availability.terminalBlocked
  const terminalFallback = structuredCreationBlocked && !terminalCreationBlocked

  const handleCreate = async () => {
    if (creatingChatRef.current || !canCreate || (structuredCreationBlocked && terminalCreationBlocked)) return
    creatingChatRef.current = true
    setCreatingChat(true)
    setChatError('')
    const n = name.trim() || 'New chat'
    const effort = reasoningOptions.length === 0 ? '' : reasoningEffortForModel(cachedAcp, selectedModelId, reasoningEffort, providerId === COPILOT_CLI_PROVIDER_ID)
    try {
      const created = isRemote
        ? await addSession(n, selectedFolder?.id ?? null, providerId, selectedModelId, effort, terminalFallback ? 'cli' : 'chat', executionHostId, selectedWorkspace)
        : await addSession(n, selectedFolder!.id, providerId, selectedModelId, effort, terminalFallback ? 'cli' : 'chat')
      if (created) return // The store closes the modal only after successful creation.
      setChatError('The chat could not be created. Check the workspace and provider, then try again.')
    } catch (error) {
      setChatError(error instanceof Error ? error.message : 'The chat could not be created. Try again.')
    }
    creatingChatRef.current = false
    setCreatingChat(false)
  }

	const providerSessions = sessions.filter((session) => session.providerId === providerId)
	const workspaceKey = (value: string | undefined) => {
	  const normalized = (value ?? '').trim().replace(/\\/g, '/').replace(/\/+$/, '')
	  return isRemote && selectedExecutionHost?.platform.toLowerCase() !== 'windows'
		? normalized
		: normalized.toLowerCase()
	}
	const selectedWorkspace = isRemote ? selectedFolder?.directory ?? remoteWorkspace.trim() : selectedFolder?.directory ?? ''
	const discoverySession = providerSessions.find((session) => (session.executionHostId ?? 'local') === executionHostId && workspaceKey(session.workspaceDirectory) === workspaceKey(selectedWorkspace))
	const scopedCatalog = providerModelCatalogs.find((catalog) => catalog.providerId === providerId && catalog.executionHostId === executionHostId && workspaceKey(catalog.workspaceDirectory) === workspaceKey(selectedWorkspace))
	const cachedAcp = (discoverySession ? acpBindingBySessionId[discoverySession.id] : undefined) ?? scopedCatalog
	const modelOptions = buildModelOptions(cachedAcp, modelId, selectedProvider ?? undefined, providerId, true)
	const selectedModelId = modelOptionFor(modelOptions, modelId).id
	const runtimeSupportsReasoning = (selectedRuntimeModel(cachedAcp, selectedModelId)?.supportedReasoningEfforts?.length ?? 0) > 0
	const capabilities = providerCapabilities(providerId, selectedProvider ?? undefined)
	const reasoningOptions = (capabilities.hasReasoningEffort || runtimeSupportsReasoning) && !(selectedRuntimeModel(cachedAcp, selectedModelId)?.supportedReasoningEfforts?.length === 0)
	  ? buildCodexReasoningOptions(
	      cachedAcp,
	      selectedModelId,
	      reasoningEffort,
	      providerId === COPILOT_CLI_PROVIDER_ID ? capabilities.reasoningOptions.map((option) => option.id) : undefined
	    )
	  : []
  const requestModelDiscovery = () => {
	if (structuredCreationBlocked || !selectedWorkspace || (isRemote && !isAbsoluteRemoteWorkspace(selectedExecutionHost?.platform, selectedWorkspace))) return
	const discoveryKey = `${providerId}\n${executionHostId}\n${workspaceKey(selectedWorkspace)}`
	discoveryRequestedRef.current.add(discoveryKey)
	void discoverProviderModels(isRemote ? '' : discoverySession?.id ?? '', providerId, selectedWorkspace, executionHostId)
  }
  useEffect(() => {
	const discoveryKey = `${providerId}\n${executionHostId}\n${workspaceKey(selectedWorkspace)}`
	if (structuredCreationBlocked || !selectedWorkspace || (isRemote && !isAbsoluteRemoteWorkspace(selectedExecutionHost?.platform, selectedWorkspace)) || cachedAcp?.modelsLoading || discoveryRequestedRef.current.has(discoveryKey)) return
	if (!isRemote) requestModelDiscovery()
	}, [cachedAcp?.availableModels.length, cachedAcp?.modelsLoading, discoverProviderModels, discoverySession, executionHostId, isRemote, providerId, selectedExecutionHost?.platform, selectedWorkspace, structuredCreationBlocked])
  const canCreate = Boolean(selectedExecutionHost) && (isRemote
    ? isAbsoluteRemoteWorkspace(selectedExecutionHost?.platform, selectedWorkspace)
    : selectedFolder !== null && Boolean(selectedWorkspace))

  const workspaceIssue = workspaceError || (!canCreate
    ? isRemote ? 'Enter an absolute workspace path.' : 'Choose a workspace.' : '')
  const createWorkspace = async () => {
    if (creatingWorkspace) return
    setCreatingWorkspace(true)
    setWorkspaceError('')
    try {
      const directory = await browseFolderDirectory('')
      const workspaceName = directory?.replace(/[\\/]+$/, '').split(/[\\/]/).pop() || 'Workspace'
      const created = directory ? await addFolder(workspaceName, null, directory) : false
      if (directory && !created) setWorkspaceError('The workspace could not be created. Choose another folder.')
    } catch (error) {
      setWorkspaceError(error instanceof Error ? error.message : 'The workspace could not be created. Try again.')
    } finally {
      setCreatingWorkspace(false)
    }
  }

  const chooseProvider = (nextProviderId: string, nextHostId = executionHostId) => {
    providerChoices.current[`${executionHostId}\n${providerId}`] = { modelId, effort: reasoningEffort }
    const saved = providerChoices.current[`${nextHostId}\n${nextProviderId}`]
    const defaults = nextHostId === 'local' ? providerChatDefaults[nextProviderId] : undefined
    setProviderId(nextProviderId)
    setModelId(saved?.modelId ?? defaults?.modelId ?? '')
    setReasoningEffort(saved?.effort ?? defaults?.reasoningEffort ?? '')
  }
  const chooseHost = (hostId: string) => {
    workspaceChoices.current[executionHostId] = { folderId, path: remoteWorkspace }
    const saved = workspaceChoices.current[hostId]
    const compatible = folders.find((folder) => (folder.executionHostId || 'local') === hostId)
    chooseProvider(providerId, hostId)
    setExecutionHostId(hostId)
    setFolderId(saved ? saved.folderId : compatible?.id ?? null)
    setRemoteWorkspace(saved ? saved.path : hostId !== 'local' ? compatible?.directory ?? '' : '')
    setWorkspaceError('')
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
          <IconButton icon={<X size={16} />} variant="danger" label="Close new chat" disabled={creatingChat} onClick={requestClose} />
        </div>

        <div className="min-h-0 flex-1 overflow-y-auto p-5 space-y-5">
          {/* Name */}
          <div>
            <input
              ref={nameRef}
              aria-label="Chat name"
              type="text"
              value={name}
              onChange={(e) => setName(e.target.value)}
              onKeyDown={(e) => {
                if (e.key === 'Enter') void handleCreate()
              }}
              placeholder="Chat name"
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

          <div>
          <div className="mb-1.5 text-xs font-medium" style={{ color: 'var(--text)' }}>Runs on</div>
          <SelectionGrid
            label="Runs on"
            value={executionHostId}
            options={executionHosts.map((host) => ({
              id: host.id,
              label: host.label,
              icon: <Monitor size={18} aria-hidden />,
              disabled: creatingChat || (host.transport === 'ssh' && host.runnerStatus !== 'ready'),
              issues: host.transport === 'ssh' && host.runnerStatus !== 'ready'
                ? [`Remote runner is ${host.runnerStatus}. Open Settings to configure this computer.`] : [],
            }))}
            onChange={chooseHost}
          />
          </div>
          {hostIssue && <p role="alert" className="text-xs" style={{ color: 'var(--red)' }}>{hostIssue}</p>}

          {/* Folder */}
          {folders.some((folder) => (folder.executionHostId || 'local') === executionHostId) && (
            <div>
              <label className="text-xs font-medium mb-1.5 block" style={{ color: 'var(--text-2)' }}>
                Folder
              </label>
              <MenuSelect
                label="Folder"
                value={folderId ?? ''}
                options={[
				  ...(isRemote ? [{ value: '', label: 'New remote workspace…', description: `Create on ${selectedExecutionHost?.label}` }] : []),
				  ...folders.filter((folder) => (folder.executionHostId || 'local') === executionHostId)
					.map((folder) => ({ value: folder.id, label: folder.name, description: folder.directory })),
				]}
                onChange={(nextFolderId) => {
				  setFolderId(nextFolderId || null)
				  const folder = folders.find((candidate) => candidate.id === nextFolderId)
				  if (folder && isRemote) setRemoteWorkspace(folder.directory)
				}}
              />
            </div>
          )}

          {isRemote && (
            <div>
              <label className="text-xs font-medium mb-1.5 block" style={{ color: 'var(--text-2)' }}>
                Remote workspace path
              </label>
              <div className="flex items-center gap-2">
                <div className="relative min-w-0 flex-1">
                <input
                  aria-label="Remote workspace path"
                  aria-invalid={!canCreate || undefined}
                  aria-describedby={workspaceIssue ? 'new-chat-workspace-error' : undefined}
                  type="text"
                  value={remoteWorkspace}
                  onChange={(event) => setRemoteWorkspace(event.target.value)}
                  readOnly={selectedFolder !== null}
                  placeholder="/absolute/path/on/selected/host"
                  className="w-full rounded-md pl-3 pr-10 py-2 text-sm outline-none"
                  style={{ background: 'var(--surface-up)', border: '1px solid var(--border)', color: 'var(--text)', fontFamily: 'var(--font-mono)' }}
                />
                <span className="absolute right-3 top-1/2 -translate-y-1/2"><StatusIndicator issues={workspaceIssue ? [workspaceIssue] : []} okLabel="Absolute workspace path" /></span>
                </div>
                {!selectedFolder && (
                  <Button
                    variant="secondary"
                    size="md"
                    disabled={selectedExecutionHost?.runnerStatus !== 'ready'}
                    onClick={() => setRemoteBrowserOpen(true)}
                  >
                    Browse
                  </Button>
                )}
              </div>
              <p className="mt-1 text-xs" style={{ color: 'var(--text-3)' }}>
				{selectedFolder ? 'This path is owned by the selected workspace.' : `Enter an absolute path interpreted only by ${selectedExecutionHost?.label}.`} Computer Use is disabled for remote chats.
              </p>
            </div>
          )}

          {!selectedFolder && !isRemote && <div className="flex items-center gap-2">
            <Button className="flex-1" variant="secondary" size="sm" leadingIcon={<FolderPlus size={14} />} disabled={creatingWorkspace} onClick={() => void createWorkspace()}>
              {creatingWorkspace ? 'Choosing…' : 'Choose workspace'}
            </Button>
            <StatusIndicator issues={[workspaceIssue]} />
          </div>}
          {workspaceIssue && <p id="new-chat-workspace-error" role="alert" className="text-xs" style={{ color: 'var(--red)' }}>{workspaceIssue}</p>}
          <div>
          <div className="mb-1.5 text-xs font-medium" style={{ color: 'var(--text)' }}>Provider</div>
          <SelectionGrid
            label="Provider"
            value={providerId}
            options={providers.map((provider) => {
              const state = providerAvailability(provider)
              return {
                id: provider.id,
                label: provider.shortName || provider.name,
                icon: <ProviderLogo providerId={provider.id} />,
                disabled: creatingChat || (state.structuredBlocked && state.terminalBlocked),
                issues: state.issues,
              }
            })}
            onChange={chooseProvider}
          />
          </div>
          {availability.issues.length > 0 && <p className="text-xs" style={{ color: 'var(--text-2)' }}>{availability.issues[availability.issues.length - 1]}</p>}
          {terminalFallback && <p role="status" className="flex items-center gap-2 text-xs" style={{ color: 'var(--text)' }}><SquareTerminal size={14} aria-hidden />Creates a terminal chat. Structured chat is unavailable.</p>}

          <div role="group" aria-label="Model and reasoning" className="grid grid-cols-1 sm:grid-cols-[minmax(0,1fr)_auto] gap-3 items-start">
          <div>
            <div className="mb-1.5 text-xs font-medium" style={{ color: 'var(--text-2)' }}>Model{cachedAcp?.modelsLoading ? ' · discovering…' : ''}</div>
            <div className="flex items-center gap-2"><div className="min-w-0 flex-1"><MenuSelect
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
            /></div>
            <StatusIndicator issues={cachedAcp?.modelRefreshError ? [cachedAcp.modelRefreshError] : cachedAcp?.modelsLoading ? ['Discovering models.'] : []} okLabel="Model options available" />
            </div>
			{isRemote && (
			  <Button className="mt-2" variant="secondary" size="sm" leadingIcon={<RefreshCw size={13} />} disabled={!canCreate || structuredCreationBlocked || Boolean(cachedAcp?.modelsLoading)} onClick={requestModelDiscovery}>
				{cachedAcp?.modelsLoading ? 'Discovering remote models…' : 'Discover remote models'}
			  </Button>
			)}
            {cachedAcp?.modelRefreshError && (
              <div role="alert" className="mt-2 flex items-center justify-between gap-2 text-xs" style={{ color: 'var(--red)' }}>
                <span>{cachedAcp.modelRefreshError}</span>
                <Button variant="secondary" size="sm" onClick={() => {
				  discoveryRequestedRef.current.delete(`${providerId}\n${executionHostId}\n${workspaceKey(selectedWorkspace)}`)
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
        </div>

        {remoteBrowserOpen && selectedExecutionHost && (
          <RemoteDirectoryBrowser
            host={selectedExecutionHost}
            initialPath={remoteWorkspace}
            onCancel={() => setRemoteBrowserOpen(false)}
            onSelect={(directory) => {
              setRemoteWorkspace(directory)
              setRemoteBrowserOpen(false)
            }}
          />
        )}

        {/* Footer */}
        {chatError && <div role="alert" className="px-5 pt-3 text-xs" style={{ color: 'var(--red)' }}>{chatError}</div>}
        <div
          className="flex shrink-0 items-center justify-end gap-2 px-5 py-4"
          style={{ borderTop: '1px solid var(--border)' }}
        >
          <Button
            block
            variant="primary"
            size="md"
            onClick={() => void handleCreate()}
            disabled={creatingChat || !canCreate || (structuredCreationBlocked && terminalCreationBlocked)}
            aria-busy={creatingChat || undefined}
          >
            {creatingChat ? 'Creating…' : 'Create'}
          </Button>
        </div>
      </div>
    </div>
  )
}
