import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import type { CppAppState } from './cpp/types'

type TestWindow = Window & typeof globalThis & { cefQuery?: Window['cefQuery']; uamPush?: (payload: unknown) => void }

function fullState(revision: number, title = `Chat at ${revision}`): CppAppState {
  return {
    stateRevision: revision, selectedChatId: 'chat-1', folders: [], resourceCollections: [], providers: [],
    chats: [{ id: 'chat-1', title, folderId: '', providerId: 'codex-cli', createdAt: '2026-01-01T00:00:00Z', updatedAt: '2026-01-01T00:00:00Z' }],
    settings: {}, statusLine: '',
  } as unknown as CppAppState
}

const chatPatch = (revision: number, title = `Patch ${revision}`) => ({
  type: 'statePatch',
  data: { stateRevision: revision, chats: [{ id: 'chat-1', title, folderId: '', providerId: 'codex-cli', createdAt: '2026-01-01T00:00:00Z', updatedAt: '2026-01-01T00:00:00Z' }] },
})

async function startStore(hydrate = true) {
  const win = window as TestWindow
  vi.resetModules()
  win.dispatchEvent = vi.fn(() => true)
  let bootstrapped = false
  const pulls: Array<{ reply: (state: CppAppState) => void; fail: () => void }> = []
  win.cefQuery = ({ request, onSuccess, onFailure }) => {
    if (JSON.parse(request).action !== 'getInitialState') { onSuccess('{}'); return }
    if (!bootstrapped) {
      bootstrapped = true
      if (hydrate) onSuccess(JSON.stringify(fullState(5)))
      return
    }
    pulls.push({ reply: (state) => onSuccess(JSON.stringify(state)), fail: () => onFailure(500, 'Backend busy') })
  }
  const { useAppStore } = await import('./useAppStore')
  await new Promise((resolve) => setTimeout(resolve, 0))
  vi.useFakeTimers()
  const push = (payload: unknown) => win.uamPush?.(payload)
  const flush = (ms = 100) => vi.advanceTimersByTimeAsync(ms)
  return { store: useAppStore, pulls, push, flush }
}

