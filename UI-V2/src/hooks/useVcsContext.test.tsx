import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { describe, expect, it, vi } from 'vitest'
import { useVcsContext, vcsContextLabel, type VcsContextTarget } from './useVcsContext'
import type { VcsCommitStatus } from '../store/cpp/types'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
const status = (branch: string): VcsCommitStatus => ({ available: true, vcsTypes: ['git'], activeVcsType: 'git', workspaceDirectory: '/workspace/project', branchOrRevision: branch, changedFiles: [], lineStatsReady: false, warning: '', error: '' })

describe('repository context ownership', () => {
  it('labels actual Git and SVN status and hides errors and nonrepositories', () => {
    expect(vcsContextLabel(status('feature'))).toBe('Git: feature')
    expect(vcsContextLabel(status('ab12cd3'))).toBe('Git: ab12cd3')
    expect(vcsContextLabel({ ...status('42'), activeVcsType: 'svn', workspaceDirectory: 'C:\\work\\project\\' })).toBe('SVN: project')
    expect(vcsContextLabel({ ...status('feature'), error: 'unavailable' })).toBeNull()
    expect(vcsContextLabel({ ...status('feature'), available: false })).toBeNull()
  })
  it('rejects stale responses after changing chat, workspace, or host and refreshes on focus', async () => {
    const resolvers: ((value: VcsCommitStatus | null) => void)[] = []
    const load = vi.fn(() => new Promise<VcsCommitStatus | null>((resolve) => resolvers.push(resolve)))
    const host = document.createElement('div')
    const root = createRoot(host)
    function Probe({ target }: { target: VcsContextTarget }) {
      return <span>{vcsContextLabel(useVcsContext(target, load))}</span>
    }
    try {
      await act(async () => root.render(<Probe target={{ id: 'first', workspaceDirectory: '/one', executionHostId: 'local' }} />))
      await act(async () => root.render(<Probe target={{ id: 'second', workspaceDirectory: '/two', executionHostId: 'remote' }} />))
      await act(async () => resolvers[0](status('old')))
      expect(host.textContent).toBe('')
      await act(async () => resolvers[1](status('current')))
      expect(host.textContent).toBe('Git: current')
      await act(async () => root.render(<Probe target={{ id: 'second', workspaceDirectory: '/three', executionHostId: 'remote' }} />))
      expect(host.textContent).toBe('')
      await act(async () => resolvers[2](status('workspace')))
      await act(async () => root.render(<Probe target={{ id: 'second', workspaceDirectory: '/three', executionHostId: 'local' }} />))
      expect(host.textContent).toBe('')
      await act(async () => resolvers[3](status('local')))
      await act(async () => window.dispatchEvent(new Event('focus')))
      expect(load).toHaveBeenCalledTimes(5)
      expect(load.mock.calls[0]).toEqual(['first', 'git', expect.objectContaining({ contextOnly: true, includeLineStats: false })])
      await act(async () => resolvers[4](status('changed')))
      expect(host.textContent).toBe('Git: changed')
    } finally {
      await act(async () => root.unmount())
    }
  })
})
