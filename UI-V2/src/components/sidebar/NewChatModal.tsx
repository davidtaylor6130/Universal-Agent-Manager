import { defaultsForExecutionHost } from '../../utils/settingsScope'
import { ExecutionRoutingDialog } from '../modals/ExecutionRoutingDialog'
import { RUNNER_STATUS } from '../settings/ExecutionRoutingSettings'
import { canRouteAutomatically, sanitizeRepositoryMatches, type WorkspaceRepositoryMatches } from '../../types/executionRouting'
import { isCefContext, sendToCEF } from '../../ipc/cefBridge'
import { SSH_ENABLED } from '../../config/buildFeatures'
import { useState, useEffect, useRef, useCallback } from 'react'
import { FolderPlus, Monitor, RefreshCw, Route, SquareTerminal, X } from 'lucide-react'
import { useAppStore } from '../../store/useAppStore'
import { useShallow } from 'zustand/react/shallow'
import type { Provider } from '../../types/provider'
import { SelectionGrid } from '../shared/SelectionGrid'
import { WorkspacePicker } from './WorkspacePicker'
import { StatusIndicator } from '../shared/StatusIndicator'
import { COPILOT_CLI_PROVIDER_ID, DEFAULT_PROVIDER_ID, providerCapabilities } from '../../utils/providerMetadata'
import { Button, IconButton, MenuSelect, Notice, useOverlayState } from '../ui'
import { buildCodexReasoningOptions, buildModelOptions, modelCatalogForDefaults, modelOptionFor, reasoningEffortForModel, selectedRuntimeModel } from '../chat/modelOptions'
import { ModelPickerContent } from '../chat/ModelPickerContent'
import { isAbsoluteRemoteWorkspace } from '../../utils/remoteWorkspace'
import { RemoteDirectoryBrowser } from './RemoteDirectoryBrowser'
import { NewChatBranchPicker } from './NewChatBranchPicker'

const AUTOMATIC_HOST_OPTION = 'uam:auto-routing'

