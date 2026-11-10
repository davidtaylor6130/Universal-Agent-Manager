import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import type { CppAppState } from './cpp/types'
import type { Message } from '../types/message'
import { readChatComposerDraft, writeChatComposerDraft } from '../utils/composerDraftStorage'
import { addOutgoingAttempt, readOutgoingAttempts } from '../utils/outgoingMessagePresentation'

type TestWindow = Window & typeof globalThis & { cefQuery?: Window['cefQuery']; uamPush?: (payload: unknown) => void }

const chat = (id: string, title: string) => ({ id, title, folderId: '', providerId: 'codex-cli', createdAt: '2026-01-01T00:00:00Z', updatedAt: '2026-01-01T00:00:00Z' })

function fullState(revision: number, title = `Chat at ${revision}`, extraChats: Array<ReturnType<typeof chat>> = []): CppAppState {
  return {
    stateRevision: revision, selectedChatId: 'chat-1', folders: [], resourceCollections: [], providers: [],
    chats: [chat('chat-1', title), ...extraChats],
    settings: {}, statusLine: '',
  } as unknown as CppAppState
}

const chatPatch = (revision: number, title = `Patch ${revision}`) => ({
  type: 'statePatch',
  data: { stateRevision: revision, chats: [chat('chat-1', title)] },
})

