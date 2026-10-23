import { reconcileCppMessages } from '../../store/cpp/reconcile'
import { sanitizeCppMessage } from '../../store/cpp/sanitizers'
import claudeLogo from '../../assets/provider-logos/claude.svg'
import codexLogo from '../../assets/provider-logos/codex.svg'
import { clearSubagentView, subagentRouteFor } from '../../utils/subagentNavigation'
import { defaultChatGridLayout, readChatGridLayout, writeChatGridLayout } from '../../utils/chatGridStorage'
import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { describe, expect, it, vi } from 'vitest'
import { SubagentGroup, subagentRows, subagentState } from './SubagentGroup'
import { useAppStore } from '../../store/useAppStore'
import { SubAgentDisclosureProvider } from './ConversationWork'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
const tools = [{ id: 'spawn', title: 'Review', kind: 'sub-agent', status: 'completed', content: '', isSubAgent: true,
  subAgentId: 'one', subAgentChildren: [{ id: 'one', title: 'Code review', status: 'running' }, { id: 'two', title: 'Tests', status: 'completed' }] }]

describe('SubagentGroup', () => {
  it('retains every child and deduplicates repeated native references without inferring child state', () => {
    expect(subagentRows([...tools, { ...tools[0], id: 'wait', subAgentChildren: [{ id: 'one', status: 'completed' }] }]).map(row => [row.child.id, row.child.title, row.child.status]))
      .toEqual([['one', 'Code review', 'completed'], ['two', 'Tests', 'completed']])
    expect(subagentRows([{ ...tools[0], subAgentChildren: undefined }])[0].child.status).toBe('')
  })

  it('shows aggregate native progress and opens the selected full conversation', async () => {
    const openSubAgentSession = vi.fn().mockResolvedValue('child-chat')
    useAppStore.setState({ openSubAgentSession })
    const host = document.createElement('div'); document.body.appendChild(host)
    const root = createRoot(host)
    try {
      await act(async () => root.render(<SubagentGroup active tools={tools} sourceChatId="parent" />))
      expect(host.textContent).toContain('2 subagents1 working')
      await act(async () => { const panel = host.querySelector('details')!; panel.open = true; panel.dispatchEvent(new Event('toggle', { bubbles: true })) })
      expect(host.querySelectorAll('li')).toHaveLength(2)
      await act(async () => (host.querySelector('[aria-label="Open Tests"]') as HTMLButtonElement).click())
      expect(openSubAgentSession).toHaveBeenCalledExactlyOnceWith('parent', 'two', 'Tests', true)
    } finally { act(() => root.unmount()); host.remove() }
  })

  it('retains the group and allows retry after a failed child open', async () => {
    const openSubAgentSession = vi.fn().mockResolvedValueOnce(null).mockResolvedValueOnce('child')
    useAppStore.setState({ openSubAgentSession })
    const host = document.createElement('div'); const root = createRoot(host)
    try {
      await act(async () => root.render(<SubagentGroup active tools={tools} sourceChatId="parent" />))
      await act(async () => { const panel = host.querySelector('details')!; panel.open = true; panel.dispatchEvent(new Event('toggle', { bubbles: true })) })
      await act(async () => (host.querySelector('[aria-label="Open Tests"]') as HTMLButtonElement).click())
      expect(host.textContent).toContain('Could not open this subagent conversation')
      expect(host.querySelectorAll('li')).toHaveLength(2)
      await act(async () => (host.querySelector('[aria-label="Open Tests"]') as HTMLButtonElement).click())
      expect(host.textContent).not.toContain('Could not open this subagent conversation')
    } finally { act(() => root.unmount()); host.remove() }
  })
})

it('uses the newest native state and exposes useful labels without raw protocol spelling', () => {
  const latest = { ...tools[0], id: 'wait-done', subAgentChildren: [{ id: 'one' }] }
  expect(subagentRows([...tools, latest]).find(row => row.child.id === 'one')?.child.status).toBe('')
})

