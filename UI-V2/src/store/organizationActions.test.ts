import { beforeEach, describe, expect, it, vi } from 'vitest'
const bridge = vi.hoisted(() => ({ cef: false, send: vi.fn(async () => ({ ok: true })) }))
vi.mock('../ipc/cefBridge', async (original) => ({ ...await original<typeof import('../ipc/cefBridge')>(), isCefContext: () => bridge.cef, sendToCEF: bridge.send }))
import { useAppStore } from './useAppStore'
import { APP_FAILURE_EVENT, type AppFailure } from '../utils/appFailure'
import { pendingRequestIdsByKey } from './push/pushBuffers'
const collection = (id: string) => ({ id, name: id, collapsed: false, references: [] })
beforeEach(() => {
  pendingRequestIdsByKey.clear()
  bridge.cef = false
  bridge.send.mockReset().mockResolvedValue({ ok: true })
  useAppStore.setState({ resourceCollections: [collection('one'), collection('two')],
    sessions: [{ id: 'chat', name: 'Chat', viewMode: 'chat', folderId: null, createdAt: new Date(), updatedAt: new Date(), isPinned: false }] })
})

describe('overlapping optimistic pin saves', () => {
  function deferPinSaves() {
    bridge.cef = true
    const requests: Array<{ finish: (ok: boolean) => void }> = []
    bridge.send.mockImplementation((message: { requestId?: string }) => new Promise((resolve) => {
      requests.push({ finish: (ok) => resolve({ ok, requestId: message.requestId }) })
    }))
    return requests
  }

  async function collectFailures(run: (failures: AppFailure[]) => Promise<void>) {
    const failures: AppFailure[] = []
    const listener = (event: Event) => failures.push((event as CustomEvent<AppFailure>).detail)
    window.addEventListener(APP_FAILURE_EVENT, listener)
    try {
      await run(failures)
    } finally {
      window.removeEventListener(APP_FAILURE_EVENT, listener)
    }
  }

  it.each([
    [false, false], [false, true], [true, false], [true, true], [undefined, false], [undefined, true],
  ] as const)('restores saved %s after both failures (newer fails first: %s)', async (savedPinned, newerFirst) => {
    const requests = deferPinSaves()
    useAppStore.setState((state) => ({ sessions: state.sessions.map((session) => ({ ...session, isPinned: savedPinned })) }))
    await collectFailures(async (failures) => {
      const first = useAppStore.getState().setSessionPinned('chat', !(savedPinned ?? false))
      const second = useAppStore.getState().setSessionPinned('chat', savedPinned ?? false)
      expect(requests).toHaveLength(2)
      expect(useAppStore.getState().sessions[0].isPinned).toBe(savedPinned ?? false)

      const firstToFinish = newerFirst ? 1 : 0
      requests[firstToFinish].finish(false)
      expect(await [first, second][firstToFinish]).toBe(false)
      requests[1 - firstToFinish].finish(false)
      expect(await [first, second][1 - firstToFinish]).toBe(false)

      expect(useAppStore.getState().sessions[0].isPinned).toBe(savedPinned)
      expect(failures.map((failure) => failure.message)).toEqual(['Could not save the pin change.'])
      expect(pendingRequestIdsByKey.has('setSessionPinned:chat')).toBe(false)
    })
  })

  it('ignores an older failure after a newer pin save succeeds', async () => {
    const requests = deferPinSaves()
    await collectFailures(async (failures) => {
      const first = useAppStore.getState().setSessionPinned('chat', true)
      const second = useAppStore.getState().setSessionPinned('chat', false)
      requests[1].finish(true)
      expect(await second).toBe(true)
      requests[0].finish(false)
      expect(await first).toBe(false)
      expect(useAppStore.getState().sessions[0].isPinned).toBe(false)
      expect(failures).toHaveLength(0)
    })
  })

  it('keeps a newer saved pin when older requests fail or succeed later', async () => {
    const requests = deferPinSaves()
    await collectFailures(async (failures) => {
      const first = useAppStore.getState().setSessionPinned('chat', true)
      const second = useAppStore.getState().setSessionPinned('chat', false)
      const third = useAppStore.getState().setSessionPinned('chat', true)
      requests[2].finish(true)
      expect(await third).toBe(true)
      requests[0].finish(false)
      expect(await first).toBe(false)
      requests[1].finish(true)
      expect(await second).toBe(true)
      expect(useAppStore.getState().sessions[0].isPinned).toBe(true)
      expect(failures).toHaveLength(0)
    })
  })

  it.each([false, true])('uses saved state while an older request is unresolved, then settles its result (%s)', async (olderSucceeds) => {
    const requests = deferPinSaves()
    const first = useAppStore.getState().setSessionPinned('chat', true)
    const second = useAppStore.getState().setSessionPinned('chat', false)
    requests[1].finish(false)
    expect(await second).toBe(false)
    expect(useAppStore.getState().sessions[0].isPinned).toBe(false)
    requests[0].finish(olderSucceeds)
    expect(await first).toBe(olderSucceeds)
    expect(useAppStore.getState().sessions[0].isPinned).toBe(olderSucceeds)
  })

  it('rolls back to an older acknowledged success when the current save fails', async () => {
    const requests = deferPinSaves()
    const first = useAppStore.getState().setSessionPinned('chat', true)
    const second = useAppStore.getState().setSessionPinned('chat', false)
    requests[0].finish(true)
    expect(await first).toBe(true)
    expect(useAppStore.getState().sessions[0].isPinned).toBe(false)
    requests[1].finish(false)
    expect(await second).toBe(false)
    expect(useAppStore.getState().sessions[0].isPinned).toBe(true)
  })

  it('retains the saved baseline across another failed attempt while an older save is unresolved', async () => {
    const requests = deferPinSaves()
    const first = useAppStore.getState().setSessionPinned('chat', true)
    const second = useAppStore.getState().setSessionPinned('chat', false)
    requests[1].finish(false)
    expect(await second).toBe(false)
    const third = useAppStore.getState().setSessionPinned('chat', true)
    requests[2].finish(false)
    expect(await third).toBe(false)
    requests[0].finish(false)
    expect(await first).toBe(false)
    expect(useAppStore.getState().sessions[0].isPinned).toBe(false)
  })

  it('keeps a third optimistic pin until failure after an older save is acknowledged', async () => {
    const requests = deferPinSaves()
    await collectFailures(async (failures) => {
      const first = useAppStore.getState().setSessionPinned('chat', true)
      const second = useAppStore.getState().setSessionPinned('chat', false)
      requests[1].finish(false)
      expect(await second).toBe(false)
      expect(useAppStore.getState().sessions[0].isPinned).toBe(false)

      // Native may publish the older saved pin before its acknowledgement arrives.
      // This makes the third optimistic value differ from the older saved value.
      useAppStore.setState((state) => ({ sessions: state.sessions.map((session) => ({ ...session, isPinned: true })) }))
      const third = useAppStore.getState().setSessionPinned('chat', false)
      expect(requests).toHaveLength(3)
      expect(useAppStore.getState().sessions[0].isPinned).toBe(false)

      requests[0].finish(true)
      expect(await first).toBe(true)
      expect(pendingRequestIdsByKey.has('setSessionPinned:chat')).toBe(true)
      expect(useAppStore.getState().sessions[0].isPinned).toBe(false)

      requests[2].finish(false)
      expect(await third).toBe(false)
      expect(useAppStore.getState().sessions[0].isPinned).toBe(true)
      expect(pendingRequestIdsByKey.has('setSessionPinned:chat')).toBe(false)
      expect(failures.map((failure) => failure.message)).toEqual([
        'Could not save the pin change.', 'Could not save the pin change.',
      ])
    })
  })

  it('starts a fresh baseline after pending saves settle and preserves other metadata', async () => {
    const requests = deferPinSaves()
    const first = useAppStore.getState().setSessionPinned('chat', true)
    requests[0].finish(false)
    expect(await first).toBe(false)
    useAppStore.setState((state) => ({ sessions: state.sessions.map((session) => ({ ...session, isPinned: true, name: 'Native title' })) }))
    const second = useAppStore.getState().setSessionPinned('chat', false)
    requests[1].finish(false)
    expect(await second).toBe(false)
    expect(useAppStore.getState().sessions[0]).toMatchObject({ isPinned: true, name: 'Native title' })
  })

  it('keeps separate saved baselines for different chats', async () => {
    const requests = deferPinSaves()
    useAppStore.setState((state) => ({ sessions: [...state.sessions, { ...state.sessions[0], id: 'other', isPinned: true }] }))
    const first = useAppStore.getState().setSessionPinned('chat', true)
    const second = useAppStore.getState().setSessionPinned('other', false)
    requests[0].finish(false)
    expect(await first).toBe(false)
    requests[1].finish(false)
    expect(await second).toBe(false)
    expect(useAppStore.getState().sessions.map((session) => session.isPinned)).toEqual([false, true])
  })
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
