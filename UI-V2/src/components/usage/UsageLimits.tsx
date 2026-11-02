import { useEffect, useMemo, useState, type CSSProperties, type ReactNode } from 'react'
import { useAppStore } from '../../store/useAppStore'
import { ALL_MACHINES_HOST_ID, USAGE_PROVIDERS, formatUsageCount, type UsageProvider, type UsageProviderId, type UsageWindowDays } from '../../services/usageHistory'
import type { AcpContextUsage, AcpRateLimitWindow, AcpTokenUsage } from '../../store/cpp/types'
import { CODEX_ENABLED } from '../../config/buildFeatures'
import { ANTIGRAVITY_CLI_PROVIDER_ID, CODEX_CLI_PROVIDER_ID, GEMINI_CLI_PROVIDER_ID, OPENCODE_CLI_PROVIDER_ID, providerDisabledOn, providerShortName } from '../../utils/providerMetadata'
import { useProviderAllowances } from '../../hooks/useProviderAllowances'
import { preloadProviderAllowances, type ProviderAllowance } from '../../services/providerAllowances'
import { Button, Disclosure, Notice, ProgressFill, StatusDot, Tooltip, cx, Reveal } from '../ui'
import { allowancePace, type AllowancePace } from './allowancePace'
import { SettingsSection } from '../settings/SettingsPrimitives'
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
/** Percent text stays neutral until the bar needs attention. */
const TONE_TEXT = { neutral: 'var(--text)', warning: 'var(--warning)', error: 'var(--error)' } as const

const percentFormat = new Intl.NumberFormat(undefined, { maximumFractionDigits: 1 })
const formatPercent = (percent: number) => percentFormat.format(percent)
const isValidPercent = (percent: number) => Number.isFinite(percent) && percent >= 0 && percent <= 100

const PERIOD_PHRASE: Record<UsageWindowDays, string> = { 1: 'in the past 24 hours', 7: 'in the past 7 days', 30: 'in the past 30 days', 90: 'in the past 90 days', 0: 'in all recorded history' }

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

/** The plain-words pace verdict for a window. Compact rows name the run-out time without the "ahead of pace" lead. */
function verdictText(pace: AllowancePace, now: number, compact = false) {
  if (pace.verdict === 'out') return 'used up'
  if (pace.verdict === 'ahead') return `${compact ? '' : 'ahead of pace · '}runs out ${approx(pace.runsOutAt!, now)}`
  return pace.verdict === 'on' ? 'on pace' : pace.verdict === 'under' ? 'plenty left' : null
}

/**
 * One window's text, bar and percent: the shared look of the compact provider row and each expanded window. The bar
 * fills to the used share and carries a tick at the share of the window's time already passed; an unknown share keeps
 * the same empty, greyed bar with ??% (… while reading) and says why in text.
 */
function WindowMeter({ row, prefix, now, awaitingUpdate = false, compact = false, unknownText = '—', unknownDetail, pending = false }: { row: WindowRow; prefix?: string; now: number; awaitingUpdate?: boolean; compact?: boolean; unknownText?: string | null; unknownDetail?: string; pending?: boolean }) {
  const { window, pace } = row
  if (!window || !pace) {
    return <>
      <Tooltip label={unknownDetail}><small className="uam-limit-row__reset" aria-hidden>{line([prefix, unknownText])}</small></Tooltip>
      <ProgressFill variant="meter" label={row.meterLabel} active={pending} className="uam-limit-bar" />
      <span className="uam-limit-row__value uam-usage-muted" aria-hidden>{pending ? '…' : '??%'}</span>
      <span className="sr-only">{pending ? `Reading ${row.label}` : `${row.label} unknown: ${row.missingLabel ?? 'Not readable at this time'}`}</span>
    </>
  }
  const percent = window.usedPercent
  const tone = barTone(percent, pace)
  const reset = resetLabel(window, now, awaitingUpdate) ?? (row.unknownReset ? 'reset time unavailable' : null)
  const full = line([verdictText(pace, now), reset])
  const passed = pace.elapsedPercent === null ? null : `${Math.round(pace.elapsedPercent)}% of the window has passed`
  return <>
    <Tooltip label={line([full, window.resetsAt !== null ? new Date(window.resetsAt * 1000).toLocaleString() : null])}>
      {/* The compact row names only the run-out time when there is one; the reset is in the tooltip and the expanded window. */}
      <small className="uam-limit-row__reset">{line([prefix, verdictText(pace, now, compact), compact && pace.verdict === 'ahead' ? null : reset])}</small>
    </Tooltip>
    {/* Inside the provider row's button a meter role is flattened, so the compact bar is decorative; its text says the same. */}
    <Tooltip label={passed ? `Tick: ${passed}` : undefined}><span className="uam-limit-bar" aria-hidden={compact || undefined}>
      <ProgressFill variant="meter" label={row.meterLabel} value={percent} marker={pace.elapsedPercent} tone={tone}
        valueText={line([`${formatPercent(percent)}% used`, passed, full])} />
    </span></Tooltip>
    <span className="uam-limit-row__value" style={{ color: TONE_TEXT[tone] }}>{`${formatPercent(percent)}% used`}</span>
  </>
}

