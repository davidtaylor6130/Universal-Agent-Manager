import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { InitialManagedSubagentGroup, InlineSubagentProvider } from './InlineSubagentGroup'
import { PersistedMessageContent, TurnTimelineContent } from './MessageBlocks'
import { useAppStore } from '../../store/useAppStore'
import type { Message, MessageBlock } from '../../types/message'
import { buildMessageFromCpp } from '../../store/cpp/reconcile'
import nativeGreenPage from '../../store/cpp/fixtures/deferredManagedLaunchPage.native-green.json'
const bridge = vi.hoisted(() => ({ connected: false, send: vi.fn() }))
vi.mock('../../ipc/cefBridge', async importOriginal => ({ ...(await importOriginal<typeof import('../../ipc/cefBridge')>()), isCefContext: () => bridge.connected, sendToCEF: bridge.send }))
;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
const ids = ['8509f0ba-ea06-4068-a6ed-3e1bab09bc53', '36491b65-5598-42cf-bbb8-22fc00467bd6', '2e9e8886-f095-414e-99a3-eee440c18fc0']
const tools = ids.map((runId, index) => ({ id: `launch-${index}`, title: 'agent_delegate', kind: 'mcpToolCall', status: 'completed', content: JSON.stringify({ runId, status: 'queued' }) }))
const call = (index: number): MessageBlock => ({ type: 'tool_call', toolCallId: tools[index].id })
const prose = (text: string): MessageBlock => ({ type: 'assistant_text', text })
const run = (index: number, status = 'failed') => ({ runId: ids[index], transcriptChatId: `child-${index}`, agentId: `Worker ${index + 1}`, taskTitle: `Worker ${index + 1}`, providerId: 'opencode-cli', modelId: 'opencode/glm-5.3-flash-free', status, canCancel: status === 'running', lastReported: false, summary: '' })
const page = (runs = [run(0), run(1), run(2)]) => ({ ok: true, data: { chatId: 'parent', controlScopeId: 'current-scope', runs, offset: 0, nextOffset: null, totalCount: runs.length } })
let host: HTMLDivElement; let root: ReturnType<typeof createRoot>
beforeEach(() => { vi.stubGlobal('ResizeObserver', class { observe() {} unobserve() {} disconnect() {} }); bridge.connected = true; bridge.send.mockReset(); bridge.send.mockResolvedValue(page()); useAppStore.setState({ expandWorkTraces: true, sessions: [] }); host = document.createElement('div'); document.body.appendChild(host); root = createRoot(host) })
afterEach(() => { act(() => root.unmount()); host.remove(); vi.useRealTimers(); vi.unstubAllGlobals() })
// Slot-placement fixtures contain only their declared tools; partial inventories are tested separately.
const message = (blocks: MessageBlock[]): Message => ({ id: 'assistant', role: 'assistant', sessionId: 'parent', content: 'Final answer', blocks, toolCalls: tools.filter(tool => blocks.some(block => block.type === 'tool_call' && block.toolCallId === tool.id)), createdAt: new Date(0) })
const showSaved = async (blocks: MessageBlock[], open = vi.fn()) => { await act(async () => root.render(<InlineSubagentProvider sourceChatId="parent" onOpenManagedRun={open}><PersistedMessageContent message={message(blocks)} sourceChatId="parent" onSelectTool={() => undefined} /></InlineSubagentProvider>)); return open }
describe('inline launch placement', () => {
  it('retains an unreferenced launch after the recorded text boundary without moving the recorded launch', async () => {
    const saved = { ...message([call(0), prose('Recorded boundary')]), toolCalls: tools.slice(0, 2) }
    await act(async () => root.render(<InlineSubagentProvider sourceChatId="parent">
      <PersistedMessageContent message={saved} sourceChatId="parent" onSelectTool={() => undefined} />
    </InlineSubagentProvider>))
    expect(host.querySelectorAll('[aria-label="Subagent launch"]')).toHaveLength(2)
    expect(host.querySelector('[aria-label="Consecutive subagent launches"]')).toBeNull()
    for (const tool of saved.toolCalls) expect(host.querySelectorAll(`[data-launch-tool="${tool.id}"]`)).toHaveLength(1)
    const text = host.textContent!
    expect(text.indexOf('Worker 1')).toBeLessThan(text.indexOf('Recorded boundary'))
    expect(text.indexOf('Recorded boundary')).toBeLessThan(text.indexOf('Worker 2'))
    expect(saved.blocks).toEqual([call(0), prose('Recorded boundary')])
  })
  it('retains consecutive launches between their prose and shows child failure rather than successful enqueue', async () => {
    await showSaved([prose('Before launches'), call(0), call(1), prose('After launches')])
    const group = host.querySelector('[aria-label="Consecutive subagent launches"]')!
    expect(group).not.toBeNull(); expect(group.querySelectorAll('[data-launch-tool]')).toHaveLength(2); expect(group.textContent).toContain('Failed'); expect(group.textContent).not.toContain('Completed')
    const content = host.textContent!; expect(content.indexOf('Before launches')).toBeLessThan(content.indexOf('Worker 1')); expect(content.indexOf('Worker 2')).toBeLessThan(content.indexOf('After launches')); expect(bridge.send).toHaveBeenCalledTimes(1)
  })
  it.each(['compact', 'verbose'] as const)('keeps separated launches separate in %s mode', async workingMode => {
    await act(async () => root.render(<InlineSubagentProvider sourceChatId="parent"><PersistedMessageContent message={message([call(0), prose('Review before another launch'), call(1)])} sourceChatId="parent" workingMode={workingMode} onSelectTool={() => undefined} /></InlineSubagentProvider>))
    expect(host.querySelectorAll('[aria-label="Subagent launch"]')).toHaveLength(2); expect(host.querySelector('[aria-label="Consecutive subagent launches"]')).toBeNull()
    const text = host.textContent!; expect(text.indexOf('Worker 1')).toBeLessThan(text.indexOf('Review before another launch')); expect(text.indexOf('Review before another launch')).toBeLessThan(text.indexOf('Worker 2'))
  })
  it('keeps an empty saved text boundary between launch slots', async () => {
    await showSaved([call(0), prose(''), call(1)])
    expect(host.querySelectorAll('[aria-label="Subagent launch"]')).toHaveLength(2)
    expect(host.querySelector('[aria-label="Consecutive subagent launches"]')).toBeNull()
  })
  it('uses the same boundaries while streaming and after saving', async () => {
    const blocks = [prose('Before'), call(0), call(1), prose('Between'), call(2), prose('After')]
    await act(async () => root.render(<InlineSubagentProvider sourceChatId="parent"><TurnTimelineContent sourceChatId="parent" active events={blocks as never} tools={tools} pendingPermission={null} pendingUserInput={null} onSelectTool={() => undefined} onResolvePermission={async () => true} onCancelTurn={() => undefined} onStopRuntime={() => undefined} /></InlineSubagentProvider>))
    expect(host.querySelectorAll('[aria-label="Consecutive subagent launches"]')).toHaveLength(1); expect(host.querySelectorAll('[aria-label="Subagent launch"]')).toHaveLength(1)
    await showSaved(blocks); expect(host.querySelectorAll('[aria-label="Consecutive subagent launches"]')).toHaveLength(1); expect(host.querySelectorAll('[aria-label="Subagent launch"]')).toHaveLength(1)
  })
  it('keeps pending native permissions outside subagent grouping', async () => {
    const native = { ...tools[0], title: 'Task', isSubAgent: true, subAgentId: 'native-child' }
    await act(async () => root.render(<TurnTimelineContent sourceChatId="parent" active events={[{ type: 'tool_call', toolCallId: native.id }]} tools={[native]} pendingPermission={{ requestId: 'permission', toolCallId: native.id, title: 'Allow worker command', kind: 'execute', status: 'pending', content: 'read source', options: [{ id: 'allow', name: 'Allow once', kind: 'decision' }] }} pendingUserInput={null} onSelectTool={() => undefined} onResolvePermission={async () => true} onCancelTurn={() => undefined} onStopRuntime={() => undefined} />))
    expect(host.textContent).toContain('Allow worker command'); expect(host.textContent).toContain('Allow once')
  })
})
describe('scoped status and actions', () => {
  it('opens only the exact listed managed run, never a native-session interpretation', async () => {
    const open = await showSaved([call(0)]); const button = host.querySelector<HTMLButtonElement>('[data-managed-open]')!; expect(button.disabled).toBe(false); act(() => button.click()); expect(open).toHaveBeenCalledWith(ids[0]); expect(open).not.toHaveBeenCalledWith('child-0')
  })
  it('does not grant navigation from a foreign parent listing', async () => {
    vi.useFakeTimers()
    bridge.send.mockResolvedValue({ ...page(), data: { ...page().data, chatId: 'other-parent' } })
    const open = await showSaved([call(0)])
    expect(host.querySelector<HTMLButtonElement>('[data-managed-open]')!.disabled).toBe(true); expect(open).not.toHaveBeenCalled()
    // The existing reader retries a changed listing after 1s, then 2s; wait for its terminal result.
    await act(async () => { await vi.advanceTimersByTimeAsync(1000) })
    expect(host.querySelector<HTMLButtonElement>('[data-managed-open]')!.disabled).toBe(true); expect(open).not.toHaveBeenCalled()
    await act(async () => { await vi.advanceTimersByTimeAsync(2000) })
    expect(bridge.send).toHaveBeenCalledTimes(3)
    expect(host.querySelector<HTMLButtonElement>('[data-managed-open]')!.disabled).toBe(true); expect(open).not.toHaveBeenCalled(); expect(host.textContent).toContain('Could not read the latest subagent status')
  })
  it('shares paginated metadata reads across separated groups', async () => {
    bridge.send.mockImplementation(({ payload }) => Promise.resolve(payload.offset === 0 ? { ok: true, data: { ...page([run(0)]).data, totalCount: 3, nextOffset: 1 } } : { ok: true, data: { ...page([run(1), run(2)]).data, offset: 1, totalCount: 3 } })); await showSaved([call(0), prose('Between'), call(2)]); expect(host.textContent).toContain('Worker 3'); expect(bridge.send).toHaveBeenCalledTimes(2)
  })
  it('rejects a changed control scope between listing pages', async () => {
    bridge.send.mockImplementation(({ payload }) => Promise.resolve(payload.offset === 0 ? { ok: true, data: { ...page([run(0)]).data, totalCount: 3, nextOffset: 1 } } : { ok: true, data: { ...page([run(1), run(2)]).data, controlScopeId: 'replacement-scope', offset: 1, totalCount: 3 } })); await showSaved([call(0), prose('Between'), call(2)]); expect(host.textContent).toContain('Could not read the latest subagent status'); expect([...host.querySelectorAll<HTMLButtonElement>('[data-managed-open]')].every(button => button.disabled)).toBe(true)
  })
  it('retains previously read failure evidence when a refresh fails', async () => {
    await showSaved([call(0)]); bridge.send.mockRejectedValue(new Error('lost connection')); expect([...host.querySelectorAll('button')].some(button => /refresh/i.test(button.textContent || ''))).toBe(false); await act(async () => document.dispatchEvent(new Event('visibilitychange'))); expect(host.textContent).toContain('Last reported: Failed'); expect(host.textContent).toContain('Could not read the latest subagent status')
  })
  it('sends Stop once and never retries an unconfirmed mutation', async () => {
    bridge.send.mockResolvedValue(page([run(0, 'running')])); await showSaved([call(0)]); bridge.send.mockResolvedValue({ ok: false, error: 'connection lost' }); const stop = host.querySelector<HTMLButtonElement>('[aria-label="Stop managed subagent Worker 1"]')!; await act(async () => stop.click()); expect(bridge.send.mock.calls.filter(([request]) => request.action === 'cancelManagedSubagent')).toEqual([[{ action: 'cancelManagedSubagent', payload: { chatId: 'parent', runId: ids[0], controlScopeId: 'current-scope' } }, false]]); expect(host.textContent).toContain('Stop was not confirmed')
  })
  it('does not request live data or enable managed navigation in a preview', async () => {
    bridge.connected = false; await showSaved([call(0)]); expect(bridge.send).not.toHaveBeenCalled(); expect(host.querySelector<HTMLButtonElement>('[data-managed-open]')!.disabled).toBe(true)
  })
})

