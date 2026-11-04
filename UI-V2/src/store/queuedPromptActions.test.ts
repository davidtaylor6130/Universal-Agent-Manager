import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

const bridge = vi.hoisted(() => ({ cef: false, companion: false, send: vi.fn(async () => ({ ok: true })) }))
vi.mock('../ipc/cefBridge', async (importOriginal) => ({
  ...await importOriginal<typeof import('../ipc/cefBridge')>(),
  isCefContext: () => bridge.cef,
  isCompanionContext: () => bridge.companion,
  sendToCEF: bridge.send,
}))
import { useAppStore } from './useAppStore'
import { sanitizeQueuedPrompt } from './cpp/sanitizers'
import { readChatComposerDraft, removeComposerDrafts, writeChatComposerDraft } from '../utils/composerDraftStorage'
import { queuedPromptsEquivalent, reconcileCppMessages } from './cpp/reconcile'

const prompt = (id: string, text = id) => ({ id, revision: 1, text, uamAgentId: 'build', markdownStoreFiles: ['/skill.md'], attachments: [], goalMode: true, goalId: 'goal-1', computerUseMode: true })

beforeEach(() => {
  bridge.cef = false
  bridge.companion = false
  bridge.send.mockReset().mockResolvedValue({ ok: true })
  useAppStore.setState({ acpBindingBySessionId: { chat: { queuedPrompts: [prompt('one'), prompt('two')] } }, cliBindingBySessionId: {}, messages: { chat: [] }, cliTranscriptBySessionId: {}, chatHistoryErrorBySessionId: {} })
})

