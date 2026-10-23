import { act } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { MainPanel } from './MainPanel'
import { FolderTree } from '../sidebar/FolderTree'
import { useAppStore } from '../../store/useAppStore'
import { fallbackProviderForId } from '../../utils/providerMetadata'
import type { Session } from '../../types/session'
import { chatGridLeaves, defaultChatGridLayout, readChatGridLayout, setChatInLeaf, splitChatLeaf, writeChatGridLayout } from '../../utils/chatGridStorage'
import { clearSubagentView, openSubagentView, rememberSubagentParent, subagentRouteFor } from '../../utils/subagentNavigation'
import { sanitizeCppChat } from '../../store/cpp/sanitizers'
import { sessionsEquivalent, sessionFromCppChat } from '../../store/cpp/reconcile'

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
const parent: Session = { id: 'navigation-root', name: 'Main task', providerId: 'codex-cli', viewMode: 'chat', folderId: null, createdAt: new Date('2026-10-16T10:00:00Z'), updatedAt: new Date('2026-10-16T10:00:00Z') }
const other: Session = { ...parent, id: 'navigation-other', name: 'Other pane' }
const run = { runId: 'run-selected', transcriptChatId: 'hidden-transcript', agentId: 'review-agent', taskTitle: 'Review worker', status: 'completed', canCancel: false }
let host: HTMLDivElement, root: Root, snapshot: ReturnType<typeof useAppStore.getState>, previousCef: Window['cefQuery']
let requests: Array<{ action: string; payload: Record<string, unknown> }>
function list(runs = [run]) { return { chatId: parent.id, controlScopeId: 'root-scope', runs, offset: 0, nextOffset: null, totalCount: runs.length } }
function click(selector: string) { const button = host.querySelector<HTMLElement>(selector); expect(button, selector).not.toBeNull(); button!.click() }
// Sidebar rows mount their managed reader only when near the viewport; tests reveal them explicitly.
const watchers = new Set<() => void>()
// Children are collapsed by default: opening each parent mounts its reader, which then observes the viewport.
async function reveal() {
  const closed = () => host.querySelectorAll<HTMLButtonElement>('button.uam-subagent-toggle[aria-expanded="false"]:not(:disabled)')
  for (let depth = 0; depth < 4 && closed().length; ++depth) await act(async () => closed().forEach(button => button.click()))
  await act(async () => [...watchers].forEach(fire => fire()))
}
function back() { [...host.querySelectorAll<HTMLButtonElement>('button')].find(button => button.textContent === 'Back to parent')!.click() }
function orphanReturn(pane: Element) {
  const header = pane.querySelector('nav[aria-label="Subagent lineage"]')!
  expect(header).not.toBeNull()
  const controls = [...header.querySelectorAll<HTMLButtonElement>('button')]
  const back = controls.find(button => button.textContent === 'Back to chats')
  expect(back, 'The actual orphan route must expose its existing view-exit callback as Back to chats').toBeDefined()
  expect(back!.disabled).toBe(false)
  expect(controls.some(button => button.textContent === 'Back to parent')).toBe(false)
  expect(header.querySelector('[aria-label="Close subagent view"], svg.lucide-x')).toBeNull()
  expect(pane.textContent).toContain('Parent unavailable')
  return back!
}
function input(text: string) {
  const field = host.querySelector<HTMLTextAreaElement>('[data-testid="chat-pane-navigation-root"] textarea')!
  Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')!.set!.call(field, text)
  field.dispatchEvent(new Event('input', { bubbles: true }))
}
beforeEach(() => {
  snapshot = useAppStore.getState(); previousCef = window.cefQuery; requests = []
  const values = new Map<string, string>()
  vi.stubGlobal('localStorage', { getItem: (key: string) => values.get(key) ?? null, setItem: (key: string, value: string) => values.set(key, value), removeItem: (key: string) => values.delete(key) })
  vi.stubGlobal('ResizeObserver', class { observe() {} unobserve() {} disconnect() {} })
  watchers.clear()
  vi.stubGlobal('IntersectionObserver', class { private fire = () => this.callback([{ isIntersecting: true }]); constructor(private callback: (entries: unknown[]) => void) {} observe() { watchers.add(this.fire) } disconnect() { watchers.delete(this.fire) } })
  window.cefQuery = ({ request, onSuccess }) => {
    const query = JSON.parse(request); requests.push(query)
    onSuccess(JSON.stringify(query.action === 'listManagedSubagents' ? list() : query.action === 'getManagedAgentTranscript'
      ? { runId: run.runId, title: 'Review worker', status: 'completed', messages: [{ role: 'assistant', content: '**Child alone**' }] }
      : { ok: true, data: {} }))
  }
  for (const leaf of chatGridLeaves(readChatGridLayout().root)) clearSubagentView(leaf.id)
  writeChatGridLayout(setChatInLeaf(defaultChatGridLayout, parent.id, defaultChatGridLayout.activeLeafId))
  useAppStore.setState({ sessions: [{ ...parent, managedSubagentCount: 1 }], folders: [], resourceCollections: [], activeSessionId: parent.id,
    lastAppliedStateRevision: 1, messages: { [parent.id]: [{ id: 'parent-message', sessionId: parent.id, role: 'assistant', content: 'Parent transcript', createdAt: new Date() }] },
    acpBindingBySessionId: {}, cliBindingBySessionId: {}, loadSessionMessages: vi.fn().mockResolvedValue(undefined), unloadSessionMessages: vi.fn(),
    getVcsCommitStatus: vi.fn().mockResolvedValue(null), discoverProviderModels: vi.fn().mockResolvedValue(undefined) })
  host = document.createElement('div'); document.body.append(host); root = createRoot(host)
})
afterEach(() => {
  act(() => root.unmount()); host.remove()
  for (const leaf of chatGridLeaves(readChatGridLayout().root)) clearSubagentView(leaf.id)
  window.cefQuery = previousCef; useAppStore.setState(snapshot, true); vi.unstubAllGlobals(); vi.restoreAllMocks()
})

