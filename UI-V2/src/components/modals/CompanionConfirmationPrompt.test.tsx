import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, describe, expect, it, vi } from 'vitest'
import { parseUamPushPayload } from '../../store/push/uamPush'
import { APP_FAILURE_EVENT } from '../../utils/appFailure'

const bridge = vi.hoisted(() => ({ send: vi.fn() }))
vi.mock('../../ipc/cefBridge', async (original) => ({ ...await original<typeof import('../../ipc/cefBridge')>(), isCompanionContext: () => false, sendToCEF: bridge.send }))
import { COMPANION_CONFIRMATIONS_EVENT, CompanionConfirmationPrompt } from './CompanionConfirmationPrompt'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

const later = () => Date.now() + 60_000
const items = () => [
  { id: 'confirm-1', chatId: 'chat-a', chatTitle: 'Release notes', description: 'Change permission mode to YOLO', expiresAt: later() },
  { id: 'confirm-2', chatId: '', chatTitle: '', description: 'Change provider chat defaults', expiresAt: later() },
]

async function mount() {
  const host = document.createElement('div')
  document.body.append(host)
  const root = createRoot(host)
  await act(async () => root.render(<CompanionConfirmationPrompt />))
  return { dialog: () => document.querySelector('[role="alertdialog"]'), close: () => { act(() => root.unmount()); host.remove() } }
}

const push = (list: unknown[]) => act(async () => { window.dispatchEvent(new CustomEvent(COMPANION_CONFIRMATIONS_EVENT, { detail: { items: list } })) })
const button = (label: string) => Array.from(document.querySelectorAll('button')).find((item) => item.textContent === label)!

afterEach(() => { bridge.send.mockReset(); vi.useRealTimers() })

describe('CompanionConfirmationPrompt', () => {
  it('stays closed until the main device pushes a held phone request', async () => {
    const view = await mount()
    expect(view.dialog()).toBeNull()
    await push(items())
    expect(view.dialog()?.textContent).toContain('Change permission mode to YOLO in “Release notes”.')
    view.close()
  })

  it('allows and denies each request in turn by id', async () => {
    bridge.send.mockResolvedValue({ ok: true, data: {} })
    const view = await mount()
    await push(items())
    await act(async () => button('Allow').click())
    expect(bridge.send).toHaveBeenLastCalledWith({ action: 'resolveCompanionConfirmation', payload: { id: 'confirm-1', allow: true } })
    expect(view.dialog()?.textContent).toContain('Change provider chat defaults.')
    await act(async () => button('Deny').click())
    expect(bridge.send).toHaveBeenLastCalledWith({ action: 'resolveCompanionConfirmation', payload: { id: 'confirm-2', allow: false } })
    expect(view.dialog()).toBeNull()
    view.close()
  })

  it('reports a failed allow and hides expired requests', async () => {
    bridge.send.mockResolvedValue({ ok: false, error: 'This phone request expired or was already answered.' })
    const failures: string[] = []
    const onFailure = (event: Event) => failures.push((event as CustomEvent<{ detail: string }>).detail.detail)
    window.addEventListener(APP_FAILURE_EVENT, onFailure)
    const view = await mount()
    await push(items().slice(0, 1))
    await act(async () => button('Allow').click())
    expect(failures).toEqual(['This phone request expired or was already answered.'])
    window.removeEventListener(APP_FAILURE_EVENT, onFailure)
    vi.useFakeTimers()
    await push([{ ...items()[0], expiresAt: Date.now() + 1000 }])
    expect(view.dialog()).not.toBeNull()
    await act(async () => { vi.advanceTimersByTime(1001) })
    expect(view.dialog()).toBeNull()
    view.close()
  })

  it('parses only well-formed confirmation pushes', () => {
    const parsed = parseUamPushPayload({ type: 'companionConfirmations', items: [{ ...items()[0], payload: { commandSafetyTier: 'yolo' } }] })
    expect(parsed.ok && parsed.message).toEqual({ type: 'companionConfirmations', items: [{ ...items()[0], expiresAt: expect.any(Number) }] })
    expect(parseUamPushPayload({ type: 'companionConfirmations', items: [{ id: 'x' }] }).ok).toBe(false)
    expect(parseUamPushPayload({ type: 'companionConfirmations' }).ok).toBe(false)
  })
})
