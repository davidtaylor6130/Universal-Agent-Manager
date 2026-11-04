import { describe, expect, it } from 'vitest'
import type { Session } from '../types/session'
import { chatWorktreeBranches, preferredStartingBranch, rankBranches } from './branchRecency'

const chat = (id: string, fields: Partial<Session>): Session =>
  ({ id, folderId: 'project', createdAt: new Date(0), updatedAt: new Date(1_000), ...fields }) as Session

describe('branch defaults', () => {
  it('keeps native newest-commit order with other chats\' worktree branches last', () => {
    const sessions = [chat('a', { workspaceBranchName: 'agents/a' }), chat('b', { workspaceBranchName: 'agents/b' }), chat('shared', { workspaceBranchName: 'agents/b' }), chat('c', { workspaceBranchName: 'agents/c', folderId: 'elsewhere' })]
    const worktrees = chatWorktreeBranches(sessions, 'project', 'b')
    expect([...worktrees]).toEqual(['agents/a'])
    const native = [{ name: 'agents/a' }, { name: 'newest' }, { name: 'agents/b' }, { name: 'older' }]
    expect(rankBranches(native, worktrees).map((branch) => branch.name)).toEqual(['newest', 'agents/b', 'older', 'agents/a'])
  })

  it('prefers the furthest-ahead branch, the configured default (else the checked-out branch) on the same commit, and never a chat worktree', () => {
    const none = new Set<string>()
    const tied = [{ name: 'feature', commit: 'a' }, { name: 'main', commit: 'a' }, { name: 'dev', commit: 'a' }]
    expect(preferredStartingBranch(tied, none, 'main', 'dev')).toBe('main')
    expect(preferredStartingBranch(tied, none, '', 'dev')).toBe('dev')
    expect(preferredStartingBranch([{ name: 'ahead', commit: 'b' }, ...tied], none, 'main', 'dev')).toBe('ahead')
    expect(preferredStartingBranch([{ name: 'agents/a', commit: 'b' }], new Set(['agents/a']), 'main', 'dev')).toBeUndefined()
    expect(preferredStartingBranch([], none, 'main', 'dev')).toBeUndefined()
  })
})
