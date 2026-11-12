import { describe, expect, it } from 'vitest'
import { applyManualOrder, sortPreview } from './sortableDrag'

const rows = [
  { id: 'a', top: 100, height: 20 },
  { id: 'b', top: 120, height: 60 },
  { id: 'c', top: 180, height: 20 },
]

describe('sortPreview', () => {
  it('keeps the order while the dragged row stays in its slot', () => {
    const { order, offsets } = sortPreview(rows, 'a', 110)
    expect(order).toEqual(['a', 'b', 'c'])
    expect([...offsets.values()]).toEqual([0, 0, 0])
  })

  it('opens a gap the size of the dragged row when it passes a sibling midpoint', () => {
    // Dragging the 20 px row a past the middle of the 60 px row b moves b up by 20 px.
    const { order, offsets } = sortPreview(rows, 'a', 151)
    expect(order).toEqual(['b', 'a', 'c'])
    expect(offsets.get('b')).toBe(-20)
    expect(offsets.get('a')).toBe(60)
    expect(offsets.get('c')).toBe(0)
  })

  it('moves a tall row past several short ones with variable heights', () => {
    const { order, offsets } = sortPreview(rows, 'b', 500)
    expect(order).toEqual(['a', 'c', 'b'])
    expect(offsets.get('c')).toBe(-60)
    expect(offsets.get('b')).toBe(20)
  })

  it('places a row first when dragged above the list', () => {
    const { order, offsets } = sortPreview(rows, 'c', 0)
    expect(order).toEqual(['c', 'a', 'b'])
    expect(offsets.get('c')).toBe(-80)
    expect(offsets.get('a')).toBe(20)
    expect(offsets.get('b')).toBe(20)
  })
})

describe('applyManualOrder', () => {
  it('keeps the default order until the user saves one', () => {
    expect(applyManualOrder(['c', 'b', 'a'], [])).toEqual(['c', 'b', 'a'])
  })

  it('puts saved ids in their saved order and new ids on top in default order', () => {
    // d and e are new pins or newly active chats; a, b, c were placed by the user.
    expect(applyManualOrder(['e', 'a', 'd', 'b', 'c'], ['b', 'c', 'a'])).toEqual(['e', 'd', 'b', 'c', 'a'])
  })

  it('puts ids reported as returned since the save back on top', () => {
    expect(applyManualOrder(['a', 'b', 'c'], ['c', 'b', 'a'], (id) => id === 'a')).toEqual(['a', 'c', 'b'])
  })

  it('ignores saved ids that are not in the list', () => {
    expect(applyManualOrder(['a', 'b'], ['gone', 'b', 'a'])).toEqual(['b', 'a'])
  })
})
