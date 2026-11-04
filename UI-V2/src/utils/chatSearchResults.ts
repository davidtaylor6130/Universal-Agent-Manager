import type { ChatSearchHit } from '../types/chatSearch'

/** Only bounded, explicit message provenance can request a transcript jump. */
export function sanitizeChatSearchHits(input: unknown, chatIds: ReadonlySet<string>): ChatSearchHit[] {
  if (!Array.isArray(input)) return []
  const hits: ChatSearchHit[] = []
  const seen = new Set<string>()
  for (const candidate of input.slice(0, 32)) {
    if (!candidate || typeof candidate !== 'object') continue
    const hit = candidate as Record<string, unknown>
    if (typeof hit.chatId !== 'string' || !chatIds.has(hit.chatId) ||
        !Number.isSafeInteger(hit.messageIndex) || (hit.messageIndex as number) < 0 ||
        typeof hit.messagesDigest !== 'string' || !hit.messagesDigest || hit.messagesDigest.length > 256 ||
        (hit.field !== 'content' && hit.field !== 'thoughts' && hit.field !== 'planSummary') ||
        typeof hit.excerpt !== 'string') continue
    const key = JSON.stringify([hit.chatId, hit.messageIndex, hit.field])
    if (seen.has(key)) continue
    let excerpt = ''
    let length = 0
    for (const character of hit.excerpt) {
      if (length++ === 240) break
      excerpt += character
    }
    seen.add(key)
    hits.push({ chatId: hit.chatId, messageIndex: hit.messageIndex as number, messagesDigest: hit.messagesDigest, field: hit.field, excerpt })
  }
  return hits
}

export function chatSearchPageRequest(hit: ChatSearchHit) {
  return {
    chatId: hit.chatId,
    limit: 50,
    before: Math.min(Number.MAX_SAFE_INTEGER, hit.messageIndex + 26),
    deferToolCallContent: true,
    expectedMessagesDigest: hit.messagesDigest,
    targetMessageIndex: hit.messageIndex,
  }
}