// Adapter-shaped DTOs retain the same recorded tool anchor in both render paths.
describe('provider-shaped managed launch anchors', () => {
  it.each([
    { label: 'Codex', title: 'agent_delegate', kind: 'mcpToolCall', wrapped: true },
    { label: 'Claude', title: 'mcp__uam-control__agent_delegate', kind: 'mcp__uam-control__agent_delegate', wrapped: false },
    { label: 'Copilot SDK', title: 'agent_delegate', kind: 'other', wrapped: false },
  ])('keeps $label at the triggering tool and opens only its run', async ({ title, kind, wrapped }) => {
    const accepted = { runId: ids[0], status: 'queued' }
    const tool = { ...tools[0], title, kind, content: wrapped
      ? JSON.stringify({ type: 'mcpToolCall', server: 'uam-control', tool: 'agent_delegate', status: 'completed',
        result: { content: [{ type: 'text', text: JSON.stringify(accepted) }] } })
      : `Arguments:\n{"agentId":"build"}\n\nResult:\n${JSON.stringify(accepted)}` }
    const blocks = [prose('Before exact launch'), call(0), prose('After exact launch')]
    const open = vi.fn()
    await act(async () => root.render(<InlineSubagentProvider sourceChatId="parent" onOpenManagedRun={open}>
      <PersistedMessageContent message={{ ...message(blocks), toolCalls: [tool] }} sourceChatId="parent" onSelectTool={() => undefined} />
    </InlineSubagentProvider>))
    expect(host.querySelectorAll(`[data-launch-tool="${tool.id}"]`)).toHaveLength(1)
    expect(host.textContent!.indexOf('Before exact launch')).toBeLessThan(host.textContent!.indexOf('Worker 1'))
    expect(host.textContent!.indexOf('Worker 1')).toBeLessThan(host.textContent!.indexOf('After exact launch'))
    await act(async () => host.querySelector<HTMLButtonElement>('[data-managed-open]')!.click())
    expect(open).toHaveBeenCalledTimes(1); expect(open).toHaveBeenCalledWith(ids[0])
    await act(async () => root.render(<InlineSubagentProvider sourceChatId="parent" onOpenManagedRun={open}>
      <TurnTimelineContent sourceChatId="parent" active events={blocks as never} tools={[tool]} pendingPermission={null} pendingUserInput={null}
        onSelectTool={() => undefined} onResolvePermission={async () => true} onCancelTurn={() => undefined} onStopRuntime={() => undefined} />
    </InlineSubagentProvider>))
    expect(host.querySelectorAll(`[data-launch-tool="${tool.id}"]`)).toHaveLength(1)
    expect(host.textContent!.indexOf('Before exact launch')).toBeLessThan(host.textContent!.indexOf('Worker 1'))
    expect(host.textContent!.indexOf('Worker 1')).toBeLessThan(host.textContent!.indexOf('After exact launch'))
    expect(host.querySelector('[aria-label="Consecutive subagent launches"]')).toBeNull()
  })
})

