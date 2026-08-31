import { useState, useRef, useEffect, memo } from 'react'
import type { MouseEvent as ReactMouseEvent } from 'react'
import {
  Pin, MoreHorizontal, Pencil, Trash2, HelpCircle, ClipboardList, Brain,
  ShieldCheck, SquareChevronRight, FileText, TriangleAlert, CircleAlert, Check,
} from 'lucide-react'
import { useAppStore, type AcpAttentionKind } from '../../store/useAppStore'
import { useShallow } from 'zustand/react/shallow'
import type { Session } from '../../types/session'
import { Button, Tooltip, ViewportMenu } from '../ui'
import { ProviderLogo } from '../shared/ProviderLogo'
import {
  chatGridLeaves,
  chatPaneColors,
  readChatGridLayout,
  subscribeChatGridLayout,
} from '../../utils/chatGridStorage'
import { providerShortName } from '../../utils/providerMetadata'
import { CollectionMenuItems } from './CollectionMenuItems'
import { displayedChatStatus } from './chatSearch'

function formatSidebarTime(date: Date | null): string {
  if (!date || Number.isNaN(date.getTime())) {
    return ''
  }

  const now = new Date()
  const isSameDay = date.toDateString() === now.toDateString()
  if (isSameDay) {
    return date.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })
  }

  const yesterday = new Date(now)
  yesterday.setDate(now.getDate() - 1)
  if (date.toDateString() === yesterday.toDateString()) {
    return 'Yesterday'
  }

  return date.toLocaleDateString([], {
    month: 'short',
    day: 'numeric',
  })
}

function formatSidebarTimeTitle(date: Date | null): string {
  if (!date || Number.isNaN(date.getTime())) {
    return 'Last opened time unavailable'
  }

  return `Last opened ${date.toLocaleString([], {
    month: 'short',
    day: 'numeric',
    year: 'numeric',
    hour: '2-digit',
    minute: '2-digit',
  })}`
}

export function formatSidebarWorktreePath(path: string): string {
  const parts = path.replace(/\\/g, '/').replace(/\/+$/, '').split('/').filter(Boolean)
  return parts.length > 2 ? `.../${parts.slice(-2).join('/')}` : parts.join('/')
}

interface SessionItemProps {
  sessionId: string
  session?: Session
  familySessionIds?: string[]
  selected?: boolean
  onSessionClick?: (sessionId: string, event: ReactMouseEvent<HTMLDivElement>) => boolean
}

const ATTENTION_LABELS: Record<AcpAttentionKind, string> = {
  question: 'Needs answer',
  plan: 'Plan needs review',
  memory: 'Memory input needed',
  permission: 'Permission needed',
  command: 'Command approval needed',
  file: 'File approval needed',
  error: 'Needs attention',
  generic: 'Input needed',
}

function sidebarStatusIcon(kind: AcpAttentionKind) {
  const props = { size: 12, 'aria-hidden': true } as const
  switch (kind) {
    case 'question': return <HelpCircle {...props} />
    case 'plan': return <ClipboardList {...props} />
    case 'memory': return <Brain {...props} />
    case 'permission': return <ShieldCheck {...props} />
    case 'command': return <SquareChevronRight {...props} />
    case 'file': return <FileText {...props} />
    case 'error': return <TriangleAlert {...props} />
    default: return <CircleAlert {...props} />
  }
}

