import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { useAppStore } from '../../store/useAppStore'
import { MemoryLibraryModal } from './MemoryLibraryModal'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

function renderModal(createMemoryEntry = vi.fn().mockResolvedValue(true)) {
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
    memoryLibraryEntries: [
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
    ],
    memoryLibraryLoading: false,
    memoryLibraryError: '',
    closeMemoryLibrary: vi.fn(),
    refreshMemoryLibrary: vi.fn().mockResolvedValue(true),
    createMemoryEntry,
    deleteMemoryEntry: vi.fn().mockResolvedValue(true),
    openMemoryRoot: vi.fn().mockResolvedValue(true),
    revealMemoryEntry: vi.fn().mockResolvedValue(true),
  })

  const host = document.createElement('div')
  document.body.appendChild(host)
  const root = createRoot(host)

  act(() => {
    root.render(<MemoryLibraryModal />)
  })

  return { host, root, createMemoryEntry }
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

  it('shows aggregate entries with scope metadata', () => {
    const { host, root } = renderModal()

    expect(host.textContent).toContain('All Memory')
    expect(host.textContent).toContain('Local lesson')
    expect(host.textContent).toContain('General')
    expect(host.textContent).toContain('/tmp/project/.UAM')
    expect(host.textContent).not.toContain('Open memory root')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('shows a target picker when adding from all memory', () => {
    const createMemoryEntry = vi.fn().mockResolvedValue(true)
    const { host, root } = renderModal(createMemoryEntry)

    const addButton = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Add memory') as HTMLButtonElement
    act(() => {
      addButton.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

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
})
