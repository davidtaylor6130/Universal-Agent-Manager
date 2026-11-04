import { useEffect, useId, useRef, useState } from 'react'
import { Target, MoreHorizontal, Check, Pause, Play, Trash2, Pencil } from 'lucide-react'
import type { Goal } from '../../types/goal'
import { Button, IconButton, MenuItem, StatusDot, Swap, Tooltip, ViewportMenu, usePresence, type StatusTone } from '../ui'
import './GoalBanner.css'

interface GoalBannerProps {
  goal: Goal
  onComplete: () => void
  onPause?: () => void
  onResume?: () => void
  onRestart?: () => void
  turnRunning?: boolean
  resumePending?: boolean
  onRemove: () => void
  onEdit?: (objective: string) => Promise<boolean>
  workerModelLabel?: string
  reviewerModelLabel?: string
  /** Controlled edit mode, so the owning window can let Escape cancel the edit first. */
  editing?: boolean
  onEditingChange?: (editing: boolean) => void
}

function statusTone(status: Goal['status']): StatusTone {
  switch (status) {
    case 'active': return 'info'
    case 'complete': return 'success'
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

export function GoalBanner({ goal, onComplete, onPause, onResume, onRestart, turnRunning = false, resumePending = false, onRemove, onEdit, workerModelLabel, reviewerModelLabel, editing: editingProp, onEditingChange }: GoalBannerProps) {
  const [menuOpen, setMenuOpen] = useState(false)
  const [localEditing, setLocalEditing] = useState(false)
  const editing = Boolean(onEdit) && (editingProp ?? localEditing)
  const setEditing = onEditingChange ?? setLocalEditing
  const [draft, setDraft] = useState(goal.objective)
  const [editError, setEditError] = useState('')
  const [saving, setSaving] = useState(false)
  const savingRef = useRef(false)
  const mountedRef = useRef(true)
  const inputRef = useRef<HTMLTextAreaElement>(null)
  const wasEditingRef = useRef(false)
  const menuRef = useRef<HTMLDivElement>(null)
  const triggerRef = useRef<HTMLButtonElement>(null)
  const popupRef = useRef<HTMLDivElement>(null)
  const menuId = useId()
  const menuPresence = usePresence(menuOpen)
  const budgetDisplay = goal.tokenBudget ? `${goal.tokensUsed ?? 0}/${goal.tokenBudget}` : null
  const blockerDisplay = goal.status === 'blocked' && goal.lastBlocker ? goal.lastBlocker : ''
	const blockerLabel = blockerKindLabel(goal)
	const blockerAction = blockerDisplay ? blockerNextAction(goal) : ''
  const tone = statusTone(goal.status)
  const budgetPercent = goal.tokenBudget ? Math.min(100, ((goal.tokensUsed ?? 0) / goal.tokenBudget) * 100) : 0
  const budgetLevel = budgetPercent > 95 ? 'error' : budgetPercent > 80 ? 'warning' : 'normal'
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

  useEffect(() => { mountedRef.current = true; return () => { mountedRef.current = false } }, [])
  useEffect(() => {
    if (editing) { setDraft(goal.objective); setEditError(''); inputRef.current?.focus() }
    else if (wasEditingRef.current && (!document.activeElement || document.activeElement === document.body)) triggerRef.current?.focus()
    wasEditingRef.current = editing
  }, [editing]) // seed the draft only on entering edit

  const saveEdit = async () => {
    if (savingRef.current || !onEdit) return
    const nextObjective = draft.trim()
    if (!nextObjective) { setEditError('Goal objective is required.'); return }
    savingRef.current = true
    setSaving(true)
    setEditError('')
    try {
      const saved = await onEdit(nextObjective)
      if (!mountedRef.current) return
      if (saved) setEditing(false)
      else setEditError('Failed to update goal.')
    } catch {
      if (mountedRef.current) setEditError('Failed to update goal.')
    } finally {
      savingRef.current = false
      if (mountedRef.current) setSaving(false)
    }
  }
  const cancelEdit = () => { if (!savingRef.current) setEditing(false) }

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
    <div className="uam-goal-banner text-sm">
      <div className="uam-goal-banner__header">
        <Target size={16} style={{ color: 'var(--accent)', flexShrink: 0 }} aria-hidden />
        <Tooltip label={goal.executionOwner === 'provider' ? 'Provider managed' : 'UAM managed'}>
          <span className="uam-goal-pill" data-tone={tone}>
            <StatusDot tone={tone} pulse={goal.status === 'active'} size={7} />
            {statusLabel(goal.status)}
          </span>
        </Tooltip>
        {goal.pendingContinuation ? <span className="text-xs" style={{ color: 'var(--text-3)' }}>Queued after current turn</span> : turnRunning && goal.status === 'paused' ? <span className="text-xs" style={{ color: 'var(--text-3)' }}>Current turn will finish</span> : null}
      </div>
      <Swap swapKey={editing ? 'edit' : 'view'}>
        {editing ? <div>
          <label className="mb-1 block text-xs font-medium" htmlFor="goal-objective-edit" style={{ color: 'var(--text-2)' }}>Objective</label>
          <textarea
            ref={inputRef}
            id="goal-objective-edit"
            rows={4}
            value={draft}
            disabled={saving}
            onChange={(event) => setDraft(event.target.value)}
            onKeyDown={(event) => {
              if (event.key === 'Escape') { event.preventDefault(); event.stopPropagation(); cancelEdit() }
              else if (event.key === 'Enter' && (event.metaKey || event.ctrlKey)) { event.preventDefault(); void saveEdit() }
            }}
            className="w-full resize-y rounded-md p-3 text-sm outline-none"
            style={{ minHeight: 96, maxHeight: '40vh', background: 'var(--bg)', border: '1px solid var(--border-bright)', color: 'var(--text)' }}
          />
          {editError && <div role="alert" className="mt-1 text-xs" style={{ color: 'var(--error)' }}>{editError}</div>}
        </div> : <p className="uam-goal-banner__objective">{goal.objective}</p>}
      </Swap>
      {blockerDisplay && (
        <p role="alert" className="text-xs" style={{ color: 'var(--error)' }}>
          <strong>{blockerLabel}:</strong> {blockerDisplay} {blockerAction}
        </p>
      )}

      <div className="uam-goal-banner__progress text-xs" style={{ color: 'var(--text-3)' }}>
        <span>Steps</span>
        <progress
          className="uam-goal-progress"
          aria-label="Goal progress"
          aria-valuetext={isComplete ? 'Goal complete' : totalItems > 0 ? `${completedCount} of ${totalItems} planned steps complete` : progressLabel}
          max={isComplete ? 1 : Math.max(totalItems, 1)}
          value={isComplete ? 1 : totalItems > 0 ? completedCount : undefined}
        />
        <span className="whitespace-nowrap">{progressLabel}</span>
        {budgetDisplay && goal.tokenBudget && (<>
          <span>Tokens</span>
          <span aria-hidden className="uam-goal-meter" data-level={budgetLevel}>
            <span style={{ width: `${budgetPercent}%` }} />
          </span>
          <span className="font-mono whitespace-nowrap" title="Tokens used / budget">{budgetDisplay}</span>
        </>)}
      </div>

      {(currentStep || completedItems.length > 0 || remainingItems.length > 0 || goal.lastDiagnostic || (goal.executionOwner === 'uam' && workerModelLabel && reviewerModelLabel)) && (
        <div
          data-goal-details-scroll
          role="region"
          aria-label="Goal details"
          tabIndex={0}
          className="uam-goal-banner__details grid gap-2 rounded-md p-3 text-xs"
          style={{ background: 'var(--bg)', border: '1px solid var(--border)' }}
        >
          {goal.executionOwner === 'uam' && workerModelLabel && reviewerModelLabel && <div><strong style={{ color: 'var(--text)' }}>Models:</strong> <span style={{ color: 'var(--text-2)' }}>Worker: {workerModelLabel} · Reviewer: {reviewerModelLabel} · locked</span></div>}
          {goal.lastDiagnostic && <div><strong style={{ color: 'var(--text)' }}>Last check:</strong> <span style={{ color: 'var(--text-2)' }}>{readableDiagnostic(goal.lastDiagnostic)}</span></div>}
          {currentStep && <div><strong style={{ color: 'var(--text)' }}>Current step:</strong> <span style={{ color: 'var(--text-2)' }}>{currentStep}</span></div>}
          {completedItems.length > 0 && <div><strong style={{ color: 'var(--text)' }}>Completed steps</strong><ul className="mt-1 list-disc pl-5" style={{ color: 'var(--text-2)' }}>{completedItems.map((item, index) => <li key={`${index}-${item}`}>{item}</li>)}</ul></div>}
          {remainingItems.length > 0 && <div><strong style={{ color: 'var(--text)' }}>Remaining steps</strong><ul className="mt-1 list-disc pl-5" style={{ color: 'var(--text-2)' }}>{remainingItems.map((item, index) => <li key={`${index}-${item}`}>{item}</li>)}</ul></div>}
        </div>
      )}

      {editing ? <div className="uam-goal-banner__footer">
        <span className="flex-1" />
        <Button type="button" size="sm" disabled={saving} onClick={cancelEdit}>Cancel</Button>
        <Button type="button" size="sm" variant="primary" disabled={saving} onClick={() => void saveEdit()}>{saving ? 'Saving…' : 'Save'}</Button>
      </div> : <div className="uam-goal-banner__footer">
      {/* All goal actions consolidated into one overflow menu */}
      <div ref={menuRef} className="relative flex-shrink-0">
        <IconButton
          ref={triggerRef}
          icon={<MoreHorizontal size={15} aria-hidden />}
          label="Goal actions"
          aria-haspopup="menu"
          aria-expanded={menuOpen}
          aria-controls={menuOpen ? menuId : undefined}
          className="uam-goal-action"
          data-open={menuOpen || undefined}
          onClick={() => setMenuOpen((v) => !v)}
        />
        {menuPresence.mounted && (
          <ViewportMenu
            ref={popupRef}
            anchorRef={triggerRef}
            align="end"
            id={menuId}
            role="menu"
            aria-label="Goal actions"
            data-state={menuPresence.closing ? 'closed' : 'open'}
            onRequestClose={() => setMenuOpen(false)}
            className="uam-goal-menu"
          >
            {goal.status === 'active' && (
              <MenuItem disabled={resumePending} icon={<Check size={14} aria-hidden />} label="Mark complete" onSelect={() => { setMenuOpen(false); onComplete() }} />
            )}
            {goal.status === 'active' && onPause && (
              <MenuItem disabled={resumePending} icon={<Pause size={14} aria-hidden />} label="Pause goal" onSelect={() => { setMenuOpen(false); onPause() }} />
            )}
            {(goal.status === 'paused' || goal.status === 'blocked') && onResume && (
              <MenuItem disabled={resumePending} icon={<Play size={14} aria-hidden />} label={resumePending ? 'Resuming…' : 'Resume goal'} onSelect={() => { setMenuOpen(false); onResume() }} />
            )}
            {onRestart && <MenuItem disabled={resumePending} icon={<Play size={14} aria-hidden />} label="Restart goal" onSelect={() => { setMenuOpen(false); onRestart() }} />}
            {!isComplete && goal.executionOwner !== 'provider' && onEdit && (
              <MenuItem disabled={resumePending} icon={<Pencil size={14} aria-hidden />} label="Edit goal" onSelect={() => { setMenuOpen(false); setEditing(true) }} />
            )}
            <MenuItem disabled={resumePending} icon={<Trash2 size={14} aria-hidden />} label="Delete goal" danger onSelect={() => { setMenuOpen(false); onRemove() }} />
          </ViewportMenu>
        )}
      </div>
        <span className="flex-1" />
        {goal.status === 'active' && onPause && (
          <Button type="button" size="sm" variant="secondary" disabled={resumePending} onClick={onPause}>Pause</Button>
        )}
        {(goal.status === 'paused' || goal.status === 'blocked') && onResume && (
          <Button type="button" size="sm" variant="secondary" disabled={resumePending} onClick={onResume}>{resumePending ? 'Resuming…' : 'Resume'}</Button>
        )}
      </div>}
    </div>
  )
}
