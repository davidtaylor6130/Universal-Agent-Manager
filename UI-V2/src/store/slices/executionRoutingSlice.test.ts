import * as bridge from '../../ipc/cefBridge'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { createSessionsSlice } from './sessionsSlice'
import { createExecutionRoutingSlice } from './executionRoutingSlice'
import { sendToCEF } from '../../ipc/cefBridge'
import type { AppState } from '../storeTypes'
import type { ExecutionRoutingSettings, RoutingReservation } from '../../types/executionRouting'
vi.mock('../../ipc/cefBridge', async (original) => ({ ...await original<typeof import('../../ipc/cefBridge')>(), isCefContext: () => true, sendToCEF: vi.fn() }))
const expected: RoutingReservation = { chatId: 'chat', executionHostId: 'ssh-a', connectionIdentity: 'host-v1', providerConfigurationIdentity: 'provider-v1', destinationFolderId: 'target', destinationDirectory: '/remote' }
const settings: ExecutionRoutingSettings = { enabled: true, hosts: [{ hostId: 'ssh-a', enabled: true, maxActiveTurns: 2, providers: [{ providerId: 'codex-cli', connectionIdentity: '', providerConfigurationIdentity: '', confirm: true }], workspaceMappings: [{ sourceFolderId: 'source', sourceDirectory: '/source', destinationFolderId: 'target' }] }] }
function fixture() {
  const state = { executionRouting: { enabled: false, hosts: [] }, executionHosts: [{ id: 'ssh-a', transport: 'ssh' }], folders: [{ id: 'source', directory: '/source' }, { id: 'target', directory: '/remote', executionHostId: 'ssh-a' }], sessions: [{ id: 'chat', executionHostId: 'ssh-a', folderId: 'target', workspaceDirectory: '/remote', routingUnresolvedReservation: true, routingReservationPending: true, routingAssignedConnectionIdentity: 'host-v1', routingAssignedProviderConfigurationIdentity: 'provider-v1' }] } as unknown as AppState
  return { state, actions: createExecutionRoutingSlice(() => state) }
}
describe('execution routing native actions', () => {
  beforeEach(() => vi.resetAllMocks())
  it('sends manual confirmation and leaves persisted settings unchanged on a failed save', async () => {
    const { state, actions } = fixture()
    vi.mocked(sendToCEF).mockResolvedValue({ ok: false, error: 'Disk full' })
    await expect(actions.setExecutionRoutingSettings(settings)).rejects.toThrow('Disk full')
    expect(sendToCEF).toHaveBeenCalledWith({ action: 'setExecutionRoutingSettings', payload: settings })
    expect(state.executionRouting).toEqual({ enabled: false, hosts: [] })
  })
  it('rejects stale reservations before IPC and acknowledges only the exact observed native ownership', async () => {
    const { state, actions } = fixture()
    await expect(actions.releaseExecutionRoutingReservation({ ...expected, connectionIdentity: 'stale' })).rejects.toThrow('changed')
    expect(sendToCEF).not.toHaveBeenCalled()
    vi.mocked(sendToCEF).mockResolvedValue({ ok: true })
    await actions.releaseExecutionRoutingReservation(expected)
    expect(sendToCEF).toHaveBeenCalledWith({ action: 'releaseExecutionRoutingReservation', payload: { ...expected, acknowledgeUnresolvedOwnership: true } })
    expect(state.sessions[0].routingUnresolvedReservation).toBe(true)
    state.sessions[0].workspaceWorktreeDirectory = '/remote/worktree'
    await expect(actions.releaseExecutionRoutingReservation(expected)).rejects.toThrow('changed')
    await actions.releaseExecutionRoutingReservation({ ...expected, destinationDirectory: '/remote/worktree' })
    expect(sendToCEF).toHaveBeenLastCalledWith({ action: 'releaseExecutionRoutingReservation', payload: { ...expected, destinationDirectory: '/remote/worktree', acknowledgeUnresolvedOwnership: true } })
  })
  it('submits automatic mode only for a configured source while explicit creation keeps its existing payload', async () => {
    const { state } = fixture()
    state.executionRouting = { ...settings, hosts: [{ ...settings.hosts[0], providers: [{ providerId: 'codex-cli', connectionIdentity: 'host-v1', providerConfigurationIdentity: 'provider-v1' }] }] }
    state.providers = [{ id: 'codex-cli', name: 'Codex', shortName: 'Codex', color: '', description: '' }]
    state.providerChatDefaults = { 'codex-cli': { modelId: 'local-only-model', reviewerModelId: 'local-reviewer', serviceTier: 'priority', reasoningEffort: 'high' } as AppState['providerChatDefaults'][string] }
    state.defaultNewChatProviderId = 'codex-cli'
    vi.mocked(sendToCEF).mockResolvedValue({ ok: true })
    const actions = createSessionsSlice(vi.fn(), () => state, true)
    await expect(actions.addSession('Automatic', 'source', 'codex-cli', '', '', 'chat', 'local', '/source', 'automatic')).resolves.toBe(true)
    expect(sendToCEF).toHaveBeenLastCalledWith(expect.objectContaining({ action: 'createSession', payload: expect.objectContaining({ folderId: 'source', routingMode: 'automatic' }) }))
    expect(vi.mocked(sendToCEF).mock.calls.at(-1)?.[0].payload).toEqual(expect.objectContaining({ defaults: expect.not.objectContaining({ modelId: expect.anything() }) }))
    expect((vi.mocked(sendToCEF).mock.calls.at(-1)?.[0].payload as { defaults: object }).defaults).not.toHaveProperty('reasoningEffort')
    expect((vi.mocked(sendToCEF).mock.calls.at(-1)?.[0].payload as { defaults: object }).defaults).not.toHaveProperty('reviewerModelId')
    expect((vi.mocked(sendToCEF).mock.calls.at(-1)?.[0].payload as { defaults: object }).defaults).not.toHaveProperty('serviceTier')
    await actions.addSession('Explicit', 'source', 'codex-cli')
    expect(vi.mocked(sendToCEF).mock.calls.at(-1)?.[0].payload).not.toHaveProperty('routingMode')
    state.executionRouting = { enabled: false, hosts: [] }
    const calls = vi.mocked(sendToCEF).mock.calls.length
    await expect(actions.addSession('Disabled', 'source', 'codex-cli', '', '', 'chat', 'local', '/source', 'automatic')).resolves.toBe(false)
    expect(sendToCEF).toHaveBeenCalledTimes(calls)
  })

  it('keeps automatic development sessions free of source-account overrides too', async () => {
    const { state } = fixture()
    state.executionRouting = { ...settings, hosts: [{ ...settings.hosts[0], providers: [{ providerId: 'codex-cli', connectionIdentity: 'host-v1', providerConfigurationIdentity: 'provider-v1' }] }] }
    state.providers = [{ id: 'codex-cli', name: 'Codex', shortName: 'Codex', color: '', description: '' }]
    state.providerChatDefaults = { 'codex-cli': { modelId: 'local-model', reviewerModelId: 'local-reviewer', reasoningEffort: 'high', serviceTier: 'priority', approvalMode: 'default', commandSafetyTier: 'off', memoryEnabled: true } }
    state.defaultNewChatProviderId = 'codex-cli'
    const cef = vi.spyOn(bridge, 'isCefContext').mockReturnValue(false)
    try {
      const actions = createSessionsSlice((change) => { Object.assign(state, typeof change === 'function' ? change(state) : change) }, () => state, false)
      await expect(actions.addSession('Automatic mock', 'source', 'codex-cli', undefined, undefined, 'chat', 'local', '/source', 'automatic')).resolves.toBe(true)
      expect(state.sessions.at(-1)).toMatchObject({ routingMode: 'automatic', modelId: '', reasoningEffort: '', serviceTier: '' })
      expect(state.providerChatDefaults['codex-cli'].modelId).toBe('local-model')
    } finally { cef.mockRestore() }
  })

})
