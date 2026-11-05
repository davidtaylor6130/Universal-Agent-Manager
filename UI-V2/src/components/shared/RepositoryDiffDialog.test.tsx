import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { describe, expect, it, vi } from 'vitest'
import { RepositoryDiffDialog } from './RepositoryDiffDialog'
import type { VcsChangedFile } from '../../store/useAppStore'

vi.stubGlobal('ResizeObserver', class { observe() {} unobserve() {} disconnect() {} })

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
      expect(document.querySelector('[role="alert"]')?.textContent).toContain('Missing file')
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

describe('Repository diff review states and keyboard', () => {
  const file: VcsChangedFile = { path: 'src/app.ts', status: 'M', staged: false, binary: false, additions: 1, deletions: 1 }
  const mount = async (getDiff: (chatId: string, path: string) => Promise<string>, nextFile = file, onClose = vi.fn()) => {
    const host = document.createElement('div'); document.body.append(host)
    const root = createRoot(host)
    await act(async () => root.render(<RepositoryDiffDialog chatId="chat" file={nextFile} vcsType="git" comparisonRef="chat" comparisonLabel="Since this chat began" getDiff={getDiff} onClose={onClose} />))
    return { onClose, dispose: () => { act(() => root.unmount()); host.remove() } }
  }

  it('shows loading, then the patch, and closes on Escape with focus restored to the opener', async () => {
    const opener = document.createElement('button'); document.body.append(opener); opener.focus()
    let finish!: (diff: string) => void
    const view = await mount(() => new Promise((resolve) => { finish = resolve }))
    try {
      expect(document.querySelector('[role="status"]')?.textContent).toBe('Loading file changes…')
      expect(document.activeElement?.getAttribute('role')).toBe('dialog')
      expect(document.body.textContent).toContain('Since this chat began')
      await act(async () => finish('@@ -1 +1 @@\n-old\n+new'))
      expect(document.querySelector('[role="status"]')).toBeNull()
      act(() => { window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })) })
      expect(view.onClose).toHaveBeenCalledTimes(1)
    } finally { view.dispose() }
    expect(document.activeElement).toBe(opener)
    opener.remove()
  })

  it('distinguishes empty, binary and failed diffs', async () => {
    const empty = await mount(() => Promise.resolve(''))
    expect(document.body.textContent).toContain('No textual diff is available for this file.')
    empty.dispose()
    const getDiff = vi.fn(() => Promise.resolve('unused'))
    const binary = await mount(getDiff, { ...file, path: 'logo.png', binary: true })
    expect(document.body.textContent).toContain('Binary changes cannot be previewed.')
    expect(getDiff).not.toHaveBeenCalled()
    binary.dispose()
    const failed = await mount(() => Promise.reject(new Error('This chat has no saved repository baseline.')))
    expect(document.querySelector('[role="alert"]')?.textContent).toContain('This chat has no saved repository baseline.')
    failed.dispose()
  })

  it('uses a single-column unified diff on narrow screens even when Split is remembered', async () => {
    vi.stubGlobal('localStorage', { getItem: () => 'split', setItem: () => {} })
    vi.stubGlobal('matchMedia', (query: string) => ({ matches: query.includes('max-width'), media: query, addEventListener: () => {}, removeEventListener: () => {} }))
    const view = await mount(() => Promise.resolve('@@ -1 +1 @@\n-old\n+new'))
    try {
      expect(document.querySelector('[aria-label="Unified diff for src/app.ts"]')).not.toBeNull()
      expect(document.querySelector('[aria-label="Split diff for src/app.ts"]')).toBeNull()
      expect(document.querySelector('[aria-label="Diff layout"]')).toBeNull()
    } finally { view.dispose(); vi.unstubAllGlobals() }
  })
})
