import { useEffect, useMemo, useState, type CSSProperties } from 'react'
import { useAppStore } from '../../store/useAppStore'
import { ALL_MACHINES_HOST_ID, USAGE_PROVIDERS, type UsageProviderId } from '../../services/usageHistory'
import type { AcpContextUsage, AcpRateLimitWindow, AcpTokenUsage } from '../../store/cpp/types'
import { providerShortName } from '../../utils/providerMetadata'
import { ProviderLogo } from '../shared/ProviderLogo'
import { useProviderAllowances } from '../../hooks/useProviderAllowances'
import type { ProviderAllowance } from '../../services/providerAllowances'
import { Button } from '../ui'
import { formatCreditBalance } from '../../utils/providerAllowanceFormatting'
import './usage.css'

/** Allowance colour: calm under 80%, warning from 80%, error from 95%. */
const meterTone = (percent: number) => percent >= 95 ? 'var(--error)' : percent >= 80 ? 'var(--warning)' : 'var(--success)'

function Meter({ label, percent }: { label: string; percent: number | null }) {
  if (percent === null) return <div aria-hidden="true" className="uam-usage-meter" />
  return <div role="meter" aria-label={label} aria-valuemin={0} aria-valuemax={100} aria-valuenow={percent} className="uam-usage-meter">
    <span style={{ width: `${percent}%`, background: meterTone(percent) }} />
  </div>
}

const percentFormat = new Intl.NumberFormat(undefined, { maximumFractionDigits: 1 })
const formatPercent = (percent: number) => percentFormat.format(percent)

function AllowanceWindow({ label, meterLabel, window, now, showUnknownReset = false, awaitingUpdate = false, missingLabel = 'Not readable at this time' }: { label: string; meterLabel: string; window: AcpRateLimitWindow | null; now: number; showUnknownReset?: boolean; awaitingUpdate?: boolean; missingLabel?: string }) {
  const percent = window ? window.usedPercent : NaN
  const valid = Number.isFinite(percent) && percent >= 0 && percent <= 100
  const expired = window !== null && window.resetsAt !== null && window.resetsAt * 1000 <= now
  return <div className="grid gap-1.5">
    <div className="flex items-baseline justify-between gap-4"><span className="uam-limits-label min-w-0 break-words">{label}</span><span className="shrink-0 tabular-nums">{valid ? <><b className="uam-limits-pct" style={{ color: meterTone(percent) }}>{`${formatPercent(percent)}% used`}</b><span className="uam-usage-muted"> · {formatPercent(100 - percent)}% remaining</span></> : <span className="uam-usage-muted">{window ? 'Usage unavailable' : missingLabel}</span>}</span></div>
    <Meter label={meterLabel} percent={valid ? percent : null} />
    {window && window.resetsAt !== null ? <p className="uam-usage-muted" title={new Date(window.resetsAt * 1000).toLocaleString()}>{expired ? awaitingUpdate ? 'Reset passed, awaiting update' : 'Provider-reported reset time has passed' : `Resets ${relativeIn(window.resetsAt * 1000, now)}`}</p> : showUnknownReset && <p className="uam-usage-muted">Reset time unavailable</p>}
  </div>
}

/** Consumed tokens as one stacked bar. Cache reads and writes are part of input; reasoning is part of output. */
function ConsumptionStack({ total }: { total: AcpTokenUsage['total'] }) {
  const cache = Math.max(0, total.cachedInputTokens) + Math.max(0, total.cacheWriteInputTokens)
  const segments = [
    ['Input', Math.max(0, total.inputTokens - cache), 'var(--token-input)'],
    ['Cache read', total.cachedInputTokens, 'var(--token-cache-read)'],
    ['Cache write', total.cacheWriteInputTokens, 'var(--token-cache-write)'],
    ['Output', Math.max(0, total.outputTokens - total.reasoningOutputTokens), 'var(--token-output)'],
    ['Reasoning', total.reasoningOutputTokens, 'var(--token-reasoning)'],
  ] as const
  return <div className="grid gap-2"><span className="uam-usage-stack" aria-hidden>{segments.map(([label, value, color]) => value > 0 ? <span key={label} title={`${label} · ${value.toLocaleString()}`} style={{ flexGrow: value, '--c': color } as CSSProperties} /> : null)}</span>
    <ul className="uam-usage-key text-xs" aria-hidden>{segments.map(([label, , color]) => <li key={label} className="uam-usage-key-item"><i className="uam-usage-swatch" style={{ '--c': color } as CSSProperties} />{label}</li>)}</ul>
  </div>
}