/** One expanded window: its name, then the shared text, bar and percent. */
function AllowanceRow({ row, now, awaitingUpdate, pending }: { row: WindowRow; now: number; awaitingUpdate: boolean; pending: boolean }) {
  if (row.unlimited) return <div className="uam-limit-row">
    <span className="uam-limit-row__name">{row.label}</span>
    <span className="uam-limit-row__value uam-usage-muted">Unlimited</span>
  </div>
  return <div className={cx('uam-limit-row', !row.pace && 'uam-limit-row--unknown')}>
    <span className="uam-limit-row__name">{row.label}</span>
    <WindowMeter row={row} now={now} awaitingUpdate={awaitingUpdate} pending={pending} />
  </div>
}

type WindowRow = { id: string; label: string; meterLabel: string; window: AcpRateLimitWindow | null; pace?: AllowancePace; unknownReset?: boolean; missingLabel?: string; unlimited?: boolean }

/** Provider windows in display order, each with its pace. Model allowances replace primary/secondary when the provider reports them. */
function windowRows(provider: UsageProviderId, name: string, allowance: ProviderAllowance | undefined, now: number): WindowRow[] {
  const limits = allowance?.limits ?? undefined
  const modelWindows = allowance?.windows ?? []
  const paced = (row: WindowRow): WindowRow => row.window && isValidPercent(row.window.usedPercent) ? { ...row, pace: allowancePace(row.window, now) } : row
  if (modelWindows.length) return modelWindows.map(window => paced({ id: window.id, label: window.label, meterLabel: `${name} ${window.label} usage`, window, unknownReset: true }))
  // ponytail: plan-name match is an inference; drop it once Codex reports a real secondary window for Pro.
  const noFiveHourCap = CODEX_ENABLED && provider === CODEX_CLI_PROVIDER_ID && !limits?.secondary && /pro/i.test(limits?.planType ?? '')
  const rows: WindowRow[] = (['primary', 'secondary'] as const).map(key => {
    const window = limits?.[key] ?? null
    if (noFiveHourCap && key === 'secondary') return { id: key, label: '5-hour', meterLabel: '', window: null, unlimited: true }
    // A window the provider did not report has no period to name, so it is "Allowance" rather than an invented Primary/Secondary window.
    return paced({ id: key, label: (key === 'primary' ? allowance?.primaryLabel : allowance?.secondaryLabel) || (window ? windowLabel(window.windowDurationMinutes, key) : 'Allowance'), meterLabel: `${name} ${key} usage`, window, missingLabel: allowance?.status === 'ready' ? 'Not reported by provider' : undefined })
  })
  // With no window reported, one unknown allowance stands in for the pair rather than two identical rows.
  return rows.every(row => !row.window && !row.unlimited && row.label === 'Allowance') ? rows.slice(0, 1) : rows
}

/** Most pressing first: used up, then the earliest projected run-out, then the most used. Readable windows only. */
const urgency = (row: WindowRow) => row.pace!.verdict === 'out' ? 0 : row.pace!.verdict === 'ahead' ? 1 : 2
const pressing = (a: WindowRow, b: WindowRow) => urgency(a) - urgency(b) || (a.pace!.runsOutAt ?? 0) - (b.pace!.runsOutAt ?? 0) || b.window!.usedPercent - a.window!.usedPercent

/** The window that matters most for a provider: its most pressing readable window, else an unlimited one. */
const leadRow = (rows: WindowRow[]) => rows.filter(row => row.pace).sort(pressing)[0] ?? rows.find(row => row.unlimited)

