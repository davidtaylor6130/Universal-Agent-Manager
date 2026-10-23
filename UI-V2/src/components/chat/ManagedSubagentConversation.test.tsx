import { act } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { ManagedSubagentConversation, isManagedSubagentTool, managedHistoryMessages } from './ManagedSubagentConversation'
import { useAppStore } from '../../store/useAppStore'
import { FileInspectionContext } from '../shared/fileInspectionContext'
import { managedTranscriptFingerprint } from './managedSubagents'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
type Query = Parameters<NonNullable<Window['cefQuery']>>[0]
let host: HTMLDivElement
let root: Root
let previous: Window['cefQuery']
const props = { sourceChatId: 'parent', runId: 'run-1', transcriptChatId: 'transcript-1', onBack: vi.fn() }
const transcript = (extra = {}) => ({ runId: 'run-1', title: 'Reviewer', status: 'completed', messages: [], ...extra })
// jsdom has no layout; tall fixed geometry keeps the continuous window from self-filling every row.
function tallTranscript() {
  const positions = new WeakMap<Element, number>()
  vi.spyOn(Element.prototype, 'scrollHeight', 'get').mockImplementation(function (this: Element) { return this.classList.contains('uam-subagent-transcript') ? 2000 : 0 })
  vi.spyOn(Element.prototype, 'clientHeight', 'get').mockImplementation(function (this: Element) { return this.classList.contains('uam-subagent-transcript') ? 300 : 0 })
  vi.spyOn(Element.prototype, 'scrollTop', 'get').mockImplementation(function (this: Element) { return positions.get(this) || 0 })
  vi.spyOn(Element.prototype, 'scrollTop', 'set').mockImplementation(function (this: Element, value: number) { positions.set(this, Math.min(Math.max(0, value), Math.max(0, this.scrollHeight - this.clientHeight))) })
}
function button(label: string) { return [...host.querySelectorAll<HTMLButtonElement>('button')].find(button => button.textContent === label || button.getAttribute('aria-label') === label)! }
beforeEach(() => { host = document.createElement('div'); document.body.appendChild(host); root = createRoot(host); previous = window.cefQuery })
afterEach(() => { act(() => root.unmount()); host.remove(); window.cefQuery = previous; vi.useRealTimers(); vi.restoreAllMocks() })

