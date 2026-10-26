import { useCallback, useEffect, useId, useLayoutEffect, useRef, useState } from 'react'
import { LoaderCircle, X } from 'lucide-react'
import type { ContextReference } from '../../types/contextReference'
import { readChatComposerDraft, subscribeChatComposerDraft, writeChatComposerDraft } from '../../utils/composerDraftStorage'
import { CONTEXT_REFERENCE_MAX_COUNT } from '../../utils/contextReferences'
import { captureWorkspaceFileReference } from '../../utils/fileContextReferences'
import { captureRepositoryContextReference, type RepositoryReferenceKind } from '../../utils/repositoryContextReferences'
import { getTopmostModal } from '../../utils/modalFocus'
import { useAppStore } from '../../store/useAppStore'
import { IconButton, Overlay } from '../ui'
import { ContextChip } from './ContextChip'

function capturedTime(value: string) {
  const time = new Date(value)
  return Number.isFinite(time.getTime()) ? time.toLocaleString(undefined, { dateStyle: 'medium', timeStyle: 'short' }) : value
}

// Candidate visibility only; native services still resolve and authorize the actual read.
const mentionSpelling = (path: string) => path.replace(/\\/g, '/').replace(/^(?:\.\/)+/, '')

function SnapshotDialog({ snapshot, label, onClose }: { snapshot: ContextReference; label: string; onClose: () => void }) {
  const dialogRef = useRef<HTMLElement>(null)
  useEffect(() => {
    const previous = document.activeElement as HTMLElement | null
    const dialog = dialogRef.current
    const backdrop = dialog?.closest('.uam-overlay')
    const preventBackdropFocus = (event: Event) => { if (event.target === backdrop) event.preventDefault() }
    backdrop?.addEventListener('mousedown', preventBackdropFocus)
    dialog?.focus()
    const escape = (event: KeyboardEvent) => {
      if (event.key === 'Escape' && !event.defaultPrevented && getTopmostModal() === dialog) {
        event.preventDefault(); onClose()
      }
    }
    window.addEventListener('keydown', escape)
    return () => {
      window.removeEventListener('keydown', escape)
      backdrop?.removeEventListener('mousedown', preventBackdropFocus)
      if (previous?.isConnected && (document.activeElement === document.body || dialog?.contains(document.activeElement))) previous.focus()
    }
  }, [onClose])
  return <Overlay open zIndex={1100} onBackdropMouseDown={onClose}>
    <section ref={dialogRef} role="dialog" aria-modal="true" aria-label={`Captured ${label}`} tabIndex={-1}
      className="uam-rv-dialog flex max-h-[85vh] w-full max-w-5xl flex-col">
      <header className="uam-rv-dialog__header">
        <div className="min-w-0 flex-1">
          <div className="break-all text-sm font-semibold">{snapshot.sourcePath ?? snapshot.worktreeDirectory ?? snapshot.branchName ?? snapshot.label}</div>
          <div className="break-all text-xs" style={{ color: 'var(--text-2)' }}>Host: {snapshot.executionHostId} · Workspace: {snapshot.workspaceDirectory}</div>
          <div className="break-all text-xs" style={{ color: 'var(--text-2)' }}>Captured: <time dateTime={snapshot.capturedAt} title={snapshot.capturedAt}>{capturedTime(snapshot.capturedAt)}</time></div>
          <div className="break-all text-xs" style={{ color: 'var(--text-2)' }}>Source chat: {snapshot.sourceChatId}{snapshot.sourceChatCreatedAt && <> · Created: <time dateTime={snapshot.sourceChatCreatedAt} title={snapshot.sourceChatCreatedAt}>{capturedTime(snapshot.sourceChatCreatedAt)}</time></>}</div>
        </div>
        <IconButton icon={<X size={16} />} label="Close captured context" onClick={onClose} />
      </header>
      <p className="px-3 py-2 text-xs" style={{ color: 'var(--text-2)' }}>
        Captured context for your message.{snapshot.truncated ? ' Limited to the first 16 KiB; this snapshot is incomplete.' : ' The source may have changed since capture.'}
        {snapshot.kind === 'branch' || snapshot.kind === 'worktree' ? ' This records context only; it does not switch branches or workspaces.' : ''}
      </p>
      <pre role="region" aria-label="Captured snapshot text" className="min-h-0 overflow-auto p-3 text-xs" tabIndex={0} style={{ fontFamily: 'var(--font-mono)', whiteSpace: 'pre-wrap', outlineOffset: -3 }}>{snapshot.text || '(Empty file)'}</pre>
    </section>
  </Overlay>
}

