import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { useAppStore } from '../../store/useAppStore'
import type { Folder, Session } from '../../types/session'
import { FolderTree } from './FolderTree'
import { chatGridLeaves, defaultChatGridLayout, setChatInLeaf, splitChatLeaf, writeChatGridLayout } from '../../utils/chatGridStorage'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

function gridLayout(...sessionIds: string[]) {
  let layout = defaultChatGridLayout
  while (chatGridLeaves(layout.root).length < sessionIds.length) layout = splitChatLeaf(layout, layout.activeLeafId, 'horizontal')
  chatGridLeaves(layout.root).forEach((leaf, index) => { layout = setChatInLeaf(layout, sessionIds[index] ?? '', leaf.id) })
  return layout
}

const now = new Date('2026-01-01T12:00:00.000Z')
const originalAddFolder = useAppStore.getState().addFolder
const originalListRemoteDirectories = useAppStore.getState().listRemoteDirectories

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
	  executionHosts: [
		{ id: 'local', label: 'This computer', transport: 'local', sshAlias: '', runnerStatus: 'ready', runnerVersion: '', platform: 'macos', architecture: 'arm64', lastSeenAt: '' },
	  ],
      isNewChatModalOpen: false,
      newChatFolderId: null,
      openFolderMemoryLibrary: vi.fn(() => Promise.resolve(true)),
      rescanFolderChats: vi.fn(() => Promise.resolve(true)),
      previewUnsortedWorkspaceFolders: vi.fn(() => Promise.resolve(null)),
      rebuildUnsortedWorkspaceFolders: vi.fn(() => Promise.resolve(true)),
      workspaceFolderRecoveryError: '',
      addFolder: originalAddFolder,
      listRemoteDirectories: originalListRemoteDirectories,
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

  it('shows the actual execution computer on workspace rows only', () => {
    useAppStore.setState({
      executionHosts: [
        { id: 'local', label: 'This computer', transport: 'local', sshAlias: '', runnerStatus: 'ready', runnerVersion: '', platform: '', architecture: '', lastSeenAt: '' },
        { id: 'gaming-ai', label: 'Gaming AI Desktop', transport: 'ssh', sshAlias: 'gaming-ai', runnerStatus: 'ready', runnerVersion: '4.5.7', platform: 'windows', architecture: 'x86_64', lastSeenAt: '' },
      ],
      folders: [{ ...useAppStore.getState().folders[0], executionHostId: 'gaming-ai' }],
      sessions: Array.from({ length: 2 }, (_, index) => ({ ...makeSession(index + 1), executionHostId: 'gaming-ai' })),
      resourceCollections: [{ id: 'work', name: 'Work', collapsed: false, references: [{ id: 'project-ref', type: 'workspace-folder', target: 'project', label: 'Project' }] }],
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    expect(host.querySelector('[data-testid="workspace-machine-project"]')?.getAttribute('aria-label')).toBe('Runs on Gaming AI Desktop')
    expect(host.querySelectorAll('[data-testid^="workspace-machine-"]')).toHaveLength(1)

    act(() => useAppStore.setState((state) => ({ sessions: state.sessions.map((session, index) => index === 0 ? { ...session, executionHostId: 'local' } : session) })))
    expect(host.querySelector('[data-testid="workspace-machine-project"]')?.getAttribute('aria-label')).toBe('Runs on Gaming AI Desktop')

    act(() => root.unmount())
    host.remove()
  })

  it('keeps same-directory workspaces on different computers separate', () => {
    useAppStore.setState({
      executionHosts: [
        { id: 'local', label: 'This computer', transport: 'local', sshAlias: '', runnerStatus: 'ready', runnerVersion: '', platform: '', architecture: '', lastSeenAt: '' },
        { id: 'homelab', label: 'Homelab', transport: 'ssh', sshAlias: 'homelab', runnerStatus: 'ready', runnerVersion: '4.5.7', platform: 'linux', architecture: 'x86_64', lastSeenAt: '' },
        { id: 'nas', label: 'NAS', transport: 'ssh', sshAlias: 'nas', runnerStatus: 'ready', runnerVersion: '4.5.7', platform: 'linux', architecture: 'x86_64', lastSeenAt: '' },
      ],
      folders: [
        { ...useAppStore.getState().folders[0], id: 'homelab-workspace', name: 'Homelab project', directory: '/srv/project', executionHostId: 'homelab' },
        { ...useAppStore.getState().folders[0], id: 'nas-workspace', name: 'NAS project', directory: '/srv/project', executionHostId: 'nas' },
      ],
      sessions: [
        { ...makeSession(1), folderId: 'homelab-workspace', workspaceDirectory: '/srv/project', executionHostId: 'homelab' },
        { ...makeSession(2), folderId: 'nas-workspace', workspaceDirectory: '/srv/project', executionHostId: 'nas' },
      ],
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    expect(host.querySelector('[data-testid="workspace-machine-homelab-workspace"]')?.getAttribute('aria-label')).toBe('Runs on Homelab')
    expect(host.querySelector('[data-testid="workspace-machine-nas-workspace"]')?.getAttribute('aria-label')).toBe('Runs on NAS')
    expect(host.querySelectorAll('[data-testid^="workspace-machine-"]')).toHaveLength(2)

    act(() => root.unmount())
    host.remove()
  })

  it('does not reclassify a workspace chat drag as a folder drag', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    const data = new Map<string, string>()
    const transfer = {
      effectAllowed: '',
      get types() { return [...data.keys()] },
      getData: (type: string) => data.get(type) ?? '',
      setData: (type: string, value: string) => { data.set(type, value) },
    }
    const dragStart = new Event('dragstart', { bubbles: true, cancelable: true })
    Object.defineProperty(dragStart, 'dataTransfer', { value: transfer })

    act(() => host.querySelector('[data-testid="session-row-chat-1"]')?.dispatchEvent(dragStart))

    expect(transfer.effectAllowed).toBe('copy')
    expect(data.get('text/x-uam-chat-id')).toBe('chat-1')
    expect(data.has('text/x-uam-folder-id')).toBe(false)
    expect(data.has('text/x-uam-folder-resource-id')).toBe(false)

    act(() => root.unmount())
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

  it('closes the collection submenu after the pointer leaves both menu surfaces', () => {
    vi.useFakeTimers()
    useAppStore.setState({
      resourceCollections: [{ id: 'work', name: 'Work', collapsed: false, references: [] }],
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    const folderHeader = host.querySelector('[data-testid="folder-header-project"]')
    act(() => folderHeader?.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true })))
    const move = Array.from(document.body.querySelectorAll<HTMLButtonElement>('button')).find((button) =>
      button.textContent?.includes('Move to collection')
    ) as HTMLButtonElement
    act(() => move.dispatchEvent(new MouseEvent('mouseover', { bubbles: true })))
    const submenu = document.body.querySelector('[role="menu"][aria-label="Move to collection"]') as HTMLElement
    expect(submenu).toBeTruthy()

    act(() => {
      move.dispatchEvent(new MouseEvent('mouseout', { bubbles: true, relatedTarget: submenu }))
      submenu.dispatchEvent(new MouseEvent('mouseover', { bubbles: true, relatedTarget: move }))
      submenu.dispatchEvent(new MouseEvent('mouseout', { bubbles: true, relatedTarget: document.body }))
      vi.advanceTimersByTime(200)
    })

    expect(document.body.querySelector('[role="menu"][aria-label="Move to collection"]')).toBeNull()
    expect(document.body.querySelector('[data-viewport-menu]')).toBeTruthy()

    act(() => root.unmount())
    host.remove()
    vi.useRealTimers()
  })

  it('opens and closes the collection submenu with nested-menu keyboard controls', () => {
    useAppStore.setState({
      resourceCollections: [{ id: 'work', name: 'Work', collapsed: false, references: [] }],
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    const folderHeader = host.querySelector('[data-testid="folder-header-project"]')
    act(() => folderHeader?.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true })))
    const move = Array.from(document.body.querySelectorAll<HTMLButtonElement>('button')).find((button) =>
      button.textContent?.includes('Move to collection')
    ) as HTMLButtonElement
    for (const [openKey, closeKey] of [['ArrowRight', 'ArrowLeft'], ['Enter', 'Escape'], [' ', 'ArrowLeft']]) {
      act(() => {
        move.focus()
        move.dispatchEvent(new KeyboardEvent('keydown', { key: openKey, bubbles: true }))
      })

      const submenu = document.body.querySelector('[role="menu"][aria-label="Move to collection"]') as HTMLElement
      expect(submenu).toBeTruthy()
      expect(submenu.contains(document.activeElement)).toBe(true)

      act(() => submenu.dispatchEvent(new KeyboardEvent('keydown', { key: closeKey, bubbles: true })))
      expect(document.body.querySelector('[role="menu"][aria-label="Move to collection"]')).toBeNull()
      expect(document.activeElement).toBe(move)
      expect(document.body.querySelector('[data-viewport-menu]')).toBeTruthy()
    }

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

    act(() => writeChatGridLayout(gridLayout('chat-1', 'chat-3', '', '')))

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
    act(() => writeChatGridLayout(gridLayout('chat-1', 'chat-3', '', '')))

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

  it('shows runtime-active chats with state counts above pinned chats without requiring a status filter', () => {
    useAppStore.setState((state) => ({
      sessions: state.sessions.map((session) => session.id === 'chat-2' ? { ...session, isPinned: true } : session),
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<FolderTree searchQuery="" />)
    })
    expect(host.textContent).not.toContain('Active chats')

    act(() => {
      useAppStore.getState().setCliBinding('chat-1', { processing: true })
      useAppStore.getState().setCliBinding('chat-3', { readySinceLastSelect: true })
      useAppStore.setState((state) => ({
        acpBindingBySessionId: {
          ...state.acpBindingBySessionId,
          'chat-2': { running: true, processing: false, lifecycleState: 'waitingPermission', attentionKind: 'permission' },
        },
      }))
    })

    expect(host.textContent).toContain('Active chats')
    expect(host.querySelector('[data-testid="active-chats"] [data-session-id="chat-1"]')).toBeTruthy()
    expect(host.textContent!.indexOf('Active chats')).toBeLessThan(host.textContent!.indexOf('Pinned chats'))
    expect(host.querySelector('[aria-label="Active chat status counts"]')?.textContent).toContain('1 running')
    expect(host.querySelector('[aria-label="Active chat status counts"]')?.textContent).toContain('1 attention')
    expect(host.querySelector('[aria-label="Active chat status counts"]')?.textContent).toContain('1 done')

    act(() => useAppStore.getState().setCliBinding('chat-1', {
      processing: false,
      readySinceLastSelect: true,
    }))

    expect(host.textContent).toContain('Active chats')
    expect(host.querySelector('[data-testid="active-chats"] [data-session-id="chat-1"]')).toBeTruthy()
    expect(host.querySelector('[data-testid="folder-row-project"] [data-session-id="chat-1"]')).toBeTruthy()

    act(() => root.unmount())
    host.remove()
  })

  it('shift-selects from the clicked duplicate row occurrence', () => {
    act(() => {
      useAppStore.getState().setCliBinding('chat-1', { processing: true })
      useAppStore.getState().setCliBinding('chat-3', { processing: true })
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    const folder = host.querySelector('[data-testid="folder-row-project"]') as HTMLElement
    const first = folder.querySelector('[data-session-id="chat-1"]') as HTMLElement
    const second = folder.querySelector('[data-session-id="chat-2"]') as HTMLElement
    act(() => first.dispatchEvent(new MouseEvent('click', { bubbles: true })))
    act(() => second.dispatchEvent(new MouseEvent('click', { bubbles: true, shiftKey: true })))

    expect(host.textContent).toContain('2 selected')
    expect(host.querySelector('[data-testid="session-row-chat-3"][data-selected="true"]')).toBeNull()

    act(() => root.unmount())
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

  it('treats folder actions as a keyboard menu and restores trigger focus', () => {
    const previousResizeObserver = globalThis.ResizeObserver
    Object.defineProperty(globalThis, 'ResizeObserver', {
      configurable: true,
      value: class {
        observe() {}
        unobserve() {}
        disconnect() {}
      },
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    const trigger = host.querySelector('button[aria-label="Folder actions"]') as HTMLButtonElement
    trigger.focus()
    act(() => trigger.click())
    const menu = document.body.querySelector('[role="menu"][aria-label="Project actions"]')
    expect(trigger.getAttribute('aria-expanded')).toBe('true')
    expect(document.activeElement?.textContent).toContain('New chat')
    act(() => document.activeElement?.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowDown', bubbles: true })))
    expect(document.activeElement?.textContent).toContain('Project memory')
    act(() => document.activeElement?.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })))
    expect(document.body.contains(menu)).toBe(false)
    expect(document.activeElement).toBe(trigger)

    act(() => root.unmount())
    host.remove()
    Object.defineProperty(globalThis, 'ResizeObserver', { configurable: true, value: previousResizeObserver })
  })

  it('keeps folder rename and delete failures visible', async () => {
    const renameFolder = vi.fn(async () => false)
    const deleteFolder = vi.fn(async () => false)
    useAppStore.setState({ renameFolder, deleteFolder })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    act(() => (host.querySelector('button[aria-label="Folder actions"]') as HTMLButtonElement).click())
    const rename = Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="menuitem"]')).find((button) => button.textContent?.includes('Rename folder'))!
    act(() => rename.click())
    const directory = host.querySelector('input[placeholder="Workspace directory"]') as HTMLInputElement
    act(() => {
      directory.value = '/tmp/other'
      directory.dispatchEvent(new Event('input', { bubbles: true }))
    })
    await act(async () => Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Save')!.click())
    expect(host.textContent).toContain('workspace could not be updated')
    expect(host.querySelector('input[placeholder="Workspace directory"]')).toBeTruthy()

    act(() => (host.querySelector('button[aria-label="Folder actions"]') as HTMLButtonElement).click())
    const remove = Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="menuitem"]')).find((button) => button.textContent?.includes('Delete folder'))!
    act(() => remove.click())
    await act(async () => Array.from(document.body.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Delete Folder')!.click())
    expect(document.body.querySelector('[aria-label="Delete folder and chats"]')?.textContent).toContain('workspace could not be deleted')

    act(() => root.unmount())
    host.remove()
  })

	it('creates a workspace with the selected execution computer', async () => {
	  const addFolder = vi.fn(async () => true)
	  useAppStore.setState({
		addFolder,
		executionHosts: [
		  { id: 'local', label: 'This computer', transport: 'local', sshAlias: '', runnerStatus: 'ready', runnerVersion: '', platform: 'macos', architecture: 'arm64', lastSeenAt: '' },
		  { id: 'lab', label: 'Homelab', transport: 'ssh', sshAlias: 'uam-homelab', runnerStatus: 'ready', runnerVersion: '4.8.0-alpha', platform: 'linux', architecture: 'x86_64', lastSeenAt: '' },
		],
	  })
	  const host = document.createElement('div')
	  document.body.appendChild(host)
	  const root = createRoot(host)
	  act(() => root.render(<FolderTree searchQuery="" />))

	  act(() => Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'New workspace')!.click())
	  act(() => host.querySelector<HTMLButtonElement>('button[aria-label="Workspace computer"]')!.click())
	  act(() => Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="option"]')).find((button) => button.textContent?.includes('Homelab'))!.click())
	  const name = host.querySelector<HTMLInputElement>('input[placeholder="Folder name"]')!
	  const directory = host.querySelector<HTMLInputElement>('input[placeholder="Absolute directory on selected computer"]')!
	  const setInputValue = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')?.set
	  act(() => {
		setInputValue?.call(name, 'Containers')
		name.dispatchEvent(new Event('input', { bubbles: true }))
	  })
	  act(() => {
		setInputValue?.call(directory, '/opt/containers')
		directory.dispatchEvent(new Event('input', { bubbles: true }))
	  })
	  const create = Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Create')!
	  expect(create.disabled).toBe(false)
	  await act(async () => create.click())

	  expect(addFolder).toHaveBeenCalledWith('Containers', null, '/opt/containers', 'lab')
	  act(() => root.unmount())
	  host.remove()
	})

	it('shows an existing workspace computer with the same read-only field styling', () => {
	  useAppStore.setState({
		executionHosts: [
		  { id: 'local', label: 'This computer', transport: 'local', sshAlias: '', runnerStatus: 'ready', runnerVersion: '', platform: 'macos', architecture: 'arm64', lastSeenAt: '' },
		  { id: 'lab', label: 'Homelab', transport: 'ssh', sshAlias: 'uam-homelab', runnerStatus: 'ready', runnerVersion: '4.8.0-alpha', platform: 'linux', architecture: 'x86_64', lastSeenAt: '' },
		],
		folders: [{ id: 'project', name: 'Containers', parentId: null, directory: '/opt/containers', executionHostId: 'lab', isExpanded: true, createdAt: new Date() }],
	  })
	  const host = document.createElement('div')
	  document.body.appendChild(host)
	  const root = createRoot(host)
	  act(() => root.render(<FolderTree searchQuery="" />))

	  act(() => (host.querySelector('button[aria-label="Folder actions"]') as HTMLButtonElement).click())
	  act(() => Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="menuitem"]')).find((button) => button.textContent?.includes('Rename folder'))!.click())
	  const computer = host.querySelector<HTMLInputElement>('input[aria-label="Workspace computer"]')!
	  const name = host.querySelector<HTMLInputElement>('input[aria-label="Workspace name"]')!
	  const directory = host.querySelector<HTMLInputElement>('input[aria-label="Workspace directory"]')!
	  expect(computer.value).toBe('Homelab')
	  expect(computer.readOnly).toBe(true)
	  expect(computer.className).toBe(name.className)
	  expect(computer.style.background).toBe(name.style.background)
	  expect(computer.style.border).toBe(directory.style.border)

	  act(() => root.unmount())
	  host.remove()
	})

	it('browses remote directories only after an explicit Browse action', async () => {
	  const listRemoteDirectories = vi.fn(async (_hostId: string, directory: string) => ({
		ok: true as const,
		listing: {
		  directory,
		  parentDirectory: directory === '/' ? '' : '/',
		  directories: directory === '/' ? [{ name: 'srv', path: '/srv' }] : [],
		  truncated: false,
		},
	  }))
	  useAppStore.setState({
		listRemoteDirectories,
		executionHosts: [
		  { id: 'local', label: 'This computer', transport: 'local', sshAlias: '', runnerStatus: 'ready', runnerVersion: '', platform: 'macos', architecture: 'arm64', lastSeenAt: '' },
		  { id: 'lab', label: 'Homelab', transport: 'ssh', sshAlias: 'uam-homelab', runnerStatus: 'ready', runnerVersion: '4.8.0-alpha', platform: 'linux', architecture: 'x86_64', lastSeenAt: '' },
		],
	  })
	  const host = document.createElement('div')
	  document.body.appendChild(host)
	  const root = createRoot(host)
	  act(() => root.render(<FolderTree searchQuery="" />))

	  act(() => Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'New workspace')!.click())
	  act(() => host.querySelector<HTMLButtonElement>('button[aria-label="Workspace computer"]')!.click())
	  act(() => Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="option"]')).find((button) => button.textContent?.includes('Homelab'))!.click())
	  expect(listRemoteDirectories).not.toHaveBeenCalled()
	  await act(async () => {
		Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Browse')!.click()
		await Promise.resolve()
		await Promise.resolve()
	  })
	  expect(listRemoteDirectories).toHaveBeenCalledWith('lab', '/')
	  await act(async () => {
		Array.from(document.body.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'srv')!.click()
		await Promise.resolve()
		await Promise.resolve()
	  })
	  expect(listRemoteDirectories).toHaveBeenLastCalledWith('lab', '/srv')
	  act(() => Array.from(document.body.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Use this directory')!.click())
	  expect(host.querySelector<HTMLInputElement>('input[placeholder="Absolute directory on selected computer"]')!.value).toBe('/srv')

	  act(() => root.unmount())
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

  it('keeps a failed rescan visible instead of looking empty', async () => {
    useAppStore.setState({ rescanFolderChats: vi.fn(() => Promise.resolve(false)) })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))
    act(() => host.querySelector('[data-testid="folder-header-project"]')
      ?.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true })))
    const rescan = Array.from(document.body.querySelectorAll('button')).find((button) => button.textContent?.includes('Rescan chats'))
    await act(async () => { rescan?.click(); await Promise.resolve() })

    expect(host.querySelector('[role="alert"]')?.textContent).toContain('Could not rescan Project')

    act(() => root.unmount())
    host.remove()
  })

  it('collapses Unsorted visually but reveals matching chats while searching', () => {
    useAppStore.setState((state) => ({
      sessions: state.sessions.slice(0, 2).map((session) => ({ ...session, folderId: null })),
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    const toggle = host.querySelector<HTMLButtonElement>('button[aria-label="Collapse Unsorted"]')
    const children = host.querySelector<HTMLElement>('#unsorted-chat-list')
    expect(toggle?.getAttribute('aria-expanded')).toBe('true')
    expect(toggle?.textContent).toContain('Unsorted')
    expect(toggle?.textContent).toContain('2')
    expect(children?.getAttribute('aria-hidden')).toBe('false')
    expect(host.textContent).toContain('Chat 1')
    expect(host.textContent).toContain('Chat 2')

    act(() => toggle?.click())
    expect(host.querySelector('button[aria-label="Expand Unsorted"]')?.getAttribute('aria-expanded')).toBe('false')
    expect(children?.getAttribute('aria-hidden')).toBe('true')
    expect(children?.hasAttribute('inert')).toBe(true)

    act(() => root.render(<FolderTree searchQuery="Chat 1" />))
    expect(host.querySelector('button[aria-label="Collapse Unsorted"]')?.getAttribute('aria-expanded')).toBe('true')
    expect(children?.getAttribute('aria-hidden')).toBe('false')
    expect(children?.hasAttribute('inert')).toBe(false)
    expect(host.textContent).toContain('Chat 1')

    act(() => root.render(<FolderTree searchQuery="" />))
    expect(host.querySelector('button[aria-label="Expand Unsorted"]')?.getAttribute('aria-expanded')).toBe('false')
    expect(children?.getAttribute('aria-hidden')).toBe('true')

    act(() => root.unmount())
    host.remove()
  })

  it('previews and applies workspace recovery from the Unsorted context menu', async () => {
    vi.stubGlobal('ResizeObserver', class { observe() {}; unobserve() {}; disconnect() {} })
    const previewUnsortedWorkspaceFolders = vi.fn(async () => ({
      groups: [
        { title: 'Alpha', directory: '/tmp/Alpha', existingFolderId: '', chatIds: ['chat-1', 'chat-2'] },
        { title: 'Project', directory: '/tmp/project', existingFolderId: 'project', chatIds: ['chat-3'] },
      ],
      missing: [{ id: 'chat-4', title: 'Chat 4', directory: '/missing/workspace', reason: 'Folder not found' }],
      unavailable: [],
      noLocation: [{ id: 'chat-5', title: 'Chat 5', directory: '', reason: 'No workspace location recorded' }],
    }))
    const rebuildUnsortedWorkspaceFolders = vi.fn(async () => true)
    useAppStore.setState((state) => ({
      sessions: state.sessions.slice(0, 5).map((session) => ({ ...session, folderId: null })),
      previewUnsortedWorkspaceFolders,
      rebuildUnsortedWorkspaceFolders,
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    act(() => host.querySelector('button[aria-label="Collapse Unsorted"]')?.parentElement
      ?.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, clientX: 40, clientY: 50 })))
    const menu = document.body.querySelector('[role="menu"][aria-label="Unsorted actions"]')
    expect(menu?.textContent).toContain('Rebuild workspace folders')
    const rebuild = menu?.querySelector('button') as HTMLButtonElement
    await act(async () => { rebuild.click(); await Promise.resolve(); await Promise.resolve() })

    expect(previewUnsortedWorkspaceFolders).toHaveBeenCalledTimes(1)
    const dialog = document.body.querySelector('[role="dialog"][aria-label="Rebuild workspace folders"]') as HTMLElement
    expect(dialog).toBeTruthy()
    expect(dialog.contains(document.activeElement)).toBe(true)
    expect(dialog.textContent).toContain('Ready · 3 chats')
    expect(dialog.textContent).toContain('Alpha')
    expect(dialog.textContent).toContain('Create · 2')
    expect(dialog.textContent).toContain('Use existing · 1')
    expect(dialog.textContent).toContain('Location not found · 1')
    expect(dialog.textContent).toContain('No saved location · 1')
    expect(rebuildUnsortedWorkspaceFolders).not.toHaveBeenCalled()

    const apply = Array.from(dialog.querySelectorAll('button')).find((button) => button.textContent?.includes('Create 1 folder and organise 3 chats'))
    await act(async () => { apply?.click(); await Promise.resolve(); await Promise.resolve() })
    expect(rebuildUnsortedWorkspaceFolders).toHaveBeenCalledTimes(1)
    expect(document.body.querySelector('[aria-label="Rebuild workspace folders"]')).toBeNull()

    act(() => root.unmount())
    host.remove()
    vi.unstubAllGlobals()
  })

  it('keeps unavailable chats by default and reuses bulk confirmation before deletion', async () => {
    const deleteSessions = vi.fn(async () => true)
    useAppStore.setState((state) => ({
      sessions: state.sessions.slice(0, 3).map((session) => ({ ...session, folderId: null })),
      deleteSessions,
      previewUnsortedWorkspaceFolders: vi.fn(async () => ({
        groups: [],
        missing: [{ id: 'chat-1', title: 'Chat 1', directory: '/missing/one', reason: 'Folder not found' }],
        unavailable: [{ id: 'chat-2', title: 'Chat 2', directory: '/blocked/two', reason: 'Permission denied' }],
        noLocation: [{ id: 'chat-3', title: 'Chat 3', directory: '', reason: 'No workspace location recorded' }],
      })),
    }))
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    act(() => host.querySelector<HTMLButtonElement>('button[aria-label="Actions for Unsorted"]')?.click())
    const recoveryAction = document.body.querySelector<HTMLButtonElement>('[role="menu"][aria-label="Unsorted actions"] button')
    await act(async () => { recoveryAction?.click(); await Promise.resolve(); await Promise.resolve() })
    const dialog = document.body.querySelector('[role="dialog"][aria-label="Rebuild workspace folders"]') as HTMLElement
    expect(dialog.textContent).toContain('stay in Unsorted unless you explicitly delete')
    expect(deleteSessions).not.toHaveBeenCalled()

    const deleteUnavailable = Array.from(dialog.querySelectorAll('button')).find((button) => button.textContent?.includes('Delete 2 unavailable'))
    act(() => deleteUnavailable?.click())
    expect(deleteSessions).not.toHaveBeenCalled()
    const confirmation = document.body.querySelector('[role="alertdialog"][aria-label="Delete 2 selected chats"]') as HTMLElement
    expect(confirmation.textContent).toContain('This cannot be undone')
    const confirm = Array.from(confirmation.querySelectorAll('button')).find((button) => button.textContent === 'Delete chats')
    await act(async () => confirm?.click())
    expect(deleteSessions).toHaveBeenCalledWith(['chat-1', 'chat-2'])

    act(() => root.unmount())
    host.remove()
  })

  it('selects a visible Shift-click range and confirms one bulk delete', async () => {
    const deleteSessions = vi.fn(async () => true)
    useAppStore.setState({ deleteSessions })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    act(() => host.querySelector<HTMLElement>('[data-testid="session-row-chat-1"]')?.click())
    act(() => host.querySelector<HTMLElement>('[data-testid="session-row-chat-4"]')
      ?.dispatchEvent(new MouseEvent('click', { bubbles: true, shiftKey: true })))

    expect(Array.from(host.querySelectorAll('[data-testid^="session-row-"][data-selected="true"]'))
      .map((row) => row.getAttribute('data-session-id'))).toEqual(['chat-1', 'chat-2', 'chat-3', 'chat-4'])
    const bulkDelete = host.querySelector<HTMLButtonElement>('button[aria-label="Delete 4 selected chats"]')
    expect(bulkDelete).toBeTruthy()
    act(() => bulkDelete?.click())
    expect(deleteSessions).not.toHaveBeenCalled()

    const dialog = document.body.querySelector('[role="alertdialog"][aria-label="Delete 4 selected chats"]')
    expect(dialog?.textContent).toContain('This cannot be undone')
    const confirm = Array.from(dialog?.querySelectorAll('button') ?? []).find((button) => button.textContent === 'Delete chats')
    await act(async () => confirm?.click())

    expect(deleteSessions).toHaveBeenCalledTimes(1)
    expect(deleteSessions).toHaveBeenCalledWith(['chat-1', 'chat-2', 'chat-3', 'chat-4'])
    expect(host.querySelector('[aria-label="Delete 4 selected chats"]')).toBeNull()

    act(() => root.unmount())
    host.remove()
  })

  it('clears selected chats when their workspace is collapsed', async () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<FolderTree searchQuery="" />))

    act(() => host.querySelector<HTMLElement>('[data-testid="session-row-chat-1"]')?.click())
    act(() => host.querySelector<HTMLElement>('[data-testid="session-row-chat-2"]')
      ?.dispatchEvent(new MouseEvent('click', { bubbles: true, shiftKey: true })))
    expect(host.textContent).toContain('2 selected')

    await act(async () => {
      host.querySelector<HTMLElement>('[data-testid="folder-header-project"]')?.click()
      await Promise.resolve()
    })

    expect(host.textContent).not.toContain('selected')
    expect(host.querySelectorAll('[data-selected="true"]')).toHaveLength(0)

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
