import { useEffect, useMemo, useState, type CSSProperties, type ReactNode } from 'react'
import { useAppStore } from '../../store/useAppStore'
import { ALL_MACHINES_HOST_ID, USAGE_PROVIDERS, formatUsageCount, type UsageProvider, type UsageProviderId, type UsageWindowDays } from '../../services/usageHistory'
import type { AcpContextUsage, AcpRateLimits, AcpRateLimitWindow, AcpTokenUsage } from '../../store/cpp/types'
import { CODEX_ENABLED } from '../../config/buildFeatures'
import { ANTIGRAVITY_CLI_PROVIDER_ID, CODEX_CLI_PROVIDER_ID, GEMINI_CLI_PROVIDER_ID, OPENCODE_CLI_PROVIDER_ID, providerDisabledOn, providerShortName } from '../../utils/providerMetadata'
import { useProviderAllowances } from '../../hooks/useProviderAllowances'
import { preloadProviderAllowances, type ProviderAllowance } from '../../services/providerAllowances'
import { Check, CircleAlert, Equal, Gauge, RefreshCw, Timer, TrendingUp } from 'lucide-react'
import { Button, Card, Disclosure, Notice, ProgressFill, StatusDot, Tooltip, cx, Reveal } from '../ui'
import { allowancePace, type AllowancePace } from './allowancePace'
import { ProviderLogo } from '../shared/ProviderLogo'
import { formatCreditBalance } from '../../utils/providerAllowanceFormatting'
import './usage.css'

/**
 * Bar tone: error once a window is nearly or fully used, warning when its pace runs out before the reset. Without a
 * known window length there is no pace, so the documented 80% threshold stands in for it.
 */
function barTone(percent: number, pace: AllowancePace): 'neutral' | 'warning' | 'error' {
  if (percent >= 95 || pace.verdict === 'out') return 'error'
  return pace.verdict === 'ahead' || (pace.elapsedPercent === null && percent >= 80) ? 'warning' : 'neutral'
}
/** Percent and verdict text stay neutral until the bar needs attention; warning and error text use the contrast tokens. */
const TONE_TEXT = { neutral: 'var(--text)', warning: 'var(--warning-text)', error: 'var(--error-text)' } as const
const VERDICT_ICON = { out: CircleAlert, ahead: TrendingUp, on: Equal, under: Check, unknown: null } as const

const percentFormat = new Intl.NumberFormat(undefined, { maximumFractionDigits: 1 })
const formatPercent = (percent: number) => percentFormat.format(percent)
const isValidPercent = (percent: number) => Number.isFinite(percent) && percent >= 0 && percent <= 100

export const PERIOD_PHRASE: Record<UsageWindowDays, string> = { 1: 'in the past 24 hours', 7: 'in the past 7 days', 30: 'in the past 30 days', 90: 'in the past 90 days', 0: 'in all recorded history' }

/** Token history for the selected period, the same answer the Tokens tab shows. */
export interface LimitsUsage {
  providers: UsageProvider[] | null
  loading: boolean
  collecting: boolean
  error: string
  days: UsageWindowDays
}

function resetLabel(window: AcpRateLimitWindow, now: number, awaitingUpdate: boolean) {
  if (window.resetsAt === null) return null
  if (window.resetsAt * 1000 > now) return `resets ${moment(window.resetsAt * 1000, now)}`
  return awaitingUpdate ? 'reset passed, awaiting update' : 'provider-reported reset time has passed'
}

/** Joins the parts of a window line and starts it with a capital. */
const line = (parts: (string | null | undefined)[]) => {
  const text = parts.filter(Boolean).join(' · ')
  return text.charAt(0).toUpperCase() + text.slice(1)
}

/** The plain-words pace verdict, spoken in full. */
function verdictText(pace: AllowancePace, now: number) {
  if (pace.verdict === 'out') return 'used up'
  if (pace.verdict === 'ahead') return `ahead of pace · would run out ${approx(pace.runsOutAt!, now)}`
  return pace.verdict === 'on' ? 'on pace' : pace.verdict === 'under' ? 'plenty left' : null
}
/** The verdict beside the percent: a later run-out names only its weekday or date, the caret under the bar gives the time. */
function verdictShort(pace: AllowancePace, now: number) {
  if (pace.verdict !== 'ahead') return line([verdictText(pace, now)])
  const at = pace.runsOutAt!
  return `Ahead · would run out ${at - now < DAY ? `in ~${roughSpan(at - now)}` : `~${(at - now < WEEK ? weekday : date).format(at)}`}`
}

/**
 * One window: name and reset | bar | percent and verdict. The bar fills to the used share, carries a tick at the share
 * of the window's time passed, day notches on weekly windows and, when ahead, a caret where a straight-line burn runs out.
 * An unknown share keeps the same row: an empty bar with no tick, ??% (… while reading), and why in text.
 */
