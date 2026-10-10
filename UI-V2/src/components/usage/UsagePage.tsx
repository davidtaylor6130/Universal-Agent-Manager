import { useEffect, useMemo, useRef, useState } from 'react'
import { ArrowLeft, RefreshCw, SlidersHorizontal } from 'lucide-react'
import { useUsageHistory } from '../../hooks/useUsageHistory'
import { EMPTY_USAGE_TOTALS, sumProviderUsage, USAGE_PROVIDERS, USAGE_WINDOWS, formatUsageCost, formatUsageCount, usageCost, usageWindow, type UsageProviderId, type UsageMetric, type UsageModel, type UsagePriceOverride, type UsageTotals, type UsageWindowDays } from '../../services/usageHistory'
import { useAppStore } from '../../store/useAppStore'
import { providerShortName } from '../../utils/providerMetadata'
import { ProviderLogo } from '../shared/ProviderLogo'
import { Button, IconButton, MenuSelect } from '../ui'
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

function TokenBreakdown({ total }: { total: UsageTotals }) {
  const fields = [['Input', total.inputTokens], ['Cached input', total.cachedInputTokens], ['Cache writes', total.cacheWriteTokens], ['Output', total.outputTokens]] as const
  return <dl className="grid gap-2 text-xs">
    {fields.map(([label, count]) => <div key={label} className="grid grid-cols-[9rem_1fr_4rem] items-center gap-3">
      <dt>{label}</dt><dd><div className="h-1" style={{ background: 'var(--border)' }}><div className="h-full" style={{ width: `${total.totalTokens ? count / total.totalTokens * 100 : 0}%`, background: 'var(--accent)' }} /></div></dd>
      <dd className="text-right tabular-nums" title={count.toLocaleString()}>{formatUsageCount(count)}</dd>
    </div>)}
    <div className="flex justify-between"><dt>Reasoning, included in output</dt><dd>{formatUsageCount(total.reasoningTokens)}</dd></div>
  </dl>
}

