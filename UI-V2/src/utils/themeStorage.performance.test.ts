import { expect, it } from 'vitest'
import { applyDocumentTheme, normalizeCustomTheme } from './themeStorage'
import { expectWithinBudget, INPUT_BUDGET_MS } from '../test/interactionBudgets'

it.each(['focus', 'light', 'midnight'] as const)('reapplies %s 200 times within the input budget', (theme) => {
  applyDocumentTheme(theme)
  expectWithinBudget(`reapplying ${theme}`, INPUT_BUDGET_MS, () => {
    for (let index = 0; index < 200; index++) applyDocumentTheme(theme)
  })
})

it('reapplies a custom theme 200 times within the input budget', () => {
  const theme = normalizeCustomTheme({
    version: 1,
    id: 'custom:ocean',
    name: 'Ocean',
    base: 'dark',
    colors: {
      background: '#101820', surface: '#17212B', surfaceUp: '#22303c', text: '#f1f5f9',
      textMuted: '#94a3b8', accent: '#38bdf8', sidebar: '#0b1219', userMessage: '#173047',
      assistantMessage: '#17212b', success: '#22c55e', warning: '#f59e0b', error: '#ef4444',
    },
  })
  if (!theme) throw new Error('Expected valid custom theme')
  applyDocumentTheme(theme.id, [theme])
  expectWithinBudget('reapplying a custom theme', INPUT_BUDGET_MS, () => {
    for (let index = 0; index < 200; index++) applyDocumentTheme(theme.id, [theme])
  })
})
