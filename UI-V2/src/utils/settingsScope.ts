import type { HostScopedSettingsValues } from '../store/cpp/types'
import type { ExecutionHost } from '../types/session'
import { isSshHost } from './remoteWorkspace'

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

/**
 * Resolve saved defaults for an execution host. An SSH host never inherits the controller's
 * global defaults (model ids are local to the controller), so it gets empty defaults until its
 * own scoped settings exist.
 */
export function defaultsForExecutionHost(state: {
  defaultNewChatProviderId: string
  providerChatDefaults: HostScopedSettingsValues['providerChatDefaults']
  hostScopedSettingsValues: Record<string, HostScopedSettingsValues>
  executionHosts: ExecutionHost[]
}, hostId: string): Pick<HostScopedSettingsValues, 'defaultNewChatProviderId' | 'providerChatDefaults'> {
  if (!state.executionHosts.some(host => host.id === hostId && isSshHost(host))) return state
  // ponytail: an SSH host with no scoped settings gets empty defaults, not the controller's
  return state.hostScopedSettingsValues[hostId] ?? { defaultNewChatProviderId: '', providerChatDefaults: {} }
}