function PriceEditor({ model, prices, onSave, onClose }: { model: UsageModel; prices: UsagePriceOverride[]; onSave: (prices: UsagePriceOverride[]) => void; onClose: () => void }) {
  const existing = prices.find(price => modelKey(price) === modelKey(model))
  const [fields, setFields] = useState(() => priceFields.map(([key]) => existing?.[key]?.toString() ?? ''))
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
  const [hostId, setHostId] = useState('local')
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
  const currentModel = data?.models.find(model => modelKey(model) === selectedModel)
  const refresh = () => { setRange(usageWindow(days)); history.refresh() }
  const warnings = data?.providers.filter(provider => provider.errors || provider.invalidLines || provider.oversizedLines || provider.pendingLines) ?? []
  return <main aria-label="Usage workspace" className="flex h-full min-h-0 min-w-0 flex-1 flex-col" style={{ background: 'var(--bg)', color: 'var(--text)' }} onKeyDown={event => {
    if (event.key === 'Escape' && !event.defaultPrevented && !document.querySelector('[aria-modal="true"]') && !editingPrice) { event.preventDefault(); onClose() }
  }}>
    <header className="flex flex-wrap items-center gap-3 border-b px-4 py-3" style={{ borderColor: 'var(--border)' }}>
      <IconButton icon={<ArrowLeft size={16} />} label="Back from usage" onClick={onClose} />
      <h1 ref={headingRef} tabIndex={-1} className="text-sm font-semibold">Usage</h1>
      <div className="max-w-48"><MenuSelect label="Usage host" value={hostId} options={[...(!hosts.some(host => host.id === 'local') ? [{ value: 'local', label: 'This computer' }] : []), ...hosts.map(host => ({ value: host.id, label: host.label }))]} onChange={value => { setHostId(value); setSelectedModel(null); setEditingPrice(false) }} /></div>
      <div role="group" aria-label="Usage metric" className="flex gap-1">{(['cost', 'tokens', 'limits'] as const).map(value => <Button key={value} size="sm" variant={metric === value ? 'primary' : 'ghost'} aria-pressed={metric === value} onClick={() => setMetric(value)}>{value[0].toUpperCase() + value.slice(1)}</Button>)}</div>
      <div role="group" aria-label="Usage period" className="flex gap-1">{USAGE_WINDOWS.map(option => <Button size="sm" key={option.days} disabled={metric === 'limits'} variant={days === option.days ? 'secondary' : 'ghost'} aria-pressed={days === option.days} onClick={() => { setDays(option.days); setRange(usageWindow(option.days)); setSelectedModel(null); setEditingPrice(false) }}>{option.label}</Button>)}</div>
      {metric !== 'limits' && <IconButton icon={<RefreshCw size={15} />} label="Refresh usage" aria-busy={history.loading} disabled={history.loading} onClick={refresh} />}
    </header>
    <div className="min-h-0 flex-1 overflow-y-auto p-4 md:p-6">
      {metric === 'limits' ? <UsageLimits executionHostId={hostId} /> : <div className="mx-auto grid max-w-6xl gap-6" aria-busy={history.loading}>
        {history.loading && <p role="status" className="text-xs">Loading usage history…{data && !history.kept ? ` ${data.filesChecked.toLocaleString()} of ${data.filesTotal.toLocaleString()} files` : ''}</p>}
        {history.error && <div role="alert" className="flex flex-wrap items-center gap-3 text-xs" style={{ color: 'var(--red)' }}>{history.error}<Button size="sm" onClick={refresh}>Retry</Button></div>}
        {data && <div className="grid gap-6" style={{ opacity: history.kept ? 0.5 : 1 }} data-kept={history.kept || undefined}>
          {history.kept && <p className="text-xs">Showing the previous answer while the selected range loads.</p>}
          {!data.complete && <p className="text-xs">Partial history. Totals will update as loading continues.</p>}
          {data.listingLimited && <p className="text-xs">Some history is outside the scan limit. These totals are incomplete.</p>}
          {warnings.map(provider => <p key={provider.providerId} className="text-xs">{providerLabel(provider.providerId)}: {provider.errors} read errors, {provider.invalidLines} invalid records, {provider.oversizedLines} oversized records, {provider.pendingLines} unfinished lines.</p>)}
          <section className="grid gap-6 lg:grid-cols-[18rem_minmax(0,1fr)]">
            <div className="grid content-start gap-4">
              <div><div className="text-4xl font-semibold tabular-nums" title={totals.totalTokens.toLocaleString()}>{amount(totals)}</div><p className="mt-2 text-xs">{totals.sessions.toLocaleString()} sessions{metric === 'cost' ? ' · API equivalent' : ' · tokens'}</p></div>
              {metric === 'cost' && <p className="text-xs">Reported: {totals.reportedCostTokens || !totals.totalTokens ? new Intl.NumberFormat(undefined, { style: 'currency', currency: 'USD' }).format(totals.reportedCostUsd) : 'Unknown'}. Estimated: {totals.estimatedCostTokens || !totals.totalTokens ? new Intl.NumberFormat(undefined, { style: 'currency', currency: 'USD' }).format(totals.estimatedCostUsd) : 'Unknown'}.{totals.unpricedTokens > 0 && ` ${formatUsageCount(totals.unpricedTokens)} tokens are unpriced.`}</p>}
              {!hiddenProviders.has('copilot-cli') && data.providers.some(provider => provider.providerId === 'copilot-cli' && provider.records > 0) && <p className="text-xs">Copilot history is grouped by saved checkpoint time.</p>}
              <div className="grid gap-3">{data.providers.map(provider => <button key={provider.providerId} type="button" aria-label={`Include ${providerLabel(provider.providerId)} usage`} aria-pressed={!hiddenProviders.has(provider.providerId)} className="uam-btn uam-btn--ghost uam-usage-model-row text-xs" style={{ opacity: hiddenProviders.has(provider.providerId) ? 0.5 : 1 }} onClick={() => { setHiddenProviders(previous => { const next = new Set(previous); if (next.has(provider.providerId)) next.delete(provider.providerId); else next.add(provider.providerId); return next }); setSelectedModel(null); setEditingPrice(false) }}><span className="flex items-center gap-2"><span className="h-2 w-2" aria-hidden="true" style={{ background: providerColors[USAGE_PROVIDERS.indexOf(provider.providerId)] }} /><ProviderLogo providerId={provider.providerId} size={16} />{providerLabel(provider.providerId)}</span><span className="tabular-nums" title={provider.available ? `${provider.files} history files` : 'Historical usage reader unavailable'}>{provider.available ? amount(provider) : 'Unavailable'}</span></button>)}</div>
            </div>
            <div className="grid content-start gap-4"><h2 className="text-sm font-medium">{hourly ? 'Hourly usage' : 'Daily usage'}</h2>
              <div className="relative flex h-40 items-end gap-1 border-b" role="img" aria-label={`${hourly ? 'Hourly' : 'Daily'} ${metric === 'cost' ? 'known costs, unpriced tokens excluded' : 'tokens'} from loaded history`} style={{ borderColor: 'var(--border)' }}>
                {metric === 'cost' && peak === 0 && totals.unpricedTokens > 0 && <span className="absolute inset-0 flex items-center justify-center text-xs">No priced usage in this range.</span>}
                {periods.map(period => <div key={period.startMs} className="flex h-full min-w-0 flex-1 items-end" title={`${new Date(period.startMs).toLocaleString()} · ${amount(period)}`}><div className="flex w-full flex-col-reverse" style={{ height: `${peak ? numeric(period) / peak * 100 : 0}%`, minHeight: numeric(period) > 0 ? 2 : 0 }}>{period.bands.map(band => <div key={band.providerId} className="w-full" title={`${providerLabel(band.providerId)} · ${amount(band)}`} style={{ height: `${numeric(period) ? numeric(band) / numeric(period) * 100 : 0}%`, background: providerColors[USAGE_PROVIDERS.indexOf(band.providerId)] }} />)}</div></div>)}
              </div><p className="text-xs">{new Date(data.startMs).toLocaleString()} to {new Date(data.endMs).toLocaleString()}{!hourly ? ' · Daily buckets use UTC' : ''}{metric === 'cost' && totals.unpricedTokens > 0 ? ' · Unpriced usage excluded' : ''}</p>
            </div>
          </section>
          <section className="grid gap-3"><h2 className="text-sm font-medium">Totals</h2><TokenBreakdown total={totals} />
            {metric === 'cost' && <dl className="flex flex-wrap gap-6 text-xs"><div><dt>Estimated cache savings</dt><dd className="mt-1 tabular-nums">{totals.estimatedCostTokens || !totals.totalTokens ? new Intl.NumberFormat(undefined, { style: 'currency', currency: 'USD' }).format(totals.estimatedCacheSavingsUsd) : 'Unknown'}</dd></div><div><dt>Unpriced tokens</dt><dd className="mt-1">{formatUsageCount(totals.unpricedTokens)}</dd></div></dl>}
          </section>
          <section className="grid gap-3"><div className="flex items-center justify-between gap-3"><h2 className="text-sm font-medium">Breakdown</h2><div role="group" aria-label="Usage breakdown" className="flex gap-1"><Button size="sm" variant={breakdown === 'model' ? 'secondary' : 'ghost'} aria-pressed={breakdown === 'model'} onClick={() => setBreakdown('model')}>Models</Button><Button size="sm" variant={breakdown === 'time' ? 'secondary' : 'ghost'} aria-pressed={breakdown === 'time'} onClick={() => setBreakdown('time')}>Time</Button></div></div>
            {breakdown === 'model' ? <div>{models.length === 0 && <p className="py-4 text-xs">No model usage in this range.</p>}{models.slice(0, modelLimit).map(model => <div key={modelKey(model)} className="border-b" style={{ borderColor: 'var(--border)' }}>
              <button className="uam-btn uam-btn--ghost uam-usage-model-row text-xs" aria-label={`Inspect ${providerLabel(model.providerId)} ${model.modelId} ${model.speed}`} aria-expanded={modelKey(model) === selectedModel} onClick={() => { setSelectedModel(previous => previous === modelKey(model) ? null : modelKey(model)); setEditingPrice(false) }}>
                <span className="flex min-w-0 items-center gap-2"><ProviderLogo providerId={model.providerId} size={16} /><span className="truncate" title={model.modelId}>{model.modelId}</span>{model.speed !== 'standard' && <span>{model.speed}</span>}</span><span className="shrink-0 tabular-nums">{amount(model)}</span>
              </button>
              {currentModel === model && <div className="grid gap-3 pb-4"><TokenBreakdown total={model} /><p className="text-xs">{model.sessions} sessions · {model.records} usage records · {formatUsageCount(model.unpricedTokens)} unpriced tokens</p><Button size="sm" leadingIcon={<SlidersHorizontal size={13} />} onClick={() => setEditingPrice(value => !value)}>Model prices</Button>{editingPrice && <PriceEditor model={model} prices={prices} onSave={setPrices} onClose={() => setEditingPrice(false)} />}</div>}
            </div>)}{models.length > modelLimit && <Button size="sm" className="mt-3" onClick={() => setModelLimit(value => value + 100)}>Show more models ({models.length - modelLimit} remaining)</Button>}</div> : <div>{[...periods].reverse().map(period => <div key={period.startMs} className="flex justify-between gap-4 border-b py-3 text-xs" style={{ borderColor: 'var(--border)' }}><span>{new Date(period.startMs).toLocaleString(undefined, hourly ? { hour: 'numeric', month: 'short', day: 'numeric' } : { year: 'numeric', month: 'short', day: 'numeric', timeZone: 'UTC' })}{!hourly ? ' UTC' : ''}</span><span className="tabular-nums">{amount(period)}</span></div>)}</div>}
          </section>
          {(data.duplicates > 0 || data.unidentifiedRecords > 0 || data.providers.some(provider => provider.suppressedForkRecords > 0)) && <details className="text-xs"><summary className="cursor-pointer">History diagnostics</summary><p className="mt-2">{data.duplicates} duplicate records, {data.unidentifiedRecords} records without stable identity, {data.providers.reduce((sum, provider) => sum + provider.suppressedForkRecords, 0)} copied fork records suppressed.</p></details>}
        </div>}
      </div>}
    </div>
  </main>
}
