import tailwindcss from 'tailwindcss'
import autoprefixer from 'autoprefixer'

// Selectors and custom properties named after a provider or feature that the build excludes
// (UAM_FEATURE_<NAME>=0 from CMake) are dropped, so their CSS never ships.
const FEATURE_CSS_NAMES = {
  GEMINI: /gemini/i, CODEX: /codex/i, CLAUDE: /claude/i, OPENCODE: /opencode/i, COPILOT: /copilot/i, ANTIGRAVITY: /antigravity/i,
  SSH: /remote-host|\bssh\b/i, COMPUTER_USE: /computer-use/i, MOBILE_COMPANION: /companion/i,
}
const excluded = Object.entries(FEATURE_CSS_NAMES).filter(([name]) => process.env[`UAM_FEATURE_${name}`] === '0').map(([, pattern]) => pattern)
const isExcluded = (text) => excluded.some((pattern) => pattern.test(text))
const stripExcludedFeatures = {
  postcssPlugin: 'uam-strip-excluded-features',
  Rule(rule) {
    if (!excluded.length || !rule.selector || !isExcluded(rule.selector)) return
    const kept = rule.selectors.filter((selector) => !isExcluded(selector))
    if (kept.length) rule.selectors = kept
    else rule.remove()
  },
  Declaration(declaration) {
    if (excluded.length && isExcluded(declaration.prop)) declaration.remove()
  },
}

export default {
  plugins: [tailwindcss, autoprefixer, stripExcludedFeatures],
}
