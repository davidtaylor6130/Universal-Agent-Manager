import { describe, expect, it } from 'vitest'
import { normalizeMemoryLevel, providerChatDefaultsForNewChat, sanitizeCppChat, sanitizeCppSettings, sanitizeProviderChatDefaults } from './sanitizers'
import { sessionFromCppChat } from './reconcile'

describe('native memory policy transport', () => {
  it('preserves Read Only while keeping legacy disabled concrete policies off', () => {
    expect(normalizeMemoryLevel(' readOnly ')).toBe('readOnly')
    expect(normalizeMemoryLevel('READONLY')).toBe('readOnly')
    expect(sanitizeCppChat({ id: 'chat', memoryLevel: 'readOnly', memoryEnabled: true })).toMatchObject({ memoryLevel: 'readOnly', memoryEnabled: true })
    expect(sanitizeProviderChatDefaults({ memoryLevel: 'readOnly', memoryEnabled: true })).toMatchObject({ memoryLevel: 'readOnly', memoryEnabled: true })
    expect(normalizeMemoryLevel('readOnly', false)).toBe('off')
    expect(normalizeMemoryLevel(undefined, false)).toBe('off')
    expect(normalizeMemoryLevel('invalid')).toBe('strict')
  })

  it('preserves Default markers and native effective enabled values for chats and providers', () => {
    for (const enabled of [false, true]) {
      expect(normalizeMemoryLevel(' Default ', enabled)).toBe('default')
      expect(sanitizeCppChat({ id: 'chat', memoryLevel: 'default', memoryEnabled: enabled })).toMatchObject({ memoryLevel: 'default', memoryEnabled: enabled })
      expect(sanitizeProviderChatDefaults({ memoryLevel: 'default', memoryEnabled: enabled })).toMatchObject({ memoryLevel: 'default', memoryEnabled: enabled })
    }
  })

  it('keeps inherited Off through session reconciliation and later native enabled changes', () => {
    const off = sanitizeCppChat({ id: 'chat', providerId: 'codex-cli', memoryLevel: 'default', memoryEnabled: false })!
    const first = sessionFromCppChat(off, undefined, [])
    expect(first).toMatchObject({ memoryLevel: 'default', memoryEnabled: false })
    const on = sanitizeCppChat({ id: 'chat', providerId: 'codex-cli', memoryLevel: 'default', memoryEnabled: true })!
    expect(sessionFromCppChat(on, first, [])).toMatchObject({ memoryLevel: 'default', memoryEnabled: true })
  })

  it('keeps app defaults concrete and provider Default selections intact for new chats', () => {
    expect(sanitizeCppSettings({ memoryLevelDefault: 'default', memoryEnabledDefault: true })).toMatchObject({ memoryLevelDefault: 'strict', memoryEnabledDefault: true })
    expect(sanitizeCppSettings({ memoryLevelDefault: 'default', memoryEnabledDefault: false })).toMatchObject({ memoryLevelDefault: 'off', memoryEnabledDefault: false })
    expect(sanitizeCppSettings({ memoryLevelDefault: 'readOnly', memoryEnabledDefault: true })).toMatchObject({ memoryLevelDefault: 'readOnly', memoryEnabledDefault: true })
    const provider = sanitizeProviderChatDefaults({ memoryLevel: 'default', memoryEnabled: false })
    expect(providerChatDefaultsForNewChat({ providerChatDefaults: { 'codex-cli': provider }, memoryEnabledDefault: true, memoryLevelDefault: 'open' }, 'codex-cli')).toMatchObject({ memoryLevel: 'default', memoryEnabled: false })
  })
})
