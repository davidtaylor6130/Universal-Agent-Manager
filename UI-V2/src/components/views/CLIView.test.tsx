import { act, Profiler } from 'react'
import { createRoot } from 'react-dom/client'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { CLIView } from './CLIView'
import { useAppStore } from '../../store/useAppStore'

const xtermState = vi.hoisted(() => ({
  resizeByInstance: [] as Array<(size: { rows: number; cols: number }) => void>,
  resizeObservers: [] as Array<() => void>,
  constructCount: 0,
  disposeCount: 0,
  inputByInstance: [] as Array<(data: string) => void>,
  writesByInstance: [] as Array<Array<string | Uint8Array>>,
  optionsByInstance: [] as Array<{ theme?: { background?: string } }>,
}))

vi.mock('@xterm/xterm', () => ({
  Terminal: class {
    rows = 24
    cols = 80
    index: number
    options: { theme?: { background?: string } }
    constructor(options: { theme?: { background?: string } }) {
      this.index = xtermState.constructCount++
      xtermState.writesByInstance[this.index] = []
      this.options = options
      xtermState.optionsByInstance[this.index] = options
    }
    loadAddon() {}
    open() {}
    write(data: string | Uint8Array) {
      xtermState.writesByInstance[this.index].push(data)
    }
    writeln() {}
    dispose() {
      xtermState.disposeCount += 1
    }
    onResize(callback: (size: { rows: number; cols: number }) => void) {
      xtermState.resizeByInstance[this.index] = callback
      return { dispose() {} }
    }
    onData(callback: (data: string) => void) {
      xtermState.inputByInstance[this.index] = callback
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
  constructor(callback: () => void) { xtermState.resizeObservers.push(callback) }
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
    cliVersionManager: { providers: [] },
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
    xtermState.resizeByInstance = []
    xtermState.resizeObservers = []
    xtermState.constructCount = 0
    xtermState.disposeCount = 0
    xtermState.inputByInstance = []
    xtermState.writesByInstance = []
    xtermState.optionsByInstance = []
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

  it('scopes late cleanup to its old attachment after a replacement view mounts', async () => {
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

    const completeOldStart = resolveStart
    const replacement = createRoot(host)
    await act(async () => { replacement.render(<CLIView session={session} />) })
    const startRequests = requests.filter((request) => request.action === 'startCliTerminal')
    expect(startRequests[1]?.payload?.attachmentId).not.toBe(startRequests[0]?.payload?.attachmentId)
    await act(async () => {
      resolveStart?.(JSON.stringify({ terminalId: 'term-new', sourceChatId: 'chat-1', running: true, lifecycleState: 'idle' }))
      await Promise.resolve()
    })
    await act(async () => {
      completeOldStart?.(JSON.stringify({
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
    const attachmentId = requests.find((request) => request.action === 'startCliTerminal')?.payload?.attachmentId
    expect(attachmentId).toEqual(expect.any(String))
    expect(attachmentId).not.toBe('')
    expect(stopRequests.every((request) => request.payload?.attachmentId === attachmentId)).toBe(true)
    expect(stopRequests.length).toBeGreaterThanOrEqual(2)
    expect(stopRequests[stopRequests.length - 1]?.payload).toMatchObject({
      chatId: 'chat-1',
      terminalId: 'term-late',
    })
    expect(useAppStore.getState().cliBindingBySessionId['chat-1']?.terminalId).toBe('term-new')
    await act(async () => { replacement.unmount() })
    expect(requests.filter((request) => request.action === 'stopCliTerminal').at(-1)?.payload?.attachmentId)
      .toBe(startRequests[1]?.payload?.attachmentId)

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

  it('retries Copilot terminal startup when its compatibility check finishes', async () => {
    const requests: Array<{ action: string }> = []
    ;(window as TestWindow).cefQuery = ({ request, onSuccess, onFailure }) => {
      const parsed = JSON.parse(request)
      requests.push(parsed)
      if (parsed.action === 'startCliTerminal') {
        if (requests.filter((item) => item.action === 'startCliTerminal').length === 1) {
          onFailure(1, 'Checking GitHub Copilot CLI compatibility. Try again in a moment.')
        } else {
          onSuccess(JSON.stringify({
            terminalId: 'term-copilot',
            sourceChatId: 'chat-copilot',
            running: true,
            lifecycleState: 'idle',
            turnState: 'idle',
            lastError: '',
          }))
        }
      } else {
        onSuccess('{}')
      }
    }
    const copilotVersion = {
      providerId: 'copilot-cli',
      installedVersion: '',
      selectedVersion: 'latest',
      availableVersions: [],
      preferredVersion: 'latest',
      status: 'checking' as const,
      message: '',
      running: true,
      installMethod: 'npm' as const,
      lastInstallStatus: 'none' as const,
      lastCommand: 'copilot --version',
      lastOutput: '',
    }
    useAppStore.setState({
      providers: [
        { id: 'copilot-cli', name: 'GitHub Copilot CLI', shortName: 'Copilot', color: '#22c55e', description: '', outputMode: 'cli', supportsCli: true, supportsStructured: true, structuredProtocol: 'copilot-acp' },
      ],
      cliVersionManager: { providers: [copilotVersion] },
    })
    const session = {
      id: 'chat-copilot',
      name: 'Copilot Session',
      providerId: 'copilot-cli',
      viewMode: 'cli' as const,
      folderId: null,
      createdAt: new Date(),
      updatedAt: new Date(),
    }
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    await act(async () => {
      root.render(<CLIView session={session} />)
      await new Promise((resolve) => setTimeout(resolve, 0))
    })
    expect(requests.filter((request) => request.action === 'startCliTerminal')).toHaveLength(1)
    const startsBeforeCompatibilityFinished = requests.filter((request) => request.action === 'startCliTerminal').length

    await act(async () => {
      useAppStore.setState({
        cliVersionManager: {
          providers: [{ ...copilotVersion, installedVersion: '1.0.75', status: 'verified', running: false }],
        },
      })
      await new Promise((resolve) => setTimeout(resolve, 0))
    })

    expect(requests.filter((request) => request.action === 'startCliTerminal').length).toBeGreaterThan(startsBeforeCompatibilityFinished)
    expect(useAppStore.getState().cliBindingBySessionId['chat-copilot']?.running).toBe(true)

    act(() => root.unmount())
    host.remove()
  })

  it('retries a transient terminal startup failure without remounting', async () => {
    let startCalls = 0
    ;(window as TestWindow).cefQuery = ({ request, onSuccess, onFailure }) => {
      const parsed = JSON.parse(request)
      if (parsed.action !== 'startCliTerminal') {
        onSuccess('{}')
        return
      }
      startCalls += 1
      if (startCalls === 1) onFailure(500, 'Provider failed to start.')
      else onSuccess(JSON.stringify({ terminalId: 'term-2', sourceChatId: 'chat-1', running: true, lifecycleState: 'idle', turnState: 'idle', lastError: '' }))
    }
    useAppStore.setState({ providers: [{ id: 'gemini-cli', name: 'Gemini CLI', shortName: 'Gemini', color: '#8ab4ff', description: '', outputMode: 'cli', supportsCli: true, supportsStructured: true, structuredProtocol: 'gemini-acp' }] })
    const session = { id: 'chat-1', name: 'Gemini Session', providerId: 'gemini-cli', viewMode: 'cli' as const, folderId: null, createdAt: new Date(), updatedAt: new Date() }
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    await act(async () => { root.render(<CLIView session={session} />); await new Promise((resolve) => setTimeout(resolve, 0)) })

    expect(host.textContent).toContain('Provider failed to start.')
    const retry = host.querySelector<HTMLButtonElement>('button[aria-label="Retry terminal start"]')
    expect(retry).toBeTruthy()
    await act(async () => { retry?.click(); await new Promise((resolve) => setTimeout(resolve, 0)) })
    expect(startCalls).toBe(2)
    expect(useAppStore.getState().cliBindingBySessionId['chat-1']).toMatchObject({ running: true, terminalId: 'term-2' })

    act(() => root.unmount())
    host.remove()
  })

  it('offers provider check and settings beside terminal startup retry', async () => {
    const refreshCliProviderVersion = vi.fn().mockResolvedValue(true)
    const setSettingsOpen = vi.fn()
    useAppStore.setState({
      refreshCliProviderVersion,
      setSettingsOpen,
      providers: [{ id: 'gemini-cli', name: 'Gemini CLI', shortName: 'Gemini', color: '#8ab4ff', description: '', outputMode: 'cli', supportsCli: true, supportsStructured: true, structuredProtocol: 'gemini-acp' }],
    })
    ;(window as TestWindow).cefQuery = ({ request, onSuccess, onFailure }) => {
      if (JSON.parse(request).action === 'startCliTerminal') onFailure(500, 'Provider failed to start.')
      else onSuccess('{}')
    }
    const session = { id: 'chat-1', name: 'Gemini Session', providerId: 'gemini-cli', viewMode: 'cli' as const, folderId: null, createdAt: new Date(), updatedAt: new Date() }
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    await act(async () => { root.render(<CLIView session={session} />); await new Promise((resolve) => setTimeout(resolve, 0)) })

    await act(async () => { (host.querySelector('button[aria-label="Check provider CLI"]') as HTMLButtonElement).click(); await Promise.resolve() })
    act(() => (host.querySelector('button[aria-label="Open CLI settings"]') as HTMLButtonElement).click())
    expect(refreshCliProviderVersion).toHaveBeenCalledWith('gemini-cli')
    expect(setSettingsOpen).toHaveBeenCalledWith(true)

    act(() => root.unmount())
    host.remove()
  })

  it('delivers terminal output without React commits or terminal restarts', async () => {
    let failInput = false
    const requests: Array<{ action: string; payload?: Record<string, unknown> }> = []
    ;(window as TestWindow).cefQuery = ({ request, onSuccess, onFailure }) => {
      const parsed = JSON.parse(request)
      requests.push(parsed)
      if (parsed.action === 'startCliTerminal') {
        onSuccess(JSON.stringify({ terminalId: 'term-1', sourceChatId: 'chat-1', running: true, lifecycleState: 'idle', turnState: 'idle', lastError: '' }))
      } else if (parsed.action === 'writeCliInput' && failInput) onFailure(500, 'Terminal input could not be delivered.')
      else onSuccess('{}')
    }
    useAppStore.setState({
      providers: [{ id: 'gemini-cli', name: 'Gemini CLI', shortName: 'Gemini', color: '#8ab4ff', description: '', outputMode: 'cli', supportsCli: true, supportsStructured: true, structuredProtocol: 'gemini-acp' }],
      cliBindingBySessionId: {
        'chat-1': { terminalId: 'term-1', boundChatId: 'chat-1', running: true, lifecycleState: 'idle', turnState: 'idle', processing: false, readySinceLastSelect: false, active: true, lastError: '' },
      },
    })
    const session = { id: 'chat-1', name: 'Gemini Session', providerId: 'gemini-cli', viewMode: 'cli' as const, folderId: null, createdAt: new Date(), updatedAt: new Date() }
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    const onRender = vi.fn()
    await act(async () => { root.render(<Profiler id="terminal" onRender={onRender}><CLIView session={session} /></Profiler>); await new Promise((resolve) => setTimeout(resolve, 0)) })

    const rawInput = '\x1b[A\x03'
    xtermState.inputByInstance[0](rawInput)
    expect(requests.find((request) => request.action === 'writeCliInput')?.payload).toEqual({
      chatId: 'chat-1', terminalId: 'term-1', data: rawInput,
    })
    for (let index = 0; index < 20; index++) xtermState.resizeObservers[0]()
    expect(requests.filter((request) => request.action === 'resizeCliTerminal')).toHaveLength(0)
    xtermState.resizeByInstance[0]({ rows: 30, cols: 100 })
    expect(requests.filter((request) => request.action === 'resizeCliTerminal').map(({ payload }) => payload)).toEqual([
      { chatId: 'chat-1', terminalId: 'term-1', rows: 30, cols: 100 },
    ])
    onRender.mockClear()
    await act(async () => {
      window.dispatchEvent(new CustomEvent('uam-cli-output', { detail: { sessionId: session.id, terminalId: 'term-1', data: '\x1b[32moutput\r\n' } }))
      useAppStore.setState({ cliTranscriptBySessionId: { 'chat-1': { terminalId: 'term-1', content: 'output' } } })
      await Promise.resolve()
    })

    for (let index = 0; index < 20; index += 1) {
      act(() => useAppStore.setState((state) => ({ cliBindingBySessionId: {
        ...state.cliBindingBySessionId,
        'chat-1': { ...state.cliBindingBySessionId['chat-1'], processing: index % 2 === 0, turnState: index % 2 === 0 ? 'busy' : 'idle', lifecycleState: index % 2 === 0 ? 'busy' : 'idle' },
      } })))
    }

    expect(requests.filter((request) => request.action === 'startCliTerminal')).toHaveLength(1)
    expect(requests.filter((request) => request.action === 'stopCliTerminal')).toHaveLength(0)
    expect(xtermState.constructCount).toBe(1)
    expect(onRender).not.toHaveBeenCalled()
    expect(Array.from(xtermState.writesByInstance[0][0] as Uint8Array)).toEqual(Array.from(new TextEncoder().encode('\x1b[32moutput\r\n')))

    failInput = true
    await act(async () => { xtermState.inputByInstance[0]('x'); await Promise.resolve() })
    expect(host.textContent).toContain('Terminal input could not be delivered.')
    act(() => (host.querySelector('[aria-label="Dismiss terminal error"]') as HTMLButtonElement).click())
    expect(host.textContent).not.toContain('Terminal input could not be delivered.')

    act(() => root.unmount())
    host.remove()
  })

  it.each(['light', 'paper'] as const)('updates the live terminal palette for %s', async (theme) => {
    ;(window as TestWindow).cefQuery = ({ request, onSuccess }) => {
      const parsed = JSON.parse(request)
      if (parsed.action === 'startCliTerminal') {
        onSuccess(JSON.stringify({ terminalId: 'term-1', sourceChatId: 'chat-1', running: true, lifecycleState: 'idle', turnState: 'idle', lastError: '' }))
      } else onSuccess('{}')
    }
    document.documentElement.setAttribute('data-theme', 'dark')
    useAppStore.setState({
      theme: 'dark',
      providers: [{ id: 'gemini-cli', name: 'Gemini CLI', shortName: 'Gemini', color: '#8ab4ff', description: '', outputMode: 'cli', supportsCli: true, supportsStructured: true, structuredProtocol: 'gemini-acp' }],
    })
    const session = { id: 'chat-1', name: 'Gemini Session', providerId: 'gemini-cli', viewMode: 'cli' as const, folderId: null, createdAt: new Date(), updatedAt: new Date() }
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    await act(async () => { root.render(<CLIView session={session} />); await new Promise((resolve) => setTimeout(resolve, 0)) })
    const liveTerminalIndex = xtermState.constructCount - 1
    expect(xtermState.optionsByInstance[liveTerminalIndex].theme?.background).toBe('#0b0b0e')

    document.documentElement.setAttribute('data-theme', theme)
    await act(async () => {
      useAppStore.setState({ theme })
      await Promise.resolve()
    })
    expect(xtermState.optionsByInstance[liveTerminalIndex].theme?.background).toBe('#f0f0f5')

    act(() => root.unmount())
    host.remove()
  })

  it('routes terminal output by source chat before a binding is known', async () => {
    ;(window as TestWindow).cefQuery = ({ request, onSuccess }) => {
      const parsed = JSON.parse(request)
      if (parsed.action !== 'startCliTerminal') onSuccess('{}')
    }
    useAppStore.setState({ providers: [{ id: 'gemini-cli', name: 'Gemini CLI', shortName: 'Gemini', color: '#8ab4ff', description: '', outputMode: 'cli', supportsCli: true, supportsStructured: true, structuredProtocol: 'gemini-acp' }] })
    const session = { id: 'chat-1', name: 'Gemini Session', providerId: 'gemini-cli', viewMode: 'cli' as const, folderId: null, createdAt: new Date(), updatedAt: new Date() }
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    await act(async () => { root.render(<CLIView session={session} />) })

    act(() => {
      window.dispatchEvent(new CustomEvent('uam-cli-output', {
        detail: { sessionId: '', sourceChatId: 'chat-1', terminalId: 'term-1', data: 'hello' },
      }))
    })

    expect(Array.from(xtermState.writesByInstance[0][0] as Uint8Array)).toEqual(Array.from(new TextEncoder().encode('hello')))

    act(() => root.unmount())
    host.remove()
  })

})
