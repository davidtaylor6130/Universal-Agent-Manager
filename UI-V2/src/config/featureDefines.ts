/** Build feature names in CMake's canonical order (cmake/build_features.cmake). */
export const BUILD_FEATURE_NAMES = ['COMPUTER_USE', 'SSH', 'MOBILE_COMPANION', 'GEMINI', 'CODEX', 'CLAUDE', 'OPENCODE', 'COPILOT', 'ANTIGRAVITY'] as const
export type BuildFeatureName = typeof BUILD_FEATURE_NAMES[number]

/**
 * Vite `define` constants (`__UAM_FEATURE_<NAME>__`) from CMake's normalised UAM_FEATURE_<NAME>=0|1 environment.
 * Standalone npm builds and tests leave them unset and get the default all-on build; any other value is rejected.
 */
export function buildFeatureDefines(env: Record<string, string | undefined>): Record<string, string> {
  return Object.fromEntries(BUILD_FEATURE_NAMES.map((name) => {
    const value = env[`UAM_FEATURE_${name}`]
    if (value !== undefined && value !== '0' && value !== '1') throw new Error(`UAM_FEATURE_${name} must be 0 or 1, got '${value}'.`)
    return [`__UAM_FEATURE_${name}__`, value === '0' ? 'false' : 'true']
  }))
}
