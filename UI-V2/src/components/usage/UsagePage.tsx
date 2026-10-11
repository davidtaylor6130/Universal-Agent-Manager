import { useEffect, useMemo, useRef, useState, type CSSProperties, type ReactNode } from 'react'
import { ArrowLeft, RefreshCw, SlidersHorizontal } from 'lucide-react'
import { useUsageHistory } from '../../hooks/useUsageHistory'
import { ALL_MACHINES_HOST_ID, EMPTY_USAGE_TOTALS, sumProviderUsage, USAGE_PROVIDERS, USAGE_WINDOWS, formatUsageCost, formatUsageCount, usageCost, usageWindow, type UsageProviderId, type UsageMetric, type UsageModel, type UsagePriceOverride, type UsageTotals, type UsageWindowDays } from '../../services/usageHistory'
import { useAppStore } from '../../store/useAppStore'
import { providerShortName } from '../../utils/providerMetadata'
import { ProviderLogo } from '../shared/ProviderLogo'
import { Button, IconButton, MenuSelect, StatusDot, cx } from '../ui'
import { UsageLimits } from './UsageLimits'
import './usage.css'

const PRICE_KEY = 'uam-usage-prices-v1'
const priceFields = [ ['inputPerMillion', 'Input'], ['cachedInputPerMillion', 'Cached input'], ['cacheWritePerMillion', 'Cache writes'], ['outputPerMillion', 'Output'] ] as const

