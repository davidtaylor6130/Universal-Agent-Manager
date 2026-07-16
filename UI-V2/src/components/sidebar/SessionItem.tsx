import { useState, useRef, useEffect, memo } from 'react'
import {
  Pin, MoreHorizontal, Pencil, Trash2, HelpCircle, ClipboardList, Brain,
  ShieldCheck, SquareChevronRight, FileText, TriangleAlert, CircleAlert,
} from 'lucide-react'
import { useAppStore, type AcpAttentionKind } from '../../store/useAppStore'
import { useShallow } from 'zustand/react/shallow'
import type { Session } from '../../types/session'
import { Tooltip, ViewportMenu } from '../ui'
import {
  assignChatToPane,
  chatPaneColors,
  readChatGridLayout,
  subscribeChatGridLayout,
} from '../../utils/chatGridStorage'

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

interface SessionItemProps {
  sessionId: string
  session?: Session
  forceShowPin?: boolean
}

type SidebarStatus =
  | { type: 'attention'; kind: AcpAttentionKind; label: string }
  | { type: 'processing'; label: string }
  | { type: 'idle'; label: string }
  | null

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

export const SessionItem = memo(function SessionItem({ sessionId, session, forceShowPin }: SessionItemProps) {
  // Fine-grained selectors — each only re-renders when its specific value changes
  const sessionSummary = useAppStore(useShallow((s) => {
    if (session) {
      return {
        name: session.name,
        lastOpenedAt: session.lastOpenedAt ?? session.updatedAt ?? null,
        isPinned: session.isPinned ?? false,
      }
    }

    const storeSession = s.sessions.find((x) => x.id === sessionId)
    return {
      name: storeSession?.name ?? '',
      lastOpenedAt: storeSession?.lastOpenedAt ?? storeSession?.updatedAt ?? null,
      isPinned: storeSession?.isPinned ?? false,
    }
  }))
  const sessionName = sessionSummary.name
  const sessionLastOpenedAt = sessionSummary.lastOpenedAt
  const isPinned = sessionSummary.isPinned
  const isActive = useAppStore((s) => s.activeSessionId === sessionId)
  const cliBinding = useAppStore(useShallow((s) => s.cliBindingBySessionId[sessionId]))
  const acpBinding = useAppStore(useShallow((s) => s.acpBindingBySessionId[sessionId]))
  const setActiveSession = useAppStore((s) => s.setActiveSession)
  const setSessionPinned = useAppStore((s) => s.setSessionPinned)
  const renameSession = useAppStore((s) => s.renameSession)
  const deleteSession = useAppStore((s) => s.deleteSession)

  const [editing, setEditing] = useState(false)
  const [editValue, setEditValue] = useState(sessionName)
  const [gridLayout, setGridLayout] = useState(readChatGridLayout)
  // Context/overflow menu anchored to a viewport position (cursor or button).
  const [menuPos, setMenuPos] = useState<{ x: number; y: number } | null>(null)
  const showMenu = menuPos !== null
  const inputRef = useRef<HTMLInputElement>(null)
  const menuRef = useRef<HTMLDivElement>(null)
  const lastOpenedLabel = formatSidebarTime(sessionLastOpenedAt)
  const lastOpenedTitle = formatSidebarTimeTitle(sessionLastOpenedAt)
  const paneIndexes = gridLayout.sessionIds
    .slice(0, gridLayout.paneCount)
    .flatMap((id, index) => id === sessionId ? [index] : [])

  const lifecycleStatus: SidebarStatus = acpBinding?.attentionKind
    ? { type: 'attention', kind: acpBinding.attentionKind, label: ATTENTION_LABELS[acpBinding.attentionKind] }
    : acpBinding?.processing || acpBinding?.lifecycleState === 'waitingPermission'
      ? { type: 'processing', label: 'Gemini running' }
      : cliBinding?.lifecycleState === 'busy' || cliBinding?.lifecycleState === 'shuttingDown'
        ? { type: 'processing', label: 'Gemini running' }
        : acpBinding?.readySinceLastSelect || cliBinding?.readySinceLastSelect
          ? { type: 'idle', label: 'Done' }
          : null

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
    const handler = (e: MouseEvent) => {
      if (menuRef.current && !menuRef.current.contains(e.target as Node)) {
        setMenuPos(null)
      }
    }
    const onKey = (e: KeyboardEvent) => { if (e.key === 'Escape') setMenuPos(null) }
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

  return (
    <div
      className="relative group"
      draggable={!editing}
      onDragStart={(event) => {
        event.dataTransfer.effectAllowed = 'copy'
        event.dataTransfer.setData('text/x-uam-chat-id', sessionId)
      }}
      style={{ animation: 'fadeIn 0.12s ease-out' }}
    >
      <div
        className="flex items-center gap-2 px-3 py-1.5 rounded-md mx-1.5 cursor-pointer transition-all duration-100"
        style={{
          background: isActive ? 'var(--sidebar-item-active)' : 'transparent',
          borderLeft: paneIndexes.length > 0 ? `3px solid ${chatPaneColors[paneIndexes[0]]}` : '3px solid transparent',
        }}
        onClick={() => !editing && setActiveSession(sessionId)}
        onDoubleClick={() => {
          setEditing(true)
          setEditValue(sessionName)
        }}
        onMouseEnter={(e) => {
          if (!isActive) e.currentTarget.style.background = 'var(--sidebar-item-hover)'
        }}
        onMouseLeave={(e) => {
          if (!isActive) e.currentTarget.style.background = 'transparent'
        }}
        onContextMenu={(e) => {
          e.preventDefault()
          setMenuPos({ x: e.clientX, y: e.clientY })
        }}
      >
        {!editing && (
          <Tooltip label={isPinned ? 'Unpin chat' : 'Pin chat'} side="top">
            <button
              type="button"
              aria-label={isPinned ? 'Unpin chat' : 'Pin chat'}
              className={`flex flex-shrink-0 items-center justify-center rounded transition-opacity transition-colors duration-100 ${
                (forceShowPin || isPinned) ? 'opacity-100' : 'opacity-0 group-hover:opacity-100 group-focus-within:opacity-100'
              }`}
              style={{
                width: 18,
                height: 18,
                background: 'transparent',
                color: isPinned ? 'var(--accent)' : 'var(--text-3)',
                border: 'none',
                cursor: 'pointer',
                padding: 0,
              }}
              onClick={(e) => {
                e.stopPropagation()
                void setSessionPinned(sessionId, !isPinned)
              }}
              onDoubleClick={(e) => e.stopPropagation()}
            >
              <Pin size={12} fill={isPinned ? 'currentColor' : 'none'} aria-hidden />
            </button>
          </Tooltip>
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
          <span
            className="flex-1 text-xs truncate"
            style={{ color: isActive ? 'var(--text)' : 'var(--text-2)' }}
          >
            {sessionName}
          </span>
        )}

        {/* Context menu trigger — visible on hover */}
        {!editing && (
          <div className="ml-auto flex items-center gap-1">
            {lastOpenedLabel && (
              <span
                className="max-w-[58px] truncate text-[10px] tabular-nums transition-opacity duration-100 group-hover:opacity-0"
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
              <span className="session-status session-status--processing" aria-label={lifecycleStatus.label} title={lifecycleStatus.label}>
                <span />
              </span>
            )}
            {lifecycleStatus?.type === 'attention' && (
              <span className={`session-status session-status--attention session-status--${lifecycleStatus.kind}`} aria-label={lifecycleStatus.label} title={lifecycleStatus.label}>
                {sidebarStatusIcon(lifecycleStatus.kind)}
              </span>
            )}
            {lifecycleStatus?.type === 'idle' && (
              <span className="session-status session-status--idle" aria-label={lifecycleStatus.label} title={lifecycleStatus.label}>
                <span />
              </span>
            )}
            <Tooltip label="More actions" side="top">
              <button
                type="button"
                aria-label="More actions"
                className="opacity-0 group-hover:opacity-100 flex-shrink-0 flex items-center justify-center rounded transition-opacity duration-100"
                style={{
                  width: 18,
                  height: 18,
                  background: 'transparent',
                  color: 'var(--text-3)',
                  border: 'none',
                  cursor: 'pointer',
                }}
                onClick={(e) => {
                  e.stopPropagation()
                  if (menuPos) { setMenuPos(null); return }
                  const r = e.currentTarget.getBoundingClientRect()
                  setMenuPos({ x: r.right, y: r.bottom + 4 })
                }}
              >
                <MoreHorizontal size={14} aria-hidden />
              </button>
            </Tooltip>
          </div>
        )}
      </div>

      {/* Context menu — anchored at the cursor / trigger position */}
      {menuPos && (
        <ViewportMenu
          ref={menuRef}
          point={menuPos}
          className="fixed z-50 rounded-md py-1 animate-fade-in"
          style={{
            minWidth: 170,
            background: 'var(--surface-up)',
            border: '1px solid var(--border-bright)',
            boxShadow: 'var(--elev-2)',
          }}
        >
          <div className="px-3 pb-1 pt-1 text-[10px] font-semibold uppercase tracking-wider" style={{ color: 'var(--text-3)' }}>Show in pane</div>
          <div className="flex gap-1 px-3 pb-2">
            {Array.from({ length: gridLayout.paneCount }, (_, paneIndex) => (
              <button
                key={paneIndex}
                type="button"
                aria-label={`Show ${sessionName} in pane ${paneIndex + 1}`}
                className="h-7 w-7 rounded"
                style={{ background: chatPaneColors[paneIndex], border: 'none' }}
                onClick={() => {
                  assignChatToPane(sessionId, paneIndex)
                  setActiveSession(sessionId)
                  setMenuPos(null)
                }}
              />
            ))}
          </div>
          <button
            className="flex w-full items-center gap-2 text-left px-3 py-1.5 text-sm transition-colors duration-100"
            style={{ background: 'transparent', color: 'var(--text-2)', cursor: 'pointer', border: 'none', fontFamily: 'inherit' }}
            onMouseEnter={(e) => e.currentTarget.style.color = 'var(--text)'}
            onMouseLeave={(e) => e.currentTarget.style.color = 'var(--text-2)'}
            onClick={() => { setMenuPos(null); setEditing(true); setEditValue(sessionName) }}
          >
            <Pencil size={13} aria-hidden />
            Rename
          </button>
          <button
            className="flex w-full items-center gap-2 text-left px-3 py-1.5 text-sm transition-colors duration-100"
            style={{ background: 'transparent', color: 'var(--red)', cursor: 'pointer', border: 'none', fontFamily: 'inherit' }}
            onClick={() => { setMenuPos(null); deleteSession(sessionId) }}
          >
            <Trash2 size={13} aria-hidden />
            Delete
          </button>
        </ViewportMenu>
      )}
    </div>
  )
})
