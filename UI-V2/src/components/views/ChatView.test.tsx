import { clearSubagentView, subagentRouteFor } from '../../utils/subagentNavigation'
import { defaultChatGridLayout, readChatGridLayout, writeChatGridLayout } from '../../utils/chatGridStorage'
import { captureContextReference, formatContextReferences } from '../../utils/contextReferences'
import { parseUamPushPayload } from '../../store/push/uamPush'
import { act, StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { buildUamAgentCycle, buildVisibleWorkSections, ChatView, matchesUamAgentCycleShortcut, projectUnsentPrompts, reviewAssistantLabel } from './ChatView'
import { useAppStore, type VcsCommitStatus } from '../../store/useAppStore'
import type { Message } from '../../types/message'
import { acpBindingFromCppChat, reconcileCppMessages } from '../../store/cpp/reconcile'
import type { CppChat } from '../../store/cpp/types'
import { sanitizeCppAcpSession, sanitizeCppSettings } from '../../store/cpp/sanitizers'
import { readChatComposerDraft, removeComposerDrafts, writeChatComposerDraft } from '../../utils/composerDraftStorage'
import { resetOutgoingPresentation } from '../../utils/outgoingMessagePresentation'
import { ManagedSubagentConversation } from '../chat/ManagedSubagentConversation'
import { NativeConversation } from '../chat/SubagentPaneContent'

const realSendAcpPrompt = useAppStore.getState().sendAcpPrompt

// Outgoing presentation rows outlive ChatView by design; isolate them per test.
beforeEach(() => resetOutgoingPresentation())

// Legacy ChatView fixtures have no managed-run server. The explicit integration case
// below uses a strict launch result and owned root listing; panel/navigation contracts
// have connected tests rather than treating generic `{}` mocks as ledger data.
const managedPanelFixture = vi.hoisted(() => ({ enabled: false }))
vi.mock('../chat/ManagedSubagentPanel', async importOriginal => {
  const actual = await importOriginal<typeof import('../chat/ManagedSubagentPanel')>()
  return { ...actual, ManagedSubagentPanel: (props: Parameters<typeof actual.ManagedSubagentPanel>[0]) => managedPanelFixture.enabled ? <actual.ManagedSubagentPanel {...props} /> : null }
});

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

// Plan / Accept-Edits / Auto / Memory now live inside the composer "Options" popover.
function openComposerOptions(host: HTMLElement) {
  act(() => {
    (host.querySelector('button[aria-label="Options"]') as HTMLButtonElement | null)
      ?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
  })
}

function openComposerGoals(host: HTMLElement) {
  const trigger = host.querySelector<HTMLButtonElement>('button[aria-label="Goals"]')
  if (trigger?.getAttribute('aria-expanded') === 'false') act(() => trigger.click())
}

function openWorkspaceActions(host: HTMLElement) {
  act(() => {
    (host.querySelector('button[aria-label="Workspace actions"]') as HTMLButtonElement | null)
      ?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
  })
  const menus = Array.from(document.body.querySelectorAll<HTMLElement>('[role="menu"][aria-label="Workspace actions"]'))
  return menus[menus.length - 1] ?? null
}

function enableSupportedManualCompactionFixture() {
  useAppStore.setState(state => ({
    sessions: state.sessions.map(session => session.id === 'chat-1' ? { ...session, providerId: 'codex-cli', modelId: 'gpt-5.4' } : session),
    acpBindingBySessionId: { ...state.acpBindingBySessionId, 'chat-1': acpBindingFromCppChat({
      id: 'chat-1', title: 'Codex compaction fixture', folderId: 'default', providerId: 'codex-cli', modelId: 'gpt-5.4',
      createdAt: '2026-01-01T00:00:00Z', updatedAt: '2026-01-01T00:00:01Z',
      acpSession: sanitizeCppAcpSession({ running: true, processing: false, lifecycleState: 'ready', providerId: 'codex-cli',
        protocolKind: 'codex-app-server', sessionId: 'native-1', threadId: 'native-1', currentModelId: 'gpt-5.4',
        manualCompaction: { available: true, providerId: 'codex-cli', modelId: 'gpt-5.4', nativeSessionId: 'native-1' },
        pendingPermission: null, pendingUserInput: null, queuedPrompts: [], pendingDirectPrompts: [], unsentDirectPrompts: [] }),
    }, undefined) },
  }))
}

describe('ChatView', () => {
  it.each([
    { name: 'clean remote refusal', refresh: true, guidance: 'The chat or its saved history changed, so this refresh was not saved. Current history was kept. Try refreshing again.' },
    { name: 'unconfirmed remote save', refresh: true, guidance: 'UAM could not confirm that the refreshed history was saved. Reopen the chat before retrying.' },
    { name: 'ordinary saved read with identical text', refresh: false, guidance: 'The chat or its saved history changed, so this refresh was not saved. Current history was kept. Try refreshing again.' },
  ])('labels the saved-history recovery action from request context: $name', async ({ refresh, guidance }) => {
    const previousLoad = useAppStore.getState().loadSessionMessages
    const loadSessionMessages = vi.fn().mockResolvedValue(undefined)
    useAppStore.setState({
      messages: { 'chat-1': [{ id: 'retained', role: 'user', content: 'Retained authored history', createdAt: new Date() }] },
      chatHistoryErrorBySessionId: { 'chat-1': guidance },
      chatHistoryErrorWasRefreshBySessionId: { 'chat-1': refresh },
      statusLine: 'Earlier retained warning',
      loadSessionMessages,
    })
    const host = document.createElement('div'); document.body.append(host)
    const root = createRoot(host)
    try {
      act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
      const notice = host.querySelector<HTMLElement>('.uam-notice')!
      expect(notice.getAttribute('role')).toBe('alert')
      expect(notice.textContent).toContain(refresh ? 'History refresh failed' : 'Chat history unavailable')
      expect(notice.textContent).toContain(guidance)
      const actionLabel = refresh ? 'Reload history' : 'Retry'
      const action = Array.from(notice.querySelectorAll<HTMLButtonElement>('button')).find(button => button.textContent === actionLabel)!
      expect(action).toBeTruthy()
      expect(host.textContent).toContain('Retained authored history')
      await act(async () => action.click())
      expect(loadSessionMessages).toHaveBeenCalledExactlyOnceWith('chat-1', true)
      expect(useAppStore.getState().statusLine).toBe('Earlier retained warning')
    } finally {
      act(() => root.unmount()); host.remove()
      useAppStore.setState({ loadSessionMessages: previousLoad, chatHistoryErrorBySessionId: {}, chatHistoryErrorWasRefreshBySessionId: {} })
    }
  })

  it('shows loading for missing history, preserves the draft and distinguishes empty history from failure', async () => {
    const previousLoad = useAppStore.getState().loadSessionMessages
    const loadSessionMessages = vi.fn().mockResolvedValue(false)
    useAppStore.setState((state) => ({ messages: {}, chatHistoryErrorBySessionId: {}, loadSessionMessages,
      chatHistoryReadBySessionId: { 'chat-1': { requestId: 'fixture-read', birth: state.sessions[0].createdAt.getTime() } },
      sessions: state.sessions.map((session) => ({ ...session, messageCount: 50 })) }))
    const host = document.createElement('div'); document.body.append(host)
    const root = createRoot(host)
    const render = () => root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    try {
      act(render)
      expect(host.textContent).toContain('Loading chat history…')
      expect(host.querySelector('.uam-message-list')?.getAttribute('aria-busy')).toBe('true')
      const input = host.querySelector<HTMLTextAreaElement>('textarea')!
      act(() => {
        Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')!.set!.call(input, 'Draft while history loads')
        input.dispatchEvent(new Event('input', { bubbles: true }))
      })
      act(() => useAppStore.setState({ messages: { 'chat-1': [{ id: 'loaded', role: 'user', content: 'Loaded history', createdAt: new Date() }] } }))
      expect(host.textContent).toContain('Loaded history')
      expect(host.textContent).not.toContain('Loading chat history…')
      expect(input.value).toBe('Draft while history loads')
      act(() => {
        useAppStore.setState((state) => ({ messages: { 'chat-1': [] }, sessions: state.sessions.map((session) => ({ ...session, messageCount: 0 })) }))
        render()
      })
      expect(host.textContent).toContain('What would you like to work on?')
      expect(host.querySelector('.uam-message-list')?.getAttribute('aria-busy')).toBe('false')
      act(() => useAppStore.setState({ chatHistoryErrorBySessionId: { 'chat-1': 'Could not read the saved transcript.' } }))
      expect(host.textContent).toContain('Chat history unavailable')
      expect(host.textContent).not.toContain('What would you like to work on?')
      await act(async () => Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Retry')!.click())
      expect(loadSessionMessages).toHaveBeenCalledWith('chat-1', true)
    } finally {
      act(() => root.unmount()); host.remove()
      useAppStore.setState({ loadSessionMessages: previousLoad, chatHistoryErrorBySessionId: {}, chatHistoryReadBySessionId: {} })
    }
  })

  it.each([false, true])('shows retained unsent input instead of an indefinite missing-history animation (read pending=%s)', (pending) => {
    const originalLoad = useAppStore.getState().loadSessionMessages
    const load = vi.fn()
    useAppStore.setState(state => ({ messages: {}, chatHistoryReadBySessionId: pending ? { 'chat-1': { requestId: 'owned-read', birth: state.sessions[0].createdAt.getTime() } } : {}, chatHistoryErrorBySessionId: {}, loadSessionMessages: load,
      sessions: state.sessions.map(session => ({ ...session, messageCount: 1 })),
      acpBindingBySessionId: { 'chat-1': { ...state.acpBindingBySessionId['chat-1'], processing: false, running: false,
        lifecycleState: 'stopped', queueHeld: true, queuedPrompts: [], pendingDirectPrompts: [],
        unsentDirectPrompts: [{ id: 'retained-input', revision: 1, text: 'Retain the exact authored input', attachments: [] }] } } }))
    const host = document.createElement('div'); document.body.append(host); const root = createRoot(host)
    try {
      act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
      expect(host.textContent).not.toContain('Loading chat history…')
      expect(host.querySelector('.conversation-skeleton')).toBeNull()
      expect(host.querySelector('.uam-message-list')?.getAttribute('aria-busy')).toBe('false')
      expect(host.textContent).toContain('Retain the exact authored input')
      expect(host.textContent).toContain('Delivery not confirmed')
      expect(host.querySelectorAll('[data-unsent-receipt-id="retained-input"]')).toHaveLength(1)
      expect(host.querySelector('[data-unsent-receipt-id="retained-input"] article')?.getAttribute('data-delivery')).toBe('unconfirmed')
      expect(host.textContent).not.toContain('What would you like to work on?')
      expect(host.textContent?.includes('Chat history unavailable')).toBe(!pending)
      expect(load).not.toHaveBeenCalled()
    } finally { act(() => root.unmount()); host.remove(); useAppStore.setState({ loadSessionMessages: originalLoad, chatHistoryReadBySessionId: {} }) }
  })

  it('does not claim an old-birth read is work on the current chat', () => {
    useAppStore.setState(state => ({ messages: {}, chatHistoryErrorBySessionId: {},
      sessions: state.sessions.map(session => ({ ...session, messageCount: 2 })),
      chatHistoryReadBySessionId: { 'chat-1': { requestId: 'old', birth: state.sessions[0].createdAt.getTime() - 1 } } }))
    const host = document.createElement('div'); document.body.append(host); const root = createRoot(host)
    try {
      act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
      expect(host.querySelector('.conversation-skeleton')).toBeNull()
      expect(host.textContent).toContain('Chat history unavailable')
      expect(host.textContent).not.toContain('What would you like to work on?')
    } finally { act(() => root.unmount()); host.remove(); useAppStore.setState({ chatHistoryReadBySessionId: {} }) }
  })

  it('keeps a real cold selection pending without a premature unavailable error', async () => {
    let finishSelection: (() => void) | undefined
    const requests: string[] = []
    vi.stubGlobal('cefQuery', ({ request, onSuccess }: { request: string; onSuccess: (text: string) => void }) => {
      const envelope = JSON.parse(request)
      requests.push(envelope.action)
      if (envelope.action === 'selectSession') finishSelection = () => onSuccess('{}')
      else if (envelope.action === 'getChatMessages') onSuccess(JSON.stringify({ chatId: 'chat-1', messages: [], startIndex: 0, totalCount: 0 }))
      else onSuccess('{}')
    })
    useAppStore.setState(state => ({ activeSessionId: null, messages: {}, chatHistoryErrorBySessionId: {}, chatHistoryReadBySessionId: {},
      sessions: state.sessions.map(session => ({ ...session, messageCount: 1 })) }))
    const select = useAppStore.getState().setActiveSession
    expect(vi.isMockFunction(select)).toBe(false)
    const host = document.createElement('div'); document.body.append(host); const root = createRoot(host)
    try {
      act(() => { select('chat-1'); root.render(<ChatView session={useAppStore.getState().sessions[0]} />) })
      expect(finishSelection).toBeTypeOf('function')
      expect(requests.filter(action => action === 'getChatMessages')).toHaveLength(0)
      expect(host.textContent).not.toContain('Chat history unavailable')
      expect(host.querySelector('.uam-message-list')?.getAttribute('aria-busy')).toBe('true')
      await act(async () => { finishSelection!(); await Promise.resolve() })
      expect(requests.filter(action => action === 'getChatMessages')).toHaveLength(1)
      expect(host.querySelector('.conversation-skeleton')).toBeNull()
    } finally { act(() => root.unmount()); host.remove(); await act(async () => { finishSelection?.(); await Promise.resolve() }) }
  })

  it('does not mislabel an active targeted history search as an unavailable read', () => {
    useAppStore.setState(state => ({ messages: {}, chatHistoryErrorBySessionId: {}, chatHistoryReadBySessionId: {},
      sessions: state.sessions.map(session => ({ ...session, messageCount: 2, messagesDigest: 'search-snapshot', userMessageIndices: [0] })),
      chatSearchNavigation: { chatId: 'chat-1', phase: 'loading', generation: 1, queryRequestId: 'query', messageIndex: 0, messagesDigest: 'search-snapshot', anchored: false } }))
    const host = document.createElement('div'); document.body.append(host); const root = createRoot(host)
    try {
      act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
      expect(host.textContent).not.toContain('Chat history unavailable')
      expect(host.textContent).not.toContain('What would you like to work on?')
    } finally { act(() => root.unmount()); host.remove(); useAppStore.setState({ chatSearchNavigation: null }) }
  })

  it('keeps held queued input visible without a false empty-history heading', () => {
    useAppStore.setState(state => ({ messages: {}, chatHistoryErrorBySessionId: {},
      sessions: state.sessions.map(session => ({ ...session, messageCount: 2 })),
      chatHistoryReadBySessionId: { 'chat-1': { requestId: 'pending-read', birth: state.sessions[0].createdAt.getTime() } },
      acpBindingBySessionId: { 'chat-1': { ...state.acpBindingBySessionId['chat-1'], processing: false, running: false,
        lifecycleState: 'stopped', queueHeld: true, pendingDirectPrompts: [], unsentDirectPrompts: [], turnEvents: [], toolCalls: [],
        queuedPrompts: [{ id: 'queued-own', revision: 1, text: 'Keep this queued input', attachments: [], uamAgentId: '', markdownStoreFiles: [], goalMode: false, goalId: '' }] } } }))
    const host = document.createElement('div'); document.body.append(host); const root = createRoot(host)
    try {
      act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
      expect(host.textContent).toContain('Keep this queued input')
      expect(host.querySelector('.conversation-skeleton')).toBeNull()
      expect(host.textContent).not.toContain('What would you like to work on?')
      expect(host.querySelector('.uam-composer-dock--centred')).toBeNull()
    } finally { act(() => root.unmount()); host.remove(); useAppStore.setState({ chatHistoryReadBySessionId: {} }) }
  })

  it('centres the composer for an empty new chat, keeps the same textarea when the first message moves it down, and not for an existing chat still loading', () => {
    const dockCentred = (host: HTMLElement) => host.querySelector('.uam-composer-dock')!.classList.contains('uam-composer-dock--centred')
    useAppStore.setState((state) => ({ messages: {}, chatHistoryErrorBySessionId: {}, loadSessionMessages: vi.fn(),
      sessions: state.sessions.map((session) => ({ ...session, messageCount: 0 })) }))
    const host = document.createElement('div'); document.body.append(host)
    const root = createRoot(host)
    try {
      act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
      expect(dockCentred(host)).toBe(true)
      const input = host.querySelector('textarea')
      act(() => useAppStore.setState({ messages: { 'chat-1': [{ id: 'first', role: 'user', content: 'Hello', createdAt: new Date() }] } }))
      expect(dockCentred(host)).toBe(false)
      expect(host.querySelector('textarea')).toBe(input)
      act(() => useAppStore.setState((state) => ({ messages: {}, sessions: state.sessions.map((session) => ({ ...session, messageCount: 50 })) })))
      act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
      expect(dockCentred(host)).toBe(false)
    } finally {
      act(() => root.unmount()); host.remove()
      useAppStore.setState({ chatHistoryErrorBySessionId: {} })
    }
  })

  it('settles goal creation and permits the next submission after StrictMode effect replay', async () => {
    const session = { ...useAppStore.getState().sessions[0], providerId: 'gemini-cli' }
    const originalCreate = useAppStore.getState().setGoal
    const originalSend = useAppStore.getState().sendAcpPrompt
    let finishCreate!: (value: { ok: boolean; goalId: string }) => void
    const create = vi.fn(() => new Promise<{ ok: boolean; goalId: string }>((resolve) => { finishCreate = resolve }))
    const send = vi.fn(async () => true)
    useAppStore.setState({ setGoal: create, sendAcpPrompt: send })
    const host = document.createElement('div'); document.body.appendChild(host); const root = createRoot(host)
    const enter = (text: string) => act(() => {
      const input = host.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')!
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')!.set!.call(input, text)
      input.dispatchEvent(new Event('input', { bubbles: true }))
    })
    const submit = () => act(() => host.querySelector('form')!.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true })))
    try {
      act(() => root.render(<StrictMode><ChatView session={session} /></StrictMode>))
      enter('/goal First objective'); submit()
      expect(create).toHaveBeenCalledTimes(1)
      await act(async () => finishCreate({ ok: true, goalId: 'strict-goal' }))
      expect(send).toHaveBeenCalledWith(session.id, 'First objective', [], undefined, expect.any(Function))
      expect(host.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')!.value).toBe('')
      enter('/goal Next objective'); submit()
      expect(create).toHaveBeenCalledTimes(2)
    } finally { act(() => root.unmount()); host.remove(); useAppStore.setState({ setGoal: originalCreate, sendAcpPrompt: originalSend }) }
  })
  it('shows goals in a bounded composer popup and keeps edits separate from its unsent draft', async () => {
    const goal = { id: 'goal-popup', chatId: 'chat-1', objective: 'Ship the popup', status: 'active' as const, executionOwner: 'uam' as const, createdAt: new Date(), updatedAt: new Date() }
    const send = vi.fn(async () => true)
    const edit = vi.fn(async () => ({ ok: true }))
    const originalSend = useAppStore.getState().sendAcpPrompt
    const originalEdit = useAppStore.getState().updateGoalObjective
    useAppStore.setState({ goalsByChatId: { 'chat-1': [goal] }, activeGoalIdByChatId: { 'chat-1': goal.id }, sendAcpPrompt: send, updateGoalObjective: edit })
    const host = document.createElement('div'); document.body.appendChild(host); const root = createRoot(host)
    try {
      act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
      expect(host.querySelector('.uam-goal-banner')).toBeNull()
      const trigger = host.querySelector<HTMLButtonElement>('button[aria-label="Goals"]')!
      expect(trigger.textContent).toContain('Ship the popup')
      const composer = host.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')!
      act(() => { Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')!.set!.call(composer, 'Unsent next prompt'); composer.dispatchEvent(new Event('input', { bubbles: true })) })
      openComposerGoals(host)
      let popup = document.querySelector<HTMLElement>('[aria-label="Chat goals"]')!
      expect(popup).toBeTruthy()
      act(() => { composer.focus(); composer.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true })) })
      expect(document.querySelector('[aria-label="Chat goals"]')).toBeNull()
      expect(composer.value).toBe('Unsent next prompt')
      openComposerGoals(host)
      popup = document.querySelector<HTMLElement>('[aria-label="Chat goals"]')!
      act(() => popup.querySelector<HTMLButtonElement>('[aria-label="Goal actions"]')!.click())
      act(() => document.querySelector<HTMLButtonElement>('button[aria-label="Edit goal"]')!.click())
      const input = document.querySelector<HTMLTextAreaElement>('[aria-label="Edit goal"] textarea')!
      act(() => { Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')!.set!.call(input, 'Updated goal'); input.dispatchEvent(new Event('input', { bubbles: true })) })
      await act(async () => document.querySelector<HTMLButtonElement>('[aria-label="Edit goal"] button[type="submit"]')!.click())
      expect(edit).toHaveBeenCalledWith('chat-1', goal.id, 'Updated goal')
      expect(send).not.toHaveBeenCalled()
      expect(composer.value).toBe('Unsent next prompt')
      act(() => popup.querySelector<HTMLButtonElement>('[aria-label="Goal actions"]')!.click())
      act(() => document.querySelector<HTMLButtonElement>('button[aria-label="Edit goal"]')!.click())
      act(() => document.activeElement!.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true })))
      expect(document.querySelector('[aria-label="Chat goals"]')).toBeTruthy()
      expect(document.querySelector('[role="dialog"][aria-label="Edit goal"]')).toBeNull()
      expect(document.activeElement).toBe(popup.querySelector('[aria-label="Goal actions"]'))
      expect(document.querySelector('[aria-modal="true"]')).toBeNull()
      act(() => document.activeElement!.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true })))
      expect(document.querySelector('[aria-label="Chat goals"]')).toBeNull()
      expect(document.activeElement).toBe(trigger)
      act(() => trigger.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowDown', bubbles: true, cancelable: true })))
      const reopened = document.querySelector<HTMLElement>('[aria-label="Chat goals"]')!
      act(() => reopened.querySelector<HTMLButtonElement>('[aria-label="Goal actions"]')!.click())
      act(() => document.querySelector<HTMLButtonElement>('button[aria-label="Edit goal"]')!.click())
      act(() => useAppStore.setState({ goalsByChatId: { 'chat-1': [{ ...goal, id: 'replacement', objective: 'Replacement goal' }] }, activeGoalIdByChatId: { 'chat-1': 'replacement' } }))
      expect(document.querySelector('[role="dialog"][aria-label="Edit goal"]')).toBeNull()
      expect(document.querySelector('[aria-label="Chat goals"]')).toBeNull()
      expect(trigger.textContent).toContain('Replacement goal')
      expect(composer.value).toBe('Unsent next prompt')
    } finally { act(() => root.unmount()); host.remove(); useAppStore.setState({ sendAcpPrompt: originalSend, updateGoalObjective: originalEdit }) }
  })
  it('isolates delayed goal edit failures after switching chats and leaves the new chat goal usable', async () => {
    const first = useAppStore.getState().sessions[0]
    const second = { ...first, id: 'chat-2', name: 'Second chat' }
    const goal = { id: 'goal-a', chatId: first.id, objective: 'First goal', status: 'active' as const, executionOwner: 'uam' as const, createdAt: new Date(), updatedAt: new Date() }
    let finish!: (value: { ok: boolean; error?: string }) => void
    const edit = vi.fn().mockImplementationOnce(() => new Promise((resolve) => { finish = resolve })).mockResolvedValue({ ok: true })
    const original = useAppStore.getState().updateGoalObjective
    useAppStore.setState({ sessions: [first, second], updateGoalObjective: edit, goalsByChatId: { 'chat-1': [goal], 'chat-2': [{ ...goal, id: 'goal-b', chatId: second.id, objective: 'Second goal' }] }, activeGoalIdByChatId: { 'chat-1': 'goal-a', 'chat-2': 'goal-b' } })
    const host = document.createElement('div'); document.body.appendChild(host); const root = createRoot(host)
    const saveGoal = async () => {
      openComposerGoals(host)
      act(() => document.querySelector<HTMLButtonElement>('[aria-label="Chat goals"] [aria-label="Goal actions"]')!.click())
      act(() => document.querySelector<HTMLButtonElement>('button[aria-label="Edit goal"]')!.click())
      await act(async () => document.querySelector<HTMLButtonElement>('[aria-label="Edit goal"] button[type="submit"]')!.click())
    }
    try {
      act(() => root.render(<ChatView session={first} />))
      await saveGoal()
      expect(edit).toHaveBeenCalledWith(first.id, goal.id, 'First goal')
      act(() => { useAppStore.setState({ activeSessionId: second.id }); root.render(<ChatView session={second} />) })
      expect(document.querySelector('[aria-label="Chat goals"]')).toBeNull()
      const composer = host.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')!
      act(() => { Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')!.set!.call(composer, 'Second chat draft'); composer.dispatchEvent(new Event('input', { bubbles: true })) })
      await act(async () => finish({ ok: false, error: 'Old chat failed' }))
      expect(composer.value).toBe('Second chat draft')
      expect(document.querySelector('[role="dialog"][aria-label="Edit goal"]')).toBeNull()
      expect(host.textContent).not.toContain('Old chat failed')
      await saveGoal()
      expect(edit).toHaveBeenLastCalledWith(second.id, 'goal-b', 'Second goal')
    } finally { act(() => root.unmount()); host.remove(); useAppStore.setState({ updateGoalObjective: original }) }
  })
  it('targets the owning goal from either pane without submitting either composer draft', async () => {
    const first = useAppStore.getState().sessions[0]
    const second = { ...first, id: 'chat-2', name: 'Other pane' }
    const goal = { id: 'goal-a', chatId: first.id, objective: 'First pane goal', status: 'active' as const, executionOwner: 'uam' as const, createdAt: new Date(), updatedAt: new Date() }
    const pause = vi.fn(async () => ({ ok: true }))
    const send = vi.fn(async () => true)
    const originalPause = useAppStore.getState().updateGoalStatus
    const originalSend = useAppStore.getState().sendAcpPrompt
    useAppStore.setState({ sessions: [first, second], activeSessionId: first.id, updateGoalStatus: pause, sendAcpPrompt: send, goalsByChatId: { 'chat-1': [goal], 'chat-2': [{ ...goal, id: 'goal-b', chatId: second.id, objective: 'Second pane goal' }] }, activeGoalIdByChatId: { 'chat-1': 'goal-a', 'chat-2': 'goal-b' } })
    const host = document.createElement('div'); document.body.appendChild(host); const root = createRoot(host)
    try {
      act(() => root.render(<><div data-test-pane="a"><ChatView session={first} /></div><div data-test-pane="b"><ChatView session={second} /></div></>))
      for (const [pane, chatId, goalId] of [['a', first.id, 'goal-a'], ['b', second.id, 'goal-b']]) {
        const element = host.querySelector<HTMLElement>(`[data-test-pane="${pane}"]`)!
        const composer = element.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')!
        act(() => { Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')!.set!.call(composer, `${pane} draft`); composer.dispatchEvent(new Event('input', { bubbles: true })) })
        openComposerGoals(element)
        const popup = Array.from(document.querySelectorAll<HTMLElement>('[aria-label="Chat goals"]')).find((candidate) => candidate.textContent?.includes(pane === 'a' ? 'First pane goal' : 'Second pane goal'))!
        act(() => popup.querySelector<HTMLButtonElement>('[aria-label="Goal actions"]')!.click())
        await act(async () => document.querySelector<HTMLButtonElement>('[aria-label="Pause goal"]')!.click())
        expect(pause).toHaveBeenLastCalledWith(chatId, goalId, 'paused')
        expect(composer.value).toBe(`${pane} draft`)
      }
      expect(send).not.toHaveBeenCalled()
      expect(useAppStore.getState().activeSessionId).toBe(first.id)
    } finally { act(() => root.unmount()); host.remove(); useAppStore.setState({ updateGoalStatus: originalPause, sendAcpPrompt: originalSend }) }
  })
  it.each(['slash', 'native', 'armed'] as const)('keeps late %s goal creation replies out of a new visit to the same chat', async (kind) => {
    const first = { ...useAppStore.getState().sessions[0], providerId: 'gemini-cli' }
    const second = { ...first, id: 'chat-2', name: 'Second chat' }
    let finishCreate!: (value: { ok: boolean; goalId?: string }) => void
    let finishSend!: (value: boolean) => void
    const create = vi.fn().mockImplementationOnce(() => new Promise((resolve) => { finishCreate = resolve })).mockImplementation(() => new Promise(() => {}))
    const send = vi.fn(() => new Promise<boolean>((resolve) => { finishSend = resolve }))
    const originalCreate = useAppStore.getState().setGoal
    const originalSend = useAppStore.getState().sendAcpPrompt
    useAppStore.setState((state) => ({ sessions: [first, second], setGoal: create, sendAcpPrompt: send,
      providers: state.providers.map((provider) => provider.id === 'gemini-cli' ? { ...provider, nativeGoalCommand: kind === 'native' ? '/ralph' : '' } : provider),
      acpBindingBySessionId: { 'chat-1': { ...state.acpBindingBySessionId['chat-1'], lifecycleState: 'ready', processing: false, pendingPermission: null, currentModeId: 'default' } } }))
    const host = document.createElement('div'); document.body.appendChild(host); const root = createRoot(host)
    const enter = (text: string) => act(() => {
      const input = host.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')!
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')!.set!.call(input, text)
      input.dispatchEvent(new Event('input', { bubbles: true }))
    })
    const submit = () => act(() => host.querySelector('form')!.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true })))
    try {
      act(() => root.render(<ChatView session={first} />))
      if (kind === 'armed') { openComposerOptions(host); act(() => document.querySelector<HTMLButtonElement>('button[title="Use the next message as a goal"]')!.click()) }
      enter(kind === 'slash' ? '/goal Old objective' : kind === 'native' ? '/ralph Old objective' : 'Old objective')
      submit()
      expect(create).toHaveBeenCalledTimes(1)
      act(() => { useAppStore.setState({ activeSessionId: second.id }); root.render(<ChatView session={second} />) })
      await act(async () => finishCreate({ ok: true, goalId: 'old-created-goal' }))
      expect(send).toHaveBeenCalledWith(first.id, kind === 'native' ? '/ralph Old objective' : 'Old objective', [], undefined, expect.any(Function))
      expect(host.textContent).not.toContain('Goal was created, but')
      act(() => { useAppStore.setState({ activeSessionId: first.id }); root.render(<ChatView session={first} />) })
      enter('/goal New objective')
      submit()
      expect(create).toHaveBeenCalledTimes(2)
      await act(async () => finishSend(false))
      expect(host.textContent).not.toContain('Goal was created, but')
      expect((host.querySelector('.uam-composer-textarea') as HTMLTextAreaElement).value).toBe('/goal New objective')
      submit()
      expect(create).toHaveBeenCalledTimes(2)
    } finally { act(() => root.unmount()); host.remove(); useAppStore.setState({ setGoal: originalCreate, sendAcpPrompt: originalSend }) }
  })
  it('renders a smaller companion window on initial load and after switching chats', () => {
    const previousPath = window.location.pathname
    window.history.pushState({}, '', '/companion')
    const firstSession = { ...useAppStore.getState().sessions[0], importedReadOnly: true }
    const secondSession = { ...firstSession, id: 'chat-2', name: 'Second chat' }
    const buildMessages = (chatId: string) => Array.from({ length: 60 }, (_, index) => ({
      id: `${chatId}-message-${index}`,
      sessionId: chatId,
      role: index % 2 === 0 ? 'user' as const : 'assistant' as const,
      content: `${chatId} message ${index}`,
      createdAt: new Date(index),
    }))
    useAppStore.setState({
      sessions: [firstSession, secondSession],
      messages: { 'chat-1': buildMessages('chat-1'), 'chat-2': buildMessages('chat-2') },
      acpBindingBySessionId: {
        'chat-1': useAppStore.getState().acpBindingBySessionId['chat-1'],
        'chat-2': { ...useAppStore.getState().acpBindingBySessionId['chat-1'], sessionId: 'native-2' },
      },
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => root.render(<ChatView session={firstSession} />))
    expect(host.textContent).not.toContain('chat-1 message 0')
    expect(host.textContent).toContain('chat-1 message 59')
    expect(host.textContent).toContain('Show earlier messages')

    act(() => root.render(<ChatView session={secondSession} />))
    expect(host.textContent).not.toContain('chat-2 message 0')
    expect(host.textContent).toContain('chat-2 message 59')

    act(() => root.unmount())
    host.remove()
    window.history.pushState({}, '', previousPath)
  })

  it('builds work metadata from the visible turn boundary and preserves its full summary', () => {
    const messages: Message[] = Array.from({ length: 400 }, (_, index) => ({
      id: `message-${index}`,
      sessionId: 'chat-1',
      role: index % 2 === 0 ? 'user' : 'assistant',
      content: '',
      processingTimeMs: index === 200 ? 7000 : index === 201 ? 3000 : 0,
      createdAt: new Date(index),
    }))

    const sections = buildVisibleWorkSections(messages, 201)
    const firstVisibleSection = sections.get(201)!

    expect(sections.size).toBe(200)
    expect(sections.has(0)).toBe(false)
    expect(firstVisibleSection.firstIndex).toBe(200)
    expect(firstVisibleSection.id).toBe('message-200')
    expect(firstVisibleSection.workedSeconds).toBe(7)
    expect(firstVisibleSection.hasAssistant).toBe(true)
  })

  beforeEach(() => {
    managedPanelFixture.enabled = false
    vi.unstubAllGlobals()
    vi.restoreAllMocks()
    vi.stubGlobal('ResizeObserver', class { observe() {} unobserve() {} disconnect() {} })
    removeComposerDrafts(['chat-1', 'chat-2'])
    useAppStore.setState({
      workingDisplayMode: 'verbose',
      collapsibleWorkSections: true,
      expandWorkTraces: true,
      folders: [
        {
          id: 'default',
          name: 'Project',
          parentId: null,
          directory: '/tmp/project',
          isExpanded: true,
          createdAt: new Date('2026-01-01T00:00:00.000Z'),
        },
      ],
      sessions: [
        {
          id: 'chat-1',
          name: 'Gemini Session',
          viewMode: 'chat',
          folderId: 'default',
          workspaceDirectory: '/tmp/project',
          createdAt: new Date('2026-01-01T00:00:00.000Z'),
          updatedAt: new Date('2026-01-01T00:00:00.000Z'),
        },
      ],
      activeSessionId: 'chat-1',
      chatSearchNavigation: null,
      chatHistoryReadBySessionId: {},
      historyStartIndexBySessionId: {},
      goalsByChatId: {},
      activeGoalIdByChatId: {},
      defaultGoalTokenBudgetByChatId: {},
      favoriteUamAgentIds: [],
      providerChatDefaults: {},
      uamAgentCycleShortcut: 'shift+tab',
      uamAgentsBySessionId: {
        'chat-1': [
          { id: 'build', description: 'Build', builtIn: true },
          { id: 'plan', description: 'Plan', builtIn: true },
        ],
      },
      refreshUamAgents: vi.fn(() => Promise.resolve(true)),
      repositoryReviewBySessionId: {},
      messages: {
        'chat-1': [
          {
            id: 'm-1',
            sessionId: 'chat-1',
            role: 'user',
            content: 'Please inspect the workspace',
            createdAt: new Date('2026-01-01T00:00:00.000Z'),
          },
          {
            id: 'm-2',
            sessionId: 'chat-1',
            role: 'assistant',
            content: 'Before tool. After tool.',
            thoughts: 'Persisted thought should not duplicate while turn events are active.',
            createdAt: new Date('2026-01-01T00:00:01.000Z'),
          },
        ],
      },
      chatHistoryErrorBySessionId: {},
      chatHistoryErrorWasRefreshBySessionId: {},
      providers: [
        { id: 'gemini-cli', name: 'Gemini CLI', shortName: 'Gemini', color: '#8ab4ff', description: '', outputMode: 'cli', supportsCli: true, supportsStructured: true, structuredProtocol: 'gemini-acp' },
        { id: 'codex-cli', name: 'Codex CLI', shortName: 'Codex', color: '#22c55e', description: '', outputMode: 'cli', supportsCli: true, supportsStructured: true, structuredProtocol: 'codex-app-server' },
        { id: 'claude-cli', name: 'Claude Code', shortName: 'Claude', color: '#7c3aed', description: '', outputMode: 'cli', supportsCli: true, supportsStructured: true, structuredProtocol: 'claude-code-stream-json' },
      ],
      acpBindingBySessionId: {
        'chat-1': {
          sessionId: 'native-1',
          providerId: 'gemini-cli',
          protocolKind: 'gemini-acp',
          threadId: '',
          running: true,
          lifecycleState: 'waitingPermission',
          processing: true,
          readySinceLastSelect: false,
            processingStartedAtMs: Date.now(),
            lastError: '',
            recentStderr: '',
            lastExitCode: null,
            diagnostics: [],
            toolCalls: [
            {
              id: 'tool-1',
              title: 'Search symbols',
              kind: 'search',
              status: 'in_progress',
              content: 'Searching workspace symbols',
            },
          ],
          planEntries: [],
          availableModes: [
            { id: 'default', name: 'Default', description: 'Run normally' },
            { id: 'plan', name: 'Plan', description: 'Plan before editing' },
          ],
          currentModeId: 'default',
          availableModels: [
            { id: 'auto-gemini-3', name: 'Auto 3', description: 'Gemini 3 routing' },
            { id: 'gemini-3-flash-preview', name: 'Gemini 3 Flash', description: 'Preview model' },
          ],
          currentModelId: 'auto-gemini-3',
          turnEvents: [
            { type: 'assistant_text', text: 'Before **tool**.\n\n```ts\nconst ok = true\n```' },
            { type: 'thought', text: 'Need to inspect the workspace first.' },
            { type: 'tool_call', toolCallId: 'tool-1' },
            { type: 'permission_request', requestId: '5', toolCallId: 'tool-1' },
            { type: 'assistant_text', text: 'After tool.' },
          ],
          turnUserMessageIndex: 0,
          turnAssistantMessageIndex: 1,
          turnSerial: 1,
          pendingPermission: {
            requestId: '5',
            toolCallId: 'tool-1',
            title: 'Read file',
            kind: 'read',
            status: 'pending',
            content: 'Read /tmp/project/file.txt',
            safetyRisk: 'warn',
            safetyTier: 'low',
            safetyRequiresApproval: true,
            options: [{ id: 'allow-once', name: 'Allow once', kind: 'allow_once' }],
          },
          pendingUserInput: null,
          agentInfo: { name: 'gemini', title: 'Gemini CLI', version: '0.36.0' },
        },
      },
      cliBindingBySessionId: {},
      getVcsCommitStatus: vi.fn(() => Promise.resolve(null)),
      branchSwitching: undefined,
    })
  })

  it('keeps the live compaction scene and timer without a fill bar while retaining completed-compaction meters', () => {
    useAppStore.setState((state) => ({
      messages: { 'chat-1': [{
        id: 'previous-compaction', sessionId: 'chat-1', role: 'assistant', content: '',
        createdAt: new Date('2026-01-01T00:00:00.000Z'),
        blocks: [{ type: 'context_compaction', text: 'Keep the workspace context.', tokensBefore: 1000, tokensAfter: 400 }],
      }] },
      acpBindingBySessionId: { 'chat-1': {
        ...state.acpBindingBySessionId['chat-1'], compacting: true, processing: true,
        lifecycleState: 'processing', pendingPermission: null, turnEvents: [],
        turnUserMessageIndex: -1, turnAssistantMessageIndex: -1,
        providerUsage: { contextUsage: { usedTokens: 800, windowTokens: 1000, updatedAt: Date.now() }, tokenUsage: null, rateLimits: null },
      } },
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    try {
      act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
      const strip = host.querySelector('.chat-status-strip[data-tone="info"]')!
      expect(strip.getAttribute('data-state')).toBe('open')
      expect(strip.textContent).toContain('Compacting context')
      expect(strip.querySelector('.chat-status-strip__scene svg')).not.toBeNull()
      expect(strip.querySelector('.chat-status-strip__time')?.getAttribute('aria-label')).toMatch(/^for /)
      expect(strip.querySelector('.conversation-meter')).toBeNull()
      expect(strip.querySelector('.conversation-status__usage')).toBeNull()
      const completed = host.querySelector('details.context-compaction')!
      expect(completed.textContent).toContain('1,000 → 400 tokens')
      expect(completed.querySelector('.conversation-meter')).not.toBeNull()
    } finally {
      act(() => root.unmount())
      host.remove()
    }
  })

  it('retains the observed idle timeout status after native reconciliation', () => {
    useAppStore.setState((state) => ({
      messages: { 'chat-1': [] },
      acpBindingBySessionId: { 'chat-1': { ...state.acpBindingBySessionId['chat-1'], running: false, processing: false, lifecycleState: 'stopped', lastError: '', lastStopReason: 'timeout' } },
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    expect(host.textContent).toContain('Stopped after timeout')
    expect(host.textContent).not.toContain('Response interrupted')
    act(() => root.unmount())
    host.remove()
  })

  it('places the stopped notice as the last divider in the transcript, not in the composer row', () => {
    useAppStore.setState((state) => ({
      messages: { 'chat-1': [] },
      acpBindingBySessionId: { 'chat-1': { ...state.acpBindingBySessionId['chat-1'], running: false, processing: false, lifecycleState: 'stopped', lastError: '', lastStopReason: 'timeout' } },
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    const list = host.querySelector('.uam-message-list') as HTMLElement
    expect(list.lastElementChild?.classList.contains('chat-status-strip')).toBe(true)
    expect(list.lastElementChild?.textContent).toContain('Stopped after timeout')
    const row = host.querySelector('.uam-composer-tabs') as HTMLElement
    expect(row.textContent).not.toContain('Stopped after timeout')
    expect(row.querySelector('.uam-composer-workspace-tab')).toBeTruthy()
    expect(row.nextElementSibling?.classList.contains('uam-composer-surface')).toBe(true)
    act(() => root.unmount())
    host.remove()
  })

  it('requires pointer or keyboard interaction to acknowledge the current completion', () => {
    const original = useAppStore.getState().acknowledgeChatAttention
    const acknowledge = vi.fn(() => Promise.resolve(true))
    useAppStore.setState((state) => ({ acknowledgeChatAttention: acknowledge,
      sessions: state.sessions.map((session) => ({ ...session, attentionRevision: 'completion-one' })) }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    act(() => {
      window.dispatchEvent(new Event('focus'))
      document.dispatchEvent(new Event('visibilitychange'))
    })
    expect(acknowledge).not.toHaveBeenCalled()
    act(() => host.firstElementChild?.dispatchEvent(new Event('pointerdown', { bubbles: true })))
    expect(acknowledge).toHaveBeenLastCalledWith('chat-1', 'completion-one')
    act(() => useAppStore.setState((state) => ({ sessions: state.sessions.map((session) => ({ ...session, attentionRevision: 'completion-two' })) })))
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    act(() => host.firstElementChild?.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowDown', bubbles: true })))
    expect(acknowledge).toHaveBeenLastCalledWith('chat-1', 'completion-two')
    act(() => root.unmount())
    host.remove()
    useAppStore.setState({ acknowledgeChatAttention: original })
  })

  it('shows scoped chat hydration errors with a retry action', () => {
    const retry = vi.fn(() => Promise.resolve(true))
    useAppStore.setState({
      chatHistoryErrorBySessionId: { 'chat-1': 'Remote transcript could not be read.' },
      loadSessionMessages: retry,
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    expect(host.querySelector('[role="alert"]')?.textContent).toContain('Remote transcript could not be read.')
    act(() => (Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Retry') as HTMLButtonElement).click())
    expect(retry).toHaveBeenCalledWith('chat-1', true)

    act(() => root.unmount())
    host.remove()
  })

  it('keeps retry loading isolated when switching chats before completion', async () => {
    const completions = new Map<string, () => void>()
    const retry = vi.fn((chatId: string) => new Promise<boolean>((resolve) => {
      completions.set(chatId, () => resolve(true))
    }))
    const secondSession = { ...useAppStore.getState().sessions[0], id: 'chat-2', name: 'Second Session' }
    useAppStore.setState({
      sessions: [...useAppStore.getState().sessions, secondSession],
      chatHistoryErrorBySessionId: {
        'chat-1': 'First transcript failed.',
        'chat-2': 'Second transcript failed.',
      },
      loadSessionMessages: retry,
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    const retryButton = () => Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Retry') as HTMLButtonElement
    act(() => retryButton().click())
    expect(retry).toHaveBeenCalledWith('chat-1', true)

    act(() => root.render(<ChatView session={secondSession} />))
    act(() => retryButton().click())
    expect(retry).toHaveBeenCalledWith('chat-2', true)
    await act(async () => {
      completions.get('chat-1')?.()
      await Promise.resolve()
    })
    expect(retryButton().getAttribute('aria-busy')).toBe('true')
    await act(async () => {
      completions.get('chat-2')?.()
      await Promise.resolve()
    })
    expect(retryButton().getAttribute('aria-busy')).toBeNull()
    act(() => root.unmount())
    host.remove()
  })

  it('shows the same rejected prompt again after dismissal and retains its draft', async () => {
    useAppStore.setState((state) => ({ acpBindingBySessionId: { 'chat-1': {
      ...state.acpBindingBySessionId['chat-1'], processing: false, lifecycleState: 'ready', pendingPermission: null,
      turnEvents: [], toolCalls: [],
    } } }))
    vi.stubGlobal('cefQuery', ({ request, onSuccess, onFailure }: { request: string; onSuccess: (text: string) => void; onFailure: (code: number, text: string) => void }) => {
      if (JSON.parse(request).action === 'sendAcpPrompt') onFailure(500, 'Native session is busy.')
      else onSuccess('{}')
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    const textarea = host.querySelector('textarea')!
    act(() => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')!.set!.call(textarea, 'Retry this prompt')
      textarea.dispatchEvent(new Event('input', { bubbles: true }))
    })
    const send = async () => act(async () => {
      host.querySelector('form')!.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true }))
      await Promise.resolve()
    })
    await send()
    expect(host.querySelector('[role="alert"]')?.textContent).toContain('Native session is busy.')
    act(() => (host.querySelector('[aria-label="Dismiss composer error"]') as HTMLButtonElement).click())
    expect(host.querySelector('[role="alert"]')).toBeNull()
    await send()
    expect(host.querySelector('[role="alert"]')?.textContent).toContain('Native session is busy.')
    expect(textarea.value).toBe('Retry this prompt')
    act(() => root.unmount())
    host.remove()
  })

  it('shows the owning runtime status across native activity and ACP handoff', () => {
    useAppStore.setState((state) => ({
      acpBindingBySessionId: {
        'chat-1': { ...state.acpBindingBySessionId['chat-1'], running: false, processing: false, lifecycleState: 'stopped', pendingPermission: null },
      },
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    const expectStatus = (label: string, color: string) => {
      const trigger = host.querySelector<HTMLButtonElement>('.uam-provider-model-trigger')!
      expect(trigger.title.endsWith(` · ${label}`)).toBe(true)
      expect((trigger.lastElementChild as HTMLElement).style.background).toBe(color)
    }
    expectStatus('Stopped', 'var(--text-3)')
    for (const processing of [false, true, false]) {
      act(() => useAppStore.setState({
        cliBindingBySessionId: {
          'chat-1': { terminalId: 'term-1', boundChatId: 'chat-1', running: true, lifecycleState: processing ? 'busy' : 'idle', turnState: processing ? 'busy' : 'idle', processing, readySinceLastSelect: false, active: !processing, lastError: '' },
        },
      }))
      expectStatus(processing ? 'Running' : 'Ready', processing ? 'var(--blue)' : 'var(--green)')
    }
    act(() => useAppStore.getState().setCliBinding('chat-1', { lifecycleState: 'unknown', turnState: 'unknown' }))
    expectStatus('Connected', 'var(--text-3)')
    act(() => useAppStore.setState((state) => ({
      cliBindingBySessionId: { 'chat-1': { ...state.cliBindingBySessionId['chat-1'], running: false, lifecycleState: 'stopped' } },
      acpBindingBySessionId: { 'chat-1': { ...state.acpBindingBySessionId['chat-1'], running: true, processing: true, lifecycleState: 'waitingPermission' } },
    })))
    expectStatus('Permission', 'var(--yellow)')
    act(() => root.unmount())
    host.remove()
  })

  it('keeps the selected real agent outside the draft and sends multiline text without the prefix', async () => {
    const sendAcpPrompt = vi.fn(() => Promise.resolve(true))
    const setSessionUamAgent = vi.fn((id: string, agentId: string) => {
      useAppStore.setState((state) => ({ sessions: state.sessions.map((session) => session.id === id ? { ...session, uamAgentId: agentId } : session) }))
      return Promise.resolve(true)
    })
    useAppStore.setState((state) => ({
      sendAcpPrompt,
      setSessionUamAgent,
      uamAgentsBySessionId: { 'chat-1': [...state.uamAgentsBySessionId['chat-1'], { id: 'custom-accessibility-reviewer', description: 'Real workspace agent', builtIn: false }] },
      acpBindingBySessionId: { 'chat-1': { ...state.acpBindingBySessionId['chat-1'], processing: false, lifecycleState: 'ready', pendingPermission: null } },
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    const render = () => root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    act(render)
    const textarea = host.querySelector('textarea')!
    const prefix = () => host.querySelector<HTMLButtonElement>('.uam-composer-agent button')!
    expect(prefix().textContent).toBe('Build')
    expect(host.querySelector('.uam-composer-agent')?.textContent).toBe('Build')
    expect(prefix().closest('.uam-composer-input-row')).toBe(textarea.parentElement)
    const agent = host.querySelector<HTMLElement>('.uam-composer-agent')!
    for (const scrollTop of [12, 60, 0]) {
      textarea.scrollTop = scrollTop
      textarea.dispatchEvent(new Event('scroll'))
      expect(agent.style.transform).toBe(`translateY(${-scrollTop}px)`)
    }

    expect(host.querySelector('.uam-composer-agent')?.nextElementSibling).toBe(textarea)
    expect(host.querySelector('.uam-composer-agent-separator')).toBeTruthy()
    expect(host.querySelector('button[aria-label="Options"]')).toBeTruthy()
    expect(host.querySelector('button[aria-label="Workspace actions"]')).toBeTruthy()
    act(() => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')!.set!.call(textarea, 'Inspect this file\nthen report findings')
      textarea.dispatchEvent(new Event('input', { bubbles: true }))
    })
    act(() => prefix().click())
    expect(setSessionUamAgent).not.toHaveBeenCalled()
    const agentChoice = Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="option"]')).find((option) => option.textContent?.includes('Custom Accessibility Reviewer'))!
    await act(async () => { agentChoice.click(); await Promise.resolve() })
    act(render)
    expect(setSessionUamAgent).toHaveBeenCalledWith('chat-1', 'custom-accessibility-reviewer')
    expect(prefix().textContent).toBe('Custom Accessibility Reviewer')
    expect(prefix().title).toBe('UAM agent: Custom Accessibility Reviewer')
    expect(textarea.value).toBe('Inspect this file\nthen report findings')
    const shiftEnter = new KeyboardEvent('keydown', { key: 'Enter', shiftKey: true, bubbles: true, cancelable: true })
    act(() => textarea.dispatchEvent(shiftEnter))
    expect(shiftEnter.defaultPrevented).toBe(false)
    expect(sendAcpPrompt).not.toHaveBeenCalled()
    await act(async () => { textarea.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true })); await Promise.resolve() })
    expect(sendAcpPrompt).toHaveBeenCalledWith('chat-1', 'Inspect this file\nthen report findings', [], undefined, expect.any(Function))
    act(() => root.unmount())
    host.remove()
  })

  it('opens short permission, memory and reasoning chips without toggling and respects a new runtime lock', () => {
    const setSessionCodexOptions = vi.fn(() => Promise.resolve(true))
    const setSessionMemoryLevel = vi.fn(() => Promise.resolve(true))
    const setSessionCommandSafetyTier = vi.fn(() => Promise.resolve({ ok: true }))
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) => ({ ...session, providerId: 'codex-cli', modelId: 'gpt-5.4', reasoningEffort: 'high', memoryLevel: 'balanced' as const, commandSafetyTier: 'off' as const })),
      acpBindingBySessionId: { 'chat-1': { ...state.acpBindingBySessionId['chat-1'], providerId: 'codex-cli', protocolKind: 'codex-app-server', currentModelId: 'gpt-5.4', availableModels: [{ id: 'gpt-5.4', name: 'GPT-5.4', description: '', supportedReasoningEfforts: ['low', 'high'] }], processing: false, lifecycleState: 'ready', pendingPermission: null } },
      setSessionCodexOptions, setSessionMemoryLevel, setSessionCommandSafetyTier,
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    for (const [label, value] of [['Permissions', 'Default'], ['Reasoning', 'High']]) {
      const button = host.querySelector<HTMLButtonElement>(`.uam-composer-status-chips [aria-label="${label}"]`)!
      expect(button.textContent).toContain(value)
      act(() => button.click())
      const menu = document.body.querySelector<HTMLElement>(`[role="listbox"][aria-label="${label}"]`)!
      expect(menu).toBeTruthy()
      expect(setSessionCodexOptions).not.toHaveBeenCalled()
      expect(setSessionMemoryLevel).not.toHaveBeenCalled()
      expect(setSessionCommandSafetyTier).not.toHaveBeenCalled()
      act(() => menu.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })))
    }
    const memoryToggle = host.querySelector<HTMLButtonElement>('.uam-composer-status-chips button[aria-pressed]')!
    expect(memoryToggle.textContent).toContain('Memory On · Balanced')
    act(() => memoryToggle.click())
    expect(setSessionMemoryLevel).toHaveBeenCalledWith('chat-1', 'off')
    expect(setSessionCodexOptions).not.toHaveBeenCalled()
    act(() => host.querySelector<HTMLButtonElement>('.uam-composer-status-chips [aria-label="Reasoning"]')!.click())
    act(() => useAppStore.setState((state) => ({ acpBindingBySessionId: { 'chat-1': { ...state.acpBindingBySessionId['chat-1'], processing: true } } })))
    const low = Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="listbox"][aria-label="Reasoning"] [role="option"]')).find((option) => option.textContent?.startsWith('Low'))!
    act(() => low.click())
    expect(setSessionCodexOptions).not.toHaveBeenCalled()
    expect(host.querySelector<HTMLButtonElement>('.uam-composer-status-chips [aria-label="Reasoning"]')?.disabled).toBe(true)
    act(() => root.unmount())
    host.remove()
  })

  it('attributes historical messages from recorded provider, model and time', () => {
    const historical = { ...useAppStore.getState().messages['chat-1'][1], providerId: 'codex-cli', modelId: 'historical-model' }
    const time = historical.createdAt.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })
    expect(reviewAssistantLabel('Codex', historical)).toBe(`Codex · historical-model · ${time}`)
    expect(reviewAssistantLabel('Codex', { ...historical, modelId: '', createdAt: new Date(NaN) })).toBe('Codex')
    const prompt = useAppStore.getState().messages['chat-1'][0]
    useAppStore.setState({ messages: { 'chat-1': [prompt, historical] }, acpBindingBySessionId: {} })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    expect(host.querySelector('article.conversation-turn--assistant')?.getAttribute('aria-label')).toBe(`Codex · historical-model · ${time}`)
    expect(host.querySelector('time.conversation-turn-start')?.textContent).toBe('Codex · historical-model')
    expect(host.querySelector('article.conversation-turn--assistant footer')?.textContent ?? '').not.toContain('historical-model')
    expect(host.querySelector('article.conversation-turn--assistant button[aria-label="Copy message"]')).toBeNull()
    act(() => root.unmount())
    host.remove()
  })

  it('builds the agent shortcut cycle from ordered selectable favorites only', () => {
    const agents = [
      { id: 'build', description: '', builtIn: true },
      { id: 'plan', description: '', builtIn: true },
      { id: 'reviewer', description: '', builtIn: false },
      { id: 'writer', description: '', builtIn: false },
    ]
    expect(buildUamAgentCycle(agents, ['writer', 'reviewer', 'writer', 'missing', 'plan'])).toEqual([
      'build', 'plan', 'writer', 'reviewer',
    ])
    expect(matchesUamAgentCycleShortcut('shift+tab', { key: 'Tab', shiftKey: true, ctrlKey: false, altKey: false, metaKey: false })).toBe(true)
    expect(matchesUamAgentCycleShortcut('shift+tab', { key: 'Tab', shiftKey: true, ctrlKey: true, altKey: false, metaKey: false })).toBe(false)
    expect(matchesUamAgentCycleShortcut('control+shift+tab', { key: 'Tab', shiftKey: true, ctrlKey: true, altKey: false, metaKey: false })).toBe(true)
    expect(matchesUamAgentCycleShortcut('disabled', { key: 'Tab', shiftKey: true, ctrlKey: false, altKey: false, metaKey: false })).toBe(false)
  })

  it('cycles a favorite only from the composer textarea and ignores repeat and composition', () => {
    const setSessionUamAgent = vi.fn(() => Promise.resolve(true))
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) => ({ ...session, uamAgentId: 'plan' })),
      favoriteUamAgentIds: ['reviewer'],
      uamAgentsBySessionId: {
        'chat-1': [
          { id: 'build', description: 'Build', builtIn: true },
          { id: 'plan', description: 'Plan', builtIn: true },
          { id: 'reviewer', description: 'Review', builtIn: false },
        ],
      },
      setSessionUamAgent,
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    const textarea = host.querySelector('textarea') as HTMLTextAreaElement

    act(() => textarea.dispatchEvent(new KeyboardEvent('keydown', { key: 'Tab', shiftKey: true, repeat: true, bubbles: true })))
    act(() => textarea.dispatchEvent(new KeyboardEvent('keydown', { key: 'Tab', shiftKey: true, isComposing: true, bubbles: true })))
    act(() => textarea.dispatchEvent(new KeyboardEvent('keydown', { key: 'Tab', shiftKey: true, ctrlKey: true, bubbles: true })))
    expect(setSessionUamAgent).not.toHaveBeenCalled()

    const exactShortcut = new KeyboardEvent('keydown', { key: 'Tab', shiftKey: true, bubbles: true, cancelable: true })
    act(() => textarea.dispatchEvent(exactShortcut))
    expect(exactShortcut.defaultPrevented).toBe(true)
    expect(setSessionUamAgent).toHaveBeenCalledWith('chat-1', 'reviewer')

    act(() => root.unmount())
    host.remove()
  })

  it('does not submit Enter while an IME composition is active', () => {
    const sendAcpPrompt = vi.fn(() => Promise.resolve(true))
    useAppStore.setState({ sendAcpPrompt })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    const textarea = host.querySelector('textarea') as HTMLTextAreaElement
    Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set?.call(textarea, '未確定')
    act(() => textarea.dispatchEvent(new Event('input', { bubbles: true })))

    const composingEnter = new KeyboardEvent('keydown', {
      key: 'Enter',
      isComposing: true,
      bubbles: true,
      cancelable: true,
    })
    act(() => textarea.dispatchEvent(composingEnter))

    expect(composingEnter.defaultPrevented).toBe(false)
    expect(sendAcpPrompt).not.toHaveBeenCalled()
    act(() => root.unmount())
    host.remove()
  })

  it('confirms provider handoff before changing anything', async () => {
    const originalSetSessionModel = useAppStore.getState().setSessionModel
    const setSessionModel = vi.fn().mockResolvedValue(true)
    const originalSetSessionProvider = useAppStore.getState().setSessionProvider
    const setSessionProvider = vi.fn(() => Promise.resolve(true))
    useAppStore.setState((state) => ({
      setSessionProvider,
      setSessionModel,
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          lifecycleState: 'ready',
          processing: false,
          pendingPermission: null,
          pendingUserInput: null,
          queuedPrompts: [],
        },
      },
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    const openProviderMenu = () => act(() => {
      (host.querySelector('button[aria-label="Select provider and model"]') as HTMLButtonElement).click()
    })
    const providerButton = (label: string) => document.body.querySelector<HTMLButtonElement>(`[data-model-picker-sidebar] button[aria-label="${label}"]`)
    const selectProviderModel = () => act(() => document.body.querySelector<HTMLButtonElement>('[data-testid="model-options"] [role="menuitemradio"]')!.click())

    openProviderMenu()
    act(() => providerButton('Gemini')?.click())
    expect(document.body.querySelector('[role="alertdialog"]')).toBeNull()
    expect(setSessionProvider).not.toHaveBeenCalled()

    act(() => providerButton('Codex')!.click())
    expect(setSessionProvider).not.toHaveBeenCalled()
    selectProviderModel()
    let dialog = document.body.querySelector<HTMLElement>('[role="alertdialog"][aria-label="Switch from Gemini to Codex"]')
    expect(dialog?.textContent).toContain('Chat messages and recorded tool results')
    expect(dialog?.textContent).toContain('Selected UAM agent and goals')
    expect(dialog?.textContent).toContain('Native session and provider-specific live tool state')
    expect(dialog?.textContent).not.toContain('agent defaults')
    expect(dialog?.textContent).toContain('Anything the current provider did not record in UAM will not transfer.')
    expect(setSessionProvider).not.toHaveBeenCalled()

    act(() => (Array.from(dialog?.querySelectorAll('button') ?? []).find((button) => button.textContent === 'Cancel') as HTMLButtonElement).click())
    expect(document.body.querySelector('[role="alertdialog"]')).toBeNull()
    expect(setSessionProvider).not.toHaveBeenCalled()

    openProviderMenu()
    act(() => providerButton('Codex')!.click())
    selectProviderModel()
    dialog = document.body.querySelector<HTMLElement>('[role="alertdialog"][aria-label="Switch from Gemini to Codex"]')
    await act(async () => {
      (Array.from(dialog?.querySelectorAll('button') ?? []).find((button) => button.textContent === 'Switch to Codex') as HTMLButtonElement).click()
      await Promise.resolve()
    })
    expect(setSessionProvider).toHaveBeenCalledOnce()
    expect(setSessionProvider).toHaveBeenCalledWith('chat-1', 'codex-cli')
    expect(setSessionModel).toHaveBeenCalledWith('chat-1', '')
    expect(document.body.querySelector('[role="alertdialog"]')).toBeNull()

    act(() => root.unmount())
    host.remove()
    useAppStore.setState({ setSessionProvider: originalSetSessionProvider, setSessionModel: originalSetSessionModel })
  })

  it('blocks handoff to a provider that cannot start structured chats', async () => {
    const originalSetSessionProvider = useAppStore.getState().setSessionProvider
    const originalCliVersionManager = useAppStore.getState().cliVersionManager
    const setSessionProvider = vi.fn(() => Promise.resolve(true))
    useAppStore.setState((state) => ({
      setSessionProvider,
      cliVersionManager: { providers: [{ providerId: 'codex-cli', installedVersion: '', selectedVersion: '', availableVersions: [], preferredVersion: '', status: 'unavailable', message: 'Codex CLI is unavailable.', running: false, lastCommand: '', lastOutput: '' }] },
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': { ...state.acpBindingBySessionId['chat-1'], lifecycleState: 'ready', processing: false, pendingPermission: null, pendingUserInput: null, queuedPrompts: [] },
      },
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    act(() => (host.querySelector('button[aria-label="Select provider and model"]') as HTMLButtonElement).click())
    act(() => document.body.querySelector<HTMLButtonElement>('[data-model-picker-sidebar] button[aria-label="Codex"]')!.click())
    act(() => document.body.querySelector<HTMLButtonElement>('[data-testid="model-options"] [role="menuitemradio"]')!.click())
    const dialog = document.body.querySelector<HTMLElement>('[role="alertdialog"]')
    const confirm = Array.from(dialog?.querySelectorAll<HTMLButtonElement>('button') ?? []).find((button) => button.textContent === 'Switch to Codex')

    expect(dialog?.textContent).toContain('Codex CLI is unavailable.')
    expect(confirm?.disabled).toBe(true)
    expect(setSessionProvider).not.toHaveBeenCalled()

    act(() => root.unmount())
    host.remove()
    useAppStore.setState({ setSessionProvider: originalSetSessionProvider, cliVersionManager: originalCliVersionManager })
  })

  it('restores a separate unsent draft for each chat across navigation and remount', () => {
    const values = new Map<string, string>()
    vi.stubGlobal('localStorage', {
      getItem: (key: string) => values.get(key) ?? null,
      setItem: (key: string, value: string) => values.set(key, value),
      removeItem: (key: string) => values.delete(key),
      clear: () => values.clear(),
    })
    useAppStore.setState((state) => ({
      sessions: [state.sessions[0], { ...state.sessions[0], id: 'chat-2', name: 'Second chat' }],
      messages: { ...state.messages, 'chat-2': [] },
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-2': { ...state.acpBindingBySessionId['chat-1'], sessionId: 'native-2', processing: false, pendingPermission: null, turnEvents: [] },
      },
    }))
    const [first, second] = useAppStore.getState().sessions
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    const typeDraft = (value: string) => {
      const textarea = host.querySelector('textarea') as HTMLTextAreaElement
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set?.call(textarea, value)
      textarea.dispatchEvent(new Event('input', { bubbles: true }))
    }

    act(() => root.render(<ChatView key={first.id} session={first} />))
    act(() => typeDraft('First unsent draft'))
    act(() => root.render(<ChatView key={second.id} session={second} />))
    act(() => typeDraft('Second unsent draft'))
    act(() => root.render(<ChatView key={first.id} session={first} />))
    expect((host.querySelector('textarea') as HTMLTextAreaElement).value).toBe('First unsent draft')

    act(() => root.unmount())
    const remounted = createRoot(host)
    act(() => remounted.render(<ChatView key={second.id} session={second} />))
    expect((host.querySelector('textarea') as HTMLTextAreaElement).value).toBe('Second unsent draft')

    act(() => remounted.unmount())
    host.remove()
  })

  it('debounces composer draft storage while typing', () => {
    vi.useFakeTimers()
    const setItem = vi.fn()
    vi.stubGlobal('localStorage', {
      getItem: () => null,
      setItem,
      removeItem: vi.fn(),
      clear: vi.fn(),
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    const textarea = host.querySelector('textarea') as HTMLTextAreaElement

    for (const value of ['a', 'ab', 'abc']) {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set?.call(textarea, value)
      act(() => textarea.dispatchEvent(new Event('input', { bubbles: true })))
    }

    expect(setItem).not.toHaveBeenCalled()
    act(() => window.dispatchEvent(new Event('pagehide')))
    expect(setItem).toHaveBeenCalledTimes(1)
    setItem.mockClear()
    act(() => vi.advanceTimersByTime(250))
    expect(setItem).toHaveBeenCalledTimes(1)

    act(() => root.unmount())
    host.remove()
    vi.useRealTimers()
  })

  it('does not rewrite a composer draft after its chat is deleted', async () => {
    const values = new Map<string, string>()
    vi.stubGlobal('localStorage', {
      getItem: (key: string) => values.get(key) ?? null,
      setItem: (key: string, value: string) => values.set(key, value),
      removeItem: (key: string) => values.delete(key),
      clear: () => values.clear(),
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    const textarea = host.querySelector('textarea') as HTMLTextAreaElement
    Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set?.call(textarea, 'discard me')
    act(() => textarea.dispatchEvent(new Event('input', { bubbles: true })))

    await act(async () => { await useAppStore.getState().deleteSession('chat-1') })
    act(() => root.unmount())

    expect(readChatComposerDraft('chat-1')).toEqual({ text: '', attachments: [] })
    host.remove()
  })

  it.each(['local', 'ssh', 'companion'])('loads ordinary chat baseline review through the existing dialog on %s', async (surface) => {
    const previousPath = window.location.pathname + window.location.search + window.location.hash
    window.history.replaceState(null, '', surface === 'companion' ? '/companion' : '/')
    const status = {
      available: true, vcsTypes: ['git' as const], activeVcsType: 'git' as const,
      workspaceDirectory: '/tmp/project', branchOrRevision: 'main',
      changedFiles: [{ path: 'src/chat.ts', status: 'M', staged: false, additions: 1, deletions: 0, binary: false }],
      lineStatsReady: true, warning: '', error: '',
    }
    const getVcsCommitStatus = vi.fn(() => Promise.resolve(status))
    const getVcsFileDiff = vi.fn(() => Promise.resolve('diff --git a/src/chat.ts b/src/chat.ts\n+chat'))
    useAppStore.setState((state) => ({
      getVcsCommitStatus, getVcsFileDiff, discoverProviderModels: vi.fn(() => Promise.resolve()),
      sessions: state.sessions.map((session) => ({ ...session, executionHostId: surface === 'ssh' ? 'remote' : 'local', workspaceDirectory: '/tmp/project', workspaceIsolationKind: undefined, workspaceBaseRef: undefined })),
      acpBindingBySessionId: { ...state.acpBindingBySessionId, 'chat-1': { ...state.acpBindingBySessionId['chat-1'], processing: false, lifecycleState: 'ready', pendingPermission: null, pendingUserInput: null, processingStartedAtMs: null, turnSerial: 2 } },
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    try {
    await act(async () => { root.render(<ChatView session={useAppStore.getState().sessions[0]} />); await Promise.resolve() })
    expect(getVcsCommitStatus).toHaveBeenCalledWith('chat-1', 'git', expect.objectContaining({ comparisonRef: 'chat' }))
    await act(async () => { (host.querySelector('[aria-label="Review changes to src/chat.ts"]') as HTMLButtonElement).click(); await Promise.resolve() })
    expect(getVcsFileDiff).toHaveBeenCalledWith('chat-1', 'src/chat.ts', 'git', 'chat')
    if (surface === 'companion') expect((host.querySelector('[aria-label="Open commit panel"]') as HTMLElement).style.display).toBe('none')
    } finally {
      act(() => root.unmount())
      host.remove()
      window.history.replaceState(null, '', previousPath)
    }
  })

  it('clears stale review counts when the original chat baseline is unavailable', async () => {
    const status = { available: true, vcsTypes: ['git' as const], activeVcsType: 'git' as const, workspaceDirectory: '/tmp/project', branchOrRevision: 'main', changedFiles: [{ path: 'prior.ts', status: 'M', staged: false, additions: 9, deletions: 0, binary: false }], lineStatsReady: true, warning: '', error: 'The original chat baseline is unavailable.' }
    useAppStore.setState((state) => ({
      repositoryReviewBySessionId: { 'chat-1': { ...status, error: '' } },
      getVcsCommitStatus: vi.fn(() => Promise.resolve(status)),
      sessions: state.sessions.map((session) => ({ ...session, workspaceDirectory: '/tmp/project', workspaceIsolationKind: undefined })),
      acpBindingBySessionId: { ...state.acpBindingBySessionId, 'chat-1': { ...state.acpBindingBySessionId['chat-1'], processing: false, lifecycleState: 'ready', pendingPermission: null, pendingUserInput: null, processingStartedAtMs: null, turnSerial: 2 } },
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    try {
    await act(async () => { root.render(<ChatView session={useAppStore.getState().sessions[0]} />); await Promise.resolve() })
    expect(host.querySelector('[aria-label="Review changes"]')).toBeNull()
    expect(useAppStore.getState().repositoryReviewBySessionId['chat-1']).toBeUndefined()
    } finally {
      act(() => root.unmount())
      host.remove()
    }
  })

  it('recomputes exact isolated-chat changes after renderer state is lost and reports diff failures', async () => {
    const comparisonRef = 'b'.repeat(40)
    const rangeStatus = {
      available: true,
      vcsTypes: ['git' as const],
      activeVcsType: 'git' as const,
      workspaceDirectory: '/tmp/project/.uam-worktrees/chat-1',
      branchOrRevision: 'uam/chat-1',
      changedFiles: [
        { path: 'src/exact.ts', status: 'M', staged: false, additions: 2, deletions: 1, binary: false, contentFingerprint: 'exact' },
      ],
      lineStatsReady: true,
      warning: '',
      error: '',
    }
    const getVcsCommitStatus = vi.fn(() => Promise.resolve(rangeStatus))
    const getVcsFileDiff = vi.fn<ReturnType<typeof useAppStore.getState>["getVcsFileDiff"]>(() => Promise.reject(new Error('The saved comparison commit is unavailable.')))
    const setCommitPanelOpen = vi.fn()
    useAppStore.setState((state) => ({
      repositoryReviewBySessionId: {},
      getVcsCommitStatus,
      getVcsFileDiff,
      setCommitPanelOpen,
      sessions: state.sessions.map((session) => ({
        ...session,
        workspaceDirectory: '/tmp/project/.uam-worktrees/chat-1',
        workspaceSourceDirectory: '/tmp/project',
        workspaceIsolationKind: 'gitWorktree',
        workspaceBaseRef: comparisonRef,
      })),
      messages: {
        ...state.messages,
        'chat-1': state.messages['chat-1'].map((message) => message.role === 'assistant'
          ? { ...message, checkpointSha: 'a'.repeat(40), checkpointParentSha: comparisonRef }
          : message),
      },
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          lifecycleState: 'ready',
          processing: false,
          processingStartedAtMs: null,
          pendingPermission: null,
          pendingUserInput: null,
          turnSerial: 2,
        },
      },
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    await act(async () => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
      await Promise.resolve()
      await Promise.resolve()
    })

    expect(getVcsCommitStatus).toHaveBeenCalledWith('chat-1', 'git', expect.objectContaining({
      comparisonRef,
      requestId: expect.stringContaining('chat-summary:chat-1:'),
    }))
    // A cached completed-turn review must disappear for startup, parent work and live child work.
    for (const patch of [{ processing: true }, { lifecycleState: 'starting' }, { backgroundTaskCount: 1 }]) {
      await act(async () => useAppStore.setState(state => ({ acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': { ...state.acpBindingBySessionId['chat-1'], ...patch },
      } })))
      expect(host.querySelector('[aria-label="Review changes"]')).toBeNull()
    }
    await act(async () => useAppStore.setState(state => ({ acpBindingBySessionId: {
      ...state.acpBindingBySessionId,
      'chat-1': { ...state.acpBindingBySessionId['chat-1'], processing: false, lifecycleState: 'ready', backgroundTaskCount: 0 },
    } })))
    const loadedMessages = useAppStore.getState().messages['chat-1']
    await act(async () => useAppStore.setState(state => ({
      messages: Object.fromEntries(Object.entries(state.messages).filter(([id]) => id !== 'chat-1')),
      sessions: state.sessions.map(session => session.id === 'chat-1' ? { ...session, messageCount: 204 } : session),
    })))
    await act(async () => root.render(<ChatView session={useAppStore.getState().sessions.find(session => session.id === 'chat-1')!} />))
    // No read was started by this fixture: missing rows must not pretend to be work.
    expect(host.textContent).toContain('Chat history unavailable')
    expect(host.querySelector('.conversation-skeleton')).toBeNull()
    expect(host.querySelector('[aria-label="Review changes"]')).toBeNull()
    await act(async () => useAppStore.setState(state => ({ messages: { ...state.messages, 'chat-1': loadedMessages } })))
    const providerGoal = { id: 'provider-review-goal', chatId: 'chat-1', objective: 'Finish implementation', status: 'active' as const, executionOwner: 'provider' as const, createdAt: new Date(), updatedAt: new Date() }
    await act(async () => useAppStore.setState({ goalsByChatId: { 'chat-1': [providerGoal] }, activeGoalIdByChatId: { 'chat-1': providerGoal.id } }))
    expect(host.querySelector('[aria-label="Review changes"]')).toBeNull()
    await act(async () => useAppStore.setState({ goalsByChatId: { 'chat-1': [{ ...providerGoal, status: 'complete' }] } }))
    const summary = host.querySelector('[aria-label="Review changes"]') as HTMLElement
    expect(summary.textContent).toContain('Chat changes')
    expect(summary.querySelector('[aria-label="Review changes to src/exact.ts"]')?.textContent).toContain('exact.ts')
    expect(summary.textContent).toContain('isolated Git checkpoint')
    expect(host.querySelector('[aria-label="Turn checkpoint"]')).toBeNull()

    act(() => {
      ;(summary.querySelector('button[aria-label="Open commit panel"]') as HTMLButtonElement).click()
    })
    expect(setCommitPanelOpen).toHaveBeenCalledWith(true)

    await act(async () => {
      ;(summary.querySelector('[aria-label="Review changes to src/exact.ts"]') as HTMLButtonElement).click()
      await Promise.resolve()
      await Promise.resolve()
    })
    expect(getVcsFileDiff).toHaveBeenCalledWith('chat-1', 'src/exact.ts', 'git', comparisonRef)
    const dialog = document.body.querySelector('[aria-label="Changes in src/exact.ts"]') as HTMLElement
    expect(dialog.querySelector('[role="alert"]')?.textContent).toContain('saved comparison commit is unavailable')
    expect(dialog.querySelector('.uam-diff-line')).toBeNull()

    // Work resuming also retires an already-open diff, including a late response.
    for (const work of ['child', 'parent', 'continuation'] as const) {
      await act(async () => useAppStore.setState(state => ({
        acpBindingBySessionId: { ...state.acpBindingBySessionId, 'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          processing: work === 'parent', backgroundTaskCount: work === 'child' ? 1 : 0,
        } },
        goalsByChatId: { 'chat-1': [{ ...providerGoal, status: work === 'continuation' ? 'active' : 'complete' }] },
      })))
      expect(document.body.querySelector('[aria-label="Changes in src/exact.ts"]')).toBeNull()
      expect(host.querySelector('[aria-label="Review changes"]')).toBeNull()
      await act(async () => useAppStore.setState(state => ({
        acpBindingBySessionId: { ...state.acpBindingBySessionId, 'chat-1': {
          ...state.acpBindingBySessionId['chat-1'], processing: false, backgroundTaskCount: 0,
        } },
        goalsByChatId: { 'chat-1': [{ ...providerGoal, status: 'complete' }] },
      })))
      expect(document.body.querySelector('[aria-label="Changes in src/exact.ts"]')).toBeNull()
      let finishDiff!: (diff: string) => void
      getVcsFileDiff.mockImplementationOnce(() => new Promise((resolve) => { finishDiff = resolve }))
      await act(async () => (host.querySelector('[aria-label="Review changes to src/exact.ts"]') as HTMLButtonElement).click())
      expect(document.body.querySelector('[aria-label="Changes in src/exact.ts"]')).not.toBeNull()
      await act(async () => useAppStore.setState(state => ({ acpBindingBySessionId: {
        ...state.acpBindingBySessionId, 'chat-1': { ...state.acpBindingBySessionId['chat-1'], backgroundTaskCount: 1 },
      } })))
      await act(async () => finishDiff("+Late stale diff"))
      expect(document.body.querySelector('[aria-label="Changes in src/exact.ts"]')).toBeNull()
    }
    await act(async () => useAppStore.setState(state => ({ acpBindingBySessionId: {
      ...state.acpBindingBySessionId, 'chat-1': { ...state.acpBindingBySessionId['chat-1'], backgroundTaskCount: 0 },
    } })))

    getVcsCommitStatus.mockResolvedValueOnce({ ...rangeStatus, available: false, changedFiles: [] })
    await act(async () => useAppStore.setState(state => ({ acpBindingBySessionId: {
      ...state.acpBindingBySessionId,
      'chat-1': { ...state.acpBindingBySessionId['chat-1'], turnSerial: 3 },
    } })))
    expect(useAppStore.getState().repositoryReviewBySessionId['chat-1']).toBeUndefined()
    expect(host.querySelector('[aria-label="Review changes to src/exact.ts"]')).toBeNull()

    act(() => root.unmount())
    host.remove()
  })

  it('shows turn identity only on change, puts the full time in screen-reader text, and quiets brief turns', () => {
    const day = new Date(2026, 4, 10, 9, 0)
    const nextDay = new Date(2026, 4, 11, 9, 0)
    const turn = (id: string, createdAt: Date, answer: Partial<Message>) => [
      { id: `${id}-user`, sessionId: 'chat-1', role: 'user' as const, content: `Ask ${id}`, createdAt },
      { id: `${id}-answer`, sessionId: 'chat-1', role: 'assistant' as const, content: `Answer ${id}`, providerId: 'claude-cli', modelId: 'claude-opus-5-5', createdAt, ...answer },
    ]
    useAppStore.setState((state) => ({
      messages: { ...state.messages, 'chat-1': [
        ...turn('brief', day, { processingTimeMs: 1000 }),
        ...turn('traced', day, { processingTimeMs: 1000, thoughts: 'Checked the files' }),
        ...turn('other-model', nextDay, { modelId: 'claude-sonnet-5', processingTimeMs: 4000 }),
      ] },
      acpBindingBySessionId: {},
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    const times = Array.from(host.querySelectorAll<HTMLTimeElement>('time.conversation-turn-start'))
    expect(times.map((time) => time.textContent)).toEqual([
      'Claude · claude-opus-5-5',
      day.toLocaleString([], { dateStyle: 'medium', timeStyle: 'short' }),
      'Claude · claude-sonnet-5',
    ])
    expect(times.map((time) => time.classList.contains('sr-only'))).toEqual([false, true, false])
    expect(host.querySelectorAll('.conversation-day-divider')).toHaveLength(2)
    const summaries = Array.from(host.querySelectorAll('[data-testid="working-summary"]'))
    expect(summaries.map((summary) => summary.textContent)).toEqual(['Worked 1s', 'Worked 4s'])
    act(() => root.unmount())
    host.remove()
  })

  it('keeps settled turn dates cached while typing and streaming, but refreshes changed metadata', () => {
    const createdAt = new Date('2026-01-01T00:00:00Z')
    const messages: Message[] = Array.from({ length: 200 }, (_, index) => ({
      id: `date-${index}`, sessionId: 'chat-1', role: index % 2 ? 'assistant' : 'user',
      content: `Message ${index}`, createdAt, providerId: 'gemini-cli', modelId: 'original-model',
    }))
    useAppStore.setState((state) => ({ messages: { 'chat-1': messages }, acpBindingBySessionId: {
      'chat-1': { ...state.acpBindingBySessionId['chat-1'], processing: false, turnEvents: [], toolCalls: [], pendingPermission: null },
    } }))
    const format = vi.spyOn(Date.prototype, 'toLocaleString')
    const host = document.createElement('div'); document.body.appendChild(host); const root = createRoot(host)
    try {
      act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
      const initialFormats = format.mock.calls.length
      expect(initialFormats).toBeGreaterThanOrEqual(100)
      const textarea = host.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')!
      for (const value of ['a', 'ab', 'abc']) act(() => {
        Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')!.set!.call(textarea, value)
        textarea.dispatchEvent(new Event('input', { bubbles: true }))
      })
      expect(format).toHaveBeenCalledTimes(initialFormats)
      act(() => useAppStore.setState((state) => ({ messages: { 'chat-1': messages.map((message, index) => index === 199 ? { ...message, content: 'Streamed replacement', isStreaming: true } : message) }, acpBindingBySessionId: { 'chat-1': { ...state.acpBindingBySessionId['chat-1'], processing: true, turnUserMessageIndex: 198, turnAssistantMessageIndex: 199, turnEvents: [{ type: 'assistant_text', text: 'Streamed replacement' }] } } })))
      expect(host.textContent).toContain('Streamed replacement')
      expect(format).toHaveBeenCalledTimes(initialFormats)
      const replacementDate = new Date('2026-02-02T03:04:00Z')
      act(() => useAppStore.setState((state) => ({ messages: { 'chat-1': state.messages['chat-1'].map((message, index) => index === 0 ? { ...message, createdAt: replacementDate, providerId: 'codex-cli', modelId: 'replacement-model' } : message) } })))
      const first = host.querySelector<HTMLTimeElement>('time.conversation-turn-start')!
      expect(first.dateTime).toBe(replacementDate.toISOString())
      expect(first.textContent).toContain('Codex · replacement-model')
      expect(format).toHaveBeenCalledTimes(initialFormats + 1)
      act(() => useAppStore.setState((state) => ({ messages: { 'chat-1': state.messages['chat-1'].map((message, index) => index === 0 ? { ...message, providerId: 'claude-cli' } : message) } })))
      expect(first.textContent).toContain('Claude · replacement-model')
      act(() => useAppStore.setState((state) => ({ messages: { 'chat-1': state.messages['chat-1'].map((message, index) => index === 0 ? { ...message, modelId: 'model-only-change' } : message) } })))
      expect(first.textContent).toContain('Claude · model-only-change')
      expect(format).toHaveBeenCalledTimes(initialFormats + 1)
    } finally { act(() => root.unmount()); host.remove() }
  })

  it('reveals a collapsed historical search target and anchors each owned generation once without changing its draft', () => {
    const draft = 'Keep my unfinished prompt'
    writeChatComposerDraft('chat-1', { text: draft, attachments: [] })
    const messages: Message[] = [
      { id: 'search-user', sessionId: 'chat-1', role: 'user', content: 'Old question', createdAt: new Date(1) },
      { id: 'search-middle', sessionId: 'chat-1', role: 'assistant', content: 'Hidden historical match', createdAt: new Date(2) },
      { id: 'search-final', sessionId: 'chat-1', role: 'assistant', content: 'Final answer', createdAt: new Date(3) },
    ]
    useAppStore.setState((state) => ({
      expandWorkTraces: false, messages: { 'chat-1': messages }, historyStartIndexBySessionId: { 'chat-1': 100 },
      acpBindingBySessionId: { 'chat-1': { ...state.acpBindingBySessionId['chat-1'], processing: false, turnEvents: [], toolCalls: [], pendingPermission: null } },
    }))
    const descriptor = Object.getOwnPropertyDescriptor(HTMLElement.prototype, 'scrollIntoView')
    const scroll = vi.fn()
    Object.defineProperty(HTMLElement.prototype, 'scrollIntoView', { configurable: true, value: scroll })
    const host = document.createElement('div'); document.body.appendChild(host); const root = createRoot(host)
    try {
      act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
      expect(host.textContent).not.toContain('Hidden historical match')
      scroll.mockClear()
      const target = { generation: 101, queryRequestId: 'search-1', chatId: 'chat-1', messageIndex: 101, messagesDigest: 'old', phase: 'ready' as const, anchored: false }
      act(() => useAppStore.setState({ chatSearchNavigation: target }))
      expect(host.querySelector('[data-message-index="101"]')?.textContent).toContain('Hidden historical match')
      expect(scroll.mock.calls.filter(([options]) => options.block === 'center')).toHaveLength(1)
      expect(useAppStore.getState().chatSearchNavigation?.anchored).toBe(true)
      act(() => useAppStore.setState({ messages: { 'chat-1': [...messages] } }))
      expect(scroll.mock.calls.filter(([options]) => options.block === 'center')).toHaveLength(1)
      act(() => useAppStore.setState({ chatSearchNavigation: { ...target, generation: 102, queryRequestId: 'search-2' } }))
      expect(scroll.mock.calls.filter(([options]) => options.block === 'center')).toHaveLength(2)
      act(() => useAppStore.setState({ chatSearchNavigation: null }))
      expect(scroll.mock.calls.filter(([options]) => options.block === 'center')).toHaveLength(2)
      expect((host.querySelector('textarea') as HTMLTextAreaElement).value).toBe(draft)
      expect(readChatComposerDraft('chat-1')?.text).toBe(draft)
    } finally {
      act(() => root.unmount()); host.remove()
      if (descriptor) Object.defineProperty(HTMLElement.prototype, 'scrollIntoView', descriptor)
      else Reflect.deleteProperty(HTMLElement.prototype, 'scrollIntoView')
    }
  })

  it('anchors an assistant search target represented by the live timeline after its user message', () => {
    useAppStore.setState((state) => ({ acpBindingBySessionId: { 'chat-1': {
      ...state.acpBindingBySessionId['chat-1'], turnEvents: [
        { type: 'tool_call', toolCallId: 'tool-1' }, { type: 'assistant_text', text: 'Live response after tool' },
      ],
    } } }))
    const descriptor = Object.getOwnPropertyDescriptor(HTMLElement.prototype, 'scrollIntoView')
    const scroll = vi.fn()
    Object.defineProperty(HTMLElement.prototype, 'scrollIntoView', { configurable: true, value: scroll })
    const host = document.createElement('div'); document.body.appendChild(host); const root = createRoot(host)
    try {
      act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
      expect(host.textContent).toContain('Live response after tool')
      scroll.mockClear()
      act(() => useAppStore.setState({ chatSearchNavigation: { generation: 104, queryRequestId: 'live-search', chatId: 'chat-1', messageIndex: 1, messagesDigest: 'live', phase: 'ready', anchored: false } }))
      expect(scroll.mock.calls.filter(([options]) => options.block === 'center')).toHaveLength(1)
      expect(useAppStore.getState().chatSearchNavigation?.anchored).toBe(true)
    } finally {
      act(() => root.unmount()); host.remove()
      if (descriptor) Object.defineProperty(HTMLElement.prototype, 'scrollIntoView', descriptor)
      else Reflect.deleteProperty(HTMLElement.prototype, 'scrollIntoView')
    }
  })

  it('does not scroll an inactive pane for a search target it cannot consume', () => {
    const descriptor = Object.getOwnPropertyDescriptor(HTMLElement.prototype, 'scrollIntoView')
    const scroll = vi.fn()
    Object.defineProperty(HTMLElement.prototype, 'scrollIntoView', { configurable: true, value: scroll })
    useAppStore.setState({ activeSessionId: 'chat-2' })
    const host = document.createElement('div'); document.body.appendChild(host); const root = createRoot(host)
    try {
      act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
      scroll.mockClear()
      act(() => useAppStore.setState({ chatSearchNavigation: { generation: 103, queryRequestId: 'stale-search', chatId: 'chat-1', messageIndex: 0, messagesDigest: 'old', phase: 'ready', anchored: false } }))
      expect(scroll.mock.calls.filter(([options]) => options.block === 'center')).toHaveLength(0)
      expect(useAppStore.getState().chatSearchNavigation?.anchored).toBe(false)
    } finally {
      act(() => root.unmount()); host.remove()
      if (descriptor) Object.defineProperty(HTMLElement.prototype, 'scrollIntoView', descriptor)
      else Reflect.deleteProperty(HTMLElement.prototype, 'scrollIntoView')
    }
  })

  it('follows streaming output at the padded end and leaves earlier reading in place', async () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    const transcript = host.querySelector('.uam-chat-transcript') as HTMLDivElement
    const content = transcript.querySelector('.uam-chat-content') as HTMLDivElement
    const bottom = transcript.lastElementChild as HTMLDivElement
    // The scroll target must include the composer clearance, rather than end inside it.
    expect(bottom.previousElementSibling).toBe(content)
    expect(content.style.paddingBottom).toBe('calc(var(--composer-dock-h, 0px) + 16px)')
    const scrollIntoView = vi.fn()
    bottom.scrollIntoView = scrollIntoView
    Object.defineProperties(transcript, {
      scrollHeight: { configurable: true, value: 1000 },
      clientHeight: { configurable: true, value: 200 },
      scrollTop: { configurable: true, writable: true, value: 800 },
    })
    const appendOutput = () => useAppStore.setState((state) => ({
      messages: { ...state.messages, 'chat-1': state.messages['chat-1'].map((message) =>
        message.role === 'assistant' ? { ...message, content: message.content + ' more output' } : message
      ) },
    }))
    act(appendOutput)
    expect(scrollIntoView).toHaveBeenCalledWith({ block: 'end' })
    scrollIntoView.mockClear()

    transcript.scrollTop = 100
    await act(async () => {
      transcript.dispatchEvent(new Event('scroll', { bubbles: true }))
      await new Promise(window.requestAnimationFrame)
    })
    act(appendOutput)
    expect(scrollIntoView).not.toHaveBeenCalled()
    expect(transcript.scrollTop).toBe(100)
    act(() => root.unmount())
    host.remove()
  })

  it('preserves bottom following across dock, content and viewport resizing without pulling readers down', async () => {
    let notifyResize = () => {}
    const observe = vi.fn()
    const disconnect = vi.fn()
    vi.stubGlobal('ResizeObserver', class {
      constructor(callback: () => void) { notifyResize = callback }
      observe = observe
      disconnect = disconnect
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    const dock = host.querySelector('.uam-composer-dock') as HTMLDivElement
    const transcript = host.querySelector('.uam-chat-transcript') as HTMLDivElement
    const content = transcript.querySelector('.uam-chat-content') as HTMLDivElement
    let dockHeight = 150
    let scrollHeight = 1000
    Object.defineProperty(dock, 'offsetHeight', { configurable: true, get: () => dockHeight })
    Object.defineProperties(transcript, {
      scrollHeight: { configurable: true, get: () => scrollHeight },
      clientHeight: { configurable: true, value: 200 },
      scrollTop: { configurable: true, writable: true, value: 800 },
    })
    expect(observe).toHaveBeenCalledWith(dock)
    expect(observe).toHaveBeenCalledWith(content)
    expect(observe).toHaveBeenCalledWith(transcript)
    act(notifyResize)
    expect(dock.parentElement?.style.getPropertyValue('--composer-dock-h')).toBe('150px')
    expect(transcript.scrollTop).toBe(1000)

    scrollHeight = 1200
    act(notifyResize)
    expect(transcript.scrollTop).toBe(1200)

    transcript.scrollTop = 100
    await act(async () => {
      transcript.dispatchEvent(new Event('scroll', { bubbles: true }))
      dockHeight = 250
      notifyResize()
      expect(transcript.scrollTop).toBe(100)
      await new Promise(window.requestAnimationFrame)
    })
    expect(dock.parentElement?.style.getPropertyValue('--composer-dock-h')).toBe('250px')
    expect(transcript.scrollTop).toBe(100)

    transcript.scrollTop = 1000
    await act(async () => {
      transcript.dispatchEvent(new Event('scroll', { bubbles: true }))
      await new Promise(window.requestAnimationFrame)
    })
    dockHeight = 100
    act(notifyResize)
    expect(transcript.scrollTop).toBe(1200)
    act(() => root.unmount())
    expect(disconnect).toHaveBeenCalled()
    host.remove()
    vi.unstubAllGlobals()
  })

  it('offers a jump to the latest message only while scrolled away from the bottom', async () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    const transcript = host.querySelector('.uam-chat-transcript') as HTMLDivElement
    const bottom = transcript.lastElementChild as HTMLDivElement
    const scrollIntoView = vi.fn()
    bottom.scrollIntoView = scrollIntoView
    Object.defineProperties(transcript, {
      scrollHeight: { configurable: true, value: 1000 },
      clientHeight: { configurable: true, value: 200 },
      scrollTop: { configurable: true, writable: true, value: 100 },
    })

    await act(async () => {
      transcript.dispatchEvent(new Event('scroll', { bubbles: true }))
      await new Promise(window.requestAnimationFrame)
    })
    const jumpButton = host.querySelector('button[aria-label="Scroll to bottom"]') as HTMLButtonElement | null
    expect(jumpButton).not.toBeNull()
    expect(transcript.classList.contains('relative')).toBe(true)
    expect(transcript.classList.contains('z-0')).toBe(true)
    expect(jumpButton?.parentElement?.classList.contains('z-10')).toBe(true)

    transcript.scrollTop = 800
    await act(async () => {
      transcript.dispatchEvent(new Event('scroll', { bubbles: true }))
      await new Promise(window.requestAnimationFrame)
    })
    expect(host.querySelector('button[aria-label="Scroll to bottom"]')).toBeNull()

    transcript.scrollTop = 100
    await act(async () => {
      transcript.dispatchEvent(new Event('scroll', { bubbles: true }))
      await new Promise(window.requestAnimationFrame)
    })
    act(() => (host.querySelector('button[aria-label="Scroll to bottom"]') as HTMLButtonElement | null)
      ?.dispatchEvent(new MouseEvent('click', { bubbles: true })))
    expect(scrollIntoView).toHaveBeenCalledWith({ block: 'end', behavior: 'smooth' })
    expect(host.querySelector('button[aria-label="Scroll to bottom"]')).toBeNull()

    act(() => root.unmount())
    host.remove()
  })

  it('uses the persisted assistant duration when a completed turn timeline stays visible', () => {
    useAppStore.setState((state) => ({
      workingDisplayMode: 'compact',
      messages: {
        ...state.messages,
        'chat-1': state.messages['chat-1'].map((message) =>
          message.role === 'assistant' ? { ...message, processingTimeMs: 83_000 } : message
        ),
      },
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          lifecycleState: 'ready',
          processing: false,
          processingStartedAtMs: null,
          pendingPermission: null,
        },
      },
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    expect(host.querySelector('[data-testid="working-summary"]')?.textContent).toContain('Worked 1m 23s')
    expect(host.querySelector('[data-testid="working-summary"]')?.textContent).not.toContain('Worked 0s')

    act(() => root.unmount())
    host.remove()
  })

  it('uses the composer action as Cancel turn while the runtime is processing', async () => {
    const cancelAcpTurn = vi.fn(() => Promise.resolve(true))
    const stopAcpSession = vi.fn(() => Promise.resolve(true))
    useAppStore.setState((state) => ({
      stopAcpSession,
      cancelAcpTurn,
      sessions: state.sessions.map((session) => ({ ...session, commandSafetyTier: 'off' as const })),
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    const stopButton = Array.from(host.querySelectorAll('button')).find((button) => button.title === 'Cancel turn') as HTMLButtonElement
    expect(stopButton).toBeTruthy()
    expect(host.querySelector('[data-mode-chip="Permissions: Default"]')).toBeTruthy()
    expect(host.querySelector('button[aria-label="Permissions: Default"]')).toBeNull()
    expect(host.querySelector('.uam-composer-toolbar .uam-composer-status-chips [data-mode-chip="Permissions: Default"]')).toBeTruthy()
    expect(host.querySelector('.uam-composer-surface')).toBeTruthy()
    expect(host.querySelector('[data-mode-chip="Permissions: Default"] button')?.textContent).toBe('Default')
    expect(host.querySelector('[aria-label="Chat settings"]')).toBeNull()
    expect(stopButton.classList.contains('shrink-0')).toBe(true)
    expect(host.textContent).toContain('Working ')

    await act(async () => {
      stopButton.dispatchEvent(new MouseEvent('click', { bubbles: true }))
      await Promise.resolve()
    })

    expect(cancelAcpTurn).toHaveBeenCalledWith('chat-1')
    expect(stopAcpSession).not.toHaveBeenCalled()

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('starts one working clock immediately and keeps time when the first turn event arrives', async () => {
    vi.useFakeTimers()
    useAppStore.setState((state) => ({
      expandWorkTraces: true,
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          lifecycleState: 'processing',
          processing: true,
          turnEvents: [],
          turnAssistantMessageIndex: -1,
        },
      },
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    expect(host.querySelectorAll('[data-testid="turn-starting"]')).toHaveLength(1)
    expect(host.querySelector('[data-testid="turn-starting"]')?.textContent).toContain('Working 0s')
    expect(host.querySelector('[data-testid="turn-starting"] [aria-label="Collapse work trace"]')).toBeNull()
    expect(host.querySelectorAll('[aria-label="Processing"]')).toHaveLength(0)
    act(() => vi.advanceTimersByTime(2000))
    expect(host.querySelector('[data-testid="turn-starting"]')?.textContent).toContain('Working 2s')

    await act(async () => {
      useAppStore.setState((state) => ({
        acpBindingBySessionId: {
          ...state.acpBindingBySessionId,
          'chat-1': {
            ...state.acpBindingBySessionId['chat-1'],
            toolCalls: [{ id: 'first-tool', title: 'Inspect workspace', kind: 'read', status: 'running', content: '' }],
            turnEvents: [{ type: 'assistant_text', text: 'First token' }, { type: 'thought', text: 'Checking the files' }, { type: 'tool_call', toolCallId: 'first-tool' }],
          },
        },
      }))
      await Promise.resolve()
    })
    expect(host.querySelector('[data-testid="turn-starting"]')).toBeNull()
    expect(host.textContent).toContain('First token')
    expect(host.querySelector('[aria-label="Collapse work trace"]')).toBeNull()
    expect(host.textContent).toContain('Inspect workspace')
    expect(host.textContent).toContain('Checking the files')
    expect(host.textContent).toContain('Working 2s')
    expect(host.querySelectorAll('[data-processing-step="true"]')).toHaveLength(0)
    expect(host.querySelector('[aria-label="Processing"]')).toBeNull()
    expect(host.querySelector('time.conversation-turn-start')?.getAttribute('datetime')).toBe('2026-01-01T00:00:00.000Z')
    act(() => useAppStore.setState((state) => ({ acpBindingBySessionId: {
      ...state.acpBindingBySessionId,
      'chat-1': { ...state.acpBindingBySessionId['chat-1'], processing: false },
    } })))
    const completedText = host.querySelector('[data-testid="working-summary"]')?.textContent
    expect(completedText).toContain('Worked')
    act(() => vi.advanceTimersByTime(3000))
    expect(host.querySelector('[data-testid="working-summary"]')?.textContent).toBe(completedText)
    expect(host.querySelector('[data-processing-step="true"]')).toBeNull()
    act(() => root.unmount())
    vi.useRealTimers()
    host.remove()
  })

  it.each(['accepted', 'rejected', 'error'] as const)('releases steering controls after a %s request within the same turn', async (outcome) => {
    let completeSteer!: (ok: boolean) => void
    let failSteer!: () => void
    const steerQueuedAcpPrompt = vi.fn(() => new Promise<boolean>((resolve, reject) => {
      completeSteer = resolve
      failSteer = () => reject(new Error('Request failed'))
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    const sessionId = useAppStore.getState().sessions[0].id

    useAppStore.setState((state) => ({
      steerQueuedAcpPrompt,
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        [sessionId]: {
          ...state.acpBindingBySessionId[sessionId],
          turnSerial: 3,
          processing: true,
          turnEvents: [{ type: 'assistant_text', text: 'Streaming now.' }],
          queuedPrompts: [{
            text: 'Re-evaluate this first.',
            markdownStoreFiles: [],
            attachments: [],
            goalMode: false,
            goalId: '',
            prioritySteer: true,
          }],
        },
      },
    }))
    const steerButtonSelector = 'button[aria-label="Steer with this prompt now"]'

    vi.useFakeTimers()
    await act(async () => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
      await Promise.resolve()
    })

    const steerButton = host.querySelector(steerButtonSelector) as HTMLButtonElement
    expect(steerButton).toBeTruthy()
    act(() => {
      steerButton.click()
    })
    expect(steerQueuedAcpPrompt).toHaveBeenCalledWith(sessionId, 0)
    expect(steerButton.disabled).toBe(true)

    act(() => vi.advanceTimersByTime(6000))
    expect((host.querySelector(steerButtonSelector) as HTMLButtonElement).disabled).toBe(true)
    await act(async () => {
      if (outcome === 'error') failSteer()
      else completeSteer(outcome === 'accepted')
      await Promise.resolve()
    })
    expect(useAppStore.getState().acpBindingBySessionId[sessionId].turnSerial).toBe(3)

    const clearedSteerButton = host.querySelector(steerButtonSelector) as HTMLButtonElement
    expect(clearedSteerButton.disabled).toBe(false)

    act(() => {
      root.unmount()
    })
    host.remove()
    vi.useRealTimers()
  })

  it('combines provider and model controls and docks the folder icon, name and workspace actions above the composer', () => {
    useAppStore.setState((state) => ({
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          lifecycleState: 'ready',
          processing: false,
          pendingPermission: null,
        },
      },
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    expect(host.querySelector('.uam-workspace-row')).toBeNull()
    expect(host.querySelector('button[aria-label="Select provider"]')).toBeNull()
    const workspaceTab = host.querySelector('.uam-composer-workspace-tab')
    expect(workspaceTab?.textContent).toContain('Project')
    expect(workspaceTab?.querySelector('svg')).toBeTruthy()
    expect(workspaceTab?.querySelector('button[aria-label="Workspace actions"]')).toBeTruthy()
    expect(workspaceTab?.parentElement?.nextElementSibling?.classList.contains('uam-composer-surface')).toBe(true)
    expect(host.querySelector('.uam-composer-toolbar button[aria-label="Workspace actions"]')).toBeNull()

    const selector = host.querySelector('button[aria-label="Select provider and model"]') as HTMLButtonElement
    expect(selector).toBeTruthy()
    expect(selector.querySelector('img, svg')).toBeTruthy()
    act(() => selector.click())

    const menu = document.body.querySelector('[role="menu"][aria-label="Provider and model"]') as HTMLElement
    expect(menu).toBeTruthy()
    expect(menu.querySelector('[role="toolbar"][aria-label="Providers"]')).toBeTruthy()
    expect(menu.querySelector('input[aria-label="Search models"]')).toBeTruthy()
    expect(menu.querySelector('button[aria-label="Gemini"]')).toBeTruthy()
    expect(menu.textContent).toContain('Auto 3')

    act(() => root.unmount())
    host.remove()
  })

  it('queues a draft during an active turn and keeps it when sending fails', async () => {
    const sendAcpPrompt = vi.fn(() => Promise.resolve(false))
    useAppStore.setState({ sendAcpPrompt })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    const textarea = host.querySelector('textarea') as HTMLTextAreaElement
    act(() => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set?.call(textarea, 'Change direction now')
      textarea.dispatchEvent(new Event('input', { bubbles: true }))
    })
    const queue = host.querySelector('button[aria-label="Queue prompt"]') as HTMLButtonElement
    expect(queue).toBeTruthy()
    await act(async () => { queue.click(); await Promise.resolve() })

    expect(sendAcpPrompt).toHaveBeenCalledWith('chat-1', 'Change direction now', [], undefined, expect.any(Function))
    expect(textarea.value).toBe('Change direction now')
    expect(host.querySelector('button[aria-label="Steering prompt"]')).toBeNull()

    act(() => root.unmount())
    host.remove()
  })

  it.each([true, false])('preserves new text and attachments during prompt admission acceptance=%s', async (accepted) => {
    let finishSend!: (ok: boolean) => void
    const send = vi.fn(() => new Promise<boolean>((resolve) => { finishSend = resolve }))
    const stageChatAttachments = vi.fn((_chatId, items) => Promise.resolve(items.map((item: { id: string; name: string }) => ({ id: item.id, name: item.name, type: 'image', size: 4, path: '/tmp/new.png' }))))
    useAppStore.setState({ sendAcpPrompt: send, stageChatAttachments })
    writeChatComposerDraft('chat-1', { text: 'My draft', attachments: [{ id: 'sent-file', name: 'sent.png', type: 'image', size: 4, path: '/tmp/sent.png' }] })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    try {
      act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
      const textarea = host.querySelector('textarea') as HTMLTextAreaElement
      expect(textarea.value).toBe('My draft')
      await act(async () => host.querySelector<HTMLButtonElement>('button[aria-label="Queue prompt"]')!.click())
      expect(send).toHaveBeenCalledTimes(1)
      const submitted = send.mock.calls[0] as unknown as [string, string]
      expect(submitted[1]).toContain('My draft')
      expect(textarea.value).toBe('My draft')
      expect(textarea.disabled).toBe(false)
      act(() => {
        Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set?.call(textarea, 'New unsent draft')
        textarea.dispatchEvent(new Event('input', { bubbles: true }))
      })
      const fileInput = host.querySelector('input[type="file"]') as HTMLInputElement
      const file = new File(['data'], 'new.png', { type: 'image/png' })
      Object.defineProperty(file, 'path', { value: '/tmp/new.png' })
      Object.defineProperty(fileInput, 'files', { value: [file], configurable: true })
      await act(async () => fileInput.dispatchEvent(new Event('change', { bubbles: true })))
      expect(host.textContent).toContain('new.png')
      await act(async () => finishSend(accepted))
      expect(host.textContent).toContain('new.png')
      expect(host.textContent?.includes('sent.png')).toBe(!accepted)
      expect(textarea.value).toBe('New unsent draft')

    } finally {
      act(() => root.unmount())
      host.remove()
    }
  })

  it('submits a prompt only once before the submitting state rerenders', async () => {
    const finishSends: Array<(ok: boolean) => void> = []
    const sendAcpPrompt = vi.fn(() => new Promise<boolean>((resolve) => finishSends.push(resolve)))
    useAppStore.setState({ sendAcpPrompt })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    const textarea = host.querySelector('textarea') as HTMLTextAreaElement
    act(() => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set?.call(textarea, 'Queue only once')
      textarea.dispatchEvent(new Event('input', { bubbles: true }))
    })
    const queue = host.querySelector('button[aria-label="Queue prompt"]') as HTMLButtonElement
    act(() => {
      queue.click()
      queue.click()
    })

    await act(async () => { await Promise.resolve() })
    expect(sendAcpPrompt).toHaveBeenCalledTimes(1)
    await act(async () => {
      finishSends.forEach((finish) => finish(false))
      await Promise.resolve()
    })
    act(() => root.unmount())
    host.remove()
  })

  it('does not let a send finishing in another chat clear the current draft', async () => {
    let finishSend: (ok: boolean) => void = () => {}
    const sendAcpPrompt = vi.fn(() => new Promise<boolean>((resolve) => { finishSend = resolve }))
    useAppStore.setState((state) => ({
      sendAcpPrompt,
      sessions: [
        state.sessions[0],
        {
          ...state.sessions[0],
          id: 'chat-2',
          name: 'Second chat',
        },
      ],
      messages: { ...state.messages, 'chat-2': [] },
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-2': {
          ...state.acpBindingBySessionId['chat-1'],
          sessionId: 'native-2',
          processing: false,
          pendingPermission: null,
          turnEvents: [],
        },
      },
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    let textarea = host.querySelector('textarea') as HTMLTextAreaElement
    act(() => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set?.call(textarea, 'First chat prompt')
      textarea.dispatchEvent(new Event('input', { bubbles: true }))
    })
    act(() => (host.querySelector('button[aria-label="Queue prompt"]') as HTMLButtonElement).click())

    act(() => root.render(<ChatView session={useAppStore.getState().sessions[1]} />))
    textarea = host.querySelector('textarea') as HTMLTextAreaElement
    act(() => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set?.call(textarea, 'Second chat draft')
      textarea.dispatchEvent(new Event('input', { bubbles: true }))
    })
    await act(async () => {
      finishSend(true)
      await Promise.resolve()
    })

    expect(textarea.value).toBe('Second chat draft')

    act(() => root.unmount())
    host.remove()
  })

  it.each(['chat-1791274259986-d110b3', 'chat-1792011220733-48d847'])('removes a legacy unsent input from the displayed family member %s while preserving its branch history', async (chatId) => {
    const previous = useAppStore.getState()
    let finish!: (ok: boolean) => void
    const discard = vi.fn(() => new Promise<boolean>((resolve) => { finish = resolve }))
    const original = { ...previous.messages['chat-1'][0], sessionId: chatId, content: 'Explain this please!', acpPromptNotSent: true, interrupted: true }
    const later = { ...original, id: 'later', content: 'Delivered branch request', acpPromptNotSent: false, interrupted: false }
    const session = { ...previous.sessions[0], id: chatId, messagesDigest: 'old-digest', userMessageIndices: [0, 1] }
    useAppStore.setState({ sessions: [session], activeSessionId: chatId, discardUnsentMessage: discard,
      messages: { [chatId]: [original, later] }, acpBindingBySessionId: { [chatId]: { ...previous.acpBindingBySessionId['chat-1'],
        sessionId: 'native', running: true, processing: false, lifecycleState: 'error', turnEvents: [], toolCalls: [],
        pendingPermission: null, pendingUserInput: null, unsentMessageIndices: [0], recoverableUnsentMessageIndices: [], queuedPrompts: [] } } })
    writeChatComposerDraft(chatId, { text: 'Independent composer draft', attachments: [] })
    const host = document.createElement('div'); document.body.append(host); const root = createRoot(host)
    try {
      const render = () => root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
      act(render)
      const remove = host.querySelector<HTMLButtonElement>('[aria-label="Remove unsent prompt"]')!
      expect(remove.disabled).toBe(false)
      act(() => { remove.click(); remove.click() })
      expect(discard).toHaveBeenCalledExactlyOnceWith(chatId, 0, original.id)
      expect(host.querySelector('[data-unsent-message-index="0"]')?.textContent).toContain(original.content)
      expect(host.querySelector('[data-message-index="0"] article[data-delivery="unsent"]')).not.toBeNull()
      act(() => {
        useAppStore.setState({ messages: { [chatId]: [{ ...original, acpPromptDiscarded: true }, later] },
          sessions: [{ ...session, messagesDigest: 'new-digest' }] })
        render()
      })
      await act(async () => finish(true))
      expect(host.querySelector('[data-unsent-message-index]')).toBeNull()
      expect(host.querySelector('.uam-message-list')?.textContent).not.toContain(original.content)
      expect(host.querySelector('[data-message-index="1"]')?.textContent).toContain(later.content)
      expect(useAppStore.getState().messages[chatId]).toHaveLength(2)
      expect(readChatComposerDraft(chatId).text).toBe('Independent composer draft')
    } finally { act(() => root.unmount()); host.remove(); useAppStore.setState({ discardUnsentMessage: previous.discardUnsentMessage }) }
  })

  it('keeps an explicitly discarded cold portable row hidden without runtime recovery authority', () => {
    const original = { ...useAppStore.getState().messages['chat-1'][0], acpPromptNotSent: true, acpPromptDiscarded: true }
    const placeholder = { ...original, role: 'assistant' as const, content: '', acpPromptNotSent: false, acpPromptDiscarded: false }
    const later = { ...original, id: 'later', content: 'Delivered branch request', acpPromptNotSent: false, acpPromptDiscarded: false }
    const projection = projectUnsentPrompts([original, placeholder, later], 0, [], [])
    expect(projection.prompts).toEqual([])
    expect([...projection.hiddenIndices]).toEqual([0, 1])
  })

  it.each([{ readOnly: false, nativeSession: '' }, { readOnly: true, nativeSession: '' }, { readOnly: false, nativeSession: 'native-ready' }])('projects legacy unsent inputs in their transcript slots with exact recovery indices (readOnly=$readOnly, nativeSession=$nativeSession)', async ({ readOnly, nativeSession }) => {
    const previous = useAppStore.getState()
    const retry = vi.fn(async () => ({ ok: false, error: 'Delivery remains unconfirmed.' }))
    const image = { id: 'image', name: 'retained.png', type: 'file', mimeType: 'image/png', size: 4, path: '/tmp/retained.png' }
    const skill = { id: '/skills/inspect.md', name: 'inspect.md', type: 'markdown-store', size: 0, path: '/skills/inspect.md' }
    useAppStore.setState((state) => ({ retryFailedMessage: retry, historyStartIndexBySessionId: { 'chat-1': 7 },
      sessions: state.sessions.map((session) => ({ ...session, importedReadOnly: readOnly, messagesDigest: 'legacy', userMessageIndices: [7, 8] })),
      messages: { 'chat-1': [
        { ...state.messages['chat-1'][0], content: 'Earlier unsent attempt', acpPromptNotSent: true, interrupted: true, attachments: [image, skill] },
        { ...state.messages['chat-1'][0], id: 'latest', content: 'Latest unsent attempt', acpPromptNotSent: true, interrupted: true },
      ] },
      acpBindingBySessionId: { 'chat-1': { ...state.acpBindingBySessionId['chat-1'], sessionId: nativeSession, running: Boolean(nativeSession), processing: false,
        lifecycleState: nativeSession ? 'ready' : 'error', turnEvents: [], toolCalls: [], turnUserMessageIndex: -1, turnAssistantMessageIndex: -1,
        pendingPermission: null, pendingUserInput: null, unsentMessageIndices: [7, 8], recoverableUnsentMessageIndices: [8] } },
    }))
    writeChatComposerDraft('chat-1', { text: 'Independent draft', attachments: [] })
    const host = document.createElement('div'); document.body.append(host); const root = createRoot(host)
    try {
      act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
      const recovery = host.querySelector('.uam-message-list')!
      expect(host.querySelector('[data-unsent-message-index="7"]')?.textContent).toContain('Earlier unsent attempt')
      expect(host.querySelector('[data-unsent-message-index="8"]')?.textContent).toContain('Latest unsent attempt')
      expect(recovery.textContent).toContain('retained.png')
      expect(recovery.textContent).toContain('inspect.md')
      expect(host.querySelectorAll('article[aria-label="You"][data-delivery="unsent"]')).toHaveLength(2)
      expect(host.querySelector('[aria-label="Navigate user messages"]')?.getAttribute('aria-valuemax')).toBe('2')
      expect(host.textContent).not.toContain('Message was not sent')
      expect(host.querySelector('[aria-label="Edit message in new branch"]')).toBeNull()
      expect(host.querySelector('.uam-composer-dock--centred')).toBeNull()
      const retryButton = recovery.querySelector<HTMLButtonElement>('[aria-label="Retry message"]')!
      expect(recovery.querySelectorAll('[aria-label="Retry message"]')).toHaveLength(1)
      expect(retryButton.disabled).toBe(readOnly)
      expect(recovery.querySelector<HTMLButtonElement>('[aria-label="Remove unsent prompt"]')!.disabled).toBe(readOnly)
      expect(retry).not.toHaveBeenCalled()
      await act(async () => retryButton.click())
      if (readOnly) expect(retry).not.toHaveBeenCalled()
      else {
        expect(retry).toHaveBeenCalledWith('chat-1', 8)
        expect(host.textContent).toContain('Delivery remains unconfirmed.')
        expect(host.querySelectorAll('[role="alert"]')).toHaveLength(1)
      }
      const editor = recovery.querySelector<HTMLButtonElement>('[aria-label="Edit unsent message"]')!
      expect(editor.disabled).toBe(readOnly)
      act(() => editor.click())
      if (readOnly) expect(host.querySelector('[aria-label="Edit message"]')).toBeNull()
      else {
        expect(host.querySelector<HTMLTextAreaElement>('[aria-label="Edit message"]')!.value).toBe('Earlier unsent attempt')
        expect(retryButton.disabled).toBe(true)
        expect(host.querySelector('[aria-label="Remove retained.png from edited message"]')).toBeTruthy()
      }
      expect(useAppStore.getState().messages['chat-1'][0].attachments).toEqual([image, skill])
      expect(readChatComposerDraft('chat-1').text).toBe('Independent draft')
    } finally { act(() => root.unmount()); host.remove(); useAppStore.setState({ retryFailedMessage: previous.retryFailedMessage }) }
  })

  it('preserves companion branch and retry restrictions for readable legacy unsent inputs', () => {
    const previousPath = window.location.pathname
    const previous = useAppStore.getState()
    window.history.pushState({}, '', '/companion')
    useAppStore.setState((state) => ({ discoverProviderModels: vi.fn(async () => true),
      messages: { 'chat-1': [{ ...state.messages['chat-1'][0], content: 'Companion unsent input', acpPromptNotSent: true, interrupted: true }] },
      acpBindingBySessionId: { 'chat-1': { ...state.acpBindingBySessionId['chat-1'], processing: false, running: false,
        turnEvents: [], toolCalls: [], pendingPermission: null, pendingUserInput: null,
        unsentMessageIndices: [0], recoverableUnsentMessageIndices: [0] } } }))
    const host = document.createElement('div'); document.body.append(host); const root = createRoot(host)
    try {
      act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
      const recovery = host.querySelector('[data-unsent-message-index="0"]')!
      expect(recovery.textContent).toContain('Companion unsent input')
      expect(recovery.querySelector('[aria-label="Edit unsent message"]')).toBeNull()
      expect(recovery.querySelector('[aria-label="Retry message"]')).toBeNull()
      expect(recovery.querySelector('[aria-label="Remove unsent prompt"]')).toBeNull()
      expect(recovery.querySelector('[aria-label="Edit message in new branch"]')).toBeNull()
      expect(recovery.querySelector('article[aria-label="You"][data-delivery="unsent"]')).not.toBeNull()
    } finally { act(() => root.unmount()); host.remove(); window.history.pushState({}, '', previousPath); useAppStore.setState({ discoverProviderModels: previous.discoverProviderModels }) }
  })

  it('keeps portable read-only unsent history readable when native recovery metadata is absent', () => {
    useAppStore.setState((state) => ({ sessions: state.sessions.map((session) => ({ ...session, importedReadOnly: true })),
      messages: { 'chat-1': [{ ...state.messages['chat-1'][0], content: 'Portable unsent input', acpPromptNotSent: true, interrupted: true }] },
      acpBindingBySessionId: { 'chat-1': { ...state.acpBindingBySessionId['chat-1'], processing: false, running: false,
        turnEvents: [], toolCalls: [], pendingPermission: null, pendingUserInput: null, unsentMessageIndices: [], recoverableUnsentMessageIndices: [] } } }))
    const host = document.createElement('div'); document.body.append(host); const root = createRoot(host)
    try {
      act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
      expect(host.querySelector('article[aria-label="You"]')?.textContent).toContain('Portable unsent input')
      expect(host.querySelector('[aria-label="Unsent prompts"]')).toBeNull()
      expect(host.querySelector<HTMLButtonElement>('[aria-label="Retry message"]')!.disabled).toBe(true)
      expect(host.querySelector<HTMLButtonElement>('[aria-label="Edit message in new branch"]')!.disabled).toBe(true)
    } finally { act(() => root.unmount()); host.remove() }
  })

  it.each([false, true])('keeps the current starting indicator independent of an older hidden unsent input (processing=%s)', (processing) => {
    useAppStore.setState((state) => ({ messages: { 'chat-1': [{ ...state.messages['chat-1'][0], content: 'Earlier failed input', acpPromptNotSent: true, interrupted: true }] },
      acpBindingBySessionId: { 'chat-1': { ...state.acpBindingBySessionId['chat-1'], running: processing, processing,
        lifecycleState: processing ? 'processing' : 'stopped', turnEvents: [], toolCalls: [], pendingPermission: null, pendingUserInput: null,
        turnUserMessageIndex: -1, turnAssistantMessageIndex: -1, unsentMessageIndices: [0], recoverableUnsentMessageIndices: [], queueHeld: !processing,
        queuedPrompts: [{ id: 'next-native', revision: 1, text: 'Next request', uamAgentId: 'build', attachments: [], markdownStoreFiles: [], goalMode: false, goalId: '' }] } } }))
    const host = document.createElement('div'); document.body.append(host); const root = createRoot(host)
    try {
      act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
      expect(Boolean(host.querySelector('[data-testid="turn-starting"]'))).toBe(processing)
      expect(host.querySelector('[data-unsent-message-index="0"]')?.textContent).toContain('Earlier failed input')
      expect(host.querySelectorAll('article[aria-label="You"]')).toHaveLength(1)
    } finally { act(() => root.unmount()); host.remove() }
  })

  it('keeps unloaded native-confirmed unsent inputs navigable at their transcript slots without renumbering loaded messages', () => {
    const previous = useAppStore.getState()
    const navigate = vi.fn(async () => true)
    useAppStore.setState((state) => ({ jumpToSessionMessage: navigate, historyStartIndexBySessionId: { 'chat-1': 7 },
      sessions: state.sessions.map((session) => ({ ...session, messagesDigest: 'paged', userMessageIndices: [0, 7] })),
      messages: { 'chat-1': [{ ...state.messages['chat-1'][0], content: 'Delivered page input' }] },
      acpBindingBySessionId: { 'chat-1': { ...state.acpBindingBySessionId['chat-1'], processing: false, turnEvents: [], toolCalls: [],
        pendingPermission: null, pendingUserInput: null, unsentMessageIndices: [0], recoverableUnsentMessageIndices: [] } } }))
    const host = document.createElement('div'); document.body.append(host); const root = createRoot(host)
    try {
      act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
      const rail = host.querySelector<HTMLElement>('[aria-label="Navigate user messages"]')!
      expect(rail.getAttribute('aria-valuemax')).toBe('2')
      act(() => rail.dispatchEvent(new KeyboardEvent('keydown', { key: 'End', bubbles: true, cancelable: true })))
      expect(navigate).toHaveBeenCalledWith('chat-1', 7)
      expect(host.querySelector('[data-message-index="7"]')?.textContent).toContain('Delivered page input')
    } finally { act(() => root.unmount()); host.remove(); useAppStore.setState({ jumpToSessionMessage: previous.jumpToSessionMessage }) }
  })

  it('keeps an earlier unsent attempt in its transcript slot after the latest attempt is queued and delivered', () => {
    const user = useAppStore.getState().messages['chat-1'][0]
    const earlier = { ...user, content: 'Earlier rejected input', acpPromptNotSent: true, interrupted: true }
    const latest = { ...user, id: 'latest', content: 'Latest rejected input', acpPromptNotSent: true, interrupted: true }
    const binding = { ...useAppStore.getState().acpBindingBySessionId['chat-1'], sessionId: '', running: false, processing: false,
      lifecycleState: 'error' as const, turnEvents: [], toolCalls: [], pendingPermission: null, pendingUserInput: null,
      turnUserMessageIndex: -1, turnAssistantMessageIndex: -1, unsentMessageIndices: [0, 1], recoverableUnsentMessageIndices: [1] }
    useAppStore.setState({ messages: { 'chat-1': [earlier, latest] }, acpBindingBySessionId: { 'chat-1': binding } })
    const host = document.createElement('div'); document.body.append(host); const root = createRoot(host)
    const noEarlierInHistory = () => {
      expect(host.querySelector('[data-unsent-message-index="0"]')?.textContent).toContain(earlier.content)
      expect(host.querySelector('[data-message-index="0"] article')?.getAttribute('data-delivery')).toBe('unsent')
    }
    try {
      act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />)); noEarlierInHistory()
      act(() => useAppStore.setState({ messages: { 'chat-1': [earlier] }, acpBindingBySessionId: { 'chat-1': { ...binding,
        unsentMessageIndices: [0], recoverableUnsentMessageIndices: [], queueHeld: true,
        queuedPrompts: [{ id: 'native-recovery', revision: 1, text: latest.content, uamAgentId: 'read-only', markdownStoreFiles: [], attachments: [], goalMode: false, goalId: '' }] } } }))
      noEarlierInHistory()
      expect(host.querySelector('[aria-label="Queued prompt"]')?.textContent).toContain(latest.content)
      expect(host.querySelector('.uam-message-list')?.textContent).not.toContain(latest.content)
      act(() => useAppStore.setState({ messages: { 'chat-1': [earlier, { ...latest, acpPromptNotSent: false, interrupted: false },
        { id: 'answer', sessionId: 'chat-1', role: 'assistant', content: 'Successful provider response', createdAt: new Date() }] },
        acpBindingBySessionId: { 'chat-1': { ...binding, sessionId: 'new-native', lifecycleState: 'ready', unsentMessageIndices: [0], recoverableUnsentMessageIndices: [], queueHeld: false, queuedPrompts: [] } } }))
      noEarlierInHistory()
      expect(host.querySelector('.uam-message-list')?.textContent).toContain(latest.content)
      expect(host.querySelector('.uam-message-list')?.textContent).toContain('Successful provider response')
      expect(host.querySelectorAll('article[aria-label="You"]')).toHaveLength(2)
      expect(host.querySelectorAll('article[aria-label="You"]:not([data-delivery])')).toHaveLength(1)
      expect(host.querySelector('[aria-label="Retry message"]')).toBeNull()
    } finally { act(() => root.unmount()); host.remove() }
  })

  it.each([
    { content: 'Partial answer' }, { thoughts: 'Partial thought' }, { planSummary: 'Plan' },
    { toolCalls: [{ id: 'delivered-tool', title: 'Read', kind: 'read', status: 'completed', content: '' }] },
    { blocks: [{ type: 'assistant_text' as const, text: 'Partial answer' }] },
    { attachments: [{ id: 'delivered-file', name: 'output.png', type: 'file', size: 1 }] },
  ])('does not label delivered output as an unsent prompt ($content)', (output) => {
    const state = useAppStore.getState()
    const user = { ...state.messages['chat-1'][0], providerId: 'opencode-cli', interrupted: true, acpPromptNotSent: true }
    const assistant = { ...state.messages['chat-1'][1], providerId: 'opencode-cli', content: '', thoughts: '', interrupted: true, ...output }
    useAppStore.setState({ expandWorkTraces: true, messages: { 'chat-1': [user, assistant] }, acpBindingBySessionId: {
      'chat-1': { ...state.acpBindingBySessionId['chat-1'], processing: false, lifecycleState: 'stopped',
        pendingPermission: null, pendingUserInput: null, turnEvents: [], unsentMessageIndices: [], recoverableUnsentMessageIndices: [] },
    } })
    const host = document.createElement('div'); document.body.append(host); const root = createRoot(host)
    try {
      act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
      expect(host.textContent).not.toContain('Message was not sent')
      expect(host.textContent).toContain('Response interrupted')
      expect(host.querySelectorAll('article[aria-label="You"]')).toHaveLength(1)
      expect(host.querySelector('[aria-label="Unsent prompts"]')).toBeNull()
      expect(host.querySelector('[aria-label="Remove unsent prompt"]')).toBeNull()
      expect(useAppStore.getState().messages['chat-1'][0]).toBe(user)
      expect(user.acpPromptNotSent).toBe(true)
    } finally { act(() => root.unmount()); host.remove() }
  })

  it.each([false, true])('keeps one truthful interruption indicator after delivered output (assistantInterrupted=%s)', (assistantInterrupted) => {
    const state = useAppStore.getState()
    const user = { ...state.messages['chat-1'][0], providerId: 'opencode-cli', interrupted: true, acpPromptNotSent: true }
    const assistant = { ...state.messages['chat-1'][1], providerId: 'opencode-cli', content: 'Delivered partial reply', interrupted: assistantInterrupted, stopReason: '' }
    useAppStore.setState({ messages: { 'chat-1': [user, assistant] }, acpBindingBySessionId: {
      'chat-1': { ...state.acpBindingBySessionId['chat-1'], processing: false, lifecycleState: 'stopped',
        pendingPermission: null, pendingUserInput: null, turnEvents: [] },
    } })
    const host = document.createElement('div'); document.body.append(host); const root = createRoot(host)
    try {
      act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
      expect(host.textContent).not.toContain('Message was not sent')
      expect(host.textContent?.match(/Response interrupted/g)).toHaveLength(1)
    } finally { act(() => root.unmount()); host.remove() }
  })

  it.each([false, true])('reuses a persisted user row while its response streams (staleNonDelivery=%s)', (staleNonDelivery) => {
    const state = useAppStore.getState()
    let userContentReads = 0
    const user = { ...state.messages['chat-1'][0], providerId: 'opencode-cli', interrupted: staleNonDelivery, acpPromptNotSent: staleNonDelivery }
    Object.defineProperty(user, 'content', { enumerable: true, get: () => { userContentReads += 1; return 'Authored request' } })
    const assistant = { ...state.messages['chat-1'][1], providerId: 'opencode-cli', content: 'Partial response', interrupted: false }
    useAppStore.setState({ messages: { 'chat-1': [user, assistant] }, acpBindingBySessionId: {
      'chat-1': { ...state.acpBindingBySessionId['chat-1'], processing: true, lifecycleState: 'processing', turnUserMessageIndex: 0,
        turnAssistantMessageIndex: 1, pendingPermission: null, pendingUserInput: null, turnEvents: [] },
    } })
    const host = document.createElement('div'); document.body.append(host); const root = createRoot(host)
    try {
      act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
      const initialReads = userContentReads
      expect(initialReads).toBeGreaterThan(0)
      for (let index = 0; index < 3; index += 1) {
        act(() => useAppStore.setState({ messages: { 'chat-1': [user, { ...assistant, content: `Partial response ${index}` }] } }))
        expect(userContentReads).toBe(initialReads)
      }
      expect(host.textContent).toContain('Partial response 2')
    } finally { act(() => root.unmount()); host.remove() }
  })

  it.each(['empty', 'foreign provider', 'later user'])('preserves uncertain nondelivery without same-turn output (%s)', (boundary) => {
    const state = useAppStore.getState()
    const user = { ...state.messages['chat-1'][0], providerId: 'opencode-cli', interrupted: true, acpPromptNotSent: true }
    const assistant = { ...state.messages['chat-1'][1], providerId: boundary === 'foreign provider' ? 'codex-cli' : 'opencode-cli',
      content: boundary === 'empty' ? '' : 'Output belonging to another turn', thoughts: '', toolCalls: [], blocks: [], planEntries: [], planSummary: '', attachments: [] }
    const messages = boundary === 'later user' ? [user, { ...user, id: 'later-user', interrupted: false, acpPromptNotSent: false }, assistant] : [user, assistant]
    useAppStore.setState({ messages: { 'chat-1': messages }, acpBindingBySessionId: {
      'chat-1': { ...state.acpBindingBySessionId['chat-1'], processing: false, lifecycleState: 'stopped',
        pendingPermission: null, pendingUserInput: null, turnEvents: [], unsentMessageIndices: [], recoverableUnsentMessageIndices: [] },
    } })
    const host = document.createElement('div'); document.body.append(host); const root = createRoot(host)
    try {
      act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
      expect(host.textContent).toContain('Message was not sent')
      expect(host.querySelector('[aria-label="Unsent prompts"]')).toBeNull()
      expect(host.querySelector('[aria-label="Remove unsent prompt"]')).toBeNull()
    } finally { act(() => root.unmount()); host.remove() }
  })

  it.each([
    { content: 'Partial answer' }, { thoughts: 'Partial thought' }, { planSummary: 'Plan' },
    { toolCalls: [{ id: 'tool', title: 'Read', kind: 'read', status: 'completed', content: '' }] },
    { blocks: [{ type: 'assistant_text' as const, text: 'Partial answer' }] },
    { attachments: [{ id: 'file', name: 'output.png', type: 'file', size: 1 }] },
  ])('retains substantive output even if a stale unsent allowlist names its user ($content)', (output) => {
    const user = { ...useAppStore.getState().messages['chat-1'][0], acpPromptNotSent: true }
    const assistant = { ...useAppStore.getState().messages['chat-1'][1], content: '', thoughts: '', ...output }
    const projection = projectUnsentPrompts([user, assistant], 7, [7], [7])
    expect(projection.prompts).toEqual([])
    expect(projection.hiddenIndices.size).toBe(0)
    expect(projectUnsentPrompts([{ ...user, acpPromptNotSent: false }], 7, [7], [7]).prompts).toEqual([])
  })

  it.each([false, true])('keeps a failed first prompt in the held outbox without delivered history (readOnly=%s)', async (readOnly) => {
    const previous = useAppStore.getState()
    const retry = vi.fn(async () => true)
    const edit = vi.fn(async () => true)
    const remove = vi.fn(async () => true)
    const files = [{ id: 'image', name: 'photo.png', type: 'file', mimeType: 'image/png', size: 4, path: '/tmp/photo.png' }]
    const failed: CppChat = {
      id: 'chat-1', title: 'OpenCode', folderId: 'default', providerId: 'opencode-cli',
      createdAt: '2026-01-01T00:00:00.000Z', updatedAt: '2026-01-01T00:00:01.000Z',
      messages: [],
      acpSession: { providerId: 'opencode-cli', running: false, processing: false, lifecycleState: 'error',
        lastError: 'Remote provider setup failed.', queueHeld: true, turnUserMessageIndex: -1, turnAssistantMessageIndex: -1,
        queuedPrompts: [{ id: 'native-outbox-1', revision: 1, text: 'Unsent first request', uamAgentId: 'read-only',
          attachments: files, markdownStoreFiles: ['/skills/inspect.md'], goalMode: false, goalId: '' }] },
    }
    useAppStore.setState((state) => ({
      providers: [...state.providers, { id: 'opencode-cli', name: 'OpenCode', shortName: 'OpenCode', color: '#fff', description: '', outputMode: 'cli', supportsCli: true, supportsStructured: true, structuredProtocol: 'opencode-acp' }],
      sessions: state.sessions.map((session) => ({ ...session, providerId: 'opencode-cli', importedReadOnly: readOnly })),
      messages: { 'chat-1': reconcileCppMessages('chat-1', state.messages['chat-1'], failed.messages!, true) },
      acpBindingBySessionId: { 'chat-1': acpBindingFromCppChat(failed, state.acpBindingBySessionId['chat-1']) },
      resumeQueuedAcpPrompts: retry, editQueuedAcpPrompt: edit, removeQueuedAcpPrompt: remove,
    }))
    writeChatComposerDraft('chat-1', { text: 'Independent draft', attachments: [] })
    const host = document.createElement('div'); document.body.append(host); const root = createRoot(host)
    try {
      act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
      const queue = host.querySelector('[aria-label="Queued prompt"]')!
      expect(queue.textContent).toContain('Unsent first request')
      expect(queue.textContent).toContain('Queue held')
      expect(queue.textContent).not.toContain('Held after restart')
      expect(queue.textContent).toContain('photo.png')
      expect(queue.textContent).toContain('inspect.md')
      expect(host.querySelector('.uam-message-list')?.textContent).not.toContain('Unsent first request')
      expect(host.querySelector('article[aria-label="You"]')).toBeNull()
      expect(host.textContent).not.toContain('Message was not sent')
      expect(host.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')!.value).toBe('Independent draft')
      const resume = Array.from(queue.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Resume queue')!
      expect(resume.disabled).toBe(readOnly)
      expect(queue.querySelector<HTMLButtonElement>('[aria-label="Edit queued prompt"]')!.disabled).toBe(readOnly)
      expect(queue.querySelector<HTMLButtonElement>('[aria-label="Remove queued prompt"]')!.disabled).toBe(readOnly)
      expect(queue.querySelector<HTMLButtonElement>('[aria-label="Steer with this prompt now"]')!.disabled).toBe(true)
      expect(retry).not.toHaveBeenCalled()
      await act(async () => resume.click())
      if (readOnly) expect(retry).not.toHaveBeenCalled()
      else expect(retry).toHaveBeenCalledWith('chat-1', [{ id: 'native-outbox-1', revision: 1 }])
      expect(useAppStore.getState().acpBindingBySessionId['chat-1'].queuedPrompts?.[0]).toMatchObject({ attachments: files, markdownStoreFiles: ['/skills/inspect.md'], uamAgentId: 'read-only' })
    } finally {
      act(() => root.unmount()); host.remove()
      useAppStore.setState({ resumeQueuedAcpPrompts: previous.resumeQueuedAcpPrompts, editQueuedAcpPrompt: previous.editQueuedAcpPrompt, removeQueuedAcpPrompt: previous.removeQueuedAcpPrompt })
    }
  })

  it.each([false, true])('shows an explicitly recovered referenced held prompt once with original index and Resume guards (readOnly=%s)', async (readOnly) => {
    const previous = useAppStore.getState()
    const resume = vi.fn(async () => true)
    const image = { id: 'image', name: 'photo.png', type: 'file', mimeType: 'image/png', size: 4, path: '/tmp/photo.png' }
    const row = { role: 'user' as const, content: 'Retry this original input', acpPromptNotSent: true, interrupted: true,
      attachments: [image], markdownStoreFiles: ['/skills/inspect.md'], createdAt: '2026-01-01T00:00:00Z' }
    const chat: CppChat = { id: 'chat-1', title: 'Codex', providerId: 'codex-cli', createdAt: '', updatedAt: '',
      acpSession: { providerId: 'codex-cli', sessionId: 'native-original', running: true, processing: false, lifecycleState: 'ready',
        unsentMessageIndices: [23], recoverableUnsentMessageIndices: [23], queueHeld: false, queuedPrompts: [] } }
    useAppStore.setState((state) => ({ sessions: state.sessions.map((session) => ({ ...session, providerId: 'codex-cli', importedReadOnly: readOnly })),
      messages: { 'chat-1': reconcileCppMessages('chat-1', undefined, [row], true, 23) }, historyStartIndexBySessionId: { 'chat-1': 23 },
      acpBindingBySessionId: { 'chat-1': acpBindingFromCppChat(chat, state.acpBindingBySessionId['chat-1']) }, resumeQueuedAcpPrompts: resume }))
    const host = document.createElement('div'); document.body.append(host); const root = createRoot(host)
    try {
      act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
      expect(host.querySelectorAll('[data-unsent-message-index="23"]')).toHaveLength(1)
      act(() => useAppStore.setState((state) => ({ acpBindingBySessionId: { 'chat-1': acpBindingFromCppChat({ ...chat,
        acpSession: { ...chat.acpSession, queueHeld: true, recoverableUnsentMessageIndices: [], queuedPrompts: [{ id: 'fresh-held', revision: 2,
          existingUserMessageIndex: 23, text: row.content, uamAgentId: 'read-only', attachments: [image], markdownStoreFiles: row.markdownStoreFiles,
          goalMode: false, goalId: '' }] } }, state.acpBindingBySessionId['chat-1']) } })))
      expect(host.querySelectorAll('[data-unsent-message-index="23"]')).toHaveLength(0)
      expect(host.querySelector('article[aria-label="You"]')).toBeNull()
      const queue = host.querySelector('[aria-label="Queued prompt"]')!
      expect(queue.textContent).toContain(row.content)
      expect(host.textContent?.split(row.content)).toHaveLength(2)
      expect(queue.textContent).toContain('photo.png')
      expect(queue.textContent).toContain('inspect.md')
      expect(useAppStore.getState().messages['chat-1'][0].nativeMessageIndex).toBe(23)
      expect(useAppStore.getState().acpBindingBySessionId['chat-1'].queuedPrompts?.[0].uamAgentId).toBe('read-only')
      expect(resume).not.toHaveBeenCalled()
      const button = Array.from(queue.querySelectorAll<HTMLButtonElement>('button')).find((candidate) => candidate.textContent === 'Resume queue')!
      expect(button.disabled).toBe(readOnly)
      await act(async () => button.click())
      if (readOnly) expect(resume).not.toHaveBeenCalled()
      else expect(resume).toHaveBeenCalledWith('chat-1', [{ id: 'fresh-held', revision: 2 }])
      if (!readOnly) {
        // An accepted Resume keeps the same raw reference while asynchronous restart is pending.
        act(() => useAppStore.setState((state) => ({ acpBindingBySessionId: { 'chat-1': {
          ...state.acpBindingBySessionId['chat-1'], queueHeld: false, running: false, lifecycleState: 'starting',
        } } })))
        expect(host.querySelectorAll('[data-unsent-message-index="23"]')).toHaveLength(0)
        expect(host.querySelectorAll('[aria-label="Queued prompt"]')).toHaveLength(1)
        expect(host.textContent?.split(row.content)).toHaveLength(2)
        expect(useAppStore.getState().acpBindingBySessionId['chat-1'].queuedPrompts?.[0]).toMatchObject({
          id: 'fresh-held', revision: 2, existingUserMessageIndex: 23,
        })
        expect(useAppStore.getState().messages['chat-1'][0].acpPromptNotSent).toBe(true)
      }
    } finally { act(() => root.unmount()); host.remove(); useAppStore.setState({ resumeQueuedAcpPrompts: previous.resumeQueuedAcpPrompts }) }
  })

  it('keeps pending direct admission separate from the future queue and restores one Unsent row on failure', () => {
    const row = { role: 'user' as const, content: 'Direct request', acpPromptNotSent: true, interrupted: true,
      createdAt: '2026-01-01T00:00:00Z' }
    const pending = { id: 'direct-receipt', revision: 1, existingUserMessageIndex: 5, text: row.content,
      uamAgentId: 'build', attachments: [], markdownStoreFiles: [], goalMode: false, goalId: '' }
    const initial: CppChat = { id: 'chat-1', title: 'Codex', providerId: 'codex-cli', createdAt: '', updatedAt: '',
      acpSession: { providerId: 'codex-cli', running: false, processing: false, lifecycleState: 'starting',
        unsentMessageIndices: [], recoverableUnsentMessageIndices: [], queueHeld: false,
        queuedPrompts: [], pendingDirectPrompts: [pending] } }
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) => ({ ...session, providerId: 'codex-cli' })),
      messages: { 'chat-1': reconcileCppMessages('chat-1', undefined, [row], true, 5) },
      historyStartIndexBySessionId: { 'chat-1': 5 },
      acpBindingBySessionId: { 'chat-1': acpBindingFromCppChat({ ...initial, acpSession: sanitizeCppAcpSession(initial.acpSession) }, state.acpBindingBySessionId['chat-1']) },
    }))
    const host = document.createElement('div'); document.body.append(host); const root = createRoot(host)
    try {
      act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
      expect(host.querySelector('[aria-label="Queued prompt"]')).toBeNull()
      expect(host.querySelector('[data-message-index="5"]')).not.toBeNull()
      expect(host.textContent?.split(row.content)).toHaveLength(2)
      expect(host.textContent).not.toContain('Message was not sent')
      expect(host.querySelector<HTMLButtonElement>('[aria-label="Select provider and model"]')?.disabled).toBe(true)
      act(() => useAppStore.setState({ messages: { 'chat-1': [] } }))
      expect(host.textContent?.split(row.content)).toHaveLength(2)
      expect(host.textContent).toContain('Sending')
      expect(host.querySelector('[data-queue-id]')).toBeNull()
      expect(host.textContent).not.toContain('Queue held')
      expect(host.querySelector('[aria-label="Retry message"]')).toBeNull()
      // Pending receipts expose no recovery while delivery is unresolved.
      expect(host.querySelector('[aria-label="Remove unsent prompt"]')).toBeNull()
      expect(host.querySelector('[data-unsent-receipt-id="direct-receipt"] article[data-delivery="sending"]')).not.toBeNull()
      act(() => useAppStore.setState((state) => ({ messages: { 'chat-1': reconcileCppMessages('chat-1', undefined, [row], true, 5) },
        acpBindingBySessionId: { 'chat-1': acpBindingFromCppChat({ ...initial,
        acpSession: { ...initial.acpSession, lifecycleState: 'error', queueHeld: true,
          pendingDirectPrompts: [], unsentMessageIndices: [5], recoverableUnsentMessageIndices: [5] },
      }, state.acpBindingBySessionId['chat-1']) } })))
      expect(host.querySelectorAll('[data-unsent-message-index="5"]')).toHaveLength(1)
      expect(host.querySelector('[data-queue-id]')).toBeNull()
      expect(host.textContent).not.toContain('Queue held')
      expect(host.textContent).not.toContain('Message was not sent')
      expect(host.querySelector<HTMLButtonElement>('[aria-label="Retry message"]')?.disabled).toBe(false)
      expect(host.querySelector<HTMLButtonElement>('[aria-label="Remove unsent prompt"]')?.disabled).toBe(false)
    } finally { act(() => root.unmount()); host.remove() }
  })

  it('preserves an unmaterialized cold direct receipt with identity-guarded Delete and disabled Retry', async () => {
    const previousRemove = useAppStore.getState().removeQueuedAcpPrompt
    const remove = vi.fn(async () => true)
    const chat: CppChat = { id: 'chat-1', title: 'Codex', providerId: 'codex-cli', createdAt: '', updatedAt: '',
      acpSession: { providerId: 'codex-cli', running: false, processing: false, lifecycleState: 'stopped',
        queueHeld: false, queuedPrompts: [], pendingDirectPrompts: [], unsentDirectPrompts: [{
          id: 'cold-direct', revision: 3, text: 'Preserve this direct request', uamAgentId: 'build',
          attachments: [], markdownStoreFiles: [], goalMode: false, goalId: '',
        }] } }
    useAppStore.setState((state) => ({ sessions: state.sessions.map((session) => ({ ...session, providerId: 'codex-cli' })),
      messages: { 'chat-1': [] }, removeQueuedAcpPrompt: remove,
      acpBindingBySessionId: { 'chat-1': acpBindingFromCppChat({ ...chat,
        acpSession: sanitizeCppAcpSession(chat.acpSession) }, state.acpBindingBySessionId['chat-1']) } }))
    const host = document.createElement('div'); document.body.append(host); const root = createRoot(host)
    try {
      act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
      expect(host.textContent?.split('Preserve this direct request')).toHaveLength(2)
      expect(host.querySelector('.uam-message-list [data-unsent-receipt-id="cold-direct"] article[data-delivery="unconfirmed"]')).not.toBeNull()
      expect(host.querySelector('[data-queue-id]')).toBeNull()
      expect(host.textContent).not.toContain('Queue held')
      expect(host.querySelector<HTMLButtonElement>('[aria-label="Retry message"]')?.disabled).toBe(true)
      expect(host.querySelector<HTMLButtonElement>('[aria-label="Select provider and model"]')?.disabled).toBe(true)
      const removeButton = host.querySelector<HTMLButtonElement>('[aria-label="Remove unsent prompt"]')!
      expect(removeButton.disabled).toBe(false)
      await act(async () => removeButton.click())
      expect(remove).toHaveBeenCalledWith('chat-1', 0, { id: 'cold-direct', revision: 3 })
    } finally { act(() => root.unmount()); host.remove(); useAppStore.setState({ removeQueuedAcpPrompt: previousRemove }) }
  })

  it('can delete a stopped cold direct receipt while preserving a held future queue', async () => {
    const previousRemove = useAppStore.getState().removeQueuedAcpPrompt
    const remove = vi.fn(async () => true)
    const future = { id: 'real-future', revision: 2, text: 'Future request', uamAgentId: 'build',
      attachments: [], markdownStoreFiles: [], goalMode: false, goalId: '' }
    const chat: CppChat = { id: 'chat-1', title: 'Codex', providerId: 'codex-cli', createdAt: '', updatedAt: '',
      acpSession: { providerId: 'codex-cli', running: false, processing: false, lifecycleState: 'stopped',
        queueHeld: true, queuedPrompts: [future], pendingDirectPrompts: [], unsentDirectPrompts: [{
          id: 'cold-direct', revision: 3, text: 'Failed direct request', uamAgentId: 'build',
          attachments: [], markdownStoreFiles: [], goalMode: false, goalId: '',
        }] } }
    useAppStore.setState((state) => ({ sessions: state.sessions.map((session) => ({ ...session, providerId: 'codex-cli' })),
      messages: { 'chat-1': [] }, removeQueuedAcpPrompt: remove,
      acpBindingBySessionId: { 'chat-1': acpBindingFromCppChat(chat, state.acpBindingBySessionId['chat-1']) } }))
    const host = document.createElement('div'); document.body.append(host); const root = createRoot(host)
    try {
      act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
      const receipt = host.querySelector('[data-unsent-receipt-id="cold-direct"]')!
      const removeButton = receipt.querySelector<HTMLButtonElement>('[aria-label="Remove unsent prompt"]')!
      expect(removeButton.disabled).toBe(false)
      expect(receipt.querySelector<HTMLButtonElement>('[aria-label="Retry message"]')?.disabled).toBe(true)
      expect(host.querySelector('[data-queue-id="real-future"]')?.textContent).toContain('Future request')
      await act(async () => removeButton.click())
      expect(remove).toHaveBeenCalledWith('chat-1', 0, { id: 'cold-direct', revision: 3 })
      expect(useAppStore.getState().acpBindingBySessionId['chat-1'].queuedPrompts).toEqual([future])
      expect(host.querySelector('[data-queue-id="real-future"]')).not.toBeNull()
    } finally { act(() => root.unmount()); host.remove(); useAppStore.setState({ removeQueuedAcpPrompt: previousRemove }) }
  })

  it('does not hide distinct same-text rows or substantive output behind a pending direct reference', () => {
    const rows = reconcileCppMessages('chat-1', undefined, [
      { role: 'user', content: 'Continue', acpPromptNotSent: true, interrupted: true, createdAt: '' },
      { role: 'user', content: 'Continue', acpPromptNotSent: true, interrupted: true, createdAt: '' },
      { role: 'assistant', content: 'Actual response', createdAt: '' },
    ], true, 4)
    const reference = { id: 'direct-receipt', revision: 1, existingUserMessageIndex: 5, text: 'Continue',
      uamAgentId: 'build', attachments: [], markdownStoreFiles: [], goalMode: false, goalId: '' }
    const projection = projectUnsentPrompts(rows, 4, [4], [4], [], [reference])
    expect(projection.prompts.map((prompt) => prompt.messageIndex)).toEqual([4])
    expect(projection.hiddenIndices.has(1)).toBe(false)
    expect(projection.hiddenIndices.has(2)).toBe(false)
  })

  it('preserves a stale receipt with different captured skills as a distinct recovery intent', () => {
    const content = 'Continue'
    const chat: CppChat = { id: 'chat-1', title: 'Codex', providerId: 'codex-cli', createdAt: '', updatedAt: '',
      acpSession: { providerId: 'codex-cli', running: false, processing: false, lifecycleState: 'stopped',
        unsentMessageIndices: [5], recoverableUnsentMessageIndices: [], queuedPrompts: [], pendingDirectPrompts: [],
        unsentDirectPrompts: [{ id: 'stale-direct', revision: 1, existingUserMessageIndex: 5, text: content,
          uamAgentId: 'build', attachments: [], markdownStoreFiles: ['/skills/old.md'], goalMode: false, goalId: '' }] } }
    useAppStore.setState((state) => ({ sessions: state.sessions.map((session) => ({ ...session, providerId: 'codex-cli' })),
      historyStartIndexBySessionId: { 'chat-1': 5 },
      messages: { 'chat-1': reconcileCppMessages('chat-1', undefined, [{ role: 'user', content,
        acpPromptNotSent: true, interrupted: true, markdownStoreFiles: ['/skills/new.md'], createdAt: '' }], true, 5) },
      acpBindingBySessionId: { 'chat-1': acpBindingFromCppChat(chat, state.acpBindingBySessionId['chat-1']) } }))
    const host = document.createElement('div'); document.body.append(host); const root = createRoot(host)
    try {
      act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
      expect(host.querySelectorAll('[data-unsent-message-index="5"]')).toHaveLength(1)
      expect(host.querySelector('[data-unsent-receipt-id="stale-direct"]')).not.toBeNull()
      expect(host.textContent).toContain('new.md')
      expect(host.textContent).toContain('old.md')
      expect(host.querySelector('[data-queue-id]')).toBeNull()
    } finally { act(() => root.unmount()); host.remove() }
  })

  it.each(['index', 'attachment', 'skill', 'unheld', 'prepared'] as const)('keeps a separate legacy recovery input when a real queue reference has mismatched %s', (mismatch) => {
    const image = { id: 'image', name: 'photo.png', type: 'file', mimeType: 'image/png', size: 4, path: '/tmp/photo.png' }
    const messages = reconcileCppMessages('chat-1', undefined, [{ role: 'user', content: 'Original', acpPromptNotSent: true,
      markdownStoreFiles: ['/skills/inspect.md'], attachments: [image], createdAt: '' }], true, 23)
    const queued = { id: 'real-held', revision: 1, existingUserMessageIndex: mismatch === 'index' ? 24 : 23,
      prepared: mismatch === 'prepared', text: 'Original', uamAgentId: 'read-only', attachments: mismatch === 'attachment' ? [] : [image],
      markdownStoreFiles: mismatch === 'skill' ? ['/skills/changed.md'] : ['/skills/inspect.md'], goalMode: false, goalId: '' }
    const projection = projectUnsentPrompts(messages, 23, [23], [], mismatch === 'unheld' ? [] : [queued])
    expect(projection.prompts).toHaveLength(1)
    expect(projection.prompts[0].messageIndex).toBe(23)
    // The recovery input keeps its own transcript slot instead of a separate panel.
    expect(projection.hiddenIndices.has(0)).toBe(false)
  })

  it('shows one combined queued prompt in a constrained composer panel', () => {
    useAppStore.setState((state) => ({
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          queuedPrompts: [
            { text: 'First queued prompt\n\nSecond queued prompt', markdownStoreFiles: ['/tmp/review.uam'], attachments: [], goalMode: false, goalId: '' },
          ],
        },
      },
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    const queue = host.querySelector('[aria-label="Queued prompt"]') as HTMLElement
    expect(queue.textContent?.indexOf('First queued prompt')).toBeLessThan(queue.textContent?.indexOf('Second queued prompt') ?? 0)
    expect(queue.textContent).toContain('review.uam')
    expect(queue.querySelector('[title="/tmp/review.uam"]')).not.toBeNull()
    expect(queue.querySelectorAll('button[aria-label="Steer with this prompt now"]')).toHaveLength(1)
    expect(queue.querySelectorAll('button[aria-label="Remove queued prompt"]')).toHaveLength(1)

    act(() => root.unmount())
    host.remove()
  })

  it('edits and reorders queued prompts without changing the composer draft', async () => {
    const removeQueuedAcpPrompt = vi.fn(async () => true)
    const steerQueuedAcpPrompt = vi.fn(async () => true)
    const editQueuedAcpPrompt = vi.fn(async () => true)
    const reorderQueuedAcpPrompts = vi.fn(async () => true)
    const queued = (id: string) => ({ id, revision: 2, text: `${id} queued`, uamAgentId: '', markdownStoreFiles: [], attachments: id === 'one' ? [
      { id: 'keep', name: 'keep.png', type: 'image' as const, size: 2, path: '/tmp/keep.png' },
      { id: 'remove', name: 'remove.txt', type: 'file' as const, size: 2, path: '/tmp/remove.txt' },
    ] : [], goalMode: false, goalId: '' })
    useAppStore.setState((state) => ({ editQueuedAcpPrompt, reorderQueuedAcpPrompts, removeQueuedAcpPrompt, steerQueuedAcpPrompt,
      acpBindingBySessionId: { ...state.acpBindingBySessionId, 'chat-1': { ...state.acpBindingBySessionId['chat-1'], queuedPrompts: [queued('one'), queued('two')] } } }))
    writeChatComposerDraft('chat-1', { text: 'Unsent composer text', attachments: [{ id: 'draft-file', name: 'draft.png', type: 'image', size: 2, path: '/tmp/draft.png' }] })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    try {
      act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
      const composer = host.querySelector('textarea:not([aria-label="Edit queued prompt text"])') as HTMLTextAreaElement
      const queue = host.querySelector('[aria-label="Queued prompt"]')!
      expect(queue.querySelectorAll('button[aria-label="Edit queued prompt"]')).toHaveLength(2)
      await act(async () => queue.querySelector<HTMLButtonElement>('button[aria-label="Edit queued prompt"]')!.click())
      const editor = queue.querySelector<HTMLTextAreaElement>('textarea')!
      act(() => {
        Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set?.call(editor, 'Updated queue text')
        editor.dispatchEvent(new Event('input', { bubbles: true }))
      })
      await act(async () => queue.querySelector<HTMLButtonElement>('button[aria-label="Remove remove.txt attachment"]')!.click())
      await act(async () => Array.from(queue.querySelectorAll<HTMLButtonElement>('button')).find((entry) => entry.textContent === 'Save')!.click())
      expect(editQueuedAcpPrompt).toHaveBeenCalledWith('chat-1', 'one', 2, 'Updated queue text', ['keep'])
      await act(async () => queue.querySelector<HTMLButtonElement>('button[aria-label="Move queued prompt down"]')!.click())
      expect(reorderQueuedAcpPrompts).toHaveBeenCalledWith('chat-1', [{ id: 'one', revision: 2 }, { id: 'two', revision: 2 }], ['two', 'one'])
      await act(async () => queue.querySelector<HTMLButtonElement>('button[aria-label="Remove queued prompt"]')!.click())
      expect(removeQueuedAcpPrompt).toHaveBeenCalledWith('chat-1', 0, { id: 'one', revision: 2 })
      await act(async () => queue.querySelectorAll<HTMLButtonElement>('button[aria-label="Steer with this prompt now"]')[1].click())
      expect(steerQueuedAcpPrompt).toHaveBeenCalledWith('chat-1', 1, { id: 'two', revision: 2 })
      expect(composer.value).toBe('Unsent composer text')
      expect(host.textContent).toContain('draft.png')
    } finally { act(() => root.unmount()); host.remove() }
  })

  it.each([0, 9950])('copies, edits, and reverts messages with branch-safe requests at history offset %s', async (historyOffset) => {
    const branchFromMessage = vi.fn(() => Promise.resolve('branch-1'))
    useAppStore.setState((state) => ({
      branchFromMessage,
      historyStartIndexBySessionId: { 'chat-1': historyOffset },
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          lifecycleState: 'ready',
          processing: false,
          processingStartedAtMs: null,
          pendingPermission: null,
          pendingUserInput: null,
          turnEvents: [],
        },
      },
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    const copyButton = host.querySelector('button[aria-label="Copy message"]')
    expect(copyButton).toBeNull()
    const editButton = host.querySelector('button[aria-label="Edit message in new branch"]')
    const revertButton = host.querySelector('button[aria-label="Revert to message in new branch"]')
    expect(editButton).toBeTruthy()
    expect(revertButton).toBeTruthy()
    expect(editButton?.parentElement?.classList.contains('conversation-turn__actions')).toBe(true)

    const composer = host.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')
    act(() => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set?.call(composer, 'Unsent draft stays here')
      composer?.dispatchEvent(new Event('input', { bubbles: true }))
    })
    act(() => editButton?.dispatchEvent(new MouseEvent('click', { bubbles: true })))
    const editTextarea = host.querySelector('textarea[aria-label="Edit message"]') as HTMLTextAreaElement | null
    expect(editTextarea).toBe(composer)
    expect(host.querySelectorAll('textarea')).toHaveLength(1)
    expect(editTextarea?.value).toBe('Please inspect the workspace')
    expect(document.activeElement).toBe(composer)
    act(() => composer?.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })))
    expect(composer?.value).toBe('Unsent draft stays here')
    act(() => editButton?.dispatchEvent(new MouseEvent('click', { bubbles: true })))
    act(() => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set?.call(editTextarea, 'Inspect only the tests')
      editTextarea?.dispatchEvent(new Event('input', { bubbles: true }))
    })
    const saveButton = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Save to new branch')
    if (historyOffset === 0) await act(async () => editTextarea?.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true })))
    else await act(async () => saveButton?.dispatchEvent(new MouseEvent('click', { bubbles: true })))
    expect(composer?.value).toBe('Unsent draft stays here')
    expect(branchFromMessage).toHaveBeenCalledWith('chat-1', historyOffset, 'Inspect only the tests', [])

    await act(async () => revertButton?.dispatchEvent(new MouseEvent('click', { bubbles: true })))
    expect(branchFromMessage).toHaveBeenCalledWith('chat-1', historyOffset, undefined)

    act(() => root.unmount())
    host.remove()
  })

  it('edits source attachment retention without mutating the message and retries a failed branch', async () => {
    const attachments = [
      { id: 'photo', name: 'photo.png', type: 'file' as const, mimeType: 'image/png', size: 4 },
      { id: 'second', name: 'second.webp', type: 'file' as const, mimeType: 'image/webp', size: 4 },
    ]
    const branch = vi.fn().mockRejectedValueOnce(new Error('Source image no longer exists.')).mockResolvedValueOnce('branch-1')
    useAppStore.setState((state) => ({
      branchFromMessage: branch,
      messages: { ...state.messages, 'chat-1': [{ ...state.messages['chat-1'][0], content: '', nativeMessageIndex: 7, attachments }] },
      acpBindingBySessionId: { ...state.acpBindingBySessionId, 'chat-1': { ...state.acpBindingBySessionId['chat-1'], processing: false, pendingPermission: null, pendingUserInput: null, turnEvents: [] } },
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    try {
      act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
      const images = host.querySelector('.conversation-image-attachments')!
      const frame = host.querySelector('.conversation-turn')!
      expect(images.compareDocumentPosition(frame) & Node.DOCUMENT_POSITION_FOLLOWING).toBeTruthy()
      expect(images.textContent).toContain('photo.png')
      expect(images.textContent).toContain('second.webp')
      const edit = () => act(() => host.querySelector<HTMLButtonElement>('button[aria-label="Edit message in new branch"]')!.click())
      edit()
      act(() => host.querySelector<HTMLButtonElement>('button[aria-label="Remove photo.png from edited message"]')!.click())
      const save = () => Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Save to new branch')!
      expect(save().disabled).toBe(false)
      await act(async () => save().click())
      expect(branch).toHaveBeenLastCalledWith('chat-1', 0, '', ['second'])
      expect(host.querySelector('textarea[aria-label="Edit message"]')).toBeTruthy()
      expect(host.textContent).toContain('Source image no longer exists.')
      expect(useAppStore.getState().messages['chat-1'][0].attachments).toEqual(attachments)
      await act(async () => save().click())
      expect(host.querySelector('textarea[aria-label="Edit message"]')).toBeNull()
      edit()
      act(() => host.querySelector<HTMLButtonElement>('button[aria-label="Remove photo.png from edited message"]')!.click())
      act(() => Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Cancel')!.click())
      edit()
      expect(host.querySelector('button[aria-label="Remove photo.png from edited message"]')).toBeTruthy()
      act(() => host.querySelector<HTMLButtonElement>('button[aria-label="Remove photo.png from edited message"]')!.click())
      act(() => host.querySelector<HTMLButtonElement>('button[aria-label="Remove second.webp from edited message"]')!.click())
      expect(save().disabled).toBe(true)
    } finally { act(() => root.unmount()); host.remove() }
  })

  it.each(['branch-success', 'branch-failure', 'retry-success', 'retry-failure'] as const)('ignores stale %s feedback after switching away and starting a new edit', async (operation) => {
    let settle!: (value: unknown) => void
    const deferred = new Promise((resolve) => { settle = resolve })
    const retry = operation.startsWith('retry')
    const image = { id: 'image', name: 'keep.png', type: 'file', mimeType: 'image/png', size: 4 }
    useAppStore.setState((state) => ({
      branchFromMessage: vi.fn(async () => await deferred as string | null),
      retryFailedMessage: vi.fn(async () => await deferred as { ok: boolean; error?: string }),
      messages: { ...state.messages, 'chat-1': [{ ...state.messages['chat-1'][0], attachments: [image], ...(retry ? { acpPromptNotSent: true } : {}) }] },
      acpBindingBySessionId: { ...state.acpBindingBySessionId, 'chat-1': { ...state.acpBindingBySessionId['chat-1'], processing: false, pendingPermission: null, pendingUserInput: null, turnEvents: [] } },
    }))
    const sessionA = useAppStore.getState().sessions[0]
    const sessionB = { ...sessionA, id: 'chat-other' }
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    const edit = () => act(() => host.querySelector<HTMLButtonElement>('button[aria-label="Edit message in new branch"]')!.click())
    try {
      act(() => root.render(<ChatView session={sessionA} />))
      if (retry) act(() => host.querySelector<HTMLButtonElement>('button[aria-label="Retry message"]')!.click())
      else {
        edit()
        act(() => Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Save to new branch')!.click())
      }
      act(() => root.render(<ChatView session={sessionB} />))
      act(() => root.render(<ChatView session={sessionA} />))
      edit()
      const textarea = host.querySelector<HTMLTextAreaElement>('textarea[aria-label="Edit message"]')!
      act(() => {
        Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')!.set!.call(textarea, 'New edit must survive')
        textarea.dispatchEvent(new Event('input', { bubbles: true }))
      })
      await act(async () => settle(retry ? { ok: operation.endsWith('success'), error: 'Old retry failure' } : operation.endsWith('success') ? 'old-branch' : null))
      expect(host.querySelector<HTMLTextAreaElement>('textarea[aria-label="Edit message"]')?.value).toBe('New edit must survive')
      expect(host.querySelector('button[aria-label="Remove keep.png from edited message"]')).toBeTruthy()
      expect(host.textContent).not.toContain('Old retry failure')
      expect(host.textContent).not.toContain('Could not create the message branch.')
    } finally { act(() => root.unmount()); host.remove() }
  })

  it('keeps newer staging errors and the draft when an older attachment send fails', async () => {
    let settle!: (ok: boolean) => void
    useAppStore.setState({
      sendAcpPrompt: vi.fn(() => new Promise<boolean>((resolve) => { settle = resolve })),
      stageChatAttachments: vi.fn(async () => { throw new Error('New image could not be staged.') }),
    })
    writeChatComposerDraft('chat-1', { text: 'Keep my text', attachments: [{ id: 'old', name: 'old.png', type: 'file', mimeType: 'image/png', size: 4 }] })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    try {
      act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
      act(() => host.querySelector<HTMLButtonElement>('button[aria-label="Queue prompt"]')!.click())
      const input = host.querySelector<HTMLInputElement>('input[type="file"]')!
      const file = new File(['image'], 'new.png', { type: 'image/png' })
      Object.defineProperty(file, 'path', { value: '/tmp/new.png' })
      Object.defineProperty(input, 'files', { value: [file], configurable: true })
      await act(async () => input.dispatchEvent(new Event('change', { bubbles: true })))
      expect(host.textContent).toContain('New image could not be staged.')
      await act(async () => {
        useAppStore.setState((state) => ({ acpBindingBySessionId: { ...state.acpBindingBySessionId, 'chat-1': { ...state.acpBindingBySessionId['chat-1'], promptActionError: { id: 'attempt', message: 'Provider rejected the old image.' } } } }))
        settle(false)
      })
      expect(host.textContent).toContain('New image could not be staged.')
      expect(host.textContent).toContain('Provider rejected the old image.')
      expect(host.querySelector<HTMLTextAreaElement>('textarea')?.value).toBe('Keep my text')
      expect(host.textContent).toContain('old.png')
      expect(host.textContent).toContain('new.png')
    } finally { act(() => root.unmount()); host.remove() }
  })

  it('uses absolute indices for actions on a paged chat', async () => {
    const branchFromMessage = vi.fn(async () => 'branch-1')
    useAppStore.setState((state) => ({
      branchFromMessage,
      historyStartIndexBySessionId: { 'chat-1': 7 },
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': { ...state.acpBindingBySessionId['chat-1'], lifecycleState: 'ready', processing: false,
          processingStartedAtMs: null, pendingPermission: null, pendingUserInput: null, turnEvents: [] },
      },
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    await act(async () => host.querySelector('button[aria-label="Revert to message in new branch"]')
      ?.dispatchEvent(new MouseEvent('click', { bubbles: true })))
    expect(branchFromMessage).toHaveBeenCalledWith('chat-1', 7, undefined)
    act(() => root.unmount())
    host.remove()
  })

  it.each([
    { notSent: true, interrupted: true, expectedRetry: true },
    { notSent: false, interrupted: true, expectedRetry: true },
    { notSent: false, interrupted: false, expectedRetry: true },
  ])('retries a latest user message without substantive output (notSent=$notSent, interrupted=$interrupted)', async ({ notSent, interrupted, expectedRetry }) => {
    const retryFailedMessage = vi.fn(async () => ({ ok: true }))
    const branchFromMessage = vi.fn(async () => 'branch-1')
    useAppStore.setState((state) => ({
      retryFailedMessage,
      branchFromMessage,
      messages: {
        ...state.messages,
        'chat-1': [{ ...state.messages['chat-1'][0], interrupted, acpPromptNotSent: notSent }],
      },
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': { ...state.acpBindingBySessionId['chat-1'], lifecycleState: 'ready', processing: false,
          pendingPermission: null, pendingUserInput: null, turnEvents: [] },
      },
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    const actionLabel = expectedRetry ? 'Retry message' : 'Revert to message in new branch'
    expect(host.querySelector<HTMLButtonElement>(`button[aria-label="${actionLabel}"]`)).toBeTruthy()
    await act(async () => {
      host.querySelector<HTMLButtonElement>(`button[aria-label="${actionLabel}"]`)!.click()
    })
    if (expectedRetry) {
      expect(retryFailedMessage).toHaveBeenCalledWith('chat-1', 0)
      expect(branchFromMessage).not.toHaveBeenCalled()
    } else {
      expect(branchFromMessage).toHaveBeenCalledWith('chat-1', 0, undefined)
      expect(retryFailedMessage).not.toHaveBeenCalled()
    }

    act(() => root.unmount())
    host.remove()
  })

  it.each([
    { name: 'delivered empty assistant placeholder', assistant: {}, notSent: false, expectedRetry: true },
    { name: 'empty assistant placeholder', assistant: {}, notSent: true, expectedRetry: true },
    { name: 'assistant text', assistant: { content: 'Partial response' }, notSent: true, expectedRetry: false },
    { name: 'assistant thought', assistant: { thoughts: 'Partial thought' }, notSent: true, expectedRetry: false },
    { name: 'assistant tool call', assistant: { toolCalls: [{ id: 'tool-1', title: 'Read', kind: 'read', status: 'completed', content: '' }] }, notSent: true, expectedRetry: false },
    { name: 'assistant plan', assistant: { planEntries: [{ content: 'Inspect', priority: 'high', status: 'pending' }] }, notSent: true, expectedRetry: false },
    { name: 'assistant block', assistant: { blocks: [{ type: 'assistant_text' as const, text: 'Partial response' }] }, notSent: true, expectedRetry: false },
    { name: 'assistant attachment', assistant: { attachments: [{ id: 'attachment-1', name: 'image.png', type: 'image/png', size: 1 }] }, notSent: true, expectedRetry: false },
  ])('retries in place only when trailing output is empty ($name)', async ({ assistant, notSent, expectedRetry }) => {
    const retryFailedMessage = vi.fn(async () => ({ ok: true }))
    const branchFromMessage = vi.fn(async () => 'branch-1')
    useAppStore.setState((state) => ({
      retryFailedMessage,
      branchFromMessage,
      messages: {
        ...state.messages,
        'chat-1': [
          { ...state.messages['chat-1'][0], interrupted: true, acpPromptNotSent: notSent },
          {
            id: 'm-2',
            sessionId: 'chat-1',
            role: 'assistant',
            content: '',
            createdAt: new Date('2026-01-01T00:00:01.000Z'),
            ...assistant,
          },
        ],
      },
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': { ...state.acpBindingBySessionId['chat-1'], lifecycleState: 'ready', processing: false, pendingPermission: null, pendingUserInput: null, turnEvents: [] },
      },
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    const actionLabel = expectedRetry ? 'Retry message' : 'Revert to message in new branch'
    expect(host.querySelector<HTMLButtonElement>(`button[aria-label="${actionLabel}"]`)).toBeTruthy()
    expect(host.textContent).toContain(notSent ? 'Message was not sent' : 'Response interrupted')
    await act(async () => {
      host.querySelector<HTMLButtonElement>(`button[aria-label="${actionLabel}"]`)!.click()
    })
    if (expectedRetry) {
      expect(retryFailedMessage).toHaveBeenCalledWith('chat-1', 0)
      expect(branchFromMessage).not.toHaveBeenCalled()
    } else {
      expect(branchFromMessage).toHaveBeenCalledWith('chat-1', 0, undefined)
      expect(retryFailedMessage).not.toHaveBeenCalled()
    }

    act(() => root.unmount())
    host.remove()
  })

  it('does not rescan hidden retry output while typing and refreshes eligibility on history replacement', () => {
    const hiddenWhitespace = ' \t'.repeat(4096)
    const user = { ...useAppStore.getState().messages['chat-1'][0], interrupted: true }
    const assistant: Message = { id: 'hidden-output', sessionId: 'chat-1', role: 'assistant', content: hiddenWhitespace, createdAt: new Date() }
    const history = [user, assistant, ...Array.from({ length: 512 }, (_, index) => ({ ...assistant, id: `empty-${index}`, content: '' }))]
    useAppStore.setState((state) => ({ messages: { ...state.messages, 'chat-1': history } }))
    const originalTrim = String.prototype.trim
    let hiddenOutputChecks = 0
    const trim = vi.spyOn(String.prototype, 'trim').mockImplementation(function (this: string) {
      if (String(this) === hiddenWhitespace) hiddenOutputChecks += 1
      return originalTrim.call(this)
    })
    const host = document.createElement('div'); document.body.appendChild(host); const root = createRoot(host)
    try {
      act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
      expect(host.querySelector('[data-message-index="1"]')).toBeNull()
      expect(hiddenOutputChecks).toBeGreaterThan(0)
      const initialChecks = hiddenOutputChecks
      const composer = host.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')!
      for (const text of ['a', 'ab', 'abc']) {
        act(() => {
          Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')!.set!.call(composer, text)
          composer.dispatchEvent(new Event('input', { bubbles: true }))
        })
        expect(composer.value).toBe(text)
      }
      expect(hiddenOutputChecks).toBe(initialChecks)
      act(() => useAppStore.setState((state) => ({ messages: { ...state.messages, 'chat-1': [user, { ...assistant, content: '' }] } })))
      expect(host.querySelector('button[aria-label="Retry message"]')).toBeTruthy()
      act(() => useAppStore.setState((state) => ({ messages: { ...state.messages, 'chat-1': [user, { ...assistant, content: 'Completed output' }] } })))
      expect(host.querySelector('button[aria-label="Retry message"]')).toBeNull()
      expect(host.querySelector('button[aria-label="Revert to message in new branch"]')).toBeTruthy()
    } finally { trim.mockRestore(); act(() => root.unmount()); host.remove() }
  })

  it('branches an edited message even when its response is empty', async () => {
    const branchFromMessage = vi.fn(async () => 'branch-1')
    const retryFailedMessage = vi.fn(async () => ({ ok: true }))
    useAppStore.setState((state) => ({
      branchFromMessage,
      retryFailedMessage,
      messages: {
        ...state.messages,
        'chat-1': [
          { ...state.messages['chat-1'][0], interrupted: true, acpPromptNotSent: false },
          { id: 'm-2', sessionId: 'chat-1', role: 'assistant', content: '', createdAt: new Date('2026-01-01T00:00:01.000Z') },
        ],
      },
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': { ...state.acpBindingBySessionId['chat-1'], lifecycleState: 'ready', processing: false, pendingPermission: null, pendingUserInput: null, turnEvents: [] },
      },
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    const editButton = host.querySelector<HTMLButtonElement>('button[aria-label="Edit message in new branch"]')!
    act(() => editButton.click())
    const editTextarea = host.querySelector<HTMLTextAreaElement>('textarea[aria-label="Edit message"]')!
    act(() => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set?.call(editTextarea, 'Edited prompt')
      editTextarea.dispatchEvent(new Event('input', { bubbles: true }))
    })
    await act(async () => {
      Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Save to new branch')?.click()
    })
    expect(branchFromMessage).toHaveBeenCalledWith('chat-1', 0, 'Edited prompt', [])
    expect(retryFailedMessage).not.toHaveBeenCalled()
    act(() => root.unmount())
    host.remove()
  })

  it('marks the edited or reverted branch point', () => {
    const session = { ...useAppStore.getState().sessions[0], branchFromMessageIndex: 0, branchMessageEdited: true }
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={session} />))
    expect(host.textContent).toContain('Edited branch')
    act(() => root.render(<ChatView session={{ ...session, branchMessageEdited: false }} />))
    expect(host.textContent).toContain('Reverted branch')
    act(() => root.unmount())
    host.remove()
  })

  it('uses the composer action as Send when the runtime is idle', () => {
    useAppStore.setState((state) => ({
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          lifecycleState: 'ready',
          processing: false,
          pendingPermission: null,
          turnEvents: [],
        },
      },
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    const sendButton = Array.from(host.querySelectorAll('button')).find((button) => button.title === 'Send prompt') as HTMLButtonElement
    expect(sendButton).toBeTruthy()
    expect(Array.from(host.querySelectorAll('button')).some((button) => button.title === 'Cancel turn' && button.textContent === 'Stop')).toBe(false)

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('renders ACP messages and resolves permission choices', () => {
    const resolveAcpPermission = vi.fn(() => Promise.resolve(true))
    useAppStore.setState({ resolveAcpPermission })

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    expect(host.textContent).toContain('Before tool.')
    expect(host.textContent).toContain('const ok = true')
    expect(host.textContent).not.toContain('Tool call:')
    expect(host.textContent).toContain('Search symbols')
    expect(host.textContent).toContain('Thoughts')
    expect(host.textContent).toContain('Need to inspect the workspace first.')
    expect(host.textContent).not.toContain('Persisted thought should not duplicate while turn events are active.')
    expect(host.textContent).toContain('After tool.')
    expect(host.textContent).toContain('Gemini')
    expect(host.textContent).not.toContain('ACP')
    expect(host.querySelector('button[aria-label="Select provider and model"]')).toBeTruthy()
    expect(host.querySelector('.uam-workspace-row')).toBeNull()
    expect(host.textContent).not.toContain('Tools on')
    expect(host.textContent).toContain('Read file')

    const streamText = host.textContent ?? ''
    expect(streamText.indexOf('Before tool.')).toBeLessThan(streamText.indexOf('Search symbols'))
    expect(streamText.indexOf('Before tool.')).toBeLessThan(streamText.indexOf('Thoughts'))
    expect(streamText.indexOf('Thoughts')).toBeLessThan(streamText.indexOf('Search symbols'))
    expect(streamText.indexOf('Search symbols')).toBeLessThan(streamText.indexOf('Read file'))
    expect(streamText.indexOf('Read file')).toBeLessThan(streamText.indexOf('After tool.'))
    const thinkingBlock = host.querySelector('[data-testid="thinking-block"]') as HTMLDetailsElement | null
    expect(host.querySelectorAll('[data-testid="thinking-block"]')).toHaveLength(1)
    expect(thinkingBlock?.tagName).toBe('DETAILS')
    expect(thinkingBlock?.textContent).toContain('Thoughts')
    expect(thinkingBlock?.textContent).toContain('Need to inspect the workspace first.')
    expect(thinkingBlock?.hasAttribute('open')).toBe(false)
    expect(thinkingBlock?.dataset.active).toBe('false')
    expect(host.querySelector('.uam-tool-row')?.getAttribute('data-active')).toBe('true')
    expect(host.querySelector('.uam-tool-row__kind')?.textContent).toBe('Tool')
    expect(host.querySelector('.conversation-turn[data-streaming="true"]')).not.toBeNull()
    expect(host.querySelector('.uam-runtime-status')).toBeNull()
    expect((host.querySelector('button[aria-label="Select provider and model"]') as HTMLButtonElement).title).toContain('Permission')

    openComposerOptions(host)
    expect(document.body.textContent).toContain('Token budget')
    expect(host.textContent).not.toContain('Unavailable')

    const toolButton = Array.from(host.querySelectorAll('button')).find((button) =>
      button.textContent?.includes('Search symbols')
    )
    expect(toolButton).toBeTruthy()
    act(() => {
      toolButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })
    expect(document.body.textContent).toContain('Searching workspace symbols')
    expect(document.body.querySelector('[role="dialog"]')?.parentElement?.className).toContain('tool-details-backdrop')
    expect(document.body.querySelector('[role="dialog"]')?.parentElement?.parentElement).toBe(document.body)

    const closeToolButton = document.body.querySelector('button[aria-label="Close tool details"]')
    expect(closeToolButton).toBeTruthy()
    act(() => {
      closeToolButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })
    expect(document.body.textContent).not.toContain('Searching workspace symbols')

    const allowButton = Array.from(host.querySelectorAll('button')).find((button) =>
      button.textContent?.includes('Allow once')
    )
    expect(allowButton).toBeTruthy()
    expect(host.querySelector('[role="alert"]')?.textContent).toContain('Command safety warning (low tier)')

    act(() => {
      allowButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(resolveAcpPermission).toHaveBeenCalledWith('chat-1', '5', 'allow-once')

    act(() => {
      useAppStore.setState((state) => ({
        acpBindingBySessionId: {
          ...state.acpBindingBySessionId,
          'chat-1': {
            ...state.acpBindingBySessionId['chat-1'],
            lifecycleState: 'ready',
            processing: false,
            pendingPermission: null,
          },
        },
      }))
    })
    expect(host.textContent).not.toContain('Working')
    expect(host.querySelector('button[title="Cancel turn"]')).toBeNull()

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('deduplicates permission actions and clarifies matching labels', () => {
    const resolveAcpPermission = vi.fn(() => Promise.resolve(true))
    useAppStore.setState((state) => ({
      resolveAcpPermission,
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          pendingPermission: {
            requestId: '5',
            toolCallId: 'tool-1',
            title: 'Command approval',
            kind: 'commandExecution',
            status: 'pending',
            content: 'npm test',
            options: [
              { id: 'accept', name: 'Allow', kind: 'decision' },
              { id: 'accept', name: 'Allow', kind: 'decision' },
              { id: 'acceptForSession', name: 'Allow', kind: 'decision' },
              { id: 'decline', name: 'Deny', kind: 'decision' },
              { id: 'cancelled', name: 'Cancel', kind: 'cancel' },
              { id: 'cancelled', name: 'Cancel', kind: 'cancel' },
              { id: 'cancel', name: 'Cancel', kind: 'cancel' },
            ],
          },
        },
      },
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    const buttonTexts = Array.from(host.querySelectorAll('button')).map((button) => button.textContent?.trim() ?? '')
    expect(buttonTexts.filter((text) => text === 'Cancel')).toHaveLength(1)
    expect(buttonTexts.filter((text) => text === 'Allow')).toHaveLength(0)
    expect(buttonTexts).toContain('Allow (accept)')
    expect(buttonTexts).toContain('Allow (acceptForSession)')
    expect(buttonTexts).toContain('Deny')

    const cancelButton = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Cancel')
    expect(cancelButton).toBeTruthy()
    act(() => {
      cancelButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })
    expect(resolveAcpPermission).toHaveBeenCalledWith('chat-1', '5', 'cancelled')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('submits one permission response at a time and retries a rejected response', async () => {
    let finishResponse: ((accepted: boolean) => void) | undefined
    const resolveAcpPermission = vi.fn(() => new Promise<boolean>((resolve) => { finishResponse = resolve }))
    useAppStore.setState({ resolveAcpPermission })

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    const allowButton = Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent?.includes('Allow once'))
    const denyButton = Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Cancel')
    expect(allowButton).toBeTruthy()
    expect(denyButton).toBeTruthy()

    act(() => {
      allowButton?.click()
      denyButton?.click()
    })
    expect(resolveAcpPermission).toHaveBeenCalledTimes(1)
    expect(resolveAcpPermission).toHaveBeenCalledWith('chat-1', '5', 'allow-once')
    expect(allowButton?.disabled).toBe(true)
    expect(denyButton?.disabled).toBe(true)

    await act(async () => {
      finishResponse?.(false)
      await Promise.resolve()
    })
    expect(Array.from(host.querySelectorAll('[role="alert"]')).some((alert) => alert.textContent?.includes('did not accept'))).toBe(true)
    expect(allowButton?.disabled).toBe(false)

    resolveAcpPermission.mockResolvedValueOnce(true)
    await act(async () => {
      allowButton?.click()
      await Promise.resolve()
    })
    expect(resolveAcpPermission).toHaveBeenCalledTimes(2)
    expect(allowButton?.disabled).toBe(true)

    act(() => root.unmount())
    host.remove()
  })

  it('flags stale permission waits and exposes recovery actions', async () => {
    const cancelAcpTurn = vi.fn(() => Promise.resolve(true))
    const stopAcpSession = vi.fn(() => Promise.resolve(true))
    useAppStore.setState((state) => ({
      cancelAcpTurn,
      stopAcpSession,
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          waitIsStale: true,
          waitStaleReason: 'No runtime activity while waiting for command or tool approval.',
          waitSeconds: 143,
        },
      },
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    expect(host.querySelector('[data-testid="stale-wait-warning"]')).toBeTruthy()
    expect(host.textContent).toContain('This approval has not had runtime activity for 143s.')
    expect(host.textContent).toContain('No runtime activity while waiting for command or tool approval.')

    const cancelButton = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Cancel turn')
    const stopButton = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Stop runtime')
    expect(cancelButton).toBeTruthy()
    expect(stopButton).toBeTruthy()

    await act(async () => {
      cancelButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
      await Promise.resolve()
    })
    expect(cancelAcpTurn).toHaveBeenCalledWith('chat-1')

    await act(async () => {
      stopButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
      await Promise.resolve()
    })
    expect(stopAcpSession).toHaveBeenCalledWith('chat-1')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('renders Codex user-input questions and submits answers', async () => {
    const resolveAcpUserInput = vi.fn(() => Promise.resolve(true))
    useAppStore.setState((state) => ({
      resolveAcpUserInput,
      sessions: state.sessions.map((session) =>
        session.id === 'chat-1' ? { ...session, providerId: 'codex-cli' } : session
      ),
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          providerId: 'codex-cli',
          protocolKind: 'codex-app-server',
          lifecycleState: 'waitingUserInput',
          processing: true,
          turnEvents: [
            { type: 'assistant_text', text: 'I need one detail.' },
            { type: 'user_input_request', requestId: '11', toolCallId: 'input-1' },
          ],
          pendingPermission: null,
          pendingUserInput: {
            requestId: '11',
            itemId: 'input-1',
            status: 'pending',
            questions: [
              {
                id: 'scope',
                header: 'Scope',
                question: 'Which scope?',
                isOther: false,
                isSecret: false,
                options: [
                  { label: 'Focused', description: 'Only the bug' },
                  { label: 'Broad', description: 'Include cleanup' },
                ],
              },
              {
                id: 'note',
                header: 'Note',
                question: 'Any extra detail?',
                isOther: true,
                isSecret: false,
                options: [],
              },
            ],
          },
        },
      },
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    expect(host.querySelector('.uam-composer-region .qr-panel')).toBeTruthy()
    expect(host.querySelectorAll('.qr-panel')).toHaveLength(1)
    expect(host.querySelector('.qr-panel')?.nextElementSibling?.classList.contains('uam-composer-surface')).toBe(true)
    expect(document.querySelector('[aria-modal="true"]')).toBeNull()
    const dialog = document.body.querySelector<HTMLElement>('.qr-panel')!
    expect(dialog.textContent).toContain('Which scope?')
    expect(dialog.textContent).toContain('Focused')
    expect(dialog.textContent).not.toContain('Any extra detail?')
    act(() => dialog.querySelector<HTMLInputElement>('input[type="radio"]')!.click())
    act(() => Array.from(dialog.querySelectorAll('button')).find((button) => button.textContent === 'Next')!.click())
    const noteInput = dialog.querySelector<HTMLInputElement>('input[aria-label="Any extra detail?"]')!
    expect(noteInput).toBeTruthy()
    act(() => {
      Object.getOwnPropertyDescriptor(window.HTMLInputElement.prototype, 'value')!.set!.call(noteInput, 'Extra context')
      noteInput.dispatchEvent(new Event('input', { bubbles: true }))
    })
    const submitButton = Array.from(dialog.querySelectorAll('button')).find((button) => button.textContent === 'Submit answers')!
    expect(submitButton.disabled).toBe(false)
    await act(async () => { submitButton.click(); await Promise.resolve() })

    expect(resolveAcpUserInput).toHaveBeenCalledWith('chat-1', '11', {
      scope: ['Focused'],
      note: ['Extra context'],
    })

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('shows MCP questions and goal approval in the chat and returns the chosen answer', async () => {
    const resolveAcpUserInput = vi.fn(() => Promise.resolve(true))
    const pendingInput = (requestId: string, id: string, question: string, options: string[], isOther: boolean) => ({
      requestId, itemId: '', status: 'pending', questions: [{ id, header: id === 'goalCreate' ? 'Goal' : 'Question',
        question, isOther, isSecret: false, options: options.map((label) => ({ label, description: '' })) }],
    })
    useAppStore.setState((state) => ({
      resolveAcpUserInput,
      acpBindingBySessionId: { ...state.acpBindingBySessionId,
        'chat-1': { ...state.acpBindingBySessionId['chat-1'], processing: true, lifecycleState: 'waitingUserInput',
          turnEvents: [], pendingUserInput: pendingInput('uam-control:question-1', 'userQuestion',
            'Which browser should I use?', ['Firefox', 'Safari'], true) },
      },
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    let dialog = document.body.querySelector<HTMLElement>('.qr-panel')!
    expect(dialog.textContent).toContain('Which browser should I use?')
    act(() => dialog.querySelector<HTMLInputElement>('input[type="radio"]')!.click())
    await act(async () => { Array.from(dialog.querySelectorAll('button')).find((button) => button.textContent === 'Submit answers')!.click(); await Promise.resolve() })
    expect(resolveAcpUserInput).toHaveBeenCalledWith('chat-1', 'uam-control:question-1', { userQuestion: ['Firefox'] })

    act(() => useAppStore.setState((state) => ({ acpBindingBySessionId: { ...state.acpBindingBySessionId,
      'chat-1': { ...state.acpBindingBySessionId['chat-1'], processing: false, lifecycleState: 'ready', pendingUserInput: pendingInput(
        'uam-control:goal-1', 'goalCreate', 'Allow OpenCode to create this goal?', ['Allow', 'Deny'], false) },
    } })))
    dialog = document.body.querySelector<HTMLElement>('.qr-panel')!
    expect(dialog.textContent).toContain('Allow OpenCode to create this goal?')
    act(() => dialog.querySelector<HTMLInputElement>('input[type="radio"]')!.click())
    await act(async () => { Array.from(dialog.querySelectorAll('button')).find((button) => button.textContent === 'Submit answers')!.click(); await Promise.resolve() })
    expect(resolveAcpUserInput).toHaveBeenCalledWith('chat-1', 'uam-control:goal-1', { goalCreate: ['Allow'] })
    act(() => root.unmount())
    host.remove()
  })

  it('locks Codex user input while submitting and permits retry after failure', async () => {
    let finishResponse: ((accepted: boolean) => void) | undefined
    const resolveAcpUserInput = vi.fn(() => new Promise<boolean>((resolve) => { finishResponse = resolve }))
    useAppStore.setState((state) => ({
      resolveAcpUserInput,
      sessions: state.sessions.map((session) => session.id === 'chat-1' ? { ...session, providerId: 'codex-cli' } : session),
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          providerId: 'codex-cli',
          protocolKind: 'codex-app-server',
          lifecycleState: 'waitingUserInput',
          processing: true,
          pendingPermission: null,
          pendingUserInput: {
            requestId: 'input-request',
            itemId: 'input-item',
            status: 'pending',
            questions: [{ id: 'scope', header: 'Scope', question: 'Which scope?', isOther: false, isSecret: false, options: [{ label: 'Focused', description: '' }] }],
          },
        },
      },
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    const dialog = document.body.querySelector<HTMLElement>('.qr-panel')!
    const answerButton = dialog.querySelector<HTMLInputElement>('input[type="radio"]')!
    act(() => answerButton.click())
    const submitButton = Array.from(dialog.querySelectorAll('button')).find((button) => button.textContent === 'Submit answers')!
    expect(submitButton.disabled).toBe(false)

    act(() => { submitButton.click(); submitButton.click() })
    expect(resolveAcpUserInput).toHaveBeenCalledTimes(1)
    expect(answerButton.closest('fieldset')?.disabled).toBe(true)
    expect(submitButton.disabled).toBe(true)

    await act(async () => { finishResponse?.(false); await Promise.resolve() })
    expect(dialog.querySelector('[role="alert"]')?.textContent).toContain('did not accept')
    expect(submitButton.disabled).toBe(false)

    resolveAcpUserInput.mockResolvedValueOnce(true)
    await act(async () => { submitButton.click(); await Promise.resolve() })
    expect(resolveAcpUserInput).toHaveBeenCalledTimes(2)
    expect(document.body.querySelector('.qr-panel fieldset')).toBeNull()
    expect(Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Answers submitted')?.disabled).toBe(true)

    act(() => root.unmount())
    host.remove()
  })

  it('renders Codex provider labels from backend provider metadata', () => {
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) =>
        session.id === 'chat-1' ? { ...session, providerId: 'codex-cli', modelId: '' } : session
      ),
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          providerId: 'codex-cli',
          protocolKind: 'codex-app-server',
          threadId: 'thread-1',
          sessionId: 'thread-1',
          availableModels: [],
          currentModelId: '',
          agentInfo: { name: 'codex', title: 'Codex', version: '1.0.0' },
        },
      },
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    expect(host.textContent).toContain('Codex')
    expect(host.textContent).not.toContain('App Server')
    expect(host.querySelector('button[aria-label="Select provider and model"]')?.textContent).toContain('Default')
    expect(host.querySelector('textarea')?.getAttribute('placeholder')).toBe('Message Codex · / for commands')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('keeps the provider label captured on a historical assistant message', () => {
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) =>
        session.id === 'chat-1' ? { ...session, providerId: 'codex-cli' } : session
      ),
      messages: {
        'chat-1': [{
          id: 'historical-gemini',
          sessionId: 'chat-1',
          role: 'assistant',
          content: 'Historical Gemini answer',
          providerId: 'gemini-cli',
          createdAt: new Date('2026-01-01T00:00:01.000Z'),
        }],
      },
      acpBindingBySessionId: {},
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    const messageFrame = Array.from(host.querySelectorAll('article'))
      .find((article) => article.textContent?.includes('Historical Gemini answer'))
    expect(messageFrame?.textContent).toContain('Gemini')
    expect(messageFrame?.textContent).not.toContain('Codex')

    act(() => root.unmount())
    host.remove()
  })

  it('shares one live work section across native steering and keeps its collapse choice after completion', () => {
    const createdAt = new Date('2026-01-01T00:00:00Z')
    useAppStore.setState((state) => ({
      workingDisplayMode: 'compact', expandWorkTraces: true,
      messages: { ...state.messages, 'chat-1': [
        { id: 'original-user', sessionId: 'chat-1', role: 'user', content: 'Review recovery', createdAt },
        { id: 'before-steer', sessionId: 'chat-1', role: 'assistant', content: 'Before steering', thoughts: 'Earlier reasoning', createdAt },
        { id: 'native-steer', sessionId: 'chat-1', role: 'user', content: 'Include SSH', continuesTurn: true, prioritySteer: true, createdAt },
        { id: 'after-steer', sessionId: 'chat-1', role: 'assistant', content: 'After steering', thoughts: 'Later reasoning', createdAt },
      ] },
      acpBindingBySessionId: { ...state.acpBindingBySessionId, 'chat-1': {
        ...state.acpBindingBySessionId['chat-1'], processing: true, turnSerial: 4,
        turnUserMessageIndex: 0, turnAssistantMessageIndex: 3,
        turnEvents: [{ type: 'assistant_text', text: 'After steering' }, { type: 'thought', text: 'Later reasoning' }],
      } },
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    expect(host.querySelectorAll('time.conversation-turn-start')).toHaveLength(1)
    expect(host.querySelectorAll('[data-testid="working-summary"]')).toHaveLength(1)
    expect(host.querySelector('[data-testid="working-summary"]')?.textContent).toContain('Working ')
    expect(host.querySelectorAll('article.conversation-turn--user')).toHaveLength(2)
    expect(host.querySelector('[aria-label="Collapse work trace"]')).toBeNull()
    expect(host.textContent).toContain('Earlier reasoning')
    expect(host.textContent).toContain('Later reasoning')
    expect(host.textContent).toContain('Before steering')
    expect(host.textContent).toContain('After steering')
    expect(host.textContent).toContain('Include SSH')
    act(() => useAppStore.setState((state) => ({
      acpBindingBySessionId: {},
      messages: { ...state.messages, 'chat-1': [...state.messages['chat-1'],
        { id: 'next-user', sessionId: 'chat-1', role: 'user', content: 'New request', prioritySteer: true, createdAt },
        { id: 'next-answer', sessionId: 'chat-1', role: 'assistant', content: 'Next answer', thoughts: 'New section reasoning', createdAt },
      ] },
    })))
    act(() => host.querySelector<HTMLButtonElement>('[aria-label="Collapse work trace"]')!.click())
    expect(host.querySelectorAll('time.conversation-turn-start')).toHaveLength(2)
    expect(host.querySelectorAll('[data-testid="working-summary"]')).toHaveLength(2)
    expect(host.textContent).not.toContain('Earlier reasoning')
    expect(host.textContent).not.toContain('Later reasoning')
    expect(host.textContent).not.toContain('Before steering')
    expect(host.textContent).not.toContain('Include SSH')
    expect(host.textContent).toContain('New section reasoning')
    expect(host.textContent).toContain('After steering')
    act(() => host.querySelector<HTMLButtonElement>('[aria-label="Expand work trace"]')!.click())
    expect(host.textContent).toContain('Earlier reasoning')
    expect(host.textContent).toContain('Later reasoning')
    act(() => root.unmount())
    host.remove()
  })

  it('keeps work sections expanded when section collapsing is disabled', () => {
    const createdAt = new Date('2026-01-01T00:00:00Z')
    useAppStore.setState((state) => ({
      collapsibleWorkSections: false,
      expandWorkTraces: false,
      messages: { ...state.messages, 'chat-1': [
        { id: 'user', sessionId: 'chat-1', role: 'user', content: 'Original request', createdAt },
        { id: 'first-answer', sessionId: 'chat-1', role: 'assistant', content: 'Earlier answer', createdAt },
        { id: 'steer', sessionId: 'chat-1', role: 'user', content: 'Steer request', continuesTurn: true, prioritySteer: true, createdAt },
        { id: 'last-answer', sessionId: 'chat-1', role: 'assistant', content: 'Final answer', createdAt },
      ] },
      acpBindingBySessionId: {},
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    expect(host.querySelector('[aria-label="Collapse work trace"]')).toBeNull()
    expect(host.textContent).toContain('Earlier answer')
    expect(host.textContent).toContain('Steer request')
    expect(host.textContent).toContain('Final answer')

    act(() => root.unmount())
    host.remove()
  })

  it('keeps steering and earlier output visible while a turn is still running', () => {
    const createdAt = new Date('2026-01-01T00:00:00Z')
    useAppStore.setState((state) => ({
      expandWorkTraces: false,
      messages: { ...state.messages, 'chat-1': [
        { id: 'user', sessionId: 'chat-1', role: 'user', content: 'Original request', createdAt },
        { id: 'first-answer', sessionId: 'chat-1', role: 'assistant', content: 'Earlier answer', createdAt },
        { id: 'steer', sessionId: 'chat-1', role: 'user', content: 'Steer request', continuesTurn: true, prioritySteer: true, createdAt },
        { id: 'last-answer', sessionId: 'chat-1', role: 'assistant', content: 'Final answer', createdAt },
      ] },
      acpBindingBySessionId: { 'chat-1': { ...state.acpBindingBySessionId['chat-1'], processing: true, turnUserMessageIndex: 0, turnAssistantMessageIndex: 3 } },
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    expect(host.textContent).toContain('Earlier answer')
    expect(host.textContent).toContain('Steer request')
    act(() => useAppStore.setState({ acpBindingBySessionId: {} }))
    expect(host.textContent).not.toContain('Earlier answer')
    expect(host.textContent).not.toContain('Steer request')
    expect(host.textContent).toContain('Final answer')
    act(() => root.unmount())
    host.remove()
  })

	it('keeps a steer visibly tied to the response it interrupted', () => {
		useAppStore.setState((state) => ({
			workingDisplayMode: 'compact',
			messages: {
				...state.messages,
				'chat-1': [
					{
						id: 'interrupted-answer', sessionId: 'chat-1', role: 'assistant',
						content: 'I was changing the remote helper.', thoughts: 'Checking the helper.', interrupted: true,
						createdAt: new Date('2026-01-01T00:00:01.000Z'),
					},
					{
						id: 'steer', sessionId: 'chat-1', role: 'user',
						content: 'Keep the work local.', prioritySteer: true,
						attachments: [{ id: 'steer-file', name: 'scope.txt', type: 'file', size: 5, path: '/tmp/scope.txt' }],
						createdAt: new Date('2026-01-01T00:00:02.000Z'),
					},
				],
			},
			acpBindingBySessionId: {},
		}))

		const host = document.createElement('div')
		document.body.appendChild(host)
		const root = createRoot(host)
		act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

		expect(host.textContent).toContain('Response interrupted')
		const steer = host.querySelector('article.conversation-turn--user')
		expect(steer?.textContent).toContain('Keep the work local.')
		expect(steer?.textContent).toContain('scope.txt')
		expect(steer?.querySelector('[data-detail="/tmp/scope.txt"]')).not.toBeNull()
		expect(steer?.querySelector('[aria-label="Copy message"]')).toBeNull()
		expect(steer?.querySelector('[aria-label="Edit message in new branch"]')).toBeTruthy()
		const text = host.textContent ?? ''
		expect(text.indexOf('I was changing the remote helper.')).toBeLessThan(text.indexOf('Response interrupted'))
		expect(text.indexOf('Response interrupted')).toBeLessThan(text.indexOf('Keep the work local.'))

		act(() => root.unmount())
		host.remove()
	})

	it('keeps a live tool-first response visible after its normal steer message', () => {
		useAppStore.setState((state) => ({
			workingDisplayMode: 'compact',
			messages: {
				...state.messages,
				'chat-1': [
					{
						id: 'interrupted-answer', sessionId: 'chat-1', role: 'assistant',
						content: 'Earlier response.', thoughts: 'Earlier work.', interrupted: true,
						createdAt: new Date('2026-01-01T00:00:01.000Z'),
					},
					{
						id: 'steer', sessionId: 'chat-1', role: 'user', content: 'Keep it local.', prioritySteer: true,
						createdAt: new Date('2026-01-01T00:00:02.000Z'),
					},
					{
						id: 'live-answer', sessionId: 'chat-1', role: 'assistant', content: '', isStreaming: true,
						createdAt: new Date('2026-01-01T00:00:03.000Z'),
					},
				],
			},
			acpBindingBySessionId: {
				...state.acpBindingBySessionId,
				'chat-1': {
					...state.acpBindingBySessionId['chat-1'],
					processing: true,
					pendingPermission: null,
					turnEvents: [{ type: 'thought', text: 'Current live reasoning.' }],
					turnUserMessageIndex: 1,
					turnAssistantMessageIndex: 2,
					turnSerial: 2,
				},
			},
		}))

		const host = document.createElement('div')
		document.body.appendChild(host)
		const root = createRoot(host)
		act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

		expect(Array.from(host.querySelectorAll('article')).filter((article) =>
			article.textContent?.includes('Current live reasoning.')
		)).toHaveLength(1)
		const steer = host.querySelector('article.conversation-turn--user')
		expect(steer?.textContent).toContain('Keep it local.')
		expect(host.textContent?.match(/Keep it local\./g)).toHaveLength(1)
		expect(host.textContent!.indexOf('Response interrupted')).toBeLessThan(host.textContent!.indexOf('Keep it local.'))
		expect(host.textContent!.indexOf('Keep it local.')).toBeLessThan(host.textContent!.indexOf('Current live reasoning.'))

		act(() => root.unmount())
		host.remove()
	})

	it('renders an attachment-only steer as a normal user message', () => {
		useAppStore.setState((state) => ({
			workingDisplayMode: 'compact',
			messages: {
				...state.messages,
				'chat-1': [
					{
						id: 'attachment-host', sessionId: 'chat-1', role: 'assistant',
						content: 'Interrupted response.', thoughts: 'Earlier work.', interrupted: true,
						createdAt: new Date('2026-01-01T00:00:01.000Z'),
					},
					{
						id: 'attachment-steer', sessionId: 'chat-1', role: 'user', content: '',
						prioritySteer: true,
						attachments: [{ id: 'scope', name: 'scope.txt', type: 'file', size: 1, path: '/tmp/scope.txt' }],
						createdAt: new Date('2026-01-01T00:00:02.000Z'),
					},
				],
			},
			acpBindingBySessionId: {},
		}))

		const host = document.createElement('div')
		document.body.appendChild(host)
		const root = createRoot(host)
		act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
		const steer = host.querySelector('article.conversation-turn--user')
		expect(steer?.textContent).toContain('scope.txt')
		expect(steer?.querySelector('[data-detail="/tmp/scope.txt"]')).not.toBeNull()
		expect(steer?.querySelector('[aria-label="Edit message in new branch"]')).toBeTruthy()
		expect(host.querySelectorAll('article.conversation-turn--user')).toHaveLength(1)
		expect(host.textContent!.indexOf('Response interrupted')).toBeLessThan(host.textContent!.indexOf('scope.txt'))

		act(() => root.unmount())
		host.remove()
	})

  it('shows either UAM agent or provider mode from the provider preference while permissions stay independent', async () => {
    const setSessionApprovalMode = vi.fn(() => Promise.resolve(true))
    const setSessionUamAgent = vi.fn(() => Promise.resolve(true))
    const setSessionCommandSafetyTier = vi.fn(() => Promise.resolve({ ok: true }))
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) => ({ ...session, approvalMode: 'default', commandSafetyTier: 'aiReview' as const })),
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          lifecycleState: 'ready',
          processing: false,
          currentModeId: 'default',
          pendingPermission: null,
        },
      },
      setSessionApprovalMode,
      setSessionUamAgent,
      setSessionCommandSafetyTier,
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    openComposerOptions(host)

    const agent = document.body.querySelector('button[aria-label="UAM agent"]') as HTMLButtonElement
    expect(agent.textContent).toContain('Build')
    act(() => agent.click())
    const plan = Array.from(document.body.querySelectorAll('[role="option"]')).find((option) => option.textContent?.includes('Plan')) as HTMLButtonElement
    await act(async () => { plan.click(); await Promise.resolve() })
	expect(setSessionUamAgent).toHaveBeenCalledWith('chat-1', 'plan')

	expect(document.body.querySelector('button[aria-label="Provider mode"]')).toBeNull()
	act(() => useAppStore.setState({ providerChatDefaults: { 'gemini-cli': { modelId: '', approvalMode: 'default', commandSafetyTier: 'off', memoryEnabled: true, featurePreference: 'provider' } } }))
	expect(document.body.querySelector('button[aria-label="UAM agent"]')).toBeNull()
	const providerMode = document.body.querySelector('button[aria-label="Provider mode"]') as HTMLButtonElement
	act(() => providerMode.click())
	const providerPlan = Array.from(document.body.querySelectorAll('[role="option"]')).find((option) => option.textContent?.includes('Plan')) as HTMLButtonElement
	await act(async () => { providerPlan.click(); await Promise.resolve() })
    expect(setSessionApprovalMode).toHaveBeenCalledWith('chat-1', 'plan')

    const permission = document.body.querySelector('button[aria-label="Permissions"]') as HTMLButtonElement
    expect(permission.textContent).toContain('AI Review')
    act(() => permission.click())
    const yolo = Array.from(document.body.querySelectorAll('[role="option"]')).find((option) => option.textContent?.includes('YOLO')) as HTMLButtonElement
    await act(async () => { yolo.click(); await Promise.resolve() })
    expect(host.textContent).toContain('Enable YOLO?')
    await act(async () => { (Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Enable YOLO') as HTMLButtonElement).click(); await Promise.resolve() })
    expect(setSessionCommandSafetyTier).toHaveBeenCalledWith('chat-1', 'yolo')

    expect(host.querySelector('select')).toBeNull()

    const optionsButton = host.querySelector('button[aria-label="Options"]') as HTMLButtonElement
    if (optionsButton.getAttribute('aria-expanded') === 'true') {
      act(() => optionsButton.click())
    }
    expect(host.querySelector('button[aria-label="Permissions"]')).toBeTruthy()
    expect(document.body.querySelector('button[aria-label="Auto Decide safety"]')).toBeNull()
    expect(setSessionCommandSafetyTier).toHaveBeenCalledTimes(1)

    act(() => root.unmount())
    host.remove()
  })

  it('discovers, inspects, validates, and applies permission slash commands', async () => {
    const setSessionApprovalMode = vi.fn(() => Promise.resolve(true))
    const setSessionCommandSafetyTier = vi.fn(() => Promise.resolve({ ok: true }))
    useAppStore.setState((state) => ({
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          lifecycleState: 'ready',
          processing: false,
          availableModes: [
            { id: 'default', name: 'Default', description: '' },
            { id: 'plan', name: 'Plan', description: '' },
          ],
          currentModeId: 'default',
        },
      },
      setSessionApprovalMode,
      setSessionCommandSafetyTier,
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    const textarea = host.querySelector('textarea') as HTMLTextAreaElement
    const setDraft = (value: string) => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set?.call(textarea, value)
      textarea.dispatchEvent(new Event('input', { bubbles: true }))
    }

    act(() => setDraft('/p'))
    const slashMenu = document.body.querySelector('[aria-label="Slash commands"]') as HTMLElement
    expect(slashMenu.textContent).toContain('/permission')
    expect(slashMenu.parentElement).toBe(document.body)
    expect(textarea.getAttribute('aria-expanded')).toBe('true')
    expect(textarea.getAttribute('aria-controls')).toBe(slashMenu.id)
    expect(textarea.getAttribute('aria-activedescendant')).toBe(slashMenu.querySelector('[aria-selected="true"]')?.id)

    await act(async () => {
      setDraft('/permission')
      textarea.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true }))
    })
    const permissionMenu = document.body.querySelector('[aria-label="Permission modes"]')
    expect(textarea.value).toBe('/permission ')
    expect(permissionMenu?.textContent).toContain('Default')
    expect(permissionMenu?.textContent).not.toContain('Plan')
    expect(permissionMenu?.textContent).toContain('YOLO')
    expect(permissionMenu?.textContent).toContain('AI Review')
    expect(permissionMenu?.textContent).toContain('Accept Edits')
    expect(permissionMenu?.querySelector('svg')).not.toBeNull()
    expect(textarea.parentElement?.querySelector('[role="status"]')).toBeNull()
    expect(setSessionApprovalMode).not.toHaveBeenCalled()

    const slashAiReview = Array.from(permissionMenu?.querySelectorAll('[role="option"]') ?? []).find((option) => option.textContent?.includes('AI Review')) as HTMLButtonElement
    await act(async () => { slashAiReview.dispatchEvent(new MouseEvent('mousedown', { bubbles: true })) })
    expect(setSessionCommandSafetyTier).toHaveBeenCalledWith('chat-1', 'aiReview')
    expect(host.textContent).not.toContain('Permission mode changed')

    await act(async () => {
      setDraft('/permission yolo')
      textarea.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true }))
    })
    expect(host.textContent).toContain('Enable YOLO?')
    await act(async () => { (Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Enable YOLO') as HTMLButtonElement).click(); await Promise.resolve() })
    expect(setSessionCommandSafetyTier).toHaveBeenCalledWith('chat-1', 'yolo')
    expect(host.textContent).not.toContain('Permission mode changed')

    await act(async () => {
      setDraft('/permission unrestricted')
      textarea.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true }))
    })
    expect(setSessionApprovalMode).not.toHaveBeenCalled()
    expect(Array.from(host.querySelectorAll('[role="alert"]')).some((alert) => alert.textContent?.includes('Unsupported permission mode "unrestricted"'))).toBe(true)
    const dismissFeedback = host.querySelector('button[aria-label="Dismiss command feedback"]') as HTMLButtonElement
    act(() => dismissFeedback.click())
    expect(host.textContent).not.toContain('Unsupported permission mode "unrestricted"')

    act(() => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set?.call(textarea, 'keep /permission yo tail')
      textarea.setSelectionRange(18, 18)
      textarea.dispatchEvent(new Event('input', { bubbles: true }))
    })
    const middleYolo = Array.from(document.body.querySelectorAll('[aria-label="Permission modes"] [role="option"]')).find((option) => option.textContent?.includes('YOLO')) as HTMLButtonElement
    await act(async () => { middleYolo.dispatchEvent(new MouseEvent('mousedown', { bubbles: true })) })
    await act(async () => { (Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Enable YOLO') as HTMLButtonElement).click(); await Promise.resolve() })
    expect(textarea.value).toBe('keep  tail')
    expect(setSessionCommandSafetyTier).toHaveBeenLastCalledWith('chat-1', 'yolo')

    act(() => root.unmount())
    host.remove()
  })

  it('shows the permission backend error as dismissible feedback and keeps cancellation silent', async () => {
    const setSessionCommandSafetyTier = vi.fn()
      .mockResolvedValueOnce({ ok: false, error: 'Native permission update was rejected.' })
      .mockResolvedValueOnce({ ok: false, cancelled: true })
    useAppStore.setState((state) => ({
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': { ...state.acpBindingBySessionId['chat-1'], lifecycleState: 'ready', processing: false },
      },
      setSessionCommandSafetyTier,
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    const textarea = host.querySelector('textarea') as HTMLTextAreaElement
    const requestYolo = async () => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set?.call(textarea, '/permission yolo')
      textarea.dispatchEvent(new Event('input', { bubbles: true }))
      textarea.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true }))
      await Promise.resolve()
    }
    const acceptYolo = async () => {
      const button = Array.from(host.querySelectorAll('button')).find((candidate) => candidate.textContent === 'Enable YOLO') as HTMLButtonElement
      expect(button).toBeTruthy()
      button.click()
      await Promise.resolve()
    }

    await act(requestYolo)
    await act(acceptYolo)
    expect(host.textContent).toContain('Native permission update was rejected.')
    act(() => (host.querySelector('button[aria-label="Dismiss command feedback"]') as HTMLButtonElement).click())
    expect(host.textContent).not.toContain('Native permission update was rejected.')

    await act(requestYolo)
    await act(acceptYolo)
    expect(host.textContent).not.toContain('Failed to change permission mode')

    act(() => root.unmount())
    host.remove()
  })

  it('toggles Computer Use from slash and composer options without opening its settings modal', async () => {
    const setSessionComputerUseEnabled = vi.fn(() => Promise.resolve({ ok: true as const }))
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) => ({ ...session, computerUseEffectiveBackend: 'provider' as const })),
      setSessionComputerUseEnabled,
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    const textarea = host.querySelector('textarea') as HTMLTextAreaElement

    act(() => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set?.call(textarea, '/computer')
      textarea.dispatchEvent(new Event('input', { bubbles: true }))
      textarea.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true }))
    })
    expect(setSessionComputerUseEnabled).toHaveBeenCalledWith('chat-1', true)
    expect(document.body.querySelector('[role="dialog"][aria-label="Computer use"]')).toBeNull()

    openComposerOptions(host)
    const computerOption = Array.from(document.body.querySelectorAll('[role="menuitem"]')).find((item) => item.textContent?.includes('Computer use')) as HTMLButtonElement
    expect(computerOption).toBeTruthy()
    await act(async () => { computerOption.click(); await Promise.resolve() })
    expect(setSessionComputerUseEnabled).toHaveBeenCalledWith('chat-1', true)
    expect(document.body.querySelector('[role="dialog"][aria-label="Computer use"]')).toBeNull()

    act(() => root.unmount())
    host.remove()
  })

  it('labels an enabled Computer Use chat as awaiting target approval until MCP grants one', () => {
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) => ({ ...session, computerUseEnabled: true, computerUseTargetId: '', computerUse: { enabled: true, state: 'armed', history: [] } })),
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    expect(host.querySelector('[data-mode-chip="Computer use: awaiting target approval"]')).toBeTruthy()
    act(() => root.unmount())
    host.remove()
  })

  it('always offers /skills and exposes only favorite collision-safe skill commands', async () => {
    const attachMarkdownStoreEntry = vi.fn()
    useAppStore.setState({
      markdownStoreEntries: [
        { id: 'one', title: 'GitHub Review', maker: '', review: '', dateCreated: '', dateUpdated: '', preview: '', favorite: true, sourceProvider: 'codex', commandName: 'github-review', filePath: '/tmp/one.uam' },
        { id: 'two', title: 'GitHub Release', maker: '', review: '', dateCreated: '', dateUpdated: '', preview: '', favorite: true, sourceProvider: 'gemini-cli', commandName: 'github-release', filePath: '/tmp/two.uam' },
        { id: 'three', title: 'Hidden', maker: '', review: '', dateCreated: '', dateUpdated: '', preview: '', favorite: false, sourceProvider: 'claude-code', commandName: 'hidden', filePath: '/tmp/three.uam' },
      ],
      refreshMarkdownStore: vi.fn(() => Promise.resolve(true)),
      attachMarkdownStoreEntry,
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    const textarea = host.querySelector('textarea') as HTMLTextAreaElement
    await act(async () => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set?.call(textarea, '/')
      textarea.dispatchEvent(new Event('input', { bubbles: true }))
      await Promise.resolve()
    })
    const palette = document.body.querySelector('[aria-label="Slash commands"]') as HTMLElement | null
    expect(palette?.style.position).toBe('fixed')
    expect(palette?.parentElement).toBe(document.body)
    expect(palette?.textContent).toContain('/skills')
    expect(palette?.textContent).toContain('/github-review')
    expect(palette?.textContent).toContain('/github-release')
    expect(Array.from(palette?.querySelectorAll('[aria-haspopup="menu"]') ?? []).some((option) => option.textContent?.includes('/github'))).toBe(false)
    expect(palette?.textContent).toContain('codex')
    expect(palette?.textContent).toContain('gemini-cli')
    expect(palette?.textContent).not.toContain('/hidden')

    const reviewTwo = Array.from(palette?.querySelectorAll('[role="option"]') ?? []).find((option) => option.textContent?.includes('/github-release')) as HTMLElement
    act(() => reviewTwo.dispatchEvent(new MouseEvent('mousedown', { bubbles: true })))
    expect(attachMarkdownStoreEntry).toHaveBeenCalledWith('chat-1', expect.objectContaining({ id: 'two' }))

    act(() => root.unmount())
    host.remove()
  })

  it('closes the slash palette at once when /compact is chosen, with no success notice', async () => {
    enableSupportedManualCompactionFixture()
    useAppStore.setState((state) => ({
      acpBindingBySessionId: { ...state.acpBindingBySessionId, 'chat-1': { ...state.acpBindingBySessionId['chat-1'], processing: false, lifecycleState: 'ready', pendingPermission: null, pendingUserInput: null } },
    }))
    window.cefQuery = ({ request }) => {
      void request // compaction request stays pending; the palette must already be closed
    }
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    const textarea = host.querySelector('textarea') as HTMLTextAreaElement
    Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set?.call(textarea, '/compact')
    await act(async () => { textarea.dispatchEvent(new Event('input', { bubbles: true })) })
    const row = Array.from(document.body.querySelectorAll('[aria-label="Slash commands"] [role="option"]')).find((option) => option.textContent?.includes('/compact')) as HTMLElement
    await act(async () => { row.dispatchEvent(new MouseEvent('mousedown', { bubbles: true })) })
    expect(document.body.querySelector('[aria-label="Slash commands"]')).toBeNull()
    expect(host.textContent).not.toContain('Compaction requested.')
    act(() => root.unmount())
    host.remove()
    delete window.cefQuery
  })

  it('shows a muted Draft label only while the composer holds unsent text', async () => {
    writeChatComposerDraft('chat-1', { text: 'Half a thought', attachments: [] })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    const label = host.querySelector('[data-draft-indicator]') as HTMLElement
    expect(label.textContent).toBe('Draft')
    expect(label.getAttribute('style')).toContain('var(--text-3)')
    act(() => root.unmount())
    host.remove()
  })

  it.each(['enter', 'send', 'palette', 'failure', 'not-accepted', 'new-draft'] as const)('routes compact via native control and preserves attachments (%s)', async (operation) => {
    enableSupportedManualCompactionFixture()
    const previousSendAcpPrompt = useAppStore.getState().sendAcpPrompt
    const previousSkills = useAppStore.getState().markdownStoreAttachedBySessionId
    const sendAcpPrompt = vi.fn().mockResolvedValue(true)
    const skill = { id: 'keep', title: 'Keep', maker: '', review: '', dateCreated: '', dateUpdated: '', preview: '', filePath: '/skills/keep.md' }
    useAppStore.setState((state) => ({
      sendAcpPrompt, markdownStoreAttachedBySessionId: { 'chat-1': [skill] },
      acpBindingBySessionId: { 'chat-1': { ...state.acpBindingBySessionId['chat-1'], processing: false, lifecycleState: 'ready', pendingPermission: null, pendingUserInput: null } },
    }))
    writeChatComposerDraft('chat-1', { text: '/compact', attachments: [{ id: 'keep-file', name: 'keep.txt', type: 'file', size: 4, path: '/tmp/keep.txt', status: 'ready' }] })
    const requests: Array<{ action: string; payload: unknown }> = []
    let finish = () => { throw new Error('Compact not requested') }
    window.cefQuery = ({ request, onSuccess, onFailure }) => {
      const parsed = JSON.parse(request)
      requests.push(parsed)
      if (parsed.action === 'compactAcpSession') finish = () => operation === 'failure'
        ? onFailure(400, 'This provider does not support manual compaction.')
        : onSuccess(JSON.stringify({ accepted: operation !== 'not-accepted' }))
      else onSuccess('{}')
    }
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    const textarea = host.querySelector('textarea') as HTMLTextAreaElement
    const setDraft = (value: string) => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set?.call(textarea, value)
      textarea.dispatchEvent(new Event('input', { bubbles: true }))
    }
    await act(async () => {
      if (operation === 'send' || operation === 'failure' || operation === 'not-accepted' || operation === 'new-draft') {
        (host.querySelector('button[aria-label="Send prompt"]') as HTMLButtonElement).click()
      } else {
        setDraft('/compact')
        if (operation === 'palette') {
          const row = Array.from(document.body.querySelectorAll('[aria-label="Slash commands"] [role="option"]')).find((option) => option.textContent?.includes('/compact')) as HTMLElement
          expect(row).toBeTruthy()
          row.dispatchEvent(new MouseEvent('mousedown', { bubbles: true }))
        } else textarea.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true }))
      }
    })
    expect(requests.filter((request) => request.action === 'compactAcpSession')).toEqual([{ action: 'compactAcpSession', payload: { chatId: 'chat-1' }, requestId: expect.any(String) }])
    expect(sendAcpPrompt).not.toHaveBeenCalled()
    if (operation === 'new-draft') act(() => setDraft('My next prompt'))
    await act(async () => finish())
    expect(textarea.value).toBe(operation === 'failure' || operation === 'not-accepted' ? '/compact' : operation === 'new-draft' ? 'My next prompt' : '')
    expect(readChatComposerDraft('chat-1').attachments).toHaveLength(1)
    expect(useAppStore.getState().markdownStoreAttachedBySessionId['chat-1']).toEqual([skill])
    expect(useAppStore.getState().messages['chat-1']).toHaveLength(2)
    if (operation === 'failure') expect(host.textContent).toContain('This provider does not support manual compaction.')
    if (operation === 'not-accepted') expect(host.textContent).toContain('Compaction failed: the request was not accepted.')
    act(() => root.unmount())
    host.remove()
    delete window.cefQuery
    useAppStore.setState({ sendAcpPrompt: previousSendAcpPrompt, markdownStoreAttachedBySessionId: previousSkills })
  })

  it.each([false, true])('keeps compact acknowledgements scoped to their original chat visit and blocks duplicates (return=%s)', async (returnToFirst) => {
    enableSupportedManualCompactionFixture()
    const first = useAppStore.getState().sessions[0]
    const second = { ...first, id: 'chat-2', name: 'Second chat' }
    useAppStore.setState((state) => ({
      sessions: [first, second],
      acpBindingBySessionId: { ...state.acpBindingBySessionId, 'chat-2': { ...state.acpBindingBySessionId['chat-1'], sessionId: 'native-2', threadId: 'native-2',
        manualCompaction: { available: true, providerId: 'codex-cli', modelId: 'gpt-5.4', nativeSessionId: 'native-2' } } },
    }))
    writeChatComposerDraft('chat-1', { text: '/compact', attachments: [] })
    writeChatComposerDraft('chat-2', { text: 'Second chat draft', attachments: [] })
    let finish = () => { throw new Error('Compact not requested') }
    let count = 0
    window.cefQuery = ({ request, onSuccess }) => {
      if (JSON.parse(request).action === 'compactAcpSession') { count++; finish = () => onSuccess(JSON.stringify({ accepted: true })) }
      else onSuccess('{}')
    }
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView key={first.id} session={first} />))
    await act(async () => {
      host.querySelector('form')?.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true }))
      host.querySelector('form')?.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true }))
    })
    expect(count).toBe(1)
    act(() => root.render(<ChatView key={second.id} session={second} />))
    if (returnToFirst) act(() => root.render(<ChatView key={first.id} session={first} />))
    await act(async () => finish())
    expect((host.querySelector('textarea') as HTMLTextAreaElement).value).toBe(returnToFirst ? '/compact' : 'Second chat draft')
    expect(readChatComposerDraft('chat-1').text).toBe('/compact')
    expect(host.textContent).not.toContain('Compaction requested.')
    act(() => root.unmount())
    host.remove()
    delete window.cefQuery
  })

  it('offers Copilot ACP advertised commands without shadowing local slash actions', async () => {
    useAppStore.setState((state) => ({
      providers: [
        ...state.providers,
        { id: 'copilot-cli', name: 'GitHub Copilot CLI', shortName: 'Copilot', color: '#22c55e', description: '', outputMode: 'cli', supportsCli: true, supportsStructured: true, structuredProtocol: 'copilot-acp' },
      ],
      sessions: state.sessions.map((session) => session.id === 'chat-1' ? { ...session, providerId: 'copilot-cli' } : session),
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          providerId: 'copilot-cli',
          protocolKind: 'copilot-acp',
          availableCommands: [
            { name: 'security-review', description: 'Review the current changes for security issues', inputHint: '[focus]' },
            { name: 'model', description: 'Provider model picker', inputHint: '' },
          ],
        },
      },
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    const textarea = host.querySelector('textarea') as HTMLTextAreaElement
    const setDraft = (value: string) => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set?.call(textarea, value)
      textarea.dispatchEvent(new Event('input', { bubbles: true }))
    }

    act(() => setDraft('/'))
    const allCommands = document.body.querySelector('[aria-label="Slash commands"]') as HTMLElement
    expect(allCommands.textContent).toContain('/security-review')
    expect(Array.from(allCommands.querySelectorAll('[role="option"]')).filter((option) => option.textContent?.includes('/model'))).toHaveLength(1)

    act(() => setDraft('/sec'))
    const command = Array.from(document.body.querySelectorAll('[aria-label="Slash commands"] [role="option"]')).find((option) => option.textContent?.includes('/security-review')) as HTMLElement
    expect(command.textContent).toContain('Review the current changes for security issues')
    expect(command.textContent).toContain('[focus]')
    act(() => command.dispatchEvent(new MouseEvent('mousedown', { bubbles: true })))
    expect(textarea.value).toBe('/security-review ')

    act(() => root.unmount())
    host.remove()
  })

  it('shows explicit groups with only their favorite skills', async () => {
    const attachMarkdownStoreEntry = vi.fn()
    useAppStore.setState({
      markdownStoreEntries: [
        { id: 'one', title: 'GitHub Fix Issue', maker: '', review: '', dateCreated: '', dateUpdated: '', preview: '', favorite: true, commandName: 'github-fix-issue-3aa611dd', group: 'GitHub', filePath: '/tmp/one.uam' },
        { id: 'two', title: 'GitHub Create PR', maker: '', review: '', dateCreated: '', dateUpdated: '', preview: '', favorite: false, commandName: 'github-create-pr-eb8349e5', group: 'GitHub', filePath: '/tmp/two.uam' },
      ],
      refreshMarkdownStore: vi.fn(() => Promise.resolve(true)),
      attachMarkdownStoreEntry,
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    const textarea = host.querySelector('textarea') as HTMLTextAreaElement
    await act(async () => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set?.call(textarea, '/github')
      textarea.dispatchEvent(new Event('input', { bubbles: true }))
      await Promise.resolve()
    })
    const group = document.body.querySelector('[aria-label="Slash commands"] [aria-haspopup="menu"]') as HTMLElement
    expect(group.textContent).toContain('/github')
    expect(group.textContent).not.toContain('/fix-issue')
    vi.spyOn(group.querySelector('[data-slash-group-anchor]') as HTMLSpanElement, 'getBoundingClientRect').mockReturnValue({ left: 180, right: 194, top: 40, bottom: 54, width: 14, height: 14, x: 180, y: 40, toJSON: () => ({}) })
    act(() => group.dispatchEvent(new MouseEvent('mousedown', { bubbles: true })))
    const submenu = document.body.querySelector('[aria-label="GitHub skills"]') as HTMLElement
    expect(document.body.querySelector('[aria-label="Slash commands"]')).toBeTruthy()
    expect(submenu.style.left).toBe('198px')
    expect(submenu.textContent).toContain('/fix-issue')
    expect(submenu.textContent).not.toContain('/create-pr')
    expect(submenu.textContent).not.toContain('3aa611dd')
    expect(submenu.textContent).not.toContain('eb8349e5')
    expect(textarea.getAttribute('aria-controls')?.split(' ')).toContain(submenu.id)
    expect(submenu.contains(document.getElementById(textarea.getAttribute('aria-activedescendant') ?? ''))).toBe(true)
    act(() => textarea.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true })))
    expect(attachMarkdownStoreEntry).toHaveBeenCalledWith('chat-1', expect.objectContaining({ id: 'one' }))
    act(() => root.unmount())
    host.remove()
  })

  it('stacks nested skill groups in the slash popup', async () => {
    useAppStore.setState({
      markdownStoreEntries: [
        { id: 'parent', title: 'Release Notes', maker: '', review: '', dateCreated: '', dateUpdated: '', preview: '', favorite: true, commandName: 'release-notes', group: 'Release', filePath: '/tmp/parent.uam' },
        { id: 'child', title: 'GitHub Release', maker: '', review: '', dateCreated: '', dateUpdated: '', preview: '', favorite: true, commandName: 'github-release', group: 'Release / GitHub', filePath: '/tmp/child.uam' },
      ],
      refreshMarkdownStore: vi.fn(() => Promise.resolve(true)),
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    const textarea = host.querySelector('textarea') as HTMLTextAreaElement
    await act(async () => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set?.call(textarea, '/release')
      textarea.dispatchEvent(new Event('input', { bubbles: true }))
      await Promise.resolve()
    })
    const release = document.body.querySelector('[aria-label="Slash commands"] [aria-haspopup="menu"]') as HTMLElement
    act(() => release.dispatchEvent(new MouseEvent('mousedown', { bubbles: true })))
    const releaseMenu = document.body.querySelector('[aria-label="Release skills"]') as HTMLElement
    expect(releaseMenu.textContent).toContain('/notes')
    expect(releaseMenu.textContent).toContain('/github')
    const github = releaseMenu.querySelector('[aria-haspopup="menu"]') as HTMLElement
    act(() => github.dispatchEvent(new MouseEvent('mouseover', { bubbles: true })))
    const githubMenu = document.body.querySelector('[aria-label="Release/GitHub skills"]') as HTMLElement
    expect(githubMenu.textContent).toContain('/release')
    expect(githubMenu.contains(document.getElementById(textarea.getAttribute('aria-activedescendant') ?? ''))).toBe(true)
    expect(githubMenu.querySelector('.is-selected')).toBe(githubMenu.querySelector('[role="menuitem"]'))
    act(() => root.unmount())
    host.remove()
  })

  it('hides the slash palette and submenu while an app modal is open', async () => {
    useAppStore.setState({
      markdownStoreEntries: [
        { id: 'one', title: 'GitHub Fix Issue', maker: '', review: '', dateCreated: '', dateUpdated: '', preview: '', favorite: true, commandName: 'github-fix-issue-3aa611dd', group: 'GitHub', filePath: '/tmp/one.uam' },
        { id: 'two', title: 'GitHub Create PR', maker: '', review: '', dateCreated: '', dateUpdated: '', preview: '', favorite: true, commandName: 'github-create-pr-eb8349e5', group: 'GitHub', filePath: '/tmp/two.uam' },
      ],
      refreshMarkdownStore: vi.fn(() => Promise.resolve(true)),
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    const textarea = host.querySelector('textarea') as HTMLTextAreaElement
    await act(async () => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set?.call(textarea, '/github')
      textarea.dispatchEvent(new Event('input', { bubbles: true }))
      await Promise.resolve()
    })
    const group = document.body.querySelector('[aria-label="Slash commands"] [role="option"]') as HTMLElement
    vi.spyOn(group.querySelector('[data-slash-group-anchor]') as HTMLSpanElement, 'getBoundingClientRect').mockReturnValue({ left: 180, right: 194, top: 40, bottom: 54, width: 14, height: 14, x: 180, y: 40, toJSON: () => ({}) })
    act(() => textarea.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowRight', bubbles: true })))
    expect(document.body.querySelector('[aria-label="GitHub skills"]')).toBeTruthy()

    act(() => useAppStore.setState({ isSettingsOpen: true }))
    expect(document.body.querySelector('[aria-label="Slash commands"]')).toBeNull()
    expect(document.body.querySelector('[aria-label="GitHub skills"]')).toBeNull()

    act(() => root.unmount())
    host.remove()
    useAppStore.setState({ isSettingsOpen: false })
  })

  it('opens reasoning and speed choices directly without changing their values', async () => {
    const setSessionCodexOptions = vi.fn(() => Promise.resolve(true))
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) => session.id === 'chat-1'
        ? { ...session, providerId: 'codex-cli', modelId: 'gpt-5.4', reasoningEffort: 'low', serviceTier: 'flex', serviceTierExplicit: true }
        : session),
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          providerId: 'codex-cli',
          protocolKind: 'codex-app-server',
          lifecycleState: 'ready',
          processing: false,
          processingStartedAtMs: null,
          pendingPermission: null,
          availableModels: [{
            id: 'gpt-5.4',
            name: 'gpt-5.4',
            description: 'Latest Codex model.',
            supportedReasoningEfforts: ['low', 'high', 'xhigh'],
            additionalSpeedTiers: ['fast', 'flex'],
          }],
          currentModelId: 'gpt-5.4',
        },
      },
      setSessionCodexOptions,
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    expect(host.querySelector('button[title="Select Codex reasoning"]')).toBeNull()
    expect(host.querySelector('button[title="Select Codex speed"]')).toBeNull()
    expect(host.querySelector('[data-mode-chip="Reasoning: Low"]')).toBeTruthy()
    expect(host.querySelector('button[aria-label="Reasoning: Low"]')).toBeNull()
    expect(host.querySelector('button[aria-label="Disable Reasoning: Low"]')).toBeNull()
    expect(host.querySelector('button[aria-label="Speed"]')).toBeTruthy()

    const reasoning = document.body.querySelector('button[aria-label="Reasoning"]') as HTMLButtonElement
    const speed = document.body.querySelector('button[aria-label="Speed"]') as HTMLButtonElement
    expect(reasoning.textContent).toContain('Low')
    expect(speed.textContent).toContain('Flex')

    act(() => reasoning.click())
    const high = Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="option"]'))
      .find((option) => option.textContent?.startsWith('High')) as HTMLButtonElement
    await act(async () => { high.click(); await Promise.resolve() })
    expect(setSessionCodexOptions).toHaveBeenNthCalledWith(1, 'chat-1', { reasoningEffort: 'high' })

    act(() => speed.click())
    const speedChoices = Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="option"]'))
    expect(speedChoices.some((option) => option.textContent?.startsWith('Inherit'))).toBe(true)
    expect(speedChoices.some((option) => option.textContent?.startsWith('Standard'))).toBe(true)
    const fast = speedChoices
      .find((option) => option.textContent?.startsWith('Fast')) as HTMLButtonElement
    await act(async () => { fast.click(); await Promise.resolve() })
    expect(setSessionCodexOptions).toHaveBeenNthCalledWith(2, 'chat-1', { serviceTier: 'fast', serviceTierExplicit: true })

    openComposerOptions(host)
    const textarea = host.querySelector('textarea') as HTMLTextAreaElement
    const setDraft = (value: string) => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set?.call(textarea, value)
      textarea.dispatchEvent(new Event('input', { bubbles: true }))
    }

    await act(async () => {
      setDraft('/reasoning xhigh')
      textarea.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true }))
      await Promise.resolve()
    })
    expect(setSessionCodexOptions).toHaveBeenNthCalledWith(3, 'chat-1', { reasoningEffort: 'xhigh' })
    expect(host.querySelector('[role="status"]')?.textContent).toContain('Reasoning changed to Extra High.')

    await act(async () => {
      setDraft('/speed flex')
      textarea.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true }))
      await Promise.resolve()
    })
    expect(setSessionCodexOptions).toHaveBeenNthCalledWith(4, 'chat-1', { serviceTier: 'flex', serviceTierExplicit: true })
    expect(host.querySelector('[role="status"]')?.textContent).toContain('Speed changed to Flex.')

    act(() => root.unmount())
    host.remove()
  })

  it('shows Copilot reasoning when ACP omits per-model effort options', async () => {
    const setSessionCodexOptions = vi.fn(() => Promise.resolve(true))
    useAppStore.setState((state) => ({
      providers: [
        ...state.providers,
        { id: 'copilot-cli', name: 'GitHub Copilot CLI', shortName: 'Copilot', color: '#22c55e', description: '', outputMode: 'cli', supportsCli: true, supportsStructured: true, structuredProtocol: 'copilot-acp' },
      ],
      sessions: state.sessions.map((session) => session.id === 'chat-1'
        ? { ...session, providerId: 'copilot-cli', modelId: 'gpt-5.1', reasoningEffort: 'max' }
        : session),
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          providerId: 'copilot-cli',
          protocolKind: 'copilot-acp',
          lifecycleState: 'ready',
          processing: false,
          processingStartedAtMs: null,
          pendingPermission: null,
          availableModels: [{ id: 'gpt-5.1', name: 'GPT-5.1', description: '', supportedReasoningEfforts: [] }],
          currentModelId: 'gpt-5.1',
        },
      },
      setSessionCodexOptions,
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    expect(host.querySelector('[data-mode-chip="Reasoning: Max"]')).toBeTruthy()
    openComposerOptions(host)
    const reasoning = document.body.querySelector('button[aria-label="Reasoning"]') as HTMLButtonElement
    expect(reasoning.textContent).toContain('Max')
    expect(document.body.querySelector('button[aria-label="Speed"]')).toBeNull()

    act(() => reasoning.click())
    const high = Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="option"]'))
      .find((option) => option.textContent?.startsWith('High')) as HTMLButtonElement
    await act(async () => { high.click(); await Promise.resolve() })
    expect(setSessionCodexOptions).toHaveBeenCalledWith('chat-1', { reasoningEffort: 'high' })

    act(() => root.unmount())
    host.remove()
  })

  it('blocks speed changes from chips and slash commands while Codex is processing', async () => {
    const setSessionCodexOptions = vi.fn(() => Promise.resolve(true))
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) => session.id === 'chat-1'
        ? { ...session, providerId: 'codex-cli', modelId: 'gpt-5.4', serviceTier: 'flex', serviceTierExplicit: true }
        : session),
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          providerId: 'codex-cli',
          protocolKind: 'codex-app-server',
          lifecycleState: 'processing',
          processing: true,
          pendingPermission: null,
          availableModels: [{
            id: 'gpt-5.4',
            name: 'gpt-5.4',
            description: 'Latest Codex model.',
            additionalSpeedTiers: ['fast', 'flex'],
          }],
          currentModelId: 'gpt-5.4',
        },
      },
      setSessionCodexOptions,
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    const clearSpeed = host.querySelector<HTMLButtonElement>('button[aria-label="Disable Speed: Flex"]')
    expect(clearSpeed).toBeNull()
    expect(host.querySelector('[data-mode-chip="Speed: Flex"]')).toBeTruthy()
    act(() => clearSpeed?.click())

    const textarea = host.querySelector('textarea') as HTMLTextAreaElement
    await act(async () => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set?.call(textarea, '/speed fast')
      textarea.dispatchEvent(new Event('input', { bubbles: true }))
      textarea.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true }))
      await Promise.resolve()
    })

    expect(setSessionCodexOptions).not.toHaveBeenCalled()
    expect(host.querySelector('[role="status"]')?.textContent).toContain('Codex is still working.')

    act(() => root.unmount())
    host.remove()
  })

  it('switches persisted edit and resend branches inside one chat', () => {
    const setActiveSession = vi.fn()
    useAppStore.setState((state) => ({
      sessions: [
        { ...state.sessions[0], id: 'chat-root', branchRootChatId: 'chat-root' },
        {
          ...state.sessions[0],
          id: 'chat-branch',
          name: 'Edited branch',
          parentChatId: 'chat-root',
          branchRootChatId: 'chat-root',
          branchFromMessageIndex: 0,
          branchMessageEdited: true,
          createdAt: new Date('2026-01-01T00:01:00.000Z'),
        },
        {
          ...state.sessions[0],
          id: 'chat-branch-2',
          name: 'Resent branch',
          parentChatId: 'chat-root',
          branchRootChatId: 'chat-root',
          branchFromMessageIndex: 0,
          branchMessageEdited: false,
          createdAt: new Date('2026-01-01T00:02:00.000Z'),
        },
      ],
      messages: {
        'chat-root': state.messages['chat-1'],
        'chat-branch': state.messages['chat-1'],
        'chat-branch-2': state.messages['chat-1'],
      },
      activeSessionId: 'chat-branch',
      setActiveSession,
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[1]} />))

    expect(host.querySelector('[aria-label="Chat branches"]')).toBeNull()
    const branchPoint = host.querySelector('.conversation-turn--user')
    const navigation = branchPoint?.querySelector('[aria-label="Message branches"]')
    expect(navigation?.textContent).toContain('2 / 3')
    expect(navigation?.classList.contains('conversation-turn__branches')).toBe(true)
    expect(branchPoint?.querySelector('button[aria-label="Edit message in new branch"]')).toBeTruthy()
    expect(branchPoint?.querySelector('button[aria-label="Revert to message in new branch"]')).toBeTruthy()
    act(() => (navigation?.querySelector('button[aria-label="Previous message branch"]') as HTMLButtonElement).click())
    expect(setActiveSession).toHaveBeenCalledWith('chat-root')
    act(() => (navigation?.querySelector('button[aria-label="Next message branch"]') as HTMLButtonElement).click())
    expect(setActiveSession).toHaveBeenCalledWith('chat-branch-2')

    act(() => root.unmount())
    host.remove()
  })

  it('renders Codex runtime model options without Gemini fallback labels', () => {
    const setSessionModel = vi.fn(() => Promise.resolve(true))
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) =>
        session.id === 'chat-1' ? { ...session, providerId: 'codex-cli', modelId: 'gpt-5.4' } : session
      ),
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          providerId: 'codex-cli',
          protocolKind: 'codex-app-server',
          lifecycleState: 'ready',
          processing: false,
          processingStartedAtMs: null,
          pendingPermission: null,
          availableModels: [
            { id: 'gpt-5.4', name: 'gpt-5.4', description: 'Latest frontier agentic coding model.' },
            { id: 'gpt-5.4-mini', name: 'GPT-5.4-Mini', description: 'Smaller frontier agentic coding model.' },
          ],
          currentModelId: 'gpt-5.4',
          agentInfo: { name: 'codex', title: 'Codex', version: '1.0.0' },
        },
      },
      setSessionModel,
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    const modelButton = host.querySelector('button[aria-label="Select provider and model"]')
    expect(modelButton).toBeTruthy()
    expect(modelButton?.textContent).toContain('gpt-5.4')
    expect(host.querySelector('.uam-provider-logo--codex')).toBeTruthy()

    act(() => {
      modelButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(document.body.textContent).not.toContain('CLI default')
    expect(document.body.textContent).toContain('gpt-5.4')
    expect(document.body.textContent).toContain('GPT-5.4-Mini')
    expect(document.body.textContent).not.toContain('Auto 3')
    expect(document.body.textContent).not.toContain('Flash Lite')

    const miniButton = Array.from(document.body.querySelectorAll('button')).find((button) =>
      button.textContent?.includes('GPT-5.4-Mini')
    )
    expect(miniButton).toBeTruthy()
    act(() => {
      miniButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(setSessionModel).toHaveBeenCalledWith('chat-1', 'gpt-5.4-mini')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('keeps a new provider-default model visible until ACP resolves it', () => {
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) =>
        session.id === 'chat-1' ? { ...session, providerId: 'codex-cli', modelId: '' } : session
      ),
      acpBindingBySessionId: {},
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    const defaultSession = useAppStore.getState().sessions[0]
    act(() => root.render(<ChatView session={defaultSession} />))

    const defaultModelButton = host.querySelector('button[aria-label="Select provider and model"]') as HTMLButtonElement
    expect(defaultModelButton.textContent).toContain('Default')
    act(() => defaultModelButton.click())
    const defaultMenu = document.body.querySelector('[role="menu"][aria-label="Provider and model"]') as HTMLElement
    expect(Array.from(defaultMenu.querySelectorAll('[data-testid="model-options"] [role="menuitemradio"]')).some((option) => option.textContent?.includes('Default'))).toBe(true)
    act(() => defaultMenu.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })))

    const explicitSession = { ...defaultSession, modelId: 'gpt-5.4' }
    act(() => root.render(<ChatView session={explicitSession} />))
    const explicitModelButton = host.querySelector('button[aria-label="Select provider and model"]') as HTMLButtonElement
    expect(explicitModelButton.textContent).toContain('GPT-5.4')
    act(() => explicitModelButton.click())
    const explicitMenu = document.body.querySelector('[role="menu"][aria-label="Provider and model"]') as HTMLElement
    expect(Array.from(explicitMenu.querySelectorAll('[data-testid="model-options"] [role="menuitemradio"]')).some((option) => option.textContent?.includes('Default'))).toBe(false)

    act(() => root.unmount())
    host.remove()
  })

  it('ignores a stale ACP model catalog after switching provider', () => {
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) =>
        session.id === 'chat-1' ? { ...session, providerId: 'codex-cli', modelId: '' } : session
      ),
      acpBindingBySessionId: {
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          providerId: 'gemini-cli',
          protocolKind: 'gemini-acp',
          lifecycleState: 'ready',
          processing: false,
          pendingPermission: null,
          availableModels: [
            { id: 'auto-gemini-3', name: 'Auto 3', description: 'Stale Gemini catalog' },
          ],
          currentModelId: 'auto-gemini-3',
        },
      },
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    const selector = host.querySelector('button[aria-label="Select provider and model"]') as HTMLButtonElement
    expect(selector.textContent).toContain('Default')
    expect(selector.textContent).not.toContain('Auto 3')

    act(() => root.unmount())
    host.remove()
  })

  it('shows the selected Codex model while ACP still reports the previous idle model', () => {
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) =>
        session.id === 'chat-1' ? { ...session, providerId: 'codex-cli', modelId: 'gpt-5.4-mini' } : session
      ),
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          providerId: 'codex-cli',
          protocolKind: 'codex-app-server',
          lifecycleState: 'ready',
          processing: false,
          processingStartedAtMs: null,
          pendingPermission: null,
          availableModels: [
            { id: 'gpt-5.4', name: 'GPT-5.4', description: '' },
            { id: 'gpt-5.4-mini', name: 'GPT-5.4 Mini', description: '' },
          ],
          currentModelId: 'gpt-5.4',
        },
      },
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    expect(host.querySelector('button[aria-label="Select provider and model"]')?.textContent).toContain('GPT-5.4 Mini')

    act(() => root.unmount())
    host.remove()
  })

  it('refuses hidden OpenCode models for both mouse and keyboard selection', () => {
    const setSessionModel = vi.fn(() => Promise.resolve(true))
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) => session.id === 'chat-1' ? { ...session, providerId: 'opencode-cli', modelId: 'test/hidden' } : session),
      providerChatDefaults: { ...state.providerChatDefaults, 'opencode-cli': { ...state.providerChatDefaults['opencode-cli'], hiddenModelIds: ['test/hidden'] } },
      acpBindingBySessionId: { ...state.acpBindingBySessionId, 'chat-1': { ...state.acpBindingBySessionId['chat-1'], providerId: 'opencode-cli', lifecycleState: 'ready', processing: false,
        availableModels: [{ id: 'test/hidden', name: 'Hidden', description: '' }, { id: 'test/visible', name: 'Visible', description: '' }], currentModelId: 'test/hidden' } },
      setSessionModel,
    }))
    const host = document.createElement('div'); document.body.appendChild(host)
    const root = createRoot(host)
    try {
      act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
      act(() => (host.querySelector('button[aria-label="Select provider and model"]') as HTMLButtonElement).click())
      const menu = document.body.querySelector('[role="menu"][aria-label="Provider and model"]')!
      const options = Array.from(menu.querySelectorAll<HTMLButtonElement>('[data-testid="model-options"] [role="menuitemradio"]'))
      const hidden = options.find((option) => option.textContent?.includes('Hidden'))!
      expect(hidden.disabled).toBe(true)
      act(() => hidden.click())
      expect(setSessionModel).not.toHaveBeenCalled()
      act(() => menu.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowDown', bubbles: true })))
      act(() => menu.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true })))
      expect(setSessionModel).not.toHaveBeenCalledWith('chat-1', 'test/hidden')
    } finally { act(() => root.unmount()); host.remove() }
  })

  it('navigates long model lists with the keyboard and keeps them scrollable', () => {
    const setSessionModel = vi.fn(() => Promise.resolve(true))
    const availableModels = Array.from({ length: 12 }, (_, index) => ({
      id: `model-${index}`,
      name: `Model ${index}`,
      description: `Model ${index} detail`,
    }))
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) =>
        session.id === 'chat-1' ? { ...session, providerId: 'codex-cli', modelId: 'model-0' } : session
      ),
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          providerId: 'codex-cli',
          lifecycleState: 'ready',
          processing: false,
          availableModels,
          currentModelId: 'model-0',
        },
      },
      setSessionModel,
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    const trigger = host.querySelector('button[aria-label="Select provider and model"]') as HTMLButtonElement
    expect(trigger.getAttribute('aria-haspopup')).toBe('menu')
    expect(trigger.style.borderRadius).toBe('6px')
    act(() => trigger.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowDown', bubbles: true })))

    const menu = document.body.querySelector('[role="menu"][aria-label="Provider and model"]') as HTMLDivElement
    const modelOptions = menu.querySelector('[data-testid="model-options"]') as HTMLDivElement
    const options = Array.from(modelOptions.querySelectorAll<HTMLButtonElement>('[role="menuitemradio"]'))
    expect(options).toHaveLength(12)
    expect(modelOptions.style.maxHeight).toBe('360px')
    expect(modelOptions.style.overflowY).toBe('auto')
    const search = menu.querySelector<HTMLInputElement>('input[aria-label="Search models"]')!
    expect(document.activeElement).toBe(search)
    act(() => search.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowDown', bubbles: true })))
    expect(document.activeElement?.textContent).toContain('Model 0')

    act(() => document.activeElement!.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowDown', bubbles: true })))
    expect(document.activeElement?.textContent).toContain('Model 1')
    expect((document.activeElement as HTMLElement).closest('[data-highlight]')?.getAttribute('data-highlight')).toBe('true')
    act(() => document.activeElement!.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true })))
    expect(setSessionModel).toHaveBeenCalledWith('chat-1', 'model-1')
    expect(document.activeElement).toBe(trigger)

    act(() => trigger.click())
    const reopened = document.body.querySelector<HTMLInputElement>('input[aria-label="Search models"]')!
    const enterSearch = (query: string) => act(() => {
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(reopened, query)
      reopened.dispatchEvent(new Event('input', { bubbles: true }))
    })
    enterSearch('MODEL-11')
    expect(document.activeElement).toBe(reopened)
    const results = document.body.querySelector('[data-testid="model-options"]')!
    expect(results.querySelectorAll('[role="menuitemradio"]')).toHaveLength(1)
    expect(results.textContent).toContain('Model 11')
    act(() => reopened.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true })))
    expect(setSessionModel).toHaveBeenLastCalledWith('chat-1', 'model-11')
    act(() => trigger.click())
    const emptySearch = document.body.querySelector<HTMLInputElement>('input[aria-label="Search models"]')!
    act(() => {
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(emptySearch, 'no-model-found')
      emptySearch.dispatchEvent(new Event('input', { bubbles: true }))
    })
    expect(document.body.textContent).toContain('No matching models')
    const calls = setSessionModel.mock.calls.length
    act(() => emptySearch.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true })))
    expect(setSessionModel).toHaveBeenCalledTimes(calls)
    act(() => emptySearch.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })))
    expect(document.activeElement).toBe(trigger)

    act(() => root.unmount())
    host.remove()
  })

  it('hides only repeated Copilot model details', () => {
    useAppStore.setState((state) => ({
      providers: [
        ...state.providers,
        { id: 'copilot-cli', name: 'GitHub Copilot CLI', shortName: 'Copilot', color: '#fff', description: '', outputMode: 'cli', supportsCli: true, supportsStructured: true, structuredProtocol: 'copilot-acp' },
      ],
      sessions: state.sessions.map((session) =>
        session.id === 'chat-1' ? { ...session, providerId: 'copilot-cli', modelId: '' } : session
      ),
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          providerId: 'copilot-cli',
          lifecycleState: 'ready',
          processing: false,
          availableModels: [
            { id: 'gpt-5.4', name: 'gpt-5.4', description: '' },
            { id: 'claude-opus', name: 'Claude Opus', description: 'Best for complex tasks' },
          ],
          currentModelId: '',
        },
      },
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    act(() => (host.querySelector('button[aria-label="Select provider and model"]') as HTMLButtonElement).click())

    const repeated = Array.from(document.body.querySelectorAll<HTMLElement>('[data-testid="model-options"] [role="menuitemradio"]')).find((option) => option.textContent?.includes('gpt-5.4'))
    expect(repeated?.textContent?.match(/gpt-5\.4/g)).toHaveLength(1)
    expect(document.body.textContent).toContain('Best for complex tasks')

    act(() => root.unmount())
    host.remove()
  })

  it('renders OpenCode model options without Gemini fallback labels', () => {
    const setSessionModel = vi.fn(() => Promise.resolve(true))
	const discoverProviderModels = vi.fn(() => Promise.resolve(true))
    useAppStore.setState((state) => ({
      providers: [
        ...state.providers,
        { id: 'opencode-cli', name: 'OpenCode', shortName: 'OpenCode', color: '#14b8a6', description: '', outputMode: 'cli', supportsCli: true, supportsStructured: true, structuredProtocol: 'opencode-acp' },
      ],
      sessions: state.sessions.map((session) =>
        session.id === 'chat-1' ? { ...session, providerId: 'opencode-cli', modelId: 'ollama-r9700/qwen3.6:35b-a3b-q4_K_M' } : session
      ),
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          providerId: 'opencode-cli',
          protocolKind: 'opencode-acp',
          lifecycleState: 'ready',
          processing: false,
          processingStartedAtMs: null,
          pendingPermission: null,
          availableModels: [
            { id: 'ollama-r9700/qwen3.6:35b-a3b-q4_K_M', name: 'Qwen3.6 35B A3B Q4', description: '' },
            { id: 'ollama-r9700/qwen3-coder:30b', name: 'Qwen3 Coder 30B', description: '' },
          ],
          currentModelId: 'ollama-r9700/qwen3.6:35b-a3b-q4_K_M',
          agentInfo: { name: 'opencode', title: 'OpenCode', version: '0.6.6' },
        },
      },
      setSessionModel,
	  discoverProviderModels,
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    const modelButton = host.querySelector('button[aria-label="Select provider and model"]')
    expect(modelButton).toBeTruthy()

    act(() => {
      modelButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })
	expect(discoverProviderModels).not.toHaveBeenCalled()

    expect(document.body.textContent).not.toContain('CLI default')
    expect(document.body.textContent).toContain('Qwen3.6 35B A3B Q4')
    expect(document.body.textContent).toContain('Qwen3 Coder 30B')
    expect(document.body.textContent).not.toContain('Auto 3')
    expect(document.body.textContent).not.toContain('Flash Lite')

    const coderButton = Array.from(document.body.querySelectorAll('button')).find((button) =>
      button.textContent?.includes('Qwen3 Coder 30B')
    )
    expect(coderButton).toBeTruthy()
    act(() => {
      coderButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(setSessionModel).toHaveBeenCalledWith('chat-1', 'ollama-r9700/qwen3-coder:30b')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('uses provider-reported OpenCode variants without optimistic current-state changes', async () => {
    const setAcpConfigOption = vi.fn(() => Promise.resolve(true))
    useAppStore.setState((state) => ({
      providers: [
        ...state.providers,
        { id: 'opencode-cli', name: 'OpenCode', shortName: 'OpenCode', color: '#14b8a6', description: '', outputMode: 'cli', supportsCli: true, supportsStructured: true, structuredProtocol: 'opencode-acp' },
      ],
      sessions: state.sessions.map((session) => session.id === 'chat-1' ? { ...session, providerId: 'opencode-cli' } : session),
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          providerId: 'opencode-cli',
          protocolKind: 'opencode-acp',
          lifecycleState: 'ready',
          processing: false,
          processingStartedAtMs: null,
          pendingPermission: null,
		  availableModes: [],
          configOptions: [
            { id: 'theme', name: 'Theme', description: '', category: 'ui', currentValue: 'dark', options: [{ value: 'dark', name: 'Dark', description: '' }] },
			{
			  id: 'effort',
			  name: 'Effort',
			  description: 'Available effort levels for this model.',
			  category: 'thought_level',
			  currentValue: 'low',
			  options: [
				{ value: 'low', name: 'Low', description: '' },
				{ value: 'xhigh', name: 'Extra High', description: '' },
			  ],
			},
			{
			  id: 'mode',
			  name: 'Session Mode',
			  description: 'OpenCode native session agent.',
			  category: 'mode',
			  currentValue: 'build',
			  options: [
				{ value: 'build', name: 'Build', description: '' },
				{ value: 'plan', name: 'Plan', description: '' },
			  ],
			},
            {
              id: 'thought_level',
              name: 'Thinking Style',
              description: 'Provider-owned model variant.',
              category: 'custom/provider-category',
              currentValue: 'balanced.v2',
              options: [
                { value: 'balanced.v2', name: 'Balanced 2.0', description: 'Normal thinking.' },
                { value: 'deep/custom', name: 'Deep + Custom', description: 'Provider-specific value.' },
              ],
            },
          ],
        },
      },
      setAcpConfigOption,
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    openComposerOptions(host)
    expect(document.body.querySelector('button[aria-label="Theme"]')).toBeNull()
	expect(document.body.querySelector('button[aria-label="Effort"]')?.textContent).toContain('Low')
	expect(document.body.querySelector('button[aria-label="Session Mode"]')).toBeNull()
	expect(document.body.textContent).toContain('Effort')
	expect(document.body.textContent).not.toContain('Session Mode')
	expect(document.body.querySelector('button[aria-label="Provider mode"]')).toBeNull()
    const variant = document.body.querySelector('button[aria-label="Thinking Style"]') as HTMLButtonElement
    expect(variant.textContent).toContain('Balanced 2.0')
    act(() => variant.click())
    const deep = Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="option"]'))
      .find((option) => option.textContent?.startsWith('Deep + Custom')) as HTMLButtonElement
    await act(async () => { deep.click(); await Promise.resolve() })
    expect(setAcpConfigOption).toHaveBeenCalledWith('chat-1', 'thought_level', 'deep/custom')

    expect((document.body.querySelector('button[aria-label="Thinking Style"]') as HTMLButtonElement).textContent).toContain('Balanced 2.0')
    act(() => (document.body.querySelector('button[aria-label="Options"]') as HTMLButtonElement)?.click())

    const textarea = host.querySelector('textarea') as HTMLTextAreaElement
    await act(async () => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set?.call(textarea, '/variants custom')
      textarea.dispatchEvent(new Event('input', { bubbles: true }))
      await Promise.resolve()
    })
    const palette = document.body.querySelector('[aria-label="OpenCode variants"]') as HTMLElement
    expect(palette.textContent).toContain('Deep + Custom')
    expect(palette.textContent).toContain('Provider-specific value.')

    act(() => root.unmount())
    host.remove()
  })

  it('renders OpenCode Zen free model options from ACP state', () => {
    const setSessionModel = vi.fn(() => Promise.resolve(true))
    useAppStore.setState((state) => ({
      providers: [
        ...state.providers,
        { id: 'opencode-cli', name: 'OpenCode', shortName: 'OpenCode', color: '#14b8a6', description: '', outputMode: 'cli', supportsCli: true, supportsStructured: true, structuredProtocol: 'opencode-acp' },
      ],
      sessions: state.sessions.map((session) =>
        session.id === 'chat-1' ? { ...session, providerId: 'opencode-cli', modelId: '' } : session
      ),
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          providerId: 'opencode-cli',
          protocolKind: 'opencode-acp',
          lifecycleState: 'ready',
          processing: false,
          processingStartedAtMs: null,
          pendingPermission: null,
          availableModels: [
            { id: 'opencode/deepseek-v4-flash-free', name: 'DeepSeek V4 Flash Free', description: 'OpenCode Zen free model.' },
            { id: 'opencode/big-pickle', name: 'Big Pickle', description: 'OpenCode Zen limited-time stealth free model.' },
          ],
          currentModelId: '',
          agentInfo: { name: 'opencode', title: 'OpenCode', version: '0.6.6' },
        },
      },
      setSessionModel,
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    const modelButton = host.querySelector('button[aria-label="Select provider and model"]')
    act(() => {
      modelButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(document.body.textContent).toContain('DeepSeek V4 Flash Free')
    expect(document.body.textContent).toContain('OpenCode Zen free model.')
    expect(document.body.textContent).toContain('Big Pickle')
    expect(host.textContent).not.toContain('Auto 3')

    const freeButton = Array.from(document.body.querySelectorAll('button')).find((button) =>
      button.textContent?.includes('DeepSeek V4 Flash Free')
    )
    expect(freeButton).toBeTruthy()
    act(() => {
      freeButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(setSessionModel).toHaveBeenCalledWith('chat-1', 'opencode/deepseek-v4-flash-free')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('renders dynamic ACP model options and applies the selected model id', () => {
    const setSessionModel = vi.fn(() => Promise.resolve(true))
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) =>
        session.id === 'chat-1' ? { ...session, modelId: 'auto-gemini-3' } : session
      ),
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          lifecycleState: 'ready',
          processing: false,
          processingStartedAtMs: null,
          pendingPermission: null,
          currentModelId: 'auto-gemini-3',
          availableModels: [
            { id: 'auto-gemini-3', name: 'Auto 3', description: 'Gemini 3 routing' },
            { id: 'gemini-3-flash-preview', name: 'Gemini 3 Flash', description: 'Preview model' },
          ],
        },
      },
      setSessionModel,
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    const modelButton = host.querySelector('button[aria-label="Select provider and model"]')
    expect(modelButton).toBeTruthy()
    expect(modelButton?.textContent).toContain('Auto 3')

    act(() => {
      modelButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(host.textContent).toContain('Auto 3')
    expect(document.body.textContent).toContain('Gemini 3 Flash')

    const flashButton = Array.from(document.body.querySelectorAll('button')).find((button) =>
      button.textContent?.includes('Gemini 3 Flash')
    )
    expect(flashButton).toBeTruthy()
    act(() => {
      flashButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(setSessionModel).toHaveBeenCalledWith('chat-1', 'gemini-3-flash-preview')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('selects a reviewer in Architect mode and locks the active Goal model pair', async () => {
    const setSessionReviewerModel = vi.fn(() => Promise.resolve(true))
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) => session.id === 'chat-1'
        ? { ...session, modelId: 'auto-gemini-3', reviewerModelId: 'auto-gemini-3', smallModelMode: true }
        : session),
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          lifecycleState: 'ready',
          processing: false,
          pendingPermission: null,
        },
      },
      setSessionReviewerModel,
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    const setSessionModel = vi.fn(() => Promise.resolve(true))
    act(() => useAppStore.setState({ setSessionModel }))
    expect((host.querySelector('button[aria-label="Worker model is managed in Options"]') as HTMLButtonElement).disabled).toBe(true)
    openComposerOptions(host)
    const worker = document.body.querySelector('button[aria-label="Worker model"]') as HTMLButtonElement
    expect(worker.textContent).toContain('Auto 3')
    act(() => worker.click())
    const workerFlash = Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="option"]'))
      .find((option) => option.textContent?.startsWith('Gemini 3 Flash')) as HTMLButtonElement
    await act(async () => { workerFlash.click(); await Promise.resolve() })
    expect(setSessionModel).toHaveBeenCalledWith('chat-1', 'gemini-3-flash-preview')

    const reviewer = document.body.querySelector('button[aria-label="Reviewer model"]') as HTMLButtonElement
    expect(reviewer.textContent).toContain('Auto 3')
    act(() => reviewer.click())
    const flash = Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="option"]'))
      .find((option) => option.textContent?.startsWith('Gemini 3 Flash')) as HTMLButtonElement
    await act(async () => { flash.click(); await Promise.resolve() })
    expect(setSessionReviewerModel).toHaveBeenCalledWith('chat-1', 'gemini-3-flash-preview')

    act(() => useAppStore.setState({
      goalsByChatId: {
        'chat-1': [{
          id: 'goal-1', chatId: 'chat-1', objective: 'Ship it', status: 'active', executionOwner: 'uam',
          workerModelId: 'auto-gemini-3', reviewerModelId: 'gemini-3-flash-preview',
          createdAt: new Date(), updatedAt: new Date(),
        }],
      },
      activeGoalIdByChatId: { 'chat-1': 'goal-1' },
    }))

    expect((document.body.querySelector('button[aria-label="Worker model"]') as HTMLButtonElement).disabled).toBe(true)
    expect((document.body.querySelector('button[aria-label="Reviewer model"]') as HTMLButtonElement).disabled).toBe(true)
    expect((host.querySelector('button[aria-label="Worker model is managed in Options"]') as HTMLButtonElement).disabled).toBe(true)
    openComposerGoals(host)
    expect(document.body.textContent).toContain('Worker: Auto 3 · Reviewer: Gemini 3 Flash · locked')

    act(() => {
      useAppStore.setState((state) => ({
        sessions: state.sessions.map((session) => session.id === 'chat-1' ? { ...session, smallModelMode: false } : session),
        goalsByChatId: { 'chat-1': [{ ...state.goalsByChatId['chat-1'][0], status: 'paused' }] },
        activeGoalIdByChatId: { 'chat-1': null },
      }))
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })
    expect(document.body.textContent).not.toContain('Worker: Auto 3 · Reviewer: Gemini 3 Flash · locked')
    expect((host.querySelector('button[aria-label="Select provider and model"]') as HTMLButtonElement).disabled).toBe(false)

    act(() => root.unmount())
    host.remove()
  })

  it('disables the model chip while ACP is processing', () => {
    const setSessionModel = vi.fn(() => Promise.resolve(true))
    useAppStore.setState({ setSessionModel })

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    const modelButton = host.querySelector('button[aria-label="Select provider and model"]') as HTMLButtonElement | null
    expect(modelButton).toBeTruthy()
    expect(modelButton?.disabled).toBe(true)
    openComposerOptions(host)
	expect((document.body.querySelector('button[aria-label="Next turn agent"]') as HTMLButtonElement | null)?.disabled).toBe(false)
    expect(document.body.querySelector('button[aria-label="Provider mode"]')).toBeNull()
    expect((document.body.querySelector('button[aria-label="Permissions"]') as HTMLButtonElement | null)?.disabled).toBe(false)

    act(() => {
      modelButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(host.textContent).not.toContain('Gemini 3 Flash')
    expect(setSessionModel).not.toHaveBeenCalled()

    act(() => {
      root.unmount()
    })
    host.remove()
  })

	it('keeps next-turn agent available while provider mode and permissions are blocked', () => {
	  useAppStore.setState((state) => ({
		acpBindingBySessionId: {
		  ...state.acpBindingBySessionId,
		  'chat-1': {
			...state.acpBindingBySessionId['chat-1'],
			lifecycleState: 'processing',
			processing: true,
			pendingPermission: null,
		  },
		},
	  }))

	  const host = document.createElement('div')
	  document.body.appendChild(host)
	  const root = createRoot(host)
	  act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
	  openComposerOptions(host)

	  expect((document.body.querySelector('button[aria-label="Next turn agent"]') as HTMLButtonElement | null)?.disabled).toBe(false)
	  expect(document.body.querySelector('button[aria-label="Provider mode"]')).toBeNull()
	  expect((document.body.querySelector('button[aria-label="Permissions"]') as HTMLButtonElement | null)?.disabled).toBe(true)

	  act(() => root.unmount())
	  host.remove()
	})

  it('does not surface a runtime-only Plan mode as a user-selected chip', () => {
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) => session.id === 'chat-1' ? { ...session, approvalMode: 'default' } : session),
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': { ...state.acpBindingBySessionId['chat-1'], currentModeId: 'plan', processing: false },
      },
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

	expect(host.querySelector('[data-mode-chip="Provider Plan"] button')).toBeNull()

    act(() => root.unmount())
    host.remove()
  })

  it('selects provider Plan only from Provider mode and clears only its chip', () => {
    const setSessionApprovalMode = vi.fn(() => Promise.resolve(true))
    useAppStore.setState((state) => ({
      providerChatDefaults: { ...state.providerChatDefaults, 'gemini-cli': { modelId: '', approvalMode: 'default', commandSafetyTier: 'off', memoryEnabled: true, featurePreference: 'provider' } },
      sessions: state.sessions.map((session) =>
        session.id === 'chat-1' ? { ...session, approvalMode: 'default' } : session
      ),
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          lifecycleState: 'ready',
          processing: false,
          processingStartedAtMs: null,
          currentModeId: 'default',
          pendingPermission: null,
        },
      },
      setSessionApprovalMode,
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    openComposerOptions(host)
    const agentButton = document.body.querySelector('button[aria-label="Provider mode"]') as HTMLButtonElement
    expect(agentButton.textContent).toContain('Default')
    act(() => agentButton.click())
    const planOption = Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="option"]')).find((option) => option.textContent?.startsWith('Plan')) as HTMLButtonElement
    act(() => planOption.click())

    expect(setSessionApprovalMode).toHaveBeenCalledWith('chat-1', 'plan')

    act(() => {
      useAppStore.setState((state) => ({
        sessions: state.sessions.map((session) =>
          session.id === 'chat-1' ? { ...session, approvalMode: 'plan' } : session
        ),
        acpBindingBySessionId: {
          ...state.acpBindingBySessionId,
          'chat-1': {
            ...state.acpBindingBySessionId['chat-1'],
            currentModeId: 'plan',
          },
        },
      }))
    })

    expect(host.querySelector('[data-mode-chip="Provider Plan"] button')).toBeTruthy()

    act(() => {
      useAppStore.setState((state) => ({
        sessions: state.sessions.map((session) =>
          session.id === 'chat-1' ? { ...session, approvalMode: 'default' } : session
        ),
        acpBindingBySessionId: {
          ...state.acpBindingBySessionId,
          'chat-1': {
            ...state.acpBindingBySessionId['chat-1'],
            currentModeId: 'plan',
          },
        },
      }))
    })

    expect(host.querySelector('[data-mode-chip="Provider Plan"] button')).toBeNull()

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('changes Permissions without changing Plan, reasoning, speed, or memory', async () => {
    const setSessionCommandSafetyTier = vi.fn(() => Promise.resolve({ ok: true }))
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) =>
        session.id === 'chat-1' ? { ...session, approvalMode: 'plan', commandSafetyTier: 'aiReview', reasoningEffort: 'high', serviceTier: 'flex', memoryLevel: 'strict' } : session
      ),
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          lifecycleState: 'ready',
          processing: false,
          processingStartedAtMs: null,
          currentModeId: 'plan',
          availableModes: [
            { id: 'default', name: 'Default', description: '' },
            { id: 'plan', name: 'Plan', description: '' },
          ],
          pendingPermission: null,
        },
      },
      setSessionCommandSafetyTier,
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    openComposerOptions(host)
    const permissionButton = document.body.querySelector('button[aria-label="Permissions"]') as HTMLButtonElement
    act(() => permissionButton.click())
    const yoloOption = Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="option"]')).find((option) => option.textContent?.startsWith('YOLO')) as HTMLButtonElement
    await act(async () => { yoloOption.click(); await Promise.resolve() })
    expect(host.textContent).toContain('Enable YOLO?')
    await act(async () => { (Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Enable YOLO') as HTMLButtonElement).click(); await Promise.resolve() })
    expect(setSessionCommandSafetyTier).toHaveBeenCalledWith('chat-1', 'yolo')

    act(() => {
      useAppStore.setState((state) => ({
        sessions: state.sessions.map((session) =>
          session.id === 'chat-1' ? { ...session, commandSafetyTier: 'yolo' } : session
        ),
      }))
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    const yoloChip = host.querySelector('[data-mode-chip="YOLO"] button') as HTMLButtonElement
    expect(yoloChip).toBeTruthy()
    act(() => yoloChip.click())
    expect(setSessionCommandSafetyTier).toHaveBeenLastCalledWith('chat-1', 'yolo')
    const defaultPermission = Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="listbox"][aria-label="Permissions"] [role="option"]')).find((option) => option.textContent?.startsWith('Default'))!
    act(() => defaultPermission.click())
    expect(setSessionCommandSafetyTier).toHaveBeenLastCalledWith('chat-1', 'off')
    expect(useAppStore.getState().sessions[0]).toMatchObject({ approvalMode: 'plan', reasoningEffort: 'high', serviceTier: 'flex', memoryLevel: 'strict' })

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('switches Claude from plan mode back to default', () => {
    const setSessionApprovalMode = vi.fn(() => Promise.resolve(true))
    useAppStore.setState((state) => ({
      providerChatDefaults: { ...state.providerChatDefaults, 'claude-cli': { modelId: '', approvalMode: 'default', commandSafetyTier: 'off', memoryEnabled: true, featurePreference: 'provider' } },
      sessions: state.sessions.map((session) =>
        session.id === 'chat-1' ? { ...session, providerId: 'claude-cli', approvalMode: 'plan' } : session
      ),
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          providerId: 'claude-cli',
          protocolKind: 'claude-code-stream-json',
          lifecycleState: 'ready',
          processing: false,
          processingStartedAtMs: null,
          availableModes: [
            { id: 'default', name: 'Default', description: '' },
            { id: 'acceptEdits', name: 'Accept Edits', description: '' },
            { id: 'plan', name: 'Plan', description: '' },
          ],
          currentModeId: 'plan',
          pendingPermission: null,
        },
      },
      setSessionApprovalMode,
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    expect(host.textContent).not.toContain('Limited structured support')

    const planButton = host.querySelector('[data-mode-chip="Provider Plan"] button') as HTMLButtonElement | null
    expect(planButton).toBeTruthy()

    act(() => {
      planButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(setSessionApprovalMode).not.toHaveBeenCalled()
    const defaultMode = Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="listbox"][aria-label="Provider mode"] [role="option"]')).find((option) => option.textContent?.startsWith('Default'))!
    act(() => defaultMode.click())
    expect(setSessionApprovalMode).toHaveBeenCalledWith('chat-1', 'default')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('allows UAM to control Claude structured permissions', () => {
    const setSessionApprovalMode = vi.fn(() => Promise.resolve(true))
    const setSessionCommandSafetyTier = vi.fn(() => Promise.resolve({ ok: true }))
    useAppStore.setState((state) => ({
      providerChatDefaults: { ...state.providerChatDefaults, 'claude-cli': { modelId: '', approvalMode: 'default', commandSafetyTier: 'off', memoryEnabled: true, featurePreference: 'provider' } },
      sessions: state.sessions.map((session) =>
        session.id === 'chat-1'
          ? { ...session, providerId: 'claude-cli', approvalMode: 'plan', commandSafetyTier: 'off', reasoningEffort: 'high', serviceTier: 'flex', memoryLevel: 'strict' }
          : session
      ),
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          providerId: 'claude-cli',
          protocolKind: 'claude-code-stream-json',
          lifecycleState: 'ready',
          processing: false,
          processingStartedAtMs: null,
          availableModes: [
            { id: 'default', name: 'Default', description: '' },
            { id: 'acceptEdits', name: 'Accept Edits', description: '' },
            { id: 'plan', name: 'Plan', description: '' },
          ],
          currentModeId: 'plan',
          pendingPermission: null,
        },
      },
      setSessionApprovalMode,
      setSessionCommandSafetyTier,
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    openComposerOptions(host)
    const agentButton = document.body.querySelector('button[aria-label="Provider mode"]') as HTMLButtonElement
    act(() => agentButton.click())
    expect(Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="option"]')).some((option) => option.textContent?.startsWith('Accept Edits'))).toBe(false)
    act(() => agentButton.click())

    const permissionButton = document.body.querySelector('button[aria-label="Permissions"]') as HTMLButtonElement
    expect(permissionButton.textContent).toContain('Default')
    expect(permissionButton.disabled).toBe(false)
    act(() => permissionButton.click())
    const reviewOption = Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="option"]')).find((option) => option.textContent?.startsWith('AI Review'))!
    act(() => reviewOption.click())
    expect(document.body.textContent).not.toContain('Accept Edits')
    expect(setSessionCommandSafetyTier).toHaveBeenCalledWith('chat-1', 'aiReview')
    expect(setSessionApprovalMode).not.toHaveBeenCalled()

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('asks how to proceed before sending a prompt from Claude plan mode', async () => {
    const setSessionApprovalMode = vi.fn(() => Promise.resolve(true))
    const setSessionCommandSafetyTier = vi.fn(() => Promise.resolve({ ok: true }))
    const sendAcpPrompt = vi.fn(() => Promise.resolve(true))
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) =>
        session.id === 'chat-1' ? { ...session, providerId: 'claude-cli', approvalMode: 'plan' } : session
      ),
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          providerId: 'claude-cli',
          protocolKind: 'claude-code-stream-json',
          lifecycleState: 'ready',
          processing: false,
          processingStartedAtMs: null,
          availableModes: [
            { id: 'default', name: 'Default', description: '' },
            { id: 'acceptEdits', name: 'Accept Edits', description: '' },
            { id: 'plan', name: 'Plan', description: '' },
          ],
          currentModeId: 'plan',
          pendingPermission: null,
        },
      },
      setSessionApprovalMode,
      setSessionCommandSafetyTier,
      sendAcpPrompt,
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    const textarea = host.querySelector('textarea') as HTMLTextAreaElement | null
    expect(textarea).toBeTruthy()
    act(() => {
      const valueSetter = Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set
      valueSetter?.call(textarea, 'proceed')
      textarea!.dispatchEvent(new Event('input', { bubbles: true }))
    })

    const form = host.querySelector('form') as HTMLFormElement | null
    await act(async () => {
      form?.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true }))
    })

    expect(sendAcpPrompt).not.toHaveBeenCalled()
    expect(host.textContent).toContain('Claude Plan mode is read-only')

    const acceptAndProceed = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Accept edits and proceed') as HTMLButtonElement | undefined
    expect(acceptAndProceed).toBeTruthy()
    await act(async () => {
      acceptAndProceed?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(setSessionApprovalMode).toHaveBeenCalledWith('chat-1', 'default')
    expect(setSessionCommandSafetyTier).toHaveBeenCalledWith('chat-1', 'acceptEdits')
    expect(sendAcpPrompt).toHaveBeenCalledWith('chat-1', 'proceed', undefined, undefined, expect.any(Function))

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('drops a held Claude plan prompt when its provider context changes', async () => {
    const sendAcpPrompt = vi.fn(() => Promise.resolve(true))
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) => session.id === 'chat-1' ? { ...session, providerId: 'claude-cli', approvalMode: 'plan' } : session),
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': { ...state.acpBindingBySessionId['chat-1'], providerId: 'claude-cli', protocolKind: 'claude-code-stream-json', lifecycleState: 'ready', processing: false, currentModeId: 'plan' },
      },
      sendAcpPrompt,
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    const textarea = host.querySelector('textarea') as HTMLTextAreaElement
    act(() => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set?.call(textarea, 'held prompt')
      textarea.dispatchEvent(new Event('input', { bubbles: true }))
    })
    await act(async () => { (host.querySelector('form') as HTMLFormElement).dispatchEvent(new Event('submit', { bubbles: true, cancelable: true })) })
    expect(host.textContent).toContain('Claude Plan mode is read-only')

    const changed = { ...useAppStore.getState().sessions[0], providerId: 'gemini-cli', approvalMode: 'default' }
    act(() => root.render(<ChatView session={changed} />))
    expect(host.textContent).not.toContain('Claude Plan mode is read-only')
    expect(sendAcpPrompt).not.toHaveBeenCalled()

    act(() => root.unmount())
    host.remove()
  })

  it('arms the next message as a goal and sends the same text with the default budget', async () => {
    let finishGoal: (result: { ok: boolean; goalId: string }) => void = () => {}
    const setGoal = vi.fn(() => new Promise<{ ok: boolean; goalId: string }>((resolve) => { finishGoal = resolve }))
    const sendAcpPrompt = vi.fn(() => Promise.resolve(true))
    useAppStore.setState((state) => ({
      setGoal,
      sendAcpPrompt,
      defaultGoalTokenBudgetByChatId: { ...state.defaultGoalTokenBudgetByChatId, 'chat-1': 1234 },
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          lifecycleState: 'ready',
          processing: false,
          processingStartedAtMs: null,
          pendingPermission: null,
          turnEvents: [],
        },
      },
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    openComposerOptions(host)
    const goalButton = document.body.querySelector('button[title="Use the next message as a goal"]') as HTMLButtonElement | null
    expect(goalButton).toBeTruthy()
    act(() => {
      goalButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })
    expect(host.querySelector('[data-mode-chip="Goal: next message"]')).toBeTruthy()

    const textarea = host.querySelector('textarea') as HTMLTextAreaElement | null
    act(() => {
      const valueSetter = Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set
      valueSetter?.call(textarea, 'Ship the goal loop')
      textarea!.dispatchEvent(new Event('input', { bubbles: true }))
    })

    const form = host.querySelector('form') as HTMLFormElement | null
    act(() => {
      form?.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true }))
      form?.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true }))
    })

	expect(setGoal).toHaveBeenCalledWith('chat-1', 'Ship the goal loop', 1234, 'uam')
    expect(setGoal).toHaveBeenCalledTimes(1)
    expect(sendAcpPrompt).not.toHaveBeenCalled()
    await act(async () => {
      finishGoal({ ok: true, goalId: 'goal-1' })
      await Promise.resolve()
    })
    expect(sendAcpPrompt).toHaveBeenCalledWith('chat-1', 'Ship the goal loop', [], undefined, expect.any(Function))

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('refreshes the /goal action when an active goal arrives after mount', async () => {
    const updateGoalStatus = vi.fn(() => Promise.resolve({ ok: true }))
    useAppStore.setState({ updateGoalStatus })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    act(() => useAppStore.setState((state) => ({
      goalsByChatId: {
        ...state.goalsByChatId,
        'chat-1': [{
          id: 'late-goal', chatId: 'chat-1', objective: 'Late goal', status: 'active' as const,
          tokenBudget: 0, tokensUsed: 0, blockedTurnCount: 0, completedItems: [], remainingItems: [],
          currentStep: '', createdAt: new Date(), updatedAt: new Date(),
        }],
      },
      activeGoalIdByChatId: { ...state.activeGoalIdByChatId, 'chat-1': 'late-goal' },
    })))

    const textarea = host.querySelector('textarea') as HTMLTextAreaElement
    await act(async () => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set?.call(textarea, '/goal')
      textarea.dispatchEvent(new Event('input', { bubbles: true }))
      textarea.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true }))
      await Promise.resolve()
    })

    expect(updateGoalStatus).toHaveBeenCalledWith('chat-1', 'late-goal', 'paused')
    expect(host.textContent).not.toContain('Goal: next message')
    act(() => root.unmount())
    host.remove()
  })

  it('blocks banner mutations while a new goal is being created', () => {
    const setGoal = vi.fn(() => new Promise<{ ok: boolean; goalId: string }>(() => {}))
    const removeGoal = vi.fn(() => Promise.resolve({ ok: true }))
    useAppStore.setState((state) => ({
      setGoal,
      removeGoal,
      goalsByChatId: {
        'chat-1': [{
          id: 'goal-existing', chatId: 'chat-1', objective: 'Existing goal', status: 'active' as const,
          tokenBudget: 0, tokensUsed: 0, blockedTurnCount: 0, completedItems: [], remainingItems: [],
          currentStep: '', createdAt: new Date(), updatedAt: new Date(),
        }],
      },
      activeGoalIdByChatId: { 'chat-1': 'goal-existing' },
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': { ...state.acpBindingBySessionId['chat-1'], lifecycleState: 'ready', processing: false },
      },
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    const textarea = host.querySelector('textarea')!
    act(() => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set?.call(textarea, '/goal Replacement goal')
      textarea.dispatchEvent(new Event('input', { bubbles: true }))
      host.querySelector('form')?.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true }))
    })
    expect(setGoal).toHaveBeenCalledTimes(1)

    openComposerGoals(host)
    act(() => (document.body.querySelector('button[aria-label="Goal actions"]') as HTMLButtonElement).click())
    const deleteGoal = Array.from(document.body.querySelectorAll('button')).find((button) => button.textContent === 'Delete goal') as HTMLButtonElement
    expect(deleteGoal.disabled).toBe(true)
    act(() => deleteGoal.click())
    expect(removeGoal).not.toHaveBeenCalled()

    act(() => root.unmount())
    host.remove()
  })

	it('delegates a configured provider goal command exactly once', async () => {
	  let finishGoal: (result: { ok: boolean; goalId: string }) => void = () => {}
	  const setGoal = vi.fn(() => new Promise<{ ok: boolean; goalId: string }>((resolve) => { finishGoal = resolve }))
	  const sendAcpPrompt = vi.fn(() => Promise.resolve(true))
	  useAppStore.setState((state) => ({
		setGoal,
		sendAcpPrompt,
		providerChatDefaults: { ...state.providerChatDefaults, 'codex-cli': { modelId: '', approvalMode: 'default', commandSafetyTier: 'off', memoryEnabled: true, featurePreference: 'provider' } },
		defaultGoalTokenBudgetByChatId: { ...state.defaultGoalTokenBudgetByChatId, 'chat-1': 0 },
		providers: state.providers.map((provider) => provider.id === 'codex-cli' ? { ...provider, nativeGoalCommand: '/ralph' } : provider),
		sessions: state.sessions.map((session) => session.id === 'chat-1' ? { ...session, providerId: 'codex-cli' } : session),
	  }))

	  const host = document.createElement('div')
	  document.body.appendChild(host)
	  const root = createRoot(host)
	  act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
	  const textarea = host.querySelector('textarea')!
	  act(() => {
		const valueSetter = Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set
		valueSetter?.call(textarea, '/goal Keep the objective literal')
		textarea.dispatchEvent(new Event('input', { bubbles: true }))
	  })
	  act(() => {
		host.querySelector('form')?.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true }))
		host.querySelector('form')?.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true }))
	  })

	  expect(setGoal).toHaveBeenCalledWith('chat-1', 'Keep the objective literal', 0, 'provider')
	  expect(setGoal).toHaveBeenCalledTimes(1)
	  expect(sendAcpPrompt).not.toHaveBeenCalled()
	  await act(async () => {
		finishGoal({ ok: true, goalId: 'goal-native' })
		await Promise.resolve()
	  })
	  expect(sendAcpPrompt).toHaveBeenCalledTimes(1)
	  expect(sendAcpPrompt).toHaveBeenCalledWith('chat-1', '/ralph Keep the objective literal', [], undefined, expect.any(Function))
	  act(() => root.unmount())
	  host.remove()
	})

	it('keeps provider goal input usable when creation rejects', async () => {
	  const setGoal = vi.fn(() => Promise.reject(new Error('bridge closed')))
	  useAppStore.setState((state) => ({
		setGoal,
		providerChatDefaults: { ...state.providerChatDefaults, 'codex-cli': { modelId: '', approvalMode: 'default', commandSafetyTier: 'off', memoryEnabled: true, featurePreference: 'provider' } },
		providers: state.providers.map((provider) => provider.id === 'codex-cli' ? { ...provider, nativeGoalCommand: '/ralph' } : provider),
		sessions: state.sessions.map((session) => session.id === 'chat-1' ? { ...session, providerId: 'codex-cli' } : session),
	  }))

	  const host = document.createElement('div')
	  document.body.appendChild(host)
	  const root = createRoot(host)
	  act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
	  const textarea = host.querySelector('textarea')!
	  act(() => {
		Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set?.call(textarea, '/ralph Keep this draft')
		textarea.dispatchEvent(new Event('input', { bubbles: true }))
	  })
	  await act(async () => {
		host.querySelector('form')?.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true }))
		await Promise.resolve()
	  })

	  expect(setGoal).toHaveBeenCalledTimes(1)
	  expect(host.textContent).toContain('Failed to create goal.')
	  expect(textarea.value).toBe('/ralph Keep this draft')
	  expect(textarea.disabled).toBe(false)
	  act(() => root.unmount())
	  host.remove()
	})

	it('submits a multi-line /goal command', async () => {
	  const setGoal = vi.fn(() => Promise.resolve({ ok: true, goalId: 'goal-1' }))
	  const sendAcpPrompt = vi.fn(() => Promise.resolve(true))
	  useAppStore.setState({ setGoal, sendAcpPrompt })

	  const host = document.createElement('div')
	  document.body.appendChild(host)
	  const root = createRoot(host)
	  act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

	  const textarea = host.querySelector('textarea')!
	  act(() => {
		const valueSetter = Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set
		valueSetter?.call(textarea, '/goal First line.\n\nSecond line.')
		textarea.dispatchEvent(new Event('input', { bubbles: true }))
	  })
	  await act(async () => {
		host.querySelector('form')?.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true }))
	  })

	  expect(setGoal).toHaveBeenCalledWith('chat-1', 'First line.\n\nSecond line.', 0, 'uam')
	  expect(sendAcpPrompt).toHaveBeenCalledWith('chat-1', 'First line.\n\nSecond line.', [], undefined, expect.any(Function))
	  act(() => root.unmount())
	  host.remove()
	})

  it('renders active goal pause/delete controls and paused goal resume control', async () => {
    const updateGoalStatus = vi.fn(() => Promise.resolve({ ok: true }))
    let finishRemove: ((result: { ok: boolean; error?: string }) => void) | undefined
    const removeGoal = vi.fn(() => new Promise<{ ok: boolean; error?: string }>((resolve) => { finishRemove = resolve }))
    let finishResume: ((result: { ok: boolean; error?: string }) => void) | undefined
    const resumeGoal = vi.fn(() => new Promise<{ ok: boolean; error?: string }>((resolve) => { finishResume = resolve }))
    const goal = {
      id: 'goal-1',
      chatId: 'chat-1',
      objective: 'Review the Ralph loop',
      status: 'active' as const,
      tokenBudget: 0,
      tokensUsed: 0,
      blockedTurnCount: 0,
      completedItems: ['Audit messages'],
      remainingItems: ['Implement editing', 'Run tests'],
      currentStep: 'Implement editing',
      createdAt: new Date('2026-01-01T00:00:00.000Z'),
      updatedAt: new Date('2026-01-01T00:00:00.000Z'),
    }
    useAppStore.setState((state) => ({
      updateGoalStatus,
      removeGoal,
      resumeGoal,
      goalsByChatId: { ...state.goalsByChatId, 'chat-1': [goal] },
      activeGoalIdByChatId: { ...state.activeGoalIdByChatId, 'chat-1': 'goal-1' },
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    openComposerGoals(host)
    const progress = document.body.querySelector('progress[aria-label="Goal progress"]') as HTMLProgressElement | null
    expect(progress?.max).toBe(3)
    expect(progress?.value).toBe(1)
    expect(document.querySelector('[aria-label="Chat goals"]')?.textContent).toContain('Implement editing')

    // Goal actions are consolidated into an overflow menu; open it to reach them.
    const openGoalMenu = () => { openComposerGoals(host); act(() => {
      (document.body.querySelector('button[aria-label="Goal actions"]') as HTMLButtonElement | null)
        ?.dispatchEvent(new MouseEvent('mousedown', { bubbles: true }))
      ;(document.body.querySelector('button[aria-label="Goal actions"]') as HTMLButtonElement | null)
        ?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    }) }
    const menuItem = (text: string) =>
      Array.from(document.body.querySelectorAll('button')).find((b) => b.textContent === text) as HTMLButtonElement | undefined

    openGoalMenu()
    expect(menuItem('Mark complete')).toBeTruthy()
    const pauseButton = menuItem('Pause goal')
    expect(pauseButton).toBeTruthy()
    expect(menuItem('Delete goal')).toBeTruthy()

    act(() => {
      pauseButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })
    expect(updateGoalStatus).toHaveBeenCalledWith('chat-1', 'goal-1', 'paused')
    await act(async () => { await Promise.resolve() })

    act(() => {
      useAppStore.setState((state) => ({
        goalsByChatId: { ...state.goalsByChatId, 'chat-1': [{ ...goal, status: 'paused' as const }] },
        activeGoalIdByChatId: { ...state.activeGoalIdByChatId, 'chat-1': null },
      }))
    })

    openGoalMenu()
    const resumeButton = menuItem('Resume goal')
    expect(resumeButton).toBeTruthy()
    act(() => {
      resumeButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })
    expect(resumeGoal).toHaveBeenCalledWith('chat-1', 'goal-1')
    openGoalMenu()
    const pendingResume = menuItem('Resuming…')
    expect(pendingResume?.disabled).toBe(true)
    await act(async () => {
      finishResume?.({ ok: false })
      await Promise.resolve()
    })
    expect(host.textContent).toContain('Failed to resume goal.')

    act(() => {
      menuItem('Delete goal')?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })
    expect(removeGoal).toHaveBeenCalledWith('chat-1', 'goal-1')
    openGoalMenu()
    act(() => {
      menuItem('Delete goal')?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })
    expect(removeGoal).toHaveBeenCalledTimes(1)
    openGoalMenu()
    await act(async () => {
      finishRemove?.({ ok: true })
      await Promise.resolve()
    })

    act(() => {
      useAppStore.setState((state) => ({
        goalsByChatId: { ...state.goalsByChatId, 'chat-1': [{ ...goal, status: 'complete' as const }] },
        activeGoalIdByChatId: { ...state.activeGoalIdByChatId, 'chat-1': null },
        acpBindingBySessionId: {
          ...state.acpBindingBySessionId,
          'chat-1': {
            ...state.acpBindingBySessionId['chat-1'],
            lifecycleState: 'ready',
            processing: false,
            pendingPermission: null,
          },
        },
      }))
    })
    openGoalMenu()
    expect(menuItem('Resume goal')).toBeUndefined()
    expect(menuItem('Delete goal')).toBeTruthy()

    act(() => {
      menuItem('Delete goal')?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })
    expect(removeGoal).toHaveBeenLastCalledWith('chat-1', 'goal-1')

    openComposerOptions(host)
    const newGoalButton = document.body.querySelector('button[title="Use the next message as a goal"]') as HTMLButtonElement | null
    expect(newGoalButton).toBeTruthy()
    act(() => {
      newGoalButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })
    expect(resumeGoal).toHaveBeenCalledTimes(1)
    expect(host.querySelector('[data-mode-chip="Goal: next message"]')).toBeTruthy()

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('keeps the composer branch display-only unless the backend advertises branch switching', async () => {
    const getVcsCommitStatus = vi.fn(() => Promise.resolve({
      available: true, vcsTypes: ['git'], activeVcsType: 'git', workspaceDirectory: '/tmp/project',
      branchOrRevision: 'feature/composer', changedFiles: [], lineStatsReady: true, warning: '', error: '',
    }))
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) => session.id === 'chat-1' ? { ...session, workspaceDirectory: '/tmp/project', workspaceIsolationKind: 'gitWorktree', workspaceBranchName: '' } : session),
      getVcsCommitStatus,
      branchSwitching: false,
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    await act(async () => { root.render(<ChatView session={useAppStore.getState().sessions[0]} />) })
    expect(host.querySelector('.uam-composer-workspace-tab button[aria-haspopup="dialog"]')).toBeNull()
    expect(host.querySelector('.uam-composer-workspace-tab')?.textContent).toContain('feature/composer')
    await act(async () => { useAppStore.setState({ branchSwitching: true }) })
    expect(host.querySelector('.uam-composer-workspace-tab button[aria-haspopup="dialog"]')?.textContent).toContain('feature/composer')
    act(() => root.unmount())
    host.remove()
    useAppStore.setState({ branchSwitching: undefined })
  })

  it('reads the repository branch for the composer tab once when the chat opens', async () => {
    const getVcsCommitStatus = vi.fn(() => Promise.resolve({
      available: true, vcsTypes: ['git'], activeVcsType: 'git', workspaceDirectory: '/tmp/project',
      branchOrRevision: 'feature/composer', changedFiles: [], lineStatsReady: true, warning: '', error: '',
    }))
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) => session.id === 'chat-1' ? { ...session, workspaceDirectory: '/tmp/project', workspaceBranchName: '' } : session),
      getVcsCommitStatus,
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    await act(async () => { root.render(<ChatView session={useAppStore.getState().sessions[0]} />) })
    expect(getVcsCommitStatus).toHaveBeenCalledTimes(1)
    expect(getVcsCommitStatus).toHaveBeenCalledWith('chat-1', 'git', expect.objectContaining({ contextOnly: true, includeLineStats: false }))
    expect(host.querySelector('.uam-composer-workspace-tab')?.textContent).toContain('feature/composer')
    act(() => root.unmount())
    host.remove()
  })

  it('shows the source workspace name before the branch while preserving the worktree path', async () => {
    const directory = '/tmp/worktrees/chat-random'
    useAppStore.setState((state) => ({
      folders: state.folders.map((folder) => ({ ...folder, name: 'Original project' })),
      sessions: state.sessions.map((session) => session.id === 'chat-1' ? { ...session, workspaceDirectory: directory, workspaceSourceDirectory: '/tmp/project', workspaceIsolationKind: 'gitWorktree' } : session),
      getVcsCommitStatus: vi.fn(async (): Promise<VcsCommitStatus> => ({ available: true, vcsTypes: ['git'], activeVcsType: 'git', workspaceDirectory: directory, branchOrRevision: 'feature/composer', changedFiles: [], lineStatsReady: true, warning: '', error: '' })),
    }))
    const host = document.createElement('div')
    document.body.append(host)
    const root = createRoot(host)
    try {
      await act(async () => { root.render(<ChatView session={useAppStore.getState().sessions[0]} />) })
      const tab = host.querySelector('.uam-composer-workspace-tab')!
      expect(tab.textContent).toContain('Original projectfeature/composer')
      expect(tab.textContent).not.toContain('chat-random')
      expect(tab.querySelector('[title="/tmp/worktrees/chat-random"]')).toBeTruthy()
      expect(useAppStore.getState().sessions[0].workspaceDirectory).toBe(directory)
      act(() => useAppStore.setState((state) => ({ folders: state.folders.map((folder) => ({ ...folder, name: 'Renamed project' })) })))
      expect(tab.textContent).toContain('Renamed projectfeature/composer')
      expect(useAppStore.getState().sessions[0].workspaceDirectory).toBe(directory)
    } finally { act(() => root.unmount()); host.remove() }
  })

  it.each([
    { label: 'plain workspace', workspaceIsolationKind: '', executionHostId: 'local', temporaryParentChatId: undefined },
    { label: 'remote worktree', workspaceIsolationKind: 'gitWorktree', executionHostId: 'remote', temporaryParentChatId: undefined },
    { label: 'side chat', workspaceIsolationKind: 'gitWorktree', executionHostId: 'local', temporaryParentChatId: 'parent' },
  ])('keeps the branch read-only for a $label despite the global capability', async (context) => {
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) => session.id === 'chat-1' ? { ...session, ...context, workspaceBranchName: 'feature/composer' } : session),
      branchSwitching: true,
    }))
    const host = document.createElement('div')
    document.body.append(host)
    const root = createRoot(host)
    try {
      await act(async () => { root.render(<ChatView session={useAppStore.getState().sessions[0]} />) })
      expect(host.querySelector('.uam-composer-workspace-tab button[aria-haspopup="dialog"]')).toBeNull()
      expect(host.querySelector('.uam-composer-workspace-tab')?.textContent).toContain('feature/composer')
    } finally { act(() => root.unmount()); host.remove() }
  })

  it('displays an SVN revision without offering Git selection or a stale saved branch', async () => {
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) => session.id === 'chat-1' ? { ...session, workspaceIsolationKind: 'gitWorktree', workspaceBranchName: 'old-git-branch' } : session),
      branchSwitching: true,
      getVcsCommitStatus: vi.fn(async (): Promise<VcsCommitStatus> => ({ available: true, vcsTypes: ['svn'], activeVcsType: 'svn', workspaceDirectory: '/tmp/project', branchOrRevision: '42', changedFiles: [], lineStatsReady: true, warning: '', error: '' })),
    }))
    const host = document.createElement('div')
    document.body.append(host)
    const root = createRoot(host)
    try {
      await act(async () => { root.render(<ChatView session={useAppStore.getState().sessions[0]} />) })
      const tab = host.querySelector('.uam-composer-workspace-tab')!
      expect(tab.querySelector('button[aria-haspopup="dialog"]')).toBeNull()
      expect(tab.querySelector('[aria-label="SVN revision 42"]')?.textContent).toBe('SVN r42')
      expect(tab.textContent).not.toContain('old-git-branch')
    } finally { act(() => root.unmount()); host.remove() }
  })

  it('offers SVN folder selection for an editable plain local working copy', async () => {
    useAppStore.setState({
      branchSwitching: true,
      getVcsCommitStatus: vi.fn(async (): Promise<VcsCommitStatus> => ({ available: true, vcsTypes: ['svn'], activeVcsType: 'svn', workspaceDirectory: '/tmp/project', branchOrRevision: '42', changedFiles: [], lineStatsReady: true, warning: '', error: '' })),
    })
    const host = document.createElement('div')
    document.body.append(host)
    const root = createRoot(host)
    try {
      await act(async () => { root.render(<ChatView session={useAppStore.getState().sessions[0]} />) })
      const trigger = host.querySelector('.uam-composer-workspace-tab button[aria-haspopup="dialog"]')
      expect(trigger?.getAttribute('aria-label')).toBe('Switch SVN folder, current revision 42')
      expect(trigger?.textContent).toBe('SVN r42')
    } finally { act(() => root.unmount()); host.remove() }
  })

  it('clears a stale saved branch when the current workspace has no repository', async () => {
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) => session.id === 'chat-1' ? { ...session, workspaceIsolationKind: 'gitWorktree', workspaceBranchName: 'old-git-branch' } : session),
      branchSwitching: true,
      getVcsCommitStatus: vi.fn(async (): Promise<VcsCommitStatus> => ({ available: false, vcsTypes: [], activeVcsType: 'git', workspaceDirectory: '/tmp/project', branchOrRevision: '', changedFiles: [], lineStatsReady: true, warning: '', error: '' })),
    }))
    const host = document.createElement('div')
    document.body.append(host)
    const root = createRoot(host)
    try {
      await act(async () => { root.render(<ChatView session={useAppStore.getState().sessions[0]} />) })
      expect(host.querySelector('.uam-composer-workspace-tab')?.textContent).not.toContain('old-git-branch')
      expect(host.querySelector('.uam-composer-workspace-tab button[aria-haspopup="dialog"]')).toBeNull()
    } finally { act(() => root.unmount()); host.remove() }
  })

  it('selects the per-chat memory level from options and /memory', async () => {
    const setSessionMemoryLevel = vi.fn((_id: string, level: 'off' | 'strict' | 'balanced' | 'open') => {
      useAppStore.setState((state) => ({ sessions: state.sessions.map((candidate) => candidate.id === 'chat-1' ? { ...candidate, memoryEnabled: level !== 'off', memoryLevel: level } : candidate) }))
      return Promise.resolve(true)
    })
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) =>
        session.id === 'chat-1' ? { ...session, memoryEnabled: true, memoryLevel: 'strict' as const } : session
      ),
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          lifecycleState: 'ready',
          processing: false,
          processingStartedAtMs: null,
          pendingPermission: null,
        },
      },
      setSessionMemoryLevel,
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    openComposerOptions(host)
    const memoryButton = document.body.querySelector('button[aria-label="Memory"]') as HTMLButtonElement | null
    expect(memoryButton).toBeTruthy()
    expect(memoryButton?.disabled).toBe(false)

    act(() => {
      memoryButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    const balanced = Array.from(document.body.querySelectorAll('button[role="option"]')).find((button) => button.textContent?.includes('Balanced'))
    act(() => {
      balanced?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(setSessionMemoryLevel).toHaveBeenCalledWith('chat-1', 'balanced')

    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    expect(host.querySelector('[data-mode-chip="Memory On"]')?.textContent).toContain('Balanced')

    act(() => { (document.body.querySelector('button[aria-label="Memory"]') as HTMLButtonElement).click() })
    const strict = Array.from(document.body.querySelectorAll('button[role="option"]')).find((button) => button.textContent?.includes('Strict')) as HTMLButtonElement
    act(() => strict.click())
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    expect(host.querySelector('[data-mode-chip="Memory On"]')?.textContent).toContain('Strict')

    const textarea = host.querySelector('textarea') as HTMLTextAreaElement
    await act(async () => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set?.call(textarea, '/memory open')
      textarea.dispatchEvent(new Event('input', { bubbles: true }))
      textarea.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true }))
      await Promise.resolve()
    })
    expect(setSessionMemoryLevel).toHaveBeenLastCalledWith('chat-1', 'open')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  // Markdown rendering (tables, code fences, inline links) is covered directly in
  // components/markdown/Markdown.test.tsx and markdownParsing.test.ts (FE-3 extraction).

  it('renders persisted assistant thoughts when no active ACP timeline is available', () => {
    useAppStore.setState((state) => {
      const currentMessages = state.messages['chat-1'] ?? []
      return {
        messages: {
          ...state.messages,
          'chat-1': currentMessages.map((message) =>
            message.id === 'm-2' ? { ...message, thoughts: 'Persisted reasoning\nwith detail.' } : message
          ),
        },
        acpBindingBySessionId: {
          ...state.acpBindingBySessionId,
          'chat-1': {
            ...state.acpBindingBySessionId['chat-1'],
            lifecycleState: 'ready',
            processing: false,
            processingStartedAtMs: null,
            turnEvents: [],
            turnUserMessageIndex: -1,
            turnAssistantMessageIndex: -1,
            pendingPermission: null,
          },
        },
      }
    })

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    expect(host.textContent).toContain('Thoughts')
    expect(host.textContent).toContain('Persisted reasoning')
    expect(host.textContent).toContain('with detail.')
    expect(host.textContent).toContain('Before tool. After tool.')
    const thinkingBlock = host.querySelector('[data-testid="thinking-block"]') as HTMLDetailsElement | null
    expect(host.querySelectorAll('[data-testid="thinking-block"]')).toHaveLength(1)
    expect(thinkingBlock?.tagName).toBe('DETAILS')
    expect(thinkingBlock?.textContent).toContain('Persisted reasoning')
    expect(thinkingBlock?.textContent).toContain('with detail.')
    expect(thinkingBlock?.hasAttribute('open')).toBe(false)
    expect(host.querySelectorAll('[data-testid="thinking-block"]')).toHaveLength(1)

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('does not rebuild historical message rows when the streaming tail changes', () => {
    let historicalContentReads = 0
    const state = useAppStore.getState()
    const historicalMessage = { ...state.messages['chat-1'][1], id: 'm-history' }
    Object.defineProperty(historicalMessage, 'content', {
      configurable: true,
      enumerable: true,
      get: () => {
        historicalContentReads += 1
        return 'Stable historical response.'
      },
    })
    const streamingMessage = {
      id: 'm-streaming',
      sessionId: 'chat-1',
      role: 'assistant' as const,
      content: 'Streaming response one.',
      isStreaming: true,
      createdAt: new Date('2026-01-01T00:00:02.000Z'),
    }
    useAppStore.setState({
      messages: {
        ...state.messages,
        'chat-1': [state.messages['chat-1'][0], historicalMessage, streamingMessage],
      },
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          lifecycleState: 'ready',
          processing: false,
          processingStartedAtMs: null,
          turnEvents: [],
          turnUserMessageIndex: -1,
          turnAssistantMessageIndex: -1,
          pendingPermission: null,
        },
      },
    })

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })
    const readsAfterInitialRender = historicalContentReads
    expect(readsAfterInitialRender).toBeGreaterThan(0)

    act(() => useAppStore.setState((current) => ({
      messages: {
        ...current.messages,
        'chat-1': [
          current.messages['chat-1'][0],
          historicalMessage,
          { ...streamingMessage, content: 'Streaming response two.' },
        ],
      },
    })))

    expect(host.textContent).toContain('Streaming response two.')
    expect(historicalContentReads).toBe(readsAfterInitialRender)

    act(() => root.unmount())
    host.remove()
  })

  it('renders persisted assistant tool calls after ACP turn state is gone', () => {
    useAppStore.setState((state) => {
      const currentMessages = state.messages['chat-1'] ?? []
      return {
        messages: {
          ...state.messages,
          'chat-1': currentMessages.map((message) =>
            message.id === 'm-2'
              ? {
                  ...message,
                  thoughts: '',
                  toolCalls: [
                    {
                      id: 'persisted-tool-1',
                      title: 'Read saved file',
                      kind: 'read',
                      status: 'completed',
                      content: 'Saved tool output',
                    },
                  ],
                }
              : message
          ),
        },
        acpBindingBySessionId: {
          ...state.acpBindingBySessionId,
          'chat-1': {
            ...state.acpBindingBySessionId['chat-1'],
            lifecycleState: 'ready',
            processing: false,
            processingStartedAtMs: null,
            toolCalls: [],
            turnEvents: [],
            turnUserMessageIndex: -1,
            turnAssistantMessageIndex: -1,
            pendingPermission: null,
          },
        },
      }
    })

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    expect(host.textContent).not.toContain('Tool call:')
    expect(host.textContent).toContain('Read saved file')
    expect(host.querySelector('[data-tool-status="completed"][aria-label="Tool status: Completed"]')).toBeTruthy()
    expect(host.querySelector('[data-tool-status="completed"] svg')).toBeTruthy()

    const toolButton = Array.from(host.querySelectorAll('button')).find((button) =>
      button.textContent?.includes('Read saved file')
    )
    expect(toolButton).toBeTruthy()
    act(() => {
      toolButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })
    expect(document.body.textContent).toContain('Saved tool output')
    const dialog = document.body.querySelector('[role="dialog"]')
    expect(dialog).toBeTruthy()
    expect(dialog?.querySelector('[data-tool-status="completed"][aria-label="Tool status: Completed"]')).toBeTruthy()
    expect(dialog?.textContent).not.toContain('status: completed')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it.each(['live', 'saved'])('loads deferred output for a %s tool with its source context', async (source) => {
    const previousCefQuery = window.cefQuery
    window.cefQuery = ({ request, onSuccess }) => {
      expect(JSON.parse(request)).toMatchObject({
        action: 'getToolCallContent',
        payload: { chatId: 'chat-1', toolCallId: 'tool-1', offset: source === 'live' ? Number.MAX_SAFE_INTEGER : 0, ...(source === 'saved' ? { messageIndex: 8 } : {}) },
      })
      onSuccess(JSON.stringify({ content: 'Active output page', offset: 0, nextOffset: 18, previousOffset: 0, lastOffset: 0, totalBytes: 18, hasPrevious: false, hasMore: false }))
    }
    useAppStore.setState((state) => ({
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          toolCalls: state.acpBindingBySessionId['chat-1'].toolCalls.map((tool) =>
            tool.id === 'tool-1' ? { ...tool, content: '', contentDeferred: true } : tool
          ),
        },
      },
    }))

    if (source === 'saved') useAppStore.setState((state) => ({
      historyStartIndexBySessionId: { ...state.historyStartIndexBySessionId, 'chat-1': 7 },
      acpBindingBySessionId: { ...state.acpBindingBySessionId, 'chat-1': {
        ...state.acpBindingBySessionId['chat-1'], processing: false, lifecycleState: 'ready',
        turnUserMessageIndex: -1, turnAssistantMessageIndex: -1, turnEvents: [],
      } },
      messages: { ...state.messages, 'chat-1': state.messages['chat-1'].map((message, index) => index === 1 ? {
        ...message, toolCalls: [{ id: 'tool-1', title: 'Saved result', kind: 'read', status: 'completed', content: '', contentDeferred: true }],
      } : message) },
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    const toolButton = Array.from(host.querySelectorAll('button')).find((button) => button.textContent?.includes(source === 'live' ? 'Search symbols' : 'Saved result'))
    await act(async () => {
      toolButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
      await Promise.resolve()
    })
    expect(document.body.querySelector('.uam-tool-modal__output')?.textContent).toContain('Active output page')

    act(() => root.unmount())
    host.remove()
    window.cefQuery = previousCefQuery
  })

  it('renders persisted ordered message blocks instead of regrouping assistant fields', () => {
    useAppStore.setState((state) => {
      const currentMessages = state.messages['chat-1'] ?? []
      return {
        messages: {
          ...state.messages,
          'chat-1': currentMessages.map((message) =>
            message.id === 'm-2'
              ? {
                  ...message,
                  content: 'Grouped content should not render.',
                  thoughts: 'Grouped thought should not render.',
                  planSummary: 'Ordered plan summary.',
                  planEntries: [{ content: 'Ordered plan step', priority: '', status: 'pending' }],
                  toolCalls: [
                    {
                      id: 'persisted-tool-1',
                      title: 'Ordered saved tool',
                      kind: 'read',
                      status: 'completed',
                      content: 'Saved tool output',
                    },
                  ],
                  blocks: [
                    { type: 'thought', text: 'First thought marker.' },
                    { type: 'assistant_text', text: 'First visible marker.' },
                    { type: 'tool_call', toolCallId: 'persisted-tool-1' },
                    { type: 'thought', text: 'Second thought marker.' },
                    { type: 'assistant_text', text: 'Final visible marker.' },
                    { type: 'plan' },
                  ],
                }
              : message
          ),
        },
        acpBindingBySessionId: {
          ...state.acpBindingBySessionId,
          'chat-1': {
            ...state.acpBindingBySessionId['chat-1'],
            lifecycleState: 'ready',
            processing: false,
            processingStartedAtMs: null,
            toolCalls: [],
            turnEvents: [],
            turnUserMessageIndex: -1,
            turnAssistantMessageIndex: -1,
            pendingPermission: null,
          },
        },
      }
    })

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    const text = host.textContent ?? ''
    const firstThought = text.indexOf('First thought marker.')
    const firstVisible = text.indexOf('First visible marker.')
    const tool = text.indexOf('Ordered saved tool')
    const secondThought = text.indexOf('Second thought marker.')
    const finalVisible = text.indexOf('Final visible marker.')
    const plan = text.indexOf('Ordered plan summary.')
    expect(firstThought).toBeGreaterThan(-1)
    expect(firstVisible).toBeGreaterThan(firstThought)
    expect(tool).toBeGreaterThan(firstVisible)
    expect(secondThought).toBeGreaterThan(tool)
    expect(finalVisible).toBeGreaterThan(secondThought)
    expect(plan).toBe(-1)
    expect(text).not.toContain('Grouped content should not render.')
    expect(text).not.toContain('Grouped thought should not render.')
    expect(text.match(/Ordered saved tool/g) ?? []).toHaveLength(1)

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('renders ordered goal review JSON as a distinct block without raw JSON', () => {
    const reviewJson = '{"decision":"continue","reason":"More work remains.","nextPrompt":"Continue with the next page.","evidence":["Reviewed the current diff."],"progressUpdate":{"currentStep":"Finish the branch UI.","lastVerification":"Focused tests passed."}}'
    useAppStore.setState((state) => {
      const currentMessages = state.messages['chat-1'] ?? []
      return {
        messages: {
          ...state.messages,
          'chat-1': currentMessages.map((message) =>
            message.id === 'm-2'
              ? {
                  ...message,
                  content: 'Grouped fallback content should not render.',
                  thoughts: '',
                  toolCalls: [],
                  planSummary: '',
                  planEntries: [],
                  blocks: [{ type: 'assistant_text' as const, text: reviewJson }],
                }
              : message
          ),
        },
        acpBindingBySessionId: {
          ...state.acpBindingBySessionId,
          'chat-1': {
            ...state.acpBindingBySessionId['chat-1'],
            lifecycleState: 'ready',
            processing: false,
            processingStartedAtMs: null,
            toolCalls: [],
            turnEvents: [],
            turnUserMessageIndex: -1,
            turnAssistantMessageIndex: -1,
            pendingPermission: null,
          },
        },
      }
    })

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    const goalReviewFrame = host.querySelector('article[aria-label="Goal Reviewer"]')
    expect(goalReviewFrame).not.toBeNull()
    expect(goalReviewFrame?.querySelector('[data-testid="goal-review-block"]')).not.toBeNull()
    expect(goalReviewFrame?.textContent).not.toContain('{"decision"')
    expect(goalReviewFrame?.textContent).not.toContain('Grouped fallback content should not render.')
    expect(host.textContent).toContain('Goal Review')
    expect(host.textContent).toContain('Continue')
    expect(host.textContent).toContain('More work remains.')
    expect(host.textContent).toContain('Next:')
    expect(host.textContent).toContain('Reviewed the current diff.')
    expect(host.textContent).toContain('Finish the branch UI.')
    expect(host.textContent).toContain('Focused tests passed.')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('renders Codex thinking and persisted plan actions', async () => {
    const sendAcpPrompt = vi.fn(() => Promise.resolve(true))
    const setSessionApprovalMode = vi.fn(() => Promise.resolve(true))
    useAppStore.setState((state) => ({
      sendAcpPrompt,
      setSessionApprovalMode,
      sessions: state.sessions.map((session) =>
        session.id === 'chat-1' ? { ...session, providerId: 'codex-cli', approvalMode: 'plan' } : session
      ),
      messages: {
        ...state.messages,
        'chat-1': [
          {
            id: 'm-1',
            sessionId: 'chat-1',
            role: 'user',
            content: 'Please make a plan',
            createdAt: new Date('2026-01-01T00:00:00.000Z'),
          },
          {
            id: 'm-2',
            sessionId: 'chat-1',
            role: 'assistant',
            content: '',
            thoughts: '### Reasoning\nInspecting files.\n\n### Summary\nNeed to patch Codex handling.',
            planSummary: 'Update Codex support.',
            planEntries: [
              { content: 'Update Codex support.', priority: 'duplicate', status: 'pending' },
              { content: 'Inspect protocol events', priority: '', status: 'completed' },
              { content: 'Patch rendering', priority: '', status: 'pending' },
            ],
            blocks: [
              { type: 'thought', text: '### Reasoning\nInspecting files.\n\n### Summary\nNeed to patch Codex handling.' },
              { type: 'plan' },
              { type: 'plan' },
            ],
            createdAt: new Date('2026-01-01T00:00:01.000Z'),
          },
        ],
      },
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          providerId: 'codex-cli',
          protocolKind: 'codex-app-server',
          lifecycleState: 'ready',
          processing: false,
          processingStartedAtMs: null,
          turnEvents: [],
          turnUserMessageIndex: -1,
          turnAssistantMessageIndex: -1,
          pendingPermission: null,
        },
      },
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    expect(host.textContent).toContain('Thoughts')
    expect(host.textContent).toContain('Reasoning')
    expect(host.textContent).toContain('Inspecting files.')
    expect(host.textContent).toContain('Summary')
    expect(host.textContent).toContain('Need to patch Codex handling.')
    expect(host.textContent).toContain('Plan')
    expect(host.textContent).toContain('Update Codex support.')
    expect((host.textContent?.match(/Update Codex support\./g) ?? [])).toHaveLength(1)
    expect(host.textContent).toContain('Inspect protocol events')
    expect(host.textContent).toContain('completed')
    expect(host.textContent).toContain('Patch rendering')
    expect(host.textContent).toContain('pending')
    expect(host.querySelectorAll('[data-testid="plan-block"]')).toHaveLength(1)

    const approveButton = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Approve')
    const denyButton = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Deny')
    expect(approveButton).toBeTruthy()
    expect(denyButton).toBeTruthy()

    await act(async () => {
      approveButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
      await Promise.resolve()
    })
    expect(setSessionApprovalMode).toHaveBeenNthCalledWith(1, 'chat-1', 'default')
    expect(sendAcpPrompt).toHaveBeenNthCalledWith(1, 'chat-1', 'Proceed with the plan.', undefined, undefined, expect.any(Function))
    expect(setSessionApprovalMode.mock.invocationCallOrder[0]).toBeLessThan(sendAcpPrompt.mock.invocationCallOrder[0])

    await act(async () => {
      denyButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
      await Promise.resolve()
    })
    expect(setSessionApprovalMode).toHaveBeenNthCalledWith(2, 'chat-1', 'plan')
    expect(sendAcpPrompt).toHaveBeenNthCalledWith(
      2,
      'chat-1',
      'Do not proceed with this plan. Please revise it before making changes.', undefined, undefined, expect.any(Function)
    )

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('does not promote ordinary Codex task progress into an actionable Plan card', () => {
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) =>
        session.id === 'chat-1' ? { ...session, providerId: 'codex-cli', approvalMode: 'default' } : session
      ),
      messages: {
        ...state.messages,
        'chat-1': [{
          id: 'm-1',
          sessionId: 'chat-1',
          role: 'assistant',
          content: 'Implemented the requested change.',
          planSummary: 'Internal task progress.',
          planEntries: [{ content: 'Run tests', priority: '', status: 'completed' }],
          createdAt: new Date('2026-01-01T00:00:01.000Z'),
        }],
      },
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          providerId: 'codex-cli',
          currentModeId: 'default',
          processing: false,
          turnEvents: [],
          turnUserMessageIndex: -1,
          turnAssistantMessageIndex: -1,
        },
      },
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    expect(host.textContent).toContain('Implemented the requested change.')
    expect(host.querySelector('[data-testid="plan-block"]')).toBeNull()

    act(() => root.unmount())
    host.remove()
  })

  it('hides historical Codex plan cards after a later user message', () => {
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) =>
        session.id === 'chat-1' ? { ...session, providerId: 'codex-cli' } : session
      ),
      messages: {
        ...state.messages,
        'chat-1': [
          {
            id: 'm-1',
            sessionId: 'chat-1',
            role: 'user',
            content: 'Plan this',
            createdAt: new Date('2026-01-01T00:00:00.000Z'),
          },
          {
            id: 'm-2',
            sessionId: 'chat-1',
            role: 'assistant',
            content: '',
            planSummary: 'Historical plan.',
            planEntries: [{ content: 'Old step', priority: '', status: 'pending' }],
            createdAt: new Date('2026-01-01T00:00:01.000Z'),
          },
          {
            id: 'm-3',
            sessionId: 'chat-1',
            role: 'user',
            content: 'Actually revise it',
            createdAt: new Date('2026-01-01T00:00:02.000Z'),
          },
        ],
      },
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          providerId: 'codex-cli',
          protocolKind: 'codex-app-server',
          lifecycleState: 'ready',
          processing: false,
          processingStartedAtMs: null,
          turnEvents: [],
          turnUserMessageIndex: -1,
          turnAssistantMessageIndex: -1,
          pendingPermission: null,
        },
      },
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    expect(host.textContent).not.toContain('Historical plan.')
    expect(host.textContent).not.toContain('Old step')
    expect(Array.from(host.querySelectorAll('button')).some((button) => button.textContent === 'Approve')).toBe(false)
    expect(Array.from(host.querySelectorAll('button')).some((button) => button.textContent === 'Deny')).toBe(false)

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('disables Codex plan actions while the active ACP plan is still processing', () => {
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) =>
        session.id === 'chat-1' ? { ...session, providerId: 'codex-cli', approvalMode: 'plan' } : session
      ),
      messages: {
        ...state.messages,
        'chat-1': [
          {
            id: 'm-1',
            sessionId: 'chat-1',
            role: 'user',
            content: 'Plan this',
            createdAt: new Date('2026-01-01T00:00:00.000Z'),
          },
          {
            id: 'm-2',
            sessionId: 'chat-1',
            role: 'assistant',
            content: '',
            planSummary: 'Active plan.',
            planEntries: [{ content: 'Working step', priority: '', status: 'inProgress' }],
            createdAt: new Date('2026-01-01T00:00:01.000Z'),
          },
        ],
      },
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          providerId: 'codex-cli',
          protocolKind: 'codex-app-server',
          lifecycleState: 'processing',
          processing: true,
          planSummary: 'Active plan.',
          planEntries: [{ content: 'Working step', priority: '', status: 'inProgress' }],
          turnEvents: [{ type: 'plan' }],
          turnUserMessageIndex: 0,
          turnAssistantMessageIndex: 1,
          pendingPermission: null,
        },
      },
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    expect(host.textContent).toContain('Active plan.')
    expect(host.textContent).toContain('Working step')
    expect(host.textContent).toContain('in progress')
    const approveButton = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Approve') as HTMLButtonElement | undefined
    const denyButton = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Deny') as HTMLButtonElement | undefined
    expect(approveButton).toBeTruthy()
    expect(denyButton).toBeTruthy()
    expect(approveButton?.disabled).toBe(true)
    expect(denyButton?.disabled).toBe(true)
    expect(approveButton?.getAttribute('title')).toBe('Codex is still working.')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('renders ACP errors in the composer area', async () => {
    const writeText = vi.fn((_text: string) => Promise.resolve())
    Object.defineProperty(globalThis.navigator, 'clipboard', {
      value: { writeText },
      configurable: true,
    })

    useAppStore.setState((state) => ({
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
            lifecycleState: 'error',
            processing: false,
            processingStartedAtMs: null,
            lastError: 'Internal ACP failure',
            recentStderr: 'stderr stack trace',
            lastExitCode: 137,
            diagnostics: [
              {
                time: '2026-01-01T00:00:02.000Z',
                event: 'response',
                reason: 'jsonrpc_error',
                method: 'session/prompt',
                requestId: '42',
                code: -32603,
                message: 'Internal error',
                detail: 'error.data={"cause":"boom"}',
                lifecycleState: 'processing',
              },
            ],
          },
        },
      }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={{ ...useAppStore.getState().sessions[0], providerId: 'opencode-cli' }} />)
    })

    const form = host.querySelector('form')
      expect(form?.textContent).toContain('Universal Agent Manager · Gemini ACP:')
      expect(form?.textContent).not.toContain('OpenCode ACP:')
      expect(form?.textContent).toContain('Internal ACP failure')
      expect(form?.textContent).not.toContain('Diagnostics')
      expect(form?.textContent).not.toContain('Exit code: 137')
      expect(form?.textContent).not.toContain('jsonrpc_error')
      expect(form?.textContent).not.toContain('stderr stack trace')
      const copyErrorButton = form?.querySelector<HTMLButtonElement>('button[aria-label="Copy error"]')
      expect(copyErrorButton).toBeTruthy()
      await act(async () => {
        copyErrorButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
        await Promise.resolve()
      })
      expect(writeText).toHaveBeenCalled()
      expect(writeText.mock.calls[0][0]).toContain('Universal Agent Manager · Gemini ACP error')
      expect(writeText.mock.calls[0][0]).toContain('Internal ACP failure')
      expect(writeText.mock.calls[0][0]).toContain('stderr stack trace')
      expect(writeText.mock.calls[0][0]).toContain('Exit code: 137')
      expect(writeText.mock.calls[0][0]).toContain('jsonrpc_error')
      expect(writeText.mock.calls[0][0]).toContain('error.data={"cause":"boom"}')
      const text = host.textContent ?? ''
      expect(text.indexOf('After tool.')).toBeLessThan(text.indexOf('Gemini ACP:'))
      const dismissErrorButton = form?.querySelector<HTMLButtonElement>('button[aria-label="Dismiss composer error"]')
      expect(dismissErrorButton).toBeTruthy()
      act(() => dismissErrorButton?.click())
      expect(form?.textContent).not.toContain('Internal ACP failure')

      act(() => useAppStore.setState((state) => ({
        acpBindingBySessionId: {
          ...state.acpBindingBySessionId,
          'chat-1': { ...state.acpBindingBySessionId['chat-1'], lastError: '' },
        },
      })))
      act(() => useAppStore.setState((state) => ({
        acpBindingBySessionId: {
          ...state.acpBindingBySessionId,
          'chat-1': { ...state.acpBindingBySessionId['chat-1'], lastError: 'Internal ACP failure' },
        },
      })))
      expect(form?.textContent).toContain('Internal ACP failure')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('keeps runtime recovery compact and dismisses only its visible error', async () => {
    const refreshCliProviderVersion = vi.fn().mockResolvedValue(true)
    const setSettingsOpen = vi.fn()
    useAppStore.setState((state) => ({
      refreshCliProviderVersion,
      setSettingsOpen,
      acpBindingBySessionId: { ...state.acpBindingBySessionId, 'chat-1': { ...state.acpBindingBySessionId['chat-1'], lifecycleState: 'error', lastError: 'Provider failed to launch.' } },
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    const error = host.querySelector('[aria-label="Dismiss composer error"]')!.closest('[role="alert"]') as HTMLElement
    expect(error.textContent).toContain('Provider failed to launch.')
    expect(error.getAttribute('style')).toContain('var(--red) 42%')
    expect(error.getAttribute('style')).toContain('var(--red) 9%')
    expect(error.querySelectorAll('button')).toHaveLength(2)
    expect(error.querySelector('[aria-expanded]')).toBeNull()
    expect(host.querySelector('[aria-label="Check provider CLI"]')).toBeNull()
    expect(host.querySelector('[aria-label="Open CLI settings"]')).toBeNull()
    expect(host.querySelector('[aria-label="Open fallback terminal"]')).toBeNull()
    const before = useAppStore.getState()
    act(() => (error.querySelector('[aria-label="Dismiss composer error"]') as HTMLButtonElement).click())
    expect(host.querySelector('[aria-label="Dismiss composer error"]')).toBeNull()
    expect(host.textContent).toContain('Command safety warning')
    expect(useAppStore.getState().acpBindingBySessionId).toBe(before.acpBindingBySessionId)
    expect(useAppStore.getState().messages).toBe(before.messages)
    expect(useAppStore.getState().sessions).toBe(before.sessions)
    expect(refreshCliProviderVersion).not.toHaveBeenCalled()
    expect(setSettingsOpen).not.toHaveBeenCalled()

    const updateError = (lastError: string) => act(() => useAppStore.setState((state) => ({
      acpBindingBySessionId: { ...state.acpBindingBySessionId, 'chat-1': { ...state.acpBindingBySessionId['chat-1'], lastError } },
    })))
    updateError('Provider failed to launch.')
    expect(host.querySelector('[aria-label="Dismiss composer error"]')).toBeNull()
    updateError('Connection lost.')
    expect(host.querySelector('[aria-label="Dismiss composer error"]')?.closest('[role="alert"]')?.textContent).toContain('Connection lost.')
    updateError('')
    updateError('Provider failed to launch.')
    expect(host.querySelector('[aria-label="Dismiss composer error"]')?.closest('[role="alert"]')?.textContent).toContain('Provider failed to launch.')

    act(() => root.unmount())
    host.remove()
  })

  it('uses turn serials to isolate the active ACP turn and close stale tool details', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    const toolButton = Array.from(host.querySelectorAll('button')).find((button) =>
      button.textContent?.includes('Search symbols')
    )
    expect(toolButton).toBeTruthy()
    act(() => {
      toolButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })
    expect(document.body.textContent).toContain('Searching workspace symbols')

    act(() => {
      useAppStore.setState((state) => ({
        messages: {
          ...state.messages,
          'chat-1': [
            ...(state.messages['chat-1'] ?? []),
            {
              id: 'm-3',
              sessionId: 'chat-1',
              role: 'user',
              content: 'Now summarize it',
              createdAt: new Date('2026-01-01T00:00:02.000Z'),
            },
            {
              id: 'm-4',
              sessionId: 'chat-1',
              role: 'assistant',
              content: 'This placeholder should be replaced.',
              createdAt: new Date('2026-01-01T00:00:03.000Z'),
            },
          ],
        },
        acpBindingBySessionId: {
          ...state.acpBindingBySessionId,
          'chat-1': {
            ...state.acpBindingBySessionId['chat-1'],
            lifecycleState: 'ready',
            processing: false,
            processingStartedAtMs: null,
            turnEvents: [{ type: 'assistant_text', text: 'Second answer only.' }],
            turnUserMessageIndex: 2,
            turnAssistantMessageIndex: 3,
            turnSerial: 2,
            toolCalls: [
              {
                id: 'tool-1',
                title: 'Second tool',
                kind: 'summary',
                status: 'completed',
                content: 'Second turn details',
              },
            ],
            pendingPermission: null,
          },
        },
      }))
    })

    expect(host.textContent).toContain('Second answer only.')
    expect(host.textContent).not.toContain('This placeholder should be replaced.')
    expect(host.textContent).not.toContain('Searching workspace symbols')
    expect(document.body.querySelector('[role="dialog"]')).toBeNull()

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('keeps tool events and initial text in one assistant message when a stream placeholder trails the durable turn', () => {
    useAppStore.setState((state) => ({
      messages: {
        ...state.messages,
        'chat-1': [
          state.messages['chat-1'][0],
          { ...state.messages['chat-1'][1], content: 'Initial text.' },
          {
            id: 'stream-chat-1',
            sessionId: 'chat-1',
            role: 'assistant' as const,
            content: 'Initial text.',
            createdAt: new Date('2026-01-01T00:00:02.000Z'),
            isStreaming: true,
          },
        ],
      },
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          lifecycleState: 'processing',
          processing: true,
          turnEvents: [
            { type: 'assistant_text', text: 'Initial text.' },
            { type: 'tool_call', toolCallId: 'tool-1' },
          ],
          turnUserMessageIndex: 0,
          turnAssistantMessageIndex: 1,
          turnSerial: 2,
          pendingPermission: null,
        },
      },
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    const assistantFrames = host.querySelectorAll('.conversation-turn--assistant')
    expect(assistantFrames).toHaveLength(1)
    expect(assistantFrames[0].textContent).toContain('Initial text.')
    expect(assistantFrames[0].textContent).toContain('Search symbols')

    act(() => root.unmount())
    host.remove()
  })

  it('falls back without hiding persisted bubbles when turn indexes no longer match their roles', () => {
    useAppStore.setState((state) => ({
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          turnEvents: [{ type: 'assistant_text', text: 'Current live answer.' }],
          turnUserMessageIndex: 1,
          turnAssistantMessageIndex: 0,
          turnSerial: 2,
        },
      },
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    expect(host.textContent).toContain('Please inspect the workspace')
    expect(host.textContent).toContain('Before tool. After tool.')
    expect(host.textContent).toContain('Current live answer.')

    act(() => root.unmount())
    host.remove()
  })

  it('falls back when stale turn indexes still point to an older user and assistant pair', () => {
    useAppStore.setState((state) => ({
      messages: {
        ...state.messages,
        'chat-1': [
          ...(state.messages['chat-1'] ?? []),
          {
            id: 'm-3',
            sessionId: 'chat-1',
            role: 'user',
            content: 'Current request',
            createdAt: new Date('2026-01-01T00:00:02.000Z'),
          },
          {
            id: 'm-4',
            sessionId: 'chat-1',
            role: 'assistant',
            content: 'Current persisted placeholder.',
            createdAt: new Date('2026-01-01T00:00:03.000Z'),
          },
        ],
      },
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          lifecycleState: 'processing',
          processing: true,
          pendingPermission: null,
          turnEvents: [{ type: 'assistant_text', text: 'Current live answer.' }],
          turnUserMessageIndex: 0,
          turnAssistantMessageIndex: 1,
          turnSerial: 2,
        },
      },
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    expect(host.textContent).toContain('Before tool. After tool.')
    expect(host.textContent).toContain('Current persisted placeholder.')
    expect(host.textContent).toContain('Current live answer.')

    act(() => root.unmount())
    host.remove()
  })

  it('fetches earlier long-chat history only after the history action and keeps absolute branch context', async () => {
    const loadOlderSessionMessages = vi.fn(async () => {
      useAppStore.setState(state => ({
        messages: { ...state.messages, 'chat-1': [{ ...state.messages['chat-1'][0], id: 'older', content: 'Demand loaded older user' }, ...state.messages['chat-1']] },
        historyStartIndexBySessionId: { 'chat-1': 9949 },
      }))
      return true
    })
    const branchFromMessage = vi.fn(async () => 'branch')
    useAppStore.setState(state => ({ loadOlderSessionMessages, branchFromMessage,
      historyStartIndexBySessionId: { 'chat-1': 9950 },
      sessions: state.sessions.map(session => ({ ...session, messageCount: 10000 })),
      acpBindingBySessionId: { ...state.acpBindingBySessionId, 'chat-1': { ...state.acpBindingBySessionId['chat-1'],
        lifecycleState: 'ready', processing: false, pendingPermission: null, pendingUserInput: null,
        turnUserMessageIndex: -1, turnAssistantMessageIndex: -1, turnEvents: [] } },
    }))
    const host = document.createElement('div'); document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    expect(loadOlderSessionMessages).not.toHaveBeenCalled()
    expect(host.textContent).not.toContain('Demand loaded older user')
    await act(async () => Array.from(host.querySelectorAll('button')).find(button => button.textContent === 'Show earlier messages')!.click())
    expect(loadOlderSessionMessages).toHaveBeenCalledWith('chat-1')
    expect(host.textContent).toContain('Demand loaded older user')
    await act(async () => host.querySelector<HTMLButtonElement>('button[aria-label="Revert to message in new branch"]')!.click())
    expect(branchFromMessage).toHaveBeenCalledWith('chat-1', 9949, undefined)
    act(() => root.unmount()); host.remove()
  })

  it('falls back when otherwise matching turn anchors are outside the rendered history window', () => {
    useAppStore.setState((state) => ({
      messages: {
        ...state.messages,
        'chat-1': [
          ...(state.messages['chat-1'] ?? []),
          ...Array.from({ length: 201 }, (_, index) => ({
            id: `system-${index}`,
            sessionId: 'chat-1',
            role: 'system' as const,
            content: `Archived system event ${index}`,
            createdAt: new Date(1_767_225_602_000 + index),
          })),
        ],
      },
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          lifecycleState: 'processing',
          processing: true,
          pendingPermission: null,
          turnEvents: [{ type: 'assistant_text', text: 'Visible fallback answer.' }],
          turnUserMessageIndex: 0,
          turnAssistantMessageIndex: 1,
          turnSerial: 2,
        },
      },
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    expect(host.textContent).toContain('Show earlier messages')
    expect(host.textContent).toContain('Visible fallback answer.')

    act(() => root.unmount())
    host.remove()
  })

  it.each(['user', 'assistant'] as const)('keeps original section metadata when the visible window starts at a %s', (role) => {
    useAppStore.setState((state) => ({
      workingDisplayMode: 'compact', expandWorkTraces: true,
      messages: { ...state.messages, 'chat-1': [
        { id: 'hidden-original', sessionId: 'chat-1', role: 'user', content: 'Original request', providerId: 'gemini-cli', modelId: 'original-model', createdAt: new Date(0) },
        { id: 'boundary', sessionId: 'chat-1', role, content: 'Visible continuation', continuesTurn: role === 'user', providerId: 'codex-cli', modelId: 'later-model', createdAt: new Date(1000) },
        { id: 'answer', sessionId: 'chat-1', role: 'assistant', content: 'Visible answer', thoughts: 'Visible reasoning', createdAt: new Date(2000) },
        ...Array.from({ length: 198 }, (_, index) => ({ id: `event-${index}`, sessionId: 'chat-1', role: 'system' as const, content: `Event ${index}`, createdAt: new Date(index + 3000) })),
      ] }, acpBindingBySessionId: {},
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    expect(host.textContent).not.toContain('Original request')
    expect(host.querySelectorAll('time.conversation-turn-start')).toHaveLength(1)
    expect(host.querySelector('time.conversation-turn-start')?.getAttribute('datetime')).toBe(new Date(0).toISOString())
    expect(host.querySelector('time.conversation-turn-start')?.textContent).toContain('Gemini · original-model')
    expect(host.querySelectorAll('[data-testid="working-summary"]')).toHaveLength(1)
    act(() => host.querySelector<HTMLButtonElement>('[aria-label="Collapse work trace"]')!.click())
    expect(host.querySelectorAll('[data-testid="working-summary"]')).toHaveLength(1)
    expect(host.textContent).not.toContain('Visible reasoning')
    act(() => Array.from(host.querySelectorAll('button')).find(button => button.textContent === 'Show earlier messages')!.click())
    expect(host.textContent).toContain('Original request')
    expect(host.querySelectorAll('time.conversation-turn-start')).toHaveLength(1)
    expect(host.querySelectorAll('[data-testid="working-summary"]')).toHaveLength(1)
    expect(host.textContent).not.toContain('Visible reasoning')
    expect(host.textContent).toContain('Visible answer')
    act(() => root.unmount())
    host.remove()
  })

  it('keeps a steer as a user message when earlier history is outside the rendered window', () => {
    useAppStore.setState((state) => ({
      workingDisplayMode: 'compact',
      messages: {
        ...state.messages,
        'chat-1': [
          { id: 'host', sessionId: 'chat-1', role: 'assistant', content: 'Hidden host', thoughts: 'Old work', createdAt: new Date(0) },
          { id: 'boundary-steer', sessionId: 'chat-1', role: 'user', content: 'Visible steer', prioritySteer: true, attachments: [{ id: 'scope', name: 'scope.txt', type: 'file', size: 1, path: '/tmp/scope.txt' }], createdAt: new Date(1) },
          ...Array.from({ length: 199 }, (_, index) => ({ id: `system-${index}`, sessionId: 'chat-1', role: 'system' as const, content: `Event ${index}`, createdAt: new Date(index + 2) })),
        ],
      },
      acpBindingBySessionId: {},
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    expect(host.textContent).toContain('Visible steer')
    expect(host.textContent).toContain('scope.txt')
    expect(host.querySelector('[data-detail="/tmp/scope.txt"]')).not.toBeNull()
    expect(host.querySelector('article.conversation-turn--user')?.textContent).toContain('Visible steer')
    act(() => root.unmount())
    host.remove()
  })

  it('does not duplicate a completed fallback already represented by persisted assistant text', () => {
    useAppStore.setState((state) => ({
      messages: {
        ...state.messages,
        'chat-1': (state.messages['chat-1'] ?? []).map((message) =>
          message.role === 'assistant' ? { ...message, content: 'Final response text.' } : message
        ),
      },
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          lifecycleState: 'ready',
          processing: false,
          pendingPermission: null,
          turnEvents: [{ type: 'assistant_text', text: 'Final response text.' }],
          turnUserMessageIndex: 1,
          turnAssistantMessageIndex: 0,
          turnSerial: 2,
        },
      },
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    expect(host.textContent?.match(/Final response text\./g)).toHaveLength(1)

    act(() => root.unmount())
    host.remove()
  })

  it('keeps the fallback visible while processing even when persisted text matches', () => {
    useAppStore.setState((state) => ({
      messages: {
        ...state.messages,
        'chat-1': (state.messages['chat-1'] ?? []).map((message) =>
          message.role === 'assistant' ? { ...message, content: 'In-flight response.' } : message
        ),
      },
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          lifecycleState: 'processing',
          processing: true,
          pendingPermission: null,
          turnEvents: [{ type: 'assistant_text', text: 'In-flight response.' }],
          turnUserMessageIndex: 1,
          turnAssistantMessageIndex: 0,
          turnSerial: 2,
        },
      },
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    expect(host.textContent?.match(/In-flight response\./g)).toHaveLength(2)

    act(() => root.unmount())
    host.remove()
  })

  it.each([
    { workingDisplayMode: 'compact', placement: 'tool-first' },
    { workingDisplayMode: 'verbose', placement: 'tool-first' },
    { workingDisplayMode: 'compact', placement: 'assistant' },
    { workingDisplayMode: 'compact', placement: 'pending-message' },
    { workingDisplayMode: 'compact', placement: 'fallback' },
  ] as const)('keeps child disclosure through $workingDisplayMode/$placement completion', async ({ workingDisplayMode, placement }) => {
    const originalOpen = useAppStore.getState().openSubAgentSession
    const savedAssistant = useAppStore.getState().messages['chat-1'][1]
    const tool = { id: 'child-tool', title: 'Reviewer', kind: 'sub-agent', status: 'running', content: '', isSubAgent: true, subAgentId: 'native-child' }
    useAppStore.setState((state) => ({
      workingDisplayMode,
      expandWorkTraces: true,
      openSubAgentSession: vi.fn(async () => 'child-chat'),
      sessions: [...state.sessions, { ...state.sessions[0], id: 'child-chat', name: 'Child' }],
      messages: { ...state.messages, 'chat-1': placement === 'pending-message' ? state.messages['chat-1'].slice(0, 1) : state.messages['chat-1'], 'child-chat': [{ id: 'child-answer', sessionId: 'child-chat', role: 'assistant', content: 'Child transcript', createdAt: new Date() }] },
      acpBindingBySessionId: { ...state.acpBindingBySessionId, 'chat-1': {
        ...state.acpBindingBySessionId['chat-1'], processing: true,
        turnUserMessageIndex: placement === 'fallback' ? -1 : 0, turnAssistantMessageIndex: placement === 'fallback' ? -1 : 1,
        toolCalls: [tool], turnEvents: [...(placement === 'assistant' ? [{ type: 'assistant_text' as const, text: 'Delegating.' }] : []), { type: 'tool_call', toolCallId: tool.id }],
      } },
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    const selector = '.uam-subagent-group'
    try {
      await act(async () => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
      await act(async () => {
        const panel = host.querySelector<HTMLDetailsElement>(selector)!
        panel.open = true; panel.dispatchEvent(new Event('toggle', { bubbles: true }))
      })
      await act(async () => { Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find(button => button.textContent === 'Preview')!.click() })
      expect(host.textContent).toContain('Child transcript')
      await act(async () => useAppStore.setState((state) => ({
        messages: { ...state.messages, 'chat-1': [state.messages['chat-1'][0], {
          ...savedAssistant, content: 'Finished',
          blocks: [{ type: 'tool_call', toolCallId: tool.id }, { type: 'assistant_text', text: 'Finished' }],
          toolCalls: [{ ...tool, status: 'completed' }],
        }] },
        acpBindingBySessionId: { ...state.acpBindingBySessionId, 'chat-1': {
          ...state.acpBindingBySessionId['chat-1'], processing: false, turnEvents: [], toolCalls: [],
        } },
      })))
      expect(host.querySelector<HTMLDetailsElement>(selector)?.open).toBe(true)
      expect(host.textContent).toContain('Child transcript')
    } finally {
      act(() => root.unmount())
      host.remove()
      useAppStore.setState({ openSubAgentSession: originalOpen })
    }
  })

  it.each([
    { workingDisplayMode: 'compact', placement: 'tool-first' },
    { workingDisplayMode: 'verbose', placement: 'tool-first' },
    { workingDisplayMode: 'compact', placement: 'assistant' },
    { workingDisplayMode: 'compact', placement: 'pending' },
    { workingDisplayMode: 'compact', placement: 'fallback' },
  ] as const)('keeps a collapsed $workingDisplayMode/$placement trace through recovery, completion and a new turn', async ({ workingDisplayMode, placement }) => {
    const tool = { id: 'trace-tool', title: 'Inspect workspace', kind: 'read', status: 'running', content: '' }
    const savedAssistant = useAppStore.getState().messages['chat-1'][1]
    useAppStore.setState((state) => ({
      workingDisplayMode, expandWorkTraces: true,
      messages: { ...state.messages, 'chat-1': placement === 'pending' ? state.messages['chat-1'].slice(0, 1) : state.messages['chat-1'] },
      acpBindingBySessionId: { ...state.acpBindingBySessionId, 'chat-1': {
        ...state.acpBindingBySessionId['chat-1'], processing: true, turnSerial: 1,
        turnUserMessageIndex: placement === 'fallback' ? -1 : 0, turnAssistantMessageIndex: ['pending', 'fallback'].includes(placement) ? -1 : 1,
        toolCalls: [tool], turnEvents: [...(placement === 'assistant' ? [{ type: 'assistant_text' as const, text: 'Inspecting.' }] : []), { type: 'tool_call', toolCallId: tool.id }],
      } },
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    try {
      await act(async () => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
      expect(host.querySelector('[aria-label="Collapse work trace"]')).toBeNull()
      expect(host.textContent).toContain(tool.title)
      await act(async () => useAppStore.setState((state) => ({
        acpBindingBySessionId: { ...state.acpBindingBySessionId, 'chat-1': {
          ...state.acpBindingBySessionId['chat-1'], sessionId: 'recovered-native-session',
        } },
      })))
      expect(host.querySelector('[aria-label="Collapse work trace"]')).toBeNull()
      await act(async () => useAppStore.setState((state) => ({
        messages: { ...state.messages, 'chat-1': [state.messages['chat-1'][0], {
          ...savedAssistant, content: 'Finished',
          blocks: [{ type: 'tool_call', toolCallId: tool.id }, { type: 'assistant_text', text: 'Finished' }],
          toolCalls: [{ ...tool, status: 'completed' }],
        }] },
        acpBindingBySessionId: { ...state.acpBindingBySessionId, 'chat-1': {
          ...state.acpBindingBySessionId['chat-1'], processing: false, turnAssistantMessageIndex: 1, turnEvents: [], toolCalls: [],
        } },
      })))
      expect(host.querySelector('[aria-label="Collapse work trace"]')).not.toBeNull()
      act(() => host.querySelector<HTMLButtonElement>('[aria-label="Collapse work trace"]')!.click())
      expect(host.querySelector('[aria-label="Expand work trace"]')).not.toBeNull()
      expect(host.textContent).not.toContain(tool.title)
      expect(host.textContent).toContain('Finished')
      await act(async () => useAppStore.setState((state) => ({
        messages: { ...state.messages, 'chat-1': [...state.messages['chat-1'], { ...state.messages['chat-1'][0], id: 'next-user', content: 'Continue' }] },
        acpBindingBySessionId: { ...state.acpBindingBySessionId, 'chat-1': {
          ...state.acpBindingBySessionId['chat-1'], processing: true, turnSerial: 2,
          turnUserMessageIndex: 2, turnAssistantMessageIndex: 3,
          toolCalls: [{ ...tool, id: 'next-tool', title: 'Next inspection' }], turnEvents: [{ type: 'tool_call', toolCallId: 'next-tool' }],
        } },
      })))
      expect(host.querySelectorAll('[aria-label="Expand work trace"]')).toHaveLength(1)
      expect(host.querySelectorAll('[aria-label="Collapse work trace"]')).toHaveLength(0)
      expect(host.textContent).toContain('Next inspection')
    } finally {
      act(() => root.unmount())
      host.remove()
    }
  })

  it.each(['compact', 'verbose'] as const)('keeps individual %s thought choices through group toggles and completion', async (workingDisplayMode) => {
    const events = [
      { type: 'thought' as const, text: 'First thought.' },
      { type: 'permission_request' as const, requestId: 'answered-permission' },
      { type: 'thought' as const, text: 'Second thought.' },
    ]
    useAppStore.setState((state) => ({
      workingDisplayMode, expandWorkTraces: true,
      acpBindingBySessionId: { ...state.acpBindingBySessionId, 'chat-1': {
        ...state.acpBindingBySessionId['chat-1'], processing: true, turnSerial: 1,
        turnUserMessageIndex: 0, turnAssistantMessageIndex: 1, toolCalls: [], turnEvents: events,
      } },
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    const thoughts = () => host.querySelectorAll<HTMLDetailsElement>('.conversation-work__thought, .uam-thinking-block')
    try {
      await act(async () => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
      expect([...thoughts()].map((row) => row.open)).toEqual([false, false])
      act(() => thoughts()[1].querySelector('summary')!.click())
      expect([...thoughts()].map((row) => row.open)).toEqual([false, true])
      await act(async () => useAppStore.setState((state) => ({
        acpBindingBySessionId: { ...state.acpBindingBySessionId, 'chat-1': {
          ...state.acpBindingBySessionId['chat-1'], processing: false, turnEvents: [...events.slice(0, 2), { type: 'thought', text: 'Second thought. More reasoning.' }],
        } },
      })))
      act(() => host.querySelector<HTMLButtonElement>('[aria-label="Collapse work trace"]')!.click())
      act(() => host.querySelector<HTMLButtonElement>('[aria-label="Expand work trace"]')!.click())
      expect([...thoughts()].map((row) => row.open)).toEqual([false, true])
      await act(async () => useAppStore.setState((state) => ({
        messages: { ...state.messages, 'chat-1': [state.messages['chat-1'][0], {
          ...state.messages['chat-1'][1], content: 'Finished',
          blocks: [{ type: 'thought', text: 'First thought.' }, { type: 'thought', text: 'Second thought. More reasoning.' }, { type: 'assistant_text', text: 'Finished' }],
        }] },
        acpBindingBySessionId: { ...state.acpBindingBySessionId, 'chat-1': {
          ...state.acpBindingBySessionId['chat-1'], processing: false, turnEvents: [],
        } },
      })))
      expect([...thoughts()].map((row) => row.open)).toEqual([false, true])
      act(() => thoughts()[1].querySelector('summary')!.click())
      act(() => host.querySelector<HTMLButtonElement>('[aria-label="Collapse work trace"]')!.click())
      act(() => host.querySelector<HTMLButtonElement>('[aria-label="Expand work trace"]')!.click())
      expect([...thoughts()].map((row) => row.open)).toEqual([false, false])
    } finally {
      act(() => root.unmount())
      host.remove()
    }
  })

  it('expands multiple sub-agent chats inline without replacing the current chat', async () => {
    vi.useFakeTimers()
    const originalOpenSubAgentSession = useAppStore.getState().openSubAgentSession
    const originalLoadSessionMessages = useAppStore.getState().loadSessionMessages
    const openSubAgentSession = vi.fn((_sourceChatId: string, nativeSessionId: string) => Promise.resolve(nativeSessionId === 'agent-session-2' ? 'agent-chat-2' : 'agent-chat'))
    const loadSessionMessages = vi.fn(() => Promise.resolve())
    useAppStore.setState((state) => ({
      openSubAgentSession,
      loadSessionMessages,
      sessions: [
        ...state.sessions,
        { ...state.sessions[0], id: 'agent-chat', name: 'Planner history' },
        { ...state.sessions[0], id: 'agent-chat-2', name: 'Reviewer history' },
      ],
      messages: {
        ...state.messages,
        'agent-chat': [
          {
            id: 'agent-message-1',
            sessionId: 'agent-chat',
            role: 'assistant',
            content: 'Sub-agent inspected the provider runtime.',
            toolCalls: [{ id: 'nested-tool-1', title: 'Inspect runtime', kind: 'read', status: 'completed', content: 'Nested tool output' }],
            createdAt: new Date('2026-01-01T00:00:00.000Z'),
          },
        ],
        'agent-chat-2': [
          {
            id: 'agent-message-2',
            sessionId: 'agent-chat-2',
            role: 'assistant',
            content: 'Sub-agent reviewed the provider runtime.',
            createdAt: new Date('2026-01-01T00:00:00.000Z'),
          },
        ],
      },
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          toolCalls: [
            {
              id: 'agent-tool-1',
              title: 'Planner agent',
              kind: 'sub-agent',
              status: 'running',
              content: 'Inspecting with a sub-agent',
              isSubAgent: true,
              subAgentId: 'agent-session-1',
              subAgentTitle: 'Planner',
            },
            {
              id: 'agent-tool-2',
              title: 'Reviewer agent',
              kind: 'sub-agent',
              status: 'completed',
              content: 'Reviewing with a sub-agent',
              isSubAgent: true,
              subAgentId: 'agent-session-2',
              subAgentTitle: 'Reviewer',
            },
          ],
          turnEvents: [
            { type: 'assistant_text', text: 'I am delegating to a sub-agent.' },
            { type: 'tool_call', toolCallId: 'agent-tool-1' },
            { type: 'tool_call', toolCallId: 'agent-tool-2' },
          ],
          turnSerial: 2,
          pendingPermission: null,
          pendingUserInput: null,
        },
      },
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    try {
      act(() => {
        root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
      })

      // These descriptive native events have exact receivers, but no recognized launch name.
      // Each remains at its own tool slot rather than a fabricated whole-turn launch group.
      const groups = [...host.querySelectorAll<HTMLDetailsElement>('.uam-subagent-group')]
      expect(groups).toHaveLength(2)
      expect(groups.map(group => group.querySelector('summary')?.textContent)).toEqual(['1 subagent', '1 subagent'])
      expect(host.querySelector('[aria-label="Consecutive subagent launches"]')).toBeNull()
      expect(groups[0].compareDocumentPosition(groups[1]) & Node.DOCUMENT_POSITION_FOLLOWING).toBeTruthy()
      await act(async () => { for (const group of groups) { group.open = true; group.dispatchEvent(new Event('toggle', { bubbles: true })) } })
      expect(groups[0].querySelector('[aria-label="Open Planner"]')).not.toBeNull()
      expect(groups[0].querySelector('[aria-label="Open Reviewer"]')).toBeNull()
      expect(groups[1].querySelector('[aria-label="Open Reviewer"]')).not.toBeNull()
      expect(groups[1].querySelector('[aria-label="Open Planner"]')).toBeNull()
      expect(groups.every(group => group.textContent?.includes('Status unavailable'))).toBe(true)
      expect(host.querySelector('[aria-label="Open Planner"]')).toBeTruthy()
      expect(host.querySelector('[aria-label="Open Reviewer"]')).toBeTruthy()
      // A completed spawn operation is not evidence that the child itself completed.
      expect(host.textContent?.match(/Status unavailable/g)).toHaveLength(2)
      await act(async () => { for (const button of host.querySelectorAll<HTMLButtonElement>('button')) if (button.textContent === 'Preview') button.click() })

      expect(openSubAgentSession).toHaveBeenCalledWith('chat-1', 'agent-session-1', 'Planner', false)
      expect(openSubAgentSession).toHaveBeenCalledWith('chat-1', 'agent-session-2', 'Reviewer', false)
      expect(host.textContent).toContain('Planner history')
      expect(host.textContent).toContain('Sub-agent inspected the provider runtime.')
      expect(host.textContent).toContain('Reviewer history')
      expect(host.textContent).toContain('Sub-agent reviewed the provider runtime.')
      expect(host.textContent?.match(/Inspect runtime/g)).toHaveLength(1)
      const nestedToolButton = Array.from(host.querySelectorAll('button')).find((button) => button.textContent?.includes('Inspect runtime'))
      act(() => nestedToolButton?.dispatchEvent(new MouseEvent('click', { bubbles: true })))
      expect(document.body.querySelector('[role="dialog"]')?.textContent).toContain('Nested tool output')
      expect(useAppStore.getState().activeSessionId).toBe('chat-1')

      await act(async () => {
        vi.advanceTimersByTime(5000)
        await Promise.resolve()
      })
      expect(openSubAgentSession.mock.calls.filter(([, nativeSessionId]) => nativeSessionId === 'agent-session-1')).toHaveLength(1)
      expect(openSubAgentSession.mock.calls.filter(([, nativeSessionId]) => nativeSessionId === 'agent-session-2')).toHaveLength(1)
      expect(loadSessionMessages).toHaveBeenCalledWith('agent-chat', false, true)

    } finally {
      act(() => root.unmount())
      host.remove()
      useAppStore.setState({ openSubAgentSession: originalOpenSubAgentSession, loadSessionMessages: originalLoadSessionMessages })
      vi.useRealTimers()
    }
  })

  it('keeps opaque provider sub-agent events distinct from transcript load failures', async () => {
    const originalOpenSubAgentSession = useAppStore.getState().openSubAgentSession
    const openSubAgentSession = vi.fn(() => Promise.resolve(null))
    useAppStore.setState((state) => ({
      openSubAgentSession,
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          lifecycleState: 'ready',
          processing: false,
          pendingPermission: null,
          pendingUserInput: null,
          toolCalls: [
            { id: 'opaque-agent', title: 'Opaque worker', kind: 'sub-agent', status: 'completed', content: 'Provider event', isSubAgent: true },
            { id: 'missing-agent', title: 'Missing worker', kind: 'sub-agent', status: 'completed', content: 'Child event', isSubAgent: true, subAgentId: 'missing-child' },
          ],
          turnEvents: [
            { type: 'tool_call', toolCallId: 'opaque-agent' },
            { type: 'tool_call', toolCallId: 'missing-agent' },
          ],
          turnSerial: 3,
        },
      },
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    try {
      act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

      const groups = [...host.querySelectorAll<HTMLDetailsElement>('.uam-subagent-group')]
      expect(groups).toHaveLength(2)
      expect(groups.map(group => group.querySelector('summary')?.textContent)).toEqual(['0 subagents, 1 event', '1 subagent'])
      expect(host.querySelector('[aria-label="Consecutive subagent launches"]')).toBeNull()
      expect(groups[0].compareDocumentPosition(groups[1]) & Node.DOCUMENT_POSITION_FOLLOWING).toBeTruthy()
      await act(async () => { for (const group of groups) { group.open = true; group.dispatchEvent(new Event('toggle', { bubbles: true })) } })
      expect(groups[0].querySelector<HTMLButtonElement>('[aria-label="Open Opaque worker"]')?.disabled).toBe(true)
      expect(groups[0].querySelector('button[aria-label^="Preview"]')).toBeNull()
      expect(groups[1].querySelector('[aria-label="Open Missing worker"]')).not.toBeNull()
      expect(host.querySelector<HTMLButtonElement>('[aria-label="Open Opaque worker"]')?.disabled).toBe(true)
      expect(host.textContent).toContain('The provider did not expose a separate conversation.')
      await act(async () => { Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find(button => button.textContent === 'Preview')!.click() })
      expect(host.querySelector('[role="alert"]')?.textContent).toContain('Sub-agent chat history is unavailable.')
      expect(openSubAgentSession).toHaveBeenCalledOnce()
      expect(openSubAgentSession).toHaveBeenCalledWith('chat-1', 'missing-child', 'Missing worker', false)

    } finally {
      act(() => root.unmount())
      host.remove()
      useAppStore.setState({ openSubAgentSession: originalOpenSubAgentSession })
    }
  })

  it('opens the current workspace from the composer workspace row', async () => {
    const originalOpenSessionWorkspace = useAppStore.getState().openSessionWorkspace
    const openSessionWorkspace = vi.fn(() => Promise.resolve(true))
    useAppStore.setState({ openSessionWorkspace })

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    const workspaceTab = host.querySelector('.uam-composer-workspace-tab') as HTMLElement
    const workspaceButton = workspaceTab.querySelector('button[aria-label="Workspace actions"]')
    expect(workspaceTab.firstElementChild?.contains(workspaceButton)).toBe(true)
    expect(workspaceTab.querySelectorAll('button')).toHaveLength(1)
    expect(workspaceTab.lastElementChild?.querySelector('svg')).toBeNull()

    openWorkspaceActions(host)
    const openButton = document.body.querySelector('button[role="menuitem"]') as HTMLButtonElement | null
    expect(openButton).toBeTruthy()
    expect(openButton?.textContent).toContain('Open workspace')

    await act(async () => {
      openButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(openSessionWorkspace).toHaveBeenCalledWith('chat-1')

    act(() => {
      root.unmount()
    })
    host.remove()
    useAppStore.setState({ openSessionWorkspace: originalOpenSessionWorkspace })
  })

  it('does not offer controller-local workspace actions for a remote directory', () => {
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) => session.id === 'chat-1'
        ? { ...session, executionHostId: 'ssh-lab', workspaceDirectory: '/srv/project' }
        : session),
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    const menu = openWorkspaceActions(host)
    expect(menu?.textContent).toContain('This directory lives on the remote computer')
    expect(menu?.querySelector('[role="menuitem"]')?.textContent).toContain('Open terminal')
    expect(menu?.textContent).not.toContain('Open workspace')
    expect(menu?.textContent).not.toContain('Create worktree')

    act(() => root.unmount())
    host.remove()
  })

  it.each([true, false])('moves only to a workspace on the same SSH host and preserves state until confirmed (%s)', async (confirmed) => {
    const previousCefQuery = window.cefQuery
    const requests: Array<{ action: string; payload?: unknown }> = []
    window.cefQuery = ({ request, onSuccess, onFailure }) => {
      const parsed = JSON.parse(request)
      requests.push(parsed)
      if (parsed.action === 'moveChatWorkspace' && !confirmed) onFailure(409, 'Stop the chat before moving its workspace.')
      else onSuccess('{}')
    }
    const remoteHost = { id: 'ssh-lab', label: 'Lab', transport: 'ssh' as const, sshAlias: 'lab', runnerStatus: 'ready' as const, runnerVersion: '1', platform: 'linux', architecture: 'x64', lastSeenAt: '' }
    const chat = { ...useAppStore.getState().sessions[0], executionHostId: remoteHost.id, workspaceDirectory: '/srv/project', nativeSessionId: 'parent-native', folderId: 'original' }
    const folder = { id: 'destination', name: 'Destination', parentId: null, directory: '/srv/new', executionHostId: remoteHost.id, isExpanded: true, createdAt: new Date() }
    useAppStore.setState({ sessions: [chat], executionHosts: [remoteHost], folders: [folder, { ...folder, id: 'other-host', executionHostId: 'other', name: 'Other host' }, { ...folder, id: 'local', executionHostId: 'local', name: 'Local' }], acpBindingBySessionId: {}, cliBindingBySessionId: {} })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={chat} />))
    const menu = openWorkspaceActions(host)!
    const select = menu.querySelector<HTMLSelectElement>('select[aria-label="Move chat workspace"]')!
    expect(Array.from(select.options).map((option) => option.value)).toEqual(['', 'destination'])
    act(() => { select.value = 'destination'; select.dispatchEvent(new Event('change', { bubbles: true })) })
    await act(async () => { Array.from(menu.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Move chat')!.click() })
    expect(requests.filter((request) => request.action === 'moveChatWorkspace')).toMatchObject([{ action: 'moveChatWorkspace', payload: { chatId: 'chat-1', folderId: 'destination' } }])
    expect(useAppStore.getState().sessions[0]).toMatchObject({ folderId: 'original', nativeSessionId: 'parent-native', workspaceDirectory: '/srv/project' })
    expect(host.textContent).toContain(confirmed ? 'Moved chat.' : 'Stop the chat before moving')
    act(() => root.unmount())
    host.remove()
    window.cefQuery = previousCefQuery
  })

  it('keeps remote move disabled while a provider runtime is running', () => {
    const remoteHost = { id: 'ssh-lab', label: 'Lab', transport: 'ssh' as const, sshAlias: 'lab', runnerStatus: 'ready' as const, runnerVersion: '1', platform: 'linux', architecture: 'x64', lastSeenAt: '' }
    const chat = { ...useAppStore.getState().sessions[0], executionHostId: remoteHost.id, workspaceDirectory: '/srv/project' }
    useAppStore.setState((state) => ({ sessions: [chat], executionHosts: [remoteHost], acpBindingBySessionId: { 'chat-1': { ...state.acpBindingBySessionId['chat-1'], running: true } } }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={chat} />))
    const menu = openWorkspaceActions(host)!
    expect(menu.querySelector<HTMLSelectElement>('select[aria-label="Move chat workspace"]')?.disabled).toBe(true)
    expect(menu.textContent).toContain('Stop the chat before moving its workspace.')
    act(() => root.unmount())
    host.remove()
  })

  it('attaches a validated directory from this SSH host and clears its browser when the host changes', async () => {
    const originalList = useAppStore.getState().listRemoteDirectories
    const originalStage = useAppStore.getState().stageChatAttachments
    const remoteHost = { id: 'ssh-lab', label: 'Lab', transport: 'ssh' as const, sshAlias: 'lab', runnerStatus: 'ready' as const, runnerVersion: '1', platform: 'linux', architecture: 'x64', lastSeenAt: '' }
    const chat = { ...useAppStore.getState().sessions[0], executionHostId: remoteHost.id, workspaceDirectory: '/srv/project' }
    const list = vi.fn(async () => ({ ok: true as const, listing: { directory: '/srv/project', parentDirectory: '/srv', directories: [], truncated: false } }))
    const stage = vi.fn(async (_chatId, items) => items.map((item: { id: string; name: string; path: string }) => ({ ...item, type: 'directory', size: 0 })))
    useAppStore.setState({ sessions: [chat], executionHosts: [remoteHost], listRemoteDirectories: list, stageChatAttachments: stage })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={chat} />))
    const attach = async () => {
      const menu = openWorkspaceActions(host)!
      await act(async () => { Array.from(menu.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent?.includes('Attach remote directory'))!.click() })
    }
    await attach()
    expect(list).toHaveBeenCalledWith('ssh-lab', '/srv/project')
    await act(async () => { Array.from(document.body.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Use this directory')!.click() })
    expect(stage).toHaveBeenCalledWith('chat-1', [expect.objectContaining({ kind: 'directory', path: '/srv/project' })])
    await attach()
    act(() => useAppStore.setState({ executionHosts: [{ ...remoteHost, sshAlias: 'different-computer' }] }))
    expect(document.body.textContent).not.toContain('Use this directory')
    act(() => root.unmount())
    host.remove()
    useAppStore.setState({ listRemoteDirectories: originalList, stageChatAttachments: originalStage })
  })

  it('expires successful workspace feedback but keeps errors until dismissed', async () => {
    vi.useFakeTimers()
    const originalEditor = useAppStore.getState().openSessionWorkspaceEditor
    const originalTerminal = useAppStore.getState().openSessionTerminal
    useAppStore.setState({
      openSessionWorkspaceEditor: vi.fn(() => Promise.resolve(true)),
      openSessionTerminal: vi.fn(() => Promise.resolve(false)),
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    openWorkspaceActions(host)
    await act(async () => {
      ;(Array.from(document.body.querySelectorAll<HTMLButtonElement>('button[role="menuitem"]')).find((button) => button.textContent?.includes('Open in editor')) as HTMLButtonElement).click()
      await Promise.resolve()
    })
    expect(host.querySelector('[role="status"]')?.textContent).toContain('Opened workspace editor.')
    expect(host.querySelector('button[aria-label="Dismiss workspace action message"]')).toBeTruthy()
    act(() => vi.advanceTimersByTime(5000))
    expect(host.textContent).not.toContain('Opened workspace editor.')

    openWorkspaceActions(host)
    await act(async () => {
      ;(Array.from(document.body.querySelectorAll<HTMLButtonElement>('button[role="menuitem"]')).find((button) => button.textContent?.includes('Open terminal')) as HTMLButtonElement).click()
      await Promise.resolve()
    })
    expect(Array.from(host.querySelectorAll('[role="alert"]')).some((alert) => alert.textContent?.includes('Failed to open terminal.'))).toBe(true)
    act(() => vi.advanceTimersByTime(6000))
    expect(host.textContent).toContain('Failed to open terminal.')
    act(() => (host.querySelector('button[aria-label="Dismiss workspace action message"]') as HTMLButtonElement).click())
    expect(host.textContent).not.toContain('Failed to open terminal.')

    act(() => root.unmount())
    host.remove()
    useAppStore.setState({ openSessionWorkspaceEditor: originalEditor, openSessionTerminal: originalTerminal })
    vi.useRealTimers()
  })

  it('matches worktree action availability to the native runtime rules', () => {
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) => ({
        ...session,
        workspaceDirectory: '/tmp/project/.uam-worktrees/chat-1',
        workspaceSourceDirectory: '/tmp/project',
        workspaceIsolationKind: 'gitWorktree',
        workspaceBranchName: 'build/fix-login',
      })),
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          lifecycleState: 'ready',
          running: true,
          processing: false,
          pendingPermission: null,
          pendingUserInput: null,
          queuedPrompts: [],
        },
      },
      cliBindingBySessionId: {},
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    expect(host.querySelector('.uam-composer-workspace-tab')?.textContent).toContain('build/fix-login')
    openWorkspaceActions(host)
    expect(document.body.querySelector('[role="menu"][aria-label="Workspace actions"]')?.textContent).toContain('build/fix-login')
    const discard = Array.from(document.body.querySelectorAll<HTMLButtonElement>('button[role="menuitem"]'))
      .find((button) => button.textContent === 'Discard & return') as HTMLButtonElement
    expect(discard.disabled).toBe(false)

    act(() => {
      useAppStore.setState({
        cliBindingBySessionId: {
          'chat-1': {
            terminalId: 'term-1',
            boundChatId: 'chat-1',
            running: true,
            lifecycleState: 'idle',
            turnState: 'idle',
            processing: false,
            readySinceLastSelect: true,
            active: true,
            lastError: '',
          },
        },
      })
    })
    expect(discard.disabled).toBe(true)

    act(() => root.unmount())
    host.remove()
  })

  it('previews and confirms an isolated turn rollback before applying it', async () => {
    const previewChatTurnRollback = vi.fn(() => Promise.resolve({
      message: 'Rollback preview is ready.',
      diff: ' app.txt | 2 +-',
      checkpointSha: 'a'.repeat(40),
      parentSha: 'b'.repeat(40),
    }))
    const rollbackChatTurn = vi.fn(() => Promise.resolve({
      message: 'Rolled the isolated worktree back to before this turn.',
      diff: ' app.txt | 2 +-',
      checkpointSha: 'a'.repeat(40),
      parentSha: 'b'.repeat(40),
    }))
    useAppStore.setState((state) => ({
      previewChatTurnRollback,
      rollbackChatTurn,
      sessions: state.sessions.map((session) => ({
        ...session,
        workspaceDirectory: '/tmp/project/.uam-worktrees/chat-1',
        workspaceSourceDirectory: '/tmp/project',
        workspaceIsolationKind: 'gitWorktree',
      })),
      messages: {
        ...state.messages,
        'chat-1': state.messages['chat-1'].map((message) => message.role === 'assistant'
          ? { ...message, checkpointSha: 'a'.repeat(40), checkpointParentSha: 'b'.repeat(40) }
          : message),
      },
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          running: false,
          lifecycleState: 'ready',
          processing: false,
          pendingPermission: null,
          pendingUserInput: null,
          queuedPrompts: [],
          turnEvents: [],
        },
      },
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    const rollback = Array.from(host.querySelectorAll<HTMLButtonElement>('button'))
      .find((button) => button.textContent === 'Roll back turn')
    expect(rollback).toBeTruthy()
    await act(async () => {
      rollback?.click()
      await Promise.resolve()
      await Promise.resolve()
    })

    expect(previewChatTurnRollback).toHaveBeenCalledWith('chat-1', 1)
    expect(host.textContent).toContain('app.txt | 2 +-')
    expect(rollbackChatTurn).not.toHaveBeenCalled()
    const confirmRollback = Array.from(host.querySelectorAll<HTMLButtonElement>('.uam-notice button'))
      .find((button) => button.textContent === 'Roll back turn')
    await act(async () => {
      confirmRollback?.click()
      await Promise.resolve()
      await Promise.resolve()
    })
    expect(rollbackChatTurn).toHaveBeenCalledWith('chat-1', 1)
    expect(host.textContent).toContain('Rolled the isolated worktree back to before this turn.')

    act(() => root.unmount())
    host.remove()
  })

  it('clears transient worktree action messages when the active chat changes', async () => {
    const originalDiscardChatWorktreeChanges = useAppStore.getState().discardChatWorktreeChanges
    const discardChatWorktreeChanges = vi.fn(() => Promise.resolve({ ok: true, message: 'Discarded changes in the chat worktree.', patchPath: '' }))
    useAppStore.setState((state) => ({
      discardChatWorktreeChanges,
      sessions: [
        {
          ...state.sessions[0],
          workspaceDirectory: '/tmp/project/.uam-worktrees/chat-1',
          workspaceSourceDirectory: '/tmp/project',
          workspaceIsolationKind: 'gitWorktree',
        },
        {
          id: 'chat-2',
          name: 'Second Chat',
          viewMode: 'chat',
          folderId: 'default',
          workspaceDirectory: '/tmp/project',
          createdAt: new Date('2026-01-01T00:00:00.000Z'),
          updatedAt: new Date('2026-01-01T00:00:00.000Z'),
        },
      ],
      messages: {
        ...state.messages,
        'chat-2': [],
      },
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          lifecycleState: 'ready',
          running: false,
          processing: false,
          pendingPermission: null,
          turnEvents: [],
        },
        'chat-2': {
          ...state.acpBindingBySessionId['chat-1'],
          providerId: 'gemini-cli',
          lifecycleState: 'ready',
          running: false,
          processing: false,
          pendingPermission: null,
          turnEvents: [],
        },
      },
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    const workspaceMenu = openWorkspaceActions(host)
    const discardButton = Array.from(workspaceMenu?.querySelectorAll<HTMLButtonElement>('button') ?? []).find((button) => button.textContent === 'Discard & return')
    expect(discardButton).toBeTruthy()

    await act(async () => {
      discardButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
      await Promise.resolve()
    })

    expect(discardChatWorktreeChanges).toHaveBeenCalledWith('chat-1')
    expect(host.textContent).toContain('Discarded changes in the chat worktree.')

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[1]} />)
    })

    expect(host.textContent).not.toContain('Discarded changes in the chat worktree.')
    expect(useAppStore.getState().activeSessionId).toBe('chat-1')

    act(() => {
      root.unmount()
    })
    host.remove()
    useAppStore.setState({ discardChatWorktreeChanges: originalDiscardChatWorktreeChanges })
  })

  it('clears transient worktree action messages when a chat returns to its source workspace', async () => {
    const originalDiscardChatWorktreeChanges = useAppStore.getState().discardChatWorktreeChanges
    const discardChatWorktreeChanges = vi.fn(() => Promise.resolve({ ok: true, message: 'Discarded changes in the chat worktree.', patchPath: '' }))
    useAppStore.setState((state) => ({
      discardChatWorktreeChanges,
      sessions: state.sessions.map((session) =>
        session.id === 'chat-1'
          ? {
              ...session,
              workspaceDirectory: '/tmp/project/.uam-worktrees/chat-1',
              workspaceSourceDirectory: '/tmp/project',
              workspaceIsolationKind: 'gitWorktree',
            }
          : session
      ),
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          lifecycleState: 'ready',
          running: false,
          processing: false,
          pendingPermission: null,
          turnEvents: [],
        },
      },
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    const workspaceMenu = openWorkspaceActions(host)
    const discardButton = Array.from(workspaceMenu?.querySelectorAll<HTMLButtonElement>('button') ?? []).find((button) => button.textContent === 'Discard & return')
    expect(discardButton).toBeTruthy()

    await act(async () => {
      discardButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
      await Promise.resolve()
    })

    expect(host.textContent).toContain('Discarded changes in the chat worktree.')

    const returnedSession = {
      ...useAppStore.getState().sessions[0],
      workspaceDirectory: '/tmp/project',
      workspaceSourceDirectory: '',
      workspaceIsolationKind: '',
    }

    act(() => {
      root.render(<ChatView session={returnedSession} />)
    })

    expect(host.textContent).not.toContain('Discarded changes in the chat worktree.')
    expect(host.textContent).not.toContain('Git worktree')
    expect(useAppStore.getState().activeSessionId).toBe('chat-1')

    act(() => {
      root.unmount()
    })
    host.remove()
    useAppStore.setState({ discardChatWorktreeChanges: originalDiscardChatWorktreeChanges })
  })

  it.each(['Use this image', ''])('stages selected files and sends them with prompt %j', async (prompt) => {
    let stagedAttachment = {
      id: 'file-1',
      name: 'diagram.png',
      type: 'image',
      size: 4,
      path: '.UAM/attachments/chat-1/diagram.png',
    }
    const stageChatAttachments = vi.fn((_sessionId, items) => {
      stagedAttachment = {
        ...stagedAttachment,
        id: items[0].id,
      }
      return Promise.resolve([stagedAttachment])
    })
    const sendAcpPrompt = vi.fn(() => Promise.resolve(true))
    useAppStore.setState((state) => ({
      stageChatAttachments,
      sendAcpPrompt,
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          lifecycleState: 'ready',
          processing: false,
          pendingPermission: null,
        },
      },
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    await act(async () => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    const input = host.querySelector('input[type="file"]') as HTMLInputElement
    const file = new File(['data'], 'diagram.png', { type: 'image/png' })
    Object.defineProperty(file, 'path', { value: '/tmp/diagram.png', configurable: true })
    Object.defineProperty(input, 'files', { value: [file], configurable: true })
    await act(async () => {
      input.dispatchEvent(new Event('change', { bubbles: true }))
    })

    expect(stageChatAttachments).toHaveBeenCalledWith('chat-1', [
      expect.objectContaining({ name: 'diagram.png', kind: 'image', mimeType: 'image/png' }),
    ])
    expect(host.textContent).toContain('diagram.png')

    const textarea = host.querySelector('textarea') as HTMLTextAreaElement
    await act(async () => {
      const valueSetter = Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set
      valueSetter?.call(textarea, prompt)
      textarea.dispatchEvent(new Event('input', { bubbles: true }))
    })

    const form = host.querySelector('form') as HTMLFormElement
    await act(async () => {
      form.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true }))
    })

    expect(sendAcpPrompt).toHaveBeenCalledWith('chat-1', prompt, [stagedAttachment], undefined, expect.any(Function))

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('keeps restored ready attachments after the initial draft save', async () => {
    writeChatComposerDraft('chat-1', {
      text: '',
      attachments: [{ id: 'restored', name: 'restored.png', type: 'image', size: 4, path: '/tmp/restored.png' }],
    })
    vi.useFakeTimers()
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
    await act(async () => { vi.advanceTimersByTime(300); await Promise.resolve() })
    expect(host.textContent).toContain('restored.png')
    expect(readChatComposerDraft('chat-1').attachments).toEqual([expect.objectContaining({ id: 'restored' })])
    act(() => root.unmount())
    host.remove()
    vi.useRealTimers()
  })

  it('keeps an attachment that finishes staging after switching chats', async () => {
    let finishStaging: ((attachments: Array<{ id: string; name: string; type: string; size: number; path: string }>) => void) | undefined
    const stageChatAttachments = vi.fn((_sessionId, items) => new Promise<Array<{ id: string; name: string; type: string; size: number; path: string }>>((resolve) => {
      finishStaging = resolve
    }))
    const first = useAppStore.getState().sessions[0]
    const second = { ...first, id: 'chat-2', name: 'Second chat' }
    useAppStore.setState((state) => ({
      sessions: [first, second],
      stageChatAttachments,
      messages: { ...state.messages, 'chat-2': [] },
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-2': { ...state.acpBindingBySessionId['chat-1'], sessionId: 'native-2' },
      },
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    await act(async () => root.render(<ChatView key={first.id} session={first} />))

    const input = host.querySelector('input[type="file"]') as HTMLInputElement
    const file = new File(['data'], 'late.png', { type: 'image/png' })
    Object.defineProperty(file, 'path', { value: '/tmp/late.png', configurable: true })
    Object.defineProperty(input, 'files', { value: [file], configurable: true })
    await act(async () => input.dispatchEvent(new Event('change', { bubbles: true })))
    const attachmentId = stageChatAttachments.mock.calls[0][1][0].id

    await act(async () => root.render(<ChatView key={second.id} session={second} />))
    await act(async () => finishStaging?.([{
      id: attachmentId,
      name: 'late.png',
      type: 'image',
      size: 4,
      path: '.UAM/attachments/chat-1/late.png',
    }]))

    expect(readChatComposerDraft(first.id).attachments).toEqual([
      expect.objectContaining({ id: attachmentId, name: 'late.png' }),
    ])

    act(() => root.unmount())
    host.remove()
    removeComposerDrafts([first.id, second.id])
  })

  it('does not restore a removed attachment after staging finishes in another chat', async () => {
    let finishStaging: ((attachments: Array<{ id: string; name: string; type: string; size: number; path: string }>) => void) | undefined
    const stageChatAttachments = vi.fn((_sessionId, items) => new Promise<Array<{ id: string; name: string; type: string; size: number; path: string }>>((resolve) => { finishStaging = resolve }))
    const first = useAppStore.getState().sessions[0]
    const second = { ...first, id: 'chat-2', name: 'Second chat' }
    useAppStore.setState((state) => ({ sessions: [first, second], stageChatAttachments, messages: { ...state.messages, 'chat-2': [] } }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    await act(async () => root.render(<ChatView key={first.id} session={first} />))
    const input = host.querySelector('input[type="file"]') as HTMLInputElement
    const file = new File(['data'], 'removed.png', { type: 'image/png' })
    Object.defineProperty(file, 'path', { value: '/tmp/removed.png', configurable: true })
    Object.defineProperty(input, 'files', { value: [file], configurable: true })
    await act(async () => input.dispatchEvent(new Event('change', { bubbles: true })))
    const attachmentId = stageChatAttachments.mock.calls[0][1][0].id
    act(() => (host.querySelector('button[aria-label="Remove removed.png attachment"]') as HTMLButtonElement).click())
    await act(async () => root.render(<ChatView key={second.id} session={second} />))
    await act(async () => finishStaging?.([{ id: attachmentId, name: 'removed.png', type: 'image', size: 4, path: '/tmp/removed.png' }]))
    expect(readChatComposerDraft(first.id).attachments).toEqual([])
    act(() => root.unmount())
    host.remove()
  })

  it('does not block normal text paste in the composer', async () => {
    const stageChatAttachments = vi.fn((_sessionId, items) => Promise.resolve([
      {
        id: items[0].id,
        name: 'pasted.png',
        type: 'image',
        size: 4,
        path: '.UAM/attachments/chat-1/pasted.png',
      },
    ]))
    useAppStore.setState((state) => ({
      stageChatAttachments,
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          lifecycleState: 'ready',
          processing: false,
          pendingPermission: null,
        },
      },
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    await act(async () => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    const textarea = host.querySelector('textarea') as HTMLTextAreaElement
    const textPaste = new Event('paste', { bubbles: true, cancelable: true })
    Object.defineProperty(textPaste, 'clipboardData', {
      value: { files: [], getData: () => 'plain text' },
    })
    await act(async () => {
      textarea.dispatchEvent(textPaste)
    })
    expect(textPaste.defaultPrevented).toBe(false)

    expect(stageChatAttachments).not.toHaveBeenCalled()

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('hides workspace actions when no workspace is selected', () => {
    useAppStore.setState((state) => ({
      folders: state.folders.map((folder) => ({ ...folder, directory: '' })),
      sessions: state.sessions.map((session) => ({ ...session, workspaceDirectory: '' })),
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    const workspaceCue = host.querySelector('button[aria-label="Workspace not selected"]') as HTMLButtonElement
    expect(workspaceCue).toBeTruthy()
    expect(workspaceCue.disabled).toBe(true)
    expect(host.querySelector('button[aria-label="Workspace actions"]')).toBeNull()

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('normalizes legacy provider chats to the Gemini-only provider list without showing Codex UI', () => {
    useAppStore.getState().loadFromCef({
      folders: [
        {
          id: 'default',
          title: 'Project',
          directory: '/tmp/project',
          collapsed: false,
        },
      ],
      resourceCollections: [],
      chats: [
        {
          id: 'chat-1',
          title: 'Legacy Codex Chat',
          folderId: 'default',
          providerId: 'codex-cli',
          createdAt: new Date('2026-01-01T00:00:00.000Z').toISOString(),
          updatedAt: new Date('2026-01-01T00:00:00.000Z').toISOString(),
          messages: [],
          acpSession: {
            providerId: 'codex-cli',
            protocolKind: 'codex-app-server',
            running: false,
            processing: false,
            lifecycleState: 'ready',
            turnEvents: [],
            turnUserMessageIndex: -1,
            turnAssistantMessageIndex: -1,
          },
        },
      ],
      selectedChatId: 'chat-1',
      providers: [
        {
          id: 'gemini-cli',
          name: 'Gemini CLI',
          shortName: 'Gemini',
          outputMode: 'cli',
          supportsCli: true,
          supportsStructured: true,
          structuredProtocol: 'gemini-acp',
        },
      ],
      settings: {
        activeProviderId: 'gemini-cli',
        theme: 'dark',
        memoryEnabledDefault: true,
        memoryIdleDelaySeconds: 60,
        memoryRecallBudgetBytes: 2048,
        goalMaxLoopIterations: 200,
        updateChecksEnabled: true,
        updateLastCheckedAt: '',
        dismissedUpdateVersions: {},
        memoryLastStatus: '',
        memoryWorkerBindings: {
          'gemini-cli': { workerProviderId: 'gemini-cli', workerModelId: '' },
        },
      },
    })

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    expect(host.textContent).not.toContain('Codex is not supported in this build')
    expect(host.querySelector('button[aria-label="Select provider and model"]')).toBeTruthy()

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it.each(['/', '/companion'])('allows model selection with one provider at %s', (path) => {
    const originalDiscovery = useAppStore.getState().discoverProviderModels
    const discover = vi.fn().mockResolvedValue(true)
    useAppStore.setState({ discoverProviderModels: discover })
    window.history.replaceState(null, '', path)
    useAppStore.setState((state) => ({ acpBindingBySessionId: { 'chat-1': { ...state.acpBindingBySessionId['chat-1'], processing: false, lifecycleState: 'ready', pendingPermission: null } } }))
    useAppStore.setState({
      providers: [
        { id: 'gemini-cli', name: 'Gemini CLI', shortName: 'Gemini', color: '#8ab4ff', description: '', outputMode: 'cli', supportsCli: true, supportsStructured: true, structuredProtocol: 'gemini-acp' },
      ],
    })

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<ChatView session={useAppStore.getState().sessions[0]} />)
    })

    if (path === '/companion') expect(discover).toHaveBeenCalledWith('chat-1')
    else expect(discover).not.toHaveBeenCalled()

    const selector = host.querySelector('button[aria-label="Select provider and model"]') as HTMLButtonElement
    expect(selector).toBeTruthy()
    expect(selector.disabled).toBe(false)
    act(() => selector.click())
    expect(document.body.querySelector('[role="group"][aria-label="Provider"]')).toBeNull()

    act(() => {
      root.unmount()
    })
    useAppStore.setState({ discoverProviderModels: originalDiscovery })
    host.remove()
    window.history.replaceState(null, '', '/')
  })

  it.each([
    { label: 'legacy local', executionHostId: undefined, expectedHost: 'local', workspaceDirectory: '/fixture/local-project', platform: 'macos' },
    { label: 'explicit local', executionHostId: 'local', expectedHost: 'local', workspaceDirectory: '/fixture/local-project', platform: 'macos' },
    { label: 'remote POSIX', executionHostId: 'fixture-linux', expectedHost: 'fixture-linux', workspaceDirectory: '/fixture/remote-project', platform: 'linux' },
    { label: 'remote Windows', executionHostId: 'fixture-windows', expectedHost: 'fixture-windows', workspaceDirectory: 'C:\\Fixture\\Project', platform: 'windows' },
  ])('scopes empty OpenCode picker discovery to $label without changing chat context', async ({ executionHostId, expectedHost, workspaceDirectory, platform }) => {
    const previous = useAppStore.getState()
    const previousPath = window.location.pathname + window.location.search + window.location.hash
    const discover = vi.fn().mockResolvedValue(true)
    const setSessionProvider = vi.fn().mockResolvedValue(true)
    const setSessionModel = vi.fn().mockResolvedValue(true)
    const sendAcpPrompt = vi.fn().mockResolvedValue(true)
    const session = {
      ...previous.sessions[0], providerId: 'opencode-cli', executionHostId, workspaceDirectory,
      modelId: 'fixture/model', temporaryParentChatId: 'fixture-parent',
    }
    const savedDraft = { text: 'Unsent fixture draft remains here', attachments: [] }
    writeChatComposerDraft(session.id, savedDraft)
    window.history.replaceState(null, '', '/')
    useAppStore.setState({
      sessions: [session], activeSessionId: session.id,
      providers: [{ id: 'opencode-cli', name: 'OpenCode CLI', shortName: 'OpenCode', color: '', description: '', outputMode: 'cli', supportsCli: true, supportsStructured: true, structuredProtocol: 'opencode-acp' }],
      executionHosts: [{ id: expectedHost, label: 'Private fixture', transport: expectedHost === 'local' ? 'local' : 'ssh', sshAlias: 'never-connect-fixture', runnerStatus: 'ready', runnerVersion: '', platform, architecture: '', lastSeenAt: '' }],
      providerModelCatalogs: [],
      discoverProviderModels: discover, setSessionProvider, setSessionModel, sendAcpPrompt,
      acpBindingBySessionId: {
        [session.id]: {
          ...previous.acpBindingBySessionId[session.id], providerId: 'opencode-cli', protocolKind: 'acp',
          processing: false, lifecycleState: 'ready', pendingPermission: null, pendingUserInput: null,
          availableModels: [], configOptions: [], currentModelId: session.modelId, modelsLoading: false,
          turnEvents: [], turnUserMessageIndex: -1, turnAssistantMessageIndex: -1,
          queuedPrompts: [], pendingDirectPrompts: [], unsentDirectPrompts: [],
        },
      },
    })
    const host = document.createElement('div'); document.body.append(host)
    const root = createRoot(host)
    try {
      act(() => root.render(<ChatView session={session} />))
      expect(discover).not.toHaveBeenCalled()
      const textarea = host.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')!
      expect(textarea.value).toBe(savedDraft.text)
      await act(async () => host.querySelector<HTMLButtonElement>('button[aria-label="Select provider and model"]')!.click())
      expect(discover).toHaveBeenCalledTimes(1)
      expect(discover).toHaveBeenCalledWith('', 'opencode-cli', workspaceDirectory, expectedHost)
      expect(host.querySelector('.uam-composer-textarea')).toBe(textarea)
      expect(textarea.value).toBe(savedDraft.text)
      expect(readChatComposerDraft(session.id)).toEqual(savedDraft)
      expect(useAppStore.getState().sessions).toEqual([session])
      expect(useAppStore.getState().activeSessionId).toBe(session.id)
      expect(useAppStore.getState().messages).toBe(previous.messages)
      expect(setSessionProvider).not.toHaveBeenCalled()
      expect(setSessionModel).not.toHaveBeenCalled()
      expect(sendAcpPrompt).not.toHaveBeenCalled()
    } finally {
      act(() => root.unmount()); host.remove()
      removeComposerDrafts([session.id])
      window.history.replaceState(null, '', previousPath)
      useAppStore.setState(previous, true)
    }
  })

  it('keeps voice mode through the first provider startup, reads the first response, and stops on runtime shutdown', async () => {
    const requests: { action: string; payload?: { dictationId?: string; speechId?: string; text?: string } }[] = []
    const sendAcpPrompt = vi.fn().mockResolvedValue(true)
    window.cefQuery = ({ request, onSuccess }) => {
      const parsed = JSON.parse(request); requests.push(parsed)
      if (parsed.action === 'startDictation') onSuccess(JSON.stringify({ started: true }))
      else if (parsed.action === 'stopDictation') {
        onSuccess(JSON.stringify({ stopped: true }))
        window.dispatchEvent(new CustomEvent('uam-dictation', { detail: { type: 'dictation', dictationId: parsed.payload.dictationId, event: 'end' } }))
      } else onSuccess(JSON.stringify({ started: true, stopped: true, speechId: parsed.payload?.speechId }))
    }
    const session = useAppStore.getState().sessions[0]
    const binding = { ...useAppStore.getState().acpBindingBySessionId[session.id], sessionId: '', running: false, lifecycleState: 'stopped' as const, processing: false, pendingPermission: null, turnEvents: [], lastError: '' }
    useAppStore.setState({ sendAcpPrompt, acpBindingBySessionId: { [session.id]: binding }, cliBindingBySessionId: {}, messages: { [session.id]: [] } })
    const host = document.createElement('div'); document.body.appendChild(host); const root = createRoot(host)
    await act(async () => root.render(<ChatView session={session}/>))
    const voiceButton = () => Array.from(host.querySelectorAll('button')).find(button => button.textContent === 'Stop voice mode')
    await act(async () => Array.from(host.querySelectorAll('button')).find(button => button.textContent === 'Start voice mode')!.click())
    const dictationId = requests.find(request => request.action === 'startDictation')?.payload?.dictationId
    const say = async (text: string) => { await act(async () => { window.dispatchEvent(new CustomEvent('uam-dictation', { detail: { type: 'dictation', event: 'final', dictationId, text } })) }) }
    await say('First prompt'); await say('send message')
    expect(sendAcpPrompt).toHaveBeenCalledWith(session.id, 'First prompt', [], undefined, expect.any(Function))
    await act(async () => useAppStore.setState({ acpBindingBySessionId: { [session.id]: { ...binding, running: true, lifecycleState: 'starting' } } }))
    expect(voiceButton()).toBeTruthy()
    await act(async () => useAppStore.setState({ acpBindingBySessionId: { [session.id]: { ...binding, sessionId: 'first-native-session', running: true, lifecycleState: 'processing', processing: true } } }))
    expect(voiceButton()).toBeTruthy()
    await act(async () => useAppStore.setState({
      acpBindingBySessionId: { [session.id]: { ...binding, sessionId: 'first-native-session', running: true, lifecycleState: 'ready' } },
      messages: { [session.id]: [{ id: 'first-response', sessionId: session.id, role: 'assistant', content: 'First response', createdAt: new Date() }] },
    }))
    expect(requests.find(request => request.action === 'startSpeechOutput')?.payload?.text).toBe('First response')
    await act(async () => useAppStore.setState({ acpBindingBySessionId: { [session.id]: { ...binding, sessionId: 'first-native-session', lifecycleState: 'stopped' } } }))
    expect(voiceButton()).toBeFalsy()
    expect(requests.some(request => request.action === 'stopSpeechOutput')).toBe(true)
    for (const lifecycleState of ['error', 'stopped'] as const) {
      await act(async () => useAppStore.setState({ acpBindingBySessionId: { [session.id]: binding } }))
      await act(async () => Array.from(host.querySelectorAll('button')).find(button => button.textContent === 'Start voice mode')!.click())
      await act(async () => useAppStore.setState({ acpBindingBySessionId: { [session.id]: { ...binding, lifecycleState: 'starting' } } }))
      expect(voiceButton()).toBeTruthy()
      const stopsBeforeFailure = requests.filter(request => request.action === 'stopDictation').length
      await act(async () => useAppStore.setState({ acpBindingBySessionId: { [session.id]: { ...binding, lifecycleState, lastError: lifecycleState === 'error' ? 'Provider startup failed' : '' } } }))
      expect(voiceButton()).toBeFalsy()
      expect(requests.filter(request => request.action === 'stopDictation')).toHaveLength(stopsBeforeFailure + 1)
    }
    act(() => root.unmount()); host.remove(); delete window.cefQuery
  })

  it('connects voice commands to the selected chat draft and existing send/navigation actions', async () => {
    const requests: { action: string; payload?: { dictationId?: string } }[] = []
    const originalSelection = useAppStore.getState().setActiveSession
    const setActiveSession = vi.fn((id: string | null) => useAppStore.setState({ activeSessionId: id }))
    const sendAcpPrompt = vi.fn().mockResolvedValue(true)
    window.cefQuery = ({ request, onSuccess }) => {
      const parsed = JSON.parse(request); requests.push(parsed)
      onSuccess(JSON.stringify(parsed.action === 'startDictation' ? { started: true } : { stopped: true }))
    }
    const session = useAppStore.getState().sessions[0]
    useAppStore.setState({ sendAcpPrompt, setActiveSession, sessions: [session, { ...session, id: 'imported', importedReadOnly: true }, { ...session, id: 'chat-hidden', parentChatId: session.id }, { ...session, id: 'chat-2', name: 'Second chat' }] })
    const host = document.createElement('div'); document.body.appendChild(host); const root = createRoot(host)
    await act(async () => root.render(<ChatView session={session}/>))
    const button = Array.from(host.querySelectorAll('button')).find(button => button.textContent === 'Start voice mode')!
    expect(button.disabled).toBe(false)
    await act(async () => button.click())
    const dictationId = requests.find(request => request.action === 'startDictation')?.payload?.dictationId
    const say = async (text: string) => { await act(async () => { window.dispatchEvent(new CustomEvent('uam-dictation', { detail: { type: 'dictation', event: 'final', dictationId, text } })) }) }
    await say('Voice draft')
    expect((host.querySelector('textarea') as HTMLTextAreaElement).value).toBe('Voice draft')
    await say('send message')
    expect(sendAcpPrompt).toHaveBeenCalledWith('chat-1', 'Voice draft', [], undefined, expect.any(Function))
    await say('next chat')
    expect(setActiveSession).toHaveBeenCalledWith('chat-2')
    expect(useAppStore.getState().activeSessionId).toBe('chat-2')
    expect(requests.some(request => request.action === 'stopDictation' && request.payload?.dictationId === dictationId)).toBe(true)
    act(() => root.unmount()); host.remove(); delete window.cefQuery; useAppStore.setState({ setActiveSession: originalSelection })
  })

  it('keeps stopped dictation as a draft and only submits when Send stops it', async () => {
    const requests: Array<{ action: string; payload?: { locale?: string; dictationId?: string } }> = []
    const sendAcpPrompt = vi.fn(() => Promise.resolve(true))
    let completeDictationStart: (() => void) | undefined
    window.cefQuery = vi.fn(({ request, onSuccess }) => {
      const parsed = JSON.parse(request) as { action: string; payload?: { locale?: string; dictationId?: string } }
      requests.push(parsed)
      if (parsed.action === 'startDictation') {
        completeDictationStart = () => onSuccess('{}')
        return
      }
      onSuccess('{}')
    })
    useAppStore.setState((state) => ({
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          lifecycleState: 'ready',
          processing: false,
          pendingPermission: null,
          turnEvents: [],
        },
      },
      sendAcpPrompt,
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    const textarea = host.querySelector('textarea') as HTMLTextAreaElement
    act(() => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set?.call(textarea, 'Please')
      textarea.dispatchEvent(new Event('input', { bubbles: true }))
    })

    const startButton = host.querySelector('button[aria-label="Start dictation"]') as HTMLButtonElement
    expect(startButton).toBeTruthy()
    expect(startButton.getAttribute('aria-pressed')).toBe('false')
    await act(async () => {
      startButton.click()
      await Promise.resolve()
    })

    expect(requests[0]?.action).toBe('startDictation')
    expect(typeof requests[0]?.payload?.locale).toBe('string')
    const startingButton = host.querySelector('button[aria-label="Starting dictation"]') as HTMLButtonElement
    expect(startingButton.dataset.dictationState).toBe('starting')
    expect(startingButton.getAttribute('aria-busy')).toBe('true')
    expect(host.querySelector('button[aria-label="Send prompt"]')).toBeTruthy()
    expect(host.querySelector('button[aria-label="Options"]')).toBeTruthy()
    expect(host.querySelector('[role="status"]')?.textContent).toContain('Starting dictation')

    await act(async () => {
      completeDictationStart?.()
      await Promise.resolve()
    })

    const listeningButton = host.querySelector('button[aria-label="Stop dictation"]') as HTMLButtonElement
    expect(listeningButton.getAttribute('aria-pressed')).toBe('true')
    expect(listeningButton.dataset.dictationState).toBe('listening')
    expect(listeningButton.querySelector('.uam-dictation-listening-indicator')).toBeTruthy()
    expect(host.querySelector('[role="status"]')?.textContent).toContain('Listening')
    expect(textarea.disabled).toBe(true)

    act(() => window.dispatchEvent(new CustomEvent('uam-dictation', {
      detail: { type: 'dictation', dictationId: 'previous-recording', event: 'interim', text: 'Wrong pane' },
    })))
    expect(textarea.value).toBe('Please')
    act(() => window.dispatchEvent(new CustomEvent('uam-dictation', {
      detail: { type: 'dictation', dictationId: 'previous-recording', event: 'end' },
    })))
    expect(host.querySelector('button[aria-label="Stop dictation"]')).toBeTruthy()

    act(() => {
      const parsed = parseUamPushPayload({ type: 'dictation', dictationId: requests.filter((request) => request.action === 'startDictation').at(-1)?.payload?.dictationId, event: 'interim', text: 'write' })
      expect(parsed.ok).toBe(true)
      if (parsed.ok) window.dispatchEvent(new CustomEvent('uam-dictation', { detail: parsed.message }))
    })
    expect(textarea.value).toBe('Please write')

    act(() => {
      window.dispatchEvent(new CustomEvent('uam-dictation', {
        detail: { type: 'dictation', dictationId: requests.filter((request) => request.action === 'startDictation').at(-1)?.payload?.dictationId, event: 'final', text: 'write tests' },
      }))
    })
    expect(textarea.value).toBe('Please write tests')

    await act(async () => {
      ;(host.querySelector('button[aria-label="Stop dictation"]') as HTMLButtonElement).click()
      await Promise.resolve()
    })
    expect(requests.some((request) => request.action === 'stopDictation')).toBe(true)
    expect(host.querySelector('[role="status"]')?.textContent).toContain('Finishing dictation')
    const stoppingButton = host.querySelector('button[aria-label="Finishing dictation"]') as HTMLButtonElement
    expect(stoppingButton.dataset.dictationState).toBe('stopping')
    expect(stoppingButton.getAttribute('aria-busy')).toBe('true')
    expect(stoppingButton.querySelector('.uam-dictation-listening-indicator')).toBeNull()

    await act(async () => {
      window.dispatchEvent(new CustomEvent('uam-dictation', {
        detail: { type: 'dictation', dictationId: requests.filter((request) => request.action === 'startDictation').at(-1)?.payload?.dictationId, event: 'end' },
      }))
      await Promise.resolve()
    })
    expect(sendAcpPrompt).not.toHaveBeenCalled()
    expect(textarea.value).toBe('Please write tests')

    await act(async () => {
      ;(host.querySelector('button[aria-label="Start dictation"]') as HTMLButtonElement).click()
      await Promise.resolve()
      completeDictationStart?.()
      await Promise.resolve()
    })
    expect(host.querySelector('button[aria-label="Stop dictation"]')).toBeTruthy()

    await act(async () => {
      ;(host.querySelector('button[aria-label="Send prompt"]') as HTMLButtonElement).click()
      await Promise.resolve()
    })
    const starts = requests.filter((request) => request.action === 'startDictation')
    const stops = requests.filter((request) => request.action === 'stopDictation')
    expect(stops).toHaveLength(2)
    expect(starts[0].payload?.dictationId).toEqual(expect.any(String))
    expect(starts[1].payload?.dictationId).not.toBe(starts[0].payload?.dictationId)
    expect(stops.map((request) => request.payload?.dictationId)).toEqual(starts.map((request) => request.payload?.dictationId))
    expect(sendAcpPrompt).not.toHaveBeenCalled()

    await act(async () => {
      window.dispatchEvent(new CustomEvent('uam-dictation', {
        detail: { type: 'dictation', dictationId: requests.filter((request) => request.action === 'startDictation').at(-1)?.payload?.dictationId, event: 'end' },
      }))
      await Promise.resolve()
    })
    expect(sendAcpPrompt).toHaveBeenCalledTimes(1)
    expect(sendAcpPrompt).toHaveBeenCalledWith('chat-1', 'Please write tests', [], undefined, expect.any(Function))

    act(() => root.unmount())
    host.remove()
    delete window.cefQuery
  })

  it('surfaces native dictation permission errors without submitting partial text', async () => {
    const sendAcpPrompt = vi.fn(() => Promise.resolve(true))
    let dictationId = ''
    window.cefQuery = vi.fn(({ request, onSuccess }) => {
      const parsed = JSON.parse(request)
      if (parsed.action === 'startDictation') dictationId = parsed.payload.dictationId
      onSuccess('{}')
    })
    useAppStore.setState((state) => ({
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        'chat-1': {
          ...state.acpBindingBySessionId['chat-1'],
          lifecycleState: 'ready',
          processing: false,
          pendingPermission: null,
          turnEvents: [],
        },
      },
      sendAcpPrompt,
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    await act(async () => {
      ;(host.querySelector('button[aria-label="Start dictation"]') as HTMLButtonElement).click()
      await Promise.resolve()
    })
    act(() => {
      window.dispatchEvent(new CustomEvent('uam-dictation', {
        detail: { type: 'dictation', dictationId, event: 'error', message: 'Microphone permission was denied.' },
      }))
    })
    expect(host.querySelector('[role="alert"][data-dictation-state="error"]')?.textContent).toContain('Microphone permission was denied.')
    const errorButton = host.querySelector('button[aria-label="Retry dictation"]') as HTMLButtonElement
    expect(errorButton.dataset.dictationState).toBe('error')
    expect(errorButton.querySelector('.uam-dictation-listening-indicator')).toBeNull()
    expect((host.querySelector('textarea') as HTMLTextAreaElement).disabled).toBe(false)

    const dismissButton = host.querySelector('button[aria-label="Dismiss dictation error"]') as HTMLButtonElement
    await act(async () => {
      dismissButton.click()
      await new Promise((resolve) => window.setTimeout(resolve, 0))
    })
    expect(host.querySelector('[role="alert"][data-dictation-state="error"]')).toBeNull()
    expect(host.querySelector('button[aria-label="Start dictation"]')).toBeTruthy()
    expect((host.querySelector('textarea') as HTMLTextAreaElement).disabled).toBe(false)
    expect(document.activeElement).toBe(host.querySelector('textarea'))

    await act(async () => {
      window.dispatchEvent(new CustomEvent('uam-dictation', {
        detail: { type: 'dictation', dictationId, event: 'end' },
      }))
      await Promise.resolve()
    })
    expect(sendAcpPrompt).not.toHaveBeenCalled()
    expect(host.querySelector('[role="alert"][data-dictation-state="error"]')).toBeNull()

    act(() => root.unmount())
    host.remove()
    delete window.cefQuery
  })

  it('can dismiss a rejected dictation start and keep typing', async () => {
    window.cefQuery = vi.fn(({ request, onFailure }) => {
      const parsed = JSON.parse(request) as { action: string }
      if (parsed.action === 'startDictation') {
        onFailure(409, 'Microphone permission was denied by policy.')
      }
    })

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))

    await act(async () => {
      ;(host.querySelector('button[aria-label="Start dictation"]') as HTMLButtonElement).click()
      await Promise.resolve()
    })

    const textarea = host.querySelector('textarea') as HTMLTextAreaElement
    expect(host.querySelector('[role="alert"][data-dictation-state="error"]')?.textContent).toContain('Microphone permission was denied by policy.')
    expect(host.querySelector('button[aria-label="Retry dictation"]')).toBeTruthy()
    expect(textarea.disabled).toBe(false)

    await act(async () => {
      ;(host.querySelector('button[aria-label="Dismiss dictation error"]') as HTMLButtonElement).click()
      await new Promise((resolve) => window.setTimeout(resolve, 0))
    })
    expect(host.querySelector('[role="alert"][data-dictation-state="error"]')).toBeNull()
    expect(host.querySelector('button[aria-label="Start dictation"]')).toBeTruthy()
    expect(document.activeElement).toBe(textarea)

    act(() => root.unmount())
    host.remove()
    delete window.cefQuery
  })

  it('keeps imported portable transcripts visibly read-only', () => {
    const session = { ...useAppStore.getState().sessions[0], importedReadOnly: true }
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => root.render(<ChatView session={session} />))

    expect(host.textContent).toContain('This portable transcript is read-only')
    expect((host.querySelector('textarea') as HTMLTextAreaElement).disabled).toBe(true)

    act(() => root.unmount())
    host.remove()
  })
  it('creates /side through the fresh native chat action instead of sending a provider prompt', async () => {
    const session = useAppStore.getState().sessions[0]
    const select = vi.fn()
    useAppStore.setState({ activeSessionId: session.id, setActiveSession: select })
    const requests: { action: string; payload?: Record<string, unknown> }[] = []
    window.cefQuery = ({ request, onSuccess }) => { const parsed = JSON.parse(request); requests.push(parsed); onSuccess(JSON.stringify(parsed.action === 'createSideChat' ? { chatId: 'side-1' } : {})) }
    const host = document.createElement('div'); document.body.appendChild(host); const root = createRoot(host)
    await act(async () => root.render(<ChatView session={session} />))
    const input = host.querySelector('textarea')!
    act(() => { Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')!.set!.call(input, '/side'); input.dispatchEvent(new Event('input', { bubbles: true })) })
    await act(async () => input.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true })))
    expect(requests.filter(request => request.action === 'createSideChat')).toEqual([expect.objectContaining({ payload: { chatId: session.id } })])
    expect(select).toHaveBeenCalledWith('side-1')
    expect(requests.some(request => request.action === 'sendAcpPrompt')).toBe(false)
    act(() => root.unmount()); host.remove(); delete window.cefQuery
  })

  it('uses one neutral side-chat icon for the pill and resume control, and the return arrow only for return', async () => {
    const host = document.createElement('div'); document.body.appendChild(host); const root = createRoot(host)
    const session = { ...useAppStore.getState().sessions[0], temporaryParentChatId: 'main-chat' }
    try {
      act(() => root.render(<ChatView session={session} />))
      const controls = host.querySelector('.uam-composer-side-controls')!
      expect(controls.querySelector('.uam-composer-side-controls__pill svg')!.classList).toContain('lucide-message-square-dashed')
      expect(controls.querySelector('[aria-label="Return to main chat"] svg')!.classList).toContain('lucide-corner-up-left')
      expect(host.querySelector('.uam-composer-side-controls svg.lucide-corner-up-right')).toBeNull()
    } finally { act(() => root.unmount()); host.remove() }
  })

  it.each(['YOLO Cancel', 'side Return'])('keeps an unsent draft when clicking %s instead of submitting the composer', async (journey) => {
    const originalSend = useAppStore.getState().sendAcpPrompt
    const originalSelect = useAppStore.getState().setActiveSession
    const send = vi.fn().mockResolvedValue(true)
    const select = vi.fn()
    const session = { ...useAppStore.getState().sessions[0], ...(journey === 'side Return' ? { temporaryParentChatId: 'main-chat' } : {}) }
    useAppStore.setState((state) => ({ sendAcpPrompt: send, setActiveSession: select,
      acpBindingBySessionId: { 'chat-1': { ...state.acpBindingBySessionId['chat-1'], processing: false, lifecycleState: 'ready', pendingPermission: null } } }))
    const host = document.createElement('div'); document.body.appendChild(host); const root = createRoot(host)
    try {
      act(() => root.render(<ChatView session={session} />))
      const input = host.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')!
      act(() => { Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')!.set!.call(input, 'Do not send this draft'); input.dispatchEvent(new Event('input', { bubbles: true })) })
      if (journey === 'YOLO Cancel') {
        openComposerOptions(host)
        act(() => document.body.querySelector<HTMLButtonElement>('button[aria-label="Permissions"]')!.click())
        await act(async () => Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="option"]')).find((option) => option.textContent?.includes('YOLO'))!.click())
        expect(host.textContent).toContain('Enable YOLO?')
        await act(async () => Array.from(host.querySelectorAll<HTMLButtonElement>('.uam-notice button')).find((button) => button.textContent === 'Cancel')!.click())
        expect(host.textContent).not.toContain('Enable YOLO?')
      } else {
        await act(async () => Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.getAttribute('aria-label') === 'Return to main chat')!.click())
        expect(select).toHaveBeenCalledWith('main-chat')
      }
      expect(send).not.toHaveBeenCalled()
      expect(input.value).toBe('Do not send this draft')
    } finally { act(() => root.unmount()); host.remove(); useAppStore.setState({ sendAcpPrompt: originalSend, setActiveSession: originalSelect }) }
  })

  it('resumes a held native queue snapshot without consuming the composer draft on failure', async () => {
    const stored = new Map<string, string>()
    vi.stubGlobal('localStorage', { getItem: (key: string) => stored.get(key) ?? null, setItem: (key: string, value: string) => stored.set(key, value), removeItem: (key: string) => stored.delete(key) })
    const send = vi.fn().mockResolvedValue(true)
    const originalSend = useAppStore.getState().sendAcpPrompt
    const originalEdit = useAppStore.getState().editQueuedAcpPrompt
    const edit = vi.fn().mockResolvedValueOnce(false).mockResolvedValueOnce(true)
    const resume = vi.fn().mockResolvedValue(false)
    const original = useAppStore.getState().resumeQueuedAcpPrompts
    writeChatComposerDraft('chat-1', { text: 'Unsent composer draft', attachments: [] })
    useAppStore.setState((state) => ({ resumeQueuedAcpPrompts: resume, sendAcpPrompt: send, editQueuedAcpPrompt: edit, acpBindingBySessionId: { 'chat-1': {
      ...state.acpBindingBySessionId['chat-1'], queueHeld: true,
      queuedPrompts: [{ id: 'restored', revision: 3, text: 'Restored prompt', uamAgentId: 'build', markdownStoreFiles: [], attachments: [], goalMode: false, goalId: '' }],
    } } }))
    const host = document.createElement('div'); document.body.appendChild(host); const root = createRoot(host)
    try {
      act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
      expect((host.querySelector('.uam-composer-textarea') as HTMLTextAreaElement).value).toBe('Unsent composer draft')
      const queue = host.querySelector('[role="group"][aria-label="Queued prompt"]')!
      const action = (label: string) => Array.from(queue.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === label)!
      act(() => queue.querySelector<HTMLButtonElement>('[aria-label="Edit queued prompt"]')!.click())
      const queuedEdit = queue.querySelector<HTMLTextAreaElement>('[aria-label="Edit queued prompt text"]')!
      act(() => { Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')!.set!.call(queuedEdit, 'Revised restored prompt'); queuedEdit.dispatchEvent(new Event('input', { bubbles: true })) })
      await act(async () => action('Save').click())
      expect(edit).toHaveBeenNthCalledWith(1, 'chat-1', 'restored', 3, 'Revised restored prompt', undefined)
      expect(queuedEdit.value).toBe('Revised restored prompt')
      expect(send).not.toHaveBeenCalled()
      expect((host.querySelector('.uam-composer-textarea') as HTMLTextAreaElement).value).toBe('Unsent composer draft')
      await act(async () => action('Save').click())
      expect(edit).toHaveBeenNthCalledWith(2, 'chat-1', 'restored', 3, 'Revised restored prompt', undefined)
      expect(queue.querySelector('[aria-label="Edit queued prompt text"]')).toBeNull()
      expect(send).not.toHaveBeenCalled()
      act(() => queue.querySelector<HTMLButtonElement>('[aria-label="Edit queued prompt"]')!.click())
      act(() => action('Cancel').click())
      expect(send).not.toHaveBeenCalled()
      expect((host.querySelector('.uam-composer-textarea') as HTMLTextAreaElement).value).toBe('Unsent composer draft')
      await act(async () => Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Resume queue')!.click())
      expect(resume).toHaveBeenCalledWith('chat-1', [{ id: 'restored', revision: 3 }])
      expect(send).not.toHaveBeenCalled()
      expect((host.querySelector('.uam-composer-textarea') as HTMLTextAreaElement).value).toBe('Unsent composer draft')
      expect(readChatComposerDraft('chat-1')?.text).toBe('Unsent composer draft')
      expect(host.textContent).toContain('could not be resumed')
    } finally { act(() => root.unmount()); host.remove(); useAppStore.setState({ resumeQueuedAcpPrompts: original, sendAcpPrompt: originalSend, editQueuedAcpPrompt: originalEdit }) }
  })

  it('routes the managed launch at its exact inline slot into a child pane without putting child content below the parent draft', async () => {
    const session = useAppStore.getState().sessions[0]
    const runId = '8509f0ba-ea06-4068-a6ed-3e1bab09bc53'
    const taskTitle = 'Inspect the parent workspace'
    const launch = { id: 'owned-managed-launch', title: 'agent_delegate', kind: 'mcpToolCall', status: 'completed', content: JSON.stringify({ runId, status: 'queued' }) }
    const createdAt = new Date('2026-01-01T00:00:00Z')
    writeChatGridLayout({ ...defaultChatGridLayout, root: { type: 'leaf', id: 'leaf-1', sessionId: session.id } })
    const originalOpen = useAppStore.getState().openSubAgentSession
    const previousCef = window.cefQuery
    const openSubAgentSession = vi.fn()
    useAppStore.setState(state => ({ activeSessionId: session.id, openSubAgentSession,
      messages: { ...state.messages, [session.id]: [
        { id: 'managed-request', sessionId: session.id, role: 'user', content: 'Inspect the parent and delegate', createdAt },
        { id: 'managed-response', sessionId: session.id, role: 'assistant', content: 'After managed launch', createdAt, toolCalls: [launch],
          blocks: [{ type: 'assistant_text', text: 'Before managed launch' }, { type: 'tool_call', toolCallId: launch.id }, { type: 'assistant_text', text: 'After managed launch' }] },
      ] },
      acpBindingBySessionId: { [session.id]: { ...state.acpBindingBySessionId[session.id],
        lifecycleState: 'ready', processing: false, pendingPermission: null, turnEvents: [],
        backgroundTaskCount: 1,
        toolCalls: [{ id: 'waiting-child', title: 'wait', kind: 'collabAgentToolCall', status: 'in_progress', isSubAgent: true }],
      } },
    }))
    const requests: Array<{ action: string; payload?: Record<string, unknown> }> = []
    window.cefQuery = ({ request, onSuccess }) => {
      const parsed = JSON.parse(request); requests.push(parsed)
      onSuccess(JSON.stringify(parsed.action === 'listManagedSubagents' ? {
        chatId: session.id, controlScopeId: 'scope-managed', offset: 0, nextOffset: null, totalCount: 1,
        runs: [{ runId, transcriptChatId: 'owned-transcript', parentRunId: '', agentId: 'Verifier', taskTitle, providerId: 'codex-cli', modelId: 'model', host: null, status: 'completed', canCancel: false }],
      } : parsed.action === 'getManagedAgentTranscript' ? {
        runId, title: 'Verifier conversation', status: 'completed', providerId: 'codex-cli', messages: [{ role: 'assistant', content: 'Managed child result with context' }],
      } : {}))
    }
    const host = document.createElement('div'); document.body.appendChild(host); const root = createRoot(host)
    try {
      await act(async () => root.render(<ChatView session={session} />))
      const slot = host.querySelector<HTMLElement>('[data-launch-tool="owned-managed-launch"]')!
      expect(slot).not.toBeNull()
      expect(slot.closest('[data-message-index="1"]')).not.toBeNull()
      expect(slot.closest('.uam-chat-content')).not.toBeNull()
      expect(host.querySelector('.uam-managed-panel, [aria-label="UAM-managed subagents"]')).toBeNull()
      const content = host.querySelector<HTMLElement>('[data-message-index="1"]')!.textContent!
      expect(content.indexOf('Before managed launch')).toBeLessThan(content.indexOf(taskTitle))
      expect(content.indexOf(taskTitle)).toBeLessThan(content.indexOf('After managed launch'))
      const launchOpen = slot.querySelector<HTMLButtonElement>(`[aria-label="Open managed conversation for ${taskTitle}"]`)!
      expect(launchOpen).not.toBeNull()
      expect(launchOpen.textContent).toContain(taskTitle)
      expect(launchOpen.textContent).not.toContain('Verifier')
      expect(slot.querySelector('.uam-inline-subagents__meta')?.textContent).toContain('Verifier')
      const waitStatus = host.querySelector<HTMLElement>('.chat-status-strip[data-tone="agents"]')!
      expect(waitStatus).not.toBeNull()
      expect(slot.compareDocumentPosition(waitStatus) & Node.DOCUMENT_POSITION_FOLLOWING).toBeTruthy()
      const input = host.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')!
      act(() => { Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')!.set!.call(input, 'Keep my parent draft'); input.dispatchEvent(new Event('input', { bubbles: true })) })
      await act(async () => launchOpen.click())
      expect(host.textContent).not.toContain('Managed child result with context')
      // Inline Open carries the exact run; the child pane resolves its transcript, never a native ID.
      expect(subagentRouteFor(readChatGridLayout().activeLeafId, session)).toMatchObject({ kind: 'managed', runId, parentId: session.id })
      expect(subagentRouteFor(readChatGridLayout().activeLeafId, session)?.transcriptChatId).toBeUndefined()
      expect(requests.filter(value => value.action === 'listManagedSubagents').length).toBeGreaterThan(0)
      expect(requests.filter(value => value.action === 'listManagedSubagents').every(value => value.payload?.chatId === session.id)).toBe(true)
      expect(requests.some(value => value.action === 'getManagedAgentTranscript')).toBe(false)
      expect(requests.some(value => ['cancelManagedSubagent', 'sendAcpPrompt', 'startAcpSession'].includes(value.action))).toBe(false)
      act(() => clearSubagentView(readChatGridLayout().activeLeafId))
      expect(input.value).toBe('Keep my parent draft')
      expect(useAppStore.getState().activeSessionId).toBe(session.id)
      expect(openSubAgentSession).not.toHaveBeenCalled()
    } finally { act(() => root.unmount()); host.remove(); clearSubagentView(readChatGridLayout().activeLeafId); window.cefQuery = previousCef; useAppStore.setState({ openSubAgentSession: originalOpen }) }
  })

  it('does not query or mount managed-root controls for a temporary related chat', async () => {
    managedPanelFixture.enabled = true
    const session = { ...useAppStore.getState().sessions[0], temporaryParentChatId: 'main-chat' }
    const requests: string[] = []
    window.cefQuery = ({ request, onSuccess }) => { const parsed = JSON.parse(request); requests.push(parsed.action); onSuccess('{}') }
    const host = document.createElement('div'); document.body.appendChild(host); const root = createRoot(host)
    try {
      await act(async () => root.render(<ChatView session={session} />))
      expect(host.querySelector('[aria-label="UAM-managed subagents"]')).toBeNull()
      expect(requests).not.toContain('listManagedSubagents')
    } finally { act(() => root.unmount()); host.remove(); delete window.cefQuery }
  })

  it.each(['verbose', 'compact'] as const)('uses identical canonical assistant rendering in main, managed and passive-native history (%s)', async (workingDisplayMode) => {
    const parent = { ...useAppStore.getState().sessions[0], providerId: 'gemini-cli', messageCount: 2 }
    const child = { ...parent, id: 'canonical-native-child', name: 'Passive child', importedReadOnly: true,
      subagentParentChatId: parent.id, subagentParentCreatedAt: parent.createdAt.toISOString() }
    const supplied = [
      { role: 'user' as const, content: 'Review this fixture.', providerId: 'gemini-cli', createdAt: '2026-01-01T00:00:00.000Z' },
      { role: 'assistant' as const, content: 'Before. After.', providerId: 'gemini-cli', modelId: 'fixture-model', createdAt: '2026-01-01T00:00:01.000Z',
        thoughts: 'Full representative reasoning.', processingTimeMs: 4000, interrupted: true, stopReason: 'timeout',
        planSummary: 'Stored plan', planEntries: [{ content: 'Retained step', priority: '', status: 'completed' }],
        blocks: [{ type: 'assistant_text' as const, text: 'Before **tool**.\n\n```ts\nconst fixture = true\n```' },
          { type: 'thought' as const, text: 'Full representative reasoning.' }, { type: 'tool_call' as const, toolCallId: 'canonical-tool' },
          { type: 'plan' as const }, { type: 'permission_request' as const, requestId: 'stored-permission', text: 'Recorded permission evidence.' },
          { type: 'user_input_request' as const, requestId: 'stored-question', text: 'Recorded question evidence.' },
          { type: 'assistant_text' as const, text: 'After tool.' }],
        toolCalls: [{ id: 'canonical-tool', title: 'Inspect fixture', kind: 'read', status: 'completed', content: 'EMBEDDED CHILD OUTPUT' }] },
    ]
    const oldLoad = useAppStore.getState().loadSessionMessages
    const oldCef = window.cefQuery
    const requests: Array<{ action: string; payload?: Record<string, unknown> }> = []
    const history = reconcileCppMessages(parent.id, undefined, supplied, true, 0)
    useAppStore.setState((state) => ({ sessions: [parent, child], activeSessionId: parent.id,
      messages: { [parent.id]: history, [child.id]: reconcileCppMessages(child.id, undefined, supplied, true, 0) },
      workingDisplayMode, expandWorkTraces: true, collapsibleWorkSections: true,
      loadSessionMessages: vi.fn().mockResolvedValue(true),
      acpBindingBySessionId: { [parent.id]: { ...state.acpBindingBySessionId[parent.id], running: false, processing: false,
        lifecycleState: 'stopped', pendingPermission: null, pendingUserInput: null, turnEvents: [], toolCalls: [], planEntries: [],
        turnUserMessageIndex: -1, turnAssistantMessageIndex: -1, unsentMessageIndices: [], recoverableUnsentMessageIndices: [] } },
    }))
    window.cefQuery = ({ request, onSuccess }) => {
      const parsed = JSON.parse(request); requests.push(parsed)
      onSuccess(JSON.stringify(parsed.action === 'getManagedAgentTranscript'
        ? { runId: 'canonical-run', title: 'Managed child', providerId: 'gemini-cli', status: 'completed', messages: supplied } : {}))
    }
    writeChatComposerDraft(parent.id, { text: 'Preserve the independent parent draft.', attachments: [] })
    const host = document.createElement('div'); document.body.append(host); const root = createRoot(host)
    const assistant = (surface: string) => host.querySelector<HTMLElement>(`[data-surface="${surface}"] article.conversation-turn--assistant`)!
    const body = (surface: string) => assistant(surface).querySelector<HTMLElement>('.conversation-turn__body')!
    const canonicalMarkup = (surface: string) => {
      const clone = body(surface).cloneNode(true) as HTMLElement
      // React's generated accessibility IDs differ across roots; every class, element, state and content remains compared.
      clone.querySelectorAll('[id],[aria-controls],[aria-describedby]').forEach(node => {
        node.removeAttribute('id'); node.removeAttribute('aria-controls'); node.removeAttribute('aria-describedby')
      })
      return clone.innerHTML
    }
    try {
      await act(async () => root.render(<>
        <div data-surface="main"><ChatView session={parent} /></div>
        <div data-surface="managed"><ManagedSubagentConversation sourceChatId={parent.id} runId="canonical-run" transcriptChatId="canonical-managed" /></div>
        <div data-surface="native"><NativeConversation parent={parent} chatId={child.id} /></div>
      </>))
      const original = canonicalMarkup('main')
      for (const surface of ['managed', 'native']) {
        expect(canonicalMarkup(surface)).toBe(original)
        expect(assistant(surface).className).toBe(assistant('main').className)
        expect(body(surface).textContent).toContain('Recorded permission evidence.')
        expect(body(surface).textContent).toContain('Recorded question evidence.')
        expect(host.querySelector(`[data-surface="${surface}"]`)?.textContent).toContain('Response interrupted')
        expect(host.querySelector(`[data-surface="${surface}"] textarea`)).toBeNull()
        expect(host.querySelector(`[data-surface="${surface}"] [data-testid="plan-block"]`)).toBeNull()
      }
      expect(history[1].stopReason).toBe('timeout')
      expect(history[1].planEntries).toMatchObject([{ content: 'Retained step', status: 'completed' }])
      // Same disclosure controls and expanded reasoning content in all three routes.
      for (const surface of ['main', 'managed', 'native']) {
        const summary = body(surface).querySelector<HTMLDetailsElement>('details.conversation-work__thought')?.querySelector('summary')
        if (summary) await act(async () => summary.click())
      }
      expect(canonicalMarkup('managed')).toBe(canonicalMarkup('main'))
      expect(canonicalMarkup('native')).toBe(canonicalMarkup('main'))
      for (const surface of ['managed', 'native']) {
        const tool = [...body(surface).querySelectorAll<HTMLButtonElement>('button')].find(control =>
          control.getAttribute('aria-label') === 'Inspect fixture, completed' || control.title === 'Open tool details')!
        expect(tool).toBeTruthy()
        await act(async () => tool.click())
        expect(document.body.textContent).toContain('EMBEDDED CHILD OUTPUT')
        const close = document.body.querySelector<HTMLButtonElement>('[aria-label="Close tool details"]')
        if (close) await act(async () => close.click())
        else await act(async () => document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })))
      }
      expect(requests.filter(request => /sendAcp|resume|startAcp|openSubAgent|AttachmentImage|ToolCallContent|resolve.*Permission/i.test(request.action))).toEqual([])
      expect(host.querySelector<HTMLTextAreaElement>('[data-surface="main"] .uam-composer-textarea')?.value).toBe('Preserve the independent parent draft.')
      expect(useAppStore.getState().activeSessionId).toBe(parent.id)
      expect(useAppStore.getState().sessions).toEqual([parent, child])
    } finally {
      act(() => root.unmount()); host.remove(); window.cefQuery = oldCef
      useAppStore.setState({ loadSessionMessages: oldLoad })
    }
  })

  it.each(['verbose', 'compact'] as const)('retains identical recorded launch boundaries in collapsed main, managed and passive-native routes (%s)', async workingDisplayMode => {
    const previous = useAppStore.getState()
    const oldCef = window.cefQuery
    const parent = { ...previous.sessions[0], providerId: 'gemini-cli', messageCount: 3 }
    const child = { ...parent, id: 'collapsed-native-child', name: 'Recorded child', importedReadOnly: true,
      subagentParentChatId: parent.id, subagentParentCreatedAt: parent.createdAt.toISOString() }
    const launch = (id: string, title: string, name = 'spawn_agent') => ({ id, title: name, kind: 'sub-agent', status: 'completed', content: `Stored result for ${title}`, isSubAgent: true,
      subAgentChildren: [{ id: `native-${id}`, title, status: 'completed', durationMs: 2000 }] })
    const supplied = [
      { role: 'user' as const, content: 'Review this recorded trace.', providerId: 'gemini-cli', createdAt: '2026-01-01T00:00:00Z' },
      { role: 'assistant' as const, content: 'Earlier recorded prose', providerId: 'gemini-cli', createdAt: '2026-01-01T00:00:01Z', processingTimeMs: 5000,
        thoughts: 'Recorded reasoning stays available.', blocks: [
          { type: 'assistant_text' as const, text: 'Before **recorded** launch.\n\n```ts\nconst retained = true\n```' },
          { type: 'thought' as const, text: 'Recorded reasoning stays available.' },
          { type: 'tool_call' as const, toolCallId: 'launch-a' }, { type: 'tool_call' as const, toolCallId: 'launch-b' },
          { type: 'assistant_text' as const, text: 'Interleaved retained boundary.' }, { type: 'tool_call' as const, toolCallId: 'launch-c' },
          { type: 'assistant_text' as const, text: '   ' },
        ], toolCalls: [launch('launch-a', 'Recorded alpha'), launch('launch-b', 'Recorded beta', 'task'), launch('launch-c', 'Recorded gamma'),
          // Sparse stored block inventories keep the unreferenced launch at their existing fallback tail.
          launch('launch-d', 'Recorded delta'), { id: 'ordinary', title: 'Recorded inspection', kind: 'read', status: 'completed', content: 'EMBEDDED RECORDED INSPECTION' }] },
      { role: 'assistant' as const, content: 'Final **retained** answer.', providerId: 'gemini-cli', createdAt: '2026-01-01T00:00:02Z' },
    ]
    const requests: Array<{ action: string; payload?: Record<string, unknown> }> = []
    const openNative = vi.fn().mockResolvedValue(null)
    const load = vi.fn(async (_id: string) => true)
    useAppStore.setState((state) => ({ sessions: [parent, child], activeSessionId: parent.id, workingDisplayMode,
      expandWorkTraces: false, collapsibleWorkSections: true, openSubAgentSession: openNative, loadSessionMessages: load,
      messages: { [parent.id]: reconcileCppMessages(parent.id, undefined, supplied, true, 0), [child.id]: reconcileCppMessages(child.id, undefined, supplied, true, 0) },
      historyStartIndexBySessionId: { [parent.id]: 0, [child.id]: 0 },
      acpBindingBySessionId: { [parent.id]: { ...state.acpBindingBySessionId[parent.id], running: false, processing: false, lifecycleState: 'stopped',
        pendingPermission: null, pendingUserInput: null, turnEvents: [], toolCalls: [], planEntries: [], turnUserMessageIndex: -1, turnAssistantMessageIndex: -1,
        queuedPrompts: [], pendingDirectPrompts: [], unsentDirectPrompts: [], unsentMessageIndices: [], recoverableUnsentMessageIndices: [] } },
    }))
    window.cefQuery = ({ request, onSuccess }) => {
      const parsed = JSON.parse(request); requests.push(parsed)
      onSuccess(JSON.stringify(parsed.action === 'getManagedAgentTranscript' ? { runId: 'collapsed-run', title: 'Recorded managed child', providerId: 'gemini-cli', status: 'completed', messages: supplied } : {}))
    }
    writeChatComposerDraft(parent.id, { text: 'Independent parent draft remains.', attachments: [] })
    const host = document.createElement('div'); document.body.append(host); const root = createRoot(host)
    const surface = (name: string) => host.querySelector<HTMLElement>(`[data-collapsed-surface="${name}"]`)!
    const list = (name: string) => surface(name).querySelector<HTMLElement>('.uam-message-list')!
    const boundarySnapshot = (name: string) => [...list(name).querySelectorAll('.uam-inline-subagents')].map(group => ({
      launches: [...group.querySelectorAll('[data-launch-tool]')].map(row => row.getAttribute('data-launch-tool')),
      // Native launches use the canonical shared nested group rather than substitute text rows.
      nativeCounts: [...group.querySelectorAll('.uam-subagent-group > summary')].map(row => row.querySelector('span')?.textContent),
      open: [...group.querySelectorAll<HTMLDetailsElement>('details.uam-subagent-group')].map(row => row.open),
    }))
    const recordedCards = (name: string) => [...list(name).querySelectorAll('[data-subagent-row]')].map(row => ({
      id: row.getAttribute('data-subagent-row'), title: row.querySelector('.uam-subagent-card__title')?.textContent,
      status: row.querySelector('.uam-subagent-card__status-label')?.textContent, summary: row.querySelector('.uam-subagent-card__summary')?.textContent,
      time: row.querySelector('.uam-subagent-card__time')?.textContent,
    }))
    try {
      await act(async () => root.render(<>
        <div data-collapsed-surface="main"><ChatView session={parent} /></div>
        <div data-collapsed-surface="managed"><ManagedSubagentConversation sourceChatId={parent.id} runId="collapsed-run" transcriptChatId="collapsed-managed" /></div>
        <div data-collapsed-surface="native"><NativeConversation parent={parent} chatId={child.id} /></div>
      </>))
      const expected = boundarySnapshot('main')
      expect(expected).toHaveLength(3)
      expect(expected.map(group => group.nativeCounts)).toEqual([['2 subagents'], ['1 subagent'], ['1 subagent']])
      expect(expected.every(group => group.open.every(open => !open))).toBe(true)
      for (const name of ['main', 'managed', 'native']) {
        expect(boundarySnapshot(name)).toEqual(expected)
        const launchSlot = list(name).querySelector('[data-message-index="1"]')!
        const finalSlot = list(name).querySelector('[data-message-index="2"]')!
        expect(launchSlot).not.toBeNull(); expect(finalSlot).not.toBeNull()
        expect(Boolean(launchSlot.compareDocumentPosition(finalSlot) & Node.DOCUMENT_POSITION_FOLLOWING)).toBe(true)
        expect(list(name).textContent).toContain('Final retained answer.')
        expect(list(name).textContent).not.toContain('Interleaved retained boundary.')
        expect(list(name).querySelector('pre')).toBeNull()
        for (const details of list(name).querySelectorAll<HTMLDetailsElement>('details.uam-subagent-group')) {
          await act(async () => { details.open = true; details.dispatchEvent(new Event('toggle', { bubbles: true })) })
        }
      }
      const cards = recordedCards('main')
      expect(cards.map(card => card.title)).toEqual(['Recorded alpha', 'Recorded beta', 'Recorded gamma', 'Recorded delta'])
      expect(recordedCards('managed')).toEqual(cards); expect(recordedCards('native')).toEqual(cards)
      for (const name of ['managed', 'native']) {
        expect([...list(name).querySelectorAll<HTMLButtonElement>('[data-subagent-row]')].every(button => button.disabled)).toBe(true)
        expect(list(name).querySelector('[aria-label^="Preview "]')).toBeNull()
        expect(list(name).querySelector('[data-source-chat]')).toBeNull()
        await act(async () => [...list(name).querySelectorAll<HTMLButtonElement>('[data-subagent-row]')].forEach(button => button.click()))
      }
      // Expand the actual work-section control in each route; stored Markdown/thought/tool content returns at its original slots.
      for (const name of ['main', 'managed', 'native']) {
        const toggle = list(name).querySelector<HTMLButtonElement>('button[aria-label="Expand work trace"]')!
        expect(toggle).not.toBeNull()
        await act(async () => toggle.click())
        expect(list(name).querySelector('pre code')?.textContent).toContain('const retained = true')
        expect(list(name).querySelector('strong')?.textContent).toContain('recorded')
        expect(list(name).textContent).toContain('Recorded reasoning stays available.')
        expect(list(name).textContent).toContain('Interleaved retained boundary.')
        expect(list(name).textContent).toContain('Recorded inspection')
        expect(boundarySnapshot(name).map(group => group.nativeCounts)).toEqual([['2 subagents'], ['1 subagent'], ['1 subagent']])
      }
      const beforeDetails = requests.length
      for (const name of ['managed', 'native']) {
        // Compact and verbose share the recorded tool action but expose its name differently.
        const detailsControls = [...list(name).querySelectorAll<HTMLButtonElement>('button')].filter(button =>
          button.getAttribute('aria-label') === 'Recorded inspection, completed' ||
          (button.title === 'Open tool details' && button.textContent?.includes('Recorded inspection')))
        expect(detailsControls).toHaveLength(1)
        expect(detailsControls[0].disabled).toBe(false)
        await act(async () => detailsControls[0].click())
        expect(document.body.textContent).toContain('EMBEDDED RECORDED INSPECTION')
        const close = document.body.querySelector<HTMLButtonElement>('[aria-label="Close tool details"]')!
        await act(async () => close.click())
      }
      expect(requests.slice(beforeDetails)).toEqual([])
      expect(requests.some(request => /listManagedSubagents|cancelManaged|openSubAgent|sendAcp|resume|AttachmentImage|ToolCallContent/i.test(request.action))).toBe(false)
      expect(openNative).not.toHaveBeenCalled()
      expect(load.mock.calls.every(([id]) => id === child.id)).toBe(true)
      expect(surface('main').querySelector<HTMLTextAreaElement>('.uam-composer-textarea')?.value).toBe('Independent parent draft remains.')
      expect(useAppStore.getState().activeSessionId).toBe(parent.id)
      expect(useAppStore.getState().sessions).toEqual([parent, child])
    } finally { act(() => root.unmount()); host.remove(); window.cefQuery = oldCef; removeComposerDrafts([parent.id]); useAppStore.setState(previous, true) }
  })

  describe('outgoing direct input keeps one transcript slot', () => {
    const history = [
      { role: 'user' as const, content: 'Earlier request', createdAt: '2026-01-01T00:00:00.000Z' },
      { role: 'assistant' as const, content: 'Earlier answer', createdAt: '2026-01-01T00:00:01.000Z' },
    ]
    const outgoing = { role: 'user' as const, content: 'Outgoing hello', createdAt: '2026-01-02T00:00:00.000Z' }
    const chat = (acpSession: Partial<NonNullable<CppChat['acpSession']>> = {}): CppChat => ({ id: 'chat-1', title: 'Gemini', providerId: 'gemini-cli', createdAt: '', updatedAt: '',
      acpSession: { providerId: 'gemini-cli', sessionId: 'native-1', running: true, processing: false, lifecycleState: 'ready',
        queueHeld: false, queuedPrompts: [], pendingDirectPrompts: [], unsentDirectPrompts: [], unsentMessageIndices: [], recoverableUnsentMessageIndices: [], ...acpSession } })
    // Native push: the real reconcile/binding path, preserving previous message identity where reconcile does.
    const push = (rows: Parameters<typeof reconcileCppMessages>[2], acpSession: Partial<NonNullable<CppChat['acpSession']>> = {}) => act(() => useAppStore.setState((state) => ({
      messages: { 'chat-1': reconcileCppMessages('chat-1', state.messages['chat-1'], rows, true, 0) },
      acpBindingBySessionId: { 'chat-1': acpBindingFromCppChat({ ...chat(acpSession), acpSession: sanitizeCppAcpSession(chat(acpSession).acpSession) }, state.acpBindingBySessionId['chat-1']) },
    })))
    const receipt = (id: string, extra: Record<string, unknown> = {}) => ({ id, revision: 1, text: outgoing.content, uamAgentId: 'build', attachments: [], markdownStoreFiles: [], goalMode: false, goalId: '', ...extra })

    function mount(initialDraft: Parameters<typeof writeChatComposerDraft>[1] = { text: outgoing.content, attachments: [] }, configure?: () => void) {
      const previousSend = useAppStore.getState().sendAcpPrompt
      useAppStore.setState({ sendAcpPrompt: realSendAcpPrompt })
      useAppStore.setState((state) => ({ messages: { 'chat-1': reconcileCppMessages('chat-1', undefined, history, true, 0) }, historyStartIndexBySessionId: { 'chat-1': 0 },
        sessions: state.sessions.map((session) => ({ ...session, messageCount: 2, messagesDigest: undefined })),
        acpBindingBySessionId: { 'chat-1': acpBindingFromCppChat({ ...chat(), acpSession: sanitizeCppAcpSession(chat().acpSession) }, state.acpBindingBySessionId['chat-1']) } }))
      const requests: Array<{ action: string; requestId: string; payload: Record<string, unknown> }> = []
      const pending: Array<{ ack: () => void; reject: (message: string) => void }> = []
      // Private bridge fixture: sendAcpPrompt stays unresolved until the test acknowledges or rejects it.
      window.cefQuery = ({ request, onSuccess, onFailure }) => {
        const parsed = JSON.parse(request)
        requests.push(parsed)
        if (parsed.action === 'sendAcpPrompt') pending.push({ ack: () => onSuccess('{}'), reject: (message) => onFailure(500, message) })
        else onSuccess('{}')
      }
      writeChatComposerDraft('chat-1', initialDraft)
      configure?.()
      const host = document.createElement('div'); document.body.append(host); let root = createRoot(host)
      act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
      // MainPanel remounts ChatView on chat/view switches; presentation rows must survive that.
      const remount = () => { act(() => root.unmount()); root = createRoot(host); act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />)) }
      const sends = () => requests.filter((request) => request.action === 'sendAcpPrompt')
      const inputIdentity = (requestIndex = 0) => {
        expect(sends()[requestIndex]?.requestId).toEqual(expect.any(String))
        return { nativeInputId: `33333333-3333-4333-8333-${String(requestIndex + 1).padStart(12, '0')}`, originRequestId: sends()[requestIndex].requestId }
      }
      const sideEffects = () => requests.filter((request) => /retry|discard|remove|resume|edit/i.test(request.action))
      const occurrences = () => (host.querySelector('.uam-message-list')?.textContent?.split(outgoing.content).length ?? 1) - 1
      const cleanup = () => { act(() => root.unmount()); host.remove(); delete window.cefQuery; useAppStore.setState({ sendAcpPrompt: previousSend }) }
      return { host, pending, sends, inputIdentity, sideEffects, occurrences, cleanup, remount,
        renderCurrentSession: () => act(() => root.render(<ChatView session={useAppStore.getState().sessions[0]} />)),
        send: () => act(async () => host.querySelector<HTMLButtonElement>('button[aria-label="Send prompt"]')!.click()) }
    }

    // David's 2026-10-23 task-1217 decision supersedes the prior goal/Claude exclusion assertion.
    // Generated plan action exclusion remains covered separately below.
    // Contract: docs/documentation-review.md#referenced-context-action-sends.
    it.each(['goal', 'claude-plan', 'claude-proceed', 'claude-review'] as const)('sends the frozen composer references with user-authored %s input and preserves later selections', async route => {
      const previous = useAppStore.getState()
      const exactPrompt = 'Exact direct request, including  internal spacing.'
      const attachment = { id: 'direct-file', name: 'direct.txt', type: 'file', size: 4, path: '/private-fixture/direct.txt' }
      const reference = captureContextReference({ id: 'selected-composer-reference', kind: 'message', sourceChatId: 'source-chat', sourceMessageIndex: 7,
        executionHostId: 'local', workspaceDirectory: '/private-fixture', capturedAt: '2026-01-01T00:00:00.000Z', label: 'Selected composer quote', text: 'Selected context for the original authored request' })!
      const later = { ...reference, id: 'later-composer-reference', text: 'NEWER CONTEXT MUST WAIT FOR THE NEXT MESSAGE' }
      let finishGoal!: () => void
      const createGoal = vi.fn(() => new Promise<{ ok: true; goalId: string }>((resolve) => { finishGoal = () => resolve({ ok: true, goalId: 'direct-goal-fixture' }) }))
      let finishMode!: () => void
      const changeMode = vi.fn(() => new Promise<boolean>((resolve) => { finishMode = () => resolve(true) }))
      const changeSafety = vi.fn(async () => true)
      const duringMode = { ...reference, id: 'mode-wait-reference', text: 'ADDED DURING THE MODE WAIT' }
      let remaining = [later]
      const draftText = route === 'goal' ? `/goal ${exactPrompt}` : exactPrompt
      const f = mount({ text: draftText, attachments: [attachment], references: [reference] }, () => {
        useAppStore.setState({ setGoal: createGoal })
        if (route !== 'goal') useAppStore.setState(state => ({
          setSessionApprovalMode: changeMode, setSessionCommandSafetyTier: changeSafety,
          sessions: state.sessions.map(session => session.id === 'chat-1' ? { ...session, providerId: 'claude-cli', approvalMode: 'plan' } : session),
          acpBindingBySessionId: { 'chat-1': acpBindingFromCppChat({ ...chat(), providerId: 'claude-cli',
            acpSession: sanitizeCppAcpSession({ ...chat().acpSession, providerId: 'claude-cli', protocolKind: 'claude-code-stream-json', currentModeId: 'plan' }) }, undefined) },
        }))
      })
      try {
        if (route !== 'goal') expect(useAppStore.getState().providers.find(provider => provider.id === 'claude-cli')).toMatchObject({
          supportsStructured: true, structuredProtocol: 'claude-code-stream-json',
        })
        await f.send()
        expect(f.sends()).toHaveLength(0)
        const newerDraft = 'A different request for the next message'
        act(() => {
          const textarea = f.host.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')!
          Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')!.set!.call(textarea, newerDraft)
          textarea.dispatchEvent(new Event('input', { bubbles: true }))
        })
        act(() => writeChatComposerDraft('chat-1', { ...readChatComposerDraft('chat-1'), text: newerDraft, references: [reference, later] }))
        // R1 P2-1: a branch/base label refresh must not discard this already frozen authored input.
        act(() => useAppStore.setState((state) => ({ sessions: state.sessions.map((session) => session.id === 'chat-1'
          ? { ...session, workspaceBranchName: 'refreshed-label', workspaceBaseRef: 'refreshed-base' } : session) })))
        if (route !== 'goal') {
          expect(f.sends()).toHaveLength(0)
          const label = route === 'claude-plan' ? 'Keep planning' : route === 'claude-proceed' ? 'Accept edits and proceed' : 'Review each edit'
          const choice = [...f.host.querySelectorAll<HTMLButtonElement>('button')].find(button => button.textContent === label)
          expect(choice).toBeTruthy()
          await act(async () => choice!.click())
          if (route !== 'claude-plan') {
            expect(f.sends()).toHaveLength(0)
            act(() => writeChatComposerDraft('chat-1', { ...readChatComposerDraft('chat-1'), references: [reference, later, duringMode] }))
            remaining = [later, duringMode]
            await act(async () => finishMode())
            expect(changeMode).toHaveBeenCalledWith('chat-1', 'default')
          }
          expect(changeSafety).toHaveBeenCalledTimes(route === 'claude-proceed' ? 1 : 0)
        } else {
          expect(createGoal).toHaveBeenCalledWith('chat-1', exactPrompt, expect.any(Number), 'uam')
          await act(async () => finishGoal())
        }
        expect(f.sends()).toHaveLength(1)
        expect(f.sends()[0].payload).toMatchObject({ text: `${exactPrompt}\n\n${formatContextReferences([reference])}`, attachments: [attachment], steerNow: false })
        expect(f.sends()[0].payload.text).not.toContain(later.text)
        expect(f.sends()[0].payload.text).not.toContain(duringMode.text)
        const row = f.host.querySelector('[data-outgoing-message]')!
        const article = row.querySelector('article')!
        expect(row.textContent).toContain(exactPrompt)
        expect(row.querySelector('[aria-label="File attachments"]')?.textContent).toContain(attachment.name)
        await act(async () => f.pending[0].ack())
        expect(row.isConnected).toBe(true)
        expect(row.querySelector('article')).toBe(article)
        expect(article.getAttribute('data-delivery')).toBe('sending')
        expect(useAppStore.getState().messages['chat-1']).toHaveLength(2)
        expect(f.host.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')!.value).toBe(newerDraft)
        expect(readChatComposerDraft('chat-1').text).toBe(newerDraft)
        expect(readChatComposerDraft('chat-1').references).toEqual(remaining)
        expect(f.sends()).toHaveLength(1)
        expect(f.sideEffects()).toEqual([])
      } finally {
        await act(async () => f.pending[0]?.ack())
        f.cleanup()
        useAppStore.setState({ setGoal: previous.setGoal, setSessionApprovalMode: previous.setSessionApprovalMode, setSessionCommandSafetyTier: previous.setSessionCommandSafetyTier })
      }
    })

    it.each((['goal', 'claude-proceed', 'claude-permission'] as const).flatMap((route) => (['birth', 'host', 'workspace', 'provider', 'fallback-workspace'] as const).map((change) => ({ route, change }))))('refuses $route first-send after target $change changes without consuming its draft or snapshots', async ({ route, change }) => {
      const previous = useAppStore.getState()
      const reference = captureContextReference({ kind: 'branch', id: 'stale-target-reference', sourceChatId: 'chat-1', sourceChatCreatedAt: '2026-10-23T11:00:00Z',
        executionHostId: 'local', workspaceDirectory: '/workspace', branchName: 'feature/context', capturedAt: '2026-10-23T12:00:00Z', label: 'feature/context', text: 'Selected source context' })!
      const prompt = route === 'goal' ? '/goal Preserve this original input' : 'Preserve this original input'
      let finish!: () => void
      const createGoal = vi.fn(() => new Promise<{ ok: true; goalId: string }>((resolve) => { finish = () => resolve({ ok: true, goalId: 'stale-target-goal' }) }))
      const changeMode = vi.fn(() => route === 'claude-permission' ? Promise.resolve(true)
        : new Promise<boolean>((resolve) => { finish = () => resolve(true) }))
      const changeSafety = vi.fn(() => route === 'claude-permission'
        ? new Promise<boolean>((resolve) => { finish = () => resolve(true) }) : Promise.resolve(true))
      const f = mount({ text: prompt, attachments: [], references: [reference] }, () => {
        useAppStore.setState({ setGoal: createGoal })
        if (change === 'fallback-workspace') useAppStore.setState((state) => ({
          sessions: state.sessions.map((session) => session.id === 'chat-1' ? { ...session, executionHostId: 'local', folderId: 'fallback-folder', workspaceIsolationKind: 'gitWorktree', workspaceWorktreeDirectory: '', workspaceDirectory: '/workspace' } : session),
          folders: [{ id: 'fallback-folder', name: 'Fallback', parentId: null, directory: '/unused-folder', executionHostId: 'local', isExpanded: true, createdAt: new Date() }],
        }))
        if (route !== 'goal') useAppStore.setState((state) => ({
          setSessionApprovalMode: changeMode, setSessionCommandSafetyTier: changeSafety,
          sessions: state.sessions.map((session) => session.id === 'chat-1' ? { ...session, providerId: 'claude-cli', approvalMode: 'plan' } : session),
          acpBindingBySessionId: { 'chat-1': acpBindingFromCppChat({ ...chat(), providerId: 'claude-cli',
            acpSession: sanitizeCppAcpSession({ ...chat().acpSession, providerId: 'claude-cli', protocolKind: 'claude-code-stream-json', currentModeId: 'plan' }) }, undefined) },
        }))
      })
      try {
        await f.send()
        if (route !== 'goal') await act(async () => Array.from(f.host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Accept edits and proceed')!.click())
        expect(finish).toBeTypeOf('function')
        act(() => useAppStore.setState((state) => ({ sessions: state.sessions.map((session) => session.id !== 'chat-1' ? session : { ...session,
          ...(change === 'birth' ? { createdAt: new Date(session.createdAt.getTime() + 1000) } : {}),
          ...(change === 'host' ? { executionHostId: 'different-host' } : {}),
          ...(change === 'provider' ? { providerId: 'opencode-cli' } : {}),
          ...(change === 'workspace' || change === 'fallback-workspace' ? { workspaceDirectory: '/different-workspace' } : {}),
        }) })))
        await act(async () => finish())
        expect(f.sends()).toHaveLength(0)
        expect(changeSafety).toHaveBeenCalledTimes(route === 'claude-permission' ? 1 : 0)
        expect(readChatComposerDraft('chat-1')).toEqual({ text: prompt, attachments: [], references: [reference] })
        expect(f.host.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')!.value).toBe(prompt)
        if (route !== 'goal') {
          expect(f.host.textContent).not.toContain('Claude Plan mode is read-only')
          if (change !== 'birth') {
            const refusal = [...f.host.querySelectorAll<HTMLElement>('.uam-notice')].find((notice) =>
              notice.textContent?.includes("so this prompt wasn't sent. Review the selected references, then send again."))
            expect(refusal).toBeTruthy()
            expect(refusal!.textContent).toContain('Message not sent')
            // A prevented send asks for recovery; it must not use the green completion treatment.
            expect(refusal!.querySelector<SVGElement>('svg')!.style.color).toBe('var(--yellow)')
          }
        }
      } finally {
        f.cleanup()
        useAppStore.setState({ setGoal: previous.setGoal, setSessionApprovalMode: previous.setSessionApprovalMode, setSessionCommandSafetyTier: previous.setSessionCommandSafetyTier })
      }
    })

    it('keeps the replacement warning when the abandoned send is accepted late', async () => {
      const reference = captureContextReference({ id: 'late-accept-reference', kind: 'branch', sourceChatId: 'chat-1', sourceChatCreatedAt: '2026-10-23T12:00:00Z', executionHostId: 'local', workspaceDirectory: '/workspace',
        branchName: 'feature/context', capturedAt: '2026-10-23T12:00:00Z', label: 'Captured branch', text: 'Original ordinary context' })!
      const f = mount({ text: 'Late accepted draft', attachments: [], references: [reference] })
      try {
        await f.send()
        act(() => useAppStore.setState((state) => ({ sessions: state.sessions.map((session) => session.id === 'chat-1'
          ? { ...session, createdAt: new Date(session.createdAt.getTime() + 1000) } : session) })))
        f.renderCurrentSession()
        await act(async () => f.pending[0].ack())
        const notice = [...f.host.querySelectorAll<HTMLElement>('.uam-notice')].find((item) => item.textContent?.includes('Check the earlier input before sending again.'))
        expect(notice).toBeTruthy()
        expect(notice!.textContent).toContain('Check earlier message')
        expect(notice!.textContent).not.toContain('Message not sent')
      } finally {
        await act(async () => { for (const request of f.pending) request.ack() })
        f.cleanup()
      }
    })

    it.each([true, false])('keeps the replacement draft and newer ordinary send busy after an abandoned bridge send settles (accepted=%s)', async (accepted) => {
      const attachment = { id: 'matching-recreated-attachment', name: 'retained.txt', type: 'file' as const, size: 4, path: '/fixture/retained.txt' }
      const reference = captureContextReference({ id: 'original-ordinary-reference', kind: 'branch', sourceChatId: 'chat-1', sourceChatCreatedAt: '2026-10-23T12:00:00Z', executionHostId: 'local', workspaceDirectory: '/workspace',
        branchName: 'feature/context', capturedAt: '2026-10-23T12:00:00Z', label: 'Captured branch', text: 'Original ordinary context' })!
      const text = 'Identical draft after recreation'
      const f = mount({ text, attachments: [attachment], references: [reference] })
      try {
        await f.send()
        expect(f.sends()).toHaveLength(1)
        act(() => useAppStore.setState((state) => ({ sessions: state.sessions.map((session) => session.id === 'chat-1'
          ? { ...session, createdAt: new Date(session.createdAt.getTime() + 1000) } : session) })))
        f.renderCurrentSession()
        const replacementNotice = [...f.host.querySelectorAll<HTMLElement>('.uam-notice')].find((notice) => notice.textContent?.includes('Check the earlier input before sending again.'))
        expect(replacementNotice).toBeTruthy()
        expect(replacementNotice!.querySelector<SVGElement>('svg')!.style.color).toBe('var(--yellow)')
        const fresh = { ...reference, id: 'replacement-ordinary-reference', text: 'Replacement selected context' }
        act(() => writeChatComposerDraft('chat-1', { text, attachments: [attachment], references: [fresh] }))
        await f.send()
        expect(f.sends()).toHaveLength(2)
        expect(f.sends()[1].payload).toMatchObject({ text: text + '\n\n' + formatContextReferences([fresh]), attachments: [attachment] })
        await act(async () => accepted ? f.pending[0].ack() : f.pending[0].reject('Abandoned synthetic send refused'))
        expect(f.host.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')!.value).toBe(text)
        expect(f.host.querySelector('[aria-label="Remove retained.txt attachment"]')).toBeTruthy()
        expect(readChatComposerDraft('chat-1')).toEqual({ text, attachments: [attachment], references: [fresh] })
        expect(f.host.querySelector<HTMLButtonElement>('button[aria-label="Send prompt"]')!.disabled).toBe(true)
        act(() => f.host.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')!.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true })))
        expect(f.sends()).toHaveLength(2)
        await act(async () => f.pending[1].ack())
        expect(readChatComposerDraft('chat-1')).toEqual({ text: '', attachments: [] })
        expect(f.sideEffects()).toEqual([])
      } finally {
        await act(async () => { for (const request of f.pending) request.ack() })
        f.cleanup()
      }
    })

    it.each(['goal', 'claude-proceed', 'claude-permission'] as const)('recovers the composer after a same-id birth change during %s without releasing a newer send', async (route) => {
      const previous = useAppStore.getState()
      const reference = captureContextReference({ kind: 'branch', id: 'recreated-chat-reference', sourceChatId: 'chat-1', sourceChatCreatedAt: '2026-10-23T11:00:00Z',
        executionHostId: 'local', workspaceDirectory: '/workspace', branchName: 'feature/context', capturedAt: '2026-10-23T12:00:00Z', label: 'Saved before recreation', text: 'Original captured context' })!
      const original = { text: route === 'goal' ? '/goal Original held input' : 'Original held input', attachments: [], references: [reference] }
      let finish!: () => void
      const createGoal = vi.fn(() => new Promise<{ ok: true; goalId: string }>((resolve) => { finish = () => resolve({ ok: true, goalId: 'held-goal' }) }))
      const changeMode = vi.fn(() => route === 'claude-permission' ? Promise.resolve(true)
        : new Promise<boolean>((resolve) => { finish = () => resolve(true) }))
      const changeSafety = vi.fn(() => route === 'claude-permission'
        ? new Promise<boolean>((resolve) => { finish = () => resolve(true) }) : Promise.resolve(true))
      const f = mount(original, () => {
        useAppStore.setState({ setGoal: createGoal })
        if (route !== 'goal') useAppStore.setState((state) => ({
          setSessionApprovalMode: changeMode, setSessionCommandSafetyTier: changeSafety,
          sessions: state.sessions.map((session) => session.id === 'chat-1' ? { ...session, providerId: 'claude-cli', approvalMode: 'plan' } : session),
          acpBindingBySessionId: { 'chat-1': acpBindingFromCppChat({ ...chat(), providerId: 'claude-cli',
            acpSession: sanitizeCppAcpSession({ ...chat().acpSession, providerId: 'claude-cli', protocolKind: 'claude-code-stream-json', currentModeId: 'plan' }) }, undefined) },
        }))
      })
      try {
        await f.send()
        if (route !== 'goal') await act(async () => [...f.host.querySelectorAll<HTMLButtonElement>('button')].find((button) => button.textContent === 'Accept edits and proceed')!.click())
        const finishOriginal = finish
        expect(finishOriginal).toBeTypeOf('function')
        act(() => useAppStore.setState((state) => ({ sessions: state.sessions.map((session) => session.id === 'chat-1'
          ? { ...session, createdAt: new Date(session.createdAt.getTime() + 1000) } : session) })))
        // MainPanel updates this prop without remounting when only the birth changes.
        f.renderCurrentSession()
        expect(f.sends()).toHaveLength(0)
        expect(readChatComposerDraft('chat-1')).toEqual(original)
        expect(f.host.textContent).not.toContain('Claude Plan mode is read-only')
        const send = () => f.host.querySelector<HTMLButtonElement>('button[aria-label="Send prompt"]')!
        expect(send().disabled).toBe(false)

        const freshPrompt = 'Fresh input after recreation'
        const fresh = { ...reference, id: 'new-birth-reference', sourceChatCreatedAt: useAppStore.getState().sessions[0].createdAt.toISOString(), text: 'New birth context' }
        const freshDraft = route === 'goal' ? `/goal ${freshPrompt}` : freshPrompt
        act(() => {
          const textarea = f.host.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')!
          Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')!.set!.call(textarea, freshDraft)
          textarea.dispatchEvent(new Event('input', { bubbles: true }))
        })
        act(() => writeChatComposerDraft('chat-1', { text: freshDraft, attachments: [], references: [fresh] }))
        createGoal.mockResolvedValueOnce({ ok: true, goalId: 'new-birth-goal' })
        await f.send()
        if (route !== 'goal') await act(async () => [...f.host.querySelectorAll<HTMLButtonElement>('button')].find((button) => button.textContent === 'Keep planning')!.click())
        expect(f.sends()).toHaveLength(1)
        expect(f.sends()[0].payload.text).toBe(freshPrompt + '\n\n' + formatContextReferences([fresh]))
        expect(send().disabled).toBe(true)
        // The abandoned request may settle after a new send has acquired the busy state.
        await act(async () => finishOriginal())
        expect(f.sends()).toHaveLength(1)
        expect(send().disabled).toBe(true)
        expect(f.host.textContent).not.toContain('Goal was created, but the first prompt failed to send.')
        await act(async () => f.pending[0].ack())
        expect(readChatComposerDraft('chat-1')).toEqual({ text: '', attachments: [] })
        expect(f.host.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')!.value).toBe('')
        expect(f.sideEffects()).toEqual([])
      } finally {
        f.cleanup()
        useAppStore.setState({ setGoal: previous.setGoal, setSessionApprovalMode: previous.setSessionApprovalMode, setSessionCommandSafetyTier: previous.setSessionCommandSafetyTier })
      }
    })

    it.each(['Keep planning', 'Review each edit', 'Accept edits and proceed'] as const)('warns after unconfirmed Claude Plan %s delivery without consuming its draft or snapshots', async (choice) => {
      const previous = useAppStore.getState()
      const reference = captureContextReference({ kind: 'branch', id: 'unconfirmed-plan-reference', sourceChatId: 'chat-1', sourceChatCreatedAt: '2026-10-23T11:00:00Z',
        executionHostId: 'local', workspaceDirectory: '/workspace', branchName: 'feature/context', capturedAt: '2026-10-23T12:00:00Z', label: 'feature/context', text: 'Original selected source context' })!
      const draft = { text: 'Send this selected context', attachments: [], references: [reference] }
      const f = mount(draft, () => useAppStore.setState((state) => ({
        setSessionApprovalMode: vi.fn(async () => true), setSessionCommandSafetyTier: vi.fn(async () => true),
        sessions: state.sessions.map((session) => session.id === 'chat-1' ? { ...session, providerId: 'claude-cli', approvalMode: 'plan' } : session),
        acpBindingBySessionId: { 'chat-1': acpBindingFromCppChat({ ...chat(), providerId: 'claude-cli',
          acpSession: sanitizeCppAcpSession({ ...chat().acpSession, providerId: 'claude-cli', protocolKind: 'claude-code-stream-json', currentModeId: 'plan' }) }, undefined) },
      })))
      try {
        await f.send()
        await act(async () => [...f.host.querySelectorAll<HTMLButtonElement>('button')].find((button) => button.textContent === choice)!.click())
        expect(f.sends()).toHaveLength(1)
        expect(f.sends()[0].payload.text).toBe(draft.text + '\n\n' + formatContextReferences([reference]))
        await act(async () => f.pending[0].reject('Synthetic acknowledgement failure'))
        const recovery = [...f.host.querySelectorAll<HTMLElement>('.uam-notice')].find((notice) =>
          notice.textContent?.includes('Could not confirm the prompt. Check its saved state before retrying.'))
        expect(recovery).toBeTruthy()
        expect(recovery!.querySelector<SVGElement>('svg')!.style.color).toBe('var(--yellow)')
        expect(readChatComposerDraft('chat-1')).toEqual(draft)
        expect(f.host.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')!.value).toBe(draft.text)
        expect(f.sends()).toHaveLength(1)
        expect(f.sideEffects()).toEqual([])
      } finally {
        f.cleanup()
        useAppStore.setState({ setSessionApprovalMode: previous.setSessionApprovalMode, setSessionCommandSafetyTier: previous.setSessionCommandSafetyTier })
      }
    })

    it.each((['file', 'branch', 'worktree'] as const).flatMap((kind) => [true, false].map((accepted) => ({ kind, accepted }))))('sends captured $kind context and clears only its acknowledged snapshot (accepted=$accepted)', async ({ kind, accepted }) => {
      const snapshot = captureContextReference({ kind, id: 'source-send-snapshot', sourceChatId: 'chat-1', sourceChatCreatedAt: '2026-10-23T11:00:00Z', sourcePath: '/workspace/src/main.ts',
        branchName: 'feature/context', worktreeDirectory: '/workspace', executionHostId: 'local', workspaceDirectory: '/workspace', capturedAt: '2026-10-23T12:00:00Z', label: 'Captured source', text: 'The captured source text.' })!
      const newer = { ...snapshot, id: 'newer-file-snapshot', text: 'A later snapshot for the next message.' }
      const f = mount({ text: 'Read @src/main.ts', attachments: [], references: [snapshot] })
      let settled = false
      try {
        expect(f.host.querySelector('.uam-context-chip')?.textContent).toContain('Captured source')
        await f.send()
        const submitted = String(f.sends()[0].payload.text)
        expect(submitted.startsWith('Read @src/main.ts\n\nReferenced context (quoted snapshot):\n')).toBe(true)
        const delivered = JSON.parse(submitted.split('Referenced context (quoted snapshot):\n')[1])
        expect(delivered).toEqual(snapshot)
        expect(delivered).not.toHaveProperty('inputWriteCompleted')
        act(() => writeChatComposerDraft('chat-1', { ...readChatComposerDraft('chat-1'), references: [snapshot, newer] }))
        await act(async () => accepted ? f.pending[0].ack() : f.pending[0].reject('Fixture refusal'))
        settled = true
        expect(readChatComposerDraft('chat-1').references).toEqual(accepted ? [newer] : [snapshot, newer])
        expect(f.sends()).toHaveLength(1)
        expect(f.sideEffects()).toEqual([])
      } finally {
        if (!settled) await act(async () => f.pending[0]?.ack())
        f.cleanup()
      }
    })

    it.each(['changed-snapshot', 'recreated-chat'] as const)('does not clear a newer reference after ordinary Send acknowledgement: %s', async (change) => {
      const snapshot = captureContextReference({ kind: 'branch', id: 'ack-owned-reference', sourceChatId: 'chat-1', sourceChatCreatedAt: '2026-10-23T11:00:00Z',
        executionHostId: 'local', workspaceDirectory: '/workspace', branchName: 'feature/context', capturedAt: '2026-10-23T12:00:00Z', label: 'feature/context', text: 'Captured context' })!
      const newer = change === 'changed-snapshot' ? { ...snapshot, text: 'Newer context using the same saved ID' } : snapshot
      const f = mount({ text: 'Read captured context', attachments: [], references: [snapshot] })
      let settled = false
      try {
        await f.send()
        act(() => {
          if (change === 'recreated-chat') useAppStore.setState((state) => ({ sessions: state.sessions.map((chat) => chat.id === 'chat-1' ? { ...chat, createdAt: new Date(chat.createdAt.getTime() + 1000) } : chat) }))
          writeChatComposerDraft('chat-1', { text: 'New draft', attachments: [], references: [newer] })
        })
        await act(async () => f.pending[0].ack())
        settled = true
        expect(readChatComposerDraft('chat-1').references).toEqual([newer])
        expect(f.sends()).toHaveLength(1)
      } finally {
        if (!settled) await act(async () => f.pending[0]?.ack())
        f.cleanup()
      }
    })

    it.each([
      { action: 'Approve', mode: 'default', exactPrompt: 'Proceed with the plan.' },
      { action: 'Deny', mode: 'plan', exactPrompt: 'Do not proceed with this plan. Please revise it before making changes.' },
    ])('sends the exact Codex $action plan action without borrowing or consuming the independent composer draft', async ({ action, mode, exactPrompt }) => {
      const previous = useAppStore.getState()
      const draftText = 'Independent composer request stays here'
      const attachment = { id: 'independent-plan-file', name: 'independent.txt', type: 'file', size: 4, path: '/private-fixture/independent.txt' }
      const reference = captureContextReference({ id: 'independent-plan-reference', kind: 'message', sourceChatId: 'unrelated-chat', sourceMessageIndex: 9,
        executionHostId: 'local', workspaceDirectory: '/private-fixture', capturedAt: '2026-01-01T00:00:00.000Z', label: 'Independent plan quote', text: 'THIS QUOTE IS NOT PART OF THE CODEX DENY ACTION' })!
      const independentDraft = { text: draftText, attachments: [attachment], references: [reference] }
      const setMode = vi.fn(async () => true)
      const f = mount(independentDraft, () => useAppStore.setState(state => ({
        setSessionApprovalMode: setMode, markdownStoreAttachedBySessionId: { 'chat-1': [] },
        sessions: state.sessions.map(session => session.id === 'chat-1' ? { ...session, providerId: 'codex-cli', approvalMode: 'plan' } : session),
        messages: { 'chat-1': reconcileCppMessages('chat-1', undefined, [history[0], { ...history[1], planSummary: 'Review this bounded fixture plan.' }], true, 0) },
        acpBindingBySessionId: { 'chat-1': acpBindingFromCppChat({ ...chat(), providerId: 'codex-cli',
          acpSession: sanitizeCppAcpSession({ ...chat().acpSession, providerId: 'codex-cli', protocolKind: 'codex-app-server', currentModeId: 'plan' }) }, undefined) },
      })))
      try {
        expect(useAppStore.getState().providers.find(provider => provider.id === 'codex-cli')).toMatchObject({ supportsStructured: true, structuredProtocol: 'codex-app-server' })
        const actionButton = [...f.host.querySelectorAll<HTMLButtonElement>('button')].find(button => button.textContent === action)
        expect(actionButton).toBeTruthy()
        expect(f.sends()).toHaveLength(0)
        await act(async () => actionButton!.click())
        expect(setMode).toHaveBeenCalledTimes(1)
        expect(setMode).toHaveBeenCalledWith('chat-1', mode)
        expect(f.sends()).toHaveLength(1)
        expect(f.sends()[0].payload).toMatchObject({ text: exactPrompt, attachments: [], markdownStoreFiles: [], steerNow: false })
        expect(f.sends()[0].payload.text).not.toContain(draftText)
        expect(f.sends()[0].payload.text).not.toContain(reference.text)
        const row = f.host.querySelector('[data-outgoing-message]')!
        const article = row.querySelector('article')!
        expect(row.textContent).toContain(exactPrompt)
        expect(row.querySelector('[aria-label="File attachments"]')).toBeNull()
        expect(useAppStore.getState().messages['chat-1']).toHaveLength(2)
        await act(async () => f.pending[0].ack())
        expect(row.isConnected).toBe(true)
        expect(row.querySelector('article')).toBe(article)
        expect(article.getAttribute('data-delivery')).toBe('sending')
        expect(useAppStore.getState().messages['chat-1']).toHaveLength(2)
        expect(f.host.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')!.value).toBe(draftText)
        expect(readChatComposerDraft('chat-1')).toEqual(independentDraft)
        expect(f.sends()).toHaveLength(1)
        expect(f.sideEffects()).toEqual([])
      } finally {
        await act(async () => f.pending[0]?.ack())
        f.cleanup()
        useAppStore.setState({ setSessionApprovalMode: previous.setSessionApprovalMode, markdownStoreAttachedBySessionId: previous.markdownStoreAttachedBySessionId })
      }
    })

    it.each([
      { label: 'attachment/context ACK first', typed: '', ackFirst: true },
      { label: 'attachment/context push first', typed: '', ackFirst: false },
      { label: 'typed whitespace/context ACK first', typed: '  Read this context  ', ackFirst: true },
      { label: 'typed whitespace/context push first', typed: '  Read this context  ', ackFirst: false },
    ])('correlates the normalized native prompt without replacing its transcript slot ($label)', async ({ typed, ackFirst }) => {
      const previousSkills = useAppStore.getState().markdownStoreAttachedBySessionId
      const skill = { id: 'quoted-skill', title: 'Quoted skill', maker: '', review: '', dateCreated: '', dateUpdated: '', preview: '', filePath: '/skills/quoted-skill.md' }
      useAppStore.setState({ markdownStoreAttachedBySessionId: { 'chat-1': [skill] } })
      const attachment = { id: 'quoted-file', name: 'quoted-fixture.txt', type: 'file', size: 4, path: '/private-fixture/quoted-fixture.txt' }
      const reference = captureContextReference({ id: 'quoted-reference', kind: 'message', sourceChatId: 'source-chat', sourceMessageIndex: 3,
        executionHostId: 'local', workspaceDirectory: '/private-fixture', capturedAt: '2026-01-01T00:00:00.000Z', label: 'Quoted fixture', text: 'Quoted fixture payload' })!
      const { text: referenceText, ...referenceSource } = reference
      const context = `Referenced context (quoted snapshot):\n${JSON.stringify({ ...referenceSource, text: referenceText })}`
      const submittedText = `${typed.trim()}\n\n${context}`
      const nativeText = submittedText.trim() // Actual sendAcpPrompt normalization, not mocked away.
      if (!typed) expect(submittedText.startsWith('\n\n')).toBe(true)
      const f = mount({ text: typed, attachments: [attachment], references: [reference] })
      try {
        await f.send()
        expect(f.sends()).toHaveLength(1)
        expect(f.sends()[0].payload).toMatchObject({ text: nativeText, attachments: [attachment], markdownStoreFiles: [skill.filePath] })
        const row = f.host.querySelector('.uam-message-list [data-outgoing-message]')!
        const article = row.querySelector('article')!
        expect(row.querySelector('[aria-label="File attachments"]')?.textContent).toContain(attachment.name)
        expect(row.querySelector('[aria-label="Skills context"]')).not.toBeNull()
        expect(useAppStore.getState().messages['chat-1']).toHaveLength(2)
        expect(useAppStore.getState().acpBindingBySessionId['chat-1'].queuedPrompts).toEqual([])
        const nativeMessage = { ...f.inputIdentity(), role: 'user' as const, content: nativeText, createdAt: '2026-01-02T00:00:00.000Z', attachments: [attachment], markdownStoreFiles: [skill.filePath] }
        if (ackFirst) {
          await act(async () => f.pending[0].ack())
          // Generic ACK admits the request; only native history can provide its storage index.
          expect(useAppStore.getState().messages['chat-1']).toHaveLength(2)
          expect(f.host.querySelector('[data-message-index="2"]')).toBeNull()
          expect(f.host.querySelector('[data-outgoing-message]')).toBe(row)
        } else {
          push([...history, nativeMessage])
          expect(f.host.querySelector('[data-message-index="2"]')).toBe(row)
        }
        expect(row.querySelector('article')).toBe(article)
        if (ackFirst) push([...history, nativeMessage])
        else await act(async () => f.pending[0].ack())
        expect(f.host.querySelector('[data-message-index="2"]')).toBe(row)
        expect(row.querySelector('article')).toBe(article)
        expect(f.host.querySelectorAll('.uam-message-list [data-message-index="2"]')).toHaveLength(1)
        expect(f.host.querySelector('[data-outgoing-message]')).toBeNull()
        // Exact native action/input identity keeps correlation; stored bytes alone supply no write fact.
        expect(article.getAttribute('data-delivery')).toBe('sending')
        expect(row.querySelector('[aria-label="File attachments"]')?.textContent).toContain(attachment.name)
        expect(row.querySelector('[aria-label="Skills context"]')).not.toBeNull()
        expect(useAppStore.getState().messages['chat-1']).toHaveLength(3)
        expect(useAppStore.getState().messages['chat-1'][2].content).toBe(nativeText)
        expect(useAppStore.getState().messages['chat-1'][2].attachments).toEqual(expect.arrayContaining([expect.objectContaining({ id: attachment.id }), expect.objectContaining({ type: 'markdown-store' })]))
        expect(f.sends()).toHaveLength(1)
        expect(f.sideEffects()).toEqual([])
      } finally {
        await act(async () => f.pending[0]?.ack())
        f.cleanup()
        useAppStore.setState({ markdownStoreAttachedBySessionId: previousSkills })
      }
    })

    it('explains retained unconfirmed input without promising an unchanged draft after a deliberate second send', async () => {
      const f = mount()
      try {
        await f.send()
        const first = f.host.querySelector('[data-outgoing-message]')!
        await act(async () => f.pending[0].reject('Private bridge lost reply'))
        await f.send() // Deliberate second user action, never automatic replay.
        await act(async () => f.pending[1].ack())
        expect(f.host.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')!.value).toBe('')
        expect(first.isConnected).toBe(true)
        expect(first.querySelector('article')?.getAttribute('data-delivery')).toBe('unconfirmed')
        expect(first.textContent).toContain(outgoing.content)
        const explanation = first.querySelector('.conversation-turn__delivery')?.getAttribute('aria-label') ?? ''
        expect(explanation).toContain('retained here')
        expect(explanation).not.toMatch(/draft.*unchanged/i)
        expect(f.host.querySelector('[data-message-index="2"]')).not.toBe(first)
        expect(f.sends()).toHaveLength(2)
        expect(f.sideEffects()).toEqual([])
      } finally { f.cleanup() }
    })

    it('keeps the same outer row and article from click through ACK and native push (ACK before push)', async () => {
      const f = mount()
      try {
        await f.send()
        expect(f.sends()).toHaveLength(1)
        const row = f.host.querySelector('.uam-message-list [data-outgoing-message]')!
        const attempt = row.querySelector('article[data-delivery="sending"]')
        expect(attempt?.textContent).toContain(outgoing.content)
        expect(f.host.querySelector('[aria-label="Unsent prompts"]')).toBeNull()
        expect(f.host.querySelector('[aria-label="Retry message"]')).toBeNull()
        expect(useAppStore.getState().messages['chat-1']).toHaveLength(2)
        expect(useAppStore.getState().acpBindingBySessionId['chat-1'].queuedPrompts).toEqual([])
        expect(f.occurrences()).toBe(1)
        await act(async () => f.pending[0].ack())
        // ACK is admission only: the display attempt retains its row, without a store placeholder.
        expect(useAppStore.getState().messages['chat-1']).toHaveLength(2)
        expect(f.host.querySelector('[data-message-index="2"]')).toBeNull()
        expect(f.host.querySelector('[data-outgoing-message]')).toBe(row)
        expect(row.querySelector('article')).toBe(attempt)
        expect(attempt!.getAttribute('data-delivery')).toBe('sending')
        expect(row.querySelector('.conversation-turn__delivery')?.getAttribute('aria-label')).toContain('waiting for the provider to confirm delivery')
        expect(f.occurrences()).toBe(1)
        expect(f.host.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')!.value).toBe('')
        push([...history, { ...outgoing, ...f.inputIdentity() }])
        expect(f.host.querySelector('[data-message-index="2"]')).toBe(row)
        expect(row.querySelector('article')).toBe(attempt)
        expect(attempt!.getAttribute('data-delivery')).toBe('sending')
        expect(row.hasAttribute('data-outgoing-message')).toBe(false)
        expect(f.occurrences()).toBe(1)
        expect(f.sends()).toHaveLength(1)
        expect(f.sideEffects()).toEqual([])
      } finally { f.cleanup() }
    })

    it('keeps one native node through pending, unsent recovery and unconfirmed materialization when push precedes ACK', async () => {
      const f = mount()
      try {
        await f.send()
        const slot = f.host.querySelector('[data-outgoing-message]')!
        const article = slot.querySelector('article[data-delivery="sending"]')
        expect(article).not.toBeNull()
        push([...history, { ...outgoing, ...f.inputIdentity(), acpPromptNotSent: true, interrupted: true }], { pendingDirectPrompts: [receipt('direct-1', { ...f.inputIdentity(), existingUserMessageIndex: 2 })] })
        expect(f.host.querySelector('[data-message-index="2"]')).toBe(slot)
        expect(slot.querySelector('article')).toBe(article)
        expect(article!.getAttribute('data-delivery')).toBe('sending')
        expect(f.host.querySelector('[data-outgoing-message]')).toBeNull()
        expect(f.occurrences()).toBe(1)
        expect(f.host.textContent).not.toContain('Not sent')
        await act(async () => f.pending[0].ack())
        expect(f.host.querySelector('[data-message-index="2"]')).toBe(slot)
        expect(f.occurrences()).toBe(1)
        push([...history, { ...outgoing, ...f.inputIdentity(), acpPromptNotSent: true, interrupted: true }], { lifecycleState: 'error', running: false, unsentMessageIndices: [2], recoverableUnsentMessageIndices: [2] })
        expect(f.host.querySelector('[data-message-index="2"]')).toBe(slot)
        expect(slot.querySelector('article')).toBe(article)
        expect(article!.getAttribute('data-delivery')).toBe('unsent')
        expect(slot.querySelector<HTMLButtonElement>('[aria-label="Retry message"]')?.disabled).toBe(false)
        const status = slot.querySelector<HTMLElement>('.conversation-turn__delivery')!
        expect(status.tabIndex).toBe(0)
        expect(status.getAttribute('aria-label')).toContain('never resent automatically')
        push([...history, { ...outgoing, ...f.inputIdentity() }, { role: 'assistant', content: 'Delivered answer', createdAt: '2026-01-02T00:00:01.000Z' }])
        expect(f.host.querySelector('[data-message-index="2"]')).toBe(slot)
        expect(slot.querySelector('article')).toBe(article)
        // A following assistant row and a cleared not-sent flag are not action-local delivery evidence.
        expect(article!.getAttribute('data-delivery')).toBe('sending')
        expect(f.sends()).toHaveLength(1)
        expect(f.sideEffects()).toEqual([])
      } finally { f.cleanup() }
    })

    it('retains a delayed rejection as the same dotted unconfirmed row without recovery and keeps the draft', async () => {
      const f = mount()
      try {
        await f.send()
        const row = f.host.querySelector('[data-outgoing-message]')!
        const article = row.querySelector('article')
        await act(async () => f.pending[0].reject('Provider rejected the prompt.'))
        expect(f.host.querySelector('[data-outgoing-message]')).toBe(row)
        expect(row.querySelector('article')).toBe(article)
        expect(article!.getAttribute('data-delivery')).toBe('unconfirmed')
        expect(row.textContent).toContain('Delivery not confirmed')
        expect(row.textContent).not.toContain('Not sent')
        expect(row.querySelector('button')).toBeNull()
        expect(f.occurrences()).toBe(1)
        expect(useAppStore.getState().messages['chat-1']).toHaveLength(2)
        expect(f.host.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')!.value).toBe(outgoing.content)
        expect(f.host.querySelector('[aria-label="Retry message"]')).toBeNull()
        expect(f.sends()).toHaveLength(1)
        expect(f.sideEffects()).toEqual([])
      } finally { f.cleanup() }
    })

    it('keeps pending and unconfirmed rows across a ChatView remount while the request settles', async () => {
      const f = mount()
      try {
        await f.send()
        f.remount()
        expect(f.host.querySelector('[data-outgoing-message] article')?.getAttribute('data-delivery')).toBe('sending')
        await act(async () => f.pending[0].reject('Bridge lost'))
        f.remount()
        const row = f.host.querySelector('[data-outgoing-message]')!
        expect(row.querySelector('article')?.getAttribute('data-delivery')).toBe('unconfirmed')
        expect(row.textContent).toContain(outgoing.content)
        expect(row.querySelector('button')).toBeNull()
        expect(f.sends()).toHaveLength(1)
        expect(f.sideEffects()).toEqual([])
      } finally { f.cleanup() }
    })

    it('keeps the bound row when a legitimate unsent edit changes its bytes', async () => {
      const f = mount()
      try {
        await f.send()
        const row = f.host.querySelector('[data-outgoing-message]')!
        const article = row.querySelector('article')
        push([...history, { ...outgoing, ...f.inputIdentity(), acpPromptNotSent: true, interrupted: true }], { pendingDirectPrompts: [receipt('direct-1', { ...f.inputIdentity(), existingUserMessageIndex: 2 })] })
        await act(async () => f.pending[0].ack())
        push([...history, { ...outgoing, ...f.inputIdentity(), acpPromptNotSent: true, interrupted: true }], { lifecycleState: 'error', running: false, unsentMessageIndices: [2], recoverableUnsentMessageIndices: [2] })
        push([...history, { ...outgoing, ...f.inputIdentity(), content: 'Edited outgoing', acpPromptNotSent: true, interrupted: true }], { lifecycleState: 'error', running: false, unsentMessageIndices: [2], recoverableUnsentMessageIndices: [2] })
        expect(f.host.querySelector('[data-message-index="2"]')).toBe(row)
        expect(row.querySelector('article')).toBe(article)
        expect(row.textContent).toContain('Edited outgoing')
        expect(article!.getAttribute('data-delivery')).toBe('unsent')
        // Unrelated history at the same index is never adopted by the alias.
        push([...history, { role: 'user', content: 'Replaced history', createdAt: '2026-03-01T00:00:00.000Z' }])
        expect(f.host.querySelector('[data-message-index="2"]')?.textContent).toContain('Replaced history')
        // Losing a previously bound identity in this loaded window does not prove removal or delivery.
        expect(f.host.querySelector('[data-outgoing-message]')).toBe(row)
        expect(row.querySelector('article')).toBe(article)
        expect(row.textContent).toContain('Edited outgoing')
        expect(article!.getAttribute('data-delivery')).toBe('unconfirmed')
        expect(f.host.querySelector('[data-message-index="2"]')).not.toBe(row)
        expect(f.sideEffects()).toEqual([])
      } finally { f.cleanup() }
    })

    it.each([false, true])('keeps the captured outgoing slot before later assistant rows without claiming their native index (processingFirst=%s)', async processingFirst => {
      const f = mount()
      const placeholder = { id: 'anchor-empty-placeholder', sessionId: 'chat-1', role: 'assistant' as const, content: '', createdAt: new Date('2026-01-02T00:00:01Z') }
      const unrelated = { id: 'anchor-unrelated-output', sessionId: 'chat-1', role: 'assistant' as const, content: 'Unrelated recorded output remains separate.', providerId: 'codex-cli', createdAt: new Date('2026-01-02T00:00:02Z') }
      const processing = () => act(() => useAppStore.setState(state => ({ acpBindingBySessionId: {
        'chat-1': { ...state.acpBindingBySessionId['chat-1'], processing: true, lifecycleState: 'processing', turnUserMessageIndex: 2, turnAssistantMessageIndex: 3 },
      } })))
      const before = (first: Element, second: Element) => Boolean(first.compareDocumentPosition(second) & Node.DOCUMENT_POSITION_FOLLOWING)
      try {
        await f.send()
        const list = f.host.querySelector<HTMLElement>('.uam-message-list')!
        const row = list.querySelector<HTMLElement>('[data-outgoing-message]')!
        const article = row.querySelector('article')!
        const earlierAnswer = list.querySelector('[data-message-index="1"]')!
        expect(row.previousElementSibling).toBe(earlierAnswer)
        await act(async () => f.pending[0].ack())
        // Ordinary admission ACK creates no native user row and supplies no delivery evidence.
        expect(useAppStore.getState().messages['chat-1']).toHaveLength(2)
        expect(list.querySelector('[data-outgoing-message]')).toBe(row)
        expect(row.querySelector('article')).toBe(article)
        if (processingFirst) processing()
        act(() => useAppStore.setState(state => ({ messages: { 'chat-1': [...state.messages['chat-1'], placeholder] } })))
        const placeholderRow = list.querySelector('[data-message-index="2"]')!
        expect(placeholderRow).not.toBeNull()
        expect(placeholderRow).not.toBe(row)
        expect(useAppStore.getState().messages['chat-1'][2].role).toBe('assistant')
        expect(list.querySelector('[data-outgoing-message]')).toBe(row)
        expect(row.querySelector('article')).toBe(article)
        expect(row.parentElement).toBe(list)
        expect(row.previousElementSibling).toBe(earlierAnswer)
        expect(before(row, placeholderRow)).toBe(true)
        if (!processingFirst) processing()
        act(() => useAppStore.setState(state => ({ messages: { 'chat-1': [...state.messages['chat-1'], unrelated] } })))
        const unrelatedRow = list.querySelector('[data-message-index="3"]')!
        expect(unrelatedRow).not.toBeNull()
        expect(before(row, placeholderRow)).toBe(true)
        expect(before(placeholderRow, unrelatedRow)).toBe(true)
        expect(row.previousElementSibling).toBe(earlierAnswer)
        expect(row.querySelector('article')).toBe(article)
        expect(article.getAttribute('data-delivery')).toBe('sending')
        expect(row.querySelector('button')).toBeNull()
        expect(f.occurrences()).toBe(1)
        expect(useAppStore.getState().messages['chat-1']).toHaveLength(4)
        expect(useAppStore.getState().messages['chat-1'].slice(2).map(message => message.role)).toEqual(['assistant', 'assistant'])
        // A later exact native row may take over identity; the display anchor never authorizes delivery/retry.
        push([...history, { ...outgoing, ...f.inputIdentity(), acpPromptNotSent: true, interrupted: true },
          { ...placeholder, createdAt: placeholder.createdAt.toISOString() }, { ...unrelated, createdAt: unrelated.createdAt.toISOString() }], {
          pendingDirectPrompts: [receipt('anchor-native-direct', { ...f.inputIdentity(), existingUserMessageIndex: 2 })], processing: true, lifecycleState: 'processing',
        })
        expect(list.querySelector('[data-message-index="2"]')).toBe(row)
        expect(row.querySelector('article')).toBe(article)
        expect(article.getAttribute('data-delivery')).toBe('sending')
        expect(before(row, list.querySelector('[data-message-index="3"]')!)).toBe(true)
        expect(before(list.querySelector('[data-message-index="3"]')!, list.querySelector('[data-message-index="4"]')!)).toBe(true)
        expect(f.sends()).toHaveLength(1)
        expect(f.sideEffects()).toEqual([])
      } finally { f.cleanup() }
    })

    it('keeps two identical local intents at one captured boundary in their original insertion order', async () => {
      const f = mount()
      const before = (first: Element, second: Element) => Boolean(first.compareDocumentPosition(second) & Node.DOCUMENT_POSITION_FOLLOWING)
      try {
        await f.send()
        const first = f.host.querySelector<HTMLElement>('[data-outgoing-message]')!
        const firstArticle = first.querySelector('article')!
        await act(async () => f.pending[0].reject('Private reply lost'))
        await f.send() // Explicit user action; never automatic replay of the first intent.
        const second = f.host.querySelectorAll<HTMLElement>('[data-outgoing-message]')[1]
        const secondArticle = second.querySelector('article')!
        await act(async () => f.pending[1].ack())
        expect(useAppStore.getState().messages['chat-1']).toHaveLength(2)
        act(() => useAppStore.setState(state => ({ messages: { 'chat-1': [...state.messages['chat-1'],
          { id: 'same-anchor-assistant', sessionId: 'chat-1', role: 'assistant' as const, content: '', createdAt: new Date('2026-01-02T00:00:03Z') },
        ] } })))
        const placeholder = f.host.querySelector('[data-message-index="2"]')!
        expect(placeholder).not.toBeNull()
        expect(f.host.querySelectorAll('[data-outgoing-message]')).toHaveLength(2)
        expect(f.host.querySelectorAll('[data-outgoing-message]')[0]).toBe(first)
        expect(f.host.querySelectorAll('[data-outgoing-message]')[1]).toBe(second)
        expect(first.querySelector('article')).toBe(firstArticle)
        expect(second.querySelector('article')).toBe(secondArticle)
        expect(before(first, second)).toBe(true)
        expect(before(second, placeholder)).toBe(true)
        expect(firstArticle.getAttribute('data-delivery')).toBe('unconfirmed')
        expect(secondArticle.getAttribute('data-delivery')).toBe('sending')
        expect(first.querySelector('button')).toBeNull(); expect(second.querySelector('button')).toBeNull()
        expect(f.occurrences()).toBe(2)
        expect(f.sends()).toHaveLength(2)
        expect(f.sideEffects()).toEqual([])
      } finally { f.cleanup() }
    })

    it('keeps an ACK-only row pending when an empty assistant placeholder or processing state follows', async () => {
      const f = mount()
      try {
        await f.send()
        const row = f.host.querySelector('[data-outgoing-message]')!
        const article = row.querySelector('article')!
        await act(async () => f.pending[0].ack())
        act(() => useAppStore.setState((state) => ({
          messages: { 'chat-1': [...state.messages['chat-1'], { id: 'empty-placeholder', sessionId: 'chat-1', role: 'assistant' as const, content: '', createdAt: new Date() }] },
          acpBindingBySessionId: { 'chat-1': { ...state.acpBindingBySessionId['chat-1'], processing: true, lifecycleState: 'processing', turnUserMessageIndex: 2, turnAssistantMessageIndex: 3 } },
        })))
        // The native assistant keeps index 2; the local attempt keeps only its own identity/position.
        const placeholder = f.host.querySelector('[data-message-index="2"]')!
        expect(placeholder).not.toBeNull()
        expect(placeholder).not.toBe(row)
        expect(useAppStore.getState().messages['chat-1'][2].role).toBe('assistant')
        expect(f.host.querySelector('[data-outgoing-message]')).toBe(row)
        expect(row.querySelector('article')).toBe(article)
        expect(Boolean(row.compareDocumentPosition(placeholder) & Node.DOCUMENT_POSITION_FOLLOWING)).toBe(true)
        expect(article.getAttribute('data-delivery')).toBe('sending')
        expect(f.sends()).toHaveLength(1)
        expect(f.sideEffects()).toEqual([])
      } finally { f.cleanup() }
    })

    it('keeps an identical unknown rejection and a second attempt as separate intents', async () => {
      const f = mount()
      try {
        await f.send()
        const first = f.host.querySelector('[data-outgoing-message]')!
        await act(async () => f.pending[0].reject('Bridge lost'))
        expect(f.host.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')!.value).toBe(outgoing.content)
        await f.send()
        expect(f.sends()).toHaveLength(2)
        await act(async () => f.pending[1].ack())
        // Both attempts match the placeholder at index 2 exactly, so neither may claim it.
        const placeholder = f.host.querySelector('[data-message-index="2"]')!
        expect(placeholder).not.toBe(first)
        expect(first.isConnected).toBe(true)
        expect(first.querySelector('article')?.getAttribute('data-delivery')).toBe('unconfirmed')
        expect(f.host.querySelectorAll('[data-outgoing-message]')).toHaveLength(2)
        expect(f.sideEffects()).toEqual([])
      } finally { f.cleanup() }
    })

    it('lets only an exact action-origin receipt represent the attempt and keeps uncorrelated same-text receipts separate', async () => {
      const f = mount()
      try {
        push(history, { unsentDirectPrompts: [receipt('older-same-text')] })
        await f.send()
        const attemptRow = f.host.querySelectorAll('[data-outgoing-message]')[1]
        // A receipt that existed before capture cannot claim the attempt, even with identical text.
        expect(f.host.querySelectorAll('[data-outgoing-message]')).toHaveLength(2)
        expect(f.host.querySelector('[data-unsent-receipt-id="older-same-text"] article')?.getAttribute('data-delivery')).toBe('unconfirmed')
        expect(f.host.querySelector('[data-unsent-receipt-id="older-same-text"]')?.textContent).not.toContain('Not sent')
        push(history, { unsentDirectPrompts: [receipt('older-same-text')], pendingDirectPrompts: [receipt('other-skill', { markdownStoreFiles: ['/skills/other.md'] })] })
        expect(f.host.querySelectorAll('[data-outgoing-message]')).toHaveLength(3)
        push(history, { unsentDirectPrompts: [receipt('older-same-text')], pendingDirectPrompts: [receipt('other-skill', { markdownStoreFiles: ['/skills/other.md'] }), receipt('exact', f.inputIdentity())] })
        expect(f.host.querySelectorAll('[data-outgoing-message]')).toHaveLength(3)
        expect(f.host.querySelector('[data-unsent-receipt-id="exact"] article')?.getAttribute('data-delivery')).toBe('sending')
        // The exact new receipt takes over the attempt's own row; distinct intents keep theirs.
        expect(f.host.querySelector('[data-unsent-receipt-id="exact"]')).toBe(attemptRow)
        expect(f.host.querySelector('[data-unsent-receipt-id="other-skill"]')).not.toBe(attemptRow)
        expect(f.host.querySelector('[data-unsent-receipt-id="exact"] button')).toBeNull()
        expect(f.host.querySelector<HTMLButtonElement>('[data-unsent-receipt-id="older-same-text"] [aria-label="Retry message"]')?.disabled).toBe(true)
        await act(async () => f.pending[0].reject('Rejected'))
        expect(f.sends()).toHaveLength(1)
        expect(f.sideEffects()).toEqual([])
      } finally { f.cleanup() }
    })
  })


  it.each([
    { label: 'identified unknown', fields: { nativeInputId: '44444444-4444-4444-8444-444444444444', originRequestId: 'historical-action' }, expected: 'unconfirmed' },
    { label: 'positive write', fields: { nativeInputId: '44444444-4444-4444-8444-444444444444', originRequestId: 'historical-action', inputWriteCompleted: true }, expected: null },
    { label: 'malformed boolean', fields: { nativeInputId: '44444444-4444-4444-8444-444444444444', inputWriteCompleted: 'true' }, expected: 'unconfirmed' },
    { label: 'malformed input ID', fields: { nativeInputId: 'bad-id', inputWriteCompleted: true }, expected: null },
    { label: 'legacy absent fields', fields: {}, expected: null },
  ])('shares historical input presentation across main, managed and passive native routes: $label', async ({ fields, expected }) => {
    const previous = useAppStore.getState()
    const rows = [
      { role: 'user', content: 'Historical **input** remains readable', createdAt: '2026-10-16T16:00:00Z', ...fields },
      { role: 'assistant', content: 'Recorded answer\n\n```ts\nconst kept = true\n```', createdAt: '2026-10-16T16:00:01Z' },
    ] as unknown as Parameters<typeof reconcileCppMessages>[2]
    const child = { ...previous.sessions[0], id: 'input-child', name: 'Input child', providerId: 'codex-cli', importedReadOnly: true,
      subagentParentChatId: 'absent-parent', subagentParentCreatedAt: '2026-01-01T00:00:00.000Z' }
    const native = vi.fn(async () => ''); const load = vi.fn(async () => true)
    useAppStore.setState(state => ({ expandWorkTraces: true, openSubAgentSession: native, loadSessionMessages: load,
      sessions: [{ ...state.sessions[0], providerId: 'codex-cli' }, child],
      messages: { 'chat-1': reconcileCppMessages('chat-1', undefined, rows, true, 0), 'input-child': reconcileCppMessages('input-child', undefined, rows, true, 0) },
      historyStartIndexBySessionId: { 'chat-1': 0, 'input-child': 0 },
      acpBindingBySessionId: { 'chat-1': { ...state.acpBindingBySessionId['chat-1'], providerId: 'codex-cli', processing: false, turnEvents: [], toolCalls: [], pendingPermission: null, pendingUserInput: null } },
    }))
    const requests: string[] = []
    window.cefQuery = ({ request, onSuccess }) => { const parsed = JSON.parse(request); requests.push(parsed.action)
      onSuccess(JSON.stringify(parsed.action === 'getManagedAgentTranscript' ? { runId: 'input-run', title: 'Input child', status: 'completed', providerId: 'codex-cli', messages: rows } : {})) }
    const normalize = (html: string) => html.replace(/\s(id|for|aria-controls|aria-labelledby|aria-describedby)="[^"]*"/g, '')
    const views: Array<{ host: HTMLDivElement; root: ReturnType<typeof createRoot> }> = []
    const render = async (node: React.ReactNode) => { const host = document.createElement('div'); document.body.append(host); const root = createRoot(host)
      views.push({ host, root }); await act(async () => root.render(node)); return host }
    try {
      const main = await render(<ChatView session={useAppStore.getState().sessions[0]} />)
      const managed = await render(<ManagedSubagentConversation sourceChatId="chat-1" runId="input-run" transcriptChatId="input-transcript" />)
      const passive = await render(<NativeConversation chatId="input-child" />)
      const capture = (host: Element) => {
        const user = host.querySelector('article.conversation-turn--user')!
        expect(user).not.toBeNull()
        return { body: normalize(user.querySelector('.conversation-turn__body')!.outerHTML), delivery: user.getAttribute('data-delivery'),
          explanation: user.querySelector('.conversation-turn__delivery')?.getAttribute('aria-label') ?? null,
          answer: normalize(host.querySelector('article.conversation-turn--assistant .conversation-turn__body')!.outerHTML) }
      }
      const same = capture(main)
      expect(same.delivery).toBe(expected)
      expect(same.body).toContain('Historical'); expect(same.answer).toContain('kept')
      expect(capture(managed)).toEqual(same); expect(capture(passive)).toEqual(same)
      if (expected === 'unconfirmed') for (const host of [main, managed, passive]) {
        const user = host.querySelector('article.conversation-turn--user')!
        expect(user.querySelector('[aria-label="Retry message"],[aria-label="Edit message in new branch"],[aria-label="Revert to message in new branch"]')).toBeNull()
        expect(user.querySelector<HTMLElement>('.conversation-turn__delivery')?.tabIndex).toBe(0)
      }
      expect(native).not.toHaveBeenCalled()
      expect(requests.filter(action => /sendAcpPrompt|retry|resume|discard|remove|edit|openSubAgent|Preview|Image/i.test(action))).toEqual([])
    } finally {
      for (const view of views) { act(() => view.root.unmount()); view.host.remove() }
      delete window.cefQuery
      useAppStore.setState(previous, true)
    }
  })

  it('renders child history through the same canonical rows as the main transcript (main vs managed vs passive native)', async () => {
    const previous = useAppStore.getState()
    // Representative serialized history: Markdown, code, thoughts, tools, plan and an interrupted stop status.
    const rows = [
      { role: 'user' as const, content: 'Review **this** change', providerId: 'codex-cli', modelId: 'gpt-x', createdAt: '2026-10-16T16:00:00Z' },
      { role: 'assistant' as const, providerId: 'codex-cli', modelId: 'gpt-x', createdAt: '2026-10-16T16:00:09Z', processingTimeMs: 9000,
        content: 'Done.\n\n```ts\nconst ok = true\n```', thoughts: 'Inspect the diff', interrupted: true, stopReason: 'interrupt',
        planSummary: 'Verify', planEntries: [{ content: 'Run tests', status: 'completed' }],
        blocks: [{ type: 'thought', text: 'Inspect the diff' }, { type: 'tool_call', toolCallId: 'tool-1' }, { type: 'plan' }, { type: 'assistant_text', text: 'Done.\n\n```ts\nconst ok = true\n```' }],
        toolCalls: [{ id: 'tool-1', title: 'Run checks', kind: 'other', status: 'completed', content: 'CHECK OUTPUT' }] },
    ]
    const native = vi.fn(async () => '')
    const load = vi.fn(async () => true)
    const child = { ...previous.sessions[0], id: 'child-native', name: 'Child', providerId: 'codex-cli', importedReadOnly: true,
      subagentParentChatId: 'absent-parent', subagentParentCreatedAt: '2026-01-01T00:00:00.000Z' }
    useAppStore.setState((state) => ({ expandWorkTraces: true, openSubAgentSession: native, loadSessionMessages: load,
      sessions: [{ ...state.sessions[0], providerId: 'codex-cli' }, child],
      messages: { 'chat-1': reconcileCppMessages('chat-1', undefined, rows, true, 0), 'child-native': reconcileCppMessages('child-native', undefined, rows, true, 0) },
      historyStartIndexBySessionId: { 'chat-1': 0, 'child-native': 0 },
      acpBindingBySessionId: { 'chat-1': { ...state.acpBindingBySessionId['chat-1'], providerId: 'codex-cli', processing: false, turnEvents: [], toolCalls: [], pendingPermission: null, pendingUserInput: null } } }))
    const requests: string[] = []
    window.cefQuery = ({ request, onSuccess }) => { const parsed = JSON.parse(request); requests.push(parsed.action)
      onSuccess(JSON.stringify(parsed.action === 'getManagedAgentTranscript' ? { runId: 'run-1', title: 'Child', status: 'completed', providerId: 'codex-cli', messages: rows } : {})) }
    // useId values differ between trees; everything else must match exactly.
    const normalize = (html: string) => html.replace(/\s(id|for|aria-controls|aria-labelledby|aria-describedby)="[^"]*"/g, '')
    const capture = (container: Element) => ({
      assistant: normalize(container.querySelector('article.conversation-turn--assistant')!.outerHTML),
      user: normalize(container.querySelector('article.conversation-turn--user .conversation-turn__body')!.outerHTML),
      dividers: [...container.querySelectorAll('.uam-message-list > div > *')].map((element) => element.className).filter((name) => /divider|turn-start|conversation-work/.test(name)),
    })
    const render = async (node: React.ReactNode) => {
      const host = document.createElement('div'); document.body.append(host); const root = createRoot(host)
      await act(async () => root.render(node))
      return { host, root }
    }
    const main = await render(<ChatView session={useAppStore.getState().sessions[0]} />)
    const managed = await render(<ManagedSubagentConversation sourceChatId="chat-1" runId="run-1" transcriptChatId="transcript-1" />)
    const passive = await render(<NativeConversation chatId="child-native" />)
    try {
      const expected = capture(main.host.querySelector('.uam-message-list')!)
      expect(expected.assistant).toContain('CHECK OUTPUT'.slice(0, 0) + 'Run checks')
      expect(capture(managed.host.querySelector('.uam-message-list')!)).toEqual(expected)
      expect(capture(passive.host.querySelector('.uam-message-list')!)).toEqual(expected)
      // Normal disclosure behaviour: collapsing the section works the same way in a child.
      for (const host of [main.host, managed.host]) {
        const toggle = host.querySelector<HTMLElement>('.uam-message-list .conversation-work__toggle')
        if (toggle) await act(async () => toggle.click())
      }
      expect(capture(managed.host.querySelector('.uam-message-list')!).assistant).toBe(capture(main.host.querySelector('.uam-message-list')!).assistant)
      // Opening a child tool shows embedded content with no parent/native mutation, preview or resume IPC.
      const before = requests.length
      await act(async () => passive.host.querySelector<HTMLButtonElement>('button[title="Open tool details"]')?.click())
      await act(async () => managed.host.querySelector<HTMLButtonElement>('button[title="Open tool details"]')?.click())
      expect(requests.slice(before)).toEqual([])
      expect(native).not.toHaveBeenCalled()
      expect(passive.host.querySelector('[aria-label="Edit message in new branch"],[aria-label="Revert to message in new branch"]')).toBeNull()
      expect(managed.host.querySelector('[aria-label="Edit message in new branch"],[aria-label="Revert to message in new branch"]')).toBeNull()
      expect(requests.filter((action) => /Preview|Image|resume|openSubAgent/i.test(action))).toEqual([])
    } finally {
      for (const view of [main, managed, passive]) { act(() => view.root.unmount()); view.host.remove() }
      delete window.cefQuery
      useAppStore.setState({ openSubAgentSession: previous.openSubAgentSession, loadSessionMessages: previous.loadSessionMessages, expandWorkTraces: previous.expandWorkTraces })
    }
  })

})

describe('ChatView definite unsent local editing', () => {
  it('keeps an ordinary definitely-unsent edit local when submitted from the composer', async () => {
    const previous = useAppStore.getState()
    const previousDraft = readChatComposerDraft('chat-1')
    const previousUrl = window.location.href
    const previousManagedPanel = managedPanelFixture.enabled
    const previousResizeObserver = Object.getOwnPropertyDescriptor(globalThis, 'ResizeObserver')
    const branchFromMessage = vi.fn(async () => null)
    const retryFailedMessage = vi.fn(async () => ({ ok: false, error: 'Unexpected retry.' }))
    const sendAcpPrompt = vi.fn(async () => false)
    const nativeFiles = [
      { id: 'keep-native', name: 'keep.txt', type: 'file', mimeType: 'text/plain', size: 4, path: '/tmp/keep.txt' },
      { id: 'drop-native', name: 'drop.txt', type: 'file', mimeType: 'text/plain', size: 4, path: '/tmp/drop.txt' },
    ]
    const independentDraft = { text: 'Independent draft stays here', attachments: [
      { id: 'draft-file', name: 'draft.txt', type: 'file', mimeType: 'text/plain', size: 5, path: '/tmp/draft.txt' },
    ] }
    const chat: CppChat = {
      id: 'chat-1', title: 'Gemini Session', folderId: 'default', providerId: 'gemini-cli',
      createdAt: '2026-01-01T00:00:00.000Z', updatedAt: '2026-01-01T00:00:00.000Z',
      importedReadOnly: false, messageCount: 8, messagesDigest: '0123456789abcdef',
      messages: [{ role: 'user', providerId: 'gemini-cli', content: 'Original definitely-unsent input',
        createdAt: '2026-01-01T00:00:07.000Z', acpPromptNotSent: true, interrupted: true,
        acpPromptDiscarded: false, attachments: nativeFiles, markdownStoreFiles: ['/skills/inspect.md'] }],
      acpSession: { sessionId: 'native-ready', providerId: 'gemini-cli', protocolKind: 'gemini-acp',
        running: true, lifecycleState: 'ready', processing: false, readySinceLastSelect: true,
        turnUserMessageIndex: -1, turnAssistantMessageIndex: -1, turnEvents: [], toolCalls: [], planEntries: [],
        pendingPermission: null, pendingUserInput: null, unsentMessageIndices: [7], recoverableUnsentMessageIndices: [7],
        queuedPrompts: [], pendingDirectPrompts: [], unsentDirectPrompts: [] },
    }
    const rows = reconcileCppMessages('chat-1', undefined, chat.messages!, true, 7)
    const originalMessage = rows[0]
    const host = document.createElement('div')
    document.body.append(host)
    const root = createRoot(host)
    try {
      // EOF append: the earlier ChatView describe's beforeEach does not cover this test.
      managedPanelFixture.enabled = false
      Object.defineProperty(globalThis, 'ResizeObserver', { configurable: true, writable: true,
        value: class { observe() {} unobserve() {} disconnect() {} } })
      window.history.replaceState({}, '', '/')
      removeComposerDrafts(['chat-1'])
      writeChatComposerDraft('chat-1', independentDraft)
      useAppStore.setState({ ...useAppStore.getInitialState(),
        workingDisplayMode: 'verbose', collapsibleWorkSections: true, expandWorkTraces: true,
        folders: [{ id: 'default', name: 'Project', parentId: null, directory: '/tmp/project',
          isExpanded: true, createdAt: new Date(chat.createdAt) }],
        sessions: [{ id: chat.id, name: chat.title, providerId: chat.providerId, viewMode: 'chat', folderId: chat.folderId,
          workspaceDirectory: '/tmp/project', executionHostId: 'local', importedReadOnly: false,
          createdAt: new Date(chat.createdAt), updatedAt: new Date(chat.updatedAt),
          messageCount: chat.messageCount, messagesDigest: chat.messagesDigest, userMessageIndices: [7] }],
        activeSessionId: 'chat-1', historyStartIndexBySessionId: { 'chat-1': 7 }, messages: { 'chat-1': rows },
        acpBindingBySessionId: { 'chat-1': acpBindingFromCppChat(chat, undefined) }, cliBindingBySessionId: {},
        providers: [{ id: 'gemini-cli', name: 'Gemini CLI', shortName: 'Gemini', color: '#8ab4ff', description: '',
          outputMode: 'cli', supportsCli: true, supportsStructured: true, structuredProtocol: 'gemini-acp' }],
        uamAgentsBySessionId: { 'chat-1': [{ id: 'build', description: 'Build', builtIn: true }] },
        refreshUamAgents: vi.fn(async () => true), getVcsCommitStatus: vi.fn(async () => null),
        branchFromMessage, retryFailedMessage, sendAcpPrompt,
      }, true)

      await act(async () => root.render(<ChatView session={useAppStore.getState().sessions[0]} />))
      const binding = useAppStore.getState().acpBindingBySessionId['chat-1']
      expect(binding.unsentMessageIndices).toEqual([7])
      expect(binding.recoverableUnsentMessageIndices).toEqual([7])
      expect(binding.queuedPrompts).toEqual([])
      expect(binding.pendingDirectPrompts).toEqual([])
      expect(binding.unsentDirectPrompts).toEqual([])
      expect(binding.processing).toBe(false)
      expect(binding.pendingPermission).toBeNull()
      expect(binding.pendingUserInput).toBeNull()
      expect(binding.turnEvents).toEqual([])
      expect(binding.toolCalls).toEqual([])
      expect(useAppStore.getState().cliBindingBySessionId['chat-1']).toBeUndefined()
      expect(rows).toHaveLength(1)
      expect(originalMessage.nativeMessageIndex).toBe(7)
      expect(originalMessage.acpPromptNotSent).toBe(true)
      expect(originalMessage.acpPromptDiscarded).toBe(false)
      expect(originalMessage.attachments?.map(attachment => attachment.id)).toEqual(['/skills/inspect.md', 'keep-native', 'drop-native'])
      const recovery = host.querySelector<HTMLElement>('.uam-message-list')
      expect(recovery).not.toBeNull()
      expect(recovery!.querySelectorAll('[data-unsent-message-index="7"]')).toHaveLength(1)
      expect(host.querySelector('[data-unsent-receipt-id]')).toBeNull()
      expect(host.querySelector('[data-queue-id]')).toBeNull()
      const row = recovery!.querySelector<HTMLElement>('[data-unsent-message-index="7"]')!
      expect(row.textContent).toContain(originalMessage.content)
      expect(row.textContent).toContain('keep.txt')
      expect(row.textContent).toContain('drop.txt')
      expect(row.textContent).toContain('inspect.md')
      expect(host.querySelectorAll('article[aria-label="You"][data-delivery="unsent"]')).toHaveLength(1)
      const composer = host.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')
      expect(composer).not.toBeNull()
      expect(composer!.value).toBe(independentDraft.text)
      const edit = row.querySelector<HTMLButtonElement>('button[aria-label="Edit unsent message"]')
      expect(edit).not.toBeNull()
      expect(edit!.disabled).toBe(false)
      await act(async () => edit!.click())
      const editor = host.querySelector<HTMLTextAreaElement>('textarea[aria-label="Edit message"]')
      expect(editor).toBe(composer)
      expect(editor!.disabled).toBe(false)
      expect(editor!.value).toBe(originalMessage.content)
      expect(host.querySelectorAll('textarea')).toHaveLength(1)
      for (const name of ['keep.txt', 'drop.txt', 'inspect.md']) {
        const remove = host.querySelector<HTMLButtonElement>(`button[aria-label="Remove ${name} from edited message"]`)
        expect(remove).not.toBeNull()
        expect(remove!.disabled).toBe(false)
      }
      act(() => {
        Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')!.set!.call(editor, 'Edited definitely-unsent input')
        editor!.dispatchEvent(new Event('input', { bubbles: true }))
      })
      await act(async () => host.querySelector<HTMLButtonElement>('button[aria-label="Remove drop.txt from edited message"]')!.click())
      expect(editor!.value).toBe('Edited definitely-unsent input')
      expect(host.querySelector('button[aria-label="Remove drop.txt from edited message"]')).toBeNull()
      expect(host.querySelector('button[aria-label="Remove keep.txt from edited message"]')).not.toBeNull()
      expect(host.querySelector('button[aria-label="Remove inspect.md from edited message"]')).not.toBeNull()
      expect(useAppStore.getState().messages['chat-1'][0]).toBe(originalMessage)
      expect(originalMessage.attachments?.map(attachment => attachment.id)).toEqual(['/skills/inspect.md', 'keep-native', 'drop-native'])
      expect(readChatComposerDraft('chat-1')).toEqual(independentDraft)

      await act(async () => editor!.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true })))
      // First post-submit behavioral oracle: ordinary definite-unsent edits must not branch.
      expect(branchFromMessage).not.toHaveBeenCalled()
      expect(retryFailedMessage).not.toHaveBeenCalled()
      expect(sendAcpPrompt).not.toHaveBeenCalled()
      expect(readChatComposerDraft('chat-1')).toEqual(independentDraft)
    } finally {
      act(() => root.unmount())
      host.remove()
      useAppStore.setState(previous, true)
      removeComposerDrafts(['chat-1'])
      writeChatComposerDraft('chat-1', previousDraft)
      window.history.replaceState({}, '', previousUrl)
      managedPanelFixture.enabled = previousManagedPanel
      if (previousResizeObserver) Object.defineProperty(globalThis, 'ResizeObserver', previousResizeObserver)
      else Reflect.deleteProperty(globalThis, 'ResizeObserver')
    }
  })
})


describe('ChatView committed unsent editing', () => {
  function fixture() {
    const previous = useAppStore.getState()
    const previousDraft = readChatComposerDraft('chat-1')
    const previousSecondDraft = readChatComposerDraft('chat-2')
    const previousUrl = window.location.href
    const previousCef = window.cefQuery
    const previousManagedPanel = managedPanelFixture.enabled
    const previousResizeObserver = Object.getOwnPropertyDescriptor(globalThis, 'ResizeObserver')
    const branch = vi.fn(async () => null)
    const retry = vi.fn(async () => ({ ok: false }))
    const send = vi.fn(async () => false)
    const native = { role: 'user' as const, providerId: 'gemini-cli', content: 'Original unsent input',
      createdAt: '2026-01-01T00:00:07.000Z', acpPromptNotSent: true, acpPromptDiscarded: false, interrupted: true,
      attachments: [
        { id: 'keep', name: 'keep.txt', type: 'file', size: 4, path: '/tmp/keep.txt' },
        { id: 'drop', name: 'drop.txt', type: 'file', size: 4, path: '/tmp/drop.txt' },
      ], markdownStoreFiles: ['/skills/original.md'] }
    const rows = reconcileCppMessages('chat-1', undefined, [native], true, 7)
    const row = rows[0]
    const page = { chatId: 'chat-1', startIndex: 7, totalCount: 8, messagesDigest: '0123456789abcdef', unchanged: false, messages: [native] }
    const committed = { ...page, messagesDigest: 'fedcba9876543210', messages: [{ ...native, content: 'Edited local input', attachments: [native.attachments[0]] }] }
    const draft = { text: 'Independent draft', attachments: [{ id: 'draft', name: 'draft.txt', type: 'file', size: 4, path: '/tmp/draft.txt' }] }
    const requests: Array<{ action: string; payload?: Record<string, unknown> }> = []
    let reply!: (data: unknown) => void
    managedPanelFixture.enabled = false
    Object.defineProperty(globalThis, 'ResizeObserver', { configurable: true, writable: true,
      value: class { observe() {} unobserve() {} disconnect() {} } })
    window.history.replaceState({}, '', '/')
    removeComposerDrafts(['chat-1', 'chat-2']); writeChatComposerDraft('chat-1', draft)
    writeChatComposerDraft('chat-2', { text: 'Second pane draft', attachments: [] })
    window.cefQuery = ({ request, onSuccess }) => {
      const parsed = JSON.parse(request); requests.push(parsed)
      if (parsed.action === 'getChatMessages') onSuccess(JSON.stringify(page))
      else if (parsed.action === 'manageQueuedAcpPrompt') reply = data => onSuccess(JSON.stringify(data))
      else throw new Error(`Unexpected editor request: ${parsed.action}`)
    }
    const binding = acpBindingFromCppChat({ id: 'chat-1', title: 'Gemini', folderId: 'default', providerId: 'gemini-cli', createdAt: '', updatedAt: '',
      acpSession: { sessionId: 'native-ready', providerId: 'gemini-cli', protocolKind: 'gemini-acp', running: true,
        lifecycleState: 'ready', processing: false, unsentMessageIndices: [7], recoverableUnsentMessageIndices: [7],
        queuedPrompts: [], pendingDirectPrompts: [], unsentDirectPrompts: [] } }, undefined)
    const session = { id: 'chat-1', name: 'Gemini', providerId: 'gemini-cli', viewMode: 'chat' as const, folderId: 'default',
      executionHostId: 'local', workspaceDirectory: '/tmp/project', importedReadOnly: false, uamAgentId: 'current-agent',
      createdAt: new Date(0), updatedAt: new Date(0), messageCount: 8, messagesDigest: page.messagesDigest, userMessageIndices: [7] }
    const second = { ...session, id: 'chat-2', messageCount: 0, messagesDigest: '', userMessageIndices: [] }
    useAppStore.setState({ ...useAppStore.getInitialState(), sessions: [session, second], activeSessionId: 'chat-1',
      messages: { 'chat-1': rows, 'chat-2': [] }, historyStartIndexBySessionId: { 'chat-1': 7 },
      folders: [{ id: 'default', name: 'Project', parentId: null, directory: '/tmp/project', isExpanded: true, createdAt: new Date(0) }],
      acpBindingBySessionId: { 'chat-1': binding, 'chat-2': { ...binding, unsentMessageIndices: [], recoverableUnsentMessageIndices: [] } },
      cliBindingBySessionId: {}, uamAgentsBySessionId: { 'chat-1': [{ id: 'current-agent', description: 'Current agent', builtIn: false }] },
      refreshUamAgents: vi.fn(async () => true), getVcsCommitStatus: vi.fn(async () => null),
      branchFromMessage: branch, retryFailedMessage: retry, sendAcpPrompt: send,
    }, true)
    const host = document.createElement('div'); document.body.append(host); const root = createRoot(host)
    let mounted = true
    const editor = () => {
      const textarea = host.querySelector<HTMLTextAreaElement>('textarea[aria-label="Edit message"]')
      expect(textarea).not.toBeNull()
      return textarea!
    }
    const changeText = (text = 'Edited local input') => act(() => {
      const textarea = editor()
      expect(textarea.disabled).toBe(false)
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')!.set!.call(textarea, text)
      textarea.dispatchEvent(new Event('input', { bubbles: true }))
    })
    const open = async () => {
      await act(async () => root.render(<ChatView key="chat-1" session={useAppStore.getState().sessions[0]} />))
      const button = host.querySelector<HTMLButtonElement>('[data-unsent-message-index="7"] button[aria-label="Edit unsent message"]')
      expect(button).not.toBeNull(); expect(button!.disabled).toBe(false)
      await act(async () => button!.click())
      changeText()
      const remove = host.querySelector<HTMLButtonElement>('button[aria-label="Remove drop.txt from edited message"]')
      expect(remove).not.toBeNull(); expect(remove!.disabled).toBe(false)
      await act(async () => remove!.click())
      expect(editor().value).toBe('Edited local input')
      expect(host.querySelector('[aria-label="Remove keep.txt from edited message"]')).not.toBeNull()
      expect(host.querySelector('[aria-label="Remove original.md from edited message"]')).not.toBeNull()
    }
    const enter = async () => { await act(async () => editor().dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true }))) }
    const noExecution = () => { expect(branch).not.toHaveBeenCalled(); expect(retry).not.toHaveBeenCalled(); expect(send).not.toHaveBeenCalled() }
    const unmount = () => { act(() => root.unmount()); mounted = false }
    const cleanup = () => {
      if (mounted) unmount()
      host.remove(); useAppStore.setState(previous, true)
      removeComposerDrafts(['chat-1', 'chat-2']); writeChatComposerDraft('chat-1', previousDraft); writeChatComposerDraft('chat-2', previousSecondDraft)
      window.history.replaceState({}, '', previousUrl)
      if (previousCef) window.cefQuery = previousCef; else delete window.cefQuery
      managedPanelFixture.enabled = previousManagedPanel
      if (previousResizeObserver) Object.defineProperty(globalThis, 'ResizeObserver', previousResizeObserver)
      else Reflect.deleteProperty(globalThis, 'ResizeObserver')
    }
    return { host, root, row, session, second, committed, draft, requests, open, editor, changeText, enter, noExecution, unmount, cleanup,
      reply: async (data: unknown) => { expect(reply).toBeTypeOf('function'); await act(async () => reply(data)) } }
  }
  it.each(['ack-only', 'commit-push-first'] as const)('saves the local row and retained file/skill context after %s while preserving the independent draft', async order => {
    const f = fixture()
    try {
      await f.open()
      expect(f.host.textContent).toContain('Editing unsent message. Saving updates this input.')
      const save = Array.from(f.host.querySelectorAll('button')).find(button => button.textContent === 'Save changes')
      expect(save).toBeTruthy(); expect(save!.disabled).toBe(false)
      await f.enter()
      expect(f.requests.map(request => request.action)).toEqual(['getChatMessages', 'manageQueuedAcpPrompt'])
      expect(f.requests[1].payload).toEqual({ chatId: 'chat-1', operation: 'editUnsent', messageIndex: 7,
        expectedMessagesDigest: '0123456789abcdef', text: 'Edited local input', retainedAttachmentIds: ['keep'], retainedMarkdownStoreFiles: ['/skills/original.md'] })
      expect(useAppStore.getState().messages['chat-1'][0]).toBe(f.row)
      expect(f.editor().value).toBe('Edited local input')
      expect(f.editor().disabled).toBe(true)
      if (order === 'commit-push-first') act(() => useAppStore.setState(state => ({
        messages: { ...state.messages, 'chat-1': reconcileCppMessages('chat-1', state.messages['chat-1'], f.committed.messages, true, 7) },
        sessions: state.sessions.map(session => session.id === 'chat-1' ? { ...session, messagesDigest: f.committed.messagesDigest } : session),
      })))
      await f.reply(f.committed)
      f.noExecution()
      const saved = useAppStore.getState().messages['chat-1'][0]
      expect(saved).toMatchObject({ id: f.row.id, nativeMessageIndex: 7, content: 'Edited local input', acpPromptNotSent: true, createdAt: f.row.createdAt })
      expect(saved.attachments?.map(attachment => attachment.id)).toEqual(['/skills/original.md', 'keep'])
      expect(f.row.attachments?.map(attachment => attachment.id)).toEqual(['/skills/original.md', 'keep', 'drop'])
      expect(f.host.querySelector('textarea[aria-label="Edit message"]')).toBeNull()
      expect(f.host.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')!.value).toBe(f.draft.text)
      expect(f.host.querySelector('[data-unsent-message-index="7"]')?.textContent).toContain('Edited local input')
      expect(f.host.querySelector('[data-unsent-message-index="7"]')?.textContent).not.toContain('drop.txt')
      expect(useAppStore.getState().sessions).toHaveLength(2)
      expect(useAppStore.getState().sessions[0].uamAgentId).toBe('current-agent')
      expect(useAppStore.getState().activeSessionId).toBe('chat-1')
      expect(readChatComposerDraft('chat-1')).toEqual(f.draft)
    } finally { f.cleanup() }
  })
  it('serializes double Enter and retains edited text/resources after refusal before allowing an explicit save again', async () => {
    const f = fixture()
    try {
      await f.open()
      const textarea = f.editor()
      await act(async () => {
        textarea.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true }))
        textarea.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true }))
      })
      expect(f.requests.filter(request => request.action === 'manageQueuedAcpPrompt')).toHaveLength(1)
      await f.reply({ ok: false, error: 'The edited input could not be saved.' })
      expect(f.editor().disabled).toBe(false)
      expect(f.editor().value).toBe('Edited local input')
      expect(f.host.querySelector('[aria-label="Remove drop.txt from edited message"]')).toBeNull()
      expect(f.host.querySelector('[aria-label="Remove keep.txt from edited message"]')).not.toBeNull()
      expect(f.host.querySelector('[aria-label="Remove original.md from edited message"]')).not.toBeNull()
      expect(f.host.querySelector('[role="alert"]')?.textContent).toContain('The edited input could not be saved.')
      expect(useAppStore.getState().messages['chat-1'][0]).toBe(f.row)
      expect(readChatComposerDraft('chat-1')).toEqual(f.draft)
      await f.enter()
      expect(f.requests.filter(request => request.action === 'manageQueuedAcpPrompt')).toHaveLength(2)
      await f.reply(f.committed)
      expect(f.host.querySelector('textarea[aria-label="Edit message"]')).toBeNull()
      f.noExecution()
    } finally { f.cleanup() }
  })
  it('keeps the local editor when its original source object was replaced before Enter', async () => {
    const f = fixture()
    try {
      await f.open()
      const replacement = { ...f.row }
      act(() => useAppStore.setState({ messages: { ...useAppStore.getState().messages, 'chat-1': [replacement] } }))
      await f.enter()
      expect(f.requests).toHaveLength(0)
      expect(f.editor().value).toBe('Edited local input')
      expect(f.host.querySelector('[role="alert"]')?.textContent).toContain('unsent message changed')
      expect(useAppStore.getState().messages['chat-1'][0]).toBe(replacement)
      expect(readChatComposerDraft('chat-1')).toEqual(f.draft)
      f.noExecution()
    } finally { f.cleanup() }
  })
  it('retains a newer pushed source and the pending editor when an older commit reply arrives', async () => {
    const f = fixture()
    try {
      await f.open(); await f.enter()
      const pushed = { ...f.row, content: 'Newer saved input' }
      act(() => useAppStore.setState(state => ({ messages: { ...state.messages, 'chat-1': [pushed] },
        sessions: state.sessions.map(session => session.id === 'chat-1' ? { ...session, messagesDigest: 'aaaaaaaaaaaaaaaa' } : session) })))
      await f.reply(f.committed)
      expect(useAppStore.getState().messages['chat-1'][0]).toBe(pushed)
      expect(useAppStore.getState().sessions[0].messagesDigest).toBe('aaaaaaaaaaaaaaaa')
      expect(f.editor().value).toBe('Edited local input')
      expect(f.editor().disabled).toBe(false)
      expect(f.host.querySelector('[role="alert"]')?.textContent).toContain('unsent message changed')
      expect(readChatComposerDraft('chat-1')).toEqual(f.draft)
      f.noExecution()
    } finally { f.cleanup() }
  })
  it.each(['pane-change', 'unmount'] as const)('does not apply old editor feedback after %s', async departure => {
    const f = fixture()
    try {
      await f.open(); await f.enter()
      if (departure === 'unmount') f.unmount()
      else {
        act(() => useAppStore.setState({ activeSessionId: 'chat-2' }))
        await act(async () => f.root.render(<ChatView key={f.second.id} session={f.second} />))
        expect(f.host.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')!.value).toBe('Second pane draft')
      }
      await f.reply(f.committed)
      if (departure === 'pane-change') {
        expect(f.host.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')!.value).toBe('Second pane draft')
        expect(f.host.querySelector('textarea[aria-label="Edit message"]')).toBeNull()
        expect(f.host.querySelector('[role="alert"]')).toBeNull()
      }
      expect(readChatComposerDraft('chat-1')).toEqual(f.draft)
      expect(readChatComposerDraft('chat-2').text).toBe('Second pane draft')
      expect(useAppStore.getState().activeSessionId).toBe(departure === 'pane-change' ? 'chat-2' : 'chat-1')
      f.noExecution()
    } finally { f.cleanup() }
  })

  function controlledUnsentFocusFrames() {
    const frames: FrameRequestCallback[] = []
    let frameId = 0
    const request = vi.spyOn(window, 'requestAnimationFrame').mockImplementation(callback => {
      frames.push(callback)
      return ++frameId
    })
    return {
      pending: () => frames.length,
      flush: async () => {
        const pending = frames.splice(0)
        await act(async () => { for (const callback of pending) callback(0) })
      },
      restore: () => { frames.length = 0; request.mockRestore() },
    }
  }

  it.each(['rejected', 'committed'] as const)('restores the owning composer focus after a %s local save blurs the pending textarea', async outcome => {
    const f = fixture()
    let frames: ReturnType<typeof controlledUnsentFocusFrames> | undefined
    const sink = document.createElement('button')
    sink.textContent = 'Another control'
    document.body.append(sink)
    try {
      await f.open()
      const textarea = f.editor()
      act(() => textarea.focus())
      expect(document.activeElement).toBe(textarea)
      expect(useAppStore.getState().messages['chat-1'][0]).toBe(f.row)
      frames = controlledUnsentFocusFrames()
      await f.enter()
      expect(f.editor()).toBe(textarea)
      expect(textarea.disabled).toBe(true)
      // jsdom ignores blur() on this disabled input. Focus a real control to model
      // browser focus exit; this tests restoration, not actual browser behaviour.
      expect(document.activeElement).toBe(textarea)
      act(() => sink.focus())
      expect(document.activeElement).toBe(sink)
      await f.reply(outcome === 'committed' ? f.committed : { ok: false, error: 'The edited input could not be saved.' })
      const target = outcome === 'rejected' ? f.editor() : f.host.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')!
      expect(target).toBe(textarea)
      expect(target.disabled).toBe(false)
      expect(target.value).toBe(outcome === 'rejected' ? 'Edited local input' : f.draft.text)
      await frames.flush()
      expect(document.activeElement).toBe(target)
      expect(readChatComposerDraft('chat-1')).toEqual(f.draft)
      f.noExecution()
    } finally { frames?.restore(); sink.remove(); f.cleanup() }
  })

  it.each(['generation-change', 'pane-change', 'unmount'] as const)('does not restore stale local-save focus after %s between scheduling and the frame', async departure => {
    const f = fixture()
    let frames: ReturnType<typeof controlledUnsentFocusFrames> | undefined
    const sink = document.createElement('button')
    sink.textContent = 'Another control'
    document.body.append(sink)
    let restoreTextareaFocus: (() => void) | undefined
    try {
      await f.open()
      const textarea = f.editor()
      act(() => textarea.focus())
      expect(document.activeElement).toBe(textarea)
      frames = controlledUnsentFocusFrames()
      // Positive control proves that this fixture reaches an owned restoration.
      await f.enter()
      expect(textarea.disabled).toBe(true)
      expect(document.activeElement).toBe(textarea)
      act(() => sink.focus()) // Real focus-exit model; jsdom alone is not browser verification.
      expect(document.activeElement).toBe(sink)
      await f.reply({ ok: false, error: 'First save was refused.' })
      expect(f.editor()).toBe(textarea)
      expect(textarea.disabled).toBe(false)
      await frames.flush()
      expect(document.activeElement).toBe(textarea)

      await f.enter()
      expect(textarea.disabled).toBe(true)
      expect(document.activeElement).toBe(textarea)
      act(() => sink.focus())
      expect(document.activeElement).toBe(sink)
      await f.reply({ ok: false, error: 'Second save was refused.' })
      expect(f.requests.filter(request => request.action === 'manageQueuedAcpPrompt')).toHaveLength(2)
      expect(textarea.disabled).toBe(false)
      expect(frames.pending()).toBeGreaterThan(0)
      // Invalidate ownership after the response schedules focus but before its callback runs.
      if (departure === 'generation-change') {
        await act(async () => textarea.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })))
        expect(f.host.querySelector('textarea[aria-label="Edit message"]')).toBeNull()
      } else if (departure === 'pane-change') {
        act(() => useAppStore.setState({ activeSessionId: 'chat-2' }))
        await act(async () => f.root.render(<ChatView key={f.second.id} session={f.second} />))
        expect(f.host.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')!.value).toBe('Second pane draft')
      } else f.unmount()
      act(() => sink.focus())
      expect(document.activeElement).toBe(sink)
      const textareaFocus = vi.spyOn(HTMLTextAreaElement.prototype, 'focus')
      restoreTextareaFocus = () => textareaFocus.mockRestore()
      await frames.flush()
      expect(document.activeElement).toBe(sink)
      expect(textareaFocus).not.toHaveBeenCalled()
      expect(readChatComposerDraft('chat-1')).toEqual(f.draft)
      f.noExecution()
    } finally { restoreTextareaFocus?.(); frames?.restore(); sink.remove(); f.cleanup() }
  })
  it.each(['native', 'skill'] as const)('keeps colliding %s chip removal separate in the local unsent editor', async removed => {
    const f = fixture()
    try {
      const colliding = { ...f.row, attachments: f.row.attachments!.map(attachment => attachment.id === 'keep'
        ? { ...attachment, id: '/skills/original.md' } : attachment) }
      const originalResources = colliding.attachments.map(attachment => ({ ...attachment }))
      act(() => useAppStore.setState(state => ({ messages: { ...state.messages, 'chat-1': [colliding] } })))
      await f.open()
      const name = removed === 'native' ? 'keep.txt' : 'original.md'
      const remove = f.host.querySelector<HTMLButtonElement>(`[aria-label="Remove ${name} from edited message"]`)
      expect(remove).not.toBeNull(); expect(remove!.disabled).toBe(false)
      await act(async () => remove!.click())
      expect(Boolean(f.host.querySelector('[aria-label="Remove keep.txt from edited message"]'))).toBe(removed !== 'native')
      expect(Boolean(f.host.querySelector('[aria-label="Remove original.md from edited message"]'))).toBe(removed !== 'skill')
      expect(useAppStore.getState().messages['chat-1'][0]).toBe(colliding)
      expect(colliding.attachments).toEqual(originalResources)
      expect(readChatComposerDraft('chat-1')).toEqual(f.draft)
      expect(f.requests).toEqual([])
      f.noExecution()
    } finally { f.cleanup() }
  })

})

describe('ChatView delivered edit skill selection', () => {
  it.each(['retain-all', 'remove-F2', 'remove-skill', 'remove-colliding-native', 'remove-colliding-skill'] as const)('separates native file and skill retention in a refused delivered edit (%s)', async selection => {
    const previous = useAppStore.getState()
    const previousDraft = readChatComposerDraft('chat-1')
    const previousUrl = window.location.href
    const previousHistoryState = window.history.state
    const previousCef = window.cefQuery
    const previousManagedPanel = managedPanelFixture.enabled
    const previousResizeObserver = Object.getOwnPropertyDescriptor(globalThis, 'ResizeObserver')
    const branchFromMessage = vi.fn<typeof previous.branchFromMessage>(async () => null)
    const retryFailedMessage = vi.fn<typeof previous.retryFailedMessage>(async () => ({ ok: false, error: 'Unexpected retry.' }))
    const sendAcpPrompt = vi.fn<typeof previous.sendAcpPrompt>(async () => false)
    const editUnsentMessage = vi.fn<typeof previous.editUnsentMessage>(async () => ({ ok: false, error: 'Unexpected local edit.' }))
    const collidingIds = selection === 'remove-colliding-native' || selection === 'remove-colliding-skill'
    const nativeFiles = [
      { id: collidingIds ? '/skills/review.md' : 'F1', name: 'first.txt', type: 'file', mimeType: 'text/plain', size: 4, path: '/tmp/first.txt' },
      { id: 'F2', name: 'second.txt', type: 'file', mimeType: 'text/plain', size: 5, path: '/tmp/second.txt' },
    ]
    const independentDraft = { text: 'Independent delivered-edit draft', attachments: [
      { id: 'draft-file', name: 'draft.txt', type: 'file', mimeType: 'text/plain', size: 5, path: '/tmp/draft.txt' },
    ] }
    const chat: CppChat = {
      id: 'chat-1', title: 'Gemini Session', folderId: 'default', providerId: 'gemini-cli',
      createdAt: '2026-01-01T00:00:00.000Z', updatedAt: '2026-01-01T00:00:09.000Z',
      importedReadOnly: false, messageCount: 9, messagesDigest: '0123456789abcdef',
      messages: [
        { role: 'user', providerId: 'gemini-cli', content: 'Original delivered request',
          createdAt: '2026-01-01T00:00:07.000Z', acpPromptNotSent: false, acpPromptDiscarded: false,
          interrupted: false, attachments: nativeFiles, markdownStoreFiles: ['/skills/review.md'] },
        { role: 'assistant', providerId: 'gemini-cli', content: 'This is the substantive delivered response.',
          createdAt: '2026-01-01T00:00:08.000Z', acpPromptNotSent: false, interrupted: false },
      ],
      acpSession: { sessionId: 'native-ready', providerId: 'gemini-cli', protocolKind: 'gemini-acp',
        running: true, lifecycleState: 'ready', processing: false, readySinceLastSelect: true,
        turnUserMessageIndex: -1, turnAssistantMessageIndex: -1, turnEvents: [], toolCalls: [], planEntries: [],
        pendingPermission: null, pendingUserInput: null, unsentMessageIndices: [], recoverableUnsentMessageIndices: [],
        queuedPrompts: [], pendingDirectPrompts: [], unsentDirectPrompts: [] },
    }
    const rows = reconcileCppMessages('chat-1', undefined, chat.messages!, true, 7)
    const source = rows[0]
    const originalResources = source.attachments!.map(attachment => ({ ...attachment }))
    const removedNativeName = selection === 'remove-F2' ? 'second.txt' : selection === 'remove-colliding-native' ? 'first.txt' : null
    const removedSkill = selection === 'remove-skill' || selection === 'remove-colliding-skill'
    const retainedNativeIds = nativeFiles.filter(file => file.name !== removedNativeName).map(file => file.id)
    const retainedSkillFiles = removedSkill ? [] : ['/skills/review.md']
    const host = document.createElement('div'); document.body.append(host); const root = createRoot(host)
    try {
      // This appended describe has its own setup; the original ChatView beforeEach is scoped to its suite.
      managedPanelFixture.enabled = false
      Object.defineProperty(globalThis, 'ResizeObserver', { configurable: true, writable: true,
        value: class { observe() {} unobserve() {} disconnect() {} } })
      window.history.replaceState({}, '', '/')
      delete window.cefQuery
      removeComposerDrafts(['chat-1']); writeChatComposerDraft('chat-1', independentDraft)
      useAppStore.setState({ ...useAppStore.getInitialState(),
        workingDisplayMode: 'verbose', collapsibleWorkSections: true, expandWorkTraces: true, providerChatDefaults: {},
        folders: [{ id: 'default', name: 'Project', parentId: null, directory: '/tmp/project',
          isExpanded: true, createdAt: new Date(chat.createdAt) }],
        sessions: [{ id: chat.id, name: chat.title, providerId: chat.providerId, viewMode: 'chat', folderId: chat.folderId,
          workspaceDirectory: '/tmp/project', executionHostId: 'local', importedReadOnly: false,
          createdAt: new Date(chat.createdAt), updatedAt: new Date(chat.updatedAt),
          messageCount: chat.messageCount, messagesDigest: chat.messagesDigest, userMessageIndices: [7] }],
        activeSessionId: 'chat-1', historyStartIndexBySessionId: { 'chat-1': 7 }, messages: { 'chat-1': rows },
        acpBindingBySessionId: { 'chat-1': acpBindingFromCppChat(chat, undefined) }, cliBindingBySessionId: {},
        providers: [{ id: 'gemini-cli', name: 'Gemini CLI', shortName: 'Gemini', color: '#8ab4ff', description: '',
          outputMode: 'cli', supportsCli: true, supportsStructured: true, structuredProtocol: 'gemini-acp' }],
        uamAgentsBySessionId: { 'chat-1': [{ id: 'build', description: 'Build', builtIn: true }] },
        refreshUamAgents: vi.fn(async () => true), getVcsCommitStatus: vi.fn(async () => null),
        branchFromMessage, retryFailedMessage, sendAcpPrompt, editUnsentMessage,
      }, true)
      await act(async () => root.render(<ChatView key={chat.id} session={useAppStore.getState().sessions[0]} />))
      const binding = useAppStore.getState().acpBindingBySessionId['chat-1']
      expect(binding.processing).toBe(false)
      expect(binding.unsentMessageIndices).toEqual([])
      expect(binding.recoverableUnsentMessageIndices).toEqual([])
      expect(binding.queuedPrompts).toEqual([])
      expect(binding.pendingDirectPrompts).toEqual([])
      expect(binding.unsentDirectPrompts).toEqual([])
      expect(binding.pendingPermission).toBeNull()
      expect(binding.pendingUserInput).toBeNull()
      expect(useAppStore.getState().cliBindingBySessionId['chat-1']).toBeUndefined()
      expect(source.nativeMessageIndex).toBe(7)
      expect(source.acpPromptNotSent).toBe(false)
      expect(rows[1]).toMatchObject({ role: 'assistant', nativeMessageIndex: 8, content: 'This is the substantive delivered response.' })
      expect(host.textContent).toContain(rows[1].content)
      expect(host.querySelector('[aria-label="Unsent prompts"]')).toBeNull()
      expect(host.querySelector('[data-queue-id]')).toBeNull()
      expect(host.querySelector('[data-unsent-receipt-id]')).toBeNull()
      expect(host.querySelector('[aria-label="Edit unsent message"]')).toBeNull()
      expect(host.querySelectorAll('article[aria-label="You"]')).toHaveLength(1)
      const composer = host.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')
      expect(composer).not.toBeNull()
      expect(composer!.value).toBe(independentDraft.text)
      const delivered = host.querySelector('article[aria-label="You"]')!
      const edit = delivered.querySelector<HTMLButtonElement>('button[aria-label="Edit message in new branch"]')
      expect(edit).not.toBeNull(); expect(edit!.disabled).toBe(false)
      await act(async () => edit!.click())
      const editor = host.querySelector<HTMLTextAreaElement>('textarea[aria-label="Edit message"]')
      expect(editor).toBe(composer)
      expect(editor!.disabled).toBe(false)
      expect(editor!.value).toBe(source.content)
      expect(host.querySelector('.conversation-edit-strip')?.textContent).toContain('Saving creates a new branch.')
      for (const name of ['first.txt', 'second.txt', 'review.md']) {
        const remove = host.querySelector<HTMLButtonElement>(`button[aria-label="Remove ${name} from edited message"]`)
        expect(remove).not.toBeNull(); expect(remove!.disabled).toBe(false)
      }
      act(() => {
        Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')!.set!.call(editor, 'Revised delivered request')
        editor!.dispatchEvent(new Event('input', { bubbles: true }))
      })
      if (removedNativeName) await act(async () => host.querySelector<HTMLButtonElement>(`button[aria-label="Remove ${removedNativeName} from edited message"]`)!.click())
      if (removedSkill) await act(async () => host.querySelector<HTMLButtonElement>('button[aria-label="Remove review.md from edited message"]')!.click())
      expect(editor!.value).toBe('Revised delivered request')
      for (const name of ['first.txt', 'second.txt']) expect(Boolean(host.querySelector(`[aria-label="Remove ${name} from edited message"]`))).toBe(name !== removedNativeName)
      expect(Boolean(host.querySelector('[aria-label="Remove review.md from edited message"]'))).toBe(!removedSkill)
      const save = Array.from(host.querySelectorAll('button')).find(button => button.textContent === 'Save to new branch')
      expect(save).toBeTruthy(); expect(save!.disabled).toBe(false)
      expect(useAppStore.getState().messages['chat-1']).toBe(rows)
      expect(source.attachments).toEqual(originalResources)
      expect(readChatComposerDraft('chat-1')).toEqual(independentDraft)
      expect(branchFromMessage).not.toHaveBeenCalled()
      expect(retryFailedMessage).not.toHaveBeenCalled()
      expect(sendAcpPrompt).not.toHaveBeenCalled()
      expect(editUnsentMessage).not.toHaveBeenCalled()

      await act(async () => editor!.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true })))
      // Preserve the R66 first oracle: only selected native records contribute to the fourth argument.
      expect(branchFromMessage.mock.calls[0]?.[3]).toEqual(retainedNativeIds)
      expect(branchFromMessage.mock.calls[0]?.[4]).toEqual(retainedSkillFiles)
      expect(branchFromMessage).toHaveBeenCalledTimes(1)
      expect(branchFromMessage.mock.calls[0].slice(0, 3)).toEqual(['chat-1', 7, 'Revised delivered request'])
      expect(retryFailedMessage).not.toHaveBeenCalled()
      expect(sendAcpPrompt).not.toHaveBeenCalled()
      expect(editUnsentMessage).not.toHaveBeenCalled()
      expect(useAppStore.getState().messages['chat-1']).toBe(rows)
      expect(source.attachments).toEqual(originalResources)
      expect(useAppStore.getState().sessions).toHaveLength(1)
      expect(useAppStore.getState().activeSessionId).toBe('chat-1')
      expect(readChatComposerDraft('chat-1')).toEqual(independentDraft)
      expect(host.querySelector<HTMLTextAreaElement>('textarea[aria-label="Edit message"]')).toBe(editor)
      expect(editor!.value).toBe('Revised delivered request')
      expect(editor!.disabled).toBe(false)
      expect(host.querySelector('[role="alert"]')?.textContent).toContain('Could not create the message branch.')
      for (const name of ['first.txt', 'second.txt']) expect(Boolean(host.querySelector(`[aria-label="Remove ${name} from edited message"]`))).toBe(name !== removedNativeName)
      expect(Boolean(host.querySelector('[aria-label="Remove review.md from edited message"]'))).toBe(!removedSkill)
    } finally {
      act(() => root.unmount()); host.remove(); useAppStore.setState(previous, true)
      removeComposerDrafts(['chat-1']); writeChatComposerDraft('chat-1', previousDraft)
      window.history.replaceState(previousHistoryState, '', previousUrl)
      if (previousCef) window.cefQuery = previousCef; else delete window.cefQuery
      managedPanelFixture.enabled = previousManagedPanel
      if (previousResizeObserver) Object.defineProperty(globalThis, 'ResizeObserver', previousResizeObserver)
      else Reflect.deleteProperty(globalThis, 'ResizeObserver')
    }
  })
})


describe('managed initial composer acknowledgement R83', () => {
  it.each(['unmount-before-ack', 'remount-before-ack', 'remount-with-newer-draft', 'pagehide-after-ack', 'newer-draft', 'parent-rejected', 'goal-unmount-before-ack', 'plan-unmount-before-ack'] as const)('preserves exact draft ownership across %s', async (scenario) => {
    const previous = useAppStore.getState()
    const prompt = '@reviewer inspect this change'
    const isGoal = scenario === 'goal-unmount-before-ack'
    const isPlan = scenario === 'plan-unmount-before-ack'
    const submittedDraft = isGoal ? `/goal ${prompt}` : prompt
    const parentMessage = { role: 'user' as const, content: prompt, createdAt: '2026-10-17T13:00:00.000Z' }
    const chat: CppChat = { id: 'managed-draft-r83', title: 'Managed request', folderId: 'default', providerId: isPlan ? 'claude-cli' : 'codex-cli',
      createdAt: '2026-10-17T12:00:00.000Z', updatedAt: '2026-10-17T12:00:00.000Z', messages: [], messageCount: 0,
      acpSession: { sessionId: '', running: false, processing: false, currentModeId: isPlan ? 'plan' : 'default', turnSerial: 0 } }
    const session = { id: chat.id, name: chat.title, folderId: chat.folderId, providerId: chat.providerId, viewMode: 'chat' as const,
      createdAt: new Date(chat.createdAt), updatedAt: new Date(chat.updatedAt), messageCount: 0, approvalMode: isPlan ? 'plan' : 'default' }
    let acknowledge!: () => void
    let sends = 0
    let mounted = true
    const host = document.createElement('div'); document.body.append(host); let root = createRoot(host)
    const enter = (text: string) => act(() => {
      const input = host.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')!
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')!.set!.call(input, text)
      input.dispatchEvent(new Event('input', { bubbles: true }))
    })
    try {
      managedPanelFixture.enabled = false
      Object.defineProperty(globalThis, 'ResizeObserver', { configurable: true, writable: true,
        value: class { observe() {} unobserve() {} disconnect() {} } })
      window.history.replaceState({}, '', '/')
      removeComposerDrafts([chat.id]); writeChatComposerDraft(chat.id, { text: submittedDraft, attachments: [] })
      window.cefQuery = ({ request, onSuccess }) => {
        const parsed = JSON.parse(request)
        if (parsed.action === 'sendAcpPrompt') {
          sends += 1
          expect(parsed.payload.text).toBe(prompt)
          acknowledge = () => onSuccess(JSON.stringify(isGoal || isPlan ? {} : { managedMention: scenario === 'parent-rejected'
            ? { inputRecorded: false, delegationState: 'rejected', error: 'Parent publication failed.' }
            : { inputRecorded: true, delegationState: 'rejected', parentMessageIndex: 0, parentMessage, error: 'Child creation failed after saving the request.' } }))
        } else onSuccess('{}')
      }
      useAppStore.setState({ ...useAppStore.getInitialState(), sessions: [session], activeSessionId: chat.id,
        folders: [{ id: 'default', name: 'Project', parentId: null, directory: '', isExpanded: true, createdAt: session.createdAt }],
        messages: { [chat.id]: [] }, acpBindingBySessionId: { [chat.id]: acpBindingFromCppChat(chat, undefined) }, cliBindingBySessionId: {},
        providers: [{ id: chat.providerId, name: isPlan ? 'Claude' : 'Codex', shortName: 'Codex', color: '#fff', description: '',
          outputMode: 'cli', supportsCli: true, supportsStructured: true, structuredProtocol: isPlan ? 'claude-stream-json' : 'codex-app-server' }],
        refreshUamAgents: vi.fn(async () => true), getVcsCommitStatus: vi.fn(async () => null),
        setGoal: vi.fn(async () => ({ ok: true, goalId: 'private-goal-r83' })),
      }, true)
      await act(async () => root.render(<ChatView key={chat.id} session={session} />))
      await act(async () => host.querySelector('form')!.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true })))
      if (isPlan) await act(async () => Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Keep planning')!.click())
      expect(sends).toBe(1)
      if (scenario === 'newer-draft') enter('My newer draft')
      if (scenario === 'unmount-before-ack' || scenario === 'remount-before-ack' || scenario === 'remount-with-newer-draft' || isGoal || isPlan) { act(() => root.unmount()); mounted = false }
      if (scenario === 'remount-before-ack' || scenario === 'remount-with-newer-draft') { root = createRoot(host); await act(async () => root.render(<ChatView key={chat.id} session={session} />)); mounted = true }
      if (scenario === 'remount-with-newer-draft') enter('My newer draft')
      await act(async () => { acknowledge(); await Promise.resolve() })
      if (mounted) {
        const expected = (scenario === 'newer-draft' || scenario === 'remount-with-newer-draft') ? 'My newer draft' : scenario === 'parent-rejected' ? prompt : ''
        expect(host.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')!.value).toBe(expected)
        expect(host.textContent).toContain(scenario === 'parent-rejected' ? 'Parent publication failed.' : 'Child creation failed after saving the request.')
        act(() => window.dispatchEvent(new Event('pagehide')))
      }
      expect(readChatComposerDraft(chat.id).text).toBe((scenario === 'newer-draft' || scenario === 'remount-with-newer-draft') ? 'My newer draft' : scenario === 'parent-rejected' ? prompt : '')
      // Goal/plan receive generic admission only; the other cases exercise an explicit parent-publication receipt.
      expect(useAppStore.getState().messages[chat.id]).toHaveLength(scenario === 'parent-rejected' || isGoal || isPlan ? 0 : 1)
      expect(sends).toBe(1)
    } finally {
      if (mounted) act(() => root.unmount())
      host.remove(); delete window.cefQuery; removeComposerDrafts([chat.id]); useAppStore.setState(previous, true)
    }
  })
})

describe('ChatView conditional manual compaction offer', () => {
  async function unsupportedCompactionFixture(draft: string) {
    const previous = useAppStore.getState()
    const previousCefQuery = window.cefQuery
    const previousLocation = window.location.href
    const previousManagedPanelEnabled = managedPanelFixture.enabled
    const previousResizeObserver = Object.getOwnPropertyDescriptor(globalThis, 'ResizeObserver')
    const chat: CppChat = {
      id: 'unsupported-compact-opencode', title: 'Ordinary OpenCode conversation', folderId: 'default', providerId: 'opencode-cli',
      modelId: 'private/ordinary', executionHostId: 'local', workspaceDirectory: '/tmp/compaction-project', importedReadOnly: false,
      createdAt: '2026-10-17T12:00:00Z', updatedAt: '2026-10-17T12:00:01Z', messageCount: 2,
      messages: [{ role: 'user', content: 'Existing ordinary request', createdAt: '2026-10-17T12:00:00Z' },
        { role: 'assistant', content: 'Existing substantive answer', createdAt: '2026-10-17T12:00:01Z' }],
      acpSession: sanitizeCppAcpSession({ providerId: 'opencode-cli', protocolKind: 'opencode-acp', sessionId: 'private-opencode-session',
        running: true, processing: false, lifecycleState: 'ready', currentModelId: 'private/ordinary',
        availableCommands: [], pendingPermission: null, pendingUserInput: null,
        unsentMessageIndices: [], recoverableUnsentMessageIndices: [], queuedPrompts: [], pendingDirectPrompts: [], unsentDirectPrompts: [] }),
    }
    const file = { id: 'keep-compact-file', name: 'keep-compaction.txt', type: 'file' as const, size: 4, path: '/tmp/keep-compaction.txt' }
    const skill = { id: 'keep-compact-skill', title: 'Keep compaction skill', maker: '', review: '', dateCreated: '', dateUpdated: '', preview: '', filePath: '/skills/keep-compaction.md' }
    const sendAcpPrompt = vi.fn().mockResolvedValue(true)
    const requests: Array<{ action: string; payload?: unknown }> = []
    managedPanelFixture.enabled = false
    window.history.replaceState({}, '', '/')
    Object.defineProperty(globalThis, 'ResizeObserver', { configurable: true, writable: true,
      value: class { observe() {} unobserve() {} disconnect() {} } })
    removeComposerDrafts([chat.id]); writeChatComposerDraft(chat.id, { text: draft, attachments: [file] })
    const persistedDraft = readChatComposerDraft(chat.id)
    expect(persistedDraft.attachments).toEqual([file])
    window.cefQuery = ({ request, onSuccess, onFailure }) => {
      const parsed = JSON.parse(request)
      requests.push(parsed)
      if (parsed.action === 'compactAcpSession') onFailure(409, 'This provider does not support manual compaction.')
      else onSuccess('{}')
    }
    useAppStore.setState({ ...useAppStore.getInitialState(),
      sessions: [{ id: chat.id, name: chat.title, providerId: chat.providerId, modelId: chat.modelId, viewMode: 'chat', folderId: chat.folderId,
        workspaceDirectory: chat.workspaceDirectory, executionHostId: 'local', importedReadOnly: false,
        createdAt: new Date(chat.createdAt), updatedAt: new Date(chat.updatedAt), messageCount: 2 }],
      activeSessionId: chat.id, providerChatDefaults: {},
      folders: [{ id: 'default', name: 'Project', parentId: null, directory: chat.workspaceDirectory!, isExpanded: true, createdAt: new Date(chat.createdAt) }],
      messages: { [chat.id]: reconcileCppMessages(chat.id, undefined, chat.messages!, true) },
      acpBindingBySessionId: { [chat.id]: acpBindingFromCppChat(chat, undefined) }, cliBindingBySessionId: {},
      providers: [{ id: 'opencode-cli', name: 'OpenCode CLI', shortName: 'OpenCode', color: '#fff', description: '',
        outputMode: 'cli', supportsCli: true, supportsStructured: true, structuredProtocol: 'opencode-acp' }],
      markdownStoreAttachedBySessionId: { [chat.id]: [skill] },
      uamAgentsBySessionId: { [chat.id]: [{ id: 'build', description: 'Build', builtIn: true }] },
      refreshUamAgents: vi.fn(async () => true), refreshMarkdownStore: vi.fn(async () => true), getVcsCommitStatus: vi.fn(async () => null), sendAcpPrompt,
    }, true)
    const host = document.createElement('div'); document.body.append(host); const root = createRoot(host)
    const cleanup = () => {
      act(() => root.unmount()); host.remove(); removeComposerDrafts([chat.id]); useAppStore.setState(previous, true)
      managedPanelFixture.enabled = previousManagedPanelEnabled
      window.history.replaceState({}, '', previousLocation)
      if (previousCefQuery) window.cefQuery = previousCefQuery; else delete window.cefQuery
      if (previousResizeObserver) Object.defineProperty(globalThis, 'ResizeObserver', previousResizeObserver); else Reflect.deleteProperty(globalThis, 'ResizeObserver')
    }
    try {
      await act(async () => root.render(<ChatView key={chat.id} session={useAppStore.getState().sessions[0]} />))
      const textarea = host.querySelector<HTMLTextAreaElement>('.uam-composer-textarea')
      expect(textarea).not.toBeNull()
      expect(textarea!.disabled).toBe(false)
      expect(textarea!.value).toBe(draft)
      const binding = useAppStore.getState().acpBindingBySessionId[chat.id]
      expect(binding).toMatchObject({ providerId: 'opencode-cli', running: true, processing: false, lifecycleState: 'ready', pendingPermission: null, pendingUserInput: null })
      expect(binding.queuedPrompts).toEqual([])
      expect(binding.unsentMessageIndices).toEqual([])
      expect(host.textContent).toContain('Existing substantive answer')
      expect(host.textContent).toContain(file.name)
      expect(host.textContent).toContain(skill.title)
      return { host, textarea: textarea!, requests, sendAcpPrompt, chat, file, skill, persistedDraft, cleanup,
        render: () => root.render(<ChatView key={chat.id} session={useAppStore.getState().sessions[0]} />) }
    } catch (error) { cleanup(); throw error }
  }

  it('does not offer manual compaction for an ordinary unsupported OpenCode runtime', async () => {
    const f = await unsupportedCompactionFixture('/')
    try {
      const palette = document.body.querySelector('[aria-label="Slash commands"]')
      expect(palette).not.toBeNull()
      const options = Array.from(palette!.querySelectorAll('[role="option"]'))
      expect(options.some(option => option.textContent?.includes('/model'))).toBe(true)
      // First behavior oracle: the current UI unconditionally offers this known-unsupported action.
      expect(options.find(option => option.textContent?.includes('/compact'))).toBeUndefined()
      expect(f.requests.filter(request => request.action === 'compactAcpSession')).toEqual([])
      expect(f.sendAcpPrompt).not.toHaveBeenCalled()
      expect(f.textarea.value).toBe('/')
      expect(readChatComposerDraft(f.chat.id).attachments).toEqual(f.persistedDraft.attachments)
      expect(useAppStore.getState().markdownStoreAttachedBySessionId[f.chat.id]).toEqual([f.skill])
    } finally { f.cleanup() }
  })

  it.each(['enter', 'send'] as const)('refuses typed unsupported OpenCode compaction before any bridge or ordinary send (%s)', async (operation) => {
    const f = await unsupportedCompactionFixture('/compact')
    try {
      const send = f.host.querySelector<HTMLButtonElement>('button[aria-label="Send prompt"]')
      expect(send).not.toBeNull()
      expect(send!.disabled).toBe(false)
      await act(async () => {
        if (operation === 'enter') f.textarea.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true }))
        else send!.click()
      })
      // First post-action oracle is the existing IPC, not an absent future API/capability field.
      expect(f.requests.filter(request => request.action === 'compactAcpSession')).toEqual([])
      expect(f.sendAcpPrompt).not.toHaveBeenCalled()
      expect(f.requests.filter(request => request.action === 'sendAcpPrompt')).toEqual([])
      expect(f.textarea.value).toBe('/compact')
      act(() => window.dispatchEvent(new Event('pagehide')))
      expect(readChatComposerDraft(f.chat.id)).toEqual(f.persistedDraft)
      expect(useAppStore.getState().markdownStoreAttachedBySessionId[f.chat.id]).toEqual([f.skill])
      expect(useAppStore.getState().messages[f.chat.id]).toHaveLength(2)
    } finally { f.cleanup() }
  })

  const supportedCompactionChat = (chat: CppChat): CppChat => ({ ...chat, providerId: 'codex-cli', modelId: 'gpt-5.4',
    acpSession: sanitizeCppAcpSession({ providerId: 'codex-cli', protocolKind: 'codex-app-server', sessionId: 'private-codex-compact', threadId: 'private-codex-compact',
      currentModelId: 'gpt-5.4', running: true, processing: false, lifecycleState: 'ready', pendingPermission: null, pendingUserInput: null,
      queuedPrompts: [], pendingDirectPrompts: [], unsentDirectPrompts: [],
      manualCompaction: { available: true, providerId: 'codex-cli', modelId: 'gpt-5.4', nativeSessionId: 'private-codex-compact' } }) })

  function publishCompactionChat(chat: CppChat) {
    const state = useAppStore.getState()
    state.loadFromCef({ stateRevision: state.lastAppliedStateRevision + 1,
      chats: [chat], selectedChatId: chat.id,
      folders: [{ id: 'default', title: 'Project', directory: chat.workspaceDirectory!, executionHostId: 'local', collapsed: false }],
      providers: [{ id: 'opencode-cli', name: 'OpenCode', shortName: 'OpenCode', supportsStructured: true, supportsCli: true, structuredProtocol: 'opencode-acp' },
        { id: 'codex-cli', name: 'Codex', shortName: 'Codex', supportsStructured: true, supportsCli: true, structuredProtocol: 'codex-app-server' }],
      settings: sanitizeCppSettings({}),
    })
  }

  it.each(['enter', 'send', 'palette'] as const)('dispatches a supported matching native Codex compaction without ordinary send (%s)', async operation => {
    const f = await unsupportedCompactionFixture('/compact')
    try {
      const supported = supportedCompactionChat(f.chat)
      await act(async () => { publishCompactionChat(supported); f.render() })
      expect(useAppStore.getState().acpBindingBySessionId[f.chat.id].manualCompaction?.available).toBe(true)
      const row = Array.from(document.body.querySelectorAll('[aria-label="Slash commands"] [role="option"]')).find(option => option.textContent?.includes('/compact'))
      expect(row).toBeTruthy()
      window.cefQuery = ({ request, onSuccess }) => { f.requests.push(JSON.parse(request)); onSuccess(JSON.stringify({ accepted: true })) }
      await act(async () => {
        if (operation === 'palette') row!.dispatchEvent(new MouseEvent('mousedown', { bubbles: true }))
        else if (operation === 'enter') f.textarea.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true }))
        else f.host.querySelector<HTMLButtonElement>('button[aria-label="Send prompt"]')!.click()
      })
      expect(f.requests.filter(request => request.action === 'compactAcpSession')).toEqual([
        { action: 'compactAcpSession', payload: { chatId: f.chat.id }, requestId: expect.any(String) },
      ])
      expect(f.sendAcpPrompt).not.toHaveBeenCalled()
      expect(f.requests.filter(request => request.action === 'sendAcpPrompt')).toEqual([])
      expect(f.textarea.value).toBe('')
      act(() => window.dispatchEvent(new Event('pagehide')))
      expect(readChatComposerDraft(f.chat.id).attachments).toEqual([f.file])
      expect(useAppStore.getState().markdownStoreAttachedBySessionId[f.chat.id]).toEqual([f.skill])
      expect(useAppStore.getState().messages[f.chat.id]).toHaveLength(2)
    } finally { f.cleanup() }
  })

  it.each(['provider', 'model', 'native'] as const)('revalidates a connected stale compact option and typed submit against a native push (%s)', async drift => {
    for (const route of ['palette', 'submit'] as const) {
      const f = await unsupportedCompactionFixture('/compact')
      try {
        const supported = supportedCompactionChat(f.chat)
        await act(async () => { publishCompactionChat(supported); f.render() })
        const oldBinding = useAppStore.getState().acpBindingBySessionId[f.chat.id]
        const row = Array.from(document.body.querySelectorAll<HTMLElement>('[aria-label="Slash commands"] [role="option"]')).find(option => option.textContent?.includes('/compact'))
        const form = f.host.querySelector('form')!
        expect(row?.isConnected).toBe(true)
        const changed: CppChat = drift === 'provider' ? { ...supported, providerId: 'opencode-cli', acpSession: { ...supported.acpSession, providerId: 'opencode-cli', protocolKind: 'opencode-acp' } }
          : drift === 'model' ? { ...supported, modelId: 'gpt-5.4-new' }
          : { ...supported, acpSession: { ...supported.acpSession, sessionId: 'replacement-native', threadId: 'replacement-native' } }
        // Deliberately retain the old projection in the raw push; real sanitizer/reconciliation must revoke its mismatching anchors.
        await act(async () => {
          publishCompactionChat(changed)
          expect(useAppStore.getState().acpBindingBySessionId[f.chat.id]).not.toBe(oldBinding)
          expect(useAppStore.getState().acpBindingBySessionId[f.chat.id].manualCompaction).toBeUndefined()
          // Dispatch BEFORE React commits the new view. A detached element would be an invalid stale-handler oracle.
          expect(row!.isConnected && form.isConnected).toBe(true)
          if (route === 'palette') row!.dispatchEvent(new MouseEvent('mousedown', { bubbles: true }))
          else form.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true }))
        })
        expect(f.requests.filter(request => request.action === 'compactAcpSession')).toEqual([])
        expect(f.sendAcpPrompt).not.toHaveBeenCalled()
        expect(f.textarea.value).toBe('/compact')
        act(() => window.dispatchEvent(new Event('pagehide')))
        expect(readChatComposerDraft(f.chat.id)).toEqual(f.persistedDraft)
        expect(useAppStore.getState().markdownStoreAttachedBySessionId[f.chat.id]).toEqual([f.skill])
        await act(async () => { publishCompactionChat(supported); f.render() })
        const send = f.host.querySelector<HTMLButtonElement>('button[aria-label="Send prompt"]')!
        expect(send.disabled).toBe(false)
        await act(async () => send.click())
        expect(f.requests.filter(request => request.action === 'compactAcpSession')).toHaveLength(1) // Fresh matching support can recover.
      } finally { f.cleanup() }
    }
  })

  it.each(['omitted', 'malformed', 'false'] as const)('does not retain or advertise an old true capability after a current native snapshot (%s)', async condition => {
    const f = await unsupportedCompactionFixture('/compact')
    try {
      const supported = supportedCompactionChat(f.chat)
      await act(async () => { publishCompactionChat(supported); f.render() })
      const previous = useAppStore.getState().acpBindingBySessionId[f.chat.id]
      expect(previous.manualCompaction?.available).toBe(true)
      const raw = { ...supported.acpSession,
        availableCommands: [{ name: 'compact', description: 'Advertised command cannot replace native availability', inputHint: '' }],
        manualCompaction: condition === 'omitted' ? undefined : { ...supported.acpSession!.manualCompaction, available: condition === 'malformed' ? 'true' : false },
      }
      await act(async () => { publishCompactionChat({ ...supported, acpSession: sanitizeCppAcpSession(raw) }); f.render() })
      const current = useAppStore.getState().acpBindingBySessionId[f.chat.id]
      expect(current).not.toBe(previous)
      expect(current.manualCompaction?.available === true).toBe(false)
      expect(Array.from(document.body.querySelectorAll('[aria-label="Slash commands"] [role="option"]')).some(option => option.textContent?.includes('/compact'))).toBe(false)
      await act(async () => f.host.querySelector('form')!.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true })))
      expect(f.requests.filter(request => request.action === 'compactAcpSession')).toEqual([])
      expect(f.sendAcpPrompt).not.toHaveBeenCalled()
      expect(f.textarea.value).toBe('/compact')
      act(() => window.dispatchEvent(new Event('pagehide')))
      expect(readChatComposerDraft(f.chat.id)).toEqual(f.persistedDraft)
      expect(useAppStore.getState().markdownStoreAttachedBySessionId[f.chat.id]).toEqual([f.skill])
    } finally { f.cleanup() }
  })

  it.each(['model', 'native'] as const)('keeps an accepted compact ACK from clearing another native target and releases its pending UI (%s)', async drift => {
    const f = await unsupportedCompactionFixture('/compact')
    try {
      const supported = supportedCompactionChat(f.chat)
      await act(async () => { publishCompactionChat(supported); f.render() })
      let acknowledge = () => { throw new Error('Compact not requested') }
      window.cefQuery = ({ request, onSuccess }) => {
        const parsed = JSON.parse(request); f.requests.push(parsed)
        if (parsed.action === 'compactAcpSession') acknowledge = () => onSuccess(JSON.stringify({ accepted: true }))
        else onSuccess('{}')
      }
      await act(async () => f.host.querySelector('form')!.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true })))
      expect(f.requests.filter(request => request.action === 'compactAcpSession')).toHaveLength(1)
      const changed = drift === 'model' ? { ...supported, modelId: 'gpt-5.4-new' }
        : { ...supported, acpSession: { ...supported.acpSession, sessionId: 'replacement-native', threadId: 'replacement-native' } }
      await act(async () => { publishCompactionChat(changed); f.render() })
      await act(async () => acknowledge())
      expect(f.textarea.value).toBe('/compact')
      expect(f.textarea.disabled).toBe(false)
      expect(f.host.querySelector<HTMLButtonElement>('button[aria-label="Send prompt"]')!.disabled).toBe(false)
      expect(f.sendAcpPrompt).not.toHaveBeenCalled()
      act(() => window.dispatchEvent(new Event('pagehide')))
      expect(readChatComposerDraft(f.chat.id)).toEqual(f.persistedDraft)
      expect(useAppStore.getState().markdownStoreAttachedBySessionId[f.chat.id]).toEqual([f.skill])
    } finally { f.cleanup() }
  })
})
