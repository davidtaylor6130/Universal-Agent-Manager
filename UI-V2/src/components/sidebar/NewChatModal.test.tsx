import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { useAppStore } from '../../store/useAppStore'
import { NewChatModal } from './NewChatModal'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

const originalDiscoverProviderModels = useAppStore.getState().discoverProviderModels

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
      newChatFolderId: 'project',
	  sessions: [],
	  acpBindingBySessionId: {},
      providerModelCatalogs: [],
      cliVersionManager: { providers: [] },
      discoverProviderModels: originalDiscoverProviderModels,
    })
  })

  it.each(['unknown', 'verified', 'untested', 'untested-newer', 'provider-managed'] as const)(
    'allows structured creation when provider readiness is %s',
    (status) => {
      useAppStore.setState({ cliVersionManager: { providers: [{ providerId: 'gemini-cli', installedVersion: '1.0.0', selectedVersion: '1.0.0', availableVersions: [], preferredVersion: '1.0.0', status, message: '', running: false, lastCommand: '', lastOutput: '' }] } })
      const host = document.createElement('div')
      document.body.appendChild(host)
      const root = createRoot(host)
      act(() => root.render(<NewChatModal />))
      expect(Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Create structured chat')?.disabled).toBe(false)
      act(() => root.unmount())
      host.remove()
    },
  )

  it.each(['checking', 'installing', 'known-incompatible'] as const)(
    'blocks only structured creation for provider readiness %s and keeps terminal creation available',
    (status) => {
      useAppStore.setState({ cliVersionManager: { providers: [{ providerId: 'gemini-cli', installedVersion: '0.1.0', selectedVersion: '1.0.0', availableVersions: [{ version: '1.0.0', preferred: true }], preferredVersion: '1.0.0', status, message: 'Provider needs attention.', running: status === 'checking' || status === 'installing', lastCommand: '', lastOutput: '' }] } })
      const host = document.createElement('div')
      document.body.appendChild(host)
      const root = createRoot(host)
      act(() => root.render(<NewChatModal />))
      expect(Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Create structured chat')?.disabled).toBe(true)
      expect(Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Create terminal chat')?.disabled).toBe(false)
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

  it('offers direct provider check and supported-version install actions', async () => {
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
    await act(async () => { (Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Install supported version'))?.click(); await Promise.resolve() })
    expect(applyCliProviderVersion).toHaveBeenCalledWith('gemini-cli', '1.0.0')
    await act(async () => { (Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Check again'))?.click(); await Promise.resolve() })
    expect(refreshCliProviderVersion).toHaveBeenCalledWith('gemini-cli')
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

    const structured = Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Create structured chat')
    const terminal = Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Create terminal chat')
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

    act(() => {
      host.querySelector<HTMLButtonElement>('button[aria-label="Provider"]')?.click()
    })
    const codex = Array.from(document.body.querySelectorAll('button')).find((button) => button.textContent?.includes('Codex'))
    act(() => codex?.click())

    expect(host.querySelector('select[aria-label="Model"]')).toBeNull()
    const model = host.querySelector<HTMLButtonElement>('button[aria-label="Model"]')!
    act(() => {
      model.click()
    })
    expect(model.getAttribute('aria-expanded')).toBe('true')
    const modelList = document.body.querySelector<HTMLElement>('[role="listbox"][aria-label="Model"]')!
    expect(modelList).toBeTruthy()

    act(() => {
      modelList.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }))
    })
    expect(document.body.querySelector('[role="listbox"][aria-label="Model"]')).toBeNull()
    expect(setNewChatModalOpen).not.toHaveBeenCalled()

    act(() => model.click())
    expect(document.body.querySelector('[role="listbox"][aria-label="Model"]')).toBeTruthy()
    act(() => document.body.dispatchEvent(new MouseEvent('mousedown', { bubbles: true })))
    expect(document.body.querySelector('[role="listbox"][aria-label="Model"]')).toBeNull()

    act(() => model.click())
    const gpt = Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="option"]')).find((option) =>
      option.textContent?.includes('GPT-5.4')
    )
    act(() => gpt?.click())

    const reasoning = host.querySelector<HTMLButtonElement>('button[aria-label="Reasoning effort"]')!
    act(() => reasoning.click())
    const high = Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="option"]')).find((option) =>
      option.textContent?.startsWith('High')
    )
    act(() => high?.click())

    const create = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Create structured chat')
    act(() => create?.click())

	expect(addSession).toHaveBeenCalledWith('New chat', 'project', 'codex-cli', 'gpt-5.4', 'high', 'chat')

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

    const create = Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Create structured chat')
    expect(create?.disabled).toBe(false)
    act(() => create?.click())
    expect(addSession).toHaveBeenCalledWith('New chat', 'project', 'gemini-cli', '', '', 'chat')

    act(() => root.unmount())
    host.remove()
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
    await act(async () => { root.render(<NewChatModal />); await Promise.resolve() })

    act(() => host.querySelector<HTMLButtonElement>('button[aria-label="Execution host"]')?.click())
    const remoteHost = Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="option"]'))
      .find((option) => option.textContent?.includes('Home lab'))
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
		.find((button) => button.textContent === 'Discover remote models')?.click()
	  await Promise.resolve()
	})
	act(() => host.querySelector<HTMLButtonElement>('button[aria-label="Model"]')?.click())
	const modelMenu = document.body.querySelector('[role="listbox"][aria-label="Model"]')
	expect(modelMenu?.textContent).toContain('Remote Only')
	expect(modelMenu?.textContent).not.toContain('Local Only')
	expect(modelMenu?.textContent).not.toContain('Wrong Case')
	expect(modelMenu?.textContent).not.toContain('Controller Default')
	act(() => Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="option"]'))
	  .find((option) => option.textContent?.includes('Remote Only'))?.click())
    const create = Array.from(host.querySelectorAll<HTMLButtonElement>('button'))
      .find((button) => button.textContent === 'Create structured chat')
    await act(async () => { create?.click(); await Promise.resolve() })

	expect(discoverProviderModels).toHaveBeenCalledWith('', 'opencode-cli', '/srv/project', 'lab')
    expect(host.textContent).toContain('Computer Use is disabled for remote chats.')
	expect(host.textContent).not.toContain('A workspace is required')
    expect(addSession).toHaveBeenCalledWith('New chat', null, 'opencode-cli', 'remote-only', '', 'chat', 'lab', '/srv/project')

    act(() => root.unmount())
    host.remove()
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

	  act(() => host.querySelector<HTMLButtonElement>('button[aria-label="Execution host"]')?.click())
	  act(() => Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="option"]'))
		.find((option) => option.textContent?.includes('Windows PC'))?.click())
	  const input = host.querySelector<HTMLInputElement>('input[placeholder="/absolute/path/on/selected/host"]')!
	  const create = () => Array.from(host.querySelectorAll<HTMLButtonElement>('button'))
		.find((button) => button.textContent === 'Create structured chat')!
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

    act(() => host.querySelector<HTMLButtonElement>('button[aria-label="Provider"]')?.click())
    const codex = Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="option"]'))
      .find((option) => option.textContent?.includes('Codex'))
    act(() => codex?.click())

    expect(host.querySelector<HTMLButtonElement>('button[aria-label="Model"]')?.textContent).toContain('Default')
    const reasoning = host.querySelector<HTMLButtonElement>('button[aria-label="Reasoning effort"]')!
    expect(reasoning.textContent).toContain('Low')
    const create = Array.from(host.querySelectorAll<HTMLButtonElement>('button'))
      .find((button) => button.textContent === 'Create structured chat')
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
      .find((button) => button.textContent === 'Create structured chat')!
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
      .find((button) => button.textContent === 'Create structured chat')!
    act(() => create.click())
    act(() => {
      window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }))
      ;(host.querySelector('button[aria-label="Close new chat"]') as HTMLButtonElement).click()
      Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Cancel')?.click()
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

  it('surfaces failed model discovery and lets the user retry', async () => {
    let discoveryCalls = 0
    window.cefQuery = ({ request, onSuccess, onFailure }) => {
      if (JSON.parse(request).action !== 'discoverProviderModels') {
        onSuccess('{}')
        return
      }
      discoveryCalls += 1
      if (discoveryCalls === 1) onFailure(500, 'Model discovery failed.')
      else onSuccess(JSON.stringify({ started: true, pending: true }))
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
          modelRefreshError: '',
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
    const retry = Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Retry')
    await act(async () => {
      retry?.click()
      await Promise.resolve()
    })
    expect(discoveryCalls).toBe(2)

    act(() => root.unmount())
    host.remove()
    delete window.cefQuery
  })

	it('discovers and displays workspace-scoped models before the first chat exists', async () => {
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

	  expect(host.querySelector<HTMLButtonElement>('button[aria-label="Model"]')?.textContent).toContain('Default')
	  act(() => host.querySelector<HTMLButtonElement>('button[aria-label="Model"]')?.click())
	  expect(document.body.querySelector('[role="listbox"][aria-label="Model"]')?.textContent).toContain('GPT First Chat')
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
    const addFolder = vi.fn().mockResolvedValue(true)
    useAppStore.setState({ folders: [], newChatFolderId: null, browseFolderDirectory, addFolder })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<NewChatModal />))
    expect(host.textContent).toContain('A workspace is required')
    expect(Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Create structured chat')?.disabled).toBe(true)
    const createWorkspace = Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Create workspace')
    await act(async () => { createWorkspace?.click(); await Promise.resolve(); await Promise.resolve() })
    expect(addFolder).toHaveBeenCalledWith('New Workspace', null, '/tmp/New Workspace')
    act(() => root.unmount())
    host.remove()
  })
})
