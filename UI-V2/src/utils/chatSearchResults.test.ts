import { describe, expect, it } from 'vitest'
import { chatSearchPageRequest, sanitizeChatSearchHits } from './chatSearchResults'

const hit = { chatId: 'chat', messageIndex: 250, messagesDigest: 'snapshot', field: 'content' as const, excerpt: 'needle' }
describe('bounded chat search provenance', () => {
  it('keeps valid contributing messages without accepting missing, duplicate or foreign provenance', () => {
    const results = sanitizeChatSearchHits([hit, hit, { ...hit, chatId: 'foreign' }, { ...hit, messageIndex: -1 }, { ...hit, messagesDigest: '' }, { ...hit, field: 'toolOutput' }, { ...hit, messageIndex: 251, field: 'thoughts', extraPayload: 'discard' }], new Set(['chat']))
    expect(results).toEqual([hit, { ...hit, messageIndex: 251, field: 'thoughts' }])
    expect(sanitizeChatSearchHits(undefined, new Set(['chat']))).toEqual([])
  })
  it('bounds rows and Unicode excerpts and requests only one verified page', () => {
    const results = sanitizeChatSearchHits(Array.from({ length: 100 }, (_, messageIndex) => ({ ...hit, messageIndex, excerpt: '🧩'.repeat(1000) })), new Set(['chat']))
    expect(results).toHaveLength(32)
    expect(Array.from(results[0].excerpt)).toHaveLength(240)
    expect(results[0].excerpt.endsWith('🧩')).toBe(true)
    expect(chatSearchPageRequest(hit)).toEqual({ chatId: 'chat', limit: 50, before: 276, deferToolCallContent: true, expectedMessagesDigest: 'snapshot', targetMessageIndex: 250 })
  })
})
