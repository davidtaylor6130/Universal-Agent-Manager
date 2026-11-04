import { describe, expect, it } from 'vitest'
import { sanitizeCppAcpSession, sanitizeQueuedPrompt } from './sanitizers'

const attachment = { id: 'authored-image', name: 'authored.png', kind: 'image', mimeType: 'image/png',
  size: 17, path: '/private-fixture/authored.png', contentDigest: 'authored-digest' }

function receipt(text: string, attachments: unknown[] = []) {
  return { id: 'admission-original', revision: 3, existingUserMessageIndex: 7, text, attachments,
    markdownStoreFiles: ['skills/original.md'], uamAgentId: 'original-agent', goalMode: false, goalId: '' }
}

describe('authored queued and direct prompt bytes', () => {
  it.each(['queuedPrompts', 'pendingDirectPrompts', 'unsentDirectPrompts'] as const)(
    'retains an attachment-only receipt in %s without granting retry authority', (field) => {
      const projected = sanitizeCppAcpSession({ [field]: [receipt('', [attachment])],
        unsentMessageIndices: [], recoverableUnsentMessageIndices: [] })
      expect(projected?.[field]).toHaveLength(1)
      expect(projected?.[field]?.[0]).toMatchObject({ id: 'admission-original', revision: 3,
        existingUserMessageIndex: 7, text: '', markdownStoreFiles: ['skills/original.md'],
        attachments: [{ id: 'authored-image', name: 'authored.png', path: attachment.path,
          contentDigest: 'authored-digest' }] })
      expect(projected?.unsentMessageIndices).toEqual([])
      expect(projected?.recoverableUnsentMessageIndices).toEqual([])
    })

  it.each(['queuedPrompts', 'pendingDirectPrompts', 'unsentDirectPrompts'] as const)(
    'preserves text byte-for-byte through %s for exact native receipt matching', (field) => {
      const text = ' \t\u00a0Original\r\n第二行\u2003\n '
      const projected = sanitizeCppAcpSession({ [field]: [receipt(text, [attachment])] })
      expect(projected?.[field]?.[0].text).toBe(text)
      expect(projected?.[field]?.[0]).toMatchObject({ id: 'admission-original', revision: 3,
        existingUserMessageIndex: 7, uamAgentId: 'original-agent' })
    })

  it('retains non-ASCII whitespace that native ASCII trimming considers authored input', () => {
    expect(sanitizeQueuedPrompt(receipt('\u00a0\u2003'))?.text).toBe('\u00a0\u2003')
  })

  it.each(['', ' \t\r\n\f\v'])(
    'rejects blank input without valid attachments and does not infer skill-only admission (%j)', (text) => {
      expect(sanitizeQueuedPrompt(receipt(text))).toBeNull()
      expect(sanitizeQueuedPrompt(receipt(text, [null, {}, { id: 'missing-name' }]))).toBeNull()
      expect(sanitizeQueuedPrompt(receipt(text, [attachment]))?.text).toBe(text)
    })

  it.each([undefined, null, 0, false, {}, []])('rejects malformed text even when attachments exist (%j)', (text) => {
    expect(sanitizeQueuedPrompt({ ...receipt('', [attachment]), text })).toBeNull()
  })

  it('retains valid attachment context while dropping malformed attachments', () => {
    const projected = sanitizeQueuedPrompt(receipt('', [null, {}, attachment]))
    expect(projected?.attachments).toHaveLength(1)
    expect(projected?.attachments?.[0]).toMatchObject({ id: attachment.id, path: attachment.path,
      contentDigest: attachment.contentDigest })
  })

  it('preserves exact identity controls and does not promote malformed controls', () => {
    const projected = sanitizeQueuedPrompt({ ...receipt('Original'), id: ' original-id ', prepared: true,
      revision: 4, existingUserMessageIndex: 0, prioritySteer: false })
    expect(projected).toMatchObject({ id: ' original-id ', revision: 4, prepared: true,
      existingUserMessageIndex: 0, prioritySteer: false })
    const malformed = sanitizeQueuedPrompt({ ...receipt('', [attachment]), id: ' \t', revision: '4',
      prepared: 'true', existingUserMessageIndex: -1, prioritySteer: 'true' })
    expect(malformed).not.toBeNull()
    expect(malformed?.id).toBeUndefined()
    expect(malformed?.revision).toBeUndefined()
    expect(malformed?.prepared).toBeUndefined()
    expect(malformed?.existingUserMessageIndex).toBeUndefined()
    expect(malformed?.prioritySteer).toBeUndefined()
  })
})