type CaptureKey = string | `repository:${RepositoryReferenceKind}`

/**
 * Reference snapshot state for one composer. Captures start from the composer's `@` list in ChatView;
 * the captured snapshots render as chips above the input (ComposerReferenceChips).
 */
export function useComposerReferences({ chatId, ownerKey, disabled, onBusyChange }: {
  chatId: string; ownerKey: string; disabled: boolean; onBusyChange: (busy: boolean) => void
}) {
  const [references, setReferences] = useState(() => readChatComposerDraft(chatId).references ?? [])
  const [pending, setPending] = useState<CaptureKey | null>(null)
  const [error, setError] = useState('')
  // Each chat incarnation gets a generation; a capture finishing after a switch is discarded.
  const generation = useRef(0)
  const busy = useRef(false)
  const worktreeConfigured = useAppStore((state) => state.sessions.some((chat) => chat.id === chatId &&
    chat.workspaceIsolationKind?.trim() === 'gitWorktree' && Boolean(chat.workspaceWorktreeDirectory?.trim())))
  // Layout effect: a previous chat's chips must not paint for a frame after a switch.
  useLayoutEffect(() => {
    const owned = ++generation.current
    busy.current = false; setPending(null); setError('')
    setReferences(readChatComposerDraft(chatId).references ?? [])
    const unsubscribe = subscribeChatComposerDraft((id) => {
      if (id === chatId) setReferences(readChatComposerDraft(chatId).references ?? [])
    })
    return () => { if (generation.current === owned) generation.current++; unsubscribe(); onBusyChange(false) }
  }, [chatId, ownerKey, onBusyChange])
  const capture = async (key: CaptureKey, read: () => Promise<ContextReference>) => {
    if (disabled || busy.current) return
    const owned = generation.current
    const current = () => generation.current === owned
    busy.current = true; setPending(key); setError(''); onBusyChange(true)
    try {
      const snapshot = await read()
      if (!current()) return
      const saved = readChatComposerDraft(chatId)
      if ((saved.references?.length ?? 0) >= CONTEXT_REFERENCE_MAX_COUNT) throw new Error('Remove a reference before adding another (maximum four).')
      writeChatComposerDraft(chatId, { ...saved, references: [...(saved.references ?? []), snapshot] })
    } catch (reason) {
      if (current()) setError(reason instanceof Error ? reason.message : 'Could not capture this context.')
    } finally {
      if (current()) { busy.current = false; setPending(null); onBusyChange(false) }
    }
  }
  const snapshots = references.filter((reference) => reference.kind === 'file' || reference.kind === 'branch' || reference.kind === 'worktree')
  const unavailableReason = references.length >= CONTEXT_REFERENCE_MAX_COUNT ? 'Four references selected. Remove one to add another.'
    : pending !== null ? 'Wait for the current capture to finish.' : 'References are unavailable while this message is sending.'
  return {
    chatId, snapshots, unavailableReason, pending, error, worktreeConfigured,
    atCapacity: references.length >= CONTEXT_REFERENCE_MAX_COUNT,
    actionsDisabled: disabled || pending !== null || references.length >= CONTEXT_REFERENCE_MAX_COUNT,
    dismissError: () => setError(''),
    fileCaptured: (path: string) => snapshots.some((reference) => reference.kind === 'file' && reference.sourcePath && mentionSpelling(reference.sourcePath) === mentionSpelling(path)),
    captureFile: (path: string) => void capture(path, () => captureWorkspaceFileReference(chatId, path)),
    captureRepository: (kind: RepositoryReferenceKind) => {
      if (kind === 'worktree' && !worktreeConfigured) return
      void capture(`repository:${kind}`, () => captureRepositoryContextReference(chatId, kind))
    },
  }
}

export type ComposerReferences = ReturnType<typeof useComposerReferences>

function displayLabelsFor(snapshots: ContextReference[]) {
  // Reserve literal labels so a generated suffix cannot impersonate another captured filename.
  const literalLabels = new Set(snapshots.map((snapshot) => snapshot.label))
  const usedLabels = new Set<string>()
  // Number duplicates 1..n by occurrence; the number also appears in the non-truncating meta text.
  const occurrence = new Map<string, number>()
  const numbers = snapshots.map((snapshot) => {
    if (snapshots.filter((candidate) => candidate.label === snapshot.label).length < 2) return 0
    const next = (occurrence.get(snapshot.label) ?? 0) + 1
    occurrence.set(snapshot.label, next)
    return next
  })
  const labels = snapshots.map((snapshot, index) => {
    let label = snapshot.label
    if (numbers[index]) {
      let position = numbers[index]
      do { label = `${snapshot.label} (${position})` } while ((literalLabels.has(label) || usedLabels.has(label)) && ++position)
      numbers[index] = position // Meta shows the suffix actually chosen.
    }
    usedLabels.add(label)
    return label
  })
  return { labels, numbers }
}

