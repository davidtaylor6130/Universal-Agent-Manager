import type { MemoryLevel } from '../types/memory'
import type { MemoryWorkerBinding } from './cpp/types'
import {
  clampedFiniteNumberOr, normalizeMemoryLevel, normalizeGoalMaxLoopIterations,
  normalizeAcpSetupInactivityTimeoutSeconds, normalizeAcpTurnOutputLimitMiB,
  MIN_MEMORY_IDLE_DELAY_SECONDS, MAX_MEMORY_IDLE_DELAY_SECONDS,
  MIN_MEMORY_RECALL_BUDGET_BYTES, MAX_MEMORY_RECALL_BUDGET_BYTES,
} from './cpp/sanitizers'

/** Only this action's ten canonical fields; never an export of all settings. */
export interface MemorySettingsValues {
  memoryEnabledDefault: boolean
  memoryLevelDefault: MemoryLevel
  memoryIdleDelaySeconds: number
  memoryRecallBudgetBytes: number
  goalMaxLoopIterations: number
  acpSetupInactivityTimeoutSeconds: number
  acpTurnOutputLimitMiB: number
  memoryWorkerBindings: Record<string, MemoryWorkerBinding>
  permissionReviewerProviderId: string
  permissionReviewerModelId: string
}
export interface MemorySettingsSnapshot {
  scope: string
  revision: number
  primaryValidated: boolean
  values?: MemorySettingsValues
}
export interface MemorySettingsResult extends MemorySettingsSnapshot {
  memoryRequestId: string
  outcome: 'committed' | 'rejected' | 'unknown'
}
interface BridgeRequest { action: 'getMemorySettings' | 'setMemorySettings'; payload: Record<string, unknown>; requestId?: string }
interface BridgeResponse { ok: boolean; data?: unknown; error?: string }
export interface MemorySettingsWriteDependencies {
  readValues: () => MemorySettingsValues
  applyValues: (values: Partial<MemorySettingsValues>) => void
  isNative: () => boolean
  request: (request: BridgeRequest) => Promise<BridgeResponse>
  requestId: () => string
  reportFailure?: (outcome: 'rejected' | 'unknown' | 'not-sent') => void
  /** Display metadata only: current visible Unknown drafts, not native/runtime authority. */
  publishUnconfirmedFields?: (fields: string[]) => void
}
const fields = [
  'memoryEnabledDefault', 'memoryLevelDefault', 'memoryIdleDelaySeconds', 'memoryRecallBudgetBytes',
  'goalMaxLoopIterations', 'acpSetupInactivityTimeoutSeconds', 'acpTurnOutputLimitMiB',
  'memoryWorkerBindings', 'permissionReviewerProviderId', 'permissionReviewerModelId',
] as const
const wireNames: Record<keyof MemorySettingsValues, string> = {
  memoryEnabledDefault: 'enabledDefault', memoryLevelDefault: 'levelDefault',
  memoryIdleDelaySeconds: 'idleDelaySeconds', memoryRecallBudgetBytes: 'recallBudgetBytes',
  goalMaxLoopIterations: 'goalMaxLoopIterations', acpSetupInactivityTimeoutSeconds: 'acpSetupInactivityTimeoutSeconds',
  acpTurnOutputLimitMiB: 'acpTurnOutputLimitMiB', memoryWorkerBindings: 'workerBindings',
  permissionReviewerProviderId: 'permissionReviewerProviderId', permissionReviewerModelId: 'permissionReviewerModelId',
}
const uuid = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/
const record = (value: unknown): value is Record<string, unknown> => typeof value === 'object' && value !== null && !Array.isArray(value)
const integer = (value: unknown): value is number => typeof value === 'number' && Number.isSafeInteger(value)
const text = (value: unknown): value is string => typeof value === 'string' && !/[\u0000-\u001f\u007f]/.test(value)
const within = (value: unknown, min: number, max: number) => integer(value) && value >= min && value <= max
const bindingEqual = (a: MemoryWorkerBinding | undefined, b: MemoryWorkerBinding | undefined) => a?.workerProviderId === b?.workerProviderId && a?.workerModelId === b?.workerModelId

