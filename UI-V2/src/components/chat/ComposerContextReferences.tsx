import { useCallback, useEffect, useId, useLayoutEffect, useRef, useState } from 'react'
import { X } from 'lucide-react'
import type { ContextReference } from '../../types/contextReference'
import { readChatComposerDraft, subscribeChatComposerDraft, writeChatComposerDraft } from '../../utils/composerDraftStorage'
import { CONTEXT_REFERENCE_MAX_COUNT } from '../../utils/contextReferences'
import { captureWorkspaceFileReference, draftFileMentions } from '../../utils/fileContextReferences'
import { captureRepositoryContextReference, type RepositoryReferenceKind } from '../../utils/repositoryContextReferences'
import { getTopmostModal } from '../../utils/modalFocus'
import { useAppStore } from '../../store/useAppStore'
import { Button, IconButton, Overlay, Tooltip } from '../ui'
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

export function ComposerContextReferences({ chatId, draft, disabled, onBusyChange }: {
  chatId: string; draft: string; disabled: boolean; onBusyChange: (busy: boolean) => void
}) {
  const [references, setReferences] = useState(() => readChatComposerDraft(chatId).references ?? [])
  const [pending, setPending] = useState<string | null>(null)
  const [error, setError] = useState('')
  const [selected, setSelected] = useState<{ snapshot: ContextReference; label: string } | null>(null)
  const mounted = useRef(false)
  const busy = useRef(false)
  const rootRef = useRef<HTMLDivElement>(null)
  const removedFocusTarget = useRef<HTMLElement | null | undefined>(undefined)
  const worktreeConfigured = useAppStore((state) => state.sessions.some((chat) => chat.id === chatId &&
    chat.workspaceIsolationKind?.trim() === 'gitWorktree' && Boolean(chat.workspaceWorktreeDirectory?.trim())))
  const worktreeHintId = useId()
  const closeSnapshot = useCallback(() => setSelected(null), [])
  useLayoutEffect(() => {
    if (removedFocusTarget.current === undefined) return
    const target = removedFocusTarget.current
    removedFocusTarget.current = undefined
    if (target?.isConnected) target.focus()
    else (rootRef.current?.querySelector<HTMLElement>('[data-reference-action]:not(:disabled):not([aria-disabled="true"])') ?? rootRef.current)?.focus()
  }, [references])
  useEffect(() => {
    mounted.current = true
    const unsubscribe = subscribeChatComposerDraft((id) => {
      if (id === chatId) setReferences(readChatComposerDraft(chatId).references ?? [])
    })
    return () => { mounted.current = false; unsubscribe(); onBusyChange(false) }
  }, [chatId, onBusyChange])
  const capture = async (key: string, read: () => Promise<ContextReference>) => {
    if (disabled || busy.current) return
    busy.current = true; setPending(key); setError(''); onBusyChange(true)
    try {
      const snapshot = await read()
      if (!mounted.current) return
      const current = readChatComposerDraft(chatId)
      if ((current.references?.length ?? 0) >= CONTEXT_REFERENCE_MAX_COUNT) throw new Error('Remove a reference before adding another (maximum four).')
      writeChatComposerDraft(chatId, { ...current, references: [...(current.references ?? []), snapshot] })
    } catch (reason) {
      if (mounted.current) setError(reason instanceof Error ? reason.message : 'Could not capture this context.')
    } finally {
      busy.current = false
      if (mounted.current) { setPending(null); onBusyChange(false) }
    }
  }
  const snapshots = references.filter((reference) => reference.kind === 'file' || reference.kind === 'branch' || reference.kind === 'worktree')
  // Reserve literal labels so a generated suffix cannot impersonate another captured filename.
  const literalLabels = new Set(snapshots.map((snapshot) => snapshot.label))
  const usedLabels = new Set<string>()
  // Number duplicates 1..n by occurrence; the number also appears in the non-truncating meta text.
  const occurrence = new Map<string, number>()
  const duplicateNumbers = snapshots.map((snapshot) => {
    if (snapshots.filter((candidate) => candidate.label === snapshot.label).length < 2) return 0
    const next = (occurrence.get(snapshot.label) ?? 0) + 1
    occurrence.set(snapshot.label, next)
    return next
  })
  const displayLabels = snapshots.map((snapshot, index) => {
    let label = snapshot.label
    if (duplicateNumbers[index]) {
      let position = duplicateNumbers[index]
      do { label = `${snapshot.label} (${position})` } while ((literalLabels.has(label) || usedLabels.has(label)) && ++position)
      duplicateNumbers[index] = position // Meta shows the suffix actually chosen.
    }
    usedLabels.add(label)
    return label
  })
  const candidates = draftFileMentions(draft).filter((path) => !snapshots.some((reference) => reference.kind === 'file' && reference.sourcePath && mentionSpelling(reference.sourcePath) === mentionSpelling(path)))
  return <div ref={rootRef} role="group" aria-label="Reference context" tabIndex={-1} className="px-3 pt-3">
    <div className="flex flex-wrap gap-2">
      {snapshots.map((snapshot, index) => {
        const label = displayLabels[index]
        return <ContextChip key={snapshot.id} kind={snapshot.kind === 'branch' ? 'branch' : snapshot.kind === 'worktree' ? 'worktree' : 'file'} name={label} partial={snapshot.truncated}
        meta={`${snapshot.truncated ? 'partial ' : ''}${snapshot.kind === 'file' ? '' : snapshot.kind + ' '}snapshot${duplicateNumbers[index] ? ` · ${duplicateNumbers[index]}` : ''}`}
        detail={`Source: ${snapshot.sourcePath ?? snapshot.worktreeDirectory ?? snapshot.branchName}\nHost: ${snapshot.executionHostId}\nWorkspace: ${snapshot.workspaceDirectory}\nCaptured: ${capturedTime(snapshot.capturedAt)}\nSource chat: ${snapshot.sourceChatId}${snapshot.sourceChatCreatedAt ? '\nCreated: ' + capturedTime(snapshot.sourceChatCreatedAt) : ''}`}
        onInspect={() => setSelected({ snapshot, label })} onRemove={() => {
          const chips = rootRef.current?.querySelectorAll<HTMLElement>('.uam-context-chip__body')
          removedFocusTarget.current = chips?.[index + 1] ?? chips?.[index - 1] ?? null
          const current = readChatComposerDraft(chatId)
          writeChatComposerDraft(chatId, { ...current, references: (current.references ?? []).filter((reference) => reference.id !== snapshot.id) })
        }} removeLabel={`Remove ${label} reference`} />
      })}
      {candidates.map((path) => <Tooltip key={path} label={<span style={{ fontFamily: 'var(--font-mono)' }}>{path}</span>}>
        <Button data-reference-action type="button" size="sm" variant="ghost" className="min-w-0 max-w-full" loading={pending === path} disabled={disabled || pending !== null || references.length >= CONTEXT_REFERENCE_MAX_COUNT}
          onClick={() => void capture(path, () => captureWorkspaceFileReference(chatId, path))}><span className="max-w-[260px] truncate">{pending === path ? 'Reading file…' : `Reference @${path}`}</span></Button>
      </Tooltip>)}
      {(['branch', 'worktree'] as RepositoryReferenceKind[]).map((kind) => <Tooltip key={kind} label={kind === 'worktree' && !worktreeConfigured ? 'Worktree references require a configured worktree for this chat.' : `Capture the current ${kind} as quoted context; this does not change the workspace.`}>
        <Button data-reference-action type="button" size="sm" variant="ghost" loading={pending === `repository:${kind}`} disabled={disabled || pending !== null || references.length >= CONTEXT_REFERENCE_MAX_COUNT}
          // Focusable when only unconfigured, so keyboard and screen-reader users can reach the reason. Spread only
          // then: an explicit undefined aria-describedby would replace the tooltip's description through Radix Slot.
          {...(kind === 'worktree' && !worktreeConfigured ? { 'aria-disabled': true, 'aria-describedby': worktreeHintId } : {})}
          onClick={() => { if (kind !== 'worktree' || worktreeConfigured) void capture(`repository:${kind}`, () => captureRepositoryContextReference(chatId, kind)) }}>Reference {kind}</Button>
      </Tooltip>)}
    </div>
    {!worktreeConfigured && <span id={worktreeHintId} hidden>Worktree references require a configured worktree for this chat.</span>}
    {references.length >= CONTEXT_REFERENCE_MAX_COUNT && <p className="pt-2 text-xs" style={{ color: 'var(--text-2)' }}>Four references selected. Remove one to add another.</p>}
    {snapshots.length > 0 && <p className="pt-2 text-xs" style={{ color: 'var(--text-2)' }}>Snapshots are included with your message when you press Send.</p>}
    {error && <p role="alert" className="pt-2 text-xs" style={{ color: 'var(--error)' }}>{error}</p>}
    {selected && <SnapshotDialog snapshot={selected.snapshot} label={selected.label} onClose={closeSnapshot} />}
  </div>
}
