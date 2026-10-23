import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { useAppStore } from '../../store/useAppStore'
import type { AcpRateLimits } from '../../store/cpp/types'
import { UsageLimits } from './UsageLimits'
import { clearProviderAllowanceCache, type ProviderAllowanceSnapshot } from '../../services/providerAllowances'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
const now = 1_700_000_000_000
beforeEach(() => clearProviderAllowanceCache())

async function renderLimits(limits: AcpRateLimits | null = null) {
  const previous = useAppStore.getState()
  useAppStore.setState({
    sessions: [{ id: 'codex', name: 'Current chat', providerId: 'codex-cli' }] as typeof previous.sessions,
    acpBindingBySessionId: { codex: { providerId: 'codex-cli', providerUsage: { rateLimits: limits } } } as typeof previous.acpBindingBySessionId,
  })
  window.cefQuery = ({ onSuccess }) => onSuccess(JSON.stringify({ executionHostId: 'local', refreshing: false, providers: [{ providerId: 'codex-cli', scope: 'local:codex-command', status: limits ? 'ready' : 'unavailable', limits, error: '' }] }))
  const host = document.createElement('div'), root = createRoot(host)
  await act(async () => root.render(<UsageLimits executionHostId="local" />))
  return { host, close: () => { act(() => root.unmount()); useAppStore.setState(previous, true) } }
}

function allowance(resetsAt: number): AcpRateLimits {
  return { updatedAt: now, limitId: '', limitName: '', planType: '', primary: { usedPercent: 25, resetsAt, windowDurationMinutes: 300 }, secondary: null, credits: null, individualLimit: null, spendControlReached: null, rateLimitReachedType: '' }
}

describe('UsageLimits allowance evidence', () => {
  afterEach(() => { delete window.cefQuery; vi.useRealTimers(); vi.restoreAllMocks() })

  it('does not invent allowance periods when no provider window was reported', async () => {
    const view = await renderLimits()
    try {
      expect(view.host.textContent).not.toContain('5 hour window')
      expect(view.host.textContent).not.toContain('Weekly window')
      expect(view.host.textContent).not.toContain('Primary window')
      expect(view.host.textContent).toContain('Account allowances unavailable.')
    } finally { view.close() }
  })

  it('announces a future reset without claiming that it has passed', async () => {
    vi.spyOn(Date, 'now').mockReturnValue(now)
    const view = await renderLimits(allowance(now / 1000 + 3600))
    try {
      expect(view.host.textContent).toContain('Resets in 1h 0m')
      expect(view.host.textContent?.toLowerCase()).not.toContain('reset passed')
    } finally { view.close() }
  })

  it('exposes numeric meters only for known usage, including a reported zero', async () => {
    const unknown = await renderLimits()
    try {
      expect(unknown.host.querySelectorAll('[role="meter"]')).toHaveLength(0)
      expect(unknown.host.textContent).toContain('Account allowances unavailable.')
    } finally { unknown.close() }

    const limits = allowance(now / 1000 + 3600)
    limits.primary!.usedPercent = 0
    const known = await renderLimits(limits)
    try {
      const meters = known.host.querySelectorAll('[role="meter"]')
      expect(meters).toHaveLength(1)
      expect(meters[0].getAttribute('aria-label')).toBe('Codex primary usage')
      expect(meters[0].getAttribute('aria-valuenow')).toBe('0')
      expect(meters[0].getAttribute('aria-hidden')).not.toBe('true')
    } finally { known.close() }
  })

  it('advances quiet-page reset and freshness labels and cleans up its local clock', async () => {
    vi.useFakeTimers(); vi.setSystemTime(now)
    const view = await renderLimits(allowance(now / 1000 + 60))
    try {
      expect(view.host.textContent).toContain('Resets in 1m')
      expect(view.host.textContent).not.toContain('Cached snapshot')
      await act(async () => vi.advanceTimersByTimeAsync(6 * 60_000))
      expect(view.host.textContent).toContain('Reset passed, awaiting update')
      expect(view.host.textContent).toContain('Updated 6m ago · Cached snapshot')
    } finally { view.close() }
    expect(vi.getTimerCount()).toBe(0)
  })

  it('uses the current clock for an account snapshot read between local clock ticks', async () => {
    vi.useFakeTimers(); vi.setSystemTime(now)
    const view = await renderLimits(allowance(now / 1000 + 60))
    try {
      vi.setSystemTime(now + 1000)
      window.cefQuery = ({ onSuccess }) => onSuccess(JSON.stringify({ executionHostId: 'local', refreshing: false, providers: [{ providerId: 'codex-cli', scope: 'local:codex-command', status: 'ready', limits: { ...allowance(now / 1000 + 60), updatedAt: now + 1000 }, error: '' }] }))
      await act(async () => view.host.querySelector('button')?.click())
      expect(view.host.querySelector('[aria-label="Codex limits"]')?.textContent).toContain('Updated just now')
      expect(view.host.textContent).not.toContain('Update time unavailable')
    } finally { view.close() }
  })
})