/** Reject malformed canonical receipts rather than manufacturing values with defaults. */
export function parseMemorySettingsValues(value: unknown): MemorySettingsValues | null {
  if (!record(value) || Object.keys(value).length !== fields.length || !fields.every((key) => Object.prototype.hasOwnProperty.call(value, key))) return null
  if (typeof value.memoryEnabledDefault !== 'boolean' || (typeof value.memoryLevelDefault !== 'string' || !['off', 'readOnly', 'strict', 'balanced', 'open'].includes(value.memoryLevelDefault)) ||
    value.memoryEnabledDefault !== (value.memoryLevelDefault !== 'off') ||
    !within(value.memoryIdleDelaySeconds, MIN_MEMORY_IDLE_DELAY_SECONDS, MAX_MEMORY_IDLE_DELAY_SECONDS) ||
    !within(value.memoryRecallBudgetBytes, MIN_MEMORY_RECALL_BUDGET_BYTES, MAX_MEMORY_RECALL_BUDGET_BYTES) ||
    !integer(value.goalMaxLoopIterations) || value.goalMaxLoopIterations !== normalizeGoalMaxLoopIterations(value.goalMaxLoopIterations) ||
    !integer(value.acpSetupInactivityTimeoutSeconds) || value.acpSetupInactivityTimeoutSeconds !== normalizeAcpSetupInactivityTimeoutSeconds(value.acpSetupInactivityTimeoutSeconds) ||
    !integer(value.acpTurnOutputLimitMiB) || value.acpTurnOutputLimitMiB !== normalizeAcpTurnOutputLimitMiB(value.acpTurnOutputLimitMiB) ||
    !text(value.permissionReviewerProviderId) || !text(value.permissionReviewerModelId) || !record(value.memoryWorkerBindings)) return null
  const bindings: Record<string, unknown> = value.memoryWorkerBindings
  for (const [key, entry] of Object.entries(bindings)) {
    if (!key || !text(key) || !record(entry) || Object.keys(entry).length !== 2 || !text(entry.workerProviderId) || !entry.workerProviderId || !text(entry.workerModelId)) return null
  }
  // Entries were validated above; construct the typed map only after narrowing unknown input.
  const validatedBindings = Object.fromEntries(Object.entries(bindings).map(([key, entry]) => [key, { ...(entry as MemoryWorkerBinding) }]))
  return { ...value, memoryWorkerBindings: validatedBindings } as unknown as MemorySettingsValues
}
export function parseMemorySettingsSnapshot(value: unknown): MemorySettingsSnapshot | null {
  if (!record(value) || typeof value.scope !== 'string' || !uuid.test(value.scope) || !integer(value.revision) || value.revision < 0 || typeof value.primaryValidated !== 'boolean') return null
  const values = value.primaryValidated ? parseMemorySettingsValues(value.values) : null
  if (value.primaryValidated && !values) return null
  return { scope: value.scope, revision: value.revision, primaryValidated: value.primaryValidated, ...(values ? { values } : {}) }
}
export function parseMemorySettingsResult(value: unknown, expected: { memoryRequestId: string; scope: string; revision: number }): MemorySettingsResult | null {
  const snapshot = parseMemorySettingsSnapshot(value)
  if (!snapshot || !record(value) || value.memoryRequestId !== expected.memoryRequestId || snapshot.scope !== expected.scope ||
    !['committed', 'rejected', 'unknown'].includes(String(value.outcome))) return null
  const outcome = value.outcome as MemorySettingsResult['outcome']
  if (outcome !== 'unknown' && (!snapshot.primaryValidated || !snapshot.values || snapshot.revision < expected.revision)) return null
  if (outcome === 'committed' && snapshot.revision <= expected.revision) return null
  // Unknown values are never canonical authority, even if a peer includes them.
  return { ...snapshot, memoryRequestId: expected.memoryRequestId, outcome, ...(outcome === 'unknown' ? { primaryValidated: false, values: undefined } : {}) }
}

