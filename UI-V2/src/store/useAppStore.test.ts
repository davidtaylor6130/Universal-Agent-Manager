import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

type CefCallbacks = {
  onSuccess: (response: string) => void
  onFailure: (errorCode: number, errorMessage: string) => void
}

type HarnessOptions = {
  failActions?: Set<string>
  holdInitialState?: boolean
}

type CppChatLike = {
  id: string
  title: string
  folderId?: string
  providerId?: string
  createdAt?: string
  updatedAt?: string
  messages?: Array<{ role: 'user' | 'assistant' | 'system'; content: string; createdAt: string }>
}

type CppStateLike = {
  stateRevision: number
  folders: Array<{ id: string; title: string; directory: string; collapsed: boolean }>
  chats: CppChatLike[]
  selectedChatId: string | null
  providers: Array<{ id: string; name: string; shortName: string }>
  settings: { activeProviderId: string; theme: string }
  cliDebug?: unknown
}

function installBrowserHarness(options: HarnessOptions = {}) {
  const failActions = options.failActions ?? new Set<string>()
  const documentElementState = new Map<string, string>()
  let pendingInitialState: CefCallbacks | null = null
  const requestLog: Array<{ action: string; requestId?: string }> = []

  Object.defineProperty(globalThis, 'window', {
    configurable: true,
    writable: true,
    value: globalThis,
  })

  Object.defineProperty(globalThis, 'document', {
    configurable: true,
    writable: true,
    value: {
      documentElement: {
        setAttribute(name: string, value: string) {
          documentElementState.set(name, value)
        },
        getAttribute(name: string) {
          return documentElementState.get(name) ?? null
        },
      },
    },
  })

  Object.defineProperty(globalThis, 'localStorage', {
    configurable: true,
    writable: true,
    value: {
      getItem: vi.fn(() => null),
      setItem: vi.fn(),
      removeItem: vi.fn(),
      clear: vi.fn(),
    },
  })

  ;(globalThis as unknown as { cefQuery?: (params: CefCallbacks & { request: string }) => void }).cefQuery =
    ({ request, onSuccess, onFailure }) => {
      const parsed = JSON.parse(request) as { action?: string; requestId?: string }
      requestLog.push({ action: parsed.action ?? '', requestId: parsed.requestId })

      if (parsed.action === 'getInitialState' && options.holdInitialState) {
        pendingInitialState = { onSuccess, onFailure }
        return
      }

      if (parsed.action === 'getInitialState') {
        onSuccess(JSON.stringify(makeCppState(0, [{ id: 'boot-chat', title: 'Boot Chat' }], 'boot-chat')))
        return
      }

      if (failActions.has(parsed.action ?? '')) {
        onFailure(500, `${parsed.action} failed`)
        return
      }

      onSuccess('{}')
    }

  return {
    requestLog,
    resolveInitialState(state: CppStateLike) {
      if (!pendingInitialState) {
        throw new Error('No pending getInitialState request to resolve.')
      }

      pendingInitialState.onSuccess(JSON.stringify(state))
      pendingInitialState = null
    },
  }
}

function makeCppState(
  revision: number,
  chats: CppChatLike[],
  selectedChatId: string | null = chats[0]?.id ?? null
): CppStateLike {
  return {
    stateRevision: revision,
    folders: [],
    chats: chats.map((chat) => ({
      id: chat.id,
      title: chat.title,
      folderId: chat.folderId ?? '',
      providerId: chat.providerId ?? 'gemini-cli',
      createdAt: chat.createdAt ?? '2025-01-01T00:00:00.000Z',
      updatedAt: chat.updatedAt ?? '2025-01-01T00:00:00.000Z',
      messages: chat.messages ?? [],
    })),
    selectedChatId,
    providers: [{ id: 'gemini-cli', name: 'Gemini CLI', shortName: 'Gemini CLI' }],
    settings: { activeProviderId: 'gemini-cli', theme: 'dark' },
    cliDebug: {
      selectedChatId,
      terminalCount: 0,
      runningTerminalCount: 0,
      busyTerminalCount: 0,
      terminals: [],
    },
  }
}

function flush() {
  return new Promise<void>((resolve) => setTimeout(resolve, 0))
}

