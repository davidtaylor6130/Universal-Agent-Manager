import { describe, expect, it } from 'vitest'
import { groupInlineSubagents, managedLaunchRunId, type LaunchTool } from './inlineSubagents'

const id = '8509f0ba-ea06-4068-a6ed-3e1bab09bc53'
const launch = (toolId: string): LaunchTool => ({ id: toolId, title: 'agent_delegate', kind: 'mcpToolCall', status: 'completed', content: JSON.stringify({ runId: id, status: 'queued' }) })
const event = (toolCallId: string) => ({ type: 'tool_call', toolCallId })

describe('ordered subagent launches', () => {
  it('groups only consecutive launch events without moving earlier or later prose', () => {
    const tools = new Map(['a', 'b', 'c'].map(key => [key, launch(key)]))
    const before = { type: 'assistant_text', text: 'Before' }; const between = { type: 'assistant_text', text: 'Between' }; const after = { type: 'assistant_text', text: 'After' }
    const result = groupInlineSubagents([before, event('a'), event('b'), between, event('c'), after], tools)
    expect(result.map(row => row.kind === 'subagents' ? row.tools.map(tool => tool.id) : row.entry)).toEqual([before, ['a', 'b'], between, ['c'], after])
  })
  it.each(['thought', 'permission_request', 'user_input_request', 'plan', 'context_compaction', 'assistant_text'])('retains a %s boundary even if its presentation is collapsed', type => {
    const result = groupInlineSubagents([event('a'), { type }, event('b')], new Map(['a', 'b'].map(key => [key, launch(key)])))
    expect(result.map(row => row.kind)).toEqual(['subagents', 'entry', 'subagents'])
  })
  it('does not group over ordinary or missing tools and does not turn a repeated event into a second child', () => {
    const read = { ...launch('read'), title: 'read_file' }
    const result = groupInlineSubagents([event('a'), event('read'), event('b'), event('missing'), event('a')], new Map([['a', launch('a')], ['b', launch('b')], ['read', read]]))
    expect(result.filter(row => row.kind === 'subagents').map(row => row.kind === 'subagents' && row.tools.map(tool => tool.id))).toEqual([['a'], ['b']])
  })
  it('keeps provider-native children separate from canonical managed result events', () => {
    const native = { ...launch('native'), title: 'Task', isSubAgent: true }
    const result = { ...native, id: 'result', title: 'uam_agent_result' }
    expect(groupInlineSubagents([event('native'), event('result')], new Map([['native', native], ['result', result]])).map(row => row.kind)).toEqual(['subagents', 'entry'])
  })
})

describe('parity launch-only distinction', () => {
  it.each(['wait_agent', 'send_input', 'close_agent', 'agent_status', 'agent_cancel', 'uam_agent_result', 'Unclassified native event'])('keeps %s in its own ordinary slot even when isSubAgent is set', title => {
    const control = { ...launch('control'), title, isSubAgent: true }
    const result = groupInlineSubagents([event('a'), event('control'), event('b')], new Map([['a', launch('a')], ['control', control], ['b', launch('b')]]))
    expect(result.map(row => row.kind)).toEqual(['subagents', 'entry', 'subagents'])
  })
  it('accepts the UAM MCP server name with a hyphen without treating the UUID as a native session', () => {
    expect(managedLaunchRunId({ ...launch('hyphen'), title: 'mcp__uam-control__agent_delegate' })).toBe(id)
  })
})