describe('state patch gap recovery', () => {
  beforeEach(() => { vi.spyOn(console, 'error').mockImplementation(() => undefined) })
  afterEach(() => { vi.clearAllTimers(); vi.useRealTimers(); vi.restoreAllMocks() })

  it('does not resync for a consecutive stream', async () => {
    const { store, pulls, push, flush } = await startStore()
    push(chatPatch(6)); push(chatPatch(7)); push(chatPatch(8))
    await flush(60000)
    expect(pulls).toHaveLength(0)
    expect(store.getState().lastAppliedStateRevision).toBe(8)
  })

  it('pulls one full state after a dropped patch and accepts a same-revision snapshot', async () => {
    const { store, pulls, push, flush } = await startStore()
    push(chatPatch(7, 'After the gap'))
    expect(store.getState().lastAppliedStateRevision).toBe(7)
    await flush()
    expect(pulls).toHaveLength(1)
    pulls[0].reply(fullState(7, 'Recovered'))
    await flush(0)
    expect(store.getState().sessions[0].name).toBe('Recovered')
    expect(store.getState().lastAppliedStateRevision).toBe(7)
    push(chatPatch(8))
    await flush(60000)
    expect(pulls).toHaveLength(1)
  })

  it('resyncs when the sanitiser rejects a patch', async () => {
    const { store, pulls, push, flush } = await startStore()
    push({ type: 'statePatch', data: 'not a patch' })
    expect(store.getState().pushChannelStatus).toBe('invalid-message')
    await flush()
    expect(pulls).toHaveLength(1)
  })

  it('does not resync for a rejected push that cannot have carried state', async () => {
    const { pulls, push, flush } = await startStore()
    push({ type: 'streamToken', chatId: 42 })
    await flush(60000)
    expect(pulls).toHaveLength(0)
  })

  it('resyncs when applying a patch throws', async () => {
    const { store, pulls, push, flush } = await startStore()
    const reconcile = store.getState().reconcileMemorySettingsProjection
    store.setState({ reconcileMemorySettingsProjection: () => { throw new Error('boom') } })
    push(chatPatch(6))
    store.setState({ reconcileMemorySettingsProjection: reconcile })
    expect(store.getState().pushChannelError).toContain('boom')
    await flush()
    expect(pulls).toHaveLength(1)
    pulls[0].reply(fullState(6, 'Recovered after throw'))
    await flush(0)
    expect(store.getState().sessions[0].name).toBe('Recovered after throw')
  })

  it('coalesces a burst of gaps into one pull and lets the snapshot cover held patches', async () => {
    const { store, pulls, push, flush } = await startStore()
    push(chatPatch(7)); push(chatPatch(9)); push({ type: 'statePatch', data: 'bad' }); push(chatPatch(12))
    await flush()
    expect(pulls).toHaveLength(1)
    push(chatPatch(14)); push(chatPatch(16))
    await flush(1000)
    expect(pulls).toHaveLength(1)
    pulls[0].reply(fullState(20, 'Snapshot'))
    await flush(60000)
    expect(pulls).toHaveLength(1)
    expect(store.getState().lastAppliedStateRevision).toBe(20)
    expect(store.getState().sessions[0].name).toBe('Snapshot')
  })

  it('replays patches held during the pull that are newer than the snapshot', async () => {
    const { store, pulls, push, flush } = await startStore()
    push(chatPatch(7))
    await flush()
    push(chatPatch(8, 'Covered by snapshot'))
    push(chatPatch(9, 'Newer than snapshot'))
    expect(store.getState().lastAppliedStateRevision).toBe(7)
    pulls[0].reply(fullState(8, 'Snapshot'))
    await flush(0)
    expect(store.getState().lastAppliedStateRevision).toBe(9)
    expect(store.getState().sessions[0].name).toBe('Newer than snapshot')
    await flush(60000)
    expect(pulls).toHaveLength(1)
  })

  it('never applies a snapshot older than newer full state', async () => {
    const { store, pulls, push, flush } = await startStore()
    push(chatPatch(7))
    await flush()
    push({ type: 'stateUpdate', data: fullState(12, 'Newest full state') })
    pulls[0].reply(fullState(9, 'Stale snapshot'))
    await flush(60000)
    expect(store.getState().lastAppliedStateRevision).toBe(12)
    expect(store.getState().sessions[0].name).toBe('Newest full state')
    expect(pulls).toHaveLength(1)
  })

  it('backs off a failing resync, stops, and retries only at the slowest rate on later patches', async () => {
    const { pulls, push, flush } = await startStore()
    push(chatPatch(7))
    for (const [index, delay] of [100, 300, 900, 5000, 30000].entries()) {
      await flush(delay)
      expect(pulls).toHaveLength(index + 1)
      pulls[index].fail()
      await flush(0)
    }
    await flush(600000)
    expect(pulls).toHaveLength(5)
    push(chatPatch(8))
    await flush(29999)
    expect(pulls).toHaveLength(5)
    await flush(1)
    expect(pulls).toHaveLength(6)
  })

  it('releases held patches when the pull never answers and ignores the late reply', async () => {
    const { store, pulls, push, flush } = await startStore()
    push(chatPatch(7))
    await flush()
    push(chatPatch(8, 'Held'))
    await flush(15000)
    expect(store.getState().lastAppliedStateRevision).toBe(8)
    expect(store.getState().sessions[0].name).toBe('Held')
    pulls[0].reply(fullState(100, 'Late reply'))
    await flush(0)
    expect(store.getState().lastAppliedStateRevision).toBe(8)
  })

  it('leaves gaps before the first hydration to the bootstrap pull', async () => {
    const { pulls, push, flush } = await startStore(false)
    push(chatPatch(3)); push(chatPatch(9))
    await flush(60000)
    expect(pulls).toHaveLength(0)
  })
})
