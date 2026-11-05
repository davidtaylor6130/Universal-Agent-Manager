import { describe, expect, it } from 'vitest'
import type { ResourceCollection } from '../types/resourceCollection'
import { MAX_COLLECTION_DEPTH, canNestCollection, collectionDepth, collectionPath, collectionSubtreeIds } from './collectionTree'

const make = (id: string, parentId?: string): ResourceCollection => ({ id, name: id.toUpperCase(), collapsed: false, references: [], ...(parentId ? { parentId } : {}) })
const chain = ['l1', 'l2', 'l3', 'l4', 'l5'].map((id, index, all) => make(id, index ? all[index - 1] : undefined))

describe('collection tree rules', () => {
  it('measures depth and builds a readable path', () => {
    expect(MAX_COLLECTION_DEPTH).toBe(5)
    expect(collectionDepth(chain, 'l1')).toBe(1)
    expect(collectionDepth(chain, 'l5')).toBe(5)
    expect(collectionPath(chain, 'l3')).toBe('L1 / L2 / L3')
    expect(collectionSubtreeIds(chain, 'l3')).toEqual(['l3', 'l4', 'l5'])
  })

  it('rejects self-parenting, cycles, unknown parents and excess depth', () => {
    expect(canNestCollection(chain, 'l1', 'l1')).toBe(false)
    expect(canNestCollection(chain, 'l1', 'l4')).toBe(false)
    expect(canNestCollection(chain, 'l1', 'missing')).toBe(false)
    expect(canNestCollection([...chain, make('x')], 'x', 'l5')).toBe(false)
    expect(canNestCollection([...chain, make('x'), make('y', 'x')], 'x', 'l4')).toBe(false)
    expect(canNestCollection([...chain, make('x'), make('y', 'x')], 'x', 'l3')).toBe(true)
    expect(canNestCollection(chain, 'l3', null)).toBe(true)
    // A new collection (no id yet) may go anywhere above the depth limit.
    expect(canNestCollection(chain, null, 'l4')).toBe(true)
    expect(canNestCollection(chain, null, 'l5')).toBe(false)
  })
})
