import { useCallback, useEffect, useRef, useState } from 'react'
import { ChevronRight, RotateCw } from 'lucide-react'
import type { ChatFileChangeReceipt, ChatFileChangeReceiptsResponse } from '../../store/cpp/types'
import { useAppStore } from '../../store/useAppStore'
import { Button, Presence } from '../ui'
import { RepositoryDiffDialog } from '../shared/RepositoryDiffDialog'
import '../shared/review.css'
import { CODEX_CLI_PROVIDER_ID } from '../../utils/providerMetadata'

function receiptStatus(receipt: ChatFileChangeReceipt): string {
  if (receipt.destinationPath) return 'R'
  const operation = typeof receipt.kind === 'string' ? receipt.kind : receipt.kind.type
  return operation === 'delete' ? 'D' : operation === 'add' ? 'A' : operation === 'update' ? 'M' : '?'
}

/** Same colour rule as the workspace changed-files list. */
function statusColor(status: string): string {
  return status === 'A' || status === '?' ? 'var(--success)' : status === 'D' ? 'var(--error)' : status === 'R' ? 'var(--info)' : 'var(--warning)'
}

function patchStats(patch: string): { additions: number; deletions: number } {
  const lines = patch.split('\n')
  return {
    additions: lines.filter((line) => line.startsWith('+') && !line.startsWith('+++')).length,
    deletions: lines.filter((line) => line.startsWith('-') && !line.startsWith('---')).length,
  }
}

function baseName(path: string): string {
  return path.split(/[\\/]/).pop() || path
}

function ChangeBar({ additions, deletions, max }: { additions: number; deletions: number; max: number }) {
  return <span className="uam-rv-bar" aria-hidden>
    <span className="uam-rv-bar__fill" style={{ width: `${((additions + deletions) / max) * 100}%` }}>
      <span className="uam-rv-bar__add" style={{ flexGrow: additions }} />
      <span className="uam-rv-bar__del" style={{ flexGrow: deletions }} />
    </span>
  </span>
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
      data.receipts.some((item) => !item || typeof item !== 'object' || item.chatId !== chatId || item.providerId !== CODEX_CLI_PROVIDER_ID || typeof item.path !== 'string' || typeof item.patch !== 'string' || typeof item.destinationPath !== 'string' ||
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
  const maxChange = receipts.reduce((max, item) => { const stats = patchStats(item.patch); return Math.max(max, stats.additions + stats.deletions) }, 1)
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
      <div className="min-h-0 overflow-auto">{receipts.map((receipt, index) => {
        const stats = patchStats(receipt.patch)
        const cut = Math.max(receipt.path.lastIndexOf('/'), receipt.path.lastIndexOf('\\'))
        const dir = cut > 0 ? receipt.path.slice(0, cut) : ''
        const name = receipt.destinationPath ? `${baseName(receipt.path)} → ${baseName(receipt.destinationPath)}` : baseName(receipt.path)
        return <div key={`${receipt.messageIndex}:${receipt.toolCallId}:${index}`} className="uam-rv-file group" style={{ color: 'var(--text-2)' }}>
          <button
            type="button"
            className="uam-rv-file__open"
            aria-label={`Review recorded edit ${index + 1}: ${receipt.path}`}
            title={`${receipt.path}${receipt.destinationPath ? ` → ${receipt.destinationPath}` : ''} · Tool ${receipt.toolCallId}`}
            onClick={() => setSelected(receipt)}
          >
            <span className="uam-rv-file__status" style={{ color: statusColor(receiptStatus(receipt)) }}>{receiptStatus(receipt)}</span>
            <span className="uam-rv-file__path">
              <span className="uam-rv-file__name">{name}</span>
              {dir && <span className="uam-rv-file__dir">{dir}</span>}
            </span>
            <span className="uam-rv-file__stat">
              <span className="uam-rv-stat">
                <ChangeBar additions={stats.additions} deletions={stats.deletions} max={maxChange} />
                <span className="uam-rv-stat__add" style={{ color: 'var(--success)' }}>+{stats.additions}</span>
                <span className="uam-rv-stat__del" style={{ color: 'var(--error)' }}>-{stats.deletions}</span>
              </span>
            </span>
            <span className="sr-only">Message {receipt.messageIndex + 1}{receipt.messageCreatedAt ? <> · <time dateTime={receipt.messageCreatedAt} title={receipt.messageCreatedAt}>{readableReceiptTime(receipt.messageCreatedAt)}</time></> : ''}</span>
            <ChevronRight size={13} aria-hidden className="uam-rv-file__chevron" style={{ color: 'var(--text-3)' }} />
          </button>
        </div>
      })}</div>
    </>}
    <Presence open={Boolean(selected)}>{selected && <RepositoryDiffDialog chatId={chatId} vcsType="git" sourcePath={selected.destinationPath || selected.path} comparisonLabel="Recorded agent edit" file={{ path: `Recorded edit: ${selected.path}`, status: receiptStatus(selected), staged: false, binary: false,
      additions: selected.patch.split('\n').filter((line) => line.startsWith('+') && !line.startsWith('+++')).length,
      deletions: selected.patch.split('\n').filter((line) => line.startsWith('-') && !line.startsWith('---')).length,
    }} getDiff={recordedDiff} onClose={() => setSelected(null)} />}</Presence>
  </div>
}
