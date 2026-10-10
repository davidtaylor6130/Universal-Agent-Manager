import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, describe, expect, it, vi } from 'vitest'
import { useUsageHistory } from '../hooks/useUsageHistory'
import { formatUsageCost, parseUsageHistory, USAGE_PROVIDERS, usageWindow, type UsageHistory, type UsageTotals } from './usageHistory'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
const windowRange = { startMs: 86400000, endMs: 172800000, days: 1 as const }
const total: UsageTotals = { inputTokens: 10, cachedInputTokens: 5, cacheWriteTokens: 0, outputTokens: 2, reasoningTokens: 1, totalTokens: 17,
  reportedCostUsd: 0, estimatedCostUsd: 0, estimatedCacheSavingsUsd: 0, reportedCostTokens: 0, estimatedCostTokens: 0, unpricedTokens: 17, records: 1, sessions: 1 }
function history(overrides: Partial<UsageHistory> = {}): UsageHistory {
  return { version: 2, source: 'native-transcripts', executionHostId: 'local', ...windowRange, totals: { ...total }, complete: true,
    filesChecked: 1, filesTotal: 1, bytesRead: 0, duplicates: 0, unidentifiedRecords: 0, listingLimited: false,
    providers: USAGE_PROVIDERS.map(providerId => ({ ...(providerId === 'codex-cli' ? total : { ...total, inputTokens: 0, cachedInputTokens: 0, outputTokens: 0, reasoningTokens: 0, totalTokens: 0, unpricedTokens: 0, records: 0, sessions: 0 }), providerId, available: providerId === 'codex-cli', files: 1, errors: 0, invalidLines: 0, oversizedLines: 0, suppressedForkRecords: 0, pendingLines: 0 })),
    models: [{ ...total, providerId: 'codex-cli', modelId: 'test-model', speed: 'standard' }], days: [{ ...total, startMs: 86400000 }], hours: [], providerDays: [{ ...total, providerId: 'codex-cli', startMs: 86400000 }], providerHours: [], ...overrides }
}
afterEach(() => { delete window.cefQuery; vi.useRealTimers() })

