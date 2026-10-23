import type { MessageToolCall } from '../../types/message'

export type LaunchTool = Pick<MessageToolCall, 'id' | 'title' | 'kind' | 'status' | 'content' | 'approvalStatus' | 'isSubAgent' | 'contentDeferred' | 'managedLaunchRunId'>
const uuid = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i
// Exact frontend DTO tuples from Codex MCP, Copilot SDK and Claude tool projection.
// A known name plus an accepted result is presentation correlation, never input authority.
const launchKinds: Readonly<Record<string, readonly string[]>> = {
  agent_delegate: ['mcpToolCall', 'other', 'agent_delegate'],
  'uam-control.agent_delegate': ['mcpToolCall', 'other', 'uam-control.agent_delegate'],
  mcp__uam_control__agent_delegate: ['mcpToolCall', 'other', 'mcp__uam_control__agent_delegate'],
  'mcp__uam-control__agent_delegate': ['mcpToolCall', 'other', 'mcp__uam-control__agent_delegate'],
}
const object = (value: unknown): Record<string, unknown> | undefined => value !== null && typeof value === 'object' && !Array.isArray(value) ? value as Record<string, unknown> : undefined

/** Exact launch-result correlation only. This is a presentation key, never native-session authority. */
export function managedLaunchRunId(tool: LaunchTool): string | undefined {
  const parsed = managedLaunchRunIdFromContent(tool)
  if (!tool.contentDeferred) return parsed
  if (!Object.prototype.hasOwnProperty.call(launchKinds, tool.title) || !launchKinds[tool.title].includes(tool.kind) || tool.status !== 'completed') return undefined
  const projected = typeof tool.managedLaunchRunId === 'string' && uuid.test(tool.managedLaunchRunId) ? tool.managedLaunchRunId : undefined
  // Deferred pages carry a native-derived scalar instead of fetching all tool output.
  // If content is present, never let projection override contradictory/invalid evidence.
  if (tool.content.trim()) return parsed && (!projected || projected === parsed) ? parsed : undefined
  return projected
}

function managedLaunchRunIdFromContent(tool: LaunchTool): string | undefined {
  if (!Object.prototype.hasOwnProperty.call(launchKinds, tool.title) || !launchKinds[tool.title].includes(tool.kind) || tool.status !== 'completed' || tool.content.length > 65536) return undefined
  let text = tool.content.trim()
  if (text.startsWith('Approval: ')) {
    // Strip only this tool's exact native presentation prefix, before Arguments.
    const approval = tool.approvalStatus
    if (!approval || /[\r\n]/.test(approval)) return undefined
    const prefix = `Approval: ${approval}\n\n`
    if (!text.startsWith(prefix)) return undefined
    text = text.slice(prefix.length)
  }
  if (text.startsWith('Arguments:\n')) {
    const at = text.lastIndexOf('\n\nResult:\n')
    if (at < 0) return undefined
    text = text.slice(at + '\n\nResult:\n'.length)
  }
  try {
    let value: unknown = JSON.parse(text)
    for (let depth = 0; depth < 5; ++depth) {
      const row = object(value)
      if (!row || row.error || row.isError === true) return undefined
      if (row.server !== undefined && (row.server !== 'uam-control' || row.tool !== 'agent_delegate' || row.status !== 'completed')) return undefined
      if (typeof row.runId === 'string' && uuid.test(row.runId) && row.status === 'queued') return row.runId
      if (row.result !== undefined) { value = row.result; continue }
      if (Array.isArray(row.content) && row.content.length === 1) {
        const block = object(row.content[0])
        if (block?.type === 'text' && typeof block.text === 'string' && block.text.length <= 8192) { value = JSON.parse(block.text); continue }
      }
      return undefined
    }
  } catch { /* Keep malformed/deferred/failed responses as ordinary tool calls. */ }
  return undefined
}

const nativeLaunchNames = new Set(['task', 'subtask', 'delegate', 'delegate_to_agent', 'spawn_agent', 'multi_agent_v1.spawn_agent', 'functions.spawn_agent'])

export function isInlineSubagentLaunch(tool: LaunchTool): boolean {
  // isSubAgent also marks wait, follow-up and result events; it is not launch evidence.
  return Boolean(managedLaunchRunId(tool) || tool.isSubAgent && nativeLaunchNames.has(tool.title.trim().toLowerCase()))
}

export type OrderedSubagentEntry<T, U> = { kind: 'entry'; entry: T; index: number } | { kind: 'subagents'; tools: U[]; index: number }

/** Only adjacent tool events group; prose, thought, permission and plan events preserve their positions. */
export function groupInlineSubagents<T extends { type: string; toolCallId?: string }, U extends LaunchTool>(entries: readonly T[], tools: ReadonlyMap<string, U>): OrderedSubagentEntry<T, U>[] {
  const result: OrderedSubagentEntry<T, U>[] = []
  let group: { kind: 'subagents'; tools: U[]; index: number } | undefined
  const seen = new Set<string>()
  for (const [index, entry] of entries.entries()) {
    const tool = entry.type === 'tool_call' && entry.toolCallId ? tools.get(entry.toolCallId) : undefined
    if (tool && isInlineSubagentLaunch(tool) && !seen.has(tool.id)) {
      if (!group) { group = { kind: 'subagents', tools: [], index }; result.push(group) }
      group.tools.push(tool); seen.add(tool.id)
    } else {
      group = undefined
      result.push({ kind: 'entry', entry, index })
    }
  }
  return result
}
