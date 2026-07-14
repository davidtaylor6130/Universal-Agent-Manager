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
import { assignChatToPane } from '../../utils/chatGridStorage'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

describe('MainPanel', () => {
  beforeEach(() => {
    vi.restoreAllMocks()
    vi.stubGlobal('ResizeObserver', class {
      observe() {}
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

    const cliButton = () =>
      Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'CLI') as HTMLButtonElement

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
      Array.from(host.querySelectorAll('button')).find((candidate) => candidate.textContent === label) as HTMLButtonElement
    await act(async () => button('CLI').click())
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

    expect(button('Chat').disabled).toBe(false)
    act(() => button('Chat').click())
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
    expect(host.querySelector('[data-testid="pane-fade-1"]')).toBeNull()

    const twoChats = host.querySelector('button[aria-label="Show two chats"]') as HTMLButtonElement
    act(() => twoChats.click())
    expect(host.querySelector('[data-testid="chat-grid-2"]')).not.toBeNull()

    const fourChats = host.querySelector('button[aria-label="Show four chats"]') as HTMLButtonElement
    act(() => fourChats.click())

    expect(host.querySelector('[data-testid="chat-grid-4"]')).not.toBeNull()
    expect(host.querySelectorAll('[data-testid^="chat-pane-"]')).toHaveLength(1)
    expect(Array.from(host.querySelectorAll('button')).filter((button) => button.textContent?.includes('Select Chat'))).toHaveLength(3)
    const firstPane = host.querySelector('[data-testid="chat-pane-chat-1"]') as HTMLElement
    expect(firstPane.style.getPropertyValue('--accent')).toBe('#f97316')
    expect(firstPane.style.border).toBe('1px solid transparent')
    const firstFade = firstPane.querySelector('[data-testid="pane-fade-1"]') as HTMLElement
    expect(firstFade.style.boxShadow).toContain('inset 0 0 12px')
    expect(firstFade.style.boxShadow).toContain('80%')
    expect(firstFade.style.zIndex).toBe('20')
    expect(firstFade.style.pointerEvents).toBe('none')
    expect(firstPane.style.filter).toBe('none')
    expect(firstPane.style.transition).toContain('140ms')

    act(() => {
      assignChatToPane('chat-2', 1)
      assignChatToPane('chat-3', 2)
      assignChatToPane('chat-4', 3)
      useAppStore.setState({ activeSessionId: 'chat-4' })
    })
    expect(host.querySelectorAll('[data-testid^="chat-pane-"]')).toHaveLength(4)
    const fourthPane = host.querySelector('[data-testid="chat-pane-chat-4"]') as HTMLElement
    expect(fourthPane.style.border).toBe('1px solid transparent')
    expect((fourthPane.querySelector('[data-testid="pane-fade-4"]') as HTMLElement).style.boxShadow).toContain('inset 0 0 12px')
    expect(firstFade.style.boxShadow).toContain('inset 0 0 9px')
    expect(firstFade.style.boxShadow).toContain('55%')
    expect(firstPane.style.filter).toContain('brightness(0.82)')

    act(() => useAppStore.setState({ activeSessionId: 'chat-1' }))
    expect(host.querySelectorAll('[data-testid="chat-pane-chat-1"]')).toHaveLength(1)
    expect(firstPane.dataset.focused).toBe('true')

    const secondPane = host.querySelector('[data-testid="chat-pane-chat-2"]') as HTMLElement
    act(() => secondPane.dispatchEvent(new MouseEvent('mousedown', { bubbles: true })))
    expect(useAppStore.getState().activeSessionId).toBe('chat-2')
    expect(secondPane.style.border).toBe('1px solid transparent')
    expect(secondPane.style.filter).toBe('none')

    act(() => root.unmount())
    host.remove()
  })
})