function AllowanceRow({ row, now, awaitingUpdate = false, pending = false, off = false, useFirst = false }: { row: WindowRow; now: number; awaitingUpdate?: boolean; pending?: boolean; off?: boolean; useFirst?: boolean }) {
  if (row.unlimited) return <div className="uam-limit-row">
    <div className="uam-limit-row__label"><span className="uam-limit-row__name">{row.label}</span></div>
    <span />
    <div className="uam-limit-row__value"><span className="uam-limit-row__pct uam-usage-muted">Unlimited</span></div>
  </div>
  const { window, pace } = row
  if (!window || !pace) return <div className="uam-limit-row uam-limit-row--unknown">
    <div className="uam-limit-row__label" aria-hidden><span className="uam-limit-row__name">{row.label}</span>
      {/* An unreadable card names its reason once in its header, so its row only says the reset is unknown. */}
      <small className="uam-limit-row__reset">{pending ? 'Reading…' : off ? 'Resets —' : row.missingLabel ?? 'Resets —'}</small></div>
    <div className="uam-limit-row__bar"><ProgressFill variant="meter" size="lg" label={row.meterLabel} active={pending} /></div>
    <div className="uam-limit-row__value" aria-hidden><span className="uam-limit-row__pct">{pending ? '…' : '??%'}</span></div>
    <span className="sr-only">{pending ? `Reading ${row.label}` : `${row.label} unknown: ${row.missingLabel ?? 'Not readable at this time'}`}</span>
  </div>
  const percent = window.usedPercent
  const tone = barTone(percent, pace)
  const reset = resetLabel(window, now, awaitingUpdate) ?? (row.unknownReset ? 'reset time unavailable' : null)
  const passed = pace.elapsedPercent === null ? null : `${Math.round(pace.elapsedPercent)}% of the window has passed`
  const runsOut = pace.verdict === 'ahead' && pace.elapsedPercent !== null && percent > 0 ? pace.runsOutAt! : null
  // Share of the window at the run-out: the elapsed share scaled up to 100% used.
  const projection = runsOut === null ? null : { at: pace.elapsedPercent! * 100 / percent, tone: tone === 'error' ? 'error' as const : 'warning' as const,
    label: `~${runsOut - now < DAY ? clock.format(runsOut) : later(runsOut, now)} at this pace` }
  const Icon = VERDICT_ICON[pace.verdict]
  return <div className="uam-limit-row">
    <div className="uam-limit-row__label"><span className="uam-limit-row__title"><span className="uam-limit-row__name">{row.label}</span>{useFirst && <span className="uam-limits-chip uam-limit-row__marker">Use first</span>}</span>
      {reset && <Tooltip label={window.resetsAt !== null ? new Date(window.resetsAt * 1000).toLocaleString() : undefined}><small className="uam-limit-row__reset">{line([reset])}</small></Tooltip>}</div>
    <div className={cx('uam-limit-row__bar', projection && 'is-projected')}>
      <Tooltip label={passed ? `Tick: ${passed}` : undefined}><span className="uam-limit-bar">
        <ProgressFill variant="meter" size="lg" label={row.meterLabel} value={percent} marker={pace.elapsedPercent} tone={tone} projection={projection}
          segments={window.windowDurationMinutes === 10080 ? 7 : undefined} valueText={line([`${formatPercent(percent)}% used`, passed, line([verdictText(pace, now), reset])])} />
      </span></Tooltip>
    </div>
    <div className="uam-limit-row__value">
      <span className="uam-limit-row__pct" style={{ color: TONE_TEXT[tone] }}>{`${formatPercent(percent)}% `}<small>used</small></span>
      {Icon && <span className="uam-limit-verdict" style={pace.verdict === 'ahead' || pace.verdict === 'out' ? { color: TONE_TEXT[tone], fontWeight: 500 } : undefined}><Icon size={12} aria-hidden />{verdictShort(pace, now)}</span>}
    </div>
  </div>
}

type WindowRow = { id: string; label: string; meterLabel: string; window: AcpRateLimitWindow | null; pace?: AllowancePace; unknownReset?: boolean; missingLabel?: string; unlimited?: boolean; period?: boolean; bucket?: boolean }

