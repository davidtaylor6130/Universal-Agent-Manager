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
