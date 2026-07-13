import { useEffect, useRef, useState } from 'react'
import type { ReactNode } from 'react'
import { Target, MoreHorizontal, Check, Pause, Play, Trash2 } from 'lucide-react'
import type { Goal } from '../../types/goal'
import { StatusDot, Tooltip, type StatusTone } from '../ui'

interface GoalBannerProps {
  goal: Goal
  onComplete: () => void
  onPause?: () => void
  onResume?: () => void
  onRemove: () => void
}

function statusTone(status: Goal['status']): StatusTone {
  switch (status) {
    case 'active': return 'success'
    case 'complete': return 'info'
    case 'blocked': return 'error'
    case 'paused': return 'warning'
  }
}

function statusLabel(status: Goal['status']): string {
  switch (status) {
    case 'active': return 'Active'
    case 'complete': return 'Complete'
    case 'blocked': return 'Blocked'
    case 'paused': return 'Paused'
  }
}

function MenuItem({ icon, label, onClick, danger }: { icon: ReactNode; label: string; onClick: () => void; danger?: boolean }) {
  return (
    <button
      type="button"
      className="flex w-full items-center gap-2 px-3 py-1.5 text-sm text-left transition-colors duration-100"
      style={{ background: 'transparent', color: danger ? 'var(--error)' : 'var(--text-2)', border: 'none', cursor: 'pointer', fontFamily: 'inherit' }}
      onMouseEnter={(e) => { e.currentTarget.style.background = 'var(--sidebar-item-hover)' }}
      onMouseLeave={(e) => { e.currentTarget.style.background = 'transparent' }}
      onClick={onClick}
    >
      {icon}
      {label}
    </button>
  )
}

export function GoalBanner({ goal, onComplete, onPause, onResume, onRemove }: GoalBannerProps) {
  const [menuOpen, setMenuOpen] = useState(false)
  const menuRef = useRef<HTMLDivElement>(null)
  const budgetDisplay = goal.tokenBudget ? `${goal.tokensUsed ?? 0}/${goal.tokenBudget}` : null
  const blockerDisplay = goal.status === 'blocked' && goal.lastBlocker ? goal.lastBlocker : ''
  const tone = statusTone(goal.status)
  const completedCount = goal.completedItems?.length ?? 0
  const totalItems = completedCount + (goal.remainingItems?.length ?? 0)

  useEffect(() => {
    if (!menuOpen) return
    const onDown = (e: MouseEvent) => {
      if (menuRef.current && !menuRef.current.contains(e.target as Node)) setMenuOpen(false)
    }
    const onKey = (e: KeyboardEvent) => { if (e.key === 'Escape') setMenuOpen(false) }
    document.addEventListener('mousedown', onDown)
    document.addEventListener('keydown', onKey)
    return () => {
      document.removeEventListener('mousedown', onDown)
      document.removeEventListener('keydown', onKey)
    }
  }, [menuOpen])

  return (
    <div
      className="flex items-center gap-2.5 px-4 py-2 text-sm"
      style={{ background: 'var(--surface)', borderTop: '1px solid var(--border)', borderBottom: '1px solid var(--border)' }}
    >
      <Target size={15} style={{ color: 'var(--accent)', flexShrink: 0 }} aria-hidden />

      {/* Status chip */}
      <span
        className="inline-flex items-center gap-1.5 rounded-full px-2 py-0.5 text-xs font-medium flex-shrink-0"
        style={{ background: `var(--${tone}-dim)`, color: `var(--${tone})` }}
      >
        <StatusDot tone={tone} pulse={goal.status === 'active'} size={7} />
        {statusLabel(goal.status)}
      </span>

      {/* Objective + optional blocker */}
      <div className="min-w-0 flex-1">
        <div className="truncate" style={{ color: 'var(--text)', fontWeight: 500 }} title={goal.objective}>
          {goal.objective}
        </div>
        {blockerDisplay && (
          <div className="truncate text-xs" style={{ color: 'var(--error)', marginTop: 1 }} title={blockerDisplay}>
            {blockerDisplay}
          </div>
        )}
        {(goal.currentStep || totalItems > 0) && (
          <div className="mt-1 flex items-center gap-2 text-[11px]" style={{ color: 'var(--text-3)' }}>
            {totalItems > 0 && (
              <progress
                aria-label="Goal progress"
                max={totalItems}
                value={completedCount}
                className="h-1.5 w-20"
              />
            )}
            {totalItems > 0 && <span>{completedCount}/{totalItems}</span>}
            {goal.currentStep && <span className="truncate">{goal.currentStep}</span>}
          </div>
        )}
      </div>

      {budgetDisplay && (
        <Tooltip label="Tokens used / budget">
          <span className="text-xs font-mono flex-shrink-0" style={{ color: 'var(--text-3)' }}>{budgetDisplay}</span>
        </Tooltip>
      )}

      {/* All goal actions consolidated into one overflow menu */}
      <div ref={menuRef} className="relative flex-shrink-0">
        <Tooltip label="Goal actions">
          <button
            type="button"
            aria-label="Goal actions"
            className="flex items-center justify-center rounded-md transition-colors duration-100"
            style={{ width: 28, height: 26, border: '1px solid var(--border)', background: menuOpen ? 'var(--surface-up)' : 'transparent', color: 'var(--text-2)', cursor: 'pointer' }}
            onClick={() => setMenuOpen((v) => !v)}
          >
            <MoreHorizontal size={15} aria-hidden />
          </button>
        </Tooltip>
        {menuOpen && (
          <div
            className="absolute right-0 z-50 rounded-md py-1 animate-fade-in"
            style={{ top: 30, minWidth: 150, background: 'var(--surface-up)', border: '1px solid var(--border-bright)', boxShadow: 'var(--elev-2)' }}
          >
            {goal.status === 'active' && (
              <MenuItem icon={<Check size={14} aria-hidden />} label="Mark complete" onClick={() => { setMenuOpen(false); onComplete() }} />
            )}
            {goal.status === 'active' && onPause && (
              <MenuItem icon={<Pause size={14} aria-hidden />} label="Pause goal" onClick={() => { setMenuOpen(false); onPause() }} />
            )}
            {goal.status !== 'active' && onResume && (
              <MenuItem icon={<Play size={14} aria-hidden />} label="Resume goal" onClick={() => { setMenuOpen(false); onResume() }} />
            )}
            <MenuItem icon={<Trash2 size={14} aria-hidden />} label="Delete goal" danger onClick={() => { setMenuOpen(false); onRemove() }} />
          </div>
        )}
      </div>
    </div>
  )
}
