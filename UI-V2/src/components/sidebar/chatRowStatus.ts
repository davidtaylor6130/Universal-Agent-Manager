import type { AcpAttentionKind } from '../../store/useAppStore'
import type { DisplayedChatStatus } from './chatSearch'

/** Short label for each attention kind: a sidebar row's status icon title and part of its spoken status. */
export const ATTENTION_LABELS: Record<AcpAttentionKind, string> = {
  question: 'Needs answer',
  plan: 'Plan needs review',
  memory: 'Memory input needed',
  permission: 'Permission needed',
  command: 'Command approval needed',
  file: 'File approval needed',
  error: 'Needs attention',
  generic: 'Input needed',
}

/**
 * Spoken status for a chat row's accessible name and the sidebar's status announcement, so the
 * state shown by icon and colour is also in words. Empty when the row shows no status.
 */
export function chatRowStatusText(status: DisplayedChatStatus, options: { automationHeld?: boolean; watchingPullRequest?: boolean } = {}): string {
  if (options.automationHeld) return 'waiting for approval'
  if (status?.type === 'processing') return 'running'
  if (status?.type === 'attention') return status.kind === 'error' ? 'failed' : `waiting, ${ATTENTION_LABELS[status.kind].toLowerCase()}`
  if (options.watchingPullRequest) return 'watching pull request'
  if (status?.type === 'done') return 'done, unread'
  return ''
}
