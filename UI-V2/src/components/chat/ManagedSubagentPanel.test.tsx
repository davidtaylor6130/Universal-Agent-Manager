import codexLogo from '../../assets/provider-logos/codex.svg'
import { clearSubagentView, subagentRouteFor } from '../../utils/subagentNavigation'
import { defaultChatGridLayout, readChatGridLayout, writeChatGridLayout } from '../../utils/chatGridStorage'
import { act } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { ManagedSubagentPanel, managedElapsed, managedParentEvidenceSelector } from './ManagedSubagentPanel'
import { useAppStore } from '../../store/useAppStore'
import { listManagedSubagents, type ManagedSubagentRun } from './managedSubagents'
import { sanitizeCppChat } from '../../store/cpp/sanitizers'
import { sessionFromCppChat } from '../../store/cpp/reconcile'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
type Query = Parameters<NonNullable<Window['cefQuery']>>[0]
const run = (extra: Partial<ManagedSubagentRun> = {}): ManagedSubagentRun => ({ runId: 'run-1', parentRunId: '', agentId: 'Reviewer', transcriptChatId: 'transcript-1', status: 'running', providerId: 'codex-cli', modelId: 'model-1', host: null, createdAt: '2026-10-16 16:00:00', startedAt: '2026-10-16 16:00:00', completedAt: '', startedAtEpochMs: 100000, executionCapability: 'native', canCancel: true, taskTitle: extra.agentId ?? 'Reviewer', ...extra })
const page = (extra = {}) => ({ chatId: 'parent', controlScopeId: 'scope-1', offset: 0, totalCount: 1, nextOffset: null, runs: [run()], ...extra })
let host: HTMLDivElement
let root: Root
let previous: Window['cefQuery']
let storeSnapshot: ReturnType<typeof useAppStore.getState>
function button(text: string) { return [...host.querySelectorAll<HTMLButtonElement>('button')].find(button => button.textContent === text)! }
beforeEach(() => {
  vi.stubGlobal('ResizeObserver', class { observe() {} unobserve() {} disconnect() {} })
  host = document.createElement('div'); document.body.appendChild(host); root = createRoot(host); previous = window.cefQuery
  storeSnapshot = useAppStore.getState()
  useAppStore.setState({ acpBindingBySessionId: {}, cliBindingBySessionId: {} })
})
afterEach(() => { act(() => root.unmount()); host.remove(); window.cefQuery = previous; useAppStore.setState(storeSnapshot, true); vi.useRealTimers(); vi.restoreAllMocks(); vi.unstubAllGlobals() })