/** Provider windows in display order, each with its pace. Model allowances replace primary/secondary when the provider reports them. */
function windowRows(provider: UsageProviderId, name: string, allowance: ProviderAllowance | undefined, now: number): WindowRow[] {
  const limits = allowance?.limits ?? undefined
  const modelWindows = allowance?.windows ?? []
  const paced = (row: WindowRow): WindowRow => row.window && isValidPercent(row.window.usedPercent) ? { ...row, pace: allowancePace(row.window, now) } : row
  if (modelWindows.length) return modelWindows.map(window => paced({ id: window.id, label: window.label, meterLabel: `${name} ${window.label} usage`, window, unknownReset: true, bucket: true }))
  // ponytail: plan-name match is an inference; drop it once Codex reports a real secondary window for Pro.
  const noFiveHourCap = CODEX_ENABLED && provider === CODEX_CLI_PROVIDER_ID && !limits?.secondary && /pro/i.test(limits?.planType ?? '')
  const rows: WindowRow[] = (['primary', 'secondary'] as const).map(key => {
    const window = limits?.[key] ?? null
    if (noFiveHourCap && key === 'secondary') return { id: key, label: '5-hour', meterLabel: '', window: null, unlimited: true }
    // A window the provider did not report has no period to name, so it is "Allowance" rather than an invented Primary/Secondary window.
    const declared = key === 'primary' ? allowance?.primaryLabel : allowance?.secondaryLabel
    return paced({ id: key, label: declared || (window ? windowLabel(window.windowDurationMinutes, key) : 'Allowance'), period: !declared && !!window, meterLabel: `${name} ${key} usage`, window, missingLabel: allowance?.status === 'ready' ? 'Not reported by provider' : undefined })
  })
  // With no window reported, one unknown allowance stands in for the pair rather than two identical rows.
  return rows.every(row => !row.window && !row.unlimited && row.label === 'Allowance') ? rows.slice(0, 1) : rows
}

/** The share left to use per millisecond before the window resets. Only for a future reset. */
const neededRate = (row: WindowRow, now: number) => (100 - row.window!.usedPercent) / (row.window!.resetsAt! * 1000 - now)
/** Least share left, in percent, for a window to be worth using first; with less there is little to lose at its reset. */
const MIN_LEFT_TO_USE_FIRST = 10
/** A window whose provider reports its length. These nest: a 5-hour allowance is spent from the weekly one. Quotas with no length (model buckets, Copilot's chat and completions) stand alone. */
const isTimeWindow = (row: WindowRow) => !!row.pace && !!row.window!.windowDurationMinutes
/** The provider's own stop, which holds whatever its windows say. */
const reportedStop = (limits: AcpRateLimits | null | undefined) => limits?.spendControlReached ? 'spend limit' : limits?.rateLimitReachedType ? 'usage limit' : null
const validUpdateTime = (limits: AcpRateLimits | null | undefined, now: number) => Boolean(limits && limits.updatedAt > 0 && limits.updatedAt <= now && Number.isFinite(new Date(limits.updatedAt).getTime()))
const CACHED_AFTER_MS = 300000
/** A cached rather than current snapshot: a failed refresh, a read not settled as ready, no valid time, or 5 minutes old. */
const snapshotStale = (allowance: ProviderAllowance | undefined, refreshFailed: boolean, now: number) => refreshFailed || allowance?.status !== 'ready' || !validUpdateTime(allowance?.limits, now) || now - allowance!.limits!.updatedAt >= CACHED_AFTER_MS
/** How long past the cached age a recommendation holds, to cover the 30-second poll and the read it starts. */
const RE_READ_GRACE_MS = 60000
/** Current enough to recommend from: read successfully, even if being read again, and within the grace period. A failed read stays out until one succeeds. */
const canRecommendFrom = (allowance: ProviderAllowance | undefined, refreshFailed: boolean, now: number) => !refreshFailed && validUpdateTime(allowance?.limits, now) && now - allowance!.limits!.updatedAt < CACHED_AFTER_MS + RE_READ_GRACE_MS
  && (allowance?.status === 'ready' || (allowance?.status === 'loading' && allowance.lastSettledStatus === 'ready'))

/** A short reason for an unreadable allowance. The native message stays whole in the card footer. */
function shortReason(allowance: ProviderAllowance | undefined, name: string): string {
  if (!allowance) return 'Not available'
  if (allowance.status === 'unavailable') return `Not offered by ${name}`
  if (allowance.status === 'ready') return 'Not reported'
  const message = allowance.error
  if (/expired/i.test(message)) return 'Login expired'
  if (/no (valid )?existing|not available for the current login/i.test(message)) return 'Not signed in'
  if (/keychain|permission/i.test(message)) return 'Needs access'
  if (/custom/i.test(message)) return 'Custom command'
  if (/timed out/i.test(message)) return 'Timed out'
  return "Couldn't read"
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
  return <div className="grid gap-2"><span className="uam-usage-stack" aria-hidden>{segments.map(([label, value, color]) => value > 0 ? <Tooltip key={label} label={`${label} · ${value.toLocaleString()}`}><span style={{ flexGrow: value, '--c': color } as CSSProperties} /></Tooltip> : null)}</span>
    <ul className="uam-usage-key text-xs" aria-hidden>{segments.map(([label, , color]) => <li key={label} className="uam-usage-key-item"><i className="uam-usage-swatch" style={{ '--c': color } as CSSProperties} />{label}</li>)}</ul>
  </div>
}

