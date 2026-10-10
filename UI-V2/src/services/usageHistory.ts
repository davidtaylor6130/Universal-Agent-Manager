import { ANTIGRAVITY_CLI_PROVIDER_ID, CLAUDE_CLI_PROVIDER_ID, CODEX_CLI_PROVIDER_ID, COPILOT_CLI_PROVIDER_ID, DEFAULT_PROVIDER_ID, OPENCODE_CLI_PROVIDER_ID } from '../utils/providerMetadata'

export const USAGE_PROVIDERS = [CODEX_CLI_PROVIDER_ID, CLAUDE_CLI_PROVIDER_ID, DEFAULT_PROVIDER_ID, OPENCODE_CLI_PROVIDER_ID, COPILOT_CLI_PROVIDER_ID, ANTIGRAVITY_CLI_PROVIDER_ID] as const
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
}
export interface UsagePeriod extends UsageTotals { startMs: number }
export interface UsageHistory {
  version: 1
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
export interface UsageWindow { startMs: number; endMs: number }
export const USAGE_WINDOWS: { days: UsageWindowDays; label: string }[] = [
  { days: 1, label: 'Past 24h' }, { days: 7, label: '7 days' }, { days: 30, label: '30 days' }, { days: 90, label: '90 days' },
]
export function usageWindow(days: UsageWindowDays, now = Date.now()): UsageWindow {
  return { startMs: Math.max(0, now - days * 86400000), endMs: now }
}

const integer = (value: unknown): value is number => typeof value === 'number' && Number.isSafeInteger(value) && value >= 0
const money = (value: unknown): value is number => typeof value === 'number' && Number.isFinite(value) && value >= 0
const object = (value: unknown): value is Record<string, unknown> => typeof value === 'object' && value !== null && !Array.isArray(value)
const provider = (value: unknown): value is UsageProviderId => USAGE_PROVIDERS.includes(value as UsageProviderId)
const speed = (value: unknown): value is UsageSpeed => value === 'standard' || value === 'fast' || value === 'ultrafast'
const countFields = ['inputTokens', 'cachedInputTokens', 'cacheWriteTokens', 'outputTokens', 'reasoningTokens', 'totalTokens', 'reportedCostTokens', 'estimatedCostTokens', 'unpricedTokens', 'records', 'sessions'] as const

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

/** Reject incompatible, stale or malformed native summaries rather than presenting missing data as zero. */
export function parseUsageHistory(value: unknown, executionHostId: string, window: UsageWindow): UsageHistory {
  if (!object(value) || value.version !== 1) throw new Error('Usage data is incompatible with this build. Update UAM and its runner.')
  if (value.executionHostId !== executionHostId || value.startMs !== window.startMs || value.endMs !== window.endMs || value.source !== 'native-transcripts') throw new Error('Usage data belongs to a different host or time range. Refresh to try again.')
  if (!totals(value.totals) || typeof value.complete !== 'boolean' || typeof value.listingLimited !== 'boolean'
    || !['filesChecked', 'filesTotal', 'bytesRead', 'duplicates', 'unidentifiedRecords'].every(key => integer(value[key]))
    || (value.filesChecked as number) > (value.filesTotal as number)) throw new Error('Usage data is incomplete or invalid. Refresh to try again.')
  const seenProviders = new Set<string>(), seenModels = new Set<string>()
  if (!Array.isArray(value.providers) || value.providers.length !== USAGE_PROVIDERS.length || !value.providers.every(row => {
    if (!totals(row) || !object(row) || !provider(row.providerId) || seenProviders.has(row.providerId) || typeof row.available !== 'boolean'
      || !['files', 'errors', 'invalidLines', 'oversizedLines', 'suppressedForkRecords', 'pendingLines'].every(key => integer(row[key]))) return false
    seenProviders.add(row.providerId)
    return true
  })) throw new Error('Usage provider data is invalid. Refresh to try again.')
  if (!Array.isArray(value.models) || value.models.length > 20000 || !value.models.every(row => {
    if (!totals(row) || !object(row) || !provider(row.providerId) || typeof row.modelId !== 'string' || !row.modelId || row.modelId.length > 512 || !speed(row.speed)) return false
    const key = JSON.stringify([row.providerId, row.modelId, row.speed])
    if (seenModels.has(key)) return false
    seenModels.add(key)
    return true
  }) || !periods(value.days, window, 86400000, 91) || !periods(value.hours, window, 3600000, 25)) throw new Error('Usage breakdown data is invalid. Refresh to try again.')
  return value as unknown as UsageHistory
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
