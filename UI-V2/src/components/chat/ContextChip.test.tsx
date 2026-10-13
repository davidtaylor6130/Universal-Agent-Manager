import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { expect, it, vi } from 'vitest'
import { ContextChip } from './ContextChip'
import { FileInspectionContext } from '../shared/fileInspectionContext'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

it('renders branch and folder kinds with their labels, opening a file at its line reference', () => {
  const host = document.createElement('div')
  document.body.append(host)
  const root = createRoot(host)
  const inspect = vi.fn()
  try {
    act(() => root.render(
      <FileInspectionContext.Provider value={inspect}>
        <ContextChip kind="branch" name="feature/chips" detail="feature/chips" />
        <ContextChip kind="file" name="main.cpp" detail="src/main.cpp:4" path="src/main.cpp" reference={{ path: 'src/main.cpp', line: 4 }} />
      </FileInspectionContext.Provider>
    ))
    const [branch, file] = Array.from(host.querySelectorAll<HTMLElement>('.uam-context-chip'))
    expect(branch.textContent).toBe('feature/chipsbranch')
    expect(branch.querySelector('button')).toBeNull()
    act(() => file.querySelector('button')?.click())
    expect(inspect).toHaveBeenCalledWith({ path: 'src/main.cpp', line: 4 })
  } finally {
    act(() => root.unmount())
    host.remove()
  }
})

it('shows the kind and name, opens a preview only when a path and inspector exist, and removes via its labelled button', () => {
  const host = document.createElement('div')
  document.body.append(host)
  const root = createRoot(host)
  const inspect = vi.fn()
  const remove = vi.fn()
  try {
    act(() => root.render(
      <FileInspectionContext.Provider value={inspect}>
        <ContextChip kind="file" name="notes.md" detail="/work/notes.md" path="/work/notes.md" onRemove={remove} removeLabel="Remove notes.md attachment" />
        <ContextChip kind="directory" name="src" detail="/work/src" path="/work/src" />
      </FileInspectionContext.Provider>
    ))
    const [file, folder] = Array.from(host.querySelectorAll<HTMLElement>('.uam-context-chip'))
    expect(file.textContent).toBe('notes.mdfile')
    expect(file.dataset.detail).toBe('/work/notes.md')
    expect(folder.querySelector('button')).toBeNull()

    act(() => file.querySelector('button')?.click())
    expect(inspect).toHaveBeenCalledWith({ path: '/work/notes.md' })

    act(() => host.querySelector<HTMLButtonElement>('[aria-label="Remove notes.md attachment"]')?.click())
    expect(remove).toHaveBeenCalledTimes(1)
  } finally {
    act(() => root.unmount())
    host.remove()
  }
})