/** Period tokens for one provider; missing history is never shown as zero use. */
function periodTokens(usage: LimitsUsage | undefined, provider: UsageProviderId) {
  if (!usage) return null
  const phrase = PERIOD_PHRASE[usage.days]
  if (!usage.providers) return { short: usage.loading ? 'Reading…' : 'No history', detail: usage.loading ? 'Reading usage history.' : usage.error || 'Usage history unavailable.' }
  const row = usage.providers.find(entry => entry.providerId === provider)
  if (!row || !row.available) return { short: 'No history', detail: 'Usage history unavailable.' }
  if (row.totalTokens > 0 || row.records > 0) return { short: `${formatUsageCount(row.totalTokens)} tokens`, detail: `${row.totalTokens.toLocaleString()} tokens ${phrase}${usage.collecting ? ' so far' : ''} · ${row.sessions.toLocaleString()} sessions` }
  return usage.collecting ? { short: 'Reading…', detail: 'Still reading history.' } : { short: 'No usage', detail: `No usage ${phrase}.` }
}

/** A window's name inside a sentence: "its weekly window", "its weekly allowance"; provider names such as gemini-2.5-pro · REQUESTS keep their case. */
const inSentence = (row: WindowRow) => row.period ? `${row.label.toLowerCase()} window` : / allowance$/.test(row.label) ? row.label.toLowerCase() : row.label

/**
 * Account allowances with each provider's token use beside them. Allowances belong to accounts and are read on this
 * computer, so the page has no machine headings whatever machine is selected; tokens follow the selected machine and period.
 * A callout says which allowance to use first, then one always-open card per provider shows every window.
 */
