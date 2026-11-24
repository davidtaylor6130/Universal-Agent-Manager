import { describe, expect, it } from 'vitest'
import { sanitizeVcsCommitStatus } from './sanitizers'

describe('VCS status local snapshot transport', () => {
  it('keeps a known snapshot source and its capabilities and drops anything else', () => {
    const base = { available: true, vcsTypes: ['git'], activeVcsType: 'git', changedFiles: [] }
    expect(sanitizeVcsCommitStatus({ ...base, localSnapshot: { sourceKind: 'svn', sourceDirectory: '/work/checkout', sourceCapabilities: ['history', 7, 'update'] } })!.localSnapshot)
      .toEqual({ sourceKind: 'svn', sourceDirectory: '/work/checkout', sourceCapabilities: ['history', 'update'] })
    expect(sanitizeVcsCommitStatus({ ...base, localSnapshot: { sourceKind: 'folder' } })!.localSnapshot)
      .toEqual({ sourceKind: 'folder', sourceDirectory: '', sourceCapabilities: [] })
    expect(sanitizeVcsCommitStatus({ ...base, localSnapshot: { sourceKind: 'hg', sourceDirectory: '/x' } })!.localSnapshot).toBeUndefined()
    expect(sanitizeVcsCommitStatus(base)!.localSnapshot).toBeUndefined()
  })
})
