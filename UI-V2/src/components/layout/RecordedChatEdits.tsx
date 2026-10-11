import { useCallback, useEffect, useRef, useState } from 'react'
import { RotateCw } from 'lucide-react'
import type { ChatFileChangeReceipt, ChatFileChangeReceiptsResponse } from '../../store/cpp/types'
import { useAppStore } from '../../store/useAppStore'
import { Button, Presence } from '../ui'
import { RepositoryDiffDialog } from '../shared/RepositoryDiffDialog'
import '../shared/review.css'

function receiptStatus(receipt: ChatFileChangeReceipt): string {
  if (receipt.destinationPath) return 'R'
  const operation = typeof receipt.kind === 'string' ? receipt.kind : receipt.kind.type
  return operation === 'delete' ? 'D' : operation === 'add' ? 'A' : operation === 'update' ? 'M' : '?'
}

function readableReceiptTime(value: string): string {
  if (!value) return ''
  const date = new Date(value)
  return Number.isNaN(date.getTime()) ? 'Time unavailable' : date.toLocaleString([], { dateStyle: 'medium', timeStyle: 'short' })
}

/** Reviews immutable provider patches, independently of current workspace commit selection. */
export function RecordedChatEdits({ chatId, workspaceDirectory, workspaceWorktreeDirectory, executionHostId }: {
  chatId: string
  workspaceDirectory?: string
  workspaceWorktreeDirectory?: string
  executionHostId?: string
}) {
  const getReceipts = useAppStore((state) => state.getChatFileChangeReceipts)
  const [receipts, setReceipts] = useState<ChatFileChangeReceipt[]>([])
  const [omitted, setOmitted] = useState(0)
  const [loading, setLoading] = useState(true)
  const [error, setError] = useState('')
  const [selected, setSelected] = useState<ChatFileChangeReceipt | null>(null)
  const generation = useRef(0)
  const refresh = useCallback(async () => {
    const request = ++generation.current
    setLoading(true)
    setError('')
    setReceipts([])
    setSelected(null)
    let data: ChatFileChangeReceiptsResponse | undefined
    let failure = ''
    try { data = await getReceipts(chatId) }
    catch (reason) { failure = reason instanceof Error ? reason.message : 'Could not load recorded chat edits.' }
    const current = useAppStore.getState()
    const session = current.sessions.find((item) => item.id === chatId)
    if (generation.current !== request || current.activeSessionId !== chatId || !session ||
      session.workspaceDirectory !== workspaceDirectory || session.workspaceWorktreeDirectory !== workspaceWorktreeDirectory || session.executionHostId !== executionHostId) return
    setLoading(false)
    if (failure) { setError(failure); return }
    if (data?.chatId !== chatId || data.scope !== 'recorded-provider-edits' || !Array.isArray(data.receipts) ||
      data.receipts.some((item) => !item || typeof item !== 'object' || item.chatId !== chatId || item.providerId !== 'codex-cli' || typeof item.path !== 'string' || typeof item.patch !== 'string' || typeof item.destinationPath !== 'string' ||
        !Number.isSafeInteger(item.messageIndex) || item.messageIndex < 0 || typeof item.toolCallId !== 'string' || typeof item.messageCreatedAt !== 'string' ||
        !(typeof item.kind === 'string' || item.kind !== null && typeof item.kind === 'object' && !Array.isArray(item.kind)))) {
      setError('Recorded chat edits returned an invalid response.')
      return
    }
    setReceipts(data.receipts)
    setOmitted(Number.isSafeInteger(data.omittedCount) && data.omittedCount > 0 ? data.omittedCount : 0)
  }, [chatId, workspaceDirectory, workspaceWorktreeDirectory, executionHostId, getReceipts])
  useEffect(() => {
    void refresh()
    return () => { generation.current += 1 }
  }, [refresh])
  const recordedDiff = useCallback(async () => selected?.patch ?? '', [selected])
  return <div className="flex min-h-0 flex-1 flex-col px-3 py-3">
    <div className="mb-3 flex items-center gap-2 text-xs">
      <span className="min-w-0 flex-1">Completed Codex edits. Read-only saved changes.</span>
      <Button type="button" size="sm" variant="ghost" leadingIcon={<RotateCw size={13} aria-hidden />} disabled={loading} onClick={() => { void refresh() }}>Refresh recorded edits</Button>
    </div>
    {loading ? <p role="status" className="text-xs">Loading recorded chat edits…</p> : error ? <div role="alert" className="mb-3 flex items-center gap-2 rounded-md border px-3 py-2 text-xs" style={{ background: 'var(--error-dim)', borderColor: 'var(--border)', color: 'var(--text)' }}>
      <span className="min-w-0 flex-1">{error}</span>
      <Button type="button" size="sm" variant="ghost" leadingIcon={<RotateCw size={13} aria-hidden />} onClick={() => { void refresh() }}>Retry</Button>
    </div> : <>
      {receipts.length === 0 && <p role="status" className="text-xs">No recorded completed Codex edits. Shell changes and other providers are not attributed.</p>}
      {omitted > 0 && <p role="status" className="mb-2 text-xs">{omitted} saved edit{omitted === 1 ? '' : 's'} could not be shown within supported limits.</p>}
      <div className="min-h-0 overflow-auto">{receipts.map((receipt, index) => <button
        key={`${receipt.messageIndex}:${receipt.toolCallId}:${index}`} type="button"
        className="uam-rv-row-btn"
        aria-label={`Review recorded edit ${index + 1}: ${receipt.path}`}
        title={`Tool ${receipt.toolCallId}`}
        onClick={() => setSelected(receipt)}
      >
        <span className="break-all font-mono text-[12px]">{receipt.path}{receipt.destinationPath ? ` → ${receipt.destinationPath}` : ''}</span>
        <span className="text-[11px]" style={{ color: 'var(--text-3)' }}>Message {receipt.messageIndex + 1}{receipt.messageCreatedAt ? <> · <time dateTime={receipt.messageCreatedAt} title={receipt.messageCreatedAt}>{readableReceiptTime(receipt.messageCreatedAt)}</time></> : ''}</span>
      </button>)}</div>
    </>}
    <Presence open={Boolean(selected)}>{selected && <RepositoryDiffDialog chatId={chatId} vcsType="git" sourcePath={selected.destinationPath || selected.path} comparisonLabel="Recorded agent edit" file={{ path: `Recorded edit: ${selected.path}`, status: receiptStatus(selected), staged: false, binary: false,
      additions: selected.patch.split('\n').filter((line) => line.startsWith('+') && !line.startsWith('+++')).length,
      deletions: selected.patch.split('\n').filter((line) => line.startsWith('-') && !line.startsWith('---')).length,
    }} getDiff={recordedDiff} onClose={() => setSelected(null)} />}</Presence>
  </div>
}
