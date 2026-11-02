import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, beforeEach, expect, it, vi } from 'vitest'
import type { Session } from '../../types/session'

type Envelope = { action: string; requestId?: string; payload?: Record<string, unknown> }
// A cold native page read; #600 measured about 1.4 s for real old chats.
const READ_MS = 400
const bridge = vi.hoisted(() => ({ cef: false, send: vi.fn() }))
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
const sleep = (ms: number) => act(() => new Promise<void>(resolve => setTimeout(resolve, ms)))

// Native history reads share one file thread that runs one read at a time; queued user-blocking reads start before
// queued user-visible (background) ones, but a started read always finishes first.
let lane: { busy: boolean; queue: { background: boolean; run: () => void }[] }
const pump = () => {
  if (lane.busy || lane.queue.length === 0) return
  const blocking = lane.queue.findIndex(job => !job.background)
  const [job] = lane.queue.splice(blocking >= 0 ? blocking : 0, 1)
  lane.busy = true
  setTimeout(() => { job.run(); lane.busy = false; pump() }, READ_MS)
}

beforeEach(() => {
  vi.stubGlobal('localStorage', { getItem: () => null, setItem: () => {}, removeItem: () => {} })
  bridge.cef = true
  lane = { busy: false, queue: [] }
  bridge.send.mockReset().mockImplementation((envelope: Envelope) => new Promise(resolve => {
    const chatId = String(envelope.payload?.chatId ?? '')
    if (envelope.action !== 'getChatMessages') return resolve({ ok: true, requestId: envelope.requestId, data: {} })
    lane.queue.push({ background: envelope.payload?.background === true, run: () => resolve({ ok: true, requestId: envelope.requestId,
      data: { chatId, messagesDigest: `digest-${chatId}`, startIndex: 0, totalCount: 1, messages: [{ role: 'user', content: 'Saved row', createdAt: '2026-10-01T08:00:00Z' }] } }) })
    pump()
  }))
})
afterEach(() => { bridge.cef = false; useAppStore.setState(initial, true); vi.unstubAllGlobals() })

/** Render real sidebar rows, optionally hover some first, then click one and measure until its first history page is in the store. */
async function openMs(target: string, hovered: string[] = []) {
  const ids = [...new Set([...hovered, target])]
  const sessions: Session[] = ids.map(id => ({ id, name: id, viewMode: 'chat', folderId: null, providerId: 'codex-cli', createdAt, updatedAt: createdAt,
    executionHostId: 'local', workspaceDirectory: '/fixture', messageCount: 1 }))
  useAppStore.setState({ sessions, activeSessionId: null, folders: [], messages: {}, chatHistoryReadBySessionId: {},
    chatHistoryErrorBySessionId: {}, acpBindingBySessionId: {}, cliBindingBySessionId: {}, chatSearchNavigation: null })
  const host = document.createElement('div'); document.body.append(host); const root = createRoot(host)
  const row = (id: string) => host.querySelector<HTMLElement>(`[data-session-id="${id}"]`)!
  try {
    act(() => root.render(<>{sessions.map(session => <SessionItem key={session.id} sessionId={session.id} session={session} familySessionIds={[session.id]} />)}</>))
    for (const id of hovered) {
      act(() => { row(id).dispatchEvent(new MouseEvent('pointerover', { bubbles: true })) })
      await sleep(id === target ? 300 : 160) // a typical dwell before clicking, or a brief rest while moving on
      if (id !== target) act(() => { row(id).dispatchEvent(new MouseEvent('pointerout', { bubbles: true })) })
    }
    const started = performance.now()
    act(() => row(target).click())
    while (useAppStore.getState().messages[target] === undefined) await sleep(5)
    const elapsed = performance.now() - started
    expect(useAppStore.getState().chatHistoryErrorBySessionId[target]).toBeUndefined()
    return elapsed
  } finally { act(() => root.unmount()); host.remove() }
}

const historyReads = () => bridge.send.mock.calls.filter(([envelope]) => (envelope as Envelope).action === 'getChatMessages').length

it('opens a hovered cold chat noticeably faster than clicking it without hovering', async () => {
  const cold = await openMs('cold-chat')
  const warm = await openMs('warm-chat', ['warm-chat'])
  process.stdout.write(`[perf] cold-chat open after click: ${cold.toFixed(0)} ms without hover, ${warm.toFixed(0)} ms after a 300 ms hover\n`)
  expect(cold).toBeGreaterThanOrEqual(READ_MS - 10)
  expect(warm).toBeLessThan(cold - 100)
  expect(historyReads()).toBe(2)
})

it('does not make an unhovered click wait behind queued hover warm-ups', async () => {
  // Two warm-ups run (one started, one queued) when the user clicks a different, unhovered chat.
  const contended = await openMs('clicked-chat', ['passed-a', 'passed-b'])
  process.stdout.write(`[perf] unhovered open with two warm-ups pending: ${contended.toFixed(0)} ms\n`)
  expect(historyReads()).toBe(3)
  // At most the started warm-up's remainder plus its own read; never behind both warm-ups.
  expect(contended).toBeLessThan(2 * READ_MS)
})
