import { useEffect, useMemo, useState } from 'react'
import { FolderOpen, X, Check, ChevronUp, ChevronDown, SearchX } from 'lucide-react'
import { useAppStore } from '../../store/useAppStore'
import { useShallow } from 'zustand/react/shallow'
import { Button, IconButton } from '../ui'
import type { Folder } from '../../types/session'
import type { MemoryEntry, MemoryEntryDraft, MemoryScope } from '../../types/memory'

const MEMORY_CATEGORIES = [
  'Failures/AI_Failures',
  'Failures/User_Failures',
  'Lessons/AI_Lessons',
  'Lessons/User_Lessons',
] as const

const MEMORY_CONFIDENCE = ['high', 'medium', 'low'] as const

const memoryCategoryLabel = (category: string) => ({
  'Failures/AI_Failures': 'AI failures',
  'Failures/User_Failures': 'Your failures',
  'Lessons/AI_Lessons': 'AI lessons',
  'Lessons/User_Lessons': 'Your lessons',
}[category] ?? category.replace(/_/g, ' ').replace('/', ' · '))

const EMPTY_DRAFT: MemoryEntryDraft = {
  category: MEMORY_CATEGORIES[2],
  title: '',
  memory: '',
  evidence: '',
  confidence: 'medium',
  sourceChatId: '',
}

interface MemoryCategoryGroup {
  category: string
  entries: MemoryEntry[]
}

interface MemoryLocationGroup {
  key: string
  label: string
  rootPath: string
  count: number
  sortIndex: number
  categories: MemoryCategoryGroup[]
}

interface MenuOption {
  value: string
  label: string
}

function InlineMenu({
  label,
  value,
  options,
  onChange,
}: {
  label: string
  value: string
  options: MenuOption[]
  onChange: (value: string) => void
}) {
  const [open, setOpen] = useState(false)
  const selectedOption = options.find((option) => option.value === value) ?? options[0]

  return (
    <div className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
      <span>{label}</span>
      <div className="relative">
        <button
          type="button"
          aria-haspopup="listbox"
          aria-expanded={open}
          className="flex w-full items-center justify-between gap-2 text-left"
          onClick={() => setOpen((current) => !current)}
          style={{ background: 'var(--surface-up)', color: 'var(--text)', border: '1px solid var(--border)', borderRadius: 8, padding: '8px 10px' }}
        >
          <span className="min-w-0 truncate">{selectedOption?.label ?? value}</span>
          <span aria-hidden="true" style={{ color: 'var(--text-3)', display: 'inline-flex' }}>{open ? <ChevronUp size={13} /> : <ChevronDown size={13} />}</span>
        </button>
        {open && (
          <div
            role="listbox"
            className="absolute left-0 right-0 top-full z-40 mt-1 max-h-56 overflow-y-auto"
            style={{ background: 'var(--surface-up)', border: '1px solid var(--border)', borderRadius: 8, boxShadow: '0 10px 24px rgba(0,0,0,0.24)' }}
          >
            {options.map((option) => {
              const selected = option.value === value
              return (
                <button
                  key={option.value}
                  type="button"
                  role="option"
                  aria-selected={selected}
                  className="flex w-full items-center justify-between gap-2 px-3 py-2 text-left"
                  onClick={() => {
                    onChange(option.value)
                    setOpen(false)
                  }}
                  style={{ background: selected ? 'var(--accent-dim)' : 'transparent', color: selected ? 'var(--text)' : 'var(--text-2)' }}
                >
                  <span className="min-w-0 truncate">{option.label}</span>
                  {selected && <Check size={13} style={{ color: 'var(--green)' }} aria-hidden />}
                </button>
              )
            })}
          </div>
        )}
      </div>
    </div>
  )
}

function memoryLocationKey(entry: MemoryEntry, scope: MemoryScope): string {
  if (entry.scopeType === 'global' || entry.scope === 'global') {
    return 'global'
  }

  if (entry.scopeType === 'folder' && entry.folderId) {
    return `folder:${entry.folderId}`
  }

  if (scope.scopeType === 'folder' && scope.folderId) {
    return `folder:${scope.folderId}`
  }

  const rootPath = (entry.rootPath || scope.rootPath || '').trim()
  return rootPath ? `root:${rootPath}` : `scope:${scope.scopeType}:${entry.scope || 'local'}`
}

function memoryLocationLabel(entry: MemoryEntry, scope: MemoryScope): string {
  if (entry.scopeType === 'global' || entry.scope === 'global') {
    return 'Global memory'
  }

  const scopeLabel = (entry.scopeLabel || '').trim()
  if (scopeLabel) {
    return scopeLabel
  }

  if (scope.scopeType === 'folder') {
    return scope.label
  }

  return entry.scope === 'global' ? 'Global memory' : 'Project memory'
}

