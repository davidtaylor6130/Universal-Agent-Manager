import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { SnoozeMenu, snoozePresets } from './SnoozeMenu'
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

describe('snooze menu', () => {
  it('uses local calendar presets and keeps next Monday in the future', () => {
    const presets = snoozePresets(new Date(2030, 3, 1, 10)) // Monday
    expect(presets.map((preset) => preset.label)).toEqual(['1 hour', '3 hours', 'This evening', 'Tomorrow', 'Next week'])
    expect(new Date(presets.at(-1)!.until).getDate()).toBe(8)
    expect(new Date(presets.at(-1)!.until).getHours()).toBe(9)
    expect(snoozePresets(new Date(2030, 3, 1, 17, 30)).some((preset) => preset.label === 'This evening')).toBe(false)
  })

  it('reports save failure and retries the exact native request without changing the provider', async () => {
    const requests: unknown[] = []; let fail = true
    window.cefQuery = ({ request, onSuccess, onFailure }) => {
      requests.push(JSON.parse(request)); if (fail) onFailure(409, 'Could not save wake time'); else onSuccess('{}')
    }
    const { host, root, button } = render()
    try {
      act(() => button('Snooze').click())
      await act(async () => button('1 hour').click())
      expect(host.querySelector('[role=alert]')?.textContent).toBe('Could not save wake time')
      expect(requests[0]).toMatchObject({ action: 'setChatSnoozedUntil', payload: { chatId: 'chat', untilMs: expect.any(Number) } })
      fail = false; await act(async () => button('1 hour').click())
      expect(host.querySelector('[role=status]')?.textContent).toContain('Snoozed until')
      expect(requests).toHaveLength(2)
    } finally { act(() => root.unmount()); host.remove() }
  })

  it('wakes with zero even when runtime attention blocks a new snooze', async () => {
    const requests: unknown[] = []
    window.cefQuery = ({ request, onSuccess }) => { requests.push(JSON.parse(request)); onSuccess('{}') }
    useAppStore.setState({ acpBindingBySessionId: { chat: { attentionKind: 'permission' } } as never })
    const { root, host, button } = render(Date.now() + 60_000)
    try {
      expect(button('Wake chat').disabled).toBe(false)
      await act(async () => button('Wake chat').click())
      expect(requests[0]).toMatchObject({ action: 'setChatSnoozedUntil', payload: { chatId: 'chat', untilMs: 0 } })
    } finally { act(() => root.unmount()); host.remove() }
  })

  it('does not hide pending input or a queued turn behind a snooze', () => {
    useAppStore.setState({ acpBindingBySessionId: { chat: { queuedPrompts: [{}] } } as never })
    const { root, host, button } = render()
    try { expect(button('Snooze').disabled).toBe(true) }
    finally { act(() => root.unmount()); host.remove() }
  })

  it('rejects a past custom time before contacting native code', async () => {
    const native = vi.fn(); window.cefQuery = native
    const { root, host, button } = render()
    try {
      act(() => button('Snooze').click())
      const input = host.querySelector<HTMLInputElement>('input')!
      act(() => {
        Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(input, '2000-01-01T12:00')
        input.dispatchEvent(new Event('input', { bubbles: true }))
      })
      await act(async () => button('Set wake time').click())
      expect(host.querySelector('[role=alert]')?.textContent).toBe('Choose a future wake time.')
      expect(native).not.toHaveBeenCalled()
    } finally { act(() => root.unmount()); host.remove() }
  })
})
