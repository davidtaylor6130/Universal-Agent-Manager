import { ANTIGRAVITY_CLI_PROVIDER_ID, CLAUDE_CLI_PROVIDER_ID, CODEX_CLI_PROVIDER_ID, COPILOT_CLI_PROVIDER_ID, DEFAULT_PROVIDER_ID, OPENCODE_CLI_PROVIDER_ID } from '../utils/providerMetadata'

export const USAGE_PROVIDERS = [CODEX_CLI_PROVIDER_ID, CLAUDE_CLI_PROVIDER_ID, DEFAULT_PROVIDER_ID, OPENCODE_CLI_PROVIDER_ID, COPILOT_CLI_PROVIDER_ID, ANTIGRAVITY_CLI_PROVIDER_ID] as const
export const ALL_MACHINES_HOST_ID = '*'
export type UsageProviderId = typeof USAGE_PROVIDERS[number]
export type UsageSpeed = 'standard' | 'fast' | 'ultrafast'
export type UsageMetric = 'cost' | 'tokens' | 'limits'
export type UsageWindowDays = 1 | 7 | 30 | 90

export interface UsageTotals {
  inputTokens: number
  cachedInputTokens: number
  cacheWriteTokens: number
  outputTokens: number
  reasoningTokens: number
  totalTokens: number
  reportedCostUsd: number
  estimatedCostUsd: number
  estimatedCacheSavingsUsd: number
  reportedCostTokens: number
  estimatedCostTokens: number
  unpricedTokens: number
  records: number
  sessions: number
}
export interface UsageProvider extends UsageTotals {
  providerId: UsageProviderId
  available: boolean
  files: number
  errors: number
  invalidLines: number
  oversizedLines: number
  suppressedForkRecords: number
  pendingLines: number
}
export interface UsageModel extends UsageTotals {
  providerId: UsageProviderId
  modelId: string
  speed: UsageSpeed
  priceReference?: UsagePriceReference
}
export interface UsagePeriod extends UsageTotals { startMs: number }
export interface UsageProviderPeriod extends UsagePeriod { providerId: UsageProviderId }
export interface UsageCoverage {
  executionHostId: string
  label: string
  status: 'ready' | 'loading' | 'cached' | 'unavailable'
  asOfMs: number | null
  error: string
}
export interface UsageHistory {
  coverage?: UsageCoverage[]
  version: 2 | 3
  asOfMs?: number
  windowDays?: UsageWindowDays
  collecting?: boolean
  collectionError?: string
  pricing?: UsagePricing
  executionHostId: string
  source: 'native-transcripts'
  startMs: number
  endMs: number
  totals: UsageTotals
  complete: boolean
  filesChecked: number
  filesTotal: number
  bytesRead: number
  duplicates: number
  unidentifiedRecords: number
  listingLimited: boolean
  providers: UsageProvider[]
  models: UsageModel[]
  days: UsagePeriod[]
  hours: UsagePeriod[]
  providerDays: UsageProviderPeriod[]
  providerHours: UsageProviderPeriod[]
}
export interface UsagePriceOverride {
  providerId: UsageProviderId
  modelId: string
  speed: UsageSpeed
  inputPerMillion: number | null
  cachedInputPerMillion: number | null
  cacheWritePerMillion: number | null
  outputPerMillion: number | null
}
export interface UsagePriceReference extends Omit<UsagePriceOverride, 'providerId' | 'modelId' | 'speed'> {
  kind: 'manual' | 'reference'
  referenceModel: string
  source: string
  verifiedDate: string
  cacheFallback: boolean
  speedFallback: boolean
}
export interface UsagePricing {
  refreshing: boolean
  source: string
  status: 'unavailable' | 'cached' | 'fresh'
  fetchedAtMs: number | null
  error: string
  aliasError: string
}
export interface UsageWindow { startMs: number; endMs: number; days?: UsageWindowDays }
export const USAGE_WINDOWS: { days: UsageWindowDays; label: string }[] = [
  { days: 1, label: 'Past 24h' }, { days: 7, label: '7 days' }, { days: 30, label: '30 days' }, { days: 90, label: '90 days' },
]
export function usageWindow(days: UsageWindowDays, now = Date.now()): UsageWindow {
  return { startMs: Math.max(0, days === 1 ? now - 86400000 : Math.floor(now / 86400000) * 86400000 - (days - 1) * 86400000), endMs: now, days }
}