function memoryLocationRootPath(entry: MemoryEntry, scope: MemoryScope): string {
  if (entry.rootPath) {
    return entry.rootPath
  }

  if (entry.scopeType === 'global' || entry.scope === 'global') {
    return 'Global memory root'
  }

  return scope.rootPath
}

function buildMemoryLocationGroups(
  entries: MemoryEntry[],
  scope: MemoryScope | null,
  folders: Folder[],
): MemoryLocationGroup[] {
  if (!scope) {
    return []
  }

  const folderOrder = new Map(folders.map((folder, index) => [folder.id, index]))
  const groups = new Map<string, {
    label: string
    rootPath: string
    sortIndex: number
    entries: MemoryEntry[]
  }>()

  for (const entry of entries) {
    const key = memoryLocationKey(entry, scope)
    const folderIndex = key.startsWith('folder:')
      ? folderOrder.get(key.replace(/^folder:/, ''))
      : undefined
    const sortIndex = key === 'global'
      ? -1
      : folderIndex !== undefined
        ? folderIndex
        : 10000
    const existing = groups.get(key)
    if (existing) {
      existing.entries.push(entry)
      continue
    }

    groups.set(key, {
      label: memoryLocationLabel(entry, scope),
      rootPath: memoryLocationRootPath(entry, scope),
      sortIndex,
      entries: [entry],
    })
  }

  return Array.from(groups.entries())
    .map(([key, group]) => ({
      key,
      label: group.label,
      rootPath: group.rootPath,
      count: group.entries.length,
      sortIndex: group.sortIndex,
      categories: MEMORY_CATEGORIES
        .map((category) => ({
          category,
          entries: group.entries.filter((entry) => entry.category === category),
        }))
        .filter((categoryGroup) => categoryGroup.entries.length > 0),
    }))
    .sort((left, right) => {
      if (left.sortIndex !== right.sortIndex) {
        return left.sortIndex - right.sortIndex
      }
      return left.label.localeCompare(right.label)
    })
}

