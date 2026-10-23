import { rememberSubagentParent } from '../../utils/subagentNavigation'
import { act, Profiler } from 'react'
import { createRoot } from 'react-dom/client'
import { beforeEach, describe, expect, it, vi } from 'vitest'

vi.mock('@xterm/xterm', () => ({
  Terminal: class {
    rows = 24
    cols = 80
    options: { fontFamily?: string; fontSize?: number }
    constructor(options: { fontFamily?: string; fontSize?: number }) { this.options = options }
    loadAddon() {}
    open() {}
    write() {}
    writeln() {}
    dispose() {}
    onResize() {
      return { dispose() {} }
    }
    onData() {
      return { dispose() {} }
    }
  },
}))

vi.mock('@xterm/addon-fit', () => ({
  FitAddon: class {
    fit() {}
  },
}))

import { MainPanel } from './MainPanel'
import { useAppStore } from '../../store/useAppStore'
import { assignChatToPane, chatGridLeaves, defaultChatGridLayout, readChatGridLayout, readChatViewMode, setChatInLeaf, splitChatLeaf, writeChatGridLayout, writeChatViewMode } from '../../utils/chatGridStorage'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

function paneLayout(...sessionIds: string[]) {
  let layout = defaultChatGridLayout
  while (chatGridLeaves(layout.root).length < sessionIds.length) layout = splitChatLeaf(layout, layout.activeLeafId, 'horizontal')
  chatGridLeaves(layout.root).forEach((leaf, index) => { layout = setChatInLeaf(layout, sessionIds[index] ?? '', leaf.id) })
  return { ...layout, activeLeafId: chatGridLeaves(layout.root)[0].id }
}