describe('ManagedSubagentPanel', () => {
  it('shows real child identity, lineage and running timing and only admitted Stop controls', async () => {
    vi.spyOn(Date, 'now').mockReturnValue(190000)
    window.cefQuery = ({ request, onSuccess }) => {
      expect(JSON.parse(request)).toMatchObject({ action: 'listManagedSubagents', payload: { chatId: 'parent', offset: 0, limit: 20 } })
      onSuccess(JSON.stringify(page({ totalCount: 2, runs: [run(), run({ runId: 'run-2', agentId: 'Tester', parentRunId: 'run-1', status: 'completed', canCancel: false })] })))
    }
    await act(async () => root.render(<ManagedSubagentPanel sourceChatId="parent" />))
    expect(host.querySelector('[aria-label="UAM-managed subagents"]')).not.toBeNull()
    expect(host.textContent).toContain('Managed subagents (2)')
    expect(host.querySelector('[data-managed-run="run-1"] .uam-subagent-card__status')?.textContent).toBe('Running')
    expect(host.querySelector('[data-managed-run="run-1"] .uam-subagent-card__time')?.textContent).toBe('1m 30s')
    expect(host.querySelector('[data-managed-run="run-2"] .uam-subagent-card__status')?.textContent).toBe('Completed')
    expect(host.querySelector('[data-managed-run="run-2"] .uam-subagent-card__time')?.getAttribute('aria-label')).toBe('Duration not recorded')
    expect(host.textContent).toContain('Subagent of Reviewer')
    expect(host.textContent).toContain('Execution host unavailable')
    expect(host.querySelectorAll('button[aria-label^="Stop managed"]')).toHaveLength(1)
  })

  it('sends the exact root/run/scope, accepts raw ok envelopes and refreshes actual stop status', async () => {
    const requests: Record<string, unknown>[] = []
    let stopped = false
    window.cefQuery = ({ request, onSuccess }) => {
      const query = JSON.parse(request); requests.push(query)
      if (query.action === 'cancelManagedSubagent') {
        expect(query.payload).toEqual({ chatId: 'parent', runId: 'run-1', controlScopeId: 'scope-1' })
        stopped = true
        onSuccess(JSON.stringify({ runId: 'run-1', ok: true }))
      } else onSuccess(JSON.stringify(page({ runs: [run(stopped ? { status: 'cancelled', canCancel: false } : {})] })))
    }
    await act(async () => root.render(<ManagedSubagentPanel sourceChatId="parent" />))
    await act(async () => { button('Stop').click(); button('Stop').click() })
    expect(requests.map(request => request.action)).toEqual(['listManagedSubagents', 'cancelManagedSubagent', 'listManagedSubagents'])
    expect(host.textContent).toContain('Stopped')
    expect(host.querySelector('button[aria-label^="Stop managed"]')).toBeNull()
  })

  it('treats a stale scope as a read-only automatic recovery gate without replaying Stop', async () => {
    vi.useFakeTimers()
    let listing = 0
    const requests: string[] = []
    window.cefQuery = ({ request, onSuccess, onFailure }) => {
      const query = JSON.parse(request); requests.push(query.action)
      if (query.action === 'listManagedSubagents') onSuccess(JSON.stringify(page({ controlScopeId: `scope-${++listing}` })))
      else onFailure(409, 'Managed subagent listing changed; refresh before stopping this child.')
    }
    await act(async () => root.render(<ManagedSubagentPanel sourceChatId="parent" />))
    await act(async () => button('Stop').click())
    expect(host.querySelector('[role="alert"]')?.textContent).toContain('Managed subagent listing changed. Its status is being read again.')
    expect(host.textContent).not.toMatch(/refresh/i)
    expect(button('Stop').disabled).toBe(true)
    expect(requests).toEqual(['listManagedSubagents', 'cancelManagedSubagent'])
    expect(host.textContent).not.toMatch(/refresh/i)
    await act(async () => { await vi.advanceTimersByTimeAsync(5000) })
    expect(button('Stop').disabled).toBe(false)
    expect(requests).toEqual(['listManagedSubagents', 'cancelManagedSubagent', 'listManagedSubagents'])
    await act(async () => { await vi.advanceTimersByTimeAsync(60000) })
    expect(requests.filter(action => action === 'cancelManagedSubagent')).toHaveLength(1)
  })

  it('routes the exact listed run into its parent pane without selecting its hidden transcript', async () => {
    const parent = { id: 'parent', name: 'Parent', folderId: null, viewMode: 'chat' as const, createdAt: new Date(), updatedAt: new Date() }
    const select = vi.fn()
    useAppStore.setState({ sessions: [parent], activeSessionId: parent.id, setActiveSession: select })
    writeChatGridLayout({ ...defaultChatGridLayout, root: { type: 'leaf', id: 'leaf-1', sessionId: parent.id } })
    const requests: string[] = []
    window.cefQuery = ({ request, onSuccess }) => { requests.push(JSON.parse(request).action); onSuccess(JSON.stringify(page())) }
    await act(async () => root.render(<ManagedSubagentPanel sourceChatId="parent" />))
    await act(async () => host.querySelector<HTMLButtonElement>('[aria-label="Open managed conversation for Reviewer"]')!.click())
    expect(subagentRouteFor(readChatGridLayout().activeLeafId, parent)).toMatchObject({ kind: 'managed', runId: 'run-1', transcriptChatId: 'transcript-1' })
    expect(select).toHaveBeenCalledExactlyOnceWith('parent')
    expect(host.querySelector('[aria-label="Managed subagent conversation"]')).toBeNull()
    expect(requests).toEqual(['listManagedSubagents'])
    act(() => clearSubagentView(readChatGridLayout().activeLeafId))
  })

  it('bounds list pages and preserves the optional lineage filter', async () => {
    const requests: unknown[] = []
    window.cefQuery = ({ request, onSuccess }) => {
      const query = JSON.parse(request); requests.push(query.payload)
      onSuccess(JSON.stringify(page(query.payload.offset ? { offset: 20, nextOffset: null, totalCount: 21, runs: [run({ runId: 'last', agentId: 'Last child' })] } : { nextOffset: 20, totalCount: 21 })))
    }
    await act(async () => root.render(<ManagedSubagentPanel sourceChatId="parent" runId="lineage-root" />))
    await act(async () => button('More children').click())
    expect(host.textContent).toContain('Last child')
    await act(async () => button('Previous children').click())
    expect(requests).toEqual([{ chatId: 'parent', runId: 'lineage-root', offset: 0, limit: 20 }, { chatId: 'parent', runId: 'lineage-root', offset: 20, limit: 20 }, { chatId: 'parent', runId: 'lineage-root', offset: 0, limit: 20 }])
  })

  it('ignores late parent and stop responses after switching the root', async () => {
    const pending: Query[] = []
    window.cefQuery = query => { pending.push(query) }
    await act(async () => root.render(<ManagedSubagentPanel sourceChatId="parent" />))
    await act(async () => pending[0].onSuccess(JSON.stringify(page())))
    await act(async () => button('Stop').click())
    await act(async () => root.render(<ManagedSubagentPanel sourceChatId="next-parent" />))
    await act(async () => pending[2].onSuccess(JSON.stringify(page({ chatId: 'next-parent', runs: [run({ agentId: 'Next parent worker' })] }))))
    await act(async () => pending[1].onSuccess(JSON.stringify({ runId: 'run-1', ok: true })))
    expect(pending).toHaveLength(3)
    expect(host.textContent).toContain('Next parent worker')
    expect(host.textContent).not.toContain('Stop requested')
  })

  it('polls active children without overlap and stops polling terminal children', async () => {
    vi.useFakeTimers()
    const pending: Query[] = []
    window.cefQuery = query => { pending.push(query) }
    await act(async () => root.render(<ManagedSubagentPanel sourceChatId="parent" />))
    await act(async () => { await vi.advanceTimersByTimeAsync(10000) })
    expect(pending).toHaveLength(1)
    await act(async () => pending[0].onSuccess(JSON.stringify(page())))
    await act(async () => { await vi.advanceTimersByTimeAsync(5000) })
    expect(pending).toHaveLength(2)
    await act(async () => { await vi.advanceTimersByTimeAsync(10000) })
    expect(pending).toHaveLength(2)
    await act(async () => pending[1].onSuccess(JSON.stringify(page({ runs: [run({ status: 'completed', canCancel: false })] }))))
    await act(async () => { await vi.advanceTimersByTimeAsync(20000) })
    expect(pending).toHaveLength(2)
  })

  it('discovers new children while the parent is actively processing even when the first page was empty', async () => {
    vi.useFakeTimers()
    useAppStore.setState({ acpBindingBySessionId: { parent: { processing: true } as never } })
    let count = 0
    window.cefQuery = ({ onSuccess }) => onSuccess(JSON.stringify(page(++count === 1 ? { totalCount: 0, runs: [] } : {})))
    await act(async () => root.render(<ManagedSubagentPanel sourceChatId="parent" />))
    expect(host.textContent).toBe('')
    await act(async () => { await vi.advanceTimersByTimeAsync(29999) })
    expect(count).toBe(1)
    await act(async () => { await vi.advanceTimersByTimeAsync(1) })
    expect(host.textContent).toContain('Reviewer')
  })

  it('captures a short parent turn completing between poll ticks', async () => {
    let count = 0
    window.cefQuery = ({ onSuccess }) => onSuccess(JSON.stringify(page(++count < 3 ? { totalCount: 0, runs: [] } : { runs: [run({ status: 'completed', canCancel: false })] })))
    await act(async () => root.render(<ManagedSubagentPanel sourceChatId="parent" />))
    await act(async () => useAppStore.setState({ acpBindingBySessionId: { parent: { processing: true } as never } }))
    await act(async () => useAppStore.setState({ acpBindingBySessionId: { parent: { processing: false } as never } }))
    expect(count).toBe(3)
    expect(host.textContent).toContain('Completed')
  })

  it('discards a late listing when the parent changes', async () => {
    const pending: Query[] = []
    window.cefQuery = query => { pending.push(query) }
    await act(async () => root.render(<ManagedSubagentPanel sourceChatId="parent" />))
    await act(async () => root.render(<ManagedSubagentPanel sourceChatId="next-parent" />))
    await act(async () => pending[1].onSuccess(JSON.stringify(page({ chatId: 'next-parent', runs: [run({ agentId: 'Current worker' })] }))))
    await act(async () => pending[0].onSuccess(JSON.stringify(page({ runs: [run({ agentId: 'STALE WORKER' })] }))))
    expect(host.textContent).toContain('Current worker')
    expect(host.textContent).not.toContain('STALE WORKER')
  })

  it('does not show an unavailable panel in an unconnected browser', async () => {
    window.cefQuery = undefined
    await act(async () => root.render(<ManagedSubagentPanel sourceChatId="parent" />))
    expect(host.textContent).toBe('')
  })

  it('rejects foreign, oversized and non-advancing metadata pages', async () => {
    for (const extra of [{ chatId: 'foreign' }, { runs: Array.from({ length: 21 }, () => run()) }, { nextOffset: 0 }]) {
      window.cefQuery = ({ onSuccess }) => onSuccess(JSON.stringify(page(extra)))
      await expect(listManagedSubagents('parent')).rejects.toThrow('invalid')
    }
  })

  it('uses only recorded epochs or explicitly zoned timestamp strings for timing', () => {
    expect(managedElapsed(run(), 190000)).toBe('1m 30s')
    expect(managedElapsed(run({ status: 'completed', completedAt: '2026-10-16 16:05:00' }), 190000)).toBe('Duration not recorded')
    expect(managedElapsed(run({ status: 'completed', completedAt: '2026-10-16 16:05:00', finishedAtEpochMs: 190000 }), 999999)).toBe('1m 30s')
    expect(managedElapsed(run({ startedAtEpochMs: 0, startedAt: '2026-10-16 16:00:00' }), 190000)).toBe('Timing unavailable')
    expect(managedElapsed(run({ startedAtEpochMs: 0, startedAt: '2026-10-16T16:00:00Z', status: 'completed', completedAt: '2026-10-16T16:01:30Z' }), 0)).toBe('1m 30s')
  })

  it('projects terminal elapsed from the real DTO epoch despite an unzoned completedAt string', async () => {
    window.cefQuery = ({ onSuccess }) => onSuccess(JSON.stringify(page({ runs: [run({ status: 'completed', canCancel: false, completedAt: '2026-10-16 16:05:00', finishedAtEpochMs: 190000 })] })))
    await act(async () => root.render(<ManagedSubagentPanel sourceChatId="parent" />))
    expect(host.querySelector('[data-managed-run="run-1"] .uam-subagent-card__status')?.textContent).toBe('Completed')
    expect(host.querySelector('[data-managed-run="run-1"] .uam-subagent-card__time')?.textContent).toBe('1m 30s')
    expect(host.textContent).not.toContain('Duration not recorded')
  })

  it('keeps focused Stop and paging enabled during a background read and accepts Stop immediately', async () => {
    vi.useFakeTimers()
    const pending: Query[] = []
    window.cefQuery = query => { pending.push(query) }
    await act(async () => root.render(<ManagedSubagentPanel sourceChatId="parent" />))
    await act(async () => pending[0].onSuccess(JSON.stringify(page({ totalCount: 21, nextOffset: 20 }))))
    const stop = button('Stop'); stop.focus()
    await act(async () => { await vi.advanceTimersByTimeAsync(5000) })
    expect(pending).toHaveLength(2)
    expect(stop.disabled).toBe(false)
    expect(document.activeElement).toBe(stop)
    expect(button('More children').disabled).toBe(false)
    await act(async () => stop.click())
    expect(JSON.parse(pending[2].request)).toMatchObject({ action: 'cancelManagedSubagent', payload: { chatId: 'parent', runId: 'run-1', controlScopeId: 'scope-1' } })
    await act(async () => pending[2].onSuccess(JSON.stringify({ ok: true, runId: 'run-1' })))
    await act(async () => pending[3].onSuccess(JSON.stringify(page({ runs: [run({ status: 'cancelled', canCancel: false })] }))))
    await act(async () => pending[1].onSuccess(JSON.stringify(page({ runs: [run({ agentId: 'STALE POLL' })] }))))
    expect(host.textContent).not.toContain('STALE POLL')
    expect(host.textContent).toContain('Stopped')
  })

  it('pages immediately during a background read and retains known parent names across pages', async () => {
    vi.useFakeTimers()
    const pending: Query[] = []
    window.cefQuery = query => { pending.push(query) }
    await act(async () => root.render(<ManagedSubagentPanel sourceChatId="parent" />))
    await act(async () => pending[0].onSuccess(JSON.stringify(page({ totalCount: 21, nextOffset: 20 }))))
    await act(async () => { await vi.advanceTimersByTimeAsync(5000) })
    await act(async () => button('More children').click())
    expect(JSON.parse(pending[2].request).payload.offset).toBe(20)
    await act(async () => pending[2].onSuccess(JSON.stringify(page({ offset: 20, totalCount: 21, runs: [run({ runId: 'child', agentId: 'Child', parentRunId: 'run-1' })] }))))
    await act(async () => pending[1].onSuccess(JSON.stringify(page())))
    expect(host.textContent).toContain('Subagent of Reviewer')
    expect(host.textContent).toContain('21–21 of 21')
  })

  it('backs off transient ledger conflicts twice and then recovers without manual refresh', async () => {
    vi.useFakeTimers()
    let calls = 0
    window.cefQuery = ({ onSuccess, onFailure }) => { if (++calls < 3) onFailure(409, 'A bounded managed subagent query is already pending; retry shortly.'); else onSuccess(JSON.stringify(page())) }
    await act(async () => root.render(<ManagedSubagentPanel sourceChatId="parent" />))
    await act(async () => { await vi.advanceTimersByTimeAsync(999) })
    expect(calls).toBe(1)
    await act(async () => { await vi.advanceTimersByTimeAsync(1) })
    expect(calls).toBe(2)
    await act(async () => { await vi.advanceTimersByTimeAsync(2000) })
    expect(calls).toBe(3)
    expect(button('Stop').disabled).toBe(false)
    expect(host.querySelector('[role="alert"]')).toBeNull()
  })

  it('keeps failed empty discovery quiet and caps automatic conflict retries', async () => {
    vi.useFakeTimers()
    let calls = 0
    window.cefQuery = ({ onFailure }) => { ++calls; onFailure(409, 'A bounded managed subagent query is already pending; retry shortly.') }
    await act(async () => root.render(<ManagedSubagentPanel sourceChatId="parent" />))
    await act(async () => { await vi.advanceTimersByTimeAsync(30000) })
    expect(calls).toBe(3)
    expect(host.querySelector('[role="alert"]')).toBeNull()
    expect(host.textContent).toContain('already pending')
    expect(host.querySelector('.uam-managed-panel__error')).toBeNull()
  })

  it('updates elapsed locally without waiting for a network refresh', async () => {
    vi.useFakeTimers(); vi.setSystemTime(190000)
    let calls = 0
    window.cefQuery = ({ onSuccess }) => { ++calls; onSuccess(JSON.stringify(page())) }
    await act(async () => root.render(<ManagedSubagentPanel sourceChatId="parent" />))
    expect(host.textContent).toContain('1m 30s')
    await act(async () => { await vi.advanceTimersByTimeAsync(1000) })
    expect(host.textContent).toContain('1m 31s')
    expect(calls).toBe(1)
  })

  it('discovers a delivered canonical managed result immediately while the parent is idle', async () => {
    let count = 0
    useAppStore.setState({ messages: {} })
    window.cefQuery = ({ onSuccess }) => onSuccess(JSON.stringify(page(++count === 1 ? { totalCount: 0, runs: [] } : {})))
    await act(async () => root.render(<ManagedSubagentPanel sourceChatId="parent" />))
    await act(async () => useAppStore.setState({ messages: { parent: [{ id: 'result', role: 'assistant', sessionId: 'parent', content: '', createdAt: new Date(), toolCalls: [{ id: 'result', title: 'uam_agent_result', kind: 'tool', status: 'completed', content: '', isSubAgent: true, subAgentId: 'run-1' }] }] } }))
    expect(count).toBe(2)
    expect(host.textContent).toContain('Reviewer')
  })

  it('discovers an idle-parent user delegation immediately on the accepted revision and retains a single optimistic follow-up read', async () => {
    vi.useFakeTimers()
    const cpp = sanitizeCppChat({ id: 'parent', title: 'Parent', createdAt: '2026-10-16T16:00:00Z', updatedAt: '2026-10-16T16:00:00Z', managedSubagentRevision: 0 })!
    const session = sessionFromCppChat(cpp, undefined, [])
    useAppStore.setState({ sessions: [session], messages: {} })
    let calls = 0
    window.cefQuery = ({ onSuccess }) => onSuccess(JSON.stringify(page(++calls < 3 ? { totalCount: 0, runs: [] } : {})))
    await act(async () => root.render(<ManagedSubagentPanel sourceChatId="parent" />))
    expect(host.textContent).toBe('')
    await act(async () => useAppStore.setState({ messages: { parent: [{ id: 'user-delegation', sessionId: 'parent', role: 'user', content: '@reviewer check this', createdAt: new Date() }] } }))
    expect(calls).toBe(2)
    expect(host.textContent).toBe('')
    const accepted = sessionFromCppChat({ ...cpp, managedSubagentRevision: 1 }, session, [])
    await act(async () => useAppStore.setState({ sessions: [accepted] }))
    expect(calls).toBe(3)
    expect(host.textContent).toContain('Running')
    expect(button('Stop').disabled).toBe(false)
    expect(useAppStore.getState().acpBindingBySessionId.parent?.processing).not.toBe(true)
    await act(async () => { await vi.advanceTimersByTimeAsync(1999) })
    expect(calls).toBe(3)
    await act(async () => { await vi.advanceTimersByTimeAsync(1) })
    expect(calls).toBe(4)
  })

  it('discovers accepted MCP work from the revision without parent processing or a user/result message', async () => {
    const session = sessionFromCppChat(sanitizeCppChat({ id: 'parent', managedSubagentRevision: 0 })!, undefined, [])
    useAppStore.setState({ sessions: [session], messages: {} })
    let calls = 0
    window.cefQuery = ({ onSuccess }) => onSuccess(JSON.stringify(page(++calls === 1 ? { totalCount: 0, runs: [] } : {})))
    await act(async () => root.render(<ManagedSubagentPanel sourceChatId="parent" />))
    await act(async () => useAppStore.setState({ sessions: [{ ...session, managedSubagentRevision: 1 }] }))
    expect(calls).toBe(2)
    expect(button('Stop').disabled).toBe(false)
  })

  it('sanitizes the revision to zero for old/invalid summaries and reconciles scalar-only metadata changes', () => {
    for (const managedSubagentRevision of [undefined, null, -1, 1.5, Infinity, '1', Number.MAX_SAFE_INTEGER + 1]) expect(sanitizeCppChat({ id: 'parent', managedSubagentRevision })?.managedSubagentRevision).toBe(0)
    const cpp = sanitizeCppChat({ id: 'parent', createdAt: '2026-10-16T16:00:00Z', updatedAt: '2026-10-16T16:00:00Z', managedSubagentRevision: 0 })!
    const first = sessionFromCppChat(cpp, undefined, [])
    expect(first.managedSubagentRevision).toBe(0)
    expect(sessionFromCppChat(cpp, first, [])).toBe(first)
    const changed = sessionFromCppChat({ ...cpp, managedSubagentRevision: 4 }, first, [])
    expect(changed).not.toBe(first)
    expect(changed.managedSubagentRevision).toBe(4)
    expect(sessionFromCppChat({ ...cpp, managedSubagentRevision: 4 }, changed, [])).toBe(changed)
  })

  it('keeps canonical/user evidence primitive and does not rescan history for streaming token replacements', () => {
    const selector = managedParentEvidenceSelector('parent')
    let toolReads = 0
    const assistant = (content: string) => ({ id: 'assistant', sessionId: 'parent', role: 'assistant' as const, content, createdAt: new Date(), get toolCalls() { ++toolReads; return [] } })
    const user = { id: 'user', sessionId: 'parent', role: 'user' as const, content: 'Task', createdAt: new Date() }
    const first = selector({ messages: { parent: [user, assistant('one')] } })
    const reads = toolReads
    expect(selector({ messages: { parent: [user, assistant('one more token')] } })).toBe(first)
    expect(toolReads).toBe(reads)
    expect(selector({ messages: { parent: [user, assistant('done'), { ...assistant(''), id: 'managed-result', toolCalls: [{ id: 'result', title: 'uam_agent_result', kind: 'tool', status: 'failed', content: '', isSubAgent: true }] }] } })).not.toBe(first)
  })

  it('preserves row focus after Stop removes its button and has no manual refresh control', async () => {
    const pending: Query[] = []
    window.cefQuery = query => { pending.push(query) }
    await act(async () => root.render(<ManagedSubagentPanel sourceChatId="parent" />))
    await act(async () => pending[0].onSuccess(JSON.stringify(page())))
    const stop = button('Stop'); stop.focus()
    await act(async () => stop.click())
    expect(document.activeElement).toBe(host.querySelector('[data-managed-run="run-1"]'))
    await act(async () => pending[1].onSuccess(JSON.stringify({ ok: true, runId: 'run-1' })))
    await act(async () => pending[2].onSuccess(JSON.stringify(page({ runs: [run({ status: 'cancelled', canCancel: false })] }))))
    expect(document.activeElement).toBe(host.querySelector('[data-managed-run="run-1"]'))
    expect(host.textContent).not.toMatch(/refresh/i)
    expect(pending).toHaveLength(3)
  })

  it('does not refetch a reopened terminal panel and labels never-started queued/cancelled work honestly', async () => {
    let calls = 0
    window.cefQuery = ({ onSuccess }) => { ++calls; onSuccess(JSON.stringify(page({ runs: [run({ status: 'completed', canCancel: false })] }))) }
    await act(async () => root.render(<ManagedSubagentPanel sourceChatId="parent" />))
    await act(async () => { const panel = host.querySelector('details')!; panel.open = false; panel.dispatchEvent(new Event('toggle', { bubbles: true })) })
    await act(async () => { const panel = host.querySelector('details')!; panel.open = true; panel.dispatchEvent(new Event('toggle', { bubbles: true })) })
    expect(calls).toBe(1)
    expect(managedElapsed(run({ status: 'queued', startedAtEpochMs: 0, startedAt: '' }), 100)).toBe('Not started')
    expect(managedElapsed(run({ status: 'cancelled', startedAtEpochMs: 0, startedAt: '' }), 100)).toBe('Not started')
  })
})


