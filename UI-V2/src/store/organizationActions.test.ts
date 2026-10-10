import { beforeEach, describe, expect, it, vi } from 'vitest'
const bridge = vi.hoisted(() => ({ cef: false, send: vi.fn(async () => ({ ok: true })) }))
vi.mock('../ipc/cefBridge', async (original) => ({ ...await original<typeof import('../ipc/cefBridge')>(), isCefContext: () => bridge.cef, sendToCEF: bridge.send }))
import { useAppStore } from './useAppStore'
const collection = (id: string) => ({ id, name: id, collapsed: false, references: [] })
beforeEach(() => {
  bridge.cef = false
  bridge.send.mockReset().mockResolvedValue({ ok: true })
  useAppStore.setState({ resourceCollections: [collection('one'), collection('two')], organizationUndo: null,
    sessions: [{ id: 'chat', name: 'Chat', viewMode: 'chat', folderId: null, createdAt: new Date(), updatedAt: new Date(), isPinned: false }] })
})
describe('single successful organization undo', () => {
  it('undoes a successful pin and rejects a changed value', async () => {
    expect(await useAppStore.getState().setSessionPinned('chat', true)).toBe(true)
    expect(await useAppStore.getState().undoOrganization()).toBe(true)
    expect(useAppStore.getState().sessions[0].isPinned).toBe(false)
    expect(useAppStore.getState().organizationUndo).toBeNull()
    await useAppStore.getState().setSessionPinned('chat', true)
    useAppStore.setState((state) => ({ sessions: state.sessions.map((entry) => ({ ...entry, isPinned: false })) }))
    expect(await useAppStore.getState().undoOrganization()).toBe(false)
  })
  it('moves resource membership atomically and restores the original identity and order', async () => {
    const reference = { id: 'ref', type: 'chat' as const, target: 'chat', label: 'Original' }
    useAppStore.setState({ resourceCollections: [{ ...collection('one'), references: [reference] }, collection('two')] })
    expect(await useAppStore.getState().moveResourceToCollection('two', 'chat', 'chat', 'Moved')).toBe(true)
    expect(useAppStore.getState().resourceCollections[0].references).toHaveLength(0)
    expect(useAppStore.getState().resourceCollections[1].references).toEqual([reference])
    expect(await useAppStore.getState().undoOrganization()).toBe(true)
    expect(useAppStore.getState().resourceCollections[0].references).toEqual([reference])
    expect(useAppStore.getState().resourceCollections[1].references).toHaveLength(0)
  })
  it('reorders only the observed list and leaves failed saves and undo records unchanged', async () => {
    expect(await useAppStore.getState().reorderResourceCollections(['two', 'one'])).toBe(true)
    const undo = useAppStore.getState().organizationUndo
    bridge.cef = true
    bridge.send.mockResolvedValue({ ok: false })
    expect(await useAppStore.getState().undoOrganization()).toBe(false)
    expect(useAppStore.getState().resourceCollections.map((entry) => entry.id)).toEqual(['two', 'one'])
    expect(useAppStore.getState().organizationUndo).toBe(undo)
    expect(await useAppStore.getState().moveResourceToCollection('one', 'chat', 'chat', 'Chat')).toBe(false)
    expect(useAppStore.getState().organizationUndo).toBe(undo)
    expect(bridge.send).toHaveBeenLastCalledWith(expect.objectContaining({ action: 'applyOrganizationAction' }))
  })
  it('rejects undo after concurrent list growth without dropping the new collection', async () => {
    await useAppStore.getState().reorderResourceCollections(['two', 'one'])
    useAppStore.setState((state) => ({ resourceCollections: [...state.resourceCollections, collection('three')] }))
    expect(await useAppStore.getState().undoOrganization()).toBe(false)
    expect(useAppStore.getState().resourceCollections).toHaveLength(3)
  })
  it('does not apply a delayed older move after a newer move completes', async () => {
    bridge.cef = true
    let finishFirst!: (value: { ok: boolean }) => void
    bridge.send.mockImplementationOnce(() => new Promise((resolve) => { finishFirst = resolve })).mockResolvedValueOnce({ ok: true })
    const first = useAppStore.getState().moveResourceToCollection('one', 'chat', 'chat', 'First')
    expect(await useAppStore.getState().moveResourceToCollection('two', 'chat', 'chat', 'Second')).toBe(true)
    const undo = useAppStore.getState().organizationUndo
    finishFirst({ ok: true })
    expect(await first).toBe(false)
    expect(useAppStore.getState().resourceCollections[0].references).toHaveLength(0)
    expect(useAppStore.getState().resourceCollections[1].references[0].label).toBe('Second')
    expect(useAppStore.getState().organizationUndo).toBe(undo)
  })

  it('undoes workspace and reference reorders without dropping metadata', async () => {
    useAppStore.setState({ folders: ['a', 'b'].map((id) => ({ id, name: id, parentId: null, directory: `/tmp/${id}`, isExpanded: true, createdAt: new Date() })), resourceCollections: [{ ...collection('one'), references: ['r1', 'r2'].map((id) => ({ id, type: 'chat' as const, target: 'chat', label: id })) }] })
    expect(await useAppStore.getState().reorderFolders(['b', 'a'])).toBe(true)
    expect(await useAppStore.getState().undoOrganization()).toBe(true)
    expect(useAppStore.getState().folders.map((entry) => entry.id)).toEqual(['a', 'b'])
    expect(await useAppStore.getState().reorderResourceReferences('one', ['r2', 'r1'])).toBe(true)
    expect(await useAppStore.getState().undoOrganization()).toBe(true)
    expect(useAppStore.getState().resourceCollections[0].references.map((entry) => entry.label)).toEqual(['r1', 'r2'])
  })

})
