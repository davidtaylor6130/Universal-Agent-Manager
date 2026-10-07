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
import { queuedPromptsEquivalent } from './cpp/reconcile'

const prompt = (id: string, text = id) => ({ id, revision: 1, text, uamAgentId: 'build', markdownStoreFiles: ['/skill.md'], attachments: [], goalMode: true, goalId: 'goal-1', computerUseMode: true })

beforeEach(() => {
  bridge.cef = false
  bridge.companion = false
  bridge.send.mockReset().mockResolvedValue({ ok: true })
  useAppStore.setState({ acpBindingBySessionId: { chat: { queuedPrompts: [prompt('one'), prompt('two')] } }, cliBindingBySessionId: {}, messages: { chat: [] }, cliTranscriptBySessionId: {}, chatHistoryErrorBySessionId: {} })
})

describe('queued prompt identity', () => {
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
