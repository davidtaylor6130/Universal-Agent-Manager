import { defineConfig } from 'vitest/config'
import react from '@vitejs/plugin-react'
import { rmSync } from 'node:fs'
import { join } from 'node:path'
import { buildFeatureDefines } from './src/config/featureDefines'

const buildEnv = (globalThis as typeof globalThis & { process?: { env?: Record<string, string | undefined> } }).process?.env ?? {}
const buildOutDir = buildEnv.VITE_UAM_OUT_DIR ?? 'dist'
const featureDefines = buildFeatureDefines(buildEnv)

// Vite emits imported assets and CSS even when tree-shaking drops the importing code, so files that
// belong to an excluded provider or feature load as empty modules and never reach dist/.
const EXCLUDED_FILES: Record<string, RegExp> = {
  GEMINI: /\/provider-logos\/gemini[^/]*$/, CODEX: /\/provider-logos\/codex[^/]*$/, CLAUDE: /\/provider-logos\/claude[^/]*$/,
  OPENCODE: /\/provider-logos\/opencode[^/]*$/, COPILOT: /\/provider-logos\/copilot[^/]*$/, ANTIGRAVITY: /\/provider-logos\/antigravity[^/]*$/,
  SSH: /\/ExecutionRoutingDialog\.css$/,
}
const excludedFiles = Object.entries(EXCLUDED_FILES).filter(([name]) => featureDefines[`__UAM_FEATURE_${name}__`] === 'false').map(([, pattern]) => pattern)
const EXCLUDED_MODULE_PREFIX = '\0uam-excluded:'
const excludedFeatureFiles = {
  name: 'uam-excluded-feature-files',
  enforce: 'pre' as const,
  resolveId(source: string) {
    // Side-effect-free virtual modules: an unused import (such as a stylesheet) disappears entirely.
    return excludedFiles.some((pattern) => pattern.test(source)) ? { id: EXCLUDED_MODULE_PREFIX + source.replace(/\W/g, '_'), moduleSideEffects: false } : undefined
  },
  load(id: string) {
    return id.startsWith(EXCLUDED_MODULE_PREFIX) ? 'export default ""' : undefined
  },
}

// Phone access ships its web manifest and service worker only when the build includes it.
const companionExcluded = featureDefines.__UAM_FEATURE_MOBILE_COMPANION__ === 'false'
const excludedCompanionAssets = {
  name: 'uam-excluded-companion-assets',
  transformIndexHtml: (html: string) => companionExcluded ? html.replace(/\s*<link rel="manifest"[^>]*>/, '') : html,
  closeBundle() {
    if (companionExcluded) for (const file of ['companion.webmanifest', 'companion-sw.js']) rmSync(join(buildOutDir, file), { force: true })
  },
}

export default defineConfig({
  plugins: [react(), excludedFeatureFiles, excludedCompanionAssets],
  define: featureDefines,
  base: './',   // relative paths so file:// serving works in CEF
  test: {
    environment: 'jsdom',
    globals: true,
    include: ['src/**/*.test.{ts,tsx}'],
  },
  build: {
    outDir: buildOutDir,
    emptyOutDir: true,
    assetsDir: 'assets',
    manifest: true,
  },
})
