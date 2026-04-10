import { useState, useRef, useEffect } from 'react'
import { Session, ViewMode } from '../../types/session'
import { useAppStore } from '../../store/useAppStore'

const MODE_ICON: Record<ViewMode, string> = {
  structured: '◈',
  cli: '⌃',
  'coding-agent': '⚙',
}

const MODE_COLOR: Record<ViewMode, string> = {
  structured: 'var(--accent)',
  cli: 'var(--green)',
  'coding-agent': 'var(--blue)',
}

interface SessionItemProps {
  session: Session
}

export function SessionItem({ session }: SessionItemProps) {
  const { activeSessionId, setActiveSession, renameSession, deleteSession } =
    useAppStore()
  const isActive = activeSessionId === session.id
  const [editing, setEditing] = useState(false)
  const [editValue, setEditValue] = useState(session.name)
  const [showMenu, setShowMenu] = useState(false)
  const inputRef = useRef<HTMLInputElement>(null)
  const menuRef = useRef<HTMLDivElement>(null)

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
    if (trimmed && trimmed !== session.name) renameSession(session.id, trimmed)
    else setEditValue(session.name)
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
        onClick={() => !editing && setActiveSession(session.id)}
        onDoubleClick={() => {
          setEditing(true)
          setEditValue(session.name)
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
        {/* Mode icon */}
        <span
          className="flex-shrink-0 text-xs"
          style={{ color: isActive ? MODE_COLOR[session.viewMode] : 'var(--text-3)', fontSize: 10 }}
        >
          {MODE_ICON[session.viewMode]}
        </span>

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
                setEditValue(session.name)
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
            {session.name}
          </span>
        )}

        {/* Context menu trigger — visible on hover */}
        {!editing && (
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
            onClick={() => { setShowMenu(false); setEditing(true); setEditValue(session.name) }}
          >
            Rename
          </button>
          <button
            className="w-full text-left px-3 py-1.5 text-xs transition-colors duration-100"
            style={{ background: 'transparent', color: 'var(--red)', cursor: 'pointer', border: 'none', fontFamily: 'inherit' }}
            onClick={() => { setShowMenu(false); deleteSession(session.id) }}
          >
            Delete
          </button>
        </div>
      )}
    </div>
  )
}