it('compact thread cards preserve owned provider, summary, live elapsed and terminal duration without changing scoped controls', async () => {
  vi.useFakeTimers(); vi.setSystemTime(190000)
  let finished = false
  const requests: string[] = []
  window.cefQuery = ({ request, onSuccess }) => { requests.push(JSON.parse(request).action); onSuccess(JSON.stringify(page({ runs: [{ ...run(), summary: 'Reviewed source.\nChecked navigation.', ...(finished ? { status: 'completed', finishedAtEpochMs: 190000, canCancel: false } : {}) }] }))) }
  await act(async () => root.render(<ManagedSubagentPanel sourceChatId="parent" />))
  const card = () => host.querySelector('[data-managed-run="run-1"]')!
  expect(card().querySelector('img')?.getAttribute('src')).toBe(codexLogo)
  expect(card().querySelector('.uam-subagent-card__summary')?.textContent).toBe('Reviewed source. Checked navigation.')
  expect(card().querySelector('.uam-subagent-card__time')?.textContent).toBe('1m 30s')
  expect(card().querySelector('.uam-working-ring')).not.toBeNull()
  expect(card().querySelector('button')).toBeNull()
  expect(host.querySelector('[data-managed-stop="run-1"]')?.closest('[data-managed-run]')).toBeNull()
  await act(async () => { await vi.advanceTimersByTimeAsync(1000) })
  expect(card().querySelector('.uam-subagent-card__time')?.textContent).toBe('1m 31s')
  finished = true
  await act(async () => { await vi.advanceTimersByTimeAsync(5000) })
  expect(card().querySelector('.uam-working-ring')).toBeNull()
  expect(card().querySelector('.uam-subagent-card__time')?.textContent).toBe('1m 30s')
  await act(async () => { await vi.advanceTimersByTimeAsync(10000) })
  expect(card().querySelector('.uam-subagent-card__time')?.textContent).toBe('1m 30s')
  expect(requests).toEqual(['listManagedSubagents', 'listManagedSubagents'])
})

