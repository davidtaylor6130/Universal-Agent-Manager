import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { alignRowStart, applyMotionSpeed, readStoredMotion, writeStoredMotion } from './motionStorage'

describe('motionStorage', () => {
  beforeEach(() => {
    const items = new Map<string, string>()
    vi.stubGlobal('localStorage', {
      getItem: (key: string) => items.get(key) ?? null,
      setItem: (key: string, value: string) => { items.set(key, value) },
    })
  })

  afterEach(() => {
    vi.unstubAllGlobals()
    document.documentElement.removeAttribute('data-motion')
  })

  it('persists a speed, falls back to default for unknown values, and sets the root attribute', () => {
    expect(readStoredMotion()).toBe('default')

    writeStoredMotion('slow')
    expect(readStoredMotion()).toBe('slow')
    applyMotionSpeed('slow')
    expect(document.documentElement.getAttribute('data-motion')).toBe('slow')

    localStorage.setItem('uam-motion', 'warp')
    expect(readStoredMotion()).toBe('default')

    applyMotionSpeed('default')
    expect(document.documentElement.hasAttribute('data-motion')).toBe(false)
  })

  it('aligns a row smoothly until scrollend or a still frame, and instantly with Motion Off', () => {
    const frames: FrameRequestCallback[] = []
    vi.stubGlobal('requestAnimationFrame', (callback: FrameRequestCallback) => frames.push(callback))
    const flush = () => { for (const callback of frames.splice(0)) callback(0) }
    const region = document.createElement('div')
    const row = document.createElement('div')
    const scrollIntoView = vi.fn()
    row.scrollIntoView = scrollIntoView
    const onEnd = vi.fn()

    expect(alignRowStart(region, row, onEnd)).toBe(true)
    expect(scrollIntoView).toHaveBeenLastCalledWith({ block: 'start', behavior: 'smooth' })
    region.scrollTop = 40
    flush(); flush()
    expect(onEnd).not.toHaveBeenCalled()
    region.dispatchEvent(new Event('scrollend'))
    region.dispatchEvent(new Event('scrollend'))
    expect(onEnd).toHaveBeenCalledTimes(1)

    // Nothing moved (row already in place): no scrollend ever arrives, so two still frames end the hold.
    expect(alignRowStart(region, row, onEnd)).toBe(true)
    flush(); flush()
    expect(onEnd).toHaveBeenCalledTimes(2)

    document.documentElement.setAttribute('data-motion', 'off')
    expect(alignRowStart(region, row, onEnd)).toBe(false)
    expect(scrollIntoView).toHaveBeenLastCalledWith({ block: 'start', behavior: 'auto' })
    flush(); flush()
    expect(onEnd).toHaveBeenCalledTimes(2)
  })
})
