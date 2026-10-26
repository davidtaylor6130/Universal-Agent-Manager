// Originally UNRUN R2 controls; task-1217 exercises them through the in-process bridge fixture, without fabricating native delivery ACKs.
import { act } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { ChatView } from './ChatView'
import { useAppStore } from '../../store/useAppStore'
import { acpBindingFromCppChat, reconcileCppMessages } from '../../store/cpp/reconcile'
import { sanitizeCppAcpSession } from '../../store/cpp/sanitizers'
import type { CppChat, CppMessage } from '../../store/cpp/types'
import { captureContextReference, formatContextReferences } from '../../utils/contextReferences'
import { readChatComposerDraft, removeComposerDrafts, writeChatComposerDraft } from '../../utils/composerDraftStorage'
import { outgoingScope, readOutgoingAttempts, resetOutgoingPresentation } from '../../utils/outgoingMessagePresentation'

vi.mock('../chat/ManagedSubagentPanel', () => ({ ManagedSubagentPanel: () => null }))
;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
const ID = 'identity-bridge-private-fixture'
const BIRTH = '2026-01-01T00:00:00.000Z'
const TEXT = 'Exact  private input\nKeep these bytes.'
const HISTORY: CppMessage[] = [
  { role: 'user', content: 'Earlier request', createdAt: BIRTH },
  { role: 'assistant', content: 'Earlier answer', createdAt: '2026-01-01T00:00:01.000Z' },
]
type Query = Parameters<NonNullable<Window['cefQuery']>>[0]
type Envelope = { action: string; requestId: string; payload: Record<string, unknown> }
type Started = { requestId: string; sessionId: string; createdAtMs: number }
const chat = (provider = 'codex-cli', mode = 'default', birth = BIRTH): CppChat => ({
  id: ID, title: 'Identity bridge fixture', providerId: provider, folderId: 'fixture-folder',
  createdAt: birth, updatedAt: birth, executionHostId: 'local',
  acpSession: sanitizeCppAcpSession({ providerId: provider,
    protocolKind: provider === 'claude-cli' ? 'claude-code-stream-json' : 'codex-app-server',
    sessionId: 'private-native-fixture', running: true, processing: false, lifecycleState: 'ready',
    currentModeId: mode, availableModes: [{ id: 'default', name: 'Default' }, { id: 'plan', name: 'Plan' }],
    queueHeld: false, queuedPrompts: [], pendingDirectPrompts: [], unsentDirectPrompts: [],
    unsentMessageIndices: [], recoverableUnsentMessageIndices: [], pendingPermission: null, pendingUserInput: null }),
})

