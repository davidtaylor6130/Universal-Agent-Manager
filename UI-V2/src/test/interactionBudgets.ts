import { expect, vi } from 'vitest'

export const INTERACTION_BUDGET_MS = 250
export const INPUT_BUDGET_MS = 100
export const HISTORY_PAGE_BUDGET_MS = 1000
export const STARTUP_BUDGET_MS = 10000

/** Measure actual code-path completion on the executing machine, never fake time. */
export function expectWithinBudget(label: string, budgetMs: number, operation: () => void): void {
  if (vi.isFakeTimers()) throw new Error('Performance budgets require real timers.')
  const start = performance.now()
  operation()
  const elapsed = performance.now() - start
  expect(elapsed, `${label}: ${elapsed.toFixed(2)}ms exceeded ${budgetMs}ms`).toBeLessThanOrEqual(budgetMs)
}
