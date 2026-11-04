import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, expect, it, vi } from 'vitest'

// A build with Codex excluded, opened on a device where a full build saved prices for Codex and Claude.
vi.mock('../../config/buildFeatures', async (importOriginal) => ({
  ...await importOriginal<typeof import('../../config/buildFeatures')>(), CODEX_ENABLED: false,
}))

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
const values = new Map<string, string>()
afterEach(async () => { delete window.cefQuery; vi.restoreAllMocks(); values.clear(); (await import('../../hooks/useUsageHistory')).clearUsageHistoryCache() })

it("keeps an excluded provider's saved prices on the device without showing or using them", async () => {
  const { UsagePage, parseUsagePrices } = await import('./UsagePage')
  const { USAGE_PROVIDERS } = await import('../../services/usageHistory')
  vi.spyOn(window, 'localStorage', 'get').mockReturnValue({ getItem: key => values.get(key) ?? null, setItem: (key, value) => { values.set(key, value) }, removeItem: key => { values.delete(key) }, clear: () => values.clear(), length: 0, key: () => null })
  const price = (providerId: string, modelId: string) => ({ providerId, modelId, speed: 'standard', inputPerMillion: 1, cachedInputPerMillion: null, cacheWritePerMillion: null, outputPerMillion: 2 })
  const saved = [price('codex-cli', 'saved-model'), price('claude-cli', 'test-model')]
  values.set('uam-usage-prices-v1', JSON.stringify(saved))
  expect(parseUsagePrices(saved)).toEqual(saved)
  expect(parseUsagePrices([{ ...saved[0], providerId: 'Not An Id' }])).toEqual([])

  const total = { inputTokens: 10, cachedInputTokens: 0, cacheWriteTokens: 0, outputTokens: 2, reasoningTokens: 0, totalTokens: 12, reportedCostUsd: 0, estimatedCostUsd: 0,
    estimatedCacheSavingsUsd: 0, reportedCostTokens: 0, estimatedCostTokens: 0, unpricedTokens: 12, records: 1, sessions: 1 }
  const blank = { ...total, inputTokens: 0, outputTokens: 0, totalTokens: 0, unpricedTokens: 0, records: 0, sessions: 0 }
  const requests: { priceOverrides: unknown[] }[] = []
  window.cefQuery = request => {
    const { action, payload } = JSON.parse(request.request)
    if (action === 'getProviderAllowances') { request.onSuccess(JSON.stringify({ executionHostId: payload.executionHostId, refreshing: false, providers: [] })); return }
    requests.push(payload)
    request.onSuccess(JSON.stringify({ version: 2, executionHostId: payload.executionHostId, source: 'native-transcripts', startMs: payload.startMs, endMs: payload.endMs, totals: total, complete: true, listingLimited: false,
      filesChecked: 1, filesTotal: 1, bytesRead: 0, duplicates: 0, unidentifiedRecords: 0,
      providers: USAGE_PROVIDERS.map(providerId => ({ ...(providerId === 'claude-cli' ? total : blank), providerId, available: true, files: 1, errors: 0, invalidLines: 0, oversizedLines: 0, suppressedForkRecords: 0, pendingLines: 0 })),
      models: [{ ...total, providerId: 'claude-cli', modelId: 'test-model', speed: 'standard' }], days: [], hours: [], providerDays: [], providerHours: [] }))
  }
  const host = document.createElement('div'), root = createRoot(host)
  document.body.appendChild(host)
  try {
    await act(async () => root.render(<UsagePage onClose={() => {}} />))
    expect(requests.at(-1)?.priceOverrides).toEqual([saved[1]])
    expect(host.textContent).not.toMatch(/codex|saved-model/i)

    // Editing an included model's price saves the excluded provider's price unchanged.
    act(() => host.querySelector<HTMLButtonElement>('[aria-label^="Inspect"][aria-label$="test-model standard"]')!.click())
    act(() => Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find(button => button.textContent === 'Model prices')!.click())
    const form = host.querySelector<HTMLFormElement>('[aria-label="Prices for test-model"]')!
    const input = form.querySelector<HTMLInputElement>('input')!
    act(() => {
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(input, '3')
      input.dispatchEvent(new Event('input', { bubbles: true }))
    })
    await act(async () => form.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true })))
    expect(JSON.parse(values.get('uam-usage-prices-v1')!)).toEqual([saved[0], { ...saved[1], inputPerMillion: 3 }])
    expect(requests.at(-1)?.priceOverrides).toEqual([{ ...saved[1], inputPerMillion: 3 }])
  } finally { act(() => root.unmount()); host.remove() }
})