describe('queued prompt identity', () => {
  it.each(['error', 'offline'] as const)('lets an explicit bound-chat send and Retry reach native SSH recovery with cached %s status', async (runnerStatus) => {
    bridge.cef = true
    bridge.send.mockResolvedValue({ ok: false })
    useAppStore.setState({ sessions: [{ id: 'chat', executionHostId: 'ssh-lab' } as never],
      executionHosts: [{ id: 'ssh-lab', label: 'Lab', transport: 'ssh', sshAlias: 'never-connect', runnerStatus,
        runnerVersion: '4.11.0', platform: 'linux', architecture: 'x64', lastSeenAt: '' }] })
    expect(await useAppStore.getState().sendAcpPrompt('chat', 'User requested retry')).toBe(false)
    expect(bridge.send).toHaveBeenLastCalledWith(expect.objectContaining({ action: 'sendAcpPrompt', payload: expect.objectContaining({ chatId: 'chat' }) }))
    expect(await useAppStore.getState().retryFailedMessage('chat', 0)).toMatchObject({ ok: false })
    expect(bridge.send).toHaveBeenLastCalledWith({ action: 'retryFailedMessage', payload: { chatId: 'chat', messageIndex: 0 } })
  })
  it.each([0, 7])('uses an anchored native body hash to discard row %s without replacing loaded history', async (messageIndex) => {
    bridge.cef = true
    const native = { role: 'user' as const, content: 'Unsent', acpPromptNotSent: true, createdAt: '2026-01-01T00:00:00.000Z' }
    const row = reconcileCppMessages('chat', undefined, [native], true, messageIndex)[0]
    const later = { ...row, id: 'later', content: 'Delivered later', acpPromptNotSent: false, nativeMessageIndex: messageIndex + 1 }
    let finish!: (value: { ok: boolean }) => void
    bridge.send.mockImplementation(async (request) => request.action === 'getChatMessages'
      ? { ok: true, data: { chatId: 'chat', messagesDigest: 'body-hash', startIndex: messageIndex, totalCount: 200, messages: [native] } }
      : new Promise((resolve) => { finish = resolve }))
    useAppStore.setState({ sessions: [{ id: 'chat', messagesDigest: 'cold-summary', messageCount: 200 } as never], messages: { chat: [row, later] },
      acpBindingBySessionId: { chat: { unsentMessageIndices: [messageIndex] } }, historyStartIndexBySessionId: { chat: messageIndex },
      chatSearchNavigation: { chatId: 'chat', phase: 'ready' } as never })
    expect(await useAppStore.getState().discardUnsentMessage('chat', messageIndex, 'stale-row')).toBe(false)
    expect(bridge.send).not.toHaveBeenCalled()
    const discard = useAppStore.getState().discardUnsentMessage('chat', messageIndex, row.id)
    await vi.waitFor(() => expect(bridge.send).toHaveBeenCalledTimes(2))
    expect(bridge.send).toHaveBeenNthCalledWith(1, { action: 'getChatMessages', payload: {
      chatId: 'chat', limit: 1, before: messageIndex + 1, deferToolCallContent: true, messagesDigest: '' } })
    expect(bridge.send).toHaveBeenLastCalledWith({ action: 'manageQueuedAcpPrompt', payload: {
      chatId: 'chat', operation: 'discardUnsent', messageIndex, expectedMessagesDigest: 'body-hash' } })
    expect(useAppStore.getState().messages.chat).toEqual([row, later])
    finish({ ok: true })
    expect(await discard).toBe(true)
    expect(useAppStore.getState().messages.chat[0]).toMatchObject({ nativeMessageIndex: messageIndex, acpPromptNotSent: true, acpPromptDiscarded: true })
    expect(useAppStore.getState().messages.chat[1]).toBe(later)
    expect(useAppStore.getState().historyStartIndexBySessionId.chat).toBe(messageIndex)
    expect(useAppStore.getState().chatSearchNavigation?.phase).toBe('ready')
  })

  it.each(['readonly', 'companion', 'discarded', 'delivered'] as const)('blocks %s unsent discard before IPC', async (restriction) => {
    bridge.cef = true
    bridge.companion = restriction === 'companion'
    useAppStore.setState({ sessions: [{ id: 'chat', importedReadOnly: restriction === 'readonly' } as never],
      messages: { chat: [{ id: 'row', sessionId: 'chat', role: 'user', content: 'Unsent', acpPromptNotSent: restriction !== 'delivered',
        acpPromptDiscarded: restriction === 'discarded', createdAt: new Date() }] },
      acpBindingBySessionId: { chat: { unsentMessageIndices: [0] } }, historyStartIndexBySessionId: { chat: 0 } })
    expect(await useAppStore.getState().discardUnsentMessage('chat', 0, 'row')).toBe(false)
    expect(bridge.send).not.toHaveBeenCalled()
  })

  it.each(['changed-row', 'save-failure'] as const)('retains the input after %s during unsent discard', async (failure) => {
    bridge.cef = true
    const native = { role: 'user' as const, content: 'Original unsent', acpPromptNotSent: true, createdAt: '2026-01-01T00:00:00.000Z' }
    const row = reconcileCppMessages('chat', undefined, [native], true, 7)[0]
    bridge.send.mockImplementation(async (request) => request.action === 'getChatMessages' ? { ok: true, data: { chatId: 'chat', messagesDigest: 'new-body',
      startIndex: 7, totalCount: 8, messages: [{ ...native, content: failure === 'changed-row' ? 'Different unsent' : native.content }] } } : { ok: false })
    useAppStore.setState({ sessions: [{ id: 'chat' } as never], messages: { chat: [row] },
      acpBindingBySessionId: { chat: { unsentMessageIndices: [7] } }, historyStartIndexBySessionId: { chat: 7 } })
    expect(await useAppStore.getState().discardUnsentMessage('chat', 7, row.id)).toBe(false)
    expect(bridge.send).toHaveBeenCalledTimes(failure === 'changed-row' ? 1 : 2)
    expect(useAppStore.getState().messages.chat[0]).toBe(row)
    expect(row.acpPromptDiscarded).toBe(false)
  })
  it('preserves identity and detects revision and agent changes', () => {
    expect(sanitizeQueuedPrompt(prompt('one'))).toMatchObject({ id: 'one', revision: 1 })
    expect(sanitizeQueuedPrompt({ ...prompt('one'), id: undefined, revision: undefined })).not.toHaveProperty('id')
    expect(queuedPromptsEquivalent([prompt('one')], [{ ...prompt('one'), revision: 2 }])).toBe(false)
    expect(queuedPromptsEquivalent([prompt('one')], [{ ...prompt('one'), uamAgentId: 'review' }])).toBe(false)
    expect(sanitizeQueuedPrompt({ ...prompt('one'), prepared: true })?.prepared).toBe(true)
    expect(queuedPromptsEquivalent([prompt('one')], [{ ...prompt('one'), prepared: true }])).toBe(false)
  })
  it('edits text while retaining attached skills and options; rejects stale edits', async () => {
    expect(await useAppStore.getState().editQueuedAcpPrompt('chat', 'one', 1, 'Edited')).toBe(true)
    expect(useAppStore.getState().acpBindingBySessionId.chat.queuedPrompts?.[0]).toEqual({ ...prompt('one'), text: 'Edited', revision: 2 })
    expect(await useAppStore.getState().editQueuedAcpPrompt('chat', 'one', 1, 'Stale')).toBe(false)
    expect(await useAppStore.getState().editQueuedAcpPrompt('chat', 'gone', 1, 'Missing')).toBe(false)
  })
  it('reorders exact observed entries and rejects concurrent append or duplicate IDs', async () => {
    const expected = [{ id: 'one', revision: 1 }, { id: 'two', revision: 1 }]
    expect(await useAppStore.getState().reorderQueuedAcpPrompts('chat', expected, ['two', 'one'])).toBe(true)
    expect(useAppStore.getState().acpBindingBySessionId.chat.queuedPrompts?.map((entry) => entry.id)).toEqual(['two', 'one'])
    expect(await useAppStore.getState().reorderQueuedAcpPrompts('chat', expected, ['one', 'two'])).toBe(false)
    expect(await useAppStore.getState().reorderQueuedAcpPrompts('chat', expected, ['one', 'one'])).toBe(false)
    useAppStore.setState({ acpBindingBySessionId: { chat: { queuedPrompts: [prompt('one'), prompt('two'), prompt('three')] } } })
    expect(await useAppStore.getState().reorderQueuedAcpPrompts('chat', expected, ['two', 'one'])).toBe(false)
    expect(useAppStore.getState().acpBindingBySessionId.chat.queuedPrompts).toHaveLength(3)
  })
  it.each(['remove', 'steer'] as const)('rejects stale indices, revisions and prepared rows for %s', async (operation) => {
    const action = operation === 'remove' ? useAppStore.getState().removeQueuedAcpPrompt : useAppStore.getState().steerQueuedAcpPrompt
    const initial = useAppStore.getState().acpBindingBySessionId.chat.queuedPrompts
    expect(await action('chat', 0, { id: 'two', revision: 1 })).toBe(false)
    expect(useAppStore.getState().acpBindingBySessionId.chat.queuedPrompts).toEqual(initial)
    useAppStore.setState({ acpBindingBySessionId: { chat: { queuedPrompts: [{ ...prompt('one'), revision: 2 }, prompt('two')] } } })
    expect(await action('chat', 0, { id: 'one', revision: 1 })).toBe(false)
    useAppStore.setState({ acpBindingBySessionId: { chat: { queuedPrompts: [{ ...prompt('one'), prepared: true }, prompt('two')] } } })
    expect(await action('chat', 0, { id: 'one', revision: 1 })).toBe(false)
    expect(await action('chat', 0)).toBe(false)
    expect(useAppStore.getState().acpBindingBySessionId.chat.queuedPrompts).toHaveLength(2)
    expect(await action('chat', 1, { id: 'two', revision: 1 })).toBe(true)
    expect(useAppStore.getState().acpBindingBySessionId.chat.queuedPrompts?.map((entry) => entry.id)).toEqual(operation === 'remove' ? ['one'] : ['two', 'one'])
  })
  it('sends guarded native operations and leaves local state unchanged on save rejection', async () => {
    bridge.cef = true
    bridge.send.mockResolvedValue({ ok: false })
    expect(await useAppStore.getState().editQueuedAcpPrompt('chat', 'one', 1, 'Edited')).toBe(false)
    expect(bridge.send).toHaveBeenLastCalledWith({ action: 'manageQueuedAcpPrompt', payload: { chatId: 'chat', operation: 'edit', promptId: 'one', expectedRevision: 1, text: 'Edited' } })
    const expected = [{ id: 'one', revision: 1 }, { id: 'two', revision: 1 }]
    expect(await useAppStore.getState().reorderQueuedAcpPrompts('chat', expected, ['two', 'one'])).toBe(false)
    expect(bridge.send).toHaveBeenLastCalledWith({ action: 'manageQueuedAcpPrompt', payload: { chatId: 'chat', operation: 'reorder', expected, orderedIds: ['two', 'one'] } })
    const identity = { id: 'two', revision: 1 }
    expect(await useAppStore.getState().removeQueuedAcpPrompt('chat', 1, identity)).toBe(false)
    expect(bridge.send).toHaveBeenLastCalledWith({ action: 'manageQueuedAcpPrompt', payload: { chatId: 'chat', operation: 'remove', index: 1, expected: identity } })
    expect(await useAppStore.getState().steerQueuedAcpPrompt('chat', 1, identity)).toBe(false)
    expect(bridge.send).toHaveBeenLastCalledWith({ action: 'manageQueuedAcpPrompt', payload: { chatId: 'chat', operation: 'steer', index: 1, expected: identity } })
    expect(useAppStore.getState().acpBindingBySessionId.chat.queuedPrompts?.[0]).toEqual(prompt('one'))
  })
})

