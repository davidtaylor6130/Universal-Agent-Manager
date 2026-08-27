import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { useAppStore } from '../../store/useAppStore'
import { MemoryLibraryModal } from './MemoryLibraryModal'
import type { MemoryEntry } from '../../types/memory'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

const defaultEntries: MemoryEntry[] = [
  {
    id: 'all/676c6f62616c/Lessons/AI_Lessons/global.md',
    title: 'Global lesson',
    category: 'Lessons/AI_Lessons',
    scope: 'global',
    confidence: 'medium',
    sourceChatId: 'chat-global',
    lastObserved: '2026-01-02T00:00:00.000Z',
    occurrenceCount: 2,
    preview: 'Remember this across projects.',
    filePath: '/tmp/data/memory/Lessons/AI_Lessons/global.md',
    scopeType: 'global',
    folderId: '',
    scopeLabel: 'Global memory',
    rootPath: '/tmp/data/memory',
  },
  {
    id: 'all/726f6f74/Lessons/User_Lessons/local.md',
    title: 'Local lesson',
    category: 'Lessons/User_Lessons',
    scope: 'local',
    confidence: 'high',
    sourceChatId: 'chat-1',
    lastObserved: '2026-01-01T00:00:00.000Z',
    occurrenceCount: 1,
    preview: 'Keep this project-specific.',
    filePath: '/tmp/project/.UAM/Lessons/User_Lessons/local.md',
    scopeType: 'folder',
    folderId: 'default',
    scopeLabel: 'General',
    rootPath: '/tmp/project/.UAM',
  },
  {
    id: 'all/726f6f74/Failures/AI_Failures/local-failure.md',
    title: 'Local failure',
    category: 'Failures/AI_Failures',
    scope: 'local',
    confidence: 'low',
    sourceChatId: 'chat-2',
    lastObserved: '2026-01-03T00:00:00.000Z',
    occurrenceCount: 1,
    preview: 'A project-specific failure.',
    filePath: '/tmp/project/.UAM/Failures/AI_Failures/local-failure.md',
    scopeType: 'folder',
    folderId: 'default',
    scopeLabel: 'General',
    rootPath: '/tmp/project/.UAM',
  },
]

function renderModal({
  createMemoryEntry = vi.fn().mockResolvedValue(true),
  deleteMemoryEntries = vi.fn().mockResolvedValue(true),
  entries = defaultEntries,
}: {
  createMemoryEntry?: ReturnType<typeof vi.fn>
  deleteMemoryEntries?: ReturnType<typeof vi.fn>
  entries?: MemoryEntry[]
} = {}) {
  useAppStore.setState({
    folders: [
      {
        id: 'default',
        name: 'General',
        parentId: null,
        directory: '/tmp/project',
        isExpanded: true,
        createdAt: new Date(),
      },
    ],
    resourceCollections: [],
    memoryLibraryScope: {
      scopeType: 'all',
      folderId: '',
      label: 'All memory',
      rootPath: 'Global and project memory roots',
      rootCount: 2,
    },
    memoryLibraryEntries: entries,
    memoryLibraryLoading: false,
    memoryLibraryError: '',
    closeMemoryLibrary: vi.fn(),
    refreshMemoryLibrary: vi.fn().mockResolvedValue(true),
    createMemoryEntry,
    deleteMemoryEntry: vi.fn().mockResolvedValue(true),
    deleteMemoryEntries,
    openMemoryRoot: vi.fn().mockResolvedValue(true),
    revealMemoryEntry: vi.fn().mockResolvedValue(true),
  })

  const host = document.createElement('div')
  document.body.appendChild(host)
  const root = createRoot(host)

  act(() => {
    root.render(<MemoryLibraryModal />)
  })

  return { host, root, createMemoryEntry, deleteMemoryEntries }
}

