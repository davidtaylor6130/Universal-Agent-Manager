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
/** Percent text stays neutral until it needs attention. */
const valueTone = (percent: number) => percent >= 80 ? meterTone(percent) : 'var(--text)'

const percentFormat = new Intl.NumberFormat(undefined, { maximumFractionDigits: 1 })
const formatPercent = (percent: number) => percentFormat.format(percent)
const isValidPercent = (percent: number) => Number.isFinite(percent) && percent >= 0 && percent <= 100

/** Used track; the tick marks how far through the provider-reported reset period we are. */
function PaceTrack({ label, percent, tick = null }: { label: string; percent: number | null; tick?: number | null }) {
  if (percent === null) return <span aria-hidden="true" className="uam-pace-track uam-pace-track--open" />
  return <span role="meter" aria-label={label} aria-valuemin={0} aria-valuemax={100} aria-valuenow={percent} className="uam-pace-track">
    <span className="uam-pace-fill" style={{ width: `${percent}%`, background: meterTone(percent) }} />
    {tick !== null && <span aria-hidden="true" className="uam-pace-tick" style={{ left: `${tick * 100}%` }} />}
  </span>
}

/** Share of the reset period already elapsed, from the provider-reported window length. Null when either is unknown. */
function elapsedShare(window: AcpRateLimitWindow, now: number) {
  if (!window.windowDurationMinutes || window.resetsAt === null) return null
  const remainingMs = window.resetsAt * 1000 - now
  if (remainingMs <= 0) return null
  return Math.min(1, Math.max(0, 1 - remainingMs / (window.windowDurationMinutes * 60000)))
}

function AllowanceRow({ label, meterLabel, window, now, showUnknownReset = false, awaitingUpdate = false, missingLabel = 'Not readable at this time' }: { label: string; meterLabel: string; window: AcpRateLimitWindow | null; now: number; showUnknownReset?: boolean; awaitingUpdate?: boolean; missingLabel?: string }) {
  const percent = window ? window.usedPercent : NaN
  const valid = isValidPercent(percent)
  const expired = window !== null && window.resetsAt !== null && window.resetsAt * 1000 <= now
  const reset = window && window.resetsAt !== null
    ? expired ? awaitingUpdate ? 'Reset passed, awaiting update' : 'Provider-reported reset time has passed' : `Resets ${relativeIn(window.resetsAt * 1000, now)}`
    : showUnknownReset ? 'Reset time unavailable' : null
  return <div className="uam-pace-row">
    <div className="uam-pace-row__label min-w-0">
      <span className="break-words">{label}</span>
      {reset && <small title={window?.resetsAt != null ? new Date(window.resetsAt * 1000).toLocaleString() : undefined}>{reset}</small>}
    </div>
    <PaceTrack label={meterLabel} percent={valid ? percent : null} tick={valid && window ? elapsedShare(window, now) : null} />
    <span className="uam-pace-row__value" style={valid ? { color: valueTone(percent) } : undefined}>{valid ? `${formatPercent(percent)}% used` : <span className="uam-usage-muted">{window ? 'Unavailable' : missingLabel}</span>}</span>
  </div>
}

/** Codex Pro reports no secondary window; the 5-hour period is shown as unlimited instead. */
function UnlimitedRow({ label }: { label: string }) {
  return <div className="uam-pace-row">
    <div className="uam-pace-row__label"><span>{label}</span></div>
    <PaceTrack label={label} percent={null} />
    <span className="uam-pace-row__value uam-usage-muted">Unlimited</span>
  </div>
}

type WindowRow = { id: string; label: string; meterLabel: string; window: AcpRateLimitWindow | null; unknownReset?: boolean; missingLabel?: string; unlimited?: boolean }

