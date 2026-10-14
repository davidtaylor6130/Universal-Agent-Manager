import type { HostScopedSettingsValues } from '../store/cpp/types'
import type { ExecutionHost } from '../types/session'

/** Settings scope id meaning "every environment" (the default). */
export const SETTINGS_ALL_ENVIRONMENTS = '*'

/**
 * Adds `scopeExecutionHostId` to a settings save payload. Unscoped saves (no id, or '*')
 * are sent unchanged, so backends that ignore the field see the same payload as before.
 */
export function withSettingsScope<T extends object>(payload: T, scopeExecutionHostId?: string): T & { scopeExecutionHostId?: string } {
  if (!scopeExecutionHostId || scopeExecutionHostId === SETTINGS_ALL_ENVIRONMENTS) return payload
  return { ...payload, scopeExecutionHostId }
}

/** Resolve saved defaults only for a still-configured explicit SSH host. */
export function defaultsForExecutionHost(state: {
  defaultNewChatProviderId: string
  providerChatDefaults: HostScopedSettingsValues['providerChatDefaults']
  hostScopedSettingsValues: Record<string, HostScopedSettingsValues>
  executionHosts: ExecutionHost[]
}, hostId: string): Pick<HostScopedSettingsValues, 'defaultNewChatProviderId' | 'providerChatDefaults'> {
  return state.executionHosts.some(host => host.id === hostId && host.transport === 'ssh')
    ? state.hostScopedSettingsValues[hostId] ?? state
    : state
}