it('keeps failed provider events and current activity visible without inventing a conversation', async () => {
  const host = document.createElement('div'); const root = createRoot(host)
  try {
    await act(async () => root.render(<SubagentGroup sourceChatId="parent-failure" tools={[{ ...tools[0], id: 'failed-spawn', subAgentId: '', subAgentChildren: [], status: 'failed', content: 'The provider could not start this worker.', currentActivity: 'Checking launch permissions' }]} />))
    await act(async () => { const panel = host.querySelector('details')!; panel.open = true; panel.dispatchEvent(new Event('toggle', { bubbles: true })) })
    expect(host.textContent).toContain('Failed')
    expect(host.textContent).toContain('The provider could not start this worker.')
    expect(host.textContent).toContain('Checking launch permissions')
    expect(host.querySelector<HTMLButtonElement>('[aria-label^="Open "]')?.disabled).toBe(true)
    expect(host.querySelector('[aria-label="Tool status: Failed"]')).not.toBeNull()
  } finally { act(() => root.unmount()); host.remove() }
})

it.each([['pendingInit', 'Starting', true], ['inProgress', 'Running', true], ['notFound', 'Unavailable', false], ['errored', 'Failed', false], ['unknownNativeState', 'Status unavailable', false]] as const)('normalizes native %s without inventing activity', (status, label, active) => {
  expect(subagentState(status)).toEqual({ label, active })
})

 it('labels saved activity as last reported without announcing live work', async () => {
  const host = document.createElement('div'); const root = createRoot(host)
  try {
    await act(async () => root.render(<SubagentGroup tools={tools} sourceChatId="saved-parent" />))
    await act(async () => { const panel = host.querySelector('details')!; panel.open = true; panel.dispatchEvent(new Event('toggle', { bubbles: true })) })
    expect(host.textContent).toContain('Last reported: Running')
    expect(host.textContent).not.toContain('1 working')
  } finally { act(() => root.unmount()); host.remove() }
})

it('counts missing native identities as events rather than invented children', async () => {
  const host = document.createElement('div'); const root = createRoot(host)
  try {
    await act(async () => root.render(<SubagentGroup tools={[...tools, { ...tools[0], id: 'no-id', subAgentId: '', subAgentChildren: [], status: 'failed' }]} sourceChatId="count-events" active />))
    expect(host.querySelector('summary')?.textContent).toContain('2 subagents, 1 event')
    expect(host.querySelector('[role="status"]')?.textContent).toBe('1 working')
  } finally { act(() => root.unmount()); host.remove() }
})

it('uses the legacy tool state for previews when a native child state is unknown', async () => {
  const host = document.createElement('div'); document.body.appendChild(host); const root = createRoot(host)
  const renderHistory = vi.fn(tool => <span>{tool.status}</span>)
  try {
    await act(async () => root.render(<SubagentGroup tools={[{ ...tools[0], status: 'running', subAgentChildren: [{ id: 'one', title: 'Unknown', status: 'future-native-state' }] }]} sourceChatId="unknown-preview" renderHistory={renderHistory} />))
    await act(async () => { const panel = host.querySelector('details')!; panel.open = true; panel.dispatchEvent(new Event('toggle', { bubbles: true })) })
    const preview = host.querySelector<HTMLButtonElement>('[aria-label="Preview Unknown"]')!
    await act(async () => preview.click())
    expect(preview.getAttribute('aria-expanded')).toBe('true')
    expect(document.getElementById(preview.getAttribute('aria-controls')!)).not.toBeNull()
    expect(renderHistory).toHaveBeenLastCalledWith(expect.objectContaining({ subAgentId: 'one', status: 'running' }))
  } finally { act(() => root.unmount()); host.remove() }
})

