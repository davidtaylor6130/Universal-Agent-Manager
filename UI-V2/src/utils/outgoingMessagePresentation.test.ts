import { beforeEach, describe, expect, it, vi } from 'vitest'
import { addOutgoingAttempt, nextOutgoingKey, outgoingScope, readOutgoingAttempts, resetOutgoingPresentation, subscribeOutgoingAttempts, updateOutgoingAttempts, type OutgoingAttempt } from './outgoingMessagePresentation'

const attempt = (text: string): OutgoingAttempt => ({ key: nextOutgoingKey(), nativeIndex: 2, text, attachments: [], createdAt: new Date(0), receiptIds: new Set(), phase: 'pending' })

describe('outgoing message presentation retention', () => {
  beforeEach(() => resetOutgoingPresentation())

  it('scopes rows to the exact chat identity and keeps updates after the view is gone', () => {
    const scope = outgoingScope('chat-1', new Date(1))
    const recreated = outgoingScope('chat-1', new Date(2))
    const listener = vi.fn()
    const unsubscribe = subscribeOutgoingAttempts(listener)
    const first = attempt('Hello')
    addOutgoingAttempt(scope, first)
    unsubscribe() // the view unmounts while the request is in flight
    updateOutgoingAttempts(scope, new Map([[first.key, { phase: 'rejected' }]]))
    expect(readOutgoingAttempts(scope)).toEqual([{ ...first, phase: 'rejected' }])
    expect(readOutgoingAttempts(recreated)).toEqual([])
    expect(listener).toHaveBeenCalledTimes(1)
  })

  it('keeps distinct same-text attempts and returns a stable snapshot until changed', () => {
    const scope = outgoingScope('chat-1', new Date(1))
    const [a, b] = [attempt('Same'), attempt('Same')]
    addOutgoingAttempt(scope, a); addOutgoingAttempt(scope, b)
    const snapshot = readOutgoingAttempts(scope)
    expect(readOutgoingAttempts(scope)).toBe(snapshot)
    updateOutgoingAttempts(scope, new Map([['missing', { phase: 'accepted' }]]))
    expect(readOutgoingAttempts(scope)).toBe(snapshot)
    expect(snapshot.map((entry) => entry.key)).toEqual([a.key, b.key])
  })
})
