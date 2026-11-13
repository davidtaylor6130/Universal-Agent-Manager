import { useCallback, useEffect, useRef, useState } from 'react'
import { Bot, CircleDashed, X } from 'lucide-react'
import { Button, IconButton, Notice, Overlay, RowButton, Tooltip } from '../ui'
import { sendToCEF } from '../../ipc/cefBridge'
import { useAppStore } from '../../store/useAppStore'
import { fromViewportMenu, getTopmostModal } from '../../utils/modalFocus'

export type GeminiChildConversation = { reference: string; preview: string; startedAt: string; updatedAt: string; messages: number }
type Listing = { chatId: string; children: GeminiChildConversation[]; unreadable: number; truncated: boolean }

function validListing(value: unknown, chatId: string): value is Listing {
  if (!value || typeof value !== 'object') return false
  const data = value as Partial<Listing>
  return data.chatId === chatId && Array.isArray(data.children) && typeof data.unreadable === 'number' && typeof data.truncated === 'boolean' &&
    data.children.every(child => typeof child?.reference === 'string' && child.reference !== '' && typeof child.preview === 'string' &&
      typeof child.startedAt === 'string' && typeof child.updatedAt === 'string' && typeof child.messages === 'number')
}

function when(child: GeminiChildConversation) {
  const value = new Date(child.updatedAt || child.startedAt)
  return Number.isNaN(value.getTime()) ? 'Time unavailable' : value.toLocaleString(undefined, { dateStyle: 'medium', timeStyle: 'short' })
}