it('routes a managed result by exact run, retains delivery status and survives group replacement without native routing', async () => {
  const snapshot = useAppStore.getState()
  const parent = { id: 'managed-parent', name: 'Parent', folderId: null, viewMode: 'chat' as const, createdAt: new Date(), updatedAt: new Date() }
  const native = vi.fn(); const select = vi.fn()
  useAppStore.setState({ sessions: [parent], activeSessionId: parent.id, openSubAgentSession: native, setActiveSession: select })
  writeChatGridLayout({ ...defaultChatGridLayout, root: { type: 'leaf', id: 'leaf-1', sessionId: parent.id } })
  const host = document.createElement('div'); const root = createRoot(host)
  const previous = window.cefQuery
  window.cefQuery = vi.fn()
  const managed = { ...tools[0], title: 'uam_agent_result', status: 'failed', content: '{"status":"cancelled"}', subAgentChildren: undefined, subAgentId: 'managed-run', subAgentTitle: 'Managed review' }
  try {
    await act(async () => root.render(<SubagentGroup sourceChatId={parent.id} tools={[managed]} renderHistory={() => <span>Native preview</span>} />))
    await act(async () => { const panel = host.querySelector('details')!; panel.open = true; panel.dispatchEvent(new Event('toggle', { bubbles: true })) })
    expect(host.textContent).toContain('Result received')
    expect(host.textContent).not.toContain('Failed')
    expect(host.querySelector('[aria-label="Preview Managed review"]')).toBeNull()
    await act(async () => host.querySelector<HTMLButtonElement>('[aria-label="Open Managed review"]')!.click())
    const leaf = readChatGridLayout().activeLeafId
    expect(subagentRouteFor(leaf, parent)).toMatchObject({ kind: 'managed', runId: 'managed-run' })
    expect(select).toHaveBeenCalledExactlyOnceWith(parent.id)
    expect(native).not.toHaveBeenCalled()
    expect(window.cefQuery).not.toHaveBeenCalled()
    expect(host.querySelector('[aria-label="Managed subagent conversation"]')).toBeNull()
    await act(async () => root.render(<SubagentGroup key="saved" sourceChatId={parent.id} tools={[managed]} />))
    expect(subagentRouteFor(leaf, parent)?.kind).toBe('managed')
    clearSubagentView(leaf)
    expect(subagentRouteFor(leaf, parent)).toBeUndefined()
  } finally { act(() => root.unmount()); host.remove(); window.cefQuery = previous; useAppStore.setState(snapshot, true) }
})

it('guards disconnected managed Open without disabling the actual companion transport', async () => {
  const host = document.createElement('div'); const root = createRoot(host)
  const previous = window.cefQuery
  const location = window.location.href
  window.cefQuery = undefined
  try {
    const render = () => root.render(<SubagentGroup sourceChatId="connection-parent" tools={[{ ...tools[0], title: 'uam_agent_result', subAgentChildren: undefined, subAgentId: 'managed-run', subAgentTitle: 'Managed review' }]} />)
    await act(async () => render())
    await act(async () => { const panel = host.querySelector('details')!; panel.open = true; panel.dispatchEvent(new Event('toggle', { bubbles: true })) })
    expect(host.querySelector<HTMLButtonElement>('[aria-label="Open Managed review"]')!.disabled).toBe(true)
    expect(host.textContent).toContain('Connect to UAM')
    window.history.replaceState({}, '', '/companion')
    await act(async () => render())
    expect(host.querySelector<HTMLButtonElement>('[aria-label="Open Managed review"]')!.disabled).toBe(false)
  } finally { act(() => root.unmount()); host.remove(); window.cefQuery = previous; window.history.replaceState({}, '', location) }
})


