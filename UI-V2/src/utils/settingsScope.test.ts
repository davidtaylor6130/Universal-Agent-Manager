import { describe, expect, it } from 'vitest'
import { defaultsForExecutionHost, withSettingsScope } from './settingsScope'
import { useAppStore } from '../store/useAppStore'

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

it('inherits global defaults unless a configured SSH host has explicit overrides', () => {
  const initial = useAppStore.getState()
  const overrides = { defaultNewChatProviderId: 'remote-provider', providerChatDefaults: {}, mcpServers: [] }
  const state: Parameters<typeof defaultsForExecutionHost>[0] = {
    ...initial,
    executionHosts: [{ id: 'lab', label: 'Lab', transport: 'ssh', sshAlias: 'lab', runnerStatus: 'ready', runnerVersion: '1', platform: 'linux', architecture: 'x64', lastSeenAt: '' }],
    hostScopedSettingsValues: {},
  }
  expect(defaultsForExecutionHost(state, 'lab')).toBe(state)
  state.hostScopedSettingsValues = { lab: overrides, local: overrides, removed: overrides }
  expect(defaultsForExecutionHost(state, 'lab')).toBe(overrides)
  expect(defaultsForExecutionHost(state, 'local')).toBe(state)
  expect(defaultsForExecutionHost(state, 'removed')).toBe(state)
})
