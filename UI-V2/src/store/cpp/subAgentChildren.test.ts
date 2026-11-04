import { describe, expect, it } from 'vitest'
import { buildMessageFromCpp, reconcileCppMessages, toolCallsEquivalent } from './reconcile'
import { sanitizeCppAcpSession, sanitizeCppMessage, sanitizeToolCall } from './sanitizers'

const tool = {
  id: 'collab-native', title: 'wait', kind: 'collabAgentToolCall', status: 'completed', content: '',
  isSubAgent: true, subAgentId: 'native-first', subAgentTitle: 'Legacy title',
  subAgentChildren: [{ id: 'native-first', status: 'completed' }, { id: 'native-second', title: 'Reviewer', status: 'running' }],
}

describe('native subagent child data', () => {
  it('retains every receiver in live and persisted state without borrowing parent status', () => {
    const children = [...tool.subAgentChildren, { id: 'native-third' }]
    expect(sanitizeCppAcpSession({ toolCalls: [{ ...tool, subAgentChildren: children }] })?.toolCalls[0].subAgentChildren).toEqual(children)
    const message = sanitizeCppMessage({ role: 'assistant', content: '', toolCalls: [{ ...tool, subAgentChildren: children }] })!
    const reconciled = reconcileCppMessages('parent', undefined, [message])
    expect(reconciled[0].toolCalls?.[0].subAgentChildren).toEqual(children)
    expect(reconciled[0].toolCalls?.[0].subAgentChildren?.[2].status).toBeUndefined()
  })

  it('drops malformed optional arrays without losing legacy child links', () => {
    for (const subAgentChildren of [undefined, null, 42, 'children', { id: 'native-first' }]) {
      expect(sanitizeToolCall({ ...tool, subAgentChildren })).toMatchObject({ subAgentId: 'native-first', subAgentChildren: undefined })
    }
    expect(sanitizeToolCall({ ...tool, subAgentChildren: [null, 42, 'child', {}, { id: false }, { id: ' ' },
      { id: 'native-first', title: 42, status: { running: true } }, { id: 'native-first', status: 'running' },
      { id: 'native-second', title: 'Reviewer', status: 'pendingInit' }] })?.subAgentChildren).toEqual([
      { id: 'native-first' }, { id: 'native-second', title: 'Reviewer', status: 'pendingInit' },
    ])
  })

  it('reconciles child-only identity, title, state, count and order changes', () => {
    const original = sanitizeToolCall(tool)!
    for (const children of [
      [{ ...tool.subAgentChildren[0], id: 'native-new' }, tool.subAgentChildren[1]],
      [tool.subAgentChildren[0], { ...tool.subAgentChildren[1], title: 'New title' }],
      [tool.subAgentChildren[0], { ...tool.subAgentChildren[1], status: 'completed' }],
      [tool.subAgentChildren[0]],
      [...tool.subAgentChildren].reverse(),
    ]) {
      const changed = sanitizeToolCall({ ...tool, subAgentChildren: children })!
      expect(toolCallsEquivalent([original], [changed])).toBe(false)
      const previous = reconcileCppMessages('parent', undefined, [sanitizeCppMessage({ role: 'assistant', content: '', toolCalls: [tool] })!])
      const next = reconcileCppMessages('parent', previous, [sanitizeCppMessage({ role: 'assistant', content: '', toolCalls: [changed] })!])
      expect(next[0]).not.toBe(previous[0])
      expect(next[0].toolCalls?.[0].subAgentChildren).toEqual(children)
    }
    expect(toolCallsEquivalent([original], [sanitizeToolCall(tool)!])).toBe(true)
    const legacy = sanitizeToolCall({ ...tool, subAgentChildren: undefined })!
    expect(toolCallsEquivalent([legacy], [{ ...legacy, subAgentChildren: [] }])).toBe(true)
  })
})


