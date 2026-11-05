import { act } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, beforeEach, expect, it, vi } from 'vitest'
import type { Session } from '../../types/session'

type Envelope = { action: string; requestId?: string; payload?: Record<string, unknown> }
const bridge = vi.hoisted(() => ({
  cef: false,
  held: [] as { envelope: Envelope; resolve: (value: unknown) => void }[],
  send: vi.fn(),
}))
vi.mock('../../ipc/cefBridge', async (importOriginal) => ({
  ...await importOriginal<typeof import('../../ipc/cefBridge')>(),
  isCefContext: () => bridge.cef,
  isCompanionContext: () => false,
  sendToCEF: bridge.send,
}))
import { SessionItem } from './SessionItem'
import { useAppStore } from '../../store/useAppStore'

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

const initial = useAppStore.getState()
const createdAt = new Date('2026-10-01T08:00:00Z')
const openedAt = new Date('2026-10-02T08:00:00Z')
const chat = (id: string, extra: Partial<Session> = {}): Session => ({ id, name: id, viewMode: 'chat', folderId: null, providerId: 'codex-cli',
  createdAt, updatedAt: createdAt, lastOpenedAt: openedAt, executionHostId: 'local', workspaceDirectory: '/fixture', attentionRevision: 'attention-1', ...extra })
const reads = () => bridge.held.filter(({ envelope }) => envelope.action === 'getChatMessages')
const sent = (action: string) => bridge.send.mock.calls.filter(([envelope]) => (envelope as Envelope).action === action)
const page = (chatId: string) => ({ chatId, messagesDigest: `digest-${chatId}`, startIndex: 0, totalCount: 1,
  messages: [{ role: 'user', content: `Saved row for ${chatId}`, createdAt: '2026-10-01T08:00:00Z' }] })
const answer = async (index: number, error?: string) => {
  const { envelope, resolve } = reads()[index]
  await act(async () => {
    resolve(error ? { ok: false, requestId: envelope.requestId, error } : { ok: true, requestId: envelope.requestId, data: page(String(envelope.payload?.chatId)) })
    for (let tick = 0; tick < 5; tick += 1) await Promise.resolve()
  })
}
const flush = () => act(async () => { for (let tick = 0; tick < 5; tick += 1) await Promise.resolve() })
const row = (id: string) => host.querySelector<HTMLElement>(`[data-session-id="${id}"]`)!
const pointer = (id: string, type: 'pointerover' | 'pointerout') => act(() => { row(id).dispatchEvent(new MouseEvent(type, { bubbles: true })) })
const wait = (ms: number) => act(() => { vi.advanceTimersByTime(ms) })

let host: HTMLDivElement, root: Root
const render = (sessions: Session[]) => {
  useAppStore.setState({ sessions, activeSessionId: null, folders: [], messages: {}, chatHistoryReadBySessionId: {}, chatHistoryErrorBySessionId: {},
    acpBindingBySessionId: {}, cliBindingBySessionId: {}, chatSearchNavigation: null })
  act(() => root.render(<>{sessions.map(session => <SessionItem key={session.id} sessionId={session.id} session={session} familySessionIds={[session.id]} />)}</>))
}

beforeEach(() => {
  vi.useFakeTimers()
  vi.stubGlobal('localStorage', { getItem: () => null, setItem: () => {}, removeItem: () => {} })
  bridge.held = []
  bridge.cef = true
  bridge.send.mockReset().mockImplementation((envelope: Envelope) => envelope.action === 'getChatMessages'
    ? new Promise(resolve => bridge.held.push({ envelope, resolve }))
    : Promise.resolve({ ok: true, requestId: envelope.requestId, data: {} }))
  host = document.createElement('div'); document.body.append(host); root = createRoot(host)
})
afterEach(() => {
  act(() => root.unmount()); host.remove()
  for (const { envelope, resolve } of bridge.held) resolve({ ok: false, requestId: envelope.requestId, error: 'fixture cleanup' })
  bridge.cef = false
  useAppStore.setState(initial, true)
  vi.useRealTimers(); vi.unstubAllGlobals()
})

it('starts one history read after the hover delay without selecting the chat or touching attention', async () => {
  render([chat('hover-a')])
  pointer('hover-a', 'pointerover')
  wait(149)
  expect(reads()).toHaveLength(0)
  wait(1)
  expect(reads()).toHaveLength(1)
  // The warm-up asks native to use the lower-priority read lane.
  expect(reads()[0].envelope.payload).toMatchObject({ chatId: 'hover-a', limit: 50, background: true })
  wait(1000)
  expect(reads()).toHaveLength(1)
  await answer(0)
  const state = useAppStore.getState()
  expect(state.messages['hover-a']).toHaveLength(1)
  expect(state.activeSessionId).toBeNull()
  expect(state.sessions[0].lastOpenedAt).toBe(openedAt)
  expect(state.sessions[0].attentionRevision).toBe('attention-1')
  expect(sent('selectSession')).toHaveLength(0)
  expect(sent('acknowledgeChatAttention')).toHaveLength(0)
  expect(sent('setVisibleChatIds')).toHaveLength(0)
})