export function NewChatModal({ companion = false, onCreated }: { companion?: boolean; onCreated?: () => void }) {
  const overlayState = useOverlayState()
  const addSession = useAppStore((s) => s.addSession)
  const setNewChatModalOpen = useAppStore((s) => s.setNewChatModalOpen)
  const folders = useAppStore(useShallow((s) => s.folders))
  const providers = useAppStore(useShallow((s) => s.providers))
  const globalDefaultNewChatProviderId = useAppStore((s) => s.defaultNewChatProviderId)
  const globalProviderChatDefaults = useAppStore(useShallow((s) => s.providerChatDefaults))
  const hostScopedSettingsValues = useAppStore(useShallow((s) => s.hostScopedSettingsValues))
	const executionHosts = useAppStore(useShallow((s) => s.executionHosts))
	const sessions = useAppStore(useShallow((s) => s.sessions))
	const resourceCollections = useAppStore(useShallow((s) => s.resourceCollections))
	const activeSessionId = useAppStore((s) => s.activeSessionId)
	const acpBindingBySessionId = useAppStore(useShallow((s) => s.acpBindingBySessionId))
  const providerModelCatalogs = useAppStore(useShallow((s) => s.providerModelCatalogs))
  const discoverProviderModels = useAppStore((s) => s.discoverProviderModels)
  const cliVersionManager = useAppStore(useShallow((s) => s.cliVersionManager))
  const addFolder = useAppStore((s) => s.addFolder)
  const browseFolderDirectory = useAppStore((s) => s.browseFolderDirectory)
  const newChatFolderId = useAppStore((s) => s.newChatFolderId)
  const newChatWorktreeContext = useAppStore((s) => s.newChatWorktreeContext)
  const gitEnabled = useAppStore((s) => s.gitEnabled)
  const gitAutoWorktree = useAppStore((s) => s.gitAutoWorktree)
  const gitDefaultBaseBranch = useAppStore((s) => s.gitDefaultBaseBranch)
  const activeFolderId = sessions.find((session) => session.id === activeSessionId)?.folderId ?? null
  const initialFolderId = [newChatFolderId, activeFolderId, folders[0]?.id]
    .find((candidate) => candidate !== null && candidate !== undefined && folders.some((folder) => folder.id === candidate)) ?? null
  const initialFolder = folders.find((folder) => folder.id === initialFolderId) ?? null
  const executionRouting = useAppStore((state) => state.executionRouting)
  const repositoryMatching = executionRouting.repositoryMatching === true
  const [repositories, setRepositories] = useState<WorkspaceRepositoryMatches[]>([])
  const [routingOpen, setRoutingOpen] = useState(false)
  const [routingMode, setRoutingMode] = useState<'explicit' | 'automatic'>('explicit')
  const [name, setName] = useState('')
  const [sourceContextInvalid, setSourceContextInvalid] = useState(false)
  const [branchChoice, setBranchChoice] = useState<{ scope: string; branch: string | undefined } | null>(null)
  const [folderId, setFolderId] = useState<string | null>(initialFolderId)
  const defaultState = { defaultNewChatProviderId: globalDefaultNewChatProviderId, providerChatDefaults: globalProviderChatDefaults, hostScopedSettingsValues, executionHosts }
  const initialDefaults = defaultsForExecutionHost(defaultState, initialFolder?.executionHostId || 'local')
  const initialProviderId =
    providers.some((provider) => provider.id === initialDefaults.defaultNewChatProviderId)
      ? initialDefaults.defaultNewChatProviderId
      : providers[0]?.id ?? DEFAULT_PROVIDER_ID
  const [providerId, setProviderId] = useState(initialProviderId)
	const [executionHostId, setExecutionHostId] = useState(initialFolder?.executionHostId || 'local')
  const effectiveDefaults = routingMode === 'explicit' ? defaultsForExecutionHost(defaultState, executionHostId) : defaultState
  const defaultNewChatProviderId = effectiveDefaults.defaultNewChatProviderId
  const providerChatDefaults = effectiveDefaults.providerChatDefaults
  const providerChoiceExplicit = useRef(false)
	const [remoteWorkspace, setRemoteWorkspace] = useState(initialFolder?.executionHostId && initialFolder.executionHostId !== 'local' ? initialFolder.directory : '')
  const [modelId, setModelId] = useState(initialDefaults.providerChatDefaults[initialProviderId]?.modelId ?? '')
  const [reasoningEffort, setReasoningEffort] = useState(initialDefaults.providerChatDefaults[initialProviderId]?.reasoningEffort ?? '')
  const [creatingChat, setCreatingChat] = useState(false)
  const [chatError, setChatError] = useState('')
  const [creatingWorkspace, setCreatingWorkspace] = useState(false)
  const [workspaceError, setWorkspaceError] = useState('')
  const [remoteBrowserOpen, setRemoteBrowserOpen] = useState(false)
  const nameRef = useRef<HTMLInputElement>(null)
  const creatingChatRef = useRef(false)
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

  // One match request per open: this modal mounts per open, so the list is cached for its lifetime.
  useEffect(() => {
    if (!repositoryMatching) return
    let cancelled = false
    void sendToCEF({ action: 'listRepositoryMatches', payload: {} }).then((response) => {
      if (!cancelled && response.ok) setRepositories(sanitizeRepositoryMatches(response.data))
    })
    return () => { cancelled = true }
  }, [repositoryMatching])

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
      if ((folder.executionHostId || 'local') !== executionHostId) chooseHost(folder.executionHostId || 'local')
      setFolderId(newChatFolderId)
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

        if (routingOpen) return
        requestClose()
      }
    }
    window.addEventListener('keydown', handler)
    return () => window.removeEventListener('keydown', handler)
  }, [remoteBrowserOpen, routingOpen, requestClose])

  const selectedFolder =
    (folderId !== null ? folders.find((f) => f.id === folderId && (f.executionHostId || 'local') === executionHostId) : null) ?? null
  const automaticAvailable = canRouteAutomatically(executionRouting, selectedFolder ?? undefined, providerId, repositories.find((item) => item.folderId === selectedFolder?.id))
  useEffect(() => { if (!automaticAvailable) setRoutingMode('explicit') }, [automaticAvailable])
  useEffect(() => {
    if (!newChatWorktreeContext) return
    const source = sessions.find((candidate) => candidate.id === newChatWorktreeContext.sourceChatId)
    if (!source || source.importedReadOnly || source.workspaceIsolationKind !== 'gitWorktree' || source.folderId !== folderId ||
      (source.executionHostId || 'local') !== newChatWorktreeContext.expectedExecutionHostId || executionHostId !== newChatWorktreeContext.expectedExecutionHostId ||
      source.workspaceWorktreeDirectory !== newChatWorktreeContext.expectedWorkspaceDirectory || routingMode !== 'explicit') {
      setSourceContextInvalid(true)
      setChatError('The selected worktree changed. Choose a workspace again before creating a chat.')
      setNewChatModalOpen(true, folderId, null)
    }
  }, [newChatWorktreeContext, sessions, folderId, executionHostId, routingMode, setNewChatModalOpen])
  const selectedProvider = providers.find((provider) => provider.id === providerId)
  const selectedExecutionHost = executionHosts.find((host) => host.id === executionHostId)
  const isRemote = executionHostId !== 'local'
  const hostIssue = !selectedExecutionHost ? 'This computer is no longer available. Choose another computer.'
    : routingMode !== 'automatic' && isRemote && selectedExecutionHost.runnerStatus !== 'ready'
      ? `Remote runner is ${selectedExecutionHost.runnerStatus}. Open Settings to configure this computer.` : ''

  // Use the same availability rules for the picker and the Create action.
  const providerAvailability = (provider: Provider | undefined) => {
    const readiness = routingMode !== 'automatic' && !isRemote ? cliVersionManager.providers.find((item) => item.providerId === provider?.id) : undefined
    const status = readiness?.status ?? 'unknown'
    const structuredBlocked = !provider || provider.supportsStructured === false ||
      (routingMode !== 'automatic' && !isRemote && ['checking', 'installing', 'known-incompatible', 'unavailable'].includes(status))
    const terminalBlocked = !provider || provider.supportsCli === false ||
      (routingMode !== 'automatic' && !isRemote && (status === 'unavailable' || (provider.id === COPILOT_CLI_PROVIDER_ID && status === 'known-incompatible')))
    const issues: string[] = []
    if (!provider) issues.push('No provider is available. Configure a provider in Settings.')
    else {
      if (provider.supportsStructured === false) issues.push(provider.supportsCli !== false ? 'This provider supports terminal chat only.' : 'This provider does not support structured chat. Choose another provider or check Settings.')
      if (provider.supportsCli === false) issues.push('This provider does not support terminal chat. Choose another provider or check Settings.')
      if (routingMode !== 'automatic' && !isRemote && (provider.supportsStructured !== false || readiness) && (['checking', 'installing', 'known-incompatible', 'unavailable'].includes(status) || Boolean(readiness?.checkError))) {
        const message = readiness?.checkError || readiness?.message || ({
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
  const terminalFallback = !companion && structuredCreationBlocked && !terminalCreationBlocked

  const handleCreate = async () => {
    if (creatingChatRef.current || !canCreate || (companion ? structuredCreationBlocked : structuredCreationBlocked && terminalCreationBlocked)) return
    creatingChatRef.current = true
    setCreatingChat(true)
    setChatError('')
    const n = name.trim() || 'New chat'
    const effort = reasoningOptions.length === 0 ? '' : reasoningEffortForModel(cachedAcp, selectedModelId, reasoningEffort, providerId === COPILOT_CLI_PROVIDER_ID)
    try {
      const created = routingMode === 'automatic'
        ? await addSession(n, selectedFolder?.id ?? null, providerId, undefined, undefined, terminalFallback ? 'cli' : 'chat', executionHostId, selectedWorkspace, 'automatic')
        : isRemote
        ? await addSession(n, selectedFolder?.id ?? null, providerId, selectedModelId, effort, terminalFallback ? 'cli' : 'chat', executionHostId, selectedWorkspace)
        : newChatWorktreeContext
        ? await addSession(n, selectedFolder!.id, providerId, selectedModelId, effort, terminalFallback ? 'cli' : 'chat', 'local', '', 'explicit', newChatWorktreeContext)
        : selectedGitBaseBranch !== undefined
        ? await addSession(n, selectedFolder!.id, providerId, selectedModelId, effort, terminalFallback ? 'cli' : 'chat', 'local', '', 'explicit', { gitBaseBranch: selectedGitBaseBranch })
        : await addSession(n, selectedFolder!.id, providerId, selectedModelId, effort, terminalFallback ? 'cli' : 'chat')
      if (created) {
        onCreated?.()
        return // The store closes the modal only after successful creation.
      }
      setChatError('The chat could not be created. Check the workspace and provider, then try again.')
    } catch (error) {
      setChatError(error instanceof Error ? error.message : 'The chat could not be created. Try again.')
    }
    creatingChatRef.current = false
    setCreatingChat(false)
  }

	const providerSessions = sessions.filter((session) => !session.temporaryParentChatId && session.providerId === providerId)
	const workspaceKey = (value: string | undefined) => {
	  const normalized = (value ?? '').trim().replace(/\\/g, '/').replace(/\/+$/, '')
	  return selectedExecutionHost?.platform.toLowerCase() === 'windows'
		? normalized.toLowerCase()
		: normalized
	}
	const selectedWorkspace = newChatWorktreeContext?.expectedWorkspaceDirectory ?? (isRemote ? selectedFolder?.directory ?? remoteWorkspace.trim() : selectedFolder?.directory ?? '')
  const branchUnavailableReason = companion || !isCefContext() ? 'Branch selection is available in the desktop app.'
    : routingMode === 'automatic' ? 'Choose a computer explicitly to select a starting branch.'
    : isRemote ? 'Starting branch selection is available for local workspaces.'
    : !gitEnabled || !gitAutoWorktree ? 'Enable Git and automatic worktrees in Version Control settings to choose a branch.'
    : !selectedFolder?.directory ? 'Choose a workspace to select a starting branch.'
    : newChatWorktreeContext || sourceContextInvalid ? 'The selected worktree keeps its current branch.' : ''
  const branchScope = JSON.stringify([folderId, selectedWorkspace, executionHostId, routingMode, branchUnavailableReason])
  // Fence both rendered choices and Create immediately; effects alone allow one stale render.
  const selectedGitBaseBranch = !branchUnavailableReason && branchChoice?.scope === branchScope ? branchChoice.branch : undefined
  useEffect(() => { setBranchChoice(null) }, [branchScope])
	const discoverySession = providerSessions.find((session) => (session.executionHostId ?? 'local') === executionHostId && workspaceKey(session.workspaceDirectory) === workspaceKey(selectedWorkspace))
	const scopedCatalog = providerModelCatalogs.find((catalog) => catalog.providerId === providerId && catalog.executionHostId === executionHostId && workspaceKey(catalog.workspaceDirectory) === workspaceKey(selectedWorkspace))
	const liveBinding = discoverySession ? acpBindingBySessionId[discoverySession.id] : undefined
	const cachedAcp = modelCatalogForDefaults(scopedCatalog
	  ? { ...(liveBinding ?? scopedCatalog),
	      availableModels: scopedCatalog.availableModels.length ? scopedCatalog.availableModels : liveBinding?.availableModels ?? [],
	      configOptions: liveBinding?.configOptions?.length ? liveBinding.configOptions : scopedCatalog.configOptions,
	      modelsLoading: scopedCatalog.modelsLoading,
	      modelRefreshError: scopedCatalog.modelRefreshError,
	    }
	  : liveBinding)
	const modelOptions = buildModelOptions(cachedAcp, modelId, selectedProvider ?? undefined, providerId, true, providerChatDefaults[providerId]).filter((option) => !option.disabled)
	const selectedModelId = modelOptionFor(modelOptions, modelId).id
  useEffect(() => {
    if (providerId === 'opencode-cli' && selectedModelId !== modelId) setModelId(selectedModelId)
  }, [providerId, selectedModelId, modelId])
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
	void discoverProviderModels('', providerId, selectedWorkspace, executionHostId)
  }
  const canCreate = !sourceContextInvalid && Boolean(selectedExecutionHost) && (isRemote
    ? isAbsoluteRemoteWorkspace(selectedExecutionHost?.platform, selectedWorkspace)
    : selectedFolder !== null && Boolean(selectedWorkspace))

  const remoteWorkspaceIssue = isRemote && !canCreate ? 'Enter an absolute workspace path.' : ''
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

  const chooseProvider = (nextProviderId: string, nextHostId = executionHostId, explicit = true) => {
    if (explicit) providerChoiceExplicit.current = true
    providerChoices.current[`${executionHostId}\n${providerId}`] = { modelId, effort: reasoningEffort }
    const saved = providerChoices.current[`${nextHostId}\n${nextProviderId}`]
    const defaults = defaultsForExecutionHost(defaultState, nextHostId).providerChatDefaults[nextProviderId]
    setProviderId(nextProviderId)
    setModelId(saved?.modelId ?? defaults?.modelId ?? '')
    setReasoningEffort(saved?.effort ?? defaults?.reasoningEffort ?? '')
  }
  const chooseHost = (hostId: string) => {
    if (newChatWorktreeContext && hostId !== executionHostId) setNewChatModalOpen(true, null, null)
    setSourceContextInvalid(false); setChatError('')
    workspaceChoices.current[executionHostId] = { folderId, path: remoteWorkspace }
    const saved = workspaceChoices.current[hostId]
    const compatible = folders.find((folder) => (folder.executionHostId || 'local') === hostId)
    const preferred = defaultsForExecutionHost(defaultState, hostId).defaultNewChatProviderId
    const nextProvider = !providerChoiceExplicit.current && providers.some(provider => provider.id === preferred) ? preferred : providerId
    chooseProvider(nextProvider, hostId, false)
    setExecutionHostId(hostId)
    setFolderId(saved ? saved.folderId : compatible?.id ?? null)
    setRemoteWorkspace(saved ? saved.path : hostId !== 'local' ? compatible?.directory ?? '' : '')
    setWorkspaceError('')
  }

  return (
    <div
      className="uam-overlay fixed inset-0 z-50 flex items-center justify-center"
      data-state={overlayState}
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
        className="flex max-h-[calc(100vh-32px)] w-full max-w-4xl flex-col overflow-hidden mx-4"
        style={{
          background: 'var(--surface)',
          borderRadius: 'var(--r-lg)',
          border: '1px solid var(--border-bright)',
          boxShadow: 'var(--elev-3)',
        }}
      >
        {/* Header */}
        <div
          className="flex shrink-0 items-center justify-between px-5 py-3"
          style={{ borderBottom: '1px solid var(--border)' }}
        >
          <span className="text-sm font-semibold" style={{ color: 'var(--text)' }}>
            New chat
          </span>
          <IconButton icon={<X size={16} />} label="Close new chat" disabled={creatingChat} onClick={requestClose} />
        </div>

        <div className="min-h-0 flex-1 overflow-y-auto p-4 space-y-3 md:grid md:grid-cols-2 md:items-start md:gap-x-5 md:space-y-0">
          <div className="min-w-0 space-y-3">
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
            />
          </div>

          <section aria-label="Workspace" className="space-y-3">
          <h3 className="-mb-2 font-medium uppercase" style={{ color: 'var(--text-3)', fontSize: 'var(--fs-xs)', letterSpacing: '0.08em' }}>Workspace</h3>
          <div>
          <div className="mb-1.5 text-xs font-medium" style={{ color: 'var(--text-2)' }}>Runs on</div>
          <SelectionGrid
            label="Runs on"
            value={routingMode === 'automatic' ? AUTOMATIC_HOST_OPTION : executionHostId}
            options={[...(SSH_ENABLED ? [{ id: AUTOMATIC_HOST_OPTION, label: 'Automatic', icon: <Monitor size={18} aria-hidden />, disabled: creatingChat || !automaticAvailable, issues: automaticAvailable ? [] : ['Configure routing and map this workspace for the selected provider.'] }] : []), ...executionHosts.filter((host) => SSH_ENABLED || host.transport !== 'ssh').map((host) => ({
              id: host.id,
              label: host.label,
              icon: <Monitor size={18} aria-hidden />,
              detail: host.transport === 'ssh' ? host.sshAlias : undefined,
              tone: host.transport === 'ssh' ? RUNNER_STATUS[host.runnerStatus].tone : undefined,
              disabled: creatingChat || (host.transport === 'ssh' && host.runnerStatus !== 'ready'),
              issues: host.transport === 'ssh' && host.runnerStatus !== 'ready'
                ? [`Remote runner is ${host.runnerStatus}. Open Settings to configure this computer.`] : [],
            }))]}
            onChange={(hostId) => { if (hostId === AUTOMATIC_HOST_OPTION) { if (newChatWorktreeContext) setNewChatModalOpen(true, folderId, null); setSourceContextInvalid(false); setChatError(''); setRoutingMode('automatic') } else { setRoutingMode('explicit'); chooseHost(hostId) } }}
          />
          </div>
          {hostIssue && <p role="alert" className="text-xs" style={{ color: 'var(--error)' }}>{hostIssue}</p>}

          {/* Folder */}
          {folders.some((folder) => (folder.executionHostId || 'local') === executionHostId) && (
            <div>
              <div className="text-xs font-medium mb-1.5 block" style={{ color: 'var(--text-2)' }}>
                Folder
              </div>
              <WorkspacePicker
                label="Folder"
                value={folderId}
                folders={folders}
                collections={resourceCollections}
                executionHostId={executionHostId}
                remoteEmptyOption={isRemote ? { label: 'New remote workspace…', description: `Create on ${selectedExecutionHost?.label}` } : undefined}
                onChange={(nextFolderId) => {
                  if (newChatWorktreeContext && nextFolderId !== folderId) setNewChatModalOpen(true, nextFolderId, null)
                  setSourceContextInvalid(false); setChatError('')
				  setFolderId(nextFolderId)
				  const folder = folders.find((candidate) => candidate.id === nextFolderId)
				  if (folder && isRemote) setRemoteWorkspace(folder.directory)
				}}
              />
            </div>
          )}

          {newChatWorktreeContext && <p role="status" className="text-xs" style={{ color: 'var(--text-2)' }}>Using the selected worktree: {sessions.find((source) => source.id === newChatWorktreeContext.sourceChatId)?.workspaceBranchName || 'existing worktree'}.</p>}

          {!newChatWorktreeContext && <NewChatBranchPicker
            key={branchScope}
            folderId={selectedFolder?.id ?? ''}
            defaultBranch={gitDefaultBaseBranch}
            value={selectedGitBaseBranch}
            unavailableReason={branchUnavailableReason}
            disabled={creatingChat}
            onChange={(branch) => { setBranchChoice({ scope: branchScope, branch }); setChatError('') }}
          />}

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
                  aria-describedby={workspaceError ? 'new-chat-workspace-error' : undefined}
                  type="text"
                  value={remoteWorkspace}
                  onChange={(event) => setRemoteWorkspace(event.target.value)}
                  readOnly={selectedFolder !== null}
                  placeholder="/absolute/path/on/selected/host"
                  className="w-full rounded-md pl-3 pr-10 py-2 text-sm outline-none"
                  style={{ background: 'var(--surface-up)', border: '1px solid var(--border)', color: 'var(--text)', fontFamily: 'var(--font-mono)' }}
                />
                <span className="absolute right-3 top-1/2 -translate-y-1/2"><StatusIndicator issues={remoteWorkspaceIssue ? [remoteWorkspaceIssue] : []} okLabel="Absolute workspace path" /></span>
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
				{selectedFolder ? 'Owned by the selected workspace.' : `Read only by ${selectedExecutionHost?.label}.`} Computer Use is off for remote chats.
              </p>
            </div>
          )}

          {!companion && !selectedFolder && !isRemote && <div className="flex items-center gap-2">
            <Button className="flex-1" variant="secondary" size="sm" leadingIcon={<FolderPlus size={14} />} disabled={creatingWorkspace} onClick={() => void createWorkspace()}>
              {creatingWorkspace ? 'Choosing…' : 'Choose workspace'}
            </Button>
          </div>}
          {workspaceError && <p id="new-chat-workspace-error" role="alert" className="text-xs" style={{ color: 'var(--error)' }}>{workspaceError}</p>}
          {SSH_ENABLED && (
            <details className="-mt-1">
              <summary className="cursor-pointer select-none text-xs" style={{ color: 'var(--text-3)' }}>More options</summary>
              <div className="mt-2 flex justify-start">
                <Button variant="ghost" size="sm" leadingIcon={<Route size={13} />} disabled={creatingChat} onClick={() => setRoutingOpen(true)}>Configure SSH routing</Button>
              </div>
            </details>
          )}
          </section>
          </div>
          <section aria-label="Provider and model" className="flex min-w-0 flex-col gap-3">
          <div className="-mb-2 flex items-center justify-between gap-2">
            <h3 className="font-medium uppercase" style={{ color: 'var(--text-3)', fontSize: 'var(--fs-xs)', letterSpacing: '0.08em' }}>Provider and model</h3>
            <IconButton
              label={cachedAcp?.modelsLoading ? isRemote ? 'Discovering remote models…' : 'Discovering models…' : isRemote ? 'Discover remote models' : 'Refresh models'}
              icon={<RefreshCw size={14} style={cachedAcp?.modelsLoading ? { animation: 'uam-status-spin 0.9s linear infinite' } : undefined} />}
              disabled={routingMode === 'automatic' || !canCreate || structuredCreationBlocked || Boolean(cachedAcp?.modelsLoading)}
              onClick={requestModelDiscovery}
            />
          </div>
          <div
            className="overflow-hidden"
            style={{
              border: '1px solid var(--border)',
              borderRadius: 'var(--r-md)',
              opacity: routingMode === 'automatic' ? 0.5 : 1,
              pointerEvents: routingMode === 'automatic' ? 'none' : undefined,
            }}
          >
            <ModelPickerContent
              providers={providers} providerId={providerId} currentModelId={selectedModelId}
              activeOptions={modelOptions} activeLoading={cachedAcp?.modelsLoading} activeError={cachedAcp?.modelRefreshError}
              modelDisabled={routingMode === 'automatic'} canChangeProvider={routingMode !== 'automatic'}
              workspaceDirectory={selectedWorkspace} executionHostId={executionHostId} listHeight={400}
              onClose={requestClose}
              onSelect={(nextProviderId, nextModelId) => {
                if (nextProviderId !== providerId) {
                  chooseProvider(nextProviderId)
                  setModelId(nextModelId)
                  return
                }
                setModelId(nextModelId)
                setReasoningEffort(reasoningEffortForModel(cachedAcp, nextModelId, reasoningEffort, providerId === COPILOT_CLI_PROVIDER_ID))
              }}
            />
          </div>
          {availability.issues.length > 0 && <p role={structuredCreationBlocked && terminalCreationBlocked ? 'alert' : undefined} className="text-xs" style={{ color: structuredCreationBlocked && terminalCreationBlocked ? 'var(--error)' : 'var(--text-2)' }}>{availability.issues[availability.issues.length - 1]}</p>}
          {terminalFallback && <p role="status" className="flex items-center gap-2 text-xs" style={{ color: 'var(--text)' }}><SquareTerminal size={14} aria-hidden />Creates a terminal chat. Structured chat is unavailable.</p>}

          <div role="group" aria-label="Model and reasoning" className="space-y-3">
            <StatusIndicator issues={cachedAcp?.modelRefreshError ? [cachedAcp.modelRefreshError] : cachedAcp?.modelsLoading ? ['Discovering models.'] : []} okLabel="Model options available" />
            {cachedAcp?.modelRefreshError && (
              <Notice
                key={`${providerId}\n${executionHostId}\n${workspaceKey(selectedWorkspace)}\n${cachedAcp.modelRefreshError}`}
                tone="error"
                title="Model refresh failed"
                dismissLabel="Dismiss model refresh error"
                actions={<Button variant="secondary" size="sm" onClick={() => {
                  requestModelDiscovery()
                }}>Retry</Button>}
              >
                {cachedAcp.modelRefreshError}
              </Notice>
            )}
            {reasoningOptions.length > 0 && (
              <div>
                <div className="mb-1.5 text-xs font-medium" style={{ color: 'var(--text-2)' }}>Reasoning effort</div>
                <MenuSelect
                  label="Reasoning effort"
                  disabled={routingMode === 'automatic'}
                  value={routingMode === 'automatic' ? '' : reasoningEffortForModel(cachedAcp, selectedModelId, reasoningEffort, providerId === COPILOT_CLI_PROVIDER_ID)}
                  options={routingMode === 'automatic' ? [{ value: '', label: 'Provider default' }] : reasoningOptions.map((option) => ({ value: option.id, label: option.label, description: option.detail }))}
                  onChange={setReasoningEffort}
                />
              </div>
            )}
          </div>
          </section>
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
        {chatError && <div role="alert" className="px-5 pt-3 text-xs" style={{ color: 'var(--error)' }}>{chatError}</div>}
        <div
          className="flex shrink-0 items-center justify-end gap-2 px-5 py-3"
          style={{ borderTop: '1px solid var(--border)' }}
        >
          <Button variant="ghost" size="md" disabled={creatingChat} onClick={requestClose}>Cancel</Button>
          <Button
            variant="primary"
            size="md"
            onClick={() => void handleCreate()}
            disabled={creatingChat || !canCreate || (companion ? structuredCreationBlocked : structuredCreationBlocked && terminalCreationBlocked)}
            aria-busy={creatingChat || undefined}
          >
            {creatingChat ? 'Creating…' : 'Create chat'}
          </Button>
        </div>
      {routingOpen && <ExecutionRoutingDialog onClose={() => setRoutingOpen(false)} />}
      </div>
    </div>
  )
}
