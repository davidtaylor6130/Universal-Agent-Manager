import { createRequestId } from '../ipc/cefBridge'

/** Failures of app-wide actions. AppShell routes them to the notifications panel and the shared transient alert. */
export const APP_FAILURE_EVENT = 'uam-app-failure'
export type AppFailure = { id: string; time: string; message: string; detail: string }

export function reportAppFailure(message: string, detail: string) {
  window.dispatchEvent(new CustomEvent<AppFailure>(APP_FAILURE_EVENT, {
    detail: { id: createRequestId('app-failure'), time: new Date().toISOString(), message, detail },
  }))
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