it('validates optional owned summaries by UTF-8 byte bound and never substitutes task/diagnostic fields', async () => {
  window.cefQuery = ({ onSuccess }) => onSuccess(JSON.stringify(page({ runs: [{ ...run(), summary: '😀'.repeat(400), task: 'PRIVATE TASK', diagnostic: 'PRIVATE DIAGNOSTIC' }] })))
  const result = await listManagedSubagents('parent')
  const summary = result.runs[0].summary!
  expect(typeof summary).toBe('string')
  expect(new TextEncoder().encode(summary).byteLength).toBeLessThanOrEqual(1024)
  expect(summary).not.toContain('�')
  window.cefQuery = ({ onSuccess }) => onSuccess(JSON.stringify(page({ runs: [{ ...run(), providerId: '', summary: { text: 'UNVALIDATED' }, task: 'PRIVATE TASK', diagnostic: 'PRIVATE DIAGNOSTIC' }] })))
  await act(async () => root.render(<ManagedSubagentPanel sourceChatId="parent" />))
  const card = host.querySelector('[data-managed-run="run-1"]')!
  expect(card.querySelector('img')).toBeNull()
  expect(card.querySelector('.lucide-bot')).not.toBeNull()
  expect(card.textContent).toContain('No summary reported')
  expect(host.textContent).not.toContain('PRIVATE')
  expect(host.textContent).not.toContain('UNVALIDATED')
})