it('compact thread cards use actual native provider, one summary line, honest missing time and only live working rings', async () => {
  const snapshot = useAppStore.getState()
  const source = { id: 'card-parent', name: 'Parent', providerId: 'codex-cli', folderId: null, viewMode: 'chat' as const, createdAt: new Date(), updatedAt: new Date() }
  const child = { ...source, id: 'one', providerId: 'claude-cli' }
  const open = vi.fn().mockResolvedValue('child-chat')
  useAppStore.setState({ sessions: [source, child], openSubAgentSession: open })
  const host = document.createElement('div'); const root = createRoot(host)
  try {
    await act(async () => root.render(<SubagentGroup sourceChatId={source.id} active tools={[{ ...tools[0], currentActivity: 'Reading source\nChecking navigation' }]} />))
    await act(async () => { const panel = host.querySelector('details')!; panel.open = true; panel.dispatchEvent(new Event('toggle', { bubbles: true })) })
    const working = host.querySelector<HTMLButtonElement>('[data-subagent-row="one"]')!
    const done = host.querySelector<HTMLButtonElement>('[data-subagent-row="two"]')!
    expect(working.querySelector('img')?.getAttribute('src')).toBe(claudeLogo)
    expect(done.querySelector('img')?.getAttribute('src')).toBe(codexLogo)
    expect(working.querySelector('.uam-subagent-card__summary')?.textContent).toBe('Reading source Checking navigation')
    expect(working.querySelector('[aria-label="Timing unavailable"]')?.textContent).toContain('—')
    expect(working.querySelector('.uam-working-ring')).not.toBeNull()
    expect(done.querySelector('.uam-working-ring')).toBeNull()
    expect(done.querySelector('.uam-subagent-card__summary')?.textContent).toBe('No summary reported')
    expect(working.querySelector('button')).toBeNull()
    await act(async () => done.click())
    expect(open).toHaveBeenCalledExactlyOnceWith(source.id, 'two', 'Tests', true)
    await act(async () => root.render(<SubagentGroup sourceChatId={source.id} tools={tools} />))
    expect(host.textContent).toContain('Last reported: Running')
    expect(host.querySelector('.uam-working-ring')).toBeNull()
  } finally { act(() => root.unmount()); host.remove(); useAppStore.setState(snapshot, true) }
})

it('compact managed result cards parse only the exact result run for provider, reported status and summary', async () => {
  const snapshot = useAppStore.getState(); const previous = window.cefQuery
  const parent = { id: 'result-card-parent', name: 'Parent', providerId: 'gemini-cli', folderId: null, viewMode: 'chat' as const, createdAt: new Date(), updatedAt: new Date() }
  useAppStore.setState({ sessions: [parent] }); window.cefQuery = vi.fn()
  const host = document.createElement('div'); const root = createRoot(host)
  const tool = { ...tools[0], title: 'uam_agent_result', subAgentId: 'exact-card-run', subAgentTitle: 'Result card', subAgentChildren: undefined,
    content: JSON.stringify({ runId: 'exact-card-run', status: 'completed', providerId: 'claude-cli', result: 'Review written.\nAll checks complete.', transcriptChatId: 'not-a-route-source', task: 'PRIVATE TASK' }) }
  try {
    await act(async () => root.render(<SubagentGroup sourceChatId={parent.id} tools={[tool]} />))
    await act(async () => { const panel = host.querySelector('details')!; panel.open = true; panel.dispatchEvent(new Event('toggle', { bubbles: true })) })
    const card = host.querySelector('[data-subagent-row="managed:exact-card-run"]')!
    expect(card.querySelector('img')?.getAttribute('src')).toBe(claudeLogo)
    expect(card.querySelector('.uam-subagent-card__summary')?.textContent).toBe('Review written. All checks complete.')
    expect(card.textContent).toContain('Completed')
    expect(card.querySelector('.uam-working-ring')).toBeNull()
    expect(card.querySelector('[aria-label="Timing unavailable"]')).not.toBeNull()
    expect(host.textContent).not.toContain('PRIVATE TASK')
    expect(window.cefQuery).not.toHaveBeenCalled()
    await act(async () => root.render(<SubagentGroup sourceChatId={parent.id} tools={[{ ...tool, content: JSON.stringify({ runId: 'foreign-run', providerId: 'gemini-cli', result: 'Foreign result' }) }]} />))
    const unknown = host.querySelector('[data-subagent-row="managed:exact-card-run"]')!
    expect(unknown.querySelector('img')).toBeNull()
    expect(unknown.querySelector('.lucide-bot')).not.toBeNull()
    expect(unknown.textContent).toContain('Result received')
    expect(unknown.textContent).not.toContain('Foreign result')
  } finally { act(() => root.unmount()); host.remove(); window.cefQuery = previous; useAppStore.setState(snapshot, true) }
})


