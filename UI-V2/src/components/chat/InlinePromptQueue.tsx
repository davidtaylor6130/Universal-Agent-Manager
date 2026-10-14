import { useLayoutEffect, useRef, useState, type ReactNode } from 'react'
import { ArrowDown, ArrowUp, CornerUpRight, GripVertical, Pencil, X } from 'lucide-react'
import type { AcpQueuedPrompt } from '../../store/cpp/types'
import type { Attachment } from '../../types/message'
import { Button, IconButton, StatusDot, motionAllowed } from '../ui'
import { EXIT_MS } from '../ui/Overlay'
import './InlinePromptQueue.css'

export interface UnsentPrompt {
  messageIndex: number
  text: string
  attachments: readonly Attachment[]
  retryable: boolean
}

interface InlinePromptQueueProps {
  prompts: readonly AcpQueuedPrompt[]
  unsentPrompts?: readonly UnsentPrompt[]
  unsentActionsDisabled?: boolean
  onRetryUnsent?: (messageIndex: number) => Promise<boolean>
  onEditUnsent?: (messageIndex: number, text: string) => void
  held?: boolean
  onResume?: (expected: { id: string; revision: number }[]) => Promise<boolean>
  disabled?: boolean
  onEdit: (id: string, revision: number, text: string, retainedAttachmentIds?: string[]) => Promise<boolean>
  onReorder: (expected: { id: string; revision: number }[], orderedIds: string[]) => Promise<boolean>
  onSteer: (index: number, expected?: { id: string; revision: number }) => Promise<boolean>
  onRemove: (index: number, expected?: { id: string; revision: number }) => Promise<boolean>
}

/** Compact file pill used for attachments and Skills context on queued prompts. */
function FileChip({ name, title, trailing }: { name: string; title?: string; trailing?: ReactNode }) {
  return <span className="uam-queue-chip" title={title}>
    <span className="uam-queue-chip__name">{name}</span>
    {trailing}
  </span>
}

/** Removed rows stay visible for the exit animation; reduced motion or no matchMedia (jsdom) drops them at once. */

