import { describe, expect, it } from 'vitest'
import { BUILD_FEATURE_NAMES, buildFeatureDefines } from './featureDefines'
import * as buildFeatures from './buildFeatures'

describe('build feature defines', () => {
  it('turns CMake 0/1 values into one compile-time boolean per feature and defaults standalone builds to the release set', () => {
    expect(buildFeatureDefines({})).toEqual(Object.fromEntries(BUILD_FEATURE_NAMES.map((name) => [`__UAM_FEATURE_${name}__`, name === 'CUSTOM_ACP' ? 'false' : 'true'])))
    expect(buildFeatureDefines({ UAM_FEATURE_CUSTOM_ACP: '1' }).__UAM_FEATURE_CUSTOM_ACP__).toBe('true')
    const defines = buildFeatureDefines({ UAM_FEATURE_SSH: '0', UAM_FEATURE_CODEX: '0', UAM_FEATURE_CLAUDE: '1' })
    expect(defines.__UAM_FEATURE_SSH__).toBe('false')
    expect(defines.__UAM_FEATURE_CODEX__).toBe('false')
    expect(defines.__UAM_FEATURE_CLAUDE__).toBe('true')
  })

  it('rejects anything but the normalised values so a stray OFF cannot leave the UI on', () => {
    for (const value of ['OFF', 'false', 'ON', '']) expect(() => buildFeatureDefines({ UAM_FEATURE_SSH: value })).toThrow(/UAM_FEATURE_SSH must be 0 or 1/)
  })

  it('exposes every define through buildFeatures without string comparison', () => {
    const exported: Record<string, boolean> = {
      COMPUTER_USE: buildFeatures.COMPUTER_USE_ENABLED, SSH: buildFeatures.SSH_ENABLED, MOBILE_COMPANION: buildFeatures.MOBILE_COMPANION_ENABLED,
      GEMINI: buildFeatures.GEMINI_ENABLED, CODEX: buildFeatures.CODEX_ENABLED, CLAUDE: buildFeatures.CLAUDE_ENABLED,
      OPENCODE: buildFeatures.OPENCODE_ENABLED, COPILOT: buildFeatures.COPILOT_ENABLED, ANTIGRAVITY: buildFeatures.ANTIGRAVITY_ENABLED,
      CUSTOM_ACP: buildFeatures.CUSTOM_ACP_ENABLED,
    }
    expect(Object.keys(exported).sort()).toEqual([...BUILD_FEATURE_NAMES].sort())
    // The same environment vite.config.ts read, so a run with an opt-in feature turned on also matches.
    const defines = buildFeatureDefines((globalThis as typeof globalThis & { process?: { env?: Record<string, string | undefined> } }).process?.env ?? {})
    for (const name of BUILD_FEATURE_NAMES) expect(String(exported[name])).toBe(defines[`__UAM_FEATURE_${name}__`])
  })
})
