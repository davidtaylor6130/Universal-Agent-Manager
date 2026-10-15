import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, expect, it, vi } from 'vitest'
import { useCountdownClock } from './useCountdownClock'

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
afterEach(() => vi.useRealTimers())

it('realigns a late tick to the next displayed-second boundary', () => {
  vi.useFakeTimers()
  vi.setSystemTime(1000)
  function Clock() { return <span>{useCountdownClock(true, 1000, 10_000)}</span> }
  const host = document.createElement('div')
  const root = createRoot(host)
  try {
    act(() => root.render(<Clock />))
    vi.setSystemTime(1300)
    act(() => vi.advanceTimersByTime(1000))
    expect(host.textContent).toBe('2300')
    act(() => vi.advanceTimersByTime(700))
    expect(host.textContent).toBe('3000')
  } finally {
    act(() => root.unmount())
  }
})
