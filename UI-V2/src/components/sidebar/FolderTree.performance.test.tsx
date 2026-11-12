import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { expect, it, vi } from 'vitest'
import { FolderTree } from './FolderTree'
import { useAppStore } from '../../store/useAppStore'
import { expectWithinBudget, INPUT_BUDGET_MS, INTERACTION_BUDGET_MS } from '../../test/interactionBudgets'
import type { Folder, Session } from '../../types/session'

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
// Mounting 1,000 rows took 8s on a hosted macOS runner. It isn't what this test measures; the budgets above are.
}, 30_000)


it('follows the pointer within the input budget while dragging a workspace among 600 chats', () => {
  vi.useRealTimers()
  const initial = useAppStore.getState()
  const now = new Date()
  const folders: Folder[] = Array.from({ length: 120 }, (_, index) => ({ id: `drag-perf-${index}`, name: `Workspace ${index}`, parentId: null, directory: '', isExpanded: true, createdAt: now }))
  const sessions: Session[] = folders.flatMap((folder, index) => Array.from({ length: 5 }, (_, chat) => ({
    id: `drag-perf-${index}-${chat}`, name: `Chat ${chat}`, viewMode: 'chat' as const, folderId: folder.id, createdAt: now, updatedAt: now, providerId: 'gemini-cli',
  })))
  useAppStore.setState({ sessions, folders, resourceCollections: [], activeSessionId: null, messages: {}, cliBindingBySessionId: {}, acpBindingBySessionId: {}, reorderFolders: vi.fn(async () => true) })
  const host = document.createElement('div')
  document.body.append(host)
  const root = createRoot(host)
  const move = (type: string, y: number, target: EventTarget = document) =>
    target.dispatchEvent(new MouseEvent(type, { bubbles: true, cancelable: true, clientX: 10, clientY: y, button: 0 }))
  try {
    act(() => root.render(<FolderTree searchQuery="" />))
    const rows = [...host.querySelectorAll<HTMLElement>('[data-sort-id]')]
    expect(rows).toHaveLength(120)
    rows.forEach((row, index) => vi.spyOn(row, 'getBoundingClientRect').mockReturnValue({ top: index * 150, bottom: index * 150 + 150, height: 150, left: 0, right: 200, width: 200, x: 0, y: index * 150, toJSON: () => ({}) }))
    act(() => move('pointerdown', 10, host.querySelector('[data-testid="folder-header-drag-perf-0"]')!))
    for (let step = 1; step <= 60; step++) {
      expectWithinBudget('workspace drag pointer move', INPUT_BUDGET_MS, () => act(() => move('pointermove', 10 + step * 250)))
    }
    expect(rows[1].style.transform).toBe('translate3d(0, -150px, 0)')
    act(() => { document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })) })
    act(() => move('pointerup', 10))
  } finally {
    act(() => root.unmount())
    host.remove()
    useAppStore.setState(initial, true)
  }
}, 30_000)
