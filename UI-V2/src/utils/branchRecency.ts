import type { Session } from '../types/session'

/** Branches checked out by UAM chat worktrees in the folder: live agent work another chat cannot switch to or should not start from. */
export function chatWorktreeBranches(sessions: readonly Session[], folderId: string | null | undefined, exceptChatId = ''): Set<string> {
  // A chat reusing another chat's worktree shares its branch; that branch stays in place for it.
  const own = sessions.find((session) => session.id === exceptChatId)?.workspaceBranchName
  return new Set(sessions.flatMap((session) => session.folderId === folderId && session.workspaceBranchName && session.workspaceBranchName !== own ? [session.workspaceBranchName] : []))
}

/** Native newest-commit-first order (stable sort) with other chats' worktree branches last. */
export function rankBranches<T extends { name: string }>(branches: readonly T[], demoted: ReadonlySet<string>): T[] {
  return [...branches].sort((a, b) => Number(demoted.has(a.name)) - Number(demoted.has(b.name)))
}

/**
 * New-chat starting branch from a ranked list: the furthest-ahead (newest commit) branch unless that is another
 * chat's worktree, except that the configured default (or the checked-out branch when none is configured) wins
 * when it points at the same commit.
 */
export function preferredStartingBranch(ranked: readonly { name: string; commit: string }[], demoted: ReadonlySet<string>, configuredDefault: string, currentBranch: string): string | undefined {
  const top = ranked[0]
  if (!top || demoted.has(top.name)) return undefined
  const fallback = ranked.find((branch) => branch.name === (configuredDefault || currentBranch))
  // ponytail: distinct commits sharing one commit second stay in native order; compare commit times if that matters.
  return fallback?.commit === top.commit ? fallback.name : top.name
}
