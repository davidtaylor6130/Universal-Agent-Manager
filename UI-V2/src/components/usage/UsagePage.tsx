import { useEffect, useMemo, useRef, useState, type CSSProperties, type KeyboardEvent } from 'react'
import { ArrowLeft, Info, RefreshCw, SlidersHorizontal } from 'lucide-react'
import { useUsageHistory } from '../../hooks/useUsageHistory'
import { ALL_MACHINES_HOST_ID, EMPTY_USAGE_TOTALS, sumProviderUsage, USAGE_PROVIDERS, USAGE_WINDOWS, formatUsageCost, formatUsageCount, usageCost, usageWindow, type UsageProviderId, type UsageMetric, type UsageModel, type UsagePriceOverride, type UsageTotals, type UsageWindowDays } from '../../services/usageHistory'
import { useAppStore } from '../../store/useAppStore'
import { ANTIGRAVITY_ENABLED, CLAUDE_ENABLED, CODEX_ENABLED, COPILOT_ENABLED, GEMINI_ENABLED, OPENCODE_ENABLED } from '../../config/buildFeatures'
import { ANTIGRAVITY_CLI_PROVIDER_ID, CLAUDE_CLI_PROVIDER_ID, CODEX_CLI_PROVIDER_ID, COPILOT_CLI_PROVIDER_ID, GEMINI_CLI_PROVIDER_ID, OPENCODE_CLI_PROVIDER_ID, providerShortName } from '../../utils/providerMetadata'
import { ProviderLogo } from '../shared/ProviderLogo'
import { Button, IconButton, MenuSelect, Tooltip, cx } from '../ui'
import { UsageLimits } from './UsageLimits'
import './usage.css'

const PRICE_KEY = 'uam-usage-prices-v1'
const priceFields = [ ['inputPerMillion', 'Input'], ['cachedInputPerMillion', 'Cached input'], ['cacheWritePerMillion', 'Cache writes'], ['outputPerMillion', 'Output'] ] as const

const ALL_PROVIDERS_INCLUDED = GEMINI_ENABLED && CODEX_ENABLED && CLAUDE_ENABLED && OPENCODE_ENABLED && COPILOT_ENABLED && ANTIGRAVITY_ENABLED
// A build that excludes providers keeps their saved prices unchanged on this device but never shows or uses them.
const savedProvider = (id: unknown) => USAGE_PROVIDERS.includes(id as UsageProviderId) || !ALL_PROVIDERS_INCLUDED && typeof id === 'string' && /^[a-z0-9_-]{1,128}$/.test(id)
const includedUsagePrices = (prices: UsagePriceOverride[]) => prices.filter(price => USAGE_PROVIDERS.includes(price.providerId))

export function parseUsagePrices(value: unknown): UsagePriceOverride[] {
  if (!Array.isArray(value) || value.length > 200) return []
  const keys = new Set<string>()
  const valid = value.every(row => {
    if (!row || typeof row !== 'object' || Array.isArray(row) || !savedProvider(row.providerId) || typeof row.modelId !== 'string' || !row.modelId || row.modelId.length > 512 || !['standard', 'fast', 'ultrafast'].includes(row.speed)
      || !priceFields.every(([key]) => row[key] === null || typeof row[key] === 'number' && Number.isFinite(row[key]) && row[key] >= 0)) return false
    const key = JSON.stringify([row.providerId, row.modelId, row.speed])
    if (keys.has(key)) return false
    keys.add(key)
    return true
  })
  return valid ? value : []
}
function readPrices(): UsagePriceOverride[] {
  try { return parseUsagePrices(JSON.parse(window.localStorage.getItem(PRICE_KEY) || '[]')) } catch { return [] }
}
const modelKey = (model: Pick<UsageModel, 'providerId' | 'modelId' | 'speed'>) => JSON.stringify([model.providerId, model.modelId, model.speed])
const providerLabel = (id: string) => providerShortName(undefined, id)
/** One colour per provider. Values live in usage.css (--usage-*); no two providers share one. */
const providerColor: Record<UsageProviderId, string> = {
  ...(CODEX_ENABLED ? { [CODEX_CLI_PROVIDER_ID]: 'var(--usage-codex)' } : {}),
  ...(CLAUDE_ENABLED ? { [CLAUDE_CLI_PROVIDER_ID]: 'var(--usage-claude)' } : {}),
  ...(GEMINI_ENABLED ? { [GEMINI_CLI_PROVIDER_ID]: 'var(--usage-gemini)' } : {}),
  ...(OPENCODE_ENABLED ? { [OPENCODE_CLI_PROVIDER_ID]: 'var(--usage-opencode)' } : {}),
  ...(COPILOT_ENABLED ? { [COPILOT_CLI_PROVIDER_ID]: 'var(--usage-copilot)' } : {}),
  ...(ANTIGRAVITY_ENABLED ? { [ANTIGRAVITY_CLI_PROVIDER_ID]: 'var(--usage-antigravity)' } : {}),
}

