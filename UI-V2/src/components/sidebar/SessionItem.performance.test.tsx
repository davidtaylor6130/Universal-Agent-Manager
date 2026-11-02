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

beforeEach(() => {
  vi.stubGlobal('localStorage', { getItem: () => null, setItem: () => {}, removeItem: () => {} })
  bridge.cef = true
  bridge.send.mockReset().mockImplementation((envelope: Envelope) => new Promise(resolve => {
    const chatId = String(envelope.payload?.chatId ?? '')
    if (envelope.action !== 'getChatMessages') return resolve({ ok: true, requestId: envelope.requestId, data: {} })
    setTimeout(() => resolve({ ok: true, requestId: envelope.requestId, data: { chatId, messagesDigest: `digest-${chatId}`, startIndex: 0, totalCount: 1,
      messages: [{ role: 'user', content: 'Saved row', createdAt: '2026-10-01T08:00:00Z' }] } }), READ_MS)
  }))
})
afterEach(() => { bridge.cef = false; useAppStore.setState(initial, true); vi.unstubAllGlobals() })

/** Click a real sidebar row and measure until the chat's first history page is in the store. */
async function openMs(hoverFirst: boolean) {
  const id = hoverFirst ? 'warm-chat' : 'cold-chat'
  const session: Session = { id, name: id, viewMode: 'chat', folderId: null, providerId: 'codex-cli', createdAt, updatedAt: createdAt,
    executionHostId: 'local', workspaceDirectory: '/fixture', messageCount: 1 }
  useAppStore.setState({ sessions: [session], activeSessionId: null, folders: [], messages: {}, chatHistoryReadBySessionId: {},
    chatHistoryErrorBySessionId: {}, acpBindingBySessionId: {}, cliBindingBySessionId: {}, chatSearchNavigation: null })
  const host = document.createElement('div'); document.body.append(host); const root = createRoot(host)
  try {
    act(() => root.render(<SessionItem sessionId={id} session={session} familySessionIds={[id]} />))
    const row = host.querySelector<HTMLElement>(`[data-session-id="${id}"]`)!
    if (hoverFirst) {
      act(() => { row.dispatchEvent(new MouseEvent('pointerover', { bubbles: true })) })
      await sleep(300) // a typical dwell before clicking
    }
    const started = performance.now()
    act(() => row.click())
    while (useAppStore.getState().messages[id] === undefined) await sleep(5)
    return performance.now() - started
  } finally { act(() => root.unmount()); host.remove() }
}

it('opens a hovered cold chat noticeably faster than clicking it without hovering', async () => {
  const cold = await openMs(false)
  const warm = await openMs(true)
  process.stdout.write(`[perf] cold-chat open after click: ${cold.toFixed(0)} ms without hover, ${warm.toFixed(0)} ms after a 300 ms hover\n`)
  expect(cold).toBeGreaterThanOrEqual(READ_MS - 10)
  expect(warm).toBeLessThan(cold - 100)
  expect(bridge.send.mock.calls.filter(([envelope]) => (envelope as Envelope).action === 'getChatMessages')).toHaveLength(2)
})
