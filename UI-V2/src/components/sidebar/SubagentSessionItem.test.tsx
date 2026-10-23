import { act } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { OrphanSubagentItem, SubagentSearchMatches, SubagentSessionItem } from './SubagentSessionItem'
import { useAppStore } from '../../store/useAppStore'
import type { Session } from '../../types/session'

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
const now = new Date(2026, 9, 20)
const parent: Session = { id: 'lazy-parent', name: 'Lazy parent', viewMode: 'chat', folderId: null, providerId: 'codex-cli', createdAt: now, updatedAt: now, managedSubagentCount: 1 }
const run = { runId: 'run-1', transcriptChatId: 'transcript-1', agentId: 'Reviewer', taskTitle: 'Review the sidebar diff', status: 'running', canCancel: true, providerId: 'codex-cli', modelId: 'gpt', parentRunId: '' }
type Observer = { callback: (entries: Array<{ isIntersecting: boolean }>) => void; options: IntersectionObserverInit | undefined; disconnected: boolean }
let observers: Observer[]
let host: HTMLDivElement, scroller: HTMLDivElement, root: Root
let snapshot: ReturnType<typeof useAppStore.getState>, previousCef: Window['cefQuery']
let actions: string[]
const intersect = (isIntersecting: boolean) => act(async () => observers.filter(observer => !observer.disconnected).forEach(observer => observer.callback([{ isIntersecting }])))

beforeEach(() => {
  snapshot = useAppStore.getState(); previousCef = window.cefQuery; actions = []; observers = []
  vi.stubGlobal('ResizeObserver', class { observe() {} unobserve() {} disconnect() {} })
  vi.stubGlobal('IntersectionObserver', class {
    entry: Observer
    constructor(callback: Observer['callback'], options?: IntersectionObserverInit) { this.entry = { callback, options, disconnected: false }; observers.push(this.entry) }
    observe() {}
    disconnect() { this.entry.disconnected = true }
  })
  window.cefQuery = ({ request, onSuccess }) => {
    const query = JSON.parse(request); actions.push(query.action)
    onSuccess(JSON.stringify({ chatId: parent.id, controlScopeId: 'scope', runs: [run], offset: 0, nextOffset: null, totalCount: 1 }))
  }
  useAppStore.setState({ sessions: [parent], activeSessionId: null, folders: [], showProviderIconsInSidebar: true, acpBindingBySessionId: {}, cliBindingBySessionId: {} })
  scroller = document.createElement('div'); scroller.style.overflowY = 'auto'
  host = document.createElement('div'); scroller.append(host); document.body.append(scroller); root = createRoot(host)
})
afterEach(() => { act(() => root.unmount()); scroller.remove(); window.cefQuery = previousCef; useAppStore.setState(snapshot, true); vi.useRealTimers(); vi.unstubAllGlobals() })

const toggle = () => host.querySelector<HTMLButtonElement>('button.uam-subagent-toggle')!
const expand = () => act(async () => toggle().click())
const mount = () => act(async () => root.render(<SubagentSessionItem sessionId={parent.id} session={parent} familySessionIds={[parent.id]} />))

