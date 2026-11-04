import { isCefContext, sendToCEF } from '../ipc/cefBridge'
import type { AcpRateLimits, AcpRateLimitWindow } from '../store/cpp/types'
import { USAGE_PROVIDERS, type UsageProviderId } from './usageHistory'

export interface ProviderAllowanceWindow extends AcpRateLimitWindow {
  id: string
  label: string
}
export interface ProviderAllowance {
  providerId: UsageProviderId
  scope: string
  status: 'ready' | 'loading' | 'unavailable' | 'error'
  limits: AcpRateLimits | null
  error: string
  primaryLabel?: string
  secondaryLabel?: string
  windows?: ProviderAllowanceWindow[]
}
export interface ProviderAllowanceSnapshot {
  executionHostId: string
  providers: ProviderAllowance[]
  refreshing: boolean
  refreshAfterMs?: number
}
interface AllowanceState {
  data: ProviderAllowanceSnapshot | null
  loading: boolean
  refreshing: boolean
  requestingRefresh: boolean
  cooldownUntil: number
  error: string
}
interface Entry {
  state: AllowanceState
  listeners: Set<() => void>
  pending?: Promise<void>
  generation: number
  pendingRefresh: boolean
  queuedRefresh: boolean
}

const cache = new Map<string, Entry>()
function entry(host: string): Entry {
  let value = cache.get(host)
  if (!value) {
    value = { state: { data: null, loading: true, refreshing: false, requestingRefresh: false, cooldownUntil: 0, error: '' }, listeners: new Set(), generation: 0, pendingRefresh: false, queuedRefresh: false }
    cache.set(host, value)
  }
  return value
}
function publish(value: Entry, state: AllowanceState) {
  value.state = state
  value.listeners.forEach(listener => listener())
}
export function getProviderAllowanceState(host: string) { return entry(host).state }
export function subscribeProviderAllowances(host: string, listener: () => void) {
  const value = entry(host)
  value.listeners.add(listener)
  return () => { value.listeners.delete(listener) }
}

const object = (value: unknown): value is Record<string, unknown> => typeof value === 'object' && value !== null && !Array.isArray(value)
const percent = (value: unknown) => typeof value === 'number' && Number.isFinite(value) && value >= 0 && value <= 100
const nonnegativeInteger = (value: unknown) => typeof value === 'number' && Number.isSafeInteger(value) && value >= 0
function validWindow(value: unknown): value is AcpRateLimitWindow {
  return object(value) && percent(value.usedPercent)
    && (value.resetsAt === null || (nonnegativeInteger(value.resetsAt) && (value.resetsAt as number) <= 253402300799))
    && (value.windowDurationMinutes === null || (nonnegativeInteger(value.windowDurationMinutes) && (value.windowDurationMinutes as number) > 0))
}
function validModelWindows(value: unknown): value is ProviderAllowanceWindow[] {
  if (!Array.isArray(value) || value.length > 64) return false
  const ids = new Set<string>()
  return value.every(window => {
    if (!validWindow(window) || !object(window) || typeof window.id !== 'string' || !window.id.trim() || window.id.length > 128 || ids.has(window.id)
      || typeof window.label !== 'string' || !window.label.trim() || window.label.length > 128) return false
    ids.add(window.id)
    return true
  })
}
function validLimits(value: unknown): value is AcpRateLimits {
  if (!object(value) || !nonnegativeInteger(value.updatedAt) || (value.updatedAt as number) > 8640000000000000
    || !['limitId', 'limitName', 'planType', 'rateLimitReachedType'].every(key => typeof value[key] === 'string')) return false
  for (const key of ['primary', 'secondary']) {
    const window = value[key]
    if (window === null) continue
    if (!validWindow(window)) return false
  }
  if (value.credits !== null && (!object(value.credits) || typeof value.credits.hasCredits !== 'boolean' || typeof value.credits.unlimited !== 'boolean' || !(value.credits.balance === null || typeof value.credits.balance === 'string'))) return false
  if (value.individualLimit !== null && (!object(value.individualLimit) || typeof value.individualLimit.limit !== 'string' || typeof value.individualLimit.used !== 'string' || !percent(value.individualLimit.remainingPercent) || !nonnegativeInteger(value.individualLimit.resetsAt))) return false
  return value.spendControlReached === null || typeof value.spendControlReached === 'boolean'
}
export function parseProviderAllowances(value: unknown, host: string): ProviderAllowanceSnapshot {
  if (!object(value) || value.executionHostId !== host || typeof value.refreshing !== 'boolean' || !Array.isArray(value.providers) || value.providers.length > USAGE_PROVIDERS.length) throw new Error('Account allowances returned an invalid computer or response.')
  if (value.refreshAfterMs !== undefined && (!nonnegativeInteger(value.refreshAfterMs) || (value.refreshAfterMs as number) > 30000)) throw new Error('Account allowances returned an invalid refresh interval.')
  const seen = new Set<string>()
  for (const row of value.providers) {
    if (!object(row) || !USAGE_PROVIDERS.includes(row.providerId as UsageProviderId) || seen.has(row.providerId as string)
      || typeof row.scope !== 'string' || !row.scope || row.scope.length > 256
      || !['ready', 'loading', 'unavailable', 'error'].includes(row.status as string) || typeof row.error !== 'string' || row.error.length > 1024
      || !(row.limits === null || validLimits(row.limits))
      || !(row.windows === undefined || validModelWindows(row.windows))
      || !['primaryLabel', 'secondaryLabel'].every(key => row[key] === undefined || (typeof row[key] === 'string' && (row[key] as string).length <= 128))) throw new Error('Account allowances returned invalid provider data.')
    seen.add(row.providerId as string)
  }
  return value as unknown as ProviderAllowanceSnapshot
}