it('renders sidebar catalogue children as one-line robot rows instead of thread cards or detail paragraphs', async () => {
  vi.spyOn(Date, 'now').mockReturnValue(190000)
  window.cefQuery = ({ onSuccess }) => onSuccess(JSON.stringify(page({ runs: [{ ...run(), summary: 'Thread-only summary' }] })))
  await act(async () => root.render(<ManagedSubagentPanel sourceChatId="parent" catalogue />))
  expect(host.querySelector('.uam-subagent-card')).toBeNull()
  expect(host.querySelector('.uam-managed-panel--catalogue')).not.toBeNull()
  const line = host.querySelector('[aria-label="Managed children"] > li')!
  expect(line.querySelector('[data-managed-run="run-1"]')?.textContent).toContain('Reviewer')
  expect(line.querySelector('svg.lucide-bot')).not.toBeNull()
  expect(line.querySelector('img,[data-provider-id]')).toBeNull()
  expect(line.querySelector('.uam-subagent-line__status')?.textContent).toBe('1m 30s')
  expect(line.querySelectorAll('p,details,summary')).toHaveLength(0)
  expect(host.textContent).not.toMatch(/refresh|Thread-only summary|Delegated UAM runs|Execution host/i)
  expect(host.querySelector('[aria-label="Managed children"] ~ *')).toBeNull()
  expect(host.querySelector('.uam-managed-panel__details')).toBeNull()
  expect(host.querySelector('[data-managed-stop="run-1"]')).not.toBeNull()
})