const money = (value: number) => new Intl.NumberFormat(undefined, { style: 'currency', currency: 'USD', maximumFractionDigits: value > 0 && value < 0.01 ? 4 : 2 }).format(value)
const percentOf = (part: number, whole: number) => whole > 0 ? `${Math.round(part / whole * 100)}%` : '0%'
/** Token segments are disjoint: input + cache read + cache write + output (reasoning is a part of output). */
const TOKEN_SEGMENTS = [
  { key: 'input', label: 'Input', color: 'var(--token-input)', value: (total: UsageTotals) => total.inputTokens },
  { key: 'cache', label: 'Cache read', color: 'var(--token-cache-read)', value: (total: UsageTotals) => total.cachedInputTokens },
  { key: 'cache-write', label: 'Cache write', color: 'var(--token-cache-write)', value: (total: UsageTotals) => total.cacheWriteTokens },
  { key: 'output', label: 'Output', color: 'var(--token-output)', value: (total: UsageTotals) => total.outputTokens - total.reasoningTokens },
  { key: 'reasoning', label: 'Reasoning, included in output', color: 'var(--token-reasoning)', value: (total: UsageTotals) => total.reasoningTokens },
] as const

/** Token composition as one proportional bar. Labels sit inside wide segments; every segment keeps an exact tooltip. */
function TokenStack({ total, height = 14, labels = false }: { total: UsageTotals; height?: number; labels?: boolean }) {
  const parts = TOKEN_SEGMENTS.map(segment => ({ segment, value: segment.value(total) }))
  const sum = parts.reduce((acc, part) => acc + Math.max(0, part.value), 0)
  return <span className="uam-usage-stack" aria-hidden style={{ height }}>
    {parts.map(({ segment, value }) => {
      if (value <= 0) return null
      const share = value / sum
      return <span key={segment.key} title={`${segment.label} · ${value.toLocaleString()}`} style={{ flexGrow: value, '--c': segment.color } as CSSProperties}>
        {labels && share >= 0.12 && <span className="uam-usage-stack-label">{share >= 0.3 ? `${segment.key === 'reasoning' ? 'Reasoning' : segment.label} ` : ''}{formatUsageCount(value)}</span>}
      </span>
    })}
  </span>
}

function TokenLegend({ total }: { total: UsageTotals }) {
  return <ul className="uam-usage-key text-xs">
    {TOKEN_SEGMENTS.map(segment => { const value = segment.value(total); return <li key={segment.key} className="uam-usage-key-item">
      <i className="uam-usage-swatch" style={{ '--c': segment.color } as CSSProperties} aria-hidden />{segment.label}
      <span className="tabular-nums uam-usage-muted" title={value.toLocaleString()}>{formatUsageCount(value)} · {percentOf(value, total.totalTokens)}</span>
    </li> })}
  </ul>
}

/** Cost share as one solid bar in the provider colour; width is relative to the largest row. */
function CostBar({ cost, max, color }: { cost: number; max: number; color: string }) {
  return <span className="uam-usage-stack" aria-hidden style={{ width: max > 0 ? `${Math.min(100, cost / max * 100)}%` : '0%' }}>
    <span style={{ flexGrow: 1, '--c': color } as CSSProperties} />
  </span>
}

function StatTile({ label, value, sub, title }: { label: string; value: string; sub?: string; title?: string }) {
  return <div className="uam-usage-tile">
    <span className="uam-usage-label">{label}</span>
    <span className="uam-usage-tile-value tabular-nums" title={title}>{value}</span>
    {sub && <span className="uam-usage-muted text-xs">{sub}</span>}
  </div>
}

