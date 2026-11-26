import { expect, it, vi } from 'vitest'

// A build with the opt-in Custom ACP agent turned on (#523).
vi.mock('../config/buildFeatures', async (importOriginal) => ({
  ...await importOriginal<typeof import('../config/buildFeatures')>(), CUSTOM_ACP_ENABLED: true,
}))

it('adds the Custom ACP agent last, best effort, without text workers and never as the default', async () => {
  const metadata = await import('./providerMetadata')
  const { USAGE_PROVIDERS } = await import('../services/usageHistory')
  expect(metadata.CUSTOM_ACP_PROVIDER_ID).toBe('custom-acp')
  expect(metadata.SUPPORTED_CLI_PROVIDER_IDS.at(-1)).toBe('custom-acp')
  expect(metadata.DEFAULT_PROVIDER_ID).not.toBe('custom-acp')
  expect([...USAGE_PROVIDERS].sort()).toEqual([...metadata.SUPPORTED_CLI_PROVIDER_IDS].sort())
  expect(metadata.providerIsBestEffort('custom-acp')).toBe(true)
  expect(metadata.providerIsBestEffort(metadata.DEFAULT_PROVIDER_ID)).toBe(false)
  expect(metadata.SUPPORTED_TEXT_WORKER_PROVIDER_IDS).not.toContain('custom-acp')
  expect(metadata.providerRuntimeKindLabel(undefined, 'custom-acp')).toBe('ACP')
})
