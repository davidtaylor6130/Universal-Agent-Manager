import { afterEach, describe, expect, it, vi } from 'vitest'
import { createSessionsSlice } from './sessionsSlice'
import type { AppState, ZustandSet } from '../storeTypes'
import { readChatComposerDraft, writeChatComposerDraft } from '../../utils/composerDraftStorage'

const hit = (index = 100) => ({ chatId: 'chat', messageIndex: index, messagesDigest: 'snapshot', field: 'content' as const, excerpt: 'needle' })
const session = (id: string) => ({ id, name: id, folderId: null, createdAt: new Date(0), updatedAt: new Date(0) })
const page = (startIndex = 80, totalCount = 200, content = 'needle', messagesDigest = 'snapshot') => ({ chatId: 'chat', startIndex, totalCount, messagesDigest,
  messages: Array.from({ length: Math.min(50, totalCount - startIndex) }, () => ({ role: 'assistant', content, createdAt: '2026-01-01T00:00:00Z' })) })
function fixture() {
  const requests: Array<{ action: string; payload: Record<string, unknown>; reply: (data: unknown) => void; fail: (error: string) => void }> = []
  window.cefQuery = ({ request, onSuccess, onFailure }) => {
    const parsed = JSON.parse(request)
    if (parsed.action === 'selectSession') { onSuccess('{}'); return }
    requests.push({ ...parsed, reply: (data) => onSuccess(JSON.stringify(data)), fail: (error) => onFailure(409, error) })
  }
  let state: AppState
  const get = () => state
  const set: ZustandSet = (input) => { state = { ...state, ...(typeof input === 'function' ? input(state) : input) } }
  state = { ...createSessionsSlice(set, get, true), sessions: [session('chat'), session('other')], activeSessionId: 'chat' } as AppState
  return { get, requests }
}
afterEach(() => { delete window.cefQuery; vi.restoreAllMocks() })
describe('search jump ownership', () => {
  it('opens a rail position using a digest-checked page and rejects invalid positions', async () => {
    const { get, requests } = fixture()
    Object.assign(get().sessions[0], { messageCount: 200, messagesDigest: 'snapshot' })
    expect(await get().jumpToSessionMessage('chat', -1)).toBe(false)
    expect(await get().jumpToSessionMessage('chat', 200)).toBe(false)
    expect(await get().jumpToSessionMessage('other', 0)).toBe(false)
    expect(requests).toHaveLength(0)
    const jump = get().jumpToSessionMessage('chat', 100)
    expect(requests[0].payload).toMatchObject({ targetMessageIndex: 100, limit: 50, expectedMessagesDigest: 'snapshot' })
    requests[0].reply(page())
    expect(await jump).toBe(true)
    expect(get().messages.chat).toHaveLength(50)
  })
  it('hydrates one verified bounded page, anchors once, and keeps composer text and attachments', async () => {
    const { get, requests } = fixture()
    const draft = { text: 'Unsent draft', attachments: [{ id: 'image', name: 'image.png', type: 'image', size: 12, path: '/tmp/image.png' }] }
    writeChatComposerDraft('chat', draft)
    const jump = get().jumpToChatSearchHit(hit(), 'query-1')
    expect(get().chatSearchNavigation?.phase).toBe('loading')
    get().loadSessionMessages('chat', true)
    expect(requests).toHaveLength(1)
    expect(requests[0].payload).toEqual({ chatId: 'chat', limit: 50, before: 126, deferToolCallContent: true, expectedMessagesDigest: 'snapshot', targetMessageIndex: 100 })
    requests[0].reply(page())
    expect(await jump).toBe(true)
    expect(get().messages.chat).toHaveLength(50)
    expect(get().historyStartIndexBySessionId.chat).toBe(80)
    const generation = get().chatSearchNavigation!.generation
    expect(get().consumeChatSearchJump('other', generation)).toBe(false)
    expect(get().consumeChatSearchJump('chat', generation)).toBe(true)
    expect(get().consumeChatSearchJump('chat', generation)).toBe(false)
    expect(readChatComposerDraft('chat')).toEqual(draft)
  })
  it('rejects stale query and selection responses without moving selection or replacing newer messages', async () => {
    const { get, requests } = fixture()
    const first = get().jumpToChatSearchHit(hit(), 'old-query')
    const second = get().jumpToChatSearchHit(hit(120), 'new-query')
    requests[1].reply(page(100, 200, 'new hit'))
    expect(await second).toBe(true)
    requests[0].reply(page(80, 200, 'old hit'))
    expect(await first).toBe(false)
    expect(get().messages.chat[0].content).toBe('new hit')
    const next = get().jumpToChatSearchHit(hit(), 'third-query')
    get().setActiveSession('other')
    requests[2].reply(page())
    expect(await next).toBe(false)
    expect(get().activeSessionId).toBe('other')
    expect(get().messages.chat[0].content).toBe('new hit')
  })
  it.each(['changed digest', 'missing index', 'native rejection'])('keeps cached history on %s', async (outcome) => {
    const { get, requests } = fixture()
    const original = get().messages
    const jump = get().jumpToChatSearchHit(hit(), 'query')
    if (outcome === 'native rejection') requests[0].fail('History changed')
    else requests[0].reply(outcome === 'changed digest' ? page(80, 200, 'needle', 'changed') : page(150))
    expect(await jump).toBe(false)
    expect(get().messages).toBe(original)
    expect(get().chatSearchNavigation).toBeNull()
    expect(get().chatHistoryErrorBySessionId.chat).toBeTruthy()
  })
  it('leaves navigation only for its own query and reloads a bounded tail without a generic hydration race', async () => {
    const { get, requests } = fixture()
    const jump = get().jumpToChatSearchHit(hit(), 'query')
    requests[0].reply(page()); await jump
    await get().clearChatSearchNavigation('foreign-query')
    expect(requests).toHaveLength(1)
    const clear = get().clearChatSearchNavigation('query')
    get().loadSessionMessages('chat', true)
    expect(requests).toHaveLength(2)
    expect(requests[1].payload).toEqual({ chatId: 'chat', limit: 50, deferToolCallContent: true })
    requests[1].reply(page(150, 200, 'tail', 'fresh'))
    await clear
    expect(get().chatSearchNavigation).toBeNull()
    expect(get().historyStartIndexBySessionId.chat).toBe(150)
    expect(get().messages.chat[0].content).toBe('tail')
  })
  it('does not let an old tail response overwrite a new search jump', async () => {
    const { get, requests } = fixture()
    const first = get().jumpToChatSearchHit(hit(), 'first')
    requests[0].reply(page()); await first
    const clear = get().clearChatSearchNavigation('first')
    const next = get().jumpToChatSearchHit(hit(), 'next')
    requests[2].reply(page(80, 200, 'new jump')); await next
    requests[1].reply(page(150, 200, 'old tail')); await clear
    expect(get().messages.chat[0].content).toBe('new jump')
    expect(get().chatSearchNavigation?.queryRequestId).toBe('next')
  })
  it('releases loading ownership and reports a thrown bridge failure without losing cached history', async () => {
    const { get } = fixture()
    const original = get().messages
    window.cefQuery = () => { throw new Error('Bridge unavailable') }
    await expect(get().jumpToChatSearchHit(hit(), 'query')).resolves.toBe(false)
    expect(get().chatSearchNavigation).toBeNull()
    expect(get().chatHistoryErrorBySessionId.chat).toBe('Bridge unavailable')
    expect(get().messages).toBe(original)
  })

})