it('starts nothing when the pointer passes over a row quickly', () => {
  render([chat('pass-a')])
  pointer('pass-a', 'pointerover')
  wait(100)
  pointer('pass-a', 'pointerout')
  wait(1000)
  expect(reads()).toHaveLength(0)
})

it('starts nothing for a chat that is already loaded, already loading, selected, errored, remote or has queued input', () => {
  const skipped = ['loaded', 'selected', 'remote', 'errored', 'queued', 'running']
  render([chat('loaded'), chat('selected'), chat('remote', { executionHostId: 'ssh-host' }), chat('errored'), chat('queued'), chat('running')])
  useAppStore.setState({ messages: { loaded: [] }, activeSessionId: 'selected', chatHistoryErrorBySessionId: { errored: 'Saved chat history could not be read.' },
    acpBindingBySessionId: { queued: { queueHeld: true } as never, running: { processing: true } as never } })
  for (const id of skipped) { pointer(id, 'pointerover'); wait(200); pointer(id, 'pointerout') }
  expect(reads()).toHaveLength(0)
  render([chat('loading')])
  void useAppStore.getState().loadSessionMessages('loading')
  pointer('loading', 'pointerover'); wait(200)
  expect(reads()).toHaveLength(1)
})

it.each(['pendingDirectPrompts', 'unsentDirectPrompts'])('starts nothing for a stopped chat with a held direct prompt in %s', (field) => {
  // Native reads settle these admissions, so a hover must not start one. queuedPrompts is empty and queueHeld false here.
  render([chat('held')])
  useAppStore.setState({ acpBindingBySessionId: { held: { queueHeld: false, queuedPrompts: [], [field]: [{ id: 'prompt-1', revision: 1, text: 'Held' }] } as never } })
  pointer('held', 'pointerover'); wait(200)
  expect(reads()).toHaveLength(0)
})

it('starts a read when keyboard focus rests on a row', () => {
  render([chat('focus-a')])
  act(() => row('focus-a').focus())
  wait(150)
  expect(reads()).toHaveLength(1)
  expect(useAppStore.getState().activeSessionId).toBeNull()
})

it('keeps at most two preloads in flight while sweeping across many rows', () => {
  const sessions = Array.from({ length: 8 }, (_, index) => chat(`sweep-${index}`))
  render(sessions)
  for (const session of sessions) { pointer(session.id, 'pointerover'); wait(200); pointer(session.id, 'pointerout') }
  expect(reads()).toHaveLength(2)
})

it('lets a click during the preload reuse it and keep showing loading', async () => {
  render([chat('click-a', { messageCount: 1 })])
  pointer('click-a', 'pointerover'); wait(150)
  expect(reads()).toHaveLength(1)
  const prefetchRequestId = reads()[0].envelope.requestId
  // The real row click: selection, its backend reply, then the pane's ordinary load.
  act(() => row('click-a').click())
  await flush()
  act(() => { void useAppStore.getState().loadSessionMessages('click-a') })
  await flush()
  expect(sent('selectSession')).toHaveLength(1)
  expect(reads()).toHaveLength(1)
  expect(useAppStore.getState().chatHistoryReadBySessionId['click-a']?.requestId).toBe(prefetchRequestId)
  expect(useAppStore.getState().chatHistoryErrorBySessionId['click-a']).toBeUndefined()
  await answer(0)
  expect(useAppStore.getState().messages['click-a']).toHaveLength(1)
  expect(useAppStore.getState().chatHistoryReadBySessionId['click-a']).toBeUndefined()
  expect(useAppStore.getState().chatHistoryErrorBySessionId['click-a']).toBeUndefined()
})

it('moves a joined preload ahead of queued background work and keeps one read', () => {
  render([chat('promote-a', { messageCount: 1 })])
  pointer('promote-a', 'pointerover'); wait(150)
  expect(sent('promoteChatHistoryRead')).toHaveLength(0)
  act(() => { void useAppStore.getState().loadSessionMessages('promote-a') })
  expect(reads()).toHaveLength(1)
  expect(sent('promoteChatHistoryRead').map(([envelope]) => (envelope as Envelope).payload)).toEqual([{ chatId: 'promote-a' }])
})

it('shows the error when a preload the click joined fails', async () => {
  render([chat('fail-a', { messageCount: 1 })])
  pointer('fail-a', 'pointerover'); wait(150)
  act(() => row('fail-a').click())
  await flush()
  act(() => { void useAppStore.getState().loadSessionMessages('fail-a') })
  await answer(0, 'Saved chat history could not be read.')
  expect(reads()).toHaveLength(1)
  expect(useAppStore.getState().chatHistoryErrorBySessionId['fail-a']).toBe('Saved chat history could not be read.')
  expect(sent('releaseChatMessages')).toHaveLength(0)
})

