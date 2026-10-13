import { beforeEach, describe, expect, it, vi } from 'vitest'
const bridge = vi.hoisted(() => ({ cef: false, send: vi.fn(async () => ({ ok: true })) }))
vi.mock('../ipc/cefBridge', async (original) => ({ ...await original<typeof import('../ipc/cefBridge')>(), isCefContext: () => bridge.cef, sendToCEF: bridge.send }))
import { useAppStore } from './useAppStore'
import { APP_FAILURE_EVENT, type AppFailure } from '../utils/appFailure'
const collection = (id: string) => ({ id, name: id, collapsed: false, references: [] })
beforeEach(() => {
  bridge.cef = false
  bridge.send.mockReset().mockResolvedValue({ ok: true })
  useAppStore.setState({ resourceCollections: [collection('one'), collection('two')],
    sessions: [{ id: 'chat', name: 'Chat', viewMode: 'chat', folderId: null, createdAt: new Date(), updatedAt: new Date(), isPinned: false }] })
})
describe('organization saves', () => {
  it('moves resource membership atomically', async () => {
    const reference = { id: 'ref', type: 'chat' as const, target: 'chat', label: 'Original' }
    useAppStore.setState({ resourceCollections: [{ ...collection('one'), references: [reference] }, collection('two')] })
    expect(await useAppStore.getState().moveResourceToCollection('two', 'chat', 'chat', 'Moved')).toBe(true)
    expect(useAppStore.getState().resourceCollections[0].references).toHaveLength(0)
    expect(useAppStore.getState().resourceCollections[1].references).toEqual([reference])
  })
  it('reorders only the observed list and leaves failed saves unchanged', async () => {
    expect(await useAppStore.getState().reorderResourceCollections(['two', 'one'])).toBe(true)
    bridge.cef = true
    bridge.send.mockResolvedValue({ ok: false })
    expect(await useAppStore.getState().reorderResourceCollections(['one', 'two'])).toBe(false)
    expect(useAppStore.getState().resourceCollections.map((entry) => entry.id)).toEqual(['two', 'one'])
    expect(await useAppStore.getState().moveResourceToCollection('one', 'chat', 'chat', 'Chat')).toBe(false)
    expect(bridge.send).toHaveBeenLastCalledWith(expect.objectContaining({ action: 'applyOrganizationAction' }))
  })
  it('does not apply a delayed older move after a newer move completes', async () => {
    bridge.cef = true
    let finishFirst!: (value: { ok: boolean }) => void
    bridge.send.mockImplementationOnce(() => new Promise((resolve) => { finishFirst = resolve })).mockResolvedValueOnce({ ok: true })
    const first = useAppStore.getState().moveResourceToCollection('one', 'chat', 'chat', 'First')
    expect(await useAppStore.getState().moveResourceToCollection('two', 'chat', 'chat', 'Second')).toBe(true)
    finishFirst({ ok: true })
    expect(await first).toBe(false)
    expect(useAppStore.getState().resourceCollections[0].references).toHaveLength(0)
    expect(useAppStore.getState().resourceCollections[1].references[0].label).toBe('Second')
  })
  it('reports failed organization saves to the app failure channel', async () => {
    const failures: AppFailure[] = []
    const listener = (event: Event) => failures.push((event as CustomEvent<AppFailure>).detail)
    window.addEventListener(APP_FAILURE_EVENT, listener)
    try {
      bridge.cef = true
      bridge.send.mockResolvedValueOnce({ ok: false })
      expect(await useAppStore.getState().reorderResourceCollections(['two', 'one'])).toBe(false)
      expect(failures.map((failure) => failure.message)).toEqual(['Could not reorder collections.'])
      bridge.send.mockImplementationOnce(async (message: { requestId?: string }) => ({ ok: false, requestId: message.requestId }))
      expect(await useAppStore.getState().setSessionPinned('chat', true)).toBe(false)
      expect(useAppStore.getState().sessions[0].isPinned).toBe(false)
      expect(failures.map((failure) => failure.message)).toEqual(['Could not reorder collections.', 'Could not save the pin change.'])
    } finally {
      window.removeEventListener(APP_FAILURE_EVENT, listener)
    }
  })
})
