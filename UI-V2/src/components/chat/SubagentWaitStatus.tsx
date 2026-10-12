import type { AcpBinding } from '../../store/cpp/types'
import { ChatStatusStrip } from './StatusHelpers'
import { AgentsScene } from './StatusScenes'

const ACTIVE = new Set(['pending', 'running', 'in_progress', 'inProgress'])

/** Only an explicit collaboration wait or live children after an idle parent means waiting. */
export function isWaitingOnSubagents(binding?: AcpBinding): boolean {
  if (!binding?.running || binding.lifecycleState === 'error' || binding.pendingPermission || binding.pendingUserInput) return false
  const children = binding.toolCalls.filter((tool) => tool.isSubAgent && ACTIVE.has(tool.status))
  return children.some((tool) => tool.kind === 'collabAgentToolCall' && tool.title === 'wait') ||
    (!binding.processing && (binding.backgroundTaskCount ?? 0) > 0 && children.length > 0)
}

export function SubagentWaitStatus({ binding }: { binding?: AcpBinding }) {
  const children = binding?.toolCalls.filter((tool) => tool.isSubAgent) ?? []
  const working = children.filter((tool) => ACTIVE.has(tool.status)).length
  const idle = children.length - working
  const title = working > 1 ? `Waiting on ${working} subagents` : 'Waiting on subagents'
  return <ChatStatusStrip open={isWaitingOnSubagents(binding)} tone="agents" scene={<AgentsScene working={working} idle={idle} />} title={title} />
}
