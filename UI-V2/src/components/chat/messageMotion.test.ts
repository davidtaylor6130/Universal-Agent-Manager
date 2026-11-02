import { afterEach, describe, expect, it, vi } from 'vitest'
import { messageMotionPhase } from './messageMotion'

const setReduced = (reduced: boolean) => {
  window.matchMedia = vi.fn().mockReturnValue({ matches: reduced }) as unknown as typeof window.matchMedia
}

describe('message motion', () => {
  afterEach(() => { document.documentElement.removeAttribute('data-motion') })

  it('moves only when motion is allowed', () => {
    setReduced(false)
    expect(messageMotionPhase({ branch: 'out-prev', editing: false, saving: false })).toBe('out-prev')
    expect(messageMotionPhase({ branch: null, editing: true, saving: true })).toBe('collapse')
    expect(messageMotionPhase({ branch: null, editing: false, saving: false })).toBeUndefined()
    setReduced(true)
    expect(messageMotionPhase({ branch: 'in-next', editing: false, saving: false })).toBeUndefined()
    expect(messageMotionPhase({ branch: null, editing: true, saving: true })).toBe('editing')
    setReduced(false)
    document.documentElement.setAttribute('data-motion', 'off')
    expect(messageMotionPhase({ branch: 'out-next', editing: false, saving: false })).toBeUndefined()
  })
})
