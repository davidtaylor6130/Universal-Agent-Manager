import { act, Profiler } from 'react'
import { createRoot } from 'react-dom/client'
import { expect, it, vi } from 'vitest'
import { SessionItem } from './SessionItem'
import { useAppStore } from '../../store/useAppStore'
import type { Session } from '../../types/session'

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

it('does not commit an unrelated real sidebar row when selection moves between two other chats', () => {
  const initial = useAppStore.getState()
  const values = new Map<string, string>()
  vi.stubGlobal('localStorage', { getItem: (key: string) => values.get(key) ?? null, setItem: (key: string, value: string) => values.set(key, value), removeItem: (key: string) => values.delete(key) })
  const now = new Date()
  const sessions: Session[] = ['select-a', 'select-b', 'select-unrelated'].map(id => ({ id, name: id, viewMode: 'chat', folderId: null, createdAt: now, updatedAt: now }))
  useAppStore.setState({ sessions, activeSessionId: sessions[0].id, folders: [], acpBindingBySessionId: {}, cliBindingBySessionId: {} })
  const host = document.createElement('div'); document.body.append(host); const root = createRoot(host)
  const commits = new Map<string, number>()
  try {
    act(() => root.render(<>{sessions.map(session => <Profiler key={session.id} id={session.id} onRender={id => commits.set(id, (commits.get(id) || 0) + 1)}><SessionItem sessionId={session.id} session={session} familySessionIds={[session.id]} /></Profiler>)}</>))
    const before = commits.get('select-unrelated')
    act(() => useAppStore.getState().setActiveSession('select-b'))
    expect(host.querySelector('[data-session-id="select-b"]')!.classList.contains('uam-sb-row--selected')).toBe(true)
    expect(commits.get('select-unrelated')).toBe(before)
    act(() => useAppStore.getState().setActiveSession('select-a'))
    expect(commits.get('select-unrelated')).toBe(before)
  } finally { act(() => root.unmount()); host.remove(); useAppStore.setState(initial, true); vi.unstubAllGlobals() }
})

it('shows a robot instead of the provider glyph only for subagent rows and leaves normal rows unchanged', () => {
  const initial = useAppStore.getState()
  const now = new Date()
  const session: Session = { id: 'icon-row', name: 'Icon row', viewMode: 'chat', folderId: null, providerId: 'codex-cli', createdAt: now, updatedAt: now }
  useAppStore.setState({ sessions: [session], activeSessionId: null, folders: [], showProviderIconsInSidebar: true, acpBindingBySessionId: {}, cliBindingBySessionId: {} })
  const host = document.createElement('div'); document.body.append(host); const root = createRoot(host)
  try {
    act(() => root.render(<SessionItem sessionId={session.id} session={session} familySessionIds={[session.id]} />))
    expect(host.querySelector('[aria-label^="Provider:"]')).not.toBeNull()
    expect(host.querySelector('[aria-label="Subagent"]')).toBeNull()
    act(() => root.render(<SessionItem sessionId={session.id} session={session} familySessionIds={[session.id]} subagentRow />))
    expect(host.querySelector('[aria-label="Subagent"] svg.lucide-bot')).not.toBeNull()
    expect(host.querySelector('[aria-label^="Provider:"]')).toBeNull()
  } finally { act(() => root.unmount()); host.remove(); useAppStore.setState(initial, true) }
})

it('derives the robot from verified native lineage on any sidebar path, while unlinked chats keep the provider glyph', () => {
  const initial = useAppStore.getState()
  const now = new Date()
  const base: Session = { id: 'lineage-parent', name: 'Lineage parent', viewMode: 'chat', folderId: null, providerId: 'codex-cli', createdAt: now, updatedAt: now }
  const child: Session = { ...base, id: 'lineage-child', name: 'Lineage child', subagentParentChatId: base.id, subagentParentCreatedAt: now.toISOString() }
  useAppStore.setState({ sessions: [base, child], activeSessionId: null, folders: [], showProviderIconsInSidebar: true, acpBindingBySessionId: {}, cliBindingBySessionId: {} })
  const host = document.createElement('div'); document.body.append(host); const root = createRoot(host)
  try {
    // No subagentRow prop: an ungrouped/active-list entry still shows the robot because the lineage is verified.
    act(() => root.render(<><SessionItem sessionId={child.id} session={child} /><SessionItem sessionId={base.id} session={base} /></>))
    expect(host.querySelector('[data-session-id="lineage-child"] [aria-label="Subagent"] svg.lucide-bot')).not.toBeNull()
    expect(host.querySelector('[data-session-id="lineage-child"] [aria-label^="Provider:"]')).toBeNull()
    expect(host.querySelector('[data-session-id="lineage-parent"] [aria-label^="Provider:"]')).not.toBeNull()
    expect(host.querySelector('[data-session-id="lineage-parent"] [aria-label="Subagent"]')).toBeNull()
  } finally { act(() => root.unmount()); host.remove(); useAppStore.setState(initial, true) }
})
