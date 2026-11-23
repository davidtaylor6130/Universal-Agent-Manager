import { describe, expect, it } from 'vitest'
import { permissionRuleError, sanitizePermissionCommandRules } from './permissionRules'

describe('permission rules', () => {
  it('drops malformed, duplicate and multi-line rules instead of repairing them', () => {
    const rules = sanitizePermissionCommandRules([
      { id: 'a', action: 'deny', tool: 'command', pattern: 'rm', enabled: false },
      { id: 'a', action: 'deny', tool: 'command', pattern: 'mv' },
      { id: 'b', action: 'approve', tool: 'command', pattern: 'rm' },
      { id: 'c', action: 'ask', tool: 'shell', pattern: 'rm' },
      { id: 'd', action: 'ask', tool: 'command', pattern: 'rm\nls' },
      { id: 'e', action: 'allow', tool: 'readOnly' },
      'not a rule',
    ])
    expect(rules).toEqual([
      { id: 'a', action: 'deny', tool: 'command', pattern: 'rm', providerId: '', workspace: '', enabled: false },
      { id: 'e', action: 'allow', tool: 'readOnly', pattern: '', providerId: '', workspace: '', enabled: true },
    ])
    expect(sanitizePermissionCommandRules({})).toEqual([])
  })

  it('mirrors the native save checks', () => {
    expect(permissionRuleError({ action: 'deny', tool: 'command', pattern: 'git ** push ** --force*' })).toBe('')
    expect(permissionRuleError({ action: 'ask', tool: 'command', pattern: 'npm test; rm' })).not.toBe('')
    expect(permissionRuleError({ action: 'ask', tool: 'command', pattern: 'npm $(rm)' })).not.toBe('')
    expect(permissionRuleError({ action: 'ask', tool: 'command', pattern: "grep 'a;b'" })).toBe('')
    expect(permissionRuleError({ action: 'allow', tool: 'any', pattern: '' })).not.toBe('')
    expect(permissionRuleError({ action: 'allow', tool: 'fetch', pattern: '' })).not.toBe('')
    expect(permissionRuleError({ action: 'allow', tool: 'command', pattern: '*' })).not.toBe('')
    expect(permissionRuleError({ action: 'allow', tool: 'command', pattern: 'npm test' })).toBe('')
    expect(permissionRuleError({ action: 'deny', tool: 'edit', pattern: 'x' })).not.toBe('')
  })
})
