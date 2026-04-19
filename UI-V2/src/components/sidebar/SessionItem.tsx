import { useState, useRef, useEffect, memo } from 'react'
import { useAppStore } from '../../store/useAppStore'
import { useShallow } from 'zustand/react/shallow'

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
}

export const SessionItem = memo(function SessionItem({ sessionId }: SessionItemProps) {
  // Fine-grained selectors — each only re-renders when its specific value changes
  const sessionName     = useAppStore((s) => s.sessions.find((x) => x.id === sessionId)?.name ?? '')
  const sessionLastOpenedAt = useAppStore((s) => {
    const session = s.sessions.find((x) => x.id === sessionId)
    return session?.lastOpenedAt ?? session?.updatedAt ?? null
  })
  const isPinned        = useAppStore((s) => s.sessions.find((x) => x.id === sessionId)?.isPinned ?? false)
  const isActive        = useAppStore((s) => s.activeSessionId === sessionId)
  const cliBinding      = useAppStore(useShallow((s) => s.cliBindingBySessionId[sessionId]))
  const acpBinding      = useAppStore(useShallow((s) => s.acpBindingBySessionId[sessionId]))
  const setActiveSession = useAppStore((s) => s.setActiveSession)
  const setSessionPinned = useAppStore((s) => s.setSessionPinned)
  const renameSession    = useAppStore((s) => s.renameSession)
  const deleteSession    = useAppStore((s) => s.deleteSession)

  const [editing, setEditing] = useState(false)
  const [editValue, setEditValue] = useState(sessionName)
  const [showMenu, setShowMenu] = useState(false)
  const inputRef = useRef<HTMLInputElement>(null)
  const menuRef = useRef<HTMLDivElement>(null)
  const lastOpenedLabel = formatSidebarTime(sessionLastOpenedAt)
  const lastOpenedTitle = formatSidebarTimeTitle(sessionLastOpenedAt)

  const lifecycleStatus =
    acpBinding?.processing || acpBinding?.lifecycleState === 'waitingPermission'
      ? 'processing'
      : acpBinding?.readySinceLastSelect || cliBinding?.readySinceLastSelect
        ? 'idle'
        : cliBinding?.lifecycleState === 'busy' || cliBinding?.lifecycleState === 'shuttingDown'
          ? 'processing'
          : null

  useEffect(() => {
    if (editing && inputRef.current) {
      inputRef.current.focus()
      inputRef.current.select()
    }
  }, [editing])

  // Close context menu on outside click
  useEffect(() => {
    if (!showMenu) return
    const handler = (e: MouseEvent) => {
      if (menuRef.current && !menuRef.current.contains(e.target as Node)) {
        setShowMenu(false)
      }
    }
    document.addEventListener('mousedown', handler)
    return () => document.removeEventListener('mousedown', handler)
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
      style={{ animation: 'fadeIn 0.12s ease-out' }}
    >
      <div
        className="flex items-center gap-2 px-3 py-1.5 rounded-sm mx-1 cursor-pointer transition-all duration-100"
        style={{
          background: isActive ? 'var(--sidebar-item-active)' : 'transparent',
          borderLeft: isActive ? '2px solid var(--accent)' : '2px solid transparent',
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
          setShowMenu(true)
        }}
      >
        {!editing && (
          <button
            type="button"
            aria-label={isPinned ? 'Unpin chat' : 'Pin chat'}
            title={isPinned ? 'Unpin chat' : 'Pin chat'}
            className="flex flex-shrink-0 items-center justify-center rounded opacity-0 transition-opacity transition-colors duration-100 group-hover:opacity-100 group-focus-within:opacity-100"
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
            <svg width="12" height="12" viewBox="0 0 16 16" fill={isPinned ? 'currentColor' : 'none'} stroke="currentColor" strokeWidth="1.6" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
              <path d="M5.4 1.7h5.2l-.9 3.8 2.3 2.4v1.2H8.7L8 14.3 7.3 9.1H4V7.9l2.3-2.4-.9-3.8z" />
            </svg>
          </button>
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
            {lifecycleStatus === 'processing' && (
              <span className="session-status session-status--processing" aria-label="Gemini running">
                <span />
              </span>
            )}
            {lifecycleStatus === 'idle' && (
              <span className="session-status session-status--idle" aria-label="Gemini idle">
                <span />
              </span>
            )}
            <button
              className="opacity-0 group-hover:opacity-100 flex-shrink-0 rounded transition-opacity duration-100"
              style={{
                width: 18,
                height: 18,
                background: 'transparent',
                color: 'var(--text-3)',
                border: 'none',
                cursor: 'pointer',
                fontSize: 12,
                lineHeight: 1,
              }}
              onClick={(e) => {
                e.stopPropagation()
                setShowMenu((v) => !v)
              }}
            >
              ···
            </button>
          </div>
        )}
      </div>

      {/* Context menu */}
      {showMenu && (
        <div
          ref={menuRef}
          className="absolute z-50 rounded-md py-1 shadow-lg animate-fade-in"
          style={{
            left: 12,
            top: '100%',
            minWidth: 140,
            background: 'var(--surface-up)',
            border: '1px solid var(--border-bright)',
          }}
        >
          <button
            className="w-full text-left px-3 py-1.5 text-xs transition-colors duration-100"
            style={{ background: 'transparent', color: 'var(--text-2)', cursor: 'pointer', border: 'none', fontFamily: 'inherit' }}
            onMouseEnter={(e) => e.currentTarget.style.color = 'var(--text)'}
            onMouseLeave={(e) => e.currentTarget.style.color = 'var(--text-2)'}
            onClick={() => { setShowMenu(false); setEditing(true); setEditValue(sessionName) }}
          >
            Rename
          </button>
          <button
            className="w-full text-left px-3 py-1.5 text-xs transition-colors duration-100"
            style={{ background: 'transparent', color: 'var(--red)', cursor: 'pointer', border: 'none', fontFamily: 'inherit' }}
            onClick={() => { setShowMenu(false); deleteSession(sessionId) }}
          >
            Delete
          </button>
        </div>
      )}
    </div>
  )
})