it('labels saved queued and running children as last reported without live motion, elapsed growth or child polling', async () => {
  vi.useFakeTimers(); vi.setSystemTime(190000)
  for (const catalogue of [false, true]) {
    useAppStore.setState({ acpBindingBySessionId: {}, cliBindingBySessionId: {} })
    const reads: string[] = []
    window.cefQuery = ({ request, onSuccess }) => {
      const query = JSON.parse(request); reads.push(query.action)
      expect(query.action).toBe('listManagedSubagents')
      onSuccess(JSON.stringify(page({ totalCount: 2, runs: [
        { ...run(), lastReported: true, canCancel: true },
        { ...run({ runId: 'saved-queued', agentId: 'Queued worker', status: 'queued', startedAt: '', startedAtEpochMs: 0 }), lastReported: true, canCancel: false },
      ] })))
    }
    await act(async () => root.render(<ManagedSubagentPanel sourceChatId="parent" catalogue={catalogue} />))
    const rows = () => [...host.querySelectorAll('[aria-label="Managed children"] > li')]
    expect(rows()).toHaveLength(2)
    expect(rows()[0].textContent).toMatch(/last reported:\s*running/i)
    expect(rows()[1].textContent).toMatch(/last reported:\s*queued/i)
    expect(host.querySelector('.uam-working-ring')).toBeNull()
    expect(host.querySelector('[data-managed-stop]')).toBeNull()
    if (!catalogue) expect(rows()[0].querySelector('.uam-subagent-card__time')?.getAttribute('aria-label')).toMatch(/timing unavailable|duration not recorded/i)
    const savedPresentation = rows().map(row => row.textContent)
    await act(async () => { await vi.advanceTimersByTimeAsync(15000) })
    expect(rows().map(row => row.textContent)).toEqual(savedPresentation)
    expect(reads).toEqual(['listManagedSubagents'])
    // A genuinely active parent retains its slower discovery loop; saved child status grants none.
    await act(async () => useAppStore.setState({ acpBindingBySessionId: { parent: { processing: true } as never } }))
    const afterParentChange = reads.length
    await act(async () => { await vi.advanceTimersByTimeAsync(29999) })
    expect(reads).toHaveLength(afterParentChange)
    await act(async () => { await vi.advanceTimersByTimeAsync(1) })
    expect(reads).toHaveLength(afterParentChange + 1)
    expect(host.querySelector('.uam-working-ring, [data-managed-stop]')).toBeNull()
    await act(async () => root.render(<div />))
  }
})

it('validates last-reported metadata, preserves saved status and fails closed on contradictory cancellation or malformed markers', async () => {
  for (const status of ['queued', 'running', 'cancelled']) {
    window.cefQuery = ({ onSuccess }) => onSuccess(JSON.stringify(page({ runs: [{ ...run({ status }), lastReported: true, canCancel: true }] })))
    const result = await listManagedSubagents('parent')
    expect(result.runs[0]).toMatchObject({ status, lastReported: true, canCancel: false })
  }
  // Omitted and explicit false markers retain the ordinary live-row contract.
  for (const marker of [{}, { lastReported: false }]) {
    window.cefQuery = ({ onSuccess }) => onSuccess(JSON.stringify(page({ runs: [{ ...run(), ...marker }] })))
    expect((await listManagedSubagents('parent')).runs[0]).toMatchObject({ status: 'running', lastReported: false, canCancel: true })
  }
  for (const lastReported of [null, 'true', 1, {}, []]) {
    window.cefQuery = ({ onSuccess }) => onSuccess(JSON.stringify(page({ runs: [{ ...run(), lastReported }] })))
    await expect(listManagedSubagents('parent')).rejects.toThrow(/invalid/i)
  }
  window.cefQuery = ({ onSuccess }) => onSuccess(JSON.stringify(page({ runs: [{ ...run(), lastReported: 'true' }] })))
  await act(async () => root.render(<ManagedSubagentPanel sourceChatId="parent" runId="run-1" />))
  expect(host.querySelector('[role="alert"]')?.textContent).toMatch(/invalid/i)
  expect(host.querySelector('[data-managed-run], [data-managed-stop], .uam-working-ring')).toBeNull()
})