function normalizeRequested(partial: Partial<MemorySettingsValues>, previous: MemorySettingsValues): Partial<MemorySettingsValues> {
  const next: Partial<MemorySettingsValues> = {}
  if (partial.memoryEnabledDefault !== undefined || partial.memoryLevelDefault !== undefined) {
    const level = normalizeMemoryLevel(partial.memoryLevelDefault ?? (partial.memoryEnabledDefault ? 'strict' : 'off'), true, false) as MemorySettingsValues['memoryLevelDefault']
    next.memoryLevelDefault = level; next.memoryEnabledDefault = level !== 'off'
  }
  if (partial.memoryIdleDelaySeconds !== undefined) next.memoryIdleDelaySeconds = clampedFiniteNumberOr(partial.memoryIdleDelaySeconds, previous.memoryIdleDelaySeconds, MIN_MEMORY_IDLE_DELAY_SECONDS, MAX_MEMORY_IDLE_DELAY_SECONDS)
  if (partial.memoryRecallBudgetBytes !== undefined) next.memoryRecallBudgetBytes = clampedFiniteNumberOr(partial.memoryRecallBudgetBytes, previous.memoryRecallBudgetBytes, MIN_MEMORY_RECALL_BUDGET_BYTES, MAX_MEMORY_RECALL_BUDGET_BYTES)
  if (partial.goalMaxLoopIterations !== undefined) next.goalMaxLoopIterations = normalizeGoalMaxLoopIterations(partial.goalMaxLoopIterations)
  if (partial.acpSetupInactivityTimeoutSeconds !== undefined) next.acpSetupInactivityTimeoutSeconds = normalizeAcpSetupInactivityTimeoutSeconds(partial.acpSetupInactivityTimeoutSeconds)
  if (partial.acpTurnOutputLimitMiB !== undefined) next.acpTurnOutputLimitMiB = normalizeAcpTurnOutputLimitMiB(partial.acpTurnOutputLimitMiB)
  if (partial.permissionReviewerProviderId !== undefined) next.permissionReviewerProviderId = partial.permissionReviewerProviderId
  if (partial.permissionReviewerModelId !== undefined) next.permissionReviewerModelId = partial.permissionReviewerModelId
  if (partial.memoryWorkerBindings !== undefined) {
    // Existing native semantics merge entries; omitted keys are not deletions.
    next.memoryWorkerBindings = Object.fromEntries(Object.entries(partial.memoryWorkerBindings).filter(([key, binding]) => !bindingEqual(previous.memoryWorkerBindings[key], binding)).map(([key, binding]) => [key, { ...binding }]))
  }
  return next
}
interface Intent { sequence: number; id: string; fields: Partial<MemorySettingsValues>; resolve: (success: boolean) => void }
interface Draft { sequence: number; value: unknown; unknown: boolean }

