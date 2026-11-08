import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, describe, expect, it, vi } from 'vitest'
import type { Goal } from '../../types/goal'
import { GoalBanner } from './GoalBanner'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
globalThis.ResizeObserver ??= class ResizeObserver {
  observe() {}
  unobserve() {}
  disconnect() {}
}

const goal: Goal = {
  id: 'goal-1',
  chatId: 'chat-1',
  objective: 'Finish the reliability pass',
  status: 'active',
  executionOwner: 'uam',
  completedItems: ['Baseline'],
  remainingItems: ['Fix regressions', 'Verify the package'],
  currentStep: 'Fix regressions',
  createdAt: new Date('2026-01-01T00:00:00Z'),
  updatedAt: new Date('2026-01-01T00:00:00Z'),
}

function renderGoal(overrides: Partial<Goal> = {}, onEdit = vi.fn(async () => true), attentionSource?: string) {
  const host = document.createElement('div')
  document.body.appendChild(host)
  const root = createRoot(host)
  act(() => root.render(
    <GoalBanner
      goal={{ ...goal, ...overrides }}
      onComplete={() => {}}
      onPause={() => {}}
      onResume={() => {}}
      onRemove={() => {}}
      onEdit={onEdit}
      attentionSource={attentionSource}
    />,
  ))
  return { host, root, onEdit }
}

afterEach(() => {
  document.body.replaceChildren()
})

/** Shared MenuItem rows carry their label as text, not aria-label. */
function menuItem(label: string) {
  return Array.from(document.querySelectorAll<HTMLButtonElement>('[role="menuitem"]')).find((item) => item.textContent === label) ?? null
}