describe('sidebar catalogue without manual refresh', () => {
  const rows = () => [...host.querySelectorAll('[aria-label="Managed children"] > li')]

  it('shows an animated loading line instead of an empty flash, then only real one-line robot rows', async () => {
    const pending: Query[] = []
    window.cefQuery = query => { pending.push(query) }
    await act(async () => root.render(<ManagedSubagentPanel sourceChatId="parent" catalogue />))
    expect(host.querySelector('.uam-working-ring')).not.toBeNull()
    expect(host.textContent).toContain('Loading subagents')
    expect(host.textContent).not.toMatch(/No managed subagents|refresh/i)
    await act(async () => pending[0].onSuccess(JSON.stringify(page({ runs: [run(), run({ runId: 'run-2', agentId: 'Nested', parentRunId: 'run-1', canCancel: false })], totalCount: 2 }))))
    expect(rows()).toHaveLength(2)
    expect(rows()[1].hasAttribute('data-nested')).toBe(true)
    for (const row of rows()) { expect(row.querySelector('svg.lucide-bot')).not.toBeNull(); expect(row.querySelectorAll('p,details,summary,img')).toHaveLength(0) }
    expect(host.querySelector('[data-managed-stop="run-2"]')).toBeNull()
  })

  it('recovers from a failed read with bounded read-only backoff and never replays Stop or fabricates rows', async () => {
    vi.useFakeTimers()
    const actions: string[] = []
    let healthy = false
    window.cefQuery = ({ request, onSuccess, onFailure }) => {
      actions.push(JSON.parse(request).action)
      if (healthy) onSuccess(JSON.stringify(page())); else onFailure(500, 'bridge down')
    }
    await act(async () => root.render(<ManagedSubagentPanel sourceChatId="parent" catalogue />))
    expect(host.textContent).toContain('Subagents unavailable')
    expect(rows()).toHaveLength(0)
    for (const delay of [5000, 10000, 20000]) await act(async () => { await vi.advanceTimersByTimeAsync(delay) })
    expect(actions).toHaveLength(4)
    await act(async () => { await vi.advanceTimersByTimeAsync(40000) })
    expect(actions).toHaveLength(5)
    healthy = true
    await act(async () => { await vi.advanceTimersByTimeAsync(60000) })
    expect(actions).toHaveLength(6)
    expect(rows()).toHaveLength(1)
    expect(actions.every(action => action === 'listManagedSubagents')).toBe(true)
    expect(host.textContent).not.toContain('unavailable')
  })

  it('reads at most two parent ledgers at once when many rows become visible', async () => {
    const pending: Query[] = []
    window.cefQuery = query => { pending.push(query) }
    await act(async () => root.render(<>{['a', 'b', 'c', 'd'].map(id => <ManagedSubagentPanel key={id} sourceChatId={`parent-${id}`} catalogue />)}</>))
    expect(pending).toHaveLength(2)
    await act(async () => pending[0].onSuccess(JSON.stringify(page({ chatId: 'parent-a', runs: [], totalCount: 0 }))))
    expect(pending).toHaveLength(3)
    expect(pending.map(query => JSON.parse(query.request).payload.chatId)).toEqual(['parent-a', 'parent-b', 'parent-c'])
    // A read slot is owned by its bridge call until that settles, so leave none dangling for later tests.
    await act(async () => { for (const query of pending.slice(1)) query.onSuccess(JSON.stringify(page({ chatId: JSON.parse(query.request).payload.chatId, runs: [], totalCount: 0 }))) })
  })

  it('opens the exact listed run and sends Stop only with the exact root, run and scope; pages stay bounded', async () => {
    const requests: Array<{ action: string; payload: Record<string, unknown> }> = []
    window.cefQuery = ({ request, onSuccess }) => {
      const query = JSON.parse(request); requests.push(query)
      onSuccess(JSON.stringify(query.action === 'listManagedSubagents' ? page({ totalCount: 21, nextOffset: 20, runs: [run(), run({ runId: 'run-2', agentId: 'Read only', canCancel: false })] }) : { ok: true, runId: 'run-1' }))
    }
    const parent = { id: 'parent', name: 'Parent', viewMode: 'chat', folderId: null, createdAt: new Date(2026, 9, 16), updatedAt: new Date(2026, 9, 16) } as never
    useAppStore.setState({ sessions: [parent], activeSessionId: 'parent' })
    await act(async () => root.render(<ManagedSubagentPanel sourceChatId="parent" catalogue />))
    expect(requests[0].payload).toEqual({ chatId: 'parent', offset: 0, limit: 20 })
    expect(host.querySelector('[data-managed-stop="run-2"]')).toBeNull()
    await act(async () => host.querySelector<HTMLButtonElement>('[data-managed-run="run-2"]')!.click())
    expect(subagentRouteFor(readChatGridLayout().activeLeafId, parent)).toMatchObject({ kind: 'managed', runId: 'run-2' })
    await act(async () => host.querySelector<HTMLButtonElement>('[data-managed-stop="run-1"]')!.click())
    expect(requests.find(query => query.action === 'cancelManagedSubagent')?.payload).toEqual({ chatId: 'parent', runId: 'run-1', controlScopeId: 'scope-1' })
    await act(async () => button('More children').click())
    expect(requests.filter(query => query.action === 'listManagedSubagents').at(-1)?.payload).toMatchObject({ offset: 20, limit: 20 })
    expect(host.querySelectorAll('[aria-label="Managed subagent pages"]')).toHaveLength(1)
  })
})

describe('sidebar catalogue R3 corrections', () => {
  const parentSession = { id: 'parent', name: 'Parent', viewMode: 'chat', folderId: null, createdAt: new Date(2026, 9, 16), updatedAt: new Date(2026, 9, 16) } as never
  const rows = () => [...host.querySelectorAll('[aria-label="Managed children"] > li')]
  const listActions = (actions: string[]) => actions.filter(action => action === 'listManagedSubagents').length

  it('pauses polling and the clock while off-screen, keeps its rows, and reads once on return', async () => {
    vi.useFakeTimers()
    const actions: string[] = []
    window.cefQuery = ({ request, onSuccess }) => { actions.push(JSON.parse(request).action); onSuccess(JSON.stringify(page())) }
    await act(async () => root.render(<ManagedSubagentPanel sourceChatId="parent" catalogue />))
    expect(actions).toHaveLength(1)
    await act(async () => { await vi.advanceTimersByTimeAsync(5000) })
    expect(actions).toHaveLength(2)
    await act(async () => root.render(<ManagedSubagentPanel sourceChatId="parent" catalogue viewportVisible={false} />))
    const before = host.textContent
    await act(async () => { await vi.advanceTimersByTimeAsync(10 * 60000) })
    expect(actions, 'no poll while the row is outside the sidebar viewport').toHaveLength(2)
    expect(vi.getTimerCount()).toBe(0)
    expect(rows()).toHaveLength(1)
    expect(host.textContent).toBe(before)
    await act(async () => root.render(<ManagedSubagentPanel sourceChatId="parent" catalogue viewportVisible />))
    expect(actions).toHaveLength(3)
    expect(rows()).toHaveLength(1)
  })

  it('does not run a missed event read while off-screen and catches up when visible', async () => {
    const actions: string[] = []
    window.cefQuery = ({ request, onSuccess }) => { actions.push(JSON.parse(request).action); onSuccess(JSON.stringify(page({ runs: [run({ status: 'completed', canCancel: false })] }))) }
    useAppStore.setState({ sessions: [{ ...parentSession, managedSubagentRevision: 1 }] })
    await act(async () => root.render(<ManagedSubagentPanel sourceChatId="parent" catalogue viewportVisible={false} />))
    expect(actions).toHaveLength(1) // the first read is the mount itself
    await act(async () => useAppStore.setState({ sessions: [{ ...parentSession, managedSubagentRevision: 2 }] }))
    expect(actions).toHaveLength(1)
    await act(async () => root.render(<ManagedSubagentPanel sourceChatId="parent" catalogue viewportVisible />))
    expect(actions).toHaveLength(2)
  })

  it('announces row status through aria-describedby on a separate hidden element, not the button subtree', async () => {
    window.cefQuery = ({ onSuccess }) => onSuccess(JSON.stringify(page()))
    await act(async () => root.render(<ManagedSubagentPanel sourceChatId="parent" catalogue />))
    const open = host.querySelector<HTMLButtonElement>('[data-managed-run="run-1"]')!
    expect(open.getAttribute('aria-label')).toBe('Open managed conversation for Reviewer')
    const ids = (open.getAttribute('aria-describedby') || '').split(' ')
    expect(ids).toHaveLength(1)
    const status = host.ownerDocument.getElementById(ids[0])!
    expect(status.textContent).toMatch(/^Running · /)
    expect(status.className).toContain('sr-only')
    expect(open.contains(status)).toBe(false)
  })

  it('says a Stop was not confirmed instead of a generic stale list, and never replays it', async () => {
    vi.useFakeTimers()
    const actions: string[] = []
    window.cefQuery = ({ request, onSuccess, onFailure }) => {
      const action = JSON.parse(request).action; actions.push(action)
      if (action === 'cancelManagedSubagent') onFailure(500, 'bridge dropped the stop'); else onSuccess(JSON.stringify(page()))
    }
    await act(async () => root.render(<ManagedSubagentPanel sourceChatId="parent" catalogue />))
    await act(async () => host.querySelector<HTMLButtonElement>('[data-managed-stop="run-1"]')!.click())
    expect(host.textContent).toContain('Stop was not confirmed · checking status')
    expect(host.textContent).not.toContain('may be out of date')
    await act(async () => { await vi.advanceTimersByTimeAsync(30000) })
    expect(actions.filter(action => action === 'cancelManagedSubagent')).toHaveLength(1)
    expect(listActions(actions)).toBeGreaterThanOrEqual(2)
    expect(host.textContent).not.toContain('Stop was not confirmed') // a fresh successful read supersedes it
  })

  it('shows rows as last reported after a failed read: no Running, spinner, ticking clock, Stop, or live status handed to the child view', async () => {
    vi.useFakeTimers()
    let healthy = true
    window.cefQuery = ({ request, onSuccess, onFailure }) => {
      const action = JSON.parse(request).action
      if (action === 'listManagedSubagents' && healthy) onSuccess(JSON.stringify(page())); else onFailure(500, 'bridge down')
    }
    useAppStore.setState({ sessions: [parentSession], activeSessionId: 'parent' })
    await act(async () => root.render(<ManagedSubagentPanel sourceChatId="parent" catalogue />))
    expect(host.querySelector('.uam-working-ring')).not.toBeNull()
    healthy = false
    await act(async () => host.querySelector<HTMLButtonElement>('[data-managed-stop="run-1"]')!.click())
    expect(host.textContent).toContain('Last reported')
    expect(host.querySelector('.uam-working-ring')).toBeNull()
    expect(host.querySelector('[data-managed-stop]')).toBeNull()
    expect(host.querySelector('[aria-describedby]') && host.ownerDocument.getElementById(host.querySelector('[aria-describedby]')!.getAttribute('aria-describedby')!)!.textContent).toMatch(/^Last reported: Running/)
    expect(vi.getTimerCount()).toBeLessThanOrEqual(1) // only the read-only retry; no 1s clock
    await act(async () => host.querySelector<HTMLButtonElement>('[data-managed-run="run-1"]')!.click())
    expect(subagentRouteFor(readChatGridLayout().activeLeafId, parentSession)).toMatchObject({ kind: 'managed', runId: 'run-1', status: undefined })
  })

  it('does not duplicate a page offset when a failed More read is pressed again', async () => {
    vi.useFakeTimers()
    const offsets: number[] = []
    let failSecond = true
    window.cefQuery = ({ request, onSuccess, onFailure }) => {
      const { payload } = JSON.parse(request); offsets.push(payload.offset)
      if (payload.offset === 20 && failSecond) onFailure(500, 'bridge down')
      else onSuccess(JSON.stringify(page({ offset: payload.offset, totalCount: 40, nextOffset: payload.offset === 0 ? 20 : null })))
    }
    await act(async () => root.render(<ManagedSubagentPanel sourceChatId="parent" catalogue />))
    expect(host.querySelector('[role="group"][aria-label="Managed subagent pages"] [role="status"]')?.textContent).toBe('1–1 of 40')
    await act(async () => button('More children').click())
    expect(host.querySelector('[role="group"][aria-label="Managed subagent pages"] [role="status"]')?.textContent).toBe('1–1 of 40') // old page is still the truth
    failSecond = false
    await act(async () => button('More children').click())
    expect(host.querySelector('[role="group"][aria-label="Managed subagent pages"] [role="status"]')?.textContent).toBe('21–21 of 40')
    await act(async () => button('Previous children').click())
    expect(offsets.at(-1)).toBe(0) // one step back, not to a duplicated offset 20
  })
})


