import { useMemo, type CSSProperties } from 'react'
import { useAppStore } from '../../store/useAppStore'
import { ALL_MACHINES_HOST_ID, USAGE_PROVIDERS } from '../../services/usageHistory'
import type { AcpContextUsage, AcpRateLimits, AcpTokenUsage } from '../../store/cpp/types'
import { providerShortName } from '../../utils/providerMetadata'
import { ProviderLogo } from '../shared/ProviderLogo'
import './usage.css'

/** Allowance colour: calm under 80%, warning from 80%, error from 95%. */
const meterTone = (percent: number) => percent >= 95 ? 'var(--error)' : percent >= 80 ? 'var(--warning)' : 'var(--success)'

function Meter({ label, percent }: { label: string; percent: number | null }) {
  const value = percent ?? 0
  return <div role="meter" aria-label={label} aria-valuemin={0} aria-valuemax={100} aria-valuenow={percent ?? undefined} className="uam-usage-meter">
    <span style={{ width: `${value}%`, background: meterTone(value) }} />
  </div>
}

/** Consumed tokens as one stacked bar. Cache reads and writes are part of input; reasoning is part of output. */
function ConsumptionStack({ total }: { total: AcpTokenUsage['total'] }) {
  const cache = Math.max(0, total.cachedInputTokens) + Math.max(0, total.cacheWriteInputTokens)
  const segments = [
    ['Input', Math.max(0, total.inputTokens - cache), 'var(--blue)'],
    ['Cache read', total.cachedInputTokens, 'var(--teal)'],
    ['Cache write', total.cacheWriteInputTokens, 'var(--purple)'],
    ['Output', Math.max(0, total.outputTokens - total.reasoningOutputTokens), 'var(--green)'],
    ['Reasoning', total.reasoningOutputTokens, 'var(--yellow)'],
  ] as const
  return <div className="grid gap-2"><span className="uam-usage-stack" aria-hidden>{segments.map(([label, value, color]) => value > 0 ? <span key={label} title={`${label} · ${value.toLocaleString()}`} style={{ flexGrow: value, '--c': color } as CSSProperties} /> : null)}</span>
    <div className="uam-usage-key text-xs" aria-hidden>{segments.map(([label, , color]) => <span key={label}><i className="uam-usage-swatch" style={{ '--c': color } as CSSProperties} />{label}</span>)}</div>
  </div>
}

/** Show provider consumption, context and allowances scoped to the selected host. */
export function UsageLimits({ executionHostId }: { executionHostId: string }) {
  const configured = useAppStore(state => state.executionHosts)
  const column = 'mx-auto grid max-w-6xl gap-8'
  if (executionHostId !== ALL_MACHINES_HOST_ID) return <div className={column}><UsageHostLimits executionHostId={executionHostId} /></div>
  const hosts = configured.some(host => host.id === 'local') ? configured : [{ id: 'local', label: 'This computer' }, ...configured]
  return <div className={column}>{hosts.map(host => <section key={host.id} aria-label={`${host.label} limits`} className="grid gap-4">
    <h2 className="text-sm font-semibold">{host.label}</h2><UsageHostLimits executionHostId={host.id} />
  </section>)}</div>
}

