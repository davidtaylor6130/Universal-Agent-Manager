import { act } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { useAppStore } from '../../store/useAppStore'
import { VcsCommitPanel } from './VcsCommitPanel'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

let host: HTMLDivElement
let root: Root

beforeEach(() => {
  host = document.createElement('div')
  document.body.appendChild(host)
  root = createRoot(host)
  useAppStore.setState({
    sessions: [
      { id: 'chat-a', name: 'A', viewMode: 'chat', folderId: null, workspaceDirectory: '/tmp/a', createdAt: new Date(), updatedAt: new Date() },
      { id: 'chat-b', name: 'B', viewMode: 'chat', folderId: null, workspaceDirectory: '/tmp/b', createdAt: new Date(), updatedAt: new Date() },
    ],
    activeSessionId: 'chat-a',
    getVcsCommitStatus: vi.fn(async () => ({
      available: true, vcsTypes: ['git'], activeVcsType: 'git', workspaceDirectory: '/tmp/a', branchOrRevision: 'main',
      changedFiles: [{ path: 'owned-by-a.ts', status: ' M', staged: false, additions: 1, deletions: 0, binary: false }],
      lineStatsReady: true, warning: '', error: '',
    })),
    getVcsFileDiff: vi.fn(async () => '+original workspace diff'),
  })
})

afterEach(() => {
  act(() => root.unmount())
  host.remove()
})

describe('commit diff ownership', () => {
  it.each(['chat', 'workspace'] as const)('closes the old diff before requesting it from a changed %s', async (change) => {
    await act(async () => root.render(<VcsCommitPanel />))
    await act(async () => host.querySelector<HTMLButtonElement>('[aria-label="Review changes to owned-by-a.ts"]')!.click())
    expect(document.querySelector('[aria-label="Changes in owned-by-a.ts"]')).not.toBeNull()
    const getDiff = useAppStore.getState().getVcsFileDiff
    expect(getDiff).toHaveBeenCalledTimes(1)

    await act(async () => {
      if (change === 'chat') useAppStore.setState({ activeSessionId: 'chat-b' })
      else useAppStore.setState((state) => ({ sessions: state.sessions.map((session) => session.id === 'chat-a' ? { ...session, workspaceDirectory: '/tmp/another-workspace' } : session) }))
    })

    expect(document.querySelector('[aria-label="Changes in owned-by-a.ts"]')).toBeNull()
    expect(getDiff).toHaveBeenCalledTimes(1)
  })
})
