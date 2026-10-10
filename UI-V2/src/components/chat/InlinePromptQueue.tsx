import { useRef, useState } from 'react'
import { ArrowDown, ArrowUp, CornerUpRight, Pencil, X } from 'lucide-react'
import type { AcpQueuedPrompt } from '../../store/cpp/types'
import { Button, IconButton } from '../ui'

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
  return <div className="mb-1.5" role="group" aria-label="Queued prompt">
    <span className="sr-only" role="status">{prompts.length} queued prompt{prompts.length === 1 ? '' : 's'}</span>
    {held && <div className="flex items-center gap-2 py-1 text-[11px]" style={{ color: 'var(--text-2)' }}><span>Restored queue is paused.</span><Button type="button" size="sm" variant="ghost" disabled={blocked || !onResume || !reorderable || prompts.length === 0 || Boolean(editing)} onClick={() => void run(() => onResume!(prompts.map((prompt) => ({ id: prompt.id!, revision: prompt.revision! }))), 'Queue changed or could not be resumed. Your queued prompts are kept.')}>Resume queue</Button></div>}
    <div className="max-h-40 overflow-y-auto">{prompts.map((prompt, index) => <div key={prompt.id ?? `legacy-${index}`} className="uam-queued-prompt border-b border-[var(--border)] py-1.5 text-[11px]">
      <div className="flex items-center gap-1">
        <span className="shrink-0 font-medium mr-1">Queued {prompts.length > 1 ? index + 1 : ''}</span>
        <span className="min-w-0 flex-1 line-clamp-2 whitespace-pre-wrap break-words">{prompt.text}</span>
        <IconButton size="sm" icon={<Pencil size={13} />} label="Edit queued prompt" disabled={blocked || !prompt.id || !prompt.revision || prompt.prepared || Boolean(editing)} onClick={() => { setEditing({ id: prompt.id!, revision: prompt.revision!, text: prompt.text }); setError('') }} />
        <IconButton size="sm" icon={<ArrowUp size={13} />} label="Move queued prompt up" disabled={blocked || !reorderable || index === 0 || Boolean(editing)} onClick={() => move(index, -1)} />
        <IconButton size="sm" icon={<ArrowDown size={13} />} label="Move queued prompt down" disabled={blocked || !reorderable || index === prompts.length - 1 || Boolean(editing)} onClick={() => move(index, 1)} />
        <IconButton size="sm" icon={<CornerUpRight size={14} />} label="Steer with this prompt now" disabled={held || blocked || prompt.prepared || Boolean(editing)} onClick={() => void run(() => onSteer(index, prompt.id && prompt.revision ? { id: prompt.id, revision: prompt.revision } : undefined), 'Queued prompt could not be sent. Try again.')} />
        <IconButton size="sm" icon={<X size={14} />} label="Remove queued prompt" disabled={blocked || prompt.prepared || Boolean(editing)} onClick={() => void run(() => onRemove(index, prompt.id && prompt.revision ? { id: prompt.id, revision: prompt.revision } : undefined), 'Queued prompt could not be removed. Try again.')} />
      </div>
      {(prompt.attachments.length > 0 || prompt.markdownStoreFiles.length > 0) && <span style={{ color: 'var(--text-3)' }}>{prompt.attachments.length + prompt.markdownStoreFiles.length} attachment{prompt.attachments.length + prompt.markdownStoreFiles.length === 1 ? '' : 's'}</span>}
    </div>)}</div>
    {editing && <div className="pt-1.5 text-[11px]" style={{ color: 'var(--text-2)' }}>
      <div className="pb-1">{editedPromptIndex >= 0 ? `Editing queued ${editedPromptIndex + 1}` : 'Editing queued prompt'}</div>
      {retainedAttachments.length > 0 && <div className="flex flex-wrap gap-2 pb-1" aria-label="Queued prompt attachments">
        {retainedAttachments.map((attachment) => <span key={attachment.id} className="inline-flex items-center gap-2 max-w-full text-[11px]" title={attachment.path || attachment.name}
          style={{ border: '1px solid var(--border)', borderRadius: 999, background: 'var(--surface-up)', color: 'var(--text-2)', padding: '3px 7px' }}>
          <span className="truncate max-w-[260px]">{attachment.name}</span>
          <span style={{ color: 'var(--text-3)' }}>{attachment.type === 'directory' ? 'dir' : attachment.type}</span>
          <button type="button" disabled={blocked || staleEdit || editedPrompt?.prepared} onClick={() => {
            if (blocked || staleEdit || editedPrompt?.prepared || pendingRef.current) return
            setEditing((current) => current ? {
              ...current,
              retainedAttachmentIds: (current.retainedAttachmentIds ?? editedPrompt?.attachments.map((item) => item.id) ?? []).filter((id) => id !== attachment.id),
            } : current)
          }} title="Remove attachment" aria-label={`Remove ${attachment.name} attachment`} className="uam-attachment-remove" style={{ color: 'var(--text-3)' }}>
            <X size={12} aria-hidden />
          </button>
        </span>)}
      </div>}
      {markdownStoreFiles.length > 0 && <div className="flex flex-wrap gap-2 pb-1" aria-label="Queued Skills context">
        {markdownStoreFiles.map((filePath) => <span key={filePath} className="inline-flex items-center max-w-full text-[11px]" title={filePath}
          style={{ border: '1px solid var(--border)', borderRadius: 999, background: 'var(--surface-up)', color: 'var(--text-2)', padding: '3px 7px' }}>
          <span className="truncate max-w-[260px]">{filePath.split(/[\\/]/).pop() || filePath}</span>
        </span>)}
      </div>}
      <textarea aria-label="Edit queued prompt text" className="w-full bg-black text-white border border-[var(--border)] px-2 py-1 text-xs resize-y" rows={3} value={editing.text} disabled={pending} onChange={(event) => setEditing({ ...editing, text: event.target.value })} onKeyDown={(event) => {
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
      <div className="flex items-center gap-2">
        <Button type="button" size="sm" variant="ghost" disabled={blocked || staleEdit || editedPrompt?.prepared || !hasPromptContent} onClick={saveEdit}>{pending ? 'Saving…' : 'Save'}</Button>
        <Button type="button" size="sm" variant="ghost" disabled={pending} onClick={() => { setEditing(null); setError('') }}>Cancel</Button>
      </div>
      {staleEdit && <div role="alert" className="text-xs py-1" style={{ color: 'var(--red)' }}>Queued prompt changed. Cancel to reload it. Your edit is kept.</div>}
    </div>}
    {error && <div role="alert" className="text-xs py-1" style={{ color: 'var(--red)' }}>{error}</div>}
  </div>
}
