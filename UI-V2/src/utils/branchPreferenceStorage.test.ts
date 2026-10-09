import { beforeEach, describe, expect, it } from 'vitest'
import { preferredBranch, preferredFamilyChat, setPreferredBranch, setPreferredFamilyChat } from './branchPreferenceStorage'

describe('branch preference storage', () => {
  const values = new Map<string, string>()
  beforeEach(() => {
    values.clear()
    Object.defineProperty(globalThis, 'localStorage', { configurable: true, value: {
      getItem: (key: string) => values.get(key) ?? null,
      setItem: (key: string, value: string) => values.set(key, value),
    } })
  })

  it('persists independently for each branch point', () => {
    setPreferredBranch('root', 2, 'branch-a')
    setPreferredBranch('root', 7, 'branch-b')
    expect(preferredBranch('root', 2, ['root', 'branch-a'])).toBe('branch-a')
    expect(preferredBranch('root', 7, ['root', 'branch-b'])).toBe('branch-b')
  })

  it('falls back deterministically and removes a deleted preference', () => {
    setPreferredBranch('root', 2, 'deleted')
    expect(preferredBranch('root', 2, ['root', 'branch-a'])).toBe('root')
    expect(preferredBranch('root', 2, ['root', 'branch-a'])).toBeNull()
  })

  it('restores a stable family chat ID across reorder and drops deleted chats', () => {
    setPreferredFamilyChat('root', 'branch-b')
    expect(preferredFamilyChat('root', ['root', 'branch-a', 'branch-b'])).toBe('branch-b')
    expect(preferredFamilyChat('root', ['branch-b', 'root', 'branch-a'])).toBe('branch-b')
    expect(preferredFamilyChat('root', ['root', 'branch-a'])).toBeNull()
    expect(preferredFamilyChat('root', ['root', 'branch-a'])).toBeNull()
  })

  it('keeps family preferences separate from message branch points', () => {
    setPreferredBranch('root', 2, 'message-branch')
    setPreferredFamilyChat('root', 'family-branch')
    expect(preferredBranch('root', 2, ['root', 'message-branch'])).toBe('message-branch')
    expect(preferredFamilyChat('root', ['root', 'family-branch'])).toBe('family-branch')
  })
})