export function UsageLimits({ executionHostId, usage }: { executionHostId: string; usage?: LimitsUsage }) {
  const allMachines = executionHostId === ALL_MACHINES_HOST_ID
  const accountHost = allMachines ? 'local' : executionHostId
  const remote = accountHost !== 'local'
  const hostLabel = useAppStore(state => state.executionHosts.find(host => host.id === executionHostId)?.label)
  const remoteHosts = useAppStore(state => state.executionHosts.some(host => host.id !== 'local'))
  const sessions = useAppStore(state => state.sessions)
  const bindings = useAppStore(state => state.acpBindingBySessionId)
  const overrides = useAppStore(state => state.providerAvailability)
  const account = useProviderAllowances(accountHost)
  const [clock, setClock] = useState(() => Date.now())
  useEffect(() => {
    const timer = setInterval(() => setClock(Date.now()), 30000)
    return () => clearInterval(timer)
  }, [])
  const now = Math.max(clock, Date.now())
  // A provider turned off on this computer is absent everywhere here, as if the build excluded it.
  const providers = useMemo(() => USAGE_PROVIDERS.filter(provider => !providerDisabledOn(overrides, provider, accountHost)), [overrides, accountHost])
  // Re-enabling reconfigures the native reader; read again now rather than showing its empty card until the next poll.
  useEffect(() => { void preloadProviderAllowances(accountHost) }, [overrides, accountHost])
  const latest = useMemo(() => {
    const contexts = new Map<string, { usage: AcpContextUsage; title: string }>()
    const tokens = new Map<string, { usage: AcpTokenUsage; title: string }>()
    for (const session of sessions) {
      if (!allMachines && (session.executionHostId || 'local') !== executionHostId) continue
      const binding = bindings[session.id]
      const provider = binding?.providerId
      if (!provider || provider !== session.providerId) continue
      const consumption = binding.providerUsage?.tokenUsage
      if (consumption && consumption.updatedAt > (tokens.get(provider)?.usage.updatedAt ?? -1)) tokens.set(provider, { usage: consumption, title: session.name })
      const context = binding.providerUsage?.contextUsage
      if (context && context.updatedAt > (contexts.get(provider)?.usage.updatedAt ?? -1)) contexts.set(provider, { usage: context, title: session.name })
    }
    return { contexts, tokens }
  }, [sessions, bindings, executionHostId, allMachines])
  const column = 'uam-usage uam-limits mx-auto grid max-w-6xl'
  if (providers.length === 0) return <div className={column}><p className="uam-usage-empty">No provider is turned on for this computer. Turn one on in Settings &gt; Providers.</p></div>

  const entries = providers.map(provider => {
    const name = providerShortName(undefined, provider)
    const allowance = account.data?.providers.find(row => row.providerId === provider)
    const reading = account.loading || allowance?.status === 'loading'
    // Unreadable: nothing known and nothing still being read. These cards are greyed and grouped last.
    const off = !reading && !(allowance?.limits || allowance?.windows?.length)
    return { provider, name, allowance, rows: windowRows(provider, name, allowance, now), off }
  })

  // Use it or lose it: the most you need to use in the shortest time, among current windows with allowance left that can still be spent. A time window
  // is capped by any other time window on its provider that has run low, unless that one's reset has passed. On a tie, the most left.
  const readable = entries.flatMap(entry => entry.rows.filter(row => row.pace).map(row => ({ entry, row })))
  const resetLive = (row: WindowRow) => row.window!.resetsAt === null || row.window!.resetsAt * 1000 > now
  const capped = (entry: typeof entries[number], row: WindowRow) => isTimeWindow(row) && entry.rows.some(other => other !== row && isTimeWindow(other) && resetLive(other) && 100 - other.window!.usedPercent < MIN_LEFT_TO_USE_FIRST)
  // A stop needs the same proof as a recommendation: native keeps these fields through failed reads. It has no reset of its own, so it lapses with its provider's windows.
  const stopRunning = (entry: typeof entries[number]) => !entry.rows.some(row => row.pace) || entry.rows.some(row => row.pace && resetLive(row))
  const currentStop = (entry: typeof entries[number]) => canRecommendFrom(entry.allowance, Boolean(account.error), now) && stopRunning(entry) ? reportedStop(entry.allowance?.limits) : null
  // A stop on data too old to confirm is not repeated as fact, but the page must not claim room over it either.
  const unconfirmed = entries.find(entry => reportedStop(entry.allowance?.limits) && stopRunning(entry) && !currentStop(entry))
  const lastReported = unconfirmed && `${unconfirmed.name} last reported its ${reportedStop(unconfirmed.allowance?.limits)}`
  const useFirst = readable.filter(({ entry, row }) => canRecommendFrom(entry.allowance, Boolean(account.error), now) && !currentStop(entry) && !capped(entry, row) && (row.window!.resetsAt ?? 0) * 1000 > now && 100 - row.window!.usedPercent >= MIN_LEFT_TO_USE_FIRST)
    .sort((a, b) => neededRate(b.row, now) - neededRate(a.row, now) || a.row.window!.usedPercent - b.row.window!.usedPercent)[0]
  const stoppedEntry = useFirst ? undefined : entries.find(currentStop)
  // A reset that has passed says nothing about now, so its window cannot be called used up or close to its limit.
  const top = useFirst ?? readable.filter(({ row }) => resetLive(row)).sort((a, b) => b.row.window!.usedPercent - a.row.window!.usedPercent)[0]
  // What cannot be spent stays in view under the headline; ahead-of-pace windows speak on their own rows.
  const others = [
    ...entries.filter(currentStop).map(entry => ({ entry, row: null })),
    ...readable.filter(({ row }) => resetLive(row) && row.pace!.verdict === 'out'),
  ].filter(item => stoppedEntry ? item.row || item.entry !== stoppedEntry : item.row !== top?.row).slice(0, 2)
  const resetIn = (window: AcpRateLimitWindow) => window.resetsAt !== null && window.resetsAt * 1000 > now ? moment(window.resetsAt * 1000, now) : null
  const shownUsage = usage?.providers?.filter(row => providers.includes(row.providerId))
  const periodTotal = shownUsage?.reduce((sum, row) => sum + row.totalTokens, 0)
  const where = allMachines && remoteHosts ? ' across all machines' : remote ? ` on ${hostLabel || 'that computer'}` : ''
  const showTotal = periodTotal !== undefined && (periodTotal > 0 || !usage!.collecting)

  let title: string, body: ReactNode = null, tone: 'neutral' | 'warning' | 'error' = 'neutral'
  if (stoppedEntry) {
    title = `${stoppedEntry.name} has reached its ${reportedStop(stoppedEntry.allowance?.limits)}`
    tone = 'error'
  } else if (top) {
    const { entry, row } = top
    const window = row.window!, pace = row.pace!, reset = resetIn(window), percent = formatPercent(window.usedPercent)
    // A model bucket speaks for itself, not for the provider's other buckets.
    const bucket = row.bucket ? ` on ${row.label}` : ''
    if (useFirst) {
      title = `Use ${entry.name} first`
      if (others.length) tone = 'error'
      body = <>It has <b>{formatPercent(100 - window.usedPercent)}%</b> of its {inSentence(row)} left, resetting <b>{resetWhen(window.resetsAt! * 1000, now)}</b>.</>
    } else if (window.usedPercent >= 80) {
      title = pace.verdict === 'out' ? `${entry.name} is used up${bucket}` : `${entry.name} is close to its limit${bucket}`
      tone = barTone(window.usedPercent, pace)
      tone = tone === 'neutral' ? 'warning' : tone
      body = `${percent}% of its ${inSentence(row)} used${reset ? `, resets ${reset}` : ''}.`
    } else {
      title = lastReported || 'Allowances have room'
      body = `Highest is ${entry.name} at ${percent}% of its ${inSentence(row)}${reset ? `, resets ${reset}` : ''}.`
    }
  } else if (readable.length) {
    title = 'Allowances are awaiting an update'
    body = 'Their reset times have passed.'
  } else if (lastReported) title = lastReported
  else if (entries.some(entry => entry.rows.some(row => row.unlimited))) title = 'Allowances have room'
  else if (account.loading && !account.data) title = 'Reading account allowances…'
  else if (remote) title = 'Allowances are read on this computer only'
  else {
    title = 'No allowances to show'
    body = `${providers.length === 1 ? entries[0].name : 'Providers'} didn’t share account limits on this computer; each card says why.`
  }
  const newest = Math.max(0, ...entries.map(entry => entry.allowance?.limits?.updatedAt ?? 0).filter(at => at <= now))
  const card = (entry: typeof entries[number]) => <ProviderLimitCard key={entry.provider} {...entry} useFirstRow={useFirst?.entry === entry ? useFirst.row.id : undefined} loading={account.loading} refreshFailed={Boolean(account.error)} remote={remote}
    context={latest.contexts.get(entry.provider)} consumption={latest.tokens.get(entry.provider)} tokens={periodTokens(usage, entry.provider)} now={now} />
  const live = entries.filter(entry => !entry.off), off = entries.filter(entry => entry.off)

  return <div className={column}>
    <Card className="uam-limits-lede" data-tone={tone === 'neutral' ? undefined : tone}>
      <span className="uam-limits-lede__icon" aria-hidden>{useFirst ? <Timer size={17} /> : <Gauge size={17} />}</span>
      <div className="min-w-0">
        <h2 className="uam-limits-lede__title">{title}</h2>
        {body && <p className="uam-limits-lede__body">{body}</p>}
        {others.length > 0 && <ul className="uam-limits-lede__more">{others.map(({ entry, row }) => {
          const reset = row && resetIn(row.window!)
          return <li key={`${entry.provider}:${row?.id ?? 'stop'}`}><ProviderLogo providerId={entry.provider} size={14} /><span>{row
            ? <><b>{entry.name} · {row.label}</b> is used up{reset ? `, resets ${reset}` : ''}</>
            : <><b>{entry.name}</b> has reached its {reportedStop(entry.allowance?.limits)}</>}</span></li>
        })}</ul>}
      </div>
      {showTotal && <p className="uam-limits-lede__aside"><strong>{formatUsageCount(periodTotal!)}</strong>tokens {usage ? PERIOD_PHRASE[usage.days] : ''}{where}</p>}
    </Card>
    <div className="uam-limits-group">
      <h2 className="uam-usage-label">Allowances · read on this computer</h2>
      {!remote && <>
        <Reveal as="span" open={account.coolingDown}><span className="uam-limits-updated">Updated moments ago; refresh available shortly.</span></Reveal>
        {newest > 0 && !account.coolingDown && <span className="uam-limits-updated">Updated {relativeAgo(newest, now)}</span>}
        <Button size="sm" variant="secondary" leadingIcon={<RefreshCw size={13} aria-hidden />} onClick={account.refresh} disabled={account.coolingDown} loading={account.refreshing || account.requestingRefresh}>Refresh allowances</Button>
      </>}
    </div>
    {entries.some(entry => entry.rows.some(row => row.pace)) && <ul className="uam-limits-key" aria-hidden>
      <li><b />Used</li><li><i />Time passed in the window</li><li><u />Run-out at this pace</li>
    </ul>}
    <Reveal open={!!account.error}><Notice tone="warning" title={account.error} dismissLabel="Dismiss allowance error" dismissible={false}>{account.data ? 'Showing the last snapshot.' : null}</Notice></Reveal>
    <div className="uam-limits-cards">
      {live.map(card)}
      {/* Unreadable providers sit last, two to a row once there are two of them. */}
      {off.length > 1 ? <div className="uam-limits-off">{off.map(card)}</div> : off.map(card)}
    </div>
  </div>
}

