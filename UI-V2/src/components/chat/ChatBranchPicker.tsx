import { useEffect, useId, useMemo, useRef, useState, type KeyboardEvent } from 'react'
import { AlertTriangle, Check, GitBranch, GitBranchPlus, LoaderCircle, Plus, Search } from 'lucide-react'
import { createRequestId, sendToCEF } from '../../ipc/cefBridge'
import { useAppStore } from '../../store/useAppStore'
import { chatWorktreeBranches, rankBranches } from '../../utils/branchRecency'
import { Button, Tooltip, ViewportMenu } from '../ui'

export interface ChatBranch {
  name: string
  current: boolean
  remote?: boolean
  path?: string
}

interface SvnBranchContext {
  directory: string
  url: string
  repository: string
  uuid: string
  revision: string
  parent: string
  repositoryRevision: string
}

function svnDisplayPath(context?: SvnBranchContext): string {
  if (context?.url === context?.repository && context) return '^/'
  return context && context.url.startsWith(`${context.repository}/`) ? `^/${context.url.slice(context.repository.length + 1)}` : ''
}

interface ListChatBranchesResponse {
  branches: ChatBranch[]
  vcsType: 'git' | 'svn'
  svnContext?: SvnBranchContext
}

/** Composer branch tab that opens a branch picker. Only rendered when the backend advertises branchSwitching. */
export function ChatBranchPicker({ chatId, branch, pendingBranch, disabled, disabledReason, onSwitched, vcsType: initialVcsType = 'git' }: {
  chatId: string
  branch: string
  /** New branch chosen before the first message; native creates it when that message is sent. */
  pendingBranch?: string
  disabled: boolean
  disabledReason: string
  onSwitched: (branch: string, vcsType?: 'git' | 'svn') => void
  vcsType?: 'git' | 'svn'
}) {
  const [open, setOpen] = useState(false)
  const [loading, setLoading] = useState(false)
  const [loadError, setLoadError] = useState('')
  const [branches, setBranches] = useState<ChatBranch[]>([])
  const [vcsType, setVcsType] = useState<'git' | 'svn'>(initialVcsType)
  const [svnParent, setSvnParent] = useState('')
  const [svnPath, setSvnPath] = useState('')
  const [svnRevision, setSvnRevision] = useState('')
  const [svnContext, setSvnContext] = useState<SvnBranchContext | null>(null)
  const [svnSelection, setSvnSelection] = useState<ChatBranch | null>(null)
  const [svnContextError, setSvnContextError] = useState('')
  const requestEpoch = useRef(0)
  const busyRef = useRef(false)
  const mountedRef = useRef(true)
  const [query, setQuery] = useState('')
  const [highlight, setHighlight] = useState(0)
  const [creating, setCreating] = useState(false)
  const [newName, setNewName] = useState('')
  const [busy, setBusy] = useState(false)
  const [switchError, setSwitchError] = useState('')
  const triggerRef = useRef<HTMLButtonElement>(null)
  const menuRef = useRef<HTMLDivElement>(null)
  const searchRef = useRef<HTMLInputElement>(null)
  const svnParentRef = useRef<HTMLInputElement>(null)
  const svnConfirmRef = useRef<HTMLButtonElement>(null)
  const restoreFocusRef = useRef(false)
  const listRef = useRef<HTMLDivElement>(null)
  const listboxId = useId()
  const dialogId = useId()

  const filtered = useMemo(() => {
    const needle = query.trim().toLowerCase()
    return needle ? branches.filter((item) => item.name.toLowerCase().includes(needle)) : branches
  }, [branches, query])

  useEffect(() => {
    mountedRef.current = true
    return () => { mountedRef.current = false; ++requestEpoch.current }
  }, [])

  useEffect(() => {
    if (initialVcsType !== 'svn' || disabled) return
    let cancelled = false
    const epoch = requestEpoch.current
    void sendToCEF<ListChatBranchesResponse>({ action: 'listChatBranches', payload: { chatId, vcsType: 'svn', svnParent: '' }, requestId: createRequestId('svn-context') }).then((response) => {
      if (cancelled || epoch !== requestEpoch.current) return
      setSvnContextError(response.ok ? '' : response.error || 'SVN folder could not be checked.')
      if (response.ok) {
        setSvnPath(svnDisplayPath(response.data?.svnContext))
        setSvnRevision(response.data?.svnContext?.revision || '')
      }
    })
    return () => { cancelled = true }
  }, [chatId, initialVcsType, disabled, branch])

  useEffect(() => {
    if (disabled) { setOpen(false); ++requestEpoch.current }
  }, [disabled])

  useEffect(() => {
    if (open && !loading) (searchRef.current ?? svnParentRef.current)?.focus()
  }, [open, loading, vcsType, svnContext?.parent])

  useEffect(() => {
    if (svnSelection) svnConfirmRef.current?.focus()
  }, [svnSelection])

  useEffect(() => {
    if (!open && !busy && restoreFocusRef.current) {
      restoreFocusRef.current = false
      triggerRef.current?.focus()
    }
  }, [open, busy])

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

  const openPicker = async (parent = svnParent) => {
    const epoch = ++requestEpoch.current
    setOpen(true)
    setLoading(true)
    setLoadError('')
    setSwitchError('')
    setQuery('')
    setCreating(false)
    setNewName('')
    setHighlight(0)
    setSvnSelection(null)
    setSvnContext(null)
    setBranches([])
    const response = await sendToCEF<ListChatBranchesResponse>({ action: 'listChatBranches', payload: vcsType === 'svn' ? { chatId, vcsType: 'svn', svnParent: parent.trim() } : { chatId }, requestId: createRequestId('list-branches') })
    if (epoch !== requestEpoch.current) return
    setLoading(false)
    if (!response.ok || !response.data) {
      setLoadError(response.error || 'Branches could not be loaded.')
      return
    }
    if (response.data.vcsType !== vcsType) {
      setLoadError(vcsType === 'git'
        ? 'This Git snapshot cannot switch its SVN source. Open a chat in the SVN working copy to select a repository folder.'
        : 'The repository type changed. Refresh this chat before selecting a repository folder.')
      return
    }
    // Git: native newest-commit order with other chats' checked-out worktree branches last. SVN keeps discovery order.
    const { sessions } = useAppStore.getState()
    const folderId = sessions.find((item) => item.id === chatId)?.folderId
    setBranches(response.data.vcsType === 'git'
      ? rankBranches(response.data.branches ?? [], chatWorktreeBranches(sessions, folderId, chatId))
      : response.data.branches ?? [])
    setVcsType(response.data.vcsType)
    setSvnContext(response.data.svnContext ?? null)
    if (response.data.vcsType === 'svn') {
      setSvnPath(svnDisplayPath(response.data.svnContext))
      setSvnRevision(response.data.svnContext?.revision || '')
    }
  }

  const switchTo = async (name: string, create = false) => {
    if (busyRef.current || disabled || (vcsType === 'svn' && (!svnContext || !svnContext.parent))) return
    // Reselecting the checked-out branch withdraws a pending one, so only skip native when nothing is pending.
    if (!create && !pendingBranch && branches.find((item) => (item.path || item.name) === name)?.current) {
      closeToTrigger()
      return
    }
    const hadDialogFocus = menuRef.current?.contains(document.activeElement) ?? false
    busyRef.current = true
    setBusy(true)
    setSwitchError('')
    const response = await sendToCEF<{ branch: string; pending?: boolean; vcsType?: 'git' | 'svn'; svnContext?: SvnBranchContext }>({
      action: 'switchChatBranch',
      payload: vcsType === 'svn' ? { chatId, vcsType: 'svn', branch: name, svnContext } : create ? { chatId, branch: name, create: true } : { chatId, branch: name },
      requestId: createRequestId('switch-branch'),
    })
    busyRef.current = false
    if (!mountedRef.current) return
    setBusy(false)
    const focusStillInPicker = document.activeElement === document.body || Boolean(menuRef.current?.contains(document.activeElement))
    restoreFocusRef.current &&= focusStillInPicker
    if (!response.ok) {
      setSwitchError(response.error || 'Branch could not be switched.')
      return
    }
    // Return keyboard users when the focused dialog disappears, while preserving
    // focus deliberately moved elsewhere during the asynchronous switch.
    restoreFocusRef.current = hadDialogFocus && focusStillInPicker
    setOpen(false)
    if (response.data?.pending) return
    if (vcsType === 'svn') {
      setSvnPath(svnDisplayPath(response.data?.svnContext) || name)
      setSvnRevision(response.data?.branch || '')
      onSwitched(response.data?.branch || '', 'svn')
    }
    else onSwitched(response.data?.branch || name)
  }

  const chooseBranch = (choice: ChatBranch) => {
    if (busyRef.current || disabled) return
    if (vcsType === 'svn' && !choice.current) setSvnSelection(choice)
    else void switchTo(choice.path || choice.name)
  }

  const closeToTrigger = () => {
    restoreFocusRef.current = true
    setOpen(false)
  }

  const onSearchKeyDown = (event: KeyboardEvent<HTMLInputElement>) => {
    if (event.key === 'ArrowDown' || event.key === 'ArrowUp') {
      event.preventDefault()
      const last = Math.max(0, filtered.length - 1)
      setHighlight((current) => event.key === 'ArrowDown' ? Math.min(current + 1, last) : Math.max(current - 1, 0))
    } else if (event.key === 'Enter') {
      event.preventDefault()
      const choice = filtered[highlight]
      if (choice) chooseBranch(choice)
    }
  }

  const canCreate = vcsType === 'git' && !loading && !loadError

  const displayedRevision = svnRevision || branch
  const pendingHint = pendingBranch ? `${pendingBranch} will be created from ${branch} when you send your first message.` : ''

  const trigger = (
    <button
      ref={triggerRef}
      type="button"
      disabled={disabled || busy}
      title={vcsType === 'svn' ? [`${svnPath || 'SVN'} r${displayedRevision}`, svnContextError].filter(Boolean).join(' · ') : pendingBranch ? `${pendingBranch}, created on first send (from ${branch})` : branch}
      aria-label={vcsType === 'svn' ? `Switch SVN folder, current ${svnPath ? `${svnPath} ` : ''}revision ${displayedRevision}${svnContextError ? `. ${svnContextError}` : ''}` : pendingBranch ? `Switch branch, ${pendingBranch} pending, created from ${branch} on first message` : `Switch branch, current ${branch}`}
      aria-haspopup="dialog"
      aria-expanded={open}
      aria-controls={open ? dialogId : undefined}
      onClick={() => (open ? setOpen(false) : void openPicker())}
      onBlur={(event) => {
        const next = event.relatedTarget as Node | null
        if (open && next && !document.getElementById(dialogId)?.contains(next) && !event.currentTarget.contains(next)) setOpen(false)
      }}
      className="inline-flex min-w-0 items-center gap-1 truncate rounded-sm px-1 text-xs hover:bg-[var(--surface-up)] focus-visible:ring-1 focus-visible:ring-[var(--accent)] disabled:opacity-50"
      style={{ color: 'inherit', cursor: disabled ? 'default' : 'pointer' }}
    >
      {pendingBranch && vcsType !== 'svn' ? <GitBranchPlus size={12} aria-hidden /> : <GitBranch size={12} aria-hidden />}
      <span className="truncate font-mono">{vcsType === 'svn' ? `${svnPath || 'SVN'} r${displayedRevision}` : pendingBranch || branch}</span>
    </button>
  )

  return (
    <>
      <Tooltip label={disabled ? disabledReason : undefined}>
        <span className="inline-flex min-w-0">{trigger}</span>
      </Tooltip>
      {!open && switchError && <Tooltip label={switchError}>
        <span role="alert" aria-label="Branch switch needs attention" tabIndex={0} className="ml-1 inline-flex shrink-0 rounded-sm" style={{ color: 'var(--error)', cursor: 'help' }}>
          <AlertTriangle size={14} aria-hidden />
          <span className="sr-only">{switchError}</span>
        </span>
      </Tooltip>}
      {open && (
        <ViewportMenu
          id={dialogId}
          ref={menuRef}
          anchorRef={triggerRef}
          side="top"
          role="dialog"
          aria-label={vcsType === 'svn' ? 'Switch SVN folder' : 'Switch branch'}
          aria-describedby={pendingBranch ? `${dialogId}-pending-hint` : undefined}
          onKeyDown={(event) => {
            if (event.key !== 'Escape') return
            if (svnSelection && !busy) { event.stopPropagation(); setSvnSelection(null); searchRef.current?.focus(); return }
            closeToTrigger()
          }}
          onBlur={(event) => {
            // Tab past the last control closes the picker instead of leaving it open behind focus.
            const next = event.relatedTarget as Node | null
            if (next && !event.currentTarget.contains(next) && !triggerRef.current?.contains(next)) setOpen(false)
          }}
          className="animate-fade-in"
          style={{ width: 280, border: '1px solid var(--border-bright)', borderRadius: 8, background: 'var(--surface)', boxShadow: 'var(--elev-3)', padding: 6 }}
        >
          {loading ? (
            <div className="flex items-center gap-2 px-2 py-2 text-xs" style={{ color: 'var(--text-3)' }} role="status">
              <LoaderCircle size={14} className="animate-spin" aria-hidden />
              {vcsType === 'svn' ? 'Loading SVN folders' : 'Loading branches'}
            </div>
          ) : loadError && vcsType !== 'svn' ? (
            <div role="alert" className="max-h-24 overflow-y-auto break-words px-2 py-2 text-xs" style={{ color: 'var(--error)' }}>{loadError}</div>
          ) : (
            <>
              {vcsType === 'svn' && <div className="space-y-2 px-2 py-2 text-xs" style={{ color: 'var(--text-2)' }}>
                <label className="block">SVN repository folder
                  <input ref={svnParentRef} aria-label="SVN repository folder" placeholder="^/project/lines" value={svnParent} disabled={busy}
                    onChange={(event) => { ++requestEpoch.current; setSvnParent(event.target.value); setSvnContext(null); setBranches([]); setSvnSelection(null); setSwitchError('') }}
                    onKeyDown={(event) => { if (event.key === 'Enter' && svnParent.trim()) { event.preventDefault(); void openPicker() } }}
                    className="mt-1 w-full rounded-sm bg-transparent px-2 py-1 font-mono outline-none" style={{ border: '1px solid var(--border-bright)', color: 'var(--text)' }} />
                </label>
                <div>Enter the repository folder containing the targets to switch between.</div>
                <Button size="sm" variant="secondary" disabled={!svnParent.trim() || busy} onClick={() => void openPicker()}>Discover folders</Button>
                {loadError && <div role="alert" style={{ color: 'var(--error)' }}>{loadError}</div>}
                {svnPath && <div className="break-all font-mono" title={svnPath}>Current: {svnPath}</div>}
              </div>}
              {(vcsType !== 'svn' || Boolean(svnContext?.parent)) && <>
              <div className="flex items-center gap-2 px-2 py-1" style={{ borderBottom: '1px solid var(--border)' }}>
                <Search size={14} aria-hidden style={{ color: 'var(--text-3)' }} />
                <input
                  ref={searchRef}
                  type="text"
                  role="combobox"
                  aria-expanded
                  aria-controls={listboxId}
                  aria-activedescendant={filtered[highlight] ? `${listboxId}-${highlight}` : undefined}
                  aria-label={vcsType === 'svn' ? 'Search SVN folders' : 'Search branches'}
                  placeholder={vcsType === 'svn' ? 'Search SVN folders' : 'Search branches'}
                  value={query}
                  onChange={(event) => { setQuery(event.target.value); setHighlight(0); setSvnSelection(null) }}
                  onKeyDown={onSearchKeyDown}
                  className="min-w-0 flex-1 bg-transparent py-1.5 text-[13px] outline-none"
                  style={{ color: 'var(--text)' }}
                />
              </div>
              <div ref={listRef} id={listboxId} role="listbox" aria-label={vcsType === 'svn' ? 'SVN folders' : 'Local branches'} className="overflow-y-auto" style={{ maxHeight: 240 }}>
                {filtered.length === 0 && (
                  <div className="px-2 py-2 text-xs" style={{ color: 'var(--text-3)' }}>
                    {vcsType === 'svn' ? branches.length === 0 ? 'No child directories' : 'No folders match' : branches.length === 0 ? 'No local branches' : 'No branches match'}
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
                      tabIndex={-1}
                      aria-selected={active}
                      aria-checked={item.current}
                      disabled={busy}
                      onMouseEnter={() => setHighlight(index)}
                      onClick={() => chooseBranch(item)}
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
              </>}
              {svnSelection && <div className="space-y-2 border-t px-2 py-2 text-xs" style={{ borderColor: 'var(--border)', color: 'var(--text-2)' }}>
                <p>Switch this working copy to <span className="break-all font-mono">{svnSelection.path || svnSelection.name}</span>? Existing local changes must be resolved first.</p>
                <Button ref={svnConfirmRef} size="sm" variant="secondary" loading={busy} disabled={busy || !svnContext?.parent} onClick={() => void switchTo(svnSelection.path || svnSelection.name)}>Switch folder</Button>
                <Button size="sm" variant="ghost" disabled={busy} onClick={() => { setSvnSelection(null); searchRef.current?.focus() }}>Cancel</Button>
              </div>}
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
                    <>
                      <button type="button" disabled={busy} onClick={() => setCreating(true)} className="flex w-full items-center gap-2 rounded-md px-2 py-1.5 text-left text-[13px] disabled:opacity-50" style={{ color: 'var(--text-2)' }}>
                        <Plus size={14} aria-hidden />
                        Create branch from current…
                      </button>
                      {pendingBranch && <div id={`${dialogId}-pending-hint`} className="break-words pb-1 pl-[30px] pr-2" style={{ color: 'var(--text-2)', fontSize: 'var(--fs-sm)' }}>{pendingHint} Select {branch} above to cancel.</div>}
                    </>
                  )}
                </div>
              )}
            </>
          )}
          {switchError && (
            <div role="alert" className="mt-1 max-h-24 overflow-y-auto break-words px-2 py-1 text-xs" style={{ color: 'var(--error)' }}>{switchError}</div>
          )}
        </ViewportMenu>
      )}
    </>
  )
}
