import { isCefContext, sendToCEF, type CEFResponse } from '../../ipc/cefBridge'

export type ManagedSubagentRun = {
  runId: string; parentRunId: string; agentId: string; transcriptChatId: string
  status: string; providerId: string; modelId: string; host: string | null
  createdAt: string; startedAt: string; completedAt: string; startedAtEpochMs: number; finishedAtEpochMs?: number
  executionCapability: string; canCancel: boolean; lastReported?: boolean; summary?: string; taskTitle?: string; followup?: ManagedFollowupState
}

export type ManagedRunTiming = Pick<ManagedSubagentRun, 'status' | 'startedAt' | 'completedAt' | 'startedAtEpochMs' | 'finishedAtEpochMs' | 'lastReported'>

export type ManagedSubagentPage = {
  chatId: string; controlScopeId: string; runs: ManagedSubagentRun[]
  offset: number; nextOffset: number | null; totalCount: number
}

export function record(value: unknown): Record<string, unknown> {
  return value !== null && typeof value === 'object' && !Array.isArray(value) ? value as Record<string, unknown> : {}
}

export function string(value: unknown): string { return typeof value === 'string' ? value : '' }
export function array(value: unknown): unknown[] { return Array.isArray(value) ? value : [] }

/** A bounded human output excerpt; never fall back to task, prompt or diagnostic fields. */
export function boundedSubagentSummary(value: unknown): string | undefined {
  if (typeof value !== 'string') return undefined
  let text = ''
  let bytes = 0
  for (const character of value) {
    const point = character.codePointAt(0)!
    const size = point <= 0x7f ? 1 : point <= 0x7ff ? 2 : point <= 0xffff ? 3 : 4
    if (bytes + size > 1024) break
    bytes += size
    text += character
  }
  return text.replace(/\s+/g, ' ').trim() || undefined
}

export const GENERIC_SUBAGENT_TITLE = 'Subagent task'

/** Presentation-only label of what a run is doing, one meaningful line of its owned task; never the agent definition, provider, result or diagnostics. */
export function boundedTaskTitle(value: unknown): string | undefined {
  if (typeof value !== 'string') return undefined
  const text = value.replace(/[\u0000-\u001f\u007f]+/g, ' ').replace(/\s+/g, ' ').trim()
  let title = ''
  let bytes = 0
  let points = 0
  for (const character of text) {
    const point = character.codePointAt(0)!
    const size = point <= 0x7f ? 1 : point <= 0x7ff ? 2 : point <= 0xffff ? 3 : 4
    if (bytes + size > 256 || points >= 100) break
    bytes += size; ++points
    title += character
  }
  return title || undefined
}

export const managedTaskTitle = (run: { taskTitle?: string }): string => run.taskTitle || GENERIC_SUBAGENT_TITLE

/** Only canonical result tools may contribute presentation, and never navigation identity. */
export function managedResultCard(tool: { title?: string; content?: string; subAgentResultContent?: string }) {
  if (!isManagedSubagentTool(tool)) return undefined
  const projected = typeof tool.subAgentResultContent === 'string' && tool.subAgentResultContent.length <= 8192 ? tool.subAgentResultContent : undefined
  const content = projected ?? (typeof tool.content === 'string' && tool.content.length <= 65536 ? tool.content : undefined)
  if (!content) return undefined
  try {
    const result = record(JSON.parse(content))
    if (!validManagedId(result.runId)) return undefined
    const epoch = (value: unknown) => typeof value === 'number' && Number.isSafeInteger(value) && value >= 0 && value <= 8640000000000000 ? value : undefined
    const start = epoch(result.startedAtEpochMs)
    const finish = epoch(result.finishedAtEpochMs)
    const startedAt = string(result.startedAt).slice(0, 128)
    const timing = start !== undefined || startedAt ? {
      status: string(result.status), startedAtEpochMs: start ?? 0, finishedAtEpochMs: finish ?? 0,
      startedAt, completedAt: string(result.completedAt).slice(0, 128),
    } : undefined
    return { runId: result.runId, providerId: string(result.providerId).slice(0, 160), agentId: string(result.agentId).slice(0, 160), status: string(result.status), summary: boundedSubagentSummary(result.result), timing }
  } catch { return undefined }
}

