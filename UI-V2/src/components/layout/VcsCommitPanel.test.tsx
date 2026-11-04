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
    gitEnabled: true,
    svnEnabled: true,
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
  it.each(['chat', 'workspace', 'worktree', 'host'] as const)('closes the old diff before requesting it from a changed %s', async (change) => {
    await act(async () => root.render(<VcsCommitPanel />))
    await act(async () => host.querySelector<HTMLButtonElement>('[aria-label="Review changes to owned-by-a.ts"]')!.click())
    expect(document.querySelector('[aria-label="Changes in owned-by-a.ts"]')).not.toBeNull()
    const getDiff = useAppStore.getState().getVcsFileDiff
    expect(getDiff).toHaveBeenCalledTimes(1)

    await act(async () => {
      if (change === 'chat') useAppStore.setState({ activeSessionId: 'chat-b' })
      else useAppStore.setState((state) => ({ sessions: state.sessions.map((session) => session.id === 'chat-a' ? { ...session, ...(change === 'workspace' ? { workspaceDirectory: '/tmp/another-workspace' } : change === 'worktree' ? { workspaceWorktreeDirectory: '/tmp/another-worktree' } : { executionHostId: 'remote' }) } : session) }))
    })

    expect(document.querySelector('[aria-label="Changes in owned-by-a.ts"]')).toBeNull()
    expect(getDiff).toHaveBeenCalledTimes(1)
  })
})

describe('commit draft ownership', () => {
  function edit(label: string, value: string) {
    const input = host.querySelector<HTMLInputElement | HTMLTextAreaElement>(`[aria-label="${label}"]`)!
    act(() => {
      Object.getOwnPropertyDescriptor(input instanceof HTMLTextAreaElement ? HTMLTextAreaElement.prototype : HTMLInputElement.prototype, 'value')!.set!.call(input, value)
      input.dispatchEvent(new Event('input', { bubbles: true }))
    })
  }
  async function selectFile() {
    await act(async () => host.querySelector<HTMLButtonElement>('[aria-label="Select owned-by-a.ts"]')!.click())
  }
  it('preserves a draft and ignores a pending suggestion when VCS support changes', async () => {
    type Suggestion = Awaited<ReturnType<ReturnType<typeof useAppStore.getState>['generateVcsCommitMessage']>>
    let finish!: (value: Suggestion) => void
    const original = useAppStore.getState().generateVcsCommitMessage
    useAppStore.setState({ generateVcsCommitMessage: vi.fn(() => new Promise<Suggestion>((resolve) => { finish = resolve })) })
    try {
      await act(async () => root.render(<VcsCommitPanel />))
      await selectFile()
      edit('Commit summary', 'Keep my summary')
      edit('Commit description', 'Keep my description')
      await act(async () => host.querySelector<HTMLButtonElement>('[aria-label="Generate commit message"]')!.click())
      await act(async () => useAppStore.setState({ gitEnabled: false }))
      await act(async () => finish({ title: 'Obsolete suggestion', description: 'Obsolete details' }))
      expect(host.querySelector<HTMLInputElement>('[aria-label="Commit summary"]')!.value).toBe('Keep my summary')
      expect(host.querySelector<HTMLTextAreaElement>('[aria-label="Commit description"]')!.value).toBe('Keep my description')
      expect(host.querySelector<HTMLButtonElement>('[aria-label="Generate commit message"]')!.disabled).toBe(true)
      await act(async () => useAppStore.setState({ gitEnabled: true }))
      expect(host.querySelector<HTMLInputElement>('[aria-label="Commit summary"]')!.value).toBe('Keep my summary')
    } finally { act(() => useAppStore.setState({ generateVcsCommitMessage: original })) }
  })

  it('keeps newer summary and description edits when an older generated message arrives', async () => {
    type Suggestion = Awaited<ReturnType<ReturnType<typeof useAppStore.getState>['generateVcsCommitMessage']>>
    let finish!: (value: Suggestion) => void
    const generate = vi.fn(() => new Promise<Suggestion>((resolve) => { finish = resolve }))
    const original = useAppStore.getState().generateVcsCommitMessage
    useAppStore.setState({ generateVcsCommitMessage: generate })
    try {
      await act(async () => root.render(<VcsCommitPanel />))
      await selectFile()
      edit('Commit summary', 'Original summary')
      await act(async () => host.querySelector<HTMLButtonElement>('[aria-label="Generate commit message"]')!.click())
      edit('Commit summary', 'Newer summary')
      edit('Commit description', 'Newer description')
      await act(async () => finish({ title: 'Old generated summary', description: 'Old generated description' }))
      expect(host.querySelector<HTMLInputElement>('[aria-label="Commit summary"]')!.value).toBe('Newer summary')
      expect(host.querySelector<HTMLTextAreaElement>('[aria-label="Commit description"]')!.value).toBe('Newer description')
      await act(async () => host.querySelector<HTMLButtonElement>('[aria-label="Generate commit message"]')!.click())
      await act(async () => finish({ title: 'Accepted suggestion', description: 'Accepted details' }))
      expect(host.querySelector<HTMLInputElement>('[aria-label="Commit summary"]')!.value).toBe('Accepted suggestion')
      expect(host.querySelector<HTMLTextAreaElement>('[aria-label="Commit description"]')!.value).toBe('Accepted details')
    } finally { act(() => useAppStore.setState({ generateVcsCommitMessage: original })) }
  })
  it('keeps the next edited draft when an earlier commit succeeds and clears an unchanged submitted draft', async () => {
    type Result = Awaited<ReturnType<ReturnType<typeof useAppStore.getState>['commitVcsChanges']>>
    let finish!: (value: Result) => void
    const commit = vi.fn(() => new Promise<Result>((resolve) => { finish = resolve }))
    const original = useAppStore.getState().commitVcsChanges
    useAppStore.setState({ commitVcsChanges: commit })
    try {
      await act(async () => root.render(<VcsCommitPanel />))
      await selectFile()
      edit('Commit summary', 'Submitted summary')
      edit('Commit description', 'Submitted description')
      const commitButton = () => Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Commit 1 file')!
      await act(async () => commitButton().click())
      expect(commit).toHaveBeenCalledWith('chat-a', 'git', 'Submitted summary\n\nSubmitted description', ['owned-by-a.ts'])
      edit('Commit summary', 'Next summary')
      edit('Commit description', 'Next description')
      await act(async () => finish({ ok: true, message: 'Committed' }))
      expect(host.querySelector<HTMLInputElement>('[aria-label="Commit summary"]')!.value).toBe('Next summary')
      expect(host.querySelector<HTMLTextAreaElement>('[aria-label="Commit description"]')!.value).toBe('Next description')
      await selectFile()
      await act(async () => commitButton().click())
      await act(async () => finish({ ok: true, message: 'Committed next' }))
      expect(host.querySelector<HTMLInputElement>('[aria-label="Commit summary"]')!.value).toBe('')
      expect(host.querySelector<HTMLTextAreaElement>('[aria-label="Commit description"]')!.value).toBe('')
    } finally { act(() => useAppStore.setState({ commitVcsChanges: original })) }
  })
})
