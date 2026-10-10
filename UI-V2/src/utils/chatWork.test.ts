import { describe, expect, it } from 'vitest'
import type { AcpBinding, CliBinding } from '../store/cpp/types'
import type { Goal } from '../types/goal'
import { chatHasUnfinishedWork } from './chatWork'

describe('finished-chat review gating', () => {
  const acp = { processing: false, lifecycleState: 'ready', backgroundTaskCount: 0 } as AcpBinding
  it.each([
    { processing: true }, { lifecycleState: 'starting' }, { lifecycleState: 'waitingPermission' },
    { lifecycleState: 'waitingUserInput' }, { backgroundTaskCount: 1 },
    { queuedPrompts: [{}], queueHeld: false },
  ])('keeps work hidden while %j', patch => {
    expect(chatHasUnfinishedWork({ ...acp, ...patch } as AcpBinding)).toBe(true)
  })
  it('keeps idle persistent processes and held queues reviewable', () => {
    expect(chatHasUnfinishedWork({ ...acp, running: true, queuedPrompts: [{}], queueHeld: true } as AcpBinding)).toBe(false)
  })
  it('covers CLI startup, busy turns and shutdown', () => {
    for (const lifecycleState of ['starting', 'busy', 'shuttingDown']) {
      expect(chatHasUnfinishedWork(acp, { lifecycleState } as CliBinding)).toBe(true)
    }
    expect(chatHasUnfinishedWork(acp, { lifecycleState: 'idle', running: true, turnState: 'idle' } as CliBinding)).toBe(false)
  })
  it('keeps every active goal unfinished until it is paused, blocked or complete', () => {
    const goal = { status: 'active', executionOwner: 'provider' } as Goal
    expect(chatHasUnfinishedWork(acp, undefined, goal)).toBe(true)
    expect(chatHasUnfinishedWork(acp, undefined, { ...goal, pendingContinuation: true })).toBe(true)
    expect(chatHasUnfinishedWork(acp, undefined, { ...goal, executionOwner: 'uam' })).toBe(true)
    for (const status of ['paused', 'blocked', 'complete'] as const) {
      expect(chatHasUnfinishedWork(acp, undefined, { ...goal, status, pendingContinuation: true })).toBe(false)
    }
  })
})
