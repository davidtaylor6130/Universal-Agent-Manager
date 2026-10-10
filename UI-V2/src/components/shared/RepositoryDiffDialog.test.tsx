import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { describe, expect, it, vi } from 'vitest'
import { RepositoryDiffDialog } from './RepositoryDiffDialog'
import type { VcsChangedFile } from '../../store/useAppStore'

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

describe('Repository diff request ownership', () => {
  it('ignores an old file response after changing chat and clears errors for binary files', async () => {
    const host = document.createElement('div')
    document.body.append(host)
    const root = createRoot(host)
    const file: VcsChangedFile = { path: 'file.txt', status: 'M', staged: false, binary: false, additions: 1, deletions: 0 }
    let finishOld!: (diff: string) => void
    const getDiff = vi.fn<(_: string, path: string) => Promise<string>>()
      .mockImplementationOnce(() => new Promise((resolve) => { finishOld = resolve }))
      .mockResolvedValueOnce('@@ -0,0 +1 @@\n+current chat')
      .mockRejectedValueOnce(new Error('Missing file'))
    const render = (chatId: string, nextFile = file) => root.render(<RepositoryDiffDialog chatId={chatId} file={nextFile} vcsType="git" getDiff={getDiff} onClose={() => {}} />)
    try {
      await act(async () => render('old'))
      await act(async () => render('new'))
      await act(async () => finishOld('@@ -0,0 +1 @@\n+stale chat'))
      expect(document.body.textContent).toContain('current chat')
      expect(document.body.textContent).not.toContain('stale chat')
      await act(async () => render('new', { ...file, path: 'missing.txt' }))
      expect(document.querySelector('[role="alert"]')?.textContent).toBe('Missing file')
      await act(async () => render('new', { ...file, path: 'binary.png', binary: true }))
      expect(document.querySelector('[role="alert"]')).toBeNull()
      expect(document.body.textContent).toContain('Binary changes cannot be previewed.')
    } finally {
      act(() => root.unmount())
      host.remove()
    }
  })
})


it('remembers the layout without refetching the diff and tolerates unavailable preference storage', async () => {
  let preference = 'split'
  const setItem = vi.fn((_key: string, value: string) => { preference = value })
  vi.stubGlobal('localStorage', { getItem: () => preference, setItem })
  const host = document.createElement('div'); document.body.append(host)
  const root = createRoot(host)
  const file: VcsChangedFile = { path: 'file.txt', status: 'M', staged: false, binary: false, additions: 1, deletions: 1 }
  const getDiff = vi.fn().mockResolvedValue('@@ -1 +1 @@\n-old\n+new')
  const render = () => root.render(<RepositoryDiffDialog chatId="one" file={file} vcsType="git" getDiff={getDiff} onClose={() => {}} />)
  try {
    await act(async () => render())
    expect(document.querySelector('[aria-label="Split diff for file.txt"]')).not.toBeNull()
    act(() => (document.querySelector('[aria-label="Unified diff view"]') as HTMLButtonElement).click())
    expect(document.querySelector('[aria-label="Unified diff for file.txt"]')).not.toBeNull()
    expect(setItem).toHaveBeenCalledWith('uam-diff-layout', 'unified')
    expect(getDiff).toHaveBeenCalledTimes(1)
    act(() => root.render(null))
    await act(async () => render())
    expect(document.querySelector('[aria-label="Unified diff view"]')?.getAttribute('aria-pressed')).toBe('true')
    setItem.mockImplementation(() => { throw new Error('storage unavailable') })
    act(() => (document.querySelector('[aria-label="Split diff view"]') as HTMLButtonElement).click())
    expect(document.querySelector('[aria-label="Split diff for file.txt"]')).not.toBeNull()
  } finally { act(() => root.unmount()); host.remove(); vi.unstubAllGlobals() }
})
