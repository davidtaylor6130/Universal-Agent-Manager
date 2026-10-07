import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { useAppStore, type CppAppState } from './useAppStore'
import type { Message } from '../types/message'
import type { AcpBinding } from './cpp/types'

vi.mock('../ipc/cefBridge', async (original) => ({
  ...await original<typeof import('../ipc/cefBridge')>(),
  isCefContext: () => true,
  sendToCEF: vi.fn(async () => ({ ok: false, error: 'No request transport in push fixture' })),
}))

const historical: Message[] = [{ id: 'historical-100', sessionId: 'search-chat', role: 'assistant', content: 'Historical result', createdAt: new Date(100) }]
const fullMessages = [{ role: 'assistant', content: 'Latest result', timestamp: '2026-01-01T00:00:00Z' }]

function fullState(revision: number): CppAppState {
  return {
    stateRevision: revision, selectedChatId: 'search-chat',
    folders: [], resourceCollections: [], providers: [],
    chats: [{ id: 'search-chat', title: 'Search chat', folderId: '', providerId: 'codex-cli', createdAt: '', updatedAt: '', messages: fullMessages }],
    settings: {}, statusLine: '',
  } as unknown as CppAppState
}

describe('bounded search native push integration', () => {
  beforeEach(() => {
    vi.useFakeTimers()
    useAppStore.setState({
      sessions: [{ id: 'search-chat', name: 'Search chat', viewMode: 'chat', folderId: '', providerId: 'codex-cli', createdAt: new Date(), updatedAt: new Date() }],
      activeSessionId: 'search-chat', lastAppliedStateRevision: -1,
      messages: { 'search-chat': historical }, historyStartIndexBySessionId: { 'search-chat': 100 },
      chatSearchNavigation: { generation: 1, queryRequestId: 'query-1', chatId: 'search-chat', messageIndex: 100, messagesDigest: 'old', phase: 'ready', anchored: true },
      acpBindingBySessionId: {}, cliBindingBySessionId: {},
    })
  })
  afterEach(() => { vi.clearAllTimers(); vi.useRealTimers(); useAppStore.setState({ chatSearchNavigation: null }) })

  it('keeps the owned historical slice through authoritative full and patch pushes, then resets offsets for a later full transcript', () => {
    const state = fullState(1)
    state.chats[0].acpSession = { sessionId: 'native-live', processing: true, turnAssistantMessageIndex: 1000, turnEvents: [{ type: 'assistant_text', text: 'Live timeline' }] }
    window.uamPush?.({ type: 'stateUpdate', data: state })
    expect(useAppStore.getState().messages['search-chat']).toBe(historical)
    expect(useAppStore.getState().historyStartIndexBySessionId['search-chat']).toBe(100)
    expect(useAppStore.getState().acpBindingBySessionId['search-chat'].turnEvents).toEqual([{ type: 'assistant_text', text: 'Live timeline' }])
    window.uamPush?.({ type: 'statePatch', data: { stateRevision: 2, messagesByChatId: { 'search-chat': fullMessages } } })
    vi.runAllTimers()
    expect(useAppStore.getState().messages['search-chat']).toBe(historical)
    expect(useAppStore.getState().historyStartIndexBySessionId['search-chat']).toBe(100)
    useAppStore.setState({ chatSearchNavigation: null })
    window.uamPush?.({ type: 'stateUpdate', data: fullState(3) })
    expect(useAppStore.getState().messages['search-chat'][0].content).toBe('Latest result')
    expect(useAppStore.getState().historyStartIndexBySessionId['search-chat']).toBe(0)
  })

  it('does not attach an out-of-window live token to a false historical absolute index', () => {
    const binding: AcpBinding = {
      sessionId: 'native-live', providerId: 'codex-cli', protocolKind: 'codex-app-server', threadId: '',
      running: true, lifecycleState: 'running', processing: true, readySinceLastSelect: false,
      processingStartedAtMs: null, lastError: '', recentStderr: '', lastExitCode: null,
      diagnostics: [], toolCalls: [], planEntries: [], availableModes: [], currentModeId: '', availableModels: [],
      currentModelId: '', turnEvents: [], turnUserMessageIndex: 999, turnAssistantMessageIndex: 1000,
      turnSerial: 1, pendingPermission: null, pendingUserInput: null, agentInfo: null,
    }
    useAppStore.setState({ acpBindingBySessionId: { 'search-chat': binding } })
    window.uamPush?.({ type: 'streamToken', chatId: 'search-chat', messageIndex: 1000, token: 'Live provider token' })
    vi.runAllTimers()
    expect(useAppStore.getState().messages['search-chat']).toBe(historical)
    expect(useAppStore.getState().historyStartIndexBySessionId['search-chat']).toBe(100)
    useAppStore.setState((state) => ({ chatSearchNavigation: null,
      sessions: state.sessions.map((session) => ({ ...session, messageCount: 1001 })),
    }))
    window.uamPush?.({ type: 'streamToken', chatId: 'search-chat', messageIndex: 1000, token: 'Still outside this page' })
    vi.runAllTimers()
    expect(useAppStore.getState().messages['search-chat']).toBe(historical)
    const tail: Message[] = [
      { id: 'tail-user', sessionId: 'search-chat', role: 'user', content: 'Latest question', createdAt: new Date(999) },
      { id: 'tail-assistant', sessionId: 'search-chat', role: 'assistant', content: 'Latest answer', createdAt: new Date(1000) },
    ]
    useAppStore.setState({ messages: { 'search-chat': tail }, historyStartIndexBySessionId: { 'search-chat': 999 } })
    window.uamPush?.({ type: 'streamToken', chatId: 'search-chat', messageIndex: 1000, token: ' continued' })
    vi.runAllTimers()
    expect(useAppStore.getState().messages['search-chat']).toHaveLength(2)
    expect(useAppStore.getState().messages['search-chat'][1]).toMatchObject({ id: 'tail-assistant', content: 'Latest answer continued' })
    expect(useAppStore.getState().historyStartIndexBySessionId['search-chat']).toBe(999)
  })
})
