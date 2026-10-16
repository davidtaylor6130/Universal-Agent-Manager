import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { useProviderAllowances } from './useProviderAllowances'
import { clearProviderAllowanceCache, parseProviderAllowances, preloadProviderAllowances, type ProviderAllowanceSnapshot } from '../services/providerAllowances'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
type Request = Parameters<NonNullable<typeof window.cefQuery>>[0]
const now = 1_700_000_000_000
function snapshot(host = 'local', refreshing = false): ProviderAllowanceSnapshot {
  return { executionHostId: host, refreshing, providers: [{ providerId: 'codex-cli', scope: `${host}:codex-command`, status: 'ready', error: '', limits: { updatedAt: now, limitId: '', limitName: '', planType: 'plus', primary: { usedPercent: 29, resetsAt: now / 1000 + 3600, windowDurationMinutes: 300 }, secondary: null, credits: null, individualLimit: null, spendControlReached: null, rateLimitReachedType: '' } }] }
}
async function mount(host = 'local') {
  let state!: ReturnType<typeof useProviderAllowances>
  function Probe({ host }: { host: string }) { state = useProviderAllowances(host); return null }
  const root = createRoot(document.createElement('div'))
  await act(async () => root.render(<Probe host={host} />))
  return { get state() { return state }, close: () => act(() => root.unmount()), change: async (host: string) => { await act(async () => root.render(<Probe host={host} />)) } }
}
beforeEach(() => clearProviderAllowanceCache())
afterEach(() => { delete window.cefQuery; vi.useRealTimers() })