describe('useAppStore CEF reconciliation', () => {
  beforeEach(() => {
    vi.resetModules()
    vi.spyOn(console, 'debug').mockImplementation(() => {})
    vi.spyOn(console, 'error').mockImplementation(() => {})
  })

  afterEach(() => {
    vi.restoreAllMocks()
    delete (globalThis as unknown as { cefQuery?: unknown }).cefQuery
  })

  it('ignores a stale initial state after a newer push arrives', async () => {
    const harness = installBrowserHarness({ holdInitialState: true })
    const { useAppStore } = await import('./useAppStore')

    const pushedState = makeCppState(5, [{ id: 'chat-1', title: 'Pushed Chat' }], 'chat-1')
    window.uamPush?.({ type: 'stateUpdate', data: pushedState })
    await flush()

    expect(useAppStore.getState().sessions[0]?.name).toBe('Pushed Chat')
    expect(useAppStore.getState().lastAppliedStateRevision).toBe(5)

    harness.resolveInitialState(makeCppState(1, [{ id: 'chat-1', title: 'Stale Chat' }], 'chat-1'))
    await flush()

    expect(useAppStore.getState().sessions[0]?.name).toBe('Pushed Chat')
    expect(useAppStore.getState().lastAppliedStateRevision).toBe(5)
  })

  it('rolls back renameSession when the backend rejects the matching request', async () => {
    const harness = installBrowserHarness({
      failActions: new Set(['renameSession']),
      holdInitialState: true,
    })
    const { useAppStore } = await import('./useAppStore')

    harness.resolveInitialState(makeCppState(1, [{ id: 'chat-1', title: 'Original' }], 'chat-1'))
    await flush()

    useAppStore.getState().renameSession('chat-1', 'Renamed')
    expect(useAppStore.getState().sessions[0]?.name).toBe('Renamed')

    await flush()

    expect(useAppStore.getState().sessions[0]?.name).toBe('Original')
  })

  it('rolls back sendMessage when the backend rejects the matching request', async () => {
    const harness = installBrowserHarness({
      failActions: new Set(['sendMessage']),
      holdInitialState: true,
    })
    const { useAppStore } = await import('./useAppStore')

    harness.resolveInitialState(makeCppState(1, [{ id: 'chat-1', title: 'Original' }], 'chat-1'))
    await flush()

    useAppStore.getState().sendMessage('chat-1', 'Hello Gemini')
    expect(useAppStore.getState().messages['chat-1']?.length).toBe(2)
    expect(useAppStore.getState().streamingMessageId).toBeTruthy()

    await flush()

    expect(useAppStore.getState().messages['chat-1']?.length ?? 0).toBe(0)
    expect(useAppStore.getState().streamingMessageId).toBeNull()
  })

  it('restores the previous selection when selectSession fails', async () => {
    const harness = installBrowserHarness({
      failActions: new Set(['selectSession']),
      holdInitialState: true,
    })
    const { useAppStore } = await import('./useAppStore')

    harness.resolveInitialState(
      makeCppState(
        1,
        [
          { id: 'chat-1', title: 'First' },
          { id: 'chat-2', title: 'Second' },
        ],
        'chat-1'
      )
    )
    await flush()

    useAppStore.getState().setActiveSession('chat-2')
    expect(useAppStore.getState().activeSessionId).toBe('chat-2')

    await flush()

    expect(useAppStore.getState().activeSessionId).toBe('chat-1')
    expect(useAppStore.getState().lastAppliedStateRevision).toBe(1)
    expect(harness.requestLog.some((entry) => entry.action === 'selectSession')).toBe(true)
  })

  it('keeps the new chat modal open when createSession fails', async () => {
    const harness = installBrowserHarness({
      failActions: new Set(['createSession']),
      holdInitialState: true,
    })
    const { useAppStore } = await import('./useAppStore')

    harness.resolveInitialState(makeCppState(1, [], null))
    await flush()

    useAppStore.setState({ isNewChatModalOpen: true })
    useAppStore.getState().addSession('New Chat', 'cli', null)

    await flush()

    expect(useAppStore.getState().isNewChatModalOpen).toBe(true)
  })
})
