import { useMemo } from 'react'
import { useAppStore } from '../../store/useAppStore'
import { USAGE_PROVIDERS } from '../../services/usageHistory'
import type { AcpContextUsage, AcpRateLimits } from '../../store/cpp/types'
import { providerShortName } from '../../utils/providerMetadata'
import { ProviderLogo } from '../shared/ProviderLogo'

/** Show the latest provider-reported allowance for this host without launching or changing a chat. */
export function UsageLimits({ executionHostId }: { executionHostId: string }) {
  const sessions = useAppStore(state => state.sessions)
  const bindings = useAppStore(state => state.acpBindingBySessionId)
  const latest = useMemo(() => {
    const result = new Map<string, AcpRateLimits>()
    const contexts = new Map<string, { usage: AcpContextUsage; title: string }>()
    for (const session of sessions) {
      if ((session.executionHostId || 'local') !== executionHostId) continue
      const binding = bindings[session.id]
      const limits = binding?.providerUsage?.rateLimits
      const provider = binding?.providerId
      if (!provider || provider !== session.providerId) continue
      const context = binding.providerUsage?.contextUsage
      if (context && context.updatedAt > (contexts.get(provider)?.usage.updatedAt ?? -1)) contexts.set(provider, { usage: context, title: session.name })
      if (limits && limits.updatedAt > (result.get(provider)?.updatedAt ?? -1)) result.set(provider, limits)
    }
    return { limits: result, contexts }
  }, [sessions, bindings, executionHostId])
  const now = Date.now()
  return <div className="mx-auto grid max-w-6xl gap-6"><p className="text-xs">Latest limits reported by chats on this host. Providers update them while a chat is connected.</p>
    {USAGE_PROVIDERS.map(provider => {
      const limits = latest.limits.get(provider)
      const context = latest.contexts.get(provider)
      const timestampValid = Boolean(limits && limits.updatedAt > 0 && limits.updatedAt <= now && Number.isFinite(new Date(limits.updatedAt).getTime()))
      const stale = Boolean(limits && (!timestampValid || now - limits.updatedAt >= 300000))
      return <section key={provider} className="grid gap-3 border-b pb-5" style={{ borderColor: 'var(--border)' }}>
        <h2 className="flex items-center gap-2 text-sm font-medium"><ProviderLogo providerId={provider} size={17} />{providerShortName(undefined, provider)}</h2>
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
