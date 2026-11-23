import { createRequestId } from '../ipc/cefBridge'

/** App-wide failures and notices. AppShell routes them to the notifications panel and the shared transient alert. */
export const APP_FAILURE_EVENT = 'uam-app-failure'
export const APP_NOTICE_RESOLVED_EVENT = 'uam-app-notice-resolved'
export type AppNoticeAction = { label: string; run: () => void | Promise<void> }
/** Absent tone means a failure; info and success notices reuse the same channel. */
export type AppFailure = { id: string; time: string; message: string; detail: string; tone?: 'info' | 'success' | 'warning'; actions?: AppNoticeAction[] }

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

/** Raw transport failures carry exception text; they are logged by the bridge and never shown as a notice message. */
export function isRawBridgeError(text: string | undefined): boolean {
  return Boolean(text && /^(Bridge request failed:|Invalid bridge payload:)/.test(text))
}

/** A notice's detail from a caught value or backend error, followed by an optional next step; the message stays fixed wording. */
export function failureDetail(cause: unknown, hint = ''): string {
  const text = cause instanceof Error ? cause.message : typeof cause === 'string' ? cause : ''
  return [text.trim(), hint.trim()].filter(Boolean).join(' ')
}

export function reportAppFailure(message: string, detail: string) {
  window.dispatchEvent(new CustomEvent<AppFailure>(APP_FAILURE_EVENT, {
    detail: { id: createRequestId('app-failure'), time: new Date().toISOString(), ...noticeText(message, detail) },
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
