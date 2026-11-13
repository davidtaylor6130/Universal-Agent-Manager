import { describe, expect, it } from 'vitest'
import { formatReadableToolOutput, redactSecretRequestTool } from './toolOutputFormat'

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

describe('redactSecretRequestTool', () => {
  const args = '{"label":"GitHub token","reason":"To push the tag.","placeholder":"ghp_PLACEHOLDER_ARG","url":"https://github.com/settings/tokens"}'
  const tool = (title: string, status: string, result: string) => ({ id: 't', kind: 'mcpToolCall', title, status, content: `Arguments:\n${args}\n\nResult:\n${result}` })

  it('summarises every provider name as the label and outcome without other arguments or the path', () => {
    for (const title of ['secret_request', 'uam-control.secret_request', 'uam-control_secret_request', 'mcp__uam-control__secret_request', 'mcp__uam_control__secret_request']) {
      const shown = redactSecretRequestTool(tool(title, 'completed', '{"path":"/tmp/uam-secret-PATH"}'))
      expect(shown.title).toBe('Secret: GitHub token · provided as a one-read path')
      expect(shown.content).toBe('Label: GitHub token\nReason: To push the tag.\nOutcome: provided as a one-read path')
      expect(JSON.stringify(shown)).not.toMatch(/PLACEHOLDER_ARG|settings\/tokens|uam-secret-PATH/)
    }
  })

  it('reports cancelled, errors and waiting, and leaves other tools alone', () => {
    expect(redactSecretRequestTool(tool('secret_request', 'completed', '{"content":[{"type":"text","text":"User cancelled."}]}')).title).toBe('Secret: GitHub token · cancelled')
    expect(redactSecretRequestTool(tool('secret_request', 'failed', '{"error":"Host is not connected."}')).title).toBe('Secret: GitHub token · Host is not connected.')
    expect(redactSecretRequestTool(tool('secret_request', 'running', '')).title).toBe('Secret: GitHub token')
    expect(redactSecretRequestTool({ ...tool('secret_request', 'completed', ''), contentDeferred: true }).content).toBe('Outcome: provided as a one-read path')
    const other = tool('user_question', 'completed', '{}')
    expect(redactSecretRequestTool(other)).toBe(other)
  })
})