/** Only the newest stored message is hashed; prior transcript bodies are never serialized per poll. */
export function managedTranscriptFingerprint(messages: unknown[]): string {
  const last = messages.length ? JSON.stringify(messages[messages.length - 1]) || '' : ''
  let hash = 2166136261
  for (let index = 0; index < last.length; ++index) hash = Math.imul(hash ^ last.charCodeAt(index), 16777619)
  return `${messages.length}:${last.length}:${hash >>> 0}`
}
export function validManagedId(value: unknown): value is string {
  return typeof value === 'string' && value.length > 0 && value.length <= 160 && !value.includes('\0')
}

/** Legacy local timestamps have no offset; do not guess their execution host's timezone. */
export function managedTimestamp(value: string): number {
  return /^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?(?:Z|[+-]\d{2}:\d{2})$/.test(value) ? Date.parse(value) : NaN
}

export function managedElapsed(run: ManagedRunTiming, now: number): string {
  if (run.startedAtEpochMs === 0 && !run.startedAt) return 'Not started'
  const started = run.startedAtEpochMs > 0 ? run.startedAtEpochMs : managedTimestamp(run.startedAt)
  const finished = (run.finishedAtEpochMs ?? 0) > 0 ? run.finishedAtEpochMs! : managedTimestamp(run.completedAt)
  const active = managedRunActive(run.status, run.lastReported)
  const end = active ? now : finished
  if (!Number.isFinite(started) || started <= 0 || !Number.isFinite(end) || end < started) return active ? 'Timing unavailable' : 'Duration not recorded'
  const seconds = Math.floor((end - started) / 1000)
  return seconds < 60 ? `${seconds}s` : seconds < 3600 ? `${Math.floor(seconds / 60)}m ${seconds % 60}s` : `${Math.floor(seconds / 3600)}h ${Math.floor(seconds % 3600 / 60)}m`
}


/** Managed run IDs must never be interpreted as provider-native session IDs. */
export function isManagedSubagentTool(tool: { title?: string }): boolean {
  return tool.title === 'uam_agent_result'
}

export function managedRunActive(status: string, lastReported = false): boolean {
  return !lastReported && ['queued', 'pending', 'starting', 'running', 'waiting', 'waiting_for_input', 'cancelling'].includes(status)
}

export function managedRunLabel(status: string, lastReported = false): string {
  const labels: Record<string, string> = {
    queued: 'Queued', pending: 'Pending', starting: 'Starting', running: 'Running', waiting: 'Waiting',
    waiting_for_input: 'Waiting for input', cancelling: 'Stopping', completed: 'Completed', failed: 'Failed',
    interrupted: 'Interrupted', cancelled: 'Stopped', canceled: 'Stopped', stopped: 'Stopped',
  }
  const label = labels[status] || 'Status unavailable'
  return lastReported ? `Last reported: ${label}` : label
}

const abortError = () => new DOMException('Read cancelled', 'AbortError')

/**
 * At most two read-only ledger requests are in flight at the bridge. A slot belongs to the underlying
 * request and is freed only when that settles, not when a caller aborts or times out, so UI cancellation
 * cannot admit more physical reads. Release hands the slot to the oldest waiter directly (FIFO).
 */
const READ_SLOTS = 2
let readsInFlight = 0
const readWaiters: Array<() => void> = []
function releaseReadSlot() {
  const next = readWaiters.shift()
  if (next) next()
  else --readsInFlight
}
function acquireReadSlot(signal?: AbortSignal): Promise<() => void> {
  const release = (() => { let done = false; return () => { if (!done) { done = true; releaseReadSlot() } } })()
  if (signal?.aborted) return Promise.reject(abortError())
  if (readsInFlight < READ_SLOTS && !readWaiters.length) { ++readsInFlight; return Promise.resolve(release) }
  return new Promise((resolve, reject) => {
    const grant = () => { signal?.removeEventListener('abort', cancel); resolve(release) }
    const cancel = () => { const index = readWaiters.indexOf(grant); if (index >= 0) readWaiters.splice(index, 1); reject(abortError()) }
    readWaiters.push(grant)
    signal?.addEventListener('abort', cancel, { once: true })
  })
}

