import { act, createElement } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, describe, expect, it, vi } from 'vitest'
import { scaledMs, usePresence } from './Overlay'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

afterEach(() => {
  vi.useRealTimers()
  vi.unstubAllGlobals()
})

const stubScale = (value: string) => vi.stubGlobal('getComputedStyle', () => ({ getPropertyValue: () => value }))

describe('scaledMs', () => {
  it('follows Appearance > Animation speed so exits are not cut short', () => {
    stubScale(' 1.5')
    expect(scaledMs(170)).toBe(255)
    stubScale('0.67')
    expect(scaledMs(100)).toBeCloseTo(67)
  })

  it('keeps the base length when no scale is set (Default)', () => {
    stubScale('')
    expect(scaledMs(170)).toBe(170)
  })
})

describe('usePresence', () => {
  it('stays mounted for the exit scaled by Animation speed', () => {
    vi.useFakeTimers()
    stubScale('1.5')
    vi.stubGlobal('matchMedia', (query: string) => ({ matches: false, media: query, addEventListener() {}, removeEventListener() {} }))
    const states: Array<{ mounted: boolean; closing: boolean }> = []
    const Probe = ({ open }: { open: boolean }) => { states.push(usePresence(open)); return null }
    const root = createRoot(document.createElement('div'))
    act(() => root.render(createElement(Probe, { open: true })))
    act(() => root.render(createElement(Probe, { open: false })))
    act(() => { vi.advanceTimersByTime(170) })
    expect(states.at(-1)).toEqual({ mounted: true, closing: true })
    act(() => { vi.advanceTimersByTime(85) })
    expect(states.at(-1)).toEqual({ mounted: false, closing: false })
    act(() => root.unmount())
  })
})
