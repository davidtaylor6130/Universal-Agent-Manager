import { useEffect, useId, useMemo, useRef, useState, type KeyboardEvent } from 'react'
import { Check, GitBranch, LoaderCircle, Plus, Search } from 'lucide-react'
import { createRequestId, sendToCEF } from '../../ipc/cefBridge'
import { Button, Tooltip, ViewportMenu } from '../ui'

export interface ChatBranch {
  name: string
  current: boolean
  remote?: boolean
}

interface ListChatBranchesResponse {
  branches: ChatBranch[]
  vcsType: 'git' | 'svn'
}

const SVN_COPY = "Branch switching isn't available for SVN"

/** Composer branch tab that opens a branch picker. Only rendered when the backend advertises branchSwitching. */
export function ChatBranchPicker({ chatId, branch, disabled, disabledReason, onSwitched }: {
  chatId: string
  branch: string
  disabled: boolean
  disabledReason: string
  onSwitched: (branch: string) => void
}) {
  const [open, setOpen] = useState(false)
  const [loading, setLoading] = useState(false)
  const [loadError, setLoadError] = useState('')
  const [branches, setBranches] = useState<ChatBranch[]>([])
  const [vcsType, setVcsType] = useState<'git' | 'svn'>('git')
  const [query, setQuery] = useState('')
  const [highlight, setHighlight] = useState(0)
  const [creating, setCreating] = useState(false)
  const [newName, setNewName] = useState('')
  const [busy, setBusy] = useState(false)
  const [switchError, setSwitchError] = useState('')
  const triggerRef = useRef<HTMLButtonElement>(null)
  const menuRef = useRef<HTMLDivElement>(null)
  const searchRef = useRef<HTMLInputElement>(null)
  const listRef = useRef<HTMLDivElement>(null)
  const listboxId = useId()

  const filtered = useMemo(() => {
    const needle = query.trim().toLowerCase()
    return needle ? branches.filter((item) => item.name.toLowerCase().includes(needle)) : branches
  }, [branches, query])

  useEffect(() => {
    if (disabled) setOpen(false)
  }, [disabled])

  useEffect(() => {
    if (open) searchRef.current?.focus()
  }, [open])

  useEffect(() => {
    listRef.current?.querySelector<HTMLElement>('[aria-selected="true"]')?.scrollIntoView?.({ block: 'nearest' })
  }, [highlight])

  useEffect(() => {
    if (!open) return
    const onPointerDown = (event: MouseEvent) => {
      const target = event.target as Node
      if (!menuRef.current?.contains(target) && !triggerRef.current?.contains(target)) setOpen(false)
    }
    document.addEventListener('mousedown', onPointerDown)
    return () => document.removeEventListener('mousedown', onPointerDown)
  }, [open])

  const openPicker = async () => {
    setOpen(true)
    setLoading(true)
    setLoadError('')
    setSwitchError('')
    setQuery('')
    setCreating(false)
    setNewName('')
    setHighlight(0)
    const response = await sendToCEF<ListChatBranchesResponse>({ action: 'listChatBranches', payload: { chatId }, requestId: createRequestId('list-branches') })
    setLoading(false)
    if (!response.ok || !response.data) {
      setLoadError(response.error || 'Branches could not be loaded.')
      return
    }
    setBranches(response.data.branches ?? [])
    setVcsType(response.data.vcsType)
  }

  const switchTo = async (name: string, create = false) => {
    if (busy || disabled) return
    if (!create && branches.find((item) => item.name === name)?.current) {
      setOpen(false)
      return
    }
    setBusy(true)
    setSwitchError('')
    const response = await sendToCEF<{ branch: string }>({
      action: 'switchChatBranch',
      payload: create ? { chatId, branch: name, create: true } : { chatId, branch: name },
      requestId: createRequestId('switch-branch'),
    })
    setBusy(false)
    if (!response.ok) {
      setSwitchError(response.error || 'Branch could not be switched.')
      return
    }
    setOpen(false)
    onSwitched(response.data?.branch || name)
  }

  const closeToTrigger = () => {
    setOpen(false)
    triggerRef.current?.focus()
  }

  const onSearchKeyDown = (event: KeyboardEvent<HTMLInputElement>) => {
    if (event.key === 'ArrowDown' || event.key === 'ArrowUp') {
      event.preventDefault()
      const last = Math.max(0, filtered.length - 1)
      setHighlight((current) => event.key === 'ArrowDown' ? Math.min(current + 1, last) : Math.max(current - 1, 0))
    } else if (event.key === 'Enter') {
      event.preventDefault()
      const choice = filtered[highlight]
      if (choice) void switchTo(choice.name)
    }
  }

  const canCreate = vcsType === 'git' && !loading && !loadError

  const trigger = (
    <button
      ref={triggerRef}
      type="button"
      disabled={disabled}
      title={branch}
      aria-label={`Switch branch, current ${branch}`}
      aria-haspopup="listbox"
      aria-expanded={open}
      aria-controls={open ? listboxId : undefined}
      onClick={() => (open ? setOpen(false) : void openPicker())}
      className="inline-flex min-w-0 items-center gap-1 truncate rounded-sm px-1 text-xs disabled:opacity-50"
      style={{ color: 'inherit', cursor: disabled ? 'default' : 'pointer' }}
    >
      <GitBranch size={12} aria-hidden />
      <span className="truncate font-mono">{branch}</span>
    </button>
  )

  return (
    <>
      <Tooltip label={disabled ? disabledReason : undefined}>
        <span className="inline-flex min-w-0">{trigger}</span>
      </Tooltip>
      {open && (
        <ViewportMenu
          ref={menuRef}
          anchorRef={triggerRef}
          side="top"
          role="dialog"
          aria-label="Switch branch"
          onKeyDown={(event) => { if (event.key === 'Escape') closeToTrigger() }}
          className="animate-fade-in"
          style={{ width: 280, border: '1px solid var(--border-bright)', borderRadius: 8, background: 'var(--surface)', boxShadow: 'var(--elev-3)', padding: 6 }}
        >
          {loading ? (
            <div className="flex items-center gap-2 px-2 py-2 text-xs" style={{ color: 'var(--text-3)' }} role="status">
              <LoaderCircle size={14} className="animate-spin" aria-hidden />
              Loading branches
            </div>
          ) : loadError ? (
            <div role="alert" className="truncate px-2 py-2 text-xs" style={{ color: 'var(--error)' }} title={loadError}>{loadError}</div>
          ) : vcsType === 'svn' ? (
            <div className="px-2 py-2 text-xs" style={{ color: 'var(--text-2)' }}>{SVN_COPY}</div>
          ) : (
            <>
              <div className="flex items-center gap-2 px-2 py-1" style={{ borderBottom: '1px solid var(--border)' }}>
                <Search size={14} aria-hidden style={{ color: 'var(--text-3)' }} />
                <input
                  ref={searchRef}
                  type="text"
                  role="combobox"
                  aria-expanded
                  aria-controls={listboxId}
                  aria-activedescendant={filtered[highlight] ? `${listboxId}-${highlight}` : undefined}
                  aria-label="Search branches"
                  placeholder="Search branches"
                  value={query}
                  onChange={(event) => { setQuery(event.target.value); setHighlight(0) }}
                  onKeyDown={onSearchKeyDown}
                  className="min-w-0 flex-1 bg-transparent py-1.5 text-[13px] outline-none"
                  style={{ color: 'var(--text)' }}
                />
              </div>
              <div ref={listRef} id={listboxId} role="listbox" aria-label="Local branches" className="overflow-y-auto" style={{ maxHeight: 240 }}>
                {filtered.length === 0 && (
                  <div className="px-2 py-2 text-xs" style={{ color: 'var(--text-3)' }}>
                    {branches.length === 0 ? 'No local branches' : 'No branches match'}
                  </div>
                )}
                {filtered.map((item, index) => {
                  const active = index === highlight
                  return (
                    <button
                      key={`${item.remote ? 'remote' : 'local'}:${item.name}`}
                      id={`${listboxId}-${index}`}
                      type="button"
                      role="option"
                      aria-selected={active}
                      aria-checked={item.current}
                      disabled={busy}
                      onMouseEnter={() => setHighlight(index)}
                      onClick={() => void switchTo(item.name)}
                      className="flex w-full items-center gap-2 rounded-md px-2 py-1.5 text-left text-[13px]"
                      style={{ background: active ? 'var(--surface-up)' : 'transparent', color: 'var(--text)' }}
                    >
                      <span className="flex w-3.5 shrink-0 justify-center" aria-hidden>
                        {item.current && <Check size={14} style={{ color: 'var(--accent)' }} />}
                      </span>
                      <span className="min-w-0 flex-1 truncate font-mono text-xs">{item.name}</span>
                      {item.remote && <span className="shrink-0 text-[11px]" style={{ color: 'var(--text-3)' }}>remote</span>}
                    </button>
                  )
                })}
              </div>
              {canCreate && (
                <div className="mt-1 border-t pt-1" style={{ borderColor: 'var(--border)' }}>
                  {creating ? (
                    <div className="flex items-center gap-2 px-1 py-1">
                      <input
                        type="text"
                        autoFocus
                        aria-label="New branch name"
                        placeholder="new-branch-name"
                        value={newName}
                        onChange={(event) => setNewName(event.target.value)}
                        onKeyDown={(event) => {
                          if (event.key === 'Enter' && newName.trim()) { event.preventDefault(); void switchTo(newName.trim(), true) }
                          if (event.key === 'Escape') { event.stopPropagation(); setCreating(false) }
                        }}
                        className="min-w-0 flex-1 rounded-sm bg-transparent px-2 py-1 font-mono text-xs outline-none"
                        style={{ border: '1px solid var(--border-bright)', color: 'var(--text)' }}
                      />
                      <Button size="sm" variant="secondary" disabled={!newName.trim() || busy} loading={busy} onClick={() => void switchTo(newName.trim(), true)}>Create</Button>
                    </div>
                  ) : (
                    <button type="button" disabled={busy} onClick={() => setCreating(true)} className="flex w-full items-center gap-2 rounded-md px-2 py-1.5 text-left text-[13px]" style={{ color: 'var(--text-2)' }}>
                      <Plus size={14} aria-hidden />
                      Create branch from current…
                    </button>
                  )}
                </div>
              )}
            </>
          )}
          {switchError && (
            <div role="alert" className="mt-1 truncate px-2 py-1 text-xs" style={{ color: 'var(--error)' }} title={switchError}>{switchError}</div>
          )}
        </ViewportMenu>
      )}
    </>
  )
}