const PENDING_COPY = (pending: CaptureKey) => pending === 'repository:branch' ? 'Capturing branch…'
  : pending === 'repository:worktree' ? 'Capturing worktree…' : `Reading ${pending}…`

/** Captured snapshots as chips above the input. Renders nothing when there is nothing to show. */
export function ComposerReferenceChips({ references }: { references: ComposerReferences }) {
  const { chatId, snapshots, pending, error } = references
  const [selected, setSelected] = useState<{ snapshot: ContextReference; label: string } | null>(null)
  const rootRef = useRef<HTMLDivElement>(null)
  const removedFocusTarget = useRef<HTMLElement | null | undefined>(undefined)
  const closeSnapshot = useCallback(() => setSelected(null), [])
  const noteId = useId()
  useLayoutEffect(() => {
    if (removedFocusTarget.current === undefined) return
    const target = removedFocusTarget.current
    removedFocusTarget.current = undefined
    if (target?.isConnected) target.focus()
  }, [snapshots.length])
  // The status region stays mounted so screen readers announce capture progress when it starts.
  const status = <span aria-live="polite" data-reference-status="" className="sr-only">{pending ? PENDING_COPY(pending) : ''}</span>
  if (snapshots.length === 0 && !pending && !error && !selected) return status
  const { labels, numbers } = displayLabelsFor(snapshots)
  return <>{status}<div ref={rootRef} role="group" aria-label="Reference context" aria-describedby={snapshots.length ? noteId : undefined} tabIndex={-1} className="px-3 pt-3">
    <span id={noteId} hidden>Snapshots are included with your message when you press Send.</span>
    {(snapshots.length > 0 || pending) && <div className="flex flex-wrap items-center gap-2">
      {snapshots.map((snapshot, index) => {
        const label = labels[index]
        return <ContextChip key={snapshot.id} kind={snapshot.kind === 'branch' ? 'branch' : snapshot.kind === 'worktree' ? 'worktree' : 'file'} name={label} partial={snapshot.truncated}
        meta={`${snapshot.truncated ? 'partial ' : ''}${snapshot.kind === 'file' ? '' : snapshot.kind + ' '}snapshot${numbers[index] ? ` · ${numbers[index]}` : ''}`}
        detail={`Included when you press Send\nSource: ${snapshot.sourcePath ?? snapshot.worktreeDirectory ?? snapshot.branchName}\nHost: ${snapshot.executionHostId}\nWorkspace: ${snapshot.workspaceDirectory}\nCaptured: ${capturedTime(snapshot.capturedAt)}\nSource chat: ${snapshot.sourceChatId}${snapshot.sourceChatCreatedAt ? '\nCreated: ' + capturedTime(snapshot.sourceChatCreatedAt) : ''}`}
        onInspect={() => setSelected({ snapshot, label })} onRemove={() => {
          const chips = rootRef.current?.querySelectorAll<HTMLElement>('.uam-context-chip__body')
          // Falls back to the composer input, where @ offers more references (this group unmounts when empty).
          removedFocusTarget.current = chips?.[index + 1] ?? chips?.[index - 1] ?? rootRef.current?.closest('.uam-composer-surface')?.querySelector<HTMLElement>('textarea') ?? null
          const current = readChatComposerDraft(chatId)
          writeChatComposerDraft(chatId, { ...current, references: (current.references ?? []).filter((reference) => reference.id !== snapshot.id) })
        }} removeLabel={`Remove ${label} reference`} />
      })}
      {pending && <span aria-hidden className="inline-flex h-6 min-w-0 max-w-[260px] items-center gap-1.5 px-2 text-xs" style={{ color: 'var(--text-2)' }}>
        <LoaderCircle size={12} className="uam-reference-pending__spinner" /><span className="truncate">{PENDING_COPY(pending)}</span>
      </span>}
    </div>}
    {error && <div role="alert" className="flex items-center gap-2 pt-2 text-xs" style={{ color: 'var(--error)' }}>
      <span className="min-w-0 flex-1">{error}</span>
      <IconButton icon={<X size={12} />} label="Dismiss reference error" size="sm" onClick={references.dismissError} />
    </div>}
    {selected && <SnapshotDialog snapshot={selected.snapshot} label={selected.label} onClose={closeSnapshot} />}
  </div></>
}