function PriceEditor({ model, prices, onSave, onClose }: { model: UsageModel; prices: UsagePriceOverride[]; onSave: (prices: UsagePriceOverride[]) => void; onClose: () => void }) {
  const existing = prices.find(price => modelKey(price) === modelKey(model))
  const [fields, setFields] = useState(() => priceFields.map(([key]) => (existing ? existing[key] : model.priceReference?.[key])?.toString() ?? ''))
  const [error, setError] = useState('')
  const save = (remove = false) => {
    const next = prices.filter(price => modelKey(price) !== modelKey(model))
    if (!remove) {
      const values = fields.map(value => value.trim() === '' ? null : Number(value))
      if (values.some(value => value !== null && (!Number.isFinite(value) || value < 0))) { setError('Prices must be non-negative numbers. Leave unknown rates blank.'); return }
      if (next.length >= 200) { setError('Up to 200 model prices can be saved. Remove an unused override first.'); return }
      next.push({ providerId: model.providerId, modelId: model.modelId, speed: model.speed, inputPerMillion: values[0], cachedInputPerMillion: values[1], cacheWritePerMillion: values[2], outputPerMillion: values[3] })
    }
    try { window.localStorage.setItem(PRICE_KEY, JSON.stringify(next)); onSave(next); onClose() } catch { setError('Could not save prices on this device. Your previous prices were kept.') }
  }
  return <form className="grid gap-3 py-4" aria-label={`Prices for ${model.modelId}`} onSubmit={event => { event.preventDefault(); save() }}>
    <p className="text-xs">USD per million tokens. Blank rates stay unknown. These estimates do not change provider billing.</p>
    {model.priceReference?.kind === 'reference' && <div className="grid gap-1 text-xs">
      <p>Reference: {model.priceReference.referenceModel} · <a href={model.priceReference.source} target="_blank" rel="noreferrer" className="underline">Rate source</a>{model.priceReference.verifiedDate && ` · Verified ${model.priceReference.verifiedDate}`}</p>
      {model.priceReference.cacheFallback && <p>Unpublished cache rates use the input rate.</p>}
      {model.priceReference.speedFallback && <p>This speed uses the standard rate because a separate rate is unavailable.</p>}
    </div>}
    <div className="grid gap-3 sm:grid-cols-4">{priceFields.map(([key, label], index) => <label className="grid gap-1 text-xs" key={key}>{label}
      <input type="number" min="0" step="any" className="uam-usage-price-input w-full" value={fields[index]} onChange={event => setFields(previous => previous.map((value, i) => i === index ? event.target.value : value))} />
    </label>)}</div>
    {error && <p role="alert" className="text-xs" style={{ color: 'var(--red)' }}>{error}</p>}
    <div className="flex gap-2"><Button size="sm" variant="primary" type="submit">Save prices</Button><Button size="sm" type="button" onClick={onClose}>Cancel</Button>{existing && <Button size="sm" type="button" onClick={() => save(true)}>Remove override</Button>}</div>
  </form>
}

