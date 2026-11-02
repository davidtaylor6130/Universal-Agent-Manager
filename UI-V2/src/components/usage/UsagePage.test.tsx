import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { sumProviderUsage, USAGE_PROVIDERS, type UsageTotals } from '../../services/usageHistory'
import { clearUsageHistoryCache } from '../../hooks/useUsageHistory'
import { useAppStore } from '../../store/useAppStore'

import { UsagePage, niceTicks, parseUsagePrices } from './UsagePage'
import { UsageLimits } from './UsageLimits'
import { clearProviderAllowanceCache } from '../../services/providerAllowances'

vi.stubGlobal('ResizeObserver', class { observe() {} unobserve() {} disconnect() {} });

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
const values = new Map<string, string>()
const total: UsageTotals = { inputTokens: 10, cachedInputTokens: 5, cacheWriteTokens: 0, outputTokens: 2, reasoningTokens: 1, totalTokens: 17,
  reportedCostUsd: 0, estimatedCostUsd: 0, estimatedCacheSavingsUsd: 0, reportedCostTokens: 0, estimatedCostTokens: 0, unpricedTokens: 17, records: 1, sessions: 1 }
const blank = { ...total, inputTokens: 0, cachedInputTokens: 0, outputTokens: 0, reasoningTokens: 0, totalTokens: 0, unpricedTokens: 0, records: 0, sessions: 0 }
beforeEach(() => clearProviderAllowanceCache())
afterEach(() => { delete window.cefQuery; vi.restoreAllMocks(); values.clear(); clearUsageHistoryCache() })
function storage() {
  vi.spyOn(window, 'localStorage', 'get').mockReturnValue({ getItem: key => values.get(key) ?? null, setItem: (key, value) => { values.set(key, value) }, removeItem: key => { values.delete(key) }, clear: () => values.clear(), length: 0, key: () => null })
}

const fact = (host: HTMLElement, label: string) => [...host.querySelectorAll('.uam-limits-facts dt')].find(dt => dt.textContent === label)?.nextElementSibling?.textContent