describe('MainPanel', () => {
  beforeEach(() => {
    vi.restoreAllMocks()
    vi.stubGlobal('ResizeObserver', class {
      observe() {}
      unobserve() {}
      disconnect() {}
    })
    const stored = new Map<string, string>()
    Object.defineProperty(globalThis, 'localStorage', {
      configurable: true,
      value: {
        getItem: (key: string) => stored.get(key) ?? null,
        setItem: (key: string, value: string) => stored.set(key, value),
      },
    })
    delete window.cefQuery
    useAppStore.setState({
      sessions: [
        {
          id: 'chat-1',
          name: 'Gemini Session',
          viewMode: 'chat',
          folderId: null,
          workspaceDirectory: '/tmp/project',
          createdAt: new Date('2026-01-01T00:00:00.000Z'),
          updatedAt: new Date('2026-01-01T00:00:00.000Z'),
        },
      ],
      executionHosts: [
        { id: 'local', label: 'This computer', transport: 'local', sshAlias: '', runnerStatus: 'ready', runnerVersion: '', platform: 'macos', architecture: 'arm64', lastSeenAt: '' },
      ],
      activeSessionId: 'chat-1',
      lastAppliedStateRevision: -1,
      messages: { 'chat-1': [] },
      acpBindingBySessionId: {
        'chat-1': {
          sessionId: 'native-1',
          providerId: 'gemini-cli',
          protocolKind: 'gemini-acp',
          threadId: '',
          running: true,
          lifecycleState: 'processing',
          processing: true,
          readySinceLastSelect: false,
         processingStartedAtMs: Date.now(),
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
        },
      },
      cliBindingBySessionId: {},
    })
  })

  it('locks view switching while ACP or CLI output is active', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<MainPanel />)
    })

    expect(host.querySelectorAll('.uam-layout-button')).toHaveLength(2)
    expect(host.querySelector('[data-testid="provider-badge-chat-1"] .lucide-chevron-down')).toBeNull()

    const chatButton = () => host.querySelector('button[aria-label="Chat view"]') as HTMLButtonElement
    const cliButton = () => host.querySelector('button[aria-label="Terminal fallback"]') as HTMLButtonElement

    expect(chatButton().textContent).toBe('')
    expect(cliButton().textContent).toBe('')
    expect(host.querySelector('[data-testid="chat-workspace-chat-1"]')?.textContent).toBe('project · Local')
    expect(host.querySelector('[data-testid="chat-host-chat-1"]')?.className).toContain('shrink-0')
    expect(cliButton().disabled).toBe(true)

    act(() => {
      useAppStore.setState((state) => ({
        acpBindingBySessionId: {
          ...state.acpBindingBySessionId,
          'chat-1': {
            ...state.acpBindingBySessionId['chat-1'],
            lifecycleState: 'ready',
            processing: false,
          },
        },
      }))
    })
    expect(cliButton().disabled).toBe(false)

    act(() => {
      useAppStore.setState({
        cliBindingBySessionId: {
          'chat-1': {
            terminalId: 'term-1',
            boundChatId: 'chat-1',
            running: true,
            lifecycleState: 'busy',
            turnState: 'busy',
            processing: true,
            readySinceLastSelect: false,
            active: true,
            lastError: '',
          },
        },
      })
    })
    expect(cliButton().disabled).toBe(false)
    act(() => useAppStore.setState((state) => ({
      cliBindingBySessionId: {
        'chat-1': { ...state.cliBindingBySessionId['chat-1'], lifecycleState: 'shuttingDown' },
      },
    })))
    expect(cliButton().disabled).toBe(true)

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('shows the saved remote host beside the workspace and reacts to host updates', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => root.render(<MainPanel />))
    act(() => useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) => ({ ...session, executionHostId: 'lab', workspaceDirectory: '' })),
      executionHosts: [{
        id: 'lab', label: 'Homelab', transport: 'ssh', sshAlias: 'homelab', runnerStatus: 'ready', runnerVersion: '', platform: 'linux', architecture: 'x86_64', lastSeenAt: '',
      }],
    })))
    expect(host.querySelector('[data-testid="chat-workspace-chat-1"]')?.textContent).toBe('Remote · Homelab')
    expect(host.querySelector('[data-testid="chat-host-chat-1"]')?.textContent).toBe('Remote · Homelab')

    act(() => useAppStore.setState((state) => ({
      executionHosts: state.executionHosts.map((executionHost) => ({ ...executionHost, label: 'AI Server' })),
    })))
    expect(host.querySelector('[data-testid="chat-workspace-chat-1"]')?.textContent).toBe('Remote · AI Server')

    act(() => useAppStore.setState({ executionHosts: [] }))
    expect(host.querySelector('[data-testid="chat-workspace-chat-1"]')?.textContent).toBe('Remote · lab')

    act(() => root.unmount())
    host.remove()
  })

  it('disables runtime stop during a turn and shows the idle shutdown countdown afterward', async () => {
    const originalStop = useAppStore.getState().stopAcpSession
    const stopAcpSession = vi.fn(() => Promise.resolve(true))
    useAppStore.setState({ stopAcpSession })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<MainPanel />))

    const stopButton = () => host.querySelector<HTMLButtonElement>('.uam-chat-pane__header button[aria-label="Stop runtime"]')
    expect(stopButton()?.querySelector('.lucide-power-off')).toBeTruthy()
    expect(stopButton()?.disabled).toBe(true)
    stopButton()?.click()
    expect(stopAcpSession).not.toHaveBeenCalled()
    act(() => useAppStore.setState((state) => ({
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'], lifecycleState: 'ready', processing: false,
          idleCountdownStartsAtMs: Date.now() + 60_000, idleShutdownAtMs: Date.now() + 660_000, idleShutdownTimeoutSeconds: 600,
        },
      },
    })))
    expect(stopButton()?.querySelector('.lucide-power-off')).toBeTruthy()
    expect(stopButton()?.disabled).toBe(false)
    expect(stopButton()?.querySelector('.uam-runtime-stop-timer circle')).toBeNull()
    act(() => useAppStore.setState((state) => ({ acpBindingBySessionId: {
      ...state.acpBindingBySessionId, 'chat-1': { ...state.acpBindingBySessionId['chat-1'], idleCountdownStartsAtMs: Date.now() - 1000 },
    } })))
    expect(stopButton()?.querySelector('.uam-runtime-stop-timer circle')).toBeTruthy()
    expect(host.querySelector('.uam-composer-action[title="Stop runtime"]')).toBeNull()
    await act(async () => { stopButton()?.click(); await Promise.resolve() })
    expect(stopAcpSession).toHaveBeenCalledWith('chat-1', 'timeout')

    act(() => useAppStore.setState((state) => ({
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': { ...state.acpBindingBySessionId['chat-1'], running: false, lifecycleState: 'stopped' },
      },
    })))
    expect(stopButton()).toBeNull()
    act(() => root.unmount())
    host.remove()
    useAppStore.setState({ stopAcpSession: originalStop })
  })

  it('opens a terminal-first session in the terminal fallback view', async () => {
    const binding = useAppStore.getState().acpBindingBySessionId['chat-1']
    const onRender = vi.fn()
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) => ({ ...session, viewMode: 'cli' })),
      acpBindingBySessionId: {},
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    await act(async () => {
      root.render(<Profiler id="terminal-pane" onRender={onRender}><MainPanel /></Profiler>)
      await Promise.resolve()
    })

    expect(host.querySelector('button[aria-label="Terminal fallback"]')?.getAttribute('aria-pressed')).toBe('true')
    expect(host.textContent).toContain('Loading terminal')

    onRender.mockClear()
    for (let index = 0; index < 20; index += 1) {
      act(() => useAppStore.setState({
        acpBindingBySessionId: { 'chat-1': { ...binding, processing: false, lifecycleState: 'ready', recentStderr: `diagnostic ${index}` } },
      }))
    }
    const commits = onRender.mock.calls.length
    act(() => root.unmount())
    host.remove()
    expect(commits).toBe(0)
  })

  it.each([false, true])('retains the selected terminal view across remount before native push (storage unavailable: %s)', async (storageUnavailable) => {
    const originalLoad = useAppStore.getState().loadSessionMessages
    const loadSessionMessages = vi.fn()
    useAppStore.setState({ acpBindingBySessionId: {}, loadSessionMessages })
    if (storageUnavailable) Object.defineProperty(globalThis, 'localStorage', { configurable: true, get: () => { throw new Error('Storage unavailable') } })
    const host = document.createElement('div')
    document.body.appendChild(host)
    let root = createRoot(host)
    await act(async () => root.render(<MainPanel />))
    await act(async () => (host.querySelector('[aria-label="Terminal fallback"]') as HTMLButtonElement).click())
    expect(useAppStore.getState().sessions[0].viewMode).toBe('cli')
    act(() => root.unmount())
    root = createRoot(host)
    await act(async () => root.render(<MainPanel />))
    expect(host.querySelector('[aria-label="Terminal fallback"]')?.getAttribute('aria-pressed')).toBe('true')
    loadSessionMessages.mockClear()
    act(() => (host.querySelector('[aria-label="Chat view"]') as HTMLButtonElement).click())
    expect(useAppStore.getState().sessions[0].viewMode).toBe('chat')
    expect(loadSessionMessages.mock.calls).toEqual([['chat-1', true, true]])
    act(() => (host.querySelector('[aria-label="Chat view"]') as HTMLButtonElement).click())
    expect(loadSessionMessages).toHaveBeenCalledTimes(1)
    act(() => root.unmount())
    host.remove()
    useAppStore.setState({ loadSessionMessages: originalLoad })
  })

  it('refreshes native history only when switching from terminal to chat', () => {
    const originalLoad = useAppStore.getState().loadSessionMessages
    const loadSessionMessages = vi.fn()
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) => ({ ...session, viewMode: 'cli' })),
      acpBindingBySessionId: {},
      loadSessionMessages,
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<MainPanel />))
    loadSessionMessages.mockClear()
    const chatButton = host.querySelector('button[aria-label="Chat view"]') as HTMLButtonElement
    act(() => chatButton.click())
    expect(loadSessionMessages.mock.calls).toEqual([['chat-1', true, true]])
    act(() => chatButton.click())
    expect(loadSessionMessages).toHaveBeenCalledTimes(1)
    act(() => root.unmount())
    host.remove()
    useAppStore.setState({ loadSessionMessages: originalLoad })
  })

  it('keeps imported transcripts in chat view and disables terminal fallback', () => {
    writeChatViewMode('chat-1', 'cli')
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) => ({ ...session, viewMode: 'cli', importedReadOnly: true })),
      acpBindingBySessionId: {},
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => root.render(<MainPanel />))

    const terminal = host.querySelector('button[aria-label="Terminal fallback"]') as HTMLButtonElement
    expect(terminal.disabled).toBe(true)
    expect(terminal.getAttribute('aria-pressed')).toBe('false')
    expect(host.querySelector('button[aria-label="Chat view"]')?.getAttribute('aria-pressed')).toBe('true')
    expect(readChatViewMode('chat-1')).toBe('chat')

    act(() => root.unmount())
    host.remove()
  })

  it('keeps the close action outside the chat and terminal view selector', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => root.render(<MainPanel />))

    const selector = host.querySelector('.uam-chat-pane__view-switch') as HTMLElement
    const close = host.querySelector('button[aria-label="Close Gemini Session"]') as HTMLButtonElement
    expect(selector.querySelectorAll('button')).toHaveLength(2)
    expect(selector.contains(close)).toBe(false)
    expect(close.classList.contains('uam-segment-button')).toBe(false)

    act(() => root.unmount())
    host.remove()
  })

  it('keeps a busy CLI running when switching back to chat', async () => {
    const requests: Array<{ action: string; payload?: Record<string, unknown> }> = []
    window.cefQuery = ({ request, onSuccess }) => {
      requests.push(JSON.parse(request) as { action: string; payload?: Record<string, unknown> })
      onSuccess('{}')
    }
    useAppStore.setState((state) => ({
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          lifecycleState: 'ready',
          processing: false,
        },
      },
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => root.render(<MainPanel />))
    const button = (label: string) =>
      host.querySelector(`button[aria-label="${label}"]`) as HTMLButtonElement
    await act(async () => button('Terminal fallback').click())
    expect(readChatViewMode('chat-1')).toBe('cli')
    await act(async () => {
      await vi.dynamicImportSettled()
    })
    act(() => {
      useAppStore.setState({
        cliBindingBySessionId: {
          'chat-1': {
            terminalId: 'term-1',
            boundChatId: 'chat-1',
            running: true,
            lifecycleState: 'busy',
            turnState: 'busy',
            processing: true,
            readySinceLastSelect: false,
            active: true,
            lastError: '',
          },
        },
      })
    })

    expect(button('Chat view').disabled).toBe(false)
    act(() => button('Chat view').click())
    expect(readChatViewMode('chat-1')).toBe('chat')
    expect(button('Terminal fallback').disabled).toBe(false)
    await act(async () => button('Terminal fallback').click())
    expect(readChatViewMode('chat-1')).toBe('cli')
    expect(requests).not.toContainEqual(expect.objectContaining({
      action: 'stopCliTerminal',
      payload: expect.objectContaining({ quit: true }),
    }))

    act(() => root.unmount())
    host.remove()
  })

  it('leaves unassigned grid panes empty', () => {
    useAppStore.setState((state) => ({
      sessions: [1, 2, 3, 4].map((number) => ({
        ...state.sessions[0],
        id: `chat-${number}`,
        name: `Chat ${number}`,
      })),
      messages: { 'chat-1': [], 'chat-2': [], 'chat-3': [], 'chat-4': [] },
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => root.render(<MainPanel />))

    const splitColumns = host.querySelector('button[aria-label="Split active pane into columns"]') as HTMLButtonElement
    act(() => splitColumns.click())
    expect(host.querySelector('[data-testid="chat-grid-2"]')).not.toBeNull()

    const splitRows = host.querySelector('button[aria-label="Split active pane into rows"]') as HTMLButtonElement
    act(() => splitRows.click())
    act(() => splitRows.click())

    expect(host.querySelector('[data-testid="chat-grid-4"]')).not.toBeNull()
    expect(host.querySelectorAll('[data-testid^="chat-pane-"]')).toHaveLength(1)
    expect(host.querySelectorAll('[data-testid="empty-pane"]')).toHaveLength(3)
    const firstPane = host.querySelector('[data-testid="chat-pane-chat-1"]') as HTMLElement
    expect(firstPane.style.getPropertyValue('--pane-color')).toBe('#f97316')
    expect(firstPane.dataset.focused).toBe('false')
    expect(firstPane.dataset.multiPane).toBe('true')
    expect(firstPane.querySelector('[data-testid^="pane-fade-"]')).toBeNull()
    expect(firstPane.style.filter).toBe('')
    expect(firstPane.classList.contains('uam-pane-in')).toBe(false)
    expect(firstPane.style.opacity).toBe('')

    act(() => {
      const paneIds = chatGridLeaves(readChatGridLayout().root).map((leaf) => leaf.id)
      assignChatToPane('chat-2', paneIds[1])
      assignChatToPane('chat-3', paneIds[2])
      assignChatToPane('chat-4', paneIds[3])
      useAppStore.setState({ activeSessionId: 'chat-4' })
    })
    expect(host.querySelectorAll('[data-testid^="chat-pane-"]')).toHaveLength(4)
    const fourthPane = host.querySelector('[data-testid="chat-pane-chat-4"]') as HTMLElement
    expect(fourthPane.dataset.focused).toBe('true')
    expect(firstPane.dataset.focused).toBe('false')
    expect(firstPane.style.filter).toBe('')

    act(() => useAppStore.setState({ activeSessionId: 'chat-1' }))
    expect(host.querySelectorAll('[data-testid="chat-pane-chat-1"]')).toHaveLength(1)
    expect(firstPane.dataset.focused).toBe('true')

    const secondPane = host.querySelector('[data-testid="chat-pane-chat-2"]') as HTMLElement
    const secondPaneChatButton = secondPane.querySelector('button[aria-label="Chat view"]') as HTMLButtonElement
    act(() => secondPaneChatButton.focus())
    expect(useAppStore.getState().activeSessionId).toBe('chat-2')
    expect(secondPane.dataset.focused).toBe('true')
    expect(secondPane.style.filter).toBe('')

    act(() => root.unmount())
    host.remove()
  })

  it('replaces a specific pane when a sidebar chat is dropped on it', () => {
    useAppStore.setState((state) => ({
      sessions: [1, 2, 3].map((number) => ({
        ...state.sessions[0],
        id: `chat-${number}`,
        name: `Chat ${number}`,
      })),
      messages: { 'chat-1': [], 'chat-2': [], 'chat-3': [] },
    }))
    writeChatGridLayout(paneLayout('chat-1', 'chat-2'))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<MainPanel />))

    const target = host.querySelector<HTMLElement>('[data-testid="pane-drop-target-2"]')
    const transfer = {
      dropEffect: 'none',
      types: ['text/x-uam-chat-id'],
      getData: (type: string) => type === 'text/x-uam-chat-id' ? 'chat-3' : '',
    }
    const dragOver = new Event('dragover', { bubbles: true, cancelable: true })
    Object.defineProperty(dragOver, 'dataTransfer', { value: transfer })
    const drop = new Event('drop', { bubbles: true, cancelable: true })
    Object.defineProperty(drop, 'dataTransfer', { value: transfer })

    act(() => target?.dispatchEvent(dragOver))
    expect(dragOver.defaultPrevented).toBe(true)
    expect(transfer.dropEffect).toBe('copy')
    expect(target?.dataset.dropTarget).toBe('true')
    act(() => target?.dispatchEvent(drop))

    expect(host.querySelector('[data-testid="chat-pane-chat-3"]')).toBeTruthy()
    expect(host.querySelector('[data-testid="chat-pane-chat-2"]')).toBeNull()
    expect(chatGridLeaves(readChatGridLayout().root).map((leaf) => leaf.sessionId)).toEqual(['chat-1', 'chat-3'])
    expect(useAppStore.getState().activeSessionId).toBe('chat-3')

    act(() => root.unmount())
    host.remove()
  })

  it('opens a selected sidebar chat in the active pane without disturbing other panes', () => {
    useAppStore.setState((state) => ({
      sessions: [1, 2, 3].map((number) => ({
        ...state.sessions[0],
        id: `chat-${number}`,
        name: `Chat ${number}`,
      })),
      messages: { 'chat-1': [], 'chat-2': [], 'chat-3': [] },
    }))
    writeChatGridLayout(paneLayout('chat-1', 'chat-2'))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<MainPanel />))

    act(() => useAppStore.setState({ activeSessionId: 'chat-3' }))
    expect(chatGridLeaves(readChatGridLayout().root).map((leaf) => leaf.sessionId)).toEqual(['chat-3', 'chat-2'])
    const selectedContent = host.querySelector<HTMLElement>('[data-pane-content="chat-3"]')
    expect(selectedContent?.classList.contains('uam-pane-glide')).toBe(false)

    act(() => useAppStore.setState({ activeSessionId: 'chat-2' }))
    expect(readChatGridLayout().activeLeafId).toBe(chatGridLeaves(readChatGridLayout().root)[1].id)

    act(() => root.unmount())
    host.remove()
  })

  it('glides loaded full-size content, but not an empty chat or terminal', async () => {
    useAppStore.setState((state) => ({
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': { ...state.acpBindingBySessionId['chat-1'], lifecycleState: 'ready', processing: false },
      },
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<MainPanel />))

    const chatContent = host.querySelector<HTMLElement>('[data-pane-content="chat-1"]')
    expect(chatContent?.getAttribute('data-view')).toBe('chat')
    expect(chatContent?.classList.contains('uam-pane-glide')).toBe(false)
    await act(async () => (host.querySelector('button[aria-label="Terminal fallback"]') as HTMLButtonElement).click())
    const terminalContent = host.querySelector<HTMLElement>('[data-pane-content="chat-1"]')
    expect(terminalContent?.getAttribute('data-view')).toBe('cli')
    expect(terminalContent?.classList.contains('uam-pane-glide')).toBe(false)

    act(() => useAppStore.setState({ cliTranscriptBySessionId: { 'chat-1': { terminalId: 'terminal-1', content: 'ready' } } }))
    expect(host.querySelector<HTMLElement>('[data-pane-content="chat-1"]')?.classList.contains('uam-pane-glide')).toBe(false)

    act(() => root.unmount())
    host.remove()
  })

  it('glides a loaded chat selected into a full-size pane', () => {
    useAppStore.setState((state) => ({
      sessions: [...state.sessions, { ...state.sessions[0], id: 'chat-2', name: 'Loaded chat' }],
      messages: {
        'chat-1': [{ id: 'message-1', sessionId: 'chat-1', role: 'assistant', content: 'Existing chat', createdAt: new Date() }],
        'chat-2': [{ id: 'message-2', sessionId: 'chat-2', role: 'user', content: 'Hello', createdAt: new Date() }],
      },
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<MainPanel />))

    act(() => useAppStore.setState({ activeSessionId: 'chat-2' }))
    act(() => useAppStore.getState().unloadSessionMessages('chat-1'))
    expect(host.querySelector<HTMLElement>('[data-pane-content="chat-2"]')?.classList.contains('uam-pane-glide')).toBe(true)

    act(() => root.unmount())
    host.remove()
  })

  it('does not glide loaded chats in a compact split pane', () => {
    useAppStore.setState((state) => ({
      sessions: [...state.sessions, { ...state.sessions[0], id: 'chat-2', name: 'Second chat' }],
      messages: {
        'chat-1': [{ id: 'message-1', sessionId: 'chat-1', role: 'assistant', content: 'First', createdAt: new Date() }],
        'chat-2': [{ id: 'message-2', sessionId: 'chat-2', role: 'assistant', content: 'Second', createdAt: new Date() }],
      },
    }))
    writeChatGridLayout(paneLayout('chat-1', 'chat-2'))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<MainPanel />))

    act(() => useAppStore.setState({ activeSessionId: 'chat-2' }))
    expect(host.querySelector<HTMLElement>('[data-pane-content="chat-2"]')?.classList.contains('uam-pane-glide')).toBe(false)

    act(() => root.unmount())
    host.remove()
  })

  it('does not glide into a blank chat even when the outgoing chat has content', () => {
    useAppStore.setState((state) => ({
      sessions: [...state.sessions, { ...state.sessions[0], id: 'chat-2', name: 'Blank chat' }],
      messages: {
        'chat-1': [{ id: 'message-1', sessionId: 'chat-1', role: 'assistant', content: 'Existing chat', createdAt: new Date() }],
        'chat-2': [{ id: 'message-2', sessionId: 'chat-2', role: 'assistant', content: '   ', createdAt: new Date() }],
      },
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<MainPanel />))

    act(() => useAppStore.setState({ activeSessionId: 'chat-2' }))
    expect(host.querySelector<HTMLElement>('[data-pane-content="chat-2"]')?.classList.contains('uam-pane-glide')).toBe(false)

    act(() => root.unmount())
    host.remove()
  })

  it('opens a selected sidebar chat in the active empty pane before another empty pane', () => {
    useAppStore.setState((state) => ({
      sessions: [1, 2].map((number) => ({
        ...state.sessions[0],
        id: `chat-${number}`,
        name: `Chat ${number}`,
      })),
      messages: { 'chat-1': [], 'chat-2': [] },
    }))
    const layout = paneLayout('chat-1', '', '')
    const leaves = chatGridLeaves(layout.root)
    writeChatGridLayout({ ...layout, activeLeafId: leaves[2].id })
    useAppStore.setState({ activeSessionId: null })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<MainPanel />))

    act(() => useAppStore.setState({ activeSessionId: 'chat-2' }))

    expect(chatGridLeaves(readChatGridLayout().root).map((leaf) => leaf.sessionId)).toEqual(['chat-1', '', 'chat-2'])
    expect(readChatGridLayout().activeLeafId).toBe(leaves[2].id)

    act(() => root.unmount())
    host.remove()
  })

  it('opens a selected sidebar chat in an empty pane before replacing the active chat', () => {
    useAppStore.setState((state) => ({
      sessions: [1, 2, 3].map((number) => ({
        ...state.sessions[0],
        id: `chat-${number}`,
        name: `Chat ${number}`,
      })),
      messages: { 'chat-1': [], 'chat-2': [], 'chat-3': [] },
    }))
    writeChatGridLayout(paneLayout('chat-1', '', 'chat-2'))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<MainPanel />))

    act(() => useAppStore.setState({ activeSessionId: 'chat-3' }))

    expect(chatGridLeaves(readChatGridLayout().root).map((leaf) => leaf.sessionId)).toEqual(['chat-1', 'chat-3', 'chat-2'])

    act(() => root.unmount())
    host.remove()
  })

  it('replaces and deduplicates a visible branch family before using an empty pane', () => {
    useAppStore.setState((state) => ({
      sessions: [
        { ...state.sessions[0], id: 'chat-root', name: 'Root', branchRootChatId: 'chat-root' },
        { ...state.sessions[0], id: 'chat-branch-a', name: 'Branch A', parentChatId: 'chat-root', branchRootChatId: 'chat-root' },
        { ...state.sessions[0], id: 'chat-branch-b', name: 'Branch B', parentChatId: 'chat-root', branchRootChatId: 'chat-root' },
      ],
      messages: { 'chat-root': [], 'chat-branch-a': [], 'chat-branch-b': [] },
      activeSessionId: 'chat-root',
    }))
    writeChatGridLayout(paneLayout('chat-root', 'chat-branch-a', ''))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<MainPanel />))

    act(() => useAppStore.setState({ activeSessionId: 'chat-branch-b' }))

    expect(chatGridLeaves(readChatGridLayout().root).map((leaf) => leaf.sessionId)).toEqual(['chat-branch-b', '', ''])
    expect(host.querySelector('[data-testid="chat-pane-chat-root"]')).toBeNull()
    expect(host.querySelector('[data-testid="chat-pane-chat-branch-a"]')).toBeNull()
    expect(host.querySelector('[data-testid="chat-pane-chat-branch-b"]')).toBeTruthy()

    act(() => root.unmount())
    host.remove()
  })

  it('creates in the chosen empty view and closes that view without deleting chats', () => {
    const layout = paneLayout('chat-1', '', '')
    writeChatGridLayout(layout)
    const setNewChatModalOpen = vi.spyOn(useAppStore.getState(), 'setNewChatModalOpen')
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<MainPanel />))

    const newChat = Array.from(host.querySelectorAll('button')).filter((button) => button.textContent === 'New Chat')[1]
    act(() => newChat.click())
    expect(readChatGridLayout().activeLeafId).toBe(chatGridLeaves(layout.root)[2].id)
    expect(setNewChatModalOpen).toHaveBeenCalledWith(true)
    const close = host.querySelector('button[aria-label="Close view"]') as HTMLButtonElement
    act(() => close.click())
    expect(chatGridLeaves(readChatGridLayout().root)).toHaveLength(2)
    expect(useAppStore.getState().sessions.map((session) => session.id)).toEqual(['chat-1'])
    expect(chatGridLeaves(readChatGridLayout().root).some((leaf) => leaf.sessionId === 'chat-1')).toBe(true)

    act(() => root.unmount())
    host.remove()
  })

  it('closes an active empty leaf from the keyboard-actionable toolbar control', () => {
    const layout = paneLayout('chat-1', '')
    writeChatGridLayout({ ...layout, activeLeafId: chatGridLeaves(layout.root)[1].id })
    useAppStore.setState({ activeSessionId: null })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<MainPanel />))

    const close = host.querySelector('button[aria-label="Close active pane"]') as HTMLButtonElement
    expect(close.disabled).toBe(false)
    act(() => close.click())

    expect(chatGridLeaves(readChatGridLayout().root)).toHaveLength(1)
    expect(chatGridLeaves(readChatGridLayout().root)[0].sessionId).toBe('chat-1')

    act(() => root.unmount())
    host.remove()
  })

  it('closes the focused chat without deleting it and leaves its pane empty', () => {
    useAppStore.setState((state) => ({
      sessions: [1, 2].map((number) => ({
        ...state.sessions[0],
        id: `chat-${number}`,
        name: `Chat ${number}`,
      })),
      messages: { 'chat-1': [], 'chat-2': [] },
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': { ...state.acpBindingBySessionId['chat-1'], processing: false },
      },
    }))
    writeChatGridLayout(paneLayout('chat-1', 'chat-2'))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<MainPanel />))

    act(() => (host.querySelector('button[aria-label="Close Chat 1"]') as HTMLButtonElement).click())

    expect(useAppStore.getState().sessions).toHaveLength(2)
    expect(useAppStore.getState().activeSessionId).toBeNull()
    expect(chatGridLeaves(readChatGridLayout().root).map((leaf) => leaf.sessionId)).toEqual(['', 'chat-2'])
    expect(host.querySelector('[data-testid="chat-grid-2"]')).toBeTruthy()
    expect(host.querySelector('[data-testid="empty-pane"]')).not.toBeNull()
    expect(host.querySelector('[data-testid="chat-pane-chat-1"]')).toBeNull()
    expect(host.querySelector('[data-testid="chat-pane-chat-2"]')).toBeTruthy()
    expect(useAppStore.getState().messages['chat-1']).toBeUndefined()
    expect(useAppStore.getState().messages['chat-2']).toEqual([])

    act(() => root.unmount())
    host.remove()
  })

  it('closes an unfocused chat with one click without reopening it', () => {
    useAppStore.setState((state) => ({
      sessions: [1, 2].map((number) => ({
        ...state.sessions[0],
        id: `chat-${number}`,
        name: `Chat ${number}`,
      })),
      messages: { 'chat-1': [], 'chat-2': [] },
      activeSessionId: 'chat-1',
    }))
    writeChatGridLayout(paneLayout('chat-1', 'chat-2'))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<MainPanel />))

    const close = host.querySelector('button[aria-label="Close Chat 2"]') as HTMLButtonElement
    act(() => {
      close.focus()
      close.click()
    })

    expect(useAppStore.getState().activeSessionId).toBeNull()
    expect(chatGridLeaves(readChatGridLayout().root).map((leaf) => leaf.sessionId)).toEqual(['chat-1', ''])
    expect(host.querySelector('[data-testid="chat-pane-chat-2"]')).toBeNull()

    act(() => root.unmount())
    host.remove()
  })

  it('evicts a closed processing transcript when the turn becomes idle', () => {
    useAppStore.setState((state) => ({
      sessions: [1, 2].map((number) => ({
        ...state.sessions[0],
        id: `chat-${number}`,
        name: `Chat ${number}`,
      })),
      messages: { 'chat-1': [], 'chat-2': [] },
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': { ...state.acpBindingBySessionId['chat-1'], processing: true },
      },
    }))
    writeChatGridLayout(paneLayout('chat-1', 'chat-2'))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<MainPanel />))

    act(() => (host.querySelector('button[aria-label="Close Chat 1"]') as HTMLButtonElement).click())
    expect(useAppStore.getState().messages['chat-1']).toEqual([])

    act(() => useAppStore.setState((state) => ({
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': { ...state.acpBindingBySessionId['chat-1'], processing: false },
      },
    })))
    expect(useAppStore.getState().messages['chat-1']).toBeUndefined()

    act(() => root.unmount())
    host.remove()
  })

  it('preserves persisted pane assignments until the initial CEF state hydrates', () => {
    const hydratedSessions = [1, 2].map((number) => ({
      ...useAppStore.getState().sessions[0],
      id: `chat-${number}`,
      name: `Chat ${number}`,
    }))
    writeChatGridLayout(paneLayout('chat-1', 'chat-2'))
    window.cefQuery = ({ onSuccess }) => onSuccess('{}')
    useAppStore.setState({
      sessions: [],
      activeSessionId: null,
      lastAppliedStateRevision: -1,
      messages: {},
      acpBindingBySessionId: {},
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => root.render(<MainPanel />))
    expect(chatGridLeaves(readChatGridLayout().root).map((leaf) => leaf.sessionId)).toEqual(['chat-1', 'chat-2'])

    act(() => useAppStore.setState({
      sessions: hydratedSessions,
      lastAppliedStateRevision: 1,
      messages: { 'chat-1': [], 'chat-2': [] },
    }))
    expect(chatGridLeaves(readChatGridLayout().root).map((leaf) => leaf.sessionId)).toEqual(['chat-1', 'chat-2'])

    act(() => root.unmount())
    host.remove()
  })

  it('loads a persisted main pane after its same visible owner hydrates without a transcript', async () => {
    // Conditional caller/loader regression, not proof of the installed R9 incident's cause.
    const previous = useAppStore.getState()
    const previousCef = window.cefQuery
    const realLoadSessionMessages = useAppStore.getInitialState().loadSessionMessages
    expect(vi.isMockFunction(realLoadSessionMessages)).toBe(false)
    const owner = {
      ...previous.sessions[0], id: 'r9-cold-main-parent', name: 'Cold main parent',
      providerId: 'gemini-cli', viewMode: 'chat' as const, importedReadOnly: false,
      parentChatId: undefined, branchRootChatId: undefined,
      createdAt: new Date('2026-01-01T00:00:00.000Z'), messageCount: 1,
      messagesDigest: 'r9-owner-summary',
    }
    const requests: Array<{ action: string; payload: Record<string, unknown> }> = []
    const pending: Array<{ settled: boolean; reply: () => void; fail: () => void }> = []
    const historyRequests = () => requests.filter(request => request.action === 'getChatMessages')
    window.cefQuery = ({ request, onSuccess, onFailure }) => {
      const parsed = JSON.parse(request)
      requests.push(parsed)
      if (parsed.action !== 'getChatMessages') { onSuccess('{}'); return }
      const read = {
        settled: false,
        reply: () => {
          if (read.settled) return
          read.settled = true
          onSuccess(JSON.stringify({ chatId: owner.id, startIndex: 0, totalCount: 1,
            messagesDigest: 'r9-authoritative-page', messages: [{ role: 'user',
              content: 'Private saved main history', createdAt: '2026-01-01T00:00:01.000Z' }] }))
        },
        fail: () => {
          if (read.settled) return
          read.settled = true
          onFailure(500, 'Private history fixture cleanup')
        },
      }
      pending.push(read)
    }
    writeChatGridLayout(paneLayout(owner.id))
    useAppStore.setState({
      sessions: [], activeSessionId: null, lastAppliedStateRevision: -1, messages: {},
      historyStartIndexBySessionId: {}, chatSearchNavigation: null,
      chatHistoryErrorBySessionId: {}, chatHistoryReadBySessionId: {}, chatHistoryPageBySessionId: {},
      acpBindingBySessionId: {}, cliBindingBySessionId: {},
      loadSessionMessages: realLoadSessionMessages,
      // Isolate unrelated composer reads; the history action and transport stay real.
      refreshUamAgents: vi.fn(async () => true), getVcsCommitStatus: vi.fn(async () => null),
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    try {
      await act(async () => root.render(<MainPanel />))
      const persistedLeaf = chatGridLeaves(readChatGridLayout().root)[0]
      expect(persistedLeaf.sessionId).toBe(owner.id)
      expect(historyRequests()).toEqual([]) // No history IPC without a session owner.
      expect(useAppStore.getState().chatHistoryReadBySessionId[owner.id]).toBeUndefined()

      await act(async () => useAppStore.setState({
        sessions: [owner], activeSessionId: owner.id, lastAppliedStateRevision: 1,
        // Summary-only owner hydration intentionally supplies NO transcript.
      }))
      expect(chatGridLeaves(readChatGridLayout().root)[0]).toMatchObject({ id: persistedLeaf.id, sessionId: owner.id })
      expect(useAppStore.getState().activeSessionId).toBe(owner.id)
      expect(useAppStore.getState().messages[owner.id]).toBeUndefined()
      expect(historyRequests()).toHaveLength(1)
      expect(historyRequests()[0].payload).toEqual({
        chatId: owner.id, limit: 50, deferToolCallContent: true, messagesDigest: '',
      }) // Ordinary UAM saved-history read; no refreshNative, selection or send.
      expect(useAppStore.getState().chatHistoryReadBySessionId[owner.id]?.birth).toBe(owner.createdAt.getTime())
      expect(useAppStore.getState().loadSessionMessages).toBe(realLoadSessionMessages)

      // Metadata-only updates must not supersede the owned in-flight read.
      const pendingRead = useAppStore.getState().chatHistoryReadBySessionId[owner.id]
      await act(async () => useAppStore.setState({ sessions: [{ ...owner,
        name: 'Renamed while loading', updatedAt: new Date('2026-01-02T00:00:00.000Z'),
        messagesDigest: 'new-summary-only', messageCount: 2,
      }] }))
      expect(historyRequests()).toHaveLength(1)
      expect(useAppStore.getState().chatHistoryReadBySessionId[owner.id]).toBe(pendingRead)

      await act(async () => pending[0].reply())
      expect(historyRequests()).toHaveLength(1)
      expect(useAppStore.getState().messages[owner.id]).toHaveLength(1)
      expect(useAppStore.getState().messages[owner.id][0].content).toBe('Private saved main history')
      expect(useAppStore.getState().historyStartIndexBySessionId[owner.id] ?? 0).toBe(0)
      expect(useAppStore.getState().chatHistoryPageBySessionId[owner.id]?.startIndex).toBe(0)
      expect(useAppStore.getState().messages[owner.id][0].nativeMessageIndex).toBe(0)
      expect(useAppStore.getState().chatHistoryReadBySessionId[owner.id]).toBeUndefined()
      expect(useAppStore.getState().chatHistoryErrorBySessionId[owner.id]).toBeUndefined()
      expect(requests.every(request => request.action === 'getChatMessages' || request.action === 'setVisibleChatIds')).toBe(true)
    } finally {
      await act(async () => { pending.forEach(read => read.fail()); root.unmount() })
      host.remove()
      useAppStore.setState(previous, true)
      window.cefQuery = previousCef
    }
  })

  it('wakes only the newly hydrated visible owner while another pane read is pending', async () => {
    const previous = useAppStore.getState(), previousCef = window.cefQuery
    const realLoad = useAppStore.getInitialState().loadSessionMessages
    const first = { ...previous.sessions[0], id: 'r9-present-pane', name: 'Present pane',
      createdAt: new Date('2026-02-01T00:00:00.000Z'), messageCount: 1, messagesDigest: 'first-summary' }
    const second = { ...first, id: 'r9-late-pane', name: 'Late pane',
      createdAt: new Date('2026-02-02T00:00:00.000Z') }
    const reads: Array<{ chatId: string; done: () => void }> = []
    const actions: string[] = []
    window.cefQuery = ({ request, onSuccess }) => {
      const value = JSON.parse(request); actions.push(value.action)
      if (value.action !== 'getChatMessages') { onSuccess('{}'); return }
      let settled = false
      reads.push({ chatId: value.payload.chatId, done: () => {
        if (settled) return
        settled = true
        onSuccess(JSON.stringify({ chatId: value.payload.chatId, startIndex: 0, totalCount: 1,
          messagesDigest: 'bounded-page-' + value.payload.chatId,
          messages: [{ role: 'user', content: value.payload.chatId, createdAt: '2026-02-01T00:00:01.000Z' }] }))
      } })
    }
    writeChatGridLayout(paneLayout(first.id, second.id))
    useAppStore.setState({ sessions: [first], activeSessionId: first.id,
      lastAppliedStateRevision: -1, messages: {}, historyStartIndexBySessionId: {},
      chatSearchNavigation: null, chatHistoryReadBySessionId: {}, chatHistoryErrorBySessionId: {},
      chatHistoryPageBySessionId: {}, acpBindingBySessionId: {}, cliBindingBySessionId: {},
      loadSessionMessages: realLoad, refreshUamAgents: vi.fn(async () => true),
      getVcsCommitStatus: vi.fn(async () => null) })
    const host = document.createElement('div'); document.body.appendChild(host)
    const root = createRoot(host)
    try {
      await act(async () => root.render(<MainPanel />))
      expect(reads.map(read => read.chatId)).toEqual([first.id])
      const firstPending = useAppStore.getState().chatHistoryReadBySessionId[first.id]
      expect(firstPending?.birth).toBe(first.createdAt.getTime())
      await act(async () => useAppStore.setState({ sessions: [first, second], lastAppliedStateRevision: 1 }))
      expect(chatGridLeaves(readChatGridLayout().root).map(leaf => leaf.sessionId)).toEqual([first.id, second.id])
      expect(reads.map(read => read.chatId)).toEqual([first.id, second.id])
      expect(useAppStore.getState().chatHistoryReadBySessionId[first.id]).toBe(firstPending)
      await act(async () => reads.forEach(read => read.done()))
      expect(useAppStore.getState().messages[first.id][0].content).toBe(first.id)
      expect(useAppStore.getState().messages[second.id][0].content).toBe(second.id)
      expect(useAppStore.getState().activeSessionId).toBe(first.id)
      expect(actions.every(action => action === 'getChatMessages' || action === 'setVisibleChatIds')).toBe(true)
    } finally {
      await act(async () => { root.unmount(); reads.forEach(read => read.done()) })
      host.remove(); useAppStore.setState(previous, true); window.cefQuery = previousCef
    }
  })

  it('refreshes a visible background pane once when its turn completes', () => {
    const loadSessionMessages = vi.fn()
    useAppStore.setState((state) => ({
      sessions: [
        state.sessions[0],
        { ...state.sessions[0], id: 'chat-2', name: 'Background chat' },
      ],
      messages: { 'chat-1': [], 'chat-2': [] },
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-2': { ...state.acpBindingBySessionId['chat-1'], sessionId: 'native-2' },
      },
      loadSessionMessages,
    }))
    writeChatGridLayout(paneLayout('chat-1', 'chat-2'))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<MainPanel />))
    loadSessionMessages.mockClear()

    act(() => useAppStore.setState((state) => ({
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-2': { ...state.acpBindingBySessionId['chat-2'], processing: false, lifecycleState: 'ready' },
      },
    })))

    expect(loadSessionMessages.mock.calls.filter((call) => call[0] === 'chat-2' && call[1] === true)).toEqual([['chat-2', true]])

    act(() => root.unmount())
    host.remove()
  })

  it('commits a visible background pane after an empty turn advances without going idle', () => {
    const loadSessionMessages = vi.fn()
    useAppStore.setState((state) => ({
      sessions: [
        state.sessions[0],
        { ...state.sessions[0], id: 'chat-2', name: 'Background chat' },
      ],
      messages: { 'chat-1': [], 'chat-2': [] },
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-2': {
          ...state.acpBindingBySessionId['chat-1'],
          sessionId: 'native-2',
          turnSerial: 1,
          turnAssistantMessageIndex: -1,
        },
      },
      loadSessionMessages,
    }))
    writeChatGridLayout(paneLayout('chat-1', 'chat-2'))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<MainPanel />))
    loadSessionMessages.mockClear()

    act(() => useAppStore.setState((state) => ({
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-2': {
          ...state.acpBindingBySessionId['chat-2'],
          processing: true,
          turnSerial: 2,
          turnAssistantMessageIndex: -1,
        },
      },
    })))

    expect(loadSessionMessages.mock.calls.filter((call) => call[0] === 'chat-2' && call.length === 1)).toEqual([['chat-2']])

    act(() => root.unmount())
    host.remove()
  })
  it('returns from a temporary side chat in the current pane without stopping either runtime', async () => {
    const parent = useAppStore.getState().sessions[0]
    const side = { ...parent, id: 'side-1', temporaryParentChatId: parent.id, name: 'Side chat' }
    const stop = vi.fn()
    useAppStore.setState({ sessions: [parent, side], activeSessionId: side.id, stopAcpSession: stop })
    writeChatGridLayout(paneLayout(side.id))
    const host = document.createElement('div'); document.body.appendChild(host); const root = createRoot(host)
    await act(async () => root.render(<MainPanel />))
    await act(async () => Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find(button => button.getAttribute('aria-label') === 'Return to main chat')!.click())
    expect(readChatGridLayout().root).toMatchObject({ sessionId: parent.id })
    expect(stop).not.toHaveBeenCalled()
    act(() => root.unmount()); host.remove()
  })

  it('retains a side chat and exposes a failed native dismissal for retry', async () => {
    const parent = useAppStore.getState().sessions[0]
    const side = { ...parent, id: 'side-1', temporaryParentChatId: parent.id, name: 'Side chat' }
    useAppStore.setState({ sessions: [parent, side], activeSessionId: side.id, lastAppliedStateRevision: 1 })
    writeChatGridLayout(paneLayout(side.id))
    const requests: { action: string; payload?: Record<string, unknown> }[] = []
    window.cefQuery = ({ request, onSuccess, onFailure }) => { const parsed = JSON.parse(request); requests.push(parsed); if (parsed.action === 'dismissSideChat') onFailure(500, 'Cleanup could not be saved.'); else onSuccess('{}') }
    const host = document.createElement('div'); document.body.appendChild(host); const root = createRoot(host)
    await act(async () => root.render(<MainPanel />))
    await act(async () => Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find(button => button.getAttribute('aria-label') === 'Dismiss side chat')!.click())
    expect(requests.filter(request => request.action === 'dismissSideChat')).toEqual([expect.objectContaining({ payload: { chatId: side.id } })])
    expect(host.textContent).toContain('Cleanup could not be saved.')
    expect(readChatGridLayout().root).toMatchObject({ sessionId: side.id })
    expect(Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find(button => button.getAttribute('aria-label') === 'Dismiss side chat')?.disabled).toBe(false)
    act(() => root.unmount()); host.remove(); delete window.cefQuery
  })

  it('resumes the live side chat locally instead of creating another one', async () => {
    const parent = useAppStore.getState().sessions[0]
    const side = { ...parent, id: 'side-1', temporaryParentChatId: parent.id, name: 'Side chat' }
    useAppStore.setState({ sessions: [parent, side], activeSessionId: parent.id })
    writeChatGridLayout(paneLayout(parent.id))
    const requests: string[] = []
    window.cefQuery = ({ request, onSuccess }) => { requests.push(JSON.parse(request).action); onSuccess('{}') }
    const host = document.createElement('div'); document.body.appendChild(host); const root = createRoot(host)
    await act(async () => root.render(<MainPanel />))
    await act(async () => Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find(button => button.textContent === 'Resume side chat')!.click())
    expect(requests).not.toContain('createSideChat')
    expect(readChatGridLayout().root).toMatchObject({ sessionId: side.id })
    act(() => root.unmount()); host.remove(); delete window.cefQuery
  })

  it('reports all desktop pane chat IDs for safe native transcript retention', async () => {
    const first = useAppStore.getState().sessions[0]
    useAppStore.setState({ sessions: [first, { ...first, id: 'chat-2', name: 'Second' }], messages: { 'chat-1': [], 'chat-2': [] } })
    writeChatGridLayout(paneLayout('chat-1', 'chat-2'))
    const requests: Array<{ action: string; payload?: { chatIds?: string[] } }> = []
    window.cefQuery = ({ request, onSuccess }) => { requests.push(JSON.parse(request)); onSuccess('{}'); return 1 }
    const host = document.createElement('div')
    const root = createRoot(host)
    try {
      await act(async () => { root.render(<MainPanel />) })
      expect(requests.filter((request) => request.action === 'setVisibleChatIds').at(-1)?.payload?.chatIds).toEqual(['chat-1', 'chat-2'])
      await act(async () => { writeChatGridLayout(paneLayout('chat-1')) })
      expect(requests.filter((request) => request.action === 'setVisibleChatIds').at(-1)?.payload?.chatIds).toEqual(['chat-1'])
    } finally {
      await act(async () => root.unmount())
      delete window.cefQuery
    }
  })

  it.each([false, true])('retries cold native history release after idle stop unless reopened: %s', async (reopened) => {
    const initial = useAppStore.getState()
    useAppStore.setState({
      sessions: [initial.sessions[0], { ...initial.sessions[0], id: 'chat-2', name: 'Second' }],
      messages: { 'chat-1': [], 'chat-2': [] },
      acpBindingBySessionId: { 'chat-1': { ...initial.acpBindingBySessionId['chat-1'], processing: false, running: true, lifecycleState: 'ready' } },
    })
    writeChatGridLayout(paneLayout('chat-1'))
    const requests: Array<{ action: string; payload?: { chatId?: string } }> = []
    window.cefQuery = ({ request, onSuccess }) => { requests.push(JSON.parse(request)); onSuccess('{}'); return 1 }
    const host = document.createElement('div')
    const root = createRoot(host)
    try {
      await act(async () => { root.render(<MainPanel />) })
      await act(async () => { useAppStore.setState({ activeSessionId: 'chat-2' }); writeChatGridLayout(paneLayout('chat-2')) })
      expect(useAppStore.getState().messages['chat-1']).toBeUndefined()
      if (reopened) await act(async () => { useAppStore.setState({ activeSessionId: 'chat-1' }); writeChatGridLayout(paneLayout('chat-1')) })
      requests.length = 0
      await act(async () => { useAppStore.setState((state) => ({ acpBindingBySessionId: { ...state.acpBindingBySessionId, 'chat-1': { ...state.acpBindingBySessionId['chat-1'], running: false, processing: false, lifecycleState: 'stopped' } } })) })
      expect(requests.filter((request) => request.action === 'releaseChatMessages')).toEqual(reopened ? [] : [{ action: 'releaseChatMessages', payload: { chatId: 'chat-1' }, requestId: expect.any(String) }])
      if (!reopened) expect(useAppStore.getState().messages['chat-1']).toBeUndefined()
      expect(requests.filter((request) => request.action === 'getChatMessages' && request.payload?.chatId === 'chat-1')).toHaveLength(0)
    } finally {
      await act(async () => root.unmount())
      delete window.cefQuery
    }
  })

  it.each([
    ['acp', false, true], ['acp', true, true],
    ['cli', false, true], ['cli', true, true],
    ['acp', false, false], ['acp', true, false],
  ] as const)('preserves a reopened transcript across idle selection: %s side=%s sameUpdate=%s', (runtime, side, sameUpdate) => {
    const saved = [{ id: 'earlier-answer', sessionId: 'chat-2', role: 'assistant' as const, content: 'Earlier assistant remains.', thoughts: 'Earlier work trace remains.', createdAt: new Date(0) }]
    const concurrent = [{ ...saved[0], id: 'concurrent-answer', sessionId: 'chat-1', content: 'Concurrent assistant remains.', thoughts: 'Concurrent work trace remains.' }]
    useAppStore.setState((state) => ({
      sessions: [state.sessions[0], { ...state.sessions[0], id: 'chat-2', name: 'Second', ...(side ? { temporaryParentChatId: 'chat-1' } : {}) }],
      activeSessionId: 'chat-1',
      messages: { 'chat-1': concurrent, 'chat-2': saved },
      workingDisplayMode: 'compact', expandWorkTraces: true,
      acpBindingBySessionId: {
        'chat-1': { ...state.acpBindingBySessionId['chat-1'], processing: true },
        ...(runtime === 'acp' ? { 'chat-2': { ...state.acpBindingBySessionId['chat-1'], processing: true } } : {}),
      },
      cliBindingBySessionId: runtime === 'cli' ? { 'chat-2': { terminalId: 'term-2', boundChatId: 'chat-2', running: true, lifecycleState: 'busy', turnState: 'busy', processing: true, readySinceLastSelect: false, active: true, lastError: '' } } : {},
    }))
    writeChatGridLayout(paneLayout('chat-1', 'chat-2'))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    try {
      act(() => root.render(<MainPanel />))
      act(() => (host.querySelector('button[aria-label="Close Second"]') as HTMLButtonElement).click())
      expect(useAppStore.getState().messages['chat-2']).toBe(saved)
      if (!sameUpdate) act(() => useAppStore.setState({ activeSessionId: 'chat-2' }))
      act(() => useAppStore.setState((state) => ({
        activeSessionId: 'chat-2',
        ...(runtime === 'acp'
          ? { acpBindingBySessionId: { ...state.acpBindingBySessionId, 'chat-2': { ...state.acpBindingBySessionId['chat-2'], processing: false, lifecycleState: 'ready' } } }
          : { cliBindingBySessionId: { ...state.cliBindingBySessionId, 'chat-2': { ...state.cliBindingBySessionId['chat-2'], processing: false, lifecycleState: 'ready', turnState: 'idle' } } }),
      })))
      expect(useAppStore.getState().messages['chat-1']).toBe(concurrent)
      expect(useAppStore.getState().messages['chat-2']).toBe(saved)
      const pane = host.querySelector('[data-testid="chat-pane-chat-2"]')
      expect(pane?.textContent).toContain('Earlier assistant remains.')
      expect(pane?.textContent).toContain('Earlier work trace remains.')
    } finally {
      act(() => root.unmount())
      host.remove()
    }
  })

  it('retains hidden CLI history while processing and evicts it after idle', () => {
    const saved = [{ id: 'answer', sessionId: 'chat-2', role: 'assistant' as const, content: 'Hidden answer.', createdAt: new Date(0) }]
    useAppStore.setState((state) => ({
      sessions: [state.sessions[0], { ...state.sessions[0], id: 'chat-2', name: 'Second' }],
      messages: { 'chat-1': [], 'chat-2': saved },
      cliBindingBySessionId: { 'chat-2': { terminalId: 'term-2', boundChatId: 'chat-2', running: true, lifecycleState: 'busy', turnState: 'busy', processing: true, readySinceLastSelect: false, active: true, lastError: '' } },
    }))
    writeChatGridLayout(paneLayout('chat-1', 'chat-2'))
    const host = document.createElement('div')
    const root = createRoot(host)
    try {
      act(() => root.render(<MainPanel />))
      act(() => (host.querySelector('button[aria-label="Close Second"]') as HTMLButtonElement).click())
      act(() => useAppStore.setState({ activeSessionId: 'chat-1' }))
      expect(useAppStore.getState().messages['chat-2']).toBe(saved)
      act(() => useAppStore.setState((state) => ({ cliBindingBySessionId: { ...state.cliBindingBySessionId, 'chat-2': { ...state.cliBindingBySessionId['chat-2'], processing: false, lifecycleState: 'ready', turnState: 'idle' } } })))
      expect(useAppStore.getState().messages['chat-2']).toBeUndefined()
    } finally {
      act(() => root.unmount())
    }
  })

  it.each([false, true])('releases hidden native history unless reselected in the idle update: %s', async (reopened) => {
    const initial = useAppStore.getState()
    useAppStore.setState({
      sessions: [initial.sessions[0], { ...initial.sessions[0], id: 'chat-2', name: 'Second' }],
      acpBindingBySessionId: { 'chat-1': { ...initial.acpBindingBySessionId['chat-1'], processing: false, running: true, lifecycleState: 'ready' } },
    })
    writeChatGridLayout(paneLayout('chat-1'))
    const requests: Array<{ action: string; payload?: { chatId?: string } }> = []
    window.cefQuery = ({ request, onSuccess }) => { requests.push(JSON.parse(request)); onSuccess('{}'); return 1 }
    const host = document.createElement('div')
    const root = createRoot(host)
    try {
      await act(async () => root.render(<MainPanel />))
      await act(async () => { useAppStore.setState({ activeSessionId: 'chat-2' }); writeChatGridLayout(paneLayout('chat-2')) })
      requests.length = 0
      await act(async () => useAppStore.setState((state) => ({
        activeSessionId: reopened ? 'chat-1' : 'chat-2',
        acpBindingBySessionId: { ...state.acpBindingBySessionId, 'chat-1': { ...state.acpBindingBySessionId['chat-1'], running: false, processing: false, lifecycleState: 'stopped' } },
      })))
      expect(requests.filter((request) => request.action === 'releaseChatMessages')).toEqual(reopened ? [] : [{ action: 'releaseChatMessages', payload: { chatId: 'chat-1' }, requestId: expect.any(String) }])
    } finally {
      await act(async () => root.unmount())
      delete window.cefQuery
    }
  })

  it('opens a validated native child in its parent pane while retaining the unrelated pane', async () => {
    const parent = { ...useAppStore.getState().sessions[0], providerId: 'gemini-cli' }
    const other = { ...parent, id: 'unrelated-pane', name: 'Unrelated chat' }
    const child = { ...parent, id: 'native-child-pane', name: 'Native child', importedReadOnly: true, parentChatId: parent.id }
    rememberSubagentParent(parent, child.id)
    useAppStore.setState({ sessions: [parent, other, child], activeSessionId: other.id, messages: { [parent.id]: [], [other.id]: [], [child.id]: [] }, acpBindingBySessionId: {} })
    const layout = paneLayout(parent.id, other.id)
    writeChatGridLayout({ ...layout, activeLeafId: chatGridLeaves(layout.root)[1].id })
    const host = document.createElement('div'); const root = createRoot(host)
    try {
      await act(async () => root.render(<MainPanel />))
      await act(async () => useAppStore.getState().setActiveSession(child.id))
      expect(chatGridLeaves(readChatGridLayout().root).map(leaf => leaf.sessionId)).toEqual([parent.id, other.id])
      expect(host.querySelector('[data-testid="subagent-pane-chat-1"]')).not.toBeNull()
      expect(host.querySelector('[aria-label="Subagent lineage"]')?.textContent).toContain('Subagent of Gemini Session')
    } finally { await act(async () => root.unmount()); host.remove() }
  })

})
