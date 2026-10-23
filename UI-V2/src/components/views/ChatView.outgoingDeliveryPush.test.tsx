// Real-store delivery projection regression controls for the ChatView consumer.
// Six consumer event-order cases; no measured latency or native/provider delivery claim.
// inputWriteCompleted is an explicit simulated native local-write projection, never a real OS/native run.
// Admission, storage and provider processing remain separate; no remote/all-provider claim.
import { act } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterAll, afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { ChatView } from './ChatView'
import { useAppStore } from '../../store/useAppStore'
import { acpBindingFromCppChat, reconcileCppMessages } from '../../store/cpp/reconcile'
import { sanitizeCppAcpSession } from '../../store/cpp/sanitizers'
import type { CppChat, CppMessage } from '../../store/cpp/types'
import { removeComposerDrafts, writeChatComposerDraft } from '../../utils/composerDraftStorage'
import { resetOutgoingPresentation } from '../../utils/outgoingMessagePresentation'
import { pendingStreamTokensByChatId, pushFlushTimers, resetPushBuffersForTests, STREAM_TOKEN_FLUSH_DELAY_MS } from '../../store/push/pushBuffers'

// Establish the ACTUAL isCefContext contract before static store imports execute: a callable cefQuery.
// The bootstrap response is intentionally invalid as app state; it cannot hydrate a fake conversation.
// No bridge/store/sanitizer/reconciler or sendAcpPrompt function is mocked.
const beforeImport = vi.hoisted(() => {
  const cefQuery = window.cefQuery
  const uamPush = window.uamPush
  window.cefQuery = ({ onSuccess }) => onSuccess('null')
  return { cefQuery, uamPush }
})
afterAll(() => {
  window.cefQuery = beforeImport.cefQuery
  window.uamPush = beforeImport.uamPush
})

// Same narrow panel isolation as canonical ChatView tests; sendAcpPrompt is NEVER mocked.
vi.mock('../chat/ManagedSubagentPanel', () => ({ ManagedSubagentPanel: () => null }))
;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

const TEXT = 'Check the pending workspace.\nKeep internal whitespace  intact.'
const FILE = { id: 'fixture-file', name: 'fixture.txt', type: 'file', size: 4, path: '/private-fixture/fixture.txt' }
const SKILL = { id: 'fixture-skill', title: 'Fixture skill', maker: '', review: '', dateCreated: '', dateUpdated: '', preview: '', filePath: '/skills/fixture-skill.md' }
const HISTORY: CppMessage[] = [
  { role: 'user', content: 'Earlier request', createdAt: '2026-01-01T00:00:00.000Z' },
  { role: 'assistant', content: 'Earlier answer', createdAt: '2026-01-01T00:00:01.000Z' },
]
const chat = (runtime: Record<string, unknown> = {}): CppChat => ({
  id: 'delivery-fixture', title: 'Codex fixture', providerId: 'codex-cli', folderId: 'fixture-folder',
  createdAt: '2026-01-01T00:00:00.000Z', updatedAt: '2026-01-01T00:00:01.000Z', executionHostId: 'local',
  acpSession: sanitizeCppAcpSession({ providerId: 'codex-cli', protocolKind: 'codex-app-server', sessionId: 'native-fixture',
    running: true, processing: false, lifecycleState: 'ready', currentModeId: 'default',
    queueHeld: false, queuedPrompts: [], pendingDirectPrompts: [], unsentDirectPrompts: [],
    unsentMessageIndices: [], recoverableUnsentMessageIndices: [], pendingPermission: null, pendingUserInput: null, ...runtime }),
})