async function startStore(hydrate = true, initial = fullState(5)) {
  const win = window as TestWindow
  vi.resetModules()
  win.dispatchEvent = vi.fn(() => true)
  let bootstrapped = false
  const pulls: Array<{ reply: (state: CppAppState) => void; fail: () => void }> = []
  win.cefQuery = ({ request, onSuccess, onFailure }) => {
    if (JSON.parse(request).action !== 'getInitialState') { onSuccess('{}'); return }
    if (!bootstrapped) {
      bootstrapped = true
      if (hydrate) onSuccess(JSON.stringify(initial))
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
  let warnings: ReturnType<typeof vi.spyOn>
  beforeEach(() => {
    vi.spyOn(console, 'error').mockImplementation(() => undefined)
    warnings = vi.spyOn(console, 'warn').mockImplementation(() => undefined)
  })
  afterEach(() => { vi.clearAllTimers(); vi.useRealTimers(); vi.restoreAllMocks() })

  it('does not resync for a consecutive stream', async () => {
    const { store, pulls, push, flush } = await startStore()
    push(chatPatch(6)); push(chatPatch(7)); push(chatPatch(8))
    await flush(60000)
    expect(pulls).toHaveLength(0)
    expect(store.getState().lastAppliedStateRevision).toBe(8)
  })

  it('pulls one full state after a dropped patch, accepts a same-revision snapshot and logs the reason once', async () => {
    const { store, pulls, push, flush } = await startStore()
    push(chatPatch(7, 'After the gap'))
    expect(store.getState().lastAppliedStateRevision).toBe(7)
    await flush()
    expect(pulls).toHaveLength(1)
    expect(warnings).toHaveBeenCalledTimes(1)
    expect(String(warnings.mock.calls[0][0])).toContain('revision 7 after 5')
    pulls[0].reply(fullState(7, 'Recovered'))
    await flush(0)
    expect(store.getState().sessions[0].name).toBe('Recovered')
    expect(store.getState().lastAppliedStateRevision).toBe(7)
    push(chatPatch(8))
    await flush(60000)
    expect(pulls).toHaveLength(1)
  })

  it('accepts a corrective patch at revision + 1 without a resync', async () => {
    const { store, pulls, push, flush } = await startStore()
    push(chatPatch(6, 'Repaired by native'))
    await flush(60000)
    expect(pulls).toHaveLength(0)
    expect(store.getState().sessions[0].name).toBe('Repaired by native')
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

  it('coalesces a burst of gaps into one pull', async () => {
    const { store, pulls, push, flush } = await startStore()
    push(chatPatch(7)); push(chatPatch(9)); push({ type: 'statePatch', data: 'bad' }); push(chatPatch(12))
    await flush()
    expect(pulls).toHaveLength(1)
    push(chatPatch(13)); push(chatPatch(14))
    pulls[0].reply(fullState(20, 'Snapshot'))
    await flush(60000)
    expect(pulls).toHaveLength(1)
    expect(store.getState().lastAppliedStateRevision).toBe(20)
    expect(store.getState().sessions[0].name).toBe('Snapshot')
  })

  it('keeps applying patches while the pull is in flight and replays the newer ones over an overtaken snapshot', async () => {
    const { store, pulls, push, flush } = await startStore()
    push(chatPatch(7))
    await flush()
    push(chatPatch(8, 'Covered by snapshot'))
    push(chatPatch(9, 'Newer than snapshot'))
    // Live during the pull: approvals and turn state do not wait for the reply.
    expect(store.getState().lastAppliedStateRevision).toBe(9)
    expect(store.getState().sessions[0].name).toBe('Newer than snapshot')
    pulls[0].reply(fullState(8, 'Snapshot', [chat('chat-2', 'Only in the snapshot')]))
    await flush(0)
    expect(store.getState().lastAppliedStateRevision).toBe(9)
    expect(store.getState().sessions.map((session) => session.name)).toEqual(['Newer than snapshot', 'Only in the snapshot'])
    await flush(60000)
    expect(pulls).toHaveLength(1)
  })

  it('drops an overtaken snapshot when the held patches cannot rebuild the newer state, and pulls again', async () => {
    const { store, pulls, push, flush } = await startStore()
    push(chatPatch(7))
    await flush()
    push(chatPatch(8)); push(chatPatch(10, 'Newest live'))
    pulls[0].reply(fullState(7, 'Stale snapshot'))
    await flush(0)
    expect(store.getState().lastAppliedStateRevision).toBe(10)
    expect(store.getState().sessions[0].name).toBe('Newest live')
    await flush(5000)
    expect(pulls).toHaveLength(2)
    pulls[1].reply(fullState(10, 'Recovered'))
    await flush(0)
    expect(store.getState().sessions[0].name).toBe('Recovered')
  })

  it('never applies a snapshot older than a newer full state', async () => {
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

  it('does not let a newer full state hide a patch lost after it', async () => {
    const { store, pulls, push, flush } = await startStore()
    push(chatPatch(7))
    await flush()
    push({ type: 'stateUpdate', data: fullState(12, 'Full state during the pull') })
    // 13 is lost; 14 applies while the pull is in flight, so no gap is raised yet.
    push(chatPatch(14, 'After the lost patch'))
    pulls[0].reply(fullState(9, 'Older snapshot'))
    await flush(0)
    expect(store.getState().lastAppliedStateRevision).toBe(14)
    await flush(5000)
    expect(pulls).toHaveLength(2)
    pulls[1].reply(fullState(14, 'Repaired'))
    await flush(0)
    expect(store.getState().sessions[0].name).toBe('Repaired')
  })

  it('settles on a newer full state when the patches after it are contiguous', async () => {
    const { store, pulls, push, flush } = await startStore()
    push(chatPatch(7))
    await flush()
    push({ type: 'stateUpdate', data: fullState(12, 'Full state during the pull') })
    push(chatPatch(13)); push(chatPatch(14, 'Contiguous'))
    pulls[0].reply(fullState(9, 'Older snapshot'))
    await flush(60000)
    expect(pulls).toHaveLength(1)
    expect(store.getState().sessions[0].name).toBe('Contiguous')
  })

  it('publishes an overtaken snapshot and its replay as one update', async () => {
    const { store, pulls, push, flush } = await startStore()
    push(chatPatch(7))
    await flush()
    push(chatPatch(8)); push(chatPatch(9, 'Newest'))
    const seen: number[] = []
    const unsubscribe = store.subscribe((state) => seen.push(state.lastAppliedStateRevision))
    pulls[0].reply(fullState(8, 'Snapshot', [chat('chat-2', 'Only in the snapshot')]))
    await flush(0)
    unsubscribe()
    expect(seen.every((revision) => revision === 9)).toBe(true)
    expect(store.getState().sessions.map((session) => session.name)).toEqual(['Newest', 'Only in the snapshot'])
  })

  it('keeps a deliberate empty selection through a resync', async () => {
    const { store, pulls, push, flush } = await startStore()
    store.setState({ activeSessionId: null })
    push(chatPatch(7))
    await flush()
    pulls[0].reply(fullState(7))
    await flush(0)
    expect(store.getState().activeSessionId).toBeNull()
  })

  it('spaces pulls for a recurring fault between clean patches and resets after a quiet minute', async () => {
    const { pulls, push, flush } = await startStore()
    let revision = 5
    let answered = 0
    const answer = async () => {
      while (answered < pulls.length) { pulls[answered++].reply(fullState(revision)); await flush(0) }
    }
    for (let tick = 0; tick < 100; tick += 1) {
      push({ type: 'statePatch', data: 'recurring bad payload' })
      push(chatPatch(++revision))
      await flush(100)
      await answer()
    }
    // 10 s of faults: pulls at 0.1 s, then spaced 2 s and 4 s apart.
    expect(pulls).toHaveLength(3)
    // The last fault is still repaired, 8 s after the previous pull.
    await flush(5000)
    expect(pulls).toHaveLength(4)
    await answer()
    await flush(60000)
    expect(pulls).toHaveLength(4)
    push({ type: 'statePatch', data: 'bad after a quiet minute' })
    await flush(100)
    expect(pulls).toHaveLength(5)
  })

  it('backs off a failing resync and stops after five attempts until a later patch', async () => {
    const { pulls, push, flush } = await startStore()
    push(chatPatch(7))
    for (const [index, delay] of [100, 2000, 4000, 8000, 16000].entries()) {
      await flush(delay)
      expect(pulls).toHaveLength(index + 1)
      pulls[index].fail()
      await flush(0)
    }
    await flush(600000)
    expect(pulls).toHaveLength(5)
    push(chatPatch(8))
    await flush(100)
    expect(pulls).toHaveLength(6)
  })

  it('keeps the gap outstanding when composing the snapshot throws on the last allowed attempt', async () => {
    const { store, pulls, push, flush } = await startStore()
    push(chatPatch(7))
    for (const [index, delay] of [100, 2000, 4000, 8000].entries()) {
      await flush(delay)
      expect(pulls).toHaveLength(index + 1)
      pulls[index].fail()
      await flush(0)
    }
    await flush(16000)
    expect(pulls).toHaveLength(5)
    const reconcile = store.getState().reconcileMemorySettingsProjection
    store.setState({ reconcileMemorySettingsProjection: () => { throw new Error('compose failed') } })
    pulls[4].reply(fullState(7, 'Never composed'))
    await flush(0)
    store.setState({ reconcileMemorySettingsProjection: reconcile })
    await flush(600000)
    expect(pulls).toHaveLength(5)
    push(chatPatch(8))
    await flush(100)
    expect(pulls).toHaveLength(6)
  })

  it('does not count a full state the store rejected as closing the gap', async () => {
    const { store, pulls, push, flush } = await startStore()
    push(chatPatch(7))
    await flush()
    const loadFromCef = store.getState().loadFromCef
    store.setState({ loadFromCef: () => undefined })
    push({ type: 'stateUpdate', data: fullState(12) })
    store.setState({ loadFromCef })
    push(chatPatch(13)); push(chatPatch(14))
    expect(store.getState().lastAppliedStateRevision).toBe(14)
    pulls[0].reply(fullState(9, 'Older snapshot'))
    await flush(5000)
    expect(pulls).toHaveLength(2)
  })

  it('does not freeze live state when the pull never answers, and ignores the late reply', async () => {
    const { store, pulls, push, flush } = await startStore()
    push(chatPatch(7))
    await flush()
    push(chatPatch(8, 'Live'))
    expect(store.getState().sessions[0].name).toBe('Live')
    await flush(10000)
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

  it('keeps UI-only state through a mid-session resync', async () => {
    const initial = fullState(5, 'First', [chat('chat-2', 'Second')])
    const { store, pulls, push, flush } = await startStore(true, initial)
    const history: Message[] = [{ id: 'page-100', sessionId: 'chat-1', role: 'assistant', content: 'Loaded older page', createdAt: new Date(100) }]
    const navigation = { generation: 1, queryRequestId: 'query-1', chatId: 'chat-1', messageIndex: 100, messagesDigest: 'old', phase: 'ready' as const, anchored: true }
    // A click on chat-2 whose selectSession is not yet acknowledged, a loaded history page and an open search.
    store.setState({ activeSessionId: 'chat-2', messages: { 'chat-1': history }, historyStartIndexBySessionId: { 'chat-1': 100 }, chatSearchNavigation: navigation })
    writeChatComposerDraft('chat-1', { text: 'Unsent draft', attachments: [] })
    addOutgoingAttempt('resync-scope', { key: 'outgoing-1', nativeIndex: 3, text: 'In flight', attachments: [], createdAt: new Date(0), receiptIds: new Set(), phase: 'pending' })
    push(chatPatch(7))
    await flush()
    pulls[0].reply(fullState(7, 'Recovered', [chat('chat-2', 'Second')]))
    await flush(0)
    const state = store.getState()
    expect(state.sessions[0].name).toBe('Recovered')
    expect(state.activeSessionId).toBe('chat-2')
    expect(state.messages['chat-1']).toBe(history)
    expect(state.historyStartIndexBySessionId['chat-1']).toBe(100)
    expect(state.chatSearchNavigation).toEqual(navigation)
    expect(readChatComposerDraft('chat-1').text).toBe('Unsent draft')
    expect(readOutgoingAttempts('resync-scope').map((attempt) => attempt.phase)).toEqual(['pending'])
  })
})
