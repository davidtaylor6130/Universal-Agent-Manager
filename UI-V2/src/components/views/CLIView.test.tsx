import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { CLIView } from './CLIView'
import { useAppStore } from '../../store/useAppStore'

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

type TestWindow = Window & typeof globalThis & {
  cefQuery?: Window['cefQuery']
}

class TestResizeObserver {
  observe() {}
  disconnect() {}
}

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

function resetStore() {
  useAppStore.setState({
    folders: [],
    sessions: [],
    activeSessionId: null,
    messages: {},
    providers: [],
    cliBindingBySessionId: {},
    acpBindingBySessionId: {},
    cliTranscriptBySessionId: {},
    cliDebugState: null,
    streamingMessageId: null,
    pushChannelStatus: 'connected',
    pushChannelError: '',
    lastPushAtMs: null,
  })
}

describe('CLIView', () => {
  beforeEach(() => {
    vi.unstubAllGlobals()
    vi.restoreAllMocks()
    vi.stubGlobal('ResizeObserver', TestResizeObserver)
    vi.stubGlobal('requestAnimationFrame', (callback: FrameRequestCallback) => {
      callback(0)
      return 1
    })
    vi.stubGlobal('cancelAnimationFrame', vi.fn())
    vi.spyOn(console, 'error').mockImplementation(() => {})
    resetStore()
    delete (window as TestWindow).cefQuery
  })

  it('detaches a terminal returned after unmount without writing stale binding state', async () => {
    const requests: Array<{ action: string; payload?: Record<string, unknown> }> = []
    let resolveStart: ((response: string) => void) | null = null
    ;(window as TestWindow).cefQuery = ({ request, onSuccess }) => {
      const parsed = JSON.parse(request)
      requests.push(parsed)
      if (parsed.action === 'startCliTerminal') {
        resolveStart = onSuccess
        return
      }
      onSuccess('{}')
    }

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    useAppStore.setState({
      providers: [
        { id: 'gemini-cli', name: 'Gemini CLI', shortName: 'Gemini', color: '#8ab4ff', description: '', outputMode: 'cli', supportsCli: true, supportsStructured: true, structuredProtocol: 'gemini-acp' },
      ],
    })
    const session = {
      id: 'chat-1',
      name: 'Gemini Session',
      providerId: 'gemini-cli',
      viewMode: 'cli' as const,
      folderId: null,
      createdAt: new Date('2026-01-01T00:00:00.000Z'),
      updatedAt: new Date('2026-01-01T00:00:00.000Z'),
    }

    await act(async () => {
      root.render(<CLIView session={session} />)
    })
    expect(resolveStart).toBeTruthy()

    await act(async () => {
      root.unmount()
    })

    await act(async () => {
      resolveStart?.(JSON.stringify({
        terminalId: 'term-late',
        sourceChatId: 'chat-1',
        running: true,
        lifecycleState: 'idle',
        turnState: 'idle',
        lastError: '',
      }))
      await new Promise((resolve) => setTimeout(resolve, 0))
    })

    const stopRequests = requests.filter((request) => request.action === 'stopCliTerminal')
    expect(stopRequests.length).toBeGreaterThanOrEqual(2)
    expect(stopRequests[stopRequests.length - 1]?.payload).toMatchObject({
      chatId: 'chat-1',
      terminalId: 'term-late',
    })
    expect(useAppStore.getState().cliBindingBySessionId['chat-1']).toBeUndefined()

    host.remove()
  })

  it('shows an unsupported provider warning without starting a terminal', async () => {
    useAppStore.setState({
      providers: [
        { id: 'gemini-cli', name: 'Gemini CLI', shortName: 'Gemini', color: '#8ab4ff', description: '', outputMode: 'cli', supportsCli: true, supportsStructured: true, structuredProtocol: 'gemini-acp' },
      ],
    })

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    const session = {
      id: 'chat-1',
      name: 'Legacy Codex Session',
      providerId: 'codex-cli',
      viewMode: 'cli' as const,
      folderId: null,
      createdAt: new Date('2026-01-01T00:00:00.000Z'),
      updatedAt: new Date('2026-01-01T00:00:00.000Z'),
    }

    await act(async () => {
      root.render(<CLIView session={session} />)
    })

    expect(host.textContent).toContain("Provider 'codex-cli' is not supported in this build.")
    expect(useAppStore.getState().cliBindingBySessionId['chat-1']).toBeUndefined()
    expect(host.querySelector('.xterm')).toBeNull()

    act(() => {
      root.unmount()
    })
    host.remove()
  })
})