/** T3-inspired usage workspace. Native readers supply counts; unknown prices and scan gaps remain visible. */
export function UsagePage({ onClose }: { onClose: () => void }) {
  const hosts = useAppStore(state => state.executionHosts)
  const [hostId, setHostId] = useState(ALL_MACHINES_HOST_ID)
  const [metric, setMetric] = useState<UsageMetric>('cost')
  const [days, setDays] = useState<UsageWindowDays>(7)
  const [range, setRange] = useState(() => usageWindow(7))
  const [breakdown, setBreakdown] = useState<'model' | 'time'>('model')
  const [prices, setPrices] = useState(readPrices)
  const [selectedModel, setSelectedModel] = useState<string | null>(null)
  const [editingPrice, setEditingPrice] = useState(false)
  const [modelLimit, setModelLimit] = useState(100)
  const [hiddenProviders, setHiddenProviders] = useState<ReadonlySet<UsageProviderId>>(() => new Set())
  const includedPrices = useMemo(() => includedUsagePrices(prices), [prices])
  const history = useUsageHistory(hostId, range, includedPrices, metric !== 'limits')
  const headingRef = useRef<HTMLHeadingElement>(null)
  useEffect(() => { headingRef.current?.focus() }, [])
  useEffect(() => {
    if (hostId !== ALL_MACHINES_HOST_ID && hostId !== 'local' && !hosts.some(host => host.id === hostId)) {
      setHostId(ALL_MACHINES_HOST_ID); setSelectedModel(null); setEditingPrice(false)
    }
  }, [hosts, hostId])
  const data = history.data
  const totals = useMemo(() => sumProviderUsage((data?.providers ?? []).filter(provider => !hiddenProviders.has(provider.providerId))), [data, hiddenProviders])
  const models = useMemo(() => (data?.models ?? []).filter(model => !hiddenProviders.has(model.providerId)).sort((a, b) => metric === 'cost' ? (usageCost(b) ?? -1) - (usageCost(a) ?? -1) : b.totalTokens - a.totalTokens), [data, metric, hiddenProviders])
  const hourly = data ? data.endMs - data.startMs <= 86400000 : days === 1
  const periods = useMemo(() => {
    if (!data) return []
    const width = hourly ? 3600000 : data.chartBucketMs ?? 86400000
    const present = new Map<number, typeof data.providerDays>()
    for (const row of hourly ? data.providerHours : data.providerDays) {
      if (hiddenProviders.has(row.providerId)) continue
      const rows = present.get(row.startMs) ?? []
      rows.push(row)
      present.set(row.startMs, rows)
    }
    const result = []
    for (let startMs = data.chartStartMs ?? Math.floor(data.startMs / width) * width; startMs < data.endMs; startMs += width) {
      const bands = present.get(startMs) ?? []
      result.push({ ...EMPTY_USAGE_TOTALS, ...sumProviderUsage(bands), startMs, bands })
    }
    return result
  }, [data, hourly, hiddenProviders])
  const amount = (total: UsageTotals) => metric === 'cost' ? formatUsageCost(total) : formatUsageCount(total.totalTokens)
  const numeric = (total: UsageTotals) => metric === 'cost' ? usageCost(total) ?? 0 : total.totalTokens
  const peak = Math.max(0, ...periods.map(numeric))
  const maxModel = Math.max(0, ...models.map(numeric))
  const currentModel = data?.models.find(model => modelKey(model) === selectedModel)
  const refresh = () => { setRange(usageWindow(days)); history.refresh() }
  const warnings = data?.providers.filter(provider => provider.errors || provider.invalidLines || provider.oversizedLines || provider.pendingLines) ?? []
  // Re-keying the plot replays the grow-in animation when the metric, period or provider filter changes.
  const plotKey = [metric, days, hostId, [...hiddenProviders].sort().join(',')].join('|')
  const axisValue = (value: number) => metric === 'cost' ? money(value) : formatUsageCount(value)
  // Tokens stack by token type (per-day split from the provider rows); cost stacks by provider.
  type Band = { key: string; label: string; color: string; value: number; text: string }
  const providerBands = (period: (typeof periods)[number]): Band[] => period.bands.map(band => ({ key: band.providerId, label: providerLabel(band.providerId), color: providerColor[band.providerId], value: numeric(band), text: amount(band) }))
  const tokenBands = (period: (typeof periods)[number]): Band[] => TOKEN_SEGMENTS.map(segment => { const value = segment.value(period); return { key: segment.key, label: segment.label, color: segment.color, value, text: `${formatUsageCount(value)} · ${percentOf(value, period.totalTokens)}` } })
  const columnSummary = (period: (typeof periods)[number]) => {
    const providers = providerBands(period).filter(band => band.value > 0).map(band => `${band.label} ${band.text}`)
    const tokens = metric === 'tokens' ? tokenBands(period).filter(band => band.value > 0).map(band => `${band.label} ${formatUsageCount(band.value)}`) : []
    return [`${tipLabel(period.startMs)}: ${amount(period)}`, providers.length ? providers.join(', ') : 'No usage', tokens.length ? tokens.join(', ') : ''].filter(Boolean).join('. ')
  }
  /** Arrow keys (and Home/End) move focus between daily columns; each column shows the same tooltip on focus. */
  const moveColumnFocus = (event: KeyboardEvent<HTMLDivElement>) => {
    if (!['ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(event.key)) return
    const columns = Array.from(event.currentTarget.querySelectorAll<HTMLElement>('.uam-usage-col'))
    const current = columns.indexOf(document.activeElement as HTMLElement)
    if (current < 0) return
    event.preventDefault()
    const next = event.key === 'Home' ? 0 : event.key === 'End' ? columns.length - 1 : Math.min(columns.length - 1, Math.max(0, current + (event.key === 'ArrowRight' ? 1 : -1)))
    columns[next].focus()
  }
  const tipLabel = (startMs: number) => new Date(startMs).toLocaleString(undefined, hourly ? { hour: 'numeric', month: 'short', day: 'numeric' } : { year: days === 0 ? 'numeric' : undefined, month: 'short', day: 'numeric', timeZone: 'UTC' })
  const noCostData = metric === 'cost' && totals.totalTokens > 0 && totals.reportedCostTokens + totals.estimatedCostTokens === 0
  const hasReported = totals.reportedCostTokens > 0 || totals.reportedCostUsd > 0
  const partialCost = totals.unpricedTokens > 0 && totals.reportedCostTokens + totals.estimatedCostTokens > 0
  const coverageLabel = (status: string) => status === 'ready' ? 'Current' : status === 'cached' ? 'Cached' : status === 'loading' ? 'Loading' : 'Unavailable'
  const coverage = (data?.coverage ?? []).map(machine => [`${machine.label || machine.executionHostId}: ${coverageLabel(machine.status)}`, machine.asOfMs ? new Date(machine.asOfMs).toLocaleString() : '', machine.error].filter(Boolean).join(' · '))
  // Status lives in one header icon (tooltip + screen-reader list), not as text on the page.
  const notes = data ? [
    history.kept && 'Showing the previous answer while the selected range loads.',
    data.asOfMs && `Updated ${new Date(data.asOfMs).toLocaleString()}`,
    metric === 'cost' && data.pricing && `${data.pricing.refreshing ? 'Updating reference prices…' : data.pricing.status === 'unavailable' ? 'Using verified model references where available.' : `Using ${data.pricing.status === 'cached' ? 'cached' : 'current'} reference prices.`}${data.pricing.fetchedAtMs ? ` Retrieved ${new Date(data.pricing.fetchedAtMs).toLocaleString()}.` : ''}`,
    metric === 'cost' && data.pricing?.error,
    metric === 'cost' && data.pricing?.aliasError,
    !data.complete && 'Partial history. Totals include available and cached records.',
    data.historyCoverage?.previouslyPrunedReceiptsMayBeMissing && 'All Time includes recorded and available history. Previously deleted receipts may be missing.',
    data.listingLimited && 'Some history is outside the scan limit. These totals are incomplete.',
    noCostData && 'No cost data for this range. Tokens were recorded, but no prices are known. Add model prices under Breakdown, or switch to Tokens.',
    metric === 'cost' && partialCost && `${totals.unpricedTokens.toLocaleString()} tokens have no known price and are excluded from costs.`,
    COPILOT_ENABLED && !hiddenProviders.has(COPILOT_CLI_PROVIDER_ID) && data.providers.some(provider => provider.providerId === COPILOT_CLI_PROVIDER_ID && provider.records > 0) && 'Copilot history is grouped by saved checkpoint time.',
    ...warnings.map(provider => `${providerLabel(provider.providerId)}: ${provider.errors} read errors, ${provider.invalidLines} invalid records, ${provider.oversizedLines} oversized records, ${provider.pendingLines} unfinished lines.`),
  ].filter((line): line is string => Boolean(line)) : []
  const attention = Boolean(data && (!data.complete || data.listingLimited || data.historyCoverage?.previouslyPrunedReceiptsMayBeMissing || warnings.length > 0 || noCostData || metric === 'cost' && partialCost || data.coverage?.some(machine => machine.status !== 'ready')))
  const statusLines = [...notes, ...coverage]
  return <main aria-label="Usage workspace" className="uam-usage flex h-full min-h-0 min-w-0 flex-1 flex-col" style={{ background: 'var(--bg)', color: 'var(--text)' }} onKeyDown={event => {
    if (event.key === 'Escape' && !event.defaultPrevented && !document.querySelector('[aria-modal="true"]') && !editingPrice) { event.preventDefault(); onClose() }
  }}>
    <header className="flex flex-wrap items-center gap-3 border-b px-4 py-3" style={{ borderColor: 'var(--border)' }}>
      <IconButton icon={<ArrowLeft size={16} />} label="Back from usage" onClick={onClose} />
      <h1 ref={headingRef} tabIndex={-1} className="text-sm font-semibold">Usage</h1>
      {statusLines.length > 0 && <Tooltip side="bottom" align="start" label={<span className="uam-usage-status-tip">{statusLines.map((line, index) => <span key={index} className="block">{line}</span>)}</span>}>
        <button type="button" aria-label="Usage status" className={cx('uam-usage-status', attention && 'is-attention')}><Info size={14} aria-hidden /></button>
      </Tooltip>}
      <div className="max-w-48"><MenuSelect label="Usage host" value={hostId} options={[{ value: ALL_MACHINES_HOST_ID, label: 'All machines' }, ...(!hosts.some(host => host.id === 'local') ? [{ value: 'local', label: 'This computer' }] : []), ...hosts.map(host => ({ value: host.id, label: host.label }))]} onChange={value => { setHostId(value); setSelectedModel(null); setEditingPrice(false) }} /></div>
      <div role="group" aria-label="Usage metric" className="uam-usage-seg">{(['cost', 'tokens', 'limits'] as const).map(value => <Button key={value} size="sm" variant={metric === value ? 'secondary' : 'ghost'} aria-pressed={metric === value} onClick={() => setMetric(value)}>{value[0].toUpperCase() + value.slice(1)}</Button>)}</div>
      <div role="group" aria-label="Usage period" className="uam-usage-seg">{USAGE_WINDOWS.map(option => <Button size="sm" key={option.days} disabled={metric === 'limits'} variant={days === option.days ? 'secondary' : 'ghost'} aria-pressed={days === option.days} onClick={() => { setDays(option.days); setRange(usageWindow(option.days)); setSelectedModel(null); setEditingPrice(false) }}>{option.label}</Button>)}</div>
      {metric !== 'limits' && <IconButton icon={<RefreshCw size={15} />} label="Refresh usage" aria-busy={history.loading} disabled={history.loading} onClick={refresh} />}
    </header>
    <div className="min-h-0 flex-1 overflow-y-auto p-4 md:p-6">
      {metric === 'limits' ? <UsageLimits executionHostId={hostId} /> : <div className="mx-auto grid max-w-6xl gap-6" aria-busy={history.loading}>
        {history.loading && <p role="status" className="text-xs uam-usage-muted">Loading usage history…{data && !history.kept ? ` ${data.filesChecked.toLocaleString()} of ${data.filesTotal.toLocaleString()} files` : ''}</p>}
        {history.error && <div role="alert" className="flex flex-wrap items-center gap-3 text-xs" style={{ color: 'var(--error)' }}>{history.error}<Button size="sm" onClick={refresh}>Retry</Button></div>}
        {!data && history.loading && !history.error && <div className="uam-usage-skeleton" aria-hidden><span className="uam-usage-skel-tile" /><span className="uam-usage-skel-tile" /><span className="uam-usage-skel-tile" /><span className="uam-usage-skel-chart" /></div>}
        {data && <div className="grid gap-6" style={{ opacity: history.kept ? 0.5 : 1 }} data-kept={history.kept || undefined}>
          <ul className="sr-only" aria-label="Usage status details">{notes.map((line, index) => <li key={index}>{line}</li>)}</ul>
          {coverage.length > 0 && <ul aria-label="Usage machine coverage" className="sr-only">{coverage.map((line, index) => <li key={index}>{line}</li>)}</ul>}
          <div role="group" aria-label="Providers" className="uam-usage-chips">{data.providers.map(provider => {
            const off = hiddenProviders.has(provider.providerId)
            return <button key={provider.providerId} type="button" aria-label={`Include ${providerLabel(provider.providerId)} usage`} aria-pressed={!off} className={cx('uam-usage-chip', off && 'is-off')} style={{ '--c': providerColor[provider.providerId] } as CSSProperties} onClick={() => { setHiddenProviders(previous => { const next = new Set(previous); if (next.has(provider.providerId)) next.delete(provider.providerId); else next.add(provider.providerId); return next }); setSelectedModel(null); setEditingPrice(false) }}>
              <span className="uam-usage-chip-dot" aria-hidden />
              <ProviderLogo providerId={provider.providerId} size={14} />
              <span className="uam-usage-chip-name">{providerLabel(provider.providerId)}</span>
              <span className="uam-usage-chip-value tabular-nums" title={provider.available ? `${provider.files} history files` : 'Historical usage reader unavailable'}>{provider.available || provider.records > 0 ? amount(provider) : 'Unavailable'}</span>
            </button>
          })}</div>
          {totals.totalTokens === 0 && !history.loading && <p className="uam-usage-empty">No usage recorded in this range.</p>}
          <section className="grid gap-6 lg:grid-cols-[18rem_minmax(0,1fr)]">
            <div className="grid content-start gap-4">
              <div className="uam-usage-tiles">
                {metric === 'cost' ? <>
                  <StatTile label={partialCost ? 'Known cost' : hasReported ? 'Total cost' : 'Estimated cost'} value={amount(totals)} title={totals.totalTokens.toLocaleString()} sub={`${hasReported ? 'Provider charges and API estimates' : 'API list prices'} · ${totals.sessions.toLocaleString()} sessions`} />
                  {hasReported && <StatTile label="Reported" value={money(totals.reportedCostUsd)} sub="Provider-reported" />}
                  {hasReported && <StatTile label="Estimated" value={totals.estimatedCostTokens || !totals.totalTokens ? money(totals.estimatedCostUsd) : 'Unknown'} sub="API list prices" />}
                  {(!hasReported || totals.unpricedTokens > 0) && <StatTile label="Unpriced tokens" value={formatUsageCount(totals.unpricedTokens)} sub="No known price" />}
                </> : <>
                  <StatTile label="Total tokens" value={amount(totals)} title={totals.totalTokens.toLocaleString()} sub={`${totals.sessions.toLocaleString()} sessions`} />
                  <StatTile label="Cache read" value={percentOf(totals.cachedInputTokens, totals.totalTokens)} sub={`${formatUsageCount(totals.cachedInputTokens)} tokens`} />
                  <StatTile label="Reasoning" value={percentOf(totals.reasoningTokens, totals.totalTokens)} sub={`${formatUsageCount(totals.reasoningTokens)} tokens`} />
                </>}
              </div>
            </div>
            <div className="grid content-start gap-3">
              <h2 className="text-sm font-medium">{hourly ? 'Hourly' : 'Daily'} {metric === 'cost' ? 'cost' : 'tokens'}</h2>
              <div className="uam-usage-chart">
                <div className="uam-usage-axis text-xs tabular-nums" aria-hidden><span>{axisValue(peak)}</span><span>{axisValue(peak / 2)}</span><span>0</span></div>
                <div key={plotKey} role="group" aria-label={`${hourly ? 'Hourly' : 'Daily'} ${metric === 'cost' ? 'costs, unpriced tokens excluded' : 'tokens'} from loaded history. Use arrow keys to move between columns.`} className="uam-usage-plot" style={{ gap: periods.length > 31 ? 1 : 4 }} onKeyDown={moveColumnFocus}>
                  {metric === 'cost' && peak === 0 && totals.unpricedTokens > 0 && <span className="absolute inset-0 flex items-center justify-center text-xs">No priced usage in this range.</span>}
                  {periods.map((period, index) => {
                    const total = numeric(period)
                    const pct = peak ? total / peak * 100 : 0
                    const edge = index < periods.length * 0.2 ? 'is-start' : index >= periods.length * 0.8 ? 'is-end' : ''
                    const bands = (metric === 'tokens' ? tokenBands(period) : providerBands(period)).filter(band => band.value > 0)
                    const tipRows = metric === 'tokens' ? [...bands, ...providerBands(period).filter(band => band.value > 0).map(band => ({ ...band, text: `${formatUsageCount(band.value)} tokens` }))] : bands
                    return <div key={period.startMs} role="img" tabIndex={index === periods.length - 1 ? 0 : -1} aria-label={columnSummary(period)} className={cx('uam-usage-col', edge, pct > 55 && 'is-high')} style={{ '--i': Math.min(index, 40), '--h': `${pct}%` } as CSSProperties}>
                      <div className="uam-usage-col-stack" style={{ height: `${pct}%`, minHeight: total > 0 ? 2 : 0 }}>{bands.map(band => <div key={band.key} className="uam-usage-band" data-seg={band.key} style={{ height: `${total ? band.value / total * 100 : 0}%`, '--c': band.color } as CSSProperties} />)}</div>
                      <span className="uam-usage-col-tip" aria-hidden>
                        <span className="uam-usage-tip-head">{tipLabel(period.startMs)} · <b>{amount(period)}</b></span>
                        {tipRows.map(row => <span key={row.key} className="uam-usage-tip-row"><i className="uam-usage-swatch" style={{ '--c': row.color } as CSSProperties} />{row.label}<span className="tabular-nums uam-usage-muted">{row.text}</span></span>)}
                        {tipRows.length === 0 && <span className="uam-usage-muted">No usage</span>}
                      </span>
                    </div>
                  })}
                </div>
              </div>
              <p className="text-xs uam-usage-muted">{new Date(data.chartStartMs ?? data.startMs).toLocaleString()} to {new Date(data.endMs).toLocaleString()}{!hourly ? ` · ${data.chartBucketMs && data.chartBucketMs > 86400000 ? `${data.chartBucketMs / 86400000}-day` : 'Daily'} buckets use UTC` : ''}</p>
            </div>
          </section>
          <section className="grid gap-3"><div className="flex flex-wrap items-baseline justify-between gap-3"><h2 className="text-sm font-medium">Totals</h2>{metric === 'cost' && <p className="text-xs uam-usage-muted">Estimated cache savings: {totals.estimatedCostTokens || !totals.totalTokens ? money(totals.estimatedCacheSavingsUsd) : 'Unknown'}</p>}</div>
            <TokenStack total={totals} height={16} labels />
            <TokenLegend total={totals} />
            <div className="grid gap-3 pt-2"><h3 className="uam-usage-label">Token mix by provider</h3>
              {data.providers.filter(provider => !hiddenProviders.has(provider.providerId) && provider.totalTokens > 0).map(provider => <div key={provider.providerId} className="uam-usage-mixrow">
                <span className="flex min-w-0 items-center gap-2 text-xs"><i className="uam-usage-swatch" style={{ '--c': providerColor[provider.providerId] } as CSSProperties} aria-hidden /><span className="truncate">{providerLabel(provider.providerId)}</span></span>
                <TokenStack total={provider} height={14} labels />
                <span className="text-right text-xs tabular-nums">{formatUsageCount(provider.totalTokens)}</span>
              </div>)}
              {data.providers.every(provider => hiddenProviders.has(provider.providerId) || provider.totalTokens === 0) && <p className="text-xs uam-usage-muted">No provider usage to compare.</p>}
            </div>
          </section>
          <section className="grid gap-3"><div className="flex items-center justify-between gap-3"><h2 className="text-sm font-medium">Breakdown</h2><div role="group" aria-label="Usage breakdown" className="uam-usage-seg"><Button size="sm" variant={breakdown === 'model' ? 'secondary' : 'ghost'} aria-pressed={breakdown === 'model'} onClick={() => setBreakdown('model')}>Models</Button><Button size="sm" variant={breakdown === 'time' ? 'secondary' : 'ghost'} aria-pressed={breakdown === 'time'} onClick={() => setBreakdown('time')}>Time</Button></div></div>
            {breakdown === 'model' ? <div>{models.length === 0 && <p className="py-4 text-xs uam-usage-muted">No model usage in this range.</p>}{models.slice(0, modelLimit).map(model => <div key={modelKey(model)} className="border-b" style={{ borderColor: 'var(--border)' }}>
              <button className="uam-btn uam-btn--ghost uam-usage-model-row text-xs" aria-label={`Inspect ${providerLabel(model.providerId)} ${model.modelId} ${model.speed}`} aria-expanded={modelKey(model) === selectedModel} onClick={() => { setSelectedModel(previous => previous === modelKey(model) ? null : modelKey(model)); setEditingPrice(false) }}>
                <span className="flex min-w-0 items-center gap-2"><ProviderLogo providerId={model.providerId} size={16} /><span className="truncate" title={model.modelId}>{model.modelId}</span>{model.speed !== 'standard' && <span className="uam-usage-muted">{model.speed}</span>}</span><span className="shrink-0 tabular-nums">{amount(model)}</span>
                <span className="uam-usage-model-bar">{metric === 'cost' ? <CostBar cost={usageCost(model) ?? 0} max={maxModel} color={providerColor[model.providerId]} /> : <TokenStack total={model} height={14} />}</span>
              </button>
              {currentModel === model && <div className="grid gap-3 pb-4"><TokenLegend total={model} /><p className="text-xs uam-usage-muted">{model.sessions} sessions · {model.records} usage records · {formatUsageCount(model.unpricedTokens)} unpriced tokens</p><Button size="sm" leadingIcon={<SlidersHorizontal size={13} />} onClick={() => setEditingPrice(value => !value)}>Model prices</Button>{editingPrice && <PriceEditor model={model} prices={prices} onSave={setPrices} onClose={() => setEditingPrice(false)} />}</div>}
            </div>)}{models.length > modelLimit && <Button size="sm" className="mt-3" onClick={() => setModelLimit(value => value + 100)}>Show more models ({models.length - modelLimit} remaining)</Button>}</div> : <div>{[...periods].reverse().map(period => <div key={period.startMs} className="grid grid-cols-[minmax(0,1fr)_auto] items-center gap-x-4 gap-y-1 border-b py-3 text-xs" style={{ borderColor: 'var(--border)' }}><span>{new Date(period.startMs).toLocaleString(undefined, hourly ? { hour: 'numeric', month: 'short', day: 'numeric' } : { year: 'numeric', month: 'short', day: 'numeric', timeZone: 'UTC' })}{!hourly ? ' UTC' : ''}</span><span className="tabular-nums">{amount(period)}</span><span className="uam-usage-model-bar col-span-2"><CostBar cost={numeric(period)} max={peak} color="var(--text-3)" /></span></div>)}</div>}
          </section>
          {(data.duplicates > 0 || data.unidentifiedRecords > 0 || data.providers.some(provider => provider.suppressedForkRecords > 0)) && <details className="text-xs"><summary className="cursor-pointer">History diagnostics</summary><p className="mt-2">{data.duplicates} duplicate records, {data.unidentifiedRecords} records without stable identity, {data.providers.reduce((sum, provider) => sum + provider.suppressedForkRecords, 0)} copied fork records suppressed.</p></details>}
        </div>}
      </div>}
    </div>
  </main>
}