describe('ChatView real uamPush delivery fact ordering', () => {
  let previous: ReturnType<typeof useAppStore.getState>
  let previousCef: Window['cefQuery']
  let host: HTMLDivElement
  let root: Root
  let requests: Array<{ action: string; requestId: string; payload: Record<string, unknown> }>
  let callbacks: Array<{ settled: boolean; ack: (data?: unknown) => void; fail: () => void }>
  let previousUrl: string
  let previousHistoryState: unknown
  let revision: number
  let previousStorageDescriptor: PropertyDescriptor | undefined

  beforeEach(() => {
    vi.useFakeTimers()
    resetPushBuffersForTests()
    expect(window.uamPush).toBeTypeOf('function')
    revision = 0
    requests = []; callbacks = []
    previous = useAppStore.getState()
    previousCef = window.cefQuery
    previousUrl = window.location.href
    previousHistoryState = window.history.state
    vi.stubGlobal('ResizeObserver', class { observe() {} unobserve() {} disconnect() {} })
    const saved = new Map<string, string>()
    const storage: Storage = { get length() { return saved.size }, clear: () => saved.clear(),
      getItem: key => saved.get(key) ?? null, setItem: (key, value) => { saved.set(key, String(value)) },
      removeItem: key => { saved.delete(key) }, key: index => [...saved.keys()][index] ?? null }
    previousStorageDescriptor = Object.getOwnPropertyDescriptor(window, 'localStorage')
    vi.stubGlobal('localStorage', storage)
    Object.defineProperty(window, 'localStorage', { configurable: true, value: storage })
    window.history.replaceState({}, '', '/') // Private jsdom surface, never companion transport.
    resetOutgoingPresentation()
    removeComposerDrafts(['delivery-fixture'])
    writeChatComposerDraft('delivery-fixture', { text: TEXT, attachments: [FILE] })
    const created = new Date('2026-01-01T00:00:00.000Z')
    useAppStore.setState({ ...useAppStore.getInitialState(),
      workingDisplayMode: 'verbose', collapsibleWorkSections: true, expandWorkTraces: true,
      activeSessionId: 'delivery-fixture', chatSearchNavigation: null, lastAppliedStateRevision: -1,
      folders: [{ id: 'fixture-folder', name: 'Fixture', parentId: null, directory: '/private-fixture', isExpanded: true, createdAt: created }],
      sessions: [{ id: 'delivery-fixture', name: 'Codex fixture', providerId: 'codex-cli', viewMode: 'chat', folderId: 'fixture-folder',
        workspaceDirectory: '/private-fixture', executionHostId: 'local', createdAt: created, updatedAt: created, messageCount: 2 }],
      messages: { 'delivery-fixture': reconcileCppMessages('delivery-fixture', undefined, HISTORY, true, 0) },
      historyStartIndexBySessionId: { 'delivery-fixture': 0 }, chatHistoryErrorBySessionId: {},
      providers: [{ id: 'codex-cli', name: 'Codex', shortName: 'Codex', color: '#22c55e', description: '', outputMode: 'cli', supportsCli: true, supportsStructured: true, structuredProtocol: 'codex-app-server' }],
      acpBindingBySessionId: { 'delivery-fixture': acpBindingFromCppChat(chat(), undefined) }, cliBindingBySessionId: {},
      markdownStoreAttachedBySessionId: { 'delivery-fixture': [SKILL] },
      uamAgentsBySessionId: { 'delivery-fixture': [{ id: 'build', description: 'Build', builtIn: true }] },
      refreshUamAgents: vi.fn(async () => true), getVcsCommitStatus: vi.fn(async () => null),
    }, true)
    // Guard the real action: no replacement or wrapper can trim/correlate on its behalf.
    expect(useAppStore.getState().sendAcpPrompt).toBe(useAppStore.getInitialState().sendAcpPrompt)
    requests = []; callbacks = []
    window.cefQuery = ({ request, onSuccess, onFailure }) => {
      const parsed = JSON.parse(request); requests.push(parsed)
      if (parsed.action !== 'sendAcpPrompt') { onSuccess('{}'); return }
      const callback = { settled: false,
        ack: (data: unknown = {}) => { if (!callback.settled) { callback.settled = true; onSuccess(JSON.stringify(data)) } },
        fail: () => { if (!callback.settled) { callback.settled = true; onFailure(500, 'Private reply was not confirmed') } },
      }
      callbacks.push(callback)
    }
    host = document.createElement('div'); document.body.append(host); root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
  })

  afterEach(async () => {
    await act(async () => callbacks.forEach(callback => { if (!callback.settled) callback.fail() }))
    if (root) act(() => root.unmount())
    host?.remove()
    resetOutgoingPresentation(); removeComposerDrafts(['delivery-fixture'])
    useAppStore.setState(previous, true)
    window.cefQuery = previousCef
    window.history.replaceState(previousHistoryState, '', previousUrl)
    resetPushBuffersForTests()
    vi.clearAllTimers(); vi.useRealTimers()
    vi.restoreAllMocks(); vi.unstubAllGlobals()
    if (previousStorageDescriptor) Object.defineProperty(window, 'localStorage', previousStorageDescriptor)
    else Reflect.deleteProperty(window, 'localStorage')
  })

  const sends = () => requests.filter(request => request.action === 'sendAcpPrompt')
  const inputIdentity = (requestIndex = 0, nativeInputId = `11111111-1111-4111-8111-${String(requestIndex + 1).padStart(12, '0')}`) => {
    const request = sends()[requestIndex]
    expect(request?.requestId).toEqual(expect.any(String))
    return { nativeInputId, originRequestId: request.requestId }
  }
  const outgoingArticles = () => [...host.querySelectorAll<HTMLElement>('.uam-message-list article')].filter(article => article.textContent?.includes(TEXT.split('\n')[0]))
  const send = async () => {
    const button = host.querySelector<HTMLButtonElement>('[aria-label="Send prompt"]')
    expect(button).not.toBeNull()
    expect(button!.disabled).toBe(false)
    await act(async () => button!.click())
  }
  const noReplay = (count = 1) => {
    expect(sends()).toHaveLength(count)
    expect(requests.filter(request => /retry|resume|discard|remove|edit/i.test(request.action))).toEqual([])
  }
  const exactResources = () => {
    expect(sends()[0].payload).toMatchObject({ chatId: 'delivery-fixture', text: TEXT, attachments: [FILE], markdownStoreFiles: [SKILL.filePath] })
    expect(sends()[0].requestId).toEqual(expect.any(String))
  }
  const assertRetained = (row: Element, article: Element, state: 'sending' | 'unconfirmed') => {
    expect(row.isConnected).toBe(true)
    expect(row.querySelector('article')).toBe(article)
    expect(outgoingArticles()).toEqual([article])
    expect(article.getAttribute('data-delivery')).toBe(state)
    expect(row.querySelector('[aria-label="File attachments"]')?.textContent).toContain(FILE.name)
    expect(row.querySelector('[aria-label="Skills context"]')).not.toBeNull()
  }
  // Every observed update crosses the installed real parser, sanitizer and revision gate.
  const publish = (rows: unknown[]) => act(() => {
    const nextRevision = ++revision
    window.uamPush!({ type: 'statePatch', data: { stateRevision: nextRevision,
      messagesByChatId: { 'delivery-fixture': rows } } })
    expect(useAppStore.getState().lastAppliedStateRevision).toBe(nextRevision)
  })
  const native = (requestIndex = 0, changes: Record<string, unknown> = {}) => ({
    ...inputIdentity(requestIndex), role: 'user', content: TEXT, attachments: [FILE],
    markdownStoreFiles: [SKILL.filePath], createdAt: '2026-01-02T00:00:00Z', ...changes,
  })
  const assertStable = (row: Element, article: Element, delivery: string | null) => {
    expect(row.isConnected).toBe(true)
    expect(row.querySelector('article')).toBe(article)
    expect(article.getAttribute('data-delivery')).toBe(delivery)
  }

  it('clears on a fact-only push before ACK and before any output or timer advance', async () => {
    await send(); exactResources()
    const row = host.querySelector('[data-outgoing-message]')!; const article = row.querySelector('article')!
    publish([...HISTORY, native()])
    assertRetained(row, article, 'sending')
    const before = useAppStore.getState().messages['delivery-fixture'][2]
    publish([...HISTORY, native(0, { inputWriteCompleted: true })])
    // This assertion is adjacent to the positive push: no poll, token, streamDone, timer advance or action reply.
    expect(useAppStore.getState().messages['delivery-fixture'][2]).not.toBe(before)
    expect(useAppStore.getState().messages['delivery-fixture']).toHaveLength(3)
    expect(host.querySelector('[data-message-index="2"]')).toBe(row)
    assertStable(row, article, null)
    expect(callbacks[0].settled).toBe(false)
    await act(async () => callbacks[0].fail())
    assertStable(row, article, null)
    noReplay()
  })

  it('keeps ACK, materialization, absent/false/string facts and a foreign positive unresolved', async () => {
    await send()
    const row = host.querySelector('[data-outgoing-message]')!; const article = row.querySelector('article')!
    await act(async () => callbacks[0].ack()) // Raw {} is admission only.
    assertStable(row, article, 'sending')
    const firstOutput = { role: 'assistant', content: 'An independent first output', createdAt: '2026-01-02T00:00:01Z' }
    const foreign = native(0, { nativeInputId: '22222222-2222-4222-8222-222222222222',
      originRequestId: 'different-action', inputWriteCompleted: true })
    for (const fact of [undefined, false, 'true']) {
      publish([...HISTORY, native(0, { inputWriteCompleted: fact }), firstOutput, foreign])
      expect(useAppStore.getState().messages['delivery-fixture'][2].inputWriteCompleted).toBeUndefined()
      expect(useAppStore.getState().messages['delivery-fixture'][4].inputWriteCompleted).toBe(true)
      expect(host.querySelector('[data-message-index="2"]')).toBe(row)
      assertStable(row, article, 'sending')
    }
    noReplay()
  })

  it('settles only the positively identified action among identical authored prompts', async () => {
    await send()
    const first = host.querySelector('[data-outgoing-message]')!; const firstArticle = first.querySelector('article')!
    await act(async () => callbacks[0].fail())
    await send()
    const second = host.querySelectorAll('[data-outgoing-message]')[1]; const secondArticle = second.querySelector('article')!
    await act(async () => callbacks[1].fail())
    expect(sends()[0].requestId).not.toBe(sends()[1].requestId)
    publish([...HISTORY, native(1), native(0)])
    assertStable(first, firstArticle, 'unconfirmed'); assertStable(second, secondArticle, 'unconfirmed')
    const firstProjection = useAppStore.getState().messages['delivery-fixture'][3]
    publish([...HISTORY, native(1, { inputWriteCompleted: true }), native(0)])
    expect(host.querySelector('[data-message-index="2"]')).toBe(second)
    expect(host.querySelector('[data-message-index="3"]')).toBe(first)
    assertStable(second, secondArticle, null); assertStable(first, firstArticle, 'unconfirmed')
    expect(useAppStore.getState().messages['delivery-fixture'][3].nativeInputId).toBe(firstProjection.nativeInputId)
    noReplay(2)
  })

  it('does not transfer an old birth positive fact to a recreated same-ID chat action', async () => {
    await send(); const old = native(0, { inputWriteCompleted: true })
    await act(async () => callbacks[0].fail())
    act(() => root.unmount())
    const birth = new Date('2026-02-01T00:00:00Z')
    act(() => useAppStore.setState(state => ({
      sessions: state.sessions.map(session => ({ ...session, createdAt: birth })),
      messages: { 'delivery-fixture': reconcileCppMessages('delivery-fixture', undefined, HISTORY, true, 0) },
      acpBindingBySessionId: { 'delivery-fixture': acpBindingFromCppChat(chat(), undefined) },
    })))
    writeChatComposerDraft('delivery-fixture', { text: TEXT, attachments: [FILE] })
    root = createRoot(host); act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    await send()
    const row = host.querySelector('[data-outgoing-message]')!; const article = row.querySelector('article')!
    publish([...HISTORY, old])
    expect(host.querySelector('[data-message-index="2"]')).not.toBe(row)
    assertStable(row, article, 'sending')
    publish([...HISTORY, old, native(1)])
    assertStable(row, article, 'sending')
    publish([...HISTORY, old, native(1, { inputWriteCompleted: true })])
    expect(host.querySelector('[data-message-index="3"]')).toBe(row)
    assertStable(row, article, null)
    noReplay(2)
  })

  it('keeps historical input unknown after presentation-record loss until its positive push', async () => {
    await send(); await act(async () => callbacks[0].ack())
    publish([...HISTORY, native()])
    act(() => root.unmount()); resetOutgoingPresentation()
    root = createRoot(host); act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    const row = host.querySelector('[data-message-index="2"]')!; const article = row.querySelector('article')!
    assertStable(row, article, 'unconfirmed')
    expect(row.querySelector('[aria-label="Retry message"],[aria-label="Edit message in new branch"],[aria-label="Revert to message in new branch"]')).toBeNull()
    publish([...HISTORY, native(0, { inputWriteCompleted: false })])
    assertStable(row, article, 'unconfirmed')
    publish([...HISTORY, native(0, { inputWriteCompleted: true })])
    assertStable(row, article, null)
    noReplay()
  })

  it('does not confirm from a flushed first token or wait for a remaining token timer', async () => {
    await send()
    const row = host.querySelector('[data-outgoing-message]')!; const article = row.querySelector('article')!
    publish([...HISTORY, native()])
    act(() => window.uamPush!({ type: 'streamToken', chatId: 'delivery-fixture', messageIndex: 3, token: 'First token' }))
    act(() => vi.advanceTimersByTime(STREAM_TOKEN_FLUSH_DELAY_MS))
    expect(useAppStore.getState().messages['delivery-fixture'][3].content).toBe('First token')
    assertStable(row, article, 'sending')
    act(() => window.uamPush!({ type: 'streamToken', chatId: 'delivery-fixture', messageIndex: 3, token: ' continued' }))
    const scheduledTimer = pushFlushTimers.streamToken
    expect(scheduledTimer).not.toBeNull()
    expect(pendingStreamTokensByChatId.has('delivery-fixture')).toBe(true)
    publish([...HISTORY, native(0, { inputWriteCompleted: true }), {
      role: 'assistant', content: 'First token continued', createdAt: '2026-01-02T00:00:01Z',
    }])
    // The authoritative patch includes the buffered bytes; the existing timer has NOT fired.
    expect(pushFlushTimers.streamToken).toBe(scheduledTimer)
    expect(pendingStreamTokensByChatId.has('delivery-fixture')).toBe(false)
    assertStable(row, article, null)
    expect(callbacks[0].settled).toBe(false)
    noReplay()
  })
})