describe('UsagePage', () => {
  it.each([0, 0.05])('discloses unpriced tokens beside a partial reported cost of %s', async reportedCostUsd => {
    storage()
    const mixed = { ...total, reportedCostUsd, reportedCostTokens: 10, unpricedTokens: 7 }
    window.cefQuery = request => {
      const { payload } = JSON.parse(request.request)
      request.onSuccess(JSON.stringify({ version: 2, executionHostId: payload.executionHostId, source: 'native-transcripts', startMs: payload.startMs, endMs: payload.endMs, totals: mixed, complete: true, listingLimited: false,
        filesChecked: 1, filesTotal: 1, bytesRead: 0, duplicates: 0, unidentifiedRecords: 0,
        providers: USAGE_PROVIDERS.map(providerId => ({ ...(providerId === 'codex-cli' ? mixed : blank), providerId, available: true, files: 1, errors: 0, invalidLines: 0, oversizedLines: 0, suppressedForkRecords: 0, pendingLines: 0 })),
        models: [], days: [], hours: [], providerDays: [], providerHours: [] }))
    }
    const host = document.createElement('div'), root = createRoot(host)
    try {
      await act(async () => root.render(<UsagePage onClose={() => {}} />))
      const tiles = host.querySelector('.uam-usage-tiles')!
      expect(tiles.textContent).toContain('Known cost')
      expect(tiles.textContent).toContain('Unpriced tokens7')
      expect(tiles.textContent).toContain('Provider-reported')
      expect(host.textContent).toContain('7 tokens have no known price and are excluded from costs.')
      expect(host.querySelector('[aria-label="Usage status"]')?.classList.contains('is-attention')).toBe(true)
    } finally { act(() => root.unmount()) }
  })

  it('requests All time and renders bounded cohorts across the full retained date range', async () => {
    storage()
    const requests: { windowDays: number; startMs: number }[] = []
    window.cefQuery = request => {
      const { payload } = JSON.parse(request.request)
      requests.push(payload)
      const start = payload.windowDays === 0 ? Math.floor(payload.endMs / 86400000) * 86400000 - 999 * 86400000 : Math.floor(payload.startMs / 86400000) * 86400000
      request.onSuccess(JSON.stringify({ version: 3, executionHostId: payload.executionHostId, source: 'native-transcripts', startMs: payload.startMs, endMs: payload.endMs,
        asOfMs: payload.endMs, windowDays: payload.windowDays, collecting: false, collectionError: '', totals: total, complete: true, listingLimited: false,
        ...(payload.windowDays === 0 ? { chartStartMs: start, chartBucketMs: 12 * 86400000, historyCoverage: { kind: 'recorded-and-available-history', previouslyPrunedReceiptsMayBeMissing: true } } : {}),
        filesChecked: 1, filesTotal: 1, bytesRead: 0, duplicates: 0, unidentifiedRecords: 0,
        providers: USAGE_PROVIDERS.map(providerId => ({ ...(providerId === 'codex-cli' ? total : blank), providerId, available: true, files: 1, errors: 0, invalidLines: 0, oversizedLines: 0, suppressedForkRecords: 0, pendingLines: 0 })),
        models: [], days: [{ ...total, startMs: start }], hours: [], providerDays: [{ ...total, providerId: 'codex-cli', startMs: start }], providerHours: [] }))
    }
    const host = document.createElement('div'), root = createRoot(host)
    try {
      await act(async () => root.render(<UsagePage onClose={() => {}} />))
      const button = Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find(row => row.textContent === 'All time')!
      await act(async () => button.click())
      expect(requests.at(-1)?.windowDays).toBe(0); expect(requests.at(-1)?.startMs).toBe(0)
      expect(button.getAttribute('aria-pressed')).toBe('true')
      expect(host.querySelectorAll('.uam-usage-col').length).toBeLessThanOrEqual(90)
      expect(host.textContent).toContain('12-day buckets use UTC')
      expect(host.textContent).not.toContain('1970')
      expect(host.querySelector('[role="alert"]')).toBeNull()
    } finally { act(() => root.unmount()) }
  })

  it('prefills reference prices, exposes their source, and preserves explicit manual blanks and zero', async () => {
    storage()
    const reference = { kind: 'reference', referenceModel: 'qwen3.8-27b', source: 'https://example.com/qwen', verifiedDate: '2026-10-10', cacheFallback: true, speedFallback: true, inputPerMillion: 0.5, cachedInputPerMillion: 0.1, cacheWritePerMillion: 0.625, outputPerMillion: 3 }
    window.cefQuery = request => {
      const { payload } = JSON.parse(request.request)
      request.onSuccess(JSON.stringify({ version: 2, executionHostId: payload.executionHostId, source: 'native-transcripts', startMs: payload.startMs, endMs: payload.endMs, totals: total, complete: true, listingLimited: false,
        filesChecked: 1, filesTotal: 1, bytesRead: 0, duplicates: 0, unidentifiedRecords: 0,
        providers: USAGE_PROVIDERS.map(providerId => ({ ...(providerId === 'codex-cli' ? total : blank), providerId, available: true, files: 1, errors: 0, invalidLines: 0, oversizedLines: 0, suppressedForkRecords: 0, pendingLines: 0 })),
        models: [{ ...total, providerId: 'codex-cli', modelId: 'test-model', speed: 'standard', priceReference: reference }], days: [], hours: [], providerDays: [], providerHours: [] }))
    }
    for (const manual of [false, true]) {
      if (manual) values.set('uam-usage-prices-v1', JSON.stringify([{ providerId: 'codex-cli', modelId: 'test-model', speed: 'standard', inputPerMillion: 0, cachedInputPerMillion: null, cacheWritePerMillion: null, outputPerMillion: 2 }]))
      const host = document.createElement('div'), root = createRoot(host)
      try {
        await act(async () => root.render(<UsagePage onClose={() => {}} />))
        act(() => host.querySelector<HTMLButtonElement>('[aria-label="Inspect Codex test-model standard"]')!.click())
        act(() => Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find(button => button.textContent === 'Model prices')!.click())
        expect(Array.from(host.querySelectorAll<HTMLInputElement>('input')).map(input => input.value)).toEqual(manual ? ['0', '', '', '2'] : ['0.5', '0.1', '0.625', '3'])
        expect(host.querySelector('a')?.getAttribute('href')).toBe('https://example.com/qwen')
        expect(host.textContent).toContain('Unpublished cache rates use the input rate.')
        expect(host.textContent).toContain('This speed uses the standard rate')
      } finally { act(() => root.unmount()) }
    }
  })

  it('opens all machines, shows offline coverage, and returns there after removing the selected machine', async () => {
    storage()
    const previous = useAppStore.getState(), requests: string[] = []
    useAppStore.setState({ executionHosts: [{ id: 'local', label: 'This computer' }, { id: 'peer', label: 'Windows' }] as typeof previous.executionHosts })
    window.cefQuery = request => {
      const { payload } = JSON.parse(request.request); requests.push(payload.executionHostId)
      request.onSuccess(JSON.stringify({ version: 3, executionHostId: payload.executionHostId, source: 'native-transcripts', asOfMs: payload.endMs, windowDays: payload.windowDays, collecting: false, collectionError: '',
        startMs: payload.startMs, endMs: payload.endMs, totals: total, complete: false, listingLimited: false, filesChecked: 1, filesTotal: 1, bytesRead: 0, duplicates: 0, unidentifiedRecords: 0,
        coverage: [{ executionHostId: 'peer', label: 'Windows', status: 'cached', asOfMs: 1000, error: 'Machine is offline.' }],
        providers: USAGE_PROVIDERS.map(providerId => ({ ...(providerId === 'codex-cli' ? total : blank), providerId, available: true, files: 1, errors: 0, invalidLines: 0, oversizedLines: 0, suppressedForkRecords: 0, pendingLines: 0 })),
        models: [], days: [], hours: [], providerDays: [], providerHours: [] }))
    }
    const host = document.createElement('div'), root = createRoot(host); document.body.appendChild(host)
    try {
      await act(async () => root.render(<UsagePage onClose={() => {}} />))
      expect(requests[0]).toBe('*')
      expect(host.querySelector('[aria-label="Usage host"]')).not.toBeNull()
      expect(host.querySelector('[aria-label="Usage machine coverage"]')?.textContent).toContain('Windows: Cached')
      expect(host.textContent).toContain('Machine is offline.'); expect(host.textContent).not.toContain('Loading usage history')
      act(() => host.querySelector<HTMLButtonElement>('[aria-label="Usage host"]')!.click())
      await act(async () => Array.from(document.querySelectorAll<HTMLElement>('[role="option"]')).find(option => option.textContent?.includes('Windows'))!.click())
      expect(requests.at(-1)).toBe('peer')
      await act(async () => useAppStore.setState({ executionHosts: [{ id: 'local', label: 'This computer' }] as typeof previous.executionHosts }))
      expect(requests.at(-1)).toBe('*')
    } finally { act(() => root.unmount()); host.remove(); useAppStore.setState(previous, true) }
  })

  it.each(['Cost', 'Tokens', 'Limits'])('hides the machine picker on %s when this computer is the only machine', async tab => {
    storage()
    const previous = useAppStore.getState()
    useAppStore.setState({ executionHosts: [{ id: 'local', label: 'This computer' }] as typeof previous.executionHosts })
    window.cefQuery = request => {
      const { payload } = JSON.parse(request.request)
      request.onSuccess(JSON.stringify({ version: 2, executionHostId: payload.executionHostId, source: 'native-transcripts', startMs: payload.startMs, endMs: payload.endMs, totals: total, complete: true, listingLimited: false,
        filesChecked: 1, filesTotal: 1, bytesRead: 0, duplicates: 0, unidentifiedRecords: 0,
        providers: USAGE_PROVIDERS.map(providerId => ({ ...(providerId === 'codex-cli' ? total : blank), providerId, available: true, files: 1, errors: 0, invalidLines: 0, oversizedLines: 0, suppressedForkRecords: 0, pendingLines: 0 })),
        models: [], days: [], hours: [], providerDays: [], providerHours: [] }))
    }
    const host = document.createElement('div'), root = createRoot(host); document.body.appendChild(host)
    try {
      await act(async () => root.render(<UsagePage onClose={() => {}} />))
      await act(async () => Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find(button => button.textContent === tab)!.click())
      expect(host.querySelector('[aria-label="Usage host"]')).toBeNull()
    } finally { act(() => root.unmount()); host.remove(); useAppStore.setState(previous, true) }
  })

  it('shows unknown costs, request diagnostics, token subsets and editable zero/blank rates', async () => {
    storage()
    const requests: Record<string, unknown>[] = []
    const allowanceRequests: string[] = []
    let modelCount = 1
    window.cefQuery = request => {
      const { action, payload } = JSON.parse(request.request)
      if (action === 'getProviderAllowances') {
        allowanceRequests.push(payload.executionHostId)
        request.onSuccess(JSON.stringify({ executionHostId: payload.executionHostId, refreshing: false, providers: [] }))
        return
      }
      requests.push(payload)
      request.onSuccess(JSON.stringify({ version: 2, executionHostId: payload.executionHostId, source: 'native-transcripts', startMs: payload.startMs, endMs: payload.endMs, totals: total, complete: true, listingLimited: true,
        filesChecked: 1, filesTotal: 1, bytesRead: 0, duplicates: 1, unidentifiedRecords: 0,
        providers: USAGE_PROVIDERS.map(providerId => ({ ...(providerId === 'codex-cli' ? total : blank), providerId, available: providerId === 'codex-cli', files: providerId === 'codex-cli' ? 1 : 0, errors: 0, invalidLines: providerId === 'codex-cli' ? 1 : 0, oversizedLines: 0, suppressedForkRecords: 0, pendingLines: 0 })),
        models: Array.from({ length: modelCount }, (_, index) => ({ ...total, providerId: 'codex-cli', modelId: index === 0 ? 'test-model' : `model-${index}`, speed: 'standard' })), days: [], hours: [], providerDays: [], providerHours: [] }))
    }
    const host = document.createElement('div'), root = createRoot(host)
    document.body.appendChild(host)
    try {
      await act(async () => root.render(<UsagePage onClose={() => {}} />))
      expect(host.textContent).toContain('Unknown')
      expect(host.textContent).toContain('totals are incomplete')
      expect(host.textContent).toContain('1 invalid records')
      expect(host.textContent).toContain('Reasoning(part of output)')
      const button = (text: string) => Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find(button => button.textContent === text)!
      act(() => host.querySelector<HTMLButtonElement>('[aria-label="Inspect Codex test-model standard"]')!.click())
      act(() => button('Model prices').click())
      const form = host.querySelector<HTMLFormElement>('[aria-label="Prices for test-model"]')!
      const input = form.querySelector<HTMLInputElement>('input')!
      act(() => {
        Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(input, '0')
        input.dispatchEvent(new Event('input', { bubbles: true }))
      })
      await act(async () => form.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true })))
      expect(JSON.parse(values.get('uam-usage-prices-v1')!)[0]).toMatchObject({ inputPerMillion: 0, cachedInputPerMillion: null, outputPerMillion: null })
      expect(requests.at(-1)?.priceOverrides).toMatchObject([{ inputPerMillion: 0 }])
      modelCount = 201
      await act(async () => host.querySelector<HTMLButtonElement>('[aria-label="Refresh usage"]')!.click())
      expect(host.querySelectorAll('[aria-label^="Inspect"]')).toHaveLength(100)
      act(() => button('Show more models (101 remaining)').click())
      expect(host.querySelectorAll('[aria-label^="Inspect"]')).toHaveLength(200)
      const count = requests.length
      await act(async () => button('Limits').click())
      expect(requests).toHaveLength(count)
      expect(allowanceRequests).toContain('local')
      expect(host.textContent).toContain('Account allowances unavailable.')
    } finally { act(() => root.unmount()); host.remove() }
  })
  it('filters totals, chart bands and models together without rescanning history', async () => {
    storage()
    let requests = 0
    window.cefQuery = request => {
      requests++
      const { payload } = JSON.parse(request.request)
      const startMs = Math.floor(payload.startMs / 86400000) * 86400000
      const combined = Object.fromEntries(Object.entries(total).map(([key, value]) => [key, value * 2]))
      const providers = USAGE_PROVIDERS.map(providerId => ({ ...(['codex-cli', 'claude-cli'].includes(providerId) ? total : blank), providerId, available: ['codex-cli', 'claude-cli'].includes(providerId), files: 1, errors: 0, invalidLines: 0, oversizedLines: 0, suppressedForkRecords: 0, pendingLines: 0 }))
      request.onSuccess(JSON.stringify({ version: 2, executionHostId: payload.executionHostId, source: 'native-transcripts', startMs: payload.startMs, endMs: payload.endMs, totals: combined, complete: true, listingLimited: false,
        filesChecked: 2, filesTotal: 2, bytesRead: 0, duplicates: 0, unidentifiedRecords: 0, providers,
        models: ['codex-cli', 'claude-cli'].map(providerId => ({ ...total, providerId, modelId: 'test-model', speed: 'standard' })),
        days: [{ ...combined, startMs }], hours: [], providerDays: ['codex-cli', 'claude-cli'].map(providerId => ({ ...total, startMs, providerId })), providerHours: [] }))
    }
    const host = document.createElement('div'), root = createRoot(host)
    document.body.appendChild(host)
    try {
      await act(async () => root.render(<UsagePage onClose={() => {}} />))
      act(() => Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find(button => button.textContent === 'Tokens')!.click())
      expect(host.textContent).toContain('2 sessions')
      expect(host.querySelectorAll('[aria-label^="Inspect"]')).toHaveLength(2)
      const chart = () => host.querySelector('.uam-usage-plot')!
      const columns = () => Array.from(chart().querySelectorAll<HTMLElement>('.uam-usage-col')).map(column => column.getAttribute('aria-label')).join('|')
      expect(chart().querySelectorAll('[data-seg="input"]')).toHaveLength(1)
      expect(columns()).toContain('Codex 17, Claude 17')
      const codex = host.querySelector<HTMLButtonElement>('[aria-label="Include Codex usage"]')!
      act(() => codex.click())
      expect(codex.getAttribute('aria-pressed')).toBe('false')
      expect(host.textContent).toContain('1 session in the past 7 days')
      expect(host.querySelector('[aria-label="Inspect Codex test-model standard"]')).toBeNull()
      expect(host.querySelector('[role="group"][aria-label="Codex"]')).toBeNull()
      expect(columns()).not.toContain('Codex')
      expect(columns()).toContain('Claude 17')
      // Nothing left in the range: the headline and chart give way to one line rather than a chart of zeros.
      act(() => host.querySelector<HTMLButtonElement>('[aria-label="Include Claude usage"]')!.click())
      expect(host.textContent).toContain('No usage recorded in this range.')
      expect(host.querySelector('.uam-usage-plot')).toBeNull()
      expect(requests).toBe(1)
      act(() => codex.click())
      expect(host.querySelectorAll('[aria-label^="Inspect"]')).toHaveLength(1)
      expect(columns()).toContain('Codex 17')
      const cols = chart().querySelectorAll<HTMLElement>('.uam-usage-col')
      act(() => cols[0].focus())
      act(() => { cols[0].dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowRight', bubbles: true })) })
      expect(document.activeElement).toBe(cols[1])
      act(() => { cols[1].dispatchEvent(new KeyboardEvent('keydown', { key: 'End', bubbles: true })) })
      expect(document.activeElement).toBe(cols[cols.length - 1])
    } finally { act(() => root.unmount()); host.remove() }
  })
  // Codex estimated, Claude reported, Gemini read but idle; every other provider has no history reader.
  const basisHistory = (payload: { executionHostId: string; startMs: number; endMs: number; windowDays: number }, scale = 1) => {
    const priced = { ...total, unpricedTokens: 0, records: scale, sessions: scale }
    const rows: Record<string, UsageTotals> = { 'codex-cli': { ...priced, estimatedCostTokens: 17, estimatedCostUsd: 1.5 * scale }, 'claude-cli': { ...priced, reportedCostTokens: 17, reportedCostUsd: 2 } }
    const providers = USAGE_PROVIDERS.map(providerId => ({ ...(rows[providerId] ?? blank), providerId, available: providerId in rows || providerId === 'gemini-cli', files: 1, errors: 0, invalidLines: 0, oversizedLines: 0, suppressedForkRecords: 0, pendingLines: 0 }))
    return JSON.stringify({ version: 3, executionHostId: payload.executionHostId, source: 'native-transcripts', startMs: payload.startMs, endMs: payload.endMs, asOfMs: payload.endMs, windowDays: payload.windowDays, collecting: false, collectionError: '',
      totals: sumProviderUsage(providers), complete: true, listingLimited: false, filesChecked: 2, filesTotal: 2, bytesRead: 0, duplicates: 0, unidentifiedRecords: 0, providers,
      models: Object.entries(rows).map(([providerId, row]) => ({ ...row, providerId, modelId: `${providerId}-model`, speed: 'standard' })), days: [], hours: [], providerDays: [], providerHours: [] })
  }

  it('labels each provider and model cost with its basis, folds idle providers into one line and keeps a reason row for unsupported providers', async () => {
    storage()
    window.cefQuery = request => request.onSuccess(basisHistory(JSON.parse(request.request).payload))
    const host = document.createElement('div'), root = createRoot(host)
    try {
      await act(async () => root.render(<UsagePage onClose={() => {}} />))
      const row = (name: string) => Array.from(host.querySelectorAll('.uam-usage-providers tbody tr')).find(tr => tr.querySelector('th')?.textContent === name)?.textContent
      expect(row('Codex')).toContain('$1.50Estimated')
      expect(row('Claude')).toContain('$2.00Reported')
      expect(row('Gemini')).toBeUndefined()
      expect(host.textContent).toContain('No usage in this range: Gemini')
      expect(row('OpenCode')).toContain('Usage history unavailable')
      expect(host.querySelectorAll('.uam-usage-providers tbody tr')).toHaveLength(USAGE_PROVIDERS.length - 1)
      const chip = (name: string) => host.querySelector(`[aria-label="Include ${name} usage"]`)?.textContent
      expect(chip('Gemini')).toBe('GeminiNo usage')
      expect(chip('OpenCode')).toBe('OpenCodeUnavailable')
      expect(host.querySelector('.uam-usage-chips')?.textContent).not.toContain('$0.00')
      expect(host.querySelector('[aria-label="Inspect Codex codex-cli-model standard"]')?.textContent).toContain('Estimated')
      expect(host.querySelector('[aria-label="Inspect Claude claude-cli-model standard"]')?.textContent).toContain('Reported')
      expect(host.querySelector('.uam-usage-tiles')?.textContent).toContain('Reported + estimated$3.50')
      act(() => host.querySelector<HTMLButtonElement>('[aria-label="Include Claude usage"]')!.click())
      expect(host.querySelector('.uam-usage-tiles')?.textContent).toContain('Estimated cost$1.50')
      expect(host.querySelector('.uam-usage-tiles')?.textContent).toContain('not a bill')
      expect(row('Claude')).toBeUndefined()
      expect(host.querySelector('.uam-usage-providers [data-basis="estimated"]')?.hasAttribute('data-estimate')).toBe(true)
      expect(host.querySelector('.uam-usage-providers .uam-usage-basis')?.getAttribute('tabindex')).toBe('0')
      expect(host.querySelector('.uam-usage-model-row .uam-usage-basis')?.hasAttribute('tabindex')).toBe(false)
    } finally { act(() => root.unmount()) }
  })

  it('folds every provider into the No usage line, without an empty table, when nothing was used', async () => {
    storage()
    window.cefQuery = request => {
      const reply = JSON.parse(basisHistory(JSON.parse(request.request).payload))
      reply.providers = reply.providers.map((row: { providerId: string }) => ({ ...row, ...blank, available: true }))
      Object.assign(reply, { totals: sumProviderUsage(reply.providers), models: [] })
      request.onSuccess(JSON.stringify(reply))
    }
    const host = document.createElement('div'), root = createRoot(host)
    try {
      await act(async () => root.render(<UsagePage onClose={() => {}} />))
      expect(host.querySelector('.uam-usage-providers')).toBeNull()
      expect(host.textContent).toContain(`No usage in this range: ${Array.from(host.querySelectorAll('.uam-usage-chip-name'), name => name.textContent).join(', ')}`)
      act(() => host.querySelector<HTMLButtonElement>('[aria-label="Include Gemini usage"]')!.click())
      expect(host.textContent).not.toMatch(/No usage in this range: .*Gemini/)
    } finally { act(() => root.unmount()) }
  })

  it('shows unpriced amounts as pending while history is still being read, not as having no known price', async () => {
    storage()
    let collecting = true
    window.cefQuery = request => {
      const reply = JSON.parse(basisHistory(JSON.parse(request.request).payload))
      reply.providers = reply.providers.map((row: UsageTotals) => ({ ...row, reportedCostTokens: 0, reportedCostUsd: 0, estimatedCostTokens: 0, estimatedCostUsd: 0, unpricedTokens: row.totalTokens }))
      reply.totals = sumProviderUsage(reply.providers); reply.models = []; reply.collecting = collecting
      request.onSuccess(JSON.stringify(reply))
    }
    const host = document.createElement('div'), root = createRoot(host)
    try {
      await act(async () => root.render(<UsagePage onClose={() => {}} />))
      const tiles = () => host.querySelector('.uam-usage-tiles')?.textContent
      expect(tiles()).toContain('CostPricing…Prices still loading')
      expect(tiles()).not.toContain('Unpriced tokens')
      expect(host.querySelector('[aria-label="Include Codex usage"]')?.textContent).toBe('CodexPricing…')
      expect(host.textContent).not.toContain('no prices are known')
      expect(host.textContent).not.toContain('No priced usage')
      expect(host.textContent).toContain('Estimated cache savings: Pricing…')
      expect(host.querySelector('[aria-label="Usage status"]')?.classList.contains('is-attention')).toBe(false)
      collecting = false
      await act(async () => host.querySelector<HTMLButtonElement>('[aria-label="Refresh usage"]')!.click())
      expect(tiles()).toContain('CostUnknownNo known price')
      expect(tiles()).toContain('Unpriced tokens')
      expect(host.textContent).toContain('no prices are known')
      expect(host.querySelector('[aria-label="Usage status"]')?.classList.contains('is-attention')).toBe(true)
    } finally { act(() => root.unmount()) }
  })

  it('says partly priced tokens are awaiting prices while reference prices refresh', async () => {
    storage()
    window.cefQuery = request => {
      const reply = JSON.parse(basisHistory(JSON.parse(request.request).payload))
      reply.providers = reply.providers.map((row: UsageTotals & { providerId: string }) => row.providerId === 'codex-cli' ? { ...row, estimatedCostTokens: 10, unpricedTokens: 7 } : row)
      reply.totals = sumProviderUsage(reply.providers); reply.models = []
      reply.pricing = { refreshing: true, source: 'https://example.com/catalog', status: 'cached', fetchedAtMs: 1000, error: '', aliasError: '' }
      request.onSuccess(JSON.stringify(reply))
    }
    const host = document.createElement('div'), root = createRoot(host)
    try {
      await act(async () => root.render(<UsagePage onClose={() => {}} />))
      expect(host.querySelector('.uam-usage-tiles')?.textContent).toContain('Unpriced tokens7Awaiting prices')
      expect(host.textContent).toContain('7 tokens are still awaiting prices.')
      expect(host.querySelector('.uam-usage-providers [data-basis="partial"]')?.textContent).toContain('are still awaiting prices')
      expect(host.textContent).not.toContain('no known price')
      expect(host.querySelector('[aria-label="Usage status"]')?.classList.contains('is-attention')).toBe(false)
    } finally { act(() => root.unmount()) }
  })

  it('keeps a reported partial cost solid and names a total with no known price', async () => {
    storage()
    let priced = true
    window.cefQuery = request => {
      const reply = JSON.parse(basisHistory(JSON.parse(request.request).payload))
      reply.providers = reply.providers.map((row: UsageTotals & { providerId: string }) => row.providerId === 'codex-cli' ? { ...row, estimatedCostTokens: 0, estimatedCostUsd: 0, unpricedTokens: 17 } : row.providerId === 'claude-cli' && !priced ? { ...row, reportedCostTokens: 0, reportedCostUsd: 0, unpricedTokens: 17 } : row)
      if (priced) reply.providers = reply.providers.map((row: UsageTotals & { providerId: string }) => row.providerId === 'claude-cli' ? { ...row, reportedCostTokens: 10, unpricedTokens: 7 } : row)
      reply.totals = sumProviderUsage(reply.providers); reply.models = []
      request.onSuccess(JSON.stringify(reply))
    }
    const host = document.createElement('div'), root = createRoot(host)
    try {
      await act(async () => root.render(<UsagePage onClose={() => {}} />))
      const claude = host.querySelector('.uam-usage-providers [data-basis="partial"]')!
      expect(claude.textContent).toContain('Known cost')
      expect(claude.hasAttribute('data-estimate')).toBe(false)
      priced = false
      await act(async () => host.querySelector<HTMLButtonElement>('[aria-label="Refresh usage"]')!.click())
      expect(host.querySelector('.uam-usage-tiles')?.textContent).toContain('CostUnknownNo known price')
    } finally { act(() => root.unmount()) }
  })

  it('does not call a provider idle while its history is still being read or failed to read', async () => {
    storage()
    let collecting = true
    window.cefQuery = request => {
      const reply = JSON.parse(basisHistory(JSON.parse(request.request).payload))
      reply.collecting = collecting
      if (!collecting) reply.providers = reply.providers.map((row: { providerId: string }) => row.providerId === 'gemini-cli' ? { ...row, errors: 1 } : row)
      request.onSuccess(JSON.stringify(reply))
    }
    const host = document.createElement('div'), root = createRoot(host)
    try {
      await act(async () => root.render(<UsagePage onClose={() => {}} />))
      const row = (name: string) => Array.from(host.querySelectorAll('.uam-usage-providers tbody tr')).find(tr => tr.querySelector('th')?.textContent === name)?.textContent
      expect(row('Gemini')).toContain('Still reading history')
      expect(row('Codex')).toContain('$1.50')
      expect(host.querySelector('[aria-label="Include Gemini usage"]')?.textContent).toBe('GeminiReading…')
      expect(host.querySelector('.uam-usage-tiles')?.textContent).toContain('so far')
      collecting = false
      await act(async () => host.querySelector<HTMLButtonElement>('[aria-label="Refresh usage"]')!.click())
      expect(row('Gemini')).toContain('Usage history incomplete')
      expect(host.querySelector('[aria-label="Include Gemini usage"]')?.textContent).toBe('GeminiIncomplete')
      expect(row('Gemini')).not.toContain('No usage')
      expect(host.querySelector('.uam-usage-tiles')?.textContent).not.toContain('so far')
    } finally { act(() => root.unmount()) }
  })

  it('reopens with the cached answer at once and refreshes it in the background without blocking', async () => {
    storage()
    window.cefQuery = request => request.onSuccess(basisHistory(JSON.parse(request.request).payload))
    const first = document.createElement('div'), firstRoot = createRoot(first)
    await act(async () => firstRoot.render(<UsagePage onClose={() => {}} />))
    act(() => firstRoot.unmount())
    const held: Parameters<NonNullable<typeof window.cefQuery>>[0][] = []
    window.cefQuery = request => { held.push(request) }
    const host = document.createElement('div'), root = createRoot(host)
    try {
      act(() => root.render(<UsagePage onClose={() => {}} />))
      expect(held).toHaveLength(1)
      expect(host.querySelector('.uam-usage-skeleton')).toBeNull()
      expect(host.querySelector('[data-kept]')).toBeNull()
      expect(host.querySelector('.uam-usage-tiles')?.textContent).toContain('$3.50')
      expect(host.querySelector('header .uam-usage-progress')?.textContent).toMatch(/^Updating… Showing /)
      expect(host.querySelector('header [role="status"]')?.textContent).toBe('Updating usage')
      const refresh = host.querySelector<HTMLButtonElement>('[aria-label="Refresh usage"]')!
      expect(refresh.disabled).toBe(false); expect(refresh.getAttribute('aria-busy')).toBe('true')
      await act(async () => held[0].onSuccess(basisHistory(JSON.parse(held[0].request).payload, 2)))
      expect(host.querySelector('.uam-usage-tiles')?.textContent).toContain('$5.00')
      expect(host.querySelector('header .uam-usage-progress')?.textContent).toBe('')
      expect(host.querySelector('header [role="status"]')?.textContent).toBe('Usage updated')
      act(() => host.querySelector<HTMLButtonElement>('[aria-label="Refresh usage"]')!.click())
      await act(async () => held.at(-1)!.onFailure(500, 'Machine is offline.'))
      expect(host.querySelector('[role="alert"]')?.textContent).toMatch(/Machine is offline\. Showing figures from /)
      expect(host.querySelector('header [role="status"]')?.textContent).toBe('')
      expect(host.querySelector('.uam-usage-tiles')?.textContent).toContain('$5.00')
      // A period never opened before keeps the previous answer in place (dimmed only after a noticeable wait).
      act(() => Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find(button => button.textContent === '30 days')!.click())
      expect(host.querySelector('[data-kept]')?.textContent).toContain('$5.00')
      expect(host.querySelector('.uam-usage-skeleton')).toBeNull()
    } finally { act(() => root.unmount()) }
  })

  it('rejects malformed saved prices without treating missing fields as zero', () => {
    const price = { providerId: 'codex-cli', modelId: 'test-model', speed: 'standard', inputPerMillion: 0, cachedInputPerMillion: null, cacheWritePerMillion: null, outputPerMillion: 1 }
    expect(parseUsagePrices([price])).toEqual([price])
    for (const value of [{}, [price, price], [{ ...price, inputPerMillion: -1 }], [{ ...price, outputPerMillion: Infinity }], [{ ...price, cachedInputPerMillion: undefined }], [{ ...price, modelId: '' }], [{ ...price, providerId: 'unknown' }]]) expect(parseUsagePrices(value)).toEqual([])
  })
  it('keeps native host errors visible and offers a retry instead of displaying zero usage', async () => {
    storage()
    window.cefQuery = request => request.onFailure(400, 'Usage history is not available on remote hosts yet.')
    const host = document.createElement('div'), root = createRoot(host)
    try {
      await act(async () => root.render(<UsagePage onClose={() => {}} />))
      expect(host.querySelector('[role="alert"]')?.textContent).toContain('remote hosts')
      expect(host.textContent).toContain('Retry')
      expect(host.textContent).not.toContain('$0.00')
    } finally { act(() => root.unmount()) }
  })
})

