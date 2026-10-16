import { beforeEach, describe, expect, it, vi } from 'vitest'

const bridge = vi.hoisted(() => ({ cef: false, companion: false, send: vi.fn(async () => ({ ok: true })) }))
vi.mock('../ipc/cefBridge', async (importOriginal) => ({
  ...await importOriginal<typeof import('../ipc/cefBridge')>(),
  isCefContext: () => bridge.cef,
  isCompanionContext: () => bridge.companion,
  sendToCEF: bridge.send,
}))
import { useAppStore } from './useAppStore'
import { sanitizeQueuedPrompt } from './cpp/sanitizers'
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