/** Per-store controller. Its sequence owns local drafts, never native publication order. */
export function createMemorySettingsWrites(deps: MemorySettingsWriteDependencies) {
  let sequence = 0
  let running = false
  let canonicalBase: MemorySettingsValues | null = null
  const queue: Intent[] = []
  const drafts = new Map<string, Draft[]>()
  let publishedUnconfirmedFields: string[] | null = null
  function publishUnconfirmedFields() {
    const visible = new Set<string>()
    for (const [key, history] of drafts) {
      if (history[history.length - 1]?.unknown) visible.add(key.startsWith('memoryWorkerBindings:') ? 'memoryWorkerBindings' : key)
    }
    const next = fields.filter((field) => visible.has(field))
    if (publishedUnconfirmedFields && next.length === publishedUnconfirmedFields.length && next.every((field, index) => field === publishedUnconfirmedFields![index])) return
    try {
      deps.publishUnconfirmedFields?.([...next])
      publishedUnconfirmedFields = next
    } catch { /* A display observer cannot own or change a save outcome. */ }
  }
  // Zustand get() is not initialized while slices are constructed.
  let baselineValues: MemorySettingsValues | null = null
  const bindingSlot = (key: string) => `memoryWorkerBindings:${key}`
  function remember(intent: Intent) {
    const displayed = deps.readValues()
    baselineValues ??= displayed
    for (const field of fields) {
      if (field === 'memoryWorkerBindings') {
        for (const [key, value] of Object.entries(intent.fields.memoryWorkerBindings ?? {})) {
          const slot = bindingSlot(key)
          if (!drafts.has(slot)) {
            const bindings: Record<string, MemoryWorkerBinding> = { ...baselineValues.memoryWorkerBindings }
            if (Object.prototype.hasOwnProperty.call(displayed.memoryWorkerBindings, key)) Object.defineProperty(bindings, key, { value: { ...displayed.memoryWorkerBindings[key] }, enumerable: true, configurable: true, writable: true })
            else delete bindings[key]
            baselineValues = { ...baselineValues, memoryWorkerBindings: bindings }
          }
          drafts.set(slot, [...(drafts.get(slot) ?? []), { sequence: intent.sequence, value, unknown: false }])
        }
      } else if (intent.fields[field] !== undefined) {
        if (!drafts.has(field)) baselineValues = { ...baselineValues, [field]: displayed[field] }
        drafts.set(field, [...(drafts.get(field) ?? []), { sequence: intent.sequence, value: intent.fields[field], unknown: false }])
      }
    }
  }
  function retire(predicate: (draft: Draft) => boolean) {
    for (const [key, history] of drafts) {
      const retained = history.filter((draft) => !predicate(draft))
      if (retained.length) drafts.set(key, retained)
      else drafts.delete(key)
    }
  }
  function retainUnknown(intent: Intent) {
    for (const [key, history] of drafts) {
      if (!history.some((draft) => draft.sequence === intent.sequence)) continue
      // Only the latest actually dispatched Unknown for each field is needed;
      // retain every newer queued owner so a failed GET can expose it truthfully.
      drafts.set(key, history.filter((draft) => !draft.unknown || draft.sequence >= intent.sequence)
        .map((draft) => draft.sequence === intent.sequence ? { ...draft, unknown: true } : draft))
    }
  }
  function retireNotSent(intent: Intent) {
    retire((draft) => draft.sequence === intent.sequence)
    const restored = { ...deps.readValues() }
    const baseline = baselineValues ?? deps.readValues()
    for (const field of fields) {
      if (field === 'memoryWorkerBindings') {
        const keys = Object.keys(intent.fields.memoryWorkerBindings ?? {})
        if (!keys.length) continue
        const bindings = { ...restored.memoryWorkerBindings }
        for (const key of keys) {
          if (Object.prototype.hasOwnProperty.call(baseline.memoryWorkerBindings, key)) Object.defineProperty(bindings, key, { value: { ...baseline.memoryWorkerBindings[key] }, enumerable: true, configurable: true, writable: true })
          else delete bindings[key]
        }
        restored.memoryWorkerBindings = bindings
      } else if (intent.fields[field] !== undefined) Object.assign(restored, { [field]: baseline[field] })
    }
    deps.applyValues(overlay(restored))
  }
  /** Accepted native DISPLAY values only. This cannot acknowledge a save or retire a draft. */
  function observeProjection(supplied?: Partial<MemorySettingsValues>) {
    if (!supplied) return
    const has = (field: keyof MemorySettingsValues) => Object.prototype.hasOwnProperty.call(supplied, field) && supplied[field] !== undefined
    const observed: Partial<MemorySettingsValues> = {}
    for (const field of fields) {
      if (field === 'memoryEnabledDefault' || field === 'memoryLevelDefault' || !has(field)) continue
      if (field === 'memoryWorkerBindings') {
        // A supplied native map is a snapshot, including absence of an owned slot.
        observed.memoryWorkerBindings = Object.fromEntries(Object.entries(supplied.memoryWorkerBindings!).map(([key, binding]) => [key, { ...binding }]))
      } else Object.assign(observed, { [field]: supplied[field] })
    }
    if (has('memoryLevelDefault') || has('memoryEnabledDefault')) {
      // Derive both members solely from supplied values; the merged projection
      // may contain the other member from an optimistic local draft.
      const level = normalizeMemoryLevel(
        has('memoryLevelDefault') ? supplied.memoryLevelDefault : supplied.memoryEnabledDefault ? 'strict' : 'off',
        has('memoryEnabledDefault') ? supplied.memoryEnabledDefault! : true, false)
      observed.memoryLevelDefault = level
      observed.memoryEnabledDefault = level !== 'off'
    }
    if (Object.keys(observed).length) baselineValues = { ...(baselineValues ?? deps.readValues()), ...observed }
  }
  function overlay<T extends Partial<MemorySettingsValues>>(projection: T): T {
    const next = { ...projection } as Record<string, unknown>
    let bindings: Record<string, MemoryWorkerBinding> | undefined
    for (const [key, history] of drafts) {
      const draft = history[history.length - 1]
      if (key.startsWith('memoryWorkerBindings:')) {
        bindings ??= { ...(projection.memoryWorkerBindings ?? deps.readValues().memoryWorkerBindings) }
        Object.defineProperty(bindings, key.slice('memoryWorkerBindings:'.length), { value: { ...(draft.value as MemoryWorkerBinding) }, enumerable: true, configurable: true, writable: true })
      } else next[key] = draft.value
    }
    if (bindings) next.memoryWorkerBindings = bindings
    return next as T
  }
  function project() { deps.applyValues(overlay(canonicalBase ?? deps.readValues())) }
  function report(outcome: 'rejected' | 'unknown' | 'not-sent') { try { deps.reportFailure?.(outcome) } catch { /* Reporting cannot own or retire a save. */ } }
  async function run() {
    if (running) return
    running = true
    try {
      while (queue.length) {
        const intent = queue.shift()!
        let success = false
        let outcome: 'committed' | 'rejected' | 'unknown' | 'not-sent' = 'not-sent'
        let dispatched = false
        try {
          const readResponse = await deps.request({ action: 'getMemorySettings', payload: {}, requestId: deps.requestId() })
          const read = readResponse.ok ? parseMemorySettingsSnapshot(readResponse.data) : null
          if (read?.primaryValidated && read.values) {
            canonicalBase = read.values; baselineValues = read.values
            project() // A read never retires Pending or Unknown local drafts.
            const payload: Record<string, unknown> = {
              expectedSettingsScope: read.scope, expectedSettingsRevision: read.revision, memoryRequestId: intent.id,
            }
            for (const field of fields) if (intent.fields[field] !== undefined) payload[wireNames[field]] = intent.fields[field]
            dispatched = true
            outcome = 'unknown'
            const response = await deps.request({ action: 'setMemorySettings', payload, requestId: intent.id })
            const result = response.ok ? parseMemorySettingsResult(response.data, { memoryRequestId: intent.id, scope: read.scope, revision: read.revision }) : null
            if (result && result.outcome !== 'unknown' && result.values) {
              canonicalBase = result.values; baselineValues = result.values; outcome = result.outcome
              // A committed whole-file receipt settles every older unresolved
              // memory draft, while newer queued intents remain overlaid.
              retire((draft) => outcome === 'committed' ? draft.sequence <= intent.sequence : draft.sequence === intent.sequence)
              // The receipt established persistence before any UI callback runs.
              success = outcome === 'committed'
              project()
            }
          }
        } catch { /* Only a dispatched SET can have committed despite lost transport. */ }
        if (!dispatched) { retireNotSent(intent); outcome = 'not-sent' }
        else if (outcome === 'unknown') retainUnknown(intent)
        publishUnconfirmedFields()
        if (!success) report(outcome === 'rejected' ? 'rejected' : outcome === 'not-sent' ? 'not-sent' : 'unknown')
        intent.resolve(success)
      }
    } finally { running = false }
  }
  function save(partial: Partial<MemorySettingsValues>): Promise<boolean> {
    const requested = normalizeRequested(partial, deps.readValues())
    if (!deps.isNative()) {
      deps.applyValues({ ...requested, ...(requested.memoryWorkerBindings ? { memoryWorkerBindings: { ...deps.readValues().memoryWorkerBindings, ...requested.memoryWorkerBindings } } : {}) })
      return Promise.resolve(true)
    }
    return new Promise<boolean>((resolve) => {
      const intent: Intent = { sequence: ++sequence, id: deps.requestId(), fields: requested, resolve }
      remember(intent); deps.applyValues(overlay(deps.readValues())); publishUnconfirmedFields(); queue.push(intent); void run()
    })
  }
  return { save, overlay, observeProjection }
}