export const SessionItem = memo(function SessionItem({ sessionId, session, familySessionIds: providedFamilySessionIds, selected = false, onSessionClick }: SessionItemProps) {
  // Fine-grained selectors — each only re-renders when its specific value changes
  const sessionSummary = useAppStore(useShallow((s) => {
    if (session) {
      return {
        name: session.name,
        lastOpenedAt: session.lastOpenedAt ?? session.updatedAt ?? null,
        isPinned: session.isPinned ?? false,
        providerId: session.providerId,
        worktreeDirectory: session.workspaceWorktreeDirectory ?? '',
      }
    }

    const storeSession = s.sessions.find((x) => x.id === sessionId)
    return {
      name: storeSession?.name ?? '',
      lastOpenedAt: storeSession?.lastOpenedAt ?? storeSession?.updatedAt ?? null,
      isPinned: storeSession?.isPinned ?? false,
      providerId: storeSession?.providerId,
      worktreeDirectory: storeSession?.workspaceWorktreeDirectory ?? '',
    }
  }))
  const sessionName = sessionSummary.name
  const sessionLastOpenedAt = sessionSummary.lastOpenedAt
  const isPinned = sessionSummary.isPinned
  const showProviderIcon = useAppStore((s) => s.showProviderIconsInSidebar)
  const showWorktreePath = useAppStore((s) => s.showWorktreePathInSidebar)
  const familySessionIds = useAppStore(useShallow((s) => providedFamilySessionIds ?? s.sessions
    .filter((candidate) => (candidate.branchRootChatId || candidate.parentChatId || candidate.id) === sessionId)
    .map((candidate) => candidate.id)))
  const isActive = useAppStore((s) => familySessionIds.includes(s.activeSessionId ?? ''))
  const lifecycleStatus = useAppStore(useShallow((s) => {
    const acpBindings = familySessionIds.flatMap((id) => s.acpBindingBySessionId[id] ? [s.acpBindingBySessionId[id]] : [])
    const cliBindings = familySessionIds.flatMap((id) => s.cliBindingBySessionId[id] ? [s.cliBindingBySessionId[id]] : [])
    return displayedChatStatus(cliBindings, acpBindings)
  }))
  const setActiveSession = useAppStore((s) => s.setActiveSession)
  const setSessionPinned = useAppStore((s) => s.setSessionPinned)
  const renameSession = useAppStore((s) => s.renameSession)
  const deleteSessions = useAppStore((s) => s.deleteSessions)

  const [editing, setEditing] = useState(false)
  const [editValue, setEditValue] = useState(sessionName)
  const [gridLayout, setGridLayout] = useState(readChatGridLayout)
  // Context/overflow menu anchored to a viewport position (cursor or button).
  const [menuPos, setMenuPos] = useState<{ x: number; y: number } | null>(null)
  const [confirmDelete, setConfirmDelete] = useState(false)
  const [deleting, setDeleting] = useState(false)
  const [deleteError, setDeleteError] = useState('')
  const showMenu = menuPos !== null
  const inputRef = useRef<HTMLInputElement>(null)
  const menuRef = useRef<HTMLDivElement>(null)
  const rowRef = useRef<HTMLDivElement>(null)
  const menuReturnFocusRef = useRef<HTMLElement | null>(null)
  const lastOpenedLabel = formatSidebarTime(sessionLastOpenedAt)
  const lastOpenedTitle = formatSidebarTimeTitle(sessionLastOpenedAt)
  const gridLeaves = chatGridLeaves(gridLayout.root)
  const paneIndexes = gridLeaves.flatMap((leaf, index) => familySessionIds.includes(leaf.sessionId) ? [index] : [])
  const paneNumbers = paneIndexes.map((index) => index + 1)
  const paneLabel = paneNumbers.length === 1
    ? `Shown in pane ${paneNumbers[0]}`
    : `Shown in panes ${paneNumbers.slice(0, -1).join(', ')} and ${paneNumbers[paneNumbers.length - 1]}`

  useEffect(() => {
    if (editing && inputRef.current) {
      inputRef.current.focus()
      inputRef.current.select()
    }
  }, [editing])

  useEffect(() => subscribeChatGridLayout(setGridLayout), [])

  // Close context menu on outside click
  useEffect(() => {
    if (!showMenu) return
    menuRef.current?.querySelector<HTMLButtonElement>('button')?.focus()
    const handler = (e: MouseEvent) => {
      if (e.target instanceof Element && e.target.closest('[data-viewport-menu]')) return
      if (menuRef.current && !menuRef.current.contains(e.target as Node)) {
        setMenuPos(null)
      }
    }
    const onKey = (e: KeyboardEvent) => {
      if (e.key !== 'Escape') return
      setMenuPos(null)
      menuReturnFocusRef.current?.focus()
    }
    document.addEventListener('mousedown', handler)
    document.addEventListener('keydown', onKey)
    return () => {
      document.removeEventListener('mousedown', handler)
      document.removeEventListener('keydown', onKey)
    }
  }, [showMenu])

  const commitRename = () => {
    const trimmed = editValue.trim()
    if (trimmed && trimmed !== sessionName) renameSession(sessionId, trimmed)
    else setEditValue(sessionName)
    setEditing(false)
  }

  const confirmSessionDelete = async () => {
    if (deleting) return
    setDeleting(true)
    setDeleteError('')
    try {
      if (await deleteSessions(familySessionIds)) {
        setConfirmDelete(false)
        rowRef.current?.focus()
      } else {
        setDeleteError('The chat could not be deleted. Finish active work and try again.')
      }
    } catch {
      setDeleteError('The chat could not be deleted. Finish active work and try again.')
    } finally {
      setDeleting(false)
    }
  }

  return (
    <div
      className="relative group"
      draggable={!editing}
      onDragStart={(event) => {
        event.dataTransfer.effectAllowed = 'copy'
        event.dataTransfer.setData('text/x-uam-chat-id', sessionId)
        event.stopPropagation()
      }}
      style={{ animation: 'fadeIn 0.12s ease-out' }}
    >
      <div
        ref={rowRef}
        role="button"
        tabIndex={0}
        aria-current={isActive ? 'page' : undefined}
        aria-label={`Open chat ${sessionName}`}
        data-testid={`session-row-${sessionId}`}
        data-session-id={sessionId}
        data-selected={selected}
        className="relative flex min-h-[26px] items-center gap-1.5 px-2.5 py-1 rounded-md mx-1 cursor-pointer transition-all duration-100"
        style={{
          background: selected ? 'var(--accent-dim)' : isActive ? 'var(--sidebar-item-active)' : 'transparent',
          boxShadow: selected ? 'inset 0 0 0 1px var(--accent)' : 'none',
        }}
        onClick={(event) => {
          if (editing || onSessionClick?.(sessionId, event)) return
          if (!isActive) setActiveSession(sessionId)
        }}
        onDoubleClick={() => {
          setEditing(true)
          setEditValue(sessionName)
        }}
        onKeyDown={(event) => {
          if (event.target !== event.currentTarget || editing) return
          if (event.key === 'Enter' || event.key === ' ') {
            event.preventDefault()
            if (!isActive) setActiveSession(sessionId)
          } else if (event.key === 'F2') {
            event.preventDefault()
            setEditing(true)
            setEditValue(sessionName)
          } else if (event.key === 'ContextMenu' || (event.shiftKey && event.key === 'F10')) {
            event.preventDefault()
            const rect = event.currentTarget.getBoundingClientRect()
            menuReturnFocusRef.current = event.currentTarget
            setMenuPos({ x: rect.left + 16, y: rect.bottom })
          }
        }}
        onMouseEnter={(e) => {
          if (!isActive && !selected) e.currentTarget.style.background = 'var(--sidebar-item-hover)'
        }}
        onMouseLeave={(e) => {
          if (!isActive && !selected) e.currentTarget.style.background = 'transparent'
        }}
        onContextMenu={(e) => {
          e.preventDefault()
          menuReturnFocusRef.current = e.currentTarget
          setMenuPos({ x: e.clientX, y: e.clientY })
        }}
      >
        {selected && (
          <span role="img" aria-label="Selected for bulk actions" className="inline-flex shrink-0" style={{ color: 'var(--accent)' }}>
            <Check size={12} aria-hidden />
          </span>
        )}
        {gridLeaves.length > 1 && paneIndexes.length > 0 && (
          <span role="img" aria-label={paneLabel} className="inline-flex shrink-0 items-center gap-0.5">
            {paneIndexes.map((paneIndex) => (
              <span
                key={paneIndex}
                data-testid="pane-indicator"
                className="h-1.5 w-1.5 rounded-full"
                style={{ background: chatPaneColors[paneIndex] }}
              />
            ))}
          </span>
        )}
        {!editing && showProviderIcon && sessionSummary.providerId && (
          <span role="img" aria-label={`Provider: ${providerShortName(undefined, sessionSummary.providerId)}`} title={providerShortName(undefined, sessionSummary.providerId)} className="inline-flex shrink-0">
            <ProviderLogo providerId={sessionSummary.providerId} size={16} />
          </span>
        )}
        {!editing && isPinned && (
          <span role="img" aria-label="Pinned" title="Pinned" className="inline-flex shrink-0" style={{ color: 'var(--accent)' }}>
            <Pin size={12} fill="currentColor" aria-hidden />
          </span>
        )}

        {/* Name or edit input */}
        {editing ? (
          <input
            ref={inputRef}
            value={editValue}
            onChange={(e) => setEditValue(e.target.value)}
            onBlur={commitRename}
            onKeyDown={(e) => {
              if (e.key === 'Enter') commitRename()
              if (e.key === 'Escape') {
                setEditValue(sessionName)
                setEditing(false)
              }
            }}
            className="flex-1 bg-transparent text-xs outline-none min-w-0"
            style={{
              color: 'var(--text)',
              borderBottom: '1px solid var(--accent)',
              fontFamily: 'inherit',
            }}
            onClick={(e) => e.stopPropagation()}
          />
        ) : (
          <div className="min-w-0 flex-1">
            <span className="block truncate text-[13px]" style={{ color: isActive ? 'var(--text)' : 'var(--text-2)' }}>{sessionName}</span>
            {showWorktreePath && sessionSummary.worktreeDirectory && (
              <span className="block truncate text-[10px]" title={sessionSummary.worktreeDirectory} style={{ color: 'var(--text-3)' }}>
                {formatSidebarWorktreePath(sessionSummary.worktreeDirectory)}
              </span>
            )}
          </div>
        )}

        {!editing && (
          <>
            <div className="ml-auto flex items-center gap-1 transition-opacity duration-100 group-hover:opacity-0 group-focus-within:opacity-0">
              {lastOpenedLabel && (
                <span
                  className="max-w-[58px] truncate text-[10px] tabular-nums"
                  title={lastOpenedTitle}
                  style={{
                    color: isActive ? 'var(--text-2)' : 'var(--text-3)',
                    lineHeight: 1,
                  }}
                >
                  {lastOpenedLabel}
                </span>
              )}
              {lifecycleStatus?.type === 'processing' && (
                <span className="session-status session-status--processing" aria-label="Agent running" title="Agent running">
                  <span />
                </span>
              )}
              {lifecycleStatus?.type === 'attention' && (
                <span className={`session-status session-status--attention session-status--${lifecycleStatus.kind}`} aria-label={ATTENTION_LABELS[lifecycleStatus.kind]} title={ATTENTION_LABELS[lifecycleStatus.kind]}>
                  {sidebarStatusIcon(lifecycleStatus.kind)}
                </span>
              )}
              {lifecycleStatus?.type === 'done' && (
                <span className="session-status session-status--idle" aria-label="Done" title="Done">
                  <span />
                </span>
              )}
            </div>
            <div
              data-testid={`session-actions-${sessionId}`}
              className={`absolute right-2.5 flex items-center gap-0.5 transition-opacity duration-100 ${
                menuPos
                  ? 'opacity-100 pointer-events-auto'
                  : 'opacity-0 pointer-events-none group-hover:opacity-100 group-hover:pointer-events-auto group-focus-within:opacity-100 group-focus-within:pointer-events-auto'
              }`}
            >
              <Tooltip label={isPinned ? 'Unpin chat' : 'Pin chat'} side="top">
                <button
                  type="button"
                  aria-label={isPinned ? 'Unpin chat' : 'Pin chat'}
                  className="flex flex-shrink-0 items-center justify-center rounded focus-visible:outline focus-visible:outline-2 focus-visible:outline-offset-1"
                  style={{
                    width: 20,
                    height: 20,
                    background: 'var(--sidebar-item-hover)',
                    color: isPinned ? 'var(--accent)' : 'var(--text-3)',
                    border: 'none',
                    cursor: 'pointer',
                    padding: 0,
                    outlineColor: 'var(--accent)',
                  }}
                  onClick={(e) => {
                    e.stopPropagation()
                    void setSessionPinned(sessionId, !isPinned)
                  }}
                  onDoubleClick={(e) => e.stopPropagation()}
                >
                  <Pin size={13} fill={isPinned ? 'currentColor' : 'none'} aria-hidden />
                </button>
              </Tooltip>
              <Tooltip label="More actions" side="top">
                <button
                  type="button"
                  aria-label="More actions"
                  aria-haspopup="menu"
                  aria-expanded={showMenu}
                  className="flex flex-shrink-0 items-center justify-center rounded focus-visible:outline focus-visible:outline-2 focus-visible:outline-offset-1"
                  style={{
                    width: 20,
                    height: 20,
                    background: 'var(--sidebar-item-hover)',
                    color: 'var(--text-3)',
                    border: 'none',
                    cursor: 'pointer',
                    outlineColor: 'var(--accent)',
                  }}
                  onClick={(e) => {
                    e.stopPropagation()
                    if (menuPos) { setMenuPos(null); return }
                    const r = e.currentTarget.getBoundingClientRect()
                    menuReturnFocusRef.current = e.currentTarget
                    setMenuPos({ x: r.right, y: r.bottom + 4 })
                  }}
                >
                  <MoreHorizontal size={14} aria-hidden />
                </button>
              </Tooltip>
            </div>
          </>
        )}
      </div>

      {/* Context menu — anchored at the cursor / trigger position */}
      {menuPos && (
        <ViewportMenu
          ref={menuRef}
          point={menuPos}
          role="menu"
          aria-label={`Actions for ${sessionName}`}
          className="fixed z-50 rounded-md py-1 animate-fade-in"
          style={{
            minWidth: 170,
            background: 'var(--surface-up)',
            border: '1px solid var(--border-bright)',
            boxShadow: 'var(--elev-2)',
          }}
        >
          <button
            role="menuitem"
            className="flex w-full items-center gap-2 text-left px-3 py-1.5 text-sm transition-colors duration-100"
            style={{ background: 'transparent', color: 'var(--text-2)', cursor: 'pointer', border: 'none', fontFamily: 'inherit' }}
            onMouseEnter={(e) => e.currentTarget.style.color = 'var(--text)'}
            onMouseLeave={(e) => e.currentTarget.style.color = 'var(--text-2)'}
            onClick={() => { setMenuPos(null); setEditing(true); setEditValue(sessionName) }}
          >
            <Pencil size={13} aria-hidden />
            Rename
          </button>
          <CollectionMenuItems type="chat" target={sessionId} label={sessionName} onAdded={() => setMenuPos(null)} />
          <button
            role="menuitem"
            className="flex w-full items-center gap-2 text-left px-3 py-1.5 text-sm transition-colors duration-100"
            style={{ background: 'transparent', color: 'var(--red)', cursor: 'pointer', border: 'none', fontFamily: 'inherit' }}
            onClick={() => { setMenuPos(null); setDeleteError(''); setConfirmDelete(true) }}
          >
            <Trash2 size={13} aria-hidden />
            Delete
          </button>
        </ViewportMenu>
      )}
      {confirmDelete && (
        <div className="fixed inset-0 z-[70] flex items-center justify-center p-4" style={{ background: 'rgba(0,0,0,.5)' }}>
          <div role="alertdialog" aria-modal="true" aria-label={`Delete ${sessionName}`} className="w-full max-w-sm rounded-xl" style={{ background: 'var(--surface)', border: '1px solid var(--border-bright)', boxShadow: 'var(--elev-3)' }}>
            <div className="px-5 py-4 text-sm font-semibold" style={{ color: 'var(--text)', borderBottom: '1px solid var(--border)' }}>Delete chat?</div>
            <div className="p-5 text-sm" style={{ color: 'var(--text-2)' }}>
              <p>{familySessionIds.length > 1
                ? `${sessionName} and its ${familySessionIds.length - 1} related branch${familySessionIds.length === 2 ? '' : 'es'} will be permanently deleted.`
                : `${sessionName} will be permanently deleted.`} This cannot be undone.</p>
              {deleteError && <p role="alert" className="mt-3" style={{ color: 'var(--red)' }}>{deleteError}</p>}
            </div>
            <div className="flex justify-end gap-2 px-5 py-4" style={{ borderTop: '1px solid var(--border)' }}>
              <Button size="sm" disabled={deleting} onClick={() => { setConfirmDelete(false); setDeleteError(''); rowRef.current?.focus() }}>Cancel</Button>
              <Button size="sm" variant="danger" loading={deleting} onClick={() => { void confirmSessionDelete() }}>Delete chat</Button>
            </div>
          </div>
        </div>
      )}
    </div>
  )
})
