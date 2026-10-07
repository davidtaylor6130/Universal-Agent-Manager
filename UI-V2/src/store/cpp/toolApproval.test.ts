import { expect, it } from 'vitest'
import { sanitizeToolCall } from './sanitizers'
import { toolCallsEquivalent } from './reconcile'

it('retains approval separately and reconciles an approval-only tool update', () => {
  const running = sanitizeToolCall({
    id: 'bash-1', title: 'npm test', kind: 'execute', status: 'running', content: '',
  })!
  const approved = sanitizeToolCall({ ...running, approvalStatus: 'auto_approved' })!
  expect(approved.status).toBe('running')
  expect(approved.approvalStatus).toBe('auto_approved')
  expect(toolCallsEquivalent([running], [approved])).toBe(false)
  const completed = sanitizeToolCall({ ...approved, status: 'completed' })!
  expect(completed.kind).toBe('execute')
  expect(completed.approvalStatus).toBe('auto_approved')
  expect(toolCallsEquivalent([approved], [completed])).toBe(false)
})