describe('managed launch result identity', () => {
  it('accepts the actual structured MCP launch receipt and its wrapper', () => {
    const receipt = { runId: id, status: 'queued' }
    const wrapper = { server: 'uam-control', tool: 'agent_delegate', status: 'completed', result: { content: [{ type: 'text', text: JSON.stringify(receipt) }] } }
    expect(managedLaunchRunId({ ...launch('a'), content: `Arguments:\n{"task":"one"}\n\nResult:\n${JSON.stringify(wrapper)}` })).toBe(id)
    expect(managedLaunchRunId(launch('a'))).toBe(id)
  })
  it.each([
    { title: 'agent_status' }, { kind: 'read' }, { status: 'failed' }, { content: `pretend ${id}` },
    { content: JSON.stringify({ runId: id, status: 'running' }) },
    { content: JSON.stringify({ runId: 'native-session-id', status: 'queued' }) },
    { content: JSON.stringify({ server: 'other', runId: id, status: 'queued' }) },
    { content: JSON.stringify({ error: 'refused', runId: id, status: 'queued' }) },
    { content: JSON.stringify({ server: 'other', tool: 'agent_delegate', status: 'completed', result: { runId: id, status: 'queued' } }) },
    { content: ' '.repeat(65537) },
  ])('rejects failed, unstructured, foreign or oversized evidence %#', change => {
    expect(managedLaunchRunId({ ...launch('a'), ...change })).toBeUndefined()
  })
})

// These DTOs match current native adapter projection + StateSerializer, not a live-provider run.
describe('source-shaped managed launch projections', () => {
  const accepted = { runId: id, status: 'queued' }
  const projections: { label: string; tool: LaunchTool }[] = [
    { label: 'Codex mcpToolCall full item', tool: { ...launch('codex'), title: 'agent_delegate', kind: 'mcpToolCall',
      content: JSON.stringify({ type: 'mcpToolCall', server: 'uam-control', tool: 'agent_delegate', status: 'completed',
        result: { content: [{ type: 'text', text: JSON.stringify(accepted) }] } }) } },
    { label: 'Claude exact MCP tool kind', tool: { ...launch('claude'), title: 'mcp__uam-control__agent_delegate', kind: 'mcp__uam-control__agent_delegate',
      content: `Arguments:\n{"agentId":"build","task":"Private fixture task"}\n\nResult:\n${JSON.stringify(accepted)}` } },
    { label: 'Copilot SDK other tool kind', tool: { ...launch('copilot'), title: 'agent_delegate', kind: 'other', content: `Arguments:\n{"agentId":"build"}\n\nResult:\n${JSON.stringify(accepted)}` } },
  ]
  it.each(projections)('correlates $label to the exact accepted run', ({ tool }) => {
    expect(managedLaunchRunId(tool)).toBe(id)
    expect(groupInlineSubagents([event(tool.id)], new Map([[tool.id, tool]]))).toEqual([{ kind: 'subagents', tools: [tool], index: 0 }])
  })
  it.each(projections)('keeps the existing result and status fences for $label', ({ tool }) => {
    for (const change of [
      { status: 'running' }, { status: 'failed' }, { content: '' },
      { content: JSON.stringify({ runId: id, status: 'running' }) },
      { content: JSON.stringify({ error: 'Refused', runId: id, status: 'queued' }) },
      { content: JSON.stringify({ server: 'foreign', tool: 'agent_delegate', status: 'completed', result: accepted }) },
      { content: JSON.stringify({ runId: 'native-not-a-managed-run', status: 'queued' }) },
    ]) expect(managedLaunchRunId({ ...tool, ...change })).toBeUndefined()
  })
  it.each([
    { title: 'agent_delegate', kind: 'execute' },
    { title: 'agent_delegate', kind: 'mcp__uam-control__agent_delegate' },
    { title: 'mcp__uam-control__agent_delegate', kind: 'agent_delegate' },
    { title: 'agent_status', kind: 'other' },
    { title: 'mcp__foreign__agent_delegate', kind: 'mcp__foreign__agent_delegate' },
    { title: 'Review private task', kind: 'other' },
    { title: '__proto__', kind: 'other' },
  ])('refuses an unknown or mismatched tuple $title / $kind', tuple => {
    expect(managedLaunchRunId({ ...launch('wrong-tuple'), ...tuple })).toBeUndefined()
  })
})

