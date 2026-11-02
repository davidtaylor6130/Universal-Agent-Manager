import { useEffect, useId, useMemo, useRef, useState, type CSSProperties, type KeyboardEvent } from 'react'
import { ArrowLeft, Check, ChevronRight, Info, RefreshCw, SlidersHorizontal, X } from 'lucide-react'
import { useUsageHistory } from '../../hooks/useUsageHistory'
import { ALL_MACHINES_HOST_ID, EMPTY_USAGE_TOTALS, sumProviderUsage, USAGE_PROVIDERS, USAGE_WINDOWS, formatUsageCost, formatUsageCount, usageCost, usageCostBasis, usageWindow, type UsageCostBasis, type UsageProvider, type UsageProviderId, type UsageMetric, type UsageModel, type UsagePriceOverride, type UsageTotals, type UsageWindowDays } from '../../services/usageHistory'
import { useAppStore } from '../../store/useAppStore'
import { ANTIGRAVITY_ENABLED, CLAUDE_ENABLED, CODEX_ENABLED, COPILOT_ENABLED, GEMINI_ENABLED, OPENCODE_ENABLED } from '../../config/buildFeatures'
import { ANTIGRAVITY_CLI_PROVIDER_ID, CLAUDE_CLI_PROVIDER_ID, CODEX_CLI_PROVIDER_ID, COPILOT_CLI_PROVIDER_ID, GEMINI_CLI_PROVIDER_ID, OPENCODE_CLI_PROVIDER_ID, providerShortName } from '../../utils/providerMetadata'
import { ProviderLogo } from '../shared/ProviderLogo'
import { Button, Card, Disclosure, IconButton, MenuSelect, Notice, RowButton, SegmentedControl, StatusDot, Swap, Tooltip, ViewportMenu, cx, usePresence, Reveal } from '../ui'
import { reportAppFailure, reportAppNotice } from '../../utils/appFailure'
import { PERIOD_PHRASE, UsageLimits } from './UsageLimits'
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

/** Every provider row and chip uses the same states, so a provider without figures never reads as $0.00. Missing sources are not zero usage. */
const providerState = (provider: UsageProvider, collecting = false) => provider.records > 0 || provider.totalTokens > 0 ? 'used'
  : !provider.available ? 'unavailable' : collecting ? 'reading' : provider.errors || provider.invalidLines || provider.oversizedLines || provider.pendingLines ? 'incomplete' : 'idle'
const EMPTY_TEXT = { idle: 'No usage in this range', reading: 'Still reading history', incomplete: 'Usage history incomplete', unavailable: 'Usage history unavailable' } as const
const EMPTY_CHIP = { idle: 'No usage', reading: 'Reading…', incomplete: 'Incomplete', unavailable: 'Unavailable' } as const

const BASIS_LABEL: Record<UsageCostBasis, string> = { none: '', unpriced: '', reported: 'Reported', estimated: 'Estimated', mixed: 'Reported + est.', partial: 'Known cost' }
const source = (totals: UsageTotals) => {
  const reported = totals.reportedCostTokens > 0 || totals.reportedCostUsd > 0, estimated = totals.estimatedCostTokens > 0
  return reported && estimated ? 'Provider charges plus estimates from API list prices; estimates are not a bill.' : reported ? 'Charges reported by the provider.' : 'Estimated from API list prices; not a bill.'
}
/** Explanation read after the label, so screen readers hear e.g. "Estimated. Estimated from…" only once. */
export const usageCostBasisDetail = (totals: UsageTotals, pending = false) => usageCostBasis(totals) === 'partial'
  ? `${source(totals)} ${totals.unpricedTokens.toLocaleString()} tokens ${pending ? 'are still awaiting prices' : 'have no known price'} and are excluded.` : source(totals)
/** Text and line style say whether an amount is charged or estimated; colour is never the only cue. An empty slot keeps amounts aligned. */
function CostBasis({ totals, focusable = false, pending = false }: { totals: UsageTotals; focusable?: boolean; pending?: boolean }) {
  const basis = usageCostBasis(totals), label = BASIS_LABEL[basis]
  if (!label) return <span className="uam-usage-basis-slot" aria-hidden />
  const detail = usageCostBasisDetail(totals, pending)
  return <Tooltip label={detail}><span className="uam-usage-basis" tabIndex={focusable ? 0 : undefined} data-basis={basis} data-estimate={totals.estimatedCostTokens > 0 || undefined}>{label}<span className="sr-only">. {detail.replace(/^Estimated from/, 'From')}</span></span></Tooltip>
}

const money = (value: number) => new Intl.NumberFormat(undefined, { style: 'currency', currency: 'USD', maximumFractionDigits: value > 0 && value < 0.01 ? 4 : 2 }).format(value)
const percentOf = (part: number, whole: number) => whole > 0 ? `${Math.round(part / whole * 100)}%` : '0%'
/** Token segments are disjoint: input + cache read + cache write + output (reasoning is a part of output). */
const TOKEN_SEGMENTS = [
  { key: 'input', label: 'Input', color: 'var(--token-input)', value: (total: UsageTotals) => total.inputTokens },
  { key: 'cache', label: 'Cache read', color: 'var(--token-cache-read)', value: (total: UsageTotals) => total.cachedInputTokens },
  { key: 'cache-write', label: 'Cache write', color: 'var(--token-cache-write)', value: (total: UsageTotals) => total.cacheWriteTokens },
  { key: 'output', label: 'Output', color: 'var(--token-output)', value: (total: UsageTotals) => total.outputTokens - total.reasoningTokens },
  { key: 'reasoning', label: 'Reasoning', color: 'var(--token-reasoning)', value: (total: UsageTotals) => total.reasoningTokens },
] as const

/**
 * Token composition as one proportional bar; every segment keeps an exact tooltip. Cost keeps the solid bar with labels
 * inside wide segments; Tokens uses the gapped bar (hairline gaps, rounded ends) scaled by `width` (0-1) to a shared maximum.
 */
function TokenStack({ total, height = 14, labels = false, gapped = false, width, describe = false }: { total: UsageTotals; height?: number; labels?: boolean; gapped?: boolean; width?: number; describe?: boolean }) {
  const parts = TOKEN_SEGMENTS.map(segment => ({ segment, value: segment.value(total) })).filter(part => part.value > 0)
  const sum = parts.reduce((acc, part) => acc + part.value, 0)
  // A described bar stands alone (headline, Daily table) and names its mix; inside a labelled row it stays hidden.
  const a11y = describe ? { role: 'img', 'aria-label': parts.map(({ segment, value }) => `${segment.label} ${formatUsageCount(value)}`).join(', ') || 'No tokens' } : { 'aria-hidden': true }
  return <span className={cx('uam-usage-stack', gapped && 'uam-usage-stack--gapped')} {...a11y} style={{ height, width: width === undefined ? undefined : `${Math.min(100, Math.max(0, width) * 100)}%` }}>
    {parts.map(({ segment, value }) => {
      const share = value / sum
      return <span key={segment.key} title={`${segment.label} · ${value.toLocaleString()}`} style={{ flexGrow: value, '--c': segment.color } as CSSProperties}>
        <Reveal open={labels && share >= 0.12}><span className="uam-usage-stack-label">{share >= 0.3 ? `${segment.label} ` : ''}{formatUsageCount(value)}</span></Reveal>
      </span>
    })}
  </span>
}

