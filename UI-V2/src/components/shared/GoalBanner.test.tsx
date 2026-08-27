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

function renderGoal(overrides: Partial<Goal> = {}, onEdit = vi.fn(async () => true)) {
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
    />,
  ))
  return { host, root, onEdit }
}

afterEach(() => {
  document.body.replaceChildren()
})

describe('GoalBanner', () => {
  it('expands the full objective and exact completed and remaining steps', () => {
    const { host, root } = renderGoal()

    expect(host.querySelector('details')).toBeTruthy()
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

  it('keeps truthful planned-step progress visible while details are collapsed', () => {
    const { host, root } = renderGoal({
      completedItems: ['T01 Baseline'],
      remainingItems: ['T02 Fix regressions', 'T03: Verify the package'],
      currentStep: 'T02 Fix regressions',
    })

    const summary = host.querySelector('summary') as HTMLElement
    expect(summary.className).toContain('flex')
    expect(summary.querySelector('[data-goal-disclosure-icon]')).toBeTruthy()
    expect(summary.getAttribute('aria-label')).toBeNull()
    const progress = host.querySelector('progress[aria-label="Goal progress"]') as HTMLProgressElement
    expect(progress).toBeTruthy()
    expect(progress.closest('details')).toBeNull()
    expect(progress.closest('.uam-goal-banner__secondary')).toBeNull()
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

  it('edits a UAM-managed goal in a screen-level modal and saves trimmed text', async () => {
    const { host, root, onEdit } = renderGoal()

    act(() => { (host.querySelector('[aria-label="Goal actions"]') as HTMLButtonElement).click() })
    act(() => { (document.body.querySelector('button[aria-label="Edit goal"]') as HTMLButtonElement).click() })

    const dialog = document.body.querySelector('[role="dialog"][aria-label="Edit goal"]') as HTMLElement
    expect(dialog).toBeTruthy()
    expect(dialog.parentElement?.parentElement).toBe(document.body)
    const textarea = dialog.querySelector('textarea') as HTMLTextAreaElement
    expect(textarea.value).toBe(goal.objective)

    act(() => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set?.call(textarea, '  Updated objective  ')
      textarea.dispatchEvent(new Event('input', { bubbles: true }))
    })
    await act(async () => { (dialog.querySelector('button[type="submit"]') as HTMLButtonElement).click() })

    expect(onEdit).toHaveBeenCalledWith('Updated objective')
    expect(document.body.querySelector('[role="dialog"][aria-label="Edit goal"]')).toBeNull()
    act(() => root.unmount())
  })

  it('does not offer editing for provider-managed or completed goals', () => {
    const { host, root } = renderGoal({ executionOwner: 'provider' })
    act(() => { (host.querySelector('[aria-label="Goal actions"]') as HTMLButtonElement).click() })
    expect(document.body.querySelector('button[aria-label="Edit goal"]')).toBeNull()
    act(() => root.unmount())

    const completed = renderGoal({ status: 'complete' })
    expect(completed.host.querySelector('[aria-label="Goal actions"]')).toBeNull()
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
