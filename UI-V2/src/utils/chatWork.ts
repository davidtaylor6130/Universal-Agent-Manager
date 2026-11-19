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
    goal?.status === 'active'
  )
}

export function providerSwitchOverrideUnlocks(acp?: AcpBinding): boolean {
  return Boolean(acp?.providerSwitchOverride && acp.providerSwitchOverride !== 'none' && acp.providerSwitchOverride !== 'warn')
}

/** Why the provider rail is locked, or empty when a switch can be requested. The native refusal stays authoritative. */
export function providerSwitchLockReason(acp?: AcpBinding, cli?: CliBinding): string {
  // The user cannot clear these states, so the rail must stay open to reach the Switch anyway warning.
  if (providerSwitchOverrideUnlocks(acp)) return ''
  if (acp?.queuedPrompts?.length) return 'Resume or remove the queued messages before changing provider.'
  if (acp?.pendingPermission || acp?.pendingUserInput) return 'Answer the open request before changing provider.'
  if (acp?.processing) return 'Stop the current turn before changing provider.'
  if (acp?.pendingDirectPrompts?.length) return 'Wait for the sent message to be confirmed before changing provider.'
  if (acp?.unsentDirectPrompts?.length) return 'Remove or retry the unsent message before changing provider.'
  if (cli?.processing || cli?.turnState === 'busy') return 'Wait for the CLI View turn to finish before changing provider.'
  return ''
}
