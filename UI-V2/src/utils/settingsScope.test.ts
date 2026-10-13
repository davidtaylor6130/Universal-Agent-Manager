import { describe, expect, it } from 'vitest'
import { withSettingsScope } from './settingsScope'

describe('withSettingsScope', () => {
  it('leaves unscoped payloads unchanged', () => {
    const payload = { servers: [] }
    expect(withSettingsScope(payload)).toBe(payload)
    expect(withSettingsScope(payload, '*')).toBe(payload)
  })

  it('adds the scope host id when a host is selected', () => {
    expect(withSettingsScope({ servers: [] }, 'box-1')).toEqual({ servers: [], scopeExecutionHostId: 'box-1' })
  })
})
