import type { CSSProperties } from 'react'
import { Pause, Play, Trash2 } from 'lucide-react'
import type { Goal } from '../../types/goal'
import { Tooltip } from '../ui'

interface GoalBannerProps {
  goal: Goal
  onComplete: () => void
  onPause?: () => void
  onResume?: () => void
  onRemove: () => void
}

function statusBadgeStyle(status: Goal['status']): CSSProperties {
  switch (status) {
    case 'active':
      return { background: '#1a6d1a', color: '#b8f5b8' }
    case 'complete':
      return { background: '#1a4d6d', color: '#b8d8f5' }
    case 'blocked':
      return { background: '#6d1a1a', color: '#f5b8b8' }
    case 'paused':
      return { background: '#5a4a12', color: '#f5df8a' }
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

function iconButtonStyle(color?: string): CSSProperties {
  return {
    padding: '1px 8px',
    fontSize: 12,
    minWidth: 28,
    color,
  }
}

export function GoalBanner({ goal, onComplete, onPause, onResume, onRemove }: GoalBannerProps) {
  const budgetDisplay = goal.tokenBudget
    ? `${goal.tokensUsed ?? 0}/${goal.tokenBudget} tokens`
    : null
  const blockerDisplay = goal.status === 'blocked' && goal.lastBlocker ? goal.lastBlocker : ''

  return (
    <div
      className="flex items-center gap-3 px-4 py-2 text-xs"
      style={{
        background: 'var(--surface)',
        borderTop: '1px solid var(--border)',
        borderBottom: '1px solid var(--border)',
      }}
    >
      <span style={{ color: 'var(--text-2)', fontWeight: 600, whiteSpace: 'nowrap' }}>
        Goal:
      </span>
      <div className="min-w-0 flex-1">
        <div
          className="truncate"
          style={{ color: 'var(--text-1)', fontWeight: 500 }}
          title={goal.objective}
        >
          {goal.objective}
        </div>
        {blockerDisplay && (
          <div
            className="truncate"
            style={{ color: 'var(--text-3)', marginTop: 2 }}
            title={blockerDisplay}
          >
            Blocked: {blockerDisplay}
          </div>
        )}
      </div>
      <span
        className="rounded px-1.5 py-0.5 font-medium"
        style={statusBadgeStyle(goal.status)}
      >
        {statusLabel(goal.status)}
      </span>
      {budgetDisplay && (
        <span style={{ color: 'var(--text-3)', whiteSpace: 'nowrap' }}>
          {budgetDisplay}
        </span>
      )}
      {goal.status === 'active' && (
        <>
          <button
            type="button"
            className="uam-secondary-button"
            onClick={onComplete}
            style={{ padding: '1px 8px', fontSize: 11 }}
          >
            Complete
          </button>
          {onPause && (
            <Tooltip label="Pause goal">
              <button
                type="button"
                className="uam-secondary-button"
                aria-label="Pause goal"
                onClick={onPause}
                style={iconButtonStyle()}
              >
                <Pause size={14} aria-hidden />
              </button>
            </Tooltip>
          )}
        </>
      )}
      {goal.status !== 'active' && onResume && (
        <Tooltip label="Resume goal">
          <button
            type="button"
            className="uam-secondary-button"
            aria-label="Resume goal"
            onClick={onResume}
            style={iconButtonStyle()}
          >
            <Play size={14} aria-hidden />
          </button>
        </Tooltip>
      )}
      <Tooltip label="Delete goal">
        <button
          type="button"
          className="uam-secondary-button"
          aria-label="Delete goal"
          onClick={onRemove}
          style={iconButtonStyle('var(--danger)')}
        >
          <Trash2 size={14} aria-hidden />
        </button>
      </Tooltip>
    </div>
  )
}
