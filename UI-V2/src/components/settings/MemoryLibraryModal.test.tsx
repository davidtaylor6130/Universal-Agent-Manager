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
  const button = Array.from(host.querySelectorAll('button')).find((candidate) => candidate.textContent?.includes(text)) as HTMLButtonElement
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

  it('groups aggregate entries by location and category', () => {
    const { host, root } = renderModal()

    expect(host.textContent).toContain('All Memory')
    expect(host.textContent).toContain('Global memory')
    expect(host.textContent).toContain('Global lesson')
    expect(host.textContent).toContain('Local lesson')
    expect(host.textContent).toContain('Local failure')
    expect(host.textContent).toContain('General')
    expect(host.textContent).toContain('/tmp/project/.UAM')
    expect(host.textContent).toContain('Lessons/AI_Lessons')
    expect(host.textContent).toContain('Lessons/User_Lessons')
    expect(host.textContent).toContain('Failures/AI_Failures')
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

    const targetSelect = host.querySelector('select') as HTMLSelectElement
    expect(targetSelect).toBeTruthy()
    expect(Array.from(targetSelect.options).map((option) => option.textContent)).toEqual([
      'Global memory',
      'General',
    ])
    expect(createMemoryEntry).not.toHaveBeenCalled()

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('collapses a location group without hiding other locations', () => {
    const { host, root } = renderModal()

    clickButton(host, 'General')

    expect(host.textContent).toContain('General')
    expect(host.textContent).not.toContain('Local lesson')
    expect(host.textContent).not.toContain('Local failure')
    expect(host.textContent).toContain('Global lesson')

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
    expect(host.textContent).toContain('Lessons/AI_Lessons')
    expect(host.textContent).not.toContain('General')
    expect(host.textContent).not.toContain('Local lesson')
    expect(host.textContent).not.toContain('Failures/AI_Failures')

    act(() => {
      root.unmount()
    })
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
    expect(host.textContent).toContain('This deletes 2 matching memory entries')
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