describe('UsageLimits independent account allowances', () => {
  afterEach(() => { delete window.cefQuery; vi.useRealTimers(); vi.restoreAllMocks() })

  const snapshot = (executionHostId = 'local', usedPercent = 25): ProviderAllowanceSnapshot => ({ executionHostId, refreshing: false, providers: [{ providerId: 'codex-cli', scope: `${executionHostId}:codex-command`, status: 'ready', limits: { ...allowance(now / 1000 + 3600), primary: { usedPercent, resetsAt: now / 1000 + 3600, windowDurationMinutes: 300 } }, error: '' }] })

  it('reads account allowances without any chats or session bindings', async () => {
    const previous = useAppStore.getState()
    useAppStore.setState({ sessions: [], acpBindingBySessionId: {} })
    const requests: unknown[] = []
    window.cefQuery = ({ request, onSuccess }) => { requests.push(JSON.parse(request)); onSuccess(JSON.stringify(snapshot())) }
    const host = document.createElement('div'), root = createRoot(host)
    try {
      await act(async () => root.render(<UsageLimits executionHostId="local" />))
      expect(requests).toContainEqual(expect.objectContaining({ action: 'getProviderAllowances', payload: expect.objectContaining({ executionHostId: 'local' }) }))
      expect(host.querySelector('[aria-label="Codex limits"]')?.textContent).toContain('25% used')
      expect(host.querySelectorAll('.uam-pace-prov')).toHaveLength(6)
      expect(host.textContent).not.toContain('Latest provider snapshots from chats')
    } finally { act(() => root.unmount()); useAppStore.setState(previous, true) }
  })

  it('uses independent provider values and declared labels rather than a newer chat allowance', async () => {
    const previous = useAppStore.getState()
    useAppStore.setState({ sessions: [{ id: 'chat', providerId: 'codex-cli', name: 'Existing chat' }] as typeof previous.sessions,
      acpBindingBySessionId: { chat: { providerId: 'codex-cli', providerUsage: { rateLimits: { ...allowance(now / 1000 + 3600), updatedAt: now + 1000, primary: { usedPercent: 99, resetsAt: null, windowDurationMinutes: 300 } } } } } as typeof previous.acpBindingBySessionId })
    const data = snapshot()
    data.providers.push({ ...data.providers[0], providerId: 'claude-cli', limits: { ...allowance(now / 1000 + 3600), primary: { usedPercent: 57, resetsAt: null, windowDurationMinutes: 300 } } })
    data.providers.push({ ...data.providers[0], providerId: 'copilot-cli', ...{ primaryLabel: 'Chat allowance', secondaryLabel: 'Completions allowance' } })
    window.cefQuery = ({ onSuccess }) => onSuccess(JSON.stringify(data))
    const host = document.createElement('div'), root = createRoot(host)
    try {
      await act(async () => root.render(<UsageLimits executionHostId="local" />))
      const codex = host.querySelector('[aria-label="Codex limits"]')
      expect(codex?.textContent).toContain('25% used')
      expect(codex?.textContent).not.toContain('99% used')
      expect(host.querySelector('[aria-label="Claude limits"]')?.textContent).toContain('57% used')
      const copilot = host.querySelector('[aria-label="Copilot limits"]')
      expect(copilot?.textContent).toContain('Chat allowance')
      expect(copilot?.textContent).toContain('Completions allowance')
    } finally { act(() => root.unmount()); useAppStore.setState(previous, true) }
  })

  it('keeps known account values visible while manual refresh runs or fails', async () => {
    const previous = useAppStore.getState()
    useAppStore.setState({ sessions: [], acpBindingBySessionId: {} })
    let refresh!: Parameters<NonNullable<typeof window.cefQuery>>[0]
    window.cefQuery = request => {
      if (JSON.parse(request.request).payload.refresh) refresh = request
      else request.onSuccess(JSON.stringify(snapshot('local')))
    }
    const host = document.createElement('div'), root = createRoot(host)
    try {
      await act(async () => root.render(<UsageLimits executionHostId="local" />))
      const button = host.querySelector('button')
      expect(button?.textContent).toContain('Refresh allowances')
      await act(async () => button?.click())
      expect(host.querySelector('[aria-label="Codex limits"]')?.textContent).toContain('25% used')
      await act(async () => refresh.onFailure(503, 'Account refresh unavailable'))
      expect(host.querySelector('[aria-label="Codex limits"]')?.textContent).toContain('25% used')
      expect(host.textContent).toContain('Account refresh unavailable')
    } finally { act(() => root.unmount()); useAppStore.setState(previous, true) }
  })

  it('rejects a late response from the previously selected host', async () => {
    const previous = useAppStore.getState()
    useAppStore.setState({ sessions: [], acpBindingBySessionId: {} })
    const pending: Parameters<NonNullable<typeof window.cefQuery>>[0][] = []
    window.cefQuery = request => { pending.push(request) }
    const host = document.createElement('div'), root = createRoot(host)
    try {
      await act(async () => root.render(<UsageLimits executionHostId="first-host" />))
      await act(async () => root.render(<UsageLimits executionHostId="second-host" />))
      expect(pending).toHaveLength(2)
      await act(async () => pending[1].onSuccess(JSON.stringify(snapshot('second-host', 70))))
      expect(host.querySelector('[aria-label="Codex limits"]')?.textContent).toContain('70% used')
      await act(async () => pending[0].onSuccess(JSON.stringify(snapshot('first-host', 10))))
      expect(host.querySelector('[aria-label="Codex limits"]')?.textContent).toContain('70% used')
      expect(host.querySelector('[aria-label="Codex limits"]')?.textContent).not.toContain('10% used')
    } finally { act(() => root.unmount()); useAppStore.setState(previous, true) }
  })

  it('labels remote account reads unavailable and does not offer a nonworking refresh control', async () => {
    const previous = useAppStore.getState()
    useAppStore.setState({ sessions: [], acpBindingBySessionId: {} })
    window.cefQuery = request => request.onSuccess(JSON.stringify({ executionHostId: 'remote', refreshing: false, providers: [] }))
    const host = document.createElement('div'), root = createRoot(host)
    try {
      await act(async () => root.render(<UsageLimits executionHostId="remote" />))
      expect(host.textContent).toContain('Not available for remote computers.')
      expect(host.textContent).not.toContain('Account allowances on this computer.')
      expect(host.querySelector('button')).toBeNull()
    } finally { act(() => root.unmount()); useAppStore.setState(previous, true) }
  })

  it('shows fractional account usage without rounding a reported Copilot fraction to zero', async () => {
    const data = snapshot()
    data.providers[0] = { ...data.providers[0], providerId: 'copilot-cli', primaryLabel: 'Chat allowance', limits: { ...allowance(now / 1000 + 3600), primary: { usedPercent: 0.4, resetsAt: null, windowDurationMinutes: null } } }
    window.cefQuery = request => request.onSuccess(JSON.stringify(data))
    const host = document.createElement('div'), root = createRoot(host)
    try {
      await act(async () => root.render(<UsageLimits executionHostId="local" />))
      const copilot = host.querySelector('[aria-label="Copilot limits"]')!
      expect(copilot.textContent).toContain('0.4% used')
      expect(copilot.querySelector('[role="meter"]')?.getAttribute('aria-valuenow')).toBe('0.4')
    } finally { act(() => root.unmount()) }
  })

  it('preserves all model allowances with only the first two expanded and no invented reset or period', async () => {
    const windows = [
      { id: 'model-a:requests', label: 'Model A requests', usedPercent: 10, resetsAt: now / 1000 + 3600, windowDurationMinutes: null },
      { id: 'model-b:requests', label: 'Model B requests', usedPercent: 20, resetsAt: null, windowDurationMinutes: null },
      { id: 'model-c:tokens', label: 'Model C tokens', usedPercent: 30, resetsAt: null, windowDurationMinutes: null },
      { id: 'model-d:requests', label: 'Model D requests', usedPercent: 0, resetsAt: null, windowDurationMinutes: null },
    ]
    const data = snapshot()
    window.cefQuery = request => request.onSuccess(JSON.stringify({ ...data, providers: [{ ...data.providers[0], providerId: 'gemini-cli', windows }] }))
    const host = document.createElement('div'), root = createRoot(host)
    try {
      await act(async () => root.render(<UsageLimits executionHostId="local" />))
      const gemini = host.querySelector('[aria-label="Gemini limits"]')!
      expect(gemini.textContent).toContain('Model A requests')
      expect(gemini.textContent).toContain('Model B requests')
      expect(gemini.textContent).not.toContain('Primary window')
      expect(gemini.textContent).not.toContain('5 hour window')
      expect(gemini.textContent).toContain('Reset time unavailable')
      const extra = gemini.querySelector('details')!
      expect(extra.open).toBe(false)
      expect(extra.querySelector('summary')?.textContent).toBe('2 more model allowances')
      expect(extra.querySelectorAll('[role="meter"]')).toHaveLength(2)
      act(() => extra.querySelector('summary')!.click())
      expect(extra.open).toBe(true)
      expect(extra.textContent).toContain('Model C tokens')
      expect(extra.textContent).toContain('Model D requests')
      expect(extra.textContent).toContain('0% used')
    } finally { act(() => root.unmount()) }
  })

  it.each([['41049.6267850000', '41,049.63'], ['unknown balance', 'unknown balance']])('formats credit balance %s and retains its exact provider value', async (raw, formatted) => {
    const data = snapshot()
    data.providers[0].limits!.credits = { hasCredits: true, unlimited: false, balance: raw }
    window.cefQuery = request => request.onSuccess(JSON.stringify(data))
    const host = document.createElement('div'), root = createRoot(host)
    try {
      await act(async () => root.render(<UsageLimits executionHostId="local" />))
      const credit = host.querySelector(`[title="${raw}"]`)
      expect(credit?.textContent).toBe(`Credits: ${formatted}`)
    } finally { act(() => root.unmount()) }
  })

  it('distinguishes a fresh provider-reported past reset from a cached snapshot awaiting update', async () => {
    vi.useFakeTimers(); vi.setSystemTime(now)
    const view = await renderLimits(allowance(now / 1000 - 60))
    try {
      const codex = view.host.querySelector('[aria-label="Codex limits"]')!
      expect(codex.textContent).toContain('Provider-reported reset time has passed')
      expect(codex.textContent).not.toContain('awaiting update')
      expect(codex.textContent).toContain('Not reported by provider')
      await act(async () => vi.advanceTimersByTimeAsync(300000))
      expect(codex.textContent).toContain('Reset passed, awaiting update')
    } finally { view.close() }
  })

  it('hides made-up windows for unavailable providers while preserving initial loading unknowns', async () => {
    const data = snapshot()
    data.providers[0] = { ...data.providers[0], status: 'error', limits: null, error: 'Account allowances are not supported by this connection.' }
    let pending!: Parameters<NonNullable<typeof window.cefQuery>>[0]
    window.cefQuery = request => { pending = request }
    const host = document.createElement('div'), root = createRoot(host)
    try {
      await act(async () => root.render(<UsageLimits executionHostId="local" />))
      const codex = host.querySelector('[aria-label="Codex limits"]')!
      expect(codex.textContent).toContain('Primary window')
      expect(codex.textContent).toContain('Not readable at this time')
      await act(async () => pending.onSuccess(JSON.stringify(data)))
      expect(codex.textContent).toContain('Account allowances are not supported')
      expect(codex.textContent).not.toContain('Primary window')
      expect(codex.textContent).not.toContain('Secondary window')
      expect(codex.textContent).not.toContain('Not readable at this time')
    } finally { act(() => root.unmount()) }
  })

  it('explains the refresh cooldown and enables promptly after a cache-only check at its expiry', async () => {
    vi.useFakeTimers(); vi.setSystemTime(now)
    const requests: { refresh?: boolean }[] = []
    window.cefQuery = request => {
      requests.push(JSON.parse(request.request).payload)
      request.onSuccess(JSON.stringify({ ...snapshot(), refreshAfterMs: requests.length === 1 ? 5000 : 0 }))
    }
    const host = document.createElement('div'), root = createRoot(host)
    try {
      await act(async () => root.render(<UsageLimits executionHostId="local" />))
      const button = host.querySelector('button')!
      expect(button.disabled).toBe(true)
      expect(host.textContent).toContain('Updated moments ago; refresh available shortly')
      await act(async () => vi.advanceTimersByTimeAsync(4999))
      expect(requests).toHaveLength(1)
      expect(button.disabled).toBe(true)
      await act(async () => vi.advanceTimersByTimeAsync(1))
      expect(requests).toHaveLength(2)
      expect(requests[1].refresh).toBeUndefined()
      expect(button.disabled).toBe(false)
      expect(host.textContent).not.toContain('refresh available shortly')
      await act(async () => button.click())
      expect(requests[2].refresh).toBe(true)
    } finally { act(() => root.unmount()) }
  })

  it('shows the 5-hour period as unlimited when a Pro plan reports no secondary window', async () => {
    const view = await renderLimits({ updatedAt: now, limitId: '', limitName: '', planType: 'prolite', primary: { usedPercent: 100, resetsAt: now / 1000 + 6 * 86400, windowDurationMinutes: 10080 }, secondary: null, credits: null, individualLimit: null, spendControlReached: null, rateLimitReachedType: '' })
    try {
      expect(view.host.textContent).toContain('5-hour')
      expect(view.host.textContent).toContain('Unlimited')
      expect(view.host.textContent).not.toContain('Not reported by provider')
    } finally { view.close() }
  })
})