describe('SubagentSessionItem sidebar lazy reader', () => {
  it('is closed by default with one accessible toggle, and issues no managed read, observer or poll until opened', async () => {
    vi.useFakeTimers()
    await mount()
    const button = toggle()
    expect(button.getAttribute('aria-expanded')).toBe('false')
    expect(document.getElementById(button.getAttribute('aria-controls')!)?.hasAttribute('hidden')).toBe(true)
    expect(observers).toHaveLength(0)
    expect(host.querySelector('[data-managed-run]')).toBeNull()
    await act(async () => { await vi.advanceTimersByTimeAsync(5 * 60000) })
    expect(actions).toEqual([])
  })

  it('opens from the keyboard-reachable toggle, then observes with the sidebar scroll container as root and a 240px margin before reading once near', async () => {
    await mount()
    await expand()
    expect(toggle().getAttribute('aria-expanded')).toBe('true')
    expect(observers).toHaveLength(1)
    expect(observers[0].options?.root).toBe(scroller)
    expect(observers[0].options?.rootMargin).toBe('240px')
    expect(actions).toEqual([])
    await intersect(true)
    expect(actions).toEqual(['listManagedSubagents'])
    const open = host.querySelector<HTMLButtonElement>('[role="group"][aria-label="Subagents of Lazy parent"] [data-managed-run="run-1"]')!
    // Primary title is the task; the agent definition is secondary detail only.
    expect(open.textContent).toContain('Review the sidebar diff')
    expect(open.textContent).not.toContain('Reviewer')
    expect(open.getAttribute('aria-label')).toBe('Open managed conversation for Review the sidebar diff')
  })

  it('labels a run without a task title generically instead of using its agent definition', async () => {
    window.cefQuery = ({ request, onSuccess }) => {
      actions.push(JSON.parse(request).action)
      onSuccess(JSON.stringify({ chatId: parent.id, controlScopeId: 'scope', runs: [{ ...run, taskTitle: undefined }], offset: 0, nextOffset: null, totalCount: 1 }))
    }
    await mount(); await expand(); await intersect(true)
    expect(host.querySelector('[data-managed-run="run-1"]')?.getAttribute('aria-label')).toBe('Open managed conversation for Subagent task')
    expect(host.textContent).not.toContain('Reviewer')
  })

  it('collapse pauses without a new read, keeps rows stale and without Stop, and reopening reads exactly once', async () => {
    vi.useFakeTimers()
    await mount(); await expand(); await intersect(true)
    expect(actions).toHaveLength(1)
    expect(host.querySelector('[aria-label^="Stop managed subagent"]')).not.toBeNull()
    await expand() // collapse
    expect(toggle().getAttribute('aria-expanded')).toBe('false')
    await act(async () => { await vi.advanceTimersByTimeAsync(5 * 60000) })
    expect(actions).toHaveLength(1)
    expect(host.querySelector('[data-managed-run="run-1"]')).not.toBeNull() // cached, hidden
    await expand() // reopen
    await intersect(true)
    expect(actions).toHaveLength(2)
    expect(host.querySelector('[data-stale]')).toBeNull()
    expect(host.querySelector('[aria-label^="Stop managed subagent"]')).not.toBeNull()
  })

  it('keeps its reader and rows when scrolled away, stops polling, and reads again when it returns', async () => {
    vi.useFakeTimers()
    await mount(); await expand()
    await intersect(true)
    expect(actions).toHaveLength(1)
    await intersect(false)
    expect(observers.filter(observer => !observer.disconnected)).toHaveLength(1) // still observing so it can return
    expect(host.querySelector('[data-managed-run="run-1"]')).not.toBeNull()
    await act(async () => { await vi.advanceTimersByTimeAsync(5 * 60000) })
    expect(actions).toHaveLength(1)
    await intersect(true)
    expect(actions).toHaveLength(2)
    expect(host.querySelector('[data-managed-run="run-1"]')).not.toBeNull()
  })

  it('keeps each parent\'s disclosure independent', async () => {
    const other: Session = { ...parent, id: 'other-parent', name: 'Other parent' }
    useAppStore.setState({ sessions: [parent, other] })
    await act(async () => root.render(<><SubagentSessionItem sessionId={parent.id} session={parent} familySessionIds={[parent.id]} /><SubagentSessionItem sessionId={other.id} session={other} familySessionIds={[other.id]} /></>))
    const buttons = host.querySelectorAll<HTMLButtonElement>('button.uam-subagent-toggle')
    await act(async () => buttons[0].click())
    expect([...host.querySelectorAll('button.uam-subagent-toggle')].map(button => button.getAttribute('aria-expanded'))).toEqual(['true', 'false'])
  })

  it('renders an orphan subagent as a robot row aligned like a normal row', async () => {
    const orphan: Session = { ...parent, id: 'orphan-child', name: 'Orphan child', managedSubagentCount: 0, subagentParentChatId: 'missing-parent', subagentParentCreatedAt: now.toISOString() }
    useAppStore.setState({ sessions: [orphan] })
    await act(async () => root.render(<OrphanSubagentItem child={orphan} />))
    const row = host.querySelector<HTMLButtonElement>('button[aria-label="Open read-only subagent Orphan child"]')!
    expect(row.querySelector('svg.lucide-bot')).not.toBeNull()
    expect(row.classList.contains('mx-1')).toBe(true)
  })
})