export function parseUsagePrices(value: unknown): UsagePriceOverride[] {
  if (!Array.isArray(value) || value.length > 200) return []
  const keys = new Set<string>()
  const valid = value.every(row => {
    if (!row || typeof row !== 'object' || Array.isArray(row) || !USAGE_PROVIDERS.includes(row.providerId) || typeof row.modelId !== 'string' || !row.modelId || row.modelId.length > 512 || !['standard', 'fast', 'ultrafast'].includes(row.speed)
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
const providerColors = ['var(--accent)', 'var(--blue)', 'var(--green)', 'var(--teal)', 'var(--yellow)', 'var(--text-2)']

const money = (value: number) => new Intl.NumberFormat(undefined, { style: 'currency', currency: 'USD', maximumFractionDigits: value > 0 && value < 0.01 ? 4 : 2 }).format(value)
const percentOf = (part: number, whole: number) => whole > 0 ? `${Math.round(part / whole * 100)}%` : '0%'
/** Token segments are disjoint: input + cache read + cache write + output (reasoning is a hatched part of output). */
const TOKEN_SEGMENTS = [
  { key: 'input', label: 'Input', color: 'var(--blue)', value: (total: UsageTotals) => total.inputTokens },
  { key: 'cache', label: 'Cache read', color: 'var(--teal)', value: (total: UsageTotals) => total.cachedInputTokens },
  { key: 'cache-write', label: 'Cache write', color: 'var(--purple)', value: (total: UsageTotals) => total.cacheWriteTokens },
  { key: 'output', label: 'Output', color: 'var(--green)', value: (total: UsageTotals) => total.outputTokens - total.reasoningTokens },
  { key: 'reasoning', label: 'Reasoning, included in output', color: 'var(--yellow)', value: (total: UsageTotals) => total.reasoningTokens },
] as const

/** Stacked token composition: one horizontal bar, proportional segments, exact counts in the legend. */
function TokenStack({ total, height = 10 }: { total: UsageTotals; height?: number }) {
  return <span className="uam-usage-stack" aria-hidden style={{ height }}>
    {TOKEN_SEGMENTS.map(segment => { const value = segment.value(total); return value > 0 ? <span key={segment.key} className={segment.key === 'reasoning' ? 'uam-usage-hatch' : undefined} style={{ flexGrow: value, '--c': segment.color } as CSSProperties} title={`${segment.label} · ${value.toLocaleString()}`} /> : null })}
  </span>
}

function TokenLegend({ total }: { total: UsageTotals }) {
  return <dl className="uam-usage-legend">
    {TOKEN_SEGMENTS.map(segment => { const value = segment.value(total); return <div key={segment.key} className="uam-usage-legend-row">
      <dt><span className={cx('uam-usage-swatch', segment.key === 'reasoning' && 'uam-usage-hatch')} style={{ '--c': segment.color } as CSSProperties} aria-hidden />{segment.label}</dt>
      <dd className="tabular-nums" title={value.toLocaleString()}>{formatUsageCount(value)} <span className="uam-usage-muted">{percentOf(value, total.totalTokens)}</span></dd>
    </div> })}
  </dl>
}

/** Spend bar: solid = reported charge, hatched = local estimate. Unknown prices draw nothing. */
function SpendBar({ total, max, color }: { total: UsageTotals; max: number; color: string }) {
  const cost = total.reportedCostUsd + total.estimatedCostUsd
  const estimated = cost > 0 ? total.estimatedCostUsd / cost : 0
  return <span className="uam-usage-stack" aria-hidden style={{ width: max > 0 ? `${Math.min(100, cost / max * 100)}%` : '0%' }}>
    <span style={{ flexGrow: 1 - estimated, '--c': color } as CSSProperties} />
    <span className="uam-usage-hatch" style={{ flexGrow: estimated, '--c': color } as CSSProperties} />
  </span>
}

function StatTile({ label, value, sub, title, swatch }: { label: string; value: string; sub?: string; title?: string; swatch?: ReactNode }) {
  return <div className="uam-usage-tile">
    <span className="uam-usage-label">{swatch}{label}</span>
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
  const history = useUsageHistory(hostId, range, prices, metric !== 'limits')
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
    const width = hourly ? 3600000 : 86400000
    const present = new Map<number, typeof data.providerDays>()
    for (const row of hourly ? data.providerHours : data.providerDays) {
      if (hiddenProviders.has(row.providerId)) continue
      const rows = present.get(row.startMs) ?? []
      rows.push(row)
      present.set(row.startMs, rows)
    }
    const result = []
    for (let startMs = Math.floor(data.startMs / width) * width; startMs < data.endMs; startMs += width) {
      const bands = present.get(startMs) ?? []
      result.push({ ...EMPTY_USAGE_TOTALS, ...sumProviderUsage(bands), startMs, bands })
    }
    return result
  }, [data, hourly, hiddenProviders])
  const amount = (total: UsageTotals) => metric === 'cost' ? formatUsageCost(total) : formatUsageCount(total.totalTokens)
  const numeric = (total: UsageTotals) => metric === 'cost' ? usageCost(total) ?? 0 : total.totalTokens
  const peak = Math.max(0, ...periods.map(numeric))
  const maxModel = Math.max(0, ...models.map(numeric))
  const maxProvider = Math.max(0, ...(data?.providers ?? []).map(numeric))
  const currentModel = data?.models.find(model => modelKey(model) === selectedModel)
  const refresh = () => { setRange(usageWindow(days)); history.refresh() }
  const warnings = data?.providers.filter(provider => provider.errors || provider.invalidLines || provider.oversizedLines || provider.pendingLines) ?? []
  // Re-keying the plot replays the grow-in animation when the metric, period or provider filter changes.
  const plotKey = [metric, days, hostId, [...hiddenProviders].sort().join(',')].join('|')
  const axisValue = (value: number) => metric === 'cost' ? money(value) : formatUsageCount(value)
  const tipLabel = (startMs: number) => new Date(startMs).toLocaleString(undefined, hourly ? { hour: 'numeric', month: 'short', day: 'numeric' } : { month: 'short', day: 'numeric', timeZone: 'UTC' })
  const noCostData = metric === 'cost' && totals.totalTokens > 0 && totals.reportedCostTokens + totals.estimatedCostTokens === 0
  return <main aria-label="Usage workspace" className="flex h-full min-h-0 min-w-0 flex-1 flex-col" style={{ background: 'var(--bg)', color: 'var(--text)' }} onKeyDown={event => {
    if (event.key === 'Escape' && !event.defaultPrevented && !document.querySelector('[aria-modal="true"]') && !editingPrice) { event.preventDefault(); onClose() }
  }}>
    <header className="flex flex-wrap items-center gap-3 border-b px-4 py-3" style={{ borderColor: 'var(--border)' }}>
      <IconButton icon={<ArrowLeft size={16} />} label="Back from usage" onClick={onClose} />
      <h1 ref={headingRef} tabIndex={-1} className="text-sm font-semibold">Usage</h1>
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
          {history.kept && <p className="text-xs uam-usage-muted">Showing the previous answer while the selected range loads.</p>}
          {data.asOfMs && <time className="text-xs uam-usage-muted" dateTime={new Date(data.asOfMs).toISOString()}>Updated {new Date(data.asOfMs).toLocaleString()}</time>}
          {metric === 'cost' && data.pricing && <div className="grid gap-1 text-xs uam-usage-muted">
            <p>{data.pricing.refreshing ? 'Updating reference prices…' : data.pricing.status === 'unavailable' ? 'Using verified model references where available.' : `Using ${data.pricing.status === 'cached' ? 'cached' : 'current'} reference prices.`}{data.pricing.fetchedAtMs && ` Retrieved ${new Date(data.pricing.fetchedAtMs).toLocaleString()}.`}</p>
            {data.pricing.error && <p>{data.pricing.error}</p>}
            {data.pricing.aliasError && <p>{data.pricing.aliasError}</p>}
          </div>}
          {!data.complete && <p className="uam-usage-note">Partial history. Totals include available and cached records.</p>}
          {data.coverage && <ul aria-label="Usage machine coverage" className="flex flex-wrap gap-2 text-xs">{data.coverage.map(machine => <li key={machine.executionHostId} className="uam-usage-chip is-static">
            <StatusDot tone={machine.status === 'ready' ? 'success' : machine.status === 'loading' ? 'info' : machine.status === 'cached' ? 'warning' : 'error'} />
            <span>{machine.label || machine.executionHostId}: {machine.status === 'ready' ? 'Current' : machine.status === 'cached' ? 'Cached' : machine.status === 'loading' ? 'Loading' : 'Unavailable'}</span>
            {machine.asOfMs && <span className="uam-usage-muted">{new Date(machine.asOfMs).toLocaleString()}</span>}{machine.error && <span className="uam-usage-muted">{machine.error}</span>}
          </li>)}</ul>}
          {data.listingLimited && <p className="uam-usage-note">Some history is outside the scan limit. These totals are incomplete.</p>}
          {warnings.map(provider => <p key={provider.providerId} className="text-xs uam-usage-muted">{providerLabel(provider.providerId)}: {provider.errors} read errors, {provider.invalidLines} invalid records, {provider.oversizedLines} oversized records, {provider.pendingLines} unfinished lines.</p>)}
          {noCostData && <p className="uam-usage-note">No cost data for this range. Tokens were recorded, but no prices are known. Add model prices under Breakdown, or switch to Tokens.</p>}
          {totals.totalTokens === 0 && !history.loading && <p className="uam-usage-empty">No usage recorded in this range.</p>}
          <section className="grid gap-6 lg:grid-cols-[18rem_minmax(0,1fr)]">
            <div className="grid content-start gap-4">
              <div className="uam-usage-tiles">
                <StatTile label={metric === 'cost' ? 'API equivalent' : 'Total tokens'} value={amount(totals)} title={totals.totalTokens.toLocaleString()} sub={`${totals.sessions.toLocaleString()} sessions`} />
                {metric === 'cost' ? <>
                  <StatTile label="Reported" swatch={<span className="uam-usage-swatch" style={{ '--c': 'var(--text-2)' } as CSSProperties} aria-hidden />} value={totals.reportedCostTokens || !totals.totalTokens ? money(totals.reportedCostUsd) : 'Unknown'} sub="Provider-reported" />
                  <StatTile label="Estimated" swatch={<span className="uam-usage-swatch uam-usage-hatch" style={{ '--c': 'var(--text-2)' } as CSSProperties} aria-hidden />} value={totals.estimatedCostTokens || !totals.totalTokens ? money(totals.estimatedCostUsd) : 'Unknown'} sub="From known list prices" />
                </> : <>
                  <StatTile label="Cache read" value={percentOf(totals.cachedInputTokens, totals.totalTokens)} sub={`${formatUsageCount(totals.cachedInputTokens)} tokens`} />
                  <StatTile label="Reasoning" value={percentOf(totals.reasoningTokens, totals.totalTokens)} sub={`${formatUsageCount(totals.reasoningTokens)} tokens`} />
                </>}
              </div>
              {metric === 'cost' && <div className="grid gap-2">
                <SpendBar total={totals} max={usageCost(totals) ?? 0} color="var(--text-2)" />
                <p className="text-xs uam-usage-muted">Reported: {totals.reportedCostTokens || !totals.totalTokens ? money(totals.reportedCostUsd) : 'Unknown'}. Estimated: {totals.estimatedCostTokens || !totals.totalTokens ? money(totals.estimatedCostUsd) : 'Unknown'}.{totals.unpricedTokens > 0 && ` ${formatUsageCount(totals.unpricedTokens)} tokens are unpriced.`}</p>
              </div>}
              {!hiddenProviders.has('copilot-cli') && data.providers.some(provider => provider.providerId === 'copilot-cli' && provider.records > 0) && <p className="text-xs uam-usage-muted">Copilot history is grouped by saved checkpoint time.</p>}
              <div className="uam-usage-chips" role="group" aria-label="Providers">{data.providers.map(provider => {
                const off = hiddenProviders.has(provider.providerId)
                return <button key={provider.providerId} type="button" aria-label={`Include ${providerLabel(provider.providerId)} usage`} aria-pressed={!off} className={cx('uam-usage-chip', off && 'is-off')} onClick={() => { setHiddenProviders(previous => { const next = new Set(previous); if (next.has(provider.providerId)) next.delete(provider.providerId); else next.add(provider.providerId); return next }); setSelectedModel(null); setEditingPrice(false) }}>
                  <ProviderLogo providerId={provider.providerId} size={14} />
                  <span className="uam-usage-chip-name">{providerLabel(provider.providerId)}</span>
                  <span className="uam-usage-chip-value tabular-nums" title={provider.available ? `${provider.files} history files` : 'Historical usage reader unavailable'}>{provider.available || provider.records > 0 ? amount(provider) : 'Unavailable'}</span>
                  <span className="uam-usage-chip-bar" aria-hidden><span style={{ width: `${maxProvider ? numeric(provider) / maxProvider * 100 : 0}%`, background: providerColors[USAGE_PROVIDERS.indexOf(provider.providerId)] }} /></span>
                </button>
              })}</div>
            </div>
            <div className="grid content-start gap-3">
              <div className="flex flex-wrap items-center justify-between gap-2"><h2 className="text-sm font-medium">{hourly ? 'Hourly usage' : 'Daily usage'}</h2>
                {metric === 'cost' && <div className="uam-usage-key text-xs" aria-hidden><span><i className="uam-usage-swatch" style={{ '--c': 'var(--text-2)' } as CSSProperties} />Reported</span><span><i className="uam-usage-swatch uam-usage-hatch" style={{ '--c': 'var(--text-2)' } as CSSProperties} />Estimated</span></div>}
              </div>
              <div className="uam-usage-chart">
                <div className="uam-usage-axis text-xs tabular-nums" aria-hidden><span>{axisValue(peak)}</span><span>{axisValue(peak / 2)}</span><span>0</span></div>
                <div key={plotKey} role="img" aria-label={`${hourly ? 'Hourly' : 'Daily'} ${metric === 'cost' ? 'known costs, unpriced tokens excluded' : 'tokens'} from loaded history`} className="uam-usage-plot" style={{ gap: periods.length > 31 ? 1 : 4 }}>
                  {metric === 'cost' && peak === 0 && totals.unpricedTokens > 0 && <span className="absolute inset-0 flex items-center justify-center text-xs">No priced usage in this range.</span>}
                  {periods.map((period, index) => <div key={period.startMs} className="uam-usage-col" style={{ '--i': Math.min(index, 40) } as CSSProperties} title={`${new Date(period.startMs).toLocaleString()} · ${amount(period)}`}>
                    <div className="uam-usage-col-stack" style={{ height: `${peak ? numeric(period) / peak * 100 : 0}%`, minHeight: numeric(period) > 0 ? 2 : 0 }}>{period.bands.map(band => {
                      const value = numeric(band), cost = band.reportedCostUsd + band.estimatedCostUsd
                      const estimated = metric === 'cost' && cost > 0 ? band.estimatedCostUsd / cost : 0
                      return <div key={band.providerId} className="uam-usage-band" title={`${providerLabel(band.providerId)} · ${amount(band)}`} style={{ height: `${numeric(period) ? value / numeric(period) * 100 : 0}%`, '--c': providerColors[USAGE_PROVIDERS.indexOf(band.providerId)] } as CSSProperties}>
                        <span className="uam-usage-band-part" style={{ flexGrow: 1 - estimated }} /><span className="uam-usage-band-part uam-usage-hatch" style={{ flexGrow: estimated }} />
                      </div>
                    })}</div>
                    <span className="uam-usage-col-tip" aria-hidden>{tipLabel(period.startMs)} · {amount(period)}</span>
                  </div>)}
                </div>
              </div>
              <p className="text-xs uam-usage-muted">{new Date(data.startMs).toLocaleString()} to {new Date(data.endMs).toLocaleString()}{!hourly ? ' · Daily buckets use UTC' : ''}{metric === 'cost' && totals.unpricedTokens > 0 ? ' · Unpriced usage excluded' : ''}</p>
            </div>
          </section>
          <section className="grid gap-4"><div className="flex flex-wrap items-baseline justify-between gap-3"><h2 className="text-sm font-medium">Totals</h2>{metric === 'cost' && <p className="text-xs uam-usage-muted">Estimated cache savings: {totals.estimatedCostTokens || !totals.totalTokens ? money(totals.estimatedCacheSavingsUsd) : 'Unknown'}</p>}</div>
            <TokenStack total={totals} height={12} />
            <TokenLegend total={totals} />
            <div className="grid gap-3 pt-2"><h3 className="uam-usage-label">Token mix by provider</h3>
              {data.providers.filter(provider => !hiddenProviders.has(provider.providerId) && provider.totalTokens > 0).map(provider => <div key={provider.providerId} className="grid gap-1.5">
                <div className="flex items-center justify-between gap-3 text-xs"><span className="flex min-w-0 items-center gap-2"><ProviderLogo providerId={provider.providerId} size={14} />{providerLabel(provider.providerId)}</span><span className="tabular-nums">{formatUsageCount(provider.totalTokens)} tokens</span></div>
                <TokenStack total={provider} height={6} />
              </div>)}
              {data.providers.every(provider => hiddenProviders.has(provider.providerId) || provider.totalTokens === 0) && <p className="text-xs uam-usage-muted">No provider usage to compare.</p>}
            </div>
          </section>
          <section className="grid gap-3"><div className="flex items-center justify-between gap-3"><h2 className="text-sm font-medium">Breakdown</h2><div role="group" aria-label="Usage breakdown" className="uam-usage-seg"><Button size="sm" variant={breakdown === 'model' ? 'secondary' : 'ghost'} aria-pressed={breakdown === 'model'} onClick={() => setBreakdown('model')}>Models</Button><Button size="sm" variant={breakdown === 'time' ? 'secondary' : 'ghost'} aria-pressed={breakdown === 'time'} onClick={() => setBreakdown('time')}>Time</Button></div></div>
            {breakdown === 'model' ? <div>{models.length === 0 && <p className="py-4 text-xs uam-usage-muted">No model usage in this range.</p>}{models.slice(0, modelLimit).map(model => <div key={modelKey(model)} className="border-b" style={{ borderColor: 'var(--border)' }}>
              <button className="uam-btn uam-btn--ghost uam-usage-model-row text-xs" aria-label={`Inspect ${providerLabel(model.providerId)} ${model.modelId} ${model.speed}`} aria-expanded={modelKey(model) === selectedModel} onClick={() => { setSelectedModel(previous => previous === modelKey(model) ? null : modelKey(model)); setEditingPrice(false) }}>
                <span className="flex min-w-0 items-center gap-2"><ProviderLogo providerId={model.providerId} size={16} /><span className="truncate" title={model.modelId}>{model.modelId}</span>{model.speed !== 'standard' && <span className="uam-usage-muted">{model.speed}</span>}</span><span className="shrink-0 tabular-nums">{amount(model)}</span>
                <span className="uam-usage-model-bar">{metric === 'cost' ? <SpendBar total={model} max={maxModel} color={providerColors[USAGE_PROVIDERS.indexOf(model.providerId)]} /> : <TokenStack total={model} height={6} />}</span>
              </button>
              {currentModel === model && <div className="grid gap-3 pb-4"><TokenLegend total={model} /><p className="text-xs uam-usage-muted">{model.sessions} sessions · {model.records} usage records · {formatUsageCount(model.unpricedTokens)} unpriced tokens</p><Button size="sm" leadingIcon={<SlidersHorizontal size={13} />} onClick={() => setEditingPrice(value => !value)}>Model prices</Button>{editingPrice && <PriceEditor model={model} prices={prices} onSave={setPrices} onClose={() => setEditingPrice(false)} />}</div>}
            </div>)}{models.length > modelLimit && <Button size="sm" className="mt-3" onClick={() => setModelLimit(value => value + 100)}>Show more models ({models.length - modelLimit} remaining)</Button>}</div> : <div>{[...periods].reverse().map(period => <div key={period.startMs} className="grid grid-cols-[minmax(0,1fr)_auto] items-center gap-x-4 gap-y-1 border-b py-3 text-xs" style={{ borderColor: 'var(--border)' }}><span>{new Date(period.startMs).toLocaleString(undefined, hourly ? { hour: 'numeric', month: 'short', day: 'numeric' } : { year: 'numeric', month: 'short', day: 'numeric', timeZone: 'UTC' })}{!hourly ? ' UTC' : ''}</span><span className="tabular-nums">{amount(period)}</span><span className="uam-usage-model-bar col-span-2"><span className="uam-usage-stack" style={{ width: peak ? `${numeric(period) / peak * 100}%` : '0%' }}><span style={{ flexGrow: 1, '--c': 'var(--text-3)' } as CSSProperties} /></span></span></div>)}</div>}
          </section>
          {(data.duplicates > 0 || data.unidentifiedRecords > 0 || data.providers.some(provider => provider.suppressedForkRecords > 0)) && <details className="text-xs"><summary className="cursor-pointer">History diagnostics</summary><p className="mt-2">{data.duplicates} duplicate records, {data.unidentifiedRecords} records without stable identity, {data.providers.reduce((sum, provider) => sum + provider.suppressedForkRecords, 0)} copied fork records suppressed.</p></details>}
        </div>}
      </div>}
    </div>
  </main>
}