/** A short reason for an unreadable allowance. The native message stays whole in the tooltip and the row details. */
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
  if (!usage.providers) return { short: usage.loading ? 'Reading…' : 'No history', detail: usage.loading ? 'Reading usage history.' : usage.error || 'Usage history unavailable.', row: undefined }
  const row = usage.providers.find(entry => entry.providerId === provider)
  if (!row || !row.available) return { short: 'No history', detail: 'Usage history unavailable.', row: undefined }
  if (row.totalTokens > 0 || row.records > 0) return { short: `${formatUsageCount(row.totalTokens)} tokens`, detail: `${row.totalTokens.toLocaleString()} tokens ${phrase}${usage.collecting ? ' so far' : ''} · ${row.sessions.toLocaleString()} sessions`, row }
  return usage.collecting ? { short: 'Reading…', detail: 'Still reading history.', row: undefined } : { short: 'No usage', detail: `No usage ${phrase}.`, row: undefined }
}

/**
 * Account allowances with each provider's token use beside them. Allowances belong to accounts and are read on this
 * computer, so the page is one card whatever machine is selected; tokens follow the selected machine and period.
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
  // Re-enabling reconfigures the native reader; read again now rather than showing its empty row until the next poll.
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
  const column = 'uam-usage mx-auto grid max-w-6xl gap-4'
  if (providers.length === 0) return <div className={column}><p className="uam-limits-lede">No provider is turned on for this computer. Turn one on in Settings &gt; Providers.</p></div>

  const entries = providers.map(provider => {
    const name = providerShortName(undefined, provider)
    const allowance = account.data?.providers.find(row => row.providerId === provider)
    const rows = windowRows(provider, name, allowance, now)
    return { provider, name, allowance, rows, lead: leadRow(rows) }
  })
  const top = entries.filter(entry => entry.lead?.pace).sort((a, b) => pressing(a.lead!, b.lead!))[0]
  const topWindow = top?.lead?.window
  const topPace = top?.lead?.pace
  const topReset = topWindow && topWindow.resetsAt !== null && topWindow.resetsAt * 1000 > now ? moment(topWindow.resetsAt * 1000, now) : null
  const shownUsage = usage?.providers?.filter(row => providers.includes(row.providerId))
  const periodTotal = shownUsage?.reduce((sum, row) => sum + row.totalTokens, 0)
  const phrase = usage ? PERIOD_PHRASE[usage.days] : ''
  const where = allMachines && remoteHosts ? ' across all machines' : remote ? ` on ${hostLabel || 'that computer'}` : ''
  const tokenSentence = periodTotal === undefined ? null : periodTotal > 0 ? ` ${formatUsageCount(periodTotal)} tokens used ${phrase}${where}.` : usage!.collecting ? null : ` No token use ${phrase}${where}.`
  const headline: ReactNode = topWindow && topPace && top?.lead
    ? topPace.verdict === 'ahead'
      ? <><b>{top.name} will run out {approx(topPace.runsOutAt!, now)}</b>, before its {inSentence(top.lead.label)} resets {topReset}.</>
      : topWindow.usedPercent >= 80
      ? <><b>{top.name} is close to its limit.</b> {formatPercent(topWindow.usedPercent)}% of its {inSentence(top.lead.label)} used{topReset ? `, resets ${topReset}` : ''}.</>
      : <><b>Allowances have room.</b> Highest is {top.name} at {formatPercent(topWindow.usedPercent)}% of its {inSentence(top.lead.label)}{topReset ? `, resets ${topReset}` : ''}.</>
    : entries.some(entry => entry.lead?.unlimited) ? <><b>Allowances have room.</b></>
    : account.loading && !account.data ? <>Reading account allowances…</>
    : remote ? <><b>Allowances are read on this computer only.</b></>
    : <><b>No allowances to show.</b> {providers.length === 1 ? entries[0].name : 'Providers'} didn’t share account limits on this computer; each row says why.</>

  return <div className={column}>
    <p className="uam-limits-lede">{headline}{tokenSentence}</p>
    <SettingsSection
      title="Allowances"
      actions={remote ? undefined : <>
        <Reveal as="span" open={account.coolingDown}><span className="uam-limits-updated">Updated moments ago; refresh available shortly.</span></Reveal>
        <Button size="sm" variant="secondary" onClick={account.refresh} disabled={account.coolingDown} loading={account.refreshing || account.requestingRefresh}>Refresh allowances</Button>
      </>}
    >
      <Reveal open={!!account.error}><Notice tone="warning" title={account.error} dismissLabel="Dismiss allowance error" dismissible={false}>{account.data ? 'Showing the last snapshot.' : null}</Notice></Reveal>
      {entries.map(entry => <ProviderLimitRow key={entry.provider} {...entry} loading={account.loading} refreshFailed={Boolean(account.error)} remote={remote}
        context={latest.contexts.get(entry.provider)} consumption={latest.tokens.get(entry.provider)} tokens={periodTokens(usage, entry.provider)} now={now} />)}
    </SettingsSection>
  </div>
}

/** One compact row per provider: the row fills to its most-used window; expanding shows every window, spend, credits, tokens and the full native message. */
function ProviderLimitRow({ provider, name, allowance, rows, lead, loading, refreshFailed, remote, context, consumption, tokens, now }: {
  provider: UsageProviderId
  name: string
  allowance: ProviderAllowance | undefined
  rows: WindowRow[]
  lead: WindowRow | undefined
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
  const showWindowRows = hasAllowance || reading || allowance?.status === 'ready'
  const plan = limits?.planType || limits?.limitName
  const consumptionTimeValid = Boolean(consumption && consumption.usage.updatedAt > 0 && consumption.usage.updatedAt <= now && Number.isFinite(new Date(consumption.usage.updatedAt).getTime()))
  const timestampValid = Boolean(limits && limits.updatedAt > 0 && limits.updatedAt <= now && Number.isFinite(new Date(limits.updatedAt).getTime()))
  const stale = Boolean(hasAllowance && (refreshFailed || allowance?.status !== 'ready' || !timestampValid || (limits && now - limits.updatedAt >= 300000)))
  const contextPercent = context ? Math.min(100, Math.max(0, context.usage.usedTokens / context.usage.windowTokens * 100 || 0)) : 0
  const reason = shortReason(allowance, name)
  // A remote computer's unsupported feed is explained once above the card, so its unknown row names no reason.
  const remoteUnsupported = remote && (!allowance || allowance.status === 'unavailable')
  const unknown: WindowRow = { ...rows[0], window: null, pace: undefined, missingLabel: remoteUnsupported ? 'read on this computer only' : reason }
  // The compact row shows the most pressing window's bar; an unknown one is the same bar greyed, with the short reason
  // as text and the native message in the tooltip and details.
  const side = lead?.unlimited ? <span className="uam-usage-muted">Unlimited</span>
    : lead ? <WindowMeter row={lead} prefix={lead.label} now={now} awaitingUpdate={stale || loading} compact />
    : <WindowMeter row={unknown} prefix={unknown.label} unknownText={remoteUnsupported ? null : reading ? 'Reading…' : reason} unknownDetail={allowance?.error || undefined} now={now} pending={reading} />
  const renderRow = (row: WindowRow) => <AllowanceRow key={row.id} row={row} now={now} awaitingUpdate={stale || loading} pending={reading} />
  return <div role="group" aria-label={name} className="uam-limit-entry">
    <Disclosure summary={<span className="uam-limit-summary">
      <span className="uam-limit-summary__identity"><ProviderLogo providerId={provider} size={16} /><span className="truncate">{name}</span>
        <Reveal as="span" open={!!(hasAllowance && plan)}><span className="uam-limits-chip">{plan}</span></Reveal>
      </span>
      <span className="uam-limit-summary__tokens">{tokens && <Tooltip label={tokens.detail}><span>{tokens.short}</span></Tooltip>}</span>
      <span className="uam-limit-summary__side">{side}</span>
    </span>}>
      <div className="uam-set-row__extra uam-limit-details">
        <Reveal open={hasAllowance}><p className="uam-limits-updated"><Tooltip label={timestampValid ? new Date(limits!.updatedAt).toLocaleString() : undefined}><span>{timestampValid ? `Updated ${relativeAgo(limits!.updatedAt, now)}` : 'Update time unavailable'}{stale ? ' · Cached snapshot' : ''}</span></Tooltip></p></Reveal>
        {/* An unsupported feed is an honest neutral state; only failed reads carry the warning mark. */}
        <Reveal open={!!allowance?.error}><p className={cx('flex items-start gap-1.5', allowance?.status === 'unavailable' ? 'uam-usage-muted' : 'uam-limits-notice')}>{allowance?.status !== 'unavailable' && <StatusDot tone="warning" className="mt-1 shrink-0" />}{allowance?.error}{hasAllowance ? ' Showing cached allowances.' : ''}</p></Reveal>
        <Reveal open={!reading && !hasAllowance && !allowance?.error && allowance?.status !== 'ready'}><p className="uam-usage-muted">Account allowances unavailable.</p></Reveal>
        <Reveal open={showWindowRows}><div className="grid gap-1">
          {(modelWindows.length ? rows.slice(0, 2) : rows).map(renderRow)}
          <Reveal open={modelWindows.length > 2}><Disclosure className="uam-limits-more" summary={`${modelWindows.length - 2} more model allowance${modelWindows.length > 3 ? 's' : ''}`}><div className="grid gap-1">{rows.slice(2).map(renderRow)}</div></Disclosure></Reveal>
        </div></Reveal>
        <Reveal open={!!limits?.individualLimit}>{limits?.individualLimit && <div className="uam-limit-row">
          <span className="uam-limit-row__name">Spend limit</span>
          <small className="uam-limit-row__reset">{limits.individualLimit.used} of {limits.individualLimit.limit}</small>
          <span className="uam-limit-bar"><ProgressFill variant="meter" label={`${name} spend limit usage`} value={100 - limits.individualLimit.remainingPercent}
            tone={100 - limits.individualLimit.remainingPercent >= 95 ? 'error' : 100 - limits.individualLimit.remainingPercent >= 80 ? 'warning' : 'neutral'} /></span>
          <span className="uam-limit-row__value">{Math.round(100 - limits.individualLimit.remainingPercent)}% used</span>
        </div>}</Reveal>
        <Reveal open={!!limits?.credits}>{limits?.credits && <p className="uam-usage-muted" title={limits.credits.balance ?? undefined}>Credits: {limits.credits.unlimited ? 'Unlimited' : limits.credits.hasCredits ? limits.credits.balance === null ? 'Available' : formatCreditBalance(limits.credits.balance) : 'None'}</p>}</Reveal>
        <Reveal open={!!limits?.spendControlReached}><p className="flex items-center gap-1.5 uam-limits-notice"><StatusDot tone="error" />Spend limit reached.</p></Reveal>
        <Reveal open={!!tokens}><p className="uam-usage-muted">{tokens?.detail}</p></Reveal>
        <Reveal open={!!(context || consumption)}>{(context || consumption) && <Disclosure className="uam-limits-tokens" summary={<><span className="uam-limits-label">Latest chat context and tokens</span>{consumption && <span className="tabular-nums">{compactTokens(consumption.usage.total.totalTokens)}</span>}</>}>
          <div className="grid gap-3">
            {context && <ProgressFill label={`${name} context usage`} value={contextPercent} className="uam-limit-row">
              <span className="uam-limit-row__name">Context</span>
              <span className="uam-limit-row__value tabular-nums">{context.usage.usedTokens.toLocaleString()} of {context.usage.windowTokens.toLocaleString()} tokens</span>
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
        </Disclosure>}</Reveal>
      </div>
    </Disclosure>
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
const DAY = 86400000
const dayTime = new Intl.DateTimeFormat(undefined, { weekday: 'short', hour: 'numeric', minute: '2-digit' })
/** A future moment in words: relative within a day, otherwise its weekday and time. */
const moment = (at: number, now: number) => at - now < DAY ? `in ${span(at - now)}` : dayTime.format(at)
/** A projected moment, marked as approximate. */
const approx = (at: number, now: number) => at - now < DAY ? `in ~${span(at - now)}` : `~${dayTime.format(at)}`
/** Generated window names read in a sentence ("its weekly window"); provider and model names keep their case. */
const inSentence = (label: string) => / window$/.test(label) ? label.charAt(0).toLowerCase() + label.slice(1) : label
const relativeAgo = (at: number, now: number) => now - at < 60000 ? 'just now' : `${span(now - at)} ago`
const compactTokens = (value: number) => value >= 1e6 ? `${+(value / 1e6).toFixed(1)}M` : value >= 1e3 ? `${+(value / 1e3).toFixed(1)}k` : `${value}`