export function MemoryLibraryModal() {
  const memoryLibraryScope = useAppStore((s) => s.memoryLibraryScope)
  const memoryLibraryEntries = useAppStore(useShallow((s) => s.memoryLibraryEntries))
  const memoryLibraryLoading = useAppStore((s) => s.memoryLibraryLoading)
  const memoryLibraryError = useAppStore((s) => s.memoryLibraryError)
  const closeMemoryLibrary = useAppStore((s) => s.closeMemoryLibrary)
  const refreshMemoryLibrary = useAppStore((s) => s.refreshMemoryLibrary)
  const createMemoryEntry = useAppStore((s) => s.createMemoryEntry)
  const deleteMemoryEntry = useAppStore((s) => s.deleteMemoryEntry)
  const deleteMemoryEntries = useAppStore((s) => s.deleteMemoryEntries)
  const openMemoryRoot = useAppStore((s) => s.openMemoryRoot)
  const revealMemoryEntry = useAppStore((s) => s.revealMemoryEntry)
  const folders = useAppStore(useShallow((s) => s.folders))
  const [searchQuery, setSearchQuery] = useState('')
  const [isAdding, setIsAdding] = useState(false)
  const [draft, setDraft] = useState<MemoryEntryDraft>(EMPTY_DRAFT)
  const [submitting, setSubmitting] = useState(false)
  const [pendingDeleteEntryId, setPendingDeleteEntryId] = useState<string | null>(null)
  const [pendingMassDeleteEntryIds, setPendingMassDeleteEntryIds] = useState<string[] | null>(null)
  const [expandedLocationGroups, setExpandedLocationGroups] = useState<Record<string, boolean>>({})

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

  const groupedEntries = useMemo(
    () => buildMemoryLocationGroups(filteredEntries, memoryLibraryScope, folders),
    [filteredEntries, folders, memoryLibraryScope],
  )

  useEffect(() => {
    setExpandedLocationGroups((current) => {
      let changed = false
      const next: Record<string, boolean> = {}

      for (const group of groupedEntries) {
        next[group.key] = current[group.key] ?? true
        if (current[group.key] === undefined) {
          changed = true
        }
      }

      if (Object.keys(current).length !== Object.keys(next).length) {
        changed = true
      }

      return changed ? next : current
    })
  }, [groupedEntries])

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
  const targetOptions = [
    { value: 'global', label: 'Global memory' },
    ...folders
      .filter((folder) => folder.directory.trim().length > 0)
      .map((folder) => ({ value: `folder:${folder.id}`, label: folder.name })),
  ]
  const categoryOptions = MEMORY_CATEGORIES.map((category) => ({ value: category, label: memoryCategoryLabel(category) }))
  const confidenceOptions = MEMORY_CONFIDENCE.map((confidence) => ({ value: confidence, label: confidence }))

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
  const pendingMassDeleteCount = pendingMassDeleteEntryIds?.length ?? 0
  const visibleEntryIds = filteredEntries.map((entry) => entry.id)
  const hasSearchQuery = searchQuery.trim().length > 0
  const massDeleteLabel = hasSearchQuery ? 'Delete matches' : 'Delete all'
  const toggleLocationGroup = (key: string) => {
    setExpandedLocationGroups((current) => ({
      ...current,
      [key]: !(current[key] ?? true),
    }))
  }

  return (
    <div
      className="fixed inset-0 z-50 flex items-center justify-center animate-fade-in"
      style={{ background: 'rgba(0,0,0,0.55)', backdropFilter: 'blur(4px)' }}
      onClick={(event) => {
        if (event.target === event.currentTarget) closeMemoryLibrary()
      }}
    >
      <div
        role="dialog"
        aria-modal="true"
        aria-label={memoryTitle}
        tabIndex={-1}
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
          <IconButton
            icon={<X size={16} />}
            label="Close memory library"
            onClick={closeMemoryLibrary}
          />
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
                <Button
                  variant="secondary"
                  size="sm"
                  block
                  onClick={() => void openMemoryRoot()}
                >
                  Open memory root
                </Button>
              )}
              <Button
                variant="secondary"
                size="sm"
                block
                onClick={() => void refreshMemoryLibrary()}
              >
                Refresh list
              </Button>
              <Button
                variant={isAdding ? 'secondary' : 'primary'}
                size="sm"
                block
                onClick={() => setIsAdding((value) => !value)}
              >
                {isAdding ? 'Close add form' : 'Add memory'}
              </Button>
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
                  <InlineMenu
                    label="Save to"
                    value={targetValue}
                    options={targetOptions}
                    onChange={(value) => {
                      if (value === 'global') {
                        setDraft((current) => ({ ...current, targetScopeType: 'global', targetFolderId: '' }))
                        return
                      }
                      const folderId = value.replace(/^folder:/, '')
                      setDraft((current) => ({ ...current, targetScopeType: 'folder', targetFolderId: folderId }))
                    }}
                  />
                )}

                <InlineMenu
                  label="Category"
                  value={draft.category}
                  options={categoryOptions}
                  onChange={(value) => {
                    setDraft((current) => ({ ...current, category: value }))
                  }}
                />

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

                <div className="grid gap-2">
                  <InlineMenu
                    label="Confidence"
                    value={draft.confidence}
                    options={confidenceOptions}
                    onChange={(value) => {
                      setDraft((current) => ({ ...current, confidence: value }))
                    }}
                  />

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

                <Button
                  variant="primary"
                  block
                  size="sm"
                  disabled={submitting || !draft.title.trim() || !draft.memory.trim()}
                  onClick={() => void submitDraft()}
                >
                  {submitting ? 'Saving...' : 'Save memory'}
                </Button>
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
              <Button
                variant="danger"
                size="sm"
                disabled={memoryLibraryLoading || visibleEntryIds.length === 0}
                onClick={() => setPendingMassDeleteEntryIds(visibleEntryIds)}
              >
                {massDeleteLabel}
              </Button>
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
                className="rounded-xl p-8 text-center flex flex-col items-center gap-2 animate-fade-in"
                style={{ background: 'var(--surface-up)', border: '1px solid var(--border)', color: 'var(--text-3)' }}
              >
                {hasSearchQuery
                  ? <SearchX size={24} strokeWidth={1.5} aria-hidden />
                  : <FolderOpen size={24} strokeWidth={1.5} aria-hidden />}
                <div className="text-sm font-medium" style={{ color: 'var(--text-2)' }}>
                  {hasSearchQuery ? 'No memory matches this search' : 'No memory saved here yet'}
                </div>
                <div className="text-xs">
                  {hasSearchQuery ? 'Try another term or clear the search.' : 'Add a memory to make it available to future chats.'}
                </div>
              </div>
            ) : (
              <div className="space-y-4">
                {groupedEntries.map((location) => (
                  <section key={location.key}>
                    <button
                      type="button"
                      onClick={() => toggleLocationGroup(location.key)}
                      className="w-full relative flex items-center gap-2 px-3 py-2 cursor-pointer group rounded-md text-left"
                      style={{
                        background: 'var(--surface-up)',
                        color: 'var(--text-2)',
                        border: '1px solid var(--border)',
                        fontFamily: 'inherit',
                      }}
                    >
                      <FolderOpen
                        size={14}
                        style={{ flexShrink: 0, color: 'var(--accent)', opacity: 0.85 }}
                        aria-hidden="true"
                      />
                      <span className="min-w-0 flex-1">
                        <span className="block text-sm font-semibold truncate" style={{ color: 'var(--text)' }}>
                          {location.label}
                        </span>
                        <span className="block text-[11px] truncate mt-0.5" style={{ color: 'var(--text-3)' }}>
                          {location.rootPath}
                        </span>
                      </span>
                      <span
                        className="text-xs flex-shrink-0 rounded px-1"
                        style={{ fontSize: 10, background: 'var(--surface-high)', color: 'var(--text-3)' }}
                      >
                        {location.count}
                      </span>
                    </button>

                    {(expandedLocationGroups[location.key] ?? true) && (
                      <div className="mt-3 ml-4 space-y-4">
                        {location.categories.map(({ category, entries }) => (
                          <section key={`${location.key}:${category}`}>
                            <div className="flex items-center justify-between gap-2 mb-2">
                              <div className="text-[11px] font-semibold uppercase tracking-[0.16em]" style={{ color: 'var(--text-3)' }}>
                                {memoryCategoryLabel(category)}
                              </div>
                              <div className="text-[10px] rounded px-1.5 py-0.5" style={{ background: 'var(--surface-up)', color: 'var(--text-3)', border: '1px solid var(--border)' }}>
                                {entries.length}
                              </div>
                            </div>
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
                                        {entry.scope} • {entry.confidence} confidence • {entry.occurrenceCount} occurrence{entry.occurrenceCount === 1 ? '' : 's'}
                                      </div>
                                    </div>
                                    <div className="flex items-center gap-2">
                                      <Button
                                        variant="secondary"
                                        size="sm"
                                        onClick={() => void revealMemoryEntry(entry.id)}
                                      >
                                        Reveal file
                                      </Button>
                                      <Button
                                        variant="danger"
                                        size="sm"
                                        onClick={() => setPendingDeleteEntryId(entry.id)}
                                      >
                                        Delete
                                      </Button>
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
                          </section>
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
            role="dialog"
            aria-modal="true"
            aria-label="Delete memory entry"
            tabIndex={-1}
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
              <Button
                variant="secondary"
                size="sm"
                onClick={() => setPendingDeleteEntryId(null)}
              >
                Cancel
              </Button>
              <Button
                variant="danger"
                size="sm"
                onClick={() => {
                  void deleteMemoryEntry(pendingDelete.id)
                  setPendingDeleteEntryId(null)
                }}
              >
                Delete memory
              </Button>
            </div>
          </div>
        </div>
      )}

      {pendingMassDeleteEntryIds && (
        <div
          className="fixed inset-0 z-[60] flex items-center justify-center"
          style={{ background: 'rgba(0,0,0,0.25)' }}
          onClick={(event) => {
            if (event.target === event.currentTarget) setPendingMassDeleteEntryIds(null)
          }}
        >
          <div
            role="dialog"
            aria-modal="true"
            aria-label={hasSearchQuery ? 'Delete matching memories' : 'Delete all memories'}
            tabIndex={-1}
            className="rounded-xl shadow-2xl w-full max-w-md mx-4"
            style={{ background: 'var(--surface)', border: '1px solid var(--border-bright)' }}
          >
            <div className="px-5 py-4" style={{ borderBottom: '1px solid var(--border)' }}>
              <div className="text-sm font-semibold" style={{ color: 'var(--text)' }}>
                {hasSearchQuery ? 'Delete matching memories?' : 'Delete all memories?'}
              </div>
            </div>
            <div className="p-5 space-y-4">
              <div className="text-sm" style={{ color: 'var(--text)' }}>
                This deletes {pendingMassDeleteCount} {hasSearchQuery ? 'matching' : 'listed'} memory {pendingMassDeleteCount === 1 ? 'entry' : 'entries'} and removes the backing markdown {pendingMassDeleteCount === 1 ? 'file' : 'files'}.
              </div>
            </div>
            <div className="flex items-center justify-end gap-2 px-5 py-4" style={{ borderTop: '1px solid var(--border)' }}>
              <Button
                variant="secondary"
                size="sm"
                onClick={() => setPendingMassDeleteEntryIds(null)}
              >
                Cancel
              </Button>
              <Button
                variant="danger"
                size="sm"
                onClick={() => {
                  void deleteMemoryEntries(pendingMassDeleteEntryIds)
                  setPendingMassDeleteEntryIds(null)
                }}
              >
                Delete {pendingMassDeleteCount} {pendingMassDeleteCount === 1 ? 'memory' : 'memories'}
              </Button>
            </div>
          </div>
        </div>
      )}
    </div>
  )
}
