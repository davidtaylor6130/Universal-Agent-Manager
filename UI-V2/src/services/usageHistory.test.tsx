import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, describe, expect, it, vi } from 'vitest'
import { useUsageHistory } from '../hooks/useUsageHistory'
import { formatUsageCost, parseUsageHistory, USAGE_PROVIDERS, usageWindow, type UsageHistory, type UsageTotals } from './usageHistory'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
const windowRange = { startMs: 86400000, endMs: 172800000 }
const total: UsageTotals = { inputTokens: 10, cachedInputTokens: 5, cacheWriteTokens: 0, outputTokens: 2, reasoningTokens: 1, totalTokens: 17,
  reportedCostUsd: 0, estimatedCostUsd: 0, estimatedCacheSavingsUsd: 0, reportedCostTokens: 0, estimatedCostTokens: 0, unpricedTokens: 17, records: 1, sessions: 1 }
function history(overrides: Partial<UsageHistory> = {}): UsageHistory {
  return { version: 1, source: 'native-transcripts', executionHostId: 'local', ...windowRange, totals: { ...total }, complete: true,
    filesChecked: 1, filesTotal: 1, bytesRead: 0, duplicates: 0, unidentifiedRecords: 0, listingLimited: false,
    providers: USAGE_PROVIDERS.map(providerId => ({ ...total, providerId, available: providerId === 'codex-cli', files: 1, errors: 0, invalidLines: 0, oversizedLines: 0, suppressedForkRecords: 0, pendingLines: 0 })),
    models: [{ ...total, providerId: 'codex-cli', modelId: 'test-model', speed: 'standard' }], days: [{ ...total, startMs: 86400000 }], hours: [], ...overrides }
}
afterEach(() => { delete window.cefQuery; vi.useRealTimers() })

describe('usage history contract', () => {
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
    expect(usageWindow(90, 10000000000).endMs - usageWindow(90, 10000000000).startMs).toBe(90 * 86400000)
  })
})

describe('usage history loading', () => {
  it('loads sequential batches and ignores obsolete replies after selecting another host', async () => {
    vi.useFakeTimers()
    const pending: Parameters<NonNullable<typeof window.cefQuery>>[0][] = []
    window.cefQuery = request => { pending.push(request) }
    let state!: ReturnType<typeof useUsageHistory>
    function Probe({ host = 'local' }: { host?: string }) { state = useUsageHistory(host, windowRange); return null }
    const root = createRoot(document.createElement('div'))
    try {
      await act(async () => root.render(<Probe />))
      expect(pending).toHaveLength(1)
      await act(async () => pending[0].onSuccess(JSON.stringify(history({ complete: false, filesTotal: 2 }))))
      expect(state.data?.filesChecked).toBe(1)
      await act(async () => vi.advanceTimersByTimeAsync(0))
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
  it('stops a stalled scan rather than polling forever and ignores responses after unmount', async () => {
    vi.useFakeTimers()
    let calls = 0
    window.cefQuery = request => { calls++; request.onSuccess(JSON.stringify(history({ complete: false, filesTotal: 2 }))) }
    let state!: ReturnType<typeof useUsageHistory>
    function Probe() { state = useUsageHistory('local', windowRange); return null }
    const root = createRoot(document.createElement('div'))
    await act(async () => root.render(<Probe />))
    await act(async () => vi.advanceTimersByTimeAsync(10))
    expect(calls).toBe(3)
    expect(state.error).toContain('stopped')
    expect(state.data?.complete).toBe(false)
    act(() => root.unmount())
    await act(async () => vi.advanceTimersByTimeAsync(100))
    expect(calls).toBe(3)
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
