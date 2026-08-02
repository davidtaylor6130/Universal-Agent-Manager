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
import { assignChatToPane, readChatGridLayout, writeChatGridLayout } from '../../utils/chatGridStorage'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

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

  it('quits a busy CLI when switching back to chat', async () => {
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
    expect(requests).toContainEqual({
      action: 'stopCliTerminal',
      payload: { chatId: 'chat-1', terminalId: 'term-1', quit: true },
      requestId: expect.any(String),
    })

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

    const twoChats = host.querySelector('button[aria-label="Show two chats"]') as HTMLButtonElement
    act(() => twoChats.click())
    expect(host.querySelector('[data-testid="chat-grid-2"]')).not.toBeNull()

    const fourChats = host.querySelector('button[aria-label="Show four chats"]') as HTMLButtonElement
    act(() => fourChats.click())

    expect(host.querySelector('[data-testid="chat-grid-4"]')).not.toBeNull()
    expect(host.querySelectorAll('[data-testid^="chat-pane-"]')).toHaveLength(1)
    expect(Array.from(host.querySelectorAll('button')).filter((button) => button.textContent?.includes('Select chat'))).toHaveLength(3)
    const firstPane = host.querySelector('[data-testid="chat-pane-chat-1"]') as HTMLElement
    expect(firstPane.style.getPropertyValue('--pane-color')).toBe('#f97316')
    expect(firstPane.dataset.focused).toBe('true')
    expect(firstPane.dataset.multiPane).toBe('true')
    expect(firstPane.querySelector('[data-testid^="pane-fade-"]')).toBeNull()
    expect(firstPane.style.filter).toBe('')
    expect(firstPane.classList.contains('uam-pane-in')).toBe(false)
    expect(firstPane.style.opacity).toBe('')

    act(() => {
      assignChatToPane('chat-2', 1)
      assignChatToPane('chat-3', 2)
      assignChatToPane('chat-4', 3)
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
    writeChatGridLayout({ paneCount: 2, activePane: 0, sessionIds: ['chat-1', 'chat-2'], columnSizes: [50, 50], rowSizes: [50, 50] })
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
    expect(readChatGridLayout()).toMatchObject({ activePane: 1, sessionIds: ['chat-1', 'chat-3'] })
    expect(useAppStore.getState().activeSessionId).toBe('chat-3')

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
    writeChatGridLayout({ paneCount: 2, activePane: 0, sessionIds: ['chat-1', 'chat-2'], columnSizes: [50, 50], rowSizes: [50, 50] })
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

    expect(loadSessionMessages).toHaveBeenCalledTimes(1)
    expect(loadSessionMessages).toHaveBeenCalledWith('chat-2', true)

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
    writeChatGridLayout({ paneCount: 2, activePane: 0, sessionIds: ['chat-1', 'chat-2'], columnSizes: [50, 50], rowSizes: [50, 50] })
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

    expect(loadSessionMessages).toHaveBeenCalledTimes(1)
    expect(loadSessionMessages).toHaveBeenCalledWith('chat-2')

    act(() => root.unmount())
    host.remove()
  })
})