it('retains only a bounded optional output projection through live and saved-tool sanitizers', () => {
  const subAgentResultContent = JSON.stringify({ runId: 'run-output', providerId: 'claude-cli', status: 'completed', result: 'Saved output' })
  const canonical = { ...tool, title: 'uam_agent_result', subAgentId: 'run-output', subAgentResultContent, contentDeferred: true }
  expect(sanitizeToolCall(canonical)?.subAgentResultContent).toBe(subAgentResultContent)
  expect(sanitizeCppAcpSession({ toolCalls: [canonical] })?.toolCalls[0].subAgentResultContent).toBe(subAgentResultContent)
  const message = sanitizeCppMessage({ role: 'assistant', content: '', toolCalls: [canonical] })!
  expect(reconcileCppMessages('parent', undefined, [message])[0].toolCalls?.[0].subAgentResultContent).toBe(subAgentResultContent)
  for (const invalid of [undefined, null, 42, {}, 'x'.repeat(8193)]) expect(sanitizeToolCall({ ...canonical, subAgentResultContent: invalid })?.subAgentResultContent).toBeUndefined()
  expect(sanitizeToolCall({ ...canonical, subAgentResultContent: 'x'.repeat(8192) })?.subAgentResultContent).toHaveLength(8192)
})

it('refreshes a saved message when only canonical output metadata changes or disappears', () => {
  const projected = (result: string) => ({ ...tool, title: 'uam_agent_result', subAgentId: 'run-output', contentDeferred: true,
    subAgentResultContent: JSON.stringify({ runId: 'run-output', status: 'completed', result }) })
  const first = projected('First output'); const changed = projected('Refreshed output')
  expect(toolCallsEquivalent([first], [changed])).toBe(false)
  const message = (value: typeof first) => ({ role: 'assistant' as const, content: '', createdAt: '2026-10-16T10:00:00Z', toolCalls: [value] })
  const previous = reconcileCppMessages('parent', undefined, [message(first)])
  expect(reconcileCppMessages('parent', previous, [message(first)])[0]).toBe(previous[0])
  const next = reconcileCppMessages('parent', previous, [message(changed)])
  expect(next[0]).not.toBe(previous[0])
  expect(next[0].toolCalls?.[0].subAgentResultContent).toBe(changed.subAgentResultContent)
  const removed = { ...changed, subAgentResultContent: undefined }
  expect(toolCallsEquivalent([changed], [removed])).toBe(false)
  expect(reconcileCppMessages('parent', next, [message(removed as unknown as typeof first)])[0]).not.toBe(next[0])
})


it('normalizes a real deferred history page at buildMessageFromCpp without losing metadata or repeatedly replacing it', () => {
  const rawTool = { id: 'deferred-ingress', title: 'uam_agent_result', kind: 'sub-agent', status: 'completed', contentDeferred: true, contentDigest: 'saved-digest',
    isSubAgent: true, subAgentId: 'run-ingress', subAgentTitle: 'Ingress worker', currentActivity: 'Recorded activity',
    subAgentChildren: [{ id: 'run-ingress', status: 'completed' }], subAgentResultContent: JSON.stringify({ runId: 'run-ingress', result: 'Saved output' }) }
  const raw = { role: 'assistant', content: '', createdAt: '2026-10-16T10:00:00Z', toolCalls: [rawTool] } as unknown as Parameters<typeof buildMessageFromCpp>[1]
  const built = buildMessageFromCpp('parent', raw, 0)
  expect(built.toolCalls?.[0].content).toBe('')
  expect(built.toolCalls?.[0]).toMatchObject(rawTool)
  expect(rawTool).not.toHaveProperty('content')
  const previous = reconcileCppMessages('parent', undefined, [raw])
  expect(reconcileCppMessages('parent', previous, [raw])).toBe(previous)
  expect(toolCallsEquivalent(previous[0].toolCalls!, [{ ...previous[0].toolCalls![0], content: undefined } as unknown as typeof tool])).toBe(true)
})

