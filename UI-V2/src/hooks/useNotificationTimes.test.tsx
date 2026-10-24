import { act, StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, expect, it, vi } from 'vitest'
import { useNotificationClock, useNotificationTimes } from './useNotificationTimes'
import type { NotificationTimeSource } from '../utils/notificationTime'

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
afterEach(() => { vi.useRealTimers(); vi.restoreAllMocks() })

it('keeps first observation across rerenders and resets observations in a new shell session', () => {
  vi.useFakeTimers()
  vi.setSystemTime(1000)
  const host = document.createElement('div')
  let root = createRoot(host)
  function Shell({ sources }: { sources: NotificationTimeSource[] }) {
    const times = useNotificationTimes(sources)
    return <output>{JSON.stringify([...times])}</output>
  }
  const render = (sources: NotificationTimeSource[]) => act(() => root.render(<StrictMode><Shell sources={sources} /></StrictMode>))
  try {
    render([{ id: 'status' }])
    expect(host.textContent).toBe('[["status",1000]]')
    vi.setSystemTime(5000)
    render([{ id: 'status' }, { id: 'failure', time: new Date(0).toISOString() }])
    expect(host.textContent).toBe('[["status",1000],["failure",0]]')
    vi.setSystemTime(10_000)
    render([{ id: 'status' }, { id: 'failure', time: new Date(0).toISOString() }])
    expect(host.textContent).toBe('[["status",1000],["failure",0]]')
    act(() => root.unmount())
    root = createRoot(host)
    render([{ id: 'status' }])
    expect(host.textContent).toBe('[["status",10000]]')
  } finally {
    act(() => root.unmount())
  }
})

it('refreshes the open clock on ticks, focus and visibility and stops after disabling or unmounting', () => {
  vi.useFakeTimers()
  vi.setSystemTime(1000)
  const host = document.createElement('div')
  const root = createRoot(host)
  function Clock({ enabled }: { enabled: boolean }) {
    return <output>{useNotificationClock(enabled)}</output>
  }
  try {
    act(() => root.render(<Clock enabled />))
    act(() => vi.advanceTimersByTime(30_000))
    expect(host.textContent).toBe('31000')
    vi.setSystemTime(100_000)
    act(() => window.dispatchEvent(new Event('focus')))
    expect(host.textContent).toBe('100000')
    vi.setSystemTime(200_000)
    vi.spyOn(document, 'hidden', 'get').mockReturnValue(false)
    act(() => document.dispatchEvent(new Event('visibilitychange')))
    expect(host.textContent).toBe('200000')
    act(() => root.render(<Clock enabled={false} />))
    act(() => vi.advanceTimersByTime(60_000))
    act(() => window.dispatchEvent(new Event('focus')))
    act(() => document.dispatchEvent(new Event('visibilitychange')))
    expect(host.textContent).toBe('200000')
    expect(vi.getTimerCount()).toBe(0)
    act(() => root.render(<Clock enabled />))
    expect(host.textContent).toBe('260000')
  } finally {
    act(() => root.unmount())
  }
  expect(vi.getTimerCount()).toBe(0)
})
