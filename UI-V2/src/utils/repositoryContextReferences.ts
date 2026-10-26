import { isCefContext, sendToCEF } from '../ipc/cefBridge'
import type { ContextReference } from '../types/contextReference'
import { captureContextReference } from './contextReferences'
import { contextReferenceOwner } from './contextReferenceOwner'

export type RepositoryReferenceKind = 'branch' | 'worktree'
const bounded = (value: unknown, max: number, empty = false): value is string =>
  typeof value === 'string' && (empty || Boolean(value)) && value.length <= max && !/[\x00-\x1f\x7f]/.test(value)

/** Capture only the live native chat's observed context. No displayed branch/path is sent as a selector. */
export async function captureRepositoryContextReference(chatId: string, kind: RepositoryReferenceKind): Promise<ContextReference> {
  const owner = contextReferenceOwner(chatId)
  if (!owner || !isCefContext()) throw new Error('Reference repository context from an editable desktop chat.')
  const response = await sendToCEF<Record<string, unknown>>({ action: 'captureChatReferenceContext', payload: { chatId, kind } }, false)
  if (!response.ok) throw new Error(response.error || 'Could not capture repository context.')
  const data = response.data
  if (contextReferenceOwner(chatId)?.key !== owner.key || !data || data.kind !== kind || data.chatId !== chatId ||
    !bounded(data.chatCreatedAt, 64) || Date.parse(data.chatCreatedAt) !== owner.birth || data.executionHostId !== owner.host) {
    throw new Error('The chat, workspace or host changed. Capture this reference again.')
  }
  if (!bounded(data.workspaceDirectory, 4096) || !bounded(data.branchName, 1024, kind === 'worktree') ||
    !bounded(data.worktreeDirectory, 4096, kind === 'branch') || !bounded(data.sourceDirectory, 4096, true) ||
    !bounded(data.baseRef, 1024, true) || !bounded(data.capturedAt, 64) || !Number.isFinite(Date.parse(data.capturedAt)) ||
    (kind === 'worktree' && data.worktreeDirectory !== data.workspaceDirectory)) {
    throw new Error('The native repository context is incomplete or exceeds the snapshot limits.')
  }
  const snapshot = captureContextReference({
    kind, sourceChatId: chatId, sourceChatCreatedAt: data.chatCreatedAt, executionHostId: owner.host,
    workspaceDirectory: data.workspaceDirectory, capturedAt: data.capturedAt,
    branchName: data.branchName, ...(kind === 'worktree' ? { worktreeDirectory: data.worktreeDirectory } : {}),
    sourceDirectory: data.sourceDirectory, baseRef: data.baseRef,
    label: (kind === 'branch' ? data.branchName : data.worktreeDirectory.split(/[\\/]/).filter(Boolean).pop() || 'worktree').slice(0, 160),
    text: JSON.stringify({ observation: kind === 'branch' ? 'Current named Git branch at capture' : 'Chat-configured Git worktree at capture',
      branchName: data.branchName, ...(kind === 'worktree' ? { worktreeDirectory: data.worktreeDirectory } : {}), sourceDirectory: data.sourceDirectory, baseRef: data.baseRef }, null, 2),
  })
  if (!snapshot) throw new Error('This repository context cannot be retained as a snapshot.')
  return snapshot
}