it('retains exact native child timing through live saved and deferred message ingress', () => {
  const children = [
    { id: 'copilot-child-a', status: 'completed', startedAtEpochMs: 100000, finishedAtEpochMs: 109000, durationMs: 9000 },
    { id: 'copilot-child-b', status: 'failed', startedAtEpochMs: 200000, finishedAtEpochMs: 221000, durationMs: 21000 },
    { id: 'copilot-child-unknown', status: 'completed' },
  ]
  const timed = { ...tool, subAgentId: children[0].id, subAgentChildren: children }
  expect.soft(sanitizeCppAcpSession({ toolCalls: [timed] })?.toolCalls[0].subAgentChildren).toEqual(children)
  const saved = sanitizeCppMessage({ role: 'assistant', content: '', toolCalls: [timed] })!
  expect.soft(reconcileCppMessages('native-timing-parent', undefined, [saved])[0].toolCalls?.[0].subAgentChildren).toEqual(children)
  const raw = { role: 'assistant', content: '', createdAt: '2026-10-17T12:00:00Z', toolCalls: [{ ...timed, content: undefined, contentDeferred: true, contentDigest: 'native-timing' }] } as unknown as Parameters<typeof buildMessageFromCpp>[1]
  expect.soft(buildMessageFromCpp('native-timing-parent', raw, 0).toolCalls?.[0].subAgentChildren).toEqual(children)
})

it('rejects malformed child clock fields without losing exact child identity or inventing timing for siblings', () => {
  for (const invalid of [-1, 0.5, Number.MAX_SAFE_INTEGER + 1, Infinity, NaN, null, '9000', true, {}]) {
    const children = [{ id: 'native-invalid-clock', status: 'completed', startedAtEpochMs: invalid, finishedAtEpochMs: invalid, durationMs: invalid }, { id: 'native-unknown-clock', status: 'running' }]
    expect(sanitizeToolCall({ ...tool, subAgentChildren: children })?.subAgentChildren).toEqual([
      { id: 'native-invalid-clock', status: 'completed' }, { id: 'native-unknown-clock', status: 'running' },
    ])
  }
  const zero = [{ id: 'native-zero-duration', status: 'completed', startedAtEpochMs: 100000, finishedAtEpochMs: 100000, durationMs: 0 }]
  expect(sanitizeToolCall({ ...tool, subAgentChildren: zero })?.subAgentChildren).toEqual(zero)
})

it('reconciles each native child clock change and removal while retaining identical snapshots', () => {
  const child = { id: 'native-reconciled-clock', status: 'completed', startedAtEpochMs: 100000, finishedAtEpochMs: 109000, durationMs: 9000 }
  const rawTool = { ...tool, subAgentChildren: [child] }
  const message = (value: typeof rawTool) => sanitizeCppMessage({ role: 'assistant', content: '', createdAt: '2026-10-17T12:00:00Z', toolCalls: [value] })!
  const previous = reconcileCppMessages('native-clock-reconcile', undefined, [message(rawTool)])
  expect(reconcileCppMessages('native-clock-reconcile', previous, [message(rawTool)])).toBe(previous)
  for (const field of ['startedAtEpochMs', 'finishedAtEpochMs', 'durationMs'] as const) {
    const changed = { ...rawTool, subAgentChildren: [{ ...child, [field]: child[field] + 1000 }] }
    const next = reconcileCppMessages('native-clock-reconcile', previous, [message(changed)])
    expect.soft(toolCallsEquivalent([sanitizeToolCall(rawTool)!], [sanitizeToolCall(changed)!])).toBe(false)
    expect.soft(next[0]).not.toBe(previous[0])
    expect.soft(next[0].toolCalls?.[0].subAgentChildren).toEqual(changed.subAgentChildren)
    const removedChild: Partial<typeof child> & Pick<typeof child, 'id' | 'status'> = { ...child }
    delete removedChild[field]
    const removed = { ...rawTool, subAgentChildren: [removedChild] } as typeof rawTool
    expect.soft(toolCallsEquivalent([sanitizeToolCall(rawTool)!], [sanitizeToolCall(removed)!])).toBe(false)
    expect.soft(reconcileCppMessages('native-clock-reconcile', previous, [message(removed)])[0]).not.toBe(previous[0])
  }
})
