import { useEffect, useId, useRef, useState } from 'react'
import type { FormEvent, ReactNode } from 'react'
import { createPortal } from 'react-dom'
import { Target, MoreHorizontal, Check, Pause, Play, Trash2, Pencil, X, ChevronRight } from 'lucide-react'
import type { Goal } from '../../types/goal'
import { StatusDot, Tooltip, ViewportMenu, type StatusTone } from '../ui'

interface GoalBannerProps {
  goal: Goal
  onComplete: () => void
  onPause?: () => void
  onResume?: () => void
  resumePending?: boolean
  onRemove: () => void
  onEdit?: (objective: string) => Promise<boolean>
  workerModelLabel?: string
  reviewerModelLabel?: string
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

function blockerKindLabel(goal: Goal): string {
	const normalized = `${goal.lastBlockerKind ?? ''} ${goal.lastBlocker ?? ''}`.toLowerCase()
	if (normalized.includes('permission') || normalized.includes('access')) return 'Needs access'
	if (normalized.includes('input') || normalized.includes('user')) return 'Needs input'
	if (normalized.includes('token') || normalized.includes('budget')) return 'Budget reached'
	if (normalized.includes('remote') || normalized.includes('connection') || normalized.includes('host')) return 'Needs connection'
	if (normalized.includes('process exited during an active turn')) return 'Needs connection'
	if (normalized === 'transient') return 'Temporary issue'
	return 'Blocked'
}

function blockerNextAction(goal: Goal): string {
	const kind = `${goal.lastBlockerKind ?? ''} ${goal.lastBlocker ?? ''}`.toLowerCase()
	if (kind.includes('permission') || kind.includes('access')) return 'Approve or deny the request, then resume.'
	if (kind.includes('token') || kind.includes('budget')) return 'Increase the token budget, then resume.'
	if (kind.includes('remote') || kind.includes('connection') || kind.includes('host')) return 'Reconnect the host, then resume.'
	if (kind.includes('process exited during an active turn')) return 'Reconnect the host, then resume.'
	if (kind === 'transient') return 'Retry when the dependency is available.'
	if (goal.currentStep?.trim()) return readablePlanItem(goal.currentStep)
	return 'Resolve the blocker, then resume.'
}

function readableDiagnostic(diagnostic: string): string {
	if (diagnostic === 'goal_blocked_invalid_review') return 'The completion review was invalid.'
	if (diagnostic === 'goal_blocked_max_loop_iterations_reached') return 'The goal reached its automatic iteration limit.'
	return diagnostic
}

function readablePlanItem(item: string): string {
  const trimmed = item.trim()
  return trimmed.replace(/^T\d+\s*(?:[:.)\]_\-–—]\s*)?/i, '').trim() || trimmed
}

function MenuItem({ icon, label, onClick, danger, disabled }: { icon: ReactNode; label: string; onClick: () => void; danger?: boolean; disabled?: boolean }) {
  return (
    <button
      type="button"
      aria-label={label}
      role="menuitem"
      disabled={disabled}
      className="flex w-full items-center gap-2 px-3 py-1.5 text-sm text-left transition-colors duration-100"
      style={{ background: 'transparent', color: danger ? 'var(--error)' : 'var(--text-2)', border: 'none', cursor: disabled ? 'default' : 'pointer', fontFamily: 'inherit', opacity: disabled ? 0.6 : 1 }}
      onMouseEnter={(e) => { if (!disabled) e.currentTarget.style.background = 'var(--sidebar-item-hover)' }}
      onMouseLeave={(e) => { e.currentTarget.style.background = 'transparent' }}
      onClick={onClick}
    >
      {icon}
      {label}
    </button>
  )
}

