import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { WorkingHeading } from './ConversationWork'
import { formatWorkedDuration } from './MessageBlocks'
import { ChatStatusStrip } from './StatusHelpers'
import { managedElapsed } from './managedSubagents'

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

const mounted = new Set<() => void>()
const mount = (node: React.ReactNode) => {
  const host = document.createElement('div')
  document.body.append(host)
  const root = createRoot(host)
  act(() => root.render(node))
  const unmount = () => { if (!mounted.delete(unmount)) return; act(() => root.unmount()); host.remove() }
  mounted.add(unmount)
  return { host, unmount }
}

beforeEach(() => {
  vi.useFakeTimers()
  vi.stubGlobal('matchMedia', () => ({ matches: false }))
})
afterEach(() => {
  for (const unmount of [...mounted]) unmount()
  Reflect.deleteProperty(document, 'visibilityState')
  vi.useRealTimers()
  vi.unstubAllGlobals()
})

describe('one elapsed-time text across chat surfaces', () => {
  it('shows 3725 seconds as 1h 2m in the live header, the finished header, the status strip and managed rows', () => {
    const now = Date.now()
    const live = mount(<WorkingHeading active startedAt={now - 3_725_000} />)
    expect(live.host.textContent).toBe('Working 1h 2m')
    live.unmount()

    expect(formatWorkedDuration(3725)).toBe('1h 2m')

    const strip = mount(<ChatStatusStrip open tone="info" scene={null} title="Working" />)
    act(() => { vi.advanceTimersByTime(3_725_000) })
    expect(strip.host.querySelector('.chat-status-strip__time')?.textContent).toBe('1h 2m')
    expect(strip.host.querySelector('.chat-status-strip__time')?.getAttribute('aria-label')).toBe('for 1h 2m')
    strip.unmount()

    expect(managedElapsed({ status: 'completed', startedAt: '', completedAt: '', startedAtEpochMs: 1000, finishedAtEpochMs: 1000 + 3_725_000, lastReported: false }, 0)).toBe('1h 2m')
  })

  it('does not pad minutes or seconds in the status strip', () => {
    const strip = mount(<ChatStatusStrip open tone="info" scene={null} title="Working" />)
    act(() => { vi.advanceTimersByTime(65_000) })
    expect(strip.host.querySelector('.chat-status-strip__time')?.textContent).toBe('1m 5s')
    strip.unmount()
  })

  it('reads the same while working and once the turn has finished', () => {
    // A 59.6 s turn: the live header used to floor to 59s and the finished header round to 1m.
    const live = mount(<WorkingHeading active startedAt={Date.now() - 59_600} />)
    const finished = mount(<WorkingHeading active={false} duration={formatWorkedDuration(59.6)} />)
    expect(live.host.textContent).toBe('Working 1m 0s')
    expect(finished.host.textContent).toBe('Worked 1m 0s')
    live.unmount()
    finished.unmount()
  })

  it('stops the live header clock while the window is hidden and catches up when it is shown', () => {
    const live = mount(<WorkingHeading active startedAt={Date.now()} />)
    act(() => { vi.advanceTimersByTime(2000) })
    expect(live.host.textContent).toBe('Working 2s')
    Object.defineProperty(document, 'visibilityState', { configurable: true, value: 'hidden' })
    act(() => { document.dispatchEvent(new Event('visibilitychange')) })
    act(() => { vi.advanceTimersByTime(5000) })
    expect(live.host.textContent).toBe('Working 2s')
    Object.defineProperty(document, 'visibilityState', { configurable: true, value: 'visible' })
    act(() => { document.dispatchEvent(new Event('visibilitychange')) })
    expect(live.host.textContent).toBe('Working 7s')
    live.unmount()
  })
})
