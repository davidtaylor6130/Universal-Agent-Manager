import { describe, expect, it } from 'vitest'
import type { Message } from '../../types/message'
import { responseFlags } from './conversationHistoryModel'

const row = (role: Message['role'], content: string, extra: Partial<Message> = {}): Message =>
  ({ id: `${role}-${content}`, role, content, createdAt: new Date('2026-01-01T00:00:00Z'), ...extra } as Message)
const INPUT = { nativeInputId: '11111111-1111-4111-8111-000000000001' }

describe('responseFlags receivedByProvider', () => {
  it('is true when the provider answers directly after the input', () => {
    const messages = [row('user', 'Question', { ...INPUT, providerId: 'codex-cli' }), row('assistant', 'Answer', { providerId: 'codex-cli' })]
    expect(responseFlags(messages, 0).receivedByProvider).toBe(true)
  })

  it('counts tool calls and thoughts as an answer', () => {
    expect(responseFlags([row('user', 'Question', INPUT), row('assistant', '', { thoughts: 'Considering' })], 0).receivedByProvider).toBe(true)
  })

  it('stays false with no answer, an empty placeholder, or another provider', () => {
    expect(responseFlags([row('user', 'Question', INPUT)], 0).receivedByProvider).toBe(false)
    expect(responseFlags([row('user', 'Question', INPUT), row('assistant', '  ')], 0).receivedByProvider).toBe(false)
    expect(responseFlags([row('user', 'Question', { ...INPUT, providerId: 'codex-cli' }), row('assistant', 'Answer', { providerId: 'claude-cli' })], 0).receivedByProvider).toBe(false)
  })

  it('never lets an answer to later input confirm earlier input', () => {
    const messages = [row('user', 'Lost', INPUT), row('user', 'Sent again', { nativeInputId: '11111111-1111-4111-8111-000000000002' }), row('assistant', 'Answer')]
    expect(responseFlags(messages, 0).receivedByProvider).toBe(false)
    expect(responseFlags(messages, 1).receivedByProvider).toBe(true)
  })

  it('does not treat a compaction marker as an answer', () => {
    const marker = row('assistant', '', { blocks: [{ type: 'context_compaction', text: 'Summary' }] })
    expect(responseFlags([row('user', 'Question', INPUT), marker], 0).receivedByProvider).toBe(false)
  })
})
