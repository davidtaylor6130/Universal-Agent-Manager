import type { ReactNode } from 'react'
import { ConfirmDialog } from '../ui'

export type DeleteItemKind = 'chat' | 'workspace' | 'collection'

export function countLabel(count: number, noun: string, plural = `${noun}s`): string {
  return `${count} ${count === 1 ? noun : plural}`
}

/** Contents wording for chat deletes, shared so one chat reads the same from its menu and as a one-item selection. */
export function relatedBranchContents(chatCount: number, familyCount: number): string | undefined {
  if (familyCount <= chatCount) return undefined
  return `${chatCount === 1 ? 'its ' : ''}${countLabel(familyCount - chatCount, 'related branch', 'related branches')}`
}

/**
 * The sidebar delete confirmation for chats, workspaces and collections, single or multi-selected.
 * Thin wrapper over ConfirmDialog: states item type, count and contents in one sentence; callers add consequences as children.
 */
export function DeleteConfirmDialog({ open, kind, count, name, contents, title, busy = false, error, onConfirm, onCancel, returnFocusTo, children }: {
  open: boolean
  kind: DeleteItemKind
  count: number
  /** Item name, shown when a single item is deleted. */
  name?: string
  /** What is deleted with the items, e.g. "3 chats" or "its 1 related branch". */
  contents?: string
  /** Replaces the generated title. */
  title?: string
  busy?: boolean
  error?: string
  onConfirm: () => void
  onCancel: () => void
  /** Row to focus on close when the opening control has gone. */
  returnFocusTo?: () => HTMLElement | null | undefined
  children?: ReactNode
}) {
  const single = count === 1 && Boolean(name)
  const noun = count === 1 ? kind : `${kind}s`
  const withContents = contents ? ` and ${contents}` : ''
  return (
    <ConfirmDialog
      open={open}
      title={title ?? (count === 1 ? `Delete ${kind}?` : `Delete ${count} selected ${noun}?`)}
      label={single ? `Delete ${kind} ${name}${withContents}` : `Delete ${count} selected ${noun}`}
      confirmLabel={`Delete ${noun}`}
      busy={busy}
      error={error}
      onConfirm={onConfirm}
      onCancel={onCancel}
      returnFocusTo={returnFocusTo}
    >
      <p>{single ? `The ${kind} “${name}”` : countLabel(count, kind)}{withContents} will be permanently deleted. This cannot be undone.</p>
      {children}
      <p role="status" aria-live="polite" className={busy ? 'mt-2' : undefined}>{busy ? `Deleting ${countLabel(count, kind)}${withContents}…` : ''}</p>
    </ConfirmDialog>
  )
}
