import { beforeEach, describe, expect, it } from 'vitest'
import { captureContextReference, CONTEXT_REFERENCE_MAX_BYTES, sanitizeContextReferences, formatContextReferences } from './contextReferences'
import { readChatComposerDraft, writeChatComposerDraft } from './composerDraftStorage'
const source = { kind: 'message' as const, sourceChatId: 'source', sourceMessageIndex: 4, executionHostId: 'local', workspaceDirectory: '/workspace', capturedAt: '2026-10-07T12:00:00.000Z', label: 'Message 5', text: 'Quoted selection' }
const values = new Map<string, string>()
Object.defineProperty(globalThis, 'localStorage', { configurable: true, value: {
  getItem: (key: string) => values.get(key) ?? null,
  setItem: (key: string, value: string) => values.set(key, value),
  removeItem: (key: string) => values.delete(key),
} })
beforeEach(() => values.clear())
describe('bounded context snapshots', () => {
  it('bounds UTF-8 without splitting characters and retains provenance', () => {
    const reference = captureContextReference({ ...source, text: '😀'.repeat(10000) })!
    expect(new TextEncoder().encode(reference.text).length).toBeLessThanOrEqual(CONTEXT_REFERENCE_MAX_BYTES)
    expect(reference.text.endsWith('😀')).toBe(true)
    expect(reference.truncated).toBe(true)
    expect(reference).toMatchObject({ sourceChatId: 'source', sourceMessageIndex: 4, executionHostId: 'local', workspaceDirectory: '/workspace' })
  })
  it('rejects missing source identity and caps count on stored data', () => {
    expect(captureContextReference({ ...source, sourceMessageIndex: -1 })).toBeNull()
    expect(captureContextReference({ ...source, kind: 'terminal', terminalId: undefined })).toBeNull()
    expect(sanitizeContextReferences(Array.from({ length: 8 }, (_, index) => captureContextReference({ ...source, id: String(index) })))).toHaveLength(4)
  })
  it('persists references without overwriting draft text or attachments', () => {
    const reference = captureContextReference(source)!
    const attachment = { id: 'file', name: 'existing.txt', type: 'file', size: 10, path: '/existing.txt' }
    writeChatComposerDraft('references-test', { text: 'Existing draft', attachments: [attachment], references: [reference] })
    expect(readChatComposerDraft('references-test')).toEqual({ text: 'Existing draft', attachments: [attachment], references: [reference] })
    writeChatComposerDraft('references-test', { text: 'Edited legacy draft', attachments: [attachment] })
    expect(readChatComposerDraft('references-test').references).toEqual([reference])
    writeChatComposerDraft('references-test', { text: 'Edited legacy draft', attachments: [attachment], references: [] })
    expect(readChatComposerDraft('references-test').references).toBeUndefined()
    writeChatComposerDraft('references-only', { text: '', attachments: [], references: [reference] })
    expect(readChatComposerDraft('references-only').references).toEqual([reference])
  })
  it('whitelists saved fields and rejects malformed optional provenance', () => {
    const reference = captureContextReference(source)!
    const untrusted = { ...reference, extraPayload: 'x'.repeat(1024 * 1024) }
    const sanitized = sanitizeContextReferences([untrusted])[0]
    expect(sanitized).not.toHaveProperty('extraPayload')
    expect(formatContextReferences([untrusted]).length).toBeLessThan(2000)
    expect(sanitizeContextReferences([{ ...reference, nativeSessionId: { huge: 'invalid' } }])).toEqual([])
    expect(sanitizeContextReferences([{ ...reference, sourceMessageId: 123 }])).toEqual([])
  })

})