const integer = (value: unknown): value is number => typeof value === 'number' && Number.isSafeInteger(value) && value >= 0
const money = (value: unknown): value is number => typeof value === 'number' && Number.isFinite(value) && value >= 0
const object = (value: unknown): value is Record<string, unknown> => typeof value === 'object' && value !== null && !Array.isArray(value)
const provider = (value: unknown): value is UsageProviderId => USAGE_PROVIDERS.includes(value as UsageProviderId)
const speed = (value: unknown): value is UsageSpeed => value === 'standard' || value === 'fast' || value === 'ultrafast'
const countFields = ['inputTokens', 'cachedInputTokens', 'cacheWriteTokens', 'outputTokens', 'reasoningTokens', 'totalTokens', 'reportedCostTokens', 'estimatedCostTokens', 'unpricedTokens', 'records', 'sessions'] as const

const timestamp = (value: unknown): value is number => integer(value) && value > 0 && value <= 8640000000000000
function httpsSource(value: unknown): value is string {
  if (typeof value !== 'string' || value.length > 2048) return false
  try { const url = new URL(value); return url.protocol === 'https:' && !url.username && !url.password } catch { return false }
}
function priceReference(value: unknown): value is UsagePriceReference {
  if (!object(value) || !['manual', 'reference'].includes(value.kind as string)
    || typeof value.referenceModel !== 'string' || !value.referenceModel || value.referenceModel.length > 512
    || (value.kind === 'manual' ? value.source !== '' : !httpsSource(value.source))
    || typeof value.verifiedDate !== 'string' || (value.verifiedDate !== '' && !/^\d{4}-\d{2}-\d{2}$/.test(value.verifiedDate))
    || typeof value.cacheFallback !== 'boolean' || typeof value.speedFallback !== 'boolean') return false
  return ['inputPerMillion', 'cachedInputPerMillion', 'cacheWritePerMillion', 'outputPerMillion'].every(key => value[key] === null || money(value[key]))
}
function pricing(value: unknown): value is UsagePricing {
  return object(value) && typeof value.refreshing === 'boolean' && httpsSource(value.source)
    && ['unavailable', 'cached', 'fresh'].includes(value.status as string)
    && (value.fetchedAtMs === null || timestamp(value.fetchedAtMs))
    && ['error', 'aliasError'].every(key => typeof value[key] === 'string' && (value[key] as string).length <= 1024)
}

function coverage(value: unknown, hostId: string): value is UsageCoverage[] {
  if (!Array.isArray(value) || !value.length || value.length > 33 || (hostId !== ALL_MACHINES_HOST_ID && value.length !== 1)) return false
  const seen = new Set<string>()
  return value.every(row => {
    if (!object(row) || typeof row.executionHostId !== 'string' || !row.executionHostId || row.executionHostId.length > 160 || row.executionHostId === ALL_MACHINES_HOST_ID || seen.has(row.executionHostId)
      || (hostId !== ALL_MACHINES_HOST_ID && row.executionHostId !== hostId) || typeof row.label !== 'string' || row.label.length > 128
      || !['ready', 'loading', 'cached', 'unavailable'].includes(row.status as string) || !(row.asOfMs === null || timestamp(row.asOfMs))
      || ((row.status === 'ready' || row.status === 'cached') && row.asOfMs === null) || typeof row.error !== 'string' || row.error.length > 1024) return false
    seen.add(row.executionHostId)
    return true
  })
}