/** No fallback session selection or browser mock may imply a stored managed transcript. `bounded` is for read-only requests; Stop never queues. */
async function managedRequest<T = unknown>(action: string, payload: unknown, signal?: AbortSignal, bounded = false): Promise<CEFResponse<T>> {
  if (!isCefContext()) throw new Error('Managed subagents are available when connected to UAM.')
  let timer: ReturnType<typeof setTimeout> | undefined
  let abort: (() => void) | undefined
  // Caller abort and the 30s timeout both leave the slot queue; only the underlying transport frees a taken slot.
  const giveUp = new AbortController()
  try {
    if (signal?.aborted) throw abortError()
    const guards = [
      new Promise<never>((_, reject) => { timer = setTimeout(() => { reject(new Error('The request timed out. Checking again automatically.')); giveUp.abort() }, 30000) }),
      new Promise<never>((_, reject) => { abort = () => { reject(abortError()); giveUp.abort() }; signal?.addEventListener('abort', abort, { once: true }) }),
    ]
    let transport: Promise<CEFResponse<T>>
    if (bounded) {
      const release = await Promise.race([acquireReadSlot(giveUp.signal), ...guards])
      // A waiter woken and then aborted or timed out hands its slot straight on instead of sending.
      if (giveUp.signal.aborted) { release(); throw abortError() }
      transport = sendToCEF<T>({ action, payload }, false)
      transport.then(release, release)
    } else transport = sendToCEF<T>({ action, payload }, false)
    const response = await Promise.race([transport, ...guards])
    if (!response.ok) throw new Error(response.error || 'Managed subagent data is unavailable.')
    return response
  } finally { if (timer !== undefined) clearTimeout(timer); if (abort) signal?.removeEventListener('abort', abort) }
}

export async function managedRead<T = unknown>(action: string, payload: unknown, signal?: AbortSignal, bounded = false): Promise<T> {
  const response = await managedRequest<T>(action, payload, signal, bounded)
  if (!response.data) throw new Error('Managed subagent data is unavailable.')
  return response.data
}

export function managedTransientFailure(cause: unknown): boolean {
  // cefBridge currently retains failure text rather than the CEF numeric code.
  return cause instanceof Error && /already (?:loading|pending)|newer .*request|active or changed|changed while .*loading|ownership changed (?:while reading|during (?:the )?read)|query was replaced|listing changed|scope changed/i.test(cause.message)
}

export async function retryManagedRead<T>(operation: () => Promise<T>, signal: AbortSignal): Promise<T> {
  for (let attempt = 0; ; ++attempt) {
    try { return await operation() }
    catch (cause) {
      if (signal.aborted || attempt >= 2 || !managedTransientFailure(cause)) throw cause
      await new Promise<void>((resolve, reject) => {
        const abort = () => { clearTimeout(timer); reject(new DOMException('Read cancelled', 'AbortError')) }
        const timer = setTimeout(() => { signal.removeEventListener('abort', abort); resolve() }, (attempt + 1) * 1000)
        signal.addEventListener('abort', abort, { once: true })
      })
    }
  }
}

export async function cancelManagedSubagent(chatId: string, runId: string, controlScopeId: string): Promise<void> {
  if (![chatId, runId, controlScopeId].every(validManagedId)) throw new Error('The managed subagent listing is out of date. Its status is being read again.')
  // The native refusal text still advises a manual refresh that this UI no longer has.
  const response = await managedRequest<{ runId?: string }>('cancelManagedSubagent', { chatId, runId, controlScopeId }).catch(cause => {
    throw cause instanceof Error && /refresh/i.test(cause.message) ? new Error(cause.message.replace(/;?\s*refresh before stopping this child\.?/i, '. Its status is being read again.')) : cause
  })
  // The bridge retains raw {ok:true,runId} as an envelope, while wrapped transports use data.
  const returnedRunId = response.data?.runId || record(response).runId
  if (returnedRunId !== runId) throw new Error('The stop response did not match this child. Its status is being read again.')
}

