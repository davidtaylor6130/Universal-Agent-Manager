import { describe, expect, it } from 'vitest'
import { placeViewportMenu } from './ViewportMenu'

describe('placeViewportMenu', () => {
  it('flips above a bottom-edge anchor and clamps both horizontal edges', () => {
    expect(placeViewportMenu(
      { left: 290, right: 290, top: 190, bottom: 190 },
      { width: 120, height: 100 },
      { width: 320, height: 200 },
    )).toEqual({ left: 192, top: 86 })

    expect(placeViewportMenu(
      { left: -20, right: -20, top: 20, bottom: 20 },
      { width: 120, height: 100 },
      { width: 320, height: 200 },
    ).left).toBe(8)
  })
})
