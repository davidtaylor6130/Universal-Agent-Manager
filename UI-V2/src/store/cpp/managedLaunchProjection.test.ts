import { describe, expect, it } from 'vitest'
import { sanitizeToolCall } from './sanitizers'
import { buildMessageFromCpp, reconcileCppMessages, toolCallsEquivalent } from './reconcile'
import { managedLaunchRunId } from '../../components/chat/inlineSubagents'
import nativeRedPage from './fixtures/deferredManagedLaunchPage.native-red.json'
import nativeGreenPage from './fixtures/deferredManagedLaunchPage.native-green.json'

const id = '8509f0ba-ea06-4068-a6ed-3e1bab09bc53'
const other = '36491b65-5598-42cf-bbb8-22fc00467bd6'
// Boundary-only DTOs, not claimed native exports. Actual serializer fixture is a
// separate connected test, frozen only after the authorized native run emits it.
const deferred = { id: 'saved-launch', title: 'agent_delegate', kind: 'mcpToolCall', status: 'completed', contentDeferred: true, contentDigest: 'fixture-digest', managedLaunchRunId: id }
const rawMessage = (tool: object) => ({ role: 'assistant' as const, content: '', createdAt: '2026-10-20T19:00:00.000Z', toolCalls: [tool as never], blocks: [{ type: 'tool_call' as const, toolCallId: deferred.id }] })

describe('native-derived deferred launch projection transport', () => {
  it('retains the actual pre-fix native export as unclassified deferred content', () => {
    const exportedTool = nativeRedPage.messages[0].toolCalls[0]
    expect(exportedTool).not.toHaveProperty('content')
    expect(exportedTool).not.toHaveProperty('managedLaunchRunId')
    const tool = sanitizeToolCall(exportedTool)!
    expect(tool.content).toBe('')
    expect(tool.contentDeferred).toBe(true)
    expect(managedLaunchRunId(tool)).toBeUndefined()
  })
  it('passes exact GREEN native-export bytes through the actual boundary without adding correlation', () => {
    const exportedTool = nativeGreenPage.messages[0].toolCalls[0]
    expect(exportedTool).not.toHaveProperty('content')
    expect(exportedTool).toHaveProperty('managedLaunchRunId', id)
    const tool = sanitizeToolCall(exportedTool)!
    expect(tool.content).toBe('')
    expect(tool.contentDeferred).toBe(true)
    expect(tool.contentDigest).toBe(exportedTool.contentDigest)
    expect(managedLaunchRunId(tool)).toBe(id)
    const saved = buildMessageFromCpp(nativeGreenPage.chatId, nativeGreenPage.messages[0], nativeGreenPage.startIndex)
    expect(saved.toolCalls?.[0]).toEqual(tool)
  })
  it('preserves a bounded output-only scalar through sanitation and lazy reconciliation', () => {
    const tool = sanitizeToolCall(deferred)!
    expect(tool.content).toBe('')
    expect(tool.contentDeferred).toBe(true)
    expect(managedLaunchRunId(tool)).toBe(id)
    expect(buildMessageFromCpp('parent', rawMessage(deferred), 80).toolCalls?.[0]).toEqual(tool)
  })
  it.each([undefined, null, 1, {}, '', ' '+id, id+' ', 'not-a-run', 'x'.repeat(65537)])('rejects malformed projected scalars %#', value => {
    const tool = sanitizeToolCall({ ...deferred, managedLaunchRunId: value })!
    expect(tool.managedLaunchRunId).toBeUndefined()
    expect(managedLaunchRunId(tool)).toBeUndefined()
  })
  it.each([{ title: 'agent_wait' }, { title: 'agent_status' }, { title: 'agent_cancel' }, { title: 'foreign.agent_delegate' }, { kind: 'read' }, { status: 'failed' }])('never promotes an unrelated or failed operation %#', change => {
    expect(managedLaunchRunId(sanitizeToolCall({ ...deferred, ...change })!)).toBeUndefined()
  })
  it('uses existing content classification without projection and fails closed on contradictory content', () => {
    const content = JSON.stringify({ runId: id, status: 'queued' })
    expect(managedLaunchRunId(sanitizeToolCall({ ...deferred, contentDeferred: false, managedLaunchRunId: other, content })!)).toBe(id)
    expect(managedLaunchRunId(sanitizeToolCall({ ...deferred, content, managedLaunchRunId: other })!)).toBeUndefined()
    expect(managedLaunchRunId(sanitizeToolCall({ ...deferred, content: 'foreign or malformed receipt' })!)).toBeUndefined()
    expect(managedLaunchRunId(sanitizeToolCall({ ...deferred, contentDeferred: false })!)).toBeUndefined()
  })
  it('invalidates projection arrival, replacement and removal without retaining a prior live link', () => {
    const legacy = sanitizeToolCall({ ...deferred, managedLaunchRunId: undefined })!
    const linked = sanitizeToolCall(deferred)!
    expect(toolCallsEquivalent([legacy], [linked])).toBe(false)
    expect(toolCallsEquivalent([linked], [sanitizeToolCall({ ...deferred, managedLaunchRunId: other })!])).toBe(false)
    expect(toolCallsEquivalent([linked], [legacy])).toBe(false)
    const before = reconcileCppMessages('parent', undefined, [rawMessage(deferred)], true)
    const changed = reconcileCppMessages('parent', before, [rawMessage({ ...deferred, managedLaunchRunId: other })], true)
    expect(changed[0]).not.toBe(before[0])
    expect(changed[0].toolCalls?.[0].managedLaunchRunId).toBe(other)
    const cleared = reconcileCppMessages('parent', changed, [rawMessage({ ...deferred, managedLaunchRunId: undefined })], true)
    expect(cleared[0]).not.toBe(changed[0])
    expect(cleared[0].toolCalls?.[0].managedLaunchRunId).toBeUndefined()
    expect(managedLaunchRunId(cleared[0].toolCalls![0])).toBeUndefined()
  })
})