describe('ManagedSubagentConversation', () => {
  it('keeps timezone-unknown stored timestamps unknown while preserving message and plan data', () => {
    const rows = managedHistoryMessages('child', [{ role: 'assistant', content: 'Retained', createdAt: '2026-10-16T16:00:00',
      planEntries: [{ content: 'Retained plan', status: 'completed' }], stopReason: 'timeout' }])
    expect(Number.isNaN(rows[0].createdAt.getTime())).toBe(true)
    expect(rows[0]).toMatchObject({ content: 'Retained', planEntries: [{ content: 'Retained plan' }], stopReason: 'timeout' })
  })

  it('renders ordered rich stored content through canonical rows without native routing, visible sessions or execution controls', async () => {
    const native = vi.fn()
    const oldNative = useAppStore.getState().openSubAgentSession
    const oldExpand = useAppStore.getState().expandWorkTraces
    useAppStore.setState({ openSubAgentSession: native, expandWorkTraces: true })
    const sessions = useAppStore.getState().sessions
    const selection = useAppStore.getState().activeSessionId
    const actions: string[] = []
    let suppliedMessages: unknown[] = []
    window.cefQuery = ({ request, onSuccess }) => {
      const query = JSON.parse(request); actions.push(query.action)
      expect(query.payload).toEqual({ chatId: 'parent', transcriptChatId: 'transcript-1' })
      const reply = transcript({ providerId: 'codex-cli', messages: [
        // Actual serialized shape: content is always a string, attachments carry id/type/size, tools carry kind.
        { role: 'user', content: '**Please review**', createdAt: '2026-10-16T16:00:00Z', attachments: [{ id: 'brief', name: 'brief.png', type: 'file', mimeType: 'image/png', size: 4, path: '/tmp/brief.png' }] },
        { role: 'assistant', content: 'before after', thoughts: 'Inspect carefully', interrupted: true, stopReason: 'interrupt', createdAt: '2026-10-16T16:00:05Z',
          planSummary: 'Review changes', planEntries: [{ content: 'Check tests', status: 'completed' }],
          blocks: [{ type: 'assistant_text', text: '**Before**' }, { type: 'thought', text: 'Inspect carefully' }, { type: 'tool_call', toolCallId: 'build' }, { type: 'plan' }, { type: 'assistant_text', text: 'After' }, { type: 'permission_request', requestId: 'stored-permission', text: 'Recorded permission evidence.' }],
          toolCalls: [{ id: 'build', title: 'Build checks', kind: 'other', status: 'completed', content: 'ALL CHECKS PASSED' }] },
      ] })
      suppliedMessages = reply.messages
      onSuccess(JSON.stringify(reply))
    }
    try {
      await act(async () => root.render(<ManagedSubagentConversation {...props} />))
      expect(host.querySelector('strong')?.textContent).toBe('Please review')
      expect(host.textContent).toContain('Read-only stored conversation')
      // Settled plan data stays intact; like normal main history it is not promoted to an actionable Plan card.
      expect(managedHistoryMessages('transcript-1', suppliedMessages)[1].planEntries).toMatchObject([{ content: 'Check tests', status: 'completed' }])
      expect(host.querySelector('[data-testid="plan-block"]')).toBeNull()
      expect(host.textContent).toContain('Response interrupted')
      expect(host.textContent!.indexOf('Before')).toBeLessThan(host.textContent!.indexOf('Build checks'))
      expect(host.textContent!.indexOf('Build checks')).toBeLessThan(host.textContent!.indexOf('After'))
      expect(host.textContent).toContain('Inspect carefully')
      expect(host.textContent).toContain('brief.png')
      expect(host.textContent).toContain('Preview unavailable in this read-only view')
      expect(host.querySelector('.uam-chat-transcript .uam-chat-content .uam-message-list article.conversation-turn--assistant')).not.toBeNull()
      expect(host.querySelector('time.conversation-turn-start')?.getAttribute('dateTime')).toBe('2026-10-16T16:00:00.000Z')
      // Tool details open embedded content only: no deferred fetch, managed transcript or resume IPC.
      await act(async () => [...host.querySelectorAll<HTMLButtonElement>('button')].find(control => control.getAttribute('aria-label') === 'Build checks, completed' || control.title === 'Open tool details')!.click())
      expect(document.body.textContent).toContain('ALL CHECKS PASSED')
      expect(actions).toEqual(['getManagedAgentTranscript'])
      expect(native).not.toHaveBeenCalled()
      expect(useAppStore.getState().sessions).toBe(sessions)
      expect(useAppStore.getState().activeSessionId).toBe(selection)
      expect(host.querySelector('textarea,input,form')).toBeNull()
      expect([...host.querySelectorAll('button')].some(button => /resume|stop|approve|send/i.test(button.textContent || ''))).toBe(false)
      await act(async () => button('Back to parent').click())
      expect(props.onBack).toHaveBeenCalled()
    } finally { useAppStore.setState({ openSubAgentSession: oldNative, expandWorkTraces: oldExpand }) }
  })

  it('resolves the exact managed run through the bounded root-owned list', async () => {
    const requests: unknown[] = []
    window.cefQuery = ({ request, onSuccess }) => {
      const query = JSON.parse(request); requests.push(query)
      if (query.action === 'listManagedSubagents') onSuccess(JSON.stringify({ chatId: 'parent', controlScopeId: 'scope', offset: 0, nextOffset: null, totalCount: 2,
        runs: [{ runId: 'descendant', transcriptChatId: 'wrong', status: 'completed', canCancel: false }, { runId: 'run-1', transcriptChatId: 'exact', status: 'completed', canCancel: false }] }))
      else onSuccess(JSON.stringify(transcript({ messages: [{ role: 'assistant', content: 'FULL TRANSCRIPT' }] })))
    }
    await act(async () => root.render(<ManagedSubagentConversation {...props} transcriptChatId={undefined} />))
    expect(requests).toMatchObject([{ action: 'listManagedSubagents', payload: { chatId: 'parent', runId: 'run-1', offset: 0, limit: 100 } }, { action: 'getManagedAgentTranscript', payload: { chatId: 'parent', transcriptChatId: 'exact' } }])
    expect(host.textContent).toContain('FULL TRANSCRIPT')
  })

  it('rejects mismatched runs and permits explicit retry', async () => {
    let count = 0
    window.cefQuery = ({ onSuccess }) => onSuccess(JSON.stringify(transcript({ runId: ++count === 1 ? 'foreign' : 'run-1', messages: [{ role: 'assistant', content: count === 1 ? 'FOREIGN TEXT' : 'OWN TEXT' }] })))
    await act(async () => root.render(<ManagedSubagentConversation {...props} />))
    expect(host.querySelector('[role="alert"]')?.textContent).toContain('changed or is unavailable')
    expect(host.textContent).not.toContain('FOREIGN TEXT')
    await act(async () => button('Refresh conversation').click())
    expect(host.textContent).toContain('OWN TEXT')
  })

  it('discards a late response after switching parent or selected run', async () => {
    const pending: Query[] = []
    window.cefQuery = query => { pending.push(query) }
    await act(async () => root.render(<ManagedSubagentConversation {...props} />))
    await act(async () => root.render(<ManagedSubagentConversation {...props} sourceChatId="other-parent" runId="run-2" transcriptChatId="transcript-2" />))
    await act(async () => pending[1].onSuccess(JSON.stringify(transcript({ runId: 'run-2', title: 'Selected child' }))))
    await act(async () => pending[0].onSuccess(JSON.stringify(transcript({ title: 'STALE CHILD' }))))
    expect(host.textContent).toContain('Selected child')
    expect(host.textContent).not.toContain('STALE CHILD')
  })

  it('reveals the full message inventory by continuous scrolling and renders long text without silently dropping it', async () => {
    tallTranscript()
    window.cefQuery = ({ onSuccess }) => onSuccess(JSON.stringify(transcript({ messages: Array.from({ length: 45 }, (_, index) => ({ role: 'assistant', content: `Message ${index + 1}${index === 44 ? 'X'.repeat(17000) + 'FINAL SEGMENT' : ''}` })) })))
    await act(async () => root.render(<ManagedSubagentConversation {...props} />))
    expect(host.querySelectorAll('article')).toHaveLength(20)
    expect(host.textContent).toContain('FINAL SEGMENT')
    expect(host.querySelector('.uam-managed-conversation__pages, textarea')).toBeNull()
    const region = host.querySelector<HTMLDivElement>('[role="region"][aria-label="Stored conversation messages"]')!
    region.scrollTop = 0
    await act(async () => region.dispatchEvent(new Event('scroll', { bubbles: true })))
    expect(host.querySelectorAll('article')).toHaveLength(40)
    region.scrollTop = 0
    await act(async () => region.dispatchEvent(new Event('scroll', { bubbles: true })))
    expect(host.querySelectorAll('article')).toHaveLength(45)
    expect(region.querySelector('[data-message-index="0"]')?.textContent).toContain('Message 1')
    expect(host.textContent).toContain('FINAL SEGMENT')
  })

  it('polls only after the prior active read settles and stops at terminal state', async () => {
    vi.useFakeTimers()
    const pending: Query[] = []
    window.cefQuery = query => { pending.push(query) }
    await act(async () => root.render(<ManagedSubagentConversation {...props} status="running" />))
    await act(async () => { await vi.advanceTimersByTimeAsync(10000) })
    expect(pending).toHaveLength(1)
    await act(async () => pending[0].onSuccess(JSON.stringify(transcript({ status: 'running' }))))
    await act(async () => { await vi.advanceTimersByTimeAsync(5000) })
    expect(pending).toHaveLength(2)
    await act(async () => pending[1].onSuccess(JSON.stringify(transcript({ status: 'completed', messages: [{ role: 'assistant', content: 'FINAL RESULT' }] }))))
    await act(async () => { await vi.advanceTimersByTimeAsync(20000) })
    expect(pending).toHaveLength(2)
    expect(host.textContent).toContain('FINAL RESULT')
  })

  it('keeps the previous transcript when a refresh fails', async () => {
    let count = 0
    window.cefQuery = ({ onSuccess, onFailure }) => { if (++count === 1) onSuccess(JSON.stringify(transcript({ messages: [{ role: 'assistant', content: 'PRESERVED' }] }))); else onFailure(409, 'Transcript changed.') }
    await act(async () => root.render(<ManagedSubagentConversation {...props} />))
    await act(async () => button('Refresh conversation').click())
    expect(host.textContent).toContain('PRESERVED')
    expect(host.querySelector('[role="alert"]')?.textContent).toContain('previously loaded conversation')
  })

  it('retains tool-only messages and exposes every activity through the canonical work disclosure', async () => {
    const oldExpand = useAppStore.getState().expandWorkTraces
    useAppStore.setState({ expandWorkTraces: true })
    // Serialized tool-only messages still carry an empty content string.
    window.cefQuery = ({ onSuccess }) => onSuccess(JSON.stringify(transcript({ messages: [{ role: 'assistant', content: '', toolCalls: Array.from({ length: 65 }, (_, index) => ({ id: `tool-${index}`, title: `Tool ${index}`, kind: 'other', status: 'completed', content: `OUTPUT ${index}` })) }] })))
    try {
      await act(async () => root.render(<ManagedSubagentConversation {...props} />))
      expect(host.textContent).not.toContain('could not be read')
      const rows = [...host.querySelectorAll<HTMLButtonElement>('button')].filter(control => /^Tool [0-9]+, completed$/.test(control.getAttribute('aria-label') ?? ''))
      expect(rows).toHaveLength(65)
      await act(async () => rows.at(-1)!.click())
      expect(document.body.textContent).toContain('OUTPUT 64')
    } finally { useAppStore.setState({ expandWorkTraces: oldExpand }) }
  })

  it('keeps an unreadable stored row visible instead of dropping it', async () => {
    window.cefQuery = ({ onSuccess }) => onSuccess(JSON.stringify(transcript({ messages: [{ role: 'assistant', toolCalls: [] }, { role: 'assistant', content: 'Readable' }] })))
    await act(async () => root.render(<ManagedSubagentConversation {...props} />))
    expect(host.textContent).toContain('This stored message could not be read.')
    expect(host.textContent).toContain('Readable')
  })

  it('times out a stalled read, allows retry and ignores its eventual late response', async () => {
    vi.useFakeTimers()
    const pending: Query[] = []
    window.cefQuery = query => { pending.push(query) }
    await act(async () => root.render(<ManagedSubagentConversation {...props} />))
    await act(async () => { await vi.advanceTimersByTimeAsync(30000) })
    expect(host.querySelector('[role="alert"]')?.textContent).toContain('timed out')
    await act(async () => button('Refresh conversation').click())
    await act(async () => pending[1].onSuccess(JSON.stringify(transcript({ title: 'Retried conversation' }))))
    await act(async () => pending[0].onSuccess(JSON.stringify(transcript({ title: 'STALE TIMEOUT RESPONSE' }))))
    expect(host.textContent).toContain('Retried conversation')
    expect(host.textContent).not.toContain('STALE TIMEOUT RESPONSE')
  })

  it('classifies managed results independently of native-looking IDs', () => {
    expect(isManagedSubagentTool({ title: 'uam_agent_result' })).toBe(true)
    expect(isManagedSubagentTool({ title: 'spawn_agent' })).toBe(false)
  })

  it('focuses the heading on entry and keeps Refresh focused and enabled throughout background reads', async () => {
    // jsdom has no ResizeObserver; real CEF and the shared tooltip use this browser API.
    vi.stubGlobal('ResizeObserver', class { observe() {} unobserve() {} disconnect() {} })
    try {
      vi.useFakeTimers()
      const pending: Query[] = []
      window.cefQuery = query => { pending.push(query) }
      await act(async () => root.render(<ManagedSubagentConversation {...props} />))
      await act(async () => pending[0].onSuccess(JSON.stringify(transcript({ status: 'running', messages: [{ role: 'assistant', content: 'Active' }] }))))
      expect(document.activeElement).toBe(host.querySelector('h3'))
      const refresh = button('Refresh conversation')
      await act(async () => refresh.focus())
      expect(refresh.isConnected).toBe(true)
      expect(refresh.disabled).toBe(false)
      expect(document.activeElement).toBe(refresh)
      await act(async () => { await vi.advanceTimersByTimeAsync(5000) })
      expect(button('Refresh conversation')).toBe(refresh)
      expect(refresh.isConnected).toBe(true)
      expect(refresh.disabled).toBe(false)
      expect(document.activeElement).toBe(refresh)
      expect(host.querySelector('[role="status"]')?.textContent).toBe('Running')
      await act(async () => refresh.click())
      expect(pending).toHaveLength(3)
      await act(async () => pending[2].onSuccess(JSON.stringify(transcript({ title: 'Refreshed child' }))))
      await act(async () => pending[1].onSuccess(JSON.stringify(transcript({ title: 'STALE BACKGROUND' }))))
      expect(host.textContent).toContain('Refreshed child')
      expect(host.textContent).not.toContain('STALE BACKGROUND')
    } finally {
      vi.unstubAllGlobals()
    }
  })

  it('recovers transient read conflicts with bounded backoff and re-resolves a stale transcript reference once', async () => {
    vi.useFakeTimers()
    const actions: Array<{ action: string; payload: { transcriptChatId?: string } }> = []
    let conflicts = 0
    window.cefQuery = ({ request, onSuccess, onFailure }) => {
      const query = JSON.parse(request); actions.push(query)
      if (query.action === 'listManagedSubagents') onSuccess(JSON.stringify({ chatId: 'parent', controlScopeId: 'scope', offset: 0, totalCount: 1, nextOffset: null, runs: [{ runId: 'run-1', transcriptChatId: 'rebound', status: 'running', canCancel: true }] }))
      else if (query.payload.transcriptChatId === 'transcript-1') onFailure(404, 'Managed agent transcript is outside this chat or no longer exists.')
      else if (++conflicts < 3) onFailure(409, 'Managed agent transcript history is already loading.')
      else onSuccess(JSON.stringify(transcript({ messages: [{ role: 'assistant', content: 'REBOUND HISTORY' }] })))
    }
    await act(async () => root.render(<ManagedSubagentConversation {...props} />))
    await act(async () => { await vi.advanceTimersByTimeAsync(3000) })
    expect(actions.map(query => query.action)).toEqual(['getManagedAgentTranscript', 'listManagedSubagents', 'getManagedAgentTranscript', 'getManagedAgentTranscript', 'getManagedAgentTranscript'])
    expect(actions.slice(2).every(query => query.payload.transcriptChatId === 'rebound')).toBe(true)
    expect(host.textContent).toContain('REBOUND HISTORY')
  })

  it('keeps revealed earlier rows, signals changed off-tail content and resumes following after Show latest', async () => {
    tallTranscript()
    const messages = (count: number) => Array.from({ length: count }, (_, index) => ({ role: 'assistant', content: `Stored message ${index + 1}` }))
    let count = 0
    window.cefQuery = ({ onSuccess }) => onSuccess(JSON.stringify(transcript({ messages: messages(++count < 3 ? 41 : count === 3 ? 42 : 61) })))
    await act(async () => root.render(<ManagedSubagentConversation {...props} />))
    const region = host.querySelector<HTMLDivElement>('[role="region"][aria-label="Stored conversation messages"]')!
    expect(region.scrollTop).toBe(1700)
    region.scrollTop = 100
    await act(async () => region.dispatchEvent(new Event('scroll', { bubbles: true })))
    expect(region.querySelector('[data-message-index="1"]')).not.toBeNull()
    expect(region.scrollTop).toBe(100) // revealed rows are anchored, not jumped to
    await act(async () => button('Refresh conversation').click())
    expect(host.textContent).not.toContain('New activity · Show latest')
    expect(region.querySelector('[data-message-index="1"]')).not.toBeNull()
    await act(async () => button('Refresh conversation').click())
    expect(host.textContent).toContain('New activity · Show latest')
    expect(region.scrollTop).toBe(100) // the appended row renders below the reader without moving it
    expect(region.querySelector('[data-message-index="1"]')).not.toBeNull()
    await act(async () => button('New activity · Show latest').click())
    expect(region.querySelector('[data-message-index="41"]')?.textContent).toContain('Stored message 42')
    expect(region.scrollTop).toBe(1700)
    await act(async () => button('Refresh conversation').click())
    expect(region.querySelector('[data-message-index="60"]')?.textContent).toContain('Stored message 61')
    expect(host.textContent).not.toContain('New activity · Show latest')
  })

  it('does not inherit parent file inspection or load file-linked images from stored child Markdown', async () => {
    const inspectParent = vi.fn()
    window.cefQuery = ({ onSuccess }) => onSuccess(JSON.stringify(transcript({ messages: [{ role: 'assistant', content: '[child file](file:///private/child.txt) ![child image](file:///private/child.png) `src/child.cpp`' }] })))
    await act(async () => root.render(<FileInspectionContext.Provider value={inspectParent}><ManagedSubagentConversation {...props} /></FileInspectionContext.Provider>))
    expect(host.textContent).toContain('child file')
    expect(host.querySelector('img,a[href^="file:"],button[title="Inspect file"]')).toBeNull()
    expect(inspectParent).not.toHaveBeenCalled()
  })

  it('initializes the tail from measured geometry and never writes scroll position for unchanged poll payloads', async () => {
    vi.useFakeTimers()
    let height = 1000
    const positions = new WeakMap<Element, number>()
    let scrollWrites = 0
    vi.spyOn(Element.prototype, 'scrollHeight', 'get').mockImplementation(function () { return this.classList.contains('uam-subagent-transcript') ? height : 0 })
    vi.spyOn(Element.prototype, 'clientHeight', 'get').mockImplementation(function () { return this.classList.contains('uam-subagent-transcript') ? 300 : 0 })
    vi.spyOn(Element.prototype, 'scrollTop', 'get').mockImplementation(function () { return positions.get(this) || 0 })
    vi.spyOn(Element.prototype, 'scrollTop', 'set').mockImplementation(function (value) { ++scrollWrites; positions.set(this, Math.min(Math.max(0, value), Math.max(0, this.scrollHeight - this.clientHeight))) })
    let version = 'first'
    window.cefQuery = ({ onSuccess }) => onSuccess(JSON.stringify(transcript({ status: 'running', messages: [{ role: 'assistant', content: version }] })))
    await act(async () => root.render(<ManagedSubagentConversation {...props} />))
    const region = host.querySelector<HTMLDivElement>('[role="region"][aria-label="Stored conversation messages"]')!
    expect(region.scrollTop).toBe(700)
    const initialWrites = scrollWrites
    await act(async () => { await vi.advanceTimersByTimeAsync(5000) })
    expect(region.scrollTop).toBe(700)
    expect(scrollWrites).toBe(initialWrites)
    region.scrollTop = 200
    await act(async () => region.dispatchEvent(new Event('scroll', { bubbles: true })))
    version = 'changed'; height = 1400
    await act(async () => { await vi.advanceTimersByTimeAsync(5000) })
    expect(region.scrollTop).toBe(200)
    expect(host.textContent).toContain('New activity · Show latest')
    await act(async () => button('New activity · Show latest').click())
    expect(region.scrollTop).toBe(1100)
    version = 'changed again'; height = 1600
    await act(async () => { await vi.advanceTimersByTimeAsync(5000) })
    expect(region.scrollTop).toBe(1300)
  })

  it.each([false, true])('keeps a mounted newest-window reader on its rows through poll growth unless following the tail (followTail=%s)', async followTail => {
    vi.useFakeTimers()
    const positions = new WeakMap<Element, number>()
    vi.spyOn(Element.prototype, 'scrollHeight', 'get').mockImplementation(function () { return this.classList.contains('uam-subagent-transcript') ? 1400 : 0 })
    vi.spyOn(Element.prototype, 'clientHeight', 'get').mockImplementation(function () { return this.classList.contains('uam-subagent-transcript') ? 300 : 0 })
    vi.spyOn(Element.prototype, 'scrollTop', 'get').mockImplementation(function () { return positions.get(this) || 0 })
    vi.spyOn(Element.prototype, 'scrollTop', 'set').mockImplementation(function (value) { positions.set(this, Math.min(Math.max(0, value), Math.max(0, this.scrollHeight - this.clientHeight))) })
    let count = 80
    const reads: Query[] = []
    window.cefQuery = query => {
      const wire = JSON.parse(query.request)
      expect(wire).toMatchObject({ action: 'getManagedAgentTranscript', payload: { chatId: props.sourceChatId, transcriptChatId: props.transcriptChatId } })
      reads.push(query)
      query.onSuccess(JSON.stringify(transcript({ status: 'running', messages: Array.from({ length: count }, (_, index) => ({ role: 'assistant', content: 'Mounted stored message ' + (index + 1) })) })))
    }
    await act(async () => root.render(<ManagedSubagentConversation {...props} />))
    expect(host.textContent).toContain('Mounted stored message 61')
    expect(host.textContent).not.toContain('Mounted stored message 60')
    const region = host.querySelector<HTMLDivElement>('[role="region"][aria-label="Stored conversation messages"]')!
    expect(region.scrollTop).toBe(1100)
    region.scrollTop = followTail ? 1100 : 230
    await act(async () => region.dispatchEvent(new Event('scroll', { bubbles: true })))
    count = 81
    await act(async () => { await vi.advanceTimersByTimeAsync(5000) })
    expect(reads).toHaveLength(2)
    expect(host.querySelector('[role="region"][aria-label="Stored conversation messages"]')).toBe(region)
    expect.soft(region.textContent).toContain(followTail ? 'Mounted stored message 81' : 'Mounted stored message 80')
    expect.soft(region.textContent).toContain('Mounted stored message ' + (followTail ? 81 : 61))
    // Continuous scroll renders the appended row below the reader; the reader's rows and offset stay put.
    if (!followTail) expect.soft(region.textContent).toContain('Mounted stored message 61')
    expect.soft(region.scrollTop).toBe(followTail ? 1100 : 230)
    expect.soft(Boolean(button('New activity · Show latest'))).toBe(!followTail)
    if (!followTail) {
      await act(async () => button('New activity · Show latest').click())
      expect(region.textContent).toContain('Mounted stored message 81')
      expect(region.scrollTop).toBe(1100)
      expect(button('New activity · Show latest')).toBeUndefined()
    }
  })

  it('fingerprints only message count and the last stored message rather than serializing prior history', () => {
    const historical = { toJSON: vi.fn(() => { throw new Error('Prior history must not be serialized') }) }
    const latest = { role: 'assistant', content: 'latest', toolCalls: [{ id: 'tool', content: 'output', status: 'running' }] }
    const first = managedTranscriptFingerprint([historical, latest])
    expect(historical.toJSON).not.toHaveBeenCalled()
    expect(managedTranscriptFingerprint([historical, { ...latest }])).toBe(first)
    expect(managedTranscriptFingerprint([historical, { ...latest, content: 'changed' }])).not.toBe(first)
    expect(managedTranscriptFingerprint([historical, latest, latest])).not.toBe(first)
  })

  it('announces manual refresh progress and completion with existing content', async () => {
    const pending: Query[] = []
    window.cefQuery = query => { pending.push(query) }
    await act(async () => root.render(<ManagedSubagentConversation {...props} />))
    await act(async () => pending[0].onSuccess(JSON.stringify(transcript({ messages: [{ role: 'assistant', content: 'Stored' }] }))))
    await act(async () => button('Refresh conversation').click())
    expect(host.textContent).toContain('Refreshing conversation…')
    await act(async () => pending[1].onSuccess(JSON.stringify(transcript({ messages: [{ role: 'assistant', content: 'Stored' }] }))))
    expect(host.textContent).toContain('Conversation refreshed.')
  })
})