export async function listManagedSubagents(chatId: string, runId = '', offset = 0, limit = 20, signal?: AbortSignal): Promise<ManagedSubagentPage> {
  if (!validManagedId(chatId) || (runId && !validManagedId(runId))) throw new Error('Managed subagent identity is unavailable.')
  if (!Number.isSafeInteger(offset) || offset < 0 || offset > 10000 || !Number.isSafeInteger(limit) || limit < 1 || limit > 100) throw new Error('Managed subagent pagination is invalid.')
  const data = record(await managedRead('listManagedSubagents', { chatId, ...(runId ? { runId } : {}), offset, limit }, signal, true))
  if (data.chatId !== chatId || !validManagedId(data.controlScopeId) || !Array.isArray(data.runs) || data.runs.length > limit ||
      data.offset !== offset || !Number.isSafeInteger(data.totalCount) || (data.totalCount as number) < 0 || (data.totalCount as number) > 10000 ||
      (data.runs.length > 0 && offset + data.runs.length > (data.totalCount as number)) ||
      !(data.nextOffset === null || (Number.isSafeInteger(data.nextOffset) && (data.nextOffset as number) > offset && (data.nextOffset as number) < (data.totalCount as number)))) {
    throw new Error('The managed subagent listing changed or is invalid. It will be read again automatically.')
  }
  const seen = new Set<string>()
  const runs = data.runs.map(value => {
    const run = record(value)
    if (!validManagedId(run.runId) || seen.has(run.runId) ||
        (run.transcriptChatId !== '' && !validManagedId(run.transcriptChatId)) ||
        typeof run.canCancel !== 'boolean' || typeof run.status !== 'string' ||
        (Object.prototype.hasOwnProperty.call(run, 'lastReported') && typeof run.lastReported !== 'boolean')) {
      throw new Error('The managed subagent listing is invalid. It will be read again automatically.')
    }
    seen.add(run.runId)
    const followup = run.lastReported === true ? undefined : managedFollowupState(run.followup)
    return {
      runId: run.runId, transcriptChatId: string(run.transcriptChatId), parentRunId: string(run.parentRunId),
      agentId: string(run.agentId), status: run.status, providerId: string(run.providerId), modelId: string(run.modelId),
      host: typeof run.host === 'string' ? run.host : null, createdAt: string(run.createdAt),
      startedAt: string(run.startedAt), completedAt: string(run.completedAt),
      startedAtEpochMs: typeof run.startedAtEpochMs === 'number' && Number.isFinite(run.startedAtEpochMs) ? run.startedAtEpochMs : 0,
      finishedAtEpochMs: typeof run.finishedAtEpochMs === 'number' && Number.isFinite(run.finishedAtEpochMs) ? run.finishedAtEpochMs : 0,
      ...(followup ? { followup } : {}),
      executionCapability: string(run.executionCapability), lastReported: run.lastReported === true, canCancel: run.lastReported !== true && run.canCancel, summary: boundedSubagentSummary(run.summary), taskTitle: boundedTaskTitle(run.taskTitle),
    }
  })
  return { chatId, controlScopeId: data.controlScopeId, runs, offset, nextOffset: data.nextOffset as number | null, totalCount: data.totalCount as number }
}