/** Lists journals Gemini stored under this exact parent; each row opens through the existing exact child reader. */
export function GeminiChildBrowser({ chatId, onClose, returnFocusTo }: { chatId: string; onClose: () => void; returnFocusTo?: () => HTMLElement | null | undefined }) {
  const [listing, setListing] = useState<Listing | null>(null)
  const [error, setError] = useState('')
  const [openError, setOpenError] = useState('')
  const [loading, setLoading] = useState(true)
  const [opening, setOpening] = useState('')
  const dialogRef = useRef<HTMLDivElement>(null)
  const openingRef = useRef(false)
  const dismissedRef = useRef(false)
  const read = useRef(0)
  // Dismissing keeps the app usable through a slow SSH read; the read still saves but no longer navigates.
  const dismiss = () => { dismissedRef.current = true; onClose() }
  const dismissRef = useRef(dismiss)
  dismissRef.current = dismiss
  const returnFocusRef = useRef(returnFocusTo)
  returnFocusRef.current = returnFocusTo
  const openSubAgentSession = useAppStore(state => state.openSubAgentSession)

  const load = useCallback(async () => {
    const serial = ++read.current
    setLoading(true)
    setError('')
    try {
      const response = await sendToCEF<Listing>({ action: 'listGeminiChildConversations', payload: { chatId } })
      if (read.current !== serial) return
      if (!response.ok) setError(response.error || 'Subagent conversations could not be read.')
      else if (!validListing(response.data, chatId)) setError('Subagent conversations could not be read.')
      else setListing(response.data)
    } catch { if (read.current === serial) setError('Subagent conversations could not be read.') }
    finally { if (read.current === serial) setLoading(false) }
  }, [chatId])

  useEffect(() => { void load(); return () => { ++read.current } }, [load])

  useEffect(() => {
    const dialog = dialogRef.current
    const returnFocus = returnFocusRef.current
    dialog?.focus()
    const onKey = (event: KeyboardEvent) => {
      if (event.defaultPrevented || fromViewportMenu(event) || getTopmostModal() !== dialog) return
      if (event.key === 'Escape') { event.preventDefault(); event.stopPropagation(); dismissRef.current() }
    }
    window.addEventListener('keydown', onKey, true)
    return () => {
      window.removeEventListener('keydown', onKey, true)
      const row = returnFocus?.()
      if (row?.isConnected && (!document.activeElement || document.activeElement === document.body || dialog?.contains(document.activeElement))) row.focus()
    }
  }, [])

  const open = async (child: GeminiChildConversation) => {
    if (openingRef.current) return
    openingRef.current = true
    dialogRef.current?.focus() // The row is about to disable; keep keyboard focus in the dialog.
    setOpening(child.reference)
    setOpenError('')
    let failure = ''
    try {
      const opened = await openSubAgentSession(chatId, child.reference, child.preview, true, message => { failure = message }, () => !dismissedRef.current)
      if (opened && !dismissedRef.current) onClose()
      else setOpenError(failure || 'This subagent conversation could not be opened. Try again.')
    } catch { setOpenError('This subagent conversation could not be opened. Try again.') }
    finally { openingRef.current = false; setOpening('') }
  }

  const children = listing?.children ?? []
  return <Overlay open zIndex={80} onBackdropMouseDown={dismiss}>
    <div ref={dialogRef} role="dialog" aria-modal="true" aria-label="Subagent conversations" tabIndex={-1}
      className="flex max-h-[calc(100vh-2rem)] w-full max-w-xl flex-col overflow-hidden rounded-xl"
      style={{ background: 'var(--surface)', border: '1px solid var(--border-bright)', boxShadow: 'var(--elev-3)' }}>
      <div className="flex items-start justify-between gap-4 px-5 py-4" style={{ borderBottom: '1px solid var(--border)' }}>
        <div>
          <div className="text-sm font-semibold" style={{ color: 'var(--text)' }}>Subagent conversations</div>
          <div className="mt-1 text-xs" style={{ color: 'var(--text-3)' }}>Read-only history Gemini saved under this chat.</div>
        </div>
        <IconButton icon={<X size={14} />} label="Close" size="sm" variant="ghost" onClick={dismiss} />
      </div>
      <div className="min-h-0 flex-1 space-y-2 overflow-y-auto px-3 py-3" aria-busy={loading || undefined}>
        {loading && !listing && <p role="status" className="flex items-center gap-2 px-2 py-2 text-xs" style={{ color: 'var(--text-2)' }}><CircleDashed size={13} className="uam-working-ring" aria-hidden />Reading subagent conversations</p>}
        {error && <Notice tone="error" dismissible={false} dismissLabel="Dismiss" actions={<Button size="sm" variant="secondary" disabled={loading} onClick={() => { void load() }}>Retry</Button>}>{error}</Notice>}
        {openError && <Notice tone="error" dismissLabel="Dismiss" onDismiss={() => setOpenError('')}>{openError}</Notice>}
        {listing && !children.length && !error && !listing.unreadable && !listing.truncated && <p role="status" className="px-2 py-2 text-xs" style={{ color: 'var(--text-2)' }}>No subagent conversations are saved for this chat.</p>}
        {listing && (listing.unreadable > 0 || listing.truncated) && <Notice tone="warning" dismissible={false} dismissLabel="Dismiss">
          {listing.unreadable > 0 ? `${listing.unreadable} saved ${listing.unreadable === 1 ? 'journal' : 'journals'} could not be read. ` : ''}{listing.truncated ? 'Not every saved conversation is shown.' : ''}
        </Notice>}
        {children.length > 0 && <ul aria-label="Saved subagent conversations" className="space-y-0.5">
          {children.map(child => {
            const title = child.preview || 'Subagent conversation'
            const count = `${child.messages} ${child.messages === 1 ? 'message' : 'messages'}`
            return <li key={child.reference}>
              <RowButton type="button" aria-label={`Open subagent conversation: ${title}, ${when(child)}, ${count}`} aria-busy={opening === child.reference || undefined} disabled={Boolean(opening)} onClick={() => { void open(child) }}
                className="uam-sb-row flex w-full items-center gap-2 rounded-md px-2 py-1.5 text-left text-sm">
                {opening === child.reference ? <CircleDashed size={15} className="uam-working-ring shrink-0" aria-hidden /> : <Bot size={15} className="shrink-0" aria-hidden />}
                <Tooltip label={title}><span className="min-w-0 flex-1 truncate" style={{ color: 'var(--text)' }}>{title}</span></Tooltip>
                <span className="shrink-0 text-xs" style={{ color: 'var(--text-3)' }}>{when(child)} · {count}</span>
              </RowButton>
            </li>
          })}
        </ul>}
      </div>
    </div>
  </Overlay>
}
