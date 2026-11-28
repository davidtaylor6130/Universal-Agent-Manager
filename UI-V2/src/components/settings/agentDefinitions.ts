import { sendToCEF } from '../../ipc/cefBridge'
import type { AgentDefinitionScope } from '../../store/useAppStore'

/** One saved agent as the agent store lists it. `scope` says whether every workspace or only the active chat's workspace has it. */
export type AgentDefinition = {
  id: string
  description: string
  mode: string
  workspaceAccess: string
  skills: string[]
  delegates: string[]
  instructions: string
  builtIn: boolean
  /** 'read' for an agent imported from a read-only source. */
  maxWorkspaceAccess?: string
  scope: AgentDefinitionScope
}

export const AGENT_SCOPE_LABEL: Record<AgentDefinitionScope, string> = { global: 'All workspaces', workspace: 'This workspace' }

/** A global and a workspace agent may share a name, so rows are keyed by both. */
export const agentKey = (agent: { id: string; scope: AgentDefinitionScope }) => `${agent.scope}:${agent.id}`

/** Global agents, plus the agents saved in the open chat's workspace when a chat is given. */
export async function listAgentDefinitions(chatId?: string | null): Promise<{ ok: true; agents: AgentDefinition[]; errors: string[] } | { ok: false; error: string }> {
  const response = await sendToCEF<{ agents?: Array<Omit<AgentDefinition, 'scope'> & { scope?: string }>; errors?: string[] }>({
    action: 'listAgentDefinitions',
    payload: chatId ? { chatId } : {},
  })
  if (!response.ok || !response.data) return { ok: false, error: response.error || 'Could not load the agent store.' }
  const agents = (response.data.agents ?? []).map((agent): AgentDefinition => ({ ...agent, scope: agent.scope === 'workspace' ? 'workspace' : 'global' }))
  return { ok: true, agents, errors: response.data.errors ?? [] }
}
