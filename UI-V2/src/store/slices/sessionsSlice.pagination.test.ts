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
afterEach(() => { delete window.cefQuery; vi.restoreAllMocks() })
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
