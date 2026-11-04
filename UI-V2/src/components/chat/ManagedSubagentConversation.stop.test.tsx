import { act } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { ManagedSubagentConversation } from './ManagedSubagentConversation'
import { useAppStore } from '../../store/useAppStore'
import { sanitizeCppChat } from '../../store/cpp/sanitizers'
import { sessionFromCppChat } from '../../store/cpp/reconcile'
import { readChatComposerDraft, removeComposerDrafts, writeChatComposerDraft } from '../../utils/composerDraftStorage'

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
type Query = Parameters<NonNullable<Window['cefQuery']>>[0]
type Wire = { action: string; payload: Record<string, unknown>; requestId?: string }
type Held = { wire: Wire; query: Query; done: boolean }
let host: HTMLDivElement, root: Root, previousCef: Window['cefQuery'], saved: ReturnType<typeof useAppStore.getState>
let sequence = 0
let parentId: string, runId: string, transcriptId: string, scope: string, status: string
let transcriptExtra: Record<string, unknown>, rowExtra: Record<string, unknown>, omitRun: boolean
let holdListing: boolean, refuseListing: boolean, pending: Held[], wires: Wire[]
const parentDraft = { text: 'Unsent parent work must stay here.', attachments: [] }
const stop = () => [...host.querySelectorAll<HTMLButtonElement>('button')].find(button => button.textContent?.trim() === 'Stop child') ?? null
const refresh = () => host.querySelector<HTMLButtonElement>('button[aria-label="Refresh conversation"]')!
const messages = [{ role: 'user', content: 'Review only this child.' }, { role: 'assistant', content: 'Current child transcript stays readable.' }]
const listing = () => ({ chatId: parentId, controlScopeId: scope, offset: 0, nextOffset: null, totalCount: omitRun ? 1 : 2,
  runs: [
    { runId: `${runId}-sibling`, transcriptChatId: `${transcriptId}-sibling`, providerId: 'claude-cli', status: 'running', canCancel: true },
    ...(omitRun ? [] : [{ runId, transcriptChatId: transcriptId, providerId: 'claude-cli', status, canCancel: true, ...rowExtra }]),
  ] })
const render = async (overrides = {}) => { await act(async () => root.render(<ManagedSubagentConversation sourceChatId={parentId} runId={runId}
  transcriptChatId={transcriptId} parentName="Parent" navigationKey="stop-pane" onBack={() => {}} {...overrides} />)) }
const finish = async (held: Held, result: unknown, failure = false, failureCode = 503) => {
  await act(async () => {
    held.done = true
    if (failure) held.query.onFailure(failureCode, String(result))
    else held.query.onSuccess(JSON.stringify(result))
  })
}
const assertParent = (additionalActions: string[] = []) => {
  const state = useAppStore.getState()
  expect(state.activeSessionId).toBe(parentId)
  expect(state.messages).toBe(saved.messages)
  expect(readChatComposerDraft(parentId)).toEqual(parentDraft)
  expect(state.sendAcpPrompt).not.toHaveBeenCalled()
  expect(state.stopAcpSession).not.toHaveBeenCalled()
  expect(state.openSubAgentSession).not.toHaveBeenCalled()
  expect(wires.every(wire => ['getManagedAgentTranscript', 'listManagedSubagents', 'cancelManagedSubagent', ...additionalActions].includes(wire.action))).toBe(true)
}