function UsageHostLimits({ executionHostId }: { executionHostId: string }) {
  const sessions = useAppStore(state => state.sessions)
  const bindings = useAppStore(state => state.acpBindingBySessionId)
  const latest = useMemo(() => {
    const result = new Map<string, AcpRateLimits>()
    const contexts = new Map<string, { usage: AcpContextUsage; title: string }>()
    const tokens = new Map<string, { usage: AcpTokenUsage; title: string }>()
    for (const session of sessions) {
      if ((session.executionHostId || 'local') !== executionHostId) continue
      const binding = bindings[session.id]
      const limits = binding?.providerUsage?.rateLimits
      const provider = binding?.providerId
      if (!provider || provider !== session.providerId) continue
      const consumption = binding.providerUsage?.tokenUsage
      if (consumption && consumption.updatedAt > (tokens.get(provider)?.usage.updatedAt ?? -1)) tokens.set(provider, { usage: consumption, title: session.name })
      const context = binding.providerUsage?.contextUsage
      if (context && context.updatedAt > (contexts.get(provider)?.usage.updatedAt ?? -1)) contexts.set(provider, { usage: context, title: session.name })
      if (limits && limits.updatedAt > (result.get(provider)?.updatedAt ?? -1)) result.set(provider, limits)
    }
    return { limits: result, contexts, tokens }
  }, [sessions, bindings, executionHostId])
  const now = Date.now()
  const hasData = (provider: string) => latest.limits.has(provider) || latest.contexts.has(provider) || latest.tokens.has(provider)
  const active = USAGE_PROVIDERS.filter(hasData)
  const idle = USAGE_PROVIDERS.filter(provider => !hasData(provider))
  return <div className="grid gap-4"><p className="text-xs uam-usage-muted">Latest provider snapshots from chats on this host.</p>
    {active.length > 0 && <div className="uam-limits-grid">{active.map(provider => {
      const limits = latest.limits.get(provider)
      const context = latest.contexts.get(provider)
      const consumption = latest.tokens.get(provider)
      const consumptionTimeValid = Boolean(consumption && consumption.usage.updatedAt > 0 && consumption.usage.updatedAt <= now && Number.isFinite(new Date(consumption.usage.updatedAt).getTime()))
      const timestampValid = Boolean(limits && limits.updatedAt > 0 && limits.updatedAt <= now && Number.isFinite(new Date(limits.updatedAt).getTime()))
      const stale = Boolean(limits && (!timestampValid || now - limits.updatedAt >= 300000))
      const contextPercent = context ? Math.min(100, Math.max(0, context.usage.usedTokens / context.usage.windowTokens * 100 || 0)) : 0
      return <section key={provider} className="uam-limits-card" aria-label={`${providerShortName(undefined, provider)} limits`}>
        <header className="uam-limits-card__head">
          <h2 className="flex items-center gap-2 text-sm font-medium"><ProviderLogo providerId={provider} size={17} />{providerShortName(undefined, provider)}</h2>
          {limits && <span className="uam-limits-chip">{limits.planType || limits.limitName || 'Provider allowance'}</span>}
          {limits && <span className="uam-limits-updated" title={timestampValid ? new Date(limits.updatedAt).toLocaleString() : undefined}>{timestampValid ? `Updated ${relativeAgo(limits.updatedAt, now)}` : 'Update time unavailable'}{stale ? ' · Cached snapshot' : ''}</span>}
        </header>
        {!limits ? <p className="text-xs uam-usage-muted">No limits received.</p> : <div className="grid gap-3 text-xs">
          {(['primary', 'secondary'] as const).map(key => {
            const window = limits[key]
            if (!window) return null
            const percent = window.usedPercent
            const valid = Number.isFinite(percent) && percent >= 0 && percent <= 100
            const expired = window.resetsAt !== null && window.resetsAt * 1000 <= now
            return <div key={key} className="grid gap-1.5">
              <div className="flex items-baseline justify-between gap-4"><span className="uam-limits-label">{windowLabel(window.windowDurationMinutes, key)}</span><span className="tabular-nums"><b className="uam-limits-pct" style={{ color: valid ? meterTone(percent) : undefined }}>{valid ? `${Math.round(percent)}% used` : 'Usage unavailable'}</b>{valid ? <span className="uam-usage-muted"> · {Math.round(100 - percent)}% remaining</span> : null}</span></div>
              <Meter label={`${providerShortName(undefined, provider)} ${key} usage`} percent={valid ? percent : null} />
              {window.resetsAt !== null && <p className="uam-usage-muted" title={new Date(window.resetsAt * 1000).toLocaleString()}>{expired ? 'Reset passed, awaiting update' : `Resets ${relativeIn(window.resetsAt * 1000, now)}`}<span className="sr-only"> · reset passed, awaiting update</span></p>}
            </div>
          })}
          {limits.individualLimit && <div className="grid gap-1.5"><div className="flex justify-between gap-4"><span className="uam-limits-label">Spend limit</span><span className="tabular-nums">Spend limit: {limits.individualLimit.used} of {limits.individualLimit.limit} · {Math.round(limits.individualLimit.remainingPercent)}% remaining</span></div><Meter label={`${providerShortName(undefined, provider)} spend limit`} percent={Math.min(100, Math.max(0, 100 - limits.individualLimit.remainingPercent))} /></div>}
          {limits.credits && <p>Credits: {limits.credits.unlimited ? 'Unlimited' : limits.credits.hasCredits ? limits.credits.balance ?? 'Available' : 'None'}</p>}
          {limits.spendControlReached && <p style={{ color: 'var(--error)' }}>Provider spend limit reached.</p>}
        </div>}
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
    })}</div>}
    {idle.length > 0 && <div className="uam-limits-idle">{idle.map(provider => <span key={provider} title="No limits received. Consumed tokens unavailable."><ProviderLogo providerId={provider} size={14} />{providerShortName(undefined, provider)}</span>)}<span className="uam-usage-muted">No limits received yet. Start a chat with these providers to see allowances.</span><span className="sr-only">No limits received. Consumed tokens unavailable.</span></div>}
  </div>
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
