import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterAll, beforeAll, describe, expect, it, vi } from 'vitest'
import type { AcpRateLimits } from '../../store/cpp/types'
import { ProviderAllowanceIndicator } from './ProviderAllowanceIndicator'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
const now = 1_700_000_000_000
const limits: AcpRateLimits = {
  updatedAt: now - 60_000, limitId: 'codex', limitName: 'Codex',
  primary: { usedPercent: 25, resetsAt: now / 1000 + 3600, windowDurationMinutes: 300 },
  secondary: { usedPercent: 80, resetsAt: now / 1000 + 7200, windowDurationMinutes: 10080 },
  credits: { hasCredits: true, unlimited: false, balance: '4.50' }, individualLimit: null,
  spendControlReached: null, planType: 'plus', rateLimitReachedType: '',
}

function render(props: Parameters<typeof ProviderAllowanceIndicator>[0]) {
  const host = document.createElement('div')
  document.body.appendChild(host)
  const root = createRoot(host)
  act(() => root.render(<ProviderAllowanceIndicator {...props} />))
  return { host, root, close: () => { act(() => root.unmount()); host.remove() } }
}

describe('ProviderAllowanceIndicator', () => {
  beforeAll(() => vi.stubGlobal('ResizeObserver', class { observe() {} unobserve() {} disconnect() {} }))
  afterAll(() => vi.unstubAllGlobals())
  it('formats provider credit balances without dropping the exact source value', async () => {
    const raw = '41049.6267850000'
    const view = render({ providerId: 'codex-cli', sourceProviderId: 'codex-cli', rateLimits: { ...limits, credits: { hasCredits: true, unlimited: false, balance: raw } }, running: true, nowMs: now })
    try {
      await act(async () => view.host.querySelector('button')!.focus())
      expect(document.querySelector('[role="tooltip"]')?.textContent).toContain('Credits: 41,049.63')
      expect(document.querySelector(`[title="${raw}"]`)?.textContent).toBe('Credits: 41,049.63')
    } finally { view.close() }
  })
  it('shows the limiting native window and source/reset details on keyboard focus', async () => {
    const view = render({ providerId: 'codex-cli', sourceProviderId: 'codex-cli', rateLimits: limits, running: true, nowMs: now })
    try {
      expect(view.host.textContent).toContain('20% remaining')
      await act(async () => { view.host.querySelector('button')!.focus() })
      const tooltip = document.querySelector('[role="tooltip"]')?.textContent
      expect(tooltip).toContain('Codex session-reported account allowance')
      expect(tooltip).toContain('Updated 1 min ago')
      expect(tooltip).toContain('Primary: 75% remaining')
      expect(tooltip).toContain(new Date((now / 1000 + 3600) * 1000).toLocaleString())
      expect(tooltip).toContain('Credits: 4.5')
      expect(document.querySelector('[title="4.50"]')?.textContent).toBe('Credits: 4.5')
    } finally { view.close() }
  })

  it.each([
    { providerId: 'claude-cli', sourceProviderId: 'codex-cli', rateLimits: limits },
    { providerId: 'codex-cli', sourceProviderId: 'claude-cli', rateLimits: limits },
    { providerId: 'codex-cli', sourceProviderId: undefined, rateLimits: limits },
    { providerId: 'codex-cli', sourceProviderId: 'codex-cli', rateLimits: null },
  ])('never exposes another provider or missing session quota: $providerId / $sourceProviderId', (props) => {
    const view = render({ ...props, running: true, nowMs: now })
    try {
      expect(view.host.querySelector('button')?.getAttribute('aria-label')).toBe('Account allowance: Unavailable')
      expect(view.host.textContent).not.toContain('%')
    } finally { view.close() }
  })

  it('marks stale and stopped snapshots as cached and rejects invalid update times', async () => {
    const view = render({ providerId: 'codex-cli', sourceProviderId: 'codex-cli', rateLimits: { ...limits, updatedAt: now - 10 * 60_000 }, running: true, nowMs: now })
    try {
      expect(view.host.textContent).toContain('Cached 20% remaining')
      await act(async () => { view.host.querySelector('button')!.focus() })
      expect(document.querySelector('[role="tooltip"]')?.textContent).toContain('Updated 10 min ago (stale)')
      act(() => view.root.render(<ProviderAllowanceIndicator providerId="codex-cli" sourceProviderId="codex-cli" rateLimits={limits} running={false} nowMs={now} />))
      expect(view.host.textContent).toContain('Cached 20% remaining')
      for (const updatedAt of [0, Number.NaN, now + 1000]) {
        act(() => view.root.render(<ProviderAllowanceIndicator providerId="codex-cli" sourceProviderId="codex-cli" rateLimits={{ ...limits, updatedAt }} running nowMs={now} />))
        expect(view.host.querySelector('button')?.getAttribute('aria-label')).toBe('Account allowance: Unavailable')
      }
    } finally { view.close() }
  })

  it('updates snapshot freshness on focus without polling and marks passed resets as cached', async () => {
    vi.spyOn(Date, 'now').mockReturnValue(now)
    const view = render({ providerId: 'codex-cli', sourceProviderId: 'codex-cli', rateLimits: limits, running: true })
    try {
      expect(view.host.textContent).toContain('20% remaining')
      expect(view.host.textContent).not.toContain('Cached')
      vi.spyOn(Date, 'now').mockReturnValue(now + 10 * 60_000)
      await act(async () => { view.host.querySelector('button')!.focus() })
      expect(view.host.textContent).toContain('Cached 20% remaining')
      expect(document.querySelector('[role="tooltip"]')?.textContent).toContain('Updated 11 min ago (stale)')
      act(() => view.root.render(<ProviderAllowanceIndicator providerId="codex-cli" sourceProviderId="codex-cli" rateLimits={{ ...limits, primary: { ...limits.primary!, resetsAt: now / 1000 - 1 } }} running nowMs={now} />))
      expect(view.host.textContent).toContain('Cached 20% remaining')
      expect(document.querySelector('[role="tooltip"]')?.textContent).toContain('Reset time passed; awaiting a new snapshot')
    } finally { view.close(); vi.restoreAllMocks() }
  })

  it('shows zero remaining for a native reached spend limit without inferring it from missing credits', () => {
    const view = render({ providerId: 'codex-cli', sourceProviderId: 'codex-cli', rateLimits: { ...limits, spendControlReached: true }, running: true, nowMs: now })
    try {
      expect(view.host.textContent).toContain('0% remaining')
      act(() => view.root.render(<ProviderAllowanceIndicator providerId="codex-cli" sourceProviderId="codex-cli" rateLimits={{ ...limits, credits: { hasCredits: false, unlimited: false, balance: null } }} running nowMs={now} />))
      expect(view.host.textContent).toContain('20% remaining')
    } finally { view.close() }
  })

  it('does not invent quota percentages from credits or invalid usage windows', () => {
    const view = render({ providerId: 'codex-cli', sourceProviderId: 'codex-cli', rateLimits: { ...limits, primary: null, secondary: null }, running: true, nowMs: now })
    try {
      expect(view.host.textContent).toContain('Credits available')
      expect(view.host.textContent).not.toContain('%')
      act(() => view.root.render(<ProviderAllowanceIndicator providerId="codex-cli" sourceProviderId="codex-cli" rateLimits={{ ...limits, primary: { ...limits.primary!, usedPercent: Number.NaN }, secondary: null, credits: null }} running nowMs={now} />))
      expect(view.host.querySelector('button')?.getAttribute('aria-label')).toBe('Account allowance: Unavailable')
    } finally { view.close() }
  })

  it('marks a passed individual spend reset as cached even without token windows', async () => {
    const view = render({ providerId: 'codex-cli', sourceProviderId: 'codex-cli', rateLimits: {
      ...limits, primary: null, secondary: null, credits: null,
      individualLimit: { used: 75, limit: 100, remainingPercent: 25, resetsAt: now / 1000 - 1 },
    }, running: true, nowMs: now })
    try {
      expect(view.host.textContent).toContain('Cached 25% remaining')
      await act(async () => view.host.querySelector('button')!.focus())
      expect(document.querySelector('[role="tooltip"]')?.textContent).toContain('Reset time passed; awaiting a new snapshot')
    } finally { view.close() }
  })

  it('shows matching Claude allowance snapshots without relabelling them Codex', async () => {
    const view = render({ providerId: 'claude-cli', sourceProviderId: 'claude-cli', rateLimits: limits, running: true, nowMs: now })
    try {
      expect(view.host.textContent).toContain('20% remaining')
      await act(async () => view.host.querySelector('button')!.focus())
      expect(document.querySelector('[role="tooltip"]')?.textContent).toContain('Claude session-reported account allowance')
      expect(document.querySelector('[role="tooltip"]')?.textContent).not.toContain('Codex session-reported')
    } finally { view.close() }
  })
})
