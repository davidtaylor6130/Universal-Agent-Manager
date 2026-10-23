import { describe, expect, it } from 'vitest'
import { sanitizeCppMessage, sanitizeManagedOriginRunId } from './sanitizers'
import { buildMessageFromCpp, reconcileCppMessages } from './reconcile'
const id = '5ad39bc5-8084-4e5e-8bbf-d3974ecca5c7'
const raw = { role: 'user' as const, content: '@plan review', createdAt: '2026-10-20T09:00:00Z', managedOriginRunId: id }
describe('output-only managed origin', () => {
  it('roundtrips canonical annotation through sanitizer and lazy message reconciliation', () => {
    expect(sanitizeCppMessage(raw)?.managedOriginRunId).toBe(id)
    expect(buildMessageFromCpp('root', raw, 80)).toMatchObject({ managedOriginRunId: id, nativeMessageIndex: 80, content: raw.content, role: 'user' })
  })
  it.each([undefined, null, 1, {}, '', 'x'.repeat(10000), ' '+id, id+' ', 'urn:uuid:'+id, id.replace('-', '_')])('rejects unknown or malformed %s', value => {
    expect(sanitizeManagedOriginRunId(value)).toBeUndefined()
    expect(sanitizeCppMessage({ ...raw, managedOriginRunId: value })?.managedOriginRunId).toBeUndefined()
    expect(buildMessageFromCpp('root', { ...raw, managedOriginRunId: value as string }, 0).managedOriginRunId).toBeUndefined()
  })
  it('invalidates equality when annotation arrives, changes or is removed', () => {
    const legacy = buildMessageFromCpp('root', { ...raw, managedOriginRunId: undefined }, 0)
    const annotated = reconcileCppMessages('root', [legacy], [raw], true)
    expect(annotated[0]).not.toBe(legacy)
    expect(annotated[0].managedOriginRunId).toBe(id)
    const changed = reconcileCppMessages('root', annotated, [{ ...raw, managedOriginRunId: 'aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee' }], true)
    expect(changed[0]).not.toBe(annotated[0])
    expect(reconcileCppMessages('root', changed, [{ ...raw, managedOriginRunId: undefined }], true)[0].managedOriginRunId).toBeUndefined()
  })
})
