import { beforeEach, describe, expect, it } from 'vitest'
import { useAppStore } from './useAppStore'
import { sanitizeCppSettings } from './cpp/sanitizers'

beforeEach(() => {
  useAppStore.setState({
    providers: [{ id: 'gemini-cli', name: 'Gemini CLI', shortName: 'Gemini', color: '#fff', description: '' }],
    defaultNewChatProviderId: 'gemini-cli', providerChatDefaults: {}, mcpServers: [], hostScopedSettingsValues: {},
  })
})

describe('host scoped settings transport', () => {
  it('saves only changed scoped defaults and leaves global defaults untouched', async () => {
    const payloads: unknown[] = []
    window.cefQuery = ({ request, onSuccess }) => { payloads.push(JSON.parse(request).payload); onSuccess('{}') }
    const defaults = { modelId: 'host-model', approvalMode: 'default', commandSafetyTier: 'off', memoryEnabled: true, memoryLevel: 'strict', reasoningEffort: '', serviceTier: '', smallModelMode: false }
    await expect(useAppStore.getState().setProviderChatDefaults({ providerChatDefaults: { 'gemini-cli': defaults } }, 'lab')).resolves.toBe(true)
    expect(payloads[0]).toMatchObject({ scopeExecutionHostId: 'lab', defaults: { 'gemini-cli': { modelId: 'host-model' } } })
    expect(payloads[0]).not.toHaveProperty('defaultProviderId')
    expect(useAppStore.getState().providerChatDefaults).toEqual({})
    expect(useAppStore.getState().hostScopedSettingsValues.lab.providerChatDefaults['gemini-cli'].modelId).toBe('host-model')
  })

  it('isolates empty MCP overrides and ignores stale same-host responses', async () => {
    const completions: Array<() => void> = []
    window.cefQuery = ({ onSuccess }) => { completions.push(() => onSuccess('{}')) }
    const first = useAppStore.getState().setMcpServers([], 'lab')
    const second = useAppStore.getState().setMcpServers([], 'lab')
    completions[1]()
    await second
    const latest = useAppStore.getState().hostScopedSettingsValues
    completions[0]()
    await first
    expect(useAppStore.getState().hostScopedSettingsValues).toBe(latest)
    expect(useAppStore.getState().mcpServers).toEqual([])
    expect(useAppStore.getState().hostScopedSettingsValues.lab.mcpServers).toEqual([])
  })

  it('keeps failures outside global and scoped saved values', async () => {
    window.cefQuery = ({ onFailure }) => onFailure(500, 'private-fixture-failure')
    await expect(useAppStore.getState().setProviderChatDefaults({ defaultNewChatProviderId: 'gemini-cli' }, 'lab')).resolves.toBe(false)
    expect((await useAppStore.getState().setMcpServers([], 'lab')).ok).toBe(false)
    expect(useAppStore.getState().hostScopedSettingsValues).toEqual({})
    expect(useAppStore.getState().defaultNewChatProviderId).toBe('gemini-cli')
  })

  it('sanitizes complete host maps without interpreting nested arbitrary state', () => {
    const sanitized = sanitizeCppSettings({ hostScopedSettingsValues: { lab: { defaultNewChatProviderId: 'gemini-cli', providerChatDefaults: { 'gemini-cli': { modelId: 'host-model' } }, mcpServers: [], hostScopedSettingsValues: { recursive: {} } }, local: {}, '*': {}, bad: 4 } })
    expect(Object.keys(sanitized.hostScopedSettingsValues ?? {})).toEqual(['lab'])
    expect(sanitized.hostScopedSettingsValues?.lab.providerChatDefaults['gemini-cli'].modelId).toBe('host-model')
    expect(sanitized.hostScopedSettingsValues?.lab.mcpServers).toEqual([])
  })
  it('uses configured SSH defaults for direct creation and preserves explicit model selection', async () => {
    const payloads: Array<Record<string, unknown>> = []
    window.cefQuery = ({ request, onSuccess }) => {
      const parsed = JSON.parse(request)
      if (parsed.action === 'createSession') payloads.push(parsed.payload)
      onSuccess(parsed.action === 'createSession' ? '{"chatId":"private-created"}' : '{}')
    }
    const defaults = { modelId: 'host-model', reviewerModelId: 'host-reviewer', approvalMode: 'plan', commandSafetyTier: 'off', memoryEnabled: true, memoryLevel: 'strict' as const, reasoningEffort: 'high', serviceTier: '', smallModelMode: false }
    useAppStore.setState({
      providers: [...useAppStore.getState().providers, { id: 'codex-cli', name: 'Codex', shortName: 'Codex', color: '#fff', description: '' }],
      executionHosts: [{ id: 'lab', label: 'Lab', transport: 'ssh', sshAlias: 'private-fixture', runnerStatus: 'ready', runnerVersion: 'test', platform: 'linux', architecture: 'x64', lastSeenAt: '' }],
      hostScopedSettingsValues: { lab: { defaultNewChatProviderId: 'codex-cli', providerChatDefaults: { 'codex-cli': defaults }, mcpServers: [] } },
    })
    await expect(useAppStore.getState().addSession('Private', null, undefined, undefined, undefined, 'chat', 'lab', '/private-fixture')).resolves.toBe(true)
    expect(payloads[0]).toMatchObject({ providerId: 'codex-cli', executionHostId: 'lab', defaults: { modelId: 'host-model', reviewerModelId: 'host-reviewer', approvalMode: 'plan', reasoningEffort: 'high' } })
    await useAppStore.getState().addSession('Private explicit', null, 'codex-cli', 'explicit-model', 'low', 'chat', 'lab', '/private-fixture')
    expect(payloads[1]).toMatchObject({ defaults: { modelId: 'explicit-model', reasoningEffort: 'low' } })
  })

})
