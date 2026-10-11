import { useRef, useState, type ReactNode } from 'react'
import { ArrowDown, ArrowUp, CornerUpRight, Pencil, X } from 'lucide-react'
import type { AcpQueuedPrompt } from '../../store/cpp/types'
import { Button, IconButton, StatusDot } from '../ui'
import './InlinePromptQueue.css'

interface InlinePromptQueueProps {
  prompts: readonly AcpQueuedPrompt[]
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

/** Edits queue snapshots separately from the composer; native ID/revision guards own admission. */
export function InlinePromptQueue({ prompts, held = false, onResume, disabled = false, onEdit, onReorder, onSteer, onRemove }: InlinePromptQueueProps) {
  const [editing, setEditing] = useState<{ id: string; revision: number; text: string; retainedAttachmentIds?: string[] } | null>(null)
  const [pending, setPending] = useState(false)
  const pendingRef = useRef(false)
  const [error, setError] = useState('')
  const blocked = disabled || pending
  const reorderable = prompts.every((prompt) => prompt.id && prompt.revision && !prompt.prepared)

  async function run(operation: () => Promise<boolean>, failure: string, finish?: () => void) {
    if (blocked || pendingRef.current) return
    pendingRef.current = true
    setPending(true)
    setError('')
    try {
      if (await operation()) finish?.()
      else setError(failure)
    } catch {
      setError(failure)
    } finally {
      pendingRef.current = false
      setPending(false)
    }
  }

  function move(index: number, direction: number) {
    const expected = prompts.map((prompt) => ({ id: prompt.id!, revision: prompt.revision! }))
    const ordered = expected.map((prompt) => prompt.id)
    const previous = ordered[index]
    ordered[index] = ordered[index + direction]
    ordered[index + direction] = previous
    void run(() => onReorder(expected, ordered), 'Queue changed or could not be reordered. Try again.')
  }

  if (prompts.length === 0 && !editing) return null
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
    <span className="sr-only" role="status">{prompts.length} queued prompt{prompts.length === 1 ? '' : 's'}</span>
    {held && <div className="uam-queue-held">
      <StatusDot tone="warning" size={7} />
      <span className="uam-queue-held__copy">
        <strong>Held after restart</strong>
        <span>Resume to send these queued prompts.</span>
      </span>
      <Button type="button" size="sm" variant="secondary" disabled={blocked || !onResume || !reorderable || prompts.length === 0 || Boolean(editing)} onClick={() => void run(() => onResume!(prompts.map((prompt) => ({ id: prompt.id!, revision: prompt.revision! }))), 'Queue changed or could not be resumed. Your queued prompts are kept.')}>Resume queue</Button>
    </div>}
    <div className="uam-queue-list">{prompts.map((prompt, index) => <div key={prompt.id ?? `legacy-${index}`} className="uam-queue-row uam-queued-prompt" data-held={held || undefined}>
      <div className="uam-queue-row__main">
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
