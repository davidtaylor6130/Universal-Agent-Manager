import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, beforeEach, expect, it, vi } from 'vitest'
import { useNow } from './useNow'

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

let renders = 0
function Clock({ active }: { active: boolean }) {
  renders += 1
  return <span>{useNow(active)}</span>
}

const setVisibility = (state: DocumentVisibilityState) => {
  Object.defineProperty(document, 'visibilityState', { configurable: true, value: state })
  act(() => { document.dispatchEvent(new Event('visibilitychange')) })
}

beforeEach(() => { renders = 0; vi.useFakeTimers() })
afterEach(() => {
  Reflect.deleteProperty(document, 'visibilityState')
  vi.useRealTimers()
})

it('ticks once a second while active, pauses while hidden and catches up when visible', () => {
  const host = document.createElement('div')
  const root = createRoot(host)
  const start = Date.now()
  act(() => root.render(<Clock active />))
  act(() => { vi.advanceTimersByTime(2000) })
  expect(Number(host.textContent)).toBe(start + 2000)

  setVisibility('hidden')
  const hiddenRenders = renders
  act(() => { vi.advanceTimersByTime(10_000) })
  expect(renders).toBe(hiddenRenders)
  expect(vi.getTimerCount()).toBe(0)

  setVisibility('visible')
  expect(Number(host.textContent)).toBe(start + 12_000)
  act(() => { vi.advanceTimersByTime(1000) })
  expect(Number(host.textContent)).toBe(start + 13_000)
  act(() => root.unmount())
})

it('does not tick while inactive', () => {
  const host = document.createElement('div')
  const root = createRoot(host)
  act(() => root.render(<Clock active={false} />))
  expect(vi.getTimerCount()).toBe(0)
  setVisibility('visible')
  const before = renders
  act(() => { vi.advanceTimersByTime(5000) })
  expect(renders).toBe(before)
  act(() => root.unmount())
})