describe('usage history contract', () => {
  it('validates automatic price references and catalog freshness at the bridge boundary', () => {
    const reference = { kind: 'reference', referenceModel: 'qwen3.8-27b', source: 'https://example.com/rates', verifiedDate: '2026-10-10', cacheFallback: true, speedFallback: false, inputPerMillion: 0.5, cachedInputPerMillion: null, cacheWritePerMillion: 0, outputPerMillion: 3 }
    const pricing = { refreshing: true, source: 'https://example.com/catalog', status: 'cached', fetchedAtMs: 1000, error: '', aliasError: '' }
    const data = history()
    const value = { ...data, models: [{ ...data.models[0], priceReference: reference }], pricing }
    expect(parseUsageHistory(value, 'local', windowRange).models[0].priceReference?.cacheWritePerMillion).toBe(0)
    for (const changes of [{ source: 'javascript:alert(1)' }, { source: 'https://user:password@example.com' }, { inputPerMillion: -1 }, { cachedInputPerMillion: undefined }, { speedFallback: 'false' }, { verifiedDate: 'yesterday' }]) {
      expect(() => parseUsageHistory({ ...value, models: [{ ...data.models[0], priceReference: { ...reference, ...changes } }] }, 'local', windowRange)).toThrow('breakdown')
    }
    for (const changes of [{ fetchedAtMs: 8640000000000001 }, { refreshing: 1 }, { source: 'file:///tmp/rates' }, { status: 'other' }]) {
      expect(() => parseUsageHistory({ ...value, pricing: { ...pricing, ...changes } }, 'local', windowRange)).toThrow('pricing')
    }
    expect(() => parseUsageHistory({ ...data, version: 3, asOfMs: 8640000000000001, windowDays: 1, collecting: false, collectionError: '' }, 'local', windowRange)).toThrow('Cached usage')
  })

  it('preserves unknown cost, real zero prices, partial coverage and reasoning as a subset', () => {
    const value = parseUsageHistory(history(), 'local', windowRange)
    expect(formatUsageCost(value.totals)).toBe('Unknown')
    expect(formatUsageCost({ ...total, unpricedTokens: 0, estimatedCostTokens: 17 })).not.toBe('Unknown')
    expect(formatUsageCost({ ...total, unpricedTokens: 7, reportedCostTokens: 10, reportedCostUsd: 0.05 })).toContain('0.05')
    expect(value.totals.totalTokens).toBe(17)
  })
  it('rejects mismatched contracts, hosts, windows and malformed or overflowing money/counts', () => {
    expect(() => parseUsageHistory({}, 'local', windowRange)).toThrow('incompatible')
    expect(() => parseUsageHistory(history(), 'remote', windowRange)).toThrow('different host')
    expect(() => parseUsageHistory(history(), 'local', { ...windowRange, endMs: 172800001 })).toThrow('time range')
    for (const values of [{ totalTokens: 18 }, { reasoningTokens: 3 }, { inputTokens: '10' }, { inputTokens: Number.MAX_SAFE_INTEGER + 1 }, { reportedCostUsd: Infinity }, { reportedCostUsd: Number.MAX_VALUE, estimatedCostUsd: Number.MAX_VALUE }, { unpricedTokens: 0 }, { sessions: 2 }]) {
      expect(() => parseUsageHistory(history({ totals: { ...total, ...values } as UsageTotals }), 'local', windowRange)).toThrow('invalid')
    }
  })
  it('rejects duplicate providers/models, invalid buckets and inconsistent scan progress', () => {
    const value = history()
    expect(() => parseUsageHistory({ ...value, providers: [...value.providers.slice(1), value.providers[1]] }, 'local', windowRange)).toThrow('provider')
    expect(() => parseUsageHistory({ ...value, models: [...value.models, ...value.models] }, 'local', windowRange)).toThrow('breakdown')
    for (const startMs of [86400001, 0, 172800000]) expect(() => parseUsageHistory({ ...value, days: [{ ...total, startMs }] }, 'local', windowRange)).toThrow('breakdown')
    expect(() => parseUsageHistory({ ...value, filesChecked: 2 }, 'local', windowRange)).toThrow('invalid')
    expect(() => parseUsageHistory({ ...value, providerDays: [] }, 'local', windowRange)).toThrow('period')
    expect(() => parseUsageHistory({ ...value, providerDays: [...value.providerDays, ...value.providerDays] }, 'local', windowRange)).toThrow('breakdown')
    expect(() => parseUsageHistory({ ...value, providerDays: [{ ...value.providerDays[0], sessions: 0 }] }, 'local', windowRange)).toThrow('period')
    expect(() => parseUsageHistory({ ...value, providers: value.providers.map(provider => ({ ...provider, sessions: 0 })) }, 'local', windowRange)).toThrow('provider')
    expect(usageWindow(90, 10000000000).startMs).toBe(Math.floor(10000000000 / 86400000) * 86400000 - 89 * 86400000)
    expect(usageWindow(1, 10000000000).startMs).toBe(10000000000 - 86400000)
  })
})