export type ManagedFollowupState = { canSend: boolean; code: string; currentTurnId: string; currentTurnNumber: number }
export type ManagedFollowupTarget = { chatId: string; runId: string; transcriptChatId: string; controlScopeId: string; state: ManagedFollowupState }
export type ManagedFollowupEnvelope = Readonly<{ chatId: string; runId: string; transcriptChatId: string; afterTurnId: string; clientRequestId: string; text: string; turnNumber: number }>
export type ManagedFollowupDraft = {
  readonly key: string
  text: string
  request?: ManagedFollowupEnvelope
  phase: 'idle' | 'pending' | 'unknown'
  feedback: string
  users: number
  version: number
  invocation?: object
  listeners: Set<() => void>
}

const followupCodes = new Set(['none', 'not_implemented', 'invalid_request', 'legacy_identity_unverified', 'scope_changed', 'busy', 'cancel_pending',
  'permission_denied', 'queue_full', 'receipt_limit', 'transcript_unavailable', 'model_identity_unverified', 'native_resume_unavailable',
  'persistence_failed', 'delivery_unknown', 'idempotency_conflict'])
const followupUuid = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/
export const MANAGED_FOLLOWUP_TEXT_BYTES = 32768
const MAX_FOLLOWUP_DRAFTS = 32
const followupDrafts = new Map<string, ManagedFollowupDraft>()
const followupKey = (chatId: string, runId: string, transcriptChatId: string) => JSON.stringify([chatId, runId, transcriptChatId])

export function managedFollowupState(value: unknown): ManagedFollowupState | undefined {
  const state = record(value)
  if (typeof state.canSend !== 'boolean' || typeof state.code !== 'string' || !followupCodes.has(state.code)
    || typeof state.currentTurnId !== 'string' || !Number.isSafeInteger(state.currentTurnNumber)
    || (state.currentTurnNumber as number) < 0 || (state.currentTurnNumber as number) > 33
    || (state.currentTurnId !== '' && !followupUuid.test(state.currentTurnId))
    || (state.canSend && (state.code !== 'none' || !followupUuid.test(state.currentTurnId) || (state.currentTurnNumber as number) < 1 || (state.currentTurnNumber as number) >= 33))) return undefined
  return { canSend: state.canSend, code: state.code, currentTurnId: state.currentTurnId, currentTurnNumber: state.currentTurnNumber as number }
}

export function managedFollowupTextAllowed(text: string): boolean {
  return text.length <= MANAGED_FOLLOWUP_TEXT_BYTES && Boolean(text.trim()) && !text.includes('\0') && new TextEncoder().encode(text).length <= MANAGED_FOLLOWUP_TEXT_BYTES
}

/** Bounded inert UI drafts only; no execution/configuration/private native authority is retained here. */
export function acquireManagedFollowupDraft(target: ManagedFollowupTarget): ManagedFollowupDraft | undefined {
  const key = followupKey(target.chatId, target.runId, target.transcriptChatId)
  let entry = followupDrafts.get(key)
  if (!entry && !target.state.canSend) return undefined
  if (!entry) {
    if (followupDrafts.size >= MAX_FOLLOWUP_DRAFTS) {
      // Never forget an unresolved UUID to make room for a fresh send. Mounted drafts also retain their owner.
      const retired = [...followupDrafts].find(([, draft]) => draft.users === 0 && draft.phase === 'idle' && !draft.request)
      if (!retired) return undefined
      followupDrafts.delete(retired[0])
    }
    entry = { key, text: '', phase: 'idle', feedback: '', users: 0, version: 0, listeners: new Set() }
    followupDrafts.set(key, entry)
  }
  ++entry.users
  // Insertion order is the bounded least-recently-opened order for settled inactive drafts.
  followupDrafts.delete(key); followupDrafts.set(key, entry)
  return entry
}

export function releaseManagedFollowupDraft(entry: ManagedFollowupDraft): void { entry.users = Math.max(0, entry.users - 1) }
export function notifyManagedFollowupDraft(entry: ManagedFollowupDraft): void { ++entry.version; for (const listener of entry.listeners) listener() }
export function subscribeManagedFollowupDraft(entry: ManagedFollowupDraft, listener: () => void): () => void {
  entry.listeners.add(listener)
  return () => { entry.listeners.delete(listener) }
}