function EditGoalDialog({ objective, onClose, onSave }: { objective: string; onClose: () => void; onSave: (objective: string) => Promise<boolean> }) {
  const [draft, setDraft] = useState(objective)
  const [error, setError] = useState('')
  const [saving, setSaving] = useState(false)
  const inputRef = useRef<HTMLTextAreaElement>(null)

  useEffect(() => {
    const previouslyFocused = document.activeElement as HTMLElement | null
    inputRef.current?.focus()
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.key === 'Escape' && !saving) onClose()
    }
    window.addEventListener('keydown', onKeyDown)
    return () => {
      window.removeEventListener('keydown', onKeyDown)
      previouslyFocused?.focus?.()
    }
  }, [onClose, saving])

  const submit = async (event: FormEvent) => {
    event.preventDefault()
    const nextObjective = draft.trim()
    if (!nextObjective) {
      setError('Goal objective is required.')
      return
    }
    setSaving(true)
    setError('')
    const saved = await onSave(nextObjective)
    setSaving(false)
    if (saved) onClose()
    else setError('Failed to update goal.')
  }

  return createPortal(
    <div
      className="fixed inset-0 z-[1100] flex items-center justify-center p-4"
      style={{ background: 'rgba(0, 0, 0, 0.48)', backdropFilter: 'blur(3px)' }}
      onMouseDown={() => { if (!saving) onClose() }}
    >
      <form
        role="dialog"
        aria-modal="true"
        aria-label="Edit goal"
        className="w-full max-w-xl rounded-xl p-4"
        style={{ background: 'var(--surface)', border: '1px solid var(--border-bright)', boxShadow: 'var(--elev-3)' }}
        onMouseDown={(event) => event.stopPropagation()}
        onSubmit={submit}
      >
        <div className="mb-3 flex items-center gap-3">
          <h2 className="flex-1 text-base font-semibold" style={{ color: 'var(--text)' }}>Edit goal</h2>
          <button type="button" aria-label="Close edit goal" disabled={saving} onClick={onClose} className="rounded-md p-1.5" style={{ color: 'var(--text-2)', background: 'transparent', border: 0 }}><X size={17} aria-hidden /></button>
        </div>
        <label className="mb-1 block text-xs font-medium" htmlFor="goal-objective-edit" style={{ color: 'var(--text-2)' }}>Objective</label>
        <textarea
          ref={inputRef}
          id="goal-objective-edit"
          rows={5}
          value={draft}
          disabled={saving}
          onChange={(event) => setDraft(event.target.value)}
          className="w-full resize-y rounded-md p-3 text-sm outline-none"
          style={{ minHeight: 120, maxHeight: '55vh', background: 'var(--bg)', border: '1px solid var(--border-bright)', color: 'var(--text)' }}
        />
        {error && <div role="alert" className="mt-2 text-xs" style={{ color: 'var(--error)' }}>{error}</div>}
        <div className="mt-4 flex justify-end gap-2">
          <button type="button" disabled={saving} onClick={onClose} className="rounded-md px-3 py-1.5 text-sm" style={{ background: 'transparent', border: '1px solid var(--border)', color: 'var(--text-2)' }}>Cancel</button>
          <button type="submit" disabled={saving} className="rounded-md px-3 py-1.5 text-sm font-medium" style={{ background: 'var(--accent)', border: 0, color: 'var(--accent-contrast)' }}>{saving ? 'Saving…' : 'Save'}</button>
        </div>
      </form>
    </div>,
    document.body,
  )
}