/** Read the native account cache, optionally starting a coalesced independent refresh. No chat is created. */
export function preloadProviderAllowances(host = 'local', refresh = false): Promise<void> {
  const value = entry(host)
  if (refresh && value.state.cooldownUntil > Date.now()) return value.pending ?? Promise.resolve()
  if (value.pending) {
    if (refresh && !value.pendingRefresh) {
      value.queuedRefresh = true
      if (!value.state.requestingRefresh) publish(value, { ...value.state, requestingRefresh: true })
    }
    return value.pending
  }
  const generation = value.generation
  value.pendingRefresh = refresh
  if (refresh || value.state.loading !== !value.state.data) publish(value, { ...value.state, loading: !value.state.data, requestingRefresh: refresh })
  value.pending = (async () => {
    await Promise.resolve()
    let timeout: ReturnType<typeof setTimeout> | undefined
    try {
      if (!isCefContext()) throw new Error('Account allowances are available in the desktop app.')
      const reply = await Promise.race([
        sendToCEF({ action: 'getProviderAllowances', payload: { executionHostId: host, allowKeychainPrompt: true, ...(refresh ? { refresh: true } : {}) } }, false),
        new Promise<never>((_, reject) => { timeout = setTimeout(() => reject(new Error('Account allowances took too long to load. Try refreshing.')), 15000) }),
      ])
      if (!reply.ok) throw new Error(reply.error || 'Could not read account allowances.')
      const next = parseProviderAllowances(reply.data, host)
      if (generation !== value.generation) return
      // Native owns retention and auth invalidation; an authoritative null must clear old account data.
      const cooldownUntil = next.refreshAfterMs ? Date.now() + next.refreshAfterMs : 0
      publish(value, { data: next, loading: false, refreshing: next.refreshing, requestingRefresh: value.queuedRefresh && cooldownUntil <= Date.now(), cooldownUntil, error: '' })
    } catch (error) {
      if (generation === value.generation) publish(value, { ...value.state, loading: false, refreshing: false, requestingRefresh: value.queuedRefresh, error: error instanceof Error ? error.message : 'Could not read account allowances.' })
    } finally {
      clearTimeout(timeout)
      if (generation === value.generation) {
        const followup = value.queuedRefresh && !refresh && value.state.cooldownUntil <= Date.now()
        value.pending = undefined
        value.pendingRefresh = false
        value.queuedRefresh = false
        if (followup) await preloadProviderAllowances(host, true)
      }
    }
  })()
  return value.pending
}

/** Discard account snapshots when authentication or the application context is replaced. */
export function clearProviderAllowanceCache() {
  for (const value of cache.values()) {
    value.generation++
    value.pending = undefined
    value.pendingRefresh = false
    value.queuedRefresh = false
    publish(value, { data: null, loading: true, refreshing: false, requestingRefresh: false, cooldownUntil: 0, error: '' })
  }
}
