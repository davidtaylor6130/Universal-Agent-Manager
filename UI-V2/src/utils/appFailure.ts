import { createRequestId } from '../ipc/cefBridge'

/** App-wide failures and notices. AppShell routes them to the notifications panel and the shared transient alert. */
export const APP_FAILURE_EVENT = 'uam-app-failure'
export const APP_NOTICE_RESOLVED_EVENT = 'uam-app-notice-resolved'
export type AppNoticeAction = { label: string; run: () => void | Promise<void> }
/** Absent tone means a failure; info and success notices reuse the same channel. A failure's `title` names it in the
 * notifications panel; without one the panel uses the generic "Change not saved". */
export type AppFailure = { id: string; time: string; message: string; detail: string; tone?: 'info' | 'success' | 'warning'; actions?: AppNoticeAction[]; title?: string }

export function reportAppFailure(message: string, detail: string, options: { title?: string } = {}) {
  window.dispatchEvent(new CustomEvent<AppFailure>(APP_FAILURE_EVENT, {
    detail: { id: createRequestId('app-failure'), time: new Date().toISOString(), message, detail, ...(options.title ? { title: options.title } : {}) },
  }))
}

/** Completed or informational app-wide messages; same panel and alert as failures. */
export function reportAppNotice(message: string, tone: 'info' | 'success' | 'warning' = 'info', options: { id?: string; detail?: string; actions?: AppNoticeAction[] } = {}) {
  const id = options.id ?? createRequestId('app-notice')
  window.dispatchEvent(new CustomEvent<AppFailure>(APP_FAILURE_EVENT, {
    detail: { id, time: new Date().toISOString(), message, detail: options.detail ?? '', tone, actions: options.actions },
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
