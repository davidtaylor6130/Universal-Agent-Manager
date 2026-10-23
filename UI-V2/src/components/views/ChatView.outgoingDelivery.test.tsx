// Proposed consumer regressions, UNRUN in this packet. Real sendAcpPrompt + private fake CEF only.
// inputWriteCompleted is an explicit simulated native local-write projection, never a real OS/native run.
// Admission, storage and provider processing remain separate; no remote/all-provider claim.
import { act } from 'react'
import * as cefBridge from '../../ipc/cefBridge'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { ChatView } from './ChatView'
import { useAppStore } from '../../store/useAppStore'
import { acpBindingFromCppChat, reconcileCppMessages } from '../../store/cpp/reconcile'
import { sanitizeCppAcpSession } from '../../store/cpp/sanitizers'
import type { Message } from '../../types/message'
import type { CppChat, CppMessage } from '../../store/cpp/types'
import { removeComposerDrafts, writeChatComposerDraft } from '../../utils/composerDraftStorage'
import { outgoingScope, readOutgoingAttempts, resetOutgoingPresentation, subscribeOutgoingAttempts } from '../../utils/outgoingMessagePresentation'

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

describe('ChatView outgoing admission without a delivery witness', () => {
  let previous: ReturnType<typeof useAppStore.getState>
  let previousCef: Window['cefQuery']
  let host: HTMLDivElement
  let root: Root
  let requests: Array<{ action: string; requestId: string; payload: Record<string, unknown> }>
  let callbacks: Array<{ settled: boolean; ack: (data?: unknown) => void; fail: () => void }>
  let previousUrl: string
  let previousHistoryState: unknown
  let previousStorageDescriptor: PropertyDescriptor | undefined

  beforeEach(() => {
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
      activeSessionId: 'delivery-fixture', chatSearchNavigation: null,
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
  const remount = () => { act(() => root.unmount()); root = createRoot(host); act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />)) }
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
  const publish = (rows: CppMessage[], runtime: Record<string, unknown>) => act(() => useAppStore.setState(state => ({
    messages: { 'delivery-fixture': reconcileCppMessages('delivery-fixture', state.messages['delivery-fixture'], rows, true, 0) },
    acpBindingBySessionId: { 'delivery-fixture': acpBindingFromCppChat(chat(runtime), state.acpBindingBySessionId['delivery-fixture']) },
  }))) // Existing canonical reconcile/binding route; not a fabricated delivery receipt.

  it('keeps one stable sending row after a generic admission ACK with no push', async () => {
    await send(); exactResources()
    const row = host.querySelector('[data-outgoing-message]')!; const article = row.querySelector('article')!
    const historyBefore = useAppStore.getState().messages['delivery-fixture']
    expect(historyBefore).toHaveLength(2)
    await act(async () => callbacks[0].ack())
    assertRetained(row, article, 'sending')
    expect(host.querySelector('[data-outgoing-message]')).toBe(row)
    expect(host.querySelector('[data-message-index="2"]')).toBeNull()
    expect(useAppStore.getState().messages['delivery-fixture']).toBe(historyBefore)
    noReplay()
  })

  it('keeps failed reply input unconfirmed when unrelated assistant output arrives', async () => {
    await send(); exactResources()
    const row = host.querySelector('[data-outgoing-message]')!; const article = row.querySelector('article')!
    await act(async () => callbacks[0].fail())
    assertRetained(row, article, 'unconfirmed')
    expect(useAppStore.getState().messages['delivery-fixture']).toHaveLength(2)
    publish([...HISTORY, { role: 'assistant', content: 'Unrelated provider diagnostic output', createdAt: '2026-01-02T00:00:00.000Z' }],
      { lifecycleState: 'error', running: false, processing: false, lastError: 'Pull before chat requires a configured upstream' })
    assertRetained(row, article, 'unconfirmed')
    expect(host.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')!.value).toBe(TEXT)
    noReplay()
  })

  it.each(['ack', 'failure'] as const)('does not use exact native materialization as a delivery witness after %s', async result => {
    await send(); exactResources()
    const row = host.querySelector('[data-outgoing-message]')!; const article = row.querySelector('article')!
    await act(async () => result === 'ack' ? callbacks[0].ack() : callbacks[0].fail())
    const expected = result === 'ack' ? 'sending' : 'unconfirmed'
    assertRetained(row, article, expected)
    // No acpPromptNotSent/delivery flag supplied: native storage index is not a provider receipt.
    publish([...HISTORY, { ...inputIdentity(), role: 'user', content: TEXT, attachments: [FILE], markdownStoreFiles: [SKILL.filePath], createdAt: '2026-01-02T00:00:00.000Z' }],
      { lifecycleState: result === 'ack' ? 'waiting_workspace' : 'error', running: false, processing: false,
        lastError: 'Pull before chat requires a configured upstream' })
    assertRetained(row, article, expected)
    expect(host.querySelector('[data-message-index="2"]')).toBe(row)
    expect(useAppStore.getState().messages['delivery-fixture'][2].content).toBe(TEXT)
    noReplay()
  })

  it.each(['ack-first', 'held-first'] as const)('keeps one unconfirmed row when a source-shaped held direct admission has no durable user row (%s)', async order => {
    // Root source contract: direct_admission=true, held with no runtime-pending condition maps to unsentDirectPrompts.
    // This intentionally selected producer shape is NOT asserted to be the captured screenshot's missing flag.
    act(() => root.unmount())
    removeComposerDrafts(['delivery-fixture'])
    writeChatComposerDraft('delivery-fixture', { text: TEXT, attachments: [] })
    useAppStore.setState(state => ({ messages: { 'delivery-fixture': [] },
      sessions: state.sessions.map(session => ({ ...session, messageCount: 0 })),
      markdownStoreAttachedBySessionId: { 'delivery-fixture': [] } }))
    root = createRoot(host)
    await act(async () => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    await send()
    expect(sends()).toHaveLength(1)
    expect(sends()[0].payload).toMatchObject({ text: TEXT, attachments: [], markdownStoreFiles: [] })
    const row = host.querySelector('[data-outgoing-message]')!
    const article = row.querySelector('article')!
    expect(outgoingArticles()).toEqual([article])
    expect(useAppStore.getState().messages['delivery-fixture']).toEqual([])
    const held = { ...inputIdentity(), id: 'source-shaped-direct-held', revision: 1, text: TEXT, uamAgentId: 'build',
      attachments: [], markdownStoreFiles: [], goalMode: false, goalId: '' }
    const publishHeldWithoutHistory = () => act(() => useAppStore.setState(state => ({
      // No native message payload: absent native history must not be fabricated to justify the admission ACK.
      acpBindingBySessionId: { 'delivery-fixture': acpBindingFromCppChat(chat({
        lifecycleState: 'error', running: false, processing: false,
        lastError: 'Pull before chat requires a configured upstream',
        // Direct-only held entries do not set the serialized queueHeld bit, which requires queuedPrompts.
        queueHeld: false, queuedPrompts: [], pendingDirectPrompts: [], unsentDirectPrompts: [held],
      }), state.acpBindingBySessionId['delivery-fixture']) },
    })))
    if (order === 'held-first') publishHeldWithoutHistory()
    await act(async () => callbacks[0].ack()) // Admission only; it returns neither a delivery witness nor a queue correlation.
    if (order === 'ack-first') publishHeldWithoutHistory()
    expect(outgoingArticles()).toEqual([article])
    expect(row.isConnected).toBe(true)
    expect(row.querySelector('article')).toBe(article)
    expect(article.getAttribute('data-delivery')).toBe('unconfirmed')
    for (const line of TEXT.split('\n')) expect(row.textContent).toContain(line)
    expect(useAppStore.getState().messages['delivery-fixture'].every(message => message.nativeMessageIndex === undefined)).toBe(true)
    expect(useAppStore.getState().acpBindingBySessionId['delivery-fixture'].unsentDirectPrompts).toHaveLength(1)
    noReplay()
  })

  it('keeps two deliberate identical attempts distinct when one native row cannot identify either', async () => {
    await send(); exactResources()
    const first = host.querySelector('[data-outgoing-message]')!; const firstArticle = first.querySelector('article')!
    await act(async () => callbacks[0].fail())
    await send() // Second deliberate click; first failure preserves its draft/resources.
    expect(sends()[1].payload).toEqual(sends()[0].payload)
    expect(sends()[1].requestId).not.toBe(sends()[0].requestId)
    await act(async () => callbacks[1].fail())
    publish([...HISTORY, { role: 'user', content: TEXT, attachments: [FILE], markdownStoreFiles: [SKILL.filePath], createdAt: '2026-01-02T00:00:00.000Z' }],
      { lifecycleState: 'error', running: false, processing: false, lastError: 'Pull before chat requires a configured upstream' })
    expect(first.isConnected).toBe(true)
    expect(first.querySelector('article')).toBe(firstArticle)
    expect(firstArticle.getAttribute('data-delivery')).toBe('unconfirmed')
    const attempts = [...host.querySelectorAll('[data-outgoing-message]')]
    expect(attempts).toHaveLength(2)
    expect(attempts.every(attempt => attempt.querySelector('article')?.getAttribute('data-delivery') === 'unconfirmed')).toBe(true)
    expect(host.querySelector('[data-message-index="2"]')).not.toBe(first)
    noReplay(2)
  })

  it.each(['ack-first', 'write-first'] as const)('clears dotted presentation only for this input full-write fact (%s)', async order => {
    await send(); exactResources()
    const row = host.querySelector('[data-outgoing-message]')!; const article = row.querySelector('article')!
    const native = { ...inputIdentity(), role: 'user' as const, content: TEXT, attachments: [FILE], markdownStoreFiles: [SKILL.filePath], createdAt: '2026-01-02T00:00:00Z' }
    if (order === 'ack-first') await act(async () => callbacks[0].ack())
    publish([...HISTORY, native], {})
    expect(host.querySelector('[data-message-index="2"]')).toBe(row)
    assertRetained(row, article, 'sending')
    // A fact-only update must invalidate reconciliation even when all authored bytes and indices are unchanged.
    const beforeFact = useAppStore.getState().messages['delivery-fixture'][2]
    publish([...HISTORY, { ...native, inputWriteCompleted: true }], {})
    expect(useAppStore.getState().messages['delivery-fixture'][2]).not.toBe(beforeFact)
    expect(host.querySelector('[data-message-index="2"]')).toBe(row)
    expect(row.querySelector('article')).toBe(article)
    expect(article.hasAttribute('data-delivery')).toBe(false)
    if (order === 'write-first') {
      await act(async () => callbacks[0].fail()) // Lost action reply cannot erase the exact positive historical write fact.
      expect(article.hasAttribute('data-delivery')).toBe(false)
    }
    noReplay()
  })

  it.each([
    { label: 'absent write', change: {}, aliases: true },
    { label: 'false write', change: { inputWriteCompleted: false }, aliases: true },
    { label: 'string true', change: { inputWriteCompleted: 'true' }, aliases: true },
    { label: 'foreign origin positive', change: { originRequestId: 'foreign-action', inputWriteCompleted: true }, aliases: false },
    { label: 'malformed ID positive', change: { nativeInputId: 'not-a-native-id', inputWriteCompleted: true }, aliases: false },
    { label: 'malformed origin positive', change: { originRequestId: '\ninvalid-origin', inputWriteCompleted: true }, aliases: false },
    { label: 'assistant positive', change: { role: 'assistant', inputWriteCompleted: true }, aliases: false },
  ])('retains the action unknown for $label', async ({ change, aliases }) => {
    await send(); const row = host.querySelector('[data-outgoing-message]')!; const article = row.querySelector('article')!
    await act(async () => callbacks[0].fail())
    const native = { ...inputIdentity(), role: 'user', content: TEXT, attachments: [FILE], markdownStoreFiles: [SKILL.filePath], createdAt: '2026-01-02T00:00:00Z', ...change }
    publish([...HISTORY, native as unknown as CppMessage], { processing: true, lifecycleState: 'processing' })
    expect(row.isConnected).toBe(true); expect(row.querySelector('article')).toBe(article)
    expect(article.getAttribute('data-delivery')).toBe('unconfirmed')
    expect(row.querySelector('[aria-label="Retry message"]')).toBeNull()
    expect(host.querySelector('[data-message-index="2"]') === row).toBe(aliases)
    if (!aliases) expect(host.querySelector('[data-outgoing-message]')).toBe(row)
    publish([...HISTORY, native as unknown as CppMessage, { role: 'assistant', content: 'An unrelated result is not the write receipt', createdAt: '2026-01-02T00:00:01Z' }], {})
    expect(article.getAttribute('data-delivery')).toBe('unconfirmed')
    noReplay()
  })

  it.each(['duplicate-origin', 'duplicate-input'] as const)('refuses a first alias for %s records rather than choosing a row', async shape => {
    await send(); const row = host.querySelector('[data-outgoing-message]')!; const article = row.querySelector('article')!
    await act(async () => callbacks[0].fail())
    const first = { ...inputIdentity(), role: 'user' as const, content: TEXT, createdAt: '2026-01-02T00:00:00Z', inputWriteCompleted: true }
    const second = { ...first, nativeInputId: shape === 'duplicate-origin' ? '22222222-2222-4222-8222-222222222222' : first.nativeInputId }
    publish([...HISTORY, first, second], {})
    expect(host.querySelector('[data-outgoing-message]')).toBe(row)
    expect(row.querySelector('article')).toBe(article)
    expect(article.getAttribute('data-delivery')).toBe('unconfirmed')
    expect(host.querySelector('[data-message-index="2"]')).not.toBe(row)
    expect(host.querySelector('[data-message-index="3"]')).not.toBe(row)
    noReplay()
  })

  it('follows a strongly bound native ID after its recorded index and bytes change', async () => {
    await send(); const row = host.querySelector('[data-outgoing-message]')!; const article = row.querySelector('article')!
    const native = { ...inputIdentity(), role: 'user' as const, content: TEXT, createdAt: '2026-01-02T00:00:00Z' }
    publish([...HISTORY, native], {})
    expect(host.querySelector('[data-message-index="2"]')).toBe(row)
    const unrelated = { role: 'assistant' as const, content: 'Earlier independent native output', createdAt: '2026-01-01T00:00:02Z' }
    publish([...HISTORY, unrelated, { ...native, content: 'Updated stored bytes for this exact input', inputWriteCompleted: true }], {})
    expect(host.querySelector('[data-message-index="3"]')).toBe(row)
    expect(row.querySelector('article')).toBe(article)
    expect(article.textContent).toContain('Updated stored bytes for this exact input')
    expect(article.hasAttribute('data-delivery')).toBe(false)
    expect(host.querySelector('[data-message-index="2"]')).not.toBe(row)
    noReplay()
  })

  it('keeps two identical actions distinct and clears only the exact positively written input', async () => {
    await send(); const first = host.querySelector('[data-outgoing-message]')!; const firstArticle = first.querySelector('article')!
    await act(async () => callbacks[0].fail())
    await send(); const second = host.querySelectorAll('[data-outgoing-message]')[1]; const secondArticle = second.querySelector('article')!
    await act(async () => callbacks[1].fail())
    expect(sends()[0].requestId).not.toBe(sends()[1].requestId)
    const native = (index: number, written: boolean): CppMessage => ({ ...inputIdentity(index), role: 'user', content: TEXT,
      attachments: [FILE], markdownStoreFiles: [SKILL.filePath], createdAt: '2026-01-02T00:00:00Z', inputWriteCompleted: written })
    publish([...HISTORY, native(1, true), native(0, false)], {})
    expect(host.querySelector('[data-message-index="2"]')).toBe(second)
    expect(host.querySelector('[data-message-index="3"]')).toBe(first)
    expect(second.querySelector('article')).toBe(secondArticle); expect(first.querySelector('article')).toBe(firstArticle)
    expect(secondArticle.hasAttribute('data-delivery')).toBe(false)
    expect(firstArticle.getAttribute('data-delivery')).toBe('unconfirmed')
    noReplay(2)
  })

  it('never assigns a prior chat birth input to a recreated same-ID chat action', async () => {
    await send(); const oldIdentity = inputIdentity()
    await act(async () => callbacks[0].fail())
    act(() => root.unmount())
    const birth = new Date('2026-02-01T00:00:00Z')
    act(() => useAppStore.setState(state => ({ sessions: state.sessions.map(session => ({ ...session, createdAt: birth })),
      messages: { 'delivery-fixture': reconcileCppMessages('delivery-fixture', undefined, HISTORY, true, 0) },
      acpBindingBySessionId: { 'delivery-fixture': acpBindingFromCppChat(chat(), undefined) } })))
    writeChatComposerDraft('delivery-fixture', { text: TEXT, attachments: [FILE] })
    root = createRoot(host); act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    await send(); const current = host.querySelector('[data-outgoing-message]')!; const article = current.querySelector('article')!
    expect(sends()[1].requestId).not.toBe(oldIdentity.originRequestId)
    publish([...HISTORY, { ...oldIdentity, role: 'user', content: TEXT, createdAt: '2026-01-02T00:00:00Z', inputWriteCompleted: true }], {})
    expect(host.querySelector('[data-outgoing-message]')).toBe(current)
    expect(current.querySelector('article')).toBe(article)
    expect(article.getAttribute('data-delivery')).toBe('sending')
    expect(host.querySelector('[data-message-index="2"]')).not.toBe(current)
    noReplay(2)
  })

  it('keeps identified historical input unknown after loss of module presentation records', async () => {
    await send(); const native = { ...inputIdentity(), role: 'user' as const, content: TEXT, attachments: [FILE], markdownStoreFiles: [SKILL.filePath], createdAt: '2026-01-02T00:00:00Z' }
    await act(async () => callbacks[0].ack())
    publish([...HISTORY, native], {})
    act(() => root.unmount()); resetOutgoingPresentation()
    root = createRoot(host); act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    const row = host.querySelector('[data-message-index="2"]')!
    expect(row.querySelector('article')?.getAttribute('data-delivery')).toBe('unconfirmed')
    expect(row.textContent).toContain(TEXT.split('\n')[0])
    expect(row.querySelector('[aria-label="Retry message"],[aria-label="Edit message in new branch"],[aria-label="Revert to message in new branch"]')).toBeNull()
    noReplay()
  })

  it('preserves explicit native unsent recovery precedence even beside a positive write projection', async () => {
    await send(); const row = host.querySelector('[data-outgoing-message]')!; const article = row.querySelector('article')!
    publish([...HISTORY, { ...inputIdentity(), role: 'user', content: TEXT, attachments: [FILE], markdownStoreFiles: [SKILL.filePath],
      createdAt: '2026-01-02T00:00:00Z', inputWriteCompleted: true, acpPromptNotSent: true, interrupted: true }], {
      lifecycleState: 'error', running: false, processing: false, unsentMessageIndices: [2], recoverableUnsentMessageIndices: [2],
    })
    expect(host.querySelector('[data-message-index="2"]')).toBe(row)
    expect(row.querySelector('article')).toBe(article)
    expect(article.getAttribute('data-delivery')).toBe('unsent')
    // Recovery eligibility comes from the explicit native indices, never from the new output-only fact.
    expect(row.querySelector<HTMLButtonElement>('[aria-label="Retry message"]')?.disabled).toBe(false)
    noReplay()
  })

  it('does not turn an extra queue field or ordinary queue acknowledgement into a write fact', async () => {
    await send(); await act(async () => callbacks[0].ack())
    const row = host.querySelector('[data-outgoing-message]')!; const article = row.querySelector('article')!
    publish(HISTORY, { unsentDirectPrompts: [{ ...inputIdentity(), id: 'owned-queue', revision: 1, text: TEXT,
      attachments: [FILE], markdownStoreFiles: [SKILL.filePath], inputWriteCompleted: true }] })
    expect(host.querySelector('[data-unsent-receipt-id="owned-queue"]')).toBe(row)
    expect(row.querySelector('article')).toBe(article)
    expect(article.getAttribute('data-delivery')).toBe('unconfirmed')
    expect(row.querySelector<HTMLButtonElement>('[aria-label="Retry message"]')?.disabled).toBe(true)
    noReplay()
  })

  // R2 additive controls: exact identity loss is not removal, and current receipt state outranks old write facts.
  const nativeInput = (extra: Partial<CppMessage> = {}): CppMessage => ({ ...inputIdentity(), role: 'user', content: TEXT,
    attachments: [FILE], markdownStoreFiles: [SKILL.filePath], createdAt: '2026-01-02T00:00:00Z', ...extra })
  const directInput = (extra: Record<string, unknown> = {}) => ({ ...inputIdentity(), id: 'owned-direct', revision: 1,
    text: TEXT, attachments: [FILE], markdownStoreFiles: [SKILL.filePath], ...extra })
  const publishWindow = (rows: CppMessage[], start: number, count: number) => act(() => {
    useAppStore.setState(state => ({
      sessions: state.sessions.map(session => ({ ...session, messageCount: count })),
      historyStartIndexBySessionId: { 'delivery-fixture': start },
      messages: { 'delivery-fixture': reconcileCppMessages('delivery-fixture', state.messages['delivery-fixture'], rows, true, start) },
    }))
    root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
  })

  it.each(['missing-ID', 'conflicting-origin', 'duplicate-ID', 'missing-row'] as const)(
    'retains the same unknown action when a previously materialized in-window identity becomes %s', async shape => {
      await send(); await act(async () => callbacks[0].ack())
      const row = host.querySelector('[data-outgoing-message]')!; const article = row.querySelector('article')!
      const native = nativeInput({ inputWriteCompleted: true })
      publish([...HISTORY, native], {})
      expect(host.querySelector('[data-message-index="2"]')).toBe(row)
      expect(article.hasAttribute('data-delivery')).toBe(false)
      const changed: CppMessage[] = shape === 'missing-row' ? [] : shape === 'duplicate-ID' ? [native, { ...native }] : [
        shape === 'missing-ID' ? { ...native, nativeInputId: undefined, originRequestId: undefined } : { ...native, originRequestId: 'conflicting-current-origin' },
      ]
      publish([...HISTORY, ...changed], {})
      expect(host.querySelector('[data-outgoing-message]')).toBe(row)
      expect(row.isConnected).toBe(true); expect(row.querySelector('article')).toBe(article)
      expect(article.getAttribute('data-delivery')).toBe('unconfirmed')
      expect(row.textContent).toContain(TEXT.split('\n')[0])
      expect(row.querySelector('[aria-label="File attachments"]')?.textContent).toContain(FILE.name)
      expect(row.querySelector('[aria-label="Skills context"]')).not.toBeNull()
      expect(host.querySelector('[data-message-index="2"]')).not.toBe(row)
      expect(row.querySelector('[aria-label="Retry message"]')).toBeNull()
      publish([...HISTORY, native], {})
      expect(host.querySelector('[data-message-index="2"]')).toBe(row)
      expect(row.querySelector('article')).toBe(article)
      expect(article.hasAttribute('data-delivery')).toBe(false)
      noReplay()
    })

  it('keeps a genuinely off-page bound input owned by history and restores its unknown row on return', async () => {
    await send(); await act(async () => callbacks[0].fail())
    const native = nativeInput()
    publish([...HISTORY, native], {})
    const page: CppMessage[] = [{ role: 'assistant', content: 'Later page, unrelated stored content', createdAt: '2026-01-03T00:00:00Z' }]
    publishWindow(page, 10, 20)
    expect(host.querySelector('[data-outgoing-message]')).toBeNull()
    expect(outgoingArticles()).toHaveLength(0)
    publishWindow([...HISTORY, native], 0, 20)
    expect(host.querySelector('[data-message-index="2"] article')?.getAttribute('data-delivery')).toBe('unconfirmed')
    expect(outgoingArticles()).toHaveLength(1)
    noReplay()
  })

  it.each(['empty-window', 'shrunk-history', 'competing-origin'] as const)('does not guess an off-page disposition for %s', async shape => {
    await send(); await act(async () => callbacks[0].ack())
    const native = nativeInput(); publish([...HISTORY, native], {})
    const row = host.querySelector('[data-message-index="2"]')!; const article = row.querySelector('article')!
    const rows: CppMessage[] = shape === 'empty-window' ? [] : [{ role: 'user', content: 'Different identity stays separate',
      createdAt: '2026-01-03T00:00:00Z', ...(shape === 'competing-origin' ? { nativeInputId: '22222222-2222-4222-8222-222222222222', originRequestId: native.originRequestId } : {}) }]
    publishWindow(rows, shape === 'shrunk-history' ? 0 : 10, shape === 'shrunk-history' ? 1 : 20)
    expect(host.querySelector('[data-outgoing-message]')).toBe(row)
    expect(row.querySelector('article')).toBe(article)
    expect(article.getAttribute('data-delivery')).toBe('unconfirmed')
    noReplay()
  })

  it.each(['pending', 'unsent'] as const)('renders simultaneous native + %s direct receipt as one stable row above a positive write fact', async state => {
    await send(); await act(async () => callbacks[0].ack())
    const row = host.querySelector('[data-outgoing-message]')!; const article = row.querySelector('article')!
    const native = nativeInput({ inputWriteCompleted: true })
    const direct = directInput()
    // No raw index/reference is supplied: exact input + origin and full carriers must preserve this action.
    publish([...HISTORY, native], { lifecycleState: 'error', running: false,
      pendingDirectPrompts: state === 'pending' ? [direct] : [], unsentDirectPrompts: state === 'unsent' ? [direct] : [] })
    assertRetained(row, article, state === 'pending' ? 'sending' : 'unconfirmed')
    expect(host.querySelector('[data-message-index="2"]')).toBe(row)
    expect(host.querySelectorAll('[data-unsent-receipt-id]')).toHaveLength(0)
    const retry = row.querySelector<HTMLButtonElement>('[aria-label="Retry message"]')
    expect(retry === null || retry.disabled).toBe(true)
    expect(row.querySelector('[aria-label="Edit message in new branch"],[aria-label="Revert to message in new branch"]')).toBeNull()
    expect(row.querySelector<HTMLButtonElement>('[aria-label="Remove unsent prompt"]')?.disabled ?? true).toBe(state === 'pending')
    noReplay()
  })

  it.each(['receipt-first', 'native-first'] as const)('keeps the action row through a same-input receipt/native transition (%s)', async order => {
    await send(); await act(async () => callbacks[0].ack())
    const row = host.querySelector('[data-outgoing-message]')!; const article = row.querySelector('article')!
    const native = nativeInput({ inputWriteCompleted: true }); const direct = directInput()
    if (order === 'receipt-first') publish(HISTORY, { unsentDirectPrompts: [direct], lifecycleState: 'error', running: false })
    else publish([...HISTORY, native], {})
    publish([...HISTORY, native], { unsentDirectPrompts: [direct], lifecycleState: 'error', running: false })
    assertRetained(row, article, 'unconfirmed')
    expect(host.querySelector('[data-message-index="2"]')).toBe(row)
    noReplay()
  })

  it('does not suppress the direct carrier when its matching native row is actually hidden', async () => {
    await send(); await act(async () => callbacks[0].ack())
    const row = host.querySelector('[data-outgoing-message]')!; const article = row.querySelector('article')!
    const direct = directInput({ existingUserMessageIndex: 2 })
    publish([...HISTORY, nativeInput({ inputWriteCompleted: true, acpPromptNotSent: true, acpPromptDiscarded: true })],
      { pendingDirectPrompts: [direct] })
    assertRetained(row, article, 'sending')
    expect(host.querySelector('[data-message-index="2"]')).toBeNull()
    expect(host.querySelector('[data-unsent-receipt-id="owned-direct"]')).toBe(row)
    noReplay()
  })

  it.each(['text', 'attachments', 'skills'] as const)('keeps both full carriers when same-identity native and direct %s disagree', async carrier => {
    await send(); await act(async () => callbacks[0].fail())
    const native = nativeInput({ inputWriteCompleted: true, ...(carrier === 'text' ? { content: 'Different stored native bytes' } : {}),
      ...(carrier === 'attachments' ? { attachments: [] } : {}), ...(carrier === 'skills' ? { markdownStoreFiles: [] } : {}) })
    publish([...HISTORY, native], { unsentDirectPrompts: [directInput()], lifecycleState: 'error', running: false })
    const nativeRow = host.querySelector('[data-message-index="2"]')!
    expect(nativeRow.textContent).toContain(carrier === 'text' ? 'Different stored native bytes' : TEXT.split('\n')[0])
    const receiptRow = host.querySelector('[data-unsent-receipt-id="owned-direct"]')!
    expect(receiptRow.textContent).toContain(TEXT.split('\n')[0])
    expect(receiptRow.querySelector('[aria-label="File attachments"]')?.textContent).toContain(FILE.name)
    expect(receiptRow.querySelector('[aria-label="Skills context"]')).not.toBeNull()
    expect(receiptRow.querySelector('article')?.getAttribute('data-delivery')).toBe('unconfirmed')
    noReplay()
  })

  it('keeps one historical native/direct receipt row after losing module presentation records', async () => {
    await send(); await act(async () => callbacks[0].ack())
    publish([...HISTORY, nativeInput({ inputWriteCompleted: true })], { unsentDirectPrompts: [directInput()], lifecycleState: 'error', running: false })
    act(() => root.unmount()); resetOutgoingPresentation()
    root = createRoot(host); act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    expect(outgoingArticles()).toHaveLength(1)
    const article = host.querySelector('[data-message-index="2"] article')!
    expect(article.getAttribute('data-delivery')).toBe('unconfirmed')
    const retry = article.querySelector<HTMLButtonElement>('[aria-label="Retry message"]')
    expect(retry === null || retry.disabled).toBe(true)
    noReplay()
  })

  it.each(['false', 'true', 'unknown', 'stale-revision', 'stale-alias', 'stale-identity-loss', 'remount'] as const)(
    'retires only the captured exact display alias after guarded Remove result %s', async outcome => {
      await send(); await act(async () => callbacks[0].ack())
      const row = host.querySelector('[data-outgoing-message]')!
      const native = nativeInput({ inputWriteCompleted: true }); const direct = directInput()
      publish([...HISTORY, native], { unsentDirectPrompts: [direct], lifecycleState: 'error', running: false })
      let complete: (() => void) | undefined
      const previousQuery = window.cefQuery!
      window.cefQuery = options => {
        const parsed = JSON.parse(options.request)
        if (parsed.action !== 'manageQueuedAcpPrompt') { previousQuery(options); return }
        requests.push(parsed)
        expect(parsed.payload).toEqual({ chatId: 'delivery-fixture', operation: 'remove', index: 0, expected: { id: direct.id, revision: 1 } })
        complete = () => outcome === 'false' || outcome === 'unknown' ? options.onFailure(500, 'Private removal result not confirmed') : options.onSuccess('{}')
      }
      const remove = row.querySelector<HTMLButtonElement>('[aria-label="Remove unsent prompt"]')
      expect(remove).not.toBeNull(); expect(remove!.disabled).toBe(false)
      await act(async () => remove!.click())
      expect(complete).toEqual(expect.any(Function))
      if (outcome === 'stale-revision') publish([...HISTORY, native], { unsentDirectPrompts: [directInput({ revision: 2 })], lifecycleState: 'error', running: false })
      if (outcome === 'stale-alias') publish([...HISTORY, nativeInput({ content: 'Updated authoritative input bytes' })], { unsentDirectPrompts: [directInput({ text: 'Updated authoritative input bytes' })], lifecycleState: 'error', running: false })
      if (outcome === 'stale-identity-loss') publish([...HISTORY, { ...native, originRequestId: 'conflicting-removal-origin' }], { unsentDirectPrompts: [direct], lifecycleState: 'error', running: false })
      if (outcome === 'remount') remount()
      await act(async () => complete!())
      // Native snapshot removal is separate from the reply; no missing-row => success inference is permitted.
      publish(HISTORY, {})
      if (outcome === 'true') expect(host.querySelector('[data-outgoing-message]')).toBeNull()
      else {
        expect(host.querySelector('[data-outgoing-message] article')?.getAttribute('data-delivery')).toBe('unconfirmed')
        if (outcome === 'stale-alias') expect(host.querySelector('[data-outgoing-message]')?.textContent).toContain('Updated authoritative input bytes')
        else expect(outgoingArticles()).toHaveLength(1)
      }
      expect(requests.filter(request => request.action === 'manageQueuedAcpPrompt')).toHaveLength(1)
      expect(sends()).toHaveLength(1)
      expect(requests.filter(request => /retry|resume|discard|edit/i.test(request.action))).toEqual([])
    })
  it.each(['success', 'failure', 'unrelated-change'] as const)(
    'handles a real-store Remove whose receipt-removal projection arrives before its ACK (%s)', async outcome => {
      await send(); await act(async () => callbacks[0].ack())
      const direct = directInput()
      publish(HISTORY, { unsentDirectPrompts: [direct], lifecycleState: 'error', running: false })
      const row = host.querySelector('[data-outgoing-message]')!; const article = row.querySelector('article')!
      const realRemove = useAppStore.getInitialState().removeQueuedAcpPrompt
      expect(useAppStore.getState().removeQueuedAcpPrompt).toBe(realRemove)
      let complete: (() => void) | undefined
      const previousQuery = window.cefQuery!
      window.cefQuery = options => {
        const parsed = JSON.parse(options.request)
        if (parsed.action !== 'manageQueuedAcpPrompt') { previousQuery(options); return }
        requests.push(parsed)
        expect(parsed.payload).toEqual({ chatId: 'delivery-fixture', operation: 'remove', index: 0, expected: { id: direct.id, revision: 1 } })
        complete = () => outcome === 'failure' ? options.onFailure(500, 'Private removal reply not confirmed') : options.onSuccess('{}')
      }
      const remove = row.querySelector<HTMLButtonElement>('[aria-label="Remove unsent prompt"]')!
      expect(remove.disabled).toBe(false)
      await act(async () => remove.click())
      // Source-shaped incoming projection via the real ACP binding reconciler, not a mock action/result.
      // This models receipt removal, not a native execution or push-to-operation causality proof.
      publish(HISTORY, { lifecycleState: 'error', running: false,
        ...(outcome === 'unrelated-change' ? { lastError: 'Unrelated runtime error changed while Remove was pending' } : {}) })
      expect(row.isConnected).toBe(true); expect(row.querySelector('article')).toBe(article)
      expect(article.getAttribute('data-delivery')).toBe('unconfirmed') // Missing receipt alone is never success.
      expect(complete).toEqual(expect.any(Function))
      await act(async () => complete!())
      if (outcome === 'success') expect(host.querySelector('[data-outgoing-message]')).toBeNull()
      else {
        expect(host.querySelector('[data-outgoing-message]')).toBe(row)
        expect(article.getAttribute('data-delivery')).toBe('unconfirmed')
      }
      expect(sends()).toHaveLength(1)
      expect(requests.filter(request => request.action === 'manageQueuedAcpPrompt')).toHaveLength(1)
      expect(requests.filter(request => /retry|resume|discard|edit/i.test(request.action))).toEqual([])
    })

  it.each(['one-fresh', 'two-fresh'] as const)('excludes observed IDs before first-alias uniqueness under deliberately reused origin (%s)', async freshShape => {
    const reusedOrigin = 'private-forced-origin-reuse'
    const oldInputId = '33333333-3333-4333-8333-333333333333'
    const oldInput: CppMessage = { role: 'user', content: 'Previously recorded input with a reused origin',
      nativeInputId: oldInputId, originRequestId: reusedOrigin, inputWriteCompleted: true, createdAt: '2026-01-01T00:00:02Z' }
    publish([...HISTORY, oldInput], {})
    // Controlled generator seam only: no claim that an actual random collision has occurred.
    vi.spyOn(cefBridge, 'createRequestId').mockReturnValue(reusedOrigin)
    await send()
    expect(sends()[0].requestId).toBe(reusedOrigin)
    const row = host.querySelector('[data-outgoing-message]')!; const article = row.querySelector('article')!
    await act(async () => callbacks[0].fail())
    publish([...HISTORY, oldInput], {})
    expect(host.querySelector('[data-outgoing-message]')).toBe(row)
    expect(row.querySelector('article')).toBe(article)
    expect(article.getAttribute('data-delivery')).toBe('unconfirmed')
    expect(row.textContent).toContain(TEXT.split('\n')[0])
    expect(host.querySelector('[data-message-index="2"]')).not.toBe(row)
    expect(host.querySelector('[data-message-index="2"]')?.textContent).toContain(oldInput.content)
    const fresh = nativeInput({ nativeInputId: '44444444-4444-4444-8444-444444444444', inputWriteCompleted: true })
    const another = { ...fresh, nativeInputId: '55555555-5555-4555-8555-555555555555' }
    publish([...HISTORY, oldInput, fresh, ...(freshShape === 'two-fresh' ? [another] : [])], {})
    expect(row.isConnected).toBe(true); expect(row.querySelector('article')).toBe(article)
    if (freshShape === 'one-fresh') {
      expect(host.querySelector('[data-message-index="3"]')).toBe(row)
      expect(article.hasAttribute('data-delivery')).toBe(false)
    } else {
      expect(host.querySelector('[data-outgoing-message]')).toBe(row)
      expect(article.getAttribute('data-delivery')).toBe('unconfirmed')
      expect(host.querySelector('[data-message-index="3"]')).not.toBe(row)
      expect(host.querySelector('[data-message-index="4"]')).not.toBe(row)
    }
    noReplay()
  })

  // Claude follow-up regressions. Real send/discard actions, private fake CEF; no native execution claim.
  const seedManagedDraft = (text: string) => {
    act(() => root.unmount()) // Old view flushes its own draft before this new seed is written.
    writeChatComposerDraft('delivery-fixture', { text, attachments: [FILE] })
    root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
  }
  it.each(['rejected', 'uncertain', 'committed', 'remount', 'push-before-ack', 'replacement', 'conflict', 'foreign-scope', 'push-mismatch'] as const)(
    'keeps exactly one stable recorded managed request row after validated inputRecorded (%s)', async outcome => {
      const text = `@reviewer ${TEXT}`
      seedManagedDraft(text)
      await send()
      expect(sends()[0].payload).toMatchObject({ text, attachments: [FILE], markdownStoreFiles: [SKILL.filePath] })
      if (outcome === 'remount') remount()
      const row = host.querySelector('[data-outgoing-message]')!; const article = row.querySelector('article')!
      const delegationState = ['committed', 'uncertain'].includes(outcome) ? outcome : 'rejected'
      const parentMessage: CppMessage = { role: 'user', content: text, managedOriginRunId: '66666666-6666-4666-8666-666666666666', attachments: [FILE], markdownStoreFiles: [SKILL.filePath], createdAt: '2026-01-02T00:00:00Z' }
      if (outcome === 'push-before-ack') publish([...HISTORY, parentMessage], {})
      if (outcome === 'push-mismatch') publish([...HISTORY, { ...parentMessage, createdAt: '2026-01-02T00:00:02Z' }], {})
      if (outcome === 'foreign-scope') act(() => useAppStore.setState(state => ({ sessions: state.sessions.map(session => ({ ...session, workspaceDirectory: '/another-private-fixture' })) })))
      await act(async () => callbacks[0].ack({ managedMention: { inputRecorded: true, delegationState,
        parentMessageIndex: 2, parentMessage,
        ...(delegationState === 'committed' ? { runId: parentMessage.managedOriginRunId, transcriptChatId: 'private-child' } : { error: 'Child creation was not confirmed after saving the input.' }) } }))
      // Existing receipt is explicit recorded-input evidence, not a local/provider input-write fact.
      if (outcome === 'foreign-scope') {
        expect(useAppStore.getState().messages['delivery-fixture']).toHaveLength(2)
        expect(host.querySelector('[data-outgoing-message]')).toBe(row)
        expect(host.querySelector('[data-message-index="2"]')).toBeNull()
        expect(article.hasAttribute('data-delivery')).toBe(true)
        noReplay(); return
      }
      expect(useAppStore.getState().messages['delivery-fixture']).toHaveLength(3)
      if (outcome === 'push-mismatch') {
        // Same receipt coordinate/origin/content is insufficient when the full recorded row differs.
        expect(host.querySelector('[data-outgoing-message]')).toBe(row)
        expect(row.querySelector('article')).toBe(article)
        expect(article.hasAttribute('data-delivery')).toBe(true)
        expect(host.querySelector('[data-message-index="2"]')).not.toBe(row)
        noReplay(); return
      }
      expect(useAppStore.getState().messages['delivery-fixture'][2].nativeInputId).toBeUndefined()
      expect(host.querySelector('[data-message-index="2"]')).toBe(row)
      expect(row.querySelector('article')).toBe(article)
      expect(outgoingArticles()).toEqual([article])
      expect(article.getAttribute('data-delivery')).toBe('unconfirmed')
      expect(row.querySelector('[aria-label="File attachments"]')?.textContent).toContain(FILE.name)
      expect(row.querySelector('[aria-label="Skills context"]')).not.toBeNull()
      if (delegationState !== 'committed') expect(host.textContent).toContain('Child creation was not confirmed after saving the input.')
      expect(row.querySelector('[aria-label="Retry message"],[aria-label="Edit message in new branch"],[aria-label="Revert to message in new branch"]')).toBeNull()
      if (outcome === 'replacement') {
        publish([...HISTORY, { ...parentMessage, content: 'Reconciled managed parent bytes', createdAt: '2026-01-02T00:00:02Z' }], {})
        expect(host.querySelector('[data-message-index="2"]')).toBe(row)
        expect(row.querySelector('article')).toBe(article)
        expect(row.textContent).toContain('Reconciled managed parent bytes')
        expect(article.getAttribute('data-delivery')).toBe('unconfirmed')
      }
      if (outcome === 'conflict') {
        publish([...HISTORY, parentMessage, { ...parentMessage, createdAt: '2026-01-02T00:00:02Z' }], {})
        expect(host.querySelector('[data-outgoing-message]')).toBe(row)
        expect(row.querySelector('article')).toBe(article)
        expect(article.getAttribute('data-delivery')).toBe('unconfirmed')
        expect(host.querySelector('[data-message-index="2"]')).not.toBe(row)
        expect(host.querySelector('[data-message-index="3"]')).not.toBe(row)
      }
      noReplay()
    })

  it('does not bind an invalid managed receipt or unrelated existing user row by matching content', async () => {
    const text = `@reviewer ${TEXT}`
    seedManagedDraft(text)
    await send(); const row = host.querySelector('[data-outgoing-message]')!; const article = row.querySelector('article')!
    expect(sends()[0].payload).toMatchObject({ text, attachments: [FILE], markdownStoreFiles: [SKILL.filePath] })
    publish([...HISTORY, { role: 'user', content: text, attachments: [FILE], markdownStoreFiles: [SKILL.filePath], createdAt: '2026-01-02T00:00:00Z' }], {})
    await act(async () => callbacks[0].ack({ managedMention: { inputRecorded: true, delegationState: 'rejected', parentMessageIndex: 2,
      parentMessage: { role: 'user', content: text, createdAt: '2026-01-02T00:00:00Z' } } })) // Missing actual sent carriers: rejected by real validator.
    expect(host.querySelector('[data-outgoing-message]')).toBe(row)
    expect(row.querySelector('article')).toBe(article)
    expect(article.getAttribute('data-delivery')).toBe('unconfirmed')
    expect(host.querySelector('[data-message-index="2"]')).not.toBe(row)
    expect(host.textContent).toContain('The saved request could not be confirmed.')
    noReplay()
  })

  it.each(['success', 'failure', 'stale-target', 'unrelated-row'] as const)(
    'settles only exact guarded native discard, then history trim (%s)', async outcome => {
      await send(); await act(async () => callbacks[0].ack())
      const native = nativeInput({ acpPromptNotSent: true, interrupted: true })
      publish([...HISTORY, native], { lifecycleState: 'error', running: false, unsentMessageIndices: [2], recoverableUnsentMessageIndices: [] })
      const row = host.querySelector('[data-message-index="2"]')!; const article = row.querySelector('article')!
      const beforeRows = useAppStore.getState().messages['delivery-fixture']
      const beforeMessage = beforeRows[2]
      expect(useAppStore.getState().discardUnsentMessage).toBe(useAppStore.getInitialState().discardUnsentMessage)
      let complete: (() => void) | undefined
      const previousQuery = window.cefQuery!
      window.cefQuery = options => {
        const parsed = JSON.parse(options.request)
        if (!['getChatMessages', 'manageQueuedAcpPrompt'].includes(parsed.action)) { previousQuery(options); return }
        requests.push(parsed)
        if (parsed.action === 'getChatMessages') {
          expect(parsed.payload).toEqual({ chatId: 'delivery-fixture', limit: 1, before: 3, deferToolCallContent: true, messagesDigest: '' })
          options.onSuccess(JSON.stringify({ chatId: 'delivery-fixture', startIndex: 2, totalCount: 3, messagesDigest: 'private-discard-digest', messages: [native] }))
        } else {
          expect(parsed.payload).toEqual({ chatId: 'delivery-fixture', operation: 'discardUnsent', messageIndex: 2, expectedMessagesDigest: 'private-discard-digest' })
          complete = () => outcome === 'failure' ? options.onFailure(500, 'Discard not confirmed') : options.onSuccess('{}')
        }
      }
      const remove = row.querySelector<HTMLButtonElement>('[aria-label="Remove unsent prompt"]')!
      expect(remove.disabled).toBe(false)
      await act(async () => remove.click())
      expect(complete).toEqual(expect.any(Function))
      if (outcome === 'stale-target') publish([...HISTORY, { ...native, content: 'Revised native bytes remain owned' }], { lifecycleState: 'error', running: false, unsentMessageIndices: [2] })
      if (outcome === 'unrelated-row') act(() => useAppStore.setState(state => ({ messages: { 'delivery-fixture': state.messages['delivery-fixture'].map((message, index) => index === 1 ? { ...message, content: 'Other native row changed' } : message) } })))
      await act(async () => complete!())
      if (outcome === 'success') {
        const discarded = useAppStore.getState().messages['delivery-fixture']
        expect(discarded).not.toBe(beforeRows)
        expect(discarded[0]).toBe(beforeRows[0]); expect(discarded[1]).toBe(beforeRows[1])
        expect(discarded[2]).toEqual({ ...beforeMessage, acpPromptDiscarded: true })
      }
      publish(HISTORY, {}) // A later authoritative trim is not itself a removal receipt.
      if (outcome === 'success') expect(host.querySelector('[data-outgoing-message]')).toBeNull()
      else {
        expect(host.querySelector('[data-outgoing-message]')).toBe(row)
        expect(row.querySelector('article')).toBe(article)
        expect(article.getAttribute('data-delivery')).toBe('unconfirmed')
        if (outcome === 'stale-target') expect(row.textContent).toContain('Revised native bytes remain owned')
      }
      expect(sends()).toHaveLength(1)
      expect(requests.filter(request => request.action === 'getChatMessages')).toHaveLength(1)
      expect(requests.filter(request => request.action === 'manageQueuedAcpPrompt')).toHaveLength(1)
      expect(requests.filter(request => /retry|resume|edit/i.test(request.action))).toEqual([])
    })

  it('isolates a throwing recorded-input observer after actual validated parent projection', async () => {
    const observer = Object.assign(vi.fn(), { onInputRecorded: vi.fn((identity: { message: Message }) => {
      expect(useAppStore.getState().messages['delivery-fixture']).toContain(identity.message)
      throw new Error('Private optional presentation observer failed')
    }) })
    let result: Promise<boolean> | undefined
    await act(async () => { result = useAppStore.getState().sendAcpPrompt('delivery-fixture', TEXT, [FILE], false, observer) })
    await act(async () => callbacks[0].ack({ managedMention: { inputRecorded: true, delegationState: 'rejected', parentMessageIndex: 2,
      parentMessage: { role: 'user', content: TEXT, managedOriginRunId: '77777777-7777-4777-8777-777777777777', attachments: [FILE], markdownStoreFiles: [SKILL.filePath], createdAt: '2026-01-02T00:00:00Z' },
      error: 'Child rejected after saving input' } }))
    expect(await result).toBe(true)
    expect(observer).toHaveBeenCalledTimes(1)
    expect(observer.onInputRecorded).toHaveBeenCalledTimes(1)
    expect(useAppStore.getState().messages['delivery-fixture']).toHaveLength(3)
    expect(host.textContent).toContain('Child rejected after saving input')
    noReplay()
  })

  it('keeps the managed action row when canonical replacement follows actual recorded observation before React commit', async () => {
    const text = `@reviewer ${TEXT}`
    const replacementText = `${text}\nCanonical replacement before React commit.`
    const parentMessage: CppMessage = { role: 'user', content: text,
      managedOriginRunId: '88888888-8888-4888-8888-888888888888', attachments: [FILE], markdownStoreFiles: [SKILL.filePath], createdAt: '2026-01-02T00:00:00Z' }
    act(() => root.unmount())
    writeChatComposerDraft('delivery-fixture', { text, attachments: [FILE] })
    const session = useAppStore.getState().sessions[0]
    const scope = outgoingScope(session.id, session.createdAt)
    let notifications = 0
    let oldObjectPresentAtNotification = false
    let articleStillPrecommit = false
    let sameScopedOriginAfterReplacement = false
    let uniqueManagedRows = 0
    let notificationError: unknown
    let observedMessage: Message | undefined
    let replacementMessage: Message | undefined
    let originalRow: Element | undefined
    let originalArticle: Element | undefined
    // Register before the new React subscriber: this synchronous listener deterministically exercises the
    // real recorded observation -> canonical replacement window, without timers, mocks or alias injection.
    const unsubscribe = subscribeOutgoingAttempts(() => {
      const recorded = readOutgoingAttempts(scope).find(attempt => attempt.recordedManagedInput)?.recordedManagedInput
      if (!recorded || notifications !== 0) return
      notifications += 1
      observedMessage = recorded.message
      oldObjectPresentAtNotification = useAppStore.getState().messages[session.id].includes(observedMessage) &&
        observedMessage.managedOriginRunId === parentMessage.managedOriginRunId
      articleStillPrecommit = originalArticle !== undefined && originalRow?.querySelector('article') === originalArticle &&
        !originalArticle.textContent?.includes('Canonical replacement before React commit.')
      // Canonical reconciliation changes the Message object while retaining only the scoped native managed
      // origin. Neither a native input ID nor an input-write fact is fabricated.
      try {
      useAppStore.setState(state => ({ messages: { ...state.messages,
        [session.id]: reconcileCppMessages(session.id, state.messages[session.id], [...HISTORY, {
          ...parentMessage, content: replacementText, createdAt: '2026-01-02T00:00:02Z',
        }], true, 0),
      } }))
      replacementMessage = useAppStore.getState().messages[session.id][2]
      sameScopedOriginAfterReplacement = replacementMessage.managedOriginRunId === observedMessage.managedOriginRunId
      uniqueManagedRows = useAppStore.getState().messages[session.id].filter(message => message.managedOriginRunId === parentMessage.managedOriginRunId).length
      } catch (error) { notificationError = error }
    })
    try {
      root = createRoot(host)
      act(() => root.render(<ChatView session={session} />))
      await send()
      expect(sends()[0].payload).toMatchObject({ text, attachments: [FILE], markdownStoreFiles: [SKILL.filePath] })
      originalRow = host.querySelector('[data-outgoing-message]')!
      originalArticle = originalRow.querySelector('article')!
      await act(async () => callbacks[0].ack({ managedMention: { inputRecorded: true, delegationState: 'rejected',
        parentMessageIndex: 2, parentMessage, error: 'Child creation was not confirmed after saving the input.' } }))
      expect(notificationError).toBeUndefined()
      expect(notifications).toBe(1)
      expect(oldObjectPresentAtNotification).toBe(true)
      expect(articleStillPrecommit).toBe(true)
      expect(observedMessage).toBeDefined(); expect(replacementMessage).toBeDefined()
      expect(replacementMessage).not.toBe(observedMessage)
      expect(sameScopedOriginAfterReplacement).toBe(true)
      expect(uniqueManagedRows).toBe(1)
      expect(replacementMessage?.nativeInputId).toBeUndefined()
      expect(replacementMessage?.inputWriteCompleted).toBeUndefined()
      expect(useAppStore.getState().messages[session.id][2]).toBe(replacementMessage)
      expect(host.querySelector('[data-message-index="2"]')).toBe(originalRow)
      expect(originalRow.querySelector('article')).toBe(originalArticle)
      expect(outgoingArticles()).toEqual([originalArticle])
      expect(originalArticle.getAttribute('data-delivery')).toBe('unconfirmed')
      expect(originalArticle.textContent).toContain('Canonical replacement before React commit.')
      expect(originalRow.querySelector('[aria-label="File attachments"]')?.textContent).toContain(FILE.name)
      expect(originalRow.querySelector('[aria-label="Skills context"]')).not.toBeNull()
      expect(originalRow.querySelector('[aria-label="Retry message"],[aria-label="Edit message in new branch"],[aria-label="Revert to message in new branch"]')).toBeNull()
      expect(host.textContent).toContain('Child creation was not confirmed after saving the input.')
      noReplay()
    } finally { unsubscribe() }
  })

  describe('explicit exact-receipt retry of referenced retained input', () => {
    const receiptIdentity = { id: 'referenced-recovery-input', revision: 7 }
    const recoveryInputIdentity = { nativeInputId: '99999999-9999-4999-8999-999999999999', originRequestId: 'private-original-recovery-request' }
    const receipt = (change: Record<string, unknown> = {}) => ({ ...receiptIdentity, ...recoveryInputIdentity,
      text: TEXT, attachments: [FILE], markdownStoreFiles: [SKILL.filePath], existingUserMessageIndex: 0,
      uamAgentId: 'build', goalMode: false, goalId: '', ...change })
    const native: CppMessage = { ...recoveryInputIdentity, role: 'user', content: TEXT,
      attachments: [FILE], markdownStoreFiles: [SKILL.filePath], createdAt: '2026-01-02T00:00:00Z',
      acpPromptNotSent: true, interrupted: true }
    const seedRecovery = (loaded: boolean, mode: 'referenced' | 'unreferenced' | 'pending' | 'readonly' = 'referenced') => {
      act(() => root.unmount())
      removeComposerDrafts(['delivery-fixture'])
      resetOutgoingPresentation()
      const recovery = receipt(mode === 'unreferenced' ? { existingUserMessageIndex: undefined } : {})
      const rows = loaded ? [native] : []
      useAppStore.setState(state => ({
        sessions: state.sessions.map(session => ({ ...session, importedReadOnly: mode === 'readonly', messageCount: rows.length })),
        messages: { 'delivery-fixture': reconcileCppMessages('delivery-fixture', undefined, rows, true, 0) },
        historyStartIndexBySessionId: { 'delivery-fixture': 0 },
        // This is the native direct-held projection: no visible FIFO, no queueHeld bit. Index retry
        // allowlists stay absent; this control must use the exact receipt, not retryFailedMessage.
        acpBindingBySessionId: { 'delivery-fixture': acpBindingFromCppChat(chat({
          lifecycleState: 'stopped', running: false, processing: false, queueHeld: false, queuedPrompts: [],
          pendingDirectPrompts: mode === 'pending' ? [recovery] : [],
          unsentDirectPrompts: mode === 'pending' ? [] : [recovery],
          unsentMessageIndices: [], recoverableUnsentMessageIndices: [],
        }), undefined) },
      }))
      root = createRoot(host)
      act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
      const row = host.querySelector('[data-unsent-receipt-id="referenced-recovery-input"]') ?? host.querySelector('[data-message-index="0"]')
      expect(row).not.toBeNull()
      return { row: row!, article: row!.querySelector('article')!, recovery }
    }
    const retryOperations = () => requests.filter(request => request.action === 'manageQueuedAcpPrompt' && request.payload.operation === 'retry')
    const noSendOrResume = () => {
      expect(sends()).toHaveLength(0)
      expect(requests.filter(request => request.action === 'retryFailedMessage' || /resume/i.test(request.action))).toEqual([])
    }
    const assertRecoveryResources = (row: Element, article: Element) => {
      expect(row.isConnected).toBe(true)
      expect(row.querySelector('article')).toBe(article)
      expect(article.getAttribute('data-delivery')).toBe('unconfirmed')
      for (const line of TEXT.split('\n')) expect(row.textContent).toContain(line)
      expect(row.querySelector('[aria-label="File attachments"]')?.textContent).toContain(FILE.name)
      expect(row.querySelector('[aria-label="Skills context"]')).not.toBeNull()
    }
    const holdRetryReply = () => {
      let reply: (typeof callbacks)[number] | undefined
      const previousQuery = window.cefQuery!
      window.cefQuery = options => {
        const parsed = JSON.parse(options.request)
        if (parsed.action !== 'manageQueuedAcpPrompt') { previousQuery(options); return }
        requests.push(parsed)
        const callback = { settled: false,
          ack: (data: unknown = {}) => { if (!callback.settled) { callback.settled = true; options.onSuccess(JSON.stringify(data)) } },
          fail: () => { if (!callback.settled) { callback.settled = true; options.onFailure(409, 'Private referenced input retry was refused; original input is kept.') } },
        }
        callbacks.push(callback)
        reply = callback
      }
      return () => { expect(reply).toBeDefined(); return reply! }
    }

    it.each(['verified', 'unverified', 'wrong-time', 'changed-bytes'] as const)(
      'retains one native-verified legacy recovery row through hydration (%s)', mode => {
        seedRecovery(false)
        const legacyNative: CppMessage = { ...native, nativeInputId: undefined, originRequestId: undefined,
          ...(mode === 'changed-bytes' ? { content: TEXT + ' changed' } : {}) }
        const legacyReceipt = receipt({ nativeInputId: undefined, originRequestId: undefined,
          ...(mode === 'unverified' ? {} : { referencedMessageCreatedAt: mode === 'wrong-time' ? '2026-01-03T00:00:00Z' : native.createdAt }),
          ...(mode === 'verified' ? { retryAllowed: false } : {}) })
        const stopped = { lifecycleState: 'stopped', running: false, processing: false, queueHeld: false,
          queuedPrompts: [], pendingDirectPrompts: [], unsentDirectPrompts: [legacyReceipt],
          unsentMessageIndices: [], recoverableUnsentMessageIndices: [] }
        publish([], stopped)
        expect(outgoingArticles()).toHaveLength(1)
        publish([legacyNative], stopped)
        const rows = outgoingArticles()
        expect(rows).toHaveLength(mode === 'verified' ? 1 : 2)
        const recoveryRow = rows.find(row => row.getAttribute('data-delivery') === 'unconfirmed')
        expect(recoveryRow).toBeDefined()
        expect(host.querySelector('[data-delivery="sending"]')).toBeNull()
        expect(recoveryRow!.querySelector<HTMLButtonElement>('[aria-label="Retry message"]')?.disabled).toBe(mode === 'verified')
        expect(recoveryRow!.querySelector<HTMLButtonElement>('[aria-label="Remove unsent prompt"]')?.disabled).toBe(false)
        expect(recoveryRow!.querySelector('[aria-label="File attachments"]')?.textContent).toContain(FILE.name)
        expect(recoveryRow!.querySelector('[aria-label="Skills context"]')).not.toBeNull()
        noSendOrResume()
        expect(requests.filter(request => request.action === 'manageQueuedAcpPrompt')).toEqual([])
      })

    it.each([
      { loaded: false, outcome: 'ack' }, { loaded: true, outcome: 'ack' },
      { loaded: false, outcome: 'rejection' }, { loaded: true, outcome: 'rejection' },
    ] as const)('retries only the exact referenced receipt without fabricating delivery (loaded=$loaded, $outcome)', async ({ loaded, outcome }) => {
      const { row, article, recovery } = seedRecovery(loaded)
      const historyBefore = useAppStore.getState().messages['delivery-fixture']
      const getReply = holdRetryReply()
      const retry = row.querySelector<HTMLButtonElement>('[aria-label="Retry message"]')
      expect(retry).not.toBeNull()
      // Baseline fails here, before any use of the prospective store action.
      expect(retry!.disabled).toBe(false)
      await act(async () => retry!.click())
      expect(retryOperations()).toHaveLength(1)
      expect(retryOperations()[0].payload).toEqual({ chatId: 'delivery-fixture', operation: 'retry', index: 0, expected: receiptIdentity })
      expect(useAppStore.getState().messages['delivery-fixture']).toBe(historyBefore)
      expect(retry!.disabled).toBe(true)
      await act(async () => retry!.click()) // Pending operation cannot admit a second deliberate send.
      expect(retryOperations()).toHaveLength(1)
      noSendOrResume()
      await act(async () => outcome === 'ack' ? getReply().ack() : getReply().fail())
      assertRecoveryResources(row, article)
      expect(useAppStore.getState().messages['delivery-fixture']).toBe(historyBefore)
      expect(useAppStore.getState().acpBindingBySessionId['delivery-fixture'].unsentDirectPrompts).toEqual([recovery])
      expect(retryOperations()).toHaveLength(1)
      expect(requests.filter(request => request.action === 'manageQueuedAcpPrompt')).toHaveLength(1)
      if (outcome === 'rejection') expect(row.textContent).toContain('Private referenced input retry was refused; original input is kept.')
      expect(row.querySelector<HTMLButtonElement>('[aria-label="Retry message"]')?.disabled).toBe(false)
      noSendOrResume()
    })

    it.each(['unreferenced', 'pending', 'readonly'] as const)('does not grant referenced-receipt Retry authority for %s input', async mode => {
      const { row } = seedRecovery(false, mode)
      const retry = row.querySelector<HTMLButtonElement>('[aria-label="Retry message"]')
      expect(retry === null || retry.disabled).toBe(true)
      if (retry) await act(async () => retry.click())
      expect(retryOperations()).toEqual([])
      expect(requests.filter(request => request.action === 'manageQueuedAcpPrompt')).toEqual([])
      expect(useAppStore.getState().messages['delivery-fixture']).toEqual([])
      noSendOrResume()
    })

    it('uses the currently revised receipt when the visible Retry control is clicked', async () => {
      const { row } = seedRecovery(false)
      const retry = row.querySelector<HTMLButtonElement>('[aria-label="Retry message"]')
      expect(retry).not.toBeNull(); expect(retry!.disabled).toBe(false)
      const revised = receipt({ revision: 8 })
      publish([], { lifecycleState: 'stopped', running: false, processing: false, queueHeld: false,
        queuedPrompts: [], pendingDirectPrompts: [], unsentDirectPrompts: [revised] })
      const current = host.querySelector<HTMLButtonElement>('[data-unsent-receipt-id="referenced-recovery-input"] [aria-label="Retry message"]')
      expect(current).not.toBeNull(); expect(current!.disabled).toBe(false)
      const getReply = holdRetryReply()
      await act(async () => current!.click())
      expect(retryOperations()).toHaveLength(1)
      expect(retryOperations()[0].payload).toEqual({ chatId: 'delivery-fixture', operation: 'retry', index: 0,
        expected: { id: receiptIdentity.id, revision: 8 } })
      await act(async () => getReply().ack())
      expect(useAppStore.getState().messages['delivery-fixture']).toEqual([])
      noSendOrResume()
    })

    it.each(['revised', 'removed'] as const)('rejects a captured stale expected receipt before IPC after it is %s', async shape => {
      const { row } = seedRecovery(false)
      const retry = row.querySelector<HTMLButtonElement>('[aria-label="Retry message"]')
      expect(retry).not.toBeNull(); expect(retry!.disabled).toBe(false)
      publish([], { lifecycleState: 'stopped', running: false, processing: false, queueHeld: false,
        queuedPrompts: [], pendingDirectPrompts: [], unsentDirectPrompts: shape === 'revised' ? [receipt({ revision: 8 })] : [] })
      // Real prospective store action; the enabled assertion above is the intended baseline RED,
      // not a TypeError from calling an absent implementation.
      const action = useAppStore.getState() as ReturnType<typeof useAppStore.getState> & {
        retryUnsentAcpPrompt: (sessionId: string, expected: { id: string; revision: number }) => Promise<boolean>
      }
      await expect(action.retryUnsentAcpPrompt('delivery-fixture', receiptIdentity)).resolves.toBe(false)
      expect(retryOperations()).toEqual([])
      expect(requests.filter(request => request.action === 'manageQueuedAcpPrompt')).toEqual([])
      expect(useAppStore.getState().messages['delivery-fixture']).toEqual([])
      noSendOrResume()
    })
  })

})
