import { act, memo } from 'react'
import { createRoot } from 'react-dom/client'
import { expect, it, vi } from 'vitest'
import { useAppStore } from '../../store/useAppStore'
import { FolderTree } from './FolderTree'
import type { Session } from '../../types/session'

const rows = vi.hoisted(() => new Map<string, { renders: number; family: string[] }>())
vi.mock('./SessionItem', () => ({ SessionItem: memo(function Row(props: {
  sessionId: string
  familySessionIds: string[]
  onSessionClick: (id: string, event: React.MouseEvent<HTMLDivElement>) => boolean
}) {
  rows.set(props.sessionId, { renders: (rows.get(props.sessionId)?.renders ?? 0) + 1, family: props.familySessionIds })
  return <div data-session-id={props.sessionId} onClick={event => props.onSessionClick(props.sessionId, event)} />
}) }))

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

it('retains unrelated row props through selection and metadata updates while refreshing changed families', () => {
  const initial = useAppStore.getState()
  const now = new Date()
  const sessions: Session[] = Array.from({ length: 20 }, (_, index) => ({
    id: `render-${index}`, name: `Chat ${index}`, viewMode: 'chat', folderId: null, createdAt: now, updatedAt: now,
  }))
  rows.clear()
  useAppStore.setState({ sessions, folders: [], resourceCollections: [], activeSessionId: sessions[0].id,
    acpBindingBySessionId: {}, cliBindingBySessionId: {} })
  const host = document.createElement('div')
  document.body.append(host)
  const root = createRoot(host)
  try {
    act(() => root.render(<FolderTree searchQuery="" />))
    const before = rows.get('render-10')!.renders
    act(() => host.querySelector<HTMLElement>('[data-session-id="render-1"]')!.click())
    expect(rows.get('render-10')!.renders).toBe(before)
    act(() => useAppStore.setState({ activeSessionId: 'render-1', sessions: sessions.map(session =>
      session.id === 'render-1' ? { ...session, lastOpenedAt: new Date(now.getTime() + 1000) } : session) }))
    expect(rows.get('render-10')!.renders).toBe(before)
    const rootRenders = rows.get('render-1')!.renders
    act(() => useAppStore.setState(state => ({ sessions: [...state.sessions,
      { ...sessions[1], id: 'child', branchRootChatId: 'render-1', parentChatId: 'render-1' }] })))
    expect(rows.get('render-1')!.renders).toBeGreaterThan(rootRenders)
    expect(rows.get('render-1')!.family).toEqual(['render-1', 'child'])
    expect(rows.get('render-10')!.renders).toBe(before)
  } finally {
    act(() => root.unmount())
    host.remove()
    useAppStore.setState(initial, true)
  }
})
