import { useMemo } from 'react'
import { useAppStore } from '../../store/useAppStore'
import { ALL_MACHINES_HOST_ID, USAGE_PROVIDERS } from '../../services/usageHistory'
import type { AcpContextUsage, AcpRateLimits, AcpTokenUsage } from '../../store/cpp/types'
import { providerShortName } from '../../utils/providerMetadata'
import { ProviderLogo } from '../shared/ProviderLogo'

/** Show provider consumption, context and allowances scoped to the selected host. */
export function UsageLimits({ executionHostId }: { executionHostId: string }) {
  const configured = useAppStore(state => state.executionHosts)
  if (executionHostId !== ALL_MACHINES_HOST_ID) return <UsageHostLimits executionHostId={executionHostId} />
  const hosts = configured.some(host => host.id === 'local') ? configured : [{ id: 'local', label: 'This computer' }, ...configured]
  return <div className="mx-auto grid max-w-6xl gap-8">{hosts.map(host => <section key={host.id} aria-label={`${host.label} limits`} className="grid gap-4">
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
  return <div className="mx-auto grid max-w-6xl gap-6"><p className="text-xs">Latest provider snapshots from chats on this host.</p>
    {USAGE_PROVIDERS.map(provider => {
      const limits = latest.limits.get(provider)
      const context = latest.contexts.get(provider)
      const consumption = latest.tokens.get(provider)
      const consumptionTimeValid = Boolean(consumption && consumption.usage.updatedAt > 0 && consumption.usage.updatedAt <= now && Number.isFinite(new Date(consumption.usage.updatedAt).getTime()))
      const timestampValid = Boolean(limits && limits.updatedAt > 0 && limits.updatedAt <= now && Number.isFinite(new Date(limits.updatedAt).getTime()))
      const stale = Boolean(limits && (!timestampValid || now - limits.updatedAt >= 300000))
      return <section key={provider} className="grid gap-3 border-b pb-5" style={{ borderColor: 'var(--border)' }}>
        <h2 className="flex items-center gap-2 text-sm font-medium"><ProviderLogo providerId={provider} size={17} />{providerShortName(undefined, provider)}</h2>
        {!consumption && <p className="text-xs">Consumed tokens unavailable.</p>}
        {consumption && <div className="grid gap-1 text-xs">
          <p>{provider === 'gemini-cli' ? 'Tokens observed while attached to' : 'Tokens consumed in'} {consumption.title || 'chat'}: {consumption.usage.total.totalTokens.toLocaleString()}</p>
          <p>Input {consumption.usage.total.inputTokens.toLocaleString()} · Output {consumption.usage.total.outputTokens.toLocaleString()}</p>
          {(consumption.usage.total.cachedInputTokens > 0 || consumption.usage.total.cacheWriteInputTokens > 0) && <p>Of input: cache read {consumption.usage.total.cachedInputTokens.toLocaleString()} · cache write {consumption.usage.total.cacheWriteInputTokens.toLocaleString()}</p>}
          {consumption.usage.total.reasoningOutputTokens > 0 && <p>Of output: reasoning {consumption.usage.total.reasoningOutputTokens.toLocaleString()}</p>}
          <p>Last turn: {consumption.usage.last ? `${consumption.usage.last.totalTokens.toLocaleString()} tokens` : 'Unavailable'}</p>
          <p>{consumptionTimeValid ? `Updated ${new Date(consumption.usage.updatedAt).toLocaleString()}` : 'Update time unavailable'}{!consumptionTimeValid || now - consumption.usage.updatedAt >= 300000 ? ' · Cached snapshot' : ''}</p>
        </div>}
        {context && <p className="text-xs">Context in {context.title || 'chat'}: {context.usage.usedTokens.toLocaleString()} of {context.usage.windowTokens.toLocaleString()} tokens · {Math.round(context.usage.usedTokens / context.usage.windowTokens * 100)}%{now - context.usage.updatedAt >= 300000 ? ' · Cached snapshot' : ''}</p>}
        {!limits ? <p className="text-xs">No limits received.</p> : <div className="grid gap-3 text-xs">
          <p>{limits.planType || limits.limitName || 'Provider allowance'} · {timestampValid ? `Updated ${new Date(limits.updatedAt).toLocaleString()}` : 'Update time unavailable'}{stale ? ' · Cached snapshot' : ''}</p>
          {(['primary', 'secondary'] as const).map(key => {
            const window = limits[key]
            if (!window) return null
            const percent = window.usedPercent
            const valid = Number.isFinite(percent) && percent >= 0 && percent <= 100
            const expired = window.resetsAt !== null && window.resetsAt * 1000 <= now
            return <div key={key} className="grid gap-2"><div className="flex justify-between gap-4"><span>{window.windowDurationMinutes ? `${window.windowDurationMinutes / 60} hour window` : key === 'primary' ? 'Primary window' : 'Secondary window'}</span><span>{valid ? `${Math.round(100 - percent)}% remaining` : 'Usage unavailable'}{expired ? ' · reset passed, awaiting update' : ''}</span></div>
              <div role="meter" aria-label={`${providerShortName(undefined, provider)} ${key} usage`} aria-valuemin={0} aria-valuemax={100} aria-valuenow={valid ? percent : undefined} className="h-1" style={{ background: 'var(--border)' }}><div className="h-full" style={{ width: `${valid ? percent : 0}%`, background: 'var(--accent)' }} /></div>
              {window.resetsAt !== null && <p>Resets {new Date(window.resetsAt * 1000).toLocaleString()}</p>}
            </div>
          })}
          {limits.credits && <p>Credits: {limits.credits.unlimited ? 'Unlimited' : limits.credits.hasCredits ? limits.credits.balance ?? 'Available' : 'None'}</p>}
          {limits.individualLimit && <p>Spend limit: {limits.individualLimit.used} of {limits.individualLimit.limit} · {Math.round(limits.individualLimit.remainingPercent)}% remaining</p>}
          {limits.spendControlReached && <p>Provider spend limit reached.</p>}
        </div>}
      </section>
    })}
  </div>
}
