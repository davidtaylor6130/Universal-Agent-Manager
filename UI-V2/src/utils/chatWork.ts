import type { AcpBinding, CliBinding } from '../store/cpp/types'
import type { Goal } from '../types/goal'

/** Work can outlive the parent prompt or pause briefly between automatic turns. */
export function chatHasUnfinishedWork(acp?: AcpBinding, cli?: CliBinding, goal?: Goal | null): boolean {
  return Boolean(
    acp?.processing || (acp?.backgroundTaskCount ?? 0) > 0 ||
    (acp && ['starting', 'processing', 'waitingPermission', 'waitingUserInput'].includes(acp.lifecycleState)) ||
    acp?.pendingPermission || acp?.pendingUserInput ||
    ((acp?.queuedPrompts?.length ?? 0) > 0 && !acp?.queueHeld) ||
    cli?.processing || cli?.turnState === 'busy' ||
    (cli && ['starting', 'busy', 'shuttingDown'].includes(cli.lifecycleState)) ||
    (goal?.status === 'active' && (goal.pendingContinuation || goal.executionOwner === 'uam'))
  )
}