it('honours transcript last-reported metadata without live polling and rejects malformed markers instead of presenting current work', async () => {
  vi.useFakeTimers()
  for (const status of ['queued', 'running']) {
    const requests: string[] = []
    const onBack = vi.fn()
    window.cefQuery = ({ request, onSuccess }) => {
      const query = JSON.parse(request); requests.push(query.action)
      expect(query).toMatchObject({ action: 'getManagedAgentTranscript', payload: { chatId: 'parent', transcriptChatId: 'transcript-1' } })
      onSuccess(JSON.stringify(transcript({ status, lastReported: true, canCancel: true,
        messages: [{ role: 'assistant', content: 'Saved child content only' }] })))
    }
    await act(async () => root.render(<ManagedSubagentConversation {...props} status="running" onBack={onBack} />))
    expect(host.querySelector('.uam-managed-conversation__meta[role="status"]')?.textContent).toMatch(new RegExp(`last reported:\\s*${status}`, 'i'))
    expect(host.querySelector('h3')?.textContent).toBe('Reviewer')
    expect(host.textContent).toContain('Saved child content only')
    expect(host.querySelector('textarea,input,form,.uam-working-ring')).toBeNull()
    expect([...host.querySelectorAll('button')].some(control => /^(stop|resume|send)\b/i.test(control.textContent || ''))).toBe(false)
    await act(async () => { await vi.advanceTimersByTimeAsync(15000) })
    expect(requests).toEqual(['getManagedAgentTranscript'])
    await act(async () => button('Refresh conversation').click())
    expect(requests).toEqual(['getManagedAgentTranscript', 'getManagedAgentTranscript'])
    await act(async () => button('Back to parent').click())
    expect(onBack).toHaveBeenCalledTimes(1)
    await act(async () => root.render(<div />))
  }
  const malformedRequests: string[] = []
  window.cefQuery = ({ request, onSuccess }) => {
    malformedRequests.push(JSON.parse(request).action)
    onSuccess(JSON.stringify(transcript({ status: 'running', lastReported: 'true', canCancel: false,
      messages: [{ role: 'assistant', content: 'UNTRUSTED LAST-REPORTED BODY' }] })))
  }
  await act(async () => root.render(<ManagedSubagentConversation {...props} status="running" />))
  expect(host.querySelector('[role="alert"]')).not.toBeNull()
  expect(host.textContent).not.toContain('UNTRUSTED LAST-REPORTED BODY')
  expect(host.querySelector('.uam-managed-conversation__meta[role="status"]')?.textContent).not.toMatch(/^Running$/)
  await act(async () => { await vi.advanceTimersByTimeAsync(15000) })
  expect(malformedRequests).toEqual(['getManagedAgentTranscript'])
})

