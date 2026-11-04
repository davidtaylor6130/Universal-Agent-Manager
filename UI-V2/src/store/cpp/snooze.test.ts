import { describe, expect, it } from 'vitest'
import { sanitizeCppChat } from './sanitizers'
import { sessionFromCppChat } from './reconcile'

describe('chat snooze metadata', () => {
  it('hydrates wake times and reconciles changed values without changing transcript metadata', () => {
    const chat = sanitizeCppChat({ id: 'snooze-chat', title: 'Working', providerId: 'codex-cli', snoozedUntilMs: 1_800_000_000_000 })!
    const session = sessionFromCppChat(chat, undefined, [])
    expect(session.snoozedUntilMs).toBe(1_800_000_000_000)
    const awake = sessionFromCppChat({ ...chat, snoozedUntilMs: 0 }, session, [])
    expect(awake).not.toBe(session)
    expect(awake.snoozedUntilMs).toBe(0)
    expect(awake.messageCount).toBe(session.messageCount)
  })

  it.each([undefined, null, -1, 1.5, '1800000000000', Infinity, 8_640_000_000_000_001])('treats invalid or legacy wake value %s as awake', (value) => {
    expect(sanitizeCppChat({ id: 'snooze-chat', snoozedUntilMs: value })?.snoozedUntilMs).toBe(0)
  })
})
