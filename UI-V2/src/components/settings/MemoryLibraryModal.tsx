import { useEffect, useMemo, useState } from 'react'
import { useAppStore } from '../../store/useAppStore'
import type { MemoryEntryDraft } from '../../types/memory'

const MEMORY_CATEGORIES = [
  'Failures/AI_Failures',
  'Failures/User_Failures',
  'Lessons/AI_Lessons',
  'Lessons/User_Lessons',
] as const

const MEMORY_CONFIDENCE = ['high', 'medium', 'low'] as const

const EMPTY_DRAFT: MemoryEntryDraft = {
  category: MEMORY_CATEGORIES[2],
  title: '',
  memory: '',
  evidence: '',
  confidence: 'medium',
  sourceChatId: '',
}

export function MemoryLibraryModal() {
  const {
    memoryLibraryScope,
    memoryLibraryEntries,
    memoryLibraryLoading,
    memoryLibraryError,
    closeMemoryLibrary,
    refreshMemoryLibrary,
    createMemoryEntry,
    deleteMemoryEntry,
    openMemoryRoot,
    revealMemoryEntry,
    folders,
  } = useAppStore()
  const [searchQuery, setSearchQuery] = useState('')
  const [isAdding, setIsAdding] = useState(false)
  const [draft, setDraft] = useState<MemoryEntryDraft>(EMPTY_DRAFT)
  const [submitting, setSubmitting] = useState(false)
  const [pendingDeleteEntryId, setPendingDeleteEntryId] = useState<string | null>(null)

  useEffect(() => {
    if (!memoryLibraryScope) {
      return
    }

    const handler = (event: KeyboardEvent) => {
      if (event.key === 'Escape') {
        closeMemoryLibrary()
      }
    }

    window.addEventListener('keydown', handler)
    return () => window.removeEventListener('keydown', handler)
  }, [closeMemoryLibrary, memoryLibraryScope])

  const filteredEntries = useMemo(() => {
    const query = searchQuery.trim().toLowerCase()
    if (!query) {
      return memoryLibraryEntries
    }

    return memoryLibraryEntries.filter((entry) =>
      [
        entry.title,
        entry.category,
        entry.preview,
        entry.sourceChatId,
        entry.scopeLabel ?? '',
        entry.rootPath ?? '',
      ].some((value) => value.toLowerCase().includes(query))
    )
  }, [memoryLibraryEntries, searchQuery])

  const groupedEntries = useMemo(() => {
    return MEMORY_CATEGORIES.map((category) => ({
      category,
      entries: filteredEntries.filter((entry) => entry.category === category),
    }))
  }, [filteredEntries])

  if (!memoryLibraryScope) {
    return null
  }

  const isAllMemory = memoryLibraryScope.scopeType === 'all'
  const memoryTitle = memoryLibraryScope.scopeType === 'global'
    ? 'Global Memory'
    : isAllMemory
      ? 'All Memory'
      : `${memoryLibraryScope.label} Memory`
  const targetValue = draft.targetScopeType === 'folder' && draft.targetFolderId
    ? `folder:${draft.targetFolderId}`
    : 'global'

  const submitDraft = async () => {
    if (!draft.title.trim() || !draft.memory.trim()) {
      return
    }

    setSubmitting(true)
    const saveDraft: MemoryEntryDraft = {
      ...draft,
      title: draft.title.trim(),
      memory: draft.memory.trim(),
      evidence: draft.evidence.trim(),
      sourceChatId: draft.sourceChatId.trim(),
    }
    if (isAllMemory && !saveDraft.targetScopeType) {
      saveDraft.targetScopeType = 'global'
      saveDraft.targetFolderId = ''
    }
    const ok = await createMemoryEntry(saveDraft)
    setSubmitting(false)

    if (!ok) {
      return
    }

    setDraft(EMPTY_DRAFT)
    setIsAdding(false)
  }

  const pendingDelete = memoryLibraryEntries.find((entry) => entry.id === pendingDeleteEntryId) ?? null

  return (
    <div
      className="fixed inset-0 z-50 flex items-center justify-center animate-fade-in"
      style={{ background: 'rgba(0,0,0,0.55)', backdropFilter: 'blur(4px)' }}
      onClick={(event) => {
        if (event.target === event.currentTarget) closeMemoryLibrary()
      }}
    >
      <div
        className="rounded-2xl shadow-2xl w-full max-w-6xl mx-4 animate-slide-in overflow-hidden flex flex-col"
        style={{ background: 'var(--surface)', border: '1px solid var(--border-bright)', maxHeight: 'calc(100vh - 2rem)' }}
      >
        <div
          className="flex items-center justify-between px-5 py-4"
          style={{ borderBottom: '1px solid var(--border)' }}
        >
          <div>
            <div className="text-sm font-semibold" style={{ color: 'var(--text)' }}>
              {memoryTitle}
            </div>
            <div className="text-xs mt-1" style={{ color: 'var(--text-3)' }}>
              {memoryLibraryScope.rootPath}
            </div>
          </div>
          <button
            type="button"
            onClick={closeMemoryLibrary}
            style={{
              background: 'transparent',
              color: 'var(--text-3)',
              border: 'none',
              cursor: 'pointer',
              fontFamily: 'inherit',
              fontSize: 12,
            }}
          >
            ✕
          </button>
        </div>

        <div className="grid lg:grid-cols-[320px_minmax(0,1fr)] min-h-[620px] flex-1 min-h-0">
          <aside
            className="p-5 space-y-4 overflow-y-auto"
            style={{
              background: 'color-mix(in srgb, var(--surface-up) 72%, var(--surface))',
              borderRight: '1px solid var(--border)',
            }}
          >
            <div className="space-y-2">
              {!isAllMemory && (
                <button
                  type="button"
                  onClick={() => void openMemoryRoot()}
                  className="w-full rounded-md px-3 py-2 text-left text-xs"
                  style={{ background: 'var(--surface)', color: 'var(--text)', border: '1px solid var(--border)' }}
                >
                  Open memory root
                </button>
              )}
              <button
                type="button"
                onClick={() => void refreshMemoryLibrary()}
                className="w-full rounded-md px-3 py-2 text-left text-xs"
                style={{ background: 'var(--surface)', color: 'var(--text)', border: '1px solid var(--border)' }}
              >
                Refresh list
              </button>
              <button
                type="button"
                onClick={() => setIsAdding((value) => !value)}
                className="w-full rounded-md px-3 py-2 text-left text-xs"
                style={{ background: 'var(--accent)', color: '#fff', border: 'none' }}
              >
                {isAdding ? 'Close add form' : 'Add memory'}
              </button>
            </div>

            {isAdding && (
              <div
                className="rounded-xl p-3 space-y-3"
                style={{ background: 'var(--surface)', border: '1px solid var(--border)' }}
              >
                <div className="text-xs font-semibold" style={{ color: 'var(--text)' }}>
                  New memory
                </div>

                {isAllMemory && (
                  <label className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
                    Save to
                    <select
                      value={targetValue}
                      onChange={(event) => {
                        const value = event.currentTarget.value
                        if (value === 'global') {
                          setDraft((current) => ({ ...current, targetScopeType: 'global', targetFolderId: '' }))
                          return
                        }
                        const folderId = value.replace(/^folder:/, '')
                        setDraft((current) => ({ ...current, targetScopeType: 'folder', targetFolderId: folderId }))
                      }}
                      style={{ background: 'var(--surface-up)', color: 'var(--text)', border: '1px solid var(--border)', borderRadius: 8, padding: '8px 10px' }}
                    >
                      <option value="global">Global memory</option>
                      {folders
                        .filter((folder) => folder.directory.trim().length > 0)
                        .map((folder) => (
                          <option key={folder.id} value={`folder:${folder.id}`}>{folder.name}</option>
                        ))}
                    </select>
                  </label>
                )}

                <label className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
                  Category
                  <select
                    value={draft.category}
                    onChange={(event) => {
                      const value = event.currentTarget.value
                      setDraft((current) => ({ ...current, category: value }))
                    }}
                    style={{ background: 'var(--surface-up)', color: 'var(--text)', border: '1px solid var(--border)', borderRadius: 8, padding: '8px 10px' }}
                  >
                    {MEMORY_CATEGORIES.map((category) => (
                      <option key={category} value={category}>{category}</option>
                    ))}
                  </select>
                </label>

                <label className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
                  Title
                  <input
                    value={draft.title}
                    onChange={(event) => {
                      const value = event.currentTarget.value
                      setDraft((current) => ({ ...current, title: value }))
                    }}
                    style={{ background: 'var(--surface-up)', color: 'var(--text)', border: '1px solid var(--border)', borderRadius: 8, padding: '8px 10px' }}
                  />
                </label>

                <label className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
                  Memory
                  <textarea
                    value={draft.memory}
                    onChange={(event) => {
                      const value = event.currentTarget.value
                      setDraft((current) => ({ ...current, memory: value }))
                    }}
                    rows={5}
                    style={{ background: 'var(--surface-up)', color: 'var(--text)', border: '1px solid var(--border)', borderRadius: 8, padding: '8px 10px', resize: 'vertical' }}
                  />
                </label>

                <label className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
                  Evidence
                  <textarea
                    value={draft.evidence}
                    onChange={(event) => {
                      const value = event.currentTarget.value
                      setDraft((current) => ({ ...current, evidence: value }))
                    }}
                    rows={3}
                    style={{ background: 'var(--surface-up)', color: 'var(--text)', border: '1px solid var(--border)', borderRadius: 8, padding: '8px 10px', resize: 'vertical' }}
                  />
                </label>

                <div className="grid grid-cols-2 gap-2">
                  <label className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
                    Confidence
                    <select
                      value={draft.confidence}
                      onChange={(event) => {
                        const value = event.currentTarget.value
                        setDraft((current) => ({ ...current, confidence: value }))
                      }}
                      style={{ background: 'var(--surface-up)', color: 'var(--text)', border: '1px solid var(--border)', borderRadius: 8, padding: '8px 10px' }}
                    >
                      {MEMORY_CONFIDENCE.map((confidence) => (
                        <option key={confidence} value={confidence}>{confidence}</option>
                      ))}
                    </select>
                  </label>

                  <label className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
                    Source chat
                    <input
                      value={draft.sourceChatId}
                      onChange={(event) => {
                        const value = event.currentTarget.value
                        setDraft((current) => ({ ...current, sourceChatId: value }))
                      }}
                      style={{ background: 'var(--surface-up)', color: 'var(--text)', border: '1px solid var(--border)', borderRadius: 8, padding: '8px 10px' }}
                    />
                  </label>
                </div>

                <button
                  type="button"
                  disabled={submitting || !draft.title.trim() || !draft.memory.trim()}
                  onClick={() => void submitDraft()}
                  className="w-full rounded-md px-3 py-2 text-xs font-medium"
                  style={{ background: 'var(--accent)', color: '#fff', border: 'none', opacity: submitting ? 0.6 : 1 }}
                >
                  {submitting ? 'Saving...' : 'Save memory'}
                </button>
              </div>
            )}
          </aside>

          <div className="p-5 md:p-6 overflow-y-auto min-h-0">
            <div className="flex items-center gap-3 mb-4">
              <input
                value={searchQuery}
                onChange={(event) => {
                  const value = event.currentTarget.value
                  setSearchQuery(value)
                }}
                placeholder="Search memory titles, categories, previews, or source chats"
                className="w-full rounded-md px-3 py-2 text-sm outline-none"
                style={{ background: 'var(--surface-up)', color: 'var(--text)', border: '1px solid var(--border)' }}
              />
            </div>

            {memoryLibraryError && (
              <div className="mb-4 rounded-md px-3 py-2 text-xs" style={{ background: 'color-mix(in srgb, var(--red) 14%, transparent)', color: 'var(--red)', border: '1px solid color-mix(in srgb, var(--red) 35%, var(--border))' }}>
                {memoryLibraryError}
              </div>
            )}

            {memoryLibraryLoading ? (
              <div className="text-sm" style={{ color: 'var(--text-3)' }}>
                Loading memory library...
              </div>
            ) : filteredEntries.length === 0 ? (
              <div
                className="rounded-xl p-6 text-sm"
                style={{ background: 'var(--surface-up)', border: '1px solid var(--border)', color: 'var(--text-3)' }}
              >
                No memory entries found for this scope.
              </div>
            ) : (
              <div className="space-y-5">
                {groupedEntries.map(({ category, entries }) => (
                  <section key={category}>
                    <div className="text-[11px] font-semibold uppercase tracking-[0.16em] mb-2" style={{ color: 'var(--text-3)' }}>
                      {category}
                    </div>
                    {entries.length === 0 ? (
                      <div className="rounded-lg px-3 py-2 text-xs" style={{ background: 'var(--surface-up)', border: '1px solid var(--border)', color: 'var(--text-3)' }}>
                        No entries in this category.
                      </div>
                    ) : (
                      <div className="space-y-3">
                        {entries.map((entry) => (
                          <article
                            key={entry.id}
                            className="rounded-xl p-4"
                            style={{ background: 'var(--surface-up)', border: '1px solid var(--border)' }}
                          >
                            <div className="flex items-start justify-between gap-4">
                              <div className="min-w-0">
                                <div className="text-sm font-semibold truncate" style={{ color: 'var(--text)' }}>
                                  {entry.title}
                                </div>
                                <div className="text-xs mt-1" style={{ color: 'var(--text-3)' }}>
                                  {isAllMemory ? (entry.scopeLabel || entry.scope) : entry.scope} • {entry.confidence} confidence • {entry.occurrenceCount} occurrence{entry.occurrenceCount === 1 ? '' : 's'}
                                </div>
                                {isAllMemory && entry.rootPath && (
                                  <div className="text-[11px] mt-1 truncate" style={{ color: 'var(--text-3)' }}>
                                    {entry.rootPath}
                                  </div>
                                )}
                              </div>
                              <div className="flex items-center gap-2">
                                <button
                                  type="button"
                                  onClick={() => void revealMemoryEntry(entry.id)}
                                  className="px-2 py-1 rounded-md text-[11px]"
                                  style={{ background: 'transparent', color: 'var(--text-2)', border: '1px solid var(--border)' }}
                                >
                                  Reveal file
                                </button>
                                <button
                                  type="button"
                                  onClick={() => setPendingDeleteEntryId(entry.id)}
                                  className="px-2 py-1 rounded-md text-[11px]"
                                  style={{ background: 'transparent', color: 'var(--red)', border: '1px solid var(--border)' }}
                                >
                                  Delete
                                </button>
                              </div>
                            </div>

                            <div className="grid md:grid-cols-2 gap-2 mt-3 text-xs" style={{ color: 'var(--text-3)' }}>
                              <div>Source chat: {entry.sourceChatId || '—'}</div>
                              <div>Last observed: {entry.lastObserved || '—'}</div>
                            </div>

                            <div className="mt-3 text-sm leading-6" style={{ color: 'var(--text-2)' }}>
                              {entry.preview || 'No preview available.'}
                            </div>
                          </article>
                        ))}
                      </div>
                    )}
                  </section>
                ))}
              </div>
            )}
          </div>
        </div>
      </div>

      {pendingDelete && (
        <div
          className="fixed inset-0 z-[60] flex items-center justify-center"
          style={{ background: 'rgba(0,0,0,0.25)' }}
          onClick={(event) => {
            if (event.target === event.currentTarget) setPendingDeleteEntryId(null)
          }}
        >
          <div
            className="rounded-xl shadow-2xl w-full max-w-md mx-4"
            style={{ background: 'var(--surface)', border: '1px solid var(--border-bright)' }}
          >
            <div className="px-5 py-4" style={{ borderBottom: '1px solid var(--border)' }}>
              <div className="text-sm font-semibold" style={{ color: 'var(--text)' }}>
                Delete memory entry?
              </div>
            </div>
            <div className="p-5 space-y-4">
              <div className="text-sm" style={{ color: 'var(--text)' }}>
                This removes "{pendingDelete.title}" from the memory library and deletes the backing markdown file.
              </div>
            </div>
            <div className="flex items-center justify-end gap-2 px-5 py-4" style={{ borderTop: '1px solid var(--border)' }}>
              <button
                type="button"
                onClick={() => setPendingDeleteEntryId(null)}
                className="px-4 py-1.5 rounded-md text-xs"
                style={{ background: 'transparent', color: 'var(--text-2)', border: '1px solid var(--border)' }}
              >
                Cancel
              </button>
              <button
                type="button"
                onClick={() => {
                  void deleteMemoryEntry(pendingDelete.id)
                  setPendingDeleteEntryId(null)
                }}
                className="px-4 py-1.5 rounded-md text-xs font-medium"
                style={{ background: 'var(--red)', color: '#fff', border: 'none' }}
              >
                Delete memory
              </button>
            </div>
          </div>
        </div>
      )}
    </div>
  )
}