describe('native transcript release', () => {
  it('notifies desktop only after an actual idle transcript eviction', () => {
    bridge.cef = true
    useAppStore.getState().unloadSessionMessages('chat')
    expect(bridge.send).toHaveBeenCalledExactlyOnceWith({ action: 'releaseChatMessages', payload: { chatId: 'chat' } })
    useAppStore.getState().unloadSessionMessages('chat')
    expect(bridge.send).toHaveBeenCalledTimes(1)
  })
  it('keeps companion eviction local and does not release active turns', () => {
    bridge.cef = true
    bridge.companion = true
    useAppStore.getState().unloadSessionMessages('chat')
    expect(bridge.send).not.toHaveBeenCalled()
    bridge.companion = false
    useAppStore.setState({ messages: { chat: [] }, acpBindingBySessionId: { chat: { processing: true } } })
    useAppStore.getState().unloadSessionMessages('chat')
    expect(bridge.send).not.toHaveBeenCalled()
    expect(useAppStore.getState().messages.chat).toEqual([])
  })
})


describe('definitely-unsent local editing', () => {
  afterEach(() => removeComposerDrafts(['chat']))
  const originalDigest = '0123456789abcdef'
  const committedDigest = 'fedcba9876543210'
  function fixture() {
    bridge.cef = true
    const files = [
      { id: 'keep', name: 'keep.txt', type: 'file', size: 4, path: '/tmp/keep.txt' },
      { id: 'drop', name: 'drop.txt', type: 'file', size: 4, path: '/tmp/drop.txt' },
    ]
    const native = { role: 'user' as const, content: 'Original unsent', providerId: 'gemini-cli', interrupted: true,
      acpPromptNotSent: true, acpPromptDiscarded: false, attachments: files, markdownStoreFiles: ['/skills/original.md'],
      createdAt: '2026-01-01T00:00:07.000Z' }
    const row = reconcileCppMessages('chat', undefined, [native], true, 7)[0]
    const later = { ...row, id: 'later', nativeMessageIndex: 8, content: 'Later delivered input', acpPromptNotSent: false }
    const session = { id: 'chat', providerId: 'gemini-cli', executionHostId: 'local', importedReadOnly: false,
      messageCount: 200, messagesDigest: originalDigest, uamAgentId: 'current-agent', approvalMode: 'plan',
      createdAt: new Date(0), updatedAt: new Date(0) }
    const draft = { text: 'Independent draft', attachments: [{ id: 'draft', name: 'draft.txt', type: 'file', size: 4, path: '/tmp/draft.txt' }] }
    removeComposerDrafts(['chat']); writeChatComposerDraft('chat', draft)
    useAppStore.setState({ sessions: [session as never], activeSessionId: 'another-pane', messages: { chat: [row, later] },
      historyStartIndexBySessionId: { chat: 7 }, chatSearchNavigation: { chatId: 'chat', phase: 'ready' } as never,
      acpBindingBySessionId: { chat: { providerId: 'gemini-cli', processing: false, unsentMessageIndices: [7],
        queuedPrompts: [], pendingDirectPrompts: [], unsentDirectPrompts: [], pendingPermission: null, pendingUserInput: null } }, cliBindingBySessionId: {} })
    const retained = row.attachments!.filter(attachment => attachment.id !== 'drop')
    const page = { chatId: 'chat', messagesDigest: originalDigest, startIndex: 7, totalCount: 200, unchanged: false, messages: [native] }
    const committedNative = { ...native, content: 'Edited unsent', attachments: [files[0]] }
    const committed = { ...page, messagesDigest: committedDigest, messages: [committedNative] }
    return { row, later, native, retained, page, committed, session, draft }
  }
  it('commits only the anchored row and separates retained files and skill snapshots without changing current policy or draft', async () => {
    const f = fixture()
    let finish!: (value: unknown) => void
    bridge.send.mockImplementation(async request => request.action === 'getChatMessages'
      ? { ok: true, data: f.page } : new Promise(resolve => { finish = resolve }))
    const saving = useAppStore.getState().editUnsentMessage('chat', 7, f.row, '  Edited unsent  ', f.retained)
    await vi.waitFor(() => expect(bridge.send).toHaveBeenCalledTimes(2))
    expect(bridge.send).toHaveBeenNthCalledWith(1, { action: 'getChatMessages', payload: {
      chatId: 'chat', limit: 1, before: 8, deferToolCallContent: true, messagesDigest: '' } })
    expect(bridge.send).toHaveBeenLastCalledWith({ action: 'manageQueuedAcpPrompt', payload: {
      chatId: 'chat', operation: 'editUnsent', messageIndex: 7, expectedMessagesDigest: originalDigest, text: '  Edited unsent  ',
      retainedAttachmentIds: ['keep'], retainedMarkdownStoreFiles: ['/skills/original.md'] } })
    expect(useAppStore.getState().messages.chat).toEqual([f.row, f.later])
    expect(readChatComposerDraft('chat')).toEqual(f.draft)
    finish({ ok: true, data: f.committed })
    expect(await saving).toEqual({ ok: true })
    const state = useAppStore.getState()
    expect(state.messages.chat[0]).toMatchObject({ id: f.row.id, nativeMessageIndex: 7, content: 'Edited unsent',
      acpPromptNotSent: true, acpPromptDiscarded: false, createdAt: f.row.createdAt })
    expect(state.messages.chat[0].attachments?.map(attachment => attachment.id)).toEqual(['/skills/original.md', 'keep'])
    expect(f.row.attachments?.map(attachment => attachment.id)).toEqual(['/skills/original.md', 'keep', 'drop'])
    expect(state.messages.chat[1]).toBe(f.later)
    expect(state.sessions[0]).toMatchObject({ uamAgentId: 'current-agent', approvalMode: 'plan', messagesDigest: committedDigest, messageCount: 200 })
    expect(state.activeSessionId).toBe('another-pane')
    expect(state.historyStartIndexBySessionId.chat).toBe(7)
    expect(state.chatSearchNavigation?.phase).toBe('ready')
    expect(readChatComposerDraft('chat')).toEqual(f.draft)
    expect(bridge.send).toHaveBeenCalledTimes(2)
  })
  it('confirms native ASCII trimming without removing authored non-ASCII edge characters', async () => {
    const f = fixture()
    const text = '\u00a0Edited unsent\u00a0'
    const committed = { ...f.committed, messages: [{ ...f.committed.messages[0], content: text }] }
    bridge.send.mockImplementation(async request => ({ ok: true, data: request.action === 'getChatMessages' ? f.page : committed }))
    expect(await useAppStore.getState().editUnsentMessage('chat', 7, f.row, ` \t${text}\r\n`, f.retained)).toEqual({ ok: true })
    expect(useAppStore.getState().messages.chat[0].content).toBe(text)
  })
  it.each(['readonly', 'companion', 'queue', 'pending-direct', 'receipt', 'processing', 'output', 'stale-object', 'foreign-resource', 'duplicate-resource'] as const)(
    'rejects %s local edits before IPC', async restriction => {
      const f = fixture()
      let retained = f.retained
      if (restriction === 'readonly') useAppStore.setState({ sessions: [{ ...f.session, importedReadOnly: true } as never] })
      if (restriction === 'companion') bridge.companion = true
      if (restriction === 'stale-object') useAppStore.setState({ messages: { chat: [{ ...f.row }, f.later] } })
      if (restriction === 'output') useAppStore.setState({ messages: { chat: [f.row, { ...f.later, role: 'assistant', content: 'Substantive response' }] } })
      if (restriction === 'foreign-resource') retained = [...retained, { id: 'foreign', name: 'foreign', type: 'file', size: 0 }]
      if (restriction === 'duplicate-resource') retained = [...retained, retained[0]]
      if (['queue', 'pending-direct', 'receipt', 'processing'].includes(restriction)) {
        const field = restriction === 'queue' ? 'queuedPrompts' : restriction === 'pending-direct' ? 'pendingDirectPrompts' : 'unsentDirectPrompts'
        useAppStore.setState(state => ({ acpBindingBySessionId: { chat: { ...state.acpBindingBySessionId.chat,
          ...(restriction === 'processing' ? { processing: true } : { [field]: [prompt('owned-input')] }) } } }))
      }
      expect(await useAppStore.getState().editUnsentMessage('chat', 7, f.row, 'Edited unsent', retained)).toMatchObject({ ok: false })
      expect(bridge.send).not.toHaveBeenCalled()
      expect(f.row.content).toBe('Original unsent')
      expect(readChatComposerDraft('chat')).toEqual(f.draft)
    })
  it.each(['row-replaced', 'chat-reborn'] as const)('rejects %s while the body probe is pending', async change => {
    const f = fixture()
    let finish!: (value: unknown) => void
    bridge.send.mockImplementation(() => new Promise(resolve => { finish = resolve }))
    const saving = useAppStore.getState().editUnsentMessage('chat', 7, f.row, 'Edited unsent', f.retained)
    const pushed = change === 'row-replaced' ? { ...f.row } : f.row
    useAppStore.setState({ messages: { chat: [pushed, f.later] },
      sessions: [{ ...f.session, createdAt: new Date(change === 'chat-reborn' ? 1 : 0) } as never] })
    finish({ ok: true, data: f.page })
    expect(await saving).toMatchObject({ ok: false })
    expect(bridge.send).toHaveBeenCalledTimes(1)
    expect(useAppStore.getState().messages.chat[0]).toBe(pushed)
  })
  it.each(['bad-digest', 'different-body', 'foreign-chat', 'different-index'] as const)('refuses a %s probe without admitting a mutation', async failure => {
    const f = fixture()
    const data = { ...f.page,
      ...(failure === 'bad-digest' ? { messagesDigest: 'not-a-native-digest' } : {}),
      ...(failure === 'different-body' ? { messages: [{ ...f.native, content: 'Different saved input' }] } : {}),
      ...(failure === 'foreign-chat' ? { chatId: 'foreign' } : {}),
      ...(failure === 'different-index' ? { startIndex: 6 } : {}) }
    bridge.send.mockResolvedValue({ ok: true, data })
    expect(await useAppStore.getState().editUnsentMessage('chat', 7, f.row, 'Edited unsent', f.retained)).toMatchObject({ ok: false })
    expect(bridge.send).toHaveBeenCalledTimes(1)
    expect(useAppStore.getState().messages.chat[0]).toBe(f.row)
  })
  it('keeps the original row, resources and independent draft after native save rejection', async () => {
    const f = fixture()
    bridge.send.mockImplementation(async request => request.action === 'getChatMessages'
      ? { ok: true, data: f.page } : { ok: false, error: 'The original draft is unchanged.' })
    expect(await useAppStore.getState().editUnsentMessage('chat', 7, f.row, 'Edited unsent', f.retained))
      .toEqual({ ok: false, error: 'The original draft is unchanged.' })
    expect(useAppStore.getState().messages.chat).toEqual([f.row, f.later])
    expect(useAppStore.getState().sessions[0]).toBe(f.session)
    expect(readChatComposerDraft('chat')).toEqual(f.draft)
    expect(bridge.send).toHaveBeenCalledTimes(2)
  })
  it.each(['generic-ack', 'missing-skill', 'unselected-file', 'changed-created-at', 'wrong-count', 'unchanged', 'bad-digest'] as const)(
    'does not publish a %s success response as a committed row', async failure => {
      const f = fixture()
      const data = failure === 'generic-ack' ? {} : { ...f.committed,
        ...(failure === 'missing-skill' ? { messages: [{ ...f.committed.messages[0], markdownStoreFiles: [] }] } : {}),
        ...(failure === 'unselected-file' ? { messages: [{ ...f.committed.messages[0], attachments: f.native.attachments }] } : {}),
        ...(failure === 'changed-created-at' ? { messages: [{ ...f.committed.messages[0], createdAt: '2026-01-02T00:00:00.000Z' }] } : {}),
        ...(failure === 'wrong-count' ? { totalCount: 201 } : {}),
        ...(failure === 'unchanged' ? { unchanged: true } : {}),
        ...(failure === 'bad-digest' ? { messagesDigest: 'bad' } : {}) }
      bridge.send.mockImplementation(async request => ({ ok: true, data: request.action === 'getChatMessages' ? f.page : data }))
      expect(await useAppStore.getState().editUnsentMessage('chat', 7, f.row, 'Edited unsent', f.retained)).toMatchObject({ ok: false })
      expect(useAppStore.getState().messages.chat[0]).toBe(f.row)
      expect(readChatComposerDraft('chat')).toEqual(f.draft)
    })
  it('accepts the exact committed push without replacing its row or summary object', async () => {
    const f = fixture()
    let finish!: (value: unknown) => void
    bridge.send.mockImplementation(async request => request.action === 'getChatMessages'
      ? { ok: true, data: f.page } : new Promise(resolve => { finish = resolve }))
    const saving = useAppStore.getState().editUnsentMessage('chat', 7, f.row, 'Edited unsent', f.retained)
    await vi.waitFor(() => expect(bridge.send).toHaveBeenCalledTimes(2))
    const pushed = reconcileCppMessages('chat', undefined, f.committed.messages, true, 7)[0]
    const pushedRows = [pushed, f.later]
    const pushedSession = { ...f.session, messagesDigest: committedDigest }
    useAppStore.setState({ messages: { chat: pushedRows }, sessions: [pushedSession as never] })
    finish({ ok: true, data: f.committed })
    expect(await saving).toEqual({ ok: true })
    expect(useAppStore.getState().messages.chat).toBe(pushedRows)
    expect(useAppStore.getState().sessions[0]).toBe(pushedSession)
  })
  it('merges its original anchored row without replacing newer unrelated rows or summary metadata', async () => {
    const f = fixture()
    let finish!: (value: unknown) => void
    bridge.send.mockImplementation(async request => request.action === 'getChatMessages'
      ? { ok: true, data: f.page } : new Promise(resolve => { finish = resolve }))
    const saving = useAppStore.getState().editUnsentMessage('chat', 7, f.row, 'Edited unsent', f.retained)
    await vi.waitFor(() => expect(bridge.send).toHaveBeenCalledTimes(2))
    const newerOther = { ...f.later, content: 'Newer unrelated input' }
    const newerSummary = { ...f.session, messagesDigest: 'aaaaaaaaaaaaaaaa' }
    useAppStore.setState({ messages: { chat: [f.row, newerOther] }, sessions: [newerSummary as never] })
    finish({ ok: true, data: f.committed })
    expect(await saving).toEqual({ ok: true })
    expect(useAppStore.getState().messages.chat[0].content).toBe('Edited unsent')
    expect(useAppStore.getState().messages.chat[1]).toBe(newerOther)
    expect(useAppStore.getState().sessions[0]).toBe(newerSummary)
  })
  it.each(['changed-body', 'same-committed-body-newer-digest'] as const)('preserves a %s push when the prior commit reply arrives', async push => {
    const f = fixture()
    let finish!: (value: unknown) => void
    bridge.send.mockImplementation(async request => request.action === 'getChatMessages'
      ? { ok: true, data: f.page } : new Promise(resolve => { finish = resolve }))
    const saving = useAppStore.getState().editUnsentMessage('chat', 7, f.row, 'Edited unsent', f.retained)
    await vi.waitFor(() => expect(bridge.send).toHaveBeenCalledTimes(2))
    const pushedRow = push === 'changed-body' ? { ...f.row, content: 'Newer pushed input' }
      : reconcileCppMessages('chat', undefined, f.committed.messages, true, 7)[0]
    const pushedSession = { ...f.session, messagesDigest: 'aaaaaaaaaaaaaaaa' }
    const pushedRows = [pushedRow, f.later]
    useAppStore.setState({ messages: { chat: pushedRows }, sessions: [pushedSession as never] })
    finish({ ok: true, data: f.committed })
    expect(await saving).toMatchObject({ ok: false })
    expect(useAppStore.getState().messages.chat).toBe(pushedRows)
    expect(useAppStore.getState().sessions[0]).toBe(pushedSession)
    expect(readChatComposerDraft('chat')).toEqual(f.draft)
  })
})