describe('GoalBanner', () => {
  it('edits in place with the goal details still visible, and Escape cancels the edit', () => {
    const { host, root, onEdit } = renderGoal()
    act(() => host.querySelector<HTMLButtonElement>('[aria-label="Goal actions"]')!.click())
    act(() => menuItem('Edit goal')!.click())
    const textarea = host.querySelector<HTMLTextAreaElement>('textarea#goal-objective-edit')!
    expect(document.activeElement).toBe(textarea)
    expect(host.querySelector('[aria-label="Goal progress"]')).toBeTruthy()
    expect(document.querySelector('[role="dialog"]')).toBeNull()
    act(() => textarea.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true })))
    expect(host.querySelector('textarea')).toBeNull()
    expect(onEdit).not.toHaveBeenCalled()
    expect(document.activeElement).toBe(host.querySelector('[aria-label="Goal actions"]'))
    act(() => root.unmount())
  })

  it('offers restart while a turn runs and shows its pending continuation', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    const restart = vi.fn()
    act(() => root.render(<GoalBanner goal={{ ...goal, pendingContinuation: true }} onComplete={vi.fn()} onRemove={vi.fn()} onRestart={restart} turnRunning />))
    expect(host.textContent).toContain('Queued after current turn')
    act(() => (host.querySelector('[aria-label="Goal actions"]') as HTMLButtonElement).click())
    const button = Array.from(document.body.querySelectorAll('button')).find((item) => item.textContent?.includes('Restart goal'))!
    expect(button.disabled).toBe(false)
    act(() => button.click())
    expect(restart).toHaveBeenCalledTimes(1)
    act(() => root.render(<GoalBanner goal={{ ...goal, status: 'paused' }} onComplete={vi.fn()} onRemove={vi.fn()} turnRunning />))
    expect(host.textContent).toContain('Current turn will finish')
    act(() => root.unmount())
  })

  it('shows the full objective and exact completed and remaining steps as stacked rows', () => {
    const { host, root } = renderGoal()

    expect(host.querySelector('details')).toBeNull()
    expect(host.querySelector('.uam-goal-banner__objective')?.textContent).toBe(goal.objective)
    expect(host.querySelector('.uam-goal-banner__footer [aria-label="Goal actions"]')).toBeTruthy()
    const scrollRegion = host.querySelector('[data-goal-details-scroll]') as HTMLElement
    expect(scrollRegion).toBeTruthy()
    expect(scrollRegion.className).toContain('uam-goal-banner__details')
    expect(scrollRegion.tabIndex).toBe(0)
    expect(scrollRegion.getAttribute('aria-label')).toBe('Goal details')
    expect(host.textContent).toContain('Finish the reliability pass')
    expect(host.textContent).toContain('Completed')
    expect(host.textContent).toContain('Baseline')
    expect(host.textContent).toContain('Remaining')
    expect(host.textContent).toContain('Verify the package')

    act(() => root.unmount())
  })

  it('keeps truthful planned-step progress visible beside the details', () => {
    const { host, root } = renderGoal({
      completedItems: ['T01 Baseline'],
      remainingItems: ['T02 Fix regressions', 'T03: Verify the package'],
      currentStep: 'T02 Fix regressions',
    })

    const progress = host.querySelector('progress[aria-label="Goal progress"]') as HTMLProgressElement
    expect(progress).toBeTruthy()
    expect(progress.closest('.uam-goal-banner__progress')).toBeTruthy()
    expect(progress.closest('[data-goal-details-scroll]')).toBeNull()
    expect(host.textContent).toContain('1 of 3 steps')
    expect(host.textContent).toContain('Current step: Fix regressions')
    expect(host.textContent).toContain('Completed steps')
    expect(host.textContent).toContain('Remaining steps')
    expect(host.textContent).not.toContain('1/3')
    expect(host.textContent).not.toMatch(/T0[1-3]/)

    act(() => root.unmount())
  })

  it('renders stale legacy completion as complete instead of a false partial fraction', () => {
    const { host, root } = renderGoal({
      status: 'complete',
      completedItems: ['Verified work'],
      remainingItems: ['Stale remaining work'],
      currentStep: 'Stale remaining work',
    })

    const progress = host.querySelector('progress[aria-label="Goal progress"]') as HTMLProgressElement
    expect(progress.value).toBe(progress.max)
    expect(progress.getAttribute('aria-valuetext')).toBe('Goal complete')
    expect(host.textContent).toContain('Complete')
    expect(host.textContent).not.toContain('1 of 2')
    expect(host.textContent).not.toContain('Remaining steps')
    expect(host.textContent).not.toContain('Current step')

    act(() => root.unmount())
  })

	it('keeps the blocker category, cause, and next action visible', () => {
		const { host, root } = renderGoal({
			status: 'blocked',
			lastBlockerKind: 'user_permission_approval',
			lastBlocker: 'The remote helper needs Documents access.',
			lastDiagnostic: 'goal_blocked_invalid_review',
			currentStep: 'Continue processing the files',
		})

		const alert = host.querySelector('[role="alert"]') as HTMLElement
		expect(alert).toBeTruthy()
		expect(alert.textContent).toContain('Needs access')
		expect(alert.textContent).toContain('The remote helper needs Documents access.')
		expect(alert.textContent).toContain('Approve or deny the request, then resume.')
		expect(alert.textContent).not.toContain('Continue processing the files')
		expect(host.textContent).toContain('Last check:')
		expect(host.textContent).toContain('The completion review was invalid.')
		expect(host.textContent).toContain('Current step: Continue processing the files')

		act(() => root.unmount())
	})

	it.each([
		['model:reviewer-1', 'Last decision: Model review · reviewer-1'],
		['model:', 'Last decision: Model review · model not reported'],
		['filter', 'Last decision: UAM rule · not a model decision'],
		['fallback', 'Last decision: Fallback · no model decision'],
		['unknown', 'Last decision: Unknown · source not recorded'],
		['reviewer-1', 'Last decision: Unknown · source not recorded'],
	])('reports decision source %s without inventing a model review', (lastDecisionSource, expected) => {
		const { host, root } = renderGoal({ lastDecisionSource })
		expect(host.textContent).toContain(expected)
		if (!lastDecisionSource.startsWith('model:')) expect(host.textContent).not.toContain('Model review')
		act(() => root.unmount())
	})

	it('omits the decision row until a decision exists', () => {
		const { host, root } = renderGoal()
		expect(host.textContent).not.toContain('Last decision')
		expect(host.textContent).not.toContain('Last attention flag')
		act(() => root.unmount())
	})

	it('reports the attention flag source with the shared labels', () => {
		const { host, root } = renderGoal({}, undefined, 'filter')
		expect(host.textContent).toContain('Last attention flag: UAM rule · not a model decision')
		expect(host.textContent).not.toContain('Model review ·')
		act(() => root.unmount())
	})

	it('recognizes legacy remote blockers without a stored kind', () => {
		const { host, root } = renderGoal({
			status: 'blocked',
			lastBlocker: 'The remote turn no longer exists on the selected runner.',
			lastBlockerKind: '',
		})

		expect(host.textContent).toContain('Needs connection')
		expect(host.textContent).toContain('Reconnect the host, then resume.')
		act(() => root.unmount())
	})

	it('shows legacy remote process exits as connection recovery', () => {
		const { host, root } = renderGoal({
			status: 'blocked',
			lastBlocker: 'Codex app-server process exited during an active turn.',
			lastBlockerKind: '',
		})

		expect(host.textContent).toContain('Needs connection')
		expect(host.textContent).toContain('Reconnect the host, then resume.')
		act(() => root.unmount())
	})

  it('saves trimmed text in place and keeps validation errors in the window', async () => {
    const { host, root, onEdit } = renderGoal()
    act(() => { (host.querySelector('[aria-label="Goal actions"]') as HTMLButtonElement).click() })
    act(() => { menuItem('Edit goal')!.click() })
    const textarea = host.querySelector('textarea') as HTMLTextAreaElement
    expect(textarea.value).toBe(goal.objective)
    const setValue = (value: string) => act(() => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set?.call(textarea, value)
      textarea.dispatchEvent(new Event('input', { bubbles: true }))
    })
    const save = () => Array.from(host.querySelectorAll('button')).find((b) => b.textContent === 'Save')!
    setValue('   ')
    await act(async () => { save().click() })
    expect(host.querySelector('[role="alert"]')?.textContent).toBe('Goal objective is required.')
    expect(onEdit).not.toHaveBeenCalled()
    setValue('  Updated objective  ')
    await act(async () => { save().click() })
    expect(onEdit).toHaveBeenCalledWith('Updated objective')
    expect(host.querySelector('textarea')).toBeNull()
    act(() => root.unmount())
  })

  it('does not offer editing for provider-managed goals', () => {
    const { host, root } = renderGoal({ executionOwner: 'provider' })
    act(() => { (host.querySelector('[aria-label="Goal actions"]') as HTMLButtonElement).click() })
    expect(menuItem('Edit goal')).toBeNull()
    act(() => root.unmount())
  })

  it('keeps completed goals deletable but not editable or resumable', () => {
    const completed = renderGoal({ status: 'complete' })
    act(() => { (completed.host.querySelector('[aria-label="Goal actions"]') as HTMLButtonElement).click() })
    expect(menuItem('Edit goal')).toBeNull()
    expect(menuItem('Resume goal')).toBeNull()
    expect(menuItem('Delete goal')).toBeTruthy()
    act(() => completed.root.unmount())
  })

  it('exposes goal actions as a keyboard-managed menu and returns focus on Escape', () => {
    const { host, root } = renderGoal()
    const trigger = host.querySelector('[aria-label="Goal actions"]') as HTMLButtonElement
    act(() => trigger.click())

    const menu = document.body.querySelector('[role="menu"][aria-label="Goal actions"]') as HTMLElement
    expect(menu).toBeTruthy()
    expect(document.activeElement).toBe(menu.querySelector('[role="menuitem"]'))
    act(() => menu.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })))
    expect(document.body.querySelector('[role="menu"][aria-label="Goal actions"]')).toBeNull()
    expect(document.activeElement).toBe(trigger)

    act(() => root.unmount())
  })
})
