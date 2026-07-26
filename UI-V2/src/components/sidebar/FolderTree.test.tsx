import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { useAppStore } from '../../store/useAppStore'
import type { Folder, Session } from '../../types/session'
import { FolderTree } from './FolderTree'
import { defaultChatGridLayout, writeChatGridLayout } from '../../utils/chatGridStorage'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

const now = new Date('2026-01-01T12:00:00.000Z')

function makeFolder(): Folder {
  return {
    id: 'project',
    name: 'Project',
    parentId: null,
    directory: '/tmp/project',
    isExpanded: true,
    createdAt: now,
  }
}

function makeSession(index: number): Session {
  const openedAt = new Date(now)
  openedAt.setMinutes(now.getMinutes() - index)

  return {
    id: `chat-${index}`,
    name: `Chat ${index}`,
    viewMode: 'chat',
    folderId: 'project',
    createdAt: openedAt,
    updatedAt: openedAt,
    lastOpenedAt: openedAt,
  }
}

describe('FolderTree', () => {
  beforeEach(() => {
    vi.restoreAllMocks()
    useAppStore.setState({
      folders: [makeFolder()],
      sessions: Array.from({ length: 7 }, (_, index) => makeSession(index + 1)),
      activeSessionId: 'chat-1',
      messages: {},
      cliBindingBySessionId: {},
      acpBindingBySessionId: {},
      cliTranscriptBySessionId: {},
      resourceCollections: [],
      isNewChatModalOpen: false,
      newChatFolderId: null,
      openFolderMemoryLibrary: vi.fn(() => Promise.resolve(true)),
      rescanFolderChats: vi.fn(() => Promise.resolve(true)),
    })
  })

  it('shows five recent chats in a folder until see more is clicked', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<FolderTree searchQuery="" />)
    })

    expect(host.textContent).toContain('Chat 1')
    expect(host.textContent).toContain('Chat 5')
    expect(host.textContent).not.toContain('Chat 6')
    expect(host.textContent).not.toContain('⌃')
    expect(host.textContent).toContain('Show 2 more')
    expect(host.textContent).not.toContain('See more')
    expect(Array.from(host.querySelectorAll('button')).filter((button) => button.textContent?.trim() === 'New chat')).toHaveLength(0)
    expect((host.querySelector('[data-testid="folder-icon-project"]') as HTMLElement).style.color).toBe('var(--text-3)')
    const activeTitle = Array.from(host.querySelectorAll('span')).find((span) => span.textContent === 'Chat 1')
    expect(activeTitle?.getAttribute('style')).toContain('var(--text)')
    expect(activeTitle?.getAttribute('style')).not.toContain('#ffffff')

    const seeMoreButton = Array.from(host.querySelectorAll('button')).find((button) =>
      button.textContent?.includes('Show 2 more')
    )
    expect(seeMoreButton).toBeTruthy()
    expect(seeMoreButton?.style.borderStyle).toBe('none')

    act(() => {
      seeMoreButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(host.textContent).toContain('Chat 6')
    expect(host.textContent).toContain('Chat 7')
    expect(host.textContent).toContain('Show less')
    expect(host.querySelector('[data-testid="folder-row-project"]')?.className).toContain('mb-0')
    expect(Array.from(host.querySelectorAll('span')).find((span) => span.textContent === 'Chat 1')?.closest('.cursor-pointer')?.className).toContain('py-1')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('uses collections only to group, collapse, and accept workspace folders', async () => {
    const second = { ...makeFolder(), id: 'second', name: 'Second', directory: '/tmp/second' }
    useAppStore.setState({
      folders: [makeFolder(), second],
      resourceCollections: [{
        id: 'work',
        name: 'Work',
        collapsed: false,
        references: [{ id: 'project-ref', type: 'workspace-folder', target: 'project', label: 'Project' }],
      }],
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    const collection = host.querySelector('[data-testid="folder-collection-work"]')
    expect(collection?.querySelector('[data-testid="folder-row-project"]')).toBeTruthy()
    const collectionHeader = collection?.firstElementChild as HTMLElement
    const collectionChildren = collection?.querySelector('[data-testid="collection-children-work"]') as HTMLElement
    expect(collectionChildren.className).toContain('ml-3')
    expect(collectionChildren.className).toContain('pl-1')
    expect(collectionChildren.className).toContain('py-0')
    expect(collectionChildren.className).not.toContain('border-l-2')
    expect(collectionHeader.style.border).toBe('1px solid transparent')
    expect(collectionChildren.style.background).toBe('transparent')
    expect(collectionChildren.style.boxShadow).toBe('none')
    expect(collection?.querySelector('[data-testid="folder-row-second"]')).toBeNull()
    expect(host.querySelectorAll('[data-testid="folder-row-project"]')).toHaveLength(1)
    expect(host.textContent).not.toContain('Collections')

    act(() => {
      collection?.querySelector('button[aria-label="Collapse Work"]')
        ?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(host.querySelector('[data-testid="collection-children-work"]')?.parentElement?.parentElement?.getAttribute('aria-hidden')).toBe('true')
    expect(host.querySelector('[data-testid="folder-row-second"]')).toBeTruthy()

    const data = new Map<string, string>()
    const transfer = {
      effectAllowed: '',
      get types() { return [...data.keys()] },
      getData: (type: string) => data.get(type) ?? '',
      setData: (type: string, value: string) => { data.set(type, value) },
    }
    const dragStart = new Event('dragstart', { bubbles: true })
    Object.defineProperty(dragStart, 'dataTransfer', { value: transfer })
    const dragOver = new Event('dragover', { bubbles: true, cancelable: true })
    Object.defineProperty(dragOver, 'dataTransfer', { value: transfer })
    const drop = new Event('drop', { bubbles: true, cancelable: true })
    Object.defineProperty(drop, 'dataTransfer', { value: transfer })

    await act(async () => {
      host.querySelector('[data-testid="folder-row-second"]')?.dispatchEvent(dragStart)
      collection?.dispatchEvent(dragOver)
      collection?.dispatchEvent(drop)
    })

    expect(useAppStore.getState().resourceCollections[0].references.map((reference) => reference.target)).toEqual(['project', 'second'])
    expect(collection?.querySelector('[data-testid="folder-row-second"]')).toBeTruthy()

    act(() => root.unmount())
    host.remove()
  })

  it('compacts collection, workspace, and chat rows without hiding their actions', () => {
    useAppStore.setState({
      resourceCollections: [{
        id: 'work',
        name: 'Work',
        collapsed: false,
        references: [{ id: 'project-ref', type: 'workspace-folder', target: 'project', label: 'Project' }],
      }],
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    const collection = host.querySelector<HTMLElement>('[data-testid="folder-collection-work"]')
    const collectionHeader = host.querySelector<HTMLElement>('[data-testid="collection-header-work"]')
    const folder = host.querySelector<HTMLElement>('[data-testid="folder-row-project"]')
    const folderHeader = host.querySelector<HTMLElement>('[data-testid="folder-header-project"]')
    const chat = host.querySelector<HTMLElement>('[data-testid="session-row-chat-1"]')

    expect(collection?.className).toContain('mb-0')
    expect(collectionHeader?.className).toContain('gap-1.5')
    expect(collectionHeader?.className).toContain('px-2.5')
    expect(collectionHeader?.className).toContain('py-0.5')
    expect(folder?.className).toContain('mb-0')
    expect(folder?.style.boxShadow).toBe('none')
    expect(folderHeader?.className).toContain('gap-1.5')
    expect(folderHeader?.className).toContain('px-2.5')
    expect(folderHeader?.className).toContain('py-0.5')
    expect(chat?.className).toContain('gap-1.5')
    expect(chat?.className).toContain('px-2.5')
    expect(chat?.className).toContain('py-1')
    expect(chat?.parentElement?.parentElement?.className).toContain('pl-3.5')
    expect(chat?.querySelector('button[aria-label="More actions"]')).toBeTruthy()

    act(() => root.unmount())
    host.remove()
  })

  it('opens collection actions in a dismissible viewport menu without expanding the collection layout', () => {
    vi.stubGlobal('ResizeObserver', class { observe() {}; unobserve() {}; disconnect() {} })
    useAppStore.setState({
      resourceCollections: [{ id: 'work', name: 'Work', collapsed: false, references: [] }],
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    const collection = host.querySelector('[data-testid="folder-collection-work"]') as HTMLElement
    const trigger = collection.querySelector('button[aria-label="Actions for Work"]') as HTMLButtonElement
    const childCount = collection.children.length
    act(() => trigger.dispatchEvent(new MouseEvent('click', { bubbles: true })))

    const menu = document.body.querySelector('[role="menu"][aria-label="Work actions"]') as HTMLElement
    expect(menu).toBeTruthy()
    expect(collection.contains(menu)).toBe(false)
    expect(collection.children).toHaveLength(childCount)
    expect(trigger.getAttribute('aria-expanded')).toBe('true')
    expect(document.activeElement?.textContent).toContain('Rename')

    act(() => document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })))
    expect(document.body.querySelector('[aria-label="Work actions"]')).toBeNull()
    expect(document.activeElement).toBe(trigger)

    act(() => root.unmount())
    host.remove()
    vi.unstubAllGlobals()
  })

  it('opens collection actions from the collection context menu', () => {
    vi.stubGlobal('ResizeObserver', class { observe() {}; unobserve() {}; disconnect() {} })
    useAppStore.setState({
      resourceCollections: [{ id: 'work', name: 'Work', collapsed: false, references: [] }],
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    const header = host.querySelector('[data-testid="folder-collection-work"] > div') as HTMLElement
    act(() => header.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, clientX: 80, clientY: 90 })))

    const menu = document.body.querySelector('[role="menu"][aria-label="Work actions"]')
    expect(menu?.textContent).toContain('Rename')
    expect(menu?.textContent).toContain('Delete')

    act(() => root.unmount())
    host.remove()
    vi.unstubAllGlobals()
  })

  it('removes a workspace from its collection without deleting the workspace or chats', async () => {
    useAppStore.setState({
      resourceCollections: [{
        id: 'work',
        name: 'Work',
        collapsed: false,
        references: [{ id: 'project-ref', type: 'workspace-folder', target: 'project', label: 'Project' }],
      }],
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    const collectedFolder = host.querySelector('[data-testid="folder-collection-work"] [data-testid="folder-row-project"]') as HTMLElement
    act(() => collectedFolder.firstElementChild?.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, clientX: 20, clientY: 20 })))

    const menu = document.body.querySelector('[data-viewport-menu]') as HTMLElement
    expect(menu).toBeTruthy()
    expect(collectedFolder.contains(menu)).toBe(false)
    expect(menu.textContent).not.toContain('Remove from collection')
    const move = Array.from(menu.querySelectorAll('button')).find((button) => button.textContent?.includes('Move to collection')) as HTMLButtonElement
    act(() => move.click())
    const submenu = document.body.querySelector('[role="menu"][aria-label="Move to collection"]') as HTMLElement
    expect(submenu).toBeTruthy()
    const remove = Array.from(submenu.querySelectorAll('button')).find((button) => button.textContent?.includes('Remove from collection'))
    await act(async () => { remove?.click(); await Promise.resolve() })

    expect(useAppStore.getState().resourceCollections[0].references).toEqual([])
    expect(useAppStore.getState().folders.map((folder) => folder.id)).toContain('project')
    expect(useAppStore.getState().sessions).toHaveLength(7)
    expect(host.querySelector('[data-testid="folder-collection-work"] [data-testid="folder-row-project"]')).toBeNull()
    expect(host.querySelector('[data-testid="folder-row-project"]')).toBeTruthy()

    act(() => root.unmount())
    host.remove()
  })

  it('marks a missing workspace folder accessibly without removing its chats', () => {
    useAppStore.setState((state) => ({
      folders: state.folders.map((folder) => ({ ...folder, missing: true })),
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    const warning = host.querySelector('[data-testid="folder-missing-project"]')
    expect(warning?.getAttribute('aria-label')).toBe('Workspace folder missing: /tmp/project')
    expect(host.textContent).toContain('Chat 1')

    act(() => root.unmount())
    host.remove()
  })

  it('shows a drop indicator and persists drag reordering', () => {
    const second = { ...makeFolder(), id: 'second', name: 'Second', directory: '/tmp/second' }
    const reorderFolders = vi.fn(async () => true)
    useAppStore.setState({ folders: [makeFolder(), second], sessions: [], reorderFolders })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    const source = host.querySelector('[data-testid="folder-row-project"]') as HTMLElement
    const target = host.querySelector('[data-testid="folder-row-second"]') as HTMLElement
    vi.spyOn(target, 'getBoundingClientRect').mockReturnValue({ top: 0, bottom: 40, height: 40, left: 0, right: 100, width: 100, x: 0, y: 0, toJSON: () => ({}) })
    const transfer = { effectAllowed: '', setData: vi.fn() }
    const dragStart = new Event('dragstart', { bubbles: true })
    Object.defineProperty(dragStart, 'dataTransfer', { value: transfer })
    const dragOver = new Event('dragover', { bubbles: true, cancelable: true })
    Object.defineProperty(dragOver, 'clientY', { value: 35 })
    const drop = new Event('drop', { bubbles: true, cancelable: true })
    Object.defineProperty(drop, 'clientY', { value: 35 })

    act(() => {
      source.dispatchEvent(dragStart)
      target.dispatchEvent(dragOver)
    })
    expect(target.style.boxShadow).toContain('inset 0 -2px var(--accent)')
    act(() => target.dispatchEvent(drop))
    expect(reorderFolders).toHaveBeenCalledWith(['second', 'project'])

    act(() => root.unmount())
    host.remove()
  })

  it('persists dragged workspace order inside a collection', () => {
    const second = { ...makeFolder(), id: 'second', name: 'Second', directory: '/tmp/second' }
    const reorderFolders = vi.fn(async () => true)
    const reorderResourceReferences = vi.fn(async () => true)
    useAppStore.setState({
      folders: [makeFolder(), second],
      sessions: [],
      resourceCollections: [{
        id: 'work',
        name: 'Work',
        collapsed: false,
        references: [
          { id: 'project-ref', type: 'workspace-folder', target: 'project', label: 'Project' },
          { id: 'website-ref', type: 'website', target: 'https://example.com', label: 'Example' },
          { id: 'second-ref', type: 'workspace-folder', target: 'second', label: 'Second' },
        ],
      }],
      reorderFolders,
      reorderResourceReferences,
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    const source = host.querySelector('[data-testid="folder-row-project"]') as HTMLElement
    const keyboardSource = host.querySelector('[data-testid="folder-header-project"]') as HTMLElement
    const target = host.querySelector('[data-testid="folder-row-second"]') as HTMLElement
    vi.spyOn(target, 'getBoundingClientRect').mockReturnValue({ top: 0, bottom: 40, height: 40, left: 0, right: 100, width: 100, x: 0, y: 0, toJSON: () => ({}) })
    const data = new Map<string, string>()
    const transfer = {
      effectAllowed: '',
      get types() { return [...data.keys()] },
      getData: (type: string) => data.get(type) ?? '',
      setData: (type: string, value: string) => { data.set(type, value) },
    }
    const dragStart = new Event('dragstart', { bubbles: true })
    Object.defineProperty(dragStart, 'dataTransfer', { value: transfer })
    const drop = new Event('drop', { bubbles: true, cancelable: true })
    Object.defineProperty(drop, 'dataTransfer', { value: transfer })
    Object.defineProperty(drop, 'clientY', { value: 35 })

    act(() => source.dispatchEvent(dragStart))
    act(() => target.dispatchEvent(drop))

    expect(host.querySelector('[data-testid="folder-drag-handle-project"]')).toBeNull()
    expect(reorderResourceReferences).toHaveBeenCalledWith('work', ['second-ref', 'website-ref', 'project-ref'])
    expect(reorderFolders).not.toHaveBeenCalled()

    reorderResourceReferences.mockClear()
    act(() => keyboardSource.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowDown', bubbles: true })))
    expect(reorderResourceReferences).toHaveBeenCalledWith('work', ['second-ref', 'website-ref', 'project-ref'])

    act(() => root.unmount())
    host.remove()
  })

  it('selects a visible chat row after folders are grouped for rendering', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<FolderTree searchQuery="" />)
    })

    const chatTitle = Array.from(host.querySelectorAll('span')).find((span) => span.textContent === 'Chat 2')
    expect(chatTitle).toBeTruthy()

    act(() => {
      chatTitle?.parentElement?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(useAppStore.getState().activeSessionId).toBe('chat-2')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('shows every assigned pane colour when a collapsed folder layout changes', () => {
    const stored = new Map<string, string>()
    Object.defineProperty(globalThis, 'localStorage', {
      configurable: true,
      value: {
        getItem: (key: string) => stored.get(key) ?? null,
        setItem: (key: string, value: string) => stored.set(key, value),
      },
    })
    useAppStore.setState((state) => ({ folders: state.folders.map((folder) => ({ ...folder, isExpanded: false })) }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    expect(host.textContent).not.toContain('Chat 3')
    expect((host.querySelector('[data-testid="folder-icon-project"]') as HTMLElement).style.color).toBe('var(--text-3)')

    act(() => writeChatGridLayout({ ...defaultChatGridLayout, paneCount: 4, activePane: 1, sessionIds: ['chat-1', 'chat-3', '', ''] }))

    const folderIcon = host.querySelector('[data-testid="folder-icon-project"]') as HTMLElement
    expect(folderIcon.getAttribute('stroke')).not.toMatch(/^url\(#.+\)$/)
    expect(folderIcon.style.color).toBe('var(--text-3)')
    expect(folderIcon.style.filter).toBe('none')
    expect(host.querySelector('[role="img"][aria-label="Shown in panes 1 and 2"]')).toBeTruthy()
    expect(Array.from(host.querySelectorAll('[data-testid="folder-icon-project-marker"]')).map((marker) => (marker as HTMLElement).style.background)).toEqual([
      'rgb(249, 115, 22)', 'rgb(236, 72, 153)',
    ])

    act(() => root.unmount())
    host.remove()
  })

  it('shows every hidden workspace pane colour on a collapsed collection', () => {
    const stored = new Map<string, string>()
    Object.defineProperty(globalThis, 'localStorage', {
      configurable: true,
      value: {
        getItem: (key: string) => stored.get(key) ?? null,
        setItem: (key: string, value: string) => stored.set(key, value),
      },
    })
    useAppStore.setState({
      resourceCollections: [{
        id: 'work',
        name: 'Work',
        collapsed: true,
        references: [{ id: 'project-ref', type: 'workspace-folder', target: 'project', label: 'Project' }],
      }],
    })

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))
    act(() => writeChatGridLayout({ ...defaultChatGridLayout, paneCount: 4, activePane: 1, sessionIds: ['chat-1', 'chat-3', '', ''] }))

    const collectionIcon = host.querySelector('[data-testid="collection-icon-work"]') as HTMLElement
    expect(collectionIcon.getAttribute('stroke')).not.toMatch(/^url\(#.+\)$/)
    expect(collectionIcon.style.color).toBe('var(--text-3)')
    expect(host.querySelector('[role="img"][aria-label="Shown in panes 1 and 2"]')).toBeTruthy()
    expect(Array.from(host.querySelectorAll('[data-testid="collection-icon-work-marker"]')).map((marker) => (marker as HTMLElement).style.background)).toEqual([
      'rgb(249, 115, 22)', 'rgb(236, 72, 153)',
    ])

    act(() => root.unmount())
    host.remove()
  })

  it('selects a chat row after filters change the rendered group', () => {
    useAppStore.setState((state) => ({
      activeSessionId: 'chat-1',
      sessions: state.sessions.map((session) =>
        session.id === 'chat-6' ? { ...session, providerId: 'codex-cli' } : session
      ),
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<FolderTree searchQuery="" filters={{ providerIds: ['codex-cli'], statusIds: [] }} />)
    })

    expect(host.textContent).toContain('Chat 6')
    expect(host.textContent).not.toContain('Chat 1')

    const chatTitle = Array.from(host.querySelectorAll('span')).find((span) => span.textContent === 'Chat 6')
    expect(chatTitle).toBeTruthy()

    act(() => {
      chatTitle?.parentElement?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(useAppStore.getState().activeSessionId).toBe('chat-6')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('renders pinned chats above folders and pin clicks do not select chats', () => {
    useAppStore.setState((state) => ({
      activeSessionId: 'chat-3',
      sessions: state.sessions.map((session) =>
        session.id === 'chat-1' ? { ...session, isPinned: true } : session
      ),
    }))

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<FolderTree searchQuery="" />)
    })

    expect(host.textContent).toContain('Pinned chats')
    expect(host.textContent).toContain('Chat 1')

    expect(host.querySelector('[role="img"][aria-label="Pinned"]')).toBeTruthy()
    const pinButton = host.querySelector<HTMLButtonElement>('button[aria-label="Pin chat"]')
    expect(pinButton).toBeTruthy()
    const actions = pinButton?.closest<HTMLElement>('[data-testid^="session-actions-"]')
    expect(actions?.className).toContain('absolute')
    expect(actions?.className).toContain('opacity-0')
    expect(actions?.className).toContain('group-hover:opacity-100')
    expect(actions?.className).toContain('group-focus-within:opacity-100')

    act(() => {
      pinButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(useAppStore.getState().activeSessionId).toBe('chat-3')
    expect(useAppStore.getState().sessions.find((session) => session.id === 'chat-2')?.isPinned).toBe(true)

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('opens project memory from the folder action', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<FolderTree searchQuery="" />)
    })

    // Folder actions are now in an overflow menu — open it first.
    act(() => {
      (host.querySelector('button[aria-label="Folder actions"]') as HTMLButtonElement | null)
        ?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    const memoryButton = Array.from(document.body.querySelectorAll('button')).find((button) =>
      button.textContent?.includes('Project memory')
    )
    expect(memoryButton).toBeTruthy()

    act(() => {
      memoryButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(useAppStore.getState().openFolderMemoryLibrary).toHaveBeenCalledWith('project')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('opens the shared new-chat flow from the folder context menu', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    const folderHeader = host.querySelector('[data-testid="folder-row-project"]')?.firstElementChild
    act(() => folderHeader?.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true })))
    const newChat = Array.from(document.body.querySelectorAll('button')).find((button) => button.textContent?.includes('New chat'))
    act(() => newChat?.click())

    expect(useAppStore.getState()).toMatchObject({ isNewChatModalOpen: true, newChatFolderId: 'project' })

    act(() => root.unmount())
    host.remove()
  })

  it('rescans chats from the folder context menu', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    act(() => {
      host.querySelector('[data-testid="folder-header-project"]')
        ?.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true }))
    })
    const rescan = Array.from(document.body.querySelectorAll('button')).find((button) =>
      button.textContent?.includes('Rescan chats')
    )
    act(() => rescan?.click())

    expect(useAppStore.getState().rescanFolderChats).toHaveBeenCalledWith('project')

    act(() => root.unmount())
    host.remove()
  })

  it('shows a helpful empty search state', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="nothing-matches-this" />))

    expect(host.textContent).toContain('No chats match this search')
    expect(host.textContent).toContain('Try another term or clear the filters.')

    act(() => root.unmount())
    host.remove()
  })

  it('keeps the new-folder form visible in the scrolling sidebar', () => {
    const scrollIntoView = vi.fn()
    const previousScrollIntoView = HTMLElement.prototype.scrollIntoView
    HTMLElement.prototype.scrollIntoView = scrollIntoView
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    const actions = Array.from(host.querySelectorAll('button')).filter((button) => ['New workspace', 'New collection'].includes(button.textContent ?? ''))
    expect(actions.map((button) => button.textContent)).toEqual(['New workspace', 'New collection'])
    expect(actions[0].parentElement?.className).toContain('justify-center')
    const newFolder = actions[0]
    act(() => newFolder?.click())

    expect(scrollIntoView).toHaveBeenCalledWith({ block: 'nearest' })

    act(() => root.unmount())
    host.remove()
    HTMLElement.prototype.scrollIntoView = previousScrollIntoView
  })

  it('uses icon-only confirm and cancel actions when naming a collection', async () => {
    const createResourceCollection = vi.fn(async (name: string) => ({ id: 'ideas', name, collapsed: false, references: [] }))
    useAppStore.setState({ createResourceCollection })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    const openForm = () => Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'New collection')?.click()
    act(openForm)

    const confirm = host.querySelector('button[aria-label="Create collection"]') as HTMLButtonElement
    const cancel = host.querySelector('button[aria-label="Cancel new collection"]') as HTMLButtonElement
    expect(confirm.textContent).toBe('')
    expect(confirm.disabled).toBe(true)
    expect(confirm.style.background).toBe('var(--accent)')
    expect(cancel.textContent).toBe('')
    expect(cancel.style.color).toBe('var(--error)')

    act(() => cancel.click())
    expect(host.querySelector('input[aria-label="Collection name"]')).toBeNull()

    act(openForm)
    const input = host.querySelector('input[aria-label="Collection name"]') as HTMLInputElement
    act(() => {
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')?.set?.call(input, 'Ideas')
      input.dispatchEvent(new Event('input', { bubbles: true }))
    })
    const enabledConfirm = host.querySelector('button[aria-label="Create collection"]') as HTMLButtonElement
    expect(enabledConfirm.disabled).toBe(false)
    await act(async () => { enabledConfirm.click(); await Promise.resolve() })
    expect(createResourceCollection).toHaveBeenCalledWith('Ideas')

    act(() => root.unmount())
    host.remove()
  })
})
