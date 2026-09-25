import { afterEach, describe, expect, it, vi } from 'vitest'
import type { ResourceReferenceType } from '../../types/resourceCollection'
import { useAppStore } from '../../store/useAppStore'
import { COLLECTION_MOVE_FAILURE_EVENT, moveResourceToCollection, type CollectionMoveFailure } from './CollectionMenuItems'

const initial = useAppStore.getState()
afterEach(() => useAppStore.setState(initial, true))

describe('collection moves', () => {
  it.each(['remove', 'add', 'reject', 'rollback'] as const)('reports %s failure and attempts all necessary restorations', async (failure) => {
    const events: CollectionMoveFailure[] = []
    const listener = (event: Event) => events.push((event as CustomEvent<CollectionMoveFailure>).detail)
    window.addEventListener(COLLECTION_MOVE_FAILURE_EVENT, listener)
    const remove = vi.fn(async (collectionId: string) => failure !== 'remove' || collectionId !== 'second')
    const add = vi.fn(async (collectionId: string, type: ResourceReferenceType, target: string, label: string) => {
      if (collectionId === 'destination') {
        if (failure === 'reject') throw new Error('Connection closed')
        return null
      }
      if (failure === 'rollback' && collectionId === 'first') throw new Error('Restore failed')
      return { id: 'restored', type, target, label }
    })
    useAppStore.setState({
      resourceCollections: ['first', 'second', 'destination'].map((id) => ({
        id, name: id, collapsed: false,
        references: id === 'destination' ? [] : [{ id: `${id}-ref`, type: 'workspace-folder' as const, target: 'folder', label: id }],
      })),
      removeResourceReference: remove,
      addResourceReference: add,
    })
    try {
      expect(await moveResourceToCollection('destination', 'workspace-folder', 'folder', 'Workspace')).toBe(false)
      expect(events).toHaveLength(1)
      expect(events[0].message).toContain('Workspace')
      expect(Number.isFinite(Date.parse(events[0].time))).toBe(true)
      expect(add).toHaveBeenCalledWith('first', 'workspace-folder', 'folder', 'first')
      if (failure === 'remove') expect(add).not.toHaveBeenCalledWith('destination', expect.anything(), expect.anything(), expect.anything())
      else expect(add).toHaveBeenCalledWith('second', 'workspace-folder', 'folder', 'second')
      if (failure === 'reject') expect(events[0].detail).toContain('Connection closed')
      if (failure === 'rollback') expect(events[0].detail).toContain('Some memberships could not be restored')
    } finally {
      window.removeEventListener(COLLECTION_MOVE_FAILURE_EVENT, listener)
    }
  })

  it('does not emit a failure for a successful move or an existing destination membership', async () => {
    const listener = vi.fn()
    window.addEventListener(COLLECTION_MOVE_FAILURE_EVENT, listener)
    useAppStore.setState({ resourceCollections: [{ id: 'destination', name: 'Destination', collapsed: false, references: [] }] })
    try {
      expect(await moveResourceToCollection('destination', 'workspace-folder', 'folder', 'Workspace')).toBe(true)
      expect(await moveResourceToCollection('destination', 'workspace-folder', 'folder', 'Workspace')).toBe(true)
      expect(useAppStore.getState().resourceCollections[0].references).toHaveLength(1)
      expect(listener).not.toHaveBeenCalled()
    } finally {
      window.removeEventListener(COLLECTION_MOVE_FAILURE_EVENT, listener)
    }
  })
})
