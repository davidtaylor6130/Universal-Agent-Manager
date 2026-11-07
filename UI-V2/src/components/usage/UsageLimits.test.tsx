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
      expect(view.host.querySelector('.uam-limit-row__reset')?.textContent).toBe('Resets in 1h')
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
      const meters = known.host.querySelectorAll('.uam-limit-card [role="meter"]')
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
      // One always-open card per provider and no machine headings.
      expect(host.querySelectorAll('.uam-limit-card')).toHaveLength(6)
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
      expect(host.querySelector('.uam-limits-lede__title')?.textContent).toBe('Allowances are read on this computer only')
      expect(host.textContent).not.toContain('Refresh allowances')
      // The unsupported remote feed is explained once, not as a not-available reason in every card header or row.
      expect(host.querySelector('[aria-label="Codex"] .uam-limit-card__head')?.textContent).not.toContain('Not')
      expect(host.querySelector('[aria-label="Codex"] .uam-limit-row')?.textContent).toBe('AllowanceResets —??%Allowance unknown: read on this computer only')
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
      expect(codex.querySelector('.uam-limit-row')!.textContent).toBe('AllowanceReading……Reading Allowance')
      expect(codex.querySelector<HTMLElement>('.uam-meter')!.dataset.indeterminate).toBe('true')
      expect(codex.classList.contains('uam-limit-card--off')).toBe(false)
      expect(codex.textContent).not.toContain('??%')
      expect(codex.textContent).not.toContain('Primary window')
      await act(async () => pending.onSuccess(JSON.stringify(data)))
      // Once read and unreadable, the card joins the greyed group at the end.
      const read = host.querySelector('[aria-label="Codex"]')!
      expect(read.classList.contains('uam-limit-card--off')).toBe(true)
      expect(read.textContent).toContain('Account allowances are not supported')
      expect(read.textContent).toContain('??%')
      expect(read.textContent).not.toContain('Primary window')
      expect(read.textContent).not.toContain('Secondary window')
      expect(read.textContent).not.toContain('Reading')
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
      expect(opencode.querySelector('.uam-limit-card__facts .uam-usage-muted')?.textContent).toBe('Account allowance is not available from this provider connection.')
      expect(opencode.querySelector('.uam-limit-card__facts .uam-limits-notice')).toBeNull()
      expect(opencode.querySelectorAll('[role="meter"]')).toHaveLength(0)
      const failed = host.querySelector('[aria-label="Antigravity"] .uam-limit-card__facts .uam-limits-notice')!
      expect(failed.textContent).toBe('Could not read account allowance.')
      expect(failed.querySelector('.uam-status-dot--warning')).not.toBeNull()
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
      expect(tokenToggle.textContent).toBe('Context and tokens29')
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
  const weekday = new Intl.DateTimeFormat(undefined, { weekday: 'short' })
  const clock = new Intl.DateTimeFormat(undefined, { hour: 'numeric', minute: '2-digit' })
  const dateTime = new Intl.DateTimeFormat(undefined, { day: 'numeric', month: 'short', hour: 'numeric', minute: '2-digit' })
  const date = new Intl.DateTimeFormat(undefined, { day: 'numeric', month: 'short' })
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
  const card = (host: HTMLElement, name: string) => host.querySelector<HTMLElement>(`[aria-label="${name}"]`)!
  const rowOf = (host: HTMLElement, name: string, label: string) => [...card(host, name).querySelectorAll<HTMLElement>('.uam-limit-row')].find(row => row.querySelector('.uam-limit-row__name')?.textContent === label)!
  const tick = (element: HTMLElement) => element.querySelector<HTMLElement>('.uam-meter__marker')?.style.getPropertyValue('--marker')

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
      expect(view.host.querySelector('.uam-limits-lede__title')!.textContent).toBe('No allowances to show')
      expect(view.host.querySelector('.uam-limits-lede__aside')!.textContent).toBe('1.5Mtokens in the past 7 days')
      // Every provider is unreadable: all six greyed cards share the two-up group, with no key and no meters.
      expect(view.host.querySelectorAll('.uam-limits-off > .uam-limit-card--off')).toHaveLength(6)
      expect(view.host.querySelector('.uam-limits-key')).toBeNull()
      expect(view.host.querySelectorAll('[role="meter"]')).toHaveLength(0)
      const meta = (name: string) => card(view.host, name).querySelector('.uam-limit-card__meta')!.textContent
      expect(meta('Codex')).toBe("1.2M tokensCouldn't read")
      expect(meta('Claude')).toBe('340K tokensNot signed in')
      expect(meta('Gemini')).toBe('No usageNot signed in')
      expect(meta('OpenCode')).toBe('5K tokensNot offered by OpenCode')
      expect(meta('Copilot')).toBe("No historyCouldn't read")
      expect(meta('Antigravity')).toBe('800 tokensNot signed in')
      // Each card keeps the greyed allowance row and names the reason once, in its header; the row says the reset is unknown.
      expect(rowOf(view.host, 'Codex', 'Allowance').textContent).toBe("AllowanceResets —??%Allowance unknown: Couldn't read")
      expect(rowOf(view.host, 'OpenCode', 'Allowance').textContent).toContain('Allowance unknown: Not offered by OpenCode')
      // The full native message is the card's help line; only failed reads carry the warning mark.
      expect(card(view.host, 'Codex').querySelector('.uam-limit-card__facts')!.textContent).toBe(failed)
      expect(card(view.host, 'Antigravity').querySelector('.uam-limit-card__facts')!.textContent).toBe(keychain)
      expect(view.host.querySelectorAll('.uam-limit-card__facts .uam-status-dot--warning')).toHaveLength(5)
      expect(view.host.textContent).not.toContain('This computer')
    } finally { view.close() }
  })

  it('renders the approved mockup: callout ordering, ahead, on pace and plenty left, 95%, weekly notches, run-out carets, per model, spend, credits and greyed cards', async () => {
    vi.useFakeTimers(); vi.setSystemTime(now)
    const sec = now / 1000, minute = 60_000, hour = 3_600_000, day = 86_400_000
    const base = allowance(sec)
    const view = await render([
      // Codex: a 5-hour window 60% through at 38% (plenty left) and a weekly window 2 of 7 days in at 60% (ahead).
      { providerId: 'codex-cli', scope: 'local:codex', status: 'ready', error: '', limits: { ...base, planType: 'plus', credits: { hasCredits: false, unlimited: false, balance: null },
        primary: { usedPercent: 38, resetsAt: sec + 2 * 3600, windowDurationMinutes: 300 }, secondary: { usedPercent: 60, resetsAt: sec + 5 * 86400, windowDurationMinutes: 10080 } } },
      // Claude: a 5-hour window with 47 minutes left at 96% (ahead and past 95%); its weekly window 5 of 7 days in at 40%.
      { providerId: 'claude-cli', scope: 'local:claude', status: 'ready', error: '', limits: { ...base, planType: 'max 5x',
        primary: { usedPercent: 96, resetsAt: sec + 47 * 60, windowDurationMinutes: 300 }, secondary: { usedPercent: 40, resetsAt: sec + 2 * 86400, windowDurationMinutes: 10080 } } },
      // Gemini: four daily model buckets; Pro 15 of 24 hours in at 85% is ahead.
      { providerId: 'gemini-cli', scope: 'local:gemini', status: 'ready', error: '', limits: null, windows: [
        { id: 'pro', label: 'Pro daily', usedPercent: 85, resetsAt: sec + 9 * 3600, windowDurationMinutes: 1440 },
        { id: 'flash', label: 'Flash daily', usedPercent: 20, resetsAt: sec + 9 * 3600, windowDurationMinutes: 1440 },
        { id: 'lite', label: 'Lite requests', usedPercent: 85, resetsAt: null, windowDurationMinutes: null },
        { id: 'image', label: 'Image requests', usedPercent: 0, resetsAt: null, windowDurationMinutes: null },
      ] },
      { providerId: 'opencode-cli', scope: 'local:opencode', status: 'unavailable', limits: null, error: 'Account allowance is not available from this provider connection.' },
      // Copilot: premium requests 18 of 30 days in at 57% (on pace), and a spend limit.
      { providerId: 'copilot-cli', scope: 'local:copilot', status: 'ready', error: '', primaryLabel: 'Premium requests', limits: { ...base, planType: 'pro', individualLimit: { used: '$2.40', limit: '$10.00', remainingPercent: 76, resetsAt: 0 },
        primary: { usedPercent: 57, resetsAt: sec + 12 * 86400, windowDurationMinutes: 43200 }, secondary: null } },
      { providerId: 'antigravity-cli', scope: 'local:antigravity', status: 'error', limits: null, error: 'No existing Antigravity login is available.' },
    ], usage([tokens('codex-cli', 2_100_000), tokens('claude-cli', 1_900_000), tokens('gemini-cli', 1_900_000), tokens('opencode-cli', 1_600_000), tokens('copilot-cli', 2_300_000), tokens('antigravity-cli', 0)]))
    try {
      const claudeOut = now - 253 * minute + 253 * minute * 100 / 96
      const geminiOut = now - 15 * hour + 15 * hour * 100 / 85
      const codexOut = now - 2 * day + 2 * day * 100 / 60

      // Callout: the highest rate needed with allowance left (Claude's 5-hour would need more but has only 4% left), with the period token total aside.
      const lede = view.host.querySelector<HTMLElement>('.uam-limits-lede')!
      expect(lede.dataset.tone).toBeUndefined()
      expect(lede.querySelector('.uam-limits-lede__title')!.textContent).toBe('Use Codex first')
      expect(lede.querySelector('.uam-limits-lede__body')!.textContent).toBe('It has 62% of its 5-hour window left, resetting in 2h.')
      expect(lede.querySelector('.uam-limits-lede__more')).toBeNull()
      expect(lede.querySelector('.uam-limits-lede__aside')!.textContent).toBe('9.8Mtokens in the past 7 days')

      // Section label with Refresh, then the key.
      expect(view.host.querySelector('.uam-limits-group')!.textContent).toBe('Allowances · read on this computerUpdated just nowRefresh allowances')
      expect(view.host.querySelector('.uam-limits-key')!.textContent).toBe('UsedTime passed in the windowRun-out at this pace')

      // Fixed provider order for readable cards; unreadable cards last, two-up; a disabled provider would be absent.
      const cards = [...view.host.querySelectorAll<HTMLElement>('.uam-limits-cards .uam-limit-card')].map(element => element.getAttribute('aria-label'))
      expect(cards).toEqual(['Codex', 'Claude', 'Gemini', 'Copilot', 'OpenCode', 'Antigravity'])
      expect([...view.host.querySelectorAll('.uam-limits-off > .uam-limit-card--off')].map(element => element.getAttribute('aria-label'))).toEqual(['OpenCode', 'Antigravity'])

      // Codex header: plan chip, tokens and freshness. Rows: plenty left, then weekly ahead with notches and a caret.
      expect(card(view.host, 'Codex').querySelector('.uam-limit-card__head')!.textContent).toBe('Codexplus2.1M tokensUpdated just now')
      const codexFive = rowOf(view.host, 'Codex', '5-hour')
      expect(codexFive.textContent).toBe('5-hourUse firstResets in 2h38% usedPlenty left')
      expect(Number(tick(codexFive))).toBeCloseTo(0.6)
      expect(codexFive.querySelector('.uam-meter__segments')).toBeNull()
      expect(codexFive.querySelector('.uam-meter__projection')).toBeNull()
      const codexWeek = rowOf(view.host, 'Codex', 'Weekly')
      const weekMeter = codexWeek.querySelector<HTMLElement>('[role="meter"]')!
      expect(weekMeter.getAttribute('aria-label')).toBe('Codex secondary usage')
      expect(weekMeter.dataset.tone).toBe('warning')
      expect(weekMeter.classList.contains('uam-meter--lg')).toBe(true)
      expect(Number(tick(codexWeek))).toBeCloseTo(2 / 7)
      expect(weekMeter.querySelector<HTMLElement>('.uam-meter__segments')!.style.getPropertyValue('--segments')).toBe('7')
      expect(weekMeter.getAttribute('aria-valuetext')).toBe(`60% used · 29% of the window has passed · Ahead of pace · would run out ~${dayTime.format(codexOut)} (in ~32h) · resets ${dayTime.format(now + 5 * day)}`)
      const weekCaret = weekMeter.querySelector<HTMLElement>('.uam-meter__projection')!
      expect(weekCaret.textContent).toBe(`~${dayTime.format(codexOut)} at this pace`)
      expect(Number(weekCaret.style.getPropertyValue('--at'))).toBeCloseTo(2 / 7 * 100 / 60)
      expect(weekCaret.dataset.flip).toBeUndefined()
      expect(weekCaret.dataset.tone).toBe('warning')
      expect(codexWeek.querySelector('.uam-limit-row__bar')!.classList.contains('is-projected')).toBe(true)
      expect(codexWeek.querySelector('.uam-limit-row__value')!.textContent).toBe(`60% usedAhead · would run out ~${weekday.format(codexOut)}`)
      expect(codexWeek.querySelector<HTMLElement>('.uam-limit-row__pct')!.style.color).toBe('var(--warning-text)')
      expect(card(view.host, 'Codex').querySelector('.uam-limit-card__facts')!.textContent).toBe('Credits: None')

      // Claude: at 96% the bar, percent and caret take the error tone; the caret label flips left late in the track.
      const claudeFive = rowOf(view.host, 'Claude', '5-hour')
      expect(claudeFive.querySelector<HTMLElement>('[role="meter"]')!.dataset.tone).toBe('error')
      expect(claudeFive.querySelector<HTMLElement>('.uam-limit-row__pct')!.style.color).toBe('var(--error-text)')
      expect(claudeFive.querySelector('.uam-limit-row__value')!.textContent).toBe('96% usedAhead · would run out in ~11m')
      const claudeCaret = claudeFive.querySelector<HTMLElement>('.uam-meter__projection')!
      expect(claudeCaret.textContent).toBe(`~${clock.format(claudeOut)} at this pace`)
      expect(claudeCaret.dataset.flip).toBe('true')
      expect(claudeCaret.dataset.tone).toBe('error')
      const claudeWeek = rowOf(view.host, 'Claude', 'Weekly')
      expect(claudeWeek.textContent).toBe(`WeeklyResets ${dayTime.format(now + 2 * day)}40% usedPlenty left`)
      expect(claudeWeek.querySelectorAll('.uam-meter__segments')).toHaveLength(1)
      expect(claudeWeek.querySelector<HTMLElement>('[role="meter"]')!.dataset.tone).toBeUndefined()
      expect(card(view.host, 'Claude').querySelector('.uam-limits-chip')!.textContent).toBe('max 5x')

      // Gemini: the first two model buckets show, the rest wait behind the disclosure.
      const gemini = card(view.host, 'Gemini')
      const pro = rowOf(view.host, 'Gemini', 'Pro daily')
      expect(Number(tick(pro))).toBeCloseTo(15 / 24)
      expect(pro.querySelector('.uam-meter__segments')).toBeNull()
      expect(pro.querySelector('.uam-meter__projection')!.textContent).toBe(`~${clock.format(geminiOut)} at this pace`)
      expect(rowOf(view.host, 'Gemini', 'Flash daily').textContent).toBe('Flash dailyResets in 9h20% usedPlenty left')
      const more = gemini.querySelector('.uam-limits-more')!
      const moreToggle = more.querySelector('button')!
      expect(moreToggle.textContent).toBe('2 more model allowances')
      expect(moreToggle.getAttribute('aria-expanded')).toBe('false')
      act(() => moreToggle.click())
      expect(moreToggle.getAttribute('aria-expanded')).toBe('true')
      // With no pace to judge by, 85% still reaches the warning tone from the 80% threshold; no tick and no verdict.
      const lite = rowOf(view.host, 'Gemini', 'Lite requests')
      expect(lite.textContent).toBe('Lite requestsReset time unavailable85% used')
      expect(lite.querySelector<HTMLElement>('[role="meter"]')!.dataset.tone).toBe('warning')
      expect(tick(lite)).toBeUndefined()
      expect(rowOf(view.host, 'Gemini', 'Image requests').textContent).toContain('0% used')

      // Copilot: on pace (57% with 60% passed), and a spend-limit bar with no tick.
      expect(rowOf(view.host, 'Copilot', 'Premium requests').textContent).toBe(`Premium requestsResets ${dateTime.format(now + 12 * day)}57% usedOn pace`)
      const spend = rowOf(view.host, 'Copilot', 'Spend limit')
      expect(spend.textContent).toBe('Spend limit$2.40 of $10.0024% used')
      expect(spend.querySelector('[role="meter"]')!.getAttribute('aria-label')).toBe('Copilot spend limit usage')
      expect(tick(spend)).toBeUndefined()

      // Greyed cards: no plan or freshness, the reason in the header, the same row empty with ??%, the native message as help.
      const opencode = card(view.host, 'OpenCode')
      expect(opencode.querySelector('.uam-limit-card__head')!.textContent).toBe('OpenCode1.6M tokensNot offered by OpenCode')
      expect(opencode.querySelector('.uam-limit-row')!.textContent).toBe('AllowanceResets —??%Allowance unknown: Not offered by OpenCode')
      expect(opencode.querySelector('[role="meter"]')).toBeNull()
      expect(opencode.querySelector<HTMLElement>('.uam-meter')!.dataset.indeterminate).toBeUndefined()
      expect(opencode.querySelector('.uam-limit-card__facts')!.textContent).toBe('Account allowance is not available from this provider connection.')
      expect(card(view.host, 'Antigravity').querySelector('.uam-limit-card__meta')!.textContent).toBe('No usageNot signed in')
    } finally { view.close() }
  })

  it('dates a reset weeks away and states the row projection as conditional', async () => {
    // The 7 Nov report: a 30-day window 9d 13h 26m in at 80% and a weekly window 5d 18m in at 66%.
    vi.useFakeTimers(); vi.setSystemTime(now)
    const sec = now / 1000, minute = 60_000
    const codexElapsed = (9 * 1440 + 13 * 60 + 26) * minute, codexStart = now - codexElapsed, codexReset = codexStart + 30 * 86_400_000
    const codexOut = codexStart + codexElapsed * 100 / 80
    const claudeReset = now + (2 * 1440 - 18) * minute
    const view = await render([
      { providerId: 'codex-cli', scope: 'local:codex', status: 'ready', error: '', limits: { ...allowance(sec), planType: 'free', primary: { usedPercent: 80, resetsAt: codexReset / 1000, windowDurationMinutes: 43200 } } },
      { providerId: 'claude-cli', scope: 'local:claude', status: 'ready', error: '', limits: { ...allowance(sec), primary: null, secondary: { usedPercent: 66, resetsAt: claudeReset / 1000, windowDurationMinutes: 10080 } } },
    ])
    try {
      const lede = view.host.querySelector<HTMLElement>('.uam-limits-lede')!
      expect(lede.querySelector('.uam-limits-lede__title')!.textContent).toBe('Use Claude first')
      expect(lede.querySelector('.uam-limits-lede__body')!.textContent).toBe(`It has 34% of its weekly window left, resetting ${dayTime.format(claudeReset)} (in ~47h).`)
      expect(lede.querySelector('.uam-limits-lede__more')).toBeNull()
      const row = rowOf(view.host, 'Codex', '30-day')
      expect(row.querySelector('.uam-limit-row__reset')!.textContent).toBe(`Resets ${dateTime.format(codexReset)}`)
      expect(row.querySelector('.uam-meter__projection')!.textContent).toBe(`~${dayTime.format(codexOut)} at this pace`)
      expect(rowOf(view.host, 'Claude', 'Weekly').textContent).toBe(`WeeklyUse firstResets ${dayTime.format(claudeReset)}66% usedPlenty left`)
    } finally { view.close() }
  })

  it('switches from relative to weekday at exactly 24 hours and from weekday to date at exactly 7 days', async () => {
    vi.useFakeTimers(); vi.setSystemTime(now)
    const sec = now / 1000, minute = 60_000, day = 86_400_000
    const shown = async (offset: number) => {
      // Resets at the offset, and separately a 50% window that many milliseconds in, which runs out at the offset.
      const view = await render([
        { providerId: 'claude-cli', scope: 'local:claude', status: 'ready', error: '', limits: { ...allowance(sec), primary: { usedPercent: 1, resetsAt: (now + offset) / 1000, windowDurationMinutes: 43200 } } },
        { providerId: 'codex-cli', scope: 'local:codex', status: 'ready', error: '', limits: { ...allowance(sec), primary: { usedPercent: 50, resetsAt: (now - offset + 30 * day) / 1000, windowDurationMinutes: 43200 } } },
      ])
      const row = rowOf(view.host, 'Codex', '30-day')
      const result = [rowOf(view.host, 'Claude', '30-day').querySelector('.uam-limit-row__reset')!.textContent, row.querySelector('.uam-limit-verdict')!.textContent, row.querySelector('.uam-meter__projection')!.textContent]
      view.close(); clearProviderAllowanceCache()
      return result
    }
    const under24 = day - minute, under7 = 7 * day - minute
    expect(await shown(under24)).toEqual(['Resets in 23h 59m', 'Ahead · would run out in ~23h', `~${clock.format(now + under24)} at this pace`])
    expect(await shown(day)).toEqual([`Resets ${dayTime.format(now + day)}`, `Ahead · would run out ~${weekday.format(now + day)}`, `~${dayTime.format(now + day)} at this pace`])
    expect(await shown(under7)).toEqual([`Resets ${dayTime.format(now + under7)}`, `Ahead · would run out ~${weekday.format(now + under7)}`, `~${dayTime.format(now + under7)} at this pace`])
    expect(await shown(7 * day)).toEqual([`Resets ${dateTime.format(now + 7 * day)}`, `Ahead · would run out ~${date.format(now + 7 * day)}`, `~${dateTime.format(now + 7 * day)} at this pace`])
  })

  it('keeps projected spans rough: minutes only under 3 hours, days rounded down', async () => {
    vi.useFakeTimers(); vi.setSystemTime(now)
    const sec = now / 1000, minute = 60_000, hour = 3_600_000, day = 86_400_000
    const spoken = async (offset: number) => {
      const view = await render([{ providerId: 'codex-cli', scope: 'local:codex', status: 'ready', error: '', limits: { ...allowance(sec), primary: { usedPercent: 50, resetsAt: (now - offset + 30 * day) / 1000, windowDurationMinutes: 43200 } } }])
      const text = view.host.querySelector('[role="meter"]')!.getAttribute('aria-valuetext')!
      view.close(); clearProviderAllowanceCache()
      return text
    }
    expect(await spoken(2 * hour + 39 * minute)).toContain('would run out in ~2h 39m ·')
    expect(await spoken(3 * hour + 5 * minute)).toContain('would run out in ~3h ·')
    expect(await spoken(32 * hour + 5 * minute)).toContain(`~${dayTime.format(now + 32 * hour + 5 * minute)} (in ~32h) ·`)
    expect(await spoken(60 * hour)).toContain(`~${dayTime.format(now + 60 * hour)} (in ~2d) ·`)
  })

  describe('which allowance to use first', () => {
    const sec = now / 1000, hour = 3600, day = 86400
    const win = (usedPercent: number, resetsIn: number, windowDurationMinutes: number | null) => ({ usedPercent, resetsAt: sec + resetsIn, windowDurationMinutes })
    const ready = (providerId: UsageProviderId, limits: Partial<AcpRateLimits>, extra = {}): ProviderAllowanceSnapshot['providers'][number] => ({ providerId, scope: `local:${providerId}`, status: 'ready', error: '', limits: { ...allowance(sec), primary: null, ...limits }, ...extra })
    const callout = async (providers: ProviderAllowanceSnapshot['providers']) => {
      const view = await render(providers)
      const lede = view.host.querySelector('.uam-limits-lede')!
      const shown = { title: lede.querySelector('.uam-limits-lede__title')!.textContent, body: lede.querySelector('.uam-limits-lede__body')?.textContent, more: [...lede.querySelectorAll('.uam-limits-lede__more li')].map(li => li.textContent), tone: lede.getAttribute('data-tone') }
      view.close(); clearProviderAllowanceCache()
      return shown
    }
    beforeEach(() => { vi.useFakeTimers(); vi.setSystemTime(now) })

    it('(a) ranks 34% left in 46 hours above 20% left in 490 hours', async () => {
      const shown = await callout([
        ready('codex-cli', { primary: win(80, 490 * hour, 43200) }),
        ready('claude-cli', { secondary: win(66, 46 * hour, 10080) }, { primaryLabel: '5-hour allowance', secondaryLabel: 'Weekly allowance' }),
      ])
      expect(shown.title).toBe('Use Claude first')
      expect(shown.body).toBe(`It has 34% of its weekly allowance left, resetting ${dayTime.format(now + 46 * 3600000)} (in ~46h).`)
    })
    it('(b) ranks 50% left in 5 hours above 80% left in 3 days, naming the one chosen', async () => {
      const shown = await callout([ready('claude-cli', { primary: win(50, 5 * hour, 300), secondary: win(20, 3 * day, 10080) })])
      expect(shown.title).toBe('Use Claude first')
      expect(shown.body).toBe('It has 50% of its 5-hour window left, resetting in 5h.')
    })
    it('(c) ranks by the rate needed, not by the most left or the soonest reset', async () => {
      const fiveHours = ready('claude-cli', { primary: win(50, 5 * hour, 300) })
      // 90% left in 10 hours needs 9% an hour, under the 10% an hour of 50% left in 5 hours, despite having more left.
      expect((await callout([ready('codex-cli', { primary: win(10, 10 * hour, 600) }), fiveHours])).title).toBe('Use Claude first')
      // 12% left in 2 hours needs 6% an hour, under 10% an hour, despite resetting sooner.
      expect((await callout([ready('codex-cli', { primary: win(88, 2 * hour, 300) }), fiveHours])).title).toBe('Use Claude first')
      // 5% left in 10 minutes needs 30% an hour, yet is under the minimum left.
      expect((await callout([ready('codex-cli', { primary: win(95, 600, 300) }), fiveHours])).title).toBe('Use Claude first')
      // A higher rate wins over a later reset.
      expect((await callout([ready('codex-cli', { primary: win(50, 5 * hour, 300) }), ready('claude-cli', { primary: win(80, 2 * hour, 300) }) ])).title).toBe('Use Codex first')
    })
    it('counts exactly the minimum left as usable and just under it as not', async () => {
      const other = ready('claude-cli', { primary: win(50, 6 * hour, 300) })
      expect((await callout([ready('codex-cli', { primary: win(90, hour, 300) }), other])).title).toBe('Use Codex first')
      expect((await callout([ready('codex-cli', { primary: win(90.1, hour, 300) }), other])).title).toBe('Use Claude first')
    })
    it('(d) recommends no window of a provider with a used-up window, and keeps the used-up window visible', async () => {
      const claude = ready('claude-cli', { primary: win(20, hour, 300), secondary: win(100, 2 * day, 10080) })
      const shown = await callout([claude, ready('codex-cli', { primary: win(40, 3 * day, 10080) })])
      expect(shown.title).toBe('Use Codex first')
      expect(shown.more).toEqual([`Claude · Weekly is used up, resets ${dayTime.format(now + 2 * 86400000)}`])
      expect(shown.tone).toBe('error')
      const alone = await callout([claude])
      expect(alone.title).toBe('Claude is used up')
      expect(`${alone.title} ${alone.body}`).not.toMatch(/\bfirst\b/i)
    })
    it('(e) claims nothing should be used first when no window is eligible', async () => {
      const none = async (providers: ProviderAllowanceSnapshot['providers']) => { const shown = await callout(providers); expect(`${shown.title} ${shown.body}`).not.toMatch(/\bfirst\b/i); return shown.title }
      expect(await none([ready('codex-cli', { primary: win(95, hour, 300) })])).toBe('Codex is close to its limit')
      // No known future reset: nothing would be lost by waiting, so only the room text is true.
      expect(await none([ready('codex-cli', { primary: { usedPercent: 50, resetsAt: null, windowDurationMinutes: null } })])).toBe('Allowances have room')
      // A reset exactly now has passed, as one earlier has.
      expect(await none([ready('codex-cli', { primary: win(50, 0, 300) })])).toBe('Allowances are awaiting an update')
      expect(await none([ready('codex-cli', { primary: win(50, -hour, 300) })])).toBe('Allowances are awaiting an update')
    })
    it('lets a time window that has run low cap the others on its provider', async () => {
      const claude = (weekly: number) => ready('claude-cli', { primary: win(20, hour, 300), secondary: win(weekly, 2 * day, 10080) }, { primaryLabel: '5-hour allowance', secondaryLabel: 'Weekly allowance' })
      const codex = ready('codex-cli', { primary: win(60, 3 * day, 10080) })
      for (const weekly of [90.1, 99, 99.96]) expect((await callout([claude(weekly), codex])).title).toBe('Use Codex first')
      expect((await callout([claude(99)])).title).toBe('Claude is close to its limit')
      // Exactly the minimum left does not cap: the 5-hour window resets first.
      expect((await callout([claude(90), codex])).body).toBe('It has 80% of its 5-hour allowance left, resetting in 1h.')
    })
    it('does not let a used-up quota stop the provider\'s independent quotas', async () => {
      const gemini = await callout([ready('gemini-cli', { updatedAt: now }, { windows: [
        { id: 'pro', label: 'gemini-2.5-pro · REQUESTS', usedPercent: 100, resetsAt: sec + hour, windowDurationMinutes: null },
        { id: 'flash', label: 'gemini-2.5-flash · REQUESTS', usedPercent: 20, resetsAt: sec + 12 * hour, windowDurationMinutes: null },
      ] })])
      expect(gemini.title).toBe('Use Gemini first')
      expect(gemini.body).toBe('It has 80% of its gemini-2.5-flash · REQUESTS left, resetting in 12h.')
      expect(gemini.more).toEqual(['Gemini · gemini-2.5-pro · REQUESTS is used up, resets in 1h'])
      const copilot = await callout([ready('copilot-cli', { primary: win(100, hour, null), secondary: win(10, 3 * day, null) }, { primaryLabel: 'Chat allowance', secondaryLabel: 'Completions allowance' })])
      expect(copilot.title).toBe('Use Copilot first')
      expect(copilot.body).toContain('It has 90% of its completions allowance left')
    })
    it('names the bucket when a model bucket is used up or close to its limit', async () => {
      const bucket = async (usedPercent: number) => (await callout([ready('gemini-cli', {}, { windows: [{ id: 'pro', label: 'gemini-2.5-pro · REQUESTS', usedPercent, resetsAt: sec + hour, windowDurationMinutes: null }] })])).title
      expect(await bucket(100)).toBe('Gemini is used up on gemini-2.5-pro · REQUESTS')
      expect(await bucket(95)).toBe('Gemini is close to its limit on gemini-2.5-pro · REQUESTS')
    })
    it('does not treat a window whose reset has passed as used up or close to its limit', async () => {
      const shown = await callout([ready('claude-cli', { primary: win(20, hour, 300), secondary: win(100, -hour, 10080) })])
      expect(shown.title).toBe('Use Claude first')
      expect(shown.more).toEqual([])
      expect((await callout([ready('codex-cli', { primary: win(85, -hour, 43200) })])).title).toBe('Allowances are awaiting an update')
      expect((await callout([ready('codex-cli', { primary: win(100, -hour, 43200) })])).title).toBe('Allowances are awaiting an update')
    })
    it('headlines a provider\'s own stop, and lists it when another provider is recommended', async () => {
      const codex = (stop: Partial<AcpRateLimits>) => ready('codex-cli', { primary: win(20, hour, 300), ...stop })
      const spend = await callout([codex({ spendControlReached: true })])
      expect(spend.title).toBe('Codex has reached its spend limit')
      expect(spend.tone).toBe('error')
      expect(spend.more).toEqual([])
      expect((await callout([codex({ rateLimitReachedType: 'workspace_member_usage_limit_reached' })])).title).toBe('Codex has reached its usage limit')
      const other = await callout([codex({ rateLimitReachedType: 'workspace_member_usage_limit_reached' }), ready('claude-cli', { primary: win(50, 5 * hour, 300) })])
      expect(other.title).toBe('Use Claude first')
      expect(other.more).toEqual(['Codex has reached its usage limit'])
      expect(other.tone).toBe('error')
      // A used-up window on the stopped provider is not hidden by the stop in the headline.
      expect((await callout([codex({ spendControlReached: true, secondary: win(100, day, 10080) })])).more).toEqual([`Codex · Weekly is used up, resets ${dayTime.format(now + 86400000)}`])
    })
    it('recommends only from a current snapshot', async () => {
      const soon = { primary: win(20, hour, 300) }
      const other = ready('claude-cli', { primary: win(50, 5 * hour, 300) })
      const title = async (codex: ProviderAllowanceSnapshot['providers'][number]) => (await callout([codex, other])).title
      expect(await title(ready('codex-cli', { ...soon, updatedAt: now - 359_000 }))).toBe('Use Codex first')
      expect(await title(ready('codex-cli', { ...soon, updatedAt: now - 360_000 }))).toBe('Use Claude first')
      // Loading with no read seen yet, as at launch from the saved cache, is not known to be good.
      expect(await title({ ...ready('codex-cli', soon), status: 'loading' })).toBe('Use Claude first')
      expect(await title(ready('codex-cli', { ...soon, updatedAt: now + 60_000 }))).toBe('Use Claude first')
      expect(await title({ ...ready('codex-cli', soon), status: 'error', error: 'Timed out' })).toBe('Use Claude first')
      expect(await title({ ...ready('codex-cli', soon), status: 'unavailable' })).toBe('Use Claude first')
    })
    it('does not let a failed read flip back to recommended while it reads again', async () => {
      const providers = (status: 'ready' | 'error' | 'loading') => [{ ...ready('codex-cli', { primary: win(20, hour, 300) }), status, error: status === 'error' ? 'Timed out' : '' }]
      const view = await render(providers('error'))
      const title = () => view.host.querySelector('.uam-limits-lede__title')!.textContent
      try {
        expect(title()).not.toMatch(/first/)
        // The native service clears the error and reports loading on each retry.
        window.cefQuery = request => request.onSuccess(JSON.stringify({ executionHostId: 'local', refreshing: true, providers: providers('loading') }))
        await act(async () => { await vi.advanceTimersByTimeAsync(31_000) })
        expect(view.host.querySelector('.uam-limit-card__meta')!.textContent).not.toContain('Timed out')
        expect(view.host.querySelector('.uam-limits-lede__title')!.textContent).not.toMatch(/first/)
      } finally { view.close() }
    })
    it('holds the recommendation through an ordinary re-read, and drops it past the grace period', async () => {
      const providers = (status: 'ready' | 'loading') => [{ ...ready('codex-cli', { primary: win(20, 2 * hour, 300) }), status }]
      const view = await render(providers('ready'))
      const title = () => view.host.querySelector('.uam-limits-lede__title')!.textContent
      try {
        expect(title()).toBe('Use Codex first')
        // Past 5 minutes, before the next poll has started a read.
        await act(async () => { await vi.advanceTimersByTimeAsync(301_000) })
        expect(view.host.querySelector('.uam-limit-card__meta')!.textContent).toContain('Cached snapshot')
        expect(title()).toBe('Use Codex first')
        window.cefQuery = request => request.onSuccess(JSON.stringify({ executionHostId: 'local', refreshing: true, providers: providers('loading') }))
        await act(async () => { await vi.advanceTimersByTimeAsync(30_000) })
        expect(title()).toBe('Use Codex first')
        await act(async () => { await vi.advanceTimersByTimeAsync(30_000) })
        expect(title()).not.toMatch(/first/)
      } finally { view.close() }
    })
    it('breaks a tie on the rate needed by the most left', async () => {
      // 20% left in 2 hours and 50% left in 5 hours both need 10% an hour.
      expect((await callout([ready('codex-cli', { primary: win(80, 2 * hour, 300) }), ready('claude-cli', { primary: win(50, 5 * hour, 300) })])).title).toBe('Use Claude first')
    })
    it('lists at most two windows that cannot be spent, and not the one in the headline', async () => {
      const shown = await callout([ready('codex-cli', { primary: win(100, hour, 300) }), ready('claude-cli', { primary: win(100, hour, 300) }), ready('copilot-cli', { primary: win(100, hour, null) }), ready('antigravity-cli', { primary: win(50, day, 1440) })])
      expect(shown.title).toBe('Use Antigravity first')
      expect(shown.more).toHaveLength(2)
      expect((await callout([ready('codex-cli', { primary: win(100, hour, 300) })])).more).toEqual([])
    })
    it('shows the countdown icon only with a recommendation', async () => {
      const icon = async (usedPercent: number) => {
        const view = await render([ready('codex-cli', { primary: win(usedPercent, hour, 300) })])
        const found = [view.host.querySelector('.uam-limits-lede__icon .lucide-timer') !== null, view.host.querySelector('.uam-limits-lede__icon .lucide-gauge') !== null]
        view.close(); clearProviderAllowanceCache()
        return found
      }
      expect(await icon(50)).toEqual([true, false])
      expect(await icon(95)).toEqual([false, true])
    })
    it('marks the recommended row once and keeps it in view when it sorts past the first two buckets', async () => {
      const bucket = (id: string, resetsIn: number) => ({ id, label: `${id} · REQUESTS`, usedPercent: 20, resetsAt: sec + resetsIn, windowDurationMinutes: null })
      const view = await render([ready('gemini-cli', {}, { windows: [bucket('a', 9 * hour), bucket('b', 9 * hour), bucket('c', 9 * hour), bucket('d', hour)] })])
      try {
        const marked = [...view.host.querySelectorAll('.uam-limit-row')].filter(row => row.querySelector('.uam-limit-row__marker'))
        expect(marked).toHaveLength(1)
        expect(marked[0].querySelector('.uam-limit-row__name')!.textContent).toBe('d · REQUESTS')
        expect(marked[0].querySelector('.uam-limit-row__marker')!.textContent).toBe('Use first')
        expect(marked[0].closest('.uam-limits-more')).toBeNull()
        expect(view.host.querySelector('.uam-limits-more')!.textContent).toContain('1 more model allowance')
        expect(view.host.querySelector('.uam-limits-more')!.textContent).toContain('c · REQUESTS')
      } finally { view.close() }
      const none = await render([ready('codex-cli', { primary: win(95, hour, 300) })])
      try { expect(none.host.querySelector('.uam-limit-row__marker')).toBeNull() } finally { none.close() }
    })
    it('keeps a window ahead of pace on its row when it resets weeks away', async () => {
      const view = await render([ready('codex-cli', { primary: win(80, 20 * day, 43200) }), ready('claude-cli', { secondary: win(66, 2 * day, 10080) })])
      try {
        expect(view.host.querySelector('.uam-limits-lede__title')!.textContent).toBe('Use Claude first')
        expect(rowOf(view.host, 'Codex', '30-day').querySelector('.uam-limit-verdict')!.textContent).toContain('Ahead · would run out')
        expect(rowOf(view.host, 'Codex', '30-day').querySelector('.uam-meter__projection')!.textContent).toContain('at this pace')
      } finally { view.close() }
    })
    it('reads as a sentence with provider-declared names', async () => {
      const copilot = await callout([ready('copilot-cli', { primary: win(80, 20 * day, 43200) }, { primaryLabel: 'Premium requests' })])
      expect(copilot.title).toBe('Use Copilot first')
      expect(copilot.body).toBe(`It has 20% of its Premium requests left, resetting ${dateTime.format(now + 20 * 86400000)} (in ~20d).`)
    })
  })

  it('keeps a calm callout with no list when no window has a known reset', async () => {
    vi.useFakeTimers(); vi.setSystemTime(now)
    const limits = (primary: number, secondary: number): AcpRateLimits => ({ ...allowance(now / 1000 + 7200), planType: 'plus', primary: { usedPercent: primary, resetsAt: null, windowDurationMinutes: 300 }, secondary: { usedPercent: secondary, resetsAt: null, windowDurationMinutes: 10080 } })
    const view = await render([
      { providerId: 'codex-cli', scope: 'local:codex', status: 'ready', limits: limits(30, 50), error: '' },
      { providerId: 'claude-cli', scope: 'local:claude', status: 'ready', limits: limits(12, 40), error: '' },
    ], usage([tokens('codex-cli', 2_000_000), tokens('claude-cli', 10_000)]))
    try {
      const lede = view.host.querySelector<HTMLElement>('.uam-limits-lede')!
      expect(lede.querySelector('.uam-limits-lede__title')!.textContent).toBe('Allowances have room')
      expect(lede.querySelector('.uam-limits-lede__body')!.textContent).toBe('Highest is Codex at 50% of its weekly window.')
      expect(lede.querySelector('.uam-limits-lede__more')).toBeNull()
      expect(lede.dataset.tone).toBeUndefined()
      expect(card(view.host, 'Codex').querySelectorAll('[role="meter"]')).toHaveLength(2)
    } finally { view.close() }
  })

  it('names a used-up window in plain words with the error tone', async () => {
    vi.useFakeTimers(); vi.setSystemTime(now)
    const view = await render([{ providerId: 'codex-cli', scope: 'local:codex', status: 'ready', error: '', limits: { ...allowance(now / 1000), primary: { usedPercent: 100, resetsAt: now / 1000 + 5400, windowDurationMinutes: 300 }, secondary: null } }])
    try {
      expect(view.host.querySelector('.uam-limits-lede__title')!.textContent).toBe('Codex is used up')
      expect(view.host.querySelector('.uam-limits-lede__body')!.textContent).toBe('100% of its 5-hour window used, resets in 1h 30m.')
      const row = rowOf(view.host, 'Codex', '5-hour')
      expect(row.textContent).toBe('5-hourResets in 1h 30m100% usedUsed up')
      expect(row.querySelector<HTMLElement>('[role="meter"]')!.dataset.tone).toBe('error')
    } finally { view.close() }
  })

  it('leaves a turned-off provider out of cards, the callout and token totals', async () => {
    const previous = useAppStore.getState().providerAvailability
    useAppStore.setState({ providerAvailability: [{ providerId: 'codex-cli', executionHostId: 'local', enabled: false }] })
    const view = await render([{ providerId: 'claude-cli', scope: 'local:claude', status: 'unavailable', limits: null, error: '' }], usage([tokens('codex-cli', 9_000_000), tokens('claude-cli', 1000)]))
    try {
      expect(view.host.querySelector('[aria-label="Codex"]')).toBeNull()
      expect(view.host.textContent).not.toContain('Codex')
      expect(view.host.querySelectorAll('.uam-limit-card')).toHaveLength(5)
      expect(view.host.querySelector('.uam-limits-lede__aside')!.textContent).toBe('1Ktokens in the past 7 days')
    } finally { view.close(); useAppStore.setState({ providerAvailability: previous }) }
  })

  it('names a remote computer once and shows its tokens without per-card reasons', async () => {
    const previous = useAppStore.getState().executionHosts
    useAppStore.setState({ executionHosts: [{ id: 'lab', label: 'Lab PC' }] as typeof previous })
    const view = await render(['codex-cli', 'claude-cli'].map(providerId => ({ providerId: providerId as UsageProviderId, scope: 'remote', status: 'unavailable' as const, limits: null, error: 'Independent account allowances are not available on this remote connection.' })), usage([tokens('codex-cli', 4000)]), 'lab')
    try {
      expect(view.host.querySelector('.uam-limits-lede__title')!.textContent).toBe('Allowances are read on this computer only')
      expect(view.host.querySelector('.uam-limits-lede__aside')!.textContent).toBe('4Ktokens in the past 7 days on Lab PC')
      const codex = card(view.host, 'Codex')
      expect(codex.querySelector('.uam-limit-card__meta')!.textContent).toBe('4K tokens')
      expect(codex.querySelector('.uam-limit-row')!.textContent).toBe('AllowanceResets —??%Allowance unknown: read on this computer only')
      expect(codex.querySelector('.uam-limit-card__facts')!.textContent).toBe('Independent account allowances are not available on this remote connection.')
    } finally { view.close(); useAppStore.setState({ executionHosts: previous }) }
  })

  it('keeps a known window name on the unknown row and uses "Allowance" only when none is known', async () => {
    const view = await render([
      { providerId: 'copilot-cli', scope: 'local:copilot', status: 'error', limits: null, error: 'Could not read account allowance.', primaryLabel: 'Chat allowance' },
      { providerId: 'gemini-cli', scope: 'local:gemini', status: 'error', limits: null, error: 'Could not read account allowance.' },
    ], usage([tokens('copilot-cli', 0, true), tokens('gemini-cli', 0, true)]))
    try {
      expect(card(view.host, 'Copilot').querySelector('.uam-limit-row')!.textContent).toBe("Chat allowanceResets —??%Chat allowance unknown: Couldn't read")
      const gemini = card(view.host, 'Gemini')
      expect(gemini.querySelector('.uam-limit-row')!.textContent).toBe("AllowanceResets —??%Allowance unknown: Couldn't read")
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