/** Provider windows in display order. Model allowances replace primary/secondary when the provider reports them. */
function windowRows(provider: UsageProviderId, name: string, allowance: ProviderAllowance | undefined): WindowRow[] {
  const limits = allowance?.limits ?? undefined
  const modelWindows = allowance?.windows ?? []
  if (modelWindows.length) return modelWindows.map(window => ({ id: window.id, label: window.label, meterLabel: `${name} ${window.label} usage`, window, unknownReset: true }))
  // ponytail: plan-name match is an inference; drop it once Codex reports a real secondary window for Pro.
  const noFiveHourCap = provider === 'codex-cli' && !limits?.secondary && /pro/i.test(limits?.planType ?? '')
  return (['primary', 'secondary'] as const).map(key => noFiveHourCap && key === 'secondary'
    ? { id: key, label: '5-hour', meterLabel: '', window: null, unlimited: true }
    : { id: key, label: (key === 'primary' ? allowance?.primaryLabel : allowance?.secondaryLabel) || windowLabel(limits?.[key]?.windowDurationMinutes, key), meterLabel: `${name} ${key} usage`, window: limits?.[key] ?? null, missingLabel: allowance?.status === 'ready' ? 'Not reported by provider' : undefined })
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
    <h2 className="uam-limits-host-title">{host.label}</h2><UsageHostLimits executionHostId={host.id} now={now} />
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
  // The most-used window across providers, named in one sentence when it is near its limit.
  const closest = USAGE_PROVIDERS.flatMap(provider => {
    const name = providerShortName(undefined, provider)
    return windowRows(provider, name, account.data?.providers.find(row => row.providerId === provider))
      .filter(row => row.window && !row.unlimited && isValidPercent(row.window.usedPercent))
      .map(row => ({ name, row }))
  }).reduce<{ name: string; row: WindowRow } | null>((top, next) => !top || next.row.window!.usedPercent > top.row.window!.usedPercent ? next : top, null)
  const closestWindow = closest?.row.window
  const lede = closest && closestWindow && closestWindow.usedPercent >= 80 ? <p className="uam-limits-lede"><b>{closest.name} is closest to its limit:</b> {formatPercent(closestWindow.usedPercent)}% of {closest.row.label} used{closestWindow.resetsAt !== null && closestWindow.resetsAt * 1000 > now ? `, resets ${relativeIn(closestWindow.resetsAt * 1000, now)}` : ''}.</p> : null
  return <div className="grid gap-4">
    <div className="flex items-start justify-between gap-4">
      <div className="grid gap-2 min-w-0">
        {lede}
        <p className="uam-limits-group">{executionHostId === 'local' ? 'Account allowances · read on this computer' : 'Not available for remote computers.'}</p>
      </div>
      {executionHostId === 'local' && <Button size="sm" variant="secondary" onClick={account.refresh} disabled={account.coolingDown} loading={account.refreshing || account.requestingRefresh}>Refresh allowances</Button>}
    </div>
    {executionHostId === 'local' && <div className="uam-pace-key" aria-hidden><span><i />Where you are in the reset period</span><span><b />Used</span></div>}
    {executionHostId === 'local' && account.coolingDown && <p className="text-xs uam-usage-muted">Updated moments ago; refresh available shortly</p>}
    {account.error && <p role="status" className="text-xs" style={{ color: 'var(--warning)' }}>{account.error}{account.data ? ' Showing the last snapshot.' : ''}</p>}
    <div className="uam-pace-list">{USAGE_PROVIDERS.map(provider => <ProviderLimitCard key={provider} provider={provider} allowance={account.data?.providers.find(row => row.providerId === provider)} loading={account.loading} refreshFailed={Boolean(account.error)} context={latest.contexts.get(provider)} consumption={latest.tokens.get(provider)} now={now} />)}</div>
  </div>
}