describe('ChatView exact outgoing action identity', () => {
  let previous: ReturnType<typeof useAppStore.getState>
  let previousCef: Window['cefQuery']
  let previousUrl: string
  let previousHistory: unknown
  let previousStorage: PropertyDescriptor | undefined
  let host: HTMLDivElement
  let root: Root
  let mounted: boolean
  let requests: Envelope[]
  let callbacks: Array<{ settled: boolean; ack: () => void; fail: () => void }>
  let onSend: ((envelope: Envelope) => void) | undefined
  const scope = (birth = BIRTH) => outgoingScope(ID, new Date(birth))
  const sends = () => requests.filter(request => request.action === 'sendAcpPrompt')
  const render = async () => {
    if (!mounted) { root = createRoot(host); mounted = true }
    await act(async () => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
  }
  const unmount = () => { if (mounted) { act(() => root.unmount()); mounted = false } }
  const send = async () => {
    const button = host.querySelector<HTMLButtonElement>('[aria-label="Send prompt"]')!
    expect(button).not.toBeNull(); expect(button.disabled).toBe(false)
    await act(async () => button.click())
  }
  const enter = async (text: string) => {
    const textarea = host.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')!
    await act(async () => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')!.set!.call(textarea, text)
      textarea.dispatchEvent(new Event('input', { bubbles: true }))
    })
  }
  const noNativeReplay = () => expect(requests.filter(request => /retry|resume|remove|discard/i.test(request.action))).toEqual([])

  beforeEach(async () => {
    previous = useAppStore.getState(); previousCef = window.cefQuery
    previousUrl = window.location.href; previousHistory = window.history.state
    requests = []; callbacks = []; onSend = undefined; mounted = false
    vi.stubGlobal('ResizeObserver', class { observe() {} unobserve() {} disconnect() {} })
    vi.stubGlobal('fetch', vi.fn(() => Promise.reject(new Error('Private fixture forbids network'))))
    const values = new Map<string, string>()
    const storage: Storage = { get length() { return values.size }, clear: () => values.clear(),
      getItem: key => values.get(key) ?? null, setItem: (key, value) => { values.set(key, String(value)) },
      removeItem: key => { values.delete(key) }, key: index => [...values.keys()][index] ?? null }
    previousStorage = Object.getOwnPropertyDescriptor(window, 'localStorage')
    vi.stubGlobal('localStorage', storage)
    Object.defineProperty(window, 'localStorage', { configurable: true, value: storage })
    window.history.replaceState({}, '', '/')
    resetOutgoingPresentation(); removeComposerDrafts([ID]); writeChatComposerDraft(ID, { text: TEXT, attachments: [] })
    const birth = new Date(BIRTH)
    useAppStore.setState({ ...useAppStore.getInitialState(),
      workingDisplayMode: 'verbose', collapsibleWorkSections: true, expandWorkTraces: true,
      activeSessionId: ID, chatSearchNavigation: null,
      folders: [{ id: 'fixture-folder', name: 'Private fixture', parentId: null, directory: '/private-fixture', isExpanded: true, createdAt: birth }],
      sessions: [{ id: ID, name: 'Identity fixture', providerId: 'codex-cli', viewMode: 'chat', folderId: 'fixture-folder',
        workspaceDirectory: '/private-fixture', executionHostId: 'local', createdAt: birth, updatedAt: birth, messageCount: 2 }],
      messages: { [ID]: reconcileCppMessages(ID, undefined, HISTORY, true, 0) }, historyStartIndexBySessionId: { [ID]: 0 },
      providers: [
        { id: 'codex-cli', name: 'Codex', shortName: 'Codex', color: '#22c55e', description: '', outputMode: 'cli', supportsCli: true, supportsStructured: true, structuredProtocol: 'codex-app-server' },
        { id: 'claude-cli', name: 'Claude', shortName: 'Claude', color: '#cbaa89', description: '', outputMode: 'cli', supportsCli: true, supportsStructured: true, structuredProtocol: 'claude-code-stream-json' },
      ],
      acpBindingBySessionId: { [ID]: acpBindingFromCppChat(chat(), undefined) }, cliBindingBySessionId: {},
      markdownStoreAttachedBySessionId: { [ID]: [] },
      uamAgentsBySessionId: { [ID]: [{ id: 'build', description: 'Build', builtIn: true }] },
      refreshUamAgents: vi.fn(async () => true), getVcsCommitStatus: vi.fn(async () => null),
    }, true)
    expect(vi.isMockFunction(useAppStore.getState().sendAcpPrompt)).toBe(false)
    window.cefQuery = (query: Query) => {
      const envelope: Envelope = JSON.parse(query.request); requests.push(envelope)
      if (envelope.action === 'setGoal') { query.onSuccess(JSON.stringify({ goalId: 'private-goal' })); return }
      if (envelope.action !== 'sendAcpPrompt') { query.onSuccess('{}'); return }
      const callback = { settled: false,
        ack: () => { if (!callback.settled) { callback.settled = true; query.onSuccess('{}') } },
        fail: () => { if (!callback.settled) { callback.settled = true; query.onFailure(500, 'Private reply unknown') } },
      }
      callbacks.push(callback)
      onSend?.(envelope) // A real synchronous cefQuery boundary, BEFORE an admission callback.
    }
    host = document.createElement('div'); document.body.append(host)
    await render()
  })

  afterEach(async () => {
    await act(async () => callbacks.forEach(callback => callback.fail()))
    unmount(); host.remove(); resetOutgoingPresentation(); removeComposerDrafts([ID])
    useAppStore.setState(previous, true); window.cefQuery = previousCef
    window.history.replaceState(previousHistory, '', previousUrl)
    vi.restoreAllMocks(); vi.unstubAllGlobals()
    if (previousStorage) Object.defineProperty(window, 'localStorage', previousStorage)
    else Reflect.deleteProperty(window, 'localStorage')
  })

  it('publishes the store-minted action before a synchronous native-shaped push and before ACK', async () => {
    let observedBeforePush = false
    onSend = envelope => {
      const attempts = readOutgoingAttempts(scope())
      expect(attempts).toHaveLength(1)
      expect(attempts[0].actionCorrelationId).toBe(envelope.requestId)
      expect(attempts[0].phase).toBe('pending')
      expect(callbacks[0].settled).toBe(false)
      observedBeforePush = true
      // Materialization only: no native correlation/delivery receipt or submitted/confirmed bit is fabricated.
      const rows = [...HISTORY, { role: 'user' as const, content: TEXT, createdAt: '2026-01-01T00:00:02.000Z' }]
      useAppStore.setState(state => ({ messages: { [ID]: reconcileCppMessages(ID, state.messages[ID], rows, true, 0) } }))
    }
    await send()
    expect(observedBeforePush).toBe(true); expect(sends()).toHaveLength(1)
    expect(callbacks[0].settled).toBe(false)
    await act(async () => callbacks[0].ack())
    expect(readOutgoingAttempts(scope())[0].actionCorrelationId).toBe(sends()[0].requestId)
    noNativeReplay()
  })

  it.each(['success', 'failure'] as const)('a throwing presentation observer still dispatches once and returns the real %s response', async outcome => {
    const history = useAppStore.getState().messages[ID]
    const observer = vi.fn((_identity: Started) => { throw new Error('Private presentation failure') })
    const result = useAppStore.getState().sendAcpPrompt(ID, TEXT, [], false, observer)
    expect(observer).toHaveBeenCalledTimes(1); expect(sends()).toHaveLength(1)
    expect(observer.mock.calls[0][0]).toEqual({ requestId: sends()[0].requestId, sessionId: ID, createdAtMs: new Date(BIRTH).getTime() })
    if (outcome === 'success') callbacks[0].ack(); else callbacks[0].fail()
    await expect(result).resolves.toBe(outcome === 'success')
    expect(sends()).toHaveLength(1)
    expect(useAppStore.getState().messages[ID]).toBe(history)
    noNativeReplay()
  })

  it('two identical real store calls mint distinct action IDs without letting callers select an ID', async () => {
    const identities: Started[] = []
    const original = useAppStore.getState().messages[ID]
    const first = useAppStore.getState().sendAcpPrompt(ID, TEXT, [], false, identity => identities.push(identity))
    const second = useAppStore.getState().sendAcpPrompt(ID, TEXT, [], false, identity => identities.push(identity))
    expect(sends()).toHaveLength(2); expect(identities).toHaveLength(2)
    expect(identities.map(identity => identity.requestId)).toEqual(sends().map(request => request.requestId))
    expect(identities[0].requestId).not.toBe(identities[1].requestId)
    expect(identities.every(identity => identity.requestId.length > 0 && identity.requestId.length <= 128)).toBe(true)
    expect(sends()[0].payload).toEqual(sends()[1].payload)
    callbacks.forEach(callback => callback.ack())
    await expect(Promise.all([first, second])).resolves.toEqual([true, true])
    expect(useAppStore.getState().messages[ID]).toBe(original)
    noNativeReplay()
  })

  it.each(['same-birth-remount', 'recreated-chat'] as const)('settles a stale view callback only in its original presentation scope (%s)', async departure => {
    await send(); expect(sends()).toHaveLength(1)
    const original = readOutgoingAttempts(scope())[0]
    expect(original.actionCorrelationId).toBe(sends()[0].requestId)
    unmount()
    const newBirth = departure === 'recreated-chat' ? '2026-01-02T00:00:00.000Z' : BIRTH
    if (departure === 'recreated-chat') {
      removeComposerDrafts([ID]); writeChatComposerDraft(ID, { text: 'New incarnation draft', attachments: [] })
      useAppStore.setState(state => ({
        sessions: state.sessions.map(session => ({ ...session, createdAt: new Date(newBirth), updatedAt: new Date(newBirth) })),
        messages: { [ID]: reconcileCppMessages(ID, undefined, HISTORY, true, 0) },
        acpBindingBySessionId: { [ID]: acpBindingFromCppChat(chat('codex-cli', 'default', newBirth), undefined) },
      }))
    }
    await render()
    if (departure === 'same-birth-remount') expect(readOutgoingAttempts(scope())).toHaveLength(1)
    else expect(readOutgoingAttempts(scope(newBirth))).toEqual([])
    await act(async () => callbacks[0].ack())
    expect(readOutgoingAttempts(scope())[0]).toMatchObject({ key: original.key, actionCorrelationId: sends()[0].requestId, phase: 'accepted' })
    if (departure === 'recreated-chat') {
      expect(readOutgoingAttempts(scope(newBirth))).toEqual([])
      expect(host.querySelector('[data-outgoing-message]')).toBeNull()
      expect(host.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')!.value).toBe('New incarnation draft')
    }
    expect(sends()).toHaveLength(1); noNativeReplay()
  })

  // David's task-1217 decision on 2026-10-23 includes the originally selected references in authored goal/Claude input.
  // Generated plan commands keep their independent-draft exclusion test below.
  // Contract: docs/documentation-review.md#referenced-context-action-sends.
  it.each(['goal', 'claude-plan'] as const)('uses the bridge for frozen %s input with selected references while preserving a newer draft and later references', async route => {
    const reference = captureContextReference({ id: 'private-selected-reference', kind: 'message', sourceChatId: 'source-chat', sourceMessageIndex: 7,
      executionHostId: 'local', workspaceDirectory: '/private-fixture', capturedAt: BIRTH, label: 'Selected quote', text: 'SELECTED CONTEXT FOR THIS AUTHORED MESSAGE' })!
    const later = { ...reference, id: 'later-reference', text: 'CONTEXT FOR THE NEXT MESSAGE' }
    const exact = route === 'goal' ? 'Exact  goal input.' : TEXT
    unmount()
    writeChatComposerDraft(ID, { text: route === 'goal' ? `/goal ${exact}` : exact, attachments: [], references: [reference] })
    if (route === 'claude-plan') useAppStore.setState(state => ({
      sessions: state.sessions.map(session => ({ ...session, providerId: 'claude-cli', approvalMode: 'plan' })),
      acpBindingBySessionId: { [ID]: acpBindingFromCppChat(chat('claude-cli', 'plan'), undefined) },
    }))
    await render(); await send()
    if (route === 'claude-plan') {
      expect(sends()).toHaveLength(0)
      const keep = [...host.querySelectorAll<HTMLButtonElement>('button')].find(button => button.textContent === 'Keep planning')!
      expect(keep).toBeTruthy(); await act(async () => keep.click())
    } else expect(requests.filter(request => request.action === 'setGoal')).toHaveLength(1)
    expect(sends()).toHaveLength(1)
    expect(sends()[0].payload).toMatchObject({ text: `${exact}\n\n${formatContextReferences([reference])}`, attachments: [], markdownStoreFiles: [], steerNow: false })
    expect(String(sends()[0].payload.text)).not.toContain(later.text)
    expect(readOutgoingAttempts(scope())[0].actionCorrelationId).toBe(sends()[0].requestId)
    await enter('A newer unrelated draft')
    act(() => writeChatComposerDraft(ID, { ...readChatComposerDraft(ID), text: 'A newer unrelated draft', references: [reference, later] }))
    await act(async () => callbacks[0].ack())
    expect(host.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')!.value).toBe('A newer unrelated draft')
    act(() => window.dispatchEvent(new Event('pagehide')))
    expect(readChatComposerDraft(ID)).toMatchObject({ text: 'A newer unrelated draft', references: [later] })
    expect(sends()).toHaveLength(1); noNativeReplay()
  })

  it('bridges exact Codex Deny plan input while preserving the independent composer text, file and reference', async () => {
    const file = { id: 'independent-file', name: 'independent.txt', type: 'file', size: 4, path: '/private-fixture/independent.txt' }
    const reference = captureContextReference({ id: 'private-plan-reference', kind: 'message', sourceChatId: 'unrelated-chat', sourceMessageIndex: 9,
      executionHostId: 'local', workspaceDirectory: '/private-fixture', capturedAt: BIRTH, label: 'Independent quote', text: 'DO NOT BORROW THIS PLAN QUOTE' })!
    unmount()
    writeChatComposerDraft(ID, { text: 'Independent composer draft', attachments: [file], references: [reference] })
    const independent = readChatComposerDraft(ID)
    useAppStore.setState(state => ({
      sessions: state.sessions.map(session => ({ ...session, approvalMode: 'plan' })),
      messages: { [ID]: reconcileCppMessages(ID, undefined, [HISTORY[0], { ...HISTORY[1], planSummary: 'A bounded private plan.' }], true, 0) },
      acpBindingBySessionId: { [ID]: acpBindingFromCppChat(chat('codex-cli', 'plan'), undefined) },
    }))
    await render()
    const deny = [...host.querySelectorAll<HTMLButtonElement>('button')].find(button => button.textContent === 'Deny')!
    expect(deny).toBeTruthy(); await act(async () => deny.click())
    expect(sends()).toHaveLength(1)
    expect(sends()[0].payload).toMatchObject({ text: 'Do not proceed with this plan. Please revise it before making changes.', attachments: [], markdownStoreFiles: [], steerNow: false })
    expect(readOutgoingAttempts(scope())[0].actionCorrelationId).toBe(sends()[0].requestId)
    await act(async () => callbacks[0].ack())
    expect(host.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')!.value).toBe('Independent composer draft')
    expect(readChatComposerDraft(ID)).toEqual(independent)
    expect(sends()).toHaveLength(1); noNativeReplay()
  })
})
