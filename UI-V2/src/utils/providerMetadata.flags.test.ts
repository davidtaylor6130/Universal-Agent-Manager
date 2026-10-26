import { expect, it, vi } from 'vitest'

// A build with Gemini and Codex excluded: their ids, aliases and metadata must not be reachable.
vi.mock('../config/buildFeatures', async (importOriginal) => ({
  ...await importOriginal<typeof import('../config/buildFeatures')>(), GEMINI_ENABLED: false, CODEX_ENABLED: false,
}))

it('builds provider registries only from the providers this build includes', async () => {
  const metadata = await import('./providerMetadata')
  const { USAGE_PROVIDERS } = await import('../services/usageHistory')
  expect(metadata.SUPPORTED_CLI_PROVIDER_IDS).toEqual(['claude-cli', 'opencode-cli', 'copilot-cli', 'antigravity-cli'])
  expect(metadata.DEFAULT_PROVIDER_ID).toBe('claude-cli')
  expect([...USAGE_PROVIDERS].sort()).toEqual([...metadata.SUPPORTED_CLI_PROVIDER_IDS].sort())
  for (const id of [metadata.GEMINI_CLI_PROVIDER_ID, metadata.CODEX_CLI_PROVIDER_ID]) expect(id).not.toMatch(/gemini|codex/)
  expect(metadata.normalizeCliProviderIdAlias('codex')).toBe('codex')
  expect(metadata.normalizeCliProviderIdAlias('gemini')).toBe('gemini')
  // Native state never carries an excluded provider's chats, runs or settings, so nothing is shown for one. A stray id
  // still resolves to no excluded name and no capabilities.
  expect(metadata.providerMetadataForId('codex-cli').name).not.toMatch(/codex|gemini/i)
  expect(metadata.isCodexProvider(undefined, 'codex-cli')).toBe(false)
  expect(metadata.isCodexProvider({ id: 'x', name: '', shortName: '', color: '', description: '', outputMode: 'cli', supportsCli: true, supportsStructured: true, structuredProtocol: 'codex-app-server' })).toBe(false)
  expect(metadata.GEMINI_DEFAULT_MODEL_LABELS).toEqual({})
  expect(metadata.CODEX_SPEED_OPTIONS).toEqual([])
})
