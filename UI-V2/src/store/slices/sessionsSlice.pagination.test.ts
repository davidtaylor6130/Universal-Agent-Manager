import { afterEach, describe, expect, it, vi } from 'vitest'
import { createSessionsSlice } from './sessionsSlice'
import type { AppState, ZustandSet } from '../storeTypes'

function fixture() {
  const requests: Array<{ action: string; payload: Record<string, unknown>; reply: (data: unknown) => void }> = []
  window.cefQuery = ({ request, onSuccess }) => {
    requests.push({ ...JSON.parse(request), reply: data => onSuccess(JSON.stringify(data)) })
  }
  let state: AppState
  const get = () => state
  const set: ZustandSet = input => { state = { ...state, ...(typeof input === 'function' ? input(state) : input) } }
  state = { ...createSessionsSlice(set, get, true), acpBindingBySessionId: {}, cliBindingBySessionId: {},
    sessions: [{ id: 'chat', name: 'Long chat', folderId: null, createdAt: new Date(0), updatedAt: new Date(0) }] } as AppState
  return { get, set, requests }
}
function page(startIndex = 9950, totalCount = 10000, messagesDigest = 'snapshot') {
  return { chatId: 'chat', startIndex, totalCount, messagesDigest, messages: Array.from({ length: totalCount - startIndex }, (_, i) => ({
    role: 'assistant', content: `message ${startIndex + i}`, createdAt: '2026-01-01T00:00:00Z',
  })) }
}
afterEach(() => { delete window.cefQuery; vi.restoreAllMocks(); vi.useRealTimers() })
describe('desktop bounded history', () => {
  it('loads only the recent tail of a long chat and fetches older messages only on demand', async () => {
    const { get, requests } = fixture()
    const load = get().loadSessionMessages('chat')
    expect(requests[0].payload).toMatchObject({ chatId: 'chat', limit: 50, deferToolCallContent: true })
    requests[0].reply(page()); await load
    expect(get().messages.chat).toHaveLength(50)
    expect(get().historyStartIndexBySessionId.chat).toBe(9950)
    expect(get().messages.chat[0].nativeMessageIndex).toBe(9950)
    expect(get().sessions[0].messageCount).toBe(10000)
    expect(requests).toHaveLength(1)
    const older = get().loadOlderSessionMessages('chat')
    expect(requests[1].payload).toEqual({ chatId: 'chat', limit: 100, before: 9950, deferToolCallContent: true })
    requests[1].reply(page(9850, 9950)); expect(await older).toBe(true)
    expect(get().messages.chat).toHaveLength(150)
    expect(get().messages.chat[0].nativeMessageIndex).toBe(9850)
    const refresh = get().loadSessionMessages('chat', true)
    requests[2].reply(page(9951, 10001, 'changed')); await refresh
    expect(get().messages.chat).toHaveLength(50)
    expect(get().historyStartIndexBySessionId.chat).toBe(9951)
  })
  it('accepts a smaller complete cached tail and pages immediately before it without gaps', async () => {
    const { get, requests } = fixture()
    const load = get().loadSessionMessages('chat')
    requests[0].reply(page(9992)); await load
    expect(get().messages.chat).toHaveLength(8)
    expect(get().historyStartIndexBySessionId.chat).toBe(9992)
    const older = get().loadOlderSessionMessages('chat')
    expect(requests[1].payload.before).toBe(9992)
    requests[1].reply(page(9892, 9992)); expect(await older).toBe(true)
    expect(get().messages.chat).toHaveLength(108)
    expect(get().messages.chat.map(message => message.nativeMessageIndex)).toEqual(Array.from({ length: 108 }, (_, index) => 9892 + index))
  })
  it('keeps a longer streaming tail when an earlier bounded refresh resolves', async () => {
    const { get, set, requests } = fixture()
    const load = get().loadSessionMessages('chat')
    requests[0].reply(page(99, 100)); await load
    const binding = { turnSerial: 1, sessionId: 'runtime', threadId: 'thread', providerId: 'gemini-cli', turnAssistantMessageIndex: 99 }
    set({ acpBindingBySessionId: { chat: binding } } as Partial<AppState>)
    set({ messages: { chat: get().messages.chat.map(message => ({ ...message, isStreaming: true })) } })
    const refresh = get().loadSessionMessages('chat', true)
    set({ messages: { chat: get().messages.chat.map(message => ({ ...message, content: `${message.content} continued` })) } })
    requests[1].reply(page(99, 100)); await refresh
    expect(get().messages.chat[0].content).toBe('message 99 continued')
    expect(get().messages.chat[0].isStreaming).toBe(true)
    expect(get().messages.chat[0].nativeMessageIndex).toBe(99)
  })
  it('ignores an older reply after unload and rehydration at the same history position', async () => {
    const { get, requests } = fixture()
    const load = get().loadSessionMessages('chat'); requests[0].reply(page()); await load
    const older = get().loadOlderSessionMessages('chat')
    get().unloadSessionMessages('chat')
    const reload = get().loadSessionMessages('chat')
    requests.at(-1)!.reply(page()); await reload
    const current = get().messages.chat
    requests[1].reply(page(9850, 9950))
    expect(await older).toBe(false)
    expect(get().messages.chat).toBe(current)
    expect(get().historyStartIndexBySessionId.chat).toBe(9950)
  })
  it('keeps message identities and conditional digests on an unchanged bounded page', async () => {
    const { get, requests } = fixture()
    const load = get().loadSessionMessages('chat'); requests[0].reply(page()); await load
    const current = get().messages.chat
    const refresh = get().loadSessionMessages('chat')
    expect(requests[1].payload.messagesDigest).toBe('snapshot')
    requests[1].reply(page()); await refresh
    expect(get().messages.chat).toBe(current)
  })
  it('rejects an older page belonging to another chat', async () => {
    const { get, requests } = fixture()
    const load = get().loadSessionMessages('chat'); requests[0].reply(page()); await load
    const original = get().messages.chat
    const older = get().loadOlderSessionMessages('chat')
    requests[1].reply({ ...page(9850, 9950), chatId: 'other' })
    expect(await older).toBe(false)
    expect(get().messages.chat).toBe(original)
  })
})


