import { createRequestId } from '../ipc/cefBridge'

/** Failures of app-wide actions. AppShell routes them to the notifications panel and the shared transient alert. */
export const APP_FAILURE_EVENT = 'uam-app-failure'
export type AppFailure = { id: string; time: string; message: string; detail: string }

export function reportAppFailure(message: string, detail: string) {
  window.dispatchEvent(new CustomEvent<AppFailure>(APP_FAILURE_EVENT, {
    detail: { id: createRequestId('app-failure'), time: new Date().toISOString(), message, detail },
  }))
}