export function GoalBanner({ goal, onComplete, onPause, onResume, resumePending = false, onRemove, onEdit, workerModelLabel, reviewerModelLabel }: GoalBannerProps) {
  const [menuOpen, setMenuOpen] = useState(false)
  const [editing, setEditing] = useState(false)
  const menuRef = useRef<HTMLDivElement>(null)
  const triggerRef = useRef<HTMLButtonElement>(null)
  const popupRef = useRef<HTMLDivElement>(null)
  const menuId = useId()
  const budgetDisplay = goal.tokenBudget ? `${goal.tokensUsed ?? 0}/${goal.tokenBudget}` : null
  const blockerDisplay = goal.status === 'blocked' && goal.lastBlocker ? goal.lastBlocker : ''
	const blockerLabel = blockerKindLabel(goal)
	const blockerAction = blockerDisplay ? blockerNextAction(goal) : ''
  const tone = statusTone(goal.status)
  const completedCount = goal.completedItems?.length ?? 0
  const totalItems = completedCount + (goal.remainingItems?.length ?? 0)
  const isComplete = goal.status === 'complete'
  const completedItems = goal.completedItems?.map(readablePlanItem) ?? []
  const remainingItems = isComplete ? [] : (goal.remainingItems?.map(readablePlanItem) ?? [])
  const currentStep = isComplete || !goal.currentStep ? '' : readablePlanItem(goal.currentStep)
  const progressLabel = isComplete
    ? 'Complete'
    : totalItems > 0
      ? `${completedCount} of ${totalItems} ${totalItems === 1 ? 'step' : 'steps'}`
      : goal.status === 'active' ? 'Planning' : 'No steps yet'

  useEffect(() => {
    if (!menuOpen) return
    const onDown = (e: MouseEvent) => {
      const target = e.target as Node
      if (!menuRef.current?.contains(target) && !popupRef.current?.contains(target)) setMenuOpen(false)
    }
    document.addEventListener('mousedown', onDown)
    return () => {
      document.removeEventListener('mousedown', onDown)
    }
  }, [menuOpen])

  return (
    <div
      className="uam-goal-banner flex items-center gap-2.5 px-4 py-2 text-sm"
      style={{ background: 'var(--surface)', borderTop: '1px solid var(--border)', borderBottom: '1px solid var(--border)' }}
    >
      <div className="flex flex-shrink-0 flex-col items-center gap-1">
        <div className="flex items-center gap-1.5">
          <Target size={15} style={{ color: 'var(--accent)', flexShrink: 0 }} aria-hidden />
          <span
            className="inline-flex items-center gap-1.5 rounded-full px-2 py-0.5 text-xs font-medium"
            style={{ background: `var(--${tone}-dim)`, color: `var(--${tone})` }}
          >
            <StatusDot tone={tone} pulse={goal.status === 'active'} size={7} />
            {statusLabel(goal.status)}
          </span>
        </div>
        <span className="uam-goal-banner__secondary text-xs" style={{ color: 'var(--text-3)' }}>
          {goal.executionOwner === 'provider' ? 'Provider managed' : 'UAM managed'}
        </span>
      </div>

      {/* Objective + optional blocker */}
      <details className="group min-w-0 flex-1">
        <summary className="uam-goal-banner__summary flex min-w-0 cursor-pointer items-center gap-1.5" style={{ color: 'var(--text)' }}>
          <ChevronRight data-goal-disclosure-icon size={14} className="flex-shrink-0 transition-transform duration-150 group-open:rotate-90" aria-hidden />
					<span className="min-w-0 flex-1">
						<span className="block truncate" style={{ fontWeight: 500 }} title={goal.objective}>{goal.objective}</span>
						{blockerDisplay && (
							<span role="alert" className="block truncate text-xs" style={{ color: 'var(--error)', marginTop: 1 }} title={`${blockerLabel}: ${blockerDisplay} ${blockerAction}`}>
								<strong>{blockerLabel}:</strong> {blockerDisplay} {blockerAction}
							</span>
						)}
					</span>
        </summary>
        {goal.executionOwner === 'uam' && workerModelLabel && reviewerModelLabel && (
          <div className="uam-goal-banner__secondary mt-1 truncate text-xs" style={{ color: 'var(--text-3)' }} title={`Worker: ${workerModelLabel} · Reviewer: ${reviewerModelLabel}`}>
            Worker: {workerModelLabel} · Reviewer: {reviewerModelLabel} · locked
          </div>
        )}
        <div
          data-goal-details-scroll
          role="region"
          aria-label="Goal details"
          tabIndex={0}
          className="uam-goal-banner__details mt-2 grid gap-2 rounded-md p-2 pr-1 text-xs"
          style={{ background: 'var(--bg)', border: '1px solid var(--border)' }}
        >
          <div className="whitespace-pre-wrap" style={{ color: 'var(--text-2)' }}>{goal.objective}</div>
					{blockerDisplay && <div><strong style={{ color: 'var(--text)' }}>Blocker:</strong> <span style={{ color: 'var(--text-2)' }}>{blockerDisplay}</span></div>}
					{goal.lastDiagnostic && <div><strong style={{ color: 'var(--text)' }}>Last check:</strong> <span style={{ color: 'var(--text-2)' }}>{readableDiagnostic(goal.lastDiagnostic)}</span></div>}
          {currentStep && <div><strong style={{ color: 'var(--text)' }}>Current step:</strong> <span style={{ color: 'var(--text-2)' }}>{currentStep}</span></div>}
          {completedItems.length > 0 && <div><strong style={{ color: 'var(--text)' }}>Completed steps</strong><ul className="mt-1 list-disc pl-5" style={{ color: 'var(--text-2)' }}>{completedItems.map((item, index) => <li key={`${index}-${item}`}>{item}</li>)}</ul></div>}
          {remainingItems.length > 0 && <div><strong style={{ color: 'var(--text)' }}>Remaining steps</strong><ul className="mt-1 list-disc pl-5" style={{ color: 'var(--text-2)' }}>{remainingItems.map((item, index) => <li key={`${index}-${item}`}>{item}</li>)}</ul></div>}
        </div>
      </details>

      <div className="uam-goal-banner__progress flex flex-shrink-0 items-center gap-1.5 text-xs" style={{ color: 'var(--text-3)' }}>
        <progress
          aria-label="Goal progress"
          aria-valuetext={isComplete ? 'Goal complete' : totalItems > 0 ? `${completedCount} of ${totalItems} planned steps complete` : progressLabel}
          max={isComplete ? 1 : Math.max(totalItems, 1)}
          value={isComplete ? 1 : totalItems > 0 ? completedCount : undefined}
          className="h-1.5 w-16"
        />
        <span className="whitespace-nowrap">{progressLabel}</span>
      </div>

      {budgetDisplay && (
        <Tooltip label="Tokens used / budget">
          <span className="uam-goal-banner__secondary text-xs font-mono flex-shrink-0" style={{ color: 'var(--text-3)' }}>{budgetDisplay}</span>
        </Tooltip>
      )}

      {/* All goal actions consolidated into one overflow menu */}
      <div ref={menuRef} className="relative flex-shrink-0">
        <Tooltip label="Goal actions">
          <button
            ref={triggerRef}
            type="button"
            aria-label="Goal actions"
            aria-haspopup="menu"
            aria-expanded={menuOpen}
            aria-controls={menuOpen ? menuId : undefined}
            className="flex items-center justify-center rounded-md transition-colors duration-100"
            style={{ width: 28, height: 26, border: '1px solid var(--border)', background: menuOpen ? 'var(--surface-up)' : 'transparent', color: 'var(--text-2)', cursor: 'pointer' }}
            onClick={() => setMenuOpen((v) => !v)}
          >
            <MoreHorizontal size={15} aria-hidden />
          </button>
        </Tooltip>
        {menuOpen && (
          <ViewportMenu
            ref={popupRef}
            anchorRef={triggerRef}
            align="end"
            id={menuId}
            role="menu"
            aria-label="Goal actions"
            onRequestClose={() => setMenuOpen(false)}
            className="rounded-md py-1 animate-fade-in"
            style={{ minWidth: 150, background: 'var(--surface-up)', border: '1px solid var(--border-bright)', boxShadow: 'var(--elev-2)' }}
          >
            {goal.status === 'active' && (
              <MenuItem disabled={resumePending} icon={<Check size={14} aria-hidden />} label="Mark complete" onClick={() => { setMenuOpen(false); onComplete() }} />
            )}
            {goal.status === 'active' && onPause && (
              <MenuItem disabled={resumePending} icon={<Pause size={14} aria-hidden />} label="Pause goal" onClick={() => { setMenuOpen(false); onPause() }} />
            )}
            {(goal.status === 'paused' || goal.status === 'blocked') && onResume && (
              <MenuItem disabled={resumePending} icon={<Play size={14} aria-hidden />} label={resumePending ? 'Resuming…' : 'Resume goal'} onClick={() => { setMenuOpen(false); onResume() }} />
            )}
            {!isComplete && goal.executionOwner !== 'provider' && onEdit && (
              <MenuItem disabled={resumePending} icon={<Pencil size={14} aria-hidden />} label="Edit goal" onClick={() => { setMenuOpen(false); setEditing(true) }} />
            )}
            <MenuItem disabled={resumePending} icon={<Trash2 size={14} aria-hidden />} label="Delete goal" danger onClick={() => { setMenuOpen(false); onRemove() }} />
          </ViewportMenu>
        )}
      </div>
      {editing && onEdit && <EditGoalDialog objective={goal.objective} onClose={() => setEditing(false)} onSave={onEdit} />}
    </div>
  )
}