function TokenLegend({ total }: { total: UsageTotals }) {
  return <ul className="uam-usage-key text-xs">
    {TOKEN_SEGMENTS.map(segment => { const value = segment.value(total); return <li key={segment.key} className="uam-usage-key-item">
      <i className="uam-usage-swatch" style={{ '--c': segment.color } as CSSProperties} aria-hidden />{segment.label}{segment.key === 'reasoning' && <span className="uam-usage-muted">(part of output)</span>}
      <span className="tabular-nums uam-usage-muted" title={value.toLocaleString()}>{formatUsageCount(value)} · {percentOf(value, total.totalTokens)}</span>
    </li> })}
  </ul>
}

/** Axis ticks at 1, 2, 2.5 or 5 × 10ⁿ, at most three intervals, with about a quarter of headroom over the peak. */
export function niceTicks(peak: number): number[] {
  if (!(peak > 0)) return [0]
  const top = peak * 1.25, raw = top / 3, power = 10 ** Math.floor(Math.log10(raw))
  const step = [1, 2, 2.5, 5, 10].map(multiple => multiple * power).find(value => value >= raw) ?? raw
  return Array.from({ length: Math.ceil(top / step - 1e-9) + 1 }, (_, index) => index * step)
}

/** Cost share as one solid bar in the provider colour; width is relative to the largest row. */
function CostBar({ cost, max, color }: { cost: number; max: number; color: string }) {
  return <span className="uam-usage-stack" aria-hidden style={{ width: max > 0 ? `${Math.min(100, cost / max * 100)}%` : '0%' }}>
    <span style={{ flexGrow: 1, '--c': color } as CSSProperties} />
  </span>
}

function StatTile({ label, value, detail, sub, title }: { label: string; value: string; detail?: string; sub?: string; title?: string }) {
  return <div className="uam-usage-tile">
    <span className="uam-usage-label">{label}</span>
    <span className="uam-usage-tile-value tabular-nums" title={title}>{value}{detail && <small>{detail}</small>}</span>
    <Reveal open={!!(sub)}><span className="uam-usage-muted text-xs">{sub}</span></Reveal>
  </div>
}

const count = (n: number, one: string, many = `${one}s`) => `${n.toLocaleString()} ${n === 1 ? one : many}`