function totals(value: unknown): value is UsageTotals {
  if (!object(value) || !countFields.every(key => integer(value[key])) || !['reportedCostUsd', 'estimatedCostUsd', 'estimatedCacheSavingsUsd'].every(key => money(value[key]))) return false
  const row = value as unknown as UsageTotals
  return row.inputTokens + row.cachedInputTokens + row.cacheWriteTokens + row.outputTokens === row.totalTokens
    && row.reportedCostTokens + row.estimatedCostTokens + row.unpricedTokens === row.totalTokens
    && row.reasoningTokens <= row.outputTokens && row.sessions <= row.records && Number.isFinite(row.reportedCostUsd + row.estimatedCostUsd)
}
function periods(value: unknown, window: UsageWindow, width: number, limit: number): value is UsagePeriod[] {
  if (!Array.isArray(value) || value.length > limit) return false
  const seen = new Set<number>()
  return value.every(row => {
    if (!totals(row) || !object(row) || !integer(row.startMs) || row.startMs % width !== 0 || row.startMs < Math.floor(window.startMs / width) * width || row.startMs >= window.endMs || seen.has(row.startMs)) return false
    seen.add(row.startMs)
    return true
  })
}
function providerPeriods(value: unknown, window: UsageWindow, width: number, limit: number): value is UsageProviderPeriod[] {
  if (!Array.isArray(value) || value.length > limit * USAGE_PROVIDERS.length) return false
  const seen = new Set<string>()
  return value.every(row => {
    if (!totals(row) || !object(row) || !provider(row.providerId) || !integer(row.startMs) || row.startMs % width !== 0 || row.startMs < Math.floor(window.startMs / width) * width || row.startMs >= window.endMs) return false
    const key = JSON.stringify([row.startMs, row.providerId])
    if (seen.has(key)) return false
    seen.add(key)
    return true
  })
}

export const EMPTY_USAGE_TOTALS: UsageTotals = { inputTokens: 0, cachedInputTokens: 0, cacheWriteTokens: 0, outputTokens: 0, reasoningTokens: 0, totalTokens: 0, reportedCostUsd: 0, estimatedCostUsd: 0, estimatedCacheSavingsUsd: 0, reportedCostTokens: 0, estimatedCostTokens: 0, unpricedTokens: 0, records: 0, sessions: 0 }
/** Provider identities are disjoint, so their distinct session counts can be summed safely. */
export function sumProviderUsage(rows: readonly UsageTotals[]): UsageTotals {
  const result = { ...EMPTY_USAGE_TOTALS }
  for (const row of rows) for (const key of Object.keys(result) as (keyof UsageTotals)[]) result[key] += row[key]
  return result
}
function sameTotals(a: UsageTotals, b: UsageTotals): boolean {
  return countFields.every(key => a[key] === b[key]) && ['reportedCostUsd', 'estimatedCostUsd', 'estimatedCacheSavingsUsd'].every(key => {
    const field = key as 'reportedCostUsd' | 'estimatedCostUsd' | 'estimatedCacheSavingsUsd'
    return Number.isFinite(a[field]) && Math.abs(a[field] - b[field]) <= Math.max(1e-9, b[field] * 1e-12)
  })
}

