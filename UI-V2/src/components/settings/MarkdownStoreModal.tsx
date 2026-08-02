import { useEffect, useMemo, useRef, useState } from 'react'
import { useShallow } from 'zustand/react/shallow'
import { useAppStore } from '../../store/useAppStore'
import { Button, IconButton } from '../ui'
import { MarkdownContent } from '../markdown/Markdown'
import type { MarkdownStoreConflictAction, MarkdownStoreDraft, MarkdownStoreEntry, MarkdownStoreImportCandidate, MarkdownStoreImportResult } from '../../types/markdownStore'
import { BookOpen, ExternalLink, FileInput, FolderInput, Paperclip, Pencil, Plus, RefreshCw, Search, Star, Users, X } from 'lucide-react'

const EMPTY_DRAFT: MarkdownStoreDraft = { title: '', maker: '', review: '', body: '', group: '' }

export function MarkdownStoreModal() {
  const activeSessionId = useAppStore((s) => s.activeSessionId)
  const markdownStoreDirectory = useAppStore((s) => s.markdownStoreDirectory)
  const entries = useAppStore(useShallow((s) => s.markdownStoreEntries))
  const loading = useAppStore((s) => s.markdownStoreLoading)
  const error = useAppStore((s) => s.markdownStoreError)
  const clearError = useAppStore((s) => s.clearMarkdownStoreError)
  const close = useAppStore((s) => s.closeMarkdownStore)
  const refresh = useAppStore((s) => s.refreshMarkdownStore)
  const browseDirectory = useAppStore((s) => s.browseMarkdownStoreDirectory)
  const setDirectory = useAppStore((s) => s.setMarkdownStoreDirectory)
  const createEntry = useAppStore((s) => s.createMarkdownStoreEntry)
  const updateEntry = useAppStore((s) => s.updateMarkdownStoreEntry)
  const setFavorite = useAppStore((s) => s.setMarkdownStoreFavorite)
  const browseImport = useAppStore((s) => s.browseMarkdownStoreImport)
  const previewImports = useAppStore((s) => s.previewMarkdownStoreImports)
  const importEntries = useAppStore((s) => s.importMarkdownStoreEntries)
  const revealEntry = useAppStore((s) => s.revealMarkdownStoreEntry)
  const editEntry = useAppStore((s) => s.editMarkdownStoreEntry)
  const attachEntry = useAppStore((s) => s.attachMarkdownStoreEntry)
  const [search, setSearch] = useState('')
  const [filter, setFilter] = useState('all')
  const [selectedPath, setSelectedPath] = useState('')
  const [editing, setEditing] = useState<'new' | 'existing' | null>(null)
  const [draft, setDraft] = useState<MarkdownStoreDraft>(EMPTY_DRAFT)
  const [showEditorPreview, setShowEditorPreview] = useState(false)
  const [submitting, setSubmitting] = useState(false)
  const submittingRef = useRef(false)
  const [imports, setImports] = useState<MarkdownStoreImportCandidate[]>([])
  const [selectedImports, setSelectedImports] = useState<Set<string>>(new Set())
  const [conflicts, setConflicts] = useState<Record<string, MarkdownStoreConflictAction>>({})
  const [importResults, setImportResults] = useState<MarkdownStoreImportResult[]>([])
  const [importing, setImporting] = useState(false)
  const importingRef = useRef(false)
  const importPreviewRequestRef = useRef(0)
  const editorTriggerRef = useRef<HTMLElement | null>(null)
  const importTriggerRef = useRef<HTMLElement | null>(null)
  const editorDialogRef = useRef<HTMLDivElement | null>(null)
  const importDialogRef = useRef<HTMLDivElement | null>(null)

  useEffect(() => {
    if (editing) editorDialogRef.current?.focus()
    else if (imports.length > 0) importDialogRef.current?.focus()
  }, [editing, imports.length])

  useEffect(() => {
    const handler = (event: KeyboardEvent) => {
      if (event.key !== 'Escape' || event.defaultPrevented) return
      if (editing) {
        clearError()
        setEditing(null)
        setDraft(EMPTY_DRAFT)
        setShowEditorPreview(false)
        editorTriggerRef.current?.focus()
        return
      }
      if (imports.length > 0) {
        clearError()
        setImports([])
        importTriggerRef.current?.focus()
        return
      }
      close()
    }
    window.addEventListener('keydown', handler)
    return () => window.removeEventListener('keydown', handler)
  }, [clearError, close, editing, imports.length])

  useEffect(() => {
    if (selectedPath && !entries.some((entry) => entry.filePath === selectedPath)) setSelectedPath('')
  }, [entries, selectedPath])

  const sourceProviders = useMemo(() => Array.from(new Set(entries.map((entry) => entry.sourceProvider).filter(Boolean))).sort(), [entries])
  const groups = useMemo(() => Array.from(new Set(entries.map((entry) => entry.group?.trim()).filter(Boolean))).sort(), [entries])
  const filtered = useMemo(() => {
    const query = search.trim().toLowerCase()
    return entries.filter((entry) => {
      if (filter === 'favorites' && !entry.favorite) return false
      if (filter.startsWith('source:') && entry.sourceProvider !== filter.slice(7)) return false
      if (filter.startsWith('group:') && entry.group !== filter.slice(6)) return false
      return !query || [entry.title, entry.maker, entry.review, entry.preview, entry.sourceProvider ?? '', entry.group ?? '', entry.filePath]
        .some((value) => value.toLowerCase().includes(query))
    })
  }, [entries, filter, search])
  const selected = filtered.find((entry) => entry.filePath === selectedPath) ?? filtered[0] ?? null

  const beginNew = () => { editorTriggerRef.current = document.activeElement as HTMLElement | null; clearError(); setDraft(EMPTY_DRAFT); setEditing('new'); setShowEditorPreview(false) }
  const beginEdit = (entry: MarkdownStoreEntry) => {
    editorTriggerRef.current = document.activeElement as HTMLElement | null
    clearError()
    setSelectedPath(entry.filePath)
    setDraft({ title: entry.title, maker: entry.maker, review: entry.review, body: entry.body ?? entry.preview, group: entry.group ?? '' })
    setEditing('existing')
    setShowEditorPreview(false)
  }
  const cancelEdit = () => { clearError(); setEditing(null); setDraft(EMPTY_DRAFT); setShowEditorPreview(false); editorTriggerRef.current?.focus() }
  const saveDraft = async () => {
    if (submittingRef.current || !draft.title.trim() || !draft.body.trim()) return
    submittingRef.current = true
    setSubmitting(true)
    const clean = { title: draft.title.trim(), maker: draft.maker.trim(), review: draft.review.trim(), body: draft.body.trim(), group: draft.group.trim() }
    let ok = false
    try {
      ok = editing === 'existing' && selected ? await updateEntry(selected, clean) : await createEntry(clean)
    } finally {
      submittingRef.current = false
      setSubmitting(false)
    }
    if (ok) cancelEdit()
  }
  const attach = (entry: MarkdownStoreEntry) => {
    if (!activeSessionId) return
    attachEntry(activeSessionId, entry)
    close()
  }
  const chooseDirectory = async () => {
    const chosen = await browseDirectory(markdownStoreDirectory)
    if (chosen && await setDirectory(chosen)) await refresh()
  }
  const loadImports = async (options: { includeProviders?: boolean; paths?: string[] }) => {
    importTriggerRef.current = document.activeElement as HTMLElement | null
    clearError()
    const requestId = ++importPreviewRequestRef.current
    const candidates = await previewImports(options)
    if (requestId !== importPreviewRequestRef.current) return
    setImports(candidates)
    setSelectedImports(new Set(candidates.filter((candidate) => candidate.supported).map((candidate) => candidate.id)))
    setConflicts(Object.fromEntries(candidates.filter((candidate) => candidate.collisionPath).map((candidate) => [candidate.id, 'skip'])))
    setImportResults([])
  }
  const chooseImport = async (kind: 'file' | 'folder') => {
    const path = await browseImport(kind)
    if (path) await loadImports({ paths: [path] })
  }
  const runImport = async () => {
    if (importingRef.current) return
    const selectedCandidates = imports.filter((candidate) => candidate.supported && selectedImports.has(candidate.id))
    importingRef.current = true
    setImporting(true)
    let results: MarkdownStoreImportResult[] = []
    try {
      results = await importEntries(selectedCandidates.map((candidate) => ({
        sourceProvider: candidate.sourceProvider,
        sourcePath: candidate.sourcePath,
        conflictAction: conflicts[candidate.id] ?? 'skip',
      })))
    } finally {
      importingRef.current = false
      setImporting(false)
    }
    setImportResults(results)
    if (results.length > 0) setImports([])
  }

  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center p-4 animate-fade-in" style={{ background: 'rgba(0, 0, 0, 0.45)', backdropFilter: 'blur(4px)' }}>
      <div role="dialog" aria-modal="true" aria-label="Skills" tabIndex={-1} className="w-full max-w-6xl h-[min(780px,90vh)] flex flex-col overflow-hidden animate-slide-in" style={{ background: 'var(--surface)', border: '1px solid var(--border-bright)', borderRadius: 12, boxShadow: '0 24px 70px rgba(0, 0, 0, 0.38)' }}>
        <div className="flex items-start justify-between gap-4 px-5 py-4" style={{ borderBottom: '1px solid var(--border)' }}>
          <div className="min-w-0"><div className="text-base font-semibold" style={{ color: 'var(--text)' }}>Skills</div><div className="text-xs mt-1 truncate" style={{ color: 'var(--text-3)' }}>{markdownStoreDirectory || 'No Skills directory configured'}</div></div>
          <IconButton icon={<X size={16} />} label="Close Skills" onClick={close} />
        </div>

        <div className="flex flex-wrap items-center gap-2 px-5 py-3" style={{ borderBottom: '1px solid var(--border)' }}>
          <label className="flex min-w-56 flex-1 items-center gap-2 px-2.5" style={{ border: '1px solid var(--border)', borderRadius: 8, background: 'var(--bg)' }}>
            <Search size={14} aria-hidden style={{ color: 'var(--text-3)' }} />
            <input type="search" aria-label="Search Skills" autoFocus value={search} onChange={(event) => setSearch(event.target.value)} placeholder="Search title, summary, maker, provider, or content" className="flex-1 text-sm" style={{ border: 0, background: 'transparent', color: 'var(--text)', padding: '8px 0', outline: 'none' }} />
          </label>
          <select aria-label="Filter Skills" value={filter} onChange={(event) => setFilter(event.target.value)} className="text-xs" style={{ border: '1px solid var(--border)', borderRadius: 7, background: 'var(--bg)', color: 'var(--text)', padding: '7px 9px' }}>
            <option value="all">All entries</option><option value="favorites">Favorites</option>
            {groups.map((group) => <option key={group} value={`group:${group}`}>Group: {group}</option>)}
            {sourceProviders.map((provider) => <option key={provider} value={`source:${provider}`}>Source: {provider}</option>)}
          </select>
          <span className="text-xs tabular-nums" style={{ color: 'var(--text-3)' }}>{filtered.length} of {entries.length}</span>
          <IconButton icon={<RefreshCw size={15} className={loading ? 'animate-spin' : undefined} />} label="Refresh Skills" disabled={loading} onClick={() => void refresh()} />
          <IconButton icon={<Users size={15} />} label="Import provider skills" disabled={!markdownStoreDirectory} onClick={() => void loadImports({ includeProviders: true })} />
          <IconButton icon={<FileInput size={15} />} label="Import file" disabled={!markdownStoreDirectory} onClick={() => void chooseImport('file')} />
          <IconButton icon={<FolderInput size={15} />} label="Import folder" disabled={!markdownStoreDirectory} onClick={() => void chooseImport('folder')} />
          <IconButton icon={<Plus size={16} />} label="Create skill" disabled={!markdownStoreDirectory} onClick={beginNew} style={{ border: 0, background: 'transparent' }} />
        </div>

        {!markdownStoreDirectory && <div role="status" className="mx-5 mt-3 flex items-center justify-between gap-3 rounded-md px-3 py-2 text-xs" style={{ color: 'var(--text-2)', background: 'var(--surface-up)', border: '1px solid var(--border)' }}><span>Choose a Skills folder before creating or importing entries.</span><Button variant="primary" size="sm" onClick={() => void chooseDirectory()}>Choose folder</Button></div>}
        {error && <div className="mx-5 mt-3 text-xs" style={{ color: 'var(--red)' }}>{error}</div>}

        <div className="relative flex-1 min-h-0 overflow-hidden px-5 py-4">
          {editing && <div className="fixed inset-0 z-[60] flex items-center justify-center p-4 animate-fade-in" style={{ background: 'rgba(0,0,0,.5)', backdropFilter: 'blur(3px)' }}><div ref={editorDialogRef} role="dialog" aria-modal="true" aria-label={editing === 'new' ? 'Create skill' : 'Edit skill'} tabIndex={-1} className="grid max-h-[calc(100vh-2rem)] w-full max-w-3xl gap-3 overflow-y-auto rounded-xl p-4 animate-slide-in" style={{ border: '1px solid var(--border-bright)', background: 'var(--surface)', boxShadow: 'var(--elev-3)' }}>
            <div className="flex items-center justify-between"><strong className="text-sm" style={{ color: 'var(--text)' }}>{editing === 'new' ? 'New entry' : 'Edit entry'}</strong><Button variant="secondary" size="sm" onClick={() => setShowEditorPreview((value) => !value)}>{showEditorPreview ? 'Edit Markdown' : 'Preview Markdown'}</Button></div>
            {error && <div role="alert" className="text-xs" style={{ color: 'var(--red)' }}>{error}</div>}
            <input aria-label="Entry title" value={draft.title} onChange={(event) => setDraft({ ...draft, title: event.target.value })} placeholder="Title" className="text-sm" style={{ border: '1px solid var(--border)', borderRadius: 6, background: 'var(--surface)', color: 'var(--text)', padding: '8px 10px' }} />
            <div className="grid grid-cols-1 md:grid-cols-2 gap-3"><input aria-label="Entry maker" value={draft.maker} onChange={(event) => setDraft({ ...draft, maker: event.target.value })} placeholder="Maker" className="text-sm" style={{ border: '1px solid var(--border)', borderRadius: 6, background: 'var(--surface)', color: 'var(--text)', padding: '8px 10px' }} /><input aria-label="Entry group" value={draft.group} onChange={(event) => setDraft({ ...draft, group: event.target.value })} placeholder="Group, for example Coding / Safety" className="text-sm" style={{ border: '1px solid var(--border)', borderRadius: 6, background: 'var(--surface)', color: 'var(--text)', padding: '8px 10px' }} /></div>
            <input aria-label="Entry summary" value={draft.review} onChange={(event) => setDraft({ ...draft, review: event.target.value })} placeholder="Summary" className="text-sm" style={{ border: '1px solid var(--border)', borderRadius: 6, background: 'var(--surface)', color: 'var(--text)', padding: '8px 10px' }} />
            {showEditorPreview ? <div className="min-h-48 rounded-md p-3" style={{ border: '1px solid var(--border)', background: 'var(--surface)', color: 'var(--text)' }}><MarkdownContent content={draft.body || '_Nothing to preview._'} /></div> : <textarea aria-label="Entry Markdown body" value={draft.body} onChange={(event) => setDraft({ ...draft, body: event.target.value })} placeholder="Markdown body" rows={10} className="text-sm resize-vertical" style={{ border: '1px solid var(--border)', borderRadius: 6, background: 'var(--surface)', color: 'var(--text)', padding: '8px 10px', fontFamily: 'var(--font-mono)' }} />}
            <div className="flex justify-end gap-2"><Button variant="secondary" size="sm" onClick={cancelEdit}>Cancel</Button><Button variant="primary" size="sm" disabled={submitting || !draft.title.trim() || !draft.body.trim()} onClick={() => void saveDraft()}>Save</Button></div>
          </div></div>}

          {imports.length > 0 && <div className="fixed inset-0 z-[60] flex items-center justify-center p-4 animate-fade-in" style={{ background: 'rgba(0,0,0,.5)', backdropFilter: 'blur(3px)' }}><div ref={importDialogRef} role="dialog" aria-modal="true" aria-label="Import skills" tabIndex={-1} className="flex max-h-[calc(100vh-2rem)] w-full max-w-3xl flex-col gap-2 rounded-xl p-4 animate-slide-in" style={{ border: '1px solid var(--border-bright)', background: 'var(--surface)', boxShadow: 'var(--elev-3)' }}>
            <div className="flex items-center justify-between"><strong className="text-sm" style={{ color: 'var(--text)' }}>Import preview</strong><span className="text-xs" style={{ color: 'var(--text-3)' }}>Sources are read only; selected items are copied as .uam files.</span></div>
            {error && <div role="alert" className="text-xs" style={{ color: 'var(--red)' }}>{error}</div>}
            <div data-import-candidates className="grid min-h-0 gap-2 overflow-y-auto">{imports.map((candidate) => <label key={candidate.id} className="flex items-start gap-2 rounded-md p-2 text-xs" style={{ border: '1px solid var(--border)', color: 'var(--text-2)' }}>
              <input type="checkbox" disabled={!candidate.supported} checked={candidate.supported && selectedImports.has(candidate.id)} onChange={(event) => setSelectedImports((current) => { const next = new Set(current); if (event.target.checked) next.add(candidate.id); else next.delete(candidate.id); return next })} />
              <span className="min-w-0 flex-1"><strong style={{ color: 'var(--text)' }}>{candidate.title || candidate.sourcePath.split(/[\\/]/).pop()}</strong> · {candidate.sourceProvider}<span className="block truncate" style={{ color: 'var(--text-3)' }}>{candidate.sourcePath}</span>{candidate.validationError && <span className="block" style={{ color: 'var(--red)' }}>{candidate.validationError}</span>}</span>
              {candidate.collisionPath && <select aria-label={`Collision action for ${candidate.title}`} value={conflicts[candidate.id] ?? 'skip'} onChange={(event) => setConflicts({ ...conflicts, [candidate.id]: event.target.value as MarkdownStoreConflictAction })} style={{ background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 5, color: 'var(--text)', padding: 4 }}><option value="skip">Skip existing</option><option value="replace">Replace existing</option><option value="separate">Keep separate</option></select>}
            </label>)}</div>
            <div className="flex justify-end gap-2"><Button variant="secondary" size="sm" onClick={() => { clearError(); setImports([]); importTriggerRef.current?.focus() }}>Cancel</Button><Button variant="primary" size="sm" disabled={importing || selectedImports.size === 0} onClick={() => void runImport()}>Import selected</Button></div>
          </div></div>}
          {importResults.length > 0 && <div role="status" className="mb-3 flex items-start gap-2 rounded-md p-2 text-xs" style={{ border: '1px solid var(--border)', color: 'var(--text-2)', background: 'var(--surface-up)' }}><div className="min-w-0 flex-1">{importResults.map((result) => <div key={`${result.sourcePath}-${result.status}`}>{result.status}: {result.message}</div>)}</div><IconButton size="sm" icon={<X size={13} />} label="Dismiss import results" onClick={() => setImportResults([])} /></div>}

          {loading ? <div className="text-sm" style={{ color: 'var(--text-3)' }}>Loading Skills...</div> : filtered.length === 0 ? <div className="flex flex-col items-center gap-2 rounded-xl p-8 text-center animate-fade-in" style={{ color: 'var(--text-3)', border: '1px solid var(--border)', background: 'var(--surface-up)' }}><BookOpen size={24} strokeWidth={1.5} aria-hidden /><div className="text-sm font-medium" style={{ color: 'var(--text-2)' }}>{search.trim() || filter !== 'all' ? 'No entries match this view' : 'No skills yet'}</div></div> : <div className="grid h-full min-h-0 gap-4 lg:grid-cols-[minmax(260px,0.8fr)_minmax(0,1.2fr)]">
            <div className="grid min-h-0 content-start gap-2 overflow-y-auto pr-1">{filtered.map((entry) => <div key={entry.filePath} className="flex items-start gap-2 p-3 transition-[border-color,background,transform] duration-150 hover:-translate-y-px" style={{ border: `1px solid ${selectedPath === entry.filePath ? 'var(--accent)' : 'var(--border)'}`, borderRadius: 8, background: selectedPath === entry.filePath ? 'var(--accent-dim)' : 'transparent' }}><button type="button" onClick={() => setSelectedPath(entry.filePath)} className="min-w-0 flex-1 text-left" style={{ background: 'transparent', border: 0, padding: 0 }}><div className="text-sm font-medium truncate" style={{ color: 'var(--text)' }}>{entry.title}</div><div className="mt-1 flex flex-wrap gap-x-3 text-[11px]" style={{ color: 'var(--text-3)' }}>{entry.group && <span>{entry.group}</span>}{entry.maker && <span>{entry.maker}</span>}{entry.sourceProvider && <span>{entry.sourceProvider}</span>}</div>{(entry.review || entry.preview) && <div className="mt-2 text-xs line-clamp-2" style={{ color: 'var(--text-2)' }}>{entry.review || entry.preview}</div>}</button><IconButton size="sm" icon={<Star size={15} fill={entry.favorite ? 'currentColor' : 'none'} />} label={`${entry.favorite ? 'Remove' : 'Add'} ${entry.title} ${entry.favorite ? 'from' : 'to'} favorites`} onClick={() => void setFavorite(entry, !entry.favorite)} style={{ color: entry.favorite ? 'var(--yellow)' : 'var(--text-3)' }} /></div>)}</div>
            <div className="min-h-0 overflow-y-auto rounded-lg p-4" style={{ border: '1px solid var(--border)', background: 'var(--bg)' }}>{selected ? <><div className="flex items-start justify-between gap-3"><div><h3 className="text-base font-semibold" style={{ color: 'var(--text)' }}>{selected.title}</h3><div className="mt-1 text-xs" style={{ color: 'var(--text-3)' }}>{selected.review || selected.preview}</div></div><IconButton icon={<Pencil size={15} />} label={`Edit ${selected.title} in app`} onClick={() => beginEdit(selected)} /></div><div className="my-4 rounded-md p-3" style={{ border: '1px solid var(--border)', background: 'var(--surface)', color: 'var(--text)' }}><MarkdownContent content={selected.body ?? selected.preview} /></div><div className="mb-3 grid gap-1 text-[11px]" style={{ color: 'var(--text-3)' }}>{selected.commandName && <span>Favorite command: /{selected.commandName}</span>}{selected.sourceProvider && <span>Imported from {selected.sourceProvider}: {selected.sourcePath}</span>}<span className="truncate">{selected.filePath}</span></div><div className="flex flex-wrap gap-1"><IconButton icon={<Paperclip size={15} />} label="Attach to message" variant={activeSessionId ? 'solid' : 'ghost'} disabled={!activeSessionId} onClick={() => attach(selected)} /><IconButton icon={<BookOpen size={15} />} label="Open in external editor" onClick={() => void editEntry(selected)} /><IconButton icon={<ExternalLink size={15} />} label="Reveal file" onClick={() => void revealEntry(selected)} /></div></> : <div className="flex h-full items-center justify-center text-sm" style={{ color: 'var(--text-3)' }}>Select an entry to preview it.</div>}</div>
          </div>}
        </div>
      </div>
    </div>
  )
}
