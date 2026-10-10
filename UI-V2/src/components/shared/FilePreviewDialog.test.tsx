import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { describe, expect, it, vi } from 'vitest'
import { useAppStore } from '../../store/useAppStore'
import type { Session } from '../../types/session'
import type { WorkspaceFilePreview } from '../../types/filePreview'
import { filePreviewResponse } from '../../types/filePreview'
import { MarkdownContent } from '../markdown/Markdown'
import { FilePreviewDialog } from './FilePreviewDialog'
import { FileInspectionProvider } from './FileInspectionProvider'

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
const session: Session = { id: 'inspect', name: 'Inspect', viewMode: 'chat', folderId: null, workspaceDirectory: '/work', executionHostId: 'local', createdAt: new Date(), updatedAt: new Date() }
const preview = (text: string): WorkspaceFilePreview => ({ text, path: 'src/main.cpp', workspace: '/work', executionHostId: 'local' })
const button = (label: string) => Array.from(document.querySelectorAll<HTMLButtonElement>('button')).find((item) => item.textContent === label || item.getAttribute('aria-label') === label)!

it('rejects malformed source responses before they enter the viewer', () => {
  const valid = preview('hello')
  expect(filePreviewResponse(valid)).toEqual(valid)
  for (const change of [{ text: 'a\0b' }, { text: 'x'.repeat(1024 * 1024 + 1) }, { path: '' }, { workspace: '/work\nother' }, { executionHostId: 'host\0' }]) {
    expect(filePreviewResponse({ ...valid, ...change })).toBeNull()
  }
})

describe('Workspace source inspection', () => {
  it('ignores stale reads across workspace changes, supports reload errors, and virtualizes line ranges', async () => {
    const original = useAppStore.getState()
    let oldRead!: (file: WorkspaceFilePreview) => void
    const read = vi.fn().mockImplementationOnce(() => new Promise((resolve) => { oldRead = resolve }))
      .mockResolvedValueOnce(preview(Array.from({ length: 10000 }, (_, n) => `line ${n + 1}`).join('\n')))
      .mockRejectedValueOnce(new Error('The file was removed.'))
    useAppStore.setState({ sessions: [session], folders: [], getWorkspaceFilePreview: read })
    const host = document.createElement('div'); document.body.append(host)
    const root = createRoot(host)
    try {
      await act(async () => root.render(<FilePreviewDialog chatId="inspect" reference={{ path: 'src/main.cpp', line: 5000, endLine: 5002 }} onClose={() => {}} />))
      expect(document.body.textContent).toContain('Loading file')
      await act(async () => useAppStore.setState({ sessions: [{ ...session, workspaceDirectory: '/new-work' }] }))
      await act(async () => oldRead(preview('STALE RESPONSE')))
      expect(document.body.textContent).not.toContain('STALE RESPONSE')
      expect(document.querySelector('[data-source-line="5000"]')?.textContent).toContain('line 5000')
      expect(document.querySelector('[data-source-line="5002"]')?.getAttribute('style')).toContain('var(--accent-dim)')
      expect(document.querySelectorAll('[data-source-line]').length).toBeLessThan(80)
      await act(async () => button('Reload file').click())
      expect(document.querySelector('[role="alert"]')?.textContent).toBe('The file was removed.')
      expect(document.querySelector('[data-source-line]')).toBeNull()
    } finally { act(() => root.unmount()); host.remove(); useAppStore.setState(original, true) }
  })

  it('opens markdown files and deleted-file diffs with the worktree basis and nested escape ownership', async () => {
    const original = useAppStore.getState()
    const read = vi.fn().mockRejectedValue(new Error('Missing file'))
    const getDiff = vi.fn().mockResolvedValue('@@ -1 +0,0 @@\n-deleted')
    const status = vi.fn().mockResolvedValue({ available: true, activeVcsType: 'git', vcsTypes: ['git'], workspaceDirectory: '/work', changedFiles: [], lineStatsReady: false, error: '', warning: '', branchOrRevision: 'feature' })
    useAppStore.setState({ sessions: [{ ...session, workspaceIsolationKind: 'gitWorktree', workspaceBaseRef: 'base-ref' }], folders: [], getWorkspaceFilePreview: read, getVcsCommitStatus: status, getVcsFileDiff: getDiff })
    const host = document.createElement('div'); document.body.append(host)
    const root = createRoot(host)
    try {
      await act(async () => root.render(<FileInspectionProvider chatId="inspect"><MarkdownContent content={'[deleted](/work/src/deleted.cpp:4) and `object.method`'} /></FileInspectionProvider>))
      await act(async () => button('deleted').click())
      expect(read).toHaveBeenCalledWith('inspect', '/work/src/deleted.cpp')
      expect(document.querySelector('[role="alert"]')?.textContent).toBe('Missing file')
      await act(async () => button('Changes').click())
      expect(getDiff).toHaveBeenCalledWith('inspect', 'src/deleted.cpp', 'git', 'base-ref')
      expect(document.body.textContent).toContain('Against base-ref')
      expect(document.querySelector('[aria-label="Changes in src/deleted.cpp"] header')?.textContent).not.toContain('+0')
      expect(document.querySelectorAll('[role="dialog"]').length).toBe(2)
      act(() => window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', cancelable: true })))
      expect(document.querySelectorAll('[role="dialog"]').length).toBe(1)
      act(() => window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', cancelable: true })))
      expect(document.querySelector('[role="dialog"]')).toBeNull()
    } finally { act(() => root.unmount()); host.remove(); useAppStore.setState(original, true) }
  })

  it('discards a diff request if its workspace changes while status is loading', async () => {
    const original = useAppStore.getState()
    let finish!: (value: unknown) => void
    const status = vi.fn().mockImplementation(() => new Promise((resolve) => { finish = resolve }))
    const getDiff = vi.fn()
    useAppStore.setState({ sessions: [session], folders: [], getWorkspaceFilePreview: vi.fn().mockResolvedValue(preview('source')), getVcsCommitStatus: status, getVcsFileDiff: getDiff })
    const host = document.createElement('div'); document.body.append(host)
    const root = createRoot(host)
    try {
      await act(async () => root.render(<FileInspectionProvider chatId="inspect"><MarkdownContent content={'`src/main.cpp:1`'} /></FileInspectionProvider>))
      await act(async () => button('src/main.cpp:1').click())
      act(() => button('Changes').click())
      await act(async () => {
        useAppStore.setState({ sessions: [{ ...session, workspaceDirectory: '/other' }] })
      })
      await act(async () => finish({ available: true, activeVcsType: 'git', workspaceDirectory: '/work', error: '' }))
      expect(getDiff).not.toHaveBeenCalled()
      expect(document.querySelector('[aria-label="Changes in src/main.cpp"]')).toBeNull()
    } finally { act(() => root.unmount()); host.remove(); useAppStore.setState(original, true) }
  })
})
