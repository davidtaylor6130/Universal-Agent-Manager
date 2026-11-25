import { createRequestId } from '../ipc/cefBridge'

/** App-wide failures and notices. AppShell routes them to the notifications panel and the shared transient alert. */
export const APP_FAILURE_EVENT = 'uam-app-failure'
export const APP_NOTICE_RESOLVED_EVENT = 'uam-app-notice-resolved'
export type AppNoticeAction = { label: string; run: () => void | Promise<void> }
/** Absent tone means a failure; info and success notices reuse the same channel. A failure's `title` names it in the
 * notifications panel; without one the panel uses its message. */
export type AppFailure = { id: string; time: string; message: string; detail: string; tone?: 'info' | 'success' | 'warning'; actions?: AppNoticeAction[]; title?: string }

/**
 * A notice's message is one short line read at a glance; anything after its first line (a stack, a log excerpt or
 * numbered steps) moves to the detail, which the bell row expands and Copy message includes.
 */
export function noticeText(message: string, detail = ''): { message: string; detail: string } {
  const lines = message.split(/\r?\n/).map((line) => line.trim()).filter(Boolean)
  const rest = lines.slice(1).join(' ')
  const line = (lines[0] ?? '').replace(/\s+/g, ' ')
  // A cause that only repeats the message adds nothing to the detail.
  return { message: line, detail: [rest, detail.trim()].filter((part) => part && part !== line).join(' · ') }
}

/** Raw transport failures carry exception text; it goes to the diagnostics log, never into a notice. */
export function isRawBridgeError(text: string | undefined): boolean {
  return Boolean(text && /^(Bridge request failed[:.]|Invalid bridge payload:)/.test(text))
}

/**
 * A notice's detail from a caught value or backend error, followed by an optional next step; the message stays fixed
 * wording. Raw bridge exception text is logged (console errors reach the diagnostics log) and left out of the notice.
 */
export function failureDetail(cause: unknown, hint = ''): string {
  const raw = (cause instanceof Error ? cause.message : typeof cause === 'string' ? cause : '').trim()
  if (isRawBridgeError(raw)) console.error(`[notice] ${raw}`)
  const text = isRawBridgeError(raw) ? '' : raw && !/[.!?]$/.test(raw) && hint.trim() ? `${raw}.` : raw
  return [text, hint.trim()].filter(Boolean).join(' ')
}

/** Splits fixed multi-sentence wording into its first sentence (the message) and the rest (the detail). */
export function firstSentence(text: string): { message: string; detail: string } {
  const match = /^(.+?[.!?])\s+(?=[A-Z])(.+)$/s.exec(text.trim())
  return match ? { message: match[1], detail: match[2] } : { message: text.trim(), detail: '' }
}

export function reportAppFailure(message: string, detail: string, options: { title?: string } = {}) {
  window.dispatchEvent(new CustomEvent<AppFailure>(APP_FAILURE_EVENT, {
    detail: { id: createRequestId('app-failure'), time: new Date().toISOString(), ...noticeText(message, detail), ...(options.title ? { title: options.title } : {}) },
  }))
}

/** Completed or informational app-wide messages; same panel and alert as failures. */
export function reportAppNotice(message: string, tone: 'info' | 'success' | 'warning' = 'info', options: { id?: string; detail?: string; actions?: AppNoticeAction[] } = {}) {
  const id = options.id ?? createRequestId('app-notice')
  window.dispatchEvent(new CustomEvent<AppFailure>(APP_FAILURE_EVENT, {
    detail: { id, time: new Date().toISOString(), ...noticeText(message, options.detail), tone, actions: options.actions },
  }))
  return id
}

/** Ends a retained source notice, including its transient alert. */
export function resolveAppNotice(id: string) {
  window.dispatchEvent(new CustomEvent(APP_NOTICE_RESOLVED_EVENT, { detail: id }))
}

export type DeletionRejection = { id: string; error: string }

/** Reports each item a partial multi-selection deletion refused, by name; returns whether any item was refused. */
export function reportDeletionRejections(rejected: unknown, nameOf: (id: string) => string | undefined, noun: 'chats' | 'workspaces', requested: number): boolean {
  const items = Array.isArray(rejected)
    ? rejected.filter((item): item is DeletionRejection => typeof item?.id === 'string' && typeof item?.error === 'string')
    : []
  if (items.length === 0) return false
  reportAppFailure(`Could not delete ${items.length} of ${requested} selected ${noun}.`, items.map(({ id, error }) => `“${nameOf(id) || id}”: ${error}`).join(' · '))
  return true
}
