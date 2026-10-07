import { sanitizeCppChat } from '../cpp/sanitizers'
import { acpBindingFromCppChat } from '../cpp/reconcile'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { createSessionsSlice } from './sessionsSlice'
import { sendToCEF } from '../../ipc/cefBridge'
import type { AppState } from '../storeTypes'
vi.mock('../../ipc/cefBridge', async (original) => ({ ...await original<typeof import('../../ipc/cefBridge')>(), isCefContext: () => true, sendToCEF: vi.fn() }))
const expected = [{ id: 'a', revision: 2 }, { id: 'b', revision: 1 }]
function fixture() {
  const binding = { queueHeld: true, queuedPrompts: expected.map((entry) => ({ ...entry, text: entry.id, attachments: [{ id: 'attachment' }], markdownStoreFiles: ['/skill.md'] })) }
  const state = { acpBindingBySessionId: { chat: binding } } as unknown as AppState
  return { binding, actions: createSessionsSlice(vi.fn(), () => state, true) }
}
describe('restored queue admission', () => {
  beforeEach(() => vi.resetAllMocks())
  it('propagates native held-to-resumed transitions without reusing a stale binding', () => {
    const chat = sanitizeCppChat({ id: 'chat', title: 'Queue', acpSession: { sessionId: 'native', queueHeld: true } })!
    const held = acpBindingFromCppChat(chat, undefined)
    expect(held.queueHeld).toBe(true)
    const resumed = acpBindingFromCppChat({ ...chat, acpSession: { ...chat.acpSession!, queueHeld: false } }, held)
    expect(resumed).not.toBe(held)
    expect(resumed.queueHeld).toBe(false)
  })
  it('sends the exact ordered identities and retains held prompts when native persistence fails', async () => {
    const { binding, actions } = fixture()
    vi.mocked(sendToCEF).mockResolvedValue({ ok: false, error: 'Save failed' })
    await expect(actions.resumeQueuedAcpPrompts('chat', expected)).resolves.toBe(false)
    expect(sendToCEF).toHaveBeenCalledWith({ action: 'resumeQueuedAcpPrompts', payload: { chatId: 'chat', expected } })
    expect(binding.queueHeld).toBe(true)
    expect(binding.queuedPrompts[0]).toMatchObject({ attachments: [{ id: 'attachment' }], markdownStoreFiles: ['/skill.md'] })
  })
  it('rejects stale, duplicate, reordered and prepared snapshots and blocks held steering before IPC', async () => {
    const { binding, actions } = fixture()
    for (const snapshot of [[], [...expected].reverse(), [{ id: 'a', revision: 1 }, expected[1]], [expected[0], expected[0]]])
      await expect(actions.resumeQueuedAcpPrompts('chat', snapshot)).resolves.toBe(false)
    await expect(actions.steerQueuedAcpPrompt('chat', 0, expected[0])).resolves.toBe(false)
    Object.assign(binding.queuedPrompts[0], { prepared: true })
    await expect(actions.resumeQueuedAcpPrompts('chat', expected)).resolves.toBe(false)
    expect(sendToCEF).not.toHaveBeenCalled()
  })
})