function PriceEditor({ model, prices, onSave, onClose }: { model: UsageModel; prices: UsagePriceOverride[]; onSave: (prices: UsagePriceOverride[]) => void; onClose: () => void }) {
  const existing = prices.find(price => modelKey(price) === modelKey(model))
  const [fields, setFields] = useState(() => priceFields.map(([key]) => (existing ? existing[key] : model.priceReference?.[key])?.toString() ?? ''))
  const [error, setError] = useState('')
  const errorNote = usePresence(Boolean(error))
  const shownError = useRef(error)
  if (error) shownError.current = error
  const save = (remove = false) => {
    const next = prices.filter(price => modelKey(price) !== modelKey(model))
    if (!remove) {
      const values = fields.map(value => value.trim() === '' ? null : Number(value))
      if (values.some(value => value !== null && (!Number.isFinite(value) || value < 0))) { setError('Prices must be non-negative numbers. Leave unknown rates blank.'); return }
      if (next.length >= 200) { setError('Up to 200 model prices can be saved. Remove an unused override first.'); return }
      next.push({ providerId: model.providerId, modelId: model.modelId, speed: model.speed, inputPerMillion: values[0], cachedInputPerMillion: values[1], cacheWritePerMillion: values[2], outputPerMillion: values[3] })
    }
    try { window.localStorage.setItem(PRICE_KEY, JSON.stringify(next)); onSave(next); onClose(); reportAppNotice(remove ? 'Price override removed.' : 'Model prices saved.', 'success') } catch { reportAppFailure('Could not save prices on this device.', 'Your previous prices were kept.') }
  }
  return <form className="grid gap-3 py-4" aria-label={`Prices for ${model.modelId}`} onSubmit={event => { event.preventDefault(); save() }}>
    <p className="text-xs">USD per million tokens. Blank rates stay unknown. These estimates do not change provider billing.</p>
    {model.priceReference?.kind === 'reference' && <div className="grid gap-1 text-xs">
      <p>Reference: {model.priceReference.referenceModel} · <a href={model.priceReference.source} target="_blank" rel="noreferrer" className="underline">Rate source</a>{model.priceReference.verifiedDate && ` · Verified ${model.priceReference.verifiedDate}`}</p>
      <Reveal open={model.priceReference.cacheFallback}><p>Unpublished cache rates use the input rate.</p></Reveal>
      <Reveal open={model.priceReference.speedFallback}><p>This speed uses the standard rate because a separate rate is unavailable.</p></Reveal>
    </div>}
    <div className="grid gap-3 sm:grid-cols-4">{priceFields.map(([key, label], index) => <label className="grid gap-1 text-xs" key={key}>{label}
      <input type="number" min="0" step="any" className="uam-field w-full" value={fields[index]} onChange={event => setFields(previous => previous.map((value, i) => i === index ? event.target.value : value))} />
    </label>)}</div>
    <Reveal open={errorNote.mounted}><p role="alert" className="flex items-start gap-1.5 text-xs" style={{ color: 'var(--error)' }}><StatusDot tone="error" />{shownError.current}</p></Reveal>
    <div className="flex gap-2"><Button size="sm" variant="primary" type="submit">Save prices</Button><Button size="sm" type="button" onClick={onClose}>Cancel</Button><Reveal as="span" open={!!existing}><Button size="sm" type="button" onClick={() => save(true)}>Remove override</Button></Reveal></div>
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
  // Limits shows each provider's period tokens beside its allowance, so history stays live on every tab.
  const history = useUsageHistory(hostId, range, includedPrices)
  const headingRef = useRef<HTMLHeadingElement>(null)
  const providerHeadingId = useId()
  // "About these figures": coverage, notes and diagnostics live behind the header info control, not on the page.
  const [infoOpen, setInfoOpen] = useState(false)
  const infoRef = useRef<HTMLButtonElement>(null), infoPanelRef = useRef<HTMLDivElement>(null), infoId = useId()
  useEffect(() => { headingRef.current?.focus() }, [])
  useEffect(() => {
    if (!infoOpen) return
    const onPointerDown = (event: MouseEvent) => {
      const target = event.target as Node
      if (!infoPanelRef.current?.contains(target) && !infoRef.current?.contains(target)) setInfoOpen(false)
    }
    document.addEventListener('mousedown', onPointerDown)
    return () => document.removeEventListener('mousedown', onPointerDown)
  }, [infoOpen])
  useEffect(() => {
    if (hostId !== ALL_MACHINES_HOST_ID && hostId !== 'local' && !hosts.some(host => host.id === hostId)) {
      setHostId(ALL_MACHINES_HOST_ID); setSelectedModel(null); setEditingPrice(false)
    }
  }, [hosts, hostId])
  const data = history.data
  const totals = useMemo(() => sumProviderUsage((data?.providers ?? []).filter(provider => !hiddenProviders.has(provider.providerId))), [data, hiddenProviders])
  const models = useMemo(() => (data?.models ?? []).filter(model => !hiddenProviders.has(model.providerId)).sort((a, b) => metric === 'cost' ? (usageCost(b) ?? -1) - (usageCost(a) ?? -1) : b.totalTokens - a.totalTokens), [data, metric, hiddenProviders])
  const hourly = data ? data.endMs - data.startMs <= 86400000 : days === 1
  const bucketMs = hourly ? 3600000 : data?.chartBucketMs ?? 86400000
  const periods = useMemo(() => {
    if (!data) return []
    const width = bucketMs
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
  }, [data, hourly, bucketMs, hiddenProviders])
  const providerValue = (provider: UsageProvider) => { const state = providerState(provider, data?.collecting); return state === 'used' ? amount(provider) : EMPTY_CHIP[state] }
  // While history or reference prices are still loading, an unpriced amount is pending rather than unknown.
  const pricingPending = Boolean(data && (data.collecting || data.pricing?.refreshing))
  const costText = (total: UsageTotals) => pricingPending && usageCostBasis(total) === 'unpriced' ? 'Pricing…' : formatUsageCost(total)
  const amount = (total: UsageTotals) => metric === 'cost' ? costText(total) : formatUsageCount(total.totalTokens)
  const numeric = (total: UsageTotals) => metric === 'cost' ? usageCost(total) ?? 0 : total.totalTokens
  const peak = Math.max(0, ...periods.map(numeric))
  const maxModel = Math.max(0, ...models.map(numeric))
  const currentModel = data?.models.find(model => modelKey(model) === selectedModel)
  const refresh = () => { setRange(usageWindow(days)); history.refresh() }
  const shownAt = data?.asOfMs ? new Date(data.asOfMs).toLocaleString(undefined, { dateStyle: 'medium', timeStyle: 'short' }) : ''
  const visibleProviders = (data?.providers ?? []).filter(provider => !hiddenProviders.has(provider.providerId))
  // Fully read providers with nothing in the range fold into one line; rows that need a reason (reading, incomplete, unavailable) stay.
  const providerStates = visibleProviders.map(provider => ({ provider, state: providerState(provider, data?.collecting) }) as const)
  const providerRows = providerStates.filter(row => row.state !== 'idle'), idleProviders = providerStates.filter(row => row.state === 'idle').map(row => row.provider)
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
  const noCostData = metric === 'cost' && !pricingPending && totals.totalTokens > 0 && totals.reportedCostTokens + totals.estimatedCostTokens === 0
  const hasReported = totals.reportedCostTokens > 0 || totals.reportedCostUsd > 0
  const partialCost = totals.unpricedTokens > 0 && totals.reportedCostTokens + totals.estimatedCostTokens > 0
  const totalBasis = usageCostBasis(totals)
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
    data.historyCoverage?.previouslyPrunedReceiptsMayBeMissing && 'All time includes recorded and available history. Previously deleted records may be missing.',
    data.listingLimited && 'Some history is outside the scan limit. These totals are incomplete.',
    noCostData && 'No cost data for this range. Tokens were recorded, but no prices are known. Add model prices under Breakdown, or switch to Tokens.',
    metric === 'cost' && pricingPending && totals.unpricedTokens > 0 && `${totals.unpricedTokens.toLocaleString()} tokens are still awaiting prices.`,
    metric === 'cost' && !pricingPending && partialCost && `${totals.unpricedTokens.toLocaleString()} tokens have no known price and are excluded from costs.`,
    COPILOT_ENABLED && !hiddenProviders.has(COPILOT_CLI_PROVIDER_ID) && data.providers.some(provider => provider.providerId === COPILOT_CLI_PROVIDER_ID && provider.records > 0) && 'Copilot history is grouped by saved checkpoint time.',
    ...warnings.map(provider => `${providerLabel(provider.providerId)}: ${provider.errors} read errors, ${provider.invalidLines} invalid records, ${provider.oversizedLines} oversized records, ${provider.pendingLines} unfinished lines.`),
  ].filter((line): line is string => Boolean(line)) : []
  const attention = Boolean(data && (!data.complete || data.listingLimited || data.historyCoverage?.previouslyPrunedReceiptsMayBeMissing || warnings.length > 0 || noCostData || metric === 'cost' && !pricingPending && partialCost || data.coverage?.some(machine => machine.status !== 'ready')))
  const statusLines = [...notes, ...coverage]
  const errorNote = usePresence(Boolean(history.error))
  const readingPct = data && data.collecting && data.filesTotal > 0 ? Math.min(100, data.filesChecked / data.filesTotal * 100) : undefined
  const shownError = useRef(history.error)
  if (history.error) shownError.current = history.error
  const remoteHosts = hosts.some(host => host.id !== 'local')
  const lastView = useRef<'cost' | 'tokens'>('cost')
  if (metric !== 'limits') lastView.current = metric
  const view = lastView.current
  const forkCopies = data ? data.providers.reduce((sum, provider) => sum + provider.suppressedForkRecords, 0) : 0
  // Everything behind the header info control: status notes, machine coverage, the cost caveat and history diagnostics.
  const infoLines = [
    ...statusLines,
    data && metric === 'tokens' && 'Costs are estimates from API list prices unless marked Reported; they are not a bill. Cost has the details.',
    data && (data.duplicates > 0 || data.unidentifiedRecords > 0 || forkCopies > 0) && `History diagnostics: ${count(data.duplicates, 'duplicate record')}, ${count(data.unidentifiedRecords, 'unidentified record')} and ${count(forkCopies, 'forked copy', 'forked copies')} were skipped.`,
  ].filter((line): line is string => Boolean(line))
  const closeInfo = (event: KeyboardEvent<HTMLElement>) => {
    if (event.key !== 'Escape' || !infoOpen) return
    event.preventDefault(); event.stopPropagation(); setInfoOpen(false); infoRef.current?.focus()
  }

  // Tokens: nice axis ticks, the open (still filling) bucket, the busiest bucket and the average over complete buckets.
  const ticks = niceTicks(peak)
  const scaleMax = ticks[ticks.length - 1] || 1
  const openBucket = (startMs: number) => data !== null && startMs + bucketMs > data.endMs
  const closedPeriods = periods.filter(period => !openBucket(period.startMs))
  const averaged = closedPeriods.length ? closedPeriods : periods
  const average = averaged.reduce((sum, period) => sum + period.totalTokens, 0) / Math.max(1, averaged.length)
  const busiest = periods.reduce<(typeof periods)[number] | null>((best, period) => period.totalTokens > (best?.totalTokens ?? 0) ? period : best, null)
  const multiDay = !hourly && bucketMs > 86400000
  const labelEvery = Math.ceil(periods.length / 8)
  const utcPart = (startMs: number, options: Intl.DateTimeFormatOptions) => new Date(startMs).toLocaleString(undefined, { ...options, timeZone: 'UTC' })
  /** Column label: weekday and day, the month on the first column and where it changes, Today on the open day; long ranges label every few columns. */
  const xLabel = (startMs: number, index: number) => {
    if ((periods.length - 1 - index) % labelEvery) return ''
    if (hourly) return new Date(startMs).toLocaleString(undefined, { hour: 'numeric' })
    if (!multiDay && openBucket(startMs)) return 'Today'
    if (periods.length > 10) return tipLabel(startMs)
    const previous = periods[index - 1]
    const month = !previous || new Date(previous.startMs).getUTCMonth() !== new Date(startMs).getUTCMonth()
    return `${utcPart(startMs, { weekday: 'short' })} ${new Date(startMs).getUTCDate()}${month ? ` ${utcPart(startMs, { month: 'short' })}` : ''}`
  }
  const rangeTime = (ms: number) => new Date(ms).toLocaleString(undefined, { dateStyle: 'medium', timeStyle: 'short', timeZone: hourly ? undefined : 'UTC' })
  const rangeText = data ? `${rangeTime(data.chartStartMs ?? data.startMs)} to ${rangeTime(data.endMs)}${hourly ? '' : multiDay ? ` · ${bucketMs / 86400000}-day buckets use UTC` : ' · days in UTC'}` : ''
  const machineName = hostId === ALL_MACHINES_HOST_ID ? remoteHosts ? 'all machines' : 'this computer' : hostId === 'local' ? 'this computer' : hosts.find(host => host.id === hostId)?.label ?? hostId
  const machineIssue = data?.coverage?.find(machine => machine.status !== 'ready')
  // One short line at most; the info control holds the detail.
  const attentionSummary = !attention ? '' : machineIssue ? `Partial · ${machineIssue.label || machineIssue.executionHostId} ${machineIssue.status === 'cached' ? 'shows cached figures' : machineIssue.status === 'loading' ? 'is still loading' : 'is unavailable'}`
    : warnings.length ? 'Partial · some history could not be read' : !data?.complete || data.listingLimited ? 'Partial history' : 'Older records may be missing'
  const updatedAt = data?.asOfMs ? new Date(data.asOfMs) : null
  const updatedText = updatedAt ? `Updated ${updatedAt.toDateString() === new Date().toDateString() ? updatedAt.toLocaleTimeString(undefined, { hour: 'numeric', minute: '2-digit' }) : shownAt}` : ''
  const progressText = history.loading ? data && !history.kept && data.collecting && data.filesTotal > 0 ? `Reading history · ${data.filesChecked.toLocaleString()} of ${data.filesTotal.toLocaleString()} files` : 'Updating…' : ''
  const showFigures = totals.totalTokens > 0 || Boolean(data?.collecting)
  const selectModel = (model: UsageModel) => { setSelectedModel(previous => previous === modelKey(model) ? null : modelKey(model)); setEditingPrice(false) }
  const tipRow = (row: Band, dot = false) => <span key={row.key} className="uam-usage-tip-row"><i className={cx('uam-usage-swatch', dot && 'is-dot')} style={{ '--c': row.color } as CSSProperties} />{row.label}<span className="tabular-nums">{formatUsageCount(row.value)}</span></span>
  const offNote = (provider: UsageProvider, state: string) => state === 'reading' ? 'Figures appear as its history is read.'
    : state === 'unavailable' ? 'No figures. This is missing history, not zero use.'
    : `${([[provider.errors, 'read error'], [provider.invalidLines, 'invalid record'], [provider.oversizedLines, 'oversized record'], [provider.pendingLines, 'unfinished line']] as [number, string][]).filter(([value]) => value > 0).map(([value, one]) => count(value, one)).join(', ')}.`

  /** One model inside its provider card: id | token bar on the shared scale | tokens and share of the provider | chevron. */
  const tokenModelRow = (model: UsageModel, provider: UsageProvider) => {
    const open = currentModel === model
    return <div key={modelKey(model)} className="uam-tokens-model">
      <RowButton className="uam-tokens-model__row" aria-label={`Inspect ${providerLabel(model.providerId)} ${model.modelId} ${model.speed}`} aria-expanded={open} onClick={() => selectModel(model)}>
        <span className="uam-tokens-model__name"><Tooltip label={model.modelId}><span className="uam-tokens-model__id">{model.modelId}</span></Tooltip>{model.speed !== 'standard' && <span className="uam-tokens-tier">{model.speed}</span>}</span>
        <span className="uam-usage-model-bar"><TokenStack total={model} height={8} gapped width={maxModel ? model.totalTokens / maxModel : 0} /></span>
        <span className="uam-tokens-model__value tabular-nums">{formatUsageCount(model.totalTokens)}<small>{percentOf(model.totalTokens, provider.totalTokens)}</small></span>
        <ChevronRight size={14} aria-hidden className="uam-tokens-model__chev" />
      </RowButton>
      <Reveal open={open} className="uam-tokens-inspect"><TokenLegend total={model} />
        <div className="uam-tokens-inspect__foot"><span>{count(model.sessions, 'session')} · {count(model.records, 'usage record')} · {formatUsageCount(model.unpricedTokens)} unpriced tokens</span>
          <Button size="sm" leadingIcon={<SlidersHorizontal size={13} />} onClick={() => setEditingPrice(value => !value)}>Model prices</Button></div>
        <Reveal open={editingPrice}><PriceEditor model={model} prices={prices} onSave={setPrices} onClose={() => setEditingPrice(false)} /></Reveal>
      </Reveal>
    </div>
  }
  /** One card per provider. Used: share, tokens and cost with its basis, models, then a facts footer. Otherwise the greyed card says why. */
  const providerCard = ({ provider, state }: (typeof providerRows)[number]) => {
    const name = providerLabel(provider.providerId)
    const title = <h3 className="uam-limit-card__name"><ProviderLogo providerId={provider.providerId} size={18} className="uam-limit-card__logo inline-flex shrink-0" /><span className="truncate">{name}</span></h3>
    if (state !== 'used') return <Card key={provider.providerId} role="group" aria-label={name} elevation={0} data-state={state} className="uam-limit-card uam-limit-card--off uam-tokens-card">
      <header className="uam-limit-card__head">{title}<span className="uam-limit-card__meta"><span className="uam-limit-card__reason">{EMPTY_TEXT[state]}</span></span></header>
      <p className="uam-tokens-card__note">{offNote(provider, state)}</p>
    </Card>
    const rows = models.filter(model => model.providerId === provider.providerId)
    return <Card key={provider.providerId} role="group" aria-label={name} data-state={state} className="uam-limit-card uam-tokens-card">
      <header className="uam-limit-card__head">{title}<span className="uam-limits-chip">{percentOf(provider.totalTokens, totals.totalTokens)} of tokens</span>
        <span className="uam-limit-card__meta"><Tooltip label={provider.totalTokens.toLocaleString()}><span>{formatUsageCount(provider.totalTokens)} tokens{data?.collecting ? ' so far' : ''}</span></Tooltip>
          <span className="uam-tokens-cost"><span className="tabular-nums">{costText(provider)}</span><CostBasis totals={provider} focusable pending={pricingPending} /></span></span>
      </header>
      {rows.slice(0, modelLimit).map(model => tokenModelRow(model, provider))}
      <Reveal open={rows.length > modelLimit}><Button size="sm" className="my-2" onClick={() => setModelLimit(value => value + 100)}>Show more models ({rows.length - modelLimit} remaining)</Button></Reveal>
      <footer className="uam-limit-card__foot"><div className="uam-limit-card__facts">
        <span>Cache read <b>{percentOf(provider.cachedInputTokens, provider.totalTokens)}</b></span>
        <span>Output <b>{formatUsageCount(provider.outputTokens)}</b>{provider.reasoningTokens > 0 ? ` · ${percentOf(provider.reasoningTokens, provider.outputTokens)} reasoning` : ''}</span>
        {provider.cacheWriteTokens > 0 && <span>Cache write <b>{formatUsageCount(provider.cacheWriteTokens)}</b></span>}
        <span><b>{provider.sessions.toLocaleString()}</b> {provider.sessions === 1 ? 'session' : 'sessions'}</span>
        {usageCostBasis(provider) === 'partial' && <span className={cx('uam-usage-reason uam-tokens-card__aside', !pricingPending && 'is-warn')}>{formatUsageCount(provider.unpricedTokens)} tokens {pricingPending ? 'are still awaiting prices' : 'have no known price and are excluded from its cost'}</span>}
        {COPILOT_ENABLED && provider.providerId === COPILOT_CLI_PROVIDER_ID && <span className="uam-usage-reason uam-tokens-card__aside">Grouped by saved checkpoint time</span>}
      </div></footer>
    </Card>
  }
  const usedCards = providerRows.filter(row => row.state === 'used'), offCards = providerRows.filter(row => row.state !== 'used')
  const historyRow = <div className={cx('uam-limits-group uam-tokens-group', history.loading && 'uam-progress-fill')} aria-busy={history.loading || undefined}
    data-indeterminate={readingPct === undefined && history.loading ? '' : undefined} style={history.loading && readingPct !== undefined ? { '--progress': readingPct / 100 } as CSSProperties : undefined}>
    <h2 className="uam-usage-label">History · {machineName}</h2>
    {attentionSummary && <span className="uam-usage-reason is-warn">{attentionSummary}</span>}
    {progressText ? <span aria-hidden className="uam-limits-updated uam-tokens-group__end tabular-nums">{progressText}</span>
      : updatedText && <Tooltip label={shownAt}><span className="uam-limits-updated uam-tokens-group__end">{updatedText}</span></Tooltip>}
    <Button size="sm" variant="secondary" leadingIcon={<RefreshCw size={13} aria-hidden />} className={cx('uam-tokens-group__end uam-usage-refresh', history.loading && 'is-busy')} aria-busy={history.loading} onClick={refresh}>Refresh usage</Button>
  </div>
  const tokensHead = <Card className="uam-tokens-head">
    <div className="uam-tokens-stats">
      <StatTile label="Total tokens" value={formatUsageCount(totals.totalTokens)} title={totals.totalTokens.toLocaleString()} sub={`${count(totals.sessions, 'session')} ${PERIOD_PHRASE[days]}${data?.collecting ? ' · so far' : ''}`} />
      <StatTile label="Cache read" value={percentOf(totals.cachedInputTokens, totals.totalTokens)} detail={formatUsageCount(totals.cachedInputTokens)} sub="of all tokens" />
      <StatTile label="Reasoning" value={percentOf(totals.reasoningTokens, totals.totalTokens)} detail={formatUsageCount(totals.reasoningTokens)} sub={`part of ${formatUsageCount(totals.outputTokens)} output`} />
      {busiest && <StatTile label={hourly ? 'Busiest hour' : multiDay ? 'Busiest period' : 'Busiest day'} value={formatUsageCount(busiest.totalTokens)} detail={tipLabel(busiest.startMs)} sub={`${formatUsageCount(average)} ${hourly ? 'an hour' : multiDay ? `per ${bucketMs / 86400000} days` : 'a day'} on average`} />}
    </div>
    <div className="uam-tokens-mix"><TokenStack total={totals} height={8} gapped describe /><TokenLegend total={totals} /></div>
  </Card>
  // The table is the chart's exact equivalent, newest first; bars share the chart's peak.
  const dailyTable = <table className="uam-tokens-days text-xs">
    <thead className="sr-only"><tr><th scope="col">{hourly ? 'Hour' : 'Day'}</th><th scope="col">Token mix</th><th scope="col">Tokens</th></tr></thead>
    <tbody>{[...periods].reverse().map(period => <tr key={period.startMs}>
      <th scope="row">{tipLabel(period.startMs)}{openBucket(period.startMs) ? ' · so far' : ''}</th>
      <td><TokenStack total={period} height={8} gapped width={peak ? period.totalTokens / peak : 0} describe /></td>
      <td className="tabular-nums">{formatUsageCount(period.totalTokens)}</td>
    </tr>)}</tbody>
  </table>
  const tokensChart = <Card className="uam-tokens-chart">
    <div className="uam-tokens-chart__head"><h2>{hourly ? 'Hourly' : 'Daily'} tokens</h2><span className="uam-usage-muted">by token type</span>
      <ul className="uam-usage-key text-xs" aria-hidden>{TOKEN_SEGMENTS.map(segment => <li key={segment.key} className="uam-usage-key-item"><i className="uam-usage-swatch" style={{ '--c': segment.color } as CSSProperties} />{segment.label}</li>)}</ul></div>
    <div className="uam-usage-chart">
      <div className="uam-usage-axis text-xs tabular-nums" aria-hidden>{[...ticks].reverse().map(tick => <span key={tick}>{formatUsageCount(tick)}</span>)}</div>
      <div key={plotKey} role="group" aria-label={`${hourly ? 'Hourly' : 'Daily'} tokens by type from loaded history. Use arrow keys to move between columns; the ${hourly ? 'Hourly' : 'Daily'} table below lists the same figures.`}
        className="uam-usage-plot uam-tokens-plot" style={{ gap: periods.length > 31 ? 1 : 4, '--n': Math.max(1, ticks.length - 1) } as CSSProperties} onKeyDown={moveColumnFocus}>
        {periods.map((period, index) => {
          const pct = period.totalTokens / scaleMax * 100
          const edge = index < periods.length * 0.2 ? 'is-start' : index >= periods.length * 0.8 ? 'is-end' : ''
          const types = tokenBands(period).filter(band => band.value > 0), byProvider = providerBands(period).filter(band => band.value > 0)
          const open = openBucket(period.startMs)
          return <div key={period.startMs} role="img" tabIndex={index === periods.length - 1 ? 0 : -1} aria-label={`${columnSummary(period)}${open ? '. So far' : ''}`} className={cx('uam-usage-col', edge, pct > 55 && 'is-high')} style={{ '--i': Math.min(index, 40), '--h': `${pct}%` } as CSSProperties}>
            <div className="uam-usage-col-stack" style={{ height: `${pct}%`, minHeight: period.totalTokens > 0 ? 2 : 0 }}>{types.map(band => <div key={band.key} className="uam-usage-band" data-seg={band.key} style={{ height: `${band.value / period.totalTokens * 100}%`, '--c': band.color } as CSSProperties} />)}</div>
            {/* ponytail: totals sit above columns only while each label fits (two weeks); longer ranges keep them in the tooltip and table. */}
            {periods.length <= 14 && period.totalTokens > 0 && <span className="uam-usage-col-total tabular-nums" aria-hidden>{formatUsageCount(period.totalTokens)}</span>}
            <span className="uam-usage-col-tip uam-usage-col-tip--split" aria-hidden>
              <span className="uam-usage-tip-head"><span>{tipLabel(period.startMs)}{open ? ' · so far' : ''}</span><b>{formatUsageCount(period.totalTokens)} tokens</b></span>
              {types.length ? <span className="uam-usage-tip-cols"><span>{types.map(row => tipRow(row))}</span><span>{byProvider.map(row => tipRow(row, true))}</span></span> : <span className="uam-usage-muted">No usage</span>}
            </span>
          </div>
        })}
      </div>
    </div>
    <div className="uam-tokens-xaxis" aria-hidden style={{ gap: periods.length > 31 ? 1 : 4 }}>{periods.map((period, index) => {
      const label = xLabel(period.startMs, index), open = openBucket(period.startMs)
      return <span key={period.startMs} className={cx(open && 'is-open')}>{label}{label && open ? <small>so far</small> : null}</span>
    })}</div>
    <footer className="uam-limit-card__foot"><div className="uam-limit-card__facts"><span>{rangeText}</span></div>
      <Disclosure className="uam-tokens-table" summary={`${hourly ? 'Hourly' : 'Daily'} table`}>{dailyTable}</Disclosure></footer>
  </Card>
  const chips = <div role="group" aria-label="Providers" className="uam-usage-chips">{(data?.providers ?? []).map(provider => {
    const off = hiddenProviders.has(provider.providerId)
    // ui: raw button kept — usage provider chip with colour dot, logo and value tooltip; Chip would apply the choice-button look.
    return <button key={provider.providerId} type="button" aria-label={`Include ${providerLabel(provider.providerId)} usage`} aria-pressed={!off} className={cx('uam-usage-chip', off && 'is-off')} style={{ '--c': providerColor[provider.providerId] } as CSSProperties} onClick={() => { setHiddenProviders(previous => { const next = new Set(previous); if (next.has(provider.providerId)) next.delete(provider.providerId); else next.add(provider.providerId); return next }); setSelectedModel(null); setEditingPrice(false) }}>
      <span className="uam-usage-chip-dot" aria-hidden />
      <ProviderLogo providerId={provider.providerId} size={14} />
      <span className="uam-usage-chip-name">{providerLabel(provider.providerId)}</span>
      {off ? <X size={12} aria-hidden /> : <Check size={12} aria-hidden />}
      <Tooltip label={provider.available ? `${provider.files} history files` : 'Usage history unavailable'}><span className="uam-usage-chip-value tabular-nums">{providerValue(provider)}</span></Tooltip>
    </button>
  })}</div>
  return <main aria-label="Usage workspace" className="uam-usage flex h-full min-h-0 min-w-0 flex-1 flex-col" style={{ background: 'var(--bg)', color: 'var(--text)' }} onKeyDown={event => {
    if (event.key === 'Escape' && !event.defaultPrevented && !document.querySelector('[aria-modal="true"]') && !editingPrice) { event.preventDefault(); onClose() }
  }}>
    <header className="flex shrink-0 items-center gap-3 overflow-x-auto border-b px-4" style={{ height: 'var(--pane-header-h)', borderColor: 'var(--border)' }}>
      <IconButton icon={<ArrowLeft size={16} />} label="Back from usage" onClick={onClose} />
      <h1 ref={headingRef} tabIndex={-1} className="text-sm font-semibold">Usage</h1>
      {/* ui: raw button kept — opens the About these figures panel with the bespoke status icon and attention look; IconButton would restyle it. */}
      <Reveal as="span" open={infoLines.length > 0}><button ref={infoRef} type="button" aria-label="Usage status" aria-haspopup="dialog" aria-expanded={infoOpen} aria-controls={infoOpen ? infoId : undefined}
        className={cx('uam-usage-status', attention && 'is-attention', infoOpen && 'is-open')} onClick={() => setInfoOpen(open => !open)} onKeyDown={closeInfo}><Info size={14} aria-hidden /></button></Reveal>
      <ViewportMenu ref={infoPanelRef} open={infoOpen && infoLines.length > 0} anchorRef={infoRef} id={infoId} role="dialog" aria-label="About these figures" className="uam-usage-info" onKeyDown={closeInfo}>
        <h2 className="uam-usage-label">About these figures</h2>
        <ul>{infoLines.map((line, index) => <li key={index} className="uam-usage-reason">{line}</li>)}</ul>
      </ViewportMenu>
      {/* Only remote hosts make the machine choice meaningful; with this computer alone, "All machines" and "This computer" are the same data. */}
      {remoteHosts && <div className="max-w-48"><MenuSelect label="Usage host" value={hostId} options={[{ value: ALL_MACHINES_HOST_ID, label: 'All machines' }, ...(!hosts.some(host => host.id === 'local') ? [{ value: 'local', label: 'This computer' }] : []), ...hosts.map(host => ({ value: host.id, label: host.label }))]} onChange={value => { setHostId(value); setSelectedModel(null); setEditingPrice(false) }} /></div>}
      <SegmentedControl aria-label="Usage metric" value={metric} options={(['cost', 'tokens', 'limits'] as const).map(value => ({ value, label: value[0].toUpperCase() + value.slice(1) }))} onChange={setMetric} />
      <SegmentedControl aria-label="Usage period" value={days} options={USAGE_WINDOWS.map(option => ({ value: option.days, label: option.label }))} onChange={value => { setDays(value); setRange(usageWindow(value)); setSelectedModel(null); setEditingPrice(false) }} />
      <Reveal as="span" open={metric === 'cost'}><IconButton icon={<RefreshCw size={15} />} label="Refresh usage" aria-busy={history.loading} className={cx('uam-usage-refresh', history.loading && 'is-busy')} onClick={refresh} /></Reveal>
      {/* Background refresh never hides or shifts the page: cached figures stay in place and this line reports progress (on Tokens, in its History row). */}
      <Reveal as="span" open={metric === 'cost'}><span aria-hidden className="uam-usage-progress text-xs uam-usage-muted tabular-nums">{history.loading ? data && !history.kept && data.collecting && data.filesTotal > 0 ? `Reading history · ${data.filesChecked.toLocaleString()} of ${data.filesTotal.toLocaleString()} files` : `Updating…${shownAt && !history.kept ? ` Showing ${shownAt}` : ''}` : ''}</span></Reveal>
      {metric !== 'limits' && <span role="status" className="sr-only">{history.loading ? 'Updating usage' : history.error ? '' : data ? 'Usage updated' : ''}</span>}
    </header>
    <div className="min-h-0 flex-1 overflow-y-auto p-4 md:p-6">
      <Reveal open={metric === 'limits'}><UsageLimits executionHostId={hostId} usage={{ providers: history.data?.providers ?? null, loading: history.loading, collecting: Boolean(history.data?.collecting), error: history.error, days }} /></Reveal>
      <Reveal open={metric !== 'limits'}><div className="mx-auto grid max-w-6xl gap-6" aria-busy={history.loading && !data}>
        {errorNote.mounted && <div className="uam-presence" data-state={errorNote.closing ? 'closed' : 'open'}><Notice tone="error" title={shownError.current} dismissLabel="Dismiss usage error" dismissible={false} actions={<Button size="sm" onClick={refresh}>Retry</Button>}>{shownAt && !history.kept ? `Showing figures from ${shownAt}.` : null}</Notice></div>}
        {!data && history.loading && !history.error && <div className="uam-usage-skeleton" aria-hidden><span className="uam-usage-skel-tile" /><span className="uam-usage-skel-tile" /><span className="uam-usage-skel-tile" /><span className="uam-usage-skel-chart" /></div>}
        <Reveal open={!!data}>{data ? <div className="uam-usage-body grid gap-6" data-kept={history.kept || undefined}>
          <ul className="sr-only" aria-label="Usage status details">{notes.map((line, index) => <li key={index}>{line}</li>)}</ul>
          {coverage.length > 0 && <ul aria-label="Usage machine coverage" className="sr-only">{coverage.map((line, index) => <li key={index}>{line}</li>)}</ul>}
          {/* Cost and Tokens swap in one cell; while Limits shows, the last of them stays put for its exit. */}
          <Swap swapKey={view}>{view === 'cost' ? <div className="grid gap-6">
            {chips}
            <Reveal open={totals.totalTokens === 0 && !history.loading}><p className="uam-usage-empty">No usage recorded in this range.</p></Reveal>
            <section aria-busy={history.loading || undefined} data-indeterminate={readingPct === undefined && history.loading ? '' : undefined} className={cx('grid gap-6 lg:grid-cols-[18rem_minmax(0,1fr)]', history.loading && 'uam-progress-fill')} style={history.loading && readingPct !== undefined ? { '--progress': readingPct / 100 } as CSSProperties : undefined}>
              <div className="grid content-start gap-4">
                <div className="uam-usage-tiles">
                  <StatTile label={partialCost ? 'Known cost' : totalBasis === 'reported' ? 'Reported cost' : totalBasis === 'mixed' ? 'Reported + estimated' : totalBasis === 'unpriced' ? 'Cost' : 'Estimated cost'} value={amount(totals)} title={totals.totalTokens.toLocaleString()} sub={`${totalBasis === 'reported' ? 'Provider charges' : totalBasis === 'unpriced' ? pricingPending ? 'Prices still loading' : 'No known price' : hasReported ? 'Provider charges and API list-price estimates' : 'API list-price estimate, not a bill'} · ${totals.sessions.toLocaleString()} sessions${data.collecting ? ' · so far' : ''}`} />
                  <Reveal open={hasReported}><StatTile label="Provider-reported" value={money(totals.reportedCostUsd)} /></Reveal>
                  <Reveal open={hasReported}><StatTile label="Estimated" value={totals.estimatedCostTokens || !totals.totalTokens ? money(totals.estimatedCostUsd) : 'Unknown'} sub="API list prices" /></Reveal>
                  <Reveal open={(!hasReported || totals.unpricedTokens > 0) && !(pricingPending && totalBasis === 'unpriced')}><StatTile label="Unpriced tokens" value={formatUsageCount(totals.unpricedTokens)} sub={pricingPending && totals.unpricedTokens > 0 ? 'Awaiting prices' : 'No known price'} /></Reveal>
                </div>
              </div>
              <div className="grid content-start gap-3">
                <h2 className="text-sm font-medium">{hourly ? 'Hourly' : 'Daily'} cost</h2>
                <div className="uam-usage-chart">
                  <div className="uam-usage-axis text-xs tabular-nums" aria-hidden><span>{axisValue(peak)}</span><span>{axisValue(peak / 2)}</span><span>0</span></div>
                  <div key={plotKey} role="group" aria-label={`${hourly ? 'Hourly' : 'Daily'} costs, unpriced tokens excluded, from loaded history. Use arrow keys to move between columns.`} className="uam-usage-plot" style={{ gap: periods.length > 31 ? 1 : 4 }} onKeyDown={moveColumnFocus}>
                    <Reveal open={peak === 0 && totals.unpricedTokens > 0}><span className="absolute inset-0 flex items-center justify-center text-xs">{pricingPending ? 'Prices still loading.' : 'No priced usage in this range.'}</span></Reveal>
                    {periods.map((period, index) => {
                      const total = numeric(period)
                      const pct = peak ? total / peak * 100 : 0
                      const edge = index < periods.length * 0.2 ? 'is-start' : index >= periods.length * 0.8 ? 'is-end' : ''
                      const bands = providerBands(period).filter(band => band.value > 0)
                      return <div key={period.startMs} role="img" tabIndex={index === periods.length - 1 ? 0 : -1} aria-label={columnSummary(period)} className={cx('uam-usage-col', edge, pct > 55 && 'is-high')} style={{ '--i': Math.min(index, 40), '--h': `${pct}%` } as CSSProperties}>
                        <div className="uam-usage-col-stack" style={{ height: `${pct}%`, minHeight: total > 0 ? 2 : 0 }}>{bands.map(band => <div key={band.key} className="uam-usage-band" data-seg={band.key} style={{ height: `${total ? band.value / total * 100 : 0}%`, '--c': band.color } as CSSProperties} />)}</div>
                        <span className="uam-usage-col-tip" aria-hidden>
                          <span className="uam-usage-tip-head">{tipLabel(period.startMs)} · <b>{amount(period)}</b></span>
                          {bands.map(row => <span key={row.key} className="uam-usage-tip-row"><i className="uam-usage-swatch" style={{ '--c': row.color } as CSSProperties} />{row.label}<span className="tabular-nums uam-usage-muted">{row.text}</span></span>)}
                          <Reveal open={bands.length === 0}><span className="uam-usage-muted">No usage</span></Reveal>
                        </span>
                      </div>
                    })}
                  </div>
                </div>
                <p className="text-xs uam-usage-muted">{new Date(data.chartStartMs ?? data.startMs).toLocaleString()} to {new Date(data.endMs).toLocaleString()}{!hourly ? ` · ${data.chartBucketMs && data.chartBucketMs > 86400000 ? `${data.chartBucketMs / 86400000}-day` : 'Daily'} buckets use UTC` : ''}</p>
              </div>
            </section>
            <section className="grid gap-3"><div className="flex flex-wrap items-baseline justify-between gap-3"><h2 className="text-sm font-medium">Totals</h2><Reveal open={metric === 'cost'}><p className="text-xs uam-usage-muted">Estimated cache savings: {totals.estimatedCostTokens || !totals.totalTokens ? money(totals.estimatedCacheSavingsUsd) : pricingPending ? 'Pricing…' : 'Unknown'}</p></Reveal></div>
              <TokenStack total={totals} height={16} labels />
              <TokenLegend total={totals} />
              <div className="grid gap-3 pt-2">
                <Reveal open={providerRows.length > 0}><table className="uam-usage-providers text-xs" aria-label="By provider">
                  <colgroup><col className="uam-usage-col-name" /><col /><col className="uam-usage-col-tokens" /><col className="uam-usage-col-cost" /></colgroup>
                  <thead className="sr-only"><tr><th scope="col">Provider</th><th scope="col">Token mix</th><th scope="col">Tokens</th><th scope="col">Cost</th></tr></thead>
                  <tbody>{providerRows.map(({ provider, state }) => <tr key={provider.providerId} data-state={state}>
                    <th scope="row"><span className="flex min-w-0 items-center gap-2 font-normal"><i className="uam-usage-swatch" style={{ '--c': providerColor[provider.providerId] } as CSSProperties} aria-hidden /><span className="truncate">{providerLabel(provider.providerId)}</span></span></th>
                    {state === 'used' ? <>
                      <td><TokenStack total={provider} height={14} labels /><span className="sr-only">{TOKEN_SEGMENTS.filter(segment => segment.key !== 'reasoning' && segment.value(provider) > 0).map(segment => `${segment.label} ${percentOf(segment.value(provider), provider.totalTokens)}`).join(', ')}</span></td>
                      <td className="text-right tabular-nums"><Tooltip label={provider.totalTokens.toLocaleString()}><span>{formatUsageCount(provider.totalTokens)}</span></Tooltip></td>
                      <td><span className="uam-usage-cost-cell"><span className="tabular-nums">{costText(provider)}</span><CostBasis totals={provider} focusable pending={pricingPending} /></span></td>
                    </> : <td colSpan={3} className="uam-usage-muted">{EMPTY_TEXT[state]}</td>}
                  </tr>)}</tbody>
                </table></Reveal>
                <Reveal open={idleProviders.length > 0}><p className="text-xs uam-usage-muted">{EMPTY_TEXT.idle}: {idleProviders.map(provider => providerLabel(provider.providerId)).join(', ')}</p></Reveal>
                <Reveal open={visibleProviders.length === 0}><p className="text-xs uam-usage-muted">All providers are filtered out.</p></Reveal>
              </div>
            </section>
            <section className="grid gap-3"><div className="flex items-center justify-between gap-3"><h2 className="text-sm font-medium">Breakdown</h2><SegmentedControl aria-label="Usage breakdown" value={breakdown} options={[{ value: 'model', label: 'Models' }, { value: 'time', label: 'Time' }]} onChange={setBreakdown} /></div>
              <Reveal open={breakdown === 'model'}><div><Reveal open={models.length === 0}><p className="py-4 text-xs uam-usage-muted">No model usage in this range.</p></Reveal>{models.slice(0, modelLimit).map((model, index) => <div key={modelKey(model)} className="border-b" style={{ borderColor: 'var(--border)' }}>
                <RowButton className="uam-usage-model-row text-xs" aria-label={`Inspect ${providerLabel(model.providerId)} ${model.modelId} ${model.speed}`} aria-describedby={`${providerHeadingId}-cost-${index}`} aria-expanded={modelKey(model) === selectedModel} onClick={() => { setSelectedModel(previous => previous === modelKey(model) ? null : modelKey(model)); setEditingPrice(false) }}>
                  <span className="flex min-w-0 items-center gap-2"><ProviderLogo providerId={model.providerId} size={16} /><Tooltip label={model.modelId}><span className="truncate">{model.modelId}</span></Tooltip>{model.speed !== 'standard' && <span className="uam-usage-muted">{model.speed}</span>}</span><span className="uam-usage-model-bar">{metric === 'cost' ? <CostBar cost={usageCost(model) ?? 0} max={maxModel} color={providerColor[model.providerId]} /> : <TokenStack total={model} height={14} />}</span><span id={`${providerHeadingId}-cost-${index}`} className={cx('shrink-0', metric === 'cost' && 'uam-usage-cost-cell')}><span className="tabular-nums">{amount(model)}</span>{metric === 'cost' && <CostBasis totals={model} pending={pricingPending} />}</span>
                </RowButton>
                <Reveal open={currentModel === model} className="grid gap-3 pb-4"><TokenLegend total={model} /><p className="text-xs uam-usage-muted">{model.sessions} sessions · {model.records} usage records · {formatUsageCount(model.unpricedTokens)} unpriced tokens</p><Button size="sm" leadingIcon={<SlidersHorizontal size={13} />} onClick={() => setEditingPrice(value => !value)}>Model prices</Button><Reveal open={editingPrice}><PriceEditor model={model} prices={prices} onSave={setPrices} onClose={() => setEditingPrice(false)} /></Reveal></Reveal>
              </div>)}<Reveal open={models.length > modelLimit}><Button size="sm" className="mt-3" onClick={() => setModelLimit(value => value + 100)}>Show more models ({models.length - modelLimit} remaining)</Button></Reveal></div></Reveal><Reveal open={breakdown !== 'model'}><div>{[...periods].reverse().map(period => <div key={period.startMs} className="grid grid-cols-[minmax(0,1fr)_minmax(4rem,12rem)_auto] items-center gap-x-4 border-b py-3 text-xs" style={{ borderColor: 'var(--border)' }}><span>{new Date(period.startMs).toLocaleString(undefined, hourly ? { hour: 'numeric', month: 'short', day: 'numeric' } : { year: 'numeric', month: 'short', day: 'numeric', timeZone: 'UTC' })}{!hourly ? ' UTC' : ''}</span><span className="uam-usage-model-bar"><CostBar cost={numeric(period)} max={peak} color="var(--text-3)" /></span><span className="tabular-nums">{amount(period)}</span></div>)}</div></Reveal>
            </section>
          </div> : <div className="uam-tokens grid">
            {historyRow}
            {chips}
            <Reveal open={!showFigures && !history.loading && visibleProviders.length > 0}><p className="uam-usage-empty">No usage recorded in this range.</p></Reveal>
            <Reveal open={visibleProviders.length === 0}><p className="uam-usage-empty">All providers are filtered out.</p></Reveal>
            <Reveal open={showFigures}><div className="uam-tokens grid">{tokensHead}{tokensChart}</div></Reveal>
            <Reveal open={providerRows.length > 0}><div className="uam-tokens grid">
              <div className="uam-limits-group"><h2 className="uam-usage-label">By provider · models</h2><span className="uam-limits-updated">Bars share one scale across providers</span></div>
              <div className="uam-limits-cards">{usedCards.map(providerCard)}{offCards.length > 1 ? <div className="uam-limits-off">{offCards.map(providerCard)}</div> : offCards.map(providerCard)}</div>
            </div></Reveal>
            <Reveal open={idleProviders.length > 0}><p className="uam-tokens-idle">{EMPTY_TEXT.idle}: {idleProviders.map(provider => providerLabel(provider.providerId)).join(', ')}</p></Reveal>
          </div>}</Swap>
        </div> : null}</Reveal>
      </div></Reveal>
    </div>
  </main>
}