describe('truthful initial history read state', () => {
  it('keeps an explicit native refresh pending past the saved-read bound until it answers', async () => {
    vi.useFakeTimers()
    const { get, requests } = fixture()
    const load = get().loadSessionMessages('chat', true, true)
    expect(requests[0].payload).toMatchObject({ chatId: 'chat', refreshNative: true })
    expect(get().chatHistoryReadBySessionId.chat?.refreshNative).toBe(true)
    await vi.advanceTimersByTimeAsync(30_000)
    expect(get().chatHistoryReadBySessionId.chat?.refreshNative).toBe(true)
    expect(get().chatHistoryErrorBySessionId.chat).toBeUndefined()
    requests[0].reply(page()); await load
    expect(get().chatHistoryReadBySessionId.chat).toBeUndefined()
    expect(get().messages.chat).toHaveLength(50)
  })
  it('bounds an unanswered native refresh at five minutes with refresh guidance', async () => {
    vi.useFakeTimers()
    const { get } = fixture()
    const load = get().loadSessionMessages('chat', true, true)
    await vi.advanceTimersByTimeAsync(299_999)
    expect(get().chatHistoryReadBySessionId.chat?.refreshNative).toBe(true)
    await vi.advanceTimersByTimeAsync(1); await load
    expect(get().chatHistoryReadBySessionId.chat).toBeUndefined()
    expect(get().chatHistoryErrorBySessionId.chat ?? '').toContain('History refresh did not finish within five minutes')
  })
  it('bounds a silent history read without replay and rejects its late page', async () => {
    vi.useFakeTimers()
    const { get, set, requests } = fixture()
    const binding = { queueHeld: true, queuedPrompts: [{ id: 'held', revision: 1, text: 'Keep this input', attachments: [] }] }
    set({ acpBindingBySessionId: { chat: binding } } as Partial<AppState>)
    const load = get().loadSessionMessages('chat')
    let outcome: unknown = 'pending'
    void Promise.resolve(load).then(value => { outcome = value })
    await vi.advanceTimersByTimeAsync(30_000)
    expect(outcome).toBe(false)
    expect(get().chatHistoryReadBySessionId?.chat).toBeUndefined()
    expect(get().chatHistoryErrorBySessionId.chat ?? '').toContain('timed out')
    expect(get().messages.chat).toBeUndefined()
    expect(get().acpBindingBySessionId.chat).toBe(binding)
    const settled = get()
    requests[0].reply(page())
    await Promise.resolve()
    expect(get()).toBe(settled)
    expect(requests.map(request => request.action)).toEqual(['getChatMessages'])
  })

  it('settles an empty successful history as empty rather than still reading', async () => {
    const { get, requests } = fixture()
    const load = get().loadSessionMessages('chat')
    expect(get().chatHistoryReadBySessionId?.chat).toBeTruthy()
    requests[0].reply(page(0, 0)); await load
    expect(get().chatHistoryReadBySessionId?.chat).toBeUndefined()
    expect(get().messages.chat).toEqual([])
    expect(get().chatHistoryErrorBySessionId.chat).toBeUndefined()
  })

  it('does not let an older deadline clear or fail the replacement read', async () => {
    vi.useFakeTimers()
    const { get, requests } = fixture()
    const old = get().loadSessionMessages('chat')
    await vi.advanceTimersByTimeAsync(10_000)
    const fresh = get().loadSessionMessages('chat', true)
    const pending = get().chatHistoryReadBySessionId?.chat
    expect(pending).toBeTruthy()
    await vi.advanceTimersByTimeAsync(20_000); await old
    expect(get().chatHistoryReadBySessionId?.chat).toBe(pending)
    expect(get().chatHistoryErrorBySessionId.chat).toBeUndefined()
    requests[1].reply(page(0, 1)); await fresh
    expect(get().messages.chat[0].content).toBe('message 0')
    expect(get().chatHistoryReadBySessionId?.chat).toBeUndefined()
    requests[0].reply(page())
  })

  it('retires a discarded invalid page without claiming a read is still pending', async () => {
    const { get, requests } = fixture()
    const load = get().loadSessionMessages('chat')
    requests[0].reply({ ...page(), chatId: 'foreign' }); await load
    expect(get().chatHistoryReadBySessionId?.chat).toBeUndefined()
    expect(get().messages.chat).toBeUndefined()
    expect(requests).toHaveLength(1)
  })

  it('bounds a silent selection prerequisite without rolling back or hydrating from its late ACK', async () => {
    vi.useFakeTimers()
    const { get, set, requests } = fixture()
    set({ sessions: [{ ...get().sessions[0], messageCount: 1 }], activeSessionId: null })
    get().setActiveSession('chat')
    expect(requests.map(request => request.action)).toEqual(['selectSession'])
    await vi.advanceTimersByTimeAsync(30_000)
    expect(get().chatHistoryReadBySessionId?.chat).toBeUndefined()
    expect(get().chatHistoryErrorBySessionId.chat ?? '').toContain('timed out')
    expect(get().activeSessionId).toBe('chat')
    requests[0].reply({}); await Promise.resolve()
    expect(requests.map(request => request.action)).toEqual(['selectSession'])
    expect(get().messages.chat).toBeUndefined()
  })

  it('does not replace newly hydrated history with an old read timeout', async () => {
    vi.useFakeTimers()
    const { get, set, requests } = fixture()
    const load = get().loadSessionMessages('chat')
    const hydrated = [{ id: 'current', role: 'user' as const, content: 'Current saved input', createdAt: new Date(0) }]
    set({ messages: { chat: hydrated } })
    await vi.advanceTimersByTimeAsync(30_000); await load
    expect(get().messages.chat).toBe(hydrated)
    expect(get().chatHistoryErrorBySessionId.chat).toBeUndefined()
    expect(get().chatHistoryReadBySessionId?.chat).toBeUndefined()
    requests[0].reply(page())
    await Promise.resolve()
    expect(get().messages.chat).toBe(hydrated)
  })

  it('cannot timeout into a deleted or same-id replacement chat', async () => {
    vi.useFakeTimers()
    const { get, set, requests } = fixture()
    const old = get().loadSessionMessages('chat')
    get().unloadSessionMessages('chat')
    expect(get().chatHistoryReadBySessionId?.chat).toBeUndefined()
    set({ sessions: [{ ...get().sessions[0], createdAt: new Date(1) }] })
    const fresh = get().loadSessionMessages('chat')
    requests.filter(request => request.action === 'getChatMessages').at(-1)!.reply(page(0, 0)); await fresh
    const current = get()
    let settled = false
    void Promise.resolve(old).then(() => { settled = true })
    await vi.advanceTimersByTimeAsync(30_000)
    expect(settled).toBe(true)
    expect(get()).toBe(current)
    expect(get().chatHistoryErrorBySessionId.chat).toBeUndefined()
    requests[0].reply(page())
  })
})