// Mockup states for the Tokens tab. Codex is estimated across two service tiers, Claude has a partly unpriced reported cost,
// OpenCode has no known price, Gemini is the state under test, and Copilot and Antigravity were read with nothing in range.
const DAY = 86400000
const mix = (input: number, cache: number, write: number, output: number, reasoning: number, extra: Partial<UsageTotals> = {}): UsageTotals =>
  ({ ...blank, inputTokens: input, cachedInputTokens: cache, cacheWriteTokens: write, outputTokens: output, reasoningTokens: reasoning, totalTokens: input + cache + write + output, unpricedTokens: input + cache + write + output, ...extra })
type GeminiState = 'used' | 'reading' | 'unavailable' | 'incomplete'
function tokensHistory(payload: { executionHostId: string; startMs: number; endMs: number; windowDays: number }, { gemini = 'used' as GeminiState, collecting = false, empty = false } = {}) {
  const day0 = Math.floor(payload.startMs / DAY) * DAY
  const rows: Record<string, UsageTotals> = empty ? {} : {
    'codex-cli': mix(400, 500, 0, 100, 10, { estimatedCostTokens: 1000, unpricedTokens: 0, estimatedCostUsd: 1.5, records: 5, sessions: 3 }),
    'claude-cli': mix(200, 300, 20, 80, 0, { reportedCostTokens: 400, reportedCostUsd: 2, unpricedTokens: 200, records: 4, sessions: 2 }),
    'opencode-cli': mix(50, 0, 0, 50, 0, { unpricedTokens: 100, records: 1, sessions: 1 }),
    ...(gemini === 'used' ? { 'gemini-cli': mix(30, 0, 0, 20, 0, { estimatedCostTokens: 50, unpricedTokens: 0, estimatedCostUsd: 0.1, records: 1, sessions: 1 }) } : {}),
  }
  const providers = USAGE_PROVIDERS.map(providerId => ({ ...(rows[providerId] ?? blank), providerId, available: !(providerId === 'gemini-cli' && gemini === 'unavailable'), files: 3,
    errors: providerId === 'gemini-cli' && gemini === 'incomplete' ? 2 : 0, invalidLines: 0, oversizedLines: 0, pendingLines: providerId === 'gemini-cli' && gemini === 'incomplete' ? 5 : 0,
    suppressedForkRecords: providerId === 'codex-cli' && !empty ? 2 : 0 }))
  const day = (index: number, providerId: string, totals: UsageTotals) => ({ ...totals, startMs: day0 + index * DAY, providerId })
  const providerDays = empty ? [] : [
    day(2, 'codex-cli', mix(240, 300, 0, 60, 6)), day(6, 'codex-cli', mix(160, 200, 0, 40, 4)),
    day(2, 'claude-cli', rows['claude-cli']), day(5, 'opencode-cli', rows['opencode-cli']), ...(gemini === 'used' ? [day(4, 'gemini-cli', rows['gemini-cli'])] : []),
  ]
  const models = empty ? [] : [
    { ...mix(320, 400, 0, 80, 8), providerId: 'codex-cli', modelId: 'gpt-5-codex', speed: 'standard' },
    { ...mix(80, 100, 0, 20, 2), providerId: 'codex-cli', modelId: 'gpt-5-codex', speed: 'fast' },
    { ...rows['claude-cli'], providerId: 'claude-cli', modelId: 'claude-sonnet-4.5', speed: 'standard' },
    { ...rows['opencode-cli'], providerId: 'opencode-cli', modelId: 'kimi-k2', speed: 'standard' },
  ]
  return JSON.stringify({ version: 3, executionHostId: payload.executionHostId, source: 'native-transcripts', startMs: payload.startMs, endMs: payload.endMs, asOfMs: payload.endMs, windowDays: payload.windowDays,
    collecting, collectionError: '', totals: sumProviderUsage(providers), complete: false, listingLimited: false, filesChecked: collecting ? 412 : 1030, filesTotal: 1030, bytesRead: 0, duplicates: empty ? 0 : 14, unidentifiedRecords: 0,
    coverage: [{ executionHostId: 'local', label: 'This Mac', status: 'ready', asOfMs: payload.endMs, error: '' }, { executionHostId: 'build', label: 'build-box', status: 'cached', asOfMs: 1000, error: 'build-box (SSH) was unreachable.' }],
    providers, models, days: [...new Set(providerDays.map(row => row.startMs))].map(startMs => ({ ...sumProviderUsage(providerDays.filter(row => row.startMs === startMs)), startMs })), hours: [], providerDays, providerHours: [] })
}