it('uses canonical managed result timing only from the matching run and never a parent tool duration', async () => {
  const snapshot = useAppStore.getState(); const previous = window.cefQuery
  window.cefQuery = vi.fn()
  const host = document.createElement('div'); const root = createRoot(host)
  const tool = { ...tools[0], title: 'uam_agent_result', subAgentId: 'timed-card-run', subAgentTitle: 'Timed result', subAgentChildren: undefined,
    content: JSON.stringify({ runId: 'timed-card-run', status: 'completed', providerId: 'claude-cli', result: 'Recorded result', startedAtEpochMs: 100000, finishedAtEpochMs: 190000, startedAt: '2026-10-16 16:00:00', completedAt: '2026-10-16 16:01:30' }) }
  try {
    await act(async () => root.render(<SubagentGroup sourceChatId="timing-parent" tools={[tool]} />))
    await act(async () => { const panel = host.querySelector('details')!; panel.open = true; panel.dispatchEvent(new Event('toggle', { bubbles: true })) })
    expect(host.querySelector('.uam-subagent-card__time')?.textContent).toBe('1m 30s')
    expect(host.querySelector('.uam-working-ring')).toBeNull()
    for (const startedAtEpochMs of [-1, Number.MAX_SAFE_INTEGER + 1, 0.5]) {
      await act(async () => root.render(<SubagentGroup sourceChatId="timing-parent" tools={[{ ...tool, content: JSON.stringify({ runId: 'timed-card-run', status: 'completed', startedAtEpochMs, finishedAtEpochMs: 190000 }) }]} />))
      expect(host.querySelector('[aria-label="Timing unavailable"]')).not.toBeNull()
    }
    expect(window.cefQuery).not.toHaveBeenCalled()
  } finally { act(() => root.unmount()); host.remove(); window.cefQuery = previous; useAppStore.setState(snapshot, true) }
})

it('shows native child clocks independently and never substitutes a shared parent duration', async () => {
  vi.useFakeTimers(); vi.setSystemTime(500000)
  const snapshot = useAppStore.getState()
  const host = document.createElement('div'); const root = createRoot(host)
  const native = { ...tools[0], id: 'native-own-clocks', subAgentId: 'native-clock-a',
    content: JSON.stringify({ durationMs: 999000, startedAtEpochMs: 1, finishedAtEpochMs: 999001 }),
    subAgentChildren: [
      { id: 'native-clock-a', title: 'Nine second child', status: 'completed', startedAtEpochMs: 100000, finishedAtEpochMs: 109000, durationMs: 9000 },
      { id: 'native-clock-b', title: 'Twenty one second child', status: 'failed', startedAtEpochMs: 200000, finishedAtEpochMs: 221000, durationMs: 21000 },
      { id: 'native-clock-unknown', title: 'Unknown clock child', status: 'completed' },
    ] }
  try {
    await act(async () => root.render(<SubagentGroup active sourceChatId="native-timing-cards" tools={[native]} />))
    await act(async () => { const panel = host.querySelector('details')!; panel.open = true; panel.dispatchEvent(new Event('toggle', { bubbles: true })) })
    const a = host.querySelector('[data-subagent-row="native-clock-a"]')!
    const b = host.querySelector('[data-subagent-row="native-clock-b"]')!
    const unknown = host.querySelector('[data-subagent-row="native-clock-unknown"]')!
    expect.soft(a.querySelector('.uam-subagent-card__time')?.textContent).toBe('9s')
    expect.soft(b.querySelector('.uam-subagent-card__time')?.textContent).toBe('21s')
    expect(unknown.querySelector('[aria-label="Timing unavailable"]')).not.toBeNull()
    expect(host.querySelector('.uam-working-ring')).toBeNull()
    await act(async () => { await vi.advanceTimersByTimeAsync(60000) })
    expect.soft(a.querySelector('.uam-subagent-card__time')?.textContent).toBe('9s')
    expect.soft(b.querySelector('.uam-subagent-card__time')?.textContent).toBe('21s')
  } finally { act(() => root.unmount()); host.remove(); useAppStore.setState(snapshot, true); vi.useRealTimers() }
})