describe('unclassified native child activity stays in its tool slot', () => {
  it.each(['Review selected source', 'wait_agent'])('retains %s preview without treating it as a fresh launch', async title => {
    const tool = { ...tools[0], title, kind: 'sub-agent', content: '', isSubAgent: true,
      subAgentId: 'native-child', subAgentChildren: [{ id: 'native-child', title: 'Native worker', status: 'running' }] }
    const blocks = [prose('Before native activity'), call(0), prose('After native activity')]
    const inspect = async () => {
      const group = host.querySelector<HTMLDetailsElement>('.uam-subagent-group')!
      expect(group).not.toBeNull()
      await act(async () => { group.open = true; group.dispatchEvent(new Event('toggle', { bubbles: true })) })
      expect(host.querySelector('[aria-label="Preview Native worker"]')).not.toBeNull()
      expect(host.querySelector('[aria-label="Subagent launch"]')).toBeNull()
      expect(host.querySelector('[aria-label="Consecutive subagent launches"]')).toBeNull()
      const text = host.textContent!
      expect(text.indexOf('Before native activity')).toBeLessThan(text.indexOf('Native worker'))
      expect(text.indexOf('Native worker')).toBeLessThan(text.indexOf('After native activity'))
    }
    await act(async () => root.render(<InlineSubagentProvider sourceChatId="parent">
      <PersistedMessageContent message={{ ...message(blocks), toolCalls: [tool] }} sourceChatId="parent" onSelectTool={() => undefined} />
    </InlineSubagentProvider>))
    await inspect()
    await act(async () => root.render(<InlineSubagentProvider sourceChatId="parent">
      <TurnTimelineContent sourceChatId="parent" active events={blocks as never} tools={[tool]} pendingPermission={null} pendingUserInput={null}
        onSelectTool={() => undefined} onResolvePermission={async () => true} onCancelTurn={() => undefined} onStopRuntime={() => undefined} />
    </InlineSubagentProvider>))
    await inspect()
    expect(bridge.send).not.toHaveBeenCalled()
  })
  it('keeps an unclassified native permission actionable in its ordinary tool slot', async () => {
    const tool = { ...tools[0], title: 'Review selected source', kind: 'sub-agent', content: '', isSubAgent: true, subAgentId: 'native-child' }
    await act(async () => root.render(<TurnTimelineContent sourceChatId="parent" active events={[call(0)]} tools={[tool]}
      pendingPermission={{ requestId: 'native-permission', toolCallId: tool.id, title: 'Allow private worker read', kind: 'read', status: 'pending', content: '', options: [{ id: 'allow', name: 'Allow once', kind: 'decision' }] }} pendingUserInput={null}
      onSelectTool={() => undefined} onResolvePermission={async () => true} onCancelTurn={() => undefined} onStopRuntime={() => undefined} />))
    expect(host.textContent).toContain('Allow private worker read'); expect(host.textContent).toContain('Allow once')
    expect(host.querySelector('.uam-subagent-group')).toBeNull()
    expect(host.querySelector('[aria-label="Subagent launch"]')).toBeNull()
  })
})

