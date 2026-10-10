import { afterEach, describe, expect, it, vi } from 'vitest'
const bridge = vi.hoisted(() => ({ cef: false, send: vi.fn(async () => ({ ok: true })) }))
vi.mock('../../ipc/cefBridge', async (original) => ({ ...await original<typeof import('../../ipc/cefBridge')>(), isCefContext: () => bridge.cef, sendToCEF: bridge.send }))
import { useAppStore } from '../../store/useAppStore'
import { COLLECTION_MOVE_FAILURE_EVENT, moveResourceToCollection, type CollectionMoveFailure } from './CollectionMenuItems'
const initial = useAppStore.getState()
afterEach(() => { useAppStore.setState(initial, true); bridge.cef = false; bridge.send.mockReset() })

describe('atomic collection moves', () => {
  it.each(['save', 'stale', 'missing', 'transport'] as const)('reports %s failure without changing any original memberships', async (failure) => {
    const events: CollectionMoveFailure[] = []
    const listener = (event: Event) => events.push((event as CustomEvent<CollectionMoveFailure>).detail)
    window.addEventListener(COLLECTION_MOVE_FAILURE_EVENT, listener)
    const original = ['first', 'second', 'destination'].map((id) => ({ id, name: id, collapsed: false,
      references: id === 'destination' ? [] : [{ id: `${id}-ref`, type: 'workspace-folder' as const, target: 'folder', label: id }] }))
    const remove = vi.fn()
    const add = vi.fn()
    useAppStore.setState({ resourceCollections: original, removeResourceReference: remove, addResourceReference: add })
    bridge.cef = true
    if (failure === 'transport') bridge.send.mockRejectedValueOnce(new Error('Connection closed'))
    else bridge.send.mockResolvedValueOnce({ ok: false })
    try {
      expect(await moveResourceToCollection('destination', 'workspace-folder', 'folder', 'Workspace')).toBe(false)
      expect(bridge.send).toHaveBeenCalledTimes(1)
      expect(bridge.send).toHaveBeenCalledWith(expect.objectContaining({ action: 'applyOrganizationAction', payload: expect.objectContaining({ kind: 'memberships', target: 'folder' }) }))
      expect(useAppStore.getState().resourceCollections).toEqual(original)
      expect(remove).not.toHaveBeenCalled()
      expect(add).not.toHaveBeenCalled()
      expect(events).toHaveLength(1)
      expect(events[0].message).toContain('Workspace')
      expect(Number.isFinite(Date.parse(events[0].time))).toBe(true)
      if (failure === 'transport') expect(events[0].detail).toContain('Connection closed')
      else expect(events[0].detail).toContain('could not be saved')
    } finally { window.removeEventListener(COLLECTION_MOVE_FAILURE_EVENT, listener) }
  })

  it('does not emit failure or repeat the transaction for an existing destination membership', async () => {
    const listener = vi.fn()
    window.addEventListener(COLLECTION_MOVE_FAILURE_EVENT, listener)
    useAppStore.setState({ resourceCollections: [{ id: 'destination', name: 'Destination', collapsed: false, references: [] }] })
    try {
      expect(await moveResourceToCollection('destination', 'workspace-folder', 'folder', 'Workspace')).toBe(true)
      expect(await moveResourceToCollection('destination', 'workspace-folder', 'folder', 'Workspace')).toBe(true)
      expect(useAppStore.getState().resourceCollections[0].references).toHaveLength(1)
      expect(listener).not.toHaveBeenCalled()
    } finally { window.removeEventListener(COLLECTION_MOVE_FAILURE_EVENT, listener) }
  })
})
