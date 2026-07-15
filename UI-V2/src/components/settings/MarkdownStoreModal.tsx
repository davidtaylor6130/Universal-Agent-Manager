import { useEffect, useMemo, useState } from 'react'
import { useShallow } from 'zustand/react/shallow'
import { useAppStore } from '../../store/useAppStore'
import { Button } from '../ui'
import type { MarkdownStoreDraft, MarkdownStoreEntry } from '../../types/markdownStore'
import { BookOpen, ExternalLink, Paperclip, Search } from 'lucide-react'

const EMPTY_DRAFT: MarkdownStoreDraft = {
  title: '',
  maker: '',
  review: '',
  body: '',
}

export function MarkdownStoreModal() {
  const activeSessionId = useAppStore((s) => s.activeSessionId)
  const markdownStoreDirectory = useAppStore((s) => s.markdownStoreDirectory)
  const entries = useAppStore(useShallow((s) => s.markdownStoreEntries))
  const loading = useAppStore((s) => s.markdownStoreLoading)
  const error = useAppStore((s) => s.markdownStoreError)
  const close = useAppStore((s) => s.closeMarkdownStore)
  const refresh = useAppStore((s) => s.refreshMarkdownStore)
  const createEntry = useAppStore((s) => s.createMarkdownStoreEntry)
  const revealEntry = useAppStore((s) => s.revealMarkdownStoreEntry)
  const editEntry = useAppStore((s) => s.editMarkdownStoreEntry)
  const attachEntry = useAppStore((s) => s.attachMarkdownStoreEntry)
  const [search, setSearch] = useState('')
  const [isAdding, setIsAdding] = useState(false)
  const [draft, setDraft] = useState<MarkdownStoreDraft>(EMPTY_DRAFT)
  const [submitting, setSubmitting] = useState(false)

  useEffect(() => {
    const handler = (event: KeyboardEvent) => {
      if (event.key === 'Escape') close()
    }
    window.addEventListener('keydown', handler)
    return () => window.removeEventListener('keydown', handler)
  }, [close])

  const filtered = useMemo(() => {
    const query = search.trim().toLowerCase()
    if (!query) return entries
    return entries.filter((entry) =>
      [entry.title, entry.maker, entry.review, entry.preview, entry.filePath]
        .some((value) => value.toLowerCase().includes(query))
    )
  }, [entries, search])

  const submitDraft = async () => {
    if (!draft.title.trim() || !draft.body.trim()) return
    setSubmitting(true)
    const ok = await createEntry({
      title: draft.title.trim(),
      maker: draft.maker.trim(),
      review: draft.review.trim(),
      body: draft.body.trim(),
    })
    setSubmitting(false)
    if (ok) {
      setDraft(EMPTY_DRAFT)
      setIsAdding(false)
    }
  }

  const attach = (entry: MarkdownStoreEntry) => {
    if (!activeSessionId) return
    attachEntry(activeSessionId, entry)
    close()
  }

  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center p-4 animate-fade-in" style={{ background: 'rgba(0, 0, 0, 0.45)', backdropFilter: 'blur(4px)' }}>
      <div
        role="dialog"
        aria-modal="true"
        aria-label="Markdown Store"
        tabIndex={-1}
        className="w-full max-w-5xl max-h-[86vh] flex flex-col overflow-hidden animate-slide-in"
        style={{
          background: 'var(--surface)',
          border: '1px solid var(--border-bright)',
          borderRadius: 12,
          boxShadow: '0 24px 70px rgba(0, 0, 0, 0.38)',
        }}
      >
        <div className="flex items-start justify-between gap-4 px-5 py-4" style={{ borderBottom: '1px solid var(--border)' }}>
          <div className="min-w-0">
            <div className="text-base font-semibold" style={{ color: 'var(--text)' }}>Markdown Store</div>
            <div className="text-xs mt-1 truncate" style={{ color: 'var(--text-3)' }}>
              {markdownStoreDirectory || 'No store directory configured'}
            </div>
          </div>
          <Button variant="secondary" size="sm" onClick={close}>
            Close
          </Button>
        </div>

        <div className="flex items-center gap-2 px-5 py-3" style={{ borderBottom: '1px solid var(--border)' }}>
          <label className="flex flex-1 items-center gap-2 px-2.5" style={{ border: '1px solid var(--border)', borderRadius: 8, background: 'var(--bg)' }}>
            <Search size={14} aria-hidden style={{ color: 'var(--text-3)' }} />
            <input
              type="search"
              aria-label="Search Markdown Store"
              autoFocus
              value={search}
              onChange={(event) => setSearch(event.target.value)}
              placeholder="Find by title, maker, review, content, or path"
              className="flex-1 text-sm"
              style={{ border: 0, background: 'transparent', color: 'var(--text)', padding: '8px 0', outline: 'none' }}
            />
          </label>
          <span className="text-xs tabular-nums" style={{ color: 'var(--text-3)' }}>
            {filtered.length} of {entries.length}
          </span>
          <Button variant="secondary" size="sm" onClick={() => void refresh()}>
            Refresh
          </Button>
          <Button variant={isAdding ? 'secondary' : 'primary'} size="sm" onClick={() => setIsAdding((value) => !value)}>
            Publish
          </Button>
        </div>

        {error && (
          <div className="mx-5 mt-3 text-xs" style={{ color: 'var(--red)' }}>{error}</div>
        )}

        <div className="flex-1 overflow-auto px-5 py-4">
          {isAdding && (
            <div className="mb-4 grid gap-3 p-3 animate-fade-in" style={{ border: '1px solid var(--border)', borderRadius: 8, background: 'var(--bg)' }}>
              <input value={draft.title} onChange={(event) => setDraft({ ...draft, title: event.target.value })} placeholder="Title" className="text-sm" style={{ border: '1px solid var(--border)', borderRadius: 6, background: 'var(--surface)', color: 'var(--text)', padding: '8px 10px' }} />
              <div className="grid grid-cols-1 md:grid-cols-2 gap-3">
                <input value={draft.maker} onChange={(event) => setDraft({ ...draft, maker: event.target.value })} placeholder="Maker" className="text-sm" style={{ border: '1px solid var(--border)', borderRadius: 6, background: 'var(--surface)', color: 'var(--text)', padding: '8px 10px' }} />
                <input value={draft.review} onChange={(event) => setDraft({ ...draft, review: event.target.value })} placeholder="Review feedback" className="text-sm" style={{ border: '1px solid var(--border)', borderRadius: 6, background: 'var(--surface)', color: 'var(--text)', padding: '8px 10px' }} />
              </div>
              <textarea value={draft.body} onChange={(event) => setDraft({ ...draft, body: event.target.value })} placeholder="Markdown body" rows={8} className="text-sm resize-vertical" style={{ border: '1px solid var(--border)', borderRadius: 6, background: 'var(--surface)', color: 'var(--text)', padding: '8px 10px', fontFamily: 'var(--font-mono)' }} />
              <div className="flex justify-end gap-2">
                <Button variant="secondary" size="sm" onClick={() => setIsAdding(false)}>Cancel</Button>
                <Button variant="primary" size="sm" disabled={submitting || !draft.title.trim() || !draft.body.trim()} onClick={() => void submitDraft()}>Publish</Button>
              </div>
            </div>
          )}

          {loading ? (
            <div className="text-sm" style={{ color: 'var(--text-3)' }}>Loading Markdown Store...</div>
          ) : filtered.length === 0 ? (
            <div className="flex flex-col items-center gap-2 rounded-xl p-8 text-center animate-fade-in" style={{ color: 'var(--text-3)', border: '1px solid var(--border)', background: 'var(--surface-up)' }}>
              {search.trim() ? <Search size={24} strokeWidth={1.5} aria-hidden /> : <BookOpen size={24} strokeWidth={1.5} aria-hidden />}
              <div className="text-sm font-medium" style={{ color: 'var(--text-2)' }}>
                {search.trim() ? 'No Markdown Store entries match this search' : 'No Markdown Store entries yet'}
              </div>
              <div className="text-xs">
                {search.trim() ? 'Try another term or clear the search.' : 'Publish a `.uam` file to reuse it in future chats.'}
              </div>
            </div>
          ) : (
            <div className="grid gap-2">
              {filtered.map((entry) => (
                <div key={entry.filePath} className="p-3" style={{ border: '1px solid var(--border)', borderRadius: 8, background: 'var(--bg)' }}>
                  <div className="flex items-start gap-3">
                    <div className="min-w-0 flex-1">
                      <div className="text-sm font-medium truncate" style={{ color: 'var(--text)' }}>{entry.title}</div>
                      <div className="mt-1 flex flex-wrap gap-x-3 gap-y-1 text-[11px]" style={{ color: 'var(--text-3)' }}>
                        {entry.maker && <span>Maker: {entry.maker}</span>}
                        {entry.review && <span>Review: {entry.review}</span>}
                        {entry.dateUpdated && <span>Updated: {entry.dateUpdated}</span>}
                      </div>
                      {entry.preview && <div className="mt-2 text-xs line-clamp-2" style={{ color: 'var(--text-2)' }}>{entry.preview}</div>}
                      <div className="mt-2 text-[11px] truncate" style={{ color: 'var(--text-3)' }}>{entry.filePath}</div>
                    </div>
                    <div className="flex flex-col gap-2">
                      <Button leadingIcon={<Paperclip size={13} />} variant={activeSessionId ? 'primary' : 'secondary'} size="sm" disabled={!activeSessionId} onClick={() => attach(entry)}>
                        Attach to message
                      </Button>
                      <Button leadingIcon={<BookOpen size={13} />} variant="secondary" size="sm" onClick={() => void editEntry(entry)}>
                        Open in editor
                      </Button>
                      <Button leadingIcon={<ExternalLink size={13} />} variant="secondary" size="sm" onClick={() => void revealEntry(entry)}>
                        Reveal
                      </Button>
                    </div>
                  </div>
                </div>
              ))}
            </div>
          )}
        </div>
      </div>
    </div>
  )
}
