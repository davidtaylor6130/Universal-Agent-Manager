import { beforeEach, describe, expect, it, vi } from 'vitest'
import { CppAppState, useAppStore } from './useAppStore'

type TestWindow = Window & typeof globalThis & {
  cefQuery?: Window['cefQuery']
}

function ensureTestWindow(): TestWindow {
  if (typeof window !== 'undefined') {
    return window as TestWindow
  }

  const testWindow = {} as TestWindow
  Object.defineProperty(globalThis, 'window', {
    value: testWindow,
    configurable: true,
  })
  return testWindow
}

function makeCppState(revision: number, selectedChatId = 'chat-1'): CppAppState {
  return {
    stateRevision: revision,
    folders: [
      {
        id: 'default',
        title: 'General',
        directory: '/tmp/project',
        collapsed: false,
      },
    ],
    chats: [
      {
        id: 'chat-1',
        title: 'Gemini Session',
        folderId: 'default',
        providerId: 'gemini-cli',
        createdAt: '2026-01-01T00:00:00.000Z',
        updatedAt: '2026-01-01T00:00:01.000Z',
        messages: [],
        cliTerminal: {
          terminalId: 'term-chat-1',
          sourceChatId: 'chat-1',
          running: true,
          turnState: 'idle',
          processing: false,
          readySinceLastSelect: false,
          active: false,
          lastError: '',
        },
      },
    ],
    selectedChatId,
    providers: [
      { id: 'gemini-cli', name: 'Gemini CLI', shortName: 'Gemini', outputMode: 'cli' },
      { id: 'codex-cli', name: 'Codex', shortName: 'Codex', outputMode: 'cli' },
    ],
    settings: {
      activeProviderId: 'gemini-cli',
      theme: 'dark',
    },
  }
}

function resetStore() {
  useAppStore.setState({
    folders: [],
    sessions: [],
    activeSessionId: null,
    lastAppliedStateRevision: -1,
    messages: {},
    providers: [],
    cliBindingBySessionId: {},
    cliTranscriptBySessionId: {},
    cliDebugState: null,
    theme: 'dark',
    isNewChatModalOpen: false,
    isSettingsOpen: false,
    streamingMessageId: null,
    pushChannelStatus: 'connected',
    pushChannelError: '',
    lastPushAtMs: null,
  })
}

describe('useAppStore Gemini CLI slice', () => {
  beforeEach(() => {
    const testWindow = ensureTestWindow()
    resetStore()
    delete testWindow.cefQuery
    vi.restoreAllMocks()
  })

  it('deserializes backend state as CLI-only sessions and providers', () => {
    useAppStore.getState().loadFromCef(makeCppState(1))

    const state = useAppStore.getState()
    expect(state.sessions).toHaveLength(1)
    expect(state.sessions[0]).toMatchObject({
      id: 'chat-1',
      name: 'Gemini Session',
      viewMode: 'cli',
      folderId: 'default',
    })
    expect(state.activeSessionId).toBe('chat-1')
    expect(state.providers.map((provider) => provider.id)).toEqual(['gemini-cli'])
    expect(state.cliBindingBySessionId['chat-1']).toMatchObject({
      terminalId: 'term-chat-1',
      running: true,
      turnState: 'idle',
    })
  })

  it('ignores stale backend revisions', () => {
    useAppStore.getState().loadFromCef(makeCppState(2))
    useAppStore.getState().loadFromCef({
      ...makeCppState(1),
      chats: [{ ...makeCppState(1).chats[0], title: 'Stale' }],
    })

    expect(useAppStore.getState().sessions[0].name).toBe('Gemini Session')
    expect(useAppStore.getState().lastAppliedStateRevision).toBe(2)
  })

  it('creates CEF sessions with the Gemini CLI provider', async () => {
    const requests: Array<{ action: string; payload?: unknown }> = []
    window.cefQuery = ({ request, onSuccess }) => {
      requests.push(JSON.parse(request))
      onSuccess('{}')
    }

    useAppStore.setState({
      folders: [{ id: 'default', name: 'General', parentId: null, directory: '/tmp/project', isExpanded: true, createdAt: new Date() }],
      providers: [{ id: 'gemini-cli', name: 'Gemini CLI', shortName: 'Gemini', color: '#f97316', description: '' }],
    })

    useAppStore.getState().addSession('New Chat', 'default')
    await new Promise((resolve) => setTimeout(resolve, 0))

    expect(requests).toHaveLength(1)
    expect(requests[0].action).toBe('createSession')
    expect(requests[0].payload).toEqual({
      title: 'New Chat',
      folderId: 'default',
      providerId: 'gemini-cli',
    })
  })
})