describe('managed result is not a provider-native preview', () => {
  it('keeps its exact managed row and details at the result tool in saved and live output', async () => {
    // AgentRunScheduler emits a managed result; StateSerializer supplies the bounded result card.
    const result = { runId: ids[0], status: 'completed', agentId: 'Managed worker', providerId: 'codex-cli',
      transcriptChatId: 'managed-child-transcript', result: 'Private managed result summary.' }
    const tool = { id: `managed-result-${ids[0]}`, title: 'uam_agent_result', kind: 'uam_agent_result', status: 'completed',
      content: `Arguments:\n${JSON.stringify({ runId: ids[0] })}\n\nResult:\n${JSON.stringify(result)}`,
      subAgentResultContent: JSON.stringify(result), isSubAgent: true, subAgentId: ids[0], subAgentTitle: 'Managed worker' }
    const blocks: MessageBlock[] = [prose('Before managed result'), { type: 'tool_call', toolCallId: tool.id }, prose('After managed result')]
    const details = vi.fn()
    const nativeOpen = vi.fn()
    const originalNativeOpen = useAppStore.getState().openSubAgentSession
    useAppStore.setState({ openSubAgentSession: nativeOpen })
    const inspect = async () => {
      const group = host.querySelector<HTMLDetailsElement>('.uam-subagent-group')!
      expect(group).not.toBeNull()
      await act(async () => { group.open = true; group.dispatchEvent(new Event('toggle', { bubbles: true })) })
      const card = host.querySelector<HTMLButtonElement>(`[data-subagent-row="managed:${ids[0]}"]`)!
      expect(card).not.toBeNull(); expect(card.disabled).toBe(false)
      expect(card.textContent).toContain('Completed'); expect(card.textContent).toContain('Private managed result summary.')
      expect(host.querySelector('[aria-label="Preview Managed worker"]')).toBeNull()
      expect(host.querySelector('[aria-label="Subagent launch"]')).toBeNull()
      expect(host.querySelector('[aria-label="Consecutive subagent launches"]')).toBeNull()
      const text = host.textContent!
      expect(text.indexOf('Before managed result')).toBeLessThan(text.indexOf('Managed worker'))
      expect(text.indexOf('Managed worker')).toBeLessThan(text.indexOf('After managed result'))
      await act(async () => host.querySelector<HTMLButtonElement>('[aria-label="Details for Managed worker"]')!.click())
      expect(details).toHaveBeenLastCalledWith(tool.id)
      expect(nativeOpen).not.toHaveBeenCalled(); expect(bridge.send).not.toHaveBeenCalled()
    }
    try {
      await act(async () => root.render(<InlineSubagentProvider sourceChatId="parent">
        <PersistedMessageContent message={{ ...message(blocks), toolCalls: [tool] }} sourceChatId="parent" onSelectTool={(_message, id) => details(id)} />
      </InlineSubagentProvider>))
      await inspect()
      await act(async () => root.render(<InlineSubagentProvider sourceChatId="parent">
        <TurnTimelineContent sourceChatId="parent" active events={blocks as never} tools={[tool]} pendingPermission={null} pendingUserInput={null}
          onSelectTool={details} onResolvePermission={async () => true} onCancelTurn={() => undefined} onStopRuntime={() => undefined} />
      </InlineSubagentProvider>))
      await inspect()
      expect(details).toHaveBeenCalledTimes(2)
    } finally { useAppStore.setState({ openSubAgentSession: originalNativeOpen }) }
  })
})