it('keeps saved native running observations untimed instead of starting a clock from the parent or current time', async () => {
  vi.useFakeTimers(); vi.setSystemTime(500000)
  const host = document.createElement('div'); const root = createRoot(host)
  const native = { ...tools[0], id: 'saved-native-clock', subAgentId: 'saved-native-running', content: JSON.stringify({ durationMs: 400000 }),
    subAgentChildren: [{ id: 'saved-native-running', title: 'Last reported child', status: 'running', startedAtEpochMs: 100000 }] }
  try {
    await act(async () => root.render(<SubagentGroup sourceChatId="saved-native-clocks" tools={[native]} />))
    await act(async () => { const panel = host.querySelector('details')!; panel.open = true; panel.dispatchEvent(new Event('toggle', { bubbles: true })) })
    const card = host.querySelector('[data-subagent-row="saved-native-running"]')!
    expect(card.textContent).toContain('Last reported: Running')
    expect(card.querySelector('[aria-label="Timing unavailable"]')).not.toBeNull()
    expect(card.querySelector('.uam-working-ring')).toBeNull()
    await act(async () => { await vi.advanceTimersByTimeAsync(60000) })
    expect(card.querySelector('[aria-label="Timing unavailable"]')).not.toBeNull()
    expect(vi.getTimerCount()).toBe(0)
  } finally { act(() => root.unmount()); host.remove(); vi.useRealTimers() }
})


it('keeps native human results as summaries without decoding arbitrary JSON as managed identity', async () => {
  const snapshot = useAppStore.getState()
  const parent = { id: 'native-summary-parent', name: 'Parent', providerId: 'opencode-cli', folderId: null, viewMode: 'chat' as const, createdAt: new Date(), updatedAt: new Date() }
  const native = vi.fn().mockResolvedValue('native-child')
  useAppStore.setState({ sessions: [parent], openSubAgentSession: native })
  const host = document.createElement('div'); const root = createRoot(host)
  const tool = { ...tools[0], subAgentId: 'native-summary-child', subAgentChildren: undefined, subAgentTitle: 'Native result', content: 'Source reviewed.\nNo changes required.' }
  try {
    await act(async () => root.render(<SubagentGroup sourceChatId={parent.id} tools={[tool]} />))
    await act(async () => { const panel = host.querySelector('details')!; panel.open = true; panel.dispatchEvent(new Event('toggle', { bubbles: true })) })
    expect(host.querySelector('.uam-subagent-card__summary')?.textContent).toBe('Source reviewed. No changes required.')
    await act(async () => root.render(<SubagentGroup sourceChatId={parent.id} tools={[{ ...tool, content: JSON.stringify({ runId: 'native-summary-child', providerId: 'gemini-cli', status: 'completed', result: 'Not an attested managed result' }) }]} />))
    const card = host.querySelector<HTMLButtonElement>('[data-subagent-row="native-summary-child"]')!
    expect(card.querySelector('[data-provider-id="opencode-cli"]')).not.toBeNull()
    expect(card.querySelector('.uam-subagent-card__summary')?.textContent).toBe('No summary reported')
    expect(card.textContent).toContain('Status unavailable')
    await act(async () => card.click())
    expect(native).toHaveBeenCalledExactlyOnceWith(parent.id, 'native-summary-child', 'Native result', true)
  } finally { act(() => root.unmount()); host.remove(); useAppStore.setState(snapshot, true) }
})


it('keeps an exact canonical finished result settled even if its child-list status was earlier running', async () => {
  const host = document.createElement('div'); const root = createRoot(host)
  const managed = { ...tools[0], title: 'uam_agent_result', subAgentId: 'settled-result-run', subAgentChildren: [{ id: 'settled-result-run', title: 'Settled result', status: 'running' }],
    content: JSON.stringify({ runId: 'settled-result-run', status: 'completed', result: 'Finished review', startedAtEpochMs: 100000, finishedAtEpochMs: 190000 }) }
  try {
    await act(async () => root.render(<SubagentGroup sourceChatId="settled-parent" tools={[managed]} active />))
    await act(async () => { const panel = host.querySelector('details')!; panel.open = true; panel.dispatchEvent(new Event('toggle', { bubbles: true })) })
    expect(host.querySelector('.uam-subagent-card__status')?.textContent).toBe('Completed')
    expect(host.querySelector('.uam-working-ring')).toBeNull()
    expect(host.querySelector('.uam-subagent-card__time')?.textContent).toBe('1m 30s')
    expect(host.querySelector('summary')?.textContent).not.toContain('working')
  } finally { act(() => root.unmount()); host.remove() }
})


