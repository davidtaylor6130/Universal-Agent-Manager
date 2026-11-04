import { describe, expect, it } from 'vitest'
import { formatReadableToolOutput } from './toolOutputFormat'

describe('formatReadableToolOutput', () => {
  it('renders nested objects as key: value lines with two-space indent', () => {
    expect(formatReadableToolOutput('{"name":"build","ok":true,"exit":null,"meta":{"path":"a.ts","size":12}}'))
      .toBe('name: build\nok: yes\nexit: none\nmeta:\n  path: a.ts\n  size: 12')
  })

  it('renders arrays as dash items, including objects inside arrays', () => {
    expect(formatReadableToolOutput('["a",{"id":1,"done":false},[]]'))
      .toBe('- a\n- id: 1\n  done: no\n- none')
  })

  it('shows empty containers as none and returns null for non-JSON text', () => {
    expect(formatReadableToolOutput('{}')).toBe('none')
    expect(formatReadableToolOutput('{"files":[]}')).toBe('files: none')
    expect(formatReadableToolOutput('plain output')).toBeNull()
    expect(formatReadableToolOutput('"just a string"')).toBeNull()
    expect(formatReadableToolOutput('{"broken":')).toBeNull()
  })
})