describe('R4.2 annotated initial read recovery', () => {
  it('shows a neutral first-read error and refreshes only scoped metadata before enabling exact child controls', async () => {
    vi.useFakeTimers()
    bridge.send.mockResolvedValue({ ok: false, error: 'listing changed' })
    const open = vi.fn()
    await act(async () => root.render(<InlineSubagentProvider sourceChatId="parent" onOpenManagedRun={open}>
      <p>Authored request remains visible</p><InitialManagedSubagentGroup runId={ids[0]} sourceChatId="parent" />
    </InlineSubagentProvider>))
    await act(async () => { await vi.advanceTimersByTimeAsync(1000); await vi.advanceTimersByTimeAsync(2000) })
    expect(bridge.send).toHaveBeenCalledTimes(3)
    expect(host.querySelector('[data-managed-open]')).toBeNull()
    expect(host.querySelector('[aria-label^="Stop managed subagent"]')).toBeNull()
    expect(open).not.toHaveBeenCalled()
    expect(host.querySelector('[role="status"]')).not.toBeNull()
    expect(host.querySelector('[role="status"]')?.textContent).toContain('Could not load linked subagent status')
    expect([...host.querySelectorAll('button')].some(button => /refresh/i.test(button.textContent || ''))).toBe(false)
    bridge.send.mockResolvedValue(page([{ ...run(0, 'running'), parentRunId: '' }]))
    await act(async () => { await vi.advanceTimersByTimeAsync(5000) })
    expect(bridge.send.mock.calls.every(([request]) => request.action === 'listManagedSubagents' && request.payload.chatId === 'parent')).toBe(true)
    expect(host.textContent).not.toContain('Could not load linked subagent status')
    expect(host.querySelector<HTMLButtonElement>('[data-managed-open]')?.disabled).toBe(false)
    expect(host.querySelector('[aria-label="Stop managed subagent Worker 1"]')).not.toBeNull()
    expect(open).not.toHaveBeenCalled()
  })
})


