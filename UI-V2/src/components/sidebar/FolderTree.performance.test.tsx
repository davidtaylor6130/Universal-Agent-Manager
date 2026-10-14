import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { expect, it, vi } from 'vitest'
import { FolderTree } from './FolderTree'
import { useAppStore } from '../../store/useAppStore'
import { expectWithinBudget, INTERACTION_BUDGET_MS } from '../../test/interactionBudgets'
import type { Session } from '../../types/session'

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

it('commits chat selection within 250ms with 1,000 mounted sidebar rows', () => {
  vi.useRealTimers()
  const initial = useAppStore.getState()
  const stored = new Map<string, string>()
  vi.stubGlobal('localStorage', {
    getItem: (key: string) => stored.get(key) ?? null,
    setItem: (key: string, value: string) => stored.set(key, value),
    removeItem: (key: string) => stored.delete(key),
  })
  const now = new Date()
  const sessions: Session[] = Array.from({ length: 1000 }, (_, index) => ({
    id: `sidebar-perf-${index}`, name: `Chat ${index}`, viewMode: 'chat', folderId: 'sidebar-perf-folder',
    createdAt: now, updatedAt: now, providerId: 'gemini-cli',
  }))
  useAppStore.setState({ sessions, folders: [{ id: 'sidebar-perf-folder', name: 'Fixture workspace', parentId: null, directory: '', isExpanded: false, createdAt: now }], resourceCollections: [], activeSessionId: sessions[0].id,
    messages: {}, cliBindingBySessionId: {}, acpBindingBySessionId: {} })
  const host = document.createElement('div')
  document.body.append(host)
  const root = createRoot(host)
  try {
    // Initial mounting is a separate startup cost; measure selection with real mounted rows.
    act(() => root.render(<FolderTree searchQuery="" />))
    expect(host.querySelectorAll('[data-session-id]')).toHaveLength(1000)
    for (const index of [1, 0, 1]) {
      const id = sessions[index].id
      const row = host.querySelector<HTMLElement>(`[data-session-id="${id}"]`)!
      expectWithinBudget('large sidebar selection to committed state', INTERACTION_BUDGET_MS, () => {
        act(() => row.click())
        expect(useAppStore.getState().activeSessionId).toBe(id)
        expect(row.classList.contains('uam-sb-row--selected')).toBe(true)
      })
    }
  } finally {
    act(() => root.unmount())
    host.remove()
    useAppStore.setState(initial, true)
    vi.unstubAllGlobals()
  }
})
