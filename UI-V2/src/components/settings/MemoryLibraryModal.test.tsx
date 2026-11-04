import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { useAppStore } from '../../store/useAppStore'
import { MemoryLibraryModal } from './MemoryLibraryModal'
import type { MemoryEntry } from '../../types/memory'
import { APP_FAILURE_EVENT, type AppFailure } from '../../utils/appFailure'

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
  readMemoryEntry = vi.fn().mockResolvedValue({ ok: true, data: { content: '' } }),
  entries = defaultEntries,
}: {
  createMemoryEntry?: ReturnType<typeof vi.fn>
  deleteMemoryEntries?: ReturnType<typeof vi.fn>
  readMemoryEntry?: ReturnType<typeof vi.fn>
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
    readMemoryEntry,
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
  const button = Array.from(document.body.querySelectorAll('button')).find((candidate) =>
    candidate.textContent?.includes(text) || candidate.getAttribute('aria-label')?.includes(text)
  ) as HTMLButtonElement
  expect(button).toBeTruthy()
  act(() => {
    button.dispatchEvent(new MouseEvent('click', { bubbles: true }))
  })
}

/** Opens the "More memory actions" menu and picks the option whose text includes `text`. */
function chooseMenuOption(text: string) {
  const trigger = document.body.querySelector<HTMLButtonElement>('button[aria-label="More memory actions"]')
  expect(trigger).toBeTruthy()
  act(() => {
    trigger?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
  })
  const option = Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="option"]')).find((candidate) => candidate.textContent?.includes(text))
  expect(option).toBeTruthy()
  act(() => {
    option?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
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

    expect(document.body.textContent).toContain('All memory')
    const locations = document.body.querySelector('nav[aria-label="Memory locations"]') as HTMLElement
    expect(locations).toBeTruthy()
    expect(document.body.textContent).toContain('Global')
    expect(document.body.textContent).toContain('General')
    expect(document.body.textContent).toContain('Global lesson')
    expect(document.body.textContent).toContain('Lessons')
    expect(document.body.textContent).not.toContain('Local lesson')
    expect(locations.querySelector('button[aria-current="page"]')?.textContent).toContain('Global')
    expect(document.body.textContent).not.toContain('Open memory root')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('keeps saved SSH workspaces in the existing location rail and target picker', () => {
    const { host, root } = renderModal()
    act(() => {
      const local = useAppStore.getState().folders[0]
      useAppStore.setState({
        executionHosts: [{ id: 'ssh-one', label: 'Build host', transport: 'ssh', sshAlias: 'build-host', platform: 'linux', runnerVersion: '4.9.0', runnerProtocolVersion: 3, runnerDirectory: '.uam', runnerStatus: 'ready', architecture: 'x86_64', lastSeenAt: '' }],
        folders: [local, { ...local, id: 'remote-project', name: 'Remote project', directory: '/srv/project', executionHostId: 'ssh-one' }],
      })
    })
    expect(document.body.querySelector('nav[aria-label="Memory locations"]')?.textContent).toContain('Remote project · Build host')
    clickButton(document.body, 'New memory')
    const target = Array.from(document.body.querySelectorAll('button')).find((button) => button.getAttribute('aria-haspopup') === 'listbox' && button.textContent?.includes('Global memory'))
    act(() => { target?.dispatchEvent(new MouseEvent('click', { bubbles: true })) })
    expect(Array.from(document.body.querySelectorAll('[role="option"]')).some((option) => option.textContent?.includes('Remote project · Build host'))).toBe(true)
    act(() => { root.unmount() })
    host.remove()
  })

  it('shows a target picker when adding from all memory', () => {
    const createMemoryEntry = vi.fn().mockResolvedValue(true)
    const { host, root } = renderModal({ createMemoryEntry })

    clickButton(document.body, 'New memory')

    const sourceChatInput = document.getElementById('memory-draft-source-chat')
    expect(sourceChatInput?.parentElement?.parentElement?.className).not.toContain('grid-cols-2')

    expect(document.body.querySelector('select')).toBeNull()
    const targetButton = Array.from(document.body.querySelectorAll('button')).find(
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
    clickButton(document.body, 'New memory')
    const title = document.getElementById('memory-draft-title') as HTMLInputElement
    const memory = document.getElementById('memory-draft-memory') as HTMLTextAreaElement
    changeInput(title, 'Remember once')
    act(() => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set?.call(memory, 'Only one persisted entry')
      memory.dispatchEvent(new Event('input', { bubbles: true }))
    })
    const save = Array.from(document.body.querySelectorAll('button')).find((button) => button.textContent === 'Save memory') as HTMLButtonElement

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

    clickButton(document.body, 'New memory')
    const title = document.getElementById('memory-draft-title') as HTMLInputElement
    changeInput(title, 'Keep this draft')

    act(() => window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })))
    expect(closeMemoryLibrary).not.toHaveBeenCalled()
    expect(document.body.querySelector('[aria-label="Unsaved memory"]')).not.toBeNull()
    clickButton(document.body, 'Go back')
    expect((document.getElementById('memory-draft-title') as HTMLInputElement).value).toBe('Keep this draft')

    clickButton(document.body, 'Back')
    clickButton(document.body, 'Discard')
    expect(document.body.querySelector('[aria-label="New memory"]')).toBeNull()

    clickButton(document.body, 'Delete Global lesson')
    expect(document.body.querySelector('[role="alertdialog"][aria-label="Delete memory entry"]')).toBeTruthy()
    act(() => window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })))
    expect(document.body.querySelector('[role="alertdialog"][aria-label="Delete memory entry"]')).toBeNull()
    expect(closeMemoryLibrary).not.toHaveBeenCalled()

    act(() => window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })))
    expect(closeMemoryLibrary).toHaveBeenCalledTimes(1)

    act(() => root.unmount())
    host.remove()
  })

  it('shows the selected memory location in the content pane', () => {
    const { host, root } = renderModal()

    clickButton(document.body, 'General')

    expect(document.body.textContent).toContain('General')
    expect(document.body.textContent).toContain('Local lesson')
    expect(document.body.textContent).toContain('Local failure')
    expect(document.body.textContent).not.toContain('Global lesson')
    expect(document.body.querySelector('nav[aria-label="Memory locations"] button[aria-current="page"]')?.textContent).toContain('General')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('filters entries before building location and category groups', () => {
    const { host, root } = renderModal()

    const searchInput = document.body.querySelector('input[placeholder^="Search memory"]') as HTMLInputElement
    expect(searchInput).toBeTruthy()
    changeInput(searchInput, 'global')

    expect(document.body.textContent).toContain('Global')
    expect(document.body.textContent).toContain('Global lesson')
    expect(document.body.textContent).toContain('Lessons')
    expect(document.body.textContent).toContain('Workspaces')
    expect(document.body.textContent).toContain('General')
    expect(document.body.textContent).not.toContain('Local lesson')
    expect(document.body.textContent).not.toContain('Mistakes to avoid')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('explains when a memory search has no matches', () => {
    const { host, root } = renderModal()
    const searchInput = document.body.querySelector('input[placeholder^="Search memory"]') as HTMLInputElement

    changeInput(searchInput, 'nothing-matches-this')

    expect(document.body.textContent).toContain('No memory matches this search')
    expect(document.body.textContent).not.toContain('No memory saved here yet')

    act(() => root.unmount())
    host.remove()
  })

  it('shows a repeated memory failure after the previous error was dismissed', () => {
    const { host, root } = renderModal()
    const deleteMemoryEntry = vi.fn(async () => {
      useAppStore.setState({ memoryLibraryError: 'Permission denied' })
      return false
    })
    act(() => useAppStore.setState({ deleteMemoryEntry }))
    const attemptDelete = () => {
      clickButton(document.body, 'Delete Global lesson')
      const dialog = document.body.querySelector('[aria-label="Delete memory entry"]') as HTMLElement
      clickButton(dialog, 'Delete memory')
    }
    attemptDelete()
    expect(document.body.textContent).toContain('Permission denied')
    clickButton(document.body, 'Dismiss memory error')
    expect(document.body.textContent).not.toContain('Permission denied')
    expect(useAppStore.getState().memoryLibraryError).toBe('')

    attemptDelete()
    expect(deleteMemoryEntry).toHaveBeenCalledTimes(2)
    expect(document.body.textContent).toContain('Permission denied')
    act(() => root.unmount())
    host.remove()
  })

  it('deletes only the currently visible memory entries after confirmation', () => {
    const deleteMemoryEntries = vi.fn().mockResolvedValue(true)
    const { host, root } = renderModal({ deleteMemoryEntries })

    const searchInput = document.body.querySelector('input[placeholder^="Search memory"]') as HTMLInputElement
    expect(searchInput).toBeTruthy()
    changeInput(searchInput, 'local')

    chooseMenuOption('Delete matches')

    expect(document.body.textContent).toContain('Delete matching memories?')
    expect(document.body.textContent).toContain('This permanently deletes 2 memory entries')
    expect(deleteMemoryEntries).not.toHaveBeenCalled()

    clickButton(document.body, 'Delete 2 memories')

    expect(deleteMemoryEntries).toHaveBeenCalledWith([
      'all/726f6f74/Lessons/User_Lessons/local.md',
      'all/726f6f74/Failures/AI_Failures/local-failure.md',
    ])

    act(() => {
      root.unmount()
    })
    host.remove()
  })
  it.each(['title', 'memory'] as const)('disables invalid memory Save and leave until the %s is restored', async missing => {
    const createMemoryEntry = vi.fn().mockResolvedValue(true)
    const { host, root } = renderModal({ createMemoryEntry })
    try {
      clickButton(document.body, 'New memory')
      const title = document.body.querySelector<HTMLInputElement>('[aria-label="Title"]')!
      const memory = document.getElementById('memory-draft-memory')!
      const setMemory = (value: string) => act(() => {
        Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')!.set!.call(memory, value)
        memory.dispatchEvent(new Event('input', { bubbles: true }))
      })
      changeInput(title, 'Remember this')
      setMemory('Useful memory')
      if (missing === 'title') changeInput(title, '  ')
      else setMemory('  ')
      expect(Array.from(document.body.querySelectorAll('button')).find(button => button.textContent?.startsWith('Save memory'))!.getAttribute('aria-disabled')).toBe('true')
      act(() => window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })))
      const leaveSave = () => Array.from(document.body.querySelectorAll('button')).find(button => button.textContent === 'Save and leave')!
      expect(leaveSave().disabled).toBe(true)
      expect(Array.from(document.body.querySelectorAll('button')).find(button => button.textContent === 'Go back')!.disabled).toBe(false)
      expect(Array.from(document.body.querySelectorAll('button')).find(button => button.textContent === 'Discard')!.disabled).toBe(false)
      clickButton(document.body, 'Save and leave')
      expect(createMemoryEntry).not.toHaveBeenCalled()
      clickButton(document.body, 'Go back')
      if (missing === 'title') changeInput(title, 'Remember this')
      else setMemory('Useful memory')
      act(() => window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })))
      expect(leaveSave().disabled).toBe(false)
      await act(async () => clickButton(document.body, 'Save and leave'))
      expect(createMemoryEntry).toHaveBeenCalledOnce()
      expect(createMemoryEntry).toHaveBeenCalledWith(expect.objectContaining({ title: 'Remember this', memory: 'Useful memory' }))
    } finally { act(() => root.unmount()); host.remove() }
  })

})