describe('R4.2 initial Stop focus recovery', () => {
  it('keeps focus on the exact Open card when successful Stop removes its control', async () => {
    bridge.send.mockImplementation(({ action }) => Promise.resolve(action === 'cancelManagedSubagent'
      ? { ok: true, data: { runId: ids[0] } }
      : page([{ ...run(0, bridge.send.mock.calls.some(([request]) => request.action === 'cancelManagedSubagent') ? 'cancelled' : 'running'), parentRunId: '' }])))
    await act(async () => root.render(<InlineSubagentProvider sourceChatId="parent" onOpenManagedRun={vi.fn()}>
      <InitialManagedSubagentGroup runId={ids[0]} sourceChatId="parent" />
    </InlineSubagentProvider>))
    const card = host.querySelector<HTMLButtonElement>('[data-managed-open]')!
    const stop = host.querySelector<HTMLButtonElement>('[aria-label="Stop managed subagent Worker 1"]')!
    stop.focus()
    await act(async () => stop.click())
    expect(host.querySelector('[aria-label="Stop managed subagent Worker 1"]')).toBeNull()
    expect(document.activeElement).toBe(card)
    expect(bridge.send.mock.calls.filter(([request]) => request.action === 'cancelManagedSubagent')).toHaveLength(1)
  })
})


describe('R8 first-level managed anchors', () => {
  it.each(['launch', 'live', 'initial'] as const)('rejects a nested scoped run at the %s anchor without a card or controls', async anchor => {
    bridge.send.mockResolvedValue(page([0, 1].map(index => ({ ...run(index, 'running'), parentRunId: ids[2] }))))
    const open = vi.fn(); const details = vi.fn()
    if (anchor === 'launch') await act(async () => root.render(<InlineSubagentProvider sourceChatId="parent" onOpenManagedRun={open}>
      <PersistedMessageContent message={message([call(0), call(1)])} sourceChatId="parent" onSelectTool={(_message, id) => details(id)} />
    </InlineSubagentProvider>))
    else if (anchor === 'live') await act(async () => root.render(<InlineSubagentProvider sourceChatId="parent" onOpenManagedRun={open}>
      <TurnTimelineContent sourceChatId="parent" active events={[call(0), call(1)] as never} tools={tools.slice(0, 2)} pendingPermission={null} pendingUserInput={null}
        onSelectTool={details} onResolvePermission={async () => true} onCancelTurn={() => undefined} onStopRuntime={() => undefined} />
    </InlineSubagentProvider>))
    else await act(async () => root.render(<InlineSubagentProvider sourceChatId="parent" onOpenManagedRun={open}>
      <p>Original authored request</p><InitialManagedSubagentGroup runId={ids[0]} sourceChatId="parent" />
    </InlineSubagentProvider>))
    expect(bridge.send).toHaveBeenCalledTimes(1)
    expect(host.querySelector('[data-managed-open]'), 'a nested run must not create a first-level card').toBeNull()
    expect(host.querySelector('[aria-label^="Stop managed subagent"]')).toBeNull()
    expect(open).not.toHaveBeenCalled()
    if (anchor !== 'initial') {
      const group = host.querySelector('[aria-label="Consecutive subagent launches"]')!
      expect(group.querySelectorAll(':scope > details > ul > [data-launch-tool]'), 'counted nested launches retain both ordinary tool slots').toHaveLength(2)
      expect(group.querySelector('summary')!.textContent).toContain('2 launches')
      for (const index of [0, 1]) {
        const row = group.querySelector(`[data-launch-tool="${tools[index].id}"]`)!
        const ordinary = row.querySelector<HTMLButtonElement>('[title="Open tool details"]')
        expect(ordinary, 'the real ordinary tool renderer keeps details reachable without managed authority').not.toBeNull()
        expect(ordinary!.textContent).toContain('agent_delegate')
        await act(async () => ordinary!.click())
        expect(details).toHaveBeenNthCalledWith(index + 1, tools[index].id)
      }
      expect(group.querySelector('.uam-subagent-card')).toBeNull()
    }
    expect(bridge.send.mock.calls.every(([request]) => request.action === 'listManagedSubagents')).toBe(true)
  })
})


