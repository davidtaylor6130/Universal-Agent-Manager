import type { Attachment, Message } from '../types/message'
import { sanitizeNativeInputId } from '../store/cpp/sanitizers'

/** Historical presentation only; a valid input without an exact positive write fact stays unknown. */
export function hasUnconfirmedNativeInput(message: Message): boolean {
  return message.role === 'user' && Boolean(sanitizeNativeInputId(message.nativeInputId)) && message.inputWriteCompleted !== true
}

/** Presentation-only record of one composer send. It never authorizes delivery, retry, removal or replay; it only keeps
 * the row's position and React identity (`key`) until native state owns the input. */
export type OutgoingAttempt = {
  key: string
  /** Exact store-minted action ID; correlation only, never delivery or idempotency authority. */
  actionCorrelationId?: string
  /** Locked by the first unique exact-origin producer record; never reconstructed from text/index/time. */
  nativeInputId?: string
  /** IDs actually visible when captured cannot become this action's first alias, even if an origin is reused. */
  nativeInputIdsAtCapture?: ReadonlySet<string>
  /** Exact validated managed receipt projection; saved-input association only, never a provider write fact. */
  recordedManagedInput?: { message: Message; managedOriginRunId: string; associated?: boolean }
  nativeIndex: number
  /** Latest exact bytes observed for this input (captured, then the bound native row or receipt). */
  text: string
  attachments: Attachment[]
  createdAt: Date
  /** Receipt/queue ids that existed at capture; they can never represent this attempt. */
  receiptIds: ReadonlySet<string>
  /** pending: request in flight; accepted: admission ACK only (not delivery); rejected: request failed, delivery unknown. */
  phase: 'pending' | 'accepted' | 'rejected'
  /** Set after the first unique exact correlation; later renders follow identity, not bytes. */
  binding?: { kind: 'native'; messageId: string; nativeIndex: number } | { kind: 'receipt'; id: string }
  /** Has been represented by native state; correlated identity loss can still require an unknown display row. */
  materialized?: boolean
  /** The bound row was replaced by unrelated history; the alias is never transferred again. */
  retired?: boolean
}

const EMPTY: readonly OutgoingAttempt[] = []
const attemptsByScope = new Map<string, readonly OutgoingAttempt[]>()
const listeners = new Set<() => void>()
let serial = 0

/** Exact chat identity: a recreated chat with the same ID never inherits another chat's rows. */
export const outgoingScope = (chatId: string, createdAt: Date) => JSON.stringify([chatId, createdAt.getTime()])

export const nextOutgoingKey = () => `outgoing-attempt-${++serial}`

export function readOutgoingAttempts(scope: string): readonly OutgoingAttempt[] {
  return attemptsByScope.get(scope) ?? EMPTY
}

export function subscribeOutgoingAttempts(listener: () => void) {
  listeners.add(listener)
  return () => { listeners.delete(listener) }
}

function write(scope: string, next: readonly OutgoingAttempt[]) {
  if (next.length) attemptsByScope.set(scope, next)
  else attemptsByScope.delete(scope)
  listeners.forEach((listener) => listener())
}

export function addOutgoingAttempt(scope: string, attempt: OutgoingAttempt) {
  write(scope, [...readOutgoingAttempts(scope), attempt])
}

/** Survives ChatView remounts, so ACK/rejection updates land even after the user switched away. */
export function updateOutgoingAttempts(scope: string, patches: ReadonlyMap<string, Partial<OutgoingAttempt>>) {
  const current = readOutgoingAttempts(scope)
  if (!current.some((attempt) => patches.has(attempt.key))) return
  write(scope, current.map((attempt) => patches.has(attempt.key) ? { ...attempt, ...patches.get(attempt.key) } : attempt))
}

// Unknown inputs are never pruned to bound memory; records live for this app session only and are not durable.

/** Test isolation only: module state otherwise lives for the app session. */
export function resetOutgoingPresentation() {
  attemptsByScope.clear()
  listeners.forEach((listener) => listener())
}
