import { useState } from 'react'
import { Gauge } from 'lucide-react'
import type { AcpRateLimits } from '../../store/cpp/types'
import { Tooltip } from '../ui'

type Props = {
  providerId: string
  sourceProviderId: string | undefined
  rateLimits?: AcpRateLimits | null
  running: boolean
  nowMs?: number
}

const validPercent = (value: unknown): value is number => typeof value === 'number' && Number.isFinite(value) && value >= 0 && value <= 100
const resetLabel = (seconds: number | null) => typeof seconds === 'number' && Number.isFinite(seconds) && seconds > 0 && Number.isFinite(new Date(seconds * 1000).getTime())
  ? `Resets ${new Date(seconds * 1000).toLocaleString()}` : 'Reset time unavailable'

/** Displays only account limits reported by the matching native session. Token counts are a separate source. */
export function ProviderAllowanceIndicator({ providerId, sourceProviderId, rateLimits, running, nowMs }: Props) {
  const [interactionTime, setInteractionTime] = useState(() => Date.now())
  const currentTime = nowMs ?? Math.max(interactionTime, Date.now())
  const limits = providerId === 'codex-cli' && sourceProviderId === providerId ? rateLimits : null
  const windows = limits ? [['Primary', limits.primary], ['Secondary', limits.secondary]] as const : []
  const remaining = windows.flatMap(([, window]) => window && validPercent(window.usedPercent) ? [100 - window.usedPercent] : [])
  if (limits?.individualLimit && validPercent(limits.individualLimit.remainingPercent)) remaining.push(limits.individualLimit.remainingPercent)
  if (limits?.spendControlReached === true) remaining.push(0)
  const timestampValid = Boolean(limits && Number.isFinite(limits.updatedAt) && limits.updatedAt > 0 && limits.updatedAt <= currentTime)
  const ageMinutes = timestampValid && limits ? Math.floor((currentTime - limits.updatedAt) / 60_000) : 0
  const resetPassed = windows.some(([, window]) => window?.resetsAt && Number.isFinite(window.resetsAt) && window.resetsAt * 1000 <= currentTime)
  const cached = !running || ageMinutes >= 5 || resetPassed || !timestampValid
  const percent = remaining.length ? Math.floor(Math.min(...remaining)) : null
  const creditOnly = limits?.credits?.unlimited ? 'Unlimited' : limits?.credits?.hasCredits ? 'Credits available' : null
  const value = percent === null ? creditOnly : `${percent}% remaining`
  const label = limits && timestampValid && value ? `${cached ? 'Cached ' : ''}${value}` : 'Unavailable'
  const detail = limits ? <div className="flex flex-col gap-1">
    <span>Codex session-reported account allowance</span>
    <span>{running ? 'Current session snapshot' : 'Last session snapshot'}</span>
    <span>{timestampValid ? ageMinutes ? `Updated ${ageMinutes} min ago${ageMinutes >= 5 ? ' (stale)' : ''}` : 'Updated just now' : 'Update time unavailable'}</span>
    {resetPassed && <span>Reset time passed; awaiting a new snapshot</span>}
    {windows.map(([name, window]) => window && <span key={name}>{name}: {validPercent(window.usedPercent) ? `${Math.floor(100 - window.usedPercent)}% remaining` : 'Usage unavailable'}. {resetLabel(window.resetsAt)}</span>)}
    {limits.credits && <span>{limits.credits.unlimited ? 'Unlimited credits' : limits.credits.hasCredits ? `Credits: ${limits.credits.balance ?? 'balance unavailable'}` : 'No credits available'}</span>}
    {limits.individualLimit && <span>Spend limit: {limits.individualLimit.used} of {limits.individualLimit.limit}. {resetLabel(limits.individualLimit.resetsAt)}</span>}
    {limits.spendControlReached && <span>Spend limit reached</span>}
    {limits.planType && <span>Plan: {limits.planType}</span>}
  </div> : <span>{providerId === 'codex-cli' ? 'Codex has not reported an account allowance for this session.' : 'This provider does not expose account allowances in UAM.'}</span>
  return <Tooltip label={detail}>
    <button type="button" onFocus={() => setInteractionTime(Date.now())} onMouseEnter={() => setInteractionTime(Date.now())} aria-label={`Account allowance: ${label}`} className="inline-flex shrink-0 items-center gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
      <Gauge size={13} aria-hidden />
      {label !== 'Unavailable' && <span>{label}</span>}
    </button>
  </Tooltip>
}