describe('actual saved/deferred native DTO boundary', () => {
  it('renders the exact GREEN native DTO and opens only its listed run without fetching details', async () => {
    // Exact serializer output is imported byte-for-byte by the private driver.
    // This page, its blocks and its correlation are not augmented by this test.
    const raw = nativeGreenPage.messages[0]
    const exportedTool = raw.toolCalls[0]
    expect(exportedTool.managedLaunchRunId).toBe(ids[0])
    const saved = buildMessageFromCpp(nativeGreenPage.chatId, raw, nativeGreenPage.startIndex)
    expect(saved.toolCalls?.[0].content).toBe('')
    expect(saved.toolCalls?.[0].contentDeferred).toBe(true)
    const open = vi.fn()
    const details = vi.fn()
    bridge.send.mockResolvedValue({ ...page([run(0)]), data: { ...page([run(0)]).data, chatId: nativeGreenPage.chatId } })
    await act(async () => root.render(<InlineSubagentProvider sourceChatId={nativeGreenPage.chatId} onOpenManagedRun={open}>
      <PersistedMessageContent message={saved} sourceChatId={nativeGreenPage.chatId} onSelectTool={details} />
    </InlineSubagentProvider>))
    const group = host.querySelector('[aria-label="Subagent launch"]')!
    expect(group).not.toBeNull()
    expect(group.querySelector(`[data-launch-tool="${exportedTool.id}"]`)).not.toBeNull()
    expect(host.querySelectorAll('[data-launch-tool]')).toHaveLength(1)
    expect(saved.blocks).toEqual(raw.blocks)
    expect(host.textContent).toContain('Failed')
    const calls = bridge.send.mock.calls.map(([request]) => request)
    expect(calls).toHaveLength(1)
    expect(calls[0]).toMatchObject({ action: 'listManagedSubagents', payload: { chatId: nativeGreenPage.chatId } })
    expect(details).not.toHaveBeenCalled()
    await act(async () => group.querySelector<HTMLButtonElement>('[data-managed-open]')!.click())
    expect(open).toHaveBeenCalledExactlyOnceWith(ids[0])
    const launchDetails = group.querySelector<HTMLButtonElement>('[aria-label="Launch details for Worker 1"]')!
    await act(async () => launchDetails.click())
    expect(details).toHaveBeenCalledExactlyOnceWith(saved.id, exportedTool.id)
    expect(bridge.send.mock.calls.map(([request]) => request.action)).toEqual(['listManagedSubagents'])
  })
})

