import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { CLIView } from './CLIView'
import { useAppStore } from '../../store/useAppStore'

const xtermState = vi.hoisted(() => ({
  constructCount: 0,
  disposeCount: 0,
  writesByInstance: [] as Array<Array<string | Uint8Array>>,
}))

vi.mock('@xterm/xterm', () => ({
  Terminal: class {
    rows = 24
    cols = 80
    index: number
    constructor() {
      this.index = xtermState.constructCount++
      xtermState.writesByInstance[this.index] = []
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
    xtermState.constructCount = 0
    xtermState.disposeCount = 0
    xtermState.writesByInstance = []
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
          providers: [{ ...copilotVersion, installedVersion: '1.0.75', status: 'supported', running: false }],
        },
      })
      await new Promise((resolve) => setTimeout(resolve, 0))
    })

    expect(requests.filter((request) => request.action === 'startCliTerminal').length).toBeGreaterThan(startsBeforeCompatibilityFinished)
    expect(useAppStore.getState().cliBindingBySessionId['chat-copilot']?.running).toBe(true)

    act(() => root.unmount())
    host.remove()
  })

  it('preserves and atomically submits a terminal-fallback steering prompt', async () => {
    const requests: Array<{ action: string; payload?: Record<string, unknown> }> = []
    ;(window as TestWindow).cefQuery = ({ request, onSuccess }) => {
      const parsed = JSON.parse(request)
      requests.push(parsed)
      if (parsed.action === 'startCliTerminal') {
        onSuccess(JSON.stringify({ terminalId: 'term-1', sourceChatId: 'chat-1', running: true, lifecycleState: 'busy', turnState: 'busy', pendingSteer: false, lastError: '' }))
      } else if (parsed.action === 'steerCliTerminal') {
        onSuccess(JSON.stringify({ terminalId: 'term-1', sourceChatId: 'chat-1', running: true, lifecycleState: 'busy', turnState: 'busy', pendingSteer: true, lastError: '' }))
      } else onSuccess('{}')
    }
    useAppStore.setState({ providers: [{ id: 'gemini-cli', name: 'Gemini CLI', shortName: 'Gemini', color: '#8ab4ff', description: '', outputMode: 'cli', supportsCli: true, supportsStructured: true, structuredProtocol: 'gemini-acp' }] })
    const session = { id: 'chat-1', name: 'Gemini Session', providerId: 'gemini-cli', viewMode: 'cli' as const, folderId: null, createdAt: new Date(), updatedAt: new Date() }
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    await act(async () => { root.render(<CLIView session={session} />); await new Promise((resolve) => setTimeout(resolve, 0)) })

    const input = host.querySelector('input[aria-label="Terminal steering prompt"]') as HTMLInputElement
    act(() => {
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')?.set?.call(input, 'Change direction')
      input.dispatchEvent(new Event('input', { bubbles: true }))
    })
    await act(async () => { (host.querySelector('button[aria-label="Steer terminal now"]') as HTMLButtonElement).click(); await new Promise((resolve) => setTimeout(resolve, 0)) })
    expect(requests.find((request) => request.action === 'steerCliTerminal')?.payload).toMatchObject({ chatId: 'chat-1', terminalId: 'term-1', text: 'Change direction', retry: false })
    expect(input.value).toBe('Change direction')
    const pendingButton = host.querySelector('button[aria-label="Steering terminal prompt"]') as HTMLButtonElement
    expect(pendingButton.textContent).toContain('Steering…')
    expect(pendingButton.disabled).toBe(true)

    await act(async () => { useAppStore.getState().setCliBinding('chat-1', { pendingSteer: false, processing: true }); await Promise.resolve() })
    expect(input.value).toBe('')
    await act(async () => {
      root.unmount()
      await new Promise((resolve) => setTimeout(resolve, 0))
    })
    host.remove()
  })

  it('retries a failed pending steer and clears the draft when accepted', async () => {
    const requests: Array<{ action: string; payload?: Record<string, unknown> }> = []
    ;(window as TestWindow).cefQuery = ({ request, onSuccess }) => {
      const parsed = JSON.parse(request)
      requests.push(parsed)
      if (parsed.action === 'startCliTerminal') {
        onSuccess(JSON.stringify({ terminalId: 'term-1', sourceChatId: 'chat-1', running: true, lifecycleState: 'busy', turnState: 'busy', pendingSteer: true, lastError: 'Steer timed out.' }))
      } else if (parsed.action === 'steerCliTerminal') {
        onSuccess(JSON.stringify({ pendingSteer: false, lastError: '' }))
      } else onSuccess('{}')
    }
    useAppStore.setState({ providers: [{ id: 'gemini-cli', name: 'Gemini CLI', shortName: 'Gemini', color: '#8ab4ff', description: '', outputMode: 'cli', supportsCli: true, supportsStructured: true, structuredProtocol: 'gemini-acp' }] })
    const session = { id: 'chat-1', name: 'Gemini Session', providerId: 'gemini-cli', viewMode: 'cli' as const, folderId: null, createdAt: new Date(), updatedAt: new Date() }
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    await act(async () => { root.render(<CLIView session={session} />); await new Promise((resolve) => setTimeout(resolve, 0)) })

    const input = host.querySelector('input[aria-label="Terminal steering prompt"]') as HTMLInputElement
    act(() => {
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')?.set?.call(input, 'Try again')
      input.dispatchEvent(new Event('input', { bubbles: true }))
    })
    const retryButton = host.querySelector('button[aria-label="Retry terminal steer"]') as HTMLButtonElement
    expect(retryButton.textContent).toContain('Retry steer')
    expect(retryButton.disabled).toBe(false)

    await act(async () => { retryButton.click(); await new Promise((resolve) => setTimeout(resolve, 0)) })

    expect(requests.find((request) => request.action === 'steerCliTerminal')?.payload).toMatchObject({
      chatId: 'chat-1',
      terminalId: 'term-1',
      text: 'Try again',
      retry: true,
    })
    expect(input.value).toBe('')

    act(() => root.unmount())
    host.remove()
  })

  it('surfaces a terminal steer request failure', async () => {
    ;(window as TestWindow).cefQuery = ({ request, onSuccess, onFailure }) => {
      const parsed = JSON.parse(request)
      if (parsed.action === 'startCliTerminal') {
        onSuccess(JSON.stringify({ terminalId: 'term-1', sourceChatId: 'chat-1', running: true, lifecycleState: 'busy', turnState: 'busy', pendingSteer: false, lastError: '' }))
      } else if (parsed.action === 'steerCliTerminal') {
        onFailure(1, 'Could not steer terminal.')
      } else onSuccess('{}')
    }
    useAppStore.setState({ providers: [{ id: 'gemini-cli', name: 'Gemini CLI', shortName: 'Gemini', color: '#8ab4ff', description: '', outputMode: 'cli', supportsCli: true, supportsStructured: true, structuredProtocol: 'gemini-acp' }] })
    const session = { id: 'chat-1', name: 'Gemini Session', providerId: 'gemini-cli', viewMode: 'cli' as const, folderId: null, createdAt: new Date(), updatedAt: new Date() }
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    await act(async () => { root.render(<CLIView session={session} />); await new Promise((resolve) => setTimeout(resolve, 0)) })

    const input = host.querySelector('input[aria-label="Terminal steering prompt"]') as HTMLInputElement
    act(() => {
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')?.set?.call(input, 'Try something else')
      input.dispatchEvent(new Event('input', { bubbles: true }))
    })
    await act(async () => {
      ;(host.querySelector('button[aria-label="Steer terminal now"]') as HTMLButtonElement).click()
      await new Promise((resolve) => setTimeout(resolve, 0))
    })

    expect(host.textContent).toContain('Could not steer terminal.')
    expect(input.value).toBe('Try something else')

    act(() => {
      ;(host.querySelector('button[aria-label="Dismiss terminal error"]') as HTMLButtonElement).click()
    })
    expect(host.textContent).not.toContain('Could not steer terminal.')

    act(() => {
      useAppStore.getState().setCliBinding('chat-1', {
        lastError: 'Could not steer terminal.',
      })
    })
    expect(host.textContent).not.toContain('Could not steer terminal.')

    act(() => {
      useAppStore.getState().setCliBinding('chat-1', { lastError: '' })
    })
    act(() => {
      useAppStore.getState().setCliBinding('chat-1', {
        lastError: 'Could not steer terminal.',
      })
    })
    expect(host.textContent).toContain('Could not steer terminal.')

    act(() => root.unmount())
    host.remove()
  })

  it('does not restart a bound terminal when its transcript identity hydrates', async () => {
    const requests: Array<{ action: string }> = []
    ;(window as TestWindow).cefQuery = ({ request, onSuccess }) => {
      const parsed = JSON.parse(request)
      requests.push(parsed)
      if (parsed.action === 'startCliTerminal') {
        onSuccess(JSON.stringify({ terminalId: 'term-1', sourceChatId: 'chat-1', running: true, lifecycleState: 'idle', turnState: 'idle', lastError: '' }))
      } else onSuccess('{}')
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
    await act(async () => { root.render(<CLIView session={session} />); await new Promise((resolve) => setTimeout(resolve, 0)) })

    await act(async () => {
      useAppStore.setState({ cliTranscriptBySessionId: { 'chat-1': { terminalId: 'term-1', content: 'output' } } })
      await Promise.resolve()
    })

    expect(requests.filter((request) => request.action === 'startCliTerminal')).toHaveLength(1)
    expect(requests.filter((request) => request.action === 'stopCliTerminal')).toHaveLength(0)
    expect(xtermState.constructCount).toBe(1)

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

  it('submits a terminal steer only once before the submitting state rerenders', async () => {
    const requests: Array<{ action: string }> = []
    const finishSteers: Array<(response: string) => void> = []
    ;(window as TestWindow).cefQuery = ({ request, onSuccess }) => {
      const parsed = JSON.parse(request)
      requests.push(parsed)
      if (parsed.action === 'startCliTerminal') {
        onSuccess(JSON.stringify({ terminalId: 'term-1', sourceChatId: 'chat-1', running: true, lifecycleState: 'busy', turnState: 'busy', pendingSteer: false, lastError: '' }))
      } else if (parsed.action === 'steerCliTerminal') {
        finishSteers.push(onSuccess)
      }
    }
    useAppStore.setState({ providers: [{ id: 'gemini-cli', name: 'Gemini CLI', shortName: 'Gemini', color: '#8ab4ff', description: '', outputMode: 'cli', supportsCli: true, supportsStructured: true, structuredProtocol: 'gemini-acp' }] })
    const session = { id: 'chat-1', name: 'Gemini Session', providerId: 'gemini-cli', viewMode: 'cli' as const, folderId: null, createdAt: new Date(), updatedAt: new Date() }
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    await act(async () => { root.render(<CLIView session={session} />); await Promise.resolve() })

    const input = host.querySelector('input[aria-label="Terminal steering prompt"]') as HTMLInputElement
    act(() => {
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')?.set?.call(input, 'Steer only once')
      input.dispatchEvent(new Event('input', { bubbles: true }))
    })
    const steer = host.querySelector('button[aria-label="Steer terminal now"]') as HTMLButtonElement
    act(() => {
      steer.click()
      steer.click()
    })

    expect(requests.filter((request) => request.action === 'steerCliTerminal')).toHaveLength(1)
    await act(async () => {
      finishSteers.forEach((finish) => finish(JSON.stringify({ pendingSteer: false, lastError: '' })))
      await Promise.resolve()
    })
    await act(async () => {
      root.unmount()
      await new Promise((resolve) => setTimeout(resolve, 0))
    })
    host.remove()
  })

  it('does not let a steer finishing in another chat clear the current draft', async () => {
    let finishSteer: ((response: string) => void) | null = null
    ;(window as TestWindow).cefQuery = ({ request, onSuccess }) => {
      const parsed = JSON.parse(request)
      if (parsed.action === 'startCliTerminal') {
        onSuccess(JSON.stringify({
          terminalId: `term-${parsed.payload.chatId}`,
          sourceChatId: parsed.payload.chatId,
          running: true,
          lifecycleState: 'busy',
          turnState: 'busy',
          pendingSteer: false,
          lastError: '',
        }))
      } else if (parsed.action === 'steerCliTerminal') {
        finishSteer = onSuccess
      } else onSuccess('{}')
    }
    useAppStore.setState({
      providers: [{ id: 'gemini-cli', name: 'Gemini CLI', shortName: 'Gemini', color: '#8ab4ff', description: '', outputMode: 'cli', supportsCli: true, supportsStructured: true, structuredProtocol: 'gemini-acp' }],
    })
    const first = { id: 'chat-1', name: 'First', providerId: 'gemini-cli', viewMode: 'cli' as const, folderId: null, createdAt: new Date(), updatedAt: new Date() }
    const second = { ...first, id: 'chat-2', name: 'Second' }
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    await act(async () => { root.render(<CLIView session={first} />); await new Promise((resolve) => setTimeout(resolve, 0)) })

    let input = host.querySelector('input[aria-label="Terminal steering prompt"]') as HTMLInputElement
    act(() => {
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')?.set?.call(input, 'First steer')
      input.dispatchEvent(new Event('input', { bubbles: true }))
      ;(host.querySelector('button[aria-label="Steer terminal now"]') as HTMLButtonElement).click()
    })
    await act(async () => { root.render(<CLIView session={second} />); await new Promise((resolve) => setTimeout(resolve, 0)) })
    input = host.querySelector('input[aria-label="Terminal steering prompt"]') as HTMLInputElement
    act(() => {
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')?.set?.call(input, 'Second steer')
      input.dispatchEvent(new Event('input', { bubbles: true }))
    })
    await act(async () => {
      finishSteer?.(JSON.stringify({ pendingSteer: false, lastError: '' }))
      await new Promise((resolve) => setTimeout(resolve, 0))
    })

    expect(input.value).toBe('Second steer')

    await act(async () => {
      root.unmount()
      await new Promise((resolve) => setTimeout(resolve, 0))
    })
    host.remove()
  })
})