export function newManagedFollowupEnvelope(target: ManagedFollowupTarget, text: string): ManagedFollowupEnvelope | undefined {
  let clientRequestId: string | undefined
  try {
    const crypto = globalThis.crypto
    if (typeof crypto?.randomUUID === 'function') clientRequestId = crypto.randomUUID()
    else if (typeof crypto?.getRandomValues === 'function') {
      const bytes = crypto.getRandomValues(new Uint8Array(16))
      bytes[6] = (bytes[6] & 15) | 64; bytes[8] = (bytes[8] & 63) | 128
      const hex = [...bytes].map(byte => byte.toString(16).padStart(2, '0')).join('')
      clientRequestId = `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`
    }
  } catch { return undefined }
  if (!target.state.canSend || !clientRequestId || !followupUuid.test(clientRequestId) || !managedFollowupTextAllowed(text)) return undefined
  return Object.freeze({ chatId: target.chatId, runId: target.runId, transcriptChatId: target.transcriptChatId,
    afterTurnId: target.state.currentTurnId, clientRequestId, text, turnNumber: target.state.currentTurnNumber + 1 })
}

export function managedFollowupRefusal(code: string): string {
  if (code === 'busy' || code === 'cancel_pending' || code === 'queue_full') return 'This child is busy or stopping. Refresh before sending follow-up work.'
  if (code === 'permission_denied' || code === 'scope_changed') return 'This child or its permissions changed. Refresh before sending follow-up work.'
  if (code === 'receipt_limit') return 'This child has reached its follow-up limit.'
  if (code === 'idempotency_conflict') return 'The recorded request differs. Check this child before sending more work.'
  return 'Follow-up work is unavailable for this child. Refresh to check its status.'
}

type ManagedFollowupOutcome = { accepted: boolean; code: string }
/** One explicit mutation. Never route this through the read retry helper or a browser preview. */
export async function submitManagedFollowup(target: ManagedFollowupTarget, envelope: ManagedFollowupEnvelope): Promise<ManagedFollowupOutcome | undefined> {
  if (!isCefContext() || target.chatId !== envelope.chatId || target.runId !== envelope.runId || target.transcriptChatId !== envelope.transcriptChatId
    || ![target.chatId, target.runId, target.transcriptChatId, target.controlScopeId].every(validManagedId)) return undefined
  let timer: ReturnType<typeof setTimeout> | undefined
  try {
    const response = await Promise.race([
      sendToCEF<unknown>({ action: 'submitManagedSubagentFollowup', payload: { chatId: envelope.chatId, runId: envelope.runId,
        controlScopeId: target.controlScopeId, afterTurnId: envelope.afterTurnId, clientRequestId: envelope.clientRequestId, text: envelope.text } }, false),
      new Promise<undefined>(resolve => { timer = setTimeout(() => resolve(undefined), 30000) }),
    ])
    if (!response?.ok) return undefined
    const body = record(record(response.data).followup)
    if (typeof body.accepted !== 'boolean' || typeof body.replayed !== 'boolean' || typeof body.code !== 'string' || !followupCodes.has(body.code)) return undefined
    if (body.code === 'delivery_unknown') return undefined
    if (!body.accepted) return body.acceptance === null && body.code !== 'none' && !body.replayed ? { accepted: false, code: body.code } : undefined
    const receipt = record(body.acceptance)
    if (body.code !== 'none' || receipt.rootChatId !== envelope.chatId || receipt.runId !== envelope.runId
      || receipt.transcriptChatId !== envelope.transcriptChatId || receipt.clientRequestId !== envelope.clientRequestId
      || typeof receipt.turnId !== 'string' || !followupUuid.test(receipt.turnId) || receipt.turnId === envelope.afterTurnId
      || receipt.turnNumber !== envelope.turnNumber || receipt.statusAtAdmission !== 'queued') return undefined
    return { accepted: true, code: 'none' }
  } catch { return undefined }
  finally { if (timer !== undefined) clearTimeout(timer) }
}
