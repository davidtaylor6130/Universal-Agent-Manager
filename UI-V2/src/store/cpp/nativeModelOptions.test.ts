import { describe, expect, it } from 'vitest'
import { acpBindingFromCppChat } from './reconcile'
import { sanitizeCppChat, sanitizeNativeModelOptions } from './sanitizers'

function projection() {
  return {
    ownership: { chatId: 'chat-owner', nativeSessionId: 'native-owner', executionHostId: 'local', lifetime: 'public-token' },
    catalogStatus: 'ready', stateStatus: 'ready',
    eligibleTiers: { present: true, value: [
      { id: 'balance', displayName: 'Balanced', description: 'Balanced routing', type: 'auto', status: { enabled: true } },
      { id: ' vendor/Latency:Fast+V2 ', displayName: 'Low latency', description: 'Native latency preference', type: 'auto', status: { enabled: true } },
      { id: 'efficiency', displayName: 'Efficiency', description: 'Efficient routing', type: 'auto', status: { enabled: false, reason: 'policy_rejected', message: 'Disabled by provider policy' } },
    ] },
    providerDefault: { present: true, value: 'balance' }, currentModel: { present: true, value: 'auto' },
    effective: { present: true, value: 'balance' }, pending: { present: true, value: null }, activating: { present: false },
    requestPending: false, available: true, disabledCode: '', disabledReason: '', lastAckStatus: 'pending', lastError: '',
  }
}

function chat(autoRouting: unknown, changes: Record<string, unknown> = {}) {
  const value = sanitizeCppChat({ id: 'chat-owner', title: 'Owner', folderId: '', providerId: 'copilot-cli', executionHostId: 'local',
    createdAt: '', updatedAt: '', acpSession: { providerId: 'copilot-cli', protocolKind: 'copilot-sdk', sessionId: 'native-owner',
      running: true, processing: false, lifecycleState: 'ready', nativeModelOptions: { autoRouting } }, ...changes })
  if (!value) throw new Error('Invalid test chat')
  return value
}

describe('native Auto routing projection transport', () => {
  it('retains exact public eligibility and pending-null while stripping private and unknown fields', () => {
    const raw = projection()
    const result = sanitizeNativeModelOptions({ privateNonce: 'do-not-copy', autoRouting: { ...raw,
      requests: { private: 'do-not-copy' }, owner: { lifetime: 'do-not-copy' }, revision: 2,
      ownership: { ...raw.ownership, privateNonce: 'do-not-copy' },
      activating: { present: false, value: 'do-not-copy' },
      eligibleTiers: { present: true, value: raw.eligibleTiers.value.map((tier) => ({ ...tier, secret: 'do-not-copy', status: { ...tier.status, private: 'do-not-copy' } })) },
    } })
    expect(result).toEqual({ autoRouting: raw })
    expect(JSON.stringify(result)).not.toContain('do-not-copy')
    expect(result?.autoRouting?.pending).toEqual({ present: true, value: null })
    expect(result?.autoRouting?.activating).toEqual({ present: false })
  })

  it('rejects malformed, duplicate, overlong, oversized and multibyte-overflow catalogs atomically', () => {
    const valid = projection()
    const first = valid.eligibleTiers.value[0]
    const catalogs = [
      [{ ...first, status: { enabled: 'true' } }],
      [first, first],
      [{ ...first, id: 'é'.repeat(257) }],
      Array.from({ length: 65 }, (_, i) => ({ ...first, id: `tier-${i}` })),
      Array.from({ length: 64 }, (_, i) => ({ ...first, id: `tier-${i}`, description: 'x'.repeat(2048) })),
    ]
    for (const value of catalogs) expect(sanitizeNativeModelOptions({ autoRouting: { ...valid, eligibleTiers: { present: true, value } } })).toBeUndefined()
    for (const pending of [{ present: true, value: 0 }, { present: true }, { present: 'true', value: null }]) {
      expect(sanitizeNativeModelOptions({ autoRouting: { ...valid, pending } })).toBeUndefined()
    }
    expect(sanitizeNativeModelOptions({ autoRouting: { ...valid, available: 'true' } })).toBeUndefined()
  })

  it('publishes projection-only changes, preserves unchanged bindings and drops missing projections', () => {
    const raw = projection()
    const initial = acpBindingFromCppChat(chat(raw), undefined)
    expect(acpBindingFromCppChat(chat(raw), initial)).toBe(initial)
    const changed = acpBindingFromCppChat(chat({ ...raw, pending: { present: false } }), initial)
    expect(changed).not.toBe(initial)
    expect(changed.nativeModelOptions?.autoRouting?.pending).toEqual({ present: false })
    for (const update of [
      { ...raw, effective: { present: true, value: 'intelligence' } },
      { ...raw, ownership: { ...raw.ownership, lifetime: 'replacement-token' } },
      { ...raw, available: false, disabledReason: 'Native restriction', lastError: 'Rejected' },
    ]) expect(acpBindingFromCppChat(chat(update), initial)).not.toBe(initial)
    expect(acpBindingFromCppChat(chat(undefined), initial).nativeModelOptions).toBeUndefined()
  })

  it('does not attach a foreign chat, native session, host, provider or read-only action target', () => {
    const raw = projection()
    for (const ownership of [
      { ...raw.ownership, chatId: 'foreign' }, { ...raw.ownership, nativeSessionId: 'foreign' }, { ...raw.ownership, executionHostId: 'foreign' },
    ]) expect(acpBindingFromCppChat(chat({ ...raw, ownership }), undefined).nativeModelOptions).toBeUndefined()
    for (const changes of [{ providerId: 'codex-cli' }, { importedReadOnly: true }, { executionHostId: 'ssh-host' }]) {
      expect(acpBindingFromCppChat(chat(raw, changes), undefined).nativeModelOptions).toBeUndefined()
    }
    const foreign = chat(raw)
    foreign.acpSession!.providerId = 'claude-cli'
    expect(acpBindingFromCppChat(foreign, undefined).nativeModelOptions).toBeUndefined()
  })
})