describe('UsagePage Tokens', () => {
  async function openTokens(options: Parameters<typeof tokensHistory>[1] = {}) {
    storage()
    window.cefQuery = request => request.onSuccess(tokensHistory(JSON.parse(request.request).payload, options))
    const host = document.createElement('div'), root = createRoot(host)
    document.body.appendChild(host)
    await act(async () => root.render(<UsagePage onClose={() => {}} />))
    act(() => Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find(button => button.textContent === 'Tokens')!.click())
    return { host, done: () => { act(() => root.unmount()); host.remove() } }
  }
  const card = (host: HTMLElement, name: string) => host.querySelector<HTMLElement>(`[role="group"][aria-label="${name}"]`)

  it('picks nice axis ticks with headroom over the peak', () => {
    expect(niceTicks(2_300_000)).toEqual([0, 1_000_000, 2_000_000, 3_000_000])
    expect(niceTicks(17)).toEqual([0, 10, 20, 30])
    expect(niceTicks(1200)).toEqual([0, 500, 1000, 1500])
    expect(niceTicks(0)).toEqual([0])
  })

  it('shows the headline, the chart with Today so far, provider cards with models and the Daily table', async () => {
    const { host, done } = await openTokens()
    try {
      const head = host.querySelector('.uam-tokens-head')!.textContent
      expect(head).toContain('Total tokens1.8K')
      expect(head).toContain('7 sessions in the past 7 days')
      expect(head).toContain('Busiest day1.2K')
      expect(head).toContain('Reasoning(part of output)')
      expect(host.querySelector('.uam-usage-axis')?.textContent).toBe('1.5K1K5000')
      const labels = host.querySelector('.uam-tokens-xaxis')!.textContent
      expect(labels).toMatch(/Todayso far$/)
      expect(host.querySelectorAll('.uam-tokens-plot .uam-usage-col')).toHaveLength(7)
      expect(host.querySelector('.uam-tokens-plot .uam-usage-col:last-child')?.getAttribute('aria-label')).toMatch(/So far$/)
      // Cost stays on every provider with usage, with its basis; models and service tiers keep their own rows.
      expect(card(host, 'Codex')?.textContent).toContain('57% of tokens')
      expect(card(host, 'Codex')?.textContent).toContain('$1.50Estimated')
      expect(card(host, 'Codex')?.querySelectorAll('[aria-label^="Inspect Codex gpt-5-codex"]')).toHaveLength(2)
      expect(card(host, 'Codex')?.querySelector('.uam-tokens-tier')?.textContent).toBe('fast')
      expect(card(host, 'Claude')?.textContent).toContain('Known cost')
      expect(card(host, 'Claude')?.textContent).toContain('200 tokens have no known price and are excluded from its cost')
      expect(card(host, 'OpenCode')?.textContent).toContain('Unknown')
      expect(host.querySelector('.uam-limits-cards')?.textContent).not.toContain('$0.00')
      expect(host.textContent).toContain('No usage in this range: Copilot, Antigravity')
      expect(card(host, 'Copilot')).toBeNull()
      // Old Models/Time switch: models live in the cards, the Time view is the Daily table.
      expect(host.querySelector('[aria-label="Usage breakdown"]')).toBeNull()
      const table = Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find(button => button.textContent === 'Daily table')!
      act(() => table.click())
      expect(table.getAttribute('aria-expanded')).toBe('true')
      const rows = host.querySelectorAll('.uam-tokens-days tbody tr')
      expect(rows).toHaveLength(7)
      expect(rows[0].textContent).toMatch(/so far/)
      expect(rows[4].querySelector('[role="img"]')?.getAttribute('aria-label')).toContain('Input 440')
      // An inspected model keeps its legend, records and Model prices.
      act(() => host.querySelector<HTMLButtonElement>('[aria-label="Inspect Claude claude-sonnet-4.5 standard"]')!.click())
      expect(card(host, 'Claude')?.textContent).toContain('4 usage records')
      expect(Array.from(card(host, 'Claude')!.querySelectorAll('button')).some(button => button.textContent === 'Model prices')).toBe(true)
      // Refresh sits in the History row on Tokens; the header keeps it for Cost.
      expect(host.querySelector('header [aria-label="Refresh usage"]')).toBeNull()
      expect(host.querySelector('.uam-tokens-group')?.textContent).toContain('Refresh usage')
    } finally { done() }
  })

  it('keeps About these figures behind the info control, with a single summary line on the page', async () => {
    const { host, done } = await openTokens()
    try {
      expect(host.querySelector('.uam-tokens-group .uam-usage-reason')?.textContent).toBe('Partial · build-box shows cached figures')
      expect(host.querySelectorAll('.uam-tokens-group .uam-usage-reason')).toHaveLength(1)
      expect(host.textContent).not.toContain('About these figures')
      expect(host.textContent).not.toContain('History diagnostics')
      const info = host.querySelector<HTMLButtonElement>('[aria-label="Usage status"]')!
      expect(info.classList.contains('is-attention')).toBe(true)
      act(() => info.click())
      const panel = document.querySelector<HTMLElement>('[role="dialog"][aria-label="About these figures"]')!
      expect(info.getAttribute('aria-expanded')).toBe('true')
      expect(panel.textContent).toContain('build-box: Cached')
      expect(panel.textContent).toContain('build-box (SSH) was unreachable.')
      expect(panel.textContent).toContain('they are not a bill')
      expect(panel.textContent).toContain('History diagnostics: 14 duplicate records, 0 unidentified records and 2 forked copies were skipped.')
      act(() => { panel.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })) })
      expect(document.querySelector('[aria-label="About these figures"]')).toBeNull()
      expect(document.activeElement).toBe(info)
    } finally { done() }
  })

  it.each([
    ['reading', 'Still reading history', 'Figures appear as its history is read.'],
    ['unavailable', 'Usage history unavailable', 'No figures. This is missing history, not zero use.'],
    ['incomplete', 'Usage history incomplete', '2 read errors, 5 unfinished lines.'],
  ] as const)('keeps a greyed %s card that says why and never shows zero', async (gemini, reason, note) => {
    const { host, done } = await openTokens({ gemini, collecting: gemini === 'reading' })
    try {
      const gem = card(host, 'Gemini')!
      expect(gem.getAttribute('data-state')).toBe(gemini)
      expect(gem.textContent).toBe(`Gemini${reason}${note}`)
      expect(host.textContent).not.toMatch(/No usage in this range: .*Gemini/)
      if (gemini === 'reading') {
        expect(host.querySelector('.uam-tokens-group')?.classList.contains('uam-progress-fill')).toBe(true)
        expect(host.querySelector('.uam-tokens-group')?.textContent).toContain('Reading history · 412 of 1,030 files')
        expect(host.querySelector('.uam-tokens-head')?.textContent).toContain('so far')
        expect(card(host, 'Codex')?.textContent).toContain('1K tokens so far')
        expect(card(host, 'OpenCode')?.textContent).toContain('Pricing…')
      }
    } finally { done() }
  })

  it('filters with an excluded chip, then says when the range is empty or every provider is filtered out', async () => {
    const { host, done } = await openTokens()
    try {
      const codex = host.querySelector<HTMLButtonElement>('[aria-label="Include Codex usage"]')!
      act(() => codex.click())
      expect(codex.getAttribute('aria-pressed')).toBe('false')
      expect(codex.classList.contains('is-off')).toBe(true)
      expect(codex.querySelector('.lucide-x')).not.toBeNull()
      expect(card(host, 'Codex')).toBeNull()
      expect(host.querySelector('.uam-tokens-head')?.textContent).toContain('Total tokens750')
      expect(Array.from(host.querySelectorAll('.uam-tokens-plot .uam-usage-col'), column => column.getAttribute('aria-label')).join('|')).not.toContain('Codex')
      for (const chip of host.querySelectorAll<HTMLButtonElement>('.uam-usage-chip[aria-pressed="true"]')) act(() => chip.click())
      expect(host.textContent).toContain('All providers are filtered out.')
      expect(host.textContent).not.toContain('No usage recorded in this range.')
      expect(host.querySelector('.uam-tokens-head')).toBeNull()
    } finally { done() }
    const empty = await openTokens({ empty: true })
    try {
      expect(empty.host.textContent).toContain('No usage recorded in this range.')
      expect(empty.host.querySelector('.uam-tokens-head')).toBeNull()
      expect(empty.host.querySelector('.uam-limits-cards')).toBeNull()
      expect(empty.host.textContent).toMatch(/No usage in this range: Codex, Claude, Gemini, OpenCode, Copilot, Antigravity/)
    } finally { empty.done() }
  })
})