describe('SubagentSessionItem native child disclosure', () => {
  const plain: Session = { ...parent, managedSubagentCount: 0 }
  const link = (id: string, owner: Session): Session => ({ ...plain, id, name: id, subagentParentChatId: owner.id, subagentParentCreatedAt: owner.createdAt.toISOString() })
  const child = link('native-child', plain)
  const grandchild = link('native-grandchild', child)
  const draw = (owner: Session, matches: string[] = []) => act(async () => root.render(<SubagentSearchMatches.Provider value={new Set(matches)}>
    <SubagentSessionItem sessionId={owner.id} session={owner} familySessionIds={[owner.id]} /></SubagentSearchMatches.Provider>))
  const row = (id: string) => host.querySelector(`[data-session-id="${id}"]`)
  const toggles = () => [...host.querySelectorAll<HTMLButtonElement>('button.uam-subagent-toggle')]
  beforeEach(() => useAppStore.setState({ sessions: [plain, child, grandchild] }))

  it('lists no native child until its parent is opened', async () => {
    await draw(plain)
    expect(toggles().map(button => button.getAttribute('aria-expanded'))).toEqual(['false'])
    expect(row(child.id)).toBeNull()
    await act(async () => toggles()[0].click())
    expect(row(child.id)).not.toBeNull()
    expect(toggles().map(button => button.getAttribute('aria-expanded'))).toEqual(['true', 'false'])
    expect(row(grandchild.id)).toBeNull()
  })

  it('opens the ancestors of a selected descendant once and lets the user collapse them without reopening', async () => {
    useAppStore.setState({ activeSessionId: grandchild.id })
    await draw(plain)
    expect(row(grandchild.id)).not.toBeNull()
    await act(async () => toggles()[0].click())
    expect(toggles()[0].getAttribute('aria-expanded')).toBe('false')
    expect(row(child.id)).toBeNull()
    await act(async () => useAppStore.setState({ sessions: [{ ...plain }, child, grandchild] }))
    await draw(plain)
    expect(toggles()[0].getAttribute('aria-expanded')).toBe('false')
    expect(row(grandchild.id)).toBeNull()
  })

  it('shows a matching descendant through its closed parents only while the search holds, without changing their choice', async () => {
    await draw(plain, [grandchild.id])
    expect(row(grandchild.id)).not.toBeNull()
    expect(toggles().every(button => button.disabled && button.getAttribute('aria-expanded') === 'true')).toBe(true)
    await draw(plain, [])
    expect(row(child.id)).toBeNull()
    expect(toggles().map(button => ({ open: button.getAttribute('aria-expanded'), locked: button.disabled }))).toEqual([{ open: 'false', locked: false }])
  })

  it('keeps disclosure state out of other parents', async () => {
    const sibling: Session = { ...plain, id: 'sibling', name: 'Sibling' }
    const siblingChild = link('sibling-child', sibling)
    useAppStore.setState({ sessions: [plain, child, sibling, siblingChild] })
    await act(async () => root.render(<><SubagentSessionItem sessionId={plain.id} session={plain} familySessionIds={[plain.id]} /><SubagentSessionItem sessionId={sibling.id} session={sibling} familySessionIds={[sibling.id]} /></>))
    await act(async () => toggles()[1].click())
    expect(row(siblingChild.id)).not.toBeNull()
    expect(row(child.id)).toBeNull()
  })

  it('does not mount or read a managed ledger for a parent that is only open because a search matched its native child', async () => {
    const managedParent: Session = { ...plain, managedSubagentCount: 1 }
    const matched = link('matched-native', managedParent)
    useAppStore.setState({ sessions: [managedParent, matched] })
    await draw(managedParent, [matched.id])
    expect(row(matched.id)).not.toBeNull()
    expect(observers).toHaveLength(0)
    expect(actions).toEqual([])
    expect(host.querySelector('[data-managed-run]')).toBeNull()
  })
  it('opens for a different selected native child, while unchanged selection and token updates keep user collapse', async () => {
    useAppStore.setState({ activeSessionId: grandchild.id })
    await draw(plain)
    await act(async () => toggles()[0].click())
    expect(toggles()[0].getAttribute('aria-expanded')).toBe('false')
    await act(async () => useAppStore.setState({ messages: { [grandchild.id]: [] } }))
    expect(toggles()[0].getAttribute('aria-expanded')).toBe('false')
    await act(async () => useAppStore.setState({ activeSessionId: child.id }))
    expect(toggles()[0].getAttribute('aria-expanded')).toBe('true')
    expect(row(child.id)).not.toBeNull()
  })

})


it('does not mount a closed managed reader when IntersectionObserver is unavailable', async () => {
  vi.stubGlobal('IntersectionObserver', undefined)
  vi.useFakeTimers()
  await mount()
  await act(async () => { await vi.advanceTimersByTimeAsync(180000) })
  expect(toggle().getAttribute('aria-expanded')).toBe('false')
  expect(actions).toEqual([])
  await expand()
  expect(actions).toEqual(['listManagedSubagents'])
})