/** Edits queue snapshots separately from the composer; native ID/revision guards own admission. */
export function InlinePromptQueue({ prompts, unsentPrompts = [], unsentActionsDisabled = false, onRetryUnsent, onEditUnsent, held = false, onResume, disabled = false, onEdit, onReorder, onSteer, onRemove }: InlinePromptQueueProps) {
  const [editing, setEditing] = useState<{ id: string; revision: number; text: string; retainedAttachmentIds?: string[] } | null>(null)
  const [pending, setPending] = useState(false)
  const pendingRef = useRef(false)
  const [error, setError] = useState('')
  const blocked = disabled || pending
  const reorderable = prompts.every((prompt) => prompt.id && prompt.revision && !prompt.prepared)
  const [leaving, setLeaving] = useState<{ prompt: AcpQueuedPrompt; index: number }[]>([])
  const previousPrompts = useRef(prompts)
  useLayoutEffect(() => {
    const before = previousPrompts.current
    previousPrompts.current = prompts
    if (!motionAllowed()) return
    const ids = new Set(prompts.map((prompt) => prompt.id))
    const removed = before.flatMap((prompt, index) => prompt.id && !ids.has(prompt.id) ? [{ prompt, index }] : [])
    if (removed.length) setLeaving((current) => [...current.filter((item) => !ids.has(item.prompt.id)), ...removed])
  }, [prompts])
  useLayoutEffect(() => {
    if (leaving.length === 0) return
    const timer = window.setTimeout(() => setLeaving([]), EXIT_MS)
    return () => window.clearTimeout(timer)
  }, [leaving])
  const rows = prompts.map((prompt, index) => ({ prompt, index, leaving: false }))
  for (const item of leaving) rows.splice(Math.min(item.index, rows.length), 0, { ...item, leaving: true })
  const [drag, setDrag] = useState<{ from: number; over: number } | null>(null)
  const canDrag = reorderable && !blocked && !held && !editing
  // FLIP: when the queue order changes (drag, drop or up/down), rows slide from their old offset to the new one.
  const listRef = useRef<HTMLDivElement>(null)
  const layoutRef = useRef<{ order: string; tops: Map<string, number> }>({ order: '', tops: new Map() })
  useLayoutEffect(() => {
    const rowEls = Array.from(listRef.current?.querySelectorAll<HTMLElement>('[data-queue-id]') ?? [])
    const order = rowEls.map((row) => row.dataset.queueId).join('\n')
    const tops = new Map(rowEls.map((row) => [row.dataset.queueId!, row.getBoundingClientRect().top]))
    const previous = layoutRef.current
    if (previous.order && order !== previous.order && motionAllowed() && typeof HTMLElement.prototype.animate === 'function') {
      for (const row of rowEls) {
        const before = previous.tops.get(row.dataset.queueId!)
        const dy = before === undefined ? 0 : before - tops.get(row.dataset.queueId!)!
        if (dy) row.animate([{ transform: `translateY(${dy}px)` }, { transform: 'translateY(0)' }], { duration: 180, easing: 'cubic-bezier(0.22, 1, 0.36, 1)' })
      }
    }
    layoutRef.current = { order, tops }
  })

  async function run(operation: () => Promise<boolean>, failure: string, finish?: () => void) {
    if (blocked || pendingRef.current) return
    pendingRef.current = true
    setPending(true)
    setError('')
    try {
      if (await operation()) finish?.()
      else setError(failure)
    } catch (error) {
      setError(error instanceof Error && error.message.trim() ? error.message : failure)
    } finally {
      pendingRef.current = false
      setPending(false)
    }
  }

  function reorder(from: number, to: number) {
    const expected = prompts.map((prompt) => ({ id: prompt.id!, revision: prompt.revision! }))
    const ordered = expected.map((prompt) => prompt.id)
    const [moved] = ordered.splice(from, 1)
    ordered.splice(to, 0, moved)
    void run(() => onReorder(expected, ordered), 'Queue changed or could not be reordered. Try again.')
  }

  function move(index: number, direction: number) {
    reorder(index, index + direction)
  }

  if (prompts.length === 0 && unsentPrompts.length === 0 && !editing && leaving.length === 0) return null
  const editedPrompt = editing ? prompts.find((prompt) => prompt.id === editing.id) : undefined
  const editedPromptIndex = editedPrompt ? prompts.indexOf(editedPrompt) : -1
  const staleEdit = Boolean(editing && (!editedPrompt || editedPrompt.revision !== editing.revision))
  const markdownStoreFiles = editedPrompt?.markdownStoreFiles ?? []
  const retainedAttachments = editing && editedPrompt
    ? editedPrompt.attachments.filter((attachment) => editing.retainedAttachmentIds === undefined || editing.retainedAttachmentIds.includes(attachment.id))
    : []
  const hasPromptContent = Boolean(editing?.text.trim() || retainedAttachments.length || editedPrompt?.markdownStoreFiles.length)
  const saveEdit = () => {
    if (!editing || !editedPrompt) return
    const retainedAttachmentIds = editing.retainedAttachmentIds
    void run(() => onEdit(editing.id, editing.revision, editing.text, retainedAttachmentIds), 'Queued prompt changed or could not be saved. Your edit is kept.', () => setEditing(null))
  }
  return <div className="uam-queue mb-1.5" role="group" aria-label="Queued prompt">
    {prompts.length > 0 && <span className="sr-only" role="status">{prompts.length} queued prompt{prompts.length === 1 ? '' : 's'}</span>}
    {unsentPrompts.length > 0 && <div className="uam-queue-list" role="group" aria-label="Unsent prompts">
      {unsentPrompts.map((prompt) => <div key={prompt.messageIndex} className="uam-queue-row uam-queued-prompt" data-unsent-message-index={prompt.messageIndex}>
        <div className="uam-queue-row__main">
          <span className="uam-queue-row__label">Unsent</span>
          <span className="uam-queue-row__text">{prompt.text}</span>
          <div className="uam-queue-row__actions">
            {onEditUnsent && <IconButton size="sm" icon={<Pencil size={13} />} label="Edit message in new branch" disabled={blocked || unsentActionsDisabled || Boolean(editing)} onClick={() => onEditUnsent(prompt.messageIndex, prompt.text)} />}
            {prompt.retryable && onRetryUnsent && <IconButton size="sm" icon={<CornerUpRight size={14} />} label="Retry message" disabled={blocked || unsentActionsDisabled || Boolean(editing)} onClick={() => void run(() => onRetryUnsent(prompt.messageIndex), 'Unsent prompt could not be retried. Your input is kept.')} />}
          </div>
        </div>
        {prompt.attachments.length > 0 && <div className="uam-queue-row__files" aria-label="Unsent prompt files">
          {prompt.attachments.map((attachment) => <FileChip key={attachment.id} name={attachment.name} title={attachment.path || attachment.name} />)}
        </div>}
      </div>)}
    </div>}
    {held && <div className="uam-queue-held">
      <StatusDot tone="warning" size={7} />
      <span className="uam-queue-held__copy">
        <strong>Queue held</strong>
        <span>Resume to send these queued prompts.</span>
      </span>
      <Button type="button" size="sm" variant="secondary" disabled={blocked || !onResume || !reorderable || prompts.length === 0 || Boolean(editing)} onClick={() => void run(() => onResume!(prompts.map((prompt) => ({ id: prompt.id!, revision: prompt.revision! }))), 'Queue changed or could not be resumed. Your queued prompts are kept.')}>Resume queue</Button>
    </div>}
    <div className="uam-queue-list" ref={listRef}>{rows.map(({ prompt, index, leaving: exiting }) => exiting
      ? <div key={`leaving-${prompt.id}`} className="uam-queue-row uam-queued-prompt" data-leaving="" aria-hidden>
        <div className="uam-queue-row__main"><span className="uam-queue-row__text">{prompt.text}</span></div>
      </div>
      : <div
        key={prompt.id ?? `legacy-${index}`}
        className={['uam-queue-row uam-queued-prompt', drag?.from === index && 'is-dragging', drag && drag.from !== index && drag.over === index && (drag.over > drag.from ? 'is-drop-after' : 'is-drop-before')].filter(Boolean).join(' ')}
        data-queue-id={prompt.id}
        data-held={held || undefined}
        draggable={canDrag || undefined}
        onDragStart={canDrag ? (event) => {
          event.dataTransfer.effectAllowed = 'move'
          event.dataTransfer.setData('text/plain', prompt.id!)
          setDrag({ from: index, over: index })
        } : undefined}
        onDragOver={drag ? (event) => {
          event.preventDefault()
          event.dataTransfer.dropEffect = 'move'
          if (drag.over !== index) setDrag({ ...drag, over: index })
        } : undefined}
        onDrop={drag ? (event) => {
          event.preventDefault()
          setDrag(null)
          if (drag.from !== index) reorder(drag.from, index)
        } : undefined}
        onDragEnd={() => setDrag(null)}
      >
      <div className="uam-queue-row__main">
        <span className="uam-queue-row__grip" aria-hidden title={canDrag ? 'Drag to reorder' : undefined} data-disabled={canDrag ? undefined : ''}><GripVertical size={13} /></span>
        <span className="uam-queue-row__label">Queued{prompts.length > 1 ? ` ${index + 1}` : ''}</span>
        <span className="uam-queue-row__text">{prompt.text}</span>
        <div className="uam-queue-row__actions">
          <IconButton size="sm" icon={<Pencil size={13} />} label="Edit queued prompt" disabled={blocked || !prompt.id || !prompt.revision || prompt.prepared || Boolean(editing)} onClick={() => { setEditing({ id: prompt.id!, revision: prompt.revision!, text: prompt.text }); setError('') }} />
          <IconButton size="sm" icon={<ArrowUp size={13} />} label="Move queued prompt up" disabled={blocked || !reorderable || index === 0 || Boolean(editing)} onClick={() => move(index, -1)} />
          <IconButton size="sm" icon={<ArrowDown size={13} />} label="Move queued prompt down" disabled={blocked || !reorderable || index === prompts.length - 1 || Boolean(editing)} onClick={() => move(index, 1)} />
          <IconButton size="sm" icon={<CornerUpRight size={14} />} label="Steer with this prompt now" disabled={held || blocked || prompt.prepared || Boolean(editing)} onClick={() => void run(() => onSteer(index, prompt.id && prompt.revision ? { id: prompt.id, revision: prompt.revision } : undefined), 'Queued prompt could not be sent. Try again.')} />
          <IconButton size="sm" icon={<X size={14} />} label="Remove queued prompt" disabled={blocked || prompt.prepared || Boolean(editing)} onClick={() => void run(() => onRemove(index, prompt.id && prompt.revision ? { id: prompt.id, revision: prompt.revision } : undefined), 'Queued prompt could not be removed. Try again.')} />
        </div>
      </div>
      {(!editing || editing.id !== prompt.id) && (prompt.attachments.length > 0 || prompt.markdownStoreFiles.length > 0) && <div className="uam-queue-row__files" aria-label={`Queued ${index + 1} files`}>
        {prompt.attachments.map((attachment) => <FileChip key={attachment.id} name={attachment.name} title={attachment.path || attachment.name} />)}
        {prompt.markdownStoreFiles.map((filePath) => <FileChip key={filePath} name={filePath.split(/[\\/]/).pop() || filePath} title={filePath} />)}
      </div>}
    </div>)}</div>
    {editing && <div className="uam-queue-edit">
      <div className="uam-queue-edit__head">
        <span className="uam-queue-edit__badge" aria-hidden />
        <span>{editedPromptIndex >= 0 ? `Editing queued ${editedPromptIndex + 1}` : 'Editing queued prompt'}</span>
      </div>
      {retainedAttachments.length > 0 && <div className="uam-queue-row__files" aria-label="Queued prompt attachments">
        {retainedAttachments.map((attachment) => <FileChip key={attachment.id} name={attachment.name} title={attachment.path || attachment.name} trailing={<>
          <span className="uam-queue-chip__type">{attachment.type === 'directory' ? 'dir' : attachment.type}</span>
          <button type="button" disabled={blocked || staleEdit || editedPrompt?.prepared} onClick={() => {
            if (blocked || staleEdit || editedPrompt?.prepared || pendingRef.current) return
            setEditing((current) => current ? {
              ...current,
              retainedAttachmentIds: (current.retainedAttachmentIds ?? editedPrompt?.attachments.map((item) => item.id) ?? []).filter((id) => id !== attachment.id),
            } : current)
          }} title="Remove attachment" aria-label={`Remove ${attachment.name} attachment`} className="uam-attachment-remove">
            <X size={12} aria-hidden />
          </button>
        </>} />)}
      </div>}
      {markdownStoreFiles.length > 0 && <div className="uam-queue-row__files" aria-label="Queued Skills context">
        {markdownStoreFiles.map((filePath) => <FileChip key={filePath} name={filePath.split(/[\\/]/).pop() || filePath} title={filePath} />)}
      </div>}
      <textarea aria-label="Edit queued prompt text" className="uam-queue-edit__input" rows={3} value={editing.text} disabled={pending} onChange={(event) => setEditing({ ...editing, text: event.target.value })} onKeyDown={(event) => {
        if (event.key === 'Escape') {
          event.preventDefault()
          event.stopPropagation()
          if (!pendingRef.current) { setEditing(null); setError('') }
        } else if (event.key === 'Enter' && (event.metaKey || event.ctrlKey) && !event.nativeEvent.isComposing) {
          event.preventDefault()
          event.stopPropagation()
          if (!staleEdit && !editedPrompt?.prepared && hasPromptContent) saveEdit()
        }
      }} />
      <div className="uam-queue-edit__actions">
        <Button type="button" size="sm" variant="primary" disabled={blocked || staleEdit || editedPrompt?.prepared || !hasPromptContent} onClick={saveEdit}>{pending ? 'Saving…' : 'Save'}</Button>
        <Button type="button" size="sm" variant="ghost" disabled={pending} onClick={() => { setEditing(null); setError('') }}>Cancel</Button>
      </div>
      {staleEdit && <div role="alert" className="uam-queue-error">Queued prompt changed. Cancel to reload it. Your edit is kept.</div>}
    </div>}
    {error && <div role="alert" className="uam-queue-error">{error}</div>}
  </div>
}
