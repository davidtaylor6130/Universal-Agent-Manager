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
const answer = async (index: number) => {
  const { envelope, resolve } = reads()[index]
  await act(async () => { resolve({ ok: true, requestId: envelope.requestId, data: page(String(envelope.payload?.chatId)) }); await Promise.resolve() })
}
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
  expect(reads()[0].envelope.payload).toMatchObject({ chatId: 'hover-a', limit: 50 })
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

it('starts nothing for a chat that is already loaded, already loading, selected or remote', () => {
  render([chat('loaded'), chat('selected'), chat('remote', { executionHostId: 'ssh-host' })])
  useAppStore.setState({ messages: { loaded: [] }, activeSessionId: 'selected' })
  for (const id of ['loaded', 'selected', 'remote']) { pointer(id, 'pointerover'); wait(200); pointer(id, 'pointerout') }
  expect(reads()).toHaveLength(0)
  render([chat('loading')])
  void useAppStore.getState().loadSessionMessages('loading')
  pointer('loading', 'pointerover'); wait(200)
  expect(reads()).toHaveLength(1)
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

it('lets the click reuse the preload already in flight', async () => {
  render([chat('click-a')])
  pointer('click-a', 'pointerover'); wait(150)
  expect(reads()).toHaveLength(1)
  act(() => { useAppStore.getState().setActiveSession('click-a'); void useAppStore.getState().loadSessionMessages('click-a') })
  expect(reads()).toHaveLength(1)
  await answer(0)
  expect(useAppStore.getState().messages['click-a']).toHaveLength(1)
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
