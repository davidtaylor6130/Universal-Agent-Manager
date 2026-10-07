import type { Session } from '../types/session'
import type { AcpBinding } from '../store/cpp/types'

/** Follows visible sidebar order, then recent chats outside collapsed sections, wrapping once. */
export function nextAttentionSession(
  sessions: readonly Session[],
  bindings: Readonly<Record<string, AcpBinding>>,
  activeId: string | null,
  visibleIds: readonly string[] = [],
): string | null {
  const candidates = sessions.filter((session) => !session.temporaryParentChatId && !session.sideCleanupRequested)
  const byId = new Map(candidates.map((session) => [session.id, session]))
  const recent = [...candidates].sort((a, b) => b.updatedAt.getTime() - a.updatedAt.getTime() || a.id.localeCompare(b.id))
  const ordered = [...new Set([...visibleIds.filter((id) => byId.has(id)), ...recent.map((session) => session.id)])]
  const start = ordered.indexOf(activeId ?? '')
  for (let offset = 1; offset <= ordered.length; offset += 1) {
    const id = ordered[(start + offset) % ordered.length]
    if (id === activeId) continue
    const binding = bindings[id]
    if (binding?.attentionKind || binding?.pendingPermission || binding?.pendingUserInput || binding?.lifecycleState === 'error') return id
  }
  return null
}