describe('delivered branch skill selection', () => {
  const skill = '/skills/source.md'
  it('sends original native IDs and skill paths as separate branch selections', async () => {
    bridge.cef = true
    bridge.send.mockResolvedValue({ ok: true, data: { chatId: 'saved-branch' } })
    expect(await useAppStore.getState().branchFromMessage('chat', 7, 'Edited', ['native-file'], [skill])).toBe('saved-branch')
    expect(bridge.send).toHaveBeenCalledExactlyOnceWith({ action: 'branchFromMessage', payload: {
      chatId: 'chat', messageIndex: 7, content: 'Edited', retainedAttachmentIds: ['native-file'],
      retainedMarkdownStoreFiles: [skill], operationId: expect.any(String),
    } })
  })
  it.each([
    { name: 'legacy omission', native: undefined, skills: undefined },
    { name: 'empty native only', native: [], skills: undefined },
    { name: 'empty skills only', native: undefined, skills: [] },
    { name: 'both empty', native: [], skills: [] },
    { name: 'retained native and empty skills', native: ['native-file'], skills: [] },
    { name: 'empty native and retained skills', native: [], skills: [skill] },
  ])('preserves independent optional wire fields for $name', async ({ native, skills }) => {
    bridge.cef = true
    bridge.send.mockResolvedValue({ ok: true, data: { chatId: 'saved-branch' } })
    await useAppStore.getState().branchFromMessage('chat', 7, 'Edited', native, skills)
    const payload = bridge.send.mock.calls[0][0].payload
    if (native === undefined) expect(payload).not.toHaveProperty('retainedAttachmentIds')
    else expect(payload.retainedAttachmentIds).toEqual(native)
    if (skills === undefined) expect(payload).not.toHaveProperty('retainedMarkdownStoreFiles')
    else expect(payload.retainedMarkdownStoreFiles).toEqual(skills)
    expect(bridge.send).toHaveBeenCalledTimes(1)
  })
  it('retries the same immutable branch payload after caller arrays change during an uncertain response', async () => {
    bridge.cef = true
    const requests: Array<{ action: string; payload: Record<string, unknown> }> = []
    let finish!: (response: unknown) => void
    bridge.send.mockImplementation(request => {
      requests.push(request)
      return requests.length === 1 ? new Promise(resolve => { finish = resolve })
        : Promise.resolve({ ok: true, data: { chatId: 'saved-branch' } })
    })
    const native = ['native-file']
    const skills = [skill]
    const messages = useAppStore.getState().messages
    const saving = useAppStore.getState().branchFromMessage('chat', 7, 'Edited', native, skills)
    expect(requests).toHaveLength(1)
    const originalPayload = JSON.stringify(requests[0].payload)
    expect(Object.isFrozen(requests[0].payload)).toBe(true)
    expect(Object.isFrozen(requests[0].payload.retainedAttachmentIds)).toBe(true)
    expect(Object.isFrozen(requests[0].payload.retainedMarkdownStoreFiles)).toBe(true)
    native.push('later-native')
    skills.splice(0, 1, '/skills/later.md')
    finish({ ok: false, error: 'Connection lost. The action may have reached UAM.' })
    expect(await saving).toBe('saved-branch')
    expect(requests).toHaveLength(2)
    expect(requests[1].payload).toBe(requests[0].payload)
    expect(JSON.stringify(requests[1].payload)).toBe(originalPayload)
    expect(requests[1].payload).toMatchObject({ retainedAttachmentIds: ['native-file'], retainedMarkdownStoreFiles: [skill], operationId: expect.any(String) })
    expect(useAppStore.getState().messages).toBe(messages)
  })

  function preview(collision = false) {
    const files = [
      { id: collision ? skill : 'F1', name: 'first.txt', type: 'file', size: 4, path: '/tmp/first.txt' },
      { id: 'F2', name: 'second.txt', type: 'file', size: 5, path: '/tmp/second.txt' },
    ]
    const rows = reconcileCppMessages('chat', undefined, [
      { role: 'user', content: 'Original delivered input', createdAt: '2026-01-01T00:00:00.000Z',
        acpPromptNotSent: false, attachments: files, markdownStoreFiles: [skill] },
      { role: 'assistant', content: 'Delivered response', createdAt: '2026-01-01T00:00:01.000Z' },
    ], true)
    const resources = rows[0].attachments!.map(attachment => ({ ...attachment }))
    useAppStore.setState({ sessions: [{ id: 'chat', name: 'Source', providerId: 'gemini-cli', folderId: null,
      viewMode: 'chat', createdAt: new Date(0), updatedAt: new Date(0) }], activeSessionId: 'chat',
      messages: { chat: rows }, historyStartIndexBySessionId: { chat: 0 } })
    return { rows, resources }
  }
  const resourceKeys = (attachments: { id: string; type: string }[] | undefined) => attachments?.map(attachment =>
    `${attachment.type === 'markdown-store' ? 'skill' : 'native'}:${attachment.id}`)
  it.each([
    { name: 'legacy retains both groups', native: undefined, skills: undefined, expected: [`skill:${skill}`, 'native:F1', 'native:F2'] },
    { name: 'native empty retains omitted skills', native: [], skills: undefined, expected: [`skill:${skill}`] },
    { name: 'skills empty retains omitted native files', native: undefined, skills: [], expected: ['native:F1', 'native:F2'] },
    { name: 'native subset removes skills explicitly', native: ['F1'], skills: [], expected: ['native:F1'] },
    { name: 'skill subset removes native explicitly', native: [], skills: [skill], expected: [`skill:${skill}`] },
    { name: 'both empty with text', native: [], skills: [], expected: [] },
  ])('matches preview resource semantics for $name without changing the source', async ({ native, skills, expected }) => {
    const f = preview()
    const branchId = await useAppStore.getState().branchFromMessage('chat', 0, 'Edited', native, skills)
    expect(branchId).toBeTruthy()
    const state = useAppStore.getState()
    expect(resourceKeys(state.messages[branchId!][0].attachments)).toEqual(expected)
    expect(state.messages.chat).toBe(f.rows)
    expect(f.rows[0].attachments).toEqual(f.resources)
    expect(state.activeSessionId).toBe(branchId)
    expect(bridge.send).not.toHaveBeenCalled()
  })
  it.each([
    { name: 'duplicate native IDs', native: ['F1', 'F1'], skills: undefined },
    { name: 'duplicate skill identities', native: undefined, skills: [skill, skill] },
    { name: 'skill path in native selection', native: [skill], skills: undefined },
    { name: 'native ID in skill selection', native: undefined, skills: ['F1'] },
    { name: 'foreign skill identity', native: undefined, skills: ['/skills/foreign.md'] },
  ])('rejects $name in preview without creating a child', async ({ native, skills }) => {
    const f = preview()
    expect(await useAppStore.getState().branchFromMessage('chat', 0, 'Edited', native, skills)).toBeNull()
    expect(useAppStore.getState().sessions).toHaveLength(1)
    expect(useAppStore.getState().messages.chat).toBe(f.rows)
    expect(f.rows[0].attachments).toEqual(f.resources)
    expect(useAppStore.getState().activeSessionId).toBe('chat')
    expect(bridge.send).not.toHaveBeenCalled()
  })
  it('keeps colliding preview IDs distinct by their native or skill category', async () => {
    const f = preview(true)
    const both = await useAppStore.getState().branchFromMessage('chat', 0, 'Edited', [skill], [skill])
    expect(resourceKeys(useAppStore.getState().messages[both!][0].attachments)).toEqual([`skill:${skill}`, `native:${skill}`])
    const native = await useAppStore.getState().branchFromMessage('chat', 0, 'Edited', [skill], [])
    expect(resourceKeys(useAppStore.getState().messages[native!][0].attachments)).toEqual([`native:${skill}`])
    const skills = await useAppStore.getState().branchFromMessage('chat', 0, 'Edited', [], [skill])
    expect(resourceKeys(useAppStore.getState().messages[skills!][0].attachments)).toEqual([`skill:${skill}`])
    expect(useAppStore.getState().messages.chat).toBe(f.rows)
    expect(f.rows[0].attachments).toEqual(f.resources)
  })
  it.each([
    { name: 'explicit retained skill', skills: [skill], accepted: true },
    { name: 'legacy skill omission', skills: undefined, accepted: false },
    { name: 'explicitly empty context', skills: [], accepted: false },
  ])('keeps empty-text skill-only preview eligibility opt-in for $name', async ({ skills, accepted }) => {
    const f = preview()
    const branchId = await useAppStore.getState().branchFromMessage('chat', 0, '', [], skills)
    if (accepted) {
      expect(branchId).toBeTruthy()
      expect(resourceKeys(useAppStore.getState().messages[branchId!][0].attachments)).toEqual([`skill:${skill}`])
    } else {
      expect(branchId).toBeNull()
      expect(useAppStore.getState().sessions).toHaveLength(1)
    }
    expect(useAppStore.getState().messages.chat).toBe(f.rows)
    expect(f.rows[0].attachments).toEqual(f.resources)
    expect(bridge.send).not.toHaveBeenCalled()
  })
  it.each(['empty', 'skill-only'] as const)('preserves a plain branch of a legacy blank %s source when replacement content is omitted', async context => {
    const f = preview()
    const source = { ...f.rows[0], content: '', attachments: context === 'skill-only'
      ? f.rows[0].attachments!.filter(attachment => attachment.type === 'markdown-store') : [] }
    const rows = [source, f.rows[1]]
    useAppStore.setState({ messages: { chat: rows } })
    const sessions = useAppStore.getState().sessions
    const activeSessionId = useAppStore.getState().activeSessionId
    const resources = source.attachments.map(attachment => ({ ...attachment }))
    expect(source.content).toBe('')
    expect(source.attachments).toHaveLength(context === 'skill-only' ? 1 : 0)
    expect(source.attachments.every(attachment => attachment.type === 'markdown-store')).toBe(true)
    expect(sessions).toHaveLength(1)
    expect(activeSessionId).toBe('chat')
    expect(bridge.send).not.toHaveBeenCalled()
    const branchId = await useAppStore.getState().branchFromMessage('chat', 0)
    // First post-call oracle: omitted replacement content preserves legacy blank-source eligibility.
    if (context === 'empty') expect(branchId).toBeNull()
    else expect(branchId).toBeTruthy()
    const state = useAppStore.getState()
    if (context === 'empty') {
      expect(state.sessions).toBe(sessions)
      expect(state.activeSessionId).toBe(activeSessionId)
      expect(state.messages).toEqual({ chat: rows })
    } else {
      expect(state.messages[branchId!][0].content).toBe('')
      expect(state.messages[branchId!][0].attachments).toEqual(resources)
      expect(state.sessions.find(session => session.id === branchId)?.branchMessageEdited).toBe(false)
    }
    expect(state.messages.chat).toBe(rows)
    expect(state.messages.chat[0]).toBe(source)
    expect(source.attachments).toEqual(resources)
    expect(bridge.send).not.toHaveBeenCalled()
  })
})
