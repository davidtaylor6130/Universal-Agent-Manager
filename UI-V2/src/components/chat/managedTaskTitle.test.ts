import { describe, expect, it } from 'vitest'
import { boundedTaskTitle, managedTaskTitle, GENERIC_SUBAGENT_TITLE } from './managedSubagents'

describe('managed task title presentation', () => {
  it('falls back to a generic label, never to the agent definition', () => {
    expect(managedTaskTitle({})).toBe(GENERIC_SUBAGENT_TITLE)
    expect(managedTaskTitle({ taskTitle: '' })).toBe(GENERIC_SUBAGENT_TITLE)
    expect(managedTaskTitle({ taskTitle: 'Audit the parser' })).toBe('Audit the parser')
  })
  it('accepts only text, collapses whitespace and caps at 100 code points and 256 bytes without splitting a character', () => {
    expect(boundedTaskTitle(7)).toBeUndefined()
    expect(boundedTaskTitle(' \n\t ')).toBeUndefined()
    expect(boundedTaskTitle('  Audit\u0000 the\n parser  ')).toBe('Audit the parser')
    expect(boundedTaskTitle('a'.repeat(500))).toBe('a'.repeat(100))
    expect(Array.from(boundedTaskTitle('😀'.repeat(100))!)).toHaveLength(64)
    expect(new TextEncoder().encode(boundedTaskTitle('é'.repeat(200))!).length).toBeLessThanOrEqual(256)
  })
})