it('starts a fresh read instead of joining when the saved summary changed after the preload began', () => {
  render([chat('changed-a', { messagesDigest: 'summary-1', messageCount: 1 })])
  pointer('changed-a', 'pointerover'); wait(150)
  useAppStore.setState({ sessions: [chat('changed-a', { messagesDigest: 'summary-2', messageCount: 2 })] })
  act(() => { void useAppStore.getState().loadSessionMessages('changed-a') })
  expect(reads()).toHaveLength(2)
  expect(reads()[1].envelope.payload?.background).toBeUndefined()
})

it('releases the native copy of a preload that ends without adopting a page', async () => {
  render([chat('dropped-a')])
  pointer('dropped-a', 'pointerover'); wait(150)
  await answer(0, 'Saved chat history could not be read.')
  expect(sent('releaseChatMessages')).toHaveLength(1)
})

it('never evicts a chat another load claimed while its preload was in flight', async () => {
  const sessions = Array.from({ length: 6 }, (_, index) => chat(`claim-${index}`))
  render(sessions)
  pointer('claim-0', 'pointerover'); wait(150); pointer('claim-0', 'pointerout')
  // A pane opening the chat (for example a drop) loads it without selecting it.
  act(() => { void useAppStore.getState().loadSessionMessages('claim-0') })
  await answer(0)
  for (let index = 1; index < sessions.length; index += 1) {
    pointer(sessions[index].id, 'pointerover'); wait(150); pointer(sessions[index].id, 'pointerout')
    await answer(index)
  }
  const { messages } = useAppStore.getState()
  expect(messages['claim-0']).toHaveLength(1)
  expect(messages['claim-1']).toBeUndefined()
  expect(sessions.slice(2).every(session => messages[session.id]?.length === 1)).toBe(true)
})

it('evicts a preload that was processing as soon as it stops', async () => {
  const sessions = Array.from({ length: 5 }, (_, index) => chat(`busy-${index}`))
  render(sessions)
  const processing = (ids: string[]) => useAppStore.setState({ acpBindingBySessionId: Object.fromEntries(ids.map(id => [id, { processing: true }])) as never })
  for (let index = 0; index < 4; index += 1) {
    pointer(sessions[index].id, 'pointerover'); wait(150); pointer(sessions[index].id, 'pointerout')
    await answer(index)
  }
  pointer('busy-4', 'pointerover'); wait(150); pointer('busy-4', 'pointerout')
  processing(sessions.map(session => session.id))
  await answer(4)
  // Unload refuses processing chats, so all five stay loaded and tracked.
  expect(sessions.every(session => useAppStore.getState().messages[session.id]?.length === 1)).toBe(true)
  expect(sent('releaseChatMessages')).toHaveLength(0)
  processing(sessions.slice(1).map(session => session.id))
  await flush()
  expect(useAppStore.getState().messages['busy-0']).toBeUndefined()
  expect(sessions.slice(1).every(session => useAppStore.getState().messages[session.id]?.length === 1)).toBe(true)
  expect(sent('releaseChatMessages').map(([envelope]) => (envelope as Envelope).payload)).toEqual([{ chatId: 'busy-0' }])
})

it('keeps a preload evictable after the forced reload that follows a turn', async () => {
  const sessions = Array.from({ length: 5 }, (_, index) => chat(`forced-${index}`))
  render(sessions)
  pointer('forced-0', 'pointerover'); wait(150); pointer('forced-0', 'pointerout')
  await answer(0)
  act(() => { void useAppStore.getState().loadSessionMessages('forced-0', true) })
  await answer(1)
  for (let index = 1; index < sessions.length; index += 1) {
    pointer(sessions[index].id, 'pointerover'); wait(150); pointer(sessions[index].id, 'pointerout')
    await answer(index + 1)
  }
  expect(useAppStore.getState().messages['forced-0']).toBeUndefined()
  expect(sent('releaseChatMessages')).toHaveLength(1)
})

it('evicts the oldest unopened preload beyond four retained chats', async () => {
  const sessions = Array.from({ length: 5 }, (_, index) => chat(`keep-${index}`))
  render(sessions)
  for (let index = 0; index < sessions.length; index += 1) {
    pointer(sessions[index].id, 'pointerover'); wait(150); pointer(sessions[index].id, 'pointerout')
    await answer(index)
  }
  const { messages } = useAppStore.getState()
  expect(messages['keep-0']).toBeUndefined()
  expect(sessions.slice(1).every(session => messages[session.id]?.length === 1)).toBe(true)
  expect(sent('releaseChatMessages')).toHaveLength(1)
})
