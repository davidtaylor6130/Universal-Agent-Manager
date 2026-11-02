import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { useAppStore } from '../../store/useAppStore'
import type { AcpRateLimits } from '../../store/cpp/types'
import { UsageLimits, type LimitsUsage } from './UsageLimits'
import { allowancePace } from './allowancePace'
import { EMPTY_USAGE_TOTALS, type UsageProvider, type UsageProviderId } from '../../services/usageHistory'
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
      expect(view.host.textContent).toContain('resets in 1h 0m')
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
      const meters = known.host.querySelectorAll('.uam-limit-details [role="meter"]')
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
      expect(view.host.textContent).toContain('resets in 1m')
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
      expect(view.host.querySelector('[aria-label="Codex"]')?.textContent).toContain('Updated just now')
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
      expect(host.querySelector('[aria-label="Codex"]')?.textContent).toContain('25% used')
      // One card, one row per provider: no per-provider sections or machine headings.
      expect(host.querySelectorAll('.uam-set-card')).toHaveLength(1)
      expect(host.querySelectorAll('.uam-limit-entry')).toHaveLength(6)
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
      const codex = host.querySelector('[aria-label="Codex"]')
      expect(codex?.textContent).toContain('25% used')
      expect(codex?.textContent).not.toContain('99% used')
      expect(host.querySelector('[aria-label="Claude"]')?.textContent).toContain('57% used')
      const copilot = host.querySelector('[aria-label="Copilot"]')
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
      expect(host.querySelector('[aria-label="Codex"]')?.textContent).toContain('25% used')
      await act(async () => refresh.onFailure(503, 'Account refresh unavailable'))
      expect(host.querySelector('[aria-label="Codex"]')?.textContent).toContain('25% used')
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
      expect(host.querySelector('[aria-label="Codex"]')?.textContent).toContain('70% used')
      await act(async () => pending[0].onSuccess(JSON.stringify(snapshot('first-host', 10))))
      expect(host.querySelector('[aria-label="Codex"]')?.textContent).toContain('70% used')
      expect(host.querySelector('[aria-label="Codex"]')?.textContent).not.toContain('10% used')
    } finally { act(() => root.unmount()); useAppStore.setState(previous, true) }
  })

  it('labels remote account reads unavailable and does not offer a nonworking refresh control', async () => {
    const previous = useAppStore.getState()
    useAppStore.setState({ sessions: [], acpBindingBySessionId: {} })
    window.cefQuery = request => request.onSuccess(JSON.stringify({ executionHostId: 'remote', refreshing: false, providers: [] }))
    const host = document.createElement('div'), root = createRoot(host)
    try {
      await act(async () => root.render(<UsageLimits executionHostId="remote" />))
      expect(host.querySelector('.uam-limits-lede')?.textContent).toContain('Allowances are read on this computer only.')
      expect(host.textContent).not.toContain('Refresh allowances')
      // The unsupported remote feed is explained once, not as a lone not-available line on every row.
      expect(host.querySelector('[aria-label="Codex"] .uam-limit-summary')?.textContent).not.toContain('Not')
    } finally { act(() => root.unmount()); useAppStore.setState(previous, true) }
  })

  it('shows fractional account usage without rounding a reported Copilot fraction to zero', async () => {
    const data = snapshot()
    data.providers[0] = { ...data.providers[0], providerId: 'copilot-cli', primaryLabel: 'Chat allowance', limits: { ...allowance(now / 1000 + 3600), primary: { usedPercent: 0.4, resetsAt: null, windowDurationMinutes: null } } }
    window.cefQuery = request => request.onSuccess(JSON.stringify(data))
    const host = document.createElement('div'), root = createRoot(host)
    try {
      await act(async () => root.render(<UsageLimits executionHostId="local" />))
      const copilot = host.querySelector('[aria-label="Copilot"]')!
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
      const gemini = host.querySelector('[aria-label="Gemini"]')!
      expect(gemini.textContent).toContain('Model A requests')
      expect(gemini.textContent).toContain('Model B requests')
      expect(gemini.textContent).not.toContain('Primary window')
      expect(gemini.textContent).not.toContain('5 hour window')
      expect(gemini.textContent).toContain('Reset time unavailable')
      const extra = gemini.querySelector<HTMLElement>('.uam-limits-more')!
      const extraToggle = extra.querySelector('button')!
      expect(extraToggle.getAttribute('aria-expanded')).toBe('false')
      expect(extraToggle.textContent).toBe('2 more model allowances')
      expect(extra.querySelectorAll('[role="meter"]')).toHaveLength(2)
      act(() => extraToggle.click())
      expect(extraToggle.getAttribute('aria-expanded')).toBe('true')
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
      const codex = view.host.querySelector('[aria-label="Codex"]')!
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
      const codex = host.querySelector('[aria-label="Codex"]')!
      // While reading, the unknown row is the same greyed row with an ellipsis, not a made-up window or a percentage.
      expect(codex.querySelectorAll('.uam-limit-row')).toHaveLength(1)
      expect(codex.textContent).toContain('Allowance · Reading…')
      expect(codex.textContent).toContain('…')
      expect(codex.textContent).not.toContain('??%')
      expect(codex.textContent).not.toContain('Primary window')
      await act(async () => pending.onSuccess(JSON.stringify(data)))
      expect(codex.textContent).toContain('Account allowances are not supported')
      expect(codex.textContent).toContain('??%')
      expect(codex.textContent).not.toContain('Primary window')
      expect(codex.textContent).not.toContain('Secondary window')
      expect(codex.textContent).not.toContain('Reading')
    } finally { act(() => root.unmount()) }
  })

  it('shows an unsupported allowance feed as neutral not-available text and keeps failed reads as warnings', async () => {
    const data = snapshot()
    data.providers = [
      { providerId: 'opencode-cli', scope: 'local:opencode', status: 'unavailable', limits: null, error: 'Account allowance is not available from this provider connection.' },
      { providerId: 'antigravity-cli', scope: 'local:antigravity', status: 'error', limits: null, error: 'Could not read account allowance.' },
    ]
    window.cefQuery = request => request.onSuccess(JSON.stringify(data))
    const host = document.createElement('div'), root = createRoot(host)
    try {
      await act(async () => root.render(<UsageLimits executionHostId="local" />))
      const opencode = host.querySelector('[aria-label="OpenCode"]')!
      const unsupported = Array.from(opencode.querySelectorAll('p')).find(p => p.textContent === 'Account allowance is not available from this provider connection.')!
      expect(unsupported.className).toContain('uam-usage-muted')
      expect(unsupported.className).not.toContain('uam-limits-notice')
      expect(opencode.querySelectorAll('[role="meter"]')).toHaveLength(0)
      const failed = Array.from(host.querySelector('[aria-label="Antigravity"]')!.querySelectorAll('p')).find(p => p.textContent === 'Could not read account allowance.')!
      expect(failed.className).toContain('uam-limits-notice')
    } finally { act(() => root.unmount()) }
  })

  it('does not show a provider disabled on the selected computer, like a build-excluded provider', async () => {
    const previous = useAppStore.getState()
    useAppStore.setState({ sessions: [], acpBindingBySessionId: {}, providerAvailability: [{ providerId: 'opencode-cli', executionHostId: 'local', enabled: false }, { providerId: 'antigravity-cli', executionHostId: 'lab', enabled: false }] })
    let reads = 0
    window.cefQuery = request => { reads += 1; request.onSuccess(JSON.stringify(snapshot())) }
    const host = document.createElement('div'), root = createRoot(host)
    try {
      await act(async () => root.render(<UsageLimits executionHostId="local" />))
      expect(host.querySelector('[aria-label="OpenCode"]')).toBeNull()
      // Re-enabling reconfigures the native reader, so the page reads again instead of waiting for the next poll.
      const before = reads
      await act(async () => useAppStore.setState({ providerAvailability: [{ providerId: 'antigravity-cli', executionHostId: 'lab', enabled: false }] }))
      expect(host.querySelector('[aria-label="OpenCode"]')).not.toBeNull()
      expect(reads).toBeGreaterThan(before)
      await act(async () => useAppStore.setState({ providerAvailability: [{ providerId: 'opencode-cli', executionHostId: 'local', enabled: false }, { providerId: 'antigravity-cli', executionHostId: 'lab', enabled: false }] }))
      expect(host.querySelector('[aria-label="Antigravity"]')).not.toBeNull()
      expect(host.textContent).not.toContain('No provider is turned on')
      act(() => useAppStore.setState({ providerAvailability: ['codex-cli', 'claude-cli', 'gemini-cli', 'opencode-cli', 'copilot-cli', 'antigravity-cli'].map(providerId => ({ providerId, executionHostId: 'local', enabled: false })) }))
      expect(host.querySelector('[aria-label$=" limits"]')).toBeNull()
      expect(host.textContent).toContain('No provider is turned on for this computer.')
      expect(host.textContent).not.toContain('Refresh allowances')
    } finally { act(() => root.unmount()); useAppStore.setState(previous, true) }
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
      const tokenDetails = host.querySelector<HTMLElement>(`[aria-label="${observedOnly ? 'Antigravity' : 'Codex'}"] .uam-limits-tokens`)
      expect(tokenDetails).not.toBeNull()
      const tokenToggle = tokenDetails!.querySelector('button')!
      expect(tokenToggle.getAttribute('aria-expanded')).toBe('false')
      expect(tokenToggle.textContent).toContain('Latest chat context and tokens')
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

describe('UsageLimits overview', () => {
  afterEach(() => { delete window.cefQuery; vi.useRealTimers(); vi.restoreAllMocks() })
  const dayTime = new Intl.DateTimeFormat(undefined, { weekday: 'short', hour: 'numeric', minute: '2-digit' })
  const tokens = (providerId: UsageProviderId, totalTokens: number, available = true): UsageProvider => ({ ...EMPTY_USAGE_TOTALS, providerId, totalTokens, inputTokens: totalTokens, records: totalTokens ? 3 : 0, sessions: totalTokens ? 2 : 0, available, files: 1, errors: 0, invalidLines: 0, oversizedLines: 0, suppressedForkRecords: 0, pendingLines: 0 })
  const usage = (providers: UsageProvider[]): LimitsUsage => ({ providers, loading: false, collecting: false, error: '', days: 7 })
  async function render(providers: ProviderAllowanceSnapshot['providers'], history?: LimitsUsage, host = 'local') {
    const previous = useAppStore.getState()
    useAppStore.setState({ sessions: [], acpBindingBySessionId: {} })
    window.cefQuery = request => request.onSuccess(JSON.stringify({ executionHostId: JSON.parse(request.request).payload.executionHostId, refreshing: false, providers }))
    const element = document.createElement('div'), root = createRoot(element)
    await act(async () => root.render(<UsageLimits executionHostId={host} usage={history} />))
    return { host: element, close: () => { act(() => root.unmount()); useAppStore.setState(previous, true) } }
  }

  it('still shows each provider usefully when no allowance is readable (the seeded demo state)', async () => {
    const failed = 'The provider could not return account allowances; check its existing sign-in and CLI compatibility.'
    const keychain = 'No existing account credential was found in the login keychain.'
    const view = await render([
      { providerId: 'codex-cli', scope: 'local:codex', status: 'error', limits: null, error: failed },
      { providerId: 'claude-cli', scope: 'local:claude', status: 'error', limits: null, error: 'No existing Claude Code subscription login is available.' },
      { providerId: 'gemini-cli', scope: 'local:gemini', status: 'error', limits: null, error: 'No existing Gemini CLI subscription login is available.' },
      { providerId: 'opencode-cli', scope: 'local:opencode', status: 'unavailable', limits: null, error: 'Account allowance is not available from this provider connection.' },
      { providerId: 'copilot-cli', scope: 'local:copilot', status: 'error', limits: null, error: failed },
      { providerId: 'antigravity-cli', scope: 'local:antigravity', status: 'error', limits: null, error: keychain },
    ], usage([tokens('codex-cli', 1_200_000), tokens('claude-cli', 340_000), tokens('gemini-cli', 0), tokens('opencode-cli', 5000), tokens('copilot-cli', 0, false), tokens('antigravity-cli', 800)]))
    try {
      const lede = view.host.querySelector('.uam-limits-lede')!.textContent
      expect(lede).toContain('No allowances to show.')
      expect(lede).toContain('1.5M tokens used in the past 7 days.')
      const summary = (name: string) => view.host.querySelector(`[aria-label="${name}"] .uam-limit-summary`)!.textContent
      expect(summary('Codex')).toContain('1.2M tokens')
      expect(summary('Codex')).toContain("Couldn't read")
      expect(summary('Claude')).toContain('Not signed in')
      expect(summary('Gemini')).toContain('No usage')
      expect(summary('OpenCode')).toContain('Not offered by OpenCode')
      expect(summary('Copilot')).toContain('No history')
      expect(summary('Antigravity')).toContain('Not signed in')
      // Every unreadable row is the greyed allowance row: ??% with no fill, and an accessible statement of why.
      expect(summary('Codex')).toContain('??%')
      expect(summary('Codex')).toContain('Allowance unknown: Couldn\'t read')
      expect(summary('OpenCode')).toContain('Allowance unknown: Not offered by OpenCode')
      expect(view.host.querySelectorAll('.uam-limit-summary .uam-status-dot')).toHaveLength(0)
      expect(view.host.querySelectorAll('.uam-limit-summary .uam-limit-row__value')).toHaveLength(6)
      expect(view.host.querySelectorAll('.uam-limit-summary .uam-limit-row__value')[0].textContent).toBe('??%')
      // Short reasons on the row; every native message is kept whole in the row tooltip and details.
      expect(summary('Codex')).not.toContain(failed)
      expect(view.host.querySelector('[aria-label="Codex"] .uam-limit-summary__side > small')?.getAttribute('aria-hidden')).toBe('true')
      expect(view.host.querySelector('[aria-label="Codex"]')!.textContent).toContain(failed)
      expect(view.host.querySelector('[aria-label="Antigravity"]')!.textContent).toContain(keychain)
      expect(view.host.querySelectorAll('[role="meter"]')).toHaveLength(0)
      expect(view.host.textContent).not.toContain('This computer')
    } finally { view.close() }
  })

  it('leads with the highest use when every window has room, and keeps every window as its own meter', async () => {
    vi.useFakeTimers(); vi.setSystemTime(now)
    const limits = (primary: number, secondary: number): AcpRateLimits => ({ ...allowance(now / 1000 + 7200), planType: 'plus', primary: { usedPercent: primary, resetsAt: now / 1000 + 7200, windowDurationMinutes: 300 }, secondary: { usedPercent: secondary, resetsAt: now / 1000 + 3 * 86400, windowDurationMinutes: 10080 } })
    const view = await render([
      { providerId: 'codex-cli', scope: 'local:codex', status: 'ready', limits: limits(30, 50), error: '' },
      { providerId: 'claude-cli', scope: 'local:claude', status: 'ready', limits: limits(12, 40), error: '' },
    ], usage([tokens('codex-cli', 2_000_000), tokens('claude-cli', 10_000)]))
    try {
      expect(view.host.querySelector('.uam-limits-lede')!.textContent).toBe(`Allowances have room. Highest is Codex at 50% of its weekly window, resets ${dayTime.format(now + 3 * 86400000)}. 2M tokens used in the past 7 days.`)
      const codex = view.host.querySelector<HTMLElement>('[aria-label="Codex"]')!
      expect(codex.querySelector('.uam-limit-summary')!.textContent).toContain('plus')
      expect(codex.querySelector('.uam-limit-summary')!.textContent).toContain('Weekly window · plenty left')
      expect(codex.querySelectorAll('.uam-limit-details [role="meter"]')).toHaveLength(2)
      // The compact bar sits inside the row's button, so it is decorative there; the expanded meters carry the values.
      expect(codex.querySelector('.uam-limit-summary .uam-limit-bar')!.getAttribute('aria-hidden')).toBe('true')
    } finally { view.close() }
  })

  it('shows pace on every window with mock allowances: ahead, behind, near the limit, unknown length, per model, unreadable and reading', async () => {
    vi.useFakeTimers(); vi.setSystemTime(now)
    const sec = now / 1000, hour = 3_600_000, day = 86_400_000
    const base = allowance(sec)
    const view = await render([
      // Weekly window 3 of 7 days in at 70% used: ahead, runs out in about 31 hours, four days before its reset.
      { providerId: 'codex-cli', scope: 'local:codex', status: 'ready', error: '', limits: { ...base, planType: 'plus', primary: { usedPercent: 10, resetsAt: sec + 4 * 3600, windowDurationMinutes: 300 }, secondary: { usedPercent: 70, resetsAt: sec + 4 * 86400, windowDurationMinutes: 10080 } } },
      // 5-hour window 90% through at 97% used: nearly out; weekly window well under pace.
      { providerId: 'claude-cli', scope: 'local:claude', status: 'ready', error: '', limits: { ...base, primary: { usedPercent: 97, resetsAt: sec + 1800, windowDurationMinutes: 300 }, secondary: { usedPercent: 20, resetsAt: sec + 2 * 86400, windowDurationMinutes: 10080 } } },
      // Per-model windows: one daily window on pace, one with no window length, one with neither length nor reset.
      { providerId: 'gemini-cli', scope: 'local:gemini', status: 'ready', error: '', limits: null, windows: [
        { id: 'pro', label: 'Pro requests', usedPercent: 50, resetsAt: sec + 12 * 3600, windowDurationMinutes: 1440 },
        { id: 'flash', label: 'Flash requests', usedPercent: 30, resetsAt: sec + 3 * 3600, windowDurationMinutes: null },
        { id: 'lite', label: 'Lite requests', usedPercent: 85, resetsAt: null, windowDurationMinutes: null },
      ] },
      // Monthly allowance with no reported length: a bar and reset, but no tick and no verdict.
      { providerId: 'copilot-cli', scope: 'local:copilot', status: 'ready', error: '', primaryLabel: 'Chat allowance', limits: { ...base, primary: { usedPercent: 40, resetsAt: sec + 10 * 86400, windowDurationMinutes: null }, secondary: null } },
      { providerId: 'opencode-cli', scope: 'local:opencode', status: 'unavailable', limits: null, error: 'Account allowance is not available from this provider connection.' },
      { providerId: 'antigravity-cli', scope: 'local:antigravity', status: 'loading', limits: null, error: '' },
    ])
    try {
      const row = (name: string) => view.host.querySelector<HTMLElement>(`[aria-label="${name}"]`)!
      const summary = (name: string) => row(name).querySelector('.uam-limit-summary')!.textContent
      const meter = (name: string, label: string) => row(name).querySelector<HTMLElement>(`.uam-limit-details [role="meter"][aria-label="${label}"]`)!
      const tick = (element: HTMLElement) => element.querySelector<HTMLElement>('.uam-meter__marker')?.style.getPropertyValue('--marker')
      const codexOut = now - 3 * day + 3 * day * 100 / 70

      // The lede names the provider projected to run out first: Claude's 5-hour window, in about 8 minutes.
      expect(view.host.querySelector('.uam-limits-lede')!.textContent).toBe('Claude will run out in ~8m, before its 5 hour window resets in 30m.')

      // Codex: the compact row prefers the weekly window that runs out before its reset over the emptier 5-hour one.
      expect(summary('Codex')).toContain(`Weekly window · runs out ~${dayTime.format(codexOut)}70% used`)
      const weekly = meter('Codex', 'Codex secondary usage')
      expect(weekly.getAttribute('aria-valuenow')).toBe('70')
      expect(weekly.dataset.tone).toBe('warning')
      expect(Number(tick(weekly))).toBeCloseTo(3 / 7)
      expect(weekly.getAttribute('aria-valuetext')).toBe(`70% used · 43% of the window has passed · Ahead of pace · runs out ~${dayTime.format(codexOut)} · resets ${dayTime.format(now + 4 * day)}`)
      expect(row('Codex').querySelector('.uam-limit-details')!.textContent).toContain('Plenty left · resets in 4h 0m')
      const fiveHour = meter('Codex', 'Codex primary usage')
      expect(fiveHour.dataset.tone).toBeUndefined()
      expect(Number(tick(fiveHour))).toBeCloseTo(0.2)

      // Claude: at 97% the bar and percent turn to the error tone; the weekly window is calm.
      const nearLimit = meter('Claude', 'Claude primary usage')
      expect(nearLimit.dataset.tone).toBe('error')
      expect(summary('Claude')).toContain('5 hour window · runs out in ~8m97% used')
      expect(row('Claude').querySelector<HTMLElement>('.uam-limit-summary .uam-limit-row__value')!.style.color).toBe('var(--error)')
      expect(row('Claude').querySelector('.uam-limit-details')!.textContent).toContain(`Plenty left · resets ${dayTime.format(now + 2 * day)}`)
      expect(meter('Claude', 'Claude secondary usage').dataset.tone).toBeUndefined()

      // Gemini model windows: a daily window on pace with a tick at half way; unknown lengths have no tick or verdict.
      const pro = meter('Gemini', 'Gemini Pro requests usage')
      expect(Number(tick(pro))).toBeCloseTo(0.5)
      expect(row('Gemini').querySelector('.uam-limit-details')!.textContent).toContain('Pro requestsOn pace · resets in 12h 0m50% used')
      expect(tick(meter('Gemini', 'Gemini Flash requests usage'))).toBeUndefined()
      expect(row('Gemini').querySelector('.uam-limit-details')!.textContent).toContain('Flash requestsResets in 3h 0m30% used')
      // With no pace to judge by, 85% still reaches the warning tone from the 80% threshold.
      expect(summary('Gemini')).toContain('Lite requests · reset time unavailable85% used')
      expect(meter('Gemini', 'Gemini Lite requests usage').dataset.tone).toBe('warning')
      expect(tick(meter('Gemini', 'Gemini Lite requests usage'))).toBeUndefined()

      // Copilot: a known reset but no window length, so a bar without a tick.
      expect(tick(meter('Copilot', 'Copilot primary usage'))).toBeUndefined()
      expect(summary('Copilot')).toContain(`Chat allowance · resets ${dayTime.format(now + 10 * day)}40% used`)

      // OpenCode: the same bar greyed and empty, ??%, short reason, no meter role.
      expect(summary('OpenCode')).toContain('Allowance · Not offered by OpenCode??%')
      const empty = row('OpenCode').querySelector<HTMLElement>('.uam-limit-summary .uam-meter')!
      expect(empty.hasAttribute('role')).toBe(false)
      expect(empty.dataset.indeterminate).toBeUndefined()
      // Antigravity is still reading: the empty bar sweeps and the percent is an ellipsis.
      expect(summary('Antigravity')).toContain('Allowance · Reading……')
      expect(row('Antigravity').querySelector<HTMLElement>('.uam-limit-summary .uam-meter')!.dataset.indeterminate).toBe('true')
    } finally { view.close() }
  })

  it('names a used-up window in plain words with the error tone', async () => {
    vi.useFakeTimers(); vi.setSystemTime(now)
    const view = await render([{ providerId: 'codex-cli', scope: 'local:codex', status: 'ready', error: '', limits: { ...allowance(now / 1000), primary: { usedPercent: 100, resetsAt: now / 1000 + 5400, windowDurationMinutes: 300 }, secondary: null } }])
    try {
      expect(view.host.querySelector('.uam-limits-lede')!.textContent).toBe('Codex is close to its limit. 100% of its 5 hour window used, resets in 1h 30m.')
      expect(view.host.querySelector('[aria-label="Codex"] .uam-limit-summary')!.textContent).toContain('5 hour window · used up · resets in 1h 30m100% used')
      expect(view.host.querySelector<HTMLElement>('[aria-label="Codex"] .uam-limit-details [role="meter"]')!.dataset.tone).toBe('error')
    } finally { view.close() }
  })

  it('leaves a turned-off provider out of rows, summary and token totals', async () => {
    const previous = useAppStore.getState().providerAvailability
    useAppStore.setState({ providerAvailability: [{ providerId: 'codex-cli', executionHostId: 'local', enabled: false }] })
    const view = await render([{ providerId: 'claude-cli', scope: 'local:claude', status: 'unavailable', limits: null, error: '' }], usage([tokens('codex-cli', 9_000_000), tokens('claude-cli', 1000)]))
    try {
      expect(view.host.querySelector('[aria-label="Codex"]')).toBeNull()
      expect(view.host.textContent).not.toContain('Codex')
      expect(view.host.querySelector('.uam-limits-lede')!.textContent).toContain('1K tokens used')
    } finally { view.close(); useAppStore.setState({ providerAvailability: previous }) }
  })

  it('names a remote computer once and shows its tokens without per-row not-available lines', async () => {
    const previous = useAppStore.getState().executionHosts
    useAppStore.setState({ executionHosts: [{ id: 'lab', label: 'Lab PC' }] as typeof previous })
    const view = await render(['codex-cli', 'claude-cli'].map(providerId => ({ providerId: providerId as UsageProviderId, scope: 'remote', status: 'unavailable' as const, limits: null, error: 'Independent account allowances are not available on this remote connection.' })), usage([tokens('codex-cli', 4000)]), 'lab')
    try {
      expect(view.host.querySelector('.uam-limits-lede')!.textContent).toBe('Allowances are read on this computer only. 4K tokens used in the past 7 days on Lab PC.')
      // The row is still the greyed allowance row, but the reason is not repeated per row; its accessible text says where it is read.
      const side = view.host.querySelector('[aria-label="Codex"] .uam-limit-summary__side')!
      expect(side.textContent).toBe('Allowance??%Allowance unknown: read on this computer only')
      expect(view.host.querySelector('[aria-label="Codex"]')!.textContent).toContain('Independent account allowances are not available on this remote connection.')
    } finally { view.close(); useAppStore.setState({ executionHosts: previous }) }
  })

  it('keeps a known window name on the unknown row and uses "Allowance" only when none is known', async () => {
    const view = await render([
      { providerId: 'copilot-cli', scope: 'local:copilot', status: 'error', limits: null, error: 'Could not read account allowance.', primaryLabel: 'Chat allowance' },
      { providerId: 'gemini-cli', scope: 'local:gemini', status: 'error', limits: null, error: 'Could not read account allowance.' },
    ], usage([tokens('copilot-cli', 0, true), tokens('gemini-cli', 0, true)]))
    try {
      expect(view.host.querySelector('[aria-label="Copilot"] .uam-limit-summary')!.textContent).toContain('Chat allowance · Couldn\'t read??%')
      const gemini = view.host.querySelector('[aria-label="Gemini"]')!
      expect(gemini.querySelector('.uam-limit-summary')!.textContent).toContain('Allowance · Couldn\'t read??%')
      expect(gemini.textContent).not.toContain('Primary window')
    } finally { view.close() }
  })
})

describe('allowancePace', () => {
  const start = now - 2.5 * 3_600_000
  const fiveHour = (usedPercent: number, resetsAt: number | null = (start + 5 * 3_600_000) / 1000, windowDurationMinutes: number | null = 300) => ({ usedPercent, resetsAt, windowDurationMinutes })

  it('projects a straight-line burn from the window start and reports when it runs out before the reset', () => {
    const pace = allowancePace(fiveHour(75), now)
    expect(pace.verdict).toBe('ahead')
    expect(pace.elapsedPercent).toBe(50)
    expect(pace.runsOutAt).toBe(start + 2.5 * 3_600_000 * 100 / 75)
    expect(pace.runsOutAt!).toBeLessThan(start + 5 * 3_600_000)
  })

  it('calls use within the margin on pace and well below it plenty left, with no run-out time', () => {
    expect(allowancePace(fiveHour(52), now)).toEqual({ verdict: 'on', elapsedPercent: 50, runsOutAt: null })
    expect(allowancePace(fiveHour(30), now)).toEqual({ verdict: 'under', elapsedPercent: 50, runsOutAt: null })
  })

  it('treats 0% used as under pace once time has passed, never as running out', () => {
    expect(allowancePace(fiveHour(0), now)).toEqual({ verdict: 'under', elapsedPercent: 50, runsOutAt: null })
  })

  it('does not project at the very start of a window, or when the reset is further away than the window length', () => {
    expect(allowancePace(fiveHour(10, now / 1000 + 5 * 3600), now)).toEqual({ verdict: 'unknown', elapsedPercent: 0, runsOutAt: null })
    expect(allowancePace(fiveHour(10, now / 1000 + 9 * 3600), now)).toEqual({ verdict: 'unknown', elapsedPercent: 0, runsOutAt: null })
  })

  it('has no pace or tick when the reset has passed, or the reset or window length is unknown', () => {
    const none = { verdict: 'unknown', elapsedPercent: null, runsOutAt: null }
    expect(allowancePace(fiveHour(60, now / 1000 - 60), now)).toEqual(none)
    expect(allowancePace(fiveHour(60, now / 1000), now)).toEqual(none)
    expect(allowancePace(fiveHour(60, null), now)).toEqual(none)
    expect(allowancePace(fiveHour(60, undefined, null), now)).toEqual(none)
    expect(allowancePace(fiveHour(60, undefined, 0), now)).toEqual(none)
  })

  it('reports a used-up window as out whatever its timing, and ignores invalid use', () => {
    expect(allowancePace(fiveHour(100), now).verdict).toBe('out')
    expect(allowancePace(fiveHour(100, null, null), now).verdict).toBe('out')
    expect(allowancePace(fiveHour(NaN), now).verdict).toBe('unknown')
    expect(allowancePace(fiveHour(-1), now).verdict).toBe('unknown')
  })
})