/** Show provider consumption, context and allowances scoped to the selected host. */
export function UsageLimits({ executionHostId }: { executionHostId: string }) {
  const configured = useAppStore(state => state.executionHosts)
  const [clock, setClock] = useState(() => Date.now())
  useEffect(() => {
    const timer = setInterval(() => setClock(Date.now()), 30000)
    return () => clearInterval(timer)
  }, [])
  const now = Math.max(clock, Date.now())
  const column = 'uam-usage mx-auto grid max-w-6xl gap-8'
  if (executionHostId !== ALL_MACHINES_HOST_ID) return <div className={column}><UsageHostLimits executionHostId={executionHostId} now={now} /></div>
  const hosts = configured.some(host => host.id === 'local') ? configured : [{ id: 'local', label: 'This computer' }, ...configured]
  return <div className={column}>{hosts.map(host => <section key={host.id} aria-label={`${host.label} limits`} className="grid gap-4">
    <h2 className="text-sm font-semibold">{host.label}</h2><UsageHostLimits executionHostId={host.id} now={now} />
  </section>)}</div>
}

function UsageHostLimits({ executionHostId, now: clock }: { executionHostId: string; now: number }) {
  const sessions = useAppStore(state => state.sessions)
  const bindings = useAppStore(state => state.acpBindingBySessionId)
  const account = useProviderAllowances(executionHostId)
  const now = Math.max(clock, Date.now())
  const latest = useMemo(() => {
    const contexts = new Map<string, { usage: AcpContextUsage; title: string }>()
    const tokens = new Map<string, { usage: AcpTokenUsage; title: string }>()
    for (const session of sessions) {
      if ((session.executionHostId || 'local') !== executionHostId) continue
      const binding = bindings[session.id]
      const provider = binding?.providerId
      if (!provider || provider !== session.providerId) continue
      const consumption = binding.providerUsage?.tokenUsage
      if (consumption && consumption.updatedAt > (tokens.get(provider)?.usage.updatedAt ?? -1)) tokens.set(provider, { usage: consumption, title: session.name })
      const context = binding.providerUsage?.contextUsage
      if (context && context.updatedAt > (contexts.get(provider)?.usage.updatedAt ?? -1)) contexts.set(provider, { usage: context, title: session.name })
    }
    return { contexts, tokens }
  }, [sessions, bindings, executionHostId])
  return <div className="grid gap-4"><div className="flex items-center justify-between gap-4"><p className="text-xs uam-usage-muted">{executionHostId === 'local' ? 'Account allowances on this computer. No chat needs to be running.' : 'Account allowances are not available for remote computers.'}</p>{executionHostId === 'local' && <Button size="sm" variant="secondary" onClick={account.refresh} disabled={account.coolingDown} loading={account.refreshing || account.requestingRefresh}>Refresh allowances</Button>}</div>
    {executionHostId === 'local' && account.coolingDown && <p className="text-xs uam-usage-muted">Updated moments ago; refresh available shortly</p>}
    {account.error && <p role="status" className="text-xs" style={{ color: 'var(--warning)' }}>{account.error}{account.data ? ' Showing the last account snapshot.' : ''}</p>}
    <div className="uam-limits-grid">{USAGE_PROVIDERS.map(provider => <ProviderLimitCard key={provider} provider={provider} allowance={account.data?.providers.find(row => row.providerId === provider)} loading={account.loading} refreshFailed={Boolean(account.error)} context={latest.contexts.get(provider)} consumption={latest.tokens.get(provider)} now={now} />)}</div>
  </div>
}

