import type { AcpBinding } from '../../store/cpp/types'

const ACTIVE = new Set(['pending', 'running', 'in_progress', 'inProgress'])

/** Only an explicit collaboration wait or live children after an idle parent means waiting. */
export function isWaitingOnSubagents(binding?: AcpBinding): boolean {
  if (!binding?.running || binding.lifecycleState === 'error' || binding.pendingPermission || binding.pendingUserInput) return false
  const children = binding.toolCalls.filter((tool) => tool.isSubAgent && ACTIVE.has(tool.status))
  return children.some((tool) => tool.kind === 'collabAgentToolCall' && tool.title === 'wait') ||
    (!binding.processing && (binding.backgroundTaskCount ?? 0) > 0 && children.length > 0)
}

export function SubagentWaitStatus({ binding }: { binding?: AcpBinding }) {
  if (!isWaitingOnSubagents(binding)) return null
  return <div role="status" className="conversation-status" data-tone="accent">Waiting on subagents</div>
}
