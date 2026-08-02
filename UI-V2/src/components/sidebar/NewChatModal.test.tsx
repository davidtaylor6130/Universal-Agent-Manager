import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { useAppStore } from '../../store/useAppStore'
import { NewChatModal } from './NewChatModal'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

describe('NewChatModal', () => {
  beforeEach(() => {
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
    })
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

    const create = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Create chat')
    act(() => create?.click())

	expect(addSession).toHaveBeenCalledWith('New chat', 'project', 'codex-cli', 'gpt-5.4', 'high')

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
    expect(addSession).toHaveBeenCalledWith('New chat', 'project', 'gemini-cli', '', '')

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
      .find((button) => button.textContent === 'Create chat')
    act(() => create?.click())

    expect(addSession).toHaveBeenCalledWith('New chat', 'project', 'codex-cli', '', 'low')

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

    await act(async () => {
      create.click()
      await Promise.resolve()
    })
    expect(addSession).toHaveBeenCalledTimes(2)

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

  it('blocks chat creation and offers to create a workspace when none exists', async () => {
    const browseFolderDirectory = vi.fn().mockResolvedValue('/tmp/New Workspace')
    const addFolder = vi.fn().mockResolvedValue(true)
    useAppStore.setState({ folders: [], newChatFolderId: null, browseFolderDirectory, addFolder })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<NewChatModal />))
    expect(host.textContent).toContain('A workspace is required')
    expect(Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Create chat')?.disabled).toBe(true)
    const createWorkspace = Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Create workspace')
    await act(async () => { createWorkspace?.click(); await Promise.resolve(); await Promise.resolve() })
    expect(addFolder).toHaveBeenCalledWith('New Workspace', null, '/tmp/New Workspace')
    act(() => root.unmount())
    host.remove()
  })
})
