/** Why automatic routing passed over one computer for a waiting chat; native admission decides, this only explains. */
export type RoutingSkipReason = 'off' | 'unreachable' | 'providerUnavailable' | 'unconfirmed' | 'noMapping' | 'full'
export interface RoutingWaitHost { hostId: string; reason: RoutingSkipReason }

/** `known` alone carries figures. Every other status is unknown load, never an idle computer. */
export type HostLoadStatus = 'known' | 'pending' | 'unsupported' | 'unreachable' | 'stale' | 'notSampled'
export interface HostLoadFigures {
  status: HostLoadStatus
  cores: number | null
  /** One-minute load average (macOS and Linux). */
  loadAverage1m: number | null
  /** Busy share of all processors (Windows, which has no load average). */
  cpuPercent: number | null
  memoryAvailableBytes: number | null
  memoryTotalBytes: number | null
  /** UAM provider processes on that computer: its helper's count, or this computer's own. */
  processes: number | null
  /** Chats occupying the computer as routing counts them. */
  inUse: number
  /** Chats on this computer whose turn waits for routing or a compute pool slot. */
  waiting: number
  /** Concurrent sessions its routing policy allows; null without a policy. */
  limit: number | null
}
export interface ExecutionHostLoad { hosts: Record<string, HostLoadFigures>; unassignedWaiting: number }

export const EMPTY_HOST_LOAD: ExecutionHostLoad = { hosts: {}, unassignedWaiting: 0 }

const SKIP_REASONS: readonly RoutingSkipReason[] = ['off', 'unreachable', 'providerUnavailable', 'unconfirmed', 'noMapping', 'full']
const LOAD_STATUSES: readonly HostLoadStatus[] = ['known', 'pending', 'unsupported', 'unreachable', 'stale', 'notSampled']

/** Short reason beside a computer's name in a routing wait. */
export const ROUTING_SKIP_LABEL: Record<RoutingSkipReason, string> = {
  full: 'Full',
  unreachable: 'Unreachable',
  unconfirmed: 'Provider not confirmed',
  noMapping: 'No folder mapped',
  providerUnavailable: 'Provider unavailable',
  off: 'Off',
}

const isRecord = (value: unknown): value is Record<string, unknown> => typeof value === 'object' && value !== null && !Array.isArray(value)
const count = (value: unknown): number | null => typeof value === 'number' && Number.isInteger(value) && value >= 0 ? value : null
const figure = (value: unknown, max = Number.MAX_SAFE_INTEGER): number | null => typeof value === 'number' && Number.isFinite(value) && value >= 0 && value <= max ? value : null

export function sanitizeRoutingWaitHosts(value: unknown): RoutingWaitHost[] | undefined {
  if (!Array.isArray(value)) return undefined
  return value.flatMap((entry) => isRecord(entry) && typeof entry.hostId === 'string' && entry.hostId && SKIP_REASONS.includes(entry.reason as RoutingSkipReason)
    ? [{ hostId: entry.hostId, reason: entry.reason as RoutingSkipReason }]
    : [])
}

export function sameRoutingWaitHosts(left: RoutingWaitHost[] | undefined, right: RoutingWaitHost[] | undefined): boolean {
  if (left === right) return true
  const a = left ?? [], b = right ?? []
  return a.length === b.length && a.every((entry, index) => entry.hostId === b[index].hostId && entry.reason === b[index].reason)
}

export function sanitizeExecutionHostLoad(value: unknown): ExecutionHostLoad {
  if (!isRecord(value) || !isRecord(value.hosts)) return EMPTY_HOST_LOAD
  const hosts: Record<string, HostLoadFigures> = {}
  for (const [id, entry] of Object.entries(value.hosts)) {
    if (!isRecord(entry)) continue
    const status = LOAD_STATUSES.includes(entry.status as HostLoadStatus) ? entry.status as HostLoadStatus : 'notSampled'
    const known = status === 'known'
    hosts[id] = {
      status,
      // Figures are only trusted with a current reading; anything else stays unknown.
      cores: known ? count(entry.cores) : null,
      loadAverage1m: known ? figure(entry.loadAverage1m) : null,
      cpuPercent: known ? figure(entry.cpuPercent, 100) : null,
      memoryAvailableBytes: known ? figure(entry.memoryAvailableBytes) : null,
      memoryTotalBytes: known ? figure(entry.memoryTotalBytes) : null,
      processes: id === 'local' || known ? count(entry.processes) : null,
      inUse: count(entry.inUse) ?? 0,
      waiting: count(entry.waiting) ?? 0,
      limit: count(entry.limit),
    }
  }
  return { hosts, unassignedWaiting: count(value.unassignedWaiting) ?? 0 }
}

const oneDecimal = new Intl.NumberFormat(undefined, { maximumFractionDigits: 1, minimumFractionDigits: 1 })

/** "3/8" with a concurrent-session limit, "3 in use" without one. */
export function hostSlotsLabel(figures: HostLoadFigures | undefined): string {
  if (!figures) return ''
  return figures.limit !== null ? `${figures.inUse}/${figures.limit}` : `${figures.inUse} in use`
}

/** The chip text: "<computer> · 3/8". */
export function hostChipLabel(label: string, figures: HostLoadFigures | undefined): string {
  const slots = hostSlotsLabel(figures)
  return slots ? `${label} · ${slots}` : label
}

const UNKNOWN_LOAD: Record<Exclude<HostLoadStatus, 'known'>, string> = {
  pending: 'Reading load…',
  unsupported: 'Load unknown: update this computer’s helper',
  unreachable: 'Load unknown: no answer',
  stale: 'Load unknown: no recent reading',
  notSampled: 'Load not read',
}

/** Live load in one line, for example "Load 2.5 on 8 cores · 4.0 GB free · 3 processes". Unknown load says so. */
export function hostLoadSummary(figures: HostLoadFigures | undefined): string {
  if (!figures) return UNKNOWN_LOAD.notSampled
  const parts: string[] = []
  if (figures.status !== 'known') parts.push(UNKNOWN_LOAD[figures.status])
  else {
    const cores = figures.cores !== null ? ` on ${figures.cores} ${figures.cores === 1 ? 'core' : 'cores'}` : ''
    if (figures.loadAverage1m !== null) parts.push(`Load ${oneDecimal.format(figures.loadAverage1m)}${cores}`)
    else if (figures.cpuPercent !== null) parts.push(`CPU ${Math.round(figures.cpuPercent)}%${cores}`)
    else parts.push(`Load unknown${cores}`)
    if (figures.memoryAvailableBytes !== null) parts.push(`${oneDecimal.format(figures.memoryAvailableBytes / 1024 ** 3)} GB free`)
  }
  if (figures.processes !== null) parts.push(`${figures.processes} ${figures.processes === 1 ? 'process' : 'processes'}`)
  return parts.join(' · ')
}

/** In-use and waiting counts in one line, for example "3 of 8 in use · 2 waiting". */
export function hostUsageSummary(figures: HostLoadFigures | undefined): string {
  if (!figures) return ''
  const inUse = figures.limit !== null ? `${figures.inUse} of ${figures.limit} in use` : `${figures.inUse} in use`
  return figures.waiting > 0 ? `${inUse} · ${figures.waiting} waiting` : inUse
}