describe('UsageLimits observed native consumption', () => {
  it.each([['antigravity-cli', true], ['codex-cli', false]] as const)('labels the reported scope for %s without inventing native history', async (providerId, observedOnly) => {
    const previous = useAppStore.getState()
    const previousQuery = window.cefQuery
    vi.spyOn(Date, 'now').mockReturnValue(now)
    useAppStore.setState({
      sessions: [{ id: 'observed-native', name: 'Observed native chat', providerId, executionHostId: 'local' }] as typeof previous.sessions,
      acpBindingBySessionId: { 'observed-native': { providerId, providerUsage: {
        tokenUsage: { updatedAt: now, total: { inputTokens: 25, cachedInputTokens: 0, cacheWriteInputTokens: 0, outputTokens: 4, reasoningOutputTokens: 0, totalTokens: 29 }, last: null, modelContextWindow: null },
        contextUsage: null, rateLimits: null,
      } } } as typeof previous.acpBindingBySessionId,
    })
    window.cefQuery = ({ onSuccess }) => onSuccess(JSON.stringify({ executionHostId: 'local', refreshing: false, providers: [] }))
    const host = document.createElement('div'), root = createRoot(host)
    try {
      await act(async () => root.render(<UsageLimits executionHostId="local" />))
      const prefix = observedOnly ? 'Tokens observed while attached to' : 'Tokens consumed in'
      const other = observedOnly ? 'Tokens consumed in' : 'Tokens observed while attached to'
      expect(host.textContent).toContain(`${prefix} Observed native chat: 29`)
      expect(host.textContent).not.toContain(`${other} Observed native chat`)
      const tokenDetails = host.querySelector<HTMLDetailsElement>(`[aria-label="${observedOnly ? 'Antigravity' : 'Codex'} limits"] details.uam-limits-tokens`)
      expect(tokenDetails).not.toBeNull()
      expect(tokenDetails!.open).toBe(false)
      expect(tokenDetails!.querySelector('summary')?.textContent).toContain('Context and tokens')
      const facts = Object.fromEntries([...tokenDetails!.querySelectorAll('dt')].map(term => [term.textContent?.trim(), term.nextElementSibling?.textContent?.trim()]))
      expect(facts).toMatchObject({ Input: '25', Output: '4', 'Last turn': 'Unavailable' })
      expect(host.textContent).toContain('Account allowances unavailable.')
    } finally {
      act(() => root.unmount()); useAppStore.setState(previous, true)
      if (previousQuery) window.cefQuery = previousQuery; else delete window.cefQuery
      vi.restoreAllMocks()
    }
  })
})