// New UserRoot follow-up fixtures only; the original read-only coverage above is unchanged.
describe('ManagedSubagentConversation UserRoot follow-up', () => {
  const runId = '2a23479c-0711-4f46-8c50-a203e7691041'
  const predecessor = '9acd0525-14eb-46c0-beb2-2434a5e306b0'
  const nextTurn = '537b7748-7916-4e72-b0df-ce22d3376ed6'
  const canonicalUuid = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/
  const rawText = '  Continue this exact child.\nKeep the original native history.  '
  type FollowupWire = { action: string; payload: Record<string, unknown>; requestId?: string }
  let fixtureNumber = 0
  const supported = { canSend: true, code: 'none', currentTurnId: predecessor, currentTurnNumber: 1 }

  async function followupFixture(options: { followup?: unknown; lastReported?: boolean; omitFollowup?: boolean } = {}) {
    const saved = useAppStore.getState()
    const drafts = await import('../../utils/composerDraftStorage')
    const fixtureId = ++fixtureNumber // Isolate any inert pending-request cache across cases without a new production reset hook.
    const parentId = `user-root-followup-dom-${fixtureId}`
    const childId = `owned-followup-transcript-dom-${fixtureId}`
    const scopeId = `bounded-user-root-scope-dom-${fixtureId}`
    const parentFile = { id: 'parent-only-file', name: 'parent-only.txt', path: '/tmp/parent-only.txt', type: 'file' as const, size: 11 }
    const parentDraft = { text: 'Independent parent draft — preserve me.', attachments: [parentFile] }
    const oldDraft = drafts.readChatComposerDraft(parentId)
    const parent = { id: parentId, name: 'Original parent', providerId: 'claude-cli', modelId: 'named-private-model', viewMode: 'chat' as const,
      folderId: 'default', executionHostId: 'local', createdAt: new Date('2026-10-17T12:00:00Z'), updatedAt: new Date('2026-10-17T12:00:00Z') }
    const parentMessage = { id: 'original-parent-message', sessionId: parentId, role: 'user' as const, content: 'Original root task', createdAt: parent.createdAt }
    const skill = { id: 'parent-only-skill', title: 'Parent skill', maker: '', review: '', dateCreated: '', dateUpdated: '', preview: '', filePath: '/skills/parent-only.md' }
    const sendAcpPrompt = vi.fn().mockResolvedValue(true)
    const openSubAgentSession = vi.fn().mockResolvedValue(null)
    const branchFromMessage = vi.fn().mockResolvedValue(null)
    const retryFailedMessage = vi.fn().mockResolvedValue({ ok: true })
    drafts.writeChatComposerDraft(parentId, parentDraft)
    useAppStore.setState({ sessions: [parent], activeSessionId: parentId, messages: { [parentId]: [parentMessage] },
      markdownStoreAttachedBySessionId: { [parentId]: [skill] }, sendAcpPrompt, openSubAgentSession, branchFromMessage, retryFailedMessage })
    const sessions = useAppStore.getState().sessions
    const parentMessages = useAppStore.getState().messages[parentId]
    const requests: FollowupWire[] = []
    const submissions: Array<{ wire: FollowupWire; query: Query }> = []
    const onBack = vi.fn()
    const selectedProps = { sourceChatId: parentId, runId, onBack }
    const row = { runId, parentRunId: '', agentId: 'reviewer', transcriptChatId: childId, status: 'completed', providerId: 'claude-cli',
      modelId: 'named-private-model', host: 'local', executionCapability: 'native', canCancel: false, lastReported: options.lastReported === true,
      ...(options.omitFollowup ? {} : { followup: options.followup === undefined ? supported : options.followup }) }
    window.cefQuery = query => {
      const wire = JSON.parse(query.request) as FollowupWire
      requests.push(wire)
      if (wire.action === 'listManagedSubagents') {
        expect(wire.payload).toEqual({ chatId: parentId, runId, offset: 0, limit: 100 })
        query.onSuccess(JSON.stringify({ chatId: parentId, controlScopeId: scopeId, offset: 0, totalCount: 1, nextOffset: null, runs: [row] }))
      } else if (wire.action === 'getManagedAgentTranscript') {
        expect(wire.payload).toEqual({ chatId: parentId, transcriptChatId: childId })
        query.onSuccess(JSON.stringify({ runId, title: 'Same owned reviewer', status: 'completed', providerId: 'claude-cli', lastReported: options.lastReported === true,
          messages: [{ role: 'user', content: 'Existing child task', createdAt: '2026-10-17T12:01:00Z' },
            { role: 'assistant', content: 'Existing completed child answer', createdAt: '2026-10-17T12:01:01Z' }] }))
      } else if (wire.action === 'submitManagedSubagentFollowup') submissions.push({ wire, query })
      else throw new Error(`Unexpected follow-up fixture bridge action: ${wire.action}`)
    }
    const assertParent = () => {
      expect(useAppStore.getState().sessions).toBe(sessions)
      expect(useAppStore.getState().activeSessionId).toBe(parentId)
      expect(useAppStore.getState().messages[parentId]).toBe(parentMessages)
      expect(drafts.readChatComposerDraft(parentId)).toEqual(parentDraft)
      expect(useAppStore.getState().markdownStoreAttachedBySessionId[parentId]).toEqual([skill])
      expect(sendAcpPrompt).not.toHaveBeenCalled()
      expect(openSubAgentSession).not.toHaveBeenCalled()
      expect(branchFromMessage).not.toHaveBeenCalled()
      expect(retryFailedMessage).not.toHaveBeenCalled()
      expect(requests.filter(request => !['listManagedSubagents', 'getManagedAgentTranscript', 'submitManagedSubagentFollowup'].includes(request.action))).toEqual([])
    }
    const render = () => root.render(<ManagedSubagentConversation {...selectedProps} />)
    const cleanup = async () => {
      await act(async () => root.render(<div />))
      drafts.removeComposerDrafts([parentId]); drafts.writeChatComposerDraft(parentId, oldDraft)
      useAppStore.setState(saved, true)
    }
    try {
      await act(async () => render())
      // Existing real public read path and parent-state prerequisites come BEFORE the new feature oracle.
      expect(requests.map(request => [request.action, request.payload])).toEqual([
        ['listManagedSubagents', { chatId: parentId, runId, offset: 0, limit: 100 }],
        ['getManagedAgentTranscript', { chatId: parentId, transcriptChatId: childId }],
      ])
      expect(host.textContent).toContain('Existing completed child answer')
      expect(submissions).toHaveLength(0)
      assertParent()
      return { parentId, childId, scopeId, selectedProps, requests, submissions, row, render, cleanup, assertParent, onBack }
    } catch (error) { await cleanup(); throw error }
  }

  async function openFollowup() {
    const open = [...host.querySelectorAll<HTMLButtonElement>('nav[aria-label="Subagent lineage"] button')].find(control => control.textContent === 'Follow up')
    if (open && !host.querySelector('textarea')) await act(async () => open.click())
  }

  function namedControl(label: string) {
    return [...host.querySelectorAll<HTMLButtonElement>('button')].find(control => (control.getAttribute('aria-label') || control.textContent || '').trim() === label)
  }

  async function readyFollowup(text = rawText) {
    expect(host.querySelector('textarea'), 'Design oracle: no standing follow-up composer before the reader asks').toBeNull()
    await openFollowup()
    const textarea = host.querySelector<HTMLTextAreaElement>('textarea')
    // Best-effort real input precedes the FIRST feature oracle; absent controls cannot make this test pass.
    if (textarea) await act(async () => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')!.set!.call(textarea, text)
      textarea.dispatchEvent(new Event('input', { bubbles: true }))
    })
    const send = namedControl('Send follow-up')
    expect(send, 'Feature oracle: an eligible owned child must expose Send follow-up').toBeDefined()
    expect(send!.disabled, 'Feature oracle: the nonblank supported follow-up must be enabled').toBe(false)
    expect(textarea).not.toBeNull()
    expect(textarea!.disabled).toBe(false)
    expect(textarea!.value).toBe(text)
    return { textarea: textarea!, send: send! }
  }

  function accepted(f: Awaited<ReturnType<typeof followupFixture>>, clientRequestId: unknown) {
    return { accepted: true, replayed: false, code: 'none', acceptance: { rootChatId: f.parentId, runId,
      transcriptChatId: f.childId, clientRequestId, turnId: nextTurn, turnNumber: 2, statusAtAdmission: 'queued' } }
  }

  it.each(['enter', 'button'] as const)('sends exact raw UserRoot work to the same child once while its ACK is held (%s)', async route => {
    const f = await followupFixture()
    try {
      const { textarea, send } = await readyFollowup()
      await act(async () => route === 'enter' ? textarea.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true })) : send.click())
      expect(f.submissions).toHaveLength(1)
      const submitted = f.submissions[0]
      expect(submitted.wire.payload).toEqual({ chatId: f.parentId, runId, controlScopeId: f.scopeId, afterTurnId: predecessor,
        clientRequestId: expect.stringMatching(canonicalUuid), text: rawText })
      expect(textarea.value).toBe(rawText)
      f.assertParent()
      await act(async () => { textarea.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true })); send.click() })
      expect(f.submissions).toHaveLength(1)
      await act(async () => submitted.query.onSuccess(JSON.stringify({ followup: accepted(f, submitted.wire.payload.clientRequestId) })))
      // Accepted empty input closes; explicitly reopening verifies the same retained empty draft without another send.
      expect(host.querySelector('form')).toBeNull()
      expect(namedControl('Follow up')).toBeDefined()
      await openFollowup()
      expect(host.querySelector<HTMLTextAreaElement>('textarea')!.value).toBe('')
      expect(f.submissions).toHaveLength(1)
      f.assertParent()
      await act(async () => button('Back to parent').click())
      expect(f.onBack).toHaveBeenCalledTimes(1)
      f.assertParent()
    } finally { await f.cleanup() }
  })

  it.each(['enter', 'send', 'retry'] as const)('keeps a focusable read-only editor through a held follow-up and unknown result (%s)', async route => {
    const f = await followupFixture()
    try {
      const { textarea, send } = await readyFollowup()
      if (route === 'retry') {
        await act(async () => send.click())
        await act(async () => f.submissions[0].query.onFailure(0, 'Lost original response'))
      }
      const trigger = route === 'enter' ? textarea : route === 'send' ? send : namedControl('Retry same request')!
      trigger.focus()
      expect(document.activeElement).toBe(trigger)
      await act(async () => route === 'enter'
        ? textarea.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true, cancelable: true })) : trigger.click())
      expect(f.submissions).toHaveLength(route === 'retry' ? 2 : 1)
      const sent = f.submissions.at(-1)!
      if (route === 'retry') expect(sent.wire.payload).toEqual(f.submissions[0].wire.payload)
      expect.soft(document.activeElement).toBe(textarea)
      expect.soft(textarea.disabled).toBe(false)
      expect.soft(textarea.readOnly).toBe(true)
      expect(textarea.value).toBe(rawText)
      await act(async () => {
        textarea.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true, cancelable: true }))
        host.querySelector('form')!.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true }))
      })
      expect(f.submissions).toHaveLength(route === 'retry' ? 2 : 1)
      await act(async () => sent.query.onFailure(0, 'Lost held response'))
      expect.soft(document.activeElement).toBe(textarea)
      expect.soft(textarea.disabled).toBe(false)
      expect.soft(textarea.readOnly).toBe(false)
      expect(namedControl('Retry same request')).toBeDefined()
      f.assertParent()
    } finally { await f.cleanup() }
  })

  it('returns owned editor focus to the child heading when fresh accepted state removes the empty form', async () => {
    const f = await followupFixture()
    try {
      const { textarea } = await readyFollowup()
      textarea.focus()
      await act(async () => textarea.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true, cancelable: true })))
      f.row.followup = { ...supported, canSend: false, code: 'busy' }
      const sent = f.submissions[0]
      await act(async () => sent.query.onSuccess(JSON.stringify({ followup: accepted(f, sent.wire.payload.clientRequestId) })))
      expect(host.querySelector('form')).toBeNull()
      expect(document.activeElement).toBe(host.querySelector('h3'))
      expect(f.submissions).toHaveLength(1)
      f.assertParent()
    } finally { await f.cleanup() }
  })

  it('does not reclaim focus from another control when an accepted reply removes the empty form', async () => {
    const f = await followupFixture()
    try {
      const { textarea } = await readyFollowup()
      textarea.focus()
      await act(async () => textarea.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true, cancelable: true })))
      const back = button('Back to parent')
      back.focus()
      f.row.followup = { ...supported, canSend: false, code: 'busy' }
      const sent = f.submissions[0]
      await act(async () => sent.query.onSuccess(JSON.stringify({ followup: accepted(f, sent.wire.payload.clientRequestId) })))
      expect(host.querySelector('form')).toBeNull()
      expect(document.activeElement).toBe(back)
      expect(f.submissions).toHaveLength(1)
      f.assertParent()
    } finally { await f.cleanup() }
  })

  it('refuses a mismatched acceptance identity without clearing child text or the parent context', async () => {
    for (const field of ['rootChatId', 'runId', 'transcriptChatId', 'clientRequestId'] as const) {
      const f = await followupFixture()
      try {
        const { textarea, send } = await readyFollowup()
        await act(async () => send.click())
        expect(f.submissions).toHaveLength(1)
        const submitted = f.submissions[0]
        const response = accepted(f, submitted.wire.payload.clientRequestId)
        response.acceptance[field] = 'ab67b188-13bd-4fac-9937-5b99d7eae814'
        await act(async () => submitted.query.onSuccess(JSON.stringify({ followup: response })))
        expect(textarea.value).toBe(rawText)
        expect(host.querySelector('[role="alert"]')).not.toBeNull()
        expect(f.submissions).toHaveLength(1)
        f.assertParent()
      } finally { await f.cleanup() }
    }
  })

  it('keeps classified refusal distinct from acceptance and preserves the exact authored child draft', async () => {
    const f = await followupFixture()
    try {
      const { textarea, send } = await readyFollowup()
      await act(async () => send.click())
      expect(f.submissions).toHaveLength(1)
      await act(async () => f.submissions[0].query.onSuccess(JSON.stringify({ followup: {
        accepted: false, replayed: false, code: 'permission_denied', acceptance: null,
      } })))
      expect(textarea.value).toBe(rawText)
      expect(host.querySelector('[role="alert"]')).not.toBeNull()
      expect(f.submissions).toHaveLength(1)
      f.assertParent()
    } finally { await f.cleanup() }
  })

  it('retains an unknown request across remount and retries only explicitly with the same UUID predecessor and raw text', async () => {
    const f = await followupFixture()
    try {
      const { textarea, send } = await readyFollowup()
      await act(async () => send.click())
      expect(f.submissions).toHaveLength(1)
      const original = f.submissions[0]
      await act(async () => original.query.onFailure(0, 'Connection lost. The action may have reached UAM.'))
      expect(textarea.value).toBe(rawText)
      expect(host.querySelector('[role="alert"]')).not.toBeNull()
      expect(namedControl('Retry same request')).toBeDefined()
      await act(async () => root.render(<div>Another pane is shown.</div>))
      await act(async () => f.render())
      expect(host.querySelector<HTMLTextAreaElement>('textarea')!.value).toBe(rawText)
      expect(f.submissions).toHaveLength(1) // Neither remount nor read refresh may auto-replay this mutation.
      await act(async () => button('Refresh conversation').click())
      expect(f.submissions).toHaveLength(1)
      const retry = namedControl('Retry same request')
      expect(retry).toBeDefined()
      expect(retry!.disabled).toBe(false)
      await act(async () => retry!.click())
      expect(f.submissions).toHaveLength(2)
      expect(f.submissions[1].wire.payload).toEqual(original.wire.payload)
      const response = accepted(f, original.wire.payload.clientRequestId)
      response.replayed = true
      await act(async () => f.submissions[1].query.onSuccess(JSON.stringify({ followup: response })))
      expect(host.querySelector('form')).toBeNull()
      expect(namedControl('Retry same request')).toBeUndefined()
      expect(f.submissions).toHaveLength(2)
      await act(async () => button('Follow up').click())
      expect(host.querySelector<HTMLTextAreaElement>('textarea')!.value).toBe('')
      expect(f.submissions).toHaveLength(2) // Explicit disclosure must never send again.
      expect(f.submissions).toHaveLength(2)
      f.assertParent()
    } finally { await f.cleanup() }
  })

  it.each(['editor', 'retry'] as const)('retains the focused unknown-request %s during a delayed running poll while revalidation blocks sends', async focusTarget => {
    vi.useFakeTimers()
    const f = await followupFixture()
    try {
      const { send } = await readyFollowup()
      await act(async () => send.click())
      expect(f.submissions).toHaveLength(1)
      const original = f.submissions[0]
      await act(async () => original.query.onFailure(0, 'Connection lost. The action may have reached UAM.'))
      expect(namedControl('Retry same request')?.disabled).toBe(false)
      f.row.status = 'running'
      f.row.followup = { ...supported, canSend: false, code: 'busy', currentTurnId: nextTurn, currentTurnNumber: 2 }
      const priorQuery = window.cefQuery!
      const pending: Query[] = []
      let holdReads = false
      const runningTranscript = () => JSON.stringify({ runId, title: 'Same owned reviewer', status: 'running', providerId: 'claude-cli', lastReported: false,
        messages: [{ role: 'user', content: 'Existing child task', createdAt: '2026-10-17T12:01:00Z' },
          { role: 'assistant', content: 'Existing completed child answer', createdAt: '2026-10-17T12:01:01Z' }] })
      window.cefQuery = query => {
        const wire = JSON.parse(query.request) as FollowupWire
        if (wire.action !== 'getManagedAgentTranscript') { priorQuery(query); return }
        expect(wire.payload).toEqual({ chatId: f.parentId, transcriptChatId: f.childId })
        if (holdReads) pending.push(query)
        else query.onSuccess(runningTranscript())
      }
      await act(async () => button('Refresh conversation').click())
      expect(host.querySelector('.uam-managed-conversation__meta[role="status"]')?.textContent).toBe('Running')
      const field = host.querySelector<HTMLTextAreaElement>('textarea')!
      const form = host.querySelector<HTMLFormElement>('form[aria-label="Send follow-up work"]')!
      const retry = namedControl('Retry same request')!
      expect(field).not.toBeNull()
      expect(field.value).toBe(rawText)
      expect(field.disabled).toBe(false)
      expect(retry.disabled).toBe(false)
      const focused = focusTarget === 'retry' ? retry : field
      focused.focus()
      expect(document.activeElement).toBe(focused)
      holdReads = true
      await act(async () => { await vi.advanceTimersByTimeAsync(5000) })
      expect(pending).toHaveLength(1)
      expect.soft(host.querySelector('textarea')).toBe(field)
      expect.soft(field.isConnected).toBe(true)
      expect.soft(document.activeElement).toBe(focused)
      expect.soft(field.disabled).toBe(false)
      expect.soft(retry.disabled).toBe(false)
      expect.soft(retry.getAttribute('aria-disabled')).toBe('true')
      expect.soft(namedControl('Send follow-up')?.disabled).toBe(true)
      await act(async () => {
        field.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true }))
        form.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true }))
        retry.click()
      })
      expect(f.submissions).toHaveLength(1)
      await act(async () => { await vi.advanceTimersByTimeAsync(5000) })
      expect(pending).toHaveLength(1)
      expect.soft(host.querySelector('textarea')).toBe(field)
      expect.soft(field.isConnected).toBe(true)
      expect.soft(document.activeElement).toBe(focused)
      await act(async () => pending[0].onSuccess(runningTranscript()))
      expect.soft(host.querySelector('textarea')).toBe(field)
      expect.soft(field.isConnected).toBe(true)
      expect.soft(document.activeElement).toBe(focused)
      expect(host.querySelector<HTMLTextAreaElement>('textarea')!.value).toBe(rawText)
      expect(namedControl('Retry same request')?.disabled).toBe(false)
      expect(retry.getAttribute('aria-disabled')).not.toBe('true')
      expect(f.submissions).toHaveLength(1)
      f.assertParent()
      // Explicit retry becomes available only after fresh validation; it retains
      // the actual lost request's predecessor, UUID and original authored text.
      await act(async () => namedControl('Retry same request')!.click())
      expect(f.submissions).toHaveLength(2)
      expect(f.submissions[1].wire.payload).toEqual(original.wire.payload)
      const response = accepted(f, original.wire.payload.clientRequestId)
      response.replayed = true
      await act(async () => f.submissions[1].query.onSuccess(JSON.stringify({ followup: response })))
      f.assertParent()
    } finally { await f.cleanup() }
  })

  it.each(['retry', 'outside'] as const)('preserves a safe focus destination when fresh discovery withdraws the unknown-request form (%s)', async focusTarget => {
    vi.useFakeTimers()
    const f = await followupFixture()
    try {
      const { send } = await readyFollowup()
      await act(async () => send.click())
      expect(f.submissions).toHaveLength(1)
      await act(async () => f.submissions[0].query.onFailure(0, 'Connection lost. The action may have reached UAM.'))
      f.row.status = 'running'
      f.row.followup = { ...supported, canSend: false, code: 'busy', currentTurnId: nextTurn, currentTurnNumber: 2 }
      const priorQuery = window.cefQuery!
      const pending: Query[] = []
      let holdReads = false
      const runningTranscript = () => JSON.stringify({ runId, title: 'Same owned reviewer', status: 'running', providerId: 'claude-cli', lastReported: false,
        messages: [{ role: 'user', content: 'Existing child task', createdAt: '2026-10-17T12:01:00Z' },
          { role: 'assistant', content: 'Existing completed child answer', createdAt: '2026-10-17T12:01:01Z' }] })
      window.cefQuery = query => {
        const wire = JSON.parse(query.request) as FollowupWire
        if (wire.action !== 'getManagedAgentTranscript') { priorQuery(query); return }
        expect(wire.payload).toEqual({ chatId: f.parentId, transcriptChatId: f.childId })
        if (holdReads) pending.push(query)
        else query.onSuccess(runningTranscript())
      }
      await act(async () => button('Refresh conversation').click())
      expect(host.querySelector('.uam-managed-conversation__meta[role="status"]')?.textContent).toBe('Running')
      const field = host.querySelector<HTMLTextAreaElement>('textarea')!
      const form = host.querySelector<HTMLFormElement>('form[aria-label="Send follow-up work"]')!
      const retry = namedControl('Retry same request')!
      const back = button('Back to parent')
      expect(field.value).toBe(rawText)
      expect(retry.disabled).toBe(false)
      const focused = focusTarget === 'retry' ? retry : back
      focused.focus()
      expect(document.activeElement).toBe(focused)
      // Fresh discovery no longer offers control over this same stored child.
      // The real running poll keeps the form until its held transcript reply arrives.
      f.row.followup = undefined
      holdReads = true
      await act(async () => { await vi.advanceTimersByTimeAsync(5000) })
      expect(pending).toHaveLength(1)
      expect(host.querySelector('form')).toBe(form)
      expect(document.activeElement).toBe(focused)
      expect(retry.getAttribute('aria-disabled')).toBe('true')
      expect(f.submissions).toHaveLength(1)
      await act(async () => pending[0].onSuccess(runningTranscript()))
      expect(host.querySelector('form,textarea')).toBeNull()
      expect(form.isConnected).toBe(false)
      expect(host.textContent).toContain('Existing completed child answer')
      expect(f.submissions).toHaveLength(1)
      f.assertParent()
      expect(document.activeElement).toBe(focusTarget === 'retry' ? host.querySelector('h3') : back)
    } finally { await f.cleanup() }
  })

  it('ignores an old child ACK after switching the mounted child and never submits from the replacement view', async () => {
    const f = await followupFixture()
    try {
      const { send } = await readyFollowup()
      await act(async () => send.click())
      expect(f.submissions).toHaveLength(1)
      const pending = f.submissions[0]
      const oldQuery = window.cefQuery!
      const replacementRun = '267f92bd-b931-4e03-af21-609c98c765a4'
      window.cefQuery = query => {
        const wire = JSON.parse(query.request) as FollowupWire
        if (wire.action === 'getManagedAgentTranscript') query.onSuccess(JSON.stringify({ runId: replacementRun, title: 'Different stored child', status: 'completed',
          providerId: 'codex-cli', messages: [{ role: 'assistant', content: 'Replacement read-only child content' }] }))
        else if (wire.action === 'listManagedSubagents') query.onSuccess(JSON.stringify({ chatId: f.parentId, controlScopeId: 'different-list-scope', offset: 0, totalCount: 1, nextOffset: null,
          runs: [{ runId: replacementRun, transcriptChatId: 'different-transcript', status: 'completed', canCancel: false }] }))
        else oldQuery(query)
      }
      await act(async () => root.render(<ManagedSubagentConversation sourceChatId={f.parentId} runId={replacementRun} transcriptChatId="different-transcript" />))
      expect(host.textContent).toContain('Replacement read-only child content')
      await act(async () => pending.query.onSuccess(JSON.stringify({ followup: accepted(f, pending.wire.payload.clientRequestId) })))
      expect(host.textContent).toContain('Replacement read-only child content')
      expect(host.textContent).not.toContain('Same owned reviewer')
      expect(host.querySelector('textarea,form')).toBeNull()
      expect(f.submissions).toHaveLength(1)
      f.assertParent()
    } finally { await f.cleanup() }
  })

  it('keeps absent unsupported malformed and last-reported capability rows read-only without ordinary send', async () => {
    for (const options of [
      { omitFollowup: true },
      { followup: { ...supported, canSend: false, code: 'native_resume_unavailable' } },
      { followup: { ...supported, canSend: 'true' } },
      { followup: { ...supported, currentTurnId: 'not-a-canonical-turn' } },
      { lastReported: true }, // Even a forged true-shaped hint cannot restore saved execution ownership.
    ]) {
      const f = await followupFixture(options)
      try {
        expect(host.querySelector('textarea,input,form')).toBeNull()
        expect([...host.querySelectorAll<HTMLButtonElement>('button')].some(control => !control.disabled && /^(send|retry same request)\b/i.test(control.getAttribute('aria-label') || control.textContent || ''))).toBe(false)
        expect(host.textContent).toContain('Read-only')
        await act(async () => button('Refresh conversation').click())
        expect(f.submissions).toHaveLength(0)
        f.assertParent()
      } finally { await f.cleanup() }
    }
  })

  // Presentation-only controls; the existing fixture records every exact child-scoped read/submission.
  function setDisclosureInput(field: HTMLTextAreaElement, text: string) {
    Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')!.set!.call(field, text)
    field.dispatchEvent(new Event('input', { bubbles: true }))
  }
  function projectDisclosureStatus(initial: 'running' | 'completed') {
    let status = initial
    const bridge = window.cefQuery!
    window.cefQuery = query => {
      const wire = JSON.parse(query.request) as FollowupWire
      bridge({ ...query, onSuccess: response => {
        if (wire.action === 'getManagedAgentTranscript') query.onSuccess(JSON.stringify({ ...JSON.parse(response), status }))
        else query.onSuccess(response)
      } })
    }
    return (next: 'running' | 'completed') => { status = next }
  }

  it('hides and explicitly reopens only empty idle managed input without reads, submissions or parent changes', async () => {
    const f = await followupFixture()
    try {
      expect(host.querySelector('textarea')).toBeNull()
      const readsBefore = f.requests.length
      await openFollowup()
      expect(host.querySelector<HTMLTextAreaElement>('textarea')!.value).toBe('')
      const hide = namedControl('Hide follow-up input')
      expect(hide, 'Empty idle input must be closable without leaving the child').toBeDefined()
      expect(hide!.disabled).toBe(false)
      await act(async () => hide!.click())
      expect(host.querySelector('form')).toBeNull()
      expect(document.activeElement).toBe(host.querySelector('h3'))
      expect(namedControl('Follow up')).toBeDefined()
      await openFollowup()
      expect(host.querySelector<HTMLTextAreaElement>('textarea')!.value).toBe('')
      expect(document.activeElement).toBe(host.querySelector('textarea'))
      expect(f.requests).toHaveLength(readsBefore)
      expect(f.submissions).toHaveLength(0)
      f.assertParent()
    } finally { await f.cleanup() }
  })

  it('closes accepted empty managed input and stays closed when the fresh child remains ready', async () => {
    const f = await followupFixture()
    try {
      const { send } = await readyFollowup()
      await act(async () => send.click())
      expect(f.submissions).toHaveLength(1)
      const sent = f.submissions[0]
      expect(sent.wire.payload).toEqual({ chatId: f.parentId, runId, controlScopeId: f.scopeId, afterTurnId: predecessor,
        clientRequestId: expect.stringMatching(canonicalUuid), text: rawText })
      // No intervening canSend:false is supplied: acceptance itself must retire empty open preference.
      await act(async () => sent.query.onSuccess(JSON.stringify({ followup: accepted(f, sent.wire.payload.clientRequestId) })))
      expect(host.querySelector('form')).toBeNull()
      expect(namedControl('Follow up')).toBeDefined()
      expect(namedControl('Retry same request')).toBeUndefined()
      await act(async () => button('Refresh conversation').click())
      await act(async () => f.render())
      expect(host.querySelector('form')).toBeNull()
      expect(namedControl('Follow up')).toBeDefined()
      expect(f.submissions).toHaveLength(1)
      f.assertParent()
    } finally { await f.cleanup() }
  })

  it('keeps accepted managed input closed through a running-to-ready capability cycle until explicit Open', async () => {
    const f = await followupFixture()
    try {
      const { send } = await readyFollowup()
      const projectStatus = projectDisclosureStatus('running')
      await act(async () => send.click())
      expect(f.submissions).toHaveLength(1)
      f.row.status = 'running'
      f.row.followup = { ...supported, canSend: false, code: 'busy', currentTurnId: nextTurn, currentTurnNumber: 2 }
      const sent = f.submissions[0]
      await act(async () => sent.query.onSuccess(JSON.stringify({ followup: accepted(f, sent.wire.payload.clientRequestId) })))
      expect(host.querySelector('.uam-managed-conversation__meta[role="status"]')?.textContent).toBe('Running')
      expect(host.querySelector('form')).toBeNull()
      f.row.status = 'completed'
      f.row.followup = { ...supported, currentTurnId: nextTurn, currentTurnNumber: 2 }
      projectStatus('completed')
      await act(async () => button('Refresh conversation').click())
      expect(host.querySelector('form')).toBeNull()
      expect(namedControl('Follow up')).toBeDefined()
      expect(f.submissions).toHaveLength(1)
      await openFollowup()
      expect(host.querySelector<HTMLTextAreaElement>('textarea')!.value).toBe('')
      expect(f.submissions).toHaveLength(1)
      f.assertParent()
    } finally { await f.cleanup() }
  })

  it('keeps managed errors, drafts and pending or unknown requests visible, with only explicit same-UUID Retry', async () => {
    const f = await followupFixture()
    try {
      await openFollowup()
      let field = host.querySelector<HTMLTextAreaElement>('textarea')!
      await act(async () => field.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true, cancelable: true })))
      expect(f.submissions).toHaveLength(0)
      const feedback = host.querySelector('[role="alert"]')?.textContent
      expect(feedback).toBeTruthy()
      expect(namedControl('Hide follow-up input')).toBeUndefined()
      const projectStatus = projectDisclosureStatus('running')
      f.row.status = 'running'
      f.row.followup = { ...supported, canSend: false, code: 'busy' }
      await act(async () => button('Refresh conversation').click())
      expect(host.querySelector('form'), 'An active recovery/error must not be hidden with an empty draft').not.toBeNull()
      expect(host.querySelector('[role="alert"]')?.textContent).toBe(feedback)
      expect(namedControl('Hide follow-up input')).toBeUndefined()
      f.row.status = 'completed'; f.row.followup = supported; projectStatus('completed')
      await act(async () => button('Refresh conversation').click())
      field = host.querySelector<HTMLTextAreaElement>('textarea')!
      expect(field).not.toBeNull()
      act(() => setDisclosureInput(field, rawText))
      expect(namedControl('Hide follow-up input')).toBeUndefined()
      await act(async () => namedControl('Send follow-up')!.click())
      expect(f.submissions).toHaveLength(1)
      const original = f.submissions[0]
      expect(host.querySelector('textarea')).toBe(field)
      expect(field.readOnly).toBe(true)
      expect(field.value).toBe(rawText)
      expect(namedControl('Hide follow-up input')).toBeUndefined()
      await act(async () => original.query.onFailure(0, 'Response lost; original request is uncertain.'))
      expect(field.value).toBe(rawText)
      expect(namedControl('Retry same request')).toBeDefined()
      expect(namedControl('Hide follow-up input')).toBeUndefined()
      const newerDraft = 'A newer exact child draft must not be cleared by the old acceptance.'
      act(() => setDisclosureInput(field, newerDraft))
      await act(async () => root.render(<div>Another pane is shown.</div>))
      await act(async () => f.render())
      field = host.querySelector<HTMLTextAreaElement>('textarea')!
      expect(field.value).toBe(newerDraft)
      expect(namedControl('Hide follow-up input')).toBeUndefined()
      expect(f.submissions).toHaveLength(1)
      await act(async () => namedControl('Retry same request')!.click())
      expect(f.submissions).toHaveLength(2)
      expect(f.submissions[1].wire.payload).toEqual(original.wire.payload)
      const reply = accepted(f, original.wire.payload.clientRequestId); reply.replayed = true
      await act(async () => f.submissions[1].query.onSuccess(JSON.stringify({ followup: reply })))
      expect(host.querySelector<HTMLTextAreaElement>('textarea')!.value).toBe(newerDraft)
      expect(namedControl('Hide follow-up input')).toBeUndefined()
      expect(f.submissions).toHaveLength(2)
      f.assertParent()
    } finally { await f.cleanup() }
  })
})