it.each([false, true])('reopens actual deferred saved subagent metadata without content or a fabricated summary (managed=%s)', async managed => {
  const raw = { role: 'assistant', content: '', createdAt: '2026-10-16T10:00:00Z', toolCalls: [{ id: 'deferred-card-tool', title: managed ? 'uam_agent_result' : 'Review', kind: 'sub-agent', status: 'completed',
    isSubAgent: true, subAgentId: 'deferred-card-child', subAgentTitle: 'Deferred saved worker', contentDeferred: true, contentDigest: 'saved-digest' }] }
  // Saved history pages bypass the snapshot sanitizer, exactly as the real serializer's deferred shape does.
  const messages = reconcileCppMessages('deferred-card-parent', undefined, [raw] as unknown as Parameters<typeof reconcileCppMessages>[2])
  expect(raw.toolCalls[0]).not.toHaveProperty('content')
  const host = document.createElement('div'); const root = createRoot(host)
  try {
    await act(async () => root.render(<SubagentGroup sourceChatId="deferred-card-parent" tools={raw.toolCalls as unknown as typeof tools} />))
    await act(async () => { const panel = host.querySelector('details')!; panel.open = true; panel.dispatchEvent(new Event('toggle', { bubbles: true })) })
    expect(host.querySelector('.uam-subagent-card__summary')?.textContent).toBe('No summary reported')
    expect(host.querySelector('[aria-label="Timing unavailable"]')).not.toBeNull()
    expect(host.querySelector('[aria-label="Open Deferred saved worker"]')).not.toBeNull()
    await act(async () => root.render(<SubagentGroup sourceChatId="deferred-card-parent" tools={messages[0].toolCalls!} />))
    expect(host.querySelector('.uam-subagent-card__summary')?.textContent).toBe('No summary reported')
  } finally { act(() => root.unmount()); host.remove() }
})

it.each([false, true])('presents the projected canonical saved result independently of wrapped/deferred raw content (deferred=%s)', async deferred => {
  const snapshot = useAppStore.getState(); const previous = window.cefQuery
  const parent = { id: 'projected-card-parent', name: 'Parent', providerId: 'gemini-cli', folderId: null, viewMode: 'chat' as const, createdAt: new Date(), updatedAt: new Date() }
  const select = vi.fn(); const native = vi.fn()
  useAppStore.setState({ sessions: [parent], activeSessionId: parent.id, setActiveSession: select, openSubAgentSession: native })
  writeChatGridLayout({ ...defaultChatGridLayout, root: { type: 'leaf', id: 'leaf-1', sessionId: parent.id } })
  window.cefQuery = vi.fn()
  const projection = JSON.stringify({ runId: 'projected-card-run', providerId: 'claude-cli', agentId: 'Saved reviewer', status: 'completed', result: 'Recorded saved output', startedAtEpochMs: 100000, finishedAtEpochMs: 190000, startedAt: '', completedAt: '' })
  const tool = { id: 'projected-card-tool', title: 'uam_agent_result', kind: 'sub-agent', status: 'completed', isSubAgent: true, subAgentId: 'projected-card-run', subAgentTitle: 'Saved reviewer',
    contentDeferred: deferred, contentDigest: 'saved-digest', subAgentResultContent: projection,
    ...(!deferred ? { content: 'Arguments:\n{"task":"PRIVATE TASK"}\n\nResult:\n' + JSON.stringify({ runId: 'projected-card-run', result: 'Wrong legacy summary', diagnostic: 'PRIVATE DIAGNOSTIC' }) } : {}) }
  const message = sanitizeCppMessage({ role: 'assistant', content: '', createdAt: '2026-10-16T10:00:00Z', toolCalls: [tool] })!
  const messages = reconcileCppMessages(parent.id, undefined, [message])
  const host = document.createElement('div'); const root = createRoot(host)
  try {
    await act(async () => root.render(<SubagentGroup sourceChatId={parent.id} tools={messages[0].toolCalls!} />))
    await act(async () => { const panel = host.querySelector('details')!; panel.open = true; panel.dispatchEvent(new Event('toggle', { bubbles: true })) })
    const card = host.querySelector<HTMLButtonElement>('[data-subagent-row="managed:projected-card-run"]')!
    expect(card.querySelector('img')?.getAttribute('src')).toBe(claudeLogo)
    expect(card.querySelector('.uam-subagent-card__summary')?.textContent).toBe('Recorded saved output')
    expect(card.querySelector('.uam-subagent-card__time')?.textContent).toBe('1m 30s')
    expect(host.textContent).not.toContain('PRIVATE')
    expect(host.textContent).not.toContain('Wrong legacy summary')
    const refreshed = sanitizeCppMessage({ ...message, toolCalls: [{ ...tool, subAgentResultContent: JSON.stringify({ ...JSON.parse(projection), result: 'Metadata-only refreshed output' }) }] })!
    const next = reconcileCppMessages(parent.id, messages, [refreshed])
    expect(next[0]).not.toBe(messages[0])
    await act(async () => root.render(<SubagentGroup sourceChatId={parent.id} tools={next[0].toolCalls!} />))
    expect(host.querySelector('.uam-subagent-card__summary')?.textContent).toBe('Metadata-only refreshed output')
    await act(async () => host.querySelector<HTMLButtonElement>('[data-subagent-row="managed:projected-card-run"]')!.click())
    expect(subagentRouteFor(readChatGridLayout().activeLeafId, parent)).toMatchObject({ kind: 'managed', runId: 'projected-card-run' })
    expect(select).toHaveBeenCalledExactlyOnceWith(parent.id)
    expect(native).not.toHaveBeenCalled(); expect(window.cefQuery).not.toHaveBeenCalled()
    clearSubagentView(readChatGridLayout().activeLeafId)
  } finally { act(() => root.unmount()); host.remove(); window.cefQuery = previous; useAppStore.setState(snapshot, true) }
})