/** One card per provider with the same structure whether or not data exists; unreadable windows show as empty tracks. */
function ProviderLimitCard({ provider, allowance, loading, refreshFailed, context, consumption, now }: {
  provider: UsageProviderId
  allowance: ProviderAllowance | undefined
  loading: boolean
  refreshFailed: boolean
  context: { usage: AcpContextUsage; title: string } | undefined
  consumption: { usage: AcpTokenUsage; title: string } | undefined
  now: number
}) {
  const limits = allowance?.limits ?? undefined
  const modelWindows = allowance?.windows ?? []
  const hasAllowance = Boolean(limits || modelWindows.length)
  const showWindowRows = hasAllowance || loading || allowance?.status === 'loading' || allowance?.status === 'ready'
  const name = providerShortName(undefined, provider)
  const consumptionTimeValid = Boolean(consumption && consumption.usage.updatedAt > 0 && consumption.usage.updatedAt <= now && Number.isFinite(new Date(consumption.usage.updatedAt).getTime()))
  const timestampValid = Boolean(limits && limits.updatedAt > 0 && limits.updatedAt <= now && Number.isFinite(new Date(limits.updatedAt).getTime()))
  const stale = Boolean(hasAllowance && (refreshFailed || allowance?.status !== 'ready' || !timestampValid || (limits && now - limits.updatedAt >= 300000)))
  const contextPercent = context ? Math.min(100, Math.max(0, context.usage.usedTokens / context.usage.windowTokens * 100 || 0)) : 0
  return <section className="uam-limits-card" aria-label={`${name} limits`}>
    <header className="uam-limits-card__head">
      <h2 className="flex items-center gap-2 text-sm font-medium"><ProviderLogo providerId={provider} size={17} />{name}</h2>
      {hasAllowance && <span className="uam-limits-chip">{limits?.planType || limits?.limitName || 'Provider allowance'}</span>}
      {hasAllowance && <span className="uam-limits-updated" title={timestampValid ? new Date(limits!.updatedAt).toLocaleString() : undefined}>{timestampValid ? `Updated ${relativeAgo(limits!.updatedAt, now)}` : 'Update time unavailable'}{stale ? ' · Cached snapshot' : ''}</span>}
    </header>
    {(loading || allowance?.status === 'loading') && <p role="status" className="text-xs uam-usage-muted animate-pulse">Reading account allowances…</p>}
    {allowance?.error && <p className="text-xs" style={{ color: 'var(--warning)' }}>{allowance.error}{hasAllowance ? ' Showing cached allowances.' : ''}</p>}
    {!loading && !hasAllowance && !allowance?.error && allowance?.status !== 'loading' && <p className="text-xs uam-usage-muted">Account allowances unavailable.</p>}
    <div className="grid gap-3 text-xs">
      {showWindowRows && (modelWindows.length ? <>{modelWindows.slice(0, 2).map(window => <AllowanceWindow key={window.id} label={window.label} meterLabel={`${name} ${window.label} usage`} window={window} now={now} showUnknownReset awaitingUpdate={stale || loading} />)}{modelWindows.length > 2 && <details className="uam-limits-tokens"><summary>{modelWindows.length - 2} more model allowance{modelWindows.length > 3 ? 's' : ''}</summary><div className="grid gap-3">{modelWindows.slice(2).map(window => <AllowanceWindow key={window.id} label={window.label} meterLabel={`${name} ${window.label} usage`} window={window} now={now} showUnknownReset awaitingUpdate={stale || loading} />)}</div></details>}</> : (['primary', 'secondary'] as const).map(key => <AllowanceWindow key={key} label={(key === 'primary' ? allowance?.primaryLabel : allowance?.secondaryLabel) || windowLabel(limits?.[key]?.windowDurationMinutes, key)} meterLabel={`${name} ${key} usage`} window={limits?.[key] ?? null} now={now} awaitingUpdate={stale || loading} missingLabel={allowance?.status === 'ready' ? 'Not reported by provider' : undefined} />))}
      {limits?.individualLimit && <div className="grid gap-1.5"><div className="flex justify-between gap-4"><span className="uam-limits-label">Spend limit</span><span className="tabular-nums">Spend limit: {limits.individualLimit.used} of {limits.individualLimit.limit} · {Math.round(limits.individualLimit.remainingPercent)}% remaining</span></div><Meter label={`${name} spend limit`} percent={Math.min(100, Math.max(0, 100 - limits.individualLimit.remainingPercent))} /></div>}
      {limits?.credits && <p title={limits.credits.balance ?? undefined}>Credits: {limits.credits.unlimited ? 'Unlimited' : limits.credits.hasCredits ? limits.credits.balance === null ? 'Available' : formatCreditBalance(limits.credits.balance) : 'None'}</p>}
      {limits?.spendControlReached && <p style={{ color: 'var(--error)' }}>Provider spend limit reached.</p>}
    </div>
    {context && <div className="grid gap-1.5 text-xs">
      <div className="flex items-baseline justify-between gap-4"><span className="uam-limits-label">Context window</span><span className="tabular-nums uam-usage-muted">{Math.round(contextPercent)}%</span></div>
      <span className="uam-usage-meter" aria-hidden><span style={{ width: `${contextPercent}%`, background: meterTone(contextPercent) }} /></span>
      <p className="uam-usage-muted">Context in {context.title || 'chat'}: {context.usage.usedTokens.toLocaleString()} of {context.usage.windowTokens.toLocaleString()} tokens · {Math.round(context.usage.usedTokens / context.usage.windowTokens * 100)}%{now - context.usage.updatedAt >= 300000 ? ' · Cached snapshot' : ''}</p>
    </div>}
    {!consumption ? <p className="text-xs uam-usage-muted">Consumed tokens unavailable.</p> : <details className="uam-limits-tokens text-xs">
      <summary><span className="uam-limits-label">Tokens</span><span className="tabular-nums">{compactTokens(consumption.usage.total.totalTokens)}</span></summary>
      <ConsumptionStack total={consumption.usage.total} />
      <div className="grid gap-1 uam-usage-muted">
        <p>{(provider === 'gemini-cli' || provider === 'opencode-cli') ? 'Tokens observed while attached to' : 'Tokens consumed in'} {consumption.title || 'chat'}: {consumption.usage.total.totalTokens.toLocaleString()}</p>
        <p>Input {consumption.usage.total.inputTokens.toLocaleString()} · Output {consumption.usage.total.outputTokens.toLocaleString()}</p>
        {(consumption.usage.total.cachedInputTokens > 0 || consumption.usage.total.cacheWriteInputTokens > 0) && <p>Of input: cache read {consumption.usage.total.cachedInputTokens.toLocaleString()} · cache write {consumption.usage.total.cacheWriteInputTokens.toLocaleString()}</p>}
        {consumption.usage.total.reasoningOutputTokens > 0 && <p>Of output: reasoning {consumption.usage.total.reasoningOutputTokens.toLocaleString()}</p>}
        <p>Last turn: {consumption.usage.last ? `${consumption.usage.last.totalTokens.toLocaleString()} tokens` : 'Unavailable'}</p>
        <p>{consumptionTimeValid ? `Updated ${new Date(consumption.usage.updatedAt).toLocaleString()}` : 'Update time unavailable'}{!consumptionTimeValid || now - consumption.usage.updatedAt >= 300000 ? ' · Cached snapshot' : ''}</p>
      </div>
    </details>}
  </section>
}

function windowLabel(minutes: number | null | undefined, key: 'primary' | 'secondary') {
  if (!minutes) return key === 'primary' ? 'Primary window' : 'Secondary window'
  if (minutes === 10080) return 'Weekly window'
  if (minutes % 1440 === 0) return `${minutes / 1440} day window`
  return `${+(minutes / 60).toFixed(1)} hour window`
}

function span(ms: number) {
  const minutes = Math.max(1, Math.round(ms / 60000))
  if (minutes < 60) return `${minutes}m`
  const hours = Math.floor(minutes / 60)
  if (hours < 48) return `${hours}h ${minutes % 60}m`
  return `${Math.round(hours / 24)}d`
}
const relativeIn = (at: number, now: number) => `in ${span(at - now)}`
const relativeAgo = (at: number, now: number) => now - at < 60000 ? 'just now' : `${span(now - at)} ago`
const compactTokens = (value: number) => value >= 1e6 ? `${+(value / 1e6).toFixed(1)}M` : value >= 1e3 ? `${+(value / 1e3).toFixed(1)}k` : `${value}`