/** One always-open card per provider: header, every window, then credits, spend and the collapsed context and tokens. */
function ProviderLimitCard({ provider, name, allowance, rows, off, useFirstRow, loading, refreshFailed, remote, context, consumption, tokens, now }: {
  provider: UsageProviderId
  name: string
  allowance: ProviderAllowance | undefined
  rows: WindowRow[]
  off: boolean
  useFirstRow: string | undefined
  loading: boolean
  refreshFailed: boolean
  remote: boolean
  context: { usage: AcpContextUsage; title: string } | undefined
  consumption: { usage: AcpTokenUsage; title: string } | undefined
  tokens: ReturnType<typeof periodTokens>
  now: number
}) {
  const limits = allowance?.limits ?? undefined
  const modelWindows = allowance?.windows ?? []
  const hasAllowance = Boolean(limits || modelWindows.length)
  const reading = loading || allowance?.status === 'loading'
  const plan = limits?.planType || limits?.limitName
  const consumptionTimeValid = Boolean(consumption && consumption.usage.updatedAt > 0 && consumption.usage.updatedAt <= now && Number.isFinite(new Date(consumption.usage.updatedAt).getTime()))
  const timestampValid = validUpdateTime(limits, now)
  const stale = hasAllowance && snapshotStale(allowance, refreshFailed, now)
  const contextPercent = context ? Math.min(100, Math.max(0, context.usage.usedTokens / context.usage.windowTokens * 100 || 0)) : 0
  const reason = shortReason(allowance, name)
  // A remote computer's unsupported feed is explained once in the callout, so its card names no reason.
  const remoteUnsupported = remote && (!allowance || allowance.status === 'unavailable')
  const shown = off ? [{ ...rows[0], window: null, pace: undefined, missingLabel: remoteUnsupported ? 'read on this computer only' : reason }] : rows
  const renderRow = (row: WindowRow) => <AllowanceRow key={row.id} row={row} now={now} awaitingUpdate={stale || loading} pending={reading} off={off} useFirst={row.id === useFirstRow} />
  // The recommended bucket stays in view even when it sorts past the first two.
  const collapsed = modelWindows.length ? shown.filter((row, index) => index >= 2 && row.id !== useFirstRow) : []
  const spend = limits?.individualLimit
  const spendUsed = spend ? 100 - spend.remainingPercent : 0
  return <Card role="group" aria-label={name} elevation={off ? 0 : 1} className={cx('uam-limit-card', off && 'uam-limit-card--off')}>
    <header className="uam-limit-card__head">
      <h3 className="uam-limit-card__name"><ProviderLogo providerId={provider} size={18} className="uam-limit-card__logo inline-flex shrink-0" /><span className="truncate">{name}</span></h3>
      <Reveal as="span" open={!!(hasAllowance && plan)}><span className="uam-limits-chip">{plan}</span></Reveal>
      <span className="uam-limit-card__meta">
        {tokens && <Tooltip label={tokens.detail}><span>{tokens.short}</span></Tooltip>}
        {hasAllowance
          ? <Tooltip label={timestampValid ? new Date(limits!.updatedAt).toLocaleString() : undefined}><span>{timestampValid ? `Updated ${relativeAgo(limits!.updatedAt, now)}` : 'Update time unavailable'}{stale ? ' · Cached snapshot' : ''}</span></Tooltip>
          : off && !remoteUnsupported ? <span className="uam-limit-card__reason">{reason}</span> : null}
      </span>
    </header>
    {shown.filter(row => !collapsed.includes(row)).map(renderRow)}
    {collapsed.length > 0 && <Disclosure className="uam-limits-more" summary={`${collapsed.length} more model allowance${collapsed.length > 1 ? 's' : ''}`}>{collapsed.map(renderRow)}</Disclosure>}
    {spend && <div className="uam-limit-row">
      <div className="uam-limit-row__label"><span className="uam-limit-row__name">Spend limit</span><small className="uam-limit-row__reset">{spend.used} of {spend.limit}</small></div>
      <div className="uam-limit-row__bar"><ProgressFill variant="meter" size="lg" label={`${name} spend limit usage`} value={spendUsed} tone={spendUsed >= 95 ? 'error' : spendUsed >= 80 ? 'warning' : 'neutral'} /></div>
      <div className="uam-limit-row__value"><span className="uam-limit-row__pct">{`${Math.round(spendUsed)}% `}<small>used</small></span></div>
    </div>}
    <footer className="uam-limit-card__foot">
      <div className="uam-limit-card__facts">
        {limits?.credits && <span title={limits.credits.balance ?? undefined}>Credits: {limits.credits.unlimited ? 'Unlimited' : limits.credits.hasCredits ? limits.credits.balance === null ? 'Available' : formatCreditBalance(limits.credits.balance) : 'None'}</span>}
        <Reveal as="span" open={!!limits?.spendControlReached}><span className="inline-flex items-center gap-1.5 uam-limits-notice"><StatusDot tone="error" />Spend limit reached.</span></Reveal>
        {/* An unsupported feed is an honest neutral state; only failed reads carry the warning mark. */}
        <Reveal as="span" open={!!allowance?.error}><span className={cx('inline-flex items-start gap-1.5', allowance?.status === 'unavailable' ? 'uam-usage-muted' : 'uam-limits-notice')}>{allowance?.status !== 'unavailable' && <StatusDot tone="warning" className="mt-1 shrink-0" />}{allowance?.error}{hasAllowance ? ' Showing cached allowances.' : ''}</span></Reveal>
        {off && !allowance?.error && allowance?.status !== 'ready' && <span>Account allowances unavailable.</span>}
      </div>
      {(context || consumption) && <Disclosure className="uam-limits-tokens" summary={<>Context and tokens{consumption && <span className="tabular-nums">{compactTokens(consumption.usage.total.totalTokens)}</span>}</>}>
        <div className="grid gap-3">
          {context && <ProgressFill label={`${name} context usage`} value={contextPercent} className="uam-limit-context">
            <span>Context</span>
            <span className="tabular-nums">{context.usage.usedTokens.toLocaleString()} of {context.usage.windowTokens.toLocaleString()} tokens</span>
          </ProgressFill>}
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
            <p className="uam-usage-muted">{(provider === GEMINI_CLI_PROVIDER_ID || provider === OPENCODE_CLI_PROVIDER_ID || provider === ANTIGRAVITY_CLI_PROVIDER_ID) ? 'Tokens observed while attached to' : 'Tokens consumed in'} {consumption.title || 'chat'}: {consumption.usage.total.totalTokens.toLocaleString()}{consumptionTimeValid ? ` · ${new Date(consumption.usage.updatedAt).toLocaleString()}` : ' · Update time unavailable'}{!hasAllowance && (!consumptionTimeValid || now - consumption.usage.updatedAt >= 300000) ? ' · Cached snapshot' : ''}</p>
          </>}
        </div>
      </Disclosure>}
    </footer>
  </Card>
}