function clickButton(host: HTMLElement, text: string) {
  const button = Array.from(host.querySelectorAll('button')).find((candidate) =>
    candidate.textContent?.includes(text) || candidate.getAttribute('aria-label')?.includes(text)
  ) as HTMLButtonElement
  expect(button).toBeTruthy()
  act(() => {
    button.dispatchEvent(new MouseEvent('click', { bubbles: true }))
  })
}

function changeInput(input: HTMLInputElement, value: string) {
  const setter = Object.getOwnPropertyDescriptor(window.HTMLInputElement.prototype, 'value')?.set
  act(() => {
    setter?.call(input, value)
    input.dispatchEvent(new Event('input', { bubbles: true }))
  })
}

describe('MemoryLibraryModal all memory scope', () => {
  beforeEach(() => {
    vi.restoreAllMocks()
    useAppStore.setState({
      memoryLibraryScope: null,
      memoryLibraryEntries: [],
      memoryLibraryLoading: false,
      memoryLibraryError: '',
    })
  })

  it('separates memory locations into a navigation rail', () => {
    const { host, root } = renderModal()

    expect(host.textContent).toContain('All Memory')
    const locations = host.querySelector('nav[aria-label="Memory locations"]') as HTMLElement
    expect(locations).toBeTruthy()
    expect(host.textContent).toContain('Global memory')
    expect(host.textContent).toContain('General')
    expect(host.textContent).toContain('Global lesson')
    expect(host.textContent).toContain('AI lessons')
    expect(host.textContent).not.toContain('Local lesson')
    expect(locations.querySelector('button[aria-current="page"]')?.textContent).toContain('Global memory')
    expect(host.textContent).not.toContain('Open memory root')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('shows a target picker when adding from all memory', () => {
    const createMemoryEntry = vi.fn().mockResolvedValue(true)
    const { host, root } = renderModal({ createMemoryEntry })

    clickButton(host, 'Add memory')

    const sourceChatInput = Array.from(host.querySelectorAll('label')).find((label) => label.textContent?.includes('Source chat'))?.querySelector('input')
    expect(sourceChatInput?.parentElement?.parentElement?.className).not.toContain('grid-cols-2')

    expect(host.querySelector('select')).toBeNull()
    const targetButton = Array.from(host.querySelectorAll('button')).find(
      (button) => button.getAttribute('aria-haspopup') === 'listbox' && button.textContent?.includes('Global memory')
    ) as HTMLButtonElement | undefined
    expect(targetButton).toBeTruthy()

    act(() => {
      targetButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    const options = Array.from(document.body.querySelectorAll('[role="option"]')).map((option) => option.textContent?.replace('●', '').trim())
    expect(options).toEqual(['Global memory', 'General'])
    expect(createMemoryEntry).not.toHaveBeenCalled()

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('saves a memory only once before the submitting state rerenders', async () => {
    const finishes: Array<(ok: boolean) => void> = []
    const createMemoryEntry = vi.fn(() => new Promise<boolean>((resolve) => finishes.push(resolve)))
    const { host, root } = renderModal({ createMemoryEntry })
    clickButton(host, 'Add memory')
    const title = Array.from(host.querySelectorAll('label')).find((label) => label.textContent?.startsWith('Title'))?.querySelector('input') as HTMLInputElement
    const memory = Array.from(host.querySelectorAll('label')).find((label) => label.textContent?.startsWith('Memory'))?.querySelector('textarea') as HTMLTextAreaElement
    changeInput(title, 'Remember once')
    act(() => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set?.call(memory, 'Only one persisted entry')
      memory.dispatchEvent(new Event('input', { bubbles: true }))
    })
    const save = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Save memory') as HTMLButtonElement

    act(() => {
      save.click()
      save.click()
    })

    expect(createMemoryEntry).toHaveBeenCalledTimes(1)
    await act(async () => {
      finishes.forEach((finish) => finish(false))
      await Promise.resolve()
    })
    act(() => root.unmount())
    host.remove()
  })

  it('dismisses nested UI before the library and preserves a dirty add draft', () => {
    const { host, root } = renderModal()
    const closeMemoryLibrary = useAppStore.getState().closeMemoryLibrary as ReturnType<typeof vi.fn>

    clickButton(host, 'Add memory')
    const title = Array.from(host.querySelectorAll('label')).find((label) => label.textContent?.startsWith('Title'))?.querySelector('input') as HTMLInputElement
    changeInput(title, 'Keep this draft')

    act(() => window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })))
    expect(closeMemoryLibrary).not.toHaveBeenCalled()
    expect(host.textContent).not.toContain('New memory')

    clickButton(host, 'Add memory')
    expect((Array.from(host.querySelectorAll('label')).find((label) => label.textContent?.startsWith('Title'))?.querySelector('input') as HTMLInputElement).value).toBe('Keep this draft')

    clickButton(host, 'Delete Global lesson')
    expect(host.querySelector('[role="dialog"][aria-label="Delete memory entry"]')).toBeTruthy()
    act(() => window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })))
    expect(host.querySelector('[role="dialog"][aria-label="Delete memory entry"]')).toBeNull()
    expect(closeMemoryLibrary).not.toHaveBeenCalled()

    act(() => window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })))
    expect(host.textContent).not.toContain('New memory')
    act(() => window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })))
    expect(closeMemoryLibrary).toHaveBeenCalledTimes(1)

    act(() => root.unmount())
    host.remove()
  })

  it('shows the selected memory location in the content pane', () => {
    const { host, root } = renderModal()

    clickButton(host, 'General')

    expect(host.textContent).toContain('General')
    expect(host.textContent).toContain('Local lesson')
    expect(host.textContent).toContain('Local failure')
    expect(host.textContent).not.toContain('Global lesson')
    expect(host.querySelector('nav[aria-label="Memory locations"] button[aria-current="page"]')?.textContent).toContain('General')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('filters entries before building location and category groups', () => {
    const { host, root } = renderModal()

    const searchInput = host.querySelector('input[placeholder^="Search memory"]') as HTMLInputElement
    expect(searchInput).toBeTruthy()
    changeInput(searchInput, 'global')

    expect(host.textContent).toContain('Global memory')
    expect(host.textContent).toContain('Global lesson')
    expect(host.textContent).toContain('AI lessons')
    expect(host.textContent).toContain('Unassigned workspaces')
    expect(host.textContent).toContain('General')
    expect(host.textContent).not.toContain('Local lesson')
    expect(host.textContent).not.toContain('AI failures')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('explains when a memory search has no matches', () => {
    const { host, root } = renderModal()
    const searchInput = host.querySelector('input[placeholder^="Search memory"]') as HTMLInputElement

    changeInput(searchInput, 'nothing-matches-this')

    expect(host.textContent).toContain('No memory matches this search')
    expect(host.textContent).toContain('Try another term or clear the search.')
    expect(host.textContent).not.toContain('No memory saved here yet')

    act(() => root.unmount())
    host.remove()
  })

  it('deletes only the currently visible memory entries after confirmation', () => {
    const deleteMemoryEntries = vi.fn().mockResolvedValue(true)
    const { host, root } = renderModal({ deleteMemoryEntries })

    const searchInput = host.querySelector('input[placeholder^="Search memory"]') as HTMLInputElement
    expect(searchInput).toBeTruthy()
    changeInput(searchInput, 'local')

    clickButton(host, 'Delete matches')

    expect(host.textContent).toContain('Delete matching memories?')
    expect(host.textContent).toContain('This permanently deletes 2 matching memory entries')
    expect(deleteMemoryEntries).not.toHaveBeenCalled()

    clickButton(host, 'Delete 2 memories')

    expect(deleteMemoryEntries).toHaveBeenCalledWith([
      'all/726f6f74/Lessons/User_Lessons/local.md',
      'all/726f6f74/Failures/AI_Failures/local-failure.md',
    ])

    act(() => {
      root.unmount()
    })
    host.remove()
  })
})
