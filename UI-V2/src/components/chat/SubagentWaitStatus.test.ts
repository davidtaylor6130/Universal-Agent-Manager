import { describe, expect, it } from 'vitest'
import type { AcpBinding } from '../../store/cpp/types'
import { isWaitingOnSubagents } from './SubagentWaitStatus'

const child = { id: 'child', title: 'Review code', kind: 'sub-agent', status: 'running', content: '', isSubAgent: true }
const binding = { running: true, processing: false, lifecycleState: 'ready', backgroundTaskCount: 1, pendingPermission: null,
  pendingUserInput: null, toolCalls: [child] } as AcpBinding

describe('Parent waiting on subagents', () => {
  it('shows idle-parent waits and explicit Codex collaboration waits', () => {
    expect(isWaitingOnSubagents(binding)).toBe(true)
    expect(isWaitingOnSubagents({ ...binding, processing: true, toolCalls: [{ ...child, kind: 'collabAgentToolCall', title: 'wait' }] })).toBe(true)
  })

  it('clears on completion, stop, failure, input requests and resumed parent work', () => {
    expect(isWaitingOnSubagents({ ...binding, toolCalls: [{ ...child, status: 'completed' }] })).toBe(false)
    expect(isWaitingOnSubagents({ ...binding, running: false })).toBe(false)
    expect(isWaitingOnSubagents({ ...binding, lifecycleState: 'error' })).toBe(false)
    expect(isWaitingOnSubagents({ ...binding, processing: true })).toBe(false)
    expect(isWaitingOnSubagents({ ...binding, pendingUserInput: { requestId: 'q', itemId: 'q', status: 'pending', questions: [] } })).toBe(false)
  })

  it('does not label ordinary background commands or newly launched children as a parent wait', () => {
    expect(isWaitingOnSubagents({ ...binding, toolCalls: [{ ...child, isSubAgent: false, kind: 'shell' }] })).toBe(false)
    expect(isWaitingOnSubagents({ ...binding, processing: true, toolCalls: [{ ...child, kind: 'collabAgentToolCall', title: 'spawn_agent' }] })).toBe(false)
    expect(isWaitingOnSubagents(undefined)).toBe(false)
  })
})