it('defers an event queued during a read when its completion arrives off-screen, then catches up once', async () => {
  const pending: Query[] = []
  window.cefQuery = query => { pending.push(query) }
  const parent = { id: 'parent', name: 'Parent', viewMode: 'chat', folderId: null, createdAt: new Date(2026, 9, 16), updatedAt: new Date(2026, 9, 16) }
  useAppStore.setState({ sessions: [{ ...parent, managedSubagentRevision: 1 }] as never })
  try {
    await act(async () => root.render(<ManagedSubagentPanel sourceChatId="parent" catalogue />))
    await act(async () => useAppStore.setState({ sessions: [{ ...parent, managedSubagentRevision: 2 }] as never }))
    await act(async () => root.render(<ManagedSubagentPanel sourceChatId="parent" catalogue viewportVisible={false} />))
    await act(async () => pending[0].onSuccess(JSON.stringify(page({ runs: [run({ status: 'completed', canCancel: false })] }))))
    expect(pending).toHaveLength(1)
    await act(async () => root.render(<ManagedSubagentPanel sourceChatId="parent" catalogue viewportVisible />))
    expect(pending).toHaveLength(2)
    await act(async () => pending[1].onSuccess(JSON.stringify(page({ runs: [run({ status: 'completed', canCancel: false })] }))))
    expect(pending).toHaveLength(2)
  } finally { await act(async () => pending.forEach(query => query.onSuccess(JSON.stringify(page({ runs: [], totalCount: 0 }))))) }
})


it.each([true, false])('collapse during an in-flight Stop preserves its outcome and reads once only after reopen (ack=%s)', async acknowledged => {
  vi.useFakeTimers()
  const actions: string[] = []
  let complete!: Query
  window.cefQuery = query => {
    const action = JSON.parse(query.request).action; actions.push(action)
    if (action === 'cancelManagedSubagent') complete = query
    else query.onSuccess(JSON.stringify(page()))
  }
  await act(async () => root.render(<ManagedSubagentPanel sourceChatId="parent" catalogue />))
  await act(async () => host.querySelector<HTMLButtonElement>('[data-managed-stop="run-1"]')!.click())
  await act(async () => root.render(<ManagedSubagentPanel sourceChatId="parent" catalogue collapsed />))
  await act(async () => acknowledged ? complete.onSuccess(JSON.stringify({ ok: true, runId: 'run-1' })) : complete.onFailure(500, 'Stop outcome unknown'))
  await act(async () => { await vi.advanceTimersByTimeAsync(60000) })
  expect(actions).toEqual(['listManagedSubagents', 'cancelManagedSubagent'])
  await act(async () => root.render(<ManagedSubagentPanel sourceChatId="parent" catalogue />))
  expect(actions).toEqual(['listManagedSubagents', 'cancelManagedSubagent', 'listManagedSubagents'])
  expect(host.textContent).not.toContain('Stopped')
})