describe('native child kind is not a fresh launch classifier', () => {
  it.each(['Review selected source', 'wait_agent', 'uam_agent_result'])('retains %s as an ordinary event despite native child metadata', title => {
    const activity = { ...launch('native-activity'), title, kind: 'sub-agent', isSubAgent: true, content: '' }
    expect(groupInlineSubagents([event('a'), event(activity.id), event('b')], new Map([['a', launch('a')], [activity.id, activity], ['b', launch('b')]])).map(row => row.kind)).toEqual(['subagents', 'entry', 'subagents'])
  })
})


// These source-shaped composed strings are candidate unit fixtures. Actual fixed
// serializer output is joined separately after an authorized native GREEN run.
describe('approval-bearing exact MCP launch content', () => {
  const qualified = { ...launch('approved'), title: 'mcp__uam-control__agent_delegate', kind: 'mcp__uam-control__agent_delegate', approvalStatus: 'auto_approved' }
  const result = JSON.stringify({ runId: id, status: 'queued' })
  const composed = `Approval: auto_approved\n\nArguments:\n{"agentId":"plan","task":"Synthetic approval fixture"}\n\nResult:\n${result}`
  it('accepts the native approval prefix before Arguments while preserving the original details string', () => {
    const tool = { ...qualified, content: composed }
    expect(managedLaunchRunId(tool)).toBe(id)
    expect(tool.content).toBe(composed)
    expect(groupInlineSubagents([event(tool.id)], new Map([[tool.id, tool]]))).toEqual([{ kind: 'subagents', tools: [tool], index: 0 }])
    expect(managedLaunchRunId({ ...qualified, content: `Approval: auto_approved\n\n${result}` })).toBe(id)
  })
  it.each([
    { approvalStatus: undefined }, { approvalStatus: 'manually_approved' }, { approvalStatus: 'auto_approved\nforged' },
    { title: 'mcp__foreign__agent_delegate' }, { kind: 'read' }, { status: 'failed' },
    { content: composed.replace('status":"queued', 'status":"running') },
    { content: composed.replace('Approval: auto_approved\n\n', 'Approval: auto_approved\n') },
    { content: `Approval: auto_approved\n\n${JSON.stringify({ server: 'foreign', tool: 'agent_delegate', status: 'completed', result: { runId: id, status: 'queued' } })}` },
  ])('rejects mismatched approval metadata or unproven receipt %#', change => {
    expect(managedLaunchRunId({ ...qualified, content: composed, ...change })).toBeUndefined()
  })
})

describe('matching native launch parser boundary matrix', () => {
  const name = 'mcp__uam-control__agent_delegate'
  const receipt = { runId: id, status: 'queued' }
  const body = JSON.stringify(receipt)
  const cases: { label: string; content: string; kind: string; accepted: boolean }[] = []
  for (const size of [8192, 8193]) cases.push({ label: `inner-${size}`, content: JSON.stringify({ content: [{ type: 'text', text: body.padEnd(size, ' ') }] }), kind: name, accepted: size === 8192 })
  for (const size of [65536, 65537]) cases.push({ label: `total-${size}`, content: body.padEnd(size, ' '), kind: name, accepted: size === 65536 })
  for (const wrappers of [4, 5]) {
    let value: unknown = receipt
    for (let depth = 0; depth < wrappers; ++depth) value = { result: value }
    cases.push({ label: `result-wrappers-${wrappers}`, content: JSON.stringify(value), kind: name, accepted: wrappers === 4 })
  }
  cases.push({ label: 'kind-equals-qualified-name', content: body, kind: name, accepted: true })
  cases.push({ label: 'kind-mismatches-qualified-name', content: body, kind: 'agent_delegate', accepted: false })
  const errors = [null, false, '', 0, 0.0, true, 'refused', 1, {}, []]
  errors.forEach((error, index) => cases.push({ label: `error-truthiness-${index}`, content: JSON.stringify({ ...receipt, error }), kind: name, accepted: index < 5 }))
  it.each(cases)('$label', ({ content, kind, accepted }) => {
    expect(managedLaunchRunId({ ...launch('boundary'), title: name, kind, content })).toBe(accepted ? id : undefined)
  })
})