// Supplied child routes discover capability only through an explicit live hint followed by exact listing validation.
describe('ManagedSubagentConversation scoped route discovery', () => {
  const routeRun = 'bea3aee7-225c-4aa2-baf5-6570d85cac0b'
  const routeTurn = 'c4cc4b24-4f2f-4a9b-bcef-8e41b1a5a900'
  const routeCapability = { canSend: true, code: 'none', currentTurnId: routeTurn, currentTurnNumber: 1 }
  let routeSequence = 0
  async function routeFixture(options: { hint?: unknown; omitHint?: boolean; listedTranscript?: string; listingCapability?: unknown } = {}) {
    const serial = ++routeSequence
    const route = { sourceChatId: `supplied-route-root-${serial}`, runId: routeRun, transcriptChatId: `supplied-route-transcript-${serial}` }
    let currentScope = `supplied-route-scope-${serial}`
    let currentCapability: unknown = options.listingCapability ?? routeCapability
    const reads: Array<{ action: string; payload: Record<string, unknown> }> = []
    const writes: Array<{ payload: Record<string, unknown>; query: Query }> = []
    window.cefQuery = query => {
      const wire = JSON.parse(query.request)
      if (wire.action === 'submitManagedSubagentFollowup') { writes.push({ payload: wire.payload, query }); return }
      reads.push(wire)
      if (wire.action === 'getManagedAgentTranscript') {
        expect(wire.payload).toEqual({ chatId: route.sourceChatId, transcriptChatId: route.transcriptChatId })
        query.onSuccess(JSON.stringify({ runId: routeRun, title: 'Supplied route child', status: 'completed', providerId: 'claude-cli', lastReported: false,
          ...(options.omitHint ? {} : { followup: options.hint ?? routeCapability }), messages: [{ role: 'assistant', content: 'Existing supplied child result' }] }))
      } else if (wire.action === 'listManagedSubagents') {
        expect(wire.payload).toEqual({ chatId: route.sourceChatId, runId: routeRun, offset: 0, limit: 100 })
        query.onSuccess(JSON.stringify({ chatId: route.sourceChatId, controlScopeId: currentScope, offset: 0, nextOffset: null, totalCount: 1,
          runs: [{ runId: routeRun, transcriptChatId: options.listedTranscript ?? route.transcriptChatId, providerId: 'claude-cli', status: 'completed', canCancel: false,
            followup: currentCapability }] }))
      } else throw new Error(`Unexpected supplied-route action: ${wire.action}`)
    }
    await act(async () => root.render(<ManagedSubagentConversation {...route} />))
    expect(host.textContent).toContain('Existing supplied child result')
    return { route, reads, writes, rotateListing: (scope: string, capability: unknown) => { currentScope = scope; currentCapability = capability } }
  }
  function routeSend() { return [...host.querySelectorAll<HTMLButtonElement>('button')].find(control => control.textContent === 'Send follow-up') }
  async function typeRoute(text: string) {
    const open = [...host.querySelectorAll<HTMLButtonElement>('nav[aria-label="Subagent lineage"] button')].find(control => control.textContent === 'Follow up')
    if (open && !host.querySelector('textarea')) await act(async () => open.click())
    const field = host.querySelector<HTMLTextAreaElement>('textarea')
    expect(field).not.toBeNull()
    await act(async () => { Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')!.set!.call(field, text); field!.dispatchEvent(new Event('input', { bubbles: true })) })
    return field!
  }

  it.each([true, false])('validates a supplied transcript hint against the exact listing before enabling follow-up (hint=%s)', async hint => {
    const f = await routeFixture({ hint: { ...routeCapability, canSend: hint } })
    expect(f.reads.map(read => read.action)).toEqual(['getManagedAgentTranscript', 'listManagedSubagents'])
    const field = await typeRoute('Same supplied child only')
    expect(routeSend()?.disabled).toBe(false)
    await act(async () => routeSend()!.click())
    expect(f.writes).toHaveLength(1)
    expect(f.writes[0].payload).toMatchObject({ chatId: f.route.sourceChatId, runId: routeRun, controlScopeId: expect.any(String), afterTurnId: routeTurn, text: field.value })
    await act(async () => f.writes[0].query.onSuccess(JSON.stringify({ followup: { accepted: false, replayed: false, code: 'permission_denied', acceptance: null } })))
  })

  it('preserves supplied transcript get-only compatibility without a hint and never trusts a mismatched or unavailable listing', async () => {
    for (const options of [{ omitHint: true }, { listedTranscript: 'foreign-stored-transcript' }, { listingCapability: { ...routeCapability, canSend: false, code: 'busy' } }]) {
      const f = await routeFixture(options)
      expect(f.reads.map(read => read.action)).toEqual(options.omitHint ? ['getManagedAgentTranscript'] : ['getManagedAgentTranscript', 'listManagedSubagents'])
      expect(host.querySelector('textarea,form')).toBeNull()
      expect(routeSend()).toBeUndefined()
      expect(f.writes).toHaveLength(0)
      await act(async () => root.render(<div />))
    }
  })

  it('keeps Shift+Enter and composition local while plain Enter sends once', async () => {
    const f = await routeFixture()
    const field = await typeRoute('Text before newline')
    const shift = new KeyboardEvent('keydown', { key: 'Enter', shiftKey: true, bubbles: true, cancelable: true })
    const composing = new KeyboardEvent('keydown', { key: 'Enter', isComposing: true, bubbles: true, cancelable: true })
    await act(async () => { field.dispatchEvent(shift); field.dispatchEvent(composing) })
    expect(shift.defaultPrevented).toBe(false)
    expect(composing.defaultPrevented).toBe(false)
    expect(f.writes).toHaveLength(0)
    // jsdom does not perform the browser's default textarea newline insertion; publish that real input explicitly.
    await typeRoute('Text before newline\nSecond line')
    await act(async () => field.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true })))
    expect(f.writes).toHaveLength(1)
    expect(f.writes[0].payload.text).toBe('Text before newline\nSecond line')
    await act(async () => f.writes[0].query.onSuccess(JSON.stringify({ followup: { accepted: false, replayed: false, code: 'permission_denied', acceptance: null } })))
  })

  it('preserves the original unknown envelope when an explicit retry is refused', async () => {
    const f = await routeFixture()
    await typeRoute('Do not replace this unknown request')
    await act(async () => routeSend()!.click())
    expect(f.writes).toHaveLength(1)
    const original = f.writes[0].payload
    await act(async () => f.writes[0].query.onSuccess(JSON.stringify({ followup: { accepted: false, replayed: false, code: 'delivery_unknown', acceptance: null } })))
    const retry = () => [...host.querySelectorAll<HTMLButtonElement>('button')].find(control => control.textContent === 'Retry same request')
    const newerDraft = 'Separate newer child draft while the original is unconfirmed'
    await typeRoute(newerDraft)
    expect(retry()).toBeDefined()
    await act(async () => retry()!.click())
    expect(f.writes).toHaveLength(2)
    expect(f.writes[1].payload).toEqual(original)
    await act(async () => f.writes[1].query.onSuccess(JSON.stringify({ followup: { accepted: false, replayed: false, code: 'permission_denied', acceptance: null } })))
    expect(routeSend()?.disabled).toBe(true)
    expect(retry()).toBeDefined()
    expect(host.querySelector<HTMLTextAreaElement>('textarea')!.value).toBe(newerDraft)
    await act(async () => retry()!.click())
    expect(f.writes).toHaveLength(3)
    expect(f.writes[2].payload).toEqual(original)
    await act(async () => f.writes[2].query.onFailure(0, 'Still unconfirmed.'))
  })

  it('reauthorizes an explicit retry with the fresh scope while preserving the original predecessor UUID text and expected turn number', async () => {
    const f = await routeFixture()
    await typeRoute('Original request before the listing advanced')
    await act(async () => routeSend()!.click())
    expect(f.writes).toHaveLength(1)
    const original = f.writes[0].payload
    await act(async () => f.writes[0].query.onFailure(0, 'The result is unknown.'))
    f.rotateListing('fresh-current-scope-after-progress', { canSend: false, code: 'busy',
      currentTurnId: '59bf8d83-bccc-4690-8d46-56a290e8ed3b', currentTurnNumber: 3 })
    await act(async () => button('Refresh conversation').click())
    expect(f.writes).toHaveLength(1)
    expect(routeSend()?.disabled).toBe(true)
    const retry = [...host.querySelectorAll<HTMLButtonElement>('button')].find(control => control.textContent === 'Retry same request')
    expect(retry?.disabled).toBe(false)
    await act(async () => retry!.click())
    expect(f.writes).toHaveLength(2)
    expect(f.writes[1].payload).toEqual({ ...original, controlScopeId: 'fresh-current-scope-after-progress' })
    await act(async () => f.writes[1].query.onSuccess(JSON.stringify({ followup: { accepted: true, replayed: true, code: 'none', acceptance: {
      rootChatId: f.route.sourceChatId, runId: routeRun, transcriptChatId: f.route.transcriptChatId, clientRequestId: original.clientRequestId,
      turnId: '68689303-7114-4490-b068-38c7f48d2900', turnNumber: 2, statusAtAdmission: 'queued',
    } } })))
    // Admission status and turn number are the original receipt, not the newer listing's turn 3.
    expect(host.querySelector('button')?.isConnected).toBe(true)
    expect(host.querySelector('[role="alert"]')).toBeNull()
    expect([...host.querySelectorAll('button')].some(control => control.textContent === 'Retry same request')).toBe(false)
    expect(host.querySelector('textarea,form')).toBeNull() // The latest unavailable listing closes the now-empty, settled editor.
    expect(f.writes).toHaveLength(2)
  })

  it('bounds retained drafts without evicting an unresolved UUID or enabling a fresh duplicate after remount', async () => {
    let first: Awaited<ReturnType<typeof routeFixture>> | undefined
    let reachedBound = false
    for (let index = 0; index < 33; ++index) {
      const f = await routeFixture()
      const open = [...host.querySelectorAll<HTMLButtonElement>('nav[aria-label="Subagent lineage"] button')].find(control => control.textContent === 'Follow up')
      if (open) await act(async () => open.click())
      if (!host.querySelector('textarea')) {
        expect(host.querySelector('[role="alert"]')?.textContent).toContain('unresolved')
        expect(routeSend()).toBeUndefined()
        expect(f.writes).toHaveLength(0)
        reachedBound = true
        break
      }
      await typeRoute(`Unresolved child ${index}`)
      await act(async () => routeSend()!.click())
      expect(f.writes).toHaveLength(1)
      await act(async () => f.writes[0].query.onFailure(0, 'Connection lost. The action may have reached UAM.'))
      first ??= f
      await act(async () => root.render(<div />))
    }
    expect(reachedBound).toBe(true)
    expect(first).toBeDefined()
    // Reopen through real reads, not a cache inspection/reset hook. Existing unknown identity must remain retry-only.
    const writes: Array<{ payload: Record<string, unknown>; query: Query }> = []
    window.cefQuery = query => {
      const wire = JSON.parse(query.request)
      if (wire.action === 'getManagedAgentTranscript') query.onSuccess(JSON.stringify({ runId: routeRun, title: 'Pinned unknown child', status: 'completed', providerId: 'claude-cli', followup: routeCapability, messages: [] }))
      else if (wire.action === 'listManagedSubagents') query.onSuccess(JSON.stringify({ chatId: first!.route.sourceChatId, controlScopeId: first!.writes[0].payload.controlScopeId,
        offset: 0, totalCount: 1, nextOffset: null, runs: [{ runId: routeRun, transcriptChatId: first!.route.transcriptChatId, providerId: 'claude-cli', status: 'completed', canCancel: false, followup: routeCapability }] }))
      else if (wire.action === 'submitManagedSubagentFollowup') writes.push({ payload: wire.payload, query })
      else throw new Error(`Unexpected capacity control action: ${wire.action}`)
    }
    await act(async () => root.render(<ManagedSubagentConversation {...first!.route} />))
    expect(writes).toHaveLength(0)
    expect(routeSend()?.disabled).toBe(true)
    const retry = [...host.querySelectorAll<HTMLButtonElement>('button')].find(control => control.textContent === 'Retry same request')
    expect(retry).toBeDefined()
    await act(async () => retry!.click())
    expect(writes).toHaveLength(1)
    expect(writes[0].payload).toEqual(first!.writes[0].payload)
    await act(async () => writes[0].query.onFailure(0, 'Still unconfirmed.'))
  })
})