beforeEach(() => {
  saved = useAppStore.getState(); previousCef = window.cefQuery
  sequence += 1; parentId = `stop-parent-${sequence}`; runId = `stop-run-${sequence}`; transcriptId = `stop-transcript-${sequence}`
  scope = `owned-scope-${sequence}`; status = 'running'; transcriptExtra = {}; rowExtra = {}; omitRun = false
  holdListing = false; refuseListing = false; pending = []; wires = []
  const parent = sessionFromCppChat(sanitizeCppChat({ id: parentId, title: 'Parent', providerId: 'claude-cli',
    createdAt: '2026-10-23T12:00:00Z', updatedAt: '2026-10-23T12:00:00Z', executionHostId: 'local' })!, undefined, [{ id: 'claude-cli' }])
  useAppStore.setState({ sessions: [parent], activeSessionId: parentId, sendAcpPrompt: vi.fn(), stopAcpSession: vi.fn(), openSubAgentSession: vi.fn() })
  writeChatComposerDraft(parentId, parentDraft)
  host = document.createElement('div'); document.body.append(host); root = createRoot(host)
  window.cefQuery = query => {
    const wire = JSON.parse(query.request) as Wire
    wires.push(wire)
    if (wire.action === 'getManagedAgentTranscript') {
      expect(wire.payload).toEqual({ chatId: parentId, transcriptChatId: transcriptId })
      // No messaging hint: Stop must work for an owned child without a native follow-up API.
      query.onSuccess(JSON.stringify({ runId, providerId: 'claude-cli', title: 'Selected child', status, messages, ...transcriptExtra }))
    } else if (wire.action === 'listManagedSubagents') {
      expect(wire.payload).toEqual({ chatId: parentId, runId, offset: 0, limit: 100 })
      if (holdListing) pending.push({ wire, query, done: false })
      else if (refuseListing) query.onFailure(403, 'Control listing unavailable.')
      else query.onSuccess(JSON.stringify(listing()))
    } else if (wire.action === 'cancelManagedSubagent') pending.push({ wire, query, done: false })
    else throw new Error(`Unexpected child Stop action: ${wire.action}`)
  }
})
afterEach(async () => {
  await act(async () => root.unmount())
  for (const held of pending) if (!held.done) await finish(held, 'Retired fixture read.', true)
  expect(pending.every(held => held.done)).toBe(true)
  removeComposerDrafts([parentId]); host.remove(); window.cefQuery = previousCef
  useAppStore.setState(saved, true); vi.useRealTimers(); vi.restoreAllMocks()
})

