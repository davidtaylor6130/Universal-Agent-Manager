import { act, useLayoutEffect } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, beforeEach, expect, it, vi } from 'vitest'
import { managedElapsed } from '../components/chat/managedSubagents'
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

let root: Root | null = null
const mount = (node: React.ReactNode) => {
  const host = document.createElement('div')
  root = createRoot(host)
  act(() => root!.render(node))
  return host
}

beforeEach(() => { renders = 0; vi.useFakeTimers() })
afterEach(() => {
  if (root) act(() => root!.unmount())
  root = null
  Reflect.deleteProperty(document, 'visibilityState')
  vi.useRealTimers()
})

it('ticks once a second while active, pauses while hidden and catches up when visible', () => {
  const start = Date.now()
  const host = mount(<Clock active />)
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
})

it('does not tick while inactive', () => {
  mount(<Clock active={false} />)
  expect(vi.getTimerCount()).toBe(0)
  setVisibility('visible')
  const before = renders
  act(() => { vi.advanceTimersByTime(5000) })
  expect(renders).toBe(before)
})

it('never commits a stale time when timing data arrives as the clock starts', () => {
  // A managed run read after a long idle: the first committed render must already use the current time.
  const committed: string[] = []
  function Row({ startedAtEpochMs }: { startedAtEpochMs: number | null }) {
    const now = useNow(startedAtEpochMs !== null)
    const text = startedAtEpochMs === null ? 'none' : managedElapsed({ status: 'running', startedAt: '', completedAt: '', startedAtEpochMs, lastReported: false }, now)
    useLayoutEffect(() => { committed.push(text) })
    return <span>{text}</span>
  }
  mount(<Row startedAtEpochMs={null} />)
  act(() => { vi.advanceTimersByTime(60_000) })
  act(() => root!.render(<Row startedAtEpochMs={Date.now() - 3000} />))
  expect(committed).not.toContain('Timing unavailable')
  expect(committed.at(-1)).toBe('3s')
  // A newer run appearing between ticks also reads the current time.
  act(() => { vi.advanceTimersByTime(400) })
  act(() => root!.render(<Row startedAtEpochMs={Date.now() - 100} />))
  expect(committed).not.toContain('Timing unavailable')
  expect(committed.at(-1)).toBe('0s')
})

it('clears its timer and visibility listener on unmount', () => {
  mount(<Clock active />)
  expect(vi.getTimerCount()).toBe(1)
  act(() => root!.unmount())
  root = null
  expect(vi.getTimerCount()).toBe(0)
  const before = renders
  setVisibility('visible')
  expect(renders).toBe(before)
  expect(vi.getTimerCount()).toBe(0)
})
