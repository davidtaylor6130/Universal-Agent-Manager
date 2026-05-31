import type { CSSProperties } from 'react'
import type { Goal } from '../../types/goal'

interface GoalBannerProps {
  goal: Goal
  onComplete: () => void
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
  }
}

function statusLabel(status: Goal['status']): string {
  switch (status) {
    case 'active': return 'Active'
    case 'complete': return 'Complete'
    case 'blocked': return 'Blocked'
  }
}

export function GoalBanner({ goal, onComplete, onRemove }: GoalBannerProps) {
  const budgetDisplay = goal.tokenBudget
    ? `${goal.tokensUsed ?? 0}/${goal.tokenBudget} tokens`
    : null

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
      <span
        className="truncate flex-1"
        style={{ color: 'var(--text-1)' }}
        title={goal.objective}
      >
        {goal.objective}
      </span>
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
        <button
          type="button"
          className="uam-secondary-button"
          onClick={onComplete}
          style={{ padding: '1px 8px', fontSize: 11 }}
        >
          Complete
        </button>
      )}
      <button
        type="button"
        className="uam-secondary-button"
        onClick={onRemove}
        style={{ padding: '1px 8px', fontSize: 11, color: 'var(--danger)' }}
      >
        Remove
      </button>
    </div>
  )
}