describe('UsageLimits', () => {
  it.each(['gemini-cli', 'opencode-cli'])('labels %s attachment totals separately from complete native-session totals', providerId => {
    const previous = useAppStore.getState()
    const counts = { inputTokens: 100, cachedInputTokens: 70, cacheWriteInputTokens: 0, outputTokens: 25, reasoningOutputTokens: 5, totalTokens: 125 }
    useAppStore.setState({
      sessions: [{ id: 'gemini', name: 'Resumed chat', providerId }] as typeof previous.sessions,
      acpBindingBySessionId: { gemini: { providerId, providerUsage: { tokenUsage: { updatedAt: Date.now(), total: counts, last: counts, modelContextWindow: null }, rateLimits: null } } } as typeof previous.acpBindingBySessionId,
    })
    const host = document.createElement('div'), root = createRoot(host)
    try {
      act(() => root.render(<UsageLimits executionHostId="local" />))
      expect(host.textContent).toContain('Tokens observed while attached to Resumed chat: 125')
      expect(fact(host, 'Input')).toBe('100')
      expect(fact(host, 'Output')).toBe('25')
      expect(fact(host, 'Cache read')).toBe('70')
      expect(fact(host, 'Reasoning')).toBe('5')
      expect(fact(host, 'Last turn')).toBe('125')
      expect(host.textContent).not.toContain('Tokens consumed in Resumed chat')
    } finally { act(() => root.unmount()); useAppStore.setState(previous, true) }
  })

  it('keeps reported zero and known last-turn tokens while labeling an invalid update time', () => {
    const previous = useAppStore.getState()
    const zero = { inputTokens: 0, cachedInputTokens: 0, cacheWriteInputTokens: 0, outputTokens: 0, reasoningOutputTokens: 0, totalTokens: 0 }
    useAppStore.setState({
      sessions: [{ id: 'codex', name: 'Zero usage', providerId: 'codex-cli' }] as typeof previous.sessions,
      acpBindingBySessionId: { codex: { providerId: 'codex-cli', providerUsage: { tokenUsage: { updatedAt: Date.now() + 60000, total: zero, last: zero, modelContextWindow: null }, rateLimits: null } } } as typeof previous.acpBindingBySessionId,
    })
    const host = document.createElement('div'), root = createRoot(host)
    try {
      act(() => root.render(<UsageLimits executionHostId="local" />))
      expect(host.textContent).toContain('Tokens consumed in Zero usage: 0')
      expect(fact(host, 'Last turn')).toBe('0')
      expect(host.textContent).toContain('Update time unavailable')
      expect(host.textContent).toContain('Cached snapshot')
      expect(host.querySelectorAll('.uam-limits-facts')).toHaveLength(1)
    } finally { act(() => root.unmount()); useAppStore.setState(previous, true) }
  })

  it('shows provider consumption from the latest matching local chat without summing snapshots or cache subsets', () => {
    const previous = useAppStore.getState()
    const tokens = (time: number, count: number) => ({ updatedAt: time, total: { inputTokens: 120, cachedInputTokens: 20, cacheWriteInputTokens: 5, outputTokens: 41, reasoningOutputTokens: 3, totalTokens: count }, last: null, modelContextWindow: null })
    useAppStore.setState({
      sessions: [{ id: 'old', name: 'Old chat', providerId: 'claude-cli' }, { id: 'new', name: 'Current Claude', providerId: 'claude-cli' }, { id: 'remote', name: 'Remote chat', providerId: 'claude-cli', executionHostId: 'ssh' }, { id: 'mismatch', name: 'Wrong provider', providerId: 'opencode-cli' }] as typeof previous.sessions,
      acpBindingBySessionId: {
        old: { providerId: 'claude-cli', providerUsage: { tokenUsage: tokens(1, 99), rateLimits: null } },
        new: { providerId: 'claude-cli', providerUsage: { tokenUsage: tokens(2, 161), rateLimits: null } },
        remote: { providerId: 'claude-cli', providerUsage: { tokenUsage: tokens(3, 777), rateLimits: null } },
        mismatch: { providerId: 'claude-cli', providerUsage: { tokenUsage: tokens(4, 888), rateLimits: null } },
      } as typeof previous.acpBindingBySessionId,
    })
    const host = document.createElement('div'), root = createRoot(host)
    try {
      act(() => root.render(<UsageLimits executionHostId="local" />))
      expect(host.textContent).toContain('Tokens consumed in Current Claude: 161')
      expect(fact(host, 'Input')).toBe('120')
      expect(fact(host, 'Output')).toBe('41')
      expect(fact(host, 'Cache read')).toBe('20')
      expect(fact(host, 'Cache write')).toBe('5')
      expect(fact(host, 'Reasoning')).toBe('3')
      expect(fact(host, 'Last turn')).toBe('Unavailable')
      expect(host.textContent).toContain('Cached snapshot')
      expect(host.textContent).not.toContain('Tokens consumed in Old chat')
      expect(host.textContent).not.toContain('Remote chat')
      expect(host.textContent).not.toContain('Wrong provider')
      expect(host.textContent).not.toContain('Tokens consumed in Current Claude: 186')
    } finally { act(() => root.unmount()); useAppStore.setState(previous, true) }
  })

  it('shows account allowances once on All machines, without machine headings or remote reads', async () => {
    const previous = useAppStore.getState()
    const limits = (percent: number) => ({ updatedAt: Date.now(), limitId: '', limitName: '', planType: '', primary: { usedPercent: percent, resetsAt: null, windowDurationMinutes: 300 }, secondary: null, credits: null, individualLimit: null, spendControlReached: null, rateLimitReachedType: '' })
    useAppStore.setState({ executionHosts: [{ id: 'local', label: 'Mac' }, { id: 'peer', label: 'Windows' }] as typeof previous.executionHosts,
      sessions: [], acpBindingBySessionId: {} })
    const requestedHosts: string[] = []
    window.cefQuery = request => {
      const { action, payload } = JSON.parse(request.request)
      expect(action).toBe('getProviderAllowances')
      requestedHosts.push(payload.executionHostId)
      request.onSuccess(JSON.stringify({ executionHostId: payload.executionHostId, refreshing: false, providers: [{ providerId: 'codex-cli', scope: 'local:codex-command', status: 'ready', limits: limits(25), error: '' }] }))
    }
    const host = document.createElement('div'), root = createRoot(host)
    try {
      await act(async () => root.render(<UsageLimits executionHostId="*" />))
      expect(requestedHosts).toEqual(['local'])
      expect(host.textContent).not.toContain('Mac')
      expect(host.textContent).not.toContain('Windows')
      expect(host.querySelectorAll('[aria-label="Codex"]')).toHaveLength(1)
      expect(host.querySelector('[aria-label="Codex"]')?.textContent).toContain('25% used')
      expect(host.textContent).toContain('Refresh allowances')
    } finally { act(() => root.unmount()); useAppStore.setState(previous, true) }
  })

  it('reads account allowances independently while retaining matching chat context and passed resets', async () => {
    const previous = useAppStore.getState()
    const limits = (percent: number, time: number) => ({ updatedAt: time, limitId: '', limitName: '', planType: '', primary: { usedPercent: percent, resetsAt: Math.floor(Date.now() / 1000) - 60, windowDurationMinutes: 300 }, secondary: null, credits: null, individualLimit: null, spendControlReached: null, rateLimitReachedType: '' })
    useAppStore.setState({ sessions: [{ id: 'older', providerId: 'codex-cli' }, { id: 'newer', name: 'Latest chat', providerId: 'codex-cli' }, { id: 'remote', providerId: 'codex-cli', executionHostId: 'ssh' }, { id: 'mismatch', providerId: 'claude-cli' }] as typeof previous.sessions,
      acpBindingBySessionId: { older: { providerId: 'codex-cli', providerUsage: { tokenUsage: null, rateLimits: limits(90, 1) } }, newer: { providerId: 'codex-cli', providerUsage: { tokenUsage: null, contextUsage: { updatedAt: 2, usedTokens: 1234, windowTokens: 200000 }, rateLimits: limits(80, 2) } }, remote: { providerId: 'codex-cli', providerUsage: { tokenUsage: null, rateLimits: limits(10, 3) } }, mismatch: { providerId: 'codex-cli', providerUsage: { tokenUsage: null, rateLimits: limits(5, 4) } } } as typeof previous.acpBindingBySessionId })
    window.cefQuery = request => request.onSuccess(JSON.stringify({ executionHostId: 'local', refreshing: false, providers: [{ providerId: 'codex-cli', scope: 'local:codex-command', status: 'ready', limits: limits(25, Date.now()), error: '' }] }))
    const host = document.createElement('div'), root = createRoot(host)
    try {
      await act(async () => root.render(<UsageLimits executionHostId="local" />))
      expect(host.textContent).toContain('25% used')
      expect(host.textContent).toContain('1,234 of 200,000 tokens')
      expect(host.textContent).not.toContain('90% remaining')
      expect(host.textContent).not.toContain('95% remaining')
      expect(host.textContent).not.toContain('80% used')
      expect(host.textContent).toContain('Provider-reported reset time has passed')
      expect(host.querySelector('[role="meter"]')?.getAttribute('aria-valuenow')).toBe('25')
    } finally { act(() => root.unmount()); useAppStore.setState(previous, true) }
  })
})
