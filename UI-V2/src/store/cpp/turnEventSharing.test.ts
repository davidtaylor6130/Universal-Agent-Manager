import { describe, expect, it } from 'vitest'
import { acpBindingFromCppChat } from './reconcile'
import { sanitizeCppChat } from './sanitizers'

function chat(turnEvents: unknown[], waitSeconds = 0) {
  const value = sanitizeCppChat({ id: 'chat-live', title: 'Live', folderId: '', providerId: 'claude-cli', executionHostId: 'local',
    createdAt: '', updatedAt: '', acpSession: { providerId: 'claude-cli', sessionId: 'native-live', running: true, processing: true,
      lifecycleState: 'processing', turnSerial: 1, waitSeconds, turnEvents } })
  if (!value) throw new Error('Invalid test chat')
  return value
}

const settled = [
  { type: 'assistant_text', text: 'Reading.' },
  { type: 'tool_call', toolCallId: 'tool-1' },
]

describe('live turn event structural sharing', () => {
  it('reuses settled event objects while the streaming tail grows', () => {
    const first = acpBindingFromCppChat(chat([...settled, { type: 'assistant_text', text: 'Sum' }]), undefined)
    const second = acpBindingFromCppChat(chat([...settled, { type: 'assistant_text', text: 'Summary' }]), first)
    expect(second.turnEvents).not.toBe(first.turnEvents)
    expect(second.turnEvents[0]).toBe(first.turnEvents[0])
    expect(second.turnEvents[1]).toBe(first.turnEvents[1])
    expect(second.turnEvents[2]).not.toBe(first.turnEvents[2])
    expect(second.turnEvents[2]).toEqual({ type: 'assistant_text', text: 'Summary' })

    const appended = acpBindingFromCppChat(chat([...settled, { type: 'assistant_text', text: 'Summary' }, { type: 'tool_call', toolCallId: 'tool-2' }]), second)
    expect(appended.turnEvents.slice(0, 3)).toEqual(second.turnEvents)
    expect(appended.turnEvents[2]).toBe(second.turnEvents[2])
    expect(appended.turnEvents).toHaveLength(4)
  })

  it('keeps the same array when only another binding field changed, and drops removed events', () => {
    const first = acpBindingFromCppChat(chat(settled, 1), undefined)
    const second = acpBindingFromCppChat(chat(settled, 2), first)
    expect(second).not.toBe(first)
    expect(second.turnEvents).toBe(first.turnEvents)
    const shorter = acpBindingFromCppChat(chat(settled.slice(0, 1), 2), second)
    expect(shorter.turnEvents).toHaveLength(1)
    expect(shorter.turnEvents[0]).toBe(first.turnEvents[0])
  })
})