it('keeps projected result preference, exact-run correlation and managed classification independent of legacy text', async () => {
  const snapshot = useAppStore.getState(); const previous = window.cefQuery
  const parent = { id: 'projection-guard-parent', name: 'Parent', providerId: 'codex-cli', folderId: null, viewMode: 'chat' as const, createdAt: new Date(), updatedAt: new Date() }
  useAppStore.setState({ sessions: [parent] }); window.cefQuery = vi.fn()
  const host = document.createElement('div'); const root = createRoot(host)
  const base = { ...tools[0], title: 'uam_agent_result', subAgentChildren: undefined, subAgentId: 'projection-guard-run', subAgentTitle: 'Projection guard',
    content: JSON.stringify({ runId: 'projection-guard-run', providerId: 'claude-cli', result: 'Stale legacy result' }) }
  try {
    for (const subAgentResultContent of [JSON.stringify({ runId: 'foreign-run', providerId: 'gemini-cli', result: 'Foreign projected output' }), '{']) {
      await act(async () => root.render(<SubagentGroup sourceChatId={parent.id} tools={[{ ...base, subAgentResultContent }]} />))
      await act(async () => { const panel = host.querySelector('details')!; panel.open = true; panel.dispatchEvent(new Event('toggle', { bubbles: true })) })
      expect(host.querySelector('.uam-subagent-card__summary')?.textContent).toBe('No summary reported')
      expect(host.querySelector('.uam-subagent-card__provider img')).toBeNull()
      expect(host.textContent).not.toContain('Stale legacy result')
      expect(host.textContent).not.toContain('Foreign projected output')
    }
    await act(async () => root.render(<SubagentGroup sourceChatId={parent.id} tools={[{ ...base, title: 'Native tool', subAgentResultContent: JSON.stringify({ runId: base.subAgentId, providerId: 'gemini-cli', result: 'Not canonical managed output' }) }]} />))
    expect(host.querySelector('.uam-subagent-card__summary')?.textContent).toBe('No summary reported')
    expect(host.querySelector('[data-provider-id="codex-cli"]')).not.toBeNull()
    expect(host.querySelector('[data-subagent-row="projection-guard-run"]')).not.toBeNull()
    expect(window.cefQuery).not.toHaveBeenCalled()
  } finally { act(() => root.unmount()); host.remove(); window.cefQuery = previous; useAppStore.setState(snapshot, true) }
})