function windowLabel(minutes: number | null | undefined, key: 'primary' | 'secondary') {
  if (!minutes) return key === 'primary' ? 'Primary' : 'Secondary'
  if (minutes === 10080) return 'Weekly'
  if (minutes === 1440) return 'Daily'
  if (minutes % 1440 === 0) return `${minutes / 1440}-day`
  return `${+(minutes / 60).toFixed(1)}-hour`
}

function span(ms: number) {
  const minutes = Math.max(1, Math.round(ms / 60000))
  if (minutes < 60) return `${minutes}m`
  const hours = Math.floor(minutes / 60)
  if (hours < 48) return minutes % 60 ? `${hours}h ${minutes % 60}m` : `${hours}h`
  return `${Math.round(hours / 24)}d`
}
/** How far off an exact moment is, without rounding the last seconds up to a minute. */
const inSpan = (ms: number) => ms < 60000 ? 'in under 1m' : `in ${span(ms)}`
const DAY = 86400000
const WEEK = 7 * DAY
const dayTime = new Intl.DateTimeFormat(undefined, { weekday: 'short', hour: 'numeric', minute: '2-digit' })
const weekday = new Intl.DateTimeFormat(undefined, { weekday: 'short' })
const clock = new Intl.DateTimeFormat(undefined, { hour: 'numeric', minute: '2-digit' })
const date = new Intl.DateTimeFormat(undefined, { day: 'numeric', month: 'short' })
const dateTime = new Intl.DateTimeFormat(undefined, { day: 'numeric', month: 'short', hour: 'numeric', minute: '2-digit' })
/** A weekday can only mean the next one within a week; past that, a 30-day reset needs its date. */
const later = (at: number, now: number) => (at - now < WEEK ? dayTime : dateTime).format(at)
/** A future moment in words: relative within a day, otherwise its weekday or date and time. */
const moment = (at: number, now: number) => at - now < DAY ? inSpan(at - now) : later(at, now)
/** How far off a projected moment is: minutes only under 3 hours, and days rounded down so 60h beside a weekday reads ~2d. */
function roughSpan(ms: number) {
  const hours = Math.floor(ms / 3600000)
  return hours < 3 ? span(ms) : hours < 48 ? `${hours}h` : `${Math.floor(hours / 24)}d`
}
/** A known reset that the reader may lose allowance to, always saying how far off it is. */
const resetWhen = (at: number, now: number) => at - now < DAY ? inSpan(at - now) : `${later(at, now)} (in ~${roughSpan(at - now)})`
/** A projected moment, marked as approximate, always saying how far off it is. */
const approx = (at: number, now: number) => at - now < DAY ? `in ~${roughSpan(at - now)}` : `~${later(at, now)} (in ~${roughSpan(at - now)})`
const relativeAgo = (at: number, now: number) => now - at < 60000 ? 'just now' : `${span(now - at)} ago`
const compactTokens = (value: number) => value >= 1e6 ? `${+(value / 1e6).toFixed(1)}M` : value >= 1e3 ? `${+(value / 1e3).toFixed(1)}k` : `${value}`
