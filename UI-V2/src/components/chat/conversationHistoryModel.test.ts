import { describe, expect, it } from 'vitest'
import type { Message } from '../../types/message'
import { responseFlags } from './conversationHistoryModel'

const row = (role: Message['role'], content: string, extra: Partial<Message> = {}): Message =>
  ({ id: `${role}-${content}`, role, content, createdAt: new Date('2026-01-01T00:00:00Z'), providerId: 'codex-cli', ...extra } as Message)
const INPUT = { nativeInputId: '11111111-1111-4111-8111-000000000001' }

describe('responseFlags receivedByProvider', () => {
  it('is true when the same provider answers directly after the input and the response ended normally', () => {
    expect(responseFlags([row('user', 'Question', INPUT), row('assistant', 'Answer')], 0).receivedByProvider).toBe(true)
    expect(responseFlags([row('user', 'Question', INPUT), row('assistant', '', { thoughts: 'Considering' })], 0).receivedByProvider).toBe(true)
  })

  it('stays false with no answer or an empty placeholder', () => {
    expect(responseFlags([row('user', 'Question', INPUT)], 0).receivedByProvider).toBe(false)
    expect(responseFlags([row('user', 'Question', INPUT), row('assistant', '  ')], 0).receivedByProvider).toBe(false)
  })

  it('needs both provider IDs present and equal', () => {
    expect(responseFlags([row('user', 'Question', INPUT), row('assistant', 'Answer', { providerId: 'claude-cli' })], 0).receivedByProvider).toBe(false)
    expect(responseFlags([row('user', 'Question', INPUT), row('assistant', 'Answer', { providerId: undefined })], 0).receivedByProvider).toBe(false)
    expect(responseFlags([row('user', 'Question', { ...INPUT, providerId: undefined }), row('assistant', 'Answer')], 0).receivedByProvider).toBe(false)
  })

  it('does not count output of a response that was interrupted or stopped', () => {
    // Output can precede a turn that then failed (a background task, a late line); it proves nothing about this input.
    expect(responseFlags([row('user', 'Lost', INPUT), row('assistant', 'Background task output', { interrupted: true })], 0).receivedByProvider).toBe(false)
    expect(responseFlags([row('user', 'Lost', INPUT), row('assistant', 'Partial', { stopReason: 'failed' })], 0).receivedByProvider).toBe(false)
  })

  it('never lets an answer to later input confirm earlier input', () => {
    const messages = [row('user', 'Lost', INPUT), row('user', 'Sent again', { nativeInputId: '11111111-1111-4111-8111-000000000002' }), row('assistant', 'Answer')]
    expect(responseFlags(messages, 0).receivedByProvider).toBe(false)
    expect(responseFlags(messages, 1).receivedByProvider).toBe(true)
  })

  it('never applies to a row this build dispatched, whatever follows it', () => {
    // The native gates refused to confirm it (lost, cancelled, receipt never came): the marker must stay.
    const tracked = row('user', 'Dispatched and not confirmed', { ...INPUT, inputDeliveryTracked: true })
    expect(responseFlags([tracked, row('assistant', 'Output that followed')], 0).receivedByProvider).toBe(false)
  })

  it('does not treat a compaction marker as an answer', () => {
    const marker = row('assistant', '', { blocks: [{ type: 'context_compaction', text: 'Summary' }] })
    expect(responseFlags([row('user', 'Question', INPUT), marker], 0).receivedByProvider).toBe(false)
  })
})
