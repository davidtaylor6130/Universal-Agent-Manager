import { describe, expect, it } from 'vitest'
import { compactCommandInstructions } from './compactCommand'

describe('compactCommandInstructions', () => {
  it('reads a bare command as no instructions', () => {
    expect(compactCommandInstructions('/compact')).toBe('')
    expect(compactCommandInstructions('  /compact  ')).toBe('')
  })

  it('returns the trimmed text after the command, including later lines', () => {
    expect(compactCommandInstructions('/compact keep the API notes')).toBe('keep the API notes')
    expect(compactCommandInstructions('/compact\n  keep the API notes\nand the schema  ')).toBe('keep the API notes\nand the schema')
  })

  it('rejects other text, longer command names and commands that do not lead', () => {
    expect(compactCommandInstructions('/compactor')).toBeNull()
    expect(compactCommandInstructions('/compact/now')).toBeNull()
    expect(compactCommandInstructions('please /compact')).toBeNull()
    expect(compactCommandInstructions('/Compact')).toBeNull()
    expect(compactCommandInstructions('')).toBeNull()
  })
})