describe('MemoryLibraryModal rows and in-place view', () => {
  beforeEach(() => {
    vi.restoreAllMocks()
    useAppStore.setState({ memoryLibraryScope: null, memoryLibraryEntries: [], memoryLibraryLoading: false, memoryLibraryError: '' })
  })

  it('opens a memory in the content pane and returns with Back', () => {
    const { host, root } = renderModal()
    clickButton(document.body, 'Global lesson')
    expect(document.body.textContent).toContain('Occurrences')
    expect(document.body.querySelector('.uam-memory-row')).toBeNull()
    clickButton(document.body, 'Back')
    expect(document.body.querySelector('.uam-memory-row')).not.toBeNull()
    act(() => root.unmount())
    host.remove()
  })

  it('shows the complete memory text after the detail view loads it', async () => {
    const fullText = 'First full line.\n\nSecond full paragraph beyond the preview.'
    const readMemoryEntry = vi.fn().mockResolvedValue({ ok: true, data: { content: fullText } })
    const { host, root } = renderModal({ readMemoryEntry })

    clickButton(document.body, 'Global lesson')
    expect(readMemoryEntry).toHaveBeenCalledWith({ scopeType: 'all', folderId: '', entryId: defaultEntries[0].id })
    // The preview stays on screen until the full text arrives.
    expect(document.body.querySelector('.uam-store-markdown')?.textContent).toBe('Remember this across projects.')

    await act(async () => { await Promise.resolve() })
    expect(document.body.querySelector('.uam-store-markdown')?.textContent).toBe(fullText)

    act(() => root.unmount())
    host.remove()
  })

  it('keeps the preview and reports a failure when the full text cannot be loaded', async () => {
    const failures: AppFailure[] = []
    const onFailure = (event: Event) => failures.push((event as CustomEvent<AppFailure>).detail)
    window.addEventListener(APP_FAILURE_EVENT, onFailure)
    const readMemoryEntry = vi.fn().mockResolvedValue({ ok: false, error: 'Memory entry could not be read.' })
    const { host, root } = renderModal({ readMemoryEntry })

    clickButton(document.body, 'Global lesson')
    await act(async () => { await Promise.resolve() })

    expect(document.body.querySelector('.uam-store-markdown')?.textContent).toBe('Remember this across projects.')
    expect(failures.map(({ message, detail }) => ({ message, detail }))).toEqual([
      { message: 'Could not load the full memory.', detail: 'Try again.' },
    ])

    window.removeEventListener(APP_FAILURE_EVENT, onFailure)
    act(() => root.unmount())
    host.remove()
  })

  it('hides the search toolbar while adding a memory', () => {
    const { host, root } = renderModal()
    expect(document.body.querySelector('input[placeholder^="Search memory"]')).not.toBeNull()
    clickButton(document.body, 'New memory')
    expect(document.body.querySelector('input[placeholder^="Search memory"]')).toBeNull()
    act(() => root.unmount())
    host.remove()
  })
})
