import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { expect, it, vi } from 'vitest'
import { ChangedFilesTree, changedRowDescription } from './ChangedFilesTree'
import type { VcsChangedFile } from '../../store/useAppStore'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

vi.stubGlobal('ResizeObserver', class { observe() {} unobserve() {} disconnect() {} })
const file = (path: string, status = 'M'): VcsChangedFile => ({ path, status, staged: false, additions: 2, deletions: 1, binary: false })

it('browses changed files with the keyboard and opens the focused file on Enter', async () => {
  const host = document.createElement('div')
  document.body.append(host)
  const root = createRoot(host)
  const onOpenFile = vi.fn()
  const files = [file('src/app.ts', 'A'), file('src/lib/util.ts'), file('README.md', 'D')]
  await act(async () => root.render(<ChangedFilesTree files={files} selectedPath="README.md" onOpenFile={onOpenFile} />))

  const rows = () => [...host.querySelectorAll<HTMLElement>('[role="treeitem"]')]
  // Few files: every folder opens by default. Folders list before files within each level.
  expect(rows().map((row) => row.querySelector('.uam-tree-name')?.textContent)).toEqual(['src', 'lib', 'util.ts', 'app.ts', 'README.md'])
  expect(host.querySelector('[aria-selected="true"]')?.getAttribute('aria-label')).toBe('Review changes to README.md')
  expect(host.querySelector('[aria-label="Review changes to src/app.ts"] .uam-tree-status')?.textContent).toBe('A')

  // Left on an open folder collapses it, and Right re-opens it.
  await act(async () => { rows()[0].focus() })
  await act(async () => { rows()[0].dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowLeft', bubbles: true })) })
  expect(rows().map((row) => row.getAttribute('aria-expanded'))).toEqual(['false', null])
  await act(async () => { host.querySelector('[role="tree"]')!.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowRight', bubbles: true })) })
  expect(rows()).toHaveLength(5)

  // Down moves into the child folder, Down again into app.ts; Enter opens it.
  await act(async () => { rows()[1].focus() })
  await act(async () => { host.querySelector('[role="tree"]')!.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowDown', bubbles: true })) })
  await act(async () => { host.querySelector('[role="tree"]')!.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowDown', bubbles: true })) })
  expect(document.activeElement?.getAttribute('aria-label')).toBe('Review changes to src/app.ts')
  await act(async () => { host.querySelector('[role="tree"]')!.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true })) })
  expect(onOpenFile).toHaveBeenCalledWith(files[0])

  act(() => root.unmount())
  host.remove()
})

it('tells screen readers each change type and its line counts instead of hiding them behind the row name', async () => {
  const host = document.createElement('div')
  document.body.append(host)
  const root = createRoot(host)
  const files = [file('src/app.ts', 'A'), { ...file('src/logo.png', 'M'), binary: true }, { ...file('src/old.ts', 'D'), additions: 0, deletions: 1 }, file('notes.txt', '?')]
  await act(async () => root.render(<ChangedFilesTree files={files} onOpenFile={vi.fn()} />))
  const description = (name: string) => {
    const row = host.querySelector<HTMLElement>(`[role="treeitem"][aria-label="${name}"]`)!
    return document.getElementById(row.getAttribute('aria-describedby')!)?.textContent
  }
  expect(description('src')).toBe('2 lines added, 2 lines removed.') // binary files carry no line counts
  // A nested folder is named by its own name, so its description starts with the path the tooltip shows.
  expect(changedRowDescription({ kind: 'folder', key: 'src/lib', name: 'lib', path: 'src/lib', depth: 1, additions: 1, deletions: 0, children: [] })).toBe('src/lib, 1 line added, 0 lines removed.')
  expect(description('Review changes to src/app.ts')).toBe('Added, 2 lines added, 1 line removed.')
  expect(description('Review changes to src/logo.png')).toBe('Modified, binary file.')
  expect(description('Review changes to src/old.ts')).toBe('Deleted, 0 lines added, 1 line removed.')
  expect(description('Review changes to notes.txt')).toBe('Untracked, 2 lines added, 1 line removed.')
  // The coloured letter and +/- counts are visual duplicates of that description.
  expect([...host.querySelectorAll('.uam-tree-status, .uam-tree-counts')].every((mark) => mark.getAttribute('aria-hidden') === 'true')).toBe(true)
  act(() => root.unmount())
  host.remove()
})
