import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { beforeEach, describe, expect, it, vi } from 'vitest'

vi.mock('@xterm/xterm', () => ({
  Terminal: class {
    rows = 24
    cols = 80
    loadAddon() {}
    open() {}
    write() {}
    writeln() {}
    dispose() {}
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

    expect(host.querySelectorAll('.uam-layout-button')).toHaveLength(3)
    expect(host.querySelector('[data-testid="provider-badge-chat-1"] .lucide-chevron-down')).toBeNull()

    const chatButton = () => host.querySelector('button[aria-label="Chat view"]') as HTMLButtonElement
    const cliButton = () => host.querySelector('button[aria-label="Terminal fallback"]') as HTMLButtonElement

    expect(chatButton().textContent).toBe('')
    expect(cliButton().textContent).toBe('')
    expect(host.querySelector('[data-testid="chat-workspace-chat-1"]')?.textContent).toBe('project')
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
    expect(cliButton().disabled).toBe(true)

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('opens a terminal-first session in the terminal fallback view', async () => {
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) => ({ ...session, viewMode: 'cli' })),
      acpBindingBySessionId: {},
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    await act(async () => {
      root.render(<MainPanel />)
      await Promise.resolve()
    })

    expect(host.querySelector('button[aria-label="Terminal fallback"]')?.getAttribute('aria-pressed')).toBe('true')
    expect(host.textContent).toContain('Loading terminal')

    act(() => root.unmount())
    host.remove()
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
    expect(Array.from(host.querySelectorAll('button')).filter((button) => button.textContent?.includes('Drag a chat here or select one'))).toHaveLength(3)
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
    expect(host.querySelector('[data-testid="chat-pane-chat-3"]')).toBeTruthy()

    act(() => useAppStore.setState({ activeSessionId: 'chat-2' }))
    expect(readChatGridLayout().activeLeafId).toBe(chatGridLeaves(readChatGridLayout().root)[1].id)

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
    expect(Array.from(host.querySelectorAll('button')).some((button) => button.textContent?.includes('Drag a chat here or select one'))).toBe(true)
    expect(host.querySelector('[data-testid="chat-pane-chat-1"]')).toBeNull()
    expect(host.querySelector('[data-testid="chat-pane-chat-2"]')).toBeTruthy()

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
})
