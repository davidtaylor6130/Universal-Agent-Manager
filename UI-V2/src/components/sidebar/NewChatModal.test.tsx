import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { useAppStore } from '../../store/useAppStore'
import { NewChatModal } from './NewChatModal'
import * as modelOptions from '../chat/modelOptions'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

const originalDiscoverProviderModels = useAppStore.getState().discoverProviderModels
const originalListRemoteDirectories = useAppStore.getState().listRemoteDirectories
const originalSetNewChatModalOpen = useAppStore.getState().setNewChatModalOpen

describe('NewChatModal', () => {
  beforeEach(() => {
	delete window.cefQuery
    useAppStore.setState({
      folders: [{ id: 'project', name: 'Project', parentId: null, directory: '/tmp/project', isExpanded: true, createdAt: new Date() }],
      providers: [
        { id: 'gemini-cli', name: 'Gemini CLI', shortName: 'Gemini', color: '#8ab4ff', description: '' },
        { id: 'codex-cli', name: 'Codex CLI', shortName: 'Codex', color: '#22c55e', description: '' },
      ],
      defaultNewChatProviderId: 'gemini-cli',
      providerChatDefaults: {},
      hostScopedSettingsValues: {},
      newChatFolderId: 'project',
      resourceCollections: [],
	  sessions: [],
	  acpBindingBySessionId: {},
      providerModelCatalogs: [],
      cliVersionManager: { providers: [] },
      discoverProviderModels: originalDiscoverProviderModels,
      listRemoteDirectories: originalListRemoteDirectories,
      executionRouting: { enabled: false, hosts: [] },
      executionHosts: [
        { id: 'local', label: 'This computer', transport: 'local', sshAlias: '', runnerStatus: 'ready', runnerVersion: '', platform: 'macos', architecture: 'arm64', lastSeenAt: '' },
      ],
    })
  })

  it('does not discover models on opening or reopening the new chat picker', async () => {
    const discoverProviderModels = vi.fn().mockResolvedValue(true)
    useAppStore.setState({ discoverProviderModels })
    const host = document.createElement('div')
    document.body.appendChild(host)
    let root = createRoot(host)
    try {
      await act(async () => { root.render(<NewChatModal />) })
      act(() => root.unmount())
      root = createRoot(host)
      await act(async () => { root.render(<NewChatModal />) })
      expect(discoverProviderModels).not.toHaveBeenCalled()
    } finally {
      act(() => root.unmount())
      host.remove()
    }
  })

  it('keeps cached models with live selection and config, tolerates legacy bindings, and refreshes independently', async () => {
    const optionsSpy = vi.spyOn(modelOptions, 'buildModelOptions')
    const discoverProviderModels = vi.fn().mockResolvedValue(true)
    useAppStore.setState({ discoverProviderModels })
    useAppStore.setState({
      sessions: [{ id: 'active', name: 'Active', providerId: 'gemini-cli', folderId: 'project', workspaceDirectory: '/tmp/project', viewMode: 'chat', createdAt: new Date(), updatedAt: new Date() }],
      acpBindingBySessionId: { active: {
        sessionId: 'active-native', threadId: '', providerId: 'gemini-cli', protocolKind: 'gemini-acp',
        running: true, lifecycleState: 'ready', processing: true, processingStartedAtMs: null,
        readySinceLastSelect: false, lastError: '', recentStderr: '', lastExitCode: null,
        diagnostics: [], toolCalls: [], planEntries: [], availableModes: [], currentModeId: 'default',
        availableModels: [], currentModelId: 'cached-model', configOptions: [{ id: 'model', name: 'Model', description: '', category: 'model', currentValue: 'live-config', options: [{ value: 'live-config', name: 'Live Config', description: '' }] }], turnEvents: [], turnUserMessageIndex: -1,
        turnAssistantMessageIndex: -1, turnSerial: 0, pendingPermission: null, pendingUserInput: null,
        agentInfo: null, modelsLoading: false, modelRefreshError: '',
      } },
      providerModelCatalogs: [{ providerId: 'gemini-cli', workspaceDirectory: '/tmp/project', executionHostId: 'local',
        availableModels: [{ id: 'cached-model', name: 'Cached Model', description: '' }], currentModelId: '', modelsLoading: true, modelRefreshError: '' }],
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    try {
      await act(async () => { root.render(<NewChatModal />) })
      expect(host.querySelector('[data-testid="model-options"]')?.textContent).toContain('Cached Model')
      expect(host.textContent).toContain('Discovering models')
      expect(optionsSpy.mock.calls.filter(([, , , id]) => id === 'gemini-cli').at(-1)?.[0]).toMatchObject({ currentModelId: 'cached-model', configOptions: [{ currentValue: 'live-config' }] })
      act(() => {
        const binding = { ...useAppStore.getState().acpBindingBySessionId.active }
        Reflect.deleteProperty(binding, 'availableModels')
        useAppStore.setState((state) => ({ acpBindingBySessionId: { active: binding }, providerModelCatalogs: state.providerModelCatalogs.map((catalog) => ({ ...catalog, modelsLoading: false })) }))
      })
      expect(optionsSpy.mock.calls.filter(([, , , id]) => id === 'gemini-cli').at(-1)?.[0]?.availableModels?.[0].id).toBe('cached-model')
      await act(async () => { Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.getAttribute('aria-label') === 'Refresh models')?.click() })
      expect(discoverProviderModels).toHaveBeenCalledWith('', 'gemini-cli', '/tmp/project', 'local')
    } finally {
      optionsSpy.mockRestore()
      act(() => root.unmount())
      host.remove()
    }
  })

  it('owns independent refresh state and fresh models in the scoped catalog while a chat stays running', async () => {
    const discoverProviderModels = vi.fn().mockResolvedValue(true)
    const live = {
      sessionId: 'native', threadId: '', providerId: 'gemini-cli', protocolKind: 'gemini-acp',
      running: true, lifecycleState: 'ready' as const, processing: true, processingStartedAtMs: null,
      readySinceLastSelect: false, lastError: '', recentStderr: '', lastExitCode: null,
      diagnostics: [], toolCalls: [], planEntries: [], availableModes: [], currentModeId: 'default',
      availableModels: [{ id: 'old-model', name: 'Old Model', description: '' }], currentModelId: 'old-model',
      turnEvents: [], turnUserMessageIndex: -1, turnAssistantMessageIndex: -1, turnSerial: 0,
      pendingPermission: null, pendingUserInput: null, agentInfo: null, modelsLoading: false, modelRefreshError: '',
    }
    useAppStore.setState({
      discoverProviderModels,
      sessions: [{ id: 'active', name: 'Active', providerId: 'gemini-cli', folderId: 'project', workspaceDirectory: '/tmp/project', viewMode: 'chat', createdAt: new Date(), updatedAt: new Date() }],
      acpBindingBySessionId: { active: live },
      providerModelCatalogs: [{ providerId: 'gemini-cli', workspaceDirectory: '/tmp/project', executionHostId: 'local', currentModelId: '', availableModels: [], modelsLoading: true, modelRefreshError: '' }],
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    try {
      await act(async () => { root.render(<NewChatModal />) })
      expect(Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.getAttribute('aria-label') === 'Discovering models…')?.disabled).toBe(true)
      act(() => useAppStore.setState((state) => ({ providerModelCatalogs: state.providerModelCatalogs.map((catalog) => ({ ...catalog, modelsLoading: false, availableModels: [{ id: 'fresh-model', name: 'Fresh Model', description: '' }] })) })))
      expect(host.querySelector('[data-testid="model-options"]')?.textContent).toContain('Fresh Model')
      act(() => useAppStore.setState((state) => ({ providerModelCatalogs: state.providerModelCatalogs.map((catalog) => ({ ...catalog, modelRefreshError: 'Independent discovery failed.' })) })))
      expect(host.querySelector('[role="alert"]')?.textContent).toContain('Independent discovery failed.')
      await act(async () => { Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Retry')?.click() })
      expect(discoverProviderModels).toHaveBeenCalledWith('', 'gemini-cli', '/tmp/project', 'local')
      expect(useAppStore.getState().acpBindingBySessionId.active).toEqual(live)
    } finally {
      act(() => root.unmount())
      host.remove()
    }
  })

  it('creates terminal-only Antigravity without requesting structured model discovery', async () => {
    const addSession = vi.fn().mockResolvedValue(true)
    const discoverProviderModels = vi.fn().mockResolvedValue(true)
    const originalAddSession = useAppStore.getState().addSession
    useAppStore.setState({
      providers: [{ id: 'antigravity-cli', name: 'Antigravity CLI', shortName: 'Antigravity', color: '', description: '', supportsCli: true, supportsStructured: false, structuredProtocol: 'none' }],
      defaultNewChatProviderId: 'antigravity-cli',
      addSession,
      discoverProviderModels,
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    try {
      await act(async () => { root.render(<NewChatModal />) })
      expect(host.textContent).toContain('Creates a terminal chat.')
      expect(host.textContent).not.toContain('Settings > CLI Version')
      expect(discoverProviderModels).not.toHaveBeenCalled()
      const create = Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Create chat')
      expect(create?.disabled).toBe(false)
      await act(async () => { create?.click(); await Promise.resolve() })
      expect(addSession).toHaveBeenCalledWith('New chat', 'project', 'antigravity-cli', '', '', 'cli')
    } finally {
      act(() => root.unmount())
      host.remove()
      useAppStore.setState({ addSession: originalAddSession, discoverProviderModels: originalDiscoverProviderModels })
    }
  })

  it('keeps an actual compatibility probe failure visible for a newer unverified version', () => {
    useAppStore.setState({ cliVersionManager: { providers: [{ providerId: 'gemini-cli', installedVersion: '1.0.0', selectedVersion: '1.0.0', availableVersions: [], preferredVersion: '1.0.0', status: 'untested-newer', message: 'Newer than the verified range.', checkError: 'The executable failed its compatibility probe.', running: false, lastCommand: '', lastOutput: '' }] } })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    try {
      act(() => root.render(<NewChatModal />))
      expect(host.textContent).toContain('The executable failed its compatibility probe.')
      expect(host.textContent).not.toContain('Newer than the verified range.')
    } finally {
      act(() => root.unmount())
      host.remove()
    }
  })

  it.each(['unknown', 'verified', 'untested', 'untested-newer', 'provider-managed'] as const)(
    'allows structured creation when provider readiness is %s',
    (status) => {
      useAppStore.setState({ cliVersionManager: { providers: [{ providerId: 'gemini-cli', installedVersion: '1.0.0', selectedVersion: '1.0.0', availableVersions: [], preferredVersion: '1.0.0', status, message: '', running: false, lastCommand: '', lastOutput: '' }] } })
      const host = document.createElement('div')
      document.body.appendChild(host)
      const root = createRoot(host)
      act(() => root.render(<NewChatModal />))
      expect(Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Create chat')?.disabled).toBe(false)
      expect(host.textContent).not.toContain('Check Settings > CLI Version.')
      act(() => root.unmount())
      host.remove()
    },
  )

  it.each(['checking', 'installing', 'known-incompatible'] as const)(
    'identifies terminal fallback for provider readiness %s',
    async (status) => {
      const addSession = vi.fn().mockResolvedValue(false)
      useAppStore.setState({ addSession })
      useAppStore.setState({ cliVersionManager: { providers: [{ providerId: 'gemini-cli', installedVersion: '0.1.0', selectedVersion: '1.0.0', availableVersions: [{ version: '1.0.0', preferred: true }], preferredVersion: '1.0.0', status, message: 'Provider needs attention.', running: status === 'checking' || status === 'installing', lastCommand: '', lastOutput: '' }] } })
      const host = document.createElement('div')
      document.body.appendChild(host)
      const root = createRoot(host)
      act(() => root.render(<NewChatModal />))
      expect(host.textContent).toContain('Creates a terminal chat.')
      await act(async () => { Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Create chat')!.click() })
      expect(addSession).toHaveBeenCalledWith('New chat', 'project', 'gemini-cli', '', '', 'cli')
      expect(Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Create chat')?.disabled).toBe(false)
      act(() => root.unmount())
      host.remove()
    },
  )

  it('does not launch model discovery for a provider blocked from structured creation', async () => {
    const discoverProviderModels = vi.fn().mockResolvedValue(false)
    useAppStore.setState({
      discoverProviderModels,
      cliVersionManager: { providers: [{ providerId: 'gemini-cli', installedVersion: '0.1.0', selectedVersion: '1.0.0', availableVersions: [], preferredVersion: '1.0.0', status: 'known-incompatible', message: 'Unsupported version.', running: false, lastCommand: '', lastOutput: '' }] },
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    await act(async () => { root.render(<NewChatModal />); await Promise.resolve() })

    expect(discoverProviderModels).not.toHaveBeenCalled()

    act(() => root.unmount())
    host.remove()
  })

  it('directs provider repairs to Settings without running repair actions', async () => {
    const refreshCliProviderVersion = vi.fn().mockResolvedValue(true)
    const applyCliProviderVersion = vi.fn().mockResolvedValue(true)
    useAppStore.setState({
      refreshCliProviderVersion,
      applyCliProviderVersion,
      cliVersionManager: { providers: [{ providerId: 'gemini-cli', installedVersion: '0.1.0', selectedVersion: '1.0.0', availableVersions: [{ version: '1.0.0', preferred: true }], preferredVersion: '1.0.0', status: 'known-incompatible', message: 'Unsupported version.', running: false, lastCommand: '', lastOutput: '' }] },
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<NewChatModal />))
    expect(host.textContent).toContain('Unsupported version.')
    expect(host.textContent).toContain('Settings > CLI Version')
    expect(Array.from(host.querySelectorAll('button')).some((button) => /Install supported version|Check again/.test(button.textContent ?? ''))).toBe(false)
    expect(applyCliProviderVersion).not.toHaveBeenCalled()
    expect(refreshCliProviderVersion).not.toHaveBeenCalled()
    act(() => root.unmount())
    host.remove()
  })

  it('blocks terminal creation when the provider executable is unavailable', async () => {
    const addSession = vi.fn().mockResolvedValue(true)
    useAppStore.setState({
      addSession,
      cliVersionManager: { providers: [{ providerId: 'gemini-cli', installedVersion: '', selectedVersion: '', availableVersions: [], preferredVersion: '', status: 'unavailable', message: 'CLI unavailable.', running: false, lastCommand: '', lastOutput: '' }] },
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<NewChatModal />))

    const structured = Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Create chat')
    const terminal = Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Create chat')
    act(() => structured?.click())
    expect(addSession).not.toHaveBeenCalled()
    expect(terminal?.disabled).toBe(true)
    await act(async () => { terminal?.click(); await Promise.resolve() })
    expect(addSession).not.toHaveBeenCalled()

    act(() => root.unmount())
    host.remove()
  })

  it('creates a folder chat with the provider and model selected by the user', () => {
    const addSession = vi.fn()
    const setNewChatModalOpen = vi.fn()
    useAppStore.setState({ addSession, setNewChatModalOpen })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<NewChatModal />))

    act(() => host.querySelector<HTMLButtonElement>('button[aria-label="Codex"]')?.click())
    expect(setNewChatModalOpen).not.toHaveBeenCalled()

    const gpt = Array.from(host.querySelectorAll<HTMLButtonElement>('[role="menuitemradio"]')).find((option) =>
      option.textContent?.includes('GPT-5.4')
    )
    act(() => gpt?.click())

    const reasoning = host.querySelector<HTMLButtonElement>('button[aria-label="Reasoning effort"]')!
    act(() => reasoning.click())
    const high = Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="option"]')).find((option) =>
      option.textContent?.startsWith('High')
    )
    act(() => high?.click())

    const create = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Create chat')
    act(() => create?.click())

	expect(addSession).toHaveBeenCalledWith('New chat', 'project', 'codex-cli', 'gpt-5.4', 'high', 'chat')

    act(() => root.unmount())
    host.remove()
  })

  it('shows one inline provider and model picker at a fixed height with no popup trigger', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<NewChatModal />))
    expect(host.querySelector('button[aria-label="Model"]')).toBeNull()
    expect(host.querySelector('button[aria-label="Codex"]')).not.toBeNull()
    expect(host.querySelector<HTMLElement>('[data-testid="model-options"]')?.style.maxHeight).toBe('400px')
    expect(host.querySelector('[role="menuitemradio"]')).not.toBeNull()
    act(() => root.unmount())
    host.remove()
  })

  it('filters the new chat model picker with the composer search', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<NewChatModal />))
    const search = document.body.querySelector<HTMLInputElement>('input[aria-label="Search models"]')!
    act(() => {
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(search, 'no-such-model-xyz')
      search.dispatchEvent(new Event('input', { bubbles: true }))
    })
    expect(document.body.querySelector('[role="status"]')?.textContent).toContain('No matching models')
    act(() => root.unmount())
    host.remove()
  })

  it('defaults to the available workspace when opened globally', () => {
    const addSession = vi.fn()
    useAppStore.setState({ addSession, newChatFolderId: null })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<NewChatModal />))

    const create = Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Create chat')
    expect(create?.disabled).toBe(false)
    act(() => create?.click())
    expect(addSession).toHaveBeenCalledWith('New chat', 'project', 'gemini-cli', '', '', 'chat')

    act(() => root.unmount())
    host.remove()
  })

  it('searches sidebar groups and creates in the selected durable workspace', async () => {
    const addSession = vi.fn().mockResolvedValue(true)
    useAppStore.setState({ addSession, folders: [
      ...useAppStore.getState().folders,
      { id: 'backend', name: 'Project', parentId: null, directory: '/tmp/backend', isExpanded: false, createdAt: new Date() },
    ], resourceCollections: [{ id: 'services', name: 'Services', collapsed: true, references: [
      { id: 'ref', type: 'workspace-folder', target: 'backend', label: 'Backend' },
    ] }] })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    try {
      act(() => root.render(<NewChatModal />))
      act(() => host.querySelector<HTMLButtonElement>('[role="combobox"][aria-label="Folder"]')!.click())
      const search = document.querySelector<HTMLInputElement>('[aria-label="Search workspaces"]')!
      act(() => {
        Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(search, 'services backend')
        search.dispatchEvent(new Event('input', { bubbles: true }))
      })
      expect(document.querySelectorAll('[role="option"]')).toHaveLength(1)
      act(() => search.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true, cancelable: true })))
      const create = Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Create chat')!
      await act(async () => { create.click(); await Promise.resolve() })
      expect(addSession).toHaveBeenCalledWith('New chat', 'backend', 'gemini-cli', '', '', 'chat')
      expect(useAppStore.getState().resourceCollections[0].collapsed).toBe(true)
    } finally { act(() => root.unmount()); host.remove() }
  })

  it('keeps case-sensitive local workspace model catalogues isolated', () => {
    useAppStore.setState({ folders: [{ id: 'project', name: 'Project', parentId: null, directory: '/tmp/Project', isExpanded: true, createdAt: new Date() }],
      providerModelCatalogs: ['/tmp/project', '/tmp/Project'].map((workspaceDirectory, index) => ({
        providerId: 'gemini-cli', executionHostId: 'local', workspaceDirectory, currentModelId: '', modelsLoading: false, modelRefreshError: '',
        availableModels: [{ id: `scoped-${index}`, name: index ? 'Correct workspace' : 'Different workspace', description: '' }],
      })),
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    try {
      act(() => root.render(<NewChatModal />))
      expect(document.body.textContent).toContain('Correct workspace')
      expect(document.body.textContent).not.toContain('Different workspace')
    } finally { act(() => root.unmount()); host.remove() }
  })

  it('uses explicit ephemeral discovery on the selected remote host even when a matching chat exists', async () => {
    const addSession = vi.fn().mockResolvedValue(true)
    const discoverProviderModels = vi.fn().mockResolvedValue(true)
    useAppStore.setState({
      addSession,
      discoverProviderModels,
	  providers: [{ id: 'opencode-cli', name: 'OpenCode CLI', shortName: 'OpenCode', color: '#22c55e', description: '' }],
	  defaultNewChatProviderId: 'opencode-cli',
	  providerChatDefaults: { 'opencode-cli': { modelId: 'controller-default', approvalMode: 'default', commandSafetyTier: 'off', memoryEnabled: true, reasoningEffort: 'high', serviceTier: '' } },
      hostScopedSettingsValues: { lab: { defaultNewChatProviderId: 'opencode-cli', providerChatDefaults: { 'opencode-cli': { modelId: '', approvalMode: 'default', commandSafetyTier: 'off', memoryEnabled: true, reasoningEffort: '', serviceTier: '' } }, mcpServers: [] } },
	  sessions: [{
		id: 'existing-remote-chat',
		name: 'Existing remote chat',
		viewMode: 'chat',
		folderId: null,
		providerId: 'opencode-cli',
		executionHostId: 'lab',
		workspaceDirectory: '/srv/project',
		createdAt: new Date(),
		updatedAt: new Date(),
	  }],
	  folders: [],
	  newChatFolderId: null,
      executionHosts: [
        { id: 'local', label: 'This computer', transport: 'local', sshAlias: '', runnerStatus: 'ready', runnerVersion: '', platform: '', architecture: '', lastSeenAt: '' },
        { id: 'lab', label: 'Home lab', transport: 'ssh', sshAlias: 'home-lab', runnerStatus: 'ready', runnerVersion: '0.1.0', platform: 'linux', architecture: 'arm64', lastSeenAt: '' },
      ],
	  providerModelCatalogs: [
		{
		  providerId: 'opencode-cli',
		  workspaceDirectory: '/srv/Project',
		  executionHostId: 'lab',
		  availableModels: [{ id: 'wrong-case', name: 'Wrong Case', description: 'Different Linux workspace' }],
		  currentModelId: '',
		  modelsLoading: false,
		  modelRefreshError: '',
		},
		{
		  providerId: 'opencode-cli',
		  workspaceDirectory: '/srv/project',
		  executionHostId: 'local',
		  availableModels: [{ id: 'local-only', name: 'Local Only', description: 'Controller model' }],
		  currentModelId: '',
		  modelsLoading: false,
		  modelRefreshError: '',
		},
		{
		  providerId: 'opencode-cli',
		  workspaceDirectory: '/srv/project',
		  executionHostId: 'lab',
		  availableModels: [],
		  configOptions: [{ id: 'model', name: 'Model', description: '', category: 'model', currentValue: '', options: [{ value: 'remote-only', name: 'Remote Only', description: 'Target model' }] }],
		  currentModelId: '',
		  modelsLoading: false,
		  modelRefreshError: '',
		},
	  ],
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    try {
      await act(async () => { root.render(<NewChatModal />); await Promise.resolve() })

      const remoteHost = host.querySelector<HTMLButtonElement>('button[aria-label="Home lab"]')
      act(() => remoteHost?.click())
      discoverProviderModels.mockClear()
      const remoteWorkspace = host.querySelector<HTMLInputElement>('input[placeholder="/absolute/path/on/selected/host"]')!
      act(() => {
        Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')?.set?.call(remoteWorkspace, '/srv/project')
        remoteWorkspace.dispatchEvent(new Event('input', { bubbles: true }))
      })
      expect(discoverProviderModels).not.toHaveBeenCalled()
      await act(async () => {
        Array.from(host.querySelectorAll<HTMLButtonElement>('button'))
        .find((button) => button.getAttribute('aria-label') === 'Discover remote models')?.click()
        await Promise.resolve()
      })
      const modelMenu = host.querySelector('[data-testid="model-options"]')
      expect(modelMenu?.textContent).toContain('Remote Only')
      expect(modelMenu?.textContent).not.toContain('Local Only')
      expect(modelMenu?.textContent).not.toContain('Wrong Case')
      expect(modelMenu?.textContent).not.toContain('Controller Default')
      act(() => Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="menuitemradio"]'))
        .find((option) => option.textContent?.includes('Remote Only'))?.click())
      const create = Array.from(host.querySelectorAll<HTMLButtonElement>('button'))
        .find((button) => button.textContent === 'Create chat')
      await act(async () => { create?.click(); await Promise.resolve() })

      expect(discoverProviderModels).toHaveBeenCalledWith('', 'opencode-cli', '/srv/project', 'lab')
      expect(host.textContent).toContain('Computer Use is off for remote chats.')
      expect(host.textContent).not.toContain('A workspace is required')
      expect(addSession).toHaveBeenCalledWith('New chat', null, 'opencode-cli', 'remote-only', '', 'chat', 'lab', '/srv/project')
    } finally {
      act(() => root.unmount())
      host.remove()
    }
  })

	it('accepts Windows drive and UNC workspace paths for a ready remote host', async () => {
	  const addSession = vi.fn().mockResolvedValue(true)
	  useAppStore.setState({
		addSession,
		folders: [],
		newChatFolderId: null,
		executionHosts: [
		  { id: 'local', label: 'This computer', transport: 'local', sshAlias: '', runnerStatus: 'ready', runnerVersion: '', platform: '', architecture: '', lastSeenAt: '' },
		  { id: 'windows', label: 'Windows PC', transport: 'ssh', sshAlias: 'windows-pc', runnerStatus: 'ready', runnerVersion: '4.5.7', platform: 'windows', architecture: 'x86_64', lastSeenAt: '' },
		],
	  })
	  const host = document.createElement('div')
	  document.body.appendChild(host)
	  const root = createRoot(host)
	  await act(async () => { root.render(<NewChatModal />); await Promise.resolve() })

	  act(() => host.querySelector<HTMLButtonElement>('button[aria-label="Windows PC"]')?.click())
	  const input = host.querySelector<HTMLInputElement>('input[placeholder="/absolute/path/on/selected/host"]')!
	  const create = () => Array.from(host.querySelectorAll<HTMLButtonElement>('button'))
		.find((button) => button.textContent === 'Create chat')!
	  for (const path of ['C:\\work\\project', '\\\\server\\share']) {
		act(() => {
		  Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')?.set?.call(input, path)
		  input.dispatchEvent(new Event('input', { bubbles: true }))
		})
		expect(create().disabled).toBe(false)
	  }

	  act(() => root.unmount())
	  host.remove()
	})

  it('browses a remote workspace only after the user requests it', async () => {
    const listRemoteDirectories = vi.fn(async (_hostId: string, directory: string) => ({
      ok: true as const,
      listing: {
        directory,
        parentDirectory: directory === '/' ? '' : '/',
        directories: directory === '/' ? [{ name: 'srv', path: '/srv' }] : [],
        truncated: false,
      },
    }))
    useAppStore.setState({
      folders: [],
      newChatFolderId: null,
      providers: [{ id: 'opencode-cli', name: 'OpenCode CLI', shortName: 'OpenCode', color: '#fff', description: '' }],
      defaultNewChatProviderId: 'opencode-cli',
      listRemoteDirectories,
      executionHosts: [
        { id: 'local', label: 'This computer', transport: 'local', sshAlias: '', runnerStatus: 'ready', runnerVersion: '', platform: 'macos', architecture: 'arm64', lastSeenAt: '' },
        { id: 'lab', label: 'Homelab', transport: 'ssh', sshAlias: 'uam-homelab', runnerStatus: 'ready', runnerVersion: '4.8.0-alpha', platform: 'linux', architecture: 'x86_64', lastSeenAt: '' },
      ],
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    await act(async () => { root.render(<NewChatModal />); await Promise.resolve() })

    act(() => host.querySelector<HTMLButtonElement>('button[aria-label="Homelab"]')?.click())
    expect(listRemoteDirectories).not.toHaveBeenCalled()

    await act(async () => {
      Array.from(host.querySelectorAll<HTMLButtonElement>('button'))
        .find((button) => button.textContent === 'Browse')?.click()
      await Promise.resolve()
    })
    expect(listRemoteDirectories).toHaveBeenCalledWith('lab', '/')
    await act(async () => {
      Array.from(document.body.querySelectorAll<HTMLButtonElement>('button'))
        .find((button) => button.textContent === 'srv')?.click()
      await Promise.resolve()
    })
    act(() => Array.from(document.body.querySelectorAll<HTMLButtonElement>('button'))
      .find((button) => button.textContent === 'Use this directory')?.click())

    expect(host.querySelector<HTMLInputElement>('input[placeholder="/absolute/path/on/selected/host"]')?.value).toBe('/srv')
    act(() => root.unmount())
    host.remove()
  })

  it('creates with the provider-default model and runtime-default effort shown when saved defaults are empty', () => {
    const addSession = vi.fn()
    useAppStore.setState({
      addSession,
      sessions: [{
        id: 'codex-existing',
        name: 'Codex',
        viewMode: 'chat',
        folderId: 'project',
        providerId: 'codex-cli',
		workspaceDirectory: '/tmp/project',
        createdAt: new Date(),
        updatedAt: new Date(),
      }],
      acpBindingBySessionId: {
        'codex-existing': {
          sessionId: 'native-codex',
          providerId: 'codex-cli',
          protocolKind: 'codex-app-server',
          threadId: '',
          running: false,
          lifecycleState: 'stopped',
          processing: false,
          processingStartedAtMs: null,
          readySinceLastSelect: false,
          lastError: '',
          recentStderr: '',
          lastExitCode: null,
          diagnostics: [],
          toolCalls: [],
          planEntries: [],
          availableModes: [],
          currentModeId: 'default',
          availableModels: [{
            id: 'gpt-5.6-sol',
            name: 'GPT-5.6-Sol',
            description: 'Latest frontier model.',
            defaultReasoningEffort: 'low',
            supportedReasoningEfforts: ['low', 'medium', 'high', 'xhigh'],
          }],
          currentModelId: '',
          turnEvents: [],
          turnUserMessageIndex: -1,
          turnAssistantMessageIndex: -1,
          turnSerial: 0,
          pendingPermission: null,
          pendingUserInput: null,
          agentInfo: null,
        },
      },
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<NewChatModal />))

    const codex = host.querySelector<HTMLButtonElement>('button[aria-label="Codex"]')
    act(() => codex?.click())
    act(() => Array.from(host.querySelectorAll<HTMLButtonElement>('[role="menuitemradio"]')).find((option) => option.textContent?.includes('Default'))?.click())

    expect(host.querySelector('[role="menuitemradio"][aria-checked="true"]')?.textContent).toContain('Default')
    const reasoning = host.querySelector<HTMLButtonElement>('button[aria-label="Reasoning effort"]')!
    expect(reasoning.textContent).toContain('Low')
    const create = Array.from(host.querySelectorAll<HTMLButtonElement>('button'))
      .find((button) => button.textContent === 'Create chat')
    act(() => create?.click())

    expect(addSession).toHaveBeenCalledWith('New chat', 'project', 'codex-cli', '', 'low', 'chat')

    act(() => root.unmount())
    host.remove()
  })

  it('submits once while creation is pending and unlocks after failure', async () => {
    let finishCreation: (created: boolean) => void = () => {}
    const addSession = vi.fn()
      .mockImplementationOnce(() => new Promise<boolean>((resolve) => { finishCreation = resolve }))
      .mockResolvedValueOnce(true)
    useAppStore.setState({ addSession })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<NewChatModal />))

    const create = Array.from(host.querySelectorAll<HTMLButtonElement>('button'))
      .find((button) => button.textContent === 'Create chat')!
    act(() => {
      create.click()
      create.click()
    })

    expect(addSession).toHaveBeenCalledTimes(1)
    expect(create.disabled).toBe(true)
    expect(create.getAttribute('aria-busy')).toBe('true')

    await act(async () => {
      finishCreation(false)
      await Promise.resolve()
    })
    expect(create.disabled).toBe(false)
    expect(host.querySelector('[role="alert"]')?.textContent).toContain('chat could not be created')

    await act(async () => {
      create.click()
      await Promise.resolve()
    })
    expect(addSession).toHaveBeenCalledTimes(2)

    act(() => root.unmount())
    host.remove()
  })

  it('cannot be dismissed while chat creation is committing', async () => {
    let finishCreation: (created: boolean) => void = () => {}
    const addSession = vi.fn(() => new Promise<boolean>((resolve) => { finishCreation = resolve }))
    const setNewChatModalOpen = vi.fn()
    useAppStore.setState({ addSession, setNewChatModalOpen })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<NewChatModal />))

    const create = Array.from(host.querySelectorAll<HTMLButtonElement>('button'))
      .find((button) => button.textContent === 'Create chat')!
    act(() => create.click())
    act(() => {
      window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }))
      ;(host.querySelector('button[aria-label="Close new chat"]') as HTMLButtonElement).click()
      expect(Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Cancel')?.disabled).toBe(true)
      host.firstElementChild?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(setNewChatModalOpen).not.toHaveBeenCalled()

    await act(async () => {
      finishCreation(false)
      await Promise.resolve()
    })
    act(() => window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })))
    expect(setNewChatModalOpen).toHaveBeenCalledWith(false)

    act(() => root.unmount())
    host.remove()
  })

  it('dismisses model discovery errors without hiding a new failure or losing retry', async () => {
    let discoveryCalls = 0
    window.cefQuery = ({ request, onSuccess }) => {
      if (JSON.parse(request).action !== 'discoverProviderModels') {
        onSuccess('{}')
        return
      }
      discoveryCalls += 1
      expect(JSON.parse(request).payload.chatId).toBe('')
      onSuccess(JSON.stringify({ started: true, pending: true }))
    }
    useAppStore.setState({
      sessions: [{
        id: 'gemini-existing',
        name: 'Gemini',
        viewMode: 'chat',
        folderId: 'project',
        providerId: 'gemini-cli',
		workspaceDirectory: '/tmp/project',
        createdAt: new Date(),
        updatedAt: new Date(),
      }],
      acpBindingBySessionId: {
        'gemini-existing': {
          sessionId: 'native-gemini',
          threadId: '',
          providerId: 'gemini-cli',
          protocolKind: 'gemini-acp',
          running: false,
          lifecycleState: 'stopped',
          processing: false,
          processingStartedAtMs: null,
          readySinceLastSelect: false,
          lastError: '',
          recentStderr: '',
          lastExitCode: null,
          diagnostics: [],
          toolCalls: [],
          planEntries: [],
          availableModes: [],
          currentModeId: 'default',
          availableModels: [],
          currentModelId: '',
          turnEvents: [],
          turnUserMessageIndex: -1,
          turnAssistantMessageIndex: -1,
          turnSerial: 0,
          pendingPermission: null,
          pendingUserInput: null,
          agentInfo: null,
          modelsLoading: false,
          modelRefreshError: 'Model discovery failed.',
        },
      },
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    await act(async () => {
      root.render(<NewChatModal />)
      await Promise.resolve()
    })

    expect(host.querySelector('[role="alert"]')?.textContent).toContain('Model discovery failed.')
    act(() => host.querySelector<HTMLButtonElement>('[aria-label="Dismiss model refresh error"]')!.click())
    expect(host.querySelector('[role="alert"]')).toBeNull()
    expect(useAppStore.getState().acpBindingBySessionId['gemini-existing'].modelRefreshError).toBe('Model discovery failed.')
    act(() => useAppStore.setState((state) => ({
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'gemini-existing': { ...state.acpBindingBySessionId['gemini-existing'], modelRefreshError: 'Provider disconnected.' },
      },
    })))
    expect(host.querySelector('[role="alert"]')?.textContent).toContain('Provider disconnected.')
    const retry = Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Retry')
    await act(async () => {
      retry?.click()
      await Promise.resolve()
    })
    expect(discoveryCalls).toBe(1)

    act(() => root.unmount())
    host.remove()
    delete window.cefQuery
  })

	it('displays workspace-scoped models before the first chat and refreshes only explicitly', async () => {
	  const requests: Array<{ action: string; payload?: Record<string, unknown> }> = []
	  window.cefQuery = ({ request, onSuccess }) => {
		requests.push(JSON.parse(request))
		onSuccess(JSON.stringify({ started: true, pending: true }))
	  }
	  useAppStore.setState({
		defaultNewChatProviderId: 'codex-cli',
		sessions: [],
		providerModelCatalogs: [{
		  providerId: 'codex-cli',
		  workspaceDirectory: '/tmp/project',
		  executionHostId: 'local',
		  availableModels: [{ id: 'gpt-first-chat', name: 'GPT First Chat', description: 'Discovered without a chat.' }],
		  currentModelId: 'gpt-first-chat',
		  modelsLoading: false,
		  modelRefreshError: '',
		}],
	  })
	  const host = document.createElement('div')
	  document.body.appendChild(host)
	  const root = createRoot(host)
	  await act(async () => { root.render(<NewChatModal />); await Promise.resolve() })

	  expect(host.querySelector('[role="menuitemradio"][aria-checked="true"]')?.textContent).toContain('Default')
	  expect(host.querySelector('[data-testid="model-options"]')?.textContent).toContain('GPT First Chat')
	  expect(requests).toEqual([])
	  await act(async () => {
	    Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.getAttribute('aria-label') === 'Refresh models')?.click()
	  })
	  expect(requests).toContainEqual(expect.objectContaining({
		action: 'discoverProviderModels',
		payload: { chatId: '', providerId: 'codex-cli', workspaceDirectory: '/tmp/project', executionHostId: 'local' },
	  }))

	  act(() => root.unmount())
	  host.remove()
	  delete window.cefQuery
	})

  it('blocks chat creation and offers to create a workspace when none exists', async () => {
    const browseFolderDirectory = vi.fn().mockResolvedValue('/tmp/New Workspace')
    const addFolder = vi.fn().mockResolvedValue(false)
    useAppStore.setState({ folders: [], newChatFolderId: null, browseFolderDirectory, addFolder })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<NewChatModal />))
    expect(host.textContent).not.toContain('Choose a workspace.')
    expect(host.querySelector('[role="alert"]')).toBeNull()
    expect(Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Create chat')?.disabled).toBe(true)
    const createWorkspace = Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Choose workspace')
    await act(async () => { createWorkspace?.click(); await Promise.resolve(); await Promise.resolve() })
    expect(addFolder).toHaveBeenCalledWith('New Workspace', null, '/tmp/New Workspace')
    expect(host.querySelectorAll('[role="alert"]')).toHaveLength(1)
    expect(host.querySelector('[role="alert"]')?.textContent).toBe('The workspace could not be created. Choose another folder.')
    act(() => root.unmount())
    host.remove()
  })

  it('uses consistent field labels and a neutral close control', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<NewChatModal />))
    const label = (text: string) => Array.from(host.querySelectorAll<HTMLElement>('div')).find((element) => element.textContent?.trim() === text)
    expect(label('Runs on')?.style.color).toBe('var(--text-2)')
    expect(label('Folder')?.tagName).toBe('DIV')
    expect(host.querySelector<HTMLButtonElement>('[aria-label="Close new chat"]')?.className).toContain('uam-iconbtn--ghost')
    act(() => root.unmount())
    host.remove()
  })

  it('remembers host workspaces and provider choices with explicit remote default overrides', async () => {
    const addSession = vi.fn().mockResolvedValue(false)
    const defaults = { modelId: 'reasoning-model', approvalMode: 'default' as const, commandSafetyTier: 'off' as const, memoryEnabled: true, reasoningEffort: 'high', serviceTier: '' }
    useAppStore.setState({
      addSession,
      defaultNewChatProviderId: 'codex-cli',
      providerChatDefaults: { 'codex-cli': defaults },
      hostScopedSettingsValues: {
        lab: { defaultNewChatProviderId: 'codex-cli', providerChatDefaults: { 'codex-cli': { ...defaults, modelId: '', reasoningEffort: '' } }, mcpServers: [] },
        pc: { defaultNewChatProviderId: 'codex-cli', providerChatDefaults: { 'codex-cli': { ...defaults, modelId: '', reasoningEffort: '' } }, mcpServers: [] },
      },
      executionHosts: [...useAppStore.getState().executionHosts,
        { id: 'lab', label: 'Home lab', transport: 'ssh', sshAlias: 'lab', runnerStatus: 'ready', runnerVersion: '1', platform: 'linux', architecture: 'x64', lastSeenAt: '' },
        { id: 'pc', label: 'Desktop PC', transport: 'ssh', sshAlias: 'pc', runnerStatus: 'ready', runnerVersion: '1', platform: 'windows', architecture: 'x64', lastSeenAt: '' }],
      folders: [...useAppStore.getState().folders, { id: 'remote', name: 'Remote', parentId: null, directory: '/srv/remote', executionHostId: 'lab', isExpanded: true, createdAt: new Date() }],
      providerModelCatalogs: [{ providerId: 'codex-cli', workspaceDirectory: '/tmp/project', executionHostId: 'local', currentModelId: 'reasoning-model', modelsLoading: false, modelRefreshError: '', availableModels: [
        { id: 'reasoning-model', name: 'Reasoning model', description: '', supportedReasoningEfforts: ['low', 'high'], defaultReasoningEffort: 'low' },
        { id: 'plain-model', name: 'Plain model', description: '', supportedReasoningEfforts: [] },
      ] }],
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    try {
      await act(async () => { root.render(<NewChatModal />) })
      const click = (label: string) => act(() => {
        const button = Array.from(document.querySelectorAll<HTMLButtonElement>('button')).find((item) => item.getAttribute('aria-label') === label || (item.getAttribute('role') === 'option' && item.querySelector('span.block')?.textContent === label) || (item.getAttribute('role') === 'menuitemradio' && item.querySelector('.uam-np-model__name > span')?.textContent === label))!
        button.click()
      })
      const setInput = (label: string, value: string) => act(() => {
        const input = host.querySelector<HTMLInputElement>(`input[aria-label="${label}"]`)!
        Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(input, value)
        input.dispatchEvent(new Event('input', { bubbles: true }))
      })
      expect(host.querySelector('[aria-label="Codex"]')?.getAttribute('aria-pressed')).toBe('true')
      expect(host.querySelector('[aria-label="This computer"]')?.getAttribute('aria-pressed')).toBe('true')
      setInput('Chat name', 'Keep my draft')
      click('Home lab')
      expect(host.querySelector('[role="menuitemradio"][aria-checked="true"]')?.textContent).toContain('Default')
      click('Folder')
      click('New remote workspace…')
      setInput('Remote workspace path', '/srv/custom')
      click('Desktop PC')
      click('Home lab')
      expect(host.querySelector<HTMLInputElement>('[aria-label="Remote workspace path"]')?.value).toBe('/srv/custom')
      expect(host.querySelector<HTMLInputElement>('[aria-label="Remote workspace path"]')?.readOnly).toBe(false)
      act(() => useAppStore.setState({ folders: [...useAppStore.getState().folders] }))
      expect(host.querySelector('[aria-label="Home lab"]')?.getAttribute('aria-pressed')).toBe('true')
      click('This computer')
      expect(host.querySelector('[role="menuitemradio"][aria-checked="true"]')?.textContent).toContain('Reasoning model')
      expect(host.querySelector('[aria-label="Reasoning effort"]')?.textContent).toContain('High')
      click('Plain model')
      expect(host.querySelector('[aria-label="Reasoning effort"]')).toBeNull()
      click('Gemini')
      click('Codex')
      expect(host.querySelector('[role="menuitemradio"][aria-checked="true"]')?.textContent).toContain('Plain model')
      await act(async () => { Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Create chat')!.click() })
      expect(addSession).toHaveBeenCalledWith('Keep my draft', 'project', 'codex-cli', 'plain-model', '', 'chat')
      expect(host.querySelector<HTMLInputElement>('[aria-label="Chat name"]')?.value).toBe('Keep my draft')
    } finally {
      act(() => root.unmount())
      host.remove()
    }
  })

  it('keeps all five providers selectable and honors the saved provider default', () => {
    const providers = [
      ['gemini-cli', 'Gemini'], ['codex-cli', 'Codex'], ['opencode-cli', 'OpenCode'], ['claude-cli', 'Claude'], ['copilot-cli', 'Copilot'],
    ].map(([id, shortName]) => ({ id, name: shortName, shortName, color: '#fff', description: '' }))
    useAppStore.setState({ providers, defaultNewChatProviderId: 'claude-cli' })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<NewChatModal />))
    for (const provider of providers) expect(host.querySelector<HTMLButtonElement>(`button[aria-label="${provider.shortName}"]`)?.disabled).toBe(false)
    expect(host.querySelector('[aria-label="Claude"]')?.getAttribute('aria-pressed')).toBe('true')
    act(() => root.unmount())
    host.remove()
  })

  it('retains the draft and allows retry after a rejected create action', async () => {
    const addSession = vi.fn().mockRejectedValueOnce(new Error('Connection lost while creating chat.')).mockResolvedValueOnce(false)
    const setNewChatModalOpen = vi.fn()
    useAppStore.setState({ addSession, setNewChatModalOpen })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<NewChatModal />))
    const name = host.querySelector<HTMLInputElement>('[aria-label="Chat name"]')!
    act(() => {
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(name, 'Unsent draft')
      name.dispatchEvent(new Event('input', { bubbles: true }))
    })
    const create = Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Create chat')!
    await act(async () => { create.click() })
    expect(host.textContent).toContain('Connection lost while creating chat.')
    expect(name.value).toBe('Unsent draft')
    expect(create.disabled).toBe(false)
    expect(setNewChatModalOpen).not.toHaveBeenCalled()
    await act(async () => { create.click() })
    expect(addSession).toHaveBeenCalledTimes(2)
    act(() => root.unmount())
    host.remove()
  })

  it('rejects invalid remote paths on Enter and blocks incompatible Copilot fallback', async () => {
    const addSession = vi.fn().mockResolvedValue(false)
    useAppStore.setState({ addSession,
      providers: [{ id: 'copilot-cli', name: 'Copilot', shortName: 'Copilot', color: '#fff', description: '' }],
      defaultNewChatProviderId: 'copilot-cli',
      cliVersionManager: { providers: [{ providerId: 'copilot-cli', installedVersion: 'old', selectedVersion: '', availableVersions: [], preferredVersion: '', status: 'known-incompatible', message: 'Incompatible Copilot version.', running: false, lastCommand: '', lastOutput: '' }] },
      executionHosts: [...useAppStore.getState().executionHosts, { id: 'lab', label: 'Home lab', transport: 'ssh', sshAlias: 'lab', runnerStatus: 'ready', runnerVersion: '1', platform: 'linux', architecture: 'x64', lastSeenAt: '' }],
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<NewChatModal />))
    const create = () => Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Create chat')!
    expect(create().disabled).toBe(true)
    expect(host.textContent).toContain('Terminal fallback is also unavailable.')
    expect(host.querySelector('[role="alert"]')?.textContent).toContain('Terminal fallback is also unavailable.')
    expect((host.querySelector('[role="alert"]') as HTMLElement).style.color).toBe('var(--error)')
    act(() => host.querySelector<HTMLButtonElement>('[aria-label="Home lab"]')!.click())
    const path = host.querySelector<HTMLInputElement>('[aria-label="Remote workspace path"]')!
    act(() => {
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(path, 'relative')
      path.dispatchEvent(new Event('input', { bubbles: true }))
    })
    expect(create().disabled).toBe(true)
    await act(async () => host.querySelector('[aria-label="Chat name"]')!.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true })))
    expect(addSession).not.toHaveBeenCalled()
    act(() => root.unmount())
    host.remove()
  })
  it('offers automatic routing only for a configured source and preserves explicit creation by default', async () => {
    const addSession = vi.fn().mockResolvedValue(true)
    useAppStore.setState({ addSession, providerChatDefaults: { 'gemini-cli': { modelId: 'local-account-model', reasoningEffort: 'high', approvalMode: 'default', commandSafetyTier: 'off', memoryEnabled: true, serviceTier: '' } }, providerModelCatalogs: [{ providerId: 'gemini-cli', executionHostId: 'local', workspaceDirectory: '/tmp/project', currentModelId: 'local-account-model', modelsLoading: false, modelRefreshError: '', availableModels: [{ id: 'local-account-model', name: 'Local account model', description: '' }] }] })
    const host = document.createElement('div'); document.body.appendChild(host); const root = createRoot(host)
    try {
      act(() => root.render(<NewChatModal />))
      const automatic = () => host.querySelector<HTMLButtonElement>('button[aria-label="Automatic"]')!
      expect(automatic().disabled).toBe(true)
      const create = () => Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Create chat')!
      await act(async () => create().click())
      expect(addSession.mock.calls[0]).toHaveLength(6)
      act(() => root.render(<NewChatModal key="new-automatic-chat" />))
      act(() => useAppStore.setState({ executionRouting: { enabled: true, hosts: [{ hostId: 'ssh-a', enabled: true, maxActiveTurns: 2, providers: [{ providerId: 'gemini-cli', connectionIdentity: 'host-v1', providerConfigurationIdentity: 'provider-v1' }], workspaceMappings: [{ sourceFolderId: 'project', sourceDirectory: '/tmp/project', destinationFolderId: 'remote-project' }] }] } }))
      expect(automatic().disabled).toBe(false)
      act(() => automatic().click())
      expect(host.querySelector<HTMLButtonElement>('[role="menuitemradio"]')?.disabled).toBe(true)
      act(() => host.querySelector<HTMLButtonElement>('button[aria-label="This computer"]')!.click())
      expect(host.querySelector<HTMLButtonElement>('[role="menuitemradio"]')?.disabled).toBe(false)
      expect(host.querySelector('[role="menuitemradio"][aria-checked="true"]')?.textContent).toContain('Local account model')
      act(() => automatic().click())
      await act(async () => create().click())
      expect(addSession).toHaveBeenLastCalledWith('New chat', 'project', 'gemini-cli', undefined, undefined, 'chat', 'local', '/tmp/project', 'automatic')
    } finally { act(() => root.unmount()); host.remove() }
  })

  it('passes the captured worktree context and refuses stale context instead of default isolation', async () => {
    const source = { id: 'source-chat', name: 'Source', viewMode: 'chat' as const, folderId: 'project', createdAt: new Date(), updatedAt: new Date(), executionHostId: 'local', workspaceIsolationKind: 'gitWorktree', workspaceWorktreeDirectory: '/tmp/selected-worktree', workspaceBranchName: 'selected-branch' }
    const context = { sourceChatId: source.id, expectedWorkspaceDirectory: source.workspaceWorktreeDirectory, expectedExecutionHostId: 'local' }
    const addSession = vi.fn().mockResolvedValue(false)
    useAppStore.setState({ sessions: [source], activeSessionId: 'unrelated-chat', newChatFolderId: 'project', newChatWorktreeContext: context, addSession, setNewChatModalOpen: originalSetNewChatModalOpen })
    const host = document.createElement('div'); document.body.appendChild(host); const root = createRoot(host)
    try {
      act(() => root.render(<NewChatModal />))
      expect(host.textContent).toContain('Using the selected worktree: selected-branch')
      const create = () => Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Create chat')!
      await act(async () => create().click())
      expect(addSession).toHaveBeenCalledOnce()
      expect(addSession.mock.calls[0].slice(6)).toEqual(['local', '', 'explicit', context])
      act(() => useAppStore.setState({ sessions: [{ ...source, workspaceWorktreeDirectory: '/tmp/replaced-worktree' }] }))
      expect(useAppStore.getState().newChatWorktreeContext).toBeNull()
      expect(host.textContent).toContain('The selected worktree changed')
      expect(create().disabled).toBe(true)
      await act(async () => create().click())
      expect(addSession).toHaveBeenCalledOnce()
      act(() => host.querySelector<HTMLButtonElement>('[aria-label="Close new chat"]')!.click())
      expect(useAppStore.getState().newChatWorktreeContext).toBeNull()
      act(() => useAppStore.getState().setNewChatModalOpen(true, 'project'))
      expect(useAppStore.getState().newChatWorktreeContext).toBeNull()
    } finally { act(() => root.unmount()); host.remove(); useAppStore.setState({ newChatWorktreeContext: null }) }
  })

  it('projects a configured SSH host provider and model defaults when opening', async () => {
    const optionsSpy = vi.spyOn(modelOptions, 'buildModelOptions')
    useAppStore.setState({
      folders: [{ id: 'remote-fixture', name: 'Remote', parentId: null, directory: '/private-fixture', executionHostId: 'lab', isExpanded: true, createdAt: new Date() }],
      newChatFolderId: 'remote-fixture',
      executionHosts: [{ id: 'lab', label: 'Lab', transport: 'ssh', sshAlias: 'private-fixture', runnerStatus: 'ready', runnerVersion: 'test', platform: 'linux', architecture: 'x64', lastSeenAt: '' }],
      hostScopedSettingsValues: { lab: { defaultNewChatProviderId: 'codex-cli', providerChatDefaults: { 'codex-cli': { modelId: 'host-model', approvalMode: 'default', commandSafetyTier: 'off', memoryEnabled: true, memoryLevel: 'strict', reasoningEffort: 'high', serviceTier: '', smallModelMode: false } }, mcpServers: [] } },
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    try {
      await act(async () => root.render(<NewChatModal />))
      expect(optionsSpy.mock.calls.some(call => call[1] === 'host-model' && call[3] === 'codex-cli')).toBe(true)
    } finally {
      act(() => root.unmount())
      host.remove()
      optionsSpy.mockRestore()
    }
  })

})
