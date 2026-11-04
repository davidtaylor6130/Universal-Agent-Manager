import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { describe, expect, it, vi } from 'vitest'
import type { Session } from '../../types/session'
import { useSnoozeClock } from './useSnoozeClock'

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
it('owns one timer for the nearest expiry and releases it after the last wake', () => {
  vi.useFakeTimers(); vi.setSystemTime(1_800_000_000_000)
  const now = Date.now(); const sessions = [{ snoozedUntilMs: now + 1000 }, { snoozedUntilMs: now + 2000 }] as Session[]
  function Clock() { return <span>{useSnoozeClock(sessions)}</span> }
  const host = document.createElement('div'); const root = createRoot(host)
  try {
    act(() => root.render(<Clock />)); expect(vi.getTimerCount()).toBe(1)
    act(() => vi.advanceTimersByTime(1001)); expect(host.textContent).toBe(String(now + 1001)); expect(vi.getTimerCount()).toBe(1)
    act(() => vi.advanceTimersByTime(1000)); expect(host.textContent).toBe(String(now + 2001)); expect(vi.getTimerCount()).toBe(0)
    act(() => window.dispatchEvent(new Event('focus'))); expect(vi.getTimerCount()).toBe(0)
  } finally { act(() => root.unmount()); vi.useRealTimers() }
})