describe('David subagent catalogue and pane navigation', () => {
  it('lists children as one-line robot rows behind one closed-by-default parent toggle and reads a ledger only once opened and near the viewport', async () => {
    {
      const child = { ...parent, id: 'passive-native', name: 'Native worker', importedReadOnly: true, parentChatId: parent.id }
      rememberSubagentParent(parent, child.id)
      useAppStore.setState({ sessions: [{ ...parent, managedSubagentCount: 1 }, child], acpBindingBySessionId: { [child.id]: { processing: true } as never } })
      await act(async () => root.render(<FolderTree searchQuery="" />))
      expect(requests.filter(query => query.action === 'listManagedSubagents')).toHaveLength(0)
      expect(host.querySelector('#active-chat-list [data-session-id="passive-native"]')).toBeNull()
      const toggles = host.querySelectorAll<HTMLButtonElement>('button.uam-subagent-toggle')
      expect(toggles).toHaveLength(1)
      expect(toggles[0].getAttribute('aria-expanded')).toBe('false')
      expect(host.querySelector('[data-session-id="passive-native"]')).toBeNull()
      expect(host.querySelector('[data-managed-run]')).toBeNull()
      await reveal()
      expect(toggles[0].getAttribute('aria-expanded')).toBe('true')
      const nativeRow = host.querySelector('#all-chat-list [data-session-id="passive-native"]')!
      expect(nativeRow).not.toBeNull()
      expect(nativeRow.querySelector('[aria-label="Subagent"]')).not.toBeNull()
      expect(nativeRow.querySelector('[aria-label^="Provider:"]')).toBeNull()
      const group = host.querySelector('[role="group"][aria-label="Subagents of Main task"]')!
      const managed = group.querySelector('[aria-label="Open managed conversation for Review worker"]')!
      expect(managed).not.toBeNull()
      expect(managed.parentElement!.querySelector('svg.lucide-bot')).not.toBeNull()
      expect(group.textContent).not.toMatch(/refresh|Delegated UAM runs|Execution host/i)
      expect(host.querySelectorAll('[data-session-id="passive-native"]')).toHaveLength(1)
      expect(requests.filter(query => query.action === 'listManagedSubagents')).toHaveLength(1)
      await act(async () => root.render(<div />))
      await act(async () => root.render(<FolderTree searchQuery="Native worker" />))
      expect(host.querySelector('#all-chat-list [data-session-id="navigation-root"]')).not.toBeNull()
      expect(host.querySelectorAll('[data-session-id="passive-native"]')).toHaveLength(1)
      expect(host.querySelector('#active-chat-list [data-session-id="passive-native"]')).toBeNull()
      // A search match shows the native child through its parent without a ledger read or a second choice.
      const search = host.querySelector<HTMLButtonElement>('button.uam-subagent-toggle')!
      expect(search.getAttribute('aria-expanded')).toBe('true')
      expect(search.disabled).toBe(true)
      await act(async () => [...watchers].forEach(fire => fire()))
      expect(host.querySelector('[data-managed-run]')).toBeNull()
      expect(requests.filter(query => query.action === 'listManagedSubagents')).toHaveLength(1)
    }
  })

  it('shows only the chosen managed child with fixed top Back, preserves draft/scroll and returns in the parent pane', async () => {
    let layout = splitChatLeaf(readChatGridLayout(), readChatGridLayout().activeLeafId, 'horizontal')
    const leaves = chatGridLeaves(layout.root)
    layout = setChatInLeaf(layout, other.id, leaves[1].id)
    writeChatGridLayout({ ...layout, activeLeafId: leaves[0].id })
    useAppStore.setState(state => ({ sessions: [...state.sessions, other], messages: { ...state.messages, [other.id]: [] } }))
    await act(async () => root.render(<><FolderTree searchQuery="" /><MainPanel /></>))
    act(() => input('Unsent parent draft'))
    const region = host.querySelector<HTMLElement>('[data-testid="chat-pane-navigation-root"] .uam-chat-transcript')!
    Object.defineProperties(region, { scrollHeight: { configurable: true, value: 2000 }, clientHeight: { configurable: true, value: 300 } })
    region.scrollTop = 240
    act(() => region.dispatchEvent(new Event('scroll', { bubbles: true })))
    await reveal()
    await act(async () => click('[role="group"][aria-label="Subagents of Main task"] [data-managed-run="run-selected"]'))
    const pane = host.querySelector<HTMLElement>('[data-testid="subagent-pane-navigation-root"]')!
    expect(pane.dataset.pane).toBe('1')
    expect(pane.textContent).toContain('Child alone')
    expect(pane.textContent).not.toContain('Parent transcript')
    expect(pane.querySelector('textarea,form,input')).toBeNull()
    expect(host.querySelector('[role="group"][aria-label="Subagents of Main task"] [data-managed-run="run-selected"]')?.getAttribute('aria-current')).toBe('page')
    expect(host.querySelector('[data-testid="chat-pane-navigation-root"]')).toBeNull()
    expect(host.querySelector('[data-testid="chat-pane-navigation-other"]')).not.toBeNull()
    const nav = pane.querySelector('nav')!
    expect(pane.querySelectorAll('nav[aria-label="Subagent lineage"]')).toHaveLength(1)
    expect(pane.textContent!.startsWith(nav.textContent!)).toBe(true)
    expect(nav.closest('.uam-subagent-transcript')).toBeNull()
    expect([...pane.querySelectorAll('nav button')].map(button => button.getAttribute('aria-label') || button.textContent)).toEqual(['Back to parent', 'Refresh conversation'])
    expect(useAppStore.getState().activeSessionId).toBe(parent.id)
    expect(requests.filter(query => query.action === 'selectSession').some(query => query.payload.chatId === 'hidden-transcript')).toBe(false)
    await act(async () => back())
    const returned = host.querySelector<HTMLElement>('[data-testid="chat-pane-navigation-root"]')!
    expect(returned.dataset.pane).toBe('1')
    expect(returned.querySelector<HTMLTextAreaElement>('textarea')!.value).toBe('Unsent parent draft')
    expect(returned.querySelector<HTMLElement>('.uam-chat-transcript')!.scrollTop).toBe(240)
    expect(document.activeElement).toBe(returned.querySelector('textarea'))
    expect(chatGridLeaves(readChatGridLayout().root).map(leaf => leaf.sessionId)).toEqual([parent.id, other.id])
    expect(readChatGridLayout().activeLeafId).toBe(leaves[0].id)
  })

  it('returns through either All Chats or Active parent rows even when the selected parent ID is unchanged', async () => {
    useAppStore.setState(state => ({ sessions: state.sessions.map(session => ({ ...session, updatedAt: new Date(), interactionAt: new Date() })) }))
    await act(async () => root.render(<><FolderTree searchQuery="" /><MainPanel /></>))
    for (const section of ['#all-chat-list', '#active-chat-list']) {
      await act(async () => openSubagentView(useAppStore.getState().sessions[0], { kind: 'managed', runId: run.runId, transcriptChatId: run.transcriptChatId }))
      expect(host.querySelector('[data-testid="subagent-pane-navigation-root"]')).not.toBeNull()
      await act(async () => click(`${section} [data-session-id="navigation-root"]`))
      expect(host.querySelector('[data-testid="subagent-pane-navigation-root"]')).toBeNull()
      expect(host.querySelector('[data-testid="chat-pane-navigation-root"]')).not.toBeNull()
    }
  })

  it('discovers an accepted new child while the expanded parent is idle without scanning token replacements', async () => {
    let listed = false
    window.cefQuery = ({ request, onSuccess }) => { const query = JSON.parse(request); requests.push(query); onSuccess(JSON.stringify(list(listed ? [run] : []))) }
    await act(async () => root.render(<FolderTree searchQuery="" />))
    await reveal()
    expect(host.querySelector('[data-managed-run]')).toBeNull()
    expect(host.querySelector('.uam-managed-panel--catalogue li')).toBeNull()
    expect(host.textContent).not.toContain('No managed subagents')
    listed = true
    await act(async () => useAppStore.setState(state => ({ sessions: state.sessions.map(session => ({ ...session, managedSubagentRevision: 1, managedSubagentCount: 1 })) })))
    expect(host.querySelector('[data-managed-run="run-selected"]')).not.toBeNull()
    const reads = requests.length
    await act(async () => useAppStore.setState(state => ({ messages: { ...state.messages, [parent.id]: state.messages[parent.id].map(message => ({ ...message, content: message.content + ' token' })) } })))
    expect(requests).toHaveLength(reads)
  })

  it('shows validated passive native history in the same child-only pane with an explicit missing-transcript state and no execution path', async () => {
    const child = { ...parent, id: 'native-route', name: 'Passive child', importedReadOnly: true }
    rememberSubagentParent(parent, child.id)
    useAppStore.setState(state => ({ sessions: [...state.sessions, child] }))
    const send = vi.fn(); const resume = vi.fn(); const stop = vi.fn()
    useAppStore.setState({ sendAcpPrompt: send, startAcpSession: resume, stopAcpSession: stop })
    await act(async () => root.render(<MainPanel />))
    await act(async () => useAppStore.getState().setActiveSession(child.id))
    const pane = host.querySelector('[data-testid="subagent-pane-navigation-root"]')!
    expect(pane.textContent).toContain('provider has not supplied a readable transcript')
    expect(pane.querySelector('textarea,input,form')).toBeNull()
    expect(useAppStore.getState().activeSessionId).toBe(parent.id)
    await act(async () => useAppStore.setState(state => ({ messages: { ...state.messages, [child.id]: [{ id: 'native-message', sessionId: child.id, role: 'assistant', content: 'Available child history', createdAt: new Date() }] } })))
    expect(pane.textContent).toContain('Available child history')
    expect(pane.textContent).not.toContain('Parent transcript')
    act(() => { window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', metaKey: true, bubbles: true })); window.dispatchEvent(new CustomEvent('uam-dictation', { detail: { text: 'do not send', final: true } })) })
    expect(send).not.toHaveBeenCalled(); expect(resume).not.toHaveBeenCalled(); expect(stop).not.toHaveBeenCalled()
    expect(requests.some(query => query.action === 'selectSession' && query.payload.chatId === child.id)).toBe(false)
    await act(async () => back())
    expect(host.querySelector('[data-testid="chat-pane-navigation-root"]')).not.toBeNull()
  })

  it('opens a saved turn group through the same full-pane managed route, with no native preview or inline child composer', async () => {
    useAppStore.setState(state => ({ messages: { ...state.messages, [parent.id]: [{ ...state.messages[parent.id][0],
      toolCalls: [{ id: 'managed-result', title: 'uam_agent_result', status: 'completed', kind: 'sub-agent', content: '', isSubAgent: true, subAgentId: run.runId, subAgentTitle: 'Review worker' }] }] } }))
    await act(async () => root.render(<MainPanel />))
    await act(async () => { const details = host.querySelector<HTMLDetailsElement>('details.uam-subagent-group')!; expect(details).not.toBeNull(); details.open = true; details.dispatchEvent(new Event('toggle', { bubbles: true })) })
    await act(async () => click('[data-subagent-row="managed:run-selected"]'))
    const pane = host.querySelector('[data-testid="subagent-pane-navigation-root"]')!
    expect(pane.textContent).toContain('Child alone')
    expect(pane.querySelector('form,textarea')).toBeNull()
    expect(host.querySelector('[data-testid="chat-pane-navigation-root"]')).toBeNull()
    expect(requests.some(query => query.action === 'openNativeSessionChat')).toBe(false)
    expect(requests.filter(query => query.action === 'getManagedAgentTranscript').at(-1)?.payload).toEqual({ chatId: parent.id, transcriptChatId: run.transcriptChatId })
    await act(async () => back())
    expect(host.querySelector('[data-testid="chat-pane-navigation-root"]')).not.toBeNull()
    expect(host.querySelector<HTMLDetailsElement>('details.uam-subagent-group')!.open).toBe(true)
  })

  it('normalizes a restored passive child selection into the existing parent pane and preserves another pane', async () => {
    const child = { ...parent, id: 'restored-native', name: 'Restored native child', importedReadOnly: true, subagentParentChatId: parent.id, subagentParentCreatedAt: parent.createdAt.toISOString() }
    useAppStore.setState(state => ({ sessions: [...state.sessions, other, child], activeSessionId: child.id, messages: { ...state.messages, [other.id]: [] } }))
    let layout = splitChatLeaf(readChatGridLayout(), readChatGridLayout().activeLeafId, 'horizontal')
    const leaves = chatGridLeaves(layout.root)
    layout = setChatInLeaf(layout, other.id, leaves[1].id)
    writeChatGridLayout(layout)
    await act(async () => root.render(<MainPanel />))
    expect(host.querySelector('[data-testid="subagent-pane-navigation-root"]')).not.toBeNull()
    expect(host.querySelector('[data-testid="chat-pane-restored-native"]')).toBeNull()
    expect(host.querySelector('[data-testid="chat-pane-navigation-other"]')).not.toBeNull()
    expect(useAppStore.getState().activeSessionId).toBe(parent.id)
    expect(readChatGridLayout().activeLeafId).toBe(leaves[0].id)
    await act(async () => back())
    expect(host.querySelector('[data-testid="chat-pane-navigation-root"]')?.getAttribute('data-pane')).toBe('1')
  })

  it('keeps orphan lineage out of Active and ordinary catalogue roots, and opens only passive history with parent return unavailable', async () => {
    const child = { ...parent, id: 'orphan-native', name: 'Orphan worker', importedReadOnly: true, updatedAt: new Date(),
      subagentParentChatId: 'missing-parent', subagentParentCreatedAt: parent.createdAt.toISOString() }
    useAppStore.setState({ sessions: [child, other], activeSessionId: other.id, messages: { [other.id]: [], [child.id]: [{ id: 'orphan-message', sessionId: child.id, role: 'assistant', content: 'Orphan stored history', createdAt: new Date() }] },
      acpBindingBySessionId: { [child.id]: { processing: true } as never } })
    writeChatGridLayout(setChatInLeaf(defaultChatGridLayout, other.id, defaultChatGridLayout.activeLeafId))
    await act(async () => root.render(<><FolderTree searchQuery="" /><MainPanel /></>))
    expect(host.querySelector('#active-chat-list [data-session-id="orphan-native"]')).toBeNull()
    expect(host.querySelector('#all-chat-list [data-session-id="orphan-native"]')).toBeNull()
    const group = host.querySelector('[aria-label="Subagents with unavailable parents"]')!
    expect(group.textContent).toContain('Parent unavailable')
    await act(async () => click('[aria-label="Open read-only subagent Orphan worker"]'))
    const pane = host.querySelector('[data-testid="orphan-subagent-pane-orphan-native"]')!
    expect(pane.textContent).toContain('Orphan stored history')
    expect(pane.textContent).toContain('Parent unavailable')
    orphanReturn(pane)
    expect(pane.querySelector('textarea,input,form')).toBeNull()
    expect(host.querySelector('[aria-label="Open read-only subagent Orphan worker"]')?.getAttribute('aria-current')).toBe('page')
    expect(requests.some(query => query.action === 'selectSession' && query.payload.chatId === child.id)).toBe(false)
    await act(async () => orphanReturn(pane).click())
    expect(host.querySelector('[data-testid="orphan-subagent-pane-orphan-native"]')).toBeNull()
    await act(async () => root.render(<FolderTree searchQuery="Orphan worker" />))
    expect(host.querySelector('[aria-label="Subagents with unavailable parents"]')).not.toBeNull()
    expect(host.textContent).not.toContain('No chats match this search')
  })

  it.each(['creation', 'provider', 'host', 'workspace'])('never nests a persisted native child under a foreign parent: %s', async mismatch => {
    const original = { ...parent, workspaceDirectory: '/original-workspace' }
    const child = { ...original, id: 'foreign-scope-child', name: 'Foreign scope worker', importedReadOnly: true,
      subagentParentChatId: parent.id, subagentParentCreatedAt: original.createdAt.toISOString() }
    const changed = { ...original,
      ...(mismatch === 'creation' ? { createdAt: new Date(original.createdAt.getTime() + 1) } : {}),
      ...(mismatch === 'provider' ? { providerId: 'claude-cli' } : {}),
      ...(mismatch === 'host' ? { executionHostId: 'foreign-host' } : {}),
      ...(mismatch === 'workspace' ? { workspaceDirectory: '/foreign-workspace' } : {}),
    }
    useAppStore.setState({ sessions: [changed, child], acpBindingBySessionId: { [child.id]: { processing: true } as never } })
    await act(async () => root.render(<FolderTree searchQuery="" />))
    expect(host.querySelector('#active-chat-list [data-session-id="foreign-scope-child"]')).toBeNull()
    expect(host.querySelector('[data-subagent-parent="navigation-root"] [data-session-id="foreign-scope-child"]')).toBeNull()
    expect(host.querySelector('[aria-label="Subagents with unavailable parents"] [aria-label="Open read-only subagent Foreign scope worker"]')).not.toBeNull()
  })

  it.each(['codex-cli', 'opencode-cli', 'antigravity-cli'])('preserves native child identity and Back without standing composer submission: %s', async providerId => {
    const source = { ...parent, providerId }
    const child = { ...source, id: `executable-${providerId}`, name: 'Executable child', importedReadOnly: false,
      subagentParentChatId: source.id, subagentParentCreatedAt: source.createdAt.toISOString() }
    const send = vi.fn().mockResolvedValue(true)
    useAppStore.setState(state => ({
      providers: state.providers.some(provider => provider.id === providerId) ? state.providers : [...state.providers, fallbackProviderForId(providerId)],
      sessions: [source, child], activeSessionId: source.id, sendAcpPrompt: send, messages: { ...state.messages, [child.id]: [] },
    }))
    expect(useAppStore.getState().providers.find(provider => provider.id === providerId)).toMatchObject({ id: providerId, supportsStructured: true })
    await act(async () => root.render(<MainPanel />))
    act(() => input('Independent parent draft'))
    const parentScroll = host.querySelector<HTMLElement>('[data-testid="chat-pane-navigation-root"] .uam-chat-transcript')!
    Object.defineProperties(parentScroll, { scrollHeight: { value: 2000, configurable: true }, clientHeight: { value: 300, configurable: true } })
    parentScroll.scrollTop = 220
    act(() => parentScroll.dispatchEvent(new Event('scroll', { bubbles: true })))
    await act(async () => useAppStore.getState().setActiveSession(child.id))
    const pane = host.querySelector(`[data-testid="chat-pane-${child.id}"]`)!
    expect(pane).not.toBeNull()
    expect(host.querySelector('[data-testid="chat-pane-navigation-root"]')).toBeNull()
    expect(host.querySelectorAll('nav[aria-label="Subagent lineage"]')).toHaveLength(1)
    expect(pane.querySelector('textarea.uam-composer-textarea')).toBeNull()
    const followup = [...pane.querySelectorAll<HTMLButtonElement>('nav button')].find(button => button.getAttribute('aria-label') === 'Follow up')
    expect(followup).toBeDefined()
    expect(followup!.getAttribute('aria-expanded')).toBe('false')
    expect(pane.querySelector('[aria-label="Send prompt"], [aria-label="Queue prompt"]')).toBeNull()
    expect(pane.querySelector('[aria-label="Terminal fallback"], [aria-label="Chat view"]')).toBeNull()
    expect(useAppStore.getState().activeSessionId).toBe(child.id)
    await act(async () => {
      pane.querySelector('form')!.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true }))
      window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', ctrlKey: true, bubbles: true }))
      document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', metaKey: true, bubbles: true }))
    })
    expect(send).not.toHaveBeenCalled()
    expect(requests.some(query => query.action === 'selectSession' && query.payload.chatId === child.id)).toBe(true)
    await act(async () => back())
    const returned = host.querySelector('[data-testid="chat-pane-navigation-root"]')!
    expect(returned.querySelector<HTMLTextAreaElement>('textarea')!.value).toBe('Independent parent draft')
    expect(returned.querySelector<HTMLElement>('.uam-chat-transcript')!.scrollTop).toBe(220)
    expect(document.activeElement).toBe(returned.querySelector('textarea'))
    expect(useAppStore.getState().activeSessionId).toBe(source.id)
  })

  it.each([{ providerId: 'codex-cli', importedReadOnly: true }, { providerId: 'gemini-cli', importedReadOnly: true }, { providerId: 'claude-cli', importedReadOnly: false }, { providerId: 'antigravity-cli', importedReadOnly: true }])('never gives a passive/reference child native execution controls: %j', async capability => {
    const source = { ...parent, providerId: capability.providerId }
    const child = { ...source, ...capability, id: `passive-${capability.providerId}`, name: 'Passive reference', subagentParentChatId: source.id, subagentParentCreatedAt: source.createdAt.toISOString() }
    const send = vi.fn(); const start = vi.fn()
    useAppStore.setState(state => ({ sessions: [source, child], activeSessionId: source.id, sendAcpPrompt: send, startAcpSession: start, messages: { ...state.messages, [child.id]: [] } }))
    await act(async () => root.render(<MainPanel />))
    await act(async () => useAppStore.getState().setActiveSession(child.id))
    const pane = host.querySelector('[data-testid="subagent-pane-navigation-root"]')!
    expect(pane.querySelector('textarea,form,input')).toBeNull()
    expect(pane.querySelector('[aria-label="Terminal fallback"]')).toBeNull()
    expect(pane.textContent).toContain('Read-only native conversation')
    expect(useAppStore.getState().activeSessionId).toBe(source.id)
    expect(requests.some(query => query.action === 'selectSession' && query.payload.chatId === child.id)).toBe(false)
    act(() => window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', ctrlKey: true, bubbles: true })))
    expect(send).not.toHaveBeenCalled(); expect(start).not.toHaveBeenCalled()
  })

  it('activates and returns the exact leaf when two panes show children of the same parent', async () => {
    const layout = { version: 2 as const, activeLeafId: 'same-parent-right', root: { type: 'split' as const, id: 'same-parent-split', direction: 'horizontal' as const, sizes: [50, 50] as [number, number], children: [
      { type: 'leaf' as const, id: 'same-parent-left', sessionId: parent.id }, { type: 'leaf' as const, id: 'same-parent-right', sessionId: parent.id },
    ] as [import('../../utils/chatGridStorage').ChatPaneLeaf, import('../../utils/chatGridStorage').ChatPaneLeaf] } }
    writeChatGridLayout(layout)
    openSubagentView(parent, { kind: 'managed', runId: 'left-run', transcriptChatId: 'left-transcript' }, 'same-parent-left')
    openSubagentView(parent, { kind: 'managed', runId: 'right-run', transcriptChatId: 'right-transcript' }, 'same-parent-right')
    writeChatGridLayout({ ...readChatGridLayout(), activeLeafId: 'same-parent-left' })
    window.cefQuery = ({ request, onSuccess }) => { const query = JSON.parse(request); requests.push(query); onSuccess(JSON.stringify(query.action === 'getManagedAgentTranscript'
      ? { runId: query.payload.transcriptChatId === 'left-transcript' ? 'left-run' : 'right-run', status: 'completed', messages: [{ role: 'assistant', content: query.payload.transcriptChatId }] } : list())) }
    await act(async () => root.render(<MainPanel />))
    const panes = host.querySelectorAll<HTMLElement>('[data-testid="subagent-pane-navigation-root"]')
    expect(panes).toHaveLength(2)
    expect(readChatGridLayout().activeLeafId).toBe('same-parent-left')
    expect(panes[0].textContent).toContain('left-transcript')
    expect(panes[1].textContent).toContain('right-transcript')
    await act(async () => panes[1].dispatchEvent(new MouseEvent('mousedown', { bubbles: true })))
    expect(readChatGridLayout().activeLeafId).toBe('same-parent-right')
    expect(subagentRouteFor('same-parent-left', parent)).toMatchObject({ kind: 'managed', runId: 'left-run' })
    await act(async () => panes[1].querySelector<HTMLButtonElement>('nav button')!.click())
    expect(readChatGridLayout().activeLeafId).toBe('same-parent-right')
    expect(host.querySelector('[data-testid="chat-pane-navigation-root"]')?.getAttribute('data-pane')).toBe('2')
    expect(host.querySelectorAll('[data-testid="subagent-pane-navigation-root"]')).toHaveLength(1)
    expect(host.querySelector('[data-testid="subagent-pane-navigation-root"]')?.getAttribute('data-pane')).toBe('1')
    expect(subagentRouteFor('same-parent-left', parent)).toMatchObject({ kind: 'managed', runId: 'left-run' })
    expect(chatGridLeaves(readChatGridLayout().root).map(leaf => leaf.sessionId)).toEqual([parent.id, parent.id])
  })

  it('projects display-only native lineage from a backend summary and invalidates a lineage-only change', () => {
    const raw = sanitizeCppChat({ id: 'projected-native', providerId: parent.providerId, createdAt: parent.createdAt.toISOString(), updatedAt: parent.updatedAt.toISOString(),
      subagentParentChatId: parent.id, subagentParentCreatedAt: parent.createdAt.toISOString() })!
    const child = sessionFromCppChat(raw, undefined, [{ id: parent.providerId! }])
    expect(child.subagentParentChatId).toBe(parent.id)
    expect(child.subagentParentCreatedAt).toBe(parent.createdAt.toISOString())
    expect(sessionFromCppChat(raw, child, [{ id: parent.providerId! }])).toBe(child)
    const changed = sessionFromCppChat({ ...raw, subagentParentCreatedAt: 'invalid' }, child, [{ id: parent.providerId! }])
    expect(changed).not.toBe(child)
    expect(changed.subagentParentCreatedAt).toBe('invalid')
    expect(sanitizeCppChat({ id: 'old', subagentParentChatId: 12, subagentParentCreatedAt: {} })!.subagentParentChatId).toBeUndefined()
  })

  it('separates retained desktop and companion windows for the same actual pane and chat', async () => {
    const previousPath = window.location.pathname
    const source = { ...parent, id: 'viewport-client-isolation', name: 'Viewport isolation', importedReadOnly: true }
    const messages = Array.from({ length: 60 }, (_, index) => ({ id: `viewport-message-${index}`, sessionId: source.id,
      role: index % 2 === 0 ? 'user' as const : 'assistant' as const, content: `viewport authored message ${index}`, createdAt: new Date(index) }))
    useAppStore.setState({ sessions: [source], activeSessionId: source.id, messages: { [source.id]: messages } })
    writeChatGridLayout(setChatInLeaf(defaultChatGridLayout, source.id, defaultChatGridLayout.activeLeafId))
    try {
      window.history.pushState({}, '', '/')
      await act(async () => root.render(<MainPanel />))
      expect(host.textContent).toContain('viewport authored message 0')
      await act(async () => root.render(<div />))
      window.history.pushState({}, '', '/companion')
      await act(async () => root.render(<MainPanel />))
      expect(host.textContent).not.toContain('viewport authored message 0')
      expect(host.textContent).toContain('viewport authored message 59')
      expect(host.textContent).toContain('Show earlier messages')
    } finally {
      await act(async () => root.render(<div />))
      window.history.pushState({}, '', previousPath)
    }
  })

  it('accepts only safe nonnegative count metadata and reconciles a count-only update', () => {
    for (const count of [undefined, -1, 0.5, '2', Number.MAX_SAFE_INTEGER + 1]) {
      const raw = sanitizeCppChat({ id: parent.id, managedSubagentCount: count })!
      expect(raw.managedSubagentCount).toBe(0)
      expect(sessionFromCppChat(raw, undefined, []).managedSubagentCount).toBe(0)
    }
    const raw = sanitizeCppChat({ id: parent.id, createdAt: parent.createdAt.toISOString(), updatedAt: parent.updatedAt.toISOString(), managedSubagentCount: 1 })!
    const first = sessionFromCppChat(raw, undefined, [])
    expect(sessionFromCppChat(raw, first, [])).toBe(first)
    const next = sessionFromCppChat({ ...raw, managedSubagentCount: 2 }, first, [])
    expect(next).not.toBe(first)
    expect(next.managedSubagentCount).toBe(2)
    expect(sessionsEquivalent(first, next)).toBe(false)
  })
  it.each(['#all-chat-list', '#active-chat-list'])('NAV01: a same-ID catalogue click cancels pending native open intent through %s', async section => {
    const source = { ...parent, managedSubagentCount: 0, interactionAt: new Date(), updatedAt: new Date() }
    const child = { ...source, id: 'intent-passive-child', name: 'Pending worker', importedReadOnly: true }
    useAppStore.setState(state => ({ sessions: [source, child], messages: { ...state.messages, [child.id]: [], [source.id]: [{ ...state.messages[source.id][0],
      toolCalls: [{ id: 'intent-tool', title: 'Delegate', kind: 'sub-agent', status: 'completed', content: '', isSubAgent: true, subAgentId: 'intent-native-reference', subAgentTitle: 'Pending worker' }] }] } }))
    let release: ((response: string) => void) | undefined
    window.cefQuery = ({ request, onSuccess }) => { const query = JSON.parse(request); requests.push(query); if (query.action === 'openNativeSessionChat') release = onSuccess; else onSuccess(JSON.stringify(query.action === 'listManagedSubagents' ? list([]) : { ok: true, data: {} })) }
    await act(async () => root.render(<><FolderTree searchQuery="" /><MainPanel /></>))
    await act(async () => { const group = host.querySelector<HTMLDetailsElement>('details.uam-subagent-group')!; group.open = true; group.dispatchEvent(new Event('toggle', { bubbles: true })) })
    await act(async () => click('[data-subagent-row="intent-native-reference"]'))
    expect(release).toBeTypeOf('function')
    await act(async () => click(`${section} [data-session-id="navigation-root"]`))
    await act(async () => release!(JSON.stringify({ chatId: child.id })))
    expect(host.querySelector('[data-testid="chat-pane-navigation-root"]')).not.toBeNull()
    expect(host.querySelector('[data-testid="subagent-pane-navigation-root"]')).toBeNull()
    expect(subagentRouteFor(readChatGridLayout().activeLeafId, source)).toBeUndefined()
  })

  it.each(['#all-chat-list', '#active-chat-list'])('NAV02: catalogue return retains the active right same-parent leaf through %s', async section => {
    useAppStore.setState(state => ({ sessions: state.sessions.map(session => ({ ...session, interactionAt: new Date(), updatedAt: new Date() })) }))
    const source = useAppStore.getState().sessions[0]
    writeChatGridLayout({ version: 2, root: { type: 'leaf', id: 'catalogue-right', sessionId: source.id }, activeLeafId: 'catalogue-right' })
    await act(async () => root.render(<><FolderTree searchQuery="" /><MainPanel /></>))
    act(() => input('Right parent draft'))
    const scroll = host.querySelector<HTMLElement>('.uam-chat-transcript')!
    Object.defineProperties(scroll, { scrollHeight: { value: 2000, configurable: true }, clientHeight: { value: 300, configurable: true } })
    scroll.scrollTop = 280
    act(() => scroll.dispatchEvent(new Event('scroll', { bubbles: true })))
    await act(async () => openSubagentView(source, { kind: 'managed', runId: run.runId, transcriptChatId: run.transcriptChatId }, 'catalogue-right'))
    await act(async () => {
      writeChatGridLayout({ version: 2, activeLeafId: 'catalogue-right', root: { type: 'split', id: 'catalogue-split', direction: 'horizontal', sizes: [50, 50], children: [
        { type: 'leaf', id: 'catalogue-left', sessionId: source.id }, { type: 'leaf', id: 'catalogue-right', sessionId: source.id },
      ] } })
      openSubagentView(source, { kind: 'managed', runId: run.runId, transcriptChatId: run.transcriptChatId }, 'catalogue-left')
      writeChatGridLayout({ ...readChatGridLayout(), activeLeafId: 'catalogue-right' })
    })
    expect(host.querySelectorAll('[data-testid="subagent-pane-navigation-root"]')).toHaveLength(2)
    await act(async () => click(`${section} [data-session-id="navigation-root"]`))
    expect(readChatGridLayout().activeLeafId).toBe('catalogue-right')
    expect(subagentRouteFor('catalogue-left', source)?.kind).toBe('managed')
    expect(subagentRouteFor('catalogue-right', source)).toBeUndefined()
    const returned = host.querySelector('[data-leaf-id="catalogue-right"] [data-testid="chat-pane-navigation-root"]')!
    expect(returned.querySelector<HTMLTextAreaElement>('textarea')!.value).toBe('Right parent draft')
    expect(returned.querySelector<HTMLElement>('.uam-chat-transcript')!.scrollTop).toBe(280)
  })

  it('NAV03: normal family disclosure reaches branch-owned children with exact branch control and read authority', async () => {
    const branch = sessionFromCppChat(sanitizeCppChat({ id: 'branch-owner', title: 'Branch task', providerId: parent.providerId, parentChatId: parent.id, branchRootChatId: parent.id,
      createdAt: parent.createdAt.toISOString(), updatedAt: parent.updatedAt.toISOString(), managedSubagentCount: 1, managedSubagentRevision: 1 })!, undefined, [{ id: parent.providerId! }])
    const child = { ...branch, id: 'branch-native-child', name: 'Branch native worker', importedReadOnly: true, managedSubagentCount: 0, managedSubagentRevision: 0,
      subagentParentChatId: branch.id, subagentParentCreatedAt: branch.createdAt.toISOString() }
    useAppStore.setState({ sessions: [{ ...parent, managedSubagentCount: 0 }, branch, child], activeSessionId: branch.id, messages: { [parent.id]: [], [branch.id]: [], [child.id]: [] } })
    writeChatGridLayout(setChatInLeaf(defaultChatGridLayout, branch.id, defaultChatGridLayout.activeLeafId))
    let stopped = false
    window.cefQuery = ({ request, onSuccess }) => { const query = JSON.parse(request); requests.push(query)
      if (query.action === 'cancelManagedSubagent') { stopped = true; onSuccess(JSON.stringify({ ok: true, runId: run.runId })) }
      else if (query.action === 'listManagedSubagents') onSuccess(JSON.stringify({ ...list([{ ...run, status: stopped ? 'cancelled' : 'running', canCancel: !stopped }]), chatId: branch.id, controlScopeId: 'branch-scope' }))
      else if (query.action === 'getManagedAgentTranscript') onSuccess(JSON.stringify({ runId: run.runId, status: 'completed', title: 'Branch managed worker', messages: [{ role: 'assistant', content: 'Branch-only stored result' }] }))
      else onSuccess(JSON.stringify({ ok: true, data: {} }))
    }
    await act(async () => root.render(<><FolderTree searchQuery="" /><div /></>))
    expect(requests.filter(query => query.action === 'listManagedSubagents')).toHaveLength(0)
    expect(host.querySelector('#active-chat-list [data-session-id="branch-native-child"]')).toBeNull()
    await reveal()
    const ownerGroup = host.querySelector('[aria-label="Subagents of Branch task"]')!
    expect(ownerGroup).not.toBeNull()
    expect(ownerGroup.querySelector('[data-session-id="branch-native-child"]')).not.toBeNull()
    await act(async () => ownerGroup.querySelector<HTMLButtonElement>('[aria-label^="Stop managed subagent"]')!.click())
    expect(requests.find(query => query.action === 'cancelManagedSubagent')?.payload).toEqual({ chatId: branch.id, runId: run.runId, controlScopeId: 'branch-scope' })
    await act(async () => ownerGroup.querySelector<HTMLButtonElement>('[data-managed-run]')!.click())
    await act(async () => root.render(<><FolderTree searchQuery="" /><MainPanel /></>))
    const pane = host.querySelector('[data-testid="subagent-pane-branch-owner"]')!
    expect(pane.textContent).toContain('Subagent of Branch task')
    expect(pane.textContent).toContain('Branch-only stored result')
    expect(requests.filter(query => query.action === 'listManagedSubagents').every(query => query.payload.chatId === branch.id)).toBe(true)
    expect(requests.find(query => query.action === 'getManagedAgentTranscript')?.payload).toEqual({ chatId: branch.id, transcriptChatId: run.transcriptChatId })
    await act(async () => back())
    await act(async () => root.render(<><FolderTree searchQuery="Branch native worker" /><MainPanel /></>))
    expect(host.querySelector('[data-subagent-parent="branch-owner"]')).not.toBeNull()
    await act(async () => root.render(<><FolderTree searchQuery="" /><MainPanel /></>))
    expect(host.querySelector('[aria-label^="Expand subagents"]')).toBeNull()
    await reveal()
    await act(async () => click('[aria-label="Subagents of Branch task"] [data-session-id="branch-native-child"]'))
    expect(host.querySelector('[data-testid="subagent-pane-branch-owner"] [aria-label="Native subagent conversation"]')).not.toBeNull()
  })

  it('NAV04: passive Refresh re-imports the exact attested parent/reference before reloading the stored child', async () => {
    const source = { ...parent, providerId: 'gemini-cli' }
    const reference = 'gemini-agent-706172656e74-6167656e74-6c6f63616c'
    const child = { ...source, id: reference, name: 'Native journal', importedReadOnly: true, subagentParentChatId: source.id, subagentParentCreatedAt: source.createdAt.toISOString() }
    let imported = false
    const load = vi.fn(async (id: string) => { if (id === child.id) useAppStore.setState(state => ({ messages: { ...state.messages, [child.id]: [{ id: 'native-journal-message', sessionId: child.id, role: 'assistant', content: imported ? 'New native journal snapshot B' : 'Original native journal snapshot A', createdAt: new Date() }] } })) })
    useAppStore.setState({ sessions: [source, child], messages: { [source.id]: [], [child.id]: [] }, loadSessionMessages: load })
    window.cefQuery = ({ request, onSuccess }) => { const query = JSON.parse(request); requests.push(query)
      if (query.action === 'openNativeSessionChat') { imported = true; onSuccess(JSON.stringify({ chatId: child.id })) }
      else onSuccess(JSON.stringify(query.action === 'listManagedSubagents' ? list([]) : { ok: true, data: {} }))
    }
    await act(async () => root.render(<MainPanel />))
    await act(async () => useAppStore.getState().setActiveSession(child.id))
    expect(host.textContent).toContain('Original native journal snapshot A')
    await act(async () => click('[aria-label="Native subagent conversation"] [aria-label="Refresh conversation"]'))
    expect(requests.filter(query => query.action === 'openNativeSessionChat')).toHaveLength(1)
    expect(requests.find(query => query.action === 'openNativeSessionChat')?.payload).toEqual({ chatId: source.id, nativeSessionId: reference, title: child.name, selectChat: false, refreshHistory: true })
    expect(host.textContent).toContain('New native journal snapshot B')
    expect(host.querySelector('[aria-label="Native subagent conversation"] textarea')).toBeNull()
    expect(requests.some(query => query.action === 'selectSession' && query.payload.chatId === child.id)).toBe(false)
    expect(useAppStore.getState().activeSessionId).toBe(source.id)
  })

  it.each(['unmount', 'source change'])('NAV04: a late passive refresh cannot reload after %s', async disposal => {
    const source = { ...parent, providerId: 'gemini-cli' }
    const child = { ...source, id: 'gemini-agent-706172656e74-6167656e74-6c6f63616c', name: 'Delayed journal', importedReadOnly: true, subagentParentChatId: source.id, subagentParentCreatedAt: source.createdAt.toISOString() }
    const load = vi.fn().mockResolvedValue(undefined)
    useAppStore.setState({ sessions: [source, child], messages: { [source.id]: [], [child.id]: [] }, loadSessionMessages: load })
    let release: ((response: string) => void) | undefined
    window.cefQuery = ({ request, onSuccess }) => { const query = JSON.parse(request); requests.push(query)
      if (query.action === 'openNativeSessionChat') release = onSuccess
      else onSuccess(JSON.stringify({ ok: true, data: {} }))
    }
    await act(async () => root.render(<MainPanel />))
    await act(async () => useAppStore.getState().setActiveSession(child.id))
    await act(async () => click('[aria-label="Native subagent conversation"] [aria-label="Refresh conversation"]'))
    expect(release).toBeTypeOf('function')
    if (disposal === 'unmount') await act(async () => root.render(<div />))
    else await act(async () => useAppStore.setState({ sessions: [{ ...source, createdAt: new Date(source.createdAt.getTime() + 1000) }, child] }))
    const readsBeforeReceipt = load.mock.calls.length
    await act(async () => release!(JSON.stringify({ chatId: child.id })))
    expect(load).toHaveBeenCalledTimes(readsBeforeReceipt)
    expect(requests.some(query => query.action === 'selectSession' && query.payload.chatId === child.id)).toBe(false)
    expect(host.querySelector('[aria-label="Native subagent conversation"] textarea')).toBeNull()
  })

  it.each([false, true])('NAV-R3-01: cold native grandchildren remain nested/searchable and return through their immediate parent (passive=%s)', async passive => {
    const restore = (id: string, title: string, birth: string, parentSession?: Session): Session => sessionFromCppChat(sanitizeCppChat({
      id, title, providerId: 'codex-cli', viewMode: 'chat', createdAt: birth, updatedAt: new Date().toISOString(),
      subagentParentChatId: parentSession?.id, subagentParentCreatedAt: parentSession?.createdAt.toISOString(),
      importedReadOnly: passive && id === 'native-grandchild', managedSubagentCount: id === 'native-parent' ? 1 : 0,
    })!, undefined, [{ id: 'codex-cli' }])
    const source = restore(parent.id, 'Restored root', '2026-10-16T10:00:00Z')
    const child = restore('native-parent', 'Native parent', '2026-10-16T10:01:00Z', source)
    const grandchild = restore('native-grandchild', 'Distinct grandchild', '2026-10-16T10:02:00Z', child)
    const send = vi.fn().mockResolvedValue(true)
    useAppStore.setState({ sessions: [source, child, grandchild, other], activeSessionId: source.id, sendAcpPrompt: send, messages: Object.fromEntries([source, child, grandchild, other].map(session => [session.id, [{ id: `message-${session.id}`, sessionId: session.id, role: 'assistant', content: `Stored ${session.name} conversation`, createdAt: new Date() }]])) })
    let layout = splitChatLeaf(readChatGridLayout(), readChatGridLayout().activeLeafId, 'horizontal')
    const leaves = chatGridLeaves(layout.root)
    layout = setChatInLeaf(layout, other.id, leaves[1].id)
    writeChatGridLayout({ ...layout, activeLeafId: leaves[0].id })
    window.cefQuery = ({ request, onSuccess }) => { const query = JSON.parse(request); requests.push(query)
      onSuccess(JSON.stringify(query.action === 'listManagedSubagents' ? { ...list(), chatId: child.id, controlScopeId: 'native-parent-scope' } : { ok: true, data: {} }))
    }
    await act(async () => root.render(<><FolderTree searchQuery="" /><div /></>))
    expect(requests.filter(query => query.action === 'listManagedSubagents')).toHaveLength(0)
    expect(host.querySelector('#active-chat-list [data-session-id="native-parent"],#active-chat-list [data-session-id="native-grandchild"]')).toBeNull()
    expect(host.querySelector('[data-session-id="native-parent"]')).toBeNull()
    expect(host.querySelector('button.uam-subagent-toggle')!.getAttribute('aria-expanded')).toBe('false')
    await reveal()
    expect(host.querySelector('[aria-label="Subagents of Restored root"] [data-session-id="native-parent"]')).not.toBeNull()
    expect(host.querySelector('[aria-label="Subagents of Native parent"] [data-session-id="native-grandchild"] [aria-label="Subagent"]')).not.toBeNull()
    const nativeGroup = host.querySelector('[role="group"][aria-label="Subagents of Native parent"]')!
    expect(nativeGroup.querySelector('[data-session-id="native-grandchild"]')).not.toBeNull()
    expect(requests.filter(query => query.action === 'listManagedSubagents').map(query => query.payload.chatId)).toEqual([child.id])
    await act(async () => root.render(<><FolderTree searchQuery="" /><MainPanel /></>))
    act(() => input('Root draft before descendants'))
    await act(async () => click('#all-chat-list [data-session-id="native-parent"]'))
    const childPane = host.querySelector('[data-testid="chat-pane-native-parent"]')!
    const field = await openOwnFollowup(childPane)
    act(() => { Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')!.set!.call(field, 'Immediate parent draft'); field.dispatchEvent(new Event('input', { bubbles: true })) })
    const scroll = childPane.querySelector<HTMLElement>('.uam-chat-transcript')!
    Object.defineProperties(scroll, { scrollHeight: { value: 2000, configurable: true }, clientHeight: { value: 300, configurable: true } })
    scroll.scrollTop = 260
    act(() => scroll.dispatchEvent(new Event('scroll', { bubbles: true })))
    await act(async () => click('[aria-label="Subagents of Native parent"] [data-session-id="native-grandchild"]'))
    const grandchildPane = host.querySelector('[data-testid="subagent-pane-native-parent"]')!
    expect(grandchildPane).not.toBeNull()
    expect(grandchildPane.textContent).toContain('Subagent of Native parent')
    expect(grandchildPane.textContent).toContain('Stored Distinct grandchild conversation')
    expect(grandchildPane.textContent).not.toContain('Stored Native parent conversation')
    expect(host.querySelector('[data-testid="chat-pane-native-parent"]')).toBeNull()
    expect(grandchildPane.querySelectorAll('nav[aria-label="Subagent lineage"]')).toHaveLength(1); expect(grandchildPane.textContent!.startsWith(grandchildPane.querySelector('nav')!.textContent!)).toBe(true)
    if (passive) {
      expect(grandchildPane.querySelector('textarea,form,input')).toBeNull()
      expect(useAppStore.getState().activeSessionId).toBe(child.id)
      expect(requests.some(query => query.action === 'selectSession' && query.payload.chatId === grandchild.id)).toBe(false)
    } else {
      const executable = grandchildPane.querySelector('[data-testid="chat-pane-native-grandchild"]')!
      const grandchildInput = await openOwnFollowup(executable)
      expect(grandchildInput.disabled).toBe(false)
      expect(useAppStore.getState().activeSessionId).toBe(grandchild.id)
      expect(requests.some(query => query.action === 'selectSession' && query.payload.chatId === grandchild.id)).toBe(true)
    }
    expect(readChatGridLayout().activeLeafId).toBe(leaves[0].id)
    expect(host.querySelector('[data-testid="chat-pane-navigation-other"]')).not.toBeNull()
    await act(async () => back())
    const returned = host.querySelector('[data-testid="chat-pane-native-parent"]')!
    const returnedInput = await openOwnFollowup(returned)
    expect(returnedInput.value).toBe('Immediate parent draft')
    expect(returned.querySelector<HTMLElement>('.uam-chat-transcript')!.scrollTop).toBe(260)
    expect(document.activeElement).toBe(returned.querySelector('textarea'))
    expect(readChatGridLayout().activeLeafId).toBe(leaves[0].id)
    await act(async () => back())
    expect(host.querySelector<HTMLTextAreaElement>('[data-testid="chat-pane-navigation-root"] textarea')!.value).toBe('Root draft before descendants')
    expect(readChatGridLayout().activeLeafId).toBe(leaves[0].id)
    await act(async () => root.render(<><FolderTree searchQuery="Distinct grandchild" /><MainPanel /></>))
    expect(host.querySelector('#all-chat-list [data-session-id="navigation-root"]')).not.toBeNull()
    expect(host.querySelector('[aria-label="Subagents of Native parent"] [data-session-id="native-grandchild"]')).not.toBeNull()
    await act(async () => root.render(<><FolderTree searchQuery="" /><MainPanel /></>))
    expect(host.querySelector('#all-chat-list [data-session-id="navigation-root"]')).not.toBeNull()
    expect(host.querySelectorAll('[data-session-id="native-grandchild"]')).toHaveLength(1)
    expect(host.querySelector('#active-chat-list [data-session-id="native-parent"],#active-chat-list [data-session-id="native-grandchild"]')).toBeNull()
    expect(send).not.toHaveBeenCalled()
  })

  it.each([false, true])('NAV-R4-01: orphan recovery discloses and returns from its actual native descendant (passive=%s)', async passive => {
    const child: Session = { ...parent, id: 'orphan-owner', name: 'Orphan owner', managedSubagentCount: 1, updatedAt: new Date(),
      subagentParentChatId: 'unavailable-ancestor', subagentParentCreatedAt: parent.createdAt.toISOString() }
    const grandchild: Session = { ...child, id: 'orphan-descendant', name: 'Unique orphan descendant', managedSubagentCount: 0, importedReadOnly: passive,
      createdAt: new Date(parent.createdAt.getTime() + 1000), subagentParentChatId: child.id, subagentParentCreatedAt: child.createdAt.toISOString() }
    useAppStore.setState({ sessions: [other, child, grandchild], activeSessionId: other.id, messages: {
      [other.id]: [], [child.id]: [{ id: 'orphan-owner-message', sessionId: child.id, role: 'assistant', content: 'Owner stored conversation', createdAt: new Date() }],
      [grandchild.id]: [{ id: 'orphan-descendant-message', sessionId: grandchild.id, role: 'assistant', content: 'Descendant stored conversation', createdAt: new Date() }],
    } })
    writeChatGridLayout(setChatInLeaf(defaultChatGridLayout, other.id, defaultChatGridLayout.activeLeafId))
    const leafId = readChatGridLayout().activeLeafId
    window.cefQuery = ({ request, onSuccess }) => { const query = JSON.parse(request); requests.push(query)
      onSuccess(JSON.stringify(query.action === 'listManagedSubagents' ? { ...list(), chatId: child.id, controlScopeId: 'orphan-owner-scope' } : { ok: true, data: {} }))
    }
    await act(async () => root.render(<><FolderTree searchQuery="" /><div /></>))
    expect(requests.filter(query => query.action === 'listManagedSubagents')).toHaveLength(0)
    expect(host.querySelector('#active-chat-list [data-session-id="orphan-owner"],#active-chat-list [data-session-id="orphan-descendant"]')).toBeNull()
    const recovery = host.querySelector('[aria-label="Subagents with unavailable parents"]')!
    expect(recovery.querySelector('[aria-label="Open read-only subagent Orphan owner"]')).not.toBeNull()
    await reveal()
    expect(recovery.querySelector('[aria-label="Subagents of Orphan owner"] [data-session-id="orphan-descendant"]')).not.toBeNull()
    expect(requests.filter(query => query.action === 'listManagedSubagents').map(query => query.payload.chatId)).toEqual([child.id])
    await act(async () => root.render(<><FolderTree searchQuery="" /><MainPanel /></>))
    await act(async () => click('[aria-label="Open read-only subagent Orphan owner"]'))
    const ownerPane = host.querySelector('[data-testid="orphan-subagent-pane-orphan-owner"]')!
    expect(ownerPane.textContent).toContain('Parent unavailable')
    orphanReturn(ownerPane)
    expect(ownerPane.querySelector('[aria-label="Close subagent view"]')).toBeNull()
    const field = await openOwnFollowup(ownerPane)
    act(() => { Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')!.set!.call(field, 'Orphan owner independent draft'); field.dispatchEvent(new Event('input', { bubbles: true })) })
    const scroll = ownerPane.querySelector<HTMLElement>('.uam-chat-transcript')!
    Object.defineProperties(scroll, { scrollHeight: { value: 2000, configurable: true }, clientHeight: { value: 300, configurable: true } })
    scroll.scrollTop = 230
    act(() => scroll.dispatchEvent(new Event('scroll', { bubbles: true })))
    await act(async () => click('[aria-label="Subagents of Orphan owner"] [data-session-id="orphan-descendant"]'))
    const pane = host.querySelector('[data-testid="subagent-pane-orphan-owner"]')!
    expect(pane).not.toBeNull()
    expect(pane.textContent).toContain('Subagent of Orphan owner')
    expect(pane.textContent).toContain('Descendant stored conversation')
    expect(pane.textContent).not.toContain('Owner stored conversation')
    expect(pane.querySelector<HTMLButtonElement>('nav button')!.disabled).toBe(false)
    expect(pane.querySelector('textarea')).toBeNull()
    if (!passive) await openOwnFollowup(pane)
    expect(Boolean(pane.querySelector('textarea'))).toBe(!passive)
    expect(useAppStore.getState().activeSessionId).toBe(passive ? child.id : grandchild.id)
    if (passive) expect(requests.some(query => query.action === 'selectSession' && query.payload.chatId === grandchild.id)).toBe(false)
    expect(readChatGridLayout().activeLeafId).toBe(leafId)
    await act(async () => back())
    const returned = host.querySelector('[data-testid="orphan-subagent-pane-orphan-owner"]')!
    const returnedInput = await openOwnFollowup(returned)
    expect(returnedInput.value).toBe('Orphan owner independent draft')
    expect(returned.querySelector<HTMLElement>('.uam-chat-transcript')!.scrollTop).toBe(230)
    expect(document.activeElement).toBe(returned.querySelector('textarea'))
    orphanReturn(returned)
    expect(readChatGridLayout().activeLeafId).toBe(leafId)
    expect(useAppStore.getState().sessions.some(session => session.id === 'unavailable-ancestor')).toBe(false)
    expect(useAppStore.getState().sessions.find(session => session.id === child.id)?.subagentParentChatId).toBe('unavailable-ancestor')
    await act(async () => orphanReturn(returned).click())
    expect(host.querySelector('[data-testid="orphan-subagent-pane-orphan-owner"]')).toBeNull()
    await act(async () => root.render(<><FolderTree searchQuery="Unique orphan descendant" /><MainPanel /></>))
    const searchRecovery = host.querySelector('[aria-label="Subagents with unavailable parents"]')!
    expect(searchRecovery).not.toBeNull()
    expect(searchRecovery.querySelector('[aria-label="Open read-only subagent Orphan owner"]')).not.toBeNull()
    expect(searchRecovery.querySelector('[data-session-id="orphan-descendant"]')).not.toBeNull()
    expect(host.textContent).not.toContain('No chats match this search')
    await act(async () => root.render(<><FolderTree searchQuery="" /><MainPanel /></>))
    expect(host.querySelectorAll('[data-session-id="orphan-descendant"]')).toHaveLength(1)
    expect(host.querySelector('#active-chat-list [data-session-id="orphan-owner"],#active-chat-list [data-session-id="orphan-descendant"]')).toBeNull()
  })

  it('retains passive orphan controls while exposing its valid passive descendant', async () => {
    const owner = { ...parent, id: 'passive-orphan-owner', name: 'Passive orphan owner', importedReadOnly: true, managedSubagentCount: 0,
      subagentParentChatId: 'missing-passive-ancestor', subagentParentCreatedAt: parent.createdAt.toISOString() }
    const child = { ...owner, id: 'passive-orphan-descendant', name: 'Passive orphan descendant', subagentParentChatId: owner.id, subagentParentCreatedAt: owner.createdAt.toISOString() }
    useAppStore.setState({ sessions: [owner, child], activeSessionId: owner.id, messages: { [owner.id]: [], [child.id]: [] } })
    writeChatGridLayout(setChatInLeaf(defaultChatGridLayout, owner.id, defaultChatGridLayout.activeLeafId))
    await act(async () => root.render(<><FolderTree searchQuery="Passive orphan descendant" /><MainPanel /></>))
    expect(requests.filter(query => query.action === 'listManagedSubagents')).toHaveLength(0)
    const orphan = host.querySelector('[data-testid="orphan-subagent-pane-passive-orphan-owner"]')!
    expect(orphan.querySelector('textarea,input,form')).toBeNull()
    orphanReturn(orphan)
    await reveal()
    await act(async () => click('[aria-label="Subagents of Passive orphan owner"] [data-session-id="passive-orphan-descendant"]'))
    const pane = host.querySelector('[data-testid="subagent-pane-passive-orphan-owner"]')!
    expect(pane.querySelector('textarea,input,form')).toBeNull()
    await act(async () => back())
    const returned = host.querySelector('[data-testid="orphan-subagent-pane-passive-orphan-owner"]')!
    expect(returned.querySelector('textarea,input,form')).toBeNull()
    orphanReturn(returned)
    expect(requests.some(query => query.action === 'selectSession' && [owner.id, child.id].includes(query.payload.chatId as string))).toBe(false)
    await act(async () => orphanReturn(returned).click())
    expect(host.querySelector('[data-testid="orphan-subagent-pane-passive-orphan-owner"]')).toBeNull()
  })

  it.each([true, false])('NAV-R5-01: passive orphan owner row returns only the active child pane (passive child=%s)', async passive => {
    const owner = { ...parent, id: 'row-return-owner', name: 'Passive row return owner', importedReadOnly: true, managedSubagentCount: 0,
      subagentParentChatId: 'missing-row-return-parent', subagentParentCreatedAt: parent.createdAt.toISOString() }
    const child = { ...owner, id: 'row-return-child', name: 'Row return child', importedReadOnly: passive, subagentParentChatId: owner.id, subagentParentCreatedAt: owner.createdAt.toISOString() }
    useAppStore.setState({ sessions: [owner, child], activeSessionId: owner.id, messages: { [owner.id]: [], [child.id]: [] } })
    const layout = { version: 2 as const, activeLeafId: 'row-return-right', root: { type: 'split' as const, id: 'row-return-split', direction: 'horizontal' as const, sizes: [50, 50] as [number, number], children: [
      { type: 'leaf' as const, id: 'row-return-left', sessionId: owner.id }, { type: 'leaf' as const, id: 'row-return-right', sessionId: owner.id },
    ] as [import('../../utils/chatGridStorage').ChatPaneLeaf, import('../../utils/chatGridStorage').ChatPaneLeaf] } }
    writeChatGridLayout(layout)
    openSubagentView(owner, { kind: 'native', chatId: child.id }, 'row-return-left')
    writeChatGridLayout({ ...readChatGridLayout(), activeLeafId: 'row-return-right' })
    await act(async () => root.render(<><FolderTree searchQuery="" /><MainPanel /></>))
    expect(host.querySelector('[data-leaf-id="row-return-right"] [data-testid="orphan-subagent-pane-row-return-owner"]')).not.toBeNull()
    await reveal()
    await act(async () => click('[aria-label="Subagents of Passive row return owner"] [data-session-id="row-return-child"]'))
    expect(host.querySelectorAll('[data-testid="subagent-pane-row-return-owner"]')).toHaveLength(2)
    // Preserve requests are used while navigating into a passive descendant and must keep its route.
    await act(async () => useAppStore.getState().setActiveSession(owner.id, { preserveSubagentRoute: true, leafId: 'row-return-right' }))
    expect(subagentRouteFor('row-return-right', owner)).toMatchObject({ kind: 'native', chatId: child.id })
    expect(host.querySelectorAll('[data-testid="subagent-pane-row-return-owner"]')).toHaveLength(2)
    await act(async () => click('[aria-label="Open read-only subagent Passive row return owner"]'))
    expect(host.querySelector('[data-leaf-id="row-return-right"] [data-testid="subagent-pane-row-return-owner"]')).toBeNull()
    const returned = host.querySelector('[data-leaf-id="row-return-right"] [data-testid="orphan-subagent-pane-row-return-owner"]')!
    expect(returned).not.toBeNull()
    orphanReturn(returned)
    expect(returned.querySelector('textarea,form,input')).toBeNull()
    expect(returned.querySelector('[aria-label="Close subagent view"]')).toBeNull()
    expect(subagentRouteFor('row-return-right', owner)).toBeUndefined()
    expect(subagentRouteFor('row-return-left', owner)).toMatchObject({ kind: 'native', chatId: child.id })
    expect(readChatGridLayout().activeLeafId).toBe('row-return-right')
    expect(useAppStore.getState().activeSessionId).toBe(owner.id)
    expect(requests.some(query => query.action === 'selectSession' && query.payload.chatId === owner.id)).toBe(false)
    expect(host.querySelector('#active-chat-list [data-session-id="row-return-owner"],#active-chat-list [data-session-id="row-return-child"]')).toBeNull()
    expect(useAppStore.getState().sessions.find(session => session.id === owner.id)?.subagentParentChatId).toBe('missing-row-return-parent')
    if (passive) expect(requests.some(query => query.action === 'selectSession' && query.payload.chatId === child.id)).toBe(false)
  })

  it('NAV-R5-01: explicit passive orphan return clears only its requested leaf', async () => {
    const owner = { ...parent, id: 'scoped-return-owner', name: 'Scoped return owner', importedReadOnly: true, managedSubagentCount: 0,
      subagentParentChatId: 'missing-scoped-return-parent', subagentParentCreatedAt: parent.createdAt.toISOString() }
    const child = { ...owner, id: 'scoped-return-child', name: 'Scoped return child', subagentParentChatId: owner.id, subagentParentCreatedAt: owner.createdAt.toISOString() }
    useAppStore.setState({ sessions: [owner, child], activeSessionId: owner.id, messages: { [owner.id]: [], [child.id]: [] } })
    writeChatGridLayout({ version: 2, activeLeafId: 'scoped-return-left', root: { type: 'split', id: 'scoped-return-split', direction: 'horizontal', sizes: [50, 50], children: [
      { type: 'leaf', id: 'scoped-return-left', sessionId: owner.id }, { type: 'leaf', id: 'scoped-return-right', sessionId: owner.id },
    ] } })
    openSubagentView(owner, { kind: 'native', chatId: child.id }, 'scoped-return-left')
    openSubagentView(owner, { kind: 'native', chatId: child.id }, 'scoped-return-right')
    writeChatGridLayout({ ...readChatGridLayout(), activeLeafId: 'scoped-return-left' })
    await act(async () => root.render(<MainPanel />))
    expect(host.querySelectorAll('[data-testid="subagent-pane-scoped-return-owner"]')).toHaveLength(2)
    await act(async () => useAppStore.getState().setActiveSession(owner.id, { leafId: 'scoped-return-right' }))
    expect(subagentRouteFor('scoped-return-left', owner)).toMatchObject({ kind: 'native', chatId: child.id })
    expect(subagentRouteFor('scoped-return-right', owner)).toBeUndefined()
    expect(host.querySelector('[data-leaf-id="scoped-return-right"] [data-testid="orphan-subagent-pane-scoped-return-owner"]')).not.toBeNull()
    expect(host.querySelector('[data-leaf-id="scoped-return-left"] [data-testid="subagent-pane-scoped-return-owner"]')).not.toBeNull()
    expect(readChatGridLayout().activeLeafId).toBe('scoped-return-left')
  })

})

it('opens a last-reported saved managed child from All Chats with fixed parent return and no execution authority', async () => {
  const send = vi.fn(); const start = vi.fn(); const stop = vi.fn()
  useAppStore.setState({ sendAcpPrompt: send, startAcpSession: start, stopAcpSession: stop })
  const savedRun = { ...run, status: 'running', lastReported: true, canCancel: true, startedAtEpochMs: 100000, finishedAtEpochMs: 0 }
  window.cefQuery = ({ request, onSuccess }) => {
    const query = JSON.parse(request); requests.push(query)
    onSuccess(JSON.stringify(query.action === 'listManagedSubagents' ? list([savedRun])
      : query.action === 'getManagedAgentTranscript' ? { runId: run.runId, title: 'Saved review worker', status: 'running', lastReported: true, canCancel: false,
        messages: [{ role: 'assistant', content: 'Saved managed child alone' }] }
      : { ok: true, data: {} }))
  }
  await act(async () => root.render(<><FolderTree searchQuery="" /><MainPanel /></>))
  act(() => input('Parent draft survives saved-child inspection'))
  await reveal()
  const group = host.querySelector('[role="group"][aria-label="Subagents of Main task"]')!
  expect(group.textContent).toMatch(/last reported:\s*running/i)
  expect(group.querySelector('[data-managed-stop], .uam-working-ring')).toBeNull()
  await act(async () => click('[role="group"][aria-label="Subagents of Main task"] [data-managed-run="run-selected"]'))
  const pane = host.querySelector<HTMLElement>('[data-testid="subagent-pane-navigation-root"]')!
  expect(pane.querySelectorAll('nav[aria-label="Subagent lineage"]')).toHaveLength(1); expect(pane.textContent!.startsWith(pane.querySelector('nav')!.textContent!)).toBe(true)
  expect(pane.querySelector('nav')?.textContent).toContain('Subagent of Main task')
  expect(pane.querySelector('h3')?.textContent).toBe('Saved review worker')
  expect(pane.textContent).toMatch(/last reported:\s*running/i)
  expect(pane.textContent).toContain('Saved managed child alone')
  expect(pane.textContent).not.toContain('Parent transcript')
  expect(pane.querySelector('textarea,input,form,.uam-working-ring')).toBeNull()
  expect(useAppStore.getState().activeSessionId).toBe(parent.id)
  expect(host.querySelector('#active-chat-list [data-session-id="hidden-transcript"]')).toBeNull()
  expect(useAppStore.getState().sessions.some(session => session.id === run.transcriptChatId)).toBe(false)
  expect(requests.filter(query => query.action === 'getManagedAgentTranscript').at(-1)?.payload).toEqual({ chatId: parent.id, transcriptChatId: run.transcriptChatId })
  expect(requests.some(query => ['sendAcpPrompt', 'startAcpSession', 'resumeAcpSession', 'stopAcpSession', 'cancelManagedSubagent', 'openNativeSessionChat'].includes(query.action))).toBe(false)
  expect(requests.some(query => query.action === 'selectSession' && query.payload.chatId === run.transcriptChatId)).toBe(false)
  expect(send).not.toHaveBeenCalled(); expect(start).not.toHaveBeenCalled(); expect(stop).not.toHaveBeenCalled()
  await act(async () => back())
  const returned = host.querySelector<HTMLElement>('[data-testid="chat-pane-navigation-root"]')!
  expect(returned.querySelector<HTMLTextAreaElement>('textarea')?.value).toBe('Parent draft survives saved-child inspection')
  expect(host.querySelector('[data-testid="subagent-pane-navigation-root"]')).toBeNull()
})

it('does not announce saved child activity as current while its transcript response is pending', async () => {
  let reply: ((value: string) => void) | undefined
  const saved = { ...run, status: 'running', lastReported: true, canCancel: false }
  window.cefQuery = ({ request, onSuccess }) => {
    const query = JSON.parse(request); requests.push(query)
    if (query.action === 'getManagedAgentTranscript') { reply = onSuccess; return }
    onSuccess(JSON.stringify(query.action === 'listManagedSubagents' ? list([saved]) : { ok: true, data: {} }))
  }
  await act(async () => root.render(<><FolderTree searchQuery="" /><MainPanel /></>))
  await reveal()
  await act(async () => click('[role="group"][aria-label="Subagents of Main task"] [data-managed-run="run-selected"]'))
  expect(reply).toBeTypeOf('function')
  const pane = host.querySelector<HTMLElement>('[data-testid="subagent-pane-navigation-root"]')!
  expect(pane.querySelector('.uam-managed-conversation__meta[role="status"]')?.textContent).not.toMatch(/^Running\b/i)
  expect(pane.querySelector('.uam-working-ring, textarea, input, form')).toBeNull()
  await act(async () => reply!(JSON.stringify({ runId: run.runId, title: 'Saved review worker', status: 'running', lastReported: true, messages: [{ role: 'assistant', content: 'Previously saved work' }] })))
  expect(pane.querySelector('.uam-managed-conversation__meta[role="status"]')?.textContent).toMatch(/last reported:\s*running/i)
  expect(pane.textContent).toContain('Previously saved work')
  expect(pane.querySelectorAll('nav[aria-label="Subagent lineage"]')).toHaveLength(1); expect(pane.textContent!.startsWith(pane.querySelector('nav')!.textContent!)).toBe(true)
})

// Source-controlled row geometry: row text determines height; revealing/appending rows grows content.
// DOM order/absolute message index determines each row's actual viewport-relative position.
function measureChildReadingRegion() {
  const positions = new WeakMap<Element, number>()
  const isRegion = (element: Element) => element.classList.contains('uam-subagent-transcript')
  const rows = (region: Element) => [...region.querySelectorAll<HTMLElement>('[data-message-index]')]
  const rowHeight = (row: HTMLElement) => Math.max(40, Math.ceil((row.textContent?.length ?? 0) / 24) * 20)
  vi.spyOn(Element.prototype, 'scrollHeight', 'get').mockImplementation(function () { return isRegion(this) ? rows(this).reduce((height, row) => height + rowHeight(row), 0) : 0 })
  vi.spyOn(Element.prototype, 'clientHeight', 'get').mockImplementation(function () { return isRegion(this) ? 300 : 0 })
  vi.spyOn(Element.prototype, 'scrollTop', 'get').mockImplementation(function () { return positions.get(this) ?? 0 })
  vi.spyOn(Element.prototype, 'scrollTop', 'set').mockImplementation(function (value) {
    positions.set(this, isRegion(this) ? Math.min(Math.max(0, value), Math.max(0, this.scrollHeight - this.clientHeight)) : value)
  })
  const originalRect = Element.prototype.getBoundingClientRect
  vi.spyOn(Element.prototype, 'getBoundingClientRect').mockImplementation(function () {
    if (isRegion(this)) return new DOMRect(0, 0, 640, 300)
    const region = this.closest('.uam-subagent-transcript')
    if (region && this.hasAttribute('data-message-index')) {
      const preceding = rows(region).slice(0, rows(region).indexOf(this as HTMLElement))
      return new DOMRect(0, preceding.reduce((height, row) => height + rowHeight(row), 0) - region.scrollTop, 640, rowHeight(this as HTMLElement))
    }
    return originalRect.call(this)
  })
}

type ReadingVisit = { prefix: number; index: string; text: string; offset: number; top: number }
function readingVisit(pane: Element): ReadingVisit {
  const region = pane.querySelector<HTMLElement>('.uam-subagent-transcript')!
  expect(region).not.toBeNull()
  const rows = [...region.querySelectorAll<HTMLElement>('[data-message-index]')]
  const prefix = Number(rows[0]?.dataset.messageIndex)
  expect(rows.length).toBeGreaterThan(0)
  // A continuous window may reveal more earlier rows, but must never have a hole in its loaded rows.
  expect(rows.map(row => Number(row.dataset.messageIndex))).toEqual(rows.map((_, index) => prefix + index))
  const bounds = region.getBoundingClientRect()
  const visible = rows.find(row => row.getBoundingClientRect().bottom > bounds.top && row.getBoundingClientRect().top < bounds.bottom)
  expect(visible, 'A real visible message is required by the geometry fixture').toBeDefined()
  return { prefix, index: visible!.dataset.messageIndex!, text: visible!.textContent!,
    offset: visible!.getBoundingClientRect().top - bounds.top, top: region.scrollTop }
}
function expectReadingVisit(pane: Element, visit: ReadingVisit) {
  const current = readingVisit(pane)
  expect.soft(current.prefix).toBe(visit.prefix)
  expect.soft(current.index).toBe(visit.index)
  expect.soft(current.text).toBe(visit.text)
  expect.soft(current.offset).toBe(visit.offset)
  expect.soft(current.top).toBe(visit.top)
}
function commitReadingScroll(pane: Element): ReadingVisit {
  const before = readingVisit(pane)
  const region = pane.querySelector<HTMLElement>('.uam-subagent-transcript')!
  act(() => region.dispatchEvent(new Event('scroll', { bubbles: true })))
  const after = readingVisit(pane)
  // A prepend may extend the rendered prefix, but must leave the visible content fixed in the viewport.
  expect(after.prefix).toBeLessThanOrEqual(before.prefix)
  expect(after.index).toBe(before.index)
  expect(after.text).toBe(before.text)
  expect(after.offset).toBe(before.offset)
  return after
}
function expectAtChildTail(pane: Element) {
  const region = pane.querySelector<HTMLElement>('.uam-subagent-transcript')!
  expect(region.scrollTop).toBe(Math.max(0, region.scrollHeight - region.clientHeight))
}

async function openOwnFollowup(pane: Element) {
  expect(pane.querySelector('textarea')).toBeNull()
  const open = [...pane.querySelectorAll<HTMLButtonElement>('nav button')]
    .find(button => (button.getAttribute('aria-label') || button.textContent?.trim()) === 'Follow up')
  expect(open, 'The actually executable current Session must expose explicit Follow up').toBeDefined()
  await act(async () => open!.click())
  const field = pane.querySelector<HTMLTextAreaElement>('textarea')
  expect(field).not.toBeNull()
  return field!
}

// Continuous child scrolling: reaching the top reveals the next earlier batch of loaded rows (formerly Earlier messages).
async function revealEarlierRows(pane: Element) {
  const region = pane.querySelector<HTMLElement>('.uam-subagent-transcript')!
  region.scrollTop = 0
  await act(async () => region.dispatchEvent(new Event('scroll', { bubbles: true })))
}

/** Absolute index of the first rendered child row (the window always renders through the newest loaded row). */
function firstRendered(pane: Element) {
  return Number(pane.querySelector('.uam-subagent-transcript [data-message-index]')?.getAttribute('data-message-index'))
}

function childButton(pane: Element, label: string) {
  const found = [...pane.querySelectorAll<HTMLButtonElement>('button')].find(button => button.textContent === label)
  expect(found, label).toBeDefined()
  return found!
}

it('retains managed child page and off-tail scroll after Back and a delayed fresh transcript read', async () => {
  measureChildReadingRegion()
  const selected = { ...run, runId: 'viewport-managed-delayed', transcriptChatId: 'viewport-managed-transcript' }
  const messages = Array.from({ length: 65 }, (_, index) => ({ role: 'assistant', content: `Managed viewport message ${index + 1}` }))
  const response = { runId: selected.runId, title: 'Managed viewport reader', status: 'completed', messages }
  let reads = 0
  let reply: ((value: string) => void) | undefined
  window.cefQuery = ({ request, onSuccess }) => {
    const query = JSON.parse(request); requests.push(query)
    if (query.action === 'getManagedAgentTranscript') {
      expect(query.payload).toEqual({ chatId: parent.id, transcriptChatId: selected.transcriptChatId })
      if (++reads === 2) { reply = onSuccess; return }
      onSuccess(JSON.stringify(response)); return
    }
    onSuccess(JSON.stringify(query.action === 'listManagedSubagents' ? list([selected]) : { ok: true, data: {} }))
  }
  await act(async () => root.render(<MainPanel />))
  const open = () => openSubagentView(parent, { kind: 'managed', runId: selected.runId, transcriptChatId: selected.transcriptChatId })
  await act(async () => open())
  let pane = host.querySelector('[data-testid="subagent-pane-navigation-root"]')!
  expect(firstRendered(pane)).toBe(45)
  await revealEarlierRows(pane)
  await revealEarlierRows(pane)
  expect(firstRendered(pane)).toBe(5)
  const oldRegion = pane.querySelector<HTMLElement>('[aria-label="Stored conversation messages"]')!
  oldRegion.scrollTop = 230
  const savedVisit = commitReadingScroll(pane)
  await act(async () => childButton(pane, 'Back to parent').click())
  expect(oldRegion.isConnected).toBe(false)
  expect(host.querySelector('[data-testid="chat-pane-navigation-root"]')).not.toBeNull()
  await act(async () => open())
  pane = host.querySelector('[data-testid="subagent-pane-navigation-root"]')!
  expect(reads).toBe(2)
  expect(reply).toBeTypeOf('function')
  expect(pane.querySelector('[aria-label="Stored conversation messages"]')).toBeNull()
  expect(pane.textContent).toContain('Loading conversation')
  await act(async () => reply!(JSON.stringify(response)))
  const returned = pane.querySelector<HTMLElement>('[aria-label="Stored conversation messages"]')!
  expect(returned).not.toBe(oldRegion)
  expectReadingVisit(pane, savedVisit)
  expect.soft(returned.textContent).toContain('Managed viewport message 21')
  expect(returned.querySelector('[data-message-index=\"20\"]')).not.toBeNull() // Original owned reading rows stay available; earlier rows remain continuous.
  expect.soft(returned.scrollTop).toBe(savedVisit.top)
  expect(requests.some(query => ['sendAcpPrompt', 'submitManagedSubagentFollowup', 'startAcpSession', 'stopAcpSession', 'cancelManagedSubagent', 'openNativeSessionChat'].includes(query.action))).toBe(false)
})

it('retains passive native child page and scroll after Back while reopened history loads asynchronously', async () => {
  measureChildReadingRegion()
  const child: Session = { ...parent, id: 'viewport-passive-native', name: 'Native viewport reader', importedReadOnly: true }
  rememberSubagentParent(parent, child.id)
  const messages = Array.from({ length: 65 }, (_, index) => ({ id: `native-viewport-${index}`, sessionId: child.id, role: 'assistant' as const, content: `Native viewport message ${index + 1}`, createdAt: new Date('2026-10-16T10:00:00Z') }))
  let reads = 0
  let deferReads = false
  let delayedRead: Promise<void> | undefined
  let finishRead: (() => void) | undefined
  useAppStore.setState(state => ({ sessions: [...state.sessions, child], messages: { ...state.messages, [child.id]: messages },
    loadSessionMessages: vi.fn(async (chatId: string) => {
      if (chatId !== child.id) return
      ++reads
      if (!deferReads) return
      delayedRead ??= new Promise<void>(resolve => { finishRead = () => { useAppStore.setState(state => ({ messages: { ...state.messages, [child.id]: messages } })); resolve() } })
      await delayedRead
    }) }))
  await act(async () => root.render(<MainPanel />))
  const open = () => openSubagentView(parent, { kind: 'native', chatId: child.id })
  await act(async () => open())
  let pane = host.querySelector('[data-testid="subagent-pane-navigation-root"]')!
  expect(firstRendered(pane)).toBe(45)
  await revealEarlierRows(pane)
  await revealEarlierRows(pane)
  expect(firstRendered(pane)).toBe(5)
  const oldRegion = pane.querySelector<HTMLElement>('[aria-label="Native conversation messages"]')!
  oldRegion.scrollTop = 260
  const savedVisit = commitReadingScroll(pane)
  await act(async () => childButton(pane, 'Back to parent').click())
  expect(oldRegion.isConnected).toBe(false)
  // A fresh history response, rather than the retained Zustand message array, must restore the visit.
  await act(async () => useAppStore.setState(state => ({ messages: Object.fromEntries(Object.entries(state.messages).filter(([id]) => id !== child.id)) })))
  const readsBeforeReopen = reads
  deferReads = true
  await act(async () => open())
  pane = host.querySelector('[data-testid="subagent-pane-navigation-root"]')!
  expect(reads).toBeGreaterThan(readsBeforeReopen)
  expect(finishRead).toBeTypeOf('function')
  expect(pane.querySelector('[aria-label="Native conversation messages"]')).toBeNull()
  expect(pane.textContent).toContain('Loading native conversation')
  await act(async () => finishRead!())
  const returned = pane.querySelector<HTMLElement>('[aria-label="Native conversation messages"]')!
  expect(returned).not.toBe(oldRegion)
  expectReadingVisit(pane, savedVisit)
  expect.soft(returned.textContent).toContain('Native viewport message 21')
  expect(returned.querySelector('[data-message-index=\"20\"]')).not.toBeNull() // Original owned reading rows stay available; earlier rows remain continuous.
  expect.soft(returned.scrollTop).toBe(savedVisit.top)
  expect(pane.querySelector('textarea,input,form')).toBeNull()
  expect(requests.some(query => ['sendAcpPrompt', 'startAcpSession', 'stopAcpSession', 'cancelManagedSubagent', 'openNativeSessionChat'].includes(query.action))).toBe(false)
})

it('isolates managed child reading visits between leaves and sibling transcripts before restoring the original leaf', async () => {
  measureChildReadingRegion()
  const a = { ...run, runId: 'viewport-scope-a', transcriptChatId: 'viewport-scope-a-transcript' }
  const b = { ...run, runId: 'viewport-scope-b', transcriptChatId: 'viewport-scope-b-transcript' }
  window.cefQuery = ({ request, onSuccess }) => {
    const query = JSON.parse(request); requests.push(query)
    const selected = query.payload.transcriptChatId === b.transcriptChatId ? b : a
    onSuccess(JSON.stringify(query.action === 'listManagedSubagents' ? list([a, b]) : query.action === 'getManagedAgentTranscript'
      ? { runId: selected.runId, title: selected.runId, status: 'completed', messages: Array.from({ length: 65 }, (_, index) => ({ role: 'assistant', content: `${selected.runId} message ${index + 1}` })) }
      : { ok: true, data: {} }))
  }
  writeChatGridLayout({ version: 2, activeLeafId: 'viewport-left', root: { type: 'split', id: 'viewport-split', direction: 'horizontal', sizes: [50, 50], children: [
    { type: 'leaf', id: 'viewport-left', sessionId: parent.id }, { type: 'leaf', id: 'viewport-right', sessionId: parent.id },
  ] } })
  await act(async () => root.render(<MainPanel />))
  const open = (selected: typeof a, leaf: string) => openSubagentView(parent, { kind: 'managed', runId: selected.runId, transcriptChatId: selected.transcriptChatId }, leaf)
  const paneAt = (leaf: string) => host.querySelector(`[data-leaf-id="${leaf}"] [data-testid="subagent-pane-navigation-root"]`)!
  await act(async () => open(a, 'viewport-left'))
  let left = paneAt('viewport-left')
  await revealEarlierRows(left)
  await revealEarlierRows(left)
  const original = left.querySelector<HTMLElement>('[aria-label="Stored conversation messages"]')!
  original.scrollTop = 310
  const originalVisit = commitReadingScroll(left)
  await act(async () => childButton(left, 'Back to parent').click())
  await act(async () => open(a, 'viewport-right'))
  const right = paneAt('viewport-right')
  expect(firstRendered(right)).toBe(45)
  expectAtChildTail(right)
  await act(async () => open(b, 'viewport-left'))
  left = paneAt('viewport-left')
  expect(firstRendered(left)).toBe(45)
  expect(left.textContent).toContain('viewport-scope-b message 61')
  expectAtChildTail(left)
  await act(async () => childButton(left, 'Back to parent').click())
  await act(async () => open(a, 'viewport-left'))
  left = paneAt('viewport-left')
  expectReadingVisit(left, originalVisit)
  expect.soft(left.querySelector<HTMLElement>('[aria-label="Stored conversation messages"]')!.scrollTop).toBe(originalVisit.top)
  expect(firstRendered(right)).toBe(45)
  expectAtChildTail(right)
  expect(requests.some(query => ['sendAcpPrompt', 'submitManagedSubagentFollowup', 'startAcpSession', 'stopAcpSession', 'cancelManagedSubagent', 'openNativeSessionChat'].includes(query.action))).toBe(false)
})

it.each([false, true])('keeps a managed latest-page reader at its page after growth unless following the tail (followTail=%s)', async followTail => {
  measureChildReadingRegion()
  const selected = { ...run, runId: `viewport-growth-managed-${followTail}`, transcriptChatId: `viewport-growth-managed-transcript-${followTail}` }
  const messages = (count: number) => Array.from({ length: count }, (_, index) => ({ role: 'assistant', content: `Managed growing message ${index + 1}` }))
  let reads = 0
  let reply: ((value: string) => void) | undefined
  window.cefQuery = ({ request, onSuccess }) => {
    const query = JSON.parse(request); requests.push(query)
    if (query.action === 'getManagedAgentTranscript') {
      expect(query.payload).toEqual({ chatId: parent.id, transcriptChatId: selected.transcriptChatId })
      if (++reads > 1) { reply = onSuccess; return }
      onSuccess(JSON.stringify({ runId: selected.runId, status: 'completed', messages: messages(65) })); return
    }
    onSuccess(JSON.stringify(query.action === 'listManagedSubagents' ? list([selected]) : { ok: true, data: {} }))
  }
  await act(async () => root.render(<MainPanel />))
  const open = () => openSubagentView(parent, { kind: 'managed', runId: selected.runId, transcriptChatId: selected.transcriptChatId })
  await act(async () => open())
  let pane = host.querySelector('[data-testid="subagent-pane-navigation-root"]')!
  expect(firstRendered(pane)).toBe(45)
  const oldRegion = pane.querySelector<HTMLElement>('[aria-label="Stored conversation messages"]')!
  oldRegion.scrollTop = followTail ? oldRegion.scrollHeight - oldRegion.clientHeight : 230
  const savedVisit = commitReadingScroll(pane)
  await act(async () => childButton(pane, 'Back to parent').click())
  expect(oldRegion.isConnected).toBe(false)
  await act(async () => open())
  pane = host.querySelector('[data-testid="subagent-pane-navigation-root"]')!
  expect(reads).toBe(2)
  expect(reply).toBeTypeOf('function')
  expect(pane.querySelector('[aria-label="Stored conversation messages"]')).toBeNull()
  await act(async () => reply!(JSON.stringify({ runId: selected.runId, status: 'completed', messages: messages(81) })))
  const returned = pane.querySelector<HTMLElement>('[aria-label="Stored conversation messages"]')!
  expect(returned).not.toBe(oldRegion)
  if (followTail) expect(firstRendered(pane)).toBe(61)
  else expectReadingVisit(pane, savedVisit)
  expect.soft(returned.textContent).toContain(`Managed growing message ${followTail ? 81 : 61}`)
  expect(returned.querySelector('[data-message-index="80"]')).not.toBeNull() // New rows render below the off-tail reader, without moving its visible message.
  if (followTail) expectAtChildTail(pane)
  else expect.soft(returned.scrollTop).toBe(savedVisit.top)
  expect(requests.some(query => ['submitManagedSubagentFollowup', 'sendAcpPrompt', 'startAcpSession', 'stopAcpSession', 'cancelManagedSubagent'].includes(query.action))).toBe(false)
})

it.each([false, true])('keeps a passive native latest-page reader at its page after growth unless following the tail (followTail=%s)', async followTail => {
  measureChildReadingRegion()
  const child: Session = { ...parent, id: `viewport-growth-native-${followTail}`, name: 'Growing native reader', importedReadOnly: true }
  rememberSubagentParent(parent, child.id)
  const messages = (count: number) => Array.from({ length: count }, (_, index) => ({ id: `native-growth-${index}`, sessionId: child.id, role: 'assistant' as const, content: `Native growing message ${index + 1}`, createdAt: new Date('2026-10-16T10:00:00Z') }))
  let deferReads = false
  let delayedRead: Promise<void> | undefined
  let finishRead: (() => void) | undefined
  useAppStore.setState(state => ({ sessions: [...state.sessions, child], messages: { ...state.messages, [child.id]: messages(65) },
    loadSessionMessages: vi.fn(async (chatId: string) => {
      if (chatId !== child.id || !deferReads) return
      delayedRead ??= new Promise<void>(resolve => { finishRead = () => { useAppStore.setState(state => ({ messages: { ...state.messages, [child.id]: messages(81) } })); resolve() } })
      await delayedRead
    }) }))
  await act(async () => root.render(<MainPanel />))
  const open = () => openSubagentView(parent, { kind: 'native', chatId: child.id })
  await act(async () => open())
  let pane = host.querySelector('[data-testid="subagent-pane-navigation-root"]')!
  expect(firstRendered(pane)).toBe(45)
  const oldRegion = pane.querySelector<HTMLElement>('[aria-label="Native conversation messages"]')!
  oldRegion.scrollTop = followTail ? oldRegion.scrollHeight - oldRegion.clientHeight : 230
  const savedVisit = commitReadingScroll(pane)
  await act(async () => childButton(pane, 'Back to parent').click())
  expect(oldRegion.isConnected).toBe(false)
  await act(async () => useAppStore.setState(state => ({ messages: Object.fromEntries(Object.entries(state.messages).filter(([id]) => id !== child.id)) })))
  deferReads = true
  await act(async () => open())
  pane = host.querySelector('[data-testid="subagent-pane-navigation-root"]')!
  expect(finishRead).toBeTypeOf('function')
  expect(pane.querySelector('[aria-label="Native conversation messages"]')).toBeNull()
  await act(async () => finishRead!())
  const returned = pane.querySelector<HTMLElement>('[aria-label="Native conversation messages"]')!
  expect(returned).not.toBe(oldRegion)
  if (followTail) expect(firstRendered(pane)).toBe(61)
  else expectReadingVisit(pane, savedVisit)
  expect.soft(returned.textContent).toContain(`Native growing message ${followTail ? 81 : 61}`)
  expect(returned.querySelector('[data-message-index="80"]')).not.toBeNull() // New rows render below the off-tail reader, without moving its visible message.
  if (followTail) expectAtChildTail(pane)
  else expect.soft(returned.scrollTop).toBe(savedVisit.top)
  expect(pane.querySelector('textarea,input,form')).toBeNull()
  expect(requests.some(query => ['sendAcpPrompt', 'startAcpSession', 'stopAcpSession', 'openNativeSessionChat'].includes(query.action))).toBe(false)
})

it('replaces live sibling A with the exact saved sibling B in the same pane and ignores a late A transcript reply', async () => {
  const liveA = { ...run, runId: 'sibling-live-A', transcriptChatId: 'sibling-A-transcript', agentId: 'Live sibling A', taskTitle: 'Live sibling A', status: 'running', lastReported: false, canCancel: false }
  const savedB = { ...run, runId: 'sibling-saved-B', transcriptChatId: 'sibling-B-transcript', agentId: 'Saved sibling B', taskTitle: 'Saved sibling B', status: 'running', lastReported: true, canCancel: false }
  let aReads = 0
  let lateA: ((value: string) => void) | undefined
  let replyB: ((value: string) => void) | undefined
  useAppStore.setState(state => ({ sessions: state.sessions.map(session => session.id === parent.id ? { ...session, managedSubagentCount: 2 } : session) }))
  window.cefQuery = ({ request, onSuccess, onFailure }) => {
    const query = JSON.parse(request); requests.push(query)
    if (query.action === 'listManagedSubagents') { onSuccess(JSON.stringify(list([liveA, savedB]))); return }
    if (query.action === 'getManagedAgentTranscript') {
      expect(query.payload.chatId).toBe(parent.id)
      if (query.payload.transcriptChatId === liveA.transcriptChatId) {
        ++aReads
        if (aReads === 1) onSuccess(JSON.stringify({ runId: liveA.runId, title: liveA.agentId, status: 'running', lastReported: false, messages: [{ role: 'assistant', content: 'CURRENT-A-ONLY' }] }))
        else if (aReads === 2) lateA = onSuccess
        else onFailure(409, 'Unexpected additional A transcript request')
      } else if (query.payload.transcriptChatId === savedB.transcriptChatId) replyB = onSuccess
      else onFailure(404, 'Unknown sibling transcript identity')
      return
    }
    onSuccess(JSON.stringify({ ok: true, data: {} }))
  }
  await act(async () => root.render(<><FolderTree searchQuery="" /><MainPanel /></>))
  const leaf = readChatGridLayout().activeLeafId
  await reveal()
  await act(async () => click('[role="group"][aria-label="Subagents of Main task"] [data-managed-run="sibling-live-A"]'))
  const livePane = host.querySelector<HTMLElement>('[data-testid="subagent-pane-navigation-root"]')!
  expect(livePane.textContent).toContain('CURRENT-A-ONLY')
  expect(livePane.querySelector('.uam-managed-conversation__meta[role="status"]')?.textContent).toBe('Running')
  expect(subagentRouteFor(leaf, parent)).toMatchObject({ kind: 'managed', runId: liveA.runId, transcriptChatId: liveA.transcriptChatId })
  await act(async () => [...livePane.querySelectorAll<HTMLButtonElement>('button')].find(control => control.getAttribute('aria-label') === 'Refresh conversation')!.click())
  expect(lateA).toBeTypeOf('function')
  expect(aReads).toBe(2)
  await act(async () => click('[role="group"][aria-label="Subagents of Main task"] [data-managed-run="sibling-saved-B"]'))
  expect(replyB).toBeTypeOf('function')
  expect(requests.filter(query => query.action === 'getManagedAgentTranscript').map(query => query.payload)).toEqual([
    { chatId: parent.id, transcriptChatId: liveA.transcriptChatId },
    { chatId: parent.id, transcriptChatId: liveA.transcriptChatId },
    { chatId: parent.id, transcriptChatId: savedB.transcriptChatId },
  ])
  const pendingB = host.querySelector<HTMLElement>('[data-testid="subagent-pane-navigation-root"]')!
  expect(pendingB).not.toBeNull()
  expect(pendingB.closest('[data-leaf-id]')?.getAttribute('data-leaf-id')).toBe(leaf) // Same logical pane; a different child may remount its DOM.
  expect(readChatGridLayout().activeLeafId).toBe(leaf)
  expect(subagentRouteFor(leaf, parent)).toMatchObject({ kind: 'managed', runId: savedB.runId, transcriptChatId: savedB.transcriptChatId })
  expect(pendingB.textContent).not.toContain('CURRENT-A-ONLY')
  expect(pendingB.querySelector('h3')?.textContent).toBe(savedB.taskTitle)
  expect(pendingB.querySelector('.uam-managed-conversation__meta[role="status"]')?.textContent).not.toMatch(/^Running\b/i)
  expect(pendingB.querySelector('.uam-working-ring, textarea, input, form')).toBeNull()
  await act(async () => replyB!(JSON.stringify({ runId: savedB.runId, title: 'Correct saved sibling B', status: 'running', lastReported: true,
    messages: [{ role: 'assistant', content: 'SAVED-B-ONLY' }] })))
  expect(pendingB.textContent).toContain('SAVED-B-ONLY')
  expect(pendingB.querySelector('h3')?.textContent).toBe('Correct saved sibling B')
  expect(pendingB.querySelector('.uam-managed-conversation__meta[role="status"]')?.textContent).toMatch(/last reported:\s*running/i)
  await act(async () => lateA!(JSON.stringify({ runId: liveA.runId, title: 'Late sibling A', status: 'running', lastReported: false,
    messages: [{ role: 'assistant', content: 'LATE-A-REPLY' }] })))
  expect(pendingB.textContent).toContain('SAVED-B-ONLY')
  expect(pendingB.textContent).not.toContain('CURRENT-A-ONLY')
  expect(pendingB.textContent).not.toContain('LATE-A-REPLY')
  expect(pendingB.querySelector('h3')?.textContent).toBe('Correct saved sibling B')
  expect(pendingB.querySelector('[role="alert"]')).toBeNull()
  expect(pendingB.querySelectorAll('nav[aria-label="Subagent lineage"]')).toHaveLength(1); expect(pendingB.textContent!.startsWith(pendingB.querySelector('nav')!.textContent!)).toBe(true)
  expect(requests.filter(query => query.action === 'getManagedAgentTranscript')).toHaveLength(3)
  expect(aReads).toBe(2)
  expect(useAppStore.getState().activeSessionId).toBe(parent.id)
  expect(requests.some(query => ['sendAcpPrompt', 'startAcpSession', 'resumeAcpSession', 'stopAcpSession', 'cancelManagedSubagent', 'openNativeSessionChat'].includes(query.action))).toBe(false)
})

it('keeps a mounted passive native latest-page reader on messages 61–80 when message 81 arrives off-tail', async () => {
  measureChildReadingRegion()
  const child: Session = { ...parent, id: 'viewport-mounted-native-growth', name: 'Mounted native reader', importedReadOnly: true }
  rememberSubagentParent(parent, child.id)
  const messages = Array.from({ length: 81 }, (_, index) => ({ id: `native-mounted-growth-${index}`, sessionId: child.id,
    role: 'assistant' as const, content: `Native mounted message ${index + 1}`, createdAt: new Date('2026-10-16T10:00:00Z') }))
  useAppStore.setState(state => ({ sessions: [...state.sessions, child], messages: { ...state.messages, [child.id]: messages.slice(0, 80) } }))
  await act(async () => root.render(<MainPanel />))
  await act(async () => openSubagentView(parent, { kind: 'native', chatId: child.id }))
  const pane = host.querySelector('[data-testid="subagent-pane-navigation-root"]')!
  expect(firstRendered(pane)).toBe(60)
  const region = pane.querySelector<HTMLElement>('[aria-label="Native conversation messages"]')!
  region.scrollTop = 230
  const savedVisit = commitReadingScroll(pane)

  // This update reaches the already mounted reader; no Back, reopen or history reload occurs.
  await act(async () => useAppStore.setState(state => ({ messages: { ...state.messages, [child.id]: messages } })))
  expect(pane.querySelector('[aria-label="Native conversation messages"]')).toBe(region)
  expectReadingVisit(pane, savedVisit)
  expect.soft(region.textContent).toContain('Native mounted message 61')
  expect.soft(region.textContent).toContain('Native mounted message 80')
  expect(region.querySelector('[data-message-index="80"]')).not.toBeNull() // Appended row is below the same visible reader anchor.
  expect.soft(region.scrollTop).toBe(savedVisit.top)
  expect(pane.querySelector('textarea,input,form')).toBeNull()
  expect(requests.some(query => ['sendAcpPrompt', 'submitManagedSubagentFollowup', 'startAcpSession', 'stopAcpSession', 'cancelManagedSubagent', 'openNativeSessionChat'].includes(query.action))).toBe(false)
})

it('restores the same managed child page and scroll when reopening its thread card without a transcript route hint', async () => {
  measureChildReadingRegion()
  const selected = { ...run, runId: 'viewport-canonical-route-run', transcriptChatId: 'viewport-canonical-route-transcript' }
  const response = { runId: selected.runId, title: 'Canonical managed reader', status: 'completed',
    messages: Array.from({ length: 65 }, (_, index) => ({ role: 'assistant', content: `Canonical managed message ${index + 1}` })) }
  let reads = 0
  let reply: ((value: string) => void) | undefined
  window.cefQuery = ({ request, onSuccess }) => {
    const query = JSON.parse(request); requests.push(query)
    if (query.action === 'getManagedAgentTranscript') {
      expect(query.payload).toEqual({ chatId: parent.id, transcriptChatId: selected.transcriptChatId })
      if (++reads === 2) { reply = onSuccess; return }
      onSuccess(JSON.stringify(response)); return
    }
    onSuccess(JSON.stringify(query.action === 'listManagedSubagents' ? list([selected]) : { ok: true, data: {} }))
  }
  useAppStore.setState(state => ({ messages: { ...state.messages, [parent.id]: [{ ...state.messages[parent.id][0],
    toolCalls: [{ id: 'viewport-canonical-route-result', title: 'uam_agent_result', status: 'completed', kind: 'sub-agent', content: '',
      isSubAgent: true, subAgentId: selected.runId, subAgentTitle: 'Canonical managed reader' }] }] } }))
  await act(async () => root.render(<MainPanel />))
  const leafId = readChatGridLayout().activeLeafId
  await act(async () => openSubagentView(parent, { kind: 'managed', runId: selected.runId, transcriptChatId: selected.transcriptChatId }, leafId))
  expect(subagentRouteFor(leafId, parent)).toMatchObject({ kind: 'managed', runId: selected.runId, transcriptChatId: selected.transcriptChatId })
  let pane = host.querySelector('[data-testid="subagent-pane-navigation-root"]')!
  expect(firstRendered(pane)).toBe(45)
  await revealEarlierRows(pane)
  await revealEarlierRows(pane)
  expect(firstRendered(pane)).toBe(5)
  const oldRegion = pane.querySelector<HTMLElement>('[aria-label="Stored conversation messages"]')!
  oldRegion.scrollTop = 230
  const savedVisit = commitReadingScroll(pane)
  await act(async () => childButton(pane, 'Back to parent').click())
  expect(oldRegion.isConnected).toBe(false)
  expect(host.querySelector('[data-testid="chat-pane-navigation-root"]')).not.toBeNull()

  // The real parent thread card supplies the same run ID and resolves its transcript through the catalogue.
  await act(async () => {
    const details = host.querySelector<HTMLDetailsElement>('details.uam-subagent-group')!
    expect(details).not.toBeNull()
    details.open = true
    details.dispatchEvent(new Event('toggle', { bubbles: true }))
  })
  await act(async () => click(`[data-subagent-row="managed:${selected.runId}"]`))
  expect(readChatGridLayout().activeLeafId).toBe(leafId)
  const cardRoute = subagentRouteFor(leafId, parent)
  expect(cardRoute).toMatchObject({ kind: 'managed', runId: selected.runId })
  expect(cardRoute && cardRoute.kind === 'managed' ? cardRoute.transcriptChatId : undefined).toBeUndefined()
  expect(reads).toBe(2)
  expect(reply).toBeTypeOf('function')
  pane = host.querySelector('[data-testid="subagent-pane-navigation-root"]')!
  expect(pane.textContent).toContain('Loading conversation')
  await act(async () => reply!(JSON.stringify(response)))
  const returned = pane.querySelector<HTMLElement>('[aria-label="Stored conversation messages"]')!
  expect(returned).not.toBe(oldRegion)
  expectReadingVisit(pane, savedVisit)
  expect.soft(returned.textContent).toContain('Canonical managed message 21')
  expect.soft(returned.textContent).toContain('Canonical managed message 40')
  expect(returned.querySelector('[data-message-index=\"20\"]')).not.toBeNull() // Original owned reading rows stay available; earlier rows remain continuous.
  expect.soft(returned.scrollTop).toBe(savedVisit.top)
  expect(useAppStore.getState().activeSessionId).toBe(parent.id)
  expect(requests.some(query => ['sendAcpPrompt', 'submitManagedSubagentFollowup', 'startAcpSession', 'stopAcpSession', 'cancelManagedSubagent', 'openNativeSessionChat'].includes(query.action))).toBe(false)
})


it('preserves the open managed child reading position when its card route gains the same transcript hint', async () => {
  measureChildReadingRegion()
  const selected = { ...run, runId: 'viewport-open-route-hint-run', transcriptChatId: 'viewport-open-route-hint-transcript' }
  const response = { runId: selected.runId, title: 'Open managed reader', status: 'completed',
    messages: Array.from({ length: 65 }, (_, index) => ({ role: 'assistant', content: `Open managed message ${index + 1}` })) }
  let reads = 0
  let reply: ((value: string) => void) | undefined
  window.cefQuery = ({ request, onSuccess }) => {
    const query = JSON.parse(request); requests.push(query)
    if (query.action === 'getManagedAgentTranscript') {
      expect(query.payload).toEqual({ chatId: parent.id, transcriptChatId: selected.transcriptChatId })
      if (++reads === 2) { reply = onSuccess; return }
      onSuccess(JSON.stringify(response)); return
    }
    onSuccess(JSON.stringify(query.action === 'listManagedSubagents' ? list([selected]) : { ok: true, data: {} }))
  }
  useAppStore.setState(state => ({ messages: { ...state.messages, [parent.id]: [{ ...state.messages[parent.id][0],
    toolCalls: [{ id: 'viewport-open-route-hint-result', title: 'uam_agent_result', status: 'completed', kind: 'sub-agent', content: '',
      isSubAgent: true, subAgentId: selected.runId, subAgentTitle: 'Open managed reader' }] }] } }))
  await act(async () => root.render(<MainPanel />))
  const leafId = readChatGridLayout().activeLeafId
  await act(async () => {
    const details = host.querySelector<HTMLDetailsElement>('details.uam-subagent-group')!
    expect(details).not.toBeNull()
    details.open = true
    details.dispatchEvent(new Event('toggle', { bubbles: true }))
  })
  await act(async () => click(`[data-subagent-row="managed:${selected.runId}"]`))
  const cardRoute = subagentRouteFor(leafId, parent)
  expect(cardRoute).toMatchObject({ kind: 'managed', runId: selected.runId })
  expect(cardRoute && cardRoute.kind === 'managed' ? cardRoute.transcriptChatId : undefined).toBeUndefined()
  let pane = host.querySelector('[data-testid="subagent-pane-navigation-root"]')!
  expect(firstRendered(pane)).toBe(45)
  await revealEarlierRows(pane)
  await revealEarlierRows(pane)
  expect(firstRendered(pane)).toBe(5)
  const oldRegion = pane.querySelector<HTMLElement>('[aria-label="Stored conversation messages"]')!
  oldRegion.scrollTop = 230
  const savedVisit = commitReadingScroll(pane)
  expect(oldRegion.scrollTop).toBe(savedVisit.top)

  // Enrich the already open route without Back or switching its child, parent or leaf.
  await act(async () => openSubagentView(parent, { kind: 'managed', runId: selected.runId, transcriptChatId: selected.transcriptChatId }, leafId))
  expect(readChatGridLayout().activeLeafId).toBe(leafId)
  expect(subagentRouteFor(leafId, parent)).toMatchObject({ kind: 'managed', runId: selected.runId, transcriptChatId: selected.transcriptChatId })
  expect(host.querySelector('[data-testid="chat-pane-navigation-root"]')).toBeNull()
  expect(host.querySelector('[data-testid="subagent-pane-navigation-root"]')).not.toBeNull()
  expect(reads).toBe(2)
  expect(reply).toBeTypeOf('function')
  await act(async () => reply!(JSON.stringify(response)))
  pane = host.querySelector('[data-testid="subagent-pane-navigation-root"]')!
  const returned = pane.querySelector<HTMLElement>('[aria-label="Stored conversation messages"]')!
  expectReadingVisit(pane, savedVisit)
  expect.soft(returned.textContent).toContain('Open managed message 21')
  expect.soft(returned.textContent).toContain('Open managed message 40')
  expect(returned.querySelector('[data-message-index=\"20\"]')).not.toBeNull() // Original owned reading rows stay available; earlier rows remain continuous.
  expect.soft(returned.scrollTop).toBe(savedVisit.top)
  expect(useAppStore.getState().activeSessionId).toBe(parent.id)
  expect(requests.some(query => ['sendAcpPrompt', 'submitManagedSubagentFollowup', 'startAcpSession', 'stopAcpSession', 'cancelManagedSubagent', 'openNativeSessionChat'].includes(query.action))).toBe(false)
})

describe('R4 annotated initial UserRoot inline placement regression', () => {
  it('shows the running initial UserRoot child inline with scoped Open and Stop before any launch tool exists', async () => {
    const initialRun = {
      runId: '5ad39bc5-8084-4e5e-8bbf-d3974ecca5c7', transcriptChatId: 'chat-1792446102200-197a82', parentRunId: '',
      agentId: 'plan', taskTitle: 'Review the current change', status: 'running', providerId: 'codex-cli', modelId: 'gpt-6.1-sol', host: null,
      createdAt: '2026-10-20T09:00:00Z', startedAt: '2026-10-20T09:00:01Z', completedAt: '',
      startedAtEpochMs: 1792486801000, executionCapability: 'uam-prompt-injected', canCancel: true,
    }
    window.cefQuery = ({ request, onSuccess }) => {
      const query = JSON.parse(request); requests.push(query)
      onSuccess(JSON.stringify(query.action === 'listManagedSubagents' ? list([initialRun]) : { ok: true, data: {} }))
    }
    useAppStore.setState({
      sessions: [{ ...parent, managedSubagentCount: 1 }], activeSessionId: parent.id,
      messages: { [parent.id]: [{ managedOriginRunId: '5ad39bc5-8084-4e5e-8bbf-d3974ecca5c7', id: 'userroot-plan', sessionId: parent.id, role: 'user', content: '@plan review the current change', createdAt: new Date('2026-10-20T09:00:00Z') }] },
    })
    await act(async () => root.render(<MainPanel />))

    expect(host.textContent, 'actual UserRoot @plan parent history mounted').toContain('@plan review the current change')

    const open = host.querySelector<HTMLElement>('[aria-label="Open managed conversation for Review the current change"]')
    expect(open, 'running initial UserRoot child card must be visible without a launch tool row').not.toBeNull()
    expect(open!.closest('[data-source-chat="navigation-root"]'), 'child card must be scoped to the parent inline surface').not.toBeNull()
    const stop = host.querySelector<HTMLButtonElement>('[aria-label="Stop managed subagent Review the current change"]')
    expect(stop, 'running initial UserRoot child must expose a scoped Stop control').not.toBeNull()
    expect(stop!.closest('[data-source-chat="navigation-root"]'), 'Stop must be scoped to the parent inline surface').not.toBeNull()
    const group = open!.closest('[data-managed-origin]')!
    expect(group.previousElementSibling!.textContent).toContain('@plan review the current change')
    expect(host.querySelector('.uam-managed-panel'), 'no bottom managed panel mount').toBeNull()
    expect(requests.filter(query => query.action === 'listManagedSubagents').length, 'initial running child was read from the managed listing').toBeGreaterThan(0)
    expect(requests.some(query => ['cancelManagedSubagent', 'sendAcpPrompt', 'startAcpSession', 'openNativeSessionChat'].includes(query.action)), 'no provider or cancel action').toBe(false)
  })

})

describe('R4 authored annotation negative boundaries', () => {
  it.each(['no-run', 'foreign-run', 'foreign-scope', 'legacy', 'malformed'])('keeps %s ordinary user content without managed controls', async mode => {
    window.cefQuery = ({ request, onSuccess }) => {
      const query = JSON.parse(request); requests.push(query)
      // The current root list never includes the other root's annotated ID.
      const data = mode === 'foreign-run' ? list([{ ...run, runId: 'aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee', parentRunId: '' }]) : mode === 'foreign-scope' ? list([{ ...run, runId: '5ad39bc5-8084-4e5e-8bbf-d3974ecca5c7', parentRunId: '' }]) : list([])
      if (mode === 'foreign-scope') data.chatId = 'foreign-root'
      onSuccess(JSON.stringify(query.action === 'listManagedSubagents' ? data : { ok: true, data: {} }))
    }
    useAppStore.setState({ sessions: [{ ...parent, managedSubagentCount: 1 }], activeSessionId: parent.id,
      messages: { [parent.id]: [{ id: 'negative-userroot', sessionId: parent.id, role: 'user', content: '@plan negative boundary', createdAt: new Date('2026-10-20T09:00:00Z'),
        managedOriginRunId: mode === 'legacy' ? undefined : mode === 'malformed' ? 'bad-id' : '5ad39bc5-8084-4e5e-8bbf-d3974ecca5c7' }] } })
    await act(async () => root.render(<MainPanel />))
    expect(host.textContent).toContain('@plan negative boundary')
    expect(host.querySelector('[data-managed-origin]')).toBeNull()
    expect(host.querySelector('[data-managed-open]')).toBeNull()
    expect(host.querySelector('[aria-label^="Stop managed subagent"]')).toBeNull()
    expect(host.querySelector('.uam-managed-panel')).toBeNull()
    if (mode === 'legacy' || mode === 'malformed') expect(requests.some(q => q.action === 'listManagedSubagents')).toBe(false)
    else expect(requests.some(q => q.action === 'listManagedSubagents')).toBe(true)
    expect(requests.some(q => ['cancelManagedSubagent', 'sendAcpPrompt', 'startAcpSession', 'openNativeSessionChat'].includes(q.action))).toBe(false)
  })
})


it('R4 legacy authored row retains exact Sidebar catalogue navigation without annotation', async () => {
  useAppStore.setState({ messages: { [parent.id]: [{ id: 'legacy-user', sessionId: parent.id, role: 'user', content: '@plan legacy authored row', createdAt: new Date() }] } })
  await act(async () => root.render(<><FolderTree searchQuery="" /><MainPanel /></>))
  expect(host.textContent).toContain('@plan legacy authored row')
  expect(host.querySelector('[data-managed-origin]')).toBeNull()
  expect(host.querySelector('.uam-managed-panel')).toBeNull()
  await reveal()
  await act(async () => click('[role="group"][aria-label="Subagents of Main task"] [data-managed-run="run-selected"]'))
  expect(host.querySelector('[data-testid="subagent-pane-navigation-root"]')?.textContent).toContain('Child alone')
  expect(subagentRouteFor(readChatGridLayout().activeLeafId, parent)).toMatchObject({ kind: 'managed', runId: run.runId })
  expect(requests.some(q => ['cancelManagedSubagent', 'sendAcpPrompt', 'startAcpSession', 'openNativeSessionChat'].includes(q.action))).toBe(false)
})


it('R8 collapsed work preserves earlier assistant launch slots and exact child navigation by default', async () => {
  const id = '8509f0ba-ea06-4068-a6ed-3e1bab09bc53'
  const current = { ...run, runId: id, parentRunId: '', status: 'running', canCancel: true }
  const secondId = '36491b65-5598-42cf-bbb8-22fc00467bd6'
  const second = { ...current, runId: secondId, agentId: 'Second worker', taskTitle: 'Second worker', transcriptChatId: 'second-transcript' }
  window.cefQuery = ({ request, onSuccess }) => {
    const query = JSON.parse(request); requests.push(query)
    onSuccess(JSON.stringify(query.action === 'listManagedSubagents' ? list([current, second]) : query.action === 'getManagedAgentTranscript'
      ? { runId: id, title: 'Review worker', status: 'running', messages: [{ role: 'assistant', content: 'Exact collapsed-slot child transcript' }] }
      : { ok: true, data: {} }))
  }
  const createdAt = new Date('2026-10-20T09:00:00Z')
  const launch = { id: 'earlier-launch', title: 'agent_delegate', kind: 'mcpToolCall', status: 'completed', content: JSON.stringify({ runId: id, status: 'queued' }) }
  const laterLaunch = { ...launch, id: 'separated-launch', content: JSON.stringify({ runId: secondId, status: 'queued' }) }
  const ordinary = { id: 'ordinary-read', title: 'Inspect ordinary source', kind: 'read', status: 'completed', content: 'Ordinary trace output' }
  useAppStore.setState({ expandWorkTraces: false, collapsibleWorkSections: true, workingDisplayMode: 'compact',
    messages: { [parent.id]: [
      { id: 'collapsed-request', sessionId: parent.id, role: 'user', content: 'Request with a background worker', createdAt },
      { id: 'earlier-assistant', sessionId: parent.id, role: 'assistant', content: 'Earlier ordinary prose', thoughts: 'Earlier private reasoning', createdAt,
        blocks: [{ type: 'thought', text: 'Earlier private reasoning' }, { type: 'assistant_text', text: 'Earlier ordinary prose' }, { type: 'tool_call', toolCallId: ordinary.id }, { type: 'tool_call', toolCallId: launch.id }, { type: 'assistant_text', text: 'Do not group across this prose' }, { type: 'tool_call', toolCallId: laterLaunch.id }], toolCalls: [ordinary, launch, laterLaunch] },
      { id: 'final-assistant', sessionId: parent.id, role: 'assistant', content: 'Final parent answer', createdAt },
    ] },
  })
  await act(async () => root.render(<MainPanel />))
  act(() => input('Draft retained while child is open'))
  const slot = host.querySelector('[data-message-index="1"]')
  expect(slot, 'earlier assistant launch slot survives default section collapse').not.toBeNull()
  const open = slot!.querySelector<HTMLButtonElement>('[data-managed-open]')
  expect(open, 'current child stays visible at its actual earlier launch').not.toBeNull()
  expect(slot!.textContent).toContain('Running')
  expect(slot!.querySelector('[aria-label="Stop managed subagent Review worker"]')).not.toBeNull()
  expect(host.textContent).not.toContain('Earlier ordinary prose')
  expect(host.textContent).not.toContain('Earlier private reasoning')
  expect(host.textContent).not.toContain('Inspect ordinary source')
  expect(host.textContent).not.toContain('Do not group across this prose')
  expect(slot!.querySelectorAll('[aria-label="Subagent launch"]')).toHaveLength(2)
  expect(slot!.querySelector('[aria-label="Consecutive subagent launches"]')).toBeNull()
  expect(host.textContent).toContain('Final parent answer')
  expect(host.querySelector('.uam-managed-panel')).toBeNull()
  expect(slot!.compareDocumentPosition([...host.querySelectorAll('[data-message-index]')].find(node => node.getAttribute('data-message-index') === '2')!) & Node.DOCUMENT_POSITION_FOLLOWING).toBeTruthy()
  await act(async () => open!.click())
  expect(host.querySelector('[data-testid="subagent-pane-navigation-root"]')?.textContent).toContain('Exact collapsed-slot child transcript')
  expect(subagentRouteFor(readChatGridLayout().activeLeafId, parent)).toMatchObject({ kind: 'managed', runId: id })
  expect(requests.filter(q => q.action === 'getManagedAgentTranscript').at(-1)?.payload).toEqual({ chatId: parent.id, transcriptChatId: current.transcriptChatId })
  await act(async () => back())
  expect(host.querySelector<HTMLTextAreaElement>('[data-testid="chat-pane-navigation-root"] textarea')!.value).toBe('Draft retained while child is open')
  await act(async () => click('[aria-label="Expand work trace"]'))
  expect(host.textContent).toContain('Earlier ordinary prose')
  expect(host.textContent).toContain('Earlier private reasoning')
  expect(host.querySelectorAll('[data-launch-tool="earlier-launch"]')).toHaveLength(1)
  expect(requests.some(q => ['cancelManagedSubagent', 'sendAcpPrompt', 'startAcpSession'].includes(q.action))).toBe(false)
})