/** One section per provider with the same structure whether or not data exists; unreadable windows show as empty tracks. */
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
  const plan = limits?.planType || limits?.limitName
  const consumptionTimeValid = Boolean(consumption && consumption.usage.updatedAt > 0 && consumption.usage.updatedAt <= now && Number.isFinite(new Date(consumption.usage.updatedAt).getTime()))
  const timestampValid = Boolean(limits && limits.updatedAt > 0 && limits.updatedAt <= now && Number.isFinite(new Date(limits.updatedAt).getTime()))
  const stale = Boolean(hasAllowance && (refreshFailed || allowance?.status !== 'ready' || !timestampValid || (limits && now - limits.updatedAt >= 300000)))
  const contextPercent = context ? Math.min(100, Math.max(0, context.usage.usedTokens / context.usage.windowTokens * 100 || 0)) : 0
  const rows = windowRows(provider, name, allowance)
  const renderRow = (row: WindowRow) => row.unlimited
    ? <UnlimitedRow key={row.id} label={row.label} />
    : <AllowanceRow key={row.id} label={row.label} meterLabel={row.meterLabel} window={row.window} now={now} showUnknownReset={row.unknownReset} awaitingUpdate={stale || loading} missingLabel={row.missingLabel} />
  return <section className="uam-pace-prov" aria-label={`${name} limits`}>
    <header className="uam-pace-prov__head">
      <h2 className="uam-pace-prov__title"><ProviderLogo providerId={provider} size={16} />{name}</h2>
      {hasAllowance && plan && <span className="uam-limits-chip">{plan}</span>}
      {hasAllowance && <span className="uam-limits-updated" title={timestampValid ? new Date(limits!.updatedAt).toLocaleString() : undefined}>{timestampValid ? `Updated ${relativeAgo(limits!.updatedAt, now)}` : 'Update time unavailable'}{stale ? ' · Cached snapshot' : ''}</span>}
    </header>
    {(loading || allowance?.status === 'loading') && <p role="status" className="text-xs uam-usage-muted animate-pulse">Reading allowances…</p>}
    {allowance?.error && <p className="uam-limits-notice">{allowance.error}{hasAllowance ? ' Showing cached allowances.' : ''}</p>}
    {!loading && !hasAllowance && !allowance?.error && allowance?.status !== 'loading' && <p className="text-xs uam-usage-muted">Account allowances unavailable.</p>}
    {showWindowRows && <div className="uam-pace-rows">
      {(modelWindows.length ? rows.slice(0, 2) : rows).map(renderRow)}
      {modelWindows.length > 2 && <details className="uam-limits-tokens"><summary>{modelWindows.length - 2} more model allowance{modelWindows.length > 3 ? 's' : ''}</summary><div className="uam-pace-rows">{rows.slice(2).map(renderRow)}</div></details>}
      {limits?.individualLimit && <div className="uam-pace-row">
        <div className="uam-pace-row__label"><span>Spend limit</span><small>{limits.individualLimit.used} of {limits.individualLimit.limit}</small></div>
        <PaceTrack label={`${name} spend limit`} percent={Math.min(100, Math.max(0, 100 - limits.individualLimit.remainingPercent))} />
        <span className="uam-pace-row__value">{Math.round(100 - limits.individualLimit.remainingPercent)}% used</span>
      </div>}
      {limits?.credits && <p className="text-xs uam-usage-muted" title={limits.credits.balance ?? undefined}>Credits: {limits.credits.unlimited ? 'Unlimited' : limits.credits.hasCredits ? limits.credits.balance === null ? 'Available' : formatCreditBalance(limits.credits.balance) : 'None'}</p>}
      {limits?.spendControlReached && <p className="text-xs" style={{ color: 'var(--error)' }}>Spend limit reached.</p>}
    </div>}
    {(context || consumption) && <details className="uam-limits-tokens text-xs">
      <summary><span className="uam-limits-label">Context and tokens</span>{consumption && <span className="tabular-nums">{compactTokens(consumption.usage.total.totalTokens)}</span>}</summary>
      <div className="grid gap-3">
        {context && <div className="grid gap-1.5">
          <PaceTrack label={`${name} context usage`} percent={contextPercent} />
          <p className="uam-usage-muted" title={context.title || 'chat'}>Context: {context.usage.usedTokens.toLocaleString()} of {context.usage.windowTokens.toLocaleString()} tokens{now - context.usage.updatedAt >= 300000 ? ' · Cached snapshot' : ''}</p>
        </div>}
        {consumption && <>
          <ConsumptionStack total={consumption.usage.total} />
          <dl className="uam-limits-facts">
            <dt>Input</dt><dd>{consumption.usage.total.inputTokens.toLocaleString()}</dd>
            <dt>Output</dt><dd>{consumption.usage.total.outputTokens.toLocaleString()}</dd>
            {consumption.usage.total.cachedInputTokens > 0 && <><dt>Cache read</dt><dd>{consumption.usage.total.cachedInputTokens.toLocaleString()}</dd></>}
            {consumption.usage.total.cacheWriteInputTokens > 0 && <><dt>Cache write</dt><dd>{consumption.usage.total.cacheWriteInputTokens.toLocaleString()}</dd></>}
            {consumption.usage.total.reasoningOutputTokens > 0 && <><dt>Reasoning</dt><dd>{consumption.usage.total.reasoningOutputTokens.toLocaleString()}</dd></>}
            <dt>Last turn</dt><dd>{consumption.usage.last ? consumption.usage.last.totalTokens.toLocaleString() : 'Unavailable'}</dd>
          </dl>
          <p className="uam-usage-muted">{(provider === 'gemini-cli' || provider === 'opencode-cli') ? 'Observed in' : 'Used in'} {consumption.title || 'chat'}{consumptionTimeValid ? ` · ${new Date(consumption.usage.updatedAt).toLocaleString()}` : ' · Update time unavailable'}{!consumptionTimeValid || now - consumption.usage.updatedAt >= 300000 ? ' · Cached snapshot' : ''}</p>
        </>}
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