/** Reject incompatible, stale or malformed native summaries rather than presenting missing data as zero. */
export function parseUsageHistory(value: unknown, executionHostId: string, window: UsageWindow): UsageHistory {
  if (!object(value) || (value.version !== 2 && value.version !== 3)) throw new Error('Usage data is incompatible with this build. Update UAM and its runner.')
  if (value.version === 3) {
    if (!timestamp(value.asOfMs) || value.windowDays !== window.days || typeof value.collecting !== 'boolean'
      || typeof value.collectionError !== 'string' || value.collectionError.length > 1024) throw new Error('Cached usage data is invalid. Refresh to try again.')
    window = usageWindow(window.days!, value.asOfMs)
  }
  if (value.executionHostId !== executionHostId || value.startMs !== window.startMs || value.endMs !== window.endMs || value.source !== 'native-transcripts') throw new Error('Usage data belongs to a different host or time range. Refresh to try again.')
  if (!totals(value.totals) || typeof value.complete !== 'boolean' || typeof value.listingLimited !== 'boolean'
    || !['filesChecked', 'filesTotal', 'bytesRead', 'duplicates', 'unidentifiedRecords'].every(key => integer(value[key]))
    || (value.filesChecked as number) > (value.filesTotal as number)) throw new Error('Usage data is incomplete or invalid. Refresh to try again.')
  if (value.coverage !== undefined && !coverage(value.coverage, executionHostId)) throw new Error('Usage machine coverage is invalid. Refresh to try again.')
  if (value.pricing !== undefined && !pricing(value.pricing)) throw new Error('Usage pricing data is invalid. Refresh to try again.')
  const seenProviders = new Set<string>(), seenModels = new Set<string>()
  if (!Array.isArray(value.providers) || value.providers.length !== USAGE_PROVIDERS.length || !value.providers.every(row => {
    if (!totals(row) || !object(row) || !provider(row.providerId) || seenProviders.has(row.providerId) || typeof row.available !== 'boolean'
      || !['files', 'errors', 'invalidLines', 'oversizedLines', 'suppressedForkRecords', 'pendingLines'].every(key => integer(row[key]))) return false
    seenProviders.add(row.providerId)
    return true
  })) throw new Error('Usage provider data is invalid. Refresh to try again.')
  if (!Array.isArray(value.models) || value.models.length > 20000 || !value.models.every(row => {
    if (!totals(row) || !object(row) || !provider(row.providerId) || typeof row.modelId !== 'string' || !row.modelId || row.modelId.length > 512 || !speed(row.speed) || (row.priceReference !== undefined && !priceReference(row.priceReference))) return false
    const key = JSON.stringify([row.providerId, row.modelId, row.speed])
    if (seenModels.has(key)) return false
    seenModels.add(key)
    return true
  }) || !periods(value.days, window, 86400000, 91) || !periods(value.hours, window, 3600000, 25)
    || !providerPeriods(value.providerDays, window, 86400000, 91) || !providerPeriods(value.providerHours, window, 3600000, 25)) throw new Error('Usage breakdown data is invalid. Refresh to try again.')
  const parsed = value as unknown as UsageHistory
  if (!sameTotals(sumProviderUsage(parsed.providers), parsed.totals)) throw new Error('Usage provider totals do not match. Refresh to try again.')
  for (const [combined, providers] of [[parsed.days, parsed.providerDays], [parsed.hours, parsed.providerHours]] as const) {
    const buckets = new Map<number, UsageProviderPeriod[]>()
    for (const row of providers) buckets.set(row.startMs, [...(buckets.get(row.startMs) ?? []), row])
    if (buckets.size !== combined.length || !combined.every(row => sameTotals(sumProviderUsage(buckets.get(row.startMs) ?? []), row))) throw new Error('Usage period totals do not match. Refresh to try again.')
  }
  return parsed
}

export function usageCost(totals: UsageTotals): number | null {
  return totals.totalTokens > 0 && totals.reportedCostTokens + totals.estimatedCostTokens === 0 ? null : totals.reportedCostUsd + totals.estimatedCostUsd
}
export function formatUsageCount(value: number): string {
  return new Intl.NumberFormat(undefined, { notation: 'compact', maximumFractionDigits: 1 }).format(value)
}
export function formatUsageCost(totals: UsageTotals): string {
  const value = usageCost(totals)
  if (value === null) return 'Unknown'
  return new Intl.NumberFormat(undefined, { style: 'currency', currency: 'USD', maximumFractionDigits: value > 0 && value < 0.01 ? 4 : 2 }).format(value)
}
