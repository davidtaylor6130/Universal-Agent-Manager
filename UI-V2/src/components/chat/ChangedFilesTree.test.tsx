import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { expect, it, vi } from 'vitest'
import { ChangedFilesTree } from './ChangedFilesTree'
import type { VcsChangedFile } from '../../store/useAppStore'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

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