describe('independent provider allowance cache', () => {
  it('coalesces simultaneous readers and refreshes without creating a provider session', async () => {
    const pending: Request[] = []
    window.cefQuery = request => { pending.push(request) }
    const first = await mount(), second = await mount()
    try {
      expect(pending).toHaveLength(1)
      await act(async () => pending[0].onSuccess(JSON.stringify(snapshot())))
      expect(first.state.data?.providers[0].limits?.primary?.usedPercent).toBe(29)
      expect(second.state.data).toBe(first.state.data)
      await act(async () => { first.state.refresh(); second.state.refresh() })
      expect(pending).toHaveLength(2)
      expect(JSON.parse(pending[1].request)).toMatchObject({ action: 'getProviderAllowances', payload: { executionHostId: 'local', refresh: true } })
      expect(first.state.data?.providers[0].limits?.primary?.usedPercent).toBe(29)
      await act(async () => pending[1].onSuccess(JSON.stringify(snapshot())))
    } finally { first.close(); second.close() }
  })

  it('shows an already read account snapshot immediately on remount while rechecking the native cache', async () => {
    window.cefQuery = request => request.onSuccess(JSON.stringify(snapshot()))
    const first = await mount()
    const known = first.state.data
    first.close()
    let pending!: Request
    window.cefQuery = request => { pending = request }
    const second = await mount()
    try {
      expect(second.state.data).toBe(known)
      expect(second.state.loading).toBe(false)
      expect(second.state.refreshing).toBe(false)
      expect(second.state.requestingRefresh).toBe(false)
      await act(async () => pending.onFailure(503, 'Account service temporarily unavailable'))
      expect(second.state.data).toBe(known)
      expect(second.state.error).toBe('Account service temporarily unavailable')
    } finally { second.close() }
  })

  it('polls an active refresh quickly, then returns to the stable interval and cleans up', async () => {
    vi.useFakeTimers(); vi.setSystemTime(now)
    let calls = 0
    window.cefQuery = request => request.onSuccess(JSON.stringify(snapshot('local', ++calls < 2)))
    const view = await mount()
    try {
      expect(view.state.refreshing).toBe(true)
      await act(async () => vi.advanceTimersByTimeAsync(1000))
      expect(calls).toBe(2)
      expect(view.state.refreshing).toBe(false)
      await act(async () => vi.advanceTimersByTimeAsync(29999))
      expect(calls).toBe(2)
      await act(async () => vi.advanceTimersByTimeAsync(1))
      expect(calls).toBe(3)
    } finally { view.close() }
    expect(vi.getTimerCount()).toBe(0)
  })

  it('keeps exact snapshot age and provider labels when native retains a cached snapshot after an error', async () => {
    const ready = snapshot()
    ready.providers[0] = { ...ready.providers[0], providerId: 'copilot-cli', primaryLabel: 'Chat allowance', secondaryLabel: 'Completions allowance' }
    window.cefQuery = request => request.onSuccess(JSON.stringify(ready))
    const view = await mount()
    try {
      const failed = { ...ready, providers: [{ ...ready.providers[0], status: 'error', error: 'Quota could not be read' }] }
      window.cefQuery = request => request.onSuccess(JSON.stringify(failed))
      await act(async () => view.state.refresh())
      expect(view.state.data?.providers[0]).toMatchObject({ status: 'error', error: 'Quota could not be read', primaryLabel: 'Chat allowance', secondaryLabel: 'Completions allowance', limits: { updatedAt: now, primary: { usedPercent: 29 } } })
    } finally { view.close() }
  })

  it('drops cached account values and labels when the native provider command scope changes', async () => {
    const ready = { ...snapshot(), providers: snapshot().providers.map(row => ({ ...row, scope: 'native-command-a', primaryLabel: 'Old account allowance' })) }
    window.cefQuery = request => request.onSuccess(JSON.stringify(ready))
    const view = await mount()
    try {
      const changed = { ...ready, providers: [{ providerId: 'codex-cli', scope: 'native-command-b', status: 'loading', error: '', limits: null }], refreshing: true }
      window.cefQuery = request => request.onSuccess(JSON.stringify(changed))
      await act(async () => view.state.refresh())
      expect(view.state.data?.providers[0].limits).toBeNull()
      expect(view.state.data?.providers[0].primaryLabel).toBeUndefined()
      expect(view.state.data?.providers[0]).toMatchObject({ scope: 'native-command-b', status: 'loading' })
      window.cefQuery = request => request.onSuccess(JSON.stringify({ ...ready, providers: ready.providers.map(row => ({ ...row, scope: 'native-command-b' })) }))
      await act(async () => view.state.refresh())
      expect(view.state.data?.providers[0].limits?.primary?.usedPercent).toBe(29)
      window.cefQuery = request => request.onSuccess(JSON.stringify({ ...changed, providers: [], refreshing: false }))
      await act(async () => view.state.refresh())
      expect(view.state.data?.providers).toEqual([])
    } finally { view.close() }
  })

  it('honours native authentication invalidation even when the provider command scope is unchanged', async () => {
    const ready = snapshot()
    window.cefQuery = request => request.onSuccess(JSON.stringify(ready))
    const view = await mount()
    try {
      window.cefQuery = request => request.onSuccess(JSON.stringify({ ...ready, providers: [{ ...ready.providers[0], status: 'error', limits: null, error: 'Sign in again to read account allowances.' }] }))
      await act(async () => view.state.refresh())
      expect(view.state.data?.providers[0].limits).toBeNull()
      expect(view.state.data?.providers[0].error).toContain('Sign in again')
    } finally { view.close() }
  })

  it('rejects a foreign host or malformed percentages instead of displaying invented values', async () => {
    const ready = snapshot()
    expect(() => parseProviderAllowances(ready, 'other-host')).toThrow('invalid computer')
    for (const usedPercent of [NaN, -1, 101, '29']) {
      expect(() => parseProviderAllowances({ ...ready, providers: [{ ...ready.providers[0], limits: { ...ready.providers[0].limits, primary: { usedPercent, resetsAt: null, windowDurationMinutes: null } } }] }, 'local')).toThrow('invalid provider')
    }
    expect(() => parseProviderAllowances({ ...ready, providers: [ready.providers[0], ready.providers[0]] }, 'local')).toThrow('invalid provider')
    for (const scope of [undefined, '', 'x'.repeat(257), 12]) expect(() => parseProviderAllowances({ ...ready, providers: [{ ...ready.providers[0], scope }] }, 'local')).toThrow('invalid provider')
    window.cefQuery = request => request.onSuccess(JSON.stringify(snapshot('foreign')))
    const view = await mount()
    try { expect(view.state.data).toBeNull(); expect(view.state.error).toContain('invalid computer') } finally { view.close() }
  })

  it('reports a bounded timeout and ignores the late native response', async () => {
    vi.useFakeTimers(); vi.setSystemTime(now)
    let pending!: Request
    window.cefQuery = request => { pending = request }
    const view = await mount()
    try {
      await act(async () => vi.advanceTimersByTimeAsync(15000))
      expect(view.state.loading).toBe(false)
      expect(view.state.error).toContain('took too long')
      await act(async () => pending.onSuccess(JSON.stringify(snapshot())))
      expect(view.state.data).toBeNull()
    } finally { view.close() }
    expect(vi.getTimerCount()).toBe(0)
  })

  it('does not manufacture browser-preview account data and allows a later valid connection', async () => {
    const view = await mount()
    try {
      expect(view.state.loading).toBe(false)
      expect(view.state.error).toContain('desktop app')
      window.cefQuery = request => request.onSuccess(JSON.stringify(snapshot()))
      await act(async () => preloadProviderAllowances())
      expect(view.state.data?.providers[0].limits?.primary?.usedPercent).toBe(29)
    } finally { view.close() }
  })

  it('does not flicker native refresh state during a background cache read and queues one manual follow-up', async () => {
    vi.useFakeTimers(); vi.setSystemTime(now)
    window.cefQuery = request => request.onSuccess(JSON.stringify(snapshot()))
    const view = await mount()
    const pending: Request[] = []
    window.cefQuery = request => { pending.push(request) }
    try {
      await act(async () => vi.advanceTimersByTimeAsync(30000))
      expect(pending).toHaveLength(1)
      expect(JSON.parse(pending[0].request).payload.refresh).toBeUndefined()
      expect(view.state.refreshing).toBe(false)
      await act(async () => { view.state.refresh(); view.state.refresh() })
      expect(pending).toHaveLength(1)
      await act(async () => pending[0].onSuccess(JSON.stringify(snapshot())))
      expect(pending).toHaveLength(2)
      expect(JSON.parse(pending[1].request).payload.refresh).toBe(true)
      await act(async () => { view.state.refresh(); pending[1].onSuccess(JSON.stringify(snapshot())) })
      expect(pending).toHaveLength(2)
    } finally { view.close() }
    expect(vi.getTimerCount()).toBe(0)
  })

  it('reads remote availability once and does not poll or force-refresh unsupported remote computers', async () => {
    vi.useFakeTimers(); vi.setSystemTime(now)
    const requests: Request[] = []
    window.cefQuery = request => { requests.push(request); request.onSuccess(JSON.stringify({ executionHostId: 'ssh', providers: [], refreshing: false })) }
    const view = await mount('ssh')
    try {
      expect(requests).toHaveLength(1)
      await act(async () => { view.state.refresh(); await vi.advanceTimersByTimeAsync(120000) })
      expect(requests).toHaveLength(1)
    } finally { view.close() }
    expect(vi.getTimerCount()).toBe(0)
  })

  it('validates every model allowance without truncating or accepting duplicate and malformed windows', () => {
    const ready = snapshot()
    const window = { id: 'gemini-model:requests', label: 'Gemini model requests', usedPercent: 0.4, resetsAt: null, windowDurationMinutes: null }
    const withWindows = (windows: unknown) => ({ ...ready, providers: [{ ...ready.providers[0], windows }] })
    expect(parseProviderAllowances(withWindows([window]), 'local').providers[0]).toMatchObject({ windows: [window] })
    for (const windows of [null, {}, [window, window], Array.from({ length: 65 }, (_, index) => ({ ...window, id: String(index) })), [{ ...window, id: '' }], [{ ...window, label: 'x'.repeat(129) }], [{ ...window, usedPercent: -1 }], [{ ...window, resetsAt: -1 }], [{ ...window, windowDurationMinutes: 0 }]]) {
      expect(() => parseProviderAllowances(withWindows(windows), 'local')).toThrow('invalid provider')
    }
    const maximum = Array.from({ length: 64 }, (_, index) => ({ ...window, id: String(index), label: `Model ${index}` }))
    expect((parseProviderAllowances(withWindows(maximum), 'local').providers[0] as unknown as { windows: unknown[] }).windows).toHaveLength(64)
  })

  it('honours a native cooldown returned by a pending cache read instead of sending a queued forced request', async () => {
    const pending: Request[] = []
    window.cefQuery = request => { pending.push(request) }
    const view = await mount()
    try {
      await act(async () => view.state.refresh())
      await act(async () => pending[0].onSuccess(JSON.stringify({ ...snapshot(), refreshAfterMs: 5000 })))
      expect(pending).toHaveLength(1)
      expect(view.state.requestingRefresh).toBe(false)
      await act(async () => view.state.refresh())
      expect(pending).toHaveLength(1)
      for (const refreshAfterMs of [-1, 30001, 1.5, '5000', null]) expect(() => parseProviderAllowances({ ...snapshot(), refreshAfterMs }, 'local')).toThrow('invalid')
    } finally { view.close() }
  })
})