describe('usage history loading', () => {
  it('polls price refreshes promptly after history is complete, then returns to background cadence', async () => {
    vi.useFakeTimers()
    let calls = 0
    window.cefQuery = request => {
      calls++
      request.onSuccess(JSON.stringify(history({ pricing: { refreshing: calls === 1, source: 'https://example.com/catalog', status: 'fresh', fetchedAtMs: 1000, error: '', aliasError: '' } })))
    }
    function Probe() { useUsageHistory('local', windowRange); return null }
    const root = createRoot(document.createElement('div'))
    try {
      await act(async () => root.render(<Probe />))
      await act(async () => vi.advanceTimersByTimeAsync(2000))
      expect(calls).toBe(2)
      await act(async () => vi.advanceTimersByTimeAsync(29999))
      expect(calls).toBe(2)
      await act(async () => vi.advanceTimersByTimeAsync(1))
      expect(calls).toBe(3)
    } finally { act(() => root.unmount()) }
  })

  it('polls cached snapshots and ignores obsolete replies after selecting another host', async () => {
    vi.useFakeTimers()
    const pending: Parameters<NonNullable<typeof window.cefQuery>>[0][] = []
    window.cefQuery = request => { pending.push(request) }
    let state!: ReturnType<typeof useUsageHistory>
    function Probe({ host = 'local' }: { host?: string }) { state = useUsageHistory(host, windowRange); return null }
    const root = createRoot(document.createElement('div'))
    try {
      await act(async () => root.render(<Probe />))
      expect(pending).toHaveLength(1)
      await act(async () => pending[0].onSuccess(JSON.stringify(history({ complete: false, filesTotal: 2, collecting: true }))))
      expect(state.data?.filesChecked).toBe(1)
      await act(async () => vi.advanceTimersByTimeAsync(2000))
      expect(pending).toHaveLength(2)
      expect(JSON.parse(pending[1].request).payload.refreshId).toBe(JSON.parse(pending[0].request).payload.refreshId)
      await act(async () => root.render(<Probe host="remote" />))
      expect(state.kept).toBe(true)
      await act(async () => pending[1].onSuccess(JSON.stringify(history({ filesChecked: 2, filesTotal: 2 }))))
      expect(state.data?.filesChecked).toBe(1)
      await act(async () => pending[2].onFailure(400, 'Usage history is not available on remote hosts yet.'))
      expect(state.error).toContain('remote hosts')
      expect(state.loading).toBe(false)
      expect(state.kept).toBe(true)
    } finally { act(() => root.unmount()) }
  })
  it('retains the last answer muted while new prices load and refreshes without overlap', async () => {
    const pending: Parameters<NonNullable<typeof window.cefQuery>>[0][] = []
    window.cefQuery = request => { pending.push(request) }
    let state!: ReturnType<typeof useUsageHistory>
    function Probe({ price = 1 }: { price?: number }) { state = useUsageHistory('local', windowRange, [{ providerId: 'codex-cli', modelId: 'test-model', speed: 'standard', inputPerMillion: price, cachedInputPerMillion: null, cacheWritePerMillion: null, outputPerMillion: null }]); return null }
    const root = createRoot(document.createElement('div'))
    try {
      await act(async () => root.render(<Probe />))
      await act(async () => pending[0].onSuccess(JSON.stringify(history())))
      expect(state.kept).toBe(false)
      await act(async () => root.render(<Probe price={2} />))
      expect(state.kept).toBe(true)
      await act(async () => pending[1].onSuccess(JSON.stringify(history())))
      expect(state.kept).toBe(false)
      await act(async () => state.refresh())
      expect(pending).toHaveLength(3)
      expect(JSON.parse(pending[2].request).payload.refreshId).not.toBe(JSON.parse(pending[1].request).payload.refreshId)
    } finally { act(() => root.unmount()) }
  })
  it('keeps background progress polling bounded and stops after unmount', async () => {
    vi.useFakeTimers()
    let calls = 0
    window.cefQuery = request => { calls++; request.onSuccess(JSON.stringify(history({ complete: false, filesTotal: 2, collecting: true }))) }
    let state!: ReturnType<typeof useUsageHistory>
    function Probe() { state = useUsageHistory('local', windowRange); return null }
    const root = createRoot(document.createElement('div'))
    await act(async () => root.render(<Probe />))
    await act(async () => vi.advanceTimersByTimeAsync(10000))
    expect(calls).toBe(6)
    expect(state.error).toBe('')
    expect(state.loading).toBe(true)
    act(() => root.unmount())
    await act(async () => vi.advanceTimersByTimeAsync(60000))
    expect(calls).toBe(6)
  })
  it('validates the cached as-of period and displays stored accounting before collection completes', () => {
    const cached = history({ version: 3, asOfMs: windowRange.endMs, windowDays: 1, collecting: true, collectionError: '', complete: false })
    expect(parseUsageHistory(cached, 'local', { ...windowRange, endMs: windowRange.endMs + 123 }).totals.totalTokens).toBe(17)
    expect(() => parseUsageHistory({ ...cached, windowDays: 7 }, 'local', windowRange)).toThrow('Cached usage')
    expect(() => parseUsageHistory({ ...cached, startMs: windowRange.startMs + 1 }, 'local', windowRange)).toThrow('time range')
  })
  it('times out a missing native reply and never invents usage for browser previews', async () => {
    vi.useFakeTimers()
    let state!: ReturnType<typeof useUsageHistory>
    function Probe() { state = useUsageHistory('local', windowRange); return null }
    const root = createRoot(document.createElement('div'))
    try {
      await act(async () => root.render(<Probe />))
      expect(state.error).toContain('preview has no provider history')
      window.cefQuery = () => {}
      await act(async () => state.refresh())
      await act(async () => vi.advanceTimersByTimeAsync(30000))
      expect(state.error).toContain('too long')
      expect(state.loading).toBe(false)
    } finally { act(() => root.unmount()) }
  })
})
