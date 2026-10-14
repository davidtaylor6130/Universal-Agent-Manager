import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { SnoozeMenu, customSnoozeUntil, snoozePresets } from './SnoozeMenu'
import { useAppStore } from '../../store/useAppStore'

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
const save = useAppStore.getState().setChatSnoozedUntil
beforeEach(() => { vi.restoreAllMocks(); delete window.cefQuery; useAppStore.setState({ acpBindingBySessionId: {}, setChatSnoozedUntil: save }) })
afterEach(() => { delete window.cefQuery })
function render(untilMs = 0) {
  const host = document.createElement('div'); document.body.appendChild(host)
  const root = createRoot(host)
  act(() => root.render(<SnoozeMenu chatId="chat" familyIds={['chat']} untilMs={untilMs} />))
  const button = (label: string) => [...host.querySelectorAll<HTMLButtonElement>('button')].find((item) => item.textContent?.startsWith(label))!
  return { host, root, button }
}
function setField(element: HTMLInputElement | HTMLSelectElement, value: string) {
  const proto = element instanceof HTMLSelectElement ? HTMLSelectElement.prototype : HTMLInputElement.prototype
  Object.getOwnPropertyDescriptor(proto, 'value')!.set!.call(element, value)
  element.dispatchEvent(new Event(element instanceof HTMLSelectElement ? 'change' : 'input', { bubbles: true }))
}

describe('snooze menu', () => {
  it('offers the quick presets in order, using local calendar time for tomorrow morning', () => {
    const now = new Date(2030, 3, 1, 10) // Monday
    const presets = snoozePresets(now)
    expect(presets.map((preset) => preset.label)).toEqual(['5 minutes', '10 minutes', '15 minutes', '30 minutes', '1 hour', '3 hours', 'Tomorrow morning', '1 day', '2 days', '1 week'])
    const tomorrow = new Date(presets.find((preset) => preset.label === 'Tomorrow morning')!.until)
    expect([tomorrow.getDate(), tomorrow.getHours()]).toEqual([2, 9])
    expect(presets.find((preset) => preset.label === '1 week')!.until - now.getTime()).toBe(7 * 86_400_000)
  })

  it('turns a custom amount and unit into a future wake time and rejects unusable amounts', () => {
    const now = 1_000_000_000_000
    expect(customSnoozeUntil(90, 'minutes', now)).toBe(now + 90 * 60_000)
    expect(customSnoozeUntil(2, 'days', now)).toBe(now + 2 * 86_400_000)
    expect(customSnoozeUntil(0, 'hours', now)).toBeNull()
    expect(customSnoozeUntil(1.5, 'hours', now)).toBeNull()
    expect(customSnoozeUntil(Number.NaN, 'hours', now)).toBeNull()
  })

  it('reports save failure and retries the exact native request without changing the provider', async () => {
    const requests: unknown[] = []; let fail = true
    window.cefQuery = ({ request, onSuccess, onFailure }) => {
      requests.push(JSON.parse(request)); if (fail) onFailure(409, 'Could not save wake time'); else onSuccess('{}')
    }
    const { host, root, button } = render()
    try {
      await act(async () => button('1 hour').click())
      expect(host.querySelector('[role=alert]')?.textContent).toBe('Could not save wake time')
      expect(requests[0]).toMatchObject({ action: 'setChatSnoozedUntil', payload: { chatId: 'chat', untilMs: expect.any(Number) } })
      fail = false; await act(async () => button('1 hour').click())
      expect(host.querySelector('[role=status]')?.textContent).toContain('Snoozed until')
      expect(requests).toHaveLength(2)
    } finally { act(() => root.unmount()); host.remove() }
  })

  it('snoozes for a custom amount in the selected unit', async () => {
    const requests: { payload: { untilMs: number } }[] = []
    window.cefQuery = ({ request, onSuccess }) => { requests.push(JSON.parse(request)); onSuccess('{}') }
    const { host, root, button } = render()
    try {
      act(() => button('Custom…').click())
      const amount = host.querySelector<HTMLInputElement>('input[aria-label="Snooze amount"]')!
      const unit = host.querySelector<HTMLSelectElement>('select[aria-label="Snooze unit"]')!
      act(() => setField(amount, '3'))
      act(() => setField(unit, 'days'))
      const before = Date.now()
      await act(async () => button('Snooze').click())
      expect(requests).toHaveLength(1)
      expect(requests[0].payload.untilMs).toBeGreaterThanOrEqual(before + 3 * 86_400_000)
      expect(requests[0].payload.untilMs).toBeLessThanOrEqual(Date.now() + 3 * 86_400_000)
    } finally { act(() => root.unmount()); host.remove() }
  })

  it('wakes with zero even when runtime attention blocks a new snooze', async () => {
    const requests: unknown[] = []
    window.cefQuery = ({ request, onSuccess }) => { requests.push(JSON.parse(request)); onSuccess('{}') }
    useAppStore.setState({ acpBindingBySessionId: { chat: { attentionKind: 'permission' } } as never })
    const { root, host, button } = render(Date.now() + 60_000)
    try {
      expect(button('Wake now').disabled).toBe(false)
      await act(async () => button('Wake now').click())
      expect(requests[0]).toMatchObject({ action: 'setChatSnoozedUntil', payload: { chatId: 'chat', untilMs: 0 } })
    } finally { act(() => root.unmount()); host.remove() }
  })

  it('does not hide pending input or a queued turn behind a snooze', () => {
    useAppStore.setState({ acpBindingBySessionId: { chat: { queuedPrompts: [{}] } } as never })
    const { root, host, button } = render()
    try {
      expect(button('1 hour').disabled).toBe(true)
      expect(host.textContent).toContain('Respond to pending requests')
    }
    finally { act(() => root.unmount()); host.remove() }
  })

  it('rejects an unusable custom amount before contacting native code', async () => {
    const native = vi.fn(); window.cefQuery = native
    const { root, host, button } = render()
    try {
      act(() => button('Custom…').click())
      act(() => setField(host.querySelector<HTMLInputElement>('input[aria-label="Snooze amount"]')!, '0'))
      await act(async () => button('Snooze').click())
      expect(host.querySelector('[role=alert]')?.textContent).toBe('Enter a whole number of minutes, hours or days.')
      expect(native).not.toHaveBeenCalled()
    } finally { act(() => root.unmount()); host.remove() }
  })
})
