import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { useAppStore } from '../../store/useAppStore'

const { mainPanelRenderCount } = vi.hoisted(() => ({
  mainPanelRenderCount: { value: 0 },
}))

vi.mock('./Sidebar', () => ({
  Sidebar: () => <div data-testid="sidebar">Sidebar</div>,
}))

vi.mock('./MainPanel', () => ({
  MainPanel: () => {
    mainPanelRenderCount.value += 1
    return <div data-testid="main-panel">Main panel<input aria-label="Chat draft" defaultValue=""/></div>
  },
}))

import { AppShell } from './AppShell'
import { moveResourceToCollection } from '../sidebar/CollectionMenuItems'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

const localStorageValues = new Map<string, string>()
Object.defineProperty(window, 'localStorage', {
  configurable: true,
  value: {
    getItem: (key: string) => localStorageValues.get(key) ?? null,
    setItem: (key: string, value: string) => localStorageValues.set(key, value),
    removeItem: (key: string) => localStorageValues.delete(key),
    clear: () => localStorageValues.clear(),
  },
})

describe('AppShell', () => {
  beforeEach(() => {
    vi.restoreAllMocks()
    mainPanelRenderCount.value = 0
    Object.defineProperty(window, 'innerWidth', { configurable: true, value: 1400 })
    window.localStorage.clear()
    useAppStore.setState({
      folders: [],
      sessions: [],
      activeSessionId: null,
      isNewChatModalOpen: false,
      isSettingsOpen: false,
      memoryLibraryScope: null,
      isMemoryScanModalOpen: false,
      isMarkdownStoreOpen: false,
      sidebarCollapsed: false,
      commitPanelOpen: false,
      sidebarWidthPx: 320,
      commitPanelWidthPx: 420,
      shellActionNotification: '',
      statusLine: '',
      appVersion: 'V4.1.0',
      updateChecksEnabled: true,
      updateLastCheckedAt: new Date().toISOString(),
      dismissedUpdateVersions: {},
      cliVersionManager: { providers: [] },
      executionHosts: [{ id: 'local', label: 'This computer', transport: 'local', sshAlias: '', runnerStatus: 'ready', runnerVersion: '', platform: 'macos', architecture: 'arm64', lastSeenAt: '' }],
      acpBindingBySessionId: {},
      cliBindingBySessionId: {},
    })
  })

  it('shows the update count and opens a full sidebar panel', async () => {
    window.localStorage.setItem('uam-update-catalog-v1', JSON.stringify({
      checkedAt: '2026-07-13T20:00:00.000Z',
      uam: { version: 'V4.2.0', url: 'https://example.test/uam' },
      providers: {
        'codex-cli': { version: '0.130.0', url: 'https://example.test/codex' },
      },
    }))
    useAppStore.setState({
      providers: [{ id: 'codex-cli', name: 'Codex CLI', shortName: 'Codex', description: '', color: '#fff' }],
      cliVersionManager: {
        providers: [{
          providerId: 'codex-cli',
          installedVersion: '0.124.0',
          selectedVersion: '0.124.0',
          availableVersions: [{ version: '0.124.0', preferred: true }],
          preferredVersion: '0.124.0',
          status: 'verified',
          message: '',
          running: false,
          lastCommand: '',
          lastOutput: '',
        }],
      },
    })
    const host = document.createElement('div')
    const root = createRoot(host)
    await act(async () => root.render(<AppShell />))

    const button = host.querySelector('button[aria-label="2 updates available"]') as HTMLButtonElement
    expect(button).toBeTruthy()
    expect(button.parentElement?.querySelector('span[aria-hidden]')?.textContent).toBe('1')
    act(() => button.click())
    expect(host.querySelector('[data-testid="updates-panel"]')).toBeTruthy()
    expect(host.textContent).toContain('Universal Agent Manager')
    expect(host.textContent).toContain('Codex')

    act(() => root.unmount())
  })

  it('renders edge-to-edge with side rails and without a total header bar', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<AppShell />)
    })

    expect(host.querySelector('.uam-app')).toBeTruthy()
    expect(host.querySelector('.uam-titlebar')).toBeNull()
    expect(host.querySelector('[aria-label="Main navigation"]')).toBeTruthy()
    expect(host.querySelector('[aria-label="Tool windows"]')).toBeTruthy()
    expect(host.querySelector('[data-testid="chat-selector-panel"]')?.classList.contains('uam-shell-panel--left')).toBe(true)
    expect(host.querySelector('.uam-window')).toBeNull()
    expect(host.querySelector('.uam-window-controls')).toBeNull()
    expect(host.querySelector('.uam-window-dot')).toBeNull()

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('keeps the rail actions wired', () => {
    const openAllMemoryLibrary = vi.fn().mockResolvedValue(true)
    const setSettingsOpen = vi.fn()
    useAppStore.setState({
      openAllMemoryLibrary,
      setSettingsOpen,
    })

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<AppShell />)
    })

    const memoryButton = host.querySelector('button[aria-label="Memory library"]') as HTMLButtonElement
    const settingsButton = host.querySelector('button[aria-label="Settings"]') as HTMLButtonElement

    act(() => {
      memoryButton.dispatchEvent(new MouseEvent('click', { bubbles: true }))
      settingsButton.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(openAllMemoryLibrary).toHaveBeenCalledTimes(1)
    expect(setSettingsOpen).toHaveBeenCalledWith(true)
    expect(Array.from(host.querySelectorAll('button')).some((button) => button.textContent === '+New Chat')).toBe(false)

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('replaces the middle with grouped Settings while retaining chat state and working rails', async () => {
    useAppStore.setState({
      setSettingsOpen: vi.fn(open => useAppStore.setState({isSettingsOpen: open})),
      refreshCustomThemes: vi.fn(async () => true),
      mcpServers: [],
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    await act(async () => root.render(<AppShell/>))
    const draft = host.querySelector<HTMLInputElement>('[aria-label="Chat draft"]')!
    draft.value = 'Keep this draft'
    const sidebar = host.querySelector('[data-testid="sidebar"]')
    const settings = host.querySelector<HTMLButtonElement>('[aria-label="Settings"]')!
    act(() => settings.click())
    expect(host.querySelector<HTMLElement>('[data-testid="chat-region"]')!.hidden).toBe(true)
    expect(host.querySelector('[aria-label="Settings workspace"]')).toBeTruthy()
    for (const label of ['Main navigation', 'Tool windows']) {
      expect(host.querySelector(`[aria-label="${label}"]`)!.closest('[hidden]')).toBeNull()
    }
    expect(host.querySelector('[aria-label="Settings pages"]')).toBeTruthy()
    const collapse = host.querySelector<HTMLButtonElement>('[aria-label="Collapse chat selector"]')!
    expect(collapse.disabled).toBe(true)
    act(() => collapse.click())
    expect(host.querySelector('[aria-label="Settings pages"]')).toBeTruthy()
    expect(useAppStore.getState().sidebarCollapsed).toBe(false)
    act(() => host.querySelector<HTMLButtonElement>('[aria-label="No new alerts"]')!.click())
    expect(host.querySelector('[aria-label="Settings workspace"]')).toBeTruthy()
    expect(host.querySelector('.uam-app')!.getAttribute('data-right-panel-open')).toBe('true')
    act(() => host.querySelector<HTMLButtonElement>('[aria-label="Back to chats"]')!.click())
    expect(useAppStore.getState().isSettingsOpen).toBe(false)
    expect(collapse.disabled).toBe(false)
    expect(host.querySelector<HTMLElement>('[data-testid="chat-region"]')!.hidden).toBe(false)
    expect(host.querySelector('[aria-label="Chat draft"]')).toBe(draft)
    expect(draft.value).toBe('Keep this draft')
    expect(host.querySelector('[data-testid="sidebar"]')).toBe(sidebar)
    act(() => root.unmount())
    host.remove()
  })

  it('guards the Settings rail toggle and Back with the same unsaved draft confirmation', async () => {
    const setSettingsOpen = vi.fn((open: boolean) => useAppStore.setState({isSettingsOpen:open}))
    useAppStore.setState({setSettingsOpen, refreshCustomThemes:vi.fn(async () => true), mcpServers:[]})
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    await act(async () => root.render(<AppShell/>))
    const settings = host.querySelector<HTMLButtonElement>('[aria-label="Settings"]')!
    act(() => settings.click())
    act(() => host.querySelector<HTMLButtonElement>('[aria-label="MCP Servers"]')!.click())
    const editor = host.querySelector<HTMLTextAreaElement>('[aria-label="MCP server configuration"]')!
    act(() => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype,'value')!.set!.call(editor,'[{"name":"draft"}]')
      editor.dispatchEvent(new Event('input',{bubbles:true}))
    })
    setSettingsOpen.mockClear()
    act(() => settings.click())
    expect(setSettingsOpen).not.toHaveBeenCalled()
    const confirmation = host.querySelector('[aria-label="Discard unsaved MCP changes"]')!
    expect(confirmation).toBeTruthy()
    const buttons = confirmation.querySelectorAll<HTMLButtonElement>('button')
    buttons[buttons.length - 1].focus()
    const tab = new KeyboardEvent('keydown', {key:'Tab',bubbles:true,cancelable:true})
    act(() => buttons[buttons.length - 1].dispatchEvent(tab))
    expect(tab.defaultPrevented).toBe(true)
    expect(document.activeElement).toBe(buttons[0])
    act(() => Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find(button => button.textContent === 'Keep editing')!.click())
    act(() => host.querySelector<HTMLButtonElement>('[aria-label="Back to chats"]')!.click())
    expect(setSettingsOpen).not.toHaveBeenCalled()
    expect(editor.value).toBe('[{"name":"draft"}]')
    act(() => Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find(button => button.textContent === 'Discard changes')!.click())
    expect(setSettingsOpen).toHaveBeenCalledExactlyOnceWith(false)
    expect(host.querySelector('[aria-label="Settings workspace"]')).toBeNull()
    act(() => root.unmount())
    host.remove()
  })

  it('offers notification-specific actions for missing workspace folders', async () => {
    const browseFolderDirectory = vi.fn().mockResolvedValue('/tmp/relinked project')
    const renameFolder = vi.fn(async () => true)
    const deleteFolder = vi.fn(async () => true)
    useAppStore.setState({
      folders: [{
        id: 'missing',
        name: 'Deleted project',
        parentId: null,
        directory: '/tmp/deleted project',
        isExpanded: true,
        missing: true,
        createdAt: new Date(),
      }],
      sessions: [
        { id: 'one', name: 'One', viewMode: 'chat', folderId: 'missing', createdAt: new Date(), updatedAt: new Date() },
        { id: 'two', name: 'Two', viewMode: 'chat', folderId: 'missing', createdAt: new Date(), updatedAt: new Date() },
      ],
      browseFolderDirectory,
      renameFolder,
      deleteFolder,
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<AppShell />))

    const alerts = host.querySelector('button[aria-label="1 alert"]') as HTMLButtonElement
    expect(alerts).toBeTruthy()
    act(() => alerts.click())
    const panel = host.querySelector('[data-testid="notifications-panel"]')
    expect(panel).toBeTruthy()
    expect(panel?.classList.contains('uam-shell-panel--right')).toBe(true)
    expect(panel?.textContent).toContain('Workspace folder missing: Deleted project')
    expect(panel?.textContent).toContain('/tmp/deleted project')
    const relink = Array.from(panel?.querySelectorAll('button') ?? []).find((button) => button.textContent === 'Relink')
    const remove = Array.from(panel?.querySelectorAll('button') ?? []).find((button) => button.textContent === 'Remove')
    expect(relink).toBeTruthy()
    expect(remove).toBeTruthy()

    await act(async () => relink?.click())
    expect(browseFolderDirectory).toHaveBeenCalledWith('/tmp/deleted project')
    expect(renameFolder).toHaveBeenCalledWith('missing', 'Deleted project', '/tmp/relinked project')

    renameFolder.mockResolvedValueOnce(false)
    await act(async () => relink?.click())
    expect(panel?.textContent).toContain('workspace could not be relinked')

    await act(async () => remove?.click())
    expect(panel?.textContent).toContain('delete 2 chats')
    expect(deleteFolder).not.toHaveBeenCalled()
    const confirmRemoval = Array.from(panel?.querySelectorAll('button') ?? []).find((button) => button.textContent === 'Confirm removal')
    await act(async () => confirmRemoval?.click())
    expect(deleteFolder).toHaveBeenCalledWith('missing')

    act(() => root.unmount())
    host.remove()
  })

  it('shows shell action execution feedback in notifications', () => {
    useAppStore.setState({ shellActionNotification: 'Started shell action: Review Selection' })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<AppShell />))

    const alerts = host.querySelector('button[aria-label="1 alert"]') as HTMLButtonElement
    act(() => alerts.click())
    const panel = host.querySelector('[aria-label="Notifications"]')
    expect(panel?.textContent).toContain('Started shell action: Review Selection')
    expect(panel?.querySelector('.uam-btn')).toBeNull()

    act(() => root.unmount())
    host.remove()
  })

  it('shows backend failures in notifications', () => {
    useAppStore.setState({ statusLine: 'Failed to persist settings.' })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<AppShell />))

    act(() => (host.querySelector('button[aria-label="1 alert"]') as HTMLButtonElement).click())
    const panel = host.querySelector('[aria-label="Notifications"]')
    expect(panel?.textContent).toContain('Application status')
    expect(panel?.textContent).toContain('Failed to persist settings.')

    act(() => root.unmount())
    host.remove()
  })

  it('notifies when a remote chat disconnects and when it reconnects', async () => {
    useAppStore.setState({
      executionHosts: [
        { id: 'local', label: 'This computer', transport: 'local', sshAlias: '', runnerStatus: 'ready', runnerVersion: '', platform: 'macos', architecture: 'arm64', lastSeenAt: '' },
        { id: 'lab', label: 'Homelab', transport: 'ssh', sshAlias: 'homelab', runnerStatus: 'ready', runnerVersion: '4.8.0-alpha-2', platform: 'linux', architecture: 'x86_64', lastSeenAt: '' },
      ],
      sessions: [{ id: 'remote-chat', name: 'Containers', viewMode: 'chat', folderId: null, executionHostId: 'lab', createdAt: new Date(), updatedAt: new Date() }],
      acpBindingBySessionId: {
        'remote-chat': { lastError: 'Provider model unavailable.' } as ReturnType<typeof useAppStore.getState>['acpBindingBySessionId'][string],
      },
      cliBindingBySessionId: {
        'remote-chat': { terminalId: 'remote-terminal', boundChatId: 'remote-chat', running: false, lifecycleState: 'stopped', turnState: 'idle', processing: false, readySinceLastSelect: false, active: true, lastError: 'Codex app-server process exited during an active turn.' },
      },
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    await act(async () => root.render(<AppShell />))

    expect(host.querySelector('button[aria-label="1 alert"]')).toBeTruthy()
    act(() => (host.querySelector('button[aria-label="1 alert"]') as HTMLButtonElement).click())
    expect(host.textContent).toContain('Homelab connection issue')
    expect(host.textContent).toContain('Codex app-server process exited during an active turn.')

    await act(async () => useAppStore.setState((state) => ({
      cliBindingBySessionId: {
        ...state.cliBindingBySessionId,
        'remote-chat': { ...state.cliBindingBySessionId['remote-chat'], running: true, lifecycleState: 'idle', lastError: '' },
      },
    })))
    expect(host.textContent).toContain('Homelab reconnected')
    expect(host.textContent).toContain('The remote connection is available again.')

    act(() => root.unmount())
    host.remove()
  })

  it('does not rerender the shell for unrelated background runtime updates', async () => {
    useAppStore.setState({
      sessions: [{ id: 'background', name: 'Background', viewMode: 'chat', folderId: null, executionHostId: 'local', createdAt: new Date(), updatedAt: new Date() }],
      acpBindingBySessionId: {
        background: {
          sessionId: 'native-background',
          providerId: 'codex-cli',
          protocolKind: 'codex-acp',
          threadId: '',
          running: true,
          lifecycleState: 'processing',
          processing: true,
          readySinceLastSelect: false,
          processingStartedAtMs: Date.now(),
          lastError: '',
          recentStderr: '',
          lastExitCode: null,
          diagnostics: [],
          toolCalls: [],
          planEntries: [],
          availableModes: [],
          currentModeId: 'default',
          availableModels: [],
          currentModelId: '',
          turnEvents: [],
          turnUserMessageIndex: 0,
          turnAssistantMessageIndex: 1,
          turnSerial: 1,
          pendingPermission: null,
          pendingUserInput: null,
          agentInfo: null,
        },
      },
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    await act(async () => {
      root.render(<AppShell />)
      await Promise.resolve()
    })
    const rendersBeforeRuntimeUpdate = mainPanelRenderCount.value

    act(() => useAppStore.setState((state) => ({
      acpBindingBySessionId: {
        ...state.acpBindingBySessionId,
        background: {
          ...state.acpBindingBySessionId.background,
          turnEvents: [{ type: 'assistant_text', text: 'Streaming in the background.' }],
        },
      },
    })))

    expect(mainPanelRenderCount.value).toBe(rendersBeforeRuntimeUpdate)

    act(() => root.unmount())
    host.remove()
  })

  it('dismisses each notification and keeps it dismissed when the panel reopens', () => {
    useAppStore.setState({
      shellActionNotification: 'Shell actions applied successfully.',
      folders: [{
        id: 'missing',
        name: 'Deleted project',
        parentId: null,
        directory: '/tmp/deleted project',
        isExpanded: true,
        missing: true,
        createdAt: new Date(),
      }],
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<AppShell />))

    act(() => (host.querySelector('button[aria-label="2 alerts"]') as HTMLButtonElement).click())
    act(() => (host.querySelector('button[aria-label="Dismiss Finder / Explorer action"]') as HTMLButtonElement).click())
    expect(host.textContent).not.toContain('Shell actions applied successfully.')
    expect(useAppStore.getState().shellActionNotification).toBe('')
    expect(document.activeElement).toBe(host.querySelector('[data-notifications-heading]'))
    expect(host.querySelector('button[aria-label="1 alert"]')).toBeTruthy()

    act(() => (host.querySelector('button[aria-label="Dismiss Workspace folder missing: Deleted project"]') as HTMLButtonElement).click())
    expect(host.textContent).toContain('You’re all caught up')
    expect(host.querySelector('button[aria-label="No new alerts"]')).toBeTruthy()

    act(() => (host.querySelector('button[aria-label="Close notifications"]') as HTMLButtonElement).click())
    act(() => (host.querySelector('button[aria-label="No new alerts"]') as HTMLButtonElement).click())
    expect(host.textContent).toContain('You’re all caught up')
    expect(host.textContent).not.toContain('Shell actions applied successfully.')
    expect(host.textContent).not.toContain('Workspace folder missing: Deleted project')

    act(() => useAppStore.setState({ shellActionNotification: 'Shell actions applied successfully.' }))
    expect(host.querySelector('button[aria-label="1 alert"]')).toBeTruthy()
    act(() => useAppStore.setState({ folders: [{ ...useAppStore.getState().folders[0], missing: false }] }))
    act(() => useAppStore.setState({ folders: [{ ...useAppStore.getState().folders[0], missing: true }] }))
    expect(host.querySelector('button[aria-label="2 alerts"]')).toBeTruthy()

    act(() => root.unmount())
    host.remove()
  })

  it('keeps right-side tool windows mutually exclusive', async () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    await act(async () => root.render(<AppShell />))
    expect((host.querySelector('.uam-app') as HTMLElement).dataset.rightPanelOpen).toBe('false')

    act(() => (host.querySelector('button[aria-label="No new alerts"]') as HTMLButtonElement).click())
    expect(host.querySelector('[data-testid="notifications-panel"]')).toBeTruthy()
    expect(host.textContent).toContain('You’re all caught up')
    expect((host.querySelector('.uam-app') as HTMLElement).dataset.rightPanelOpen).toBe('true')

    act(() => (host.querySelector('button[aria-label="Check for updates"]') as HTMLButtonElement).click())
    expect(host.querySelector('[data-testid="notifications-panel"]')).toBeNull()
    expect(host.querySelector('[data-testid="updates-panel"]')?.classList.contains('uam-shell-panel--right')).toBe(true)

    await act(async () => {
      ;(host.querySelector('button[aria-label="Open Git/SVN commit panel"]') as HTMLButtonElement).click()
      await Promise.resolve()
    })
    expect(host.querySelector('[data-testid="updates-panel"]')).toBeNull()
    expect(host.querySelector('[data-testid="commit-panel"]')?.classList.contains('uam-shell-panel--right')).toBe(true)

    act(() => root.unmount())
    host.remove()
  })

  it('clamps sidebar and commit panel widths independently', () => {
    useAppStore.setState({
      sidebarWidthPx: 320,
      commitPanelWidthPx: 420,
      commitPanelOpen: true,
    })

    act(() => {
      useAppStore.getState().setSidebarWidthPx(900)
    })

    expect(useAppStore.getState().sidebarWidthPx).toBe(520)
    expect(useAppStore.getState().commitPanelWidthPx).toBe(420)

    act(() => {
      useAppStore.getState().setCommitPanelWidthPx(100)
    })

    expect(useAppStore.getState().sidebarWidthPx).toBe(520)
    expect(useAppStore.getState().commitPanelWidthPx).toBe(320)
  })

  it('temporarily hides the chat selector when both side panels would starve the chat', () => {
    Object.defineProperty(window, 'innerWidth', { configurable: true, value: 1001 })
    useAppStore.setState({ sidebarCollapsed: false, sidebarWidthPx: 320, commitPanelOpen: true, commitPanelWidthPx: 420 })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<AppShell />))

    expect(host.querySelector('[data-testid="commit-panel"]')).toBeTruthy()
    expect(host.querySelector('[data-testid="chat-selector-panel"]')).toBeNull()

    act(() => root.unmount())
    host.remove()
  })

  it('resizes both outer panels from accessible keyboard separators', () => {
    useAppStore.setState({ commitPanelOpen: true })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<AppShell />))

    const sidebar = host.querySelector('[aria-label="Resize chat selector"]') as HTMLElement
    const commit = host.querySelector('[aria-label="Resize Git/SVN commit panel"]') as HTMLElement
    expect(sidebar.tabIndex).toBe(0)
    expect(sidebar.getAttribute('aria-valuemin')).toBe('260')
    expect(sidebar.getAttribute('aria-valuenow')).toBe('320')
    act(() => sidebar.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowRight', bubbles: true })))
    expect(useAppStore.getState().sidebarWidthPx).toBe(336)
    act(() => sidebar.dispatchEvent(new KeyboardEvent('keydown', { key: 'Home', bubbles: true })))
    expect(useAppStore.getState().sidebarWidthPx).toBe(260)

    expect(commit.getAttribute('aria-valuemax')).toBe('680')
    act(() => commit.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowLeft', bubbles: true })))
    expect(useAppStore.getState().commitPanelWidthPx).toBe(436)
    act(() => commit.dispatchEvent(new KeyboardEvent('keydown', { key: 'End', bubbles: true })))
    expect(useAppStore.getState().commitPanelWidthPx).toBe(680)

    act(() => root.unmount())
    host.remove()
  })

  it('collapses and expands the sidebar without changing the active chat', () => {
    useAppStore.setState({
      sessions: [
        { id: 'chat-1', name: 'Chat 1', viewMode: 'chat', folderId: 'folder', createdAt: new Date(), updatedAt: new Date() },
        { id: 'chat-2', name: 'Chat 2', viewMode: 'chat', folderId: 'folder', createdAt: new Date(), updatedAt: new Date() },
      ],
      activeSessionId: 'chat-2',
    })

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<AppShell />)
    })

    const collapseButton = host.querySelector('button[aria-label="Collapse chat selector"]') as HTMLButtonElement
    act(() => {
      collapseButton.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(useAppStore.getState().activeSessionId).toBe('chat-2')
    expect(host.querySelector('[data-testid="sidebar"]')).toBeNull()

    const expandButton = host.querySelector('button[aria-label="Expand chat selector"]') as HTMLButtonElement
    act(() => {
      expandButton.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(useAppStore.getState().activeSessionId).toBe('chat-2')
    expect(host.querySelector('[data-testid="sidebar"]')).toBeTruthy()

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('opens and closes the commit panel without changing the active chat', async () => {
    useAppStore.setState({
      sessions: [
        { id: 'chat-1', name: 'Chat 1', viewMode: 'chat', folderId: 'folder', workspaceDirectory: '/tmp/project', createdAt: new Date(), updatedAt: new Date() },
      ],
      activeSessionId: 'chat-1',
      getVcsCommitStatus: vi.fn().mockResolvedValue({
        available: false,
        vcsTypes: [],
        activeVcsType: 'git',
        workspaceDirectory: '/tmp/project',
        branchOrRevision: '',
        changedFiles: [],
        warning: 'No Git or SVN repository detected for this workspace.',
        error: '',
      }),
    })

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<AppShell />)
    })

    const openButton = host.querySelector('button[aria-label="Open Git/SVN commit panel"]') as HTMLButtonElement
    await act(async () => {
      openButton.dispatchEvent(new MouseEvent('click', { bubbles: true }))
      await Promise.resolve()
    })

    expect(useAppStore.getState().activeSessionId).toBe('chat-1')
    expect(host.textContent).toContain('Commit')
    expect(host.textContent).toContain('No Git or SVN repository detected for this workspace.')

    const closeButton = host.querySelector('button[aria-label="Close commit panel"]') as HTMLButtonElement
    act(() => {
      closeButton.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(useAppStore.getState().activeSessionId).toBe('chat-1')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('switches the VCS panel to the newly focused chat', async () => {
    const getVcsCommitStatus = vi.fn(async (chatId: string) => ({
      available: true,
      vcsTypes: ['git' as const],
      activeVcsType: 'git' as const,
      workspaceDirectory: `/tmp/${chatId}`,
      branchOrRevision: chatId,
      changedFiles: [],
      lineStatsReady: true,
      warning: '',
      error: '',
    }))
    useAppStore.setState({
      sessions: [
        { id: 'chat-1', name: 'Chat 1', viewMode: 'chat', folderId: 'folder', workspaceDirectory: '/tmp/chat-1', createdAt: new Date(), updatedAt: new Date() },
        { id: 'chat-2', name: 'Chat 2', viewMode: 'chat', folderId: 'folder', workspaceDirectory: '/tmp/chat-2', createdAt: new Date(), updatedAt: new Date() },
      ],
      activeSessionId: 'chat-1',
      commitPanelOpen: true,
      getVcsCommitStatus,
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    await act(async () => {
      root.render(<AppShell />)
      await Promise.resolve()
    })
    expect(host.textContent).toContain('/tmp/chat-1')

    await act(async () => {
      useAppStore.setState({ activeSessionId: 'chat-2' })
      await Promise.resolve()
    })
    expect(host.textContent).toContain('/tmp/chat-2')
    expect(getVcsCommitStatus).toHaveBeenCalledWith('chat-2', 'git', expect.any(Object))

    act(() => root.unmount())
    host.remove()
  })

  it('renders commit files as checklist rows with line stats and no diff view', async () => {
    useAppStore.setState({
      sessions: [
        { id: 'chat-1', name: 'Chat 1', viewMode: 'chat', folderId: 'folder', workspaceDirectory: '/tmp/project', createdAt: new Date(), updatedAt: new Date() },
      ],
      activeSessionId: 'chat-1',
      getVcsCommitStatus: vi.fn().mockResolvedValue({
        available: true,
        vcsTypes: ['git'],
        activeVcsType: 'git',
        workspaceDirectory: '/tmp/project',
        branchOrRevision: 'main',
        changedFiles: [
          { path: 'src/app.ts', status: ' M', staged: false, additions: 12, deletions: 3, binary: false },
          { path: 'assets/logo.png', status: '??', staged: false, additions: 0, deletions: 0, binary: true },
        ],
        warning: '',
        error: '',
      }),
    })

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<AppShell />)
    })

    const openButton = host.querySelector('button[aria-label="Open Git/SVN commit panel"]') as HTMLButtonElement
    await act(async () => {
      openButton.dispatchEvent(new MouseEvent('click', { bubbles: true }))
      await Promise.resolve()
    })

    expect(host.textContent).toContain('2 changed files')
    expect(host.textContent).toContain('src/app.ts')
    expect(host.textContent).toContain('+12')
    expect(host.textContent).toContain('-3')
    expect(host.textContent).toContain('assets/logo.png')
    expect(host.textContent).toContain('BIN')
    expect(host.textContent).not.toContain('Diff')
    expect(host.textContent).not.toContain('No diff available')
    expect(useAppStore.getState().activeSessionId).toBe('chat-1')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('generates and commits title and description for selected files', async () => {
    const generateVcsCommitMessage = vi.fn().mockResolvedValue({
      title: 'Update commit panel',
      description: '- Refresh checklist file controls',
    })
    const commitVcsChanges = vi.fn().mockResolvedValue({
      ok: true,
      message: 'Git commit created.',
      error: '',
    })
    useAppStore.setState({
      sessions: [
        { id: 'chat-1', name: 'Chat 1', viewMode: 'chat', folderId: 'folder', workspaceDirectory: '/tmp/project', createdAt: new Date(), updatedAt: new Date() },
      ],
      activeSessionId: 'chat-1',
      getVcsCommitStatus: vi.fn().mockResolvedValue({
        available: true,
        vcsTypes: ['git'],
        activeVcsType: 'git',
        workspaceDirectory: '/tmp/project',
        branchOrRevision: 'main',
        changedFiles: [
          { path: 'src/app.ts', status: ' M', staged: false, additions: 12, deletions: 3, binary: false },
        ],
        warning: '',
        error: '',
      }),
      generateVcsCommitMessage,
      commitVcsChanges,
    })

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<AppShell />)
    })

    const openButton = host.querySelector('button[aria-label="Open Git/SVN commit panel"]') as HTMLButtonElement
    await act(async () => {
      openButton.dispatchEvent(new MouseEvent('click', { bubbles: true }))
      await Promise.resolve()
    })

    const fileCheckbox = host.querySelector('button[role="checkbox"][aria-label="Select src/app.ts"]') as HTMLButtonElement
    await act(async () => {
      fileCheckbox.dispatchEvent(new MouseEvent('click', { bubbles: true }))
      await Promise.resolve()
    })

    const aiButton = host.querySelector('button[aria-label="Generate commit message"]') as HTMLButtonElement
    await act(async () => {
      aiButton.dispatchEvent(new MouseEvent('click', { bubbles: true }))
      await Promise.resolve()
    })

    expect(generateVcsCommitMessage).toHaveBeenCalledWith('chat-1', 'git', ['src/app.ts'])
    expect((host.querySelector('input[placeholder="Summary"]') as HTMLInputElement).value).toBe('Update commit panel')
    expect((host.querySelector('textarea[placeholder="Description"]') as HTMLTextAreaElement).value).toBe('- Refresh checklist file controls')

    const commitButton = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Commit selected files') as HTMLButtonElement
    await act(async () => {
      commitButton.dispatchEvent(new MouseEvent('click', { bubbles: true }))
      await Promise.resolve()
    })

    expect(commitVcsChanges).toHaveBeenCalledWith('chat-1', 'git', 'Update commit panel\n\n- Refresh checklist file controls', ['src/app.ts'])
    expect(useAppStore.getState().activeSessionId).toBe('chat-1')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('does not apply a generated message after switching chats', async () => {
    let finishGeneration: (value: { title: string; description: string }) => void = () => {}
    const generateVcsCommitMessage = vi.fn(() => new Promise<{ title: string; description: string }>((resolve) => {
      finishGeneration = resolve
    }))
    useAppStore.setState({
      sessions: [
        { id: 'chat-1', name: 'Chat 1', viewMode: 'chat', folderId: 'folder', workspaceDirectory: '/tmp/one', createdAt: new Date(), updatedAt: new Date() },
        { id: 'chat-2', name: 'Chat 2', viewMode: 'chat', folderId: 'folder', workspaceDirectory: '/tmp/two', createdAt: new Date(), updatedAt: new Date() },
      ],
      activeSessionId: 'chat-1',
      commitPanelOpen: true,
      getVcsCommitStatus: vi.fn(async (chatId: string) => ({
        available: true,
        vcsTypes: ['git' as const],
        activeVcsType: 'git' as const,
        workspaceDirectory: chatId === 'chat-1' ? '/tmp/one' : '/tmp/two',
        branchOrRevision: 'main',
        changedFiles: [{
          path: chatId === 'chat-1' ? 'one.ts' : 'two.ts',
          status: ' M',
          staged: false,
          additions: 1,
          deletions: 0,
          binary: false,
        }],
        lineStatsReady: true,
        warning: '',
        error: '',
      })),
      generateVcsCommitMessage,
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    await act(async () => { root.render(<AppShell />); await Promise.resolve() })

    act(() => (host.querySelector('button[aria-label="Select one.ts"]') as HTMLButtonElement).click())
    act(() => (host.querySelector('button[aria-label="Generate commit message"]') as HTMLButtonElement).click())
    await act(async () => {
      useAppStore.setState({ activeSessionId: 'chat-2' })
      await Promise.resolve()
    })
    const summary = host.querySelector('input[placeholder="Summary"]') as HTMLInputElement
    act(() => {
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')?.set?.call(summary, 'Second chat draft')
      summary.dispatchEvent(new Event('input', { bubbles: true }))
    })
    await act(async () => {
      finishGeneration({ title: 'First chat suggestion', description: 'Only for chat 1' })
      await Promise.resolve()
    })

    expect(summary.value).toBe('Second chat draft')

    act(() => root.unmount())
    host.remove()
  })

  it('does not apply a completed commit to a different active chat', async () => {
    let finishCommit: (value: { ok: boolean; message: string; error: string }) => void = () => {}
    const commitVcsChanges = vi.fn(() => new Promise<{ ok: boolean; message: string; error: string }>((resolve) => {
      finishCommit = resolve
    }))
    useAppStore.setState({
      sessions: [
        { id: 'chat-1', name: 'Chat 1', viewMode: 'chat', folderId: 'folder', workspaceDirectory: '/tmp/one', createdAt: new Date(), updatedAt: new Date() },
        { id: 'chat-2', name: 'Chat 2', viewMode: 'chat', folderId: 'folder', workspaceDirectory: '/tmp/two', createdAt: new Date(), updatedAt: new Date() },
      ],
      activeSessionId: 'chat-1',
      commitPanelOpen: true,
      getVcsCommitStatus: vi.fn(async (chatId: string) => ({
        available: true,
        vcsTypes: ['git' as const],
        activeVcsType: 'git' as const,
        workspaceDirectory: chatId === 'chat-1' ? '/tmp/one' : '/tmp/two',
        branchOrRevision: 'main',
        changedFiles: [{ path: chatId === 'chat-1' ? 'one.ts' : 'two.ts', status: ' M', staged: false, additions: 1, deletions: 0, binary: false }],
        lineStatsReady: true,
        warning: '',
        error: '',
      })),
      commitVcsChanges,
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    await act(async () => { root.render(<AppShell />); await Promise.resolve() })

    act(() => (host.querySelector('button[aria-label="Select one.ts"]') as HTMLButtonElement).click())
    const summary = host.querySelector('input[placeholder="Summary"]') as HTMLInputElement
    act(() => {
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')?.set?.call(summary, 'Commit chat one')
      summary.dispatchEvent(new Event('input', { bubbles: true }))
    })
    act(() => (Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Commit selected files') as HTMLButtonElement).click())
    await act(async () => {
      useAppStore.setState({ activeSessionId: 'chat-2' })
      finishCommit({ ok: true, message: 'Git commit created.', error: '' })
      await Promise.resolve()
    })

    expect(host.textContent).toContain('/tmp/two')
    expect(host.textContent).not.toContain('Git commit created.')
    expect(host.textContent).not.toContain('Committing…')
    act(() => root.unmount())
    host.remove()
  })
  it('retains real move failures after toast dismissal and allows explicit history dismissal', async () => {
    vi.useFakeTimers()
    const originalAdd = useAppStore.getState().addResourceReference
    useAppStore.setState({
      updateChecksEnabled: false,
      resourceCollections: [{ id: 'destination', name: 'Destination', collapsed: false, references: [] }],
      addResourceReference: vi.fn().mockResolvedValueOnce(null).mockRejectedValueOnce(new Error('Transport closed')),
    })
    const host = document.createElement('div')
    document.body.append(host)
    const root = createRoot(host)
    try {
      await act(async () => root.render(<AppShell />))
      expect(host.querySelector('[aria-label="Dismiss collection notification"]')).toBeNull()
      await act(async () => { await moveResourceToCollection('destination', 'workspace-folder', 'folder', 'Workspace') })
      const toast = host.querySelector('[role="alert"]')!
      expect(toast.textContent).toContain('Error')
      expect(toast.textContent).toContain('Could not move "Workspace".')
      expect(toast.querySelector('time')?.getAttribute('datetime')).toBeTruthy()
      act(() => (host.querySelector('[aria-label="Dismiss collection notification"]') as HTMLButtonElement).click())
      expect(host.querySelector('[role="alert"]')).toBeNull()
      await act(async () => { await moveResourceToCollection('destination', 'workspace-folder', 'folder', 'Workspace') })
      act(() => vi.advanceTimersByTime(8000))
      expect(host.querySelector('[role="alert"]')).toBeNull()
      act(() => (host.querySelector('[aria-label="2 alerts"]') as HTMLButtonElement).click())
      const panel = host.querySelector('[data-testid="notifications-panel"]')!
      expect(panel.querySelectorAll('time')).toHaveLength(2)
      expect(panel.textContent).toContain('Transport closed')
      act(() => useAppStore.setState({ statusLine: 'Another status' }))
      expect(panel.querySelectorAll('time')).toHaveLength(2)
      act(() => (panel.querySelector('[aria-label="Dismiss Collection move failed"]') as HTMLButtonElement).click())
      expect(panel.querySelectorAll('time')).toHaveLength(1)
      act(() => (panel.querySelector('[aria-label="Close notifications"]') as HTMLButtonElement).click())
      act(() => (host.querySelector('[aria-label="2 alerts"]') as HTMLButtonElement).click())
      expect(host.querySelectorAll('[data-testid="notifications-panel"] time')).toHaveLength(1)
    } finally {
      act(() => root.unmount())
      host.remove()
      useAppStore.setState({ addResourceReference: originalAdd })
      vi.useRealTimers()
    }
  })

})