describe('inline launch cards without manual refresh', () => {
  const refreshButtons = () => [...host.querySelectorAll('button')].filter(button => /refresh/i.test(button.textContent || '') || /refresh/i.test(button.getAttribute('aria-label') || ''))
  it('keeps cards at their exact launch slots with no Refresh control, in healthy and failed states', async () => {
    await showSaved([prose('Before'), call(0), call(1), prose('After')])
    expect(refreshButtons()).toHaveLength(0)
    expect(host.querySelectorAll('[data-launch-tool]')).toHaveLength(2)
    bridge.send.mockRejectedValue(new Error('lost connection'))
    await act(async () => document.dispatchEvent(new Event('visibilitychange')))
    expect(host.textContent).toContain('Could not read the latest subagent status')
    expect(refreshButtons()).toHaveLength(0)
    const text = host.textContent!
    expect(text.indexOf('Before')).toBeLessThan(text.indexOf('Worker 1'))
    expect(text.indexOf('Worker 2')).toBeLessThan(text.indexOf('After'))
  })
  it('continues read-only retries after four failures and recovers on the60s capped backoff', async () => {
    vi.useFakeTimers()
    bridge.send.mockRejectedValue(new Error('down'))
    await act(async () => root.render(<InlineSubagentProvider sourceChatId="parent"><InitialManagedSubagentGroup runId={ids[0]} sourceChatId="parent" /></InlineSubagentProvider>))
    for (const delay of [5000, 10000, 20000]) await act(async () => { await vi.advanceTimersByTimeAsync(delay) })
    expect(bridge.send).toHaveBeenCalledTimes(4)
    await act(async () => { await vi.advanceTimersByTimeAsync(40000) })
    expect(bridge.send).toHaveBeenCalledTimes(5)
    expect(refreshButtons()).toHaveLength(0)
    expect(bridge.send.mock.calls.every(([request]) => request.action === 'listManagedSubagents')).toBe(true)
    bridge.send.mockResolvedValue(page([run(0, 'running')]))
    await act(async () => { await vi.advanceTimersByTimeAsync(60000) })
    expect(bridge.send).toHaveBeenCalledTimes(6)
    expect(host.querySelector<HTMLButtonElement>('[data-managed-open]')).not.toBeNull()
  })
})

describe('inline launch reads share the bounded read transport', () => {
  it('sends at most two ledger reads at once across parents, holds both slots after the readers unmount, and does not send the queued third', async () => {
    const answers: Array<() => void> = []
    bridge.send.mockImplementation(() => new Promise(resolve => { answers.push(() => resolve(page())) }))
    const parents = ['parent-a', 'parent-b', 'parent-c']
    await act(async () => root.render(<>{parents.map(id => <InlineSubagentProvider key={id} sourceChatId={id}><PersistedMessageContent message={message([call(0)])} sourceChatId={id} onSelectTool={() => undefined} /></InlineSubagentProvider>)}</>))
    expect(bridge.send).toHaveBeenCalledTimes(2)
    await act(async () => root.render(<></>))
    expect(bridge.send, 'unmounting aborts the callers but the two bridge calls still own their slots').toHaveBeenCalledTimes(2)
    await act(async () => root.render(<InlineSubagentProvider sourceChatId="parent-d"><PersistedMessageContent message={message([call(0)])} sourceChatId="parent-d" onSelectTool={() => undefined} /></InlineSubagentProvider>))
    expect(bridge.send).toHaveBeenCalledTimes(2)
    await act(async () => answers[0]())
    expect(bridge.send).toHaveBeenCalledTimes(3)
    await act(async () => { answers.forEach(answer => answer()) })
  })
})


it('clears inline unconfirmed-Stop feedback only after a fresh successful status read without claiming Stop ACK', async () => {
  vi.useFakeTimers()
  bridge.send.mockResolvedValue(page([run(0, 'running')]))
  await showSaved([call(0)])
  let answer!: (value: unknown) => void
  bridge.send.mockImplementation(({ action }) => action === 'cancelManagedSubagent' ? Promise.reject(new Error('Stop outcome unknown')) : new Promise(resolve => { answer = resolve }))
  await act(async () => host.querySelector<HTMLButtonElement>('[aria-label="Stop managed subagent Worker 1"]')!.click())
  expect(host.textContent).toContain('Stop was not confirmed')
  expect(host.textContent).not.toContain('Could not read the latest subagent status')
  await act(async () => { await vi.advanceTimersByTimeAsync(5000) })
  expect(host.textContent).toContain('Stop was not confirmed')
  await act(async () => answer(page([run(0, 'running')])))
  expect(host.textContent).not.toContain('Stop was not confirmed')
  expect(host.textContent).toContain('Running')
  expect(host.textContent).not.toContain('Stopped')
  expect(bridge.send.mock.calls.filter(([query]) => query.action === 'cancelManagedSubagent')).toHaveLength(1)
})


it('uses the work purpose as inline primary title and keeps the different agent definition secondary', async () => {
  bridge.send.mockResolvedValue(page([{ ...run(0), taskTitle: 'Audit the workspace parser', agentId: 'Definition Reader' }]))
  const open = await showSaved([prose('Before purpose'), call(0), prose('After purpose')])
  const card = host.querySelector<HTMLButtonElement>('[data-managed-open]')!
  expect(card.textContent).toContain('Audit the workspace parser')
  expect(card.textContent).not.toContain('Definition Reader')
  expect(host.querySelector('.uam-inline-subagents__meta')?.textContent).toContain('Definition Reader')
  await act(async () => card.click())
  expect(open).toHaveBeenCalledExactlyOnceWith(ids[0])
})