describe('managed conversation individual Stop', () => {
  it.each(['queued', 'running', 'waiting_for_input'])('stops only the displayed %s child once without leaving its conversation', async initial => {
    status = initial
    await render()
    expect(host.textContent).toContain(messages[1].content)
    expect(host.querySelector('textarea, form')).toBeNull()
    const button = stop()!
    expect(button, 'A readable owned active child needs its own Stop control').not.toBeNull()
    expect(button.getAttribute('aria-label') || button.textContent).toContain('Stop child')
    expect(host.textContent).not.toContain('native session controls are unavailable')
    expect(button.disabled).toBe(false)
    expect(wires.map(wire => wire.action)).toEqual(['getManagedAgentTranscript', 'listManagedSubagents'])
    await act(async () => { button.click(); button.click() })
    expect(pending).toHaveLength(1)
    expect(pending[0].wire).toEqual({ action: 'cancelManagedSubagent', payload: { chatId: parentId, runId, controlScopeId: scope }, requestId: expect.any(String) })
    expect(pending[0].wire.requestId?.length).toBeGreaterThan(0)
    expect(stop()?.disabled).toBe(true)
    expect(refresh().disabled).toBe(true)
    expect(host.querySelector('h3')).toBe(document.activeElement)
    expect(host.textContent).toContain(messages[1].content)
    expect(host.textContent).not.toContain('Stop requested.')
    status = 'cancelled'
    await finish(pending[0], { ok: true, runId })
    expect(host.textContent).not.toContain('Stop requested. Reading this child’s status.')
    expect(host.querySelector('nav')?.textContent).toContain('Stopped')
    expect(stop()).toBeNull()
    expect(wires.filter(wire => wire.action === 'cancelManagedSubagent')).toHaveLength(1)
    assertParent()
  })

  it.each(['saved transcript', 'saved row', 'no permission', 'different transcript', 'different provider', 'missing provider', 'missing run', 'settled row', 'read-only root', 'invalid root birth'])('does not borrow Stop authority from %s', async kind => {
    if (kind === 'saved transcript') transcriptExtra.lastReported = true
    if (kind === 'saved row') rowExtra.lastReported = true
    if (kind === 'no permission') rowExtra.canCancel = false
    if (kind === 'different transcript') rowExtra.transcriptChatId = 'foreign-transcript'
    if (kind === 'different provider') rowExtra.providerId = 'codex-cli'
    if (kind === 'missing provider') { rowExtra.providerId = ''; transcriptExtra.providerId = '' }
    if (kind === 'missing run') omitRun = true
    if (kind === 'settled row') rowExtra.status = 'completed'
    if (kind === 'read-only root') useAppStore.setState(state => ({ sessions: state.sessions.map(session => ({ ...session, importedReadOnly: true })) }))
    if (kind === 'invalid root birth') useAppStore.setState(state => ({ sessions: state.sessions.map(session => ({ ...session, createdAt: new Date(NaN) })) }))
    await render()
    expect(host.textContent).toContain(messages[1].content)
    expect(stop()).toBeNull()
    expect(wires.some(wire => wire.action === 'cancelManagedSubagent')).toBe(false)
    assertParent()
  })

  it('keeps Stop unavailable while disconnected, with no fabricated browser control', async () => {
    window.cefQuery = undefined
    await render()
    expect(stop()).toBeNull(); expect(wires).toHaveLength(0)
    expect(host.textContent).toContain('connected to UAM')
    assertParent()
  })

  it('keeps the transcript readable during control discovery and retires a replaced-root listing', async () => {
    holdListing = true
    await render()
    expect(host.textContent).toContain(messages[1].content)
    expect(stop()).toBeNull(); expect(pending).toHaveLength(1)
    await act(async () => useAppStore.setState(state => ({ sessions: state.sessions.map(session => ({ ...session, createdAt: new Date('2026-10-23T12:01:00Z') })) })))
    await finish(pending[0], listing())
    expect(stop()).toBeNull()
    expect(wires.some(wire => wire.action === 'cancelManagedSubagent')).toBe(false)
    assertParent()
  })

  it('recovers a failed control read through Refresh without hiding the conversation or sending Stop', async () => {
    refuseListing = true
    await render()
    expect(host.textContent).toContain(messages[1].content)
    expect(host.textContent).toContain('Stop is unavailable until this child’s current status can be read.')
    expect(stop()).toBeNull()
    refuseListing = false
    await act(async () => refresh().click())
    expect(stop()?.disabled).toBe(false)
    expect(host.textContent).not.toContain('Stop is unavailable until')
    expect(wires.some(wire => wire.action === 'cancelManagedSubagent')).toBe(false)
    assertParent()
  })

  it('keeps an unavailable Stop notice mounted through deferred polls until a fresh listing succeeds', async () => {
    vi.useFakeTimers()
    refuseListing = true
    await render()
    const notice = [...host.querySelectorAll('[role="status"]')].find(node => node.textContent?.startsWith('Stop is unavailable'))!
    expect(notice).toBeDefined()
    holdListing = true
    for (let index = 0; index < 2; index += 1) {
      await act(async () => { await vi.advanceTimersByTimeAsync(5000) })
      expect(notice.isConnected, 'A background read must not remove an unresolved status line').toBe(true)
      expect([...host.querySelectorAll('[role="status"]')].find(node => node.textContent?.startsWith('Stop is unavailable'))).toBe(notice)
      expect(stop()).toBeNull()
      await finish(pending[index], 'Control listing still refused.', true, 403)
      expect(notice.isConnected).toBe(true)
      expect(host.textContent).toContain(messages[1].content)
    }
    await act(async () => { await vi.advanceTimersByTimeAsync(5000) })
    scope = 'recovered-control-scope'
    await finish(pending[2], listing())
    expect(notice.isConnected).toBe(false)
    expect(stop()?.getAttribute('aria-disabled')).not.toBe('true')
    expect(wires.some(wire => wire.action === 'cancelManagedSubagent')).toBe(false)
    assertParent()
  })

  it.each(['settled child', 'saved snapshot'])('retires an unavailable Stop notice only after a validated %s read', async outcome => {
    vi.useFakeTimers()
    refuseListing = true
    await render()
    const notice = [...host.querySelectorAll('[role="status"]')].find(node => node.textContent?.startsWith('Stop is unavailable'))!
    const bridge = window.cefQuery!
    window.cefQuery = query => {
      const wire = JSON.parse(query.request) as Wire
      if (wire.action === 'getManagedAgentTranscript') { wires.push(wire); pending.push({ wire, query, done: false }) }
      else bridge(query)
    }
    await act(async () => { await vi.advanceTimersByTimeAsync(5000) })
    expect(notice.isConnected, 'Pending history is not a validated status change').toBe(true)
    await finish(pending[0], { runId, providerId: 'claude-cli', messages, status: outcome === 'settled child' ? 'completed' : 'running', lastReported: outcome === 'saved snapshot' })
    expect(notice.isConnected).toBe(false)
    expect(stop()).toBeNull()
    expect(wires.filter(wire => wire.action === 'listManagedSubagents')).toHaveLength(1)
    expect(wires.some(wire => wire.action === 'cancelManagedSubagent')).toBe(false)
    assertParent()
  })

  it.each(['refused', 'mismatched acknowledgement'])('reports %s Stop as unconfirmed and retries only status reads', async outcome => {
    vi.useFakeTimers()
    await render()
    await act(async () => stop()!.click())
    scope = 'fresh-scope-after-unconfirmed-stop'
    await finish(pending[0], outcome === 'refused' ? 'Acknowledgement lost.' : { ok: true, runId: 'another-child' }, outcome === 'refused')
    expect(host.textContent).not.toContain('Stop was not confirmed.')
    expect(host.querySelector('nav')?.textContent).not.toContain('Stopped')
    await act(async () => { await vi.advanceTimersByTimeAsync(5000) })
    await act(async () => document.dispatchEvent(new Event('visibilitychange')))
    expect(wires.filter(wire => wire.action === 'cancelManagedSubagent')).toHaveLength(1)
    expect(wires.filter(wire => wire.action === 'getManagedAgentTranscript').length).toBeGreaterThan(1)
    expect(host.textContent).toContain(messages[1].content)
    // An explicit new click uses a newly attested scope; no read reuses the old mutation.
    await act(async () => stop()!.click())
    expect(pending[1].wire.payload).toEqual({ chatId: parentId, runId, controlScopeId: scope })
    status = 'cancelled'; await finish(pending[1], { ok: true, runId })
    assertParent()
  })

  it('invalidates an old button during Refresh and requires a fresh matching scope', async () => {
    await render()
    const old = stop()!
    holdListing = true
    await act(async () => refresh().click())
    expect(stop()).toBeNull(); expect(pending).toHaveLength(1)
    await act(async () => old.click())
    expect(wires.some(wire => wire.action === 'cancelManagedSubagent')).toBe(false)
    scope = 'refreshed-scope'
    await finish(pending[0], listing())
    await act(async () => stop()!.click())
    expect(pending[1].wire.payload).toEqual({ chatId: parentId, runId, controlScopeId: scope })
    assertParent()
  })

  it.each(['poll', 'visibility', 'root revision'])('retains focused Stop through a deferred %s read without retaining authority', async trigger => {
    vi.useFakeTimers()
    await render()
    const button = stop()!
    act(() => button.focus())
    holdListing = true
    scope = `fresh-${trigger}-scope`
    await act(async () => {
      if (trigger === 'poll') await vi.advanceTimersByTimeAsync(5000)
      else if (trigger === 'visibility') document.dispatchEvent(new Event('visibilitychange'))
      else useAppStore.setState(state => ({ sessions: state.sessions.map(session => ({ ...session, managedSubagentRevision: (session.managedSubagentRevision ?? 0) + 1 })) }))
    })
    expect(pending).toHaveLength(1)
    expect(stop(), 'Background reading must keep the same Stop element').toBe(button)
    expect(document.activeElement).toBe(button)
    expect(button.disabled).toBe(false)
    expect(button.getAttribute('aria-disabled')).toBe('true')
    await act(async () => { button.click(); button.click() })
    expect(wires.some(wire => wire.action === 'cancelManagedSubagent')).toBe(false)
    expect(document.activeElement).toBe(button)
    await finish(pending[0], listing())
    expect(stop()).toBe(button)
    expect(document.activeElement).toBe(button)
    expect(button.getAttribute('aria-disabled')).not.toBe('true')
    await act(async () => button.click())
    expect(pending[1].wire.payload).toEqual({ chatId: parentId, runId, controlScopeId: scope })
    status = 'cancelled'; await finish(pending[1], { ok: true, runId })
    expect(wires.filter(wire => wire.action === 'cancelManagedSubagent')).toHaveLength(1)
    assertParent()
  })

  it('revokes Stop authority as soon as a poll is scheduled, before its read effect commits', async () => {
    vi.useFakeTimers()
    await render()
    const button = stop()!
    holdListing = true
    await act(async () => { vi.advanceTimersByTime(5000); button.click() })
    expect(wires.some(wire => wire.action === 'cancelManagedSubagent')).toBe(false)
    expect(pending).toHaveLength(1)
    scope = 'fresh-scheduled-poll-scope'
    await finish(pending[0], listing())
    await act(async () => stop()!.click())
    expect(pending[1].wire.payload).toEqual({ chatId: parentId, runId, controlScopeId: scope })
    status = 'cancelled'; await finish(pending[1], { ok: true, runId })
    assertParent()
  })

  it.each(['permission removed', 'listing refused'])('retires a retained Stop after a background read settles with %s', async outcome => {
    vi.useFakeTimers()
    await render()
    const button = stop()!
    act(() => button.focus())
    holdListing = true
    await act(async () => { await vi.advanceTimersByTimeAsync(5000) })
    expect(stop(), 'Stop remains present while the refreshed permission is unknown').toBe(button)
    expect(document.activeElement).toBe(button)
    if (outcome === 'permission removed') {
      rowExtra.canCancel = false
      await finish(pending[0], listing())
    } else await finish(pending[0], 'Control listing refused.', true, 403)
    expect(stop()).toBeNull()
    expect(document.activeElement).toBe(host.querySelector('h3'))
    await act(async () => button.click())
    expect(wires.some(wire => wire.action === 'cancelManagedSubagent')).toBe(false)
    expect(host.textContent).toContain(messages[1].content)
    assertParent()
  })

  it.each(['acknowledged', 'unconfirmed'])('retires %s Stop feedback only after validated status readback', async outcome => {
    await render()
    let holdReadback = true
    const bridge = window.cefQuery!
    window.cefQuery = query => {
      const wire = JSON.parse(query.request) as Wire
      if (holdReadback && wire.action === 'getManagedAgentTranscript') {
        wires.push(wire); pending.push({ wire, query, done: false })
      } else bridge(query)
    }
    await act(async () => stop()!.click())
    await finish(pending[0], outcome === 'acknowledged' ? { ok: true, runId } : 'Lost Stop acknowledgement.', outcome === 'unconfirmed')
    expect(pending[1].wire.action).toBe('getManagedAgentTranscript')
    expect(host.textContent).toContain(outcome === 'acknowledged' ? 'Stop requested. Reading' : 'Stop was not confirmed.')
    if (outcome === 'unconfirmed') expect(host.querySelector('[role="alert"]')?.textContent).toContain('Stop was not confirmed.')
    holdReadback = false
    status = outcome === 'acknowledged' ? 'cancelled' : 'running'
    await finish(pending[1], { runId, providerId: 'claude-cli', status, messages })
    expect(host.textContent, 'Validated status replaces transient Stop feedback').not.toContain('Stop requested. Reading')
    expect(host.textContent, 'Validated status replaces transient Stop feedback').not.toContain('Stop was not confirmed.')
    expect(host.querySelector('[role="alert"]')).toBeNull()
    expect(host.querySelector('nav')?.textContent).toContain(outcome === 'acknowledged' ? 'Stopped' : 'Running')
    expect(wires.filter(wire => wire.action === 'cancelManagedSubagent')).toHaveLength(1)
    assertParent()
  })

  it('ignores a Stop acknowledgement after returning to the parent without replay or draft loss', async () => {
    await render()
    await act(async () => stop()!.click())
    const reads = wires.length
    await act(async () => root.render(<div>Returned to parent</div>))
    await finish(pending[0], { ok: true, runId })
    expect(wires).toHaveLength(reads)
    expect(host.textContent).toBe('Returned to parent')
    assertParent()
  })

  it.each(['accepted', 'unconfirmed'])('finishes Stop readback when an earlier follow-up acknowledgement arrives while Stop is pending (%s)', async outcome => {
    status = 'completed'
    transcriptExtra.followup = false
    const predecessor = '9acd0525-14eb-46c0-beb2-2434a5e306b0'
    rowExtra.followup = { canSend: true, code: 'none', currentTurnId: predecessor, currentTurnNumber: 1 }
    const bridge = window.cefQuery!
    window.cefQuery = query => {
      const wire = JSON.parse(query.request) as Wire
      if (wire.action === 'submitManagedSubagentFollowup') {
        wires.push(wire); pending.push({ wire, query, done: false })
      } else bridge(query)
    }
    await render()
    await act(async () => [...host.querySelectorAll<HTMLButtonElement>('button')].find(control => control.textContent === 'Follow up')!.click())
    const field = host.querySelector<HTMLTextAreaElement>('textarea')!
    await act(async () => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')!.set!.call(field, 'Continue this child once.')
      field.dispatchEvent(new Event('input', { bubbles: true }))
    })
    await act(async () => [...host.querySelectorAll<HTMLButtonElement>('button')].find(control => control.textContent === 'Send follow-up')!.click())
    expect(pending).toHaveLength(1)
    expect(pending[0].wire.action).toBe('submitManagedSubagentFollowup')
    // The accepted turn becomes readable before its delivery acknowledgement arrives.
    status = 'running'
    rowExtra.followup = { canSend: false, code: 'busy', currentTurnId: predecessor, currentTurnNumber: 1 }
    await act(async () => refresh().click())
    await act(async () => stop()!.click())
    expect(pending).toHaveLength(2)
    expect(pending[1].wire.action).toBe('cancelManagedSubagent')
    const reads = wires.filter(wire => wire.action === 'getManagedAgentTranscript').length
    if (outcome === 'accepted') await finish(pending[0], { followup: { accepted: true, replayed: false, code: 'none', acceptance: {
        rootChatId: parentId, runId, transcriptChatId: transcriptId, clientRequestId: pending[0].wire.payload.clientRequestId,
        turnId: '537b7748-7916-4e72-b0df-ce22d3376ed6', turnNumber: 2, statusAtAdmission: 'queued',
      } } })
    else {
      await finish(pending[0], 'The earlier follow-up result is unknown.', true)
      const retry = [...host.querySelectorAll<HTMLButtonElement>('button')].find(control => control.textContent === 'Retry same request')!
      expect(retry).toBeDefined()
      expect(retry.getAttribute('aria-disabled')).toBe('true')
      await act(async () => retry.click())
      expect(wires.filter(wire => wire.action === 'submitManagedSubagentFollowup')).toHaveLength(1)
      expect(host.querySelector('textarea')).not.toBeNull()
    }
    expect(refresh().disabled).toBe(true)
    expect(wires.filter(wire => wire.action === 'getManagedAgentTranscript')).toHaveLength(reads)
    status = 'cancelled'
    await finish(pending[1], { ok: true, runId })
    expect(refresh().disabled, 'A late follow-up receipt must not strand the pending Stop').toBe(false)
    expect(stop()).toBeNull()
    expect(host.querySelector('nav')?.textContent).toContain('Stopped')
    expect(wires.filter(wire => wire.action === 'cancelManagedSubagent')).toHaveLength(1)
    expect(wires.filter(wire => wire.action === 'submitManagedSubagentFollowup')).toHaveLength(1)
    assertParent(['submitManagedSubagentFollowup'])
  })
})
