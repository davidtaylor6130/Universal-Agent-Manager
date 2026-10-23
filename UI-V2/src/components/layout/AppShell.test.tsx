import { act, Profiler, type ReactNode } from 'react'
import { createRoot } from 'react-dom/client'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { useAppStore } from '../../store/useAppStore'

const { mainPanelRenderCount, sidebarRenderCount } = vi.hoisted(() => ({
  mainPanelRenderCount: { value: 0 },
  sidebarRenderCount: { value: 0 },
}))

vi.mock('./Sidebar', () => ({
  Sidebar: () => {
    sidebarRenderCount.value += 1
    const folderCount = useAppStore(state => state.folders.length)
    return <div data-testid="sidebar">Sidebar {folderCount}</div>
  },
}))

vi.mock('./MainPanel', () => ({
  MainPanel: () => {
    mainPanelRenderCount.value += 1
    const activeSessionId = useAppStore(state => state.activeSessionId)
    return <div data-testid="main-panel" data-session={activeSessionId}>Main panel<input aria-label="Chat draft" defaultValue=""/></div>
  },
}))

import { AppShell } from './AppShell'
import { moveResourceToCollection } from '../sidebar/CollectionMenuItems'
import { FILE_MANAGER_NAME } from '../../utils/shortcuts'
import { APP_FAILURE_EVENT, reportAppFailure } from '../../utils/appFailure'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

const defaultSetSettingsOpen = useAppStore.getState().setSettingsOpen
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
    sidebarRenderCount.value = 0
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
    const updateAlert = host.querySelector('[data-testid="transient-alert"]')
    expect(host.querySelector('[data-testid="transient-alert-announcer"]')?.getAttribute('aria-live')).toBe('polite')
    expect(updateAlert?.textContent).toContain('2 updates available')
    expect(updateAlert?.textContent).toContain('0.130.0')
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
    const setSettingsOpen = vi.fn()
    useAppStore.setState({
      setSettingsOpen,
    })

    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)

    act(() => {
      root.render(<AppShell />)
    })

    expect(host.querySelector('button[aria-label="Memory library"]')).toBeNull()
    const settingsButton = host.querySelector('button[aria-label="Settings"]') as HTMLButtonElement

    act(() => {
      settingsButton.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(setSettingsOpen).toHaveBeenCalledWith(true)
    expect(Array.from(host.querySelectorAll('button')).some((button) => button.textContent === '+New Chat')).toBe(false)

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('does not open a standalone memory overlay when closing embedded memory settings', async () => {
    const memoryModule = await import('../settings/MemoryLibraryModal')
    const memoryComponent = memoryModule.MemoryLibraryModal as unknown as { render: (props: { embedded?: boolean }, ref: unknown) => ReactNode }
    const originalRender = memoryComponent.render
    const standaloneRender = vi.fn()
    vi.spyOn(memoryComponent, 'render').mockImplementation((props, ref) => {
      if (!props.embedded) standaloneRender()
      return originalRender(props, ref)
    })
    useAppStore.setState({
      setSettingsOpen: defaultSetSettingsOpen,
      openAllMemoryLibrary: async () => {
        useAppStore.setState({ memoryLibraryScope: { scopeType: 'all', folderId: '', label: 'All memory', rootPath: '' } })
        return true
      },
      closeMemoryLibrary: () => useAppStore.setState({ memoryLibraryScope: null }),
      refreshCustomThemes: vi.fn(async () => true),
      memoryLibraryEntries: [],
      memoryLibraryLoading: false,
      memoryLibraryError: '',
      mcpServers: [],
    })
    const host = document.createElement('div'), root = createRoot(host)
    document.body.appendChild(host)
    try {
      await act(async () => {
        useAppStore.setState({ memoryLibraryScope: { scopeType: 'all', folderId: '', label: 'All memory', rootPath: '' } })
        root.render(<AppShell />)
        await import('../settings/MemoryLibraryModal')
      })
      expect(standaloneRender).toHaveBeenCalled()
      await act(async () => useAppStore.setState({ memoryLibraryScope: null }))
      standaloneRender.mockClear()
      await act(async () => { host.querySelector<HTMLButtonElement>('[aria-label="Settings"]')!.click(); await import('../settings/SettingsModal') })
      standaloneRender.mockClear()
      await act(async () => host.querySelector<HTMLButtonElement>('[aria-label="Back to chats"]')!.click())
      expect(useAppStore.getState().isSettingsOpen).toBe(false)
      expect(standaloneRender).not.toHaveBeenCalled()
      expect(host.querySelector('[role="dialog"]')).toBeNull()
    } finally {
      act(() => root.unmount())
      host.remove()
    }
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
    const chatRendersBeforeNavigation = mainPanelRenderCount.value
    const sidebarRendersBeforeNavigation = sidebarRenderCount.value
    const settings = host.querySelector<HTMLButtonElement>('[aria-label="Settings"]')!
    act(() => settings.click())
    act(() => settings.click())
    expect(useAppStore.getState().isSettingsOpen).toBe(false)
    await act(async () => { settings.click(); await import('../settings/SettingsModal') })
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
    expect(mainPanelRenderCount.value).toBe(chatRendersBeforeNavigation)
    expect(sidebarRenderCount.value).toBe(sidebarRendersBeforeNavigation)
    act(() => useAppStore.setState({ activeSessionId: 'new-selection', folders: [{ id: 'new-folder', name: 'New folder', directory: '/tmp/new-folder' }] }))
    expect(host.querySelector('[data-testid="main-panel"]')!.getAttribute('data-session')).toBe('new-selection')
    expect(host.querySelector('[data-testid="sidebar"]')!.textContent).toContain('Sidebar 1')
    act(() => root.unmount())
    host.remove()
  })

  it('retains unsaved Settings and chat drafts when visiting Usage', async () => {
    useAppStore.setState({ setSettingsOpen: vi.fn(open => useAppStore.setState({ isSettingsOpen: open })), refreshCustomThemes: vi.fn(async () => true), mcpServers: [] })
    const host = document.createElement('div'), root = createRoot(host)
    document.body.appendChild(host)
    try {
      await act(async () => root.render(<AppShell />))
      const draft = host.querySelector<HTMLInputElement>('[aria-label="Chat draft"]')!
      draft.value = 'Unsent chat'
      const settings = host.querySelector<HTMLButtonElement>('[aria-label="Settings"]')!
      await act(async () => { settings.click(); await import('../settings/SettingsModal') })
      act(() => host.querySelector<HTMLButtonElement>('[aria-label="MCP Servers"]')!.click())
      act(() => Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find(button => button.textContent === 'JSON')!.click())
      const editor = host.querySelector<HTMLTextAreaElement>('[aria-label="MCP JSON"]')!
      act(() => {
        Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')!.set!.call(editor, '[{"name":"draft"}]')
        editor.dispatchEvent(new Event('input', { bubbles: true }))
      })
      await act(async () => { host.querySelector<HTMLButtonElement>('[aria-label="Usage"]')!.click(); await import('../usage/UsagePage') })
      expect(host.querySelector<HTMLElement>('[aria-label="Settings workspace"]')!.hidden).toBe(true)
      expect(host.querySelector('[aria-label="Usage workspace"]')).toBeTruthy()
      expect(host.querySelector<HTMLButtonElement>('[aria-label="Settings"]')!.getAttribute('aria-pressed')).not.toBe('true')
      act(() => settings.click())
      expect(host.querySelector('[aria-label="Usage workspace"]')).toBeNull()
      expect(editor.value).toBe('[{"name":"draft"}]')
      expect(draft.value).toBe('Unsent chat')
      act(() => settings.click())
      expect(document.body.querySelector('[aria-label="Discard unsaved MCP changes"]')).toBeTruthy()
    } finally { act(() => root.unmount()); host.remove() }
  })

  it('guards the Settings rail toggle and Back with the same unsaved draft confirmation', async () => {
    const setSettingsOpen = vi.fn((open: boolean) => useAppStore.setState({isSettingsOpen:open}))
    useAppStore.setState({setSettingsOpen, refreshCustomThemes:vi.fn(async () => true), mcpServers:[]})
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    await act(async () => root.render(<AppShell/>))
    const settings = host.querySelector<HTMLButtonElement>('[aria-label="Settings"]')!
    await act(async () => { settings.click(); await import('../settings/SettingsModal') })
    act(() => host.querySelector<HTMLButtonElement>('[aria-label="MCP Servers"]')!.click())
    act(() => Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find(button => button.textContent === 'JSON')!.click())
    const editor = host.querySelector<HTMLTextAreaElement>('[aria-label="MCP JSON"]')!
    act(() => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype,'value')!.set!.call(editor,'[{"name":"draft"}]')
      editor.dispatchEvent(new Event('input',{bubbles:true}))
    })
    setSettingsOpen.mockClear()
    act(() => settings.click())
    expect(setSettingsOpen).not.toHaveBeenCalled()
    const confirmation = document.body.querySelector('[aria-label="Discard unsaved MCP changes"]')!
    expect(confirmation).toBeTruthy()
    const buttons = confirmation.querySelectorAll<HTMLButtonElement>('button')
    buttons[buttons.length - 1].focus()
    const tab = new KeyboardEvent('keydown', {key:'Tab',bubbles:true,cancelable:true})
    act(() => buttons[buttons.length - 1].dispatchEvent(tab))
    expect(tab.defaultPrevented).toBe(true)
    expect(document.activeElement).toBe(buttons[0])
    act(() => Array.from(document.body.querySelectorAll<HTMLButtonElement>('button')).find(button => button.textContent === 'Keep editing')!.click())
    act(() => host.querySelector<HTMLButtonElement>('[aria-label="Back to chats"]')!.click())
    expect(setSettingsOpen).not.toHaveBeenCalled()
    expect(editor.value).toBe('[{"name":"draft"}]')
    act(() => Array.from(document.body.querySelectorAll<HTMLButtonElement>('button')).find(button => button.textContent === 'Discard changes')!.click())
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
    const onRender = vi.fn()
    act(() => root.render(<Profiler id="notifications" onRender={onRender}><AppShell /></Profiler>))

    const alerts = host.querySelector('button[aria-label="1 alert"]') as HTMLButtonElement
    expect(alerts).toBeTruthy()
    act(() => alerts.click())
    const panel = host.querySelector('[data-testid="notifications-panel"]')
    expect(panel).toBeTruthy()
    expect(panel?.classList.contains('uam-shell-panel--right')).toBe(true)
    expect(panel?.textContent).toContain('Workspace folder missing: Deleted project')
    expect(panel?.textContent).toContain('/tmp/deleted project')
    onRender.mockClear()
    for (let index = 0; index < 20; index++) {
      act(() => useAppStore.setState({ cliTranscriptBySessionId: { other: { terminalId: 'other', content: String(index) } } }))
    }
    expect(onRender).not.toHaveBeenCalled()
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

    act(() => useAppStore.setState((state) => ({ folders: state.folders.map((folder) => ({ ...folder, missing: false })) })))
    expect(panel?.textContent).not.toContain('Workspace folder missing: Deleted project')
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
    act(() => (host.querySelector(`button[aria-label="Dismiss ${FILE_MANAGER_NAME} action"]`) as HTMLButtonElement).click())
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
      await import('./VcsCommitPanel')
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
    expect(host.querySelector('[title="/tmp/chat-1"]')).toBeTruthy()

    await act(async () => {
      useAppStore.setState({ activeSessionId: 'chat-2' })
      await Promise.resolve()
    })
    expect(host.querySelector('[title="/tmp/chat-2"]')).toBeTruthy()
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
    expect(host.querySelector('[title="src/app.ts"]')).toBeTruthy()
    expect(host.textContent).toContain('+12')
    expect(host.textContent).toContain('-3')
    expect(host.querySelector('[title="assets/logo.png"]')).toBeTruthy()
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
    expect((host.querySelector('input[aria-label="Commit summary"]') as HTMLInputElement).value).toBe('Update commit panel')
    expect((host.querySelector('textarea[aria-label="Commit description"]') as HTMLTextAreaElement).value).toBe('- Refresh checklist file controls')

    const commitButton = Array.from(host.querySelectorAll('button')).find((button) => /^Commit \d+ files?$/.test(button.textContent ?? '')) as HTMLButtonElement
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
    const summary = host.querySelector('input[aria-label="Commit summary"]') as HTMLInputElement
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
    const summary = host.querySelector('input[aria-label="Commit summary"]') as HTMLInputElement
    act(() => {
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')?.set?.call(summary, 'Commit chat one')
      summary.dispatchEvent(new Event('input', { bubbles: true }))
    })
    act(() => (Array.from(host.querySelectorAll('button')).find((button) => /^Commit \d+ files?$/.test(button.textContent ?? '')) as HTMLButtonElement).click())
    await act(async () => {
      useAppStore.setState({ activeSessionId: 'chat-2' })
      finishCommit({ ok: true, message: 'Git commit created.', error: '' })
      await Promise.resolve()
    })

    expect(host.querySelector('[title="/tmp/two"]')).toBeTruthy()
    expect(host.textContent).not.toContain('Git commit created.')
    expect(host.textContent).not.toContain('Committing…')
    act(() => root.unmount())
    host.remove()
  })
  it('keeps failed moves in notifications after the transient alert is dismissed', async () => {
    vi.useFakeTimers()
    const originalMove = useAppStore.getState().moveResourceToCollection
    useAppStore.setState({
      updateChecksEnabled: false,
      resourceCollections: [{ id: 'destination', name: 'Destination', collapsed: false, references: [] }],
      moveResourceToCollection: vi.fn().mockRejectedValueOnce(new Error('Membership changed')).mockRejectedValueOnce(new Error('Transport closed')),
    })
    const host = document.createElement('div')
    document.body.append(host)
    const root = createRoot(host)
    try {
      await act(async () => root.render(<AppShell />))
      expect(host.querySelector('[data-testid="transient-alert"]')).toBeNull()
      await act(async () => { await moveResourceToCollection('destination', 'workspace-folder', 'folder', 'Workspace') })
      const alert = host.querySelector('[data-testid="transient-alert"]')!
      expect(host.querySelector('[data-testid="transient-alert-announcer"]')?.getAttribute('aria-live')).toBe('polite')
      expect(alert.textContent).toContain('Could not move "Workspace".')
      act(() => (host.querySelector('[aria-label="Dismiss alert"]') as HTMLButtonElement).click())
      act(() => vi.advanceTimersByTime(200))
      expect(host.querySelector('[data-testid="transient-alert"]')).toBeNull()
      await act(async () => { await moveResourceToCollection('destination', 'workspace-folder', 'folder', 'Workspace') })
      act(() => vi.advanceTimersByTime(8000))
      act(() => vi.advanceTimersByTime(200))
      expect(host.querySelector('[data-testid="transient-alert"]')).toBeNull()
      act(() => (host.querySelector('[aria-label="2 alerts"]') as HTMLButtonElement).click())
      const panel = host.querySelector('[data-testid="notifications-panel"]')!
      expect(panel.querySelectorAll('time')).toHaveLength(2)
      expect(panel.textContent).toContain('Transport closed')
      act(() => useAppStore.setState({ statusLine: 'Another status' }))
      expect(panel.querySelectorAll('time')).toHaveLength(2)
      act(() => (panel.querySelector('[aria-label="Dismiss Change not saved"]') as HTMLButtonElement).click())
      expect(panel.querySelectorAll('time')).toHaveLength(1)
      act(() => (panel.querySelector('[aria-label="Close notifications"]') as HTMLButtonElement).click())
      act(() => (host.querySelector('[aria-label="2 alerts"]') as HTMLButtonElement).click())
      expect(host.querySelectorAll('[data-testid="notifications-panel"] time')).toHaveLength(1)
    } finally {
      act(() => root.unmount())
      host.remove()
      useAppStore.setState({ moveResourceToCollection: originalMove })
      vi.useRealTimers()
    }
  })

  const mountShell = async () => {
    // Real keyboard focus opens the shared Radix tooltip in this jsdom fixture.
    const observed = new Set<Element>()
    const callbacks = new Set<ResizeObserverCallback>()
    vi.stubGlobal('ResizeObserver', class {
      constructor(private callback: ResizeObserverCallback) { callbacks.add(callback) }
      observe(element: Element) { callbacks.add(this.callback); observed.add(element) }
      unobserve(element: Element) { observed.delete(element) }
      disconnect() { callbacks.delete(this.callback) }
    })
    const layoutRects = new Map<Element, DOMRect>()
    const nativeRect = HTMLElement.prototype.getBoundingClientRect
    vi.spyOn(HTMLElement.prototype, 'getBoundingClientRect').mockImplementation(function () {
      if (layoutRects.has(this)) return layoutRects.get(this)!
      if (this.classList.contains('uam-app')) return new DOMRect(0, 0, window.innerWidth, 800)
      if (this.classList.contains('uam-side-rail--right')) return new DOMRect(window.innerWidth - 44, 44, 44, 756)
      if (this.hasAttribute('data-notification-bell')) return new DOMRect(window.innerWidth - 37, 722, 30, 30)
      if (this.dataset.testid === 'transient-alert') {
        const width = Number.parseFloat(this.style.width) || 360
        const height = Math.min(36, Number.parseFloat(this.style.maxHeight) || 36)
        return new DOMRect(window.innerWidth - (Number.parseFloat(this.style.right) || 0) - width,
          window.innerHeight - (Number.parseFloat(this.style.bottom) || 0) - height, width, height)
      }
      if (this.closest('[data-testid="transient-alert"]') && this.tagName === 'BUTTON')
        return new DOMRect(0, 0, this.getAttribute('aria-label') === 'Dismiss alert' ? 26 : 140, 26)
      return nativeRect.call(this)
    })
    const host = document.createElement('div')
    document.body.append(host)
    const root = createRoot(host)
    await act(async () => root.render(<AppShell />))
    return {
      host,
      alertSurface: () => host.querySelector<HTMLElement>('[data-testid="transient-alert"]'),
      bell: () => Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => /^(?:[0-9]+ alerts?|No new alerts)$/.test(button.getAttribute('aria-label') ?? ''))!,
      layoutRects,
      observed,
      activeObserverCount: () => callbacks.size,
      notifyResize: () => act(() => { callbacks.forEach((callback) => callback([], {} as ResizeObserver)); vi.advanceTimersByTime(20) }),
      settle: (ms: number) => act(() => { vi.advanceTimersByTime(ms) }),
      cleanup: () => { act(() => root.unmount()); host.remove(); vi.unstubAllGlobals() },
    }
  }

  it('publishes into one pre-existing empty polite announcer without repeating duplicate, rerender or closing notices', async () => {
    vi.useFakeTimers()
    const originalHeight = window.innerHeight
    Object.defineProperty(window, 'innerHeight', { configurable: true, value: 800 })
    useAppStore.setState({ updateChecksEnabled: false })
    const { host, alertSurface, settle, cleanup } = await mountShell()
    let observer: MutationObserver | undefined
    try {
      const announcer = host.querySelector<HTMLElement>('[data-testid="transient-alert-announcer"]')!
      expect(announcer).toBeTruthy()
      expect(announcer.textContent).toBe('')
      expect(announcer.getAttribute('role')).toBe('status')
      expect(announcer.getAttribute('aria-live')).toBe('polite')
      expect(announcer.getAttribute('aria-atomic')).toBe('true')
      expect(alertSurface()).toBeNull()
      const failure = { id: 'fixture-announcement-a', time: '2026-01-01T12:00:00.000Z', message: 'Private announcement fixture', detail: 'Retained detail' }
      const publish = (id = failure.id) => window.dispatchEvent(new CustomEvent(APP_FAILURE_EVENT, { detail: { ...failure, id } }))
      await act(async () => { publish(); await Promise.resolve() })
      expect(host.querySelector('[data-testid="transient-alert-announcer"]')).toBe(announcer)
      expect(announcer.textContent).toBe(`${failure.message} Saved in Notifications. Use the Notifications button to read it.`)
      expect(announcer.closest('[aria-hidden="true"]')).toBeNull()
      expect(alertSurface()!.hasAttribute('aria-live')).toBe(false)
      expect(alertSurface()!.getAttribute('role')).not.toBe('status')
      expect(alertSurface()!.querySelector('[aria-label="Notification message"]')?.textContent).toBe(failure.message)
      expect(host.querySelectorAll('[data-testid="transient-alert-announcer"]')).toHaveLength(1)
      expect(host.querySelectorAll('[role="status"][aria-live="polite"]')).toHaveLength(1)
      const firstAnnouncement = announcer.firstChild
      const changes: MutationRecord[] = []
      observer = new MutationObserver((records) => changes.push(...records))
      observer.observe(announcer, { childList: true, characterData: true, subtree: true })

      await act(async () => { publish(); useAppStore.setState({ statusLine: 'Unrelated shell rerender' }); await Promise.resolve() })
      expect(announcer.firstChild).toBe(firstAnnouncement)
      expect(changes).toEqual([])
      // Distinct actual notifications with identical words still produce a live-region child update.
      await act(async () => { publish('fixture-announcement-b'); await Promise.resolve() })
      expect(announcer.textContent).toBe(`${failure.message} Saved in Notifications. Use the Notifications button to read it.`)
      expect(announcer.firstChild).not.toBe(firstAnnouncement)
      expect(changes.length).toBeGreaterThan(0)
      changes.length = 0
      const secondAnnouncement = announcer.firstChild
      await act(async () => { alertSurface()!.querySelector<HTMLButtonElement>('[aria-label="Dismiss alert"]')!.click(); await Promise.resolve() })
      settle(200)
      await act(async () => { await Promise.resolve() })
      expect(alertSurface()).toBeNull()
      expect(host.querySelector('[data-testid="transient-alert-announcer"]')).toBe(announcer)
      expect(announcer.firstChild).toBe(secondAnnouncement)
      expect(changes).toEqual([])
    } finally {
      observer?.disconnect()
      cleanup()
      Object.defineProperty(window, 'innerHeight', { configurable: true, value: originalHeight })
      vi.useRealTimers()
    }
  })

  it('uses the same persistent announcer for a no-fit bell description without duplicate resize or panel announcements', async () => {
    vi.useFakeTimers()
    const originalWidth = window.innerWidth
    const originalHeight = window.innerHeight
    Object.defineProperty(window, 'innerWidth', { configurable: true, value: 720 })
    Object.defineProperty(window, 'innerHeight', { configurable: true, value: 120 })
    useAppStore.setState({ updateChecksEnabled: false })
    const { host, alertSurface, bell, layoutRects, notifyResize, settle, cleanup } = await mountShell()
    let observer: MutationObserver | undefined
    try {
      const announcer = host.querySelector<HTMLElement>('[data-testid="transient-alert-announcer"]')!
      expect(announcer).toBeTruthy()
      expect(announcer.textContent).toBe('')
      const rail = host.querySelector('.uam-side-rail--right')!
      const header = document.createElement('div'); header.className = 'uam-chat-pane__header'
      host.querySelector('.uam-app')!.append(header)
      layoutRects.set(rail, new DOMRect(676, 44, 44, 76))
      layoutRects.set(bell(), new DOMRect(683, 82, 30, 30))
      layoutRects.set(header, new DOMRect(44, 44, 632, 44))
      await act(async () => { reportAppFailure('No-fit private fixture', 'Retained detail'); await Promise.resolve() })
      expect(alertSurface()!.getAttribute('aria-hidden')).toBe('true')
      expect(document.getElementById(bell().getAttribute('aria-describedby')!)).toBe(announcer)
      expect(announcer.textContent).toBe('No-fit private fixture Saved in Notifications. Use the Notifications button to read it.')
      expect(host.querySelectorAll('[role="status"][aria-live="polite"]')).toHaveLength(1)
      expect(announcer.getAttribute('aria-atomic')).toBe('true')
      expect(announcer.closest('[aria-hidden="true"]')).toBeNull()
      const announcedNode = announcer.firstChild
      const changes: MutationRecord[] = []
      observer = new MutationObserver((records) => changes.push(...records))
      observer.observe(announcer, { childList: true, characterData: true, subtree: true })
      notifyResize()
      await act(async () => { await Promise.resolve() })
      expect(announcer.firstChild).toBe(announcedNode)
      expect(changes).toEqual([])
      // Restoring usable space changes placement, not the already announced notification.
      Object.defineProperty(window, 'innerHeight', { configurable: true, value: 300 })
      layoutRects.set(rail, new DOMRect(676, 44, 44, 256))
      layoutRects.set(bell(), new DOMRect(683, 222, 30, 30))
      notifyResize()
      await act(async () => { await Promise.resolve() })
      expect(alertSurface()!.getAttribute('aria-hidden')).toBeNull()
      expect(bell().hasAttribute('aria-describedby')).toBe(false)
      expect(host.querySelector('[data-testid="transient-alert-announcer"]')).toBe(announcer)
      expect(announcer.firstChild).toBe(announcedNode)
      expect(changes).toEqual([])
      act(() => bell().click())
      settle(200)
      await act(async () => { await Promise.resolve() })
      expect(alertSurface()).toBeNull()
      expect(announcer.firstChild).toBe(announcedNode)
      expect(changes).toEqual([])
    } finally {
      observer?.disconnect()
      cleanup()
      Object.defineProperty(window, 'innerWidth', { configurable: true, value: originalWidth })
      Object.defineProperty(window, 'innerHeight', { configurable: true, value: originalHeight })
      vi.useRealTimers()
    }
  })

  it('anchors one shared alert beside the right rail and Review updates opens the updates panel', async () => {
    vi.useFakeTimers()
    window.localStorage.setItem('uam-update-catalog-v1', JSON.stringify({
      checkedAt: '2026-07-13T20:00:00.000Z',
      uam: { version: 'V4.2.0', url: 'https://example.test/uam' },
      providers: { 'codex-cli': { version: '0.130.0', url: 'https://example.test/codex' } },
    }))
    useAppStore.setState({
      providers: [{ id: 'codex-cli', name: 'Codex CLI', shortName: 'Codex', description: '', color: '#fff' }],
      cliVersionManager: { providers: [{ providerId: 'codex-cli', installedVersion: '0.124.0', selectedVersion: '0.124.0', availableVersions: [{ version: '0.124.0', preferred: true }], preferredVersion: '0.124.0', status: 'verified', message: '', running: false, lastCommand: '', lastOutput: '' }] },
    })
    const { host, alertSurface, bell, settle, cleanup } = await mountShell()
    try {
      expect(host.querySelectorAll('[data-testid="transient-alert"]')).toHaveLength(1)
      const surface = alertSurface()!
      const announcer = host.querySelector('[data-testid="transient-alert-announcer"]')!
      expect(announcer.getAttribute('role')).toBe('status')
      expect(announcer.getAttribute('aria-live')).toBe('polite')
      expect(announcer.getAttribute('aria-atomic')).toBe('true')
      expect(surface.hasAttribute('aria-live')).toBe(false)
      expect(surface.style.right).toBe('52px')
      expect(surface.style.bottom).toBe('0px')
      expect(surface.getBoundingClientRect().bottom).toBe(window.innerHeight)
      expect(surface.style.left).toBe('')
      expect(surface.getBoundingClientRect().right).toBeLessThan(bell().getBoundingClientRect().left)
      expect(surface.getAttribute('aria-hidden')).toBeNull()

      const review = Array.from(surface.querySelectorAll('button')).find((button) => button.textContent === 'Review updates')!
      act(() => review.click())
      settle(200)
      expect(host.querySelector('[data-testid="updates-panel"]')).toBeTruthy()
      expect(alertSurface()).toBeNull()

      // The update notice outlives the transient alert in the notifications panel.
      act(() => bell().click())
      expect(host.querySelector('[data-testid="notifications-panel"]')?.textContent).toContain('2 updates available')
    } finally {
      cleanup()
      vi.useRealTimers()
    }
  })

  it('holds expiry while hovered or focused and keeps focus on the bell after explicit dismissal', async () => {
    vi.useFakeTimers()
    useAppStore.setState({ updateChecksEnabled: false })
    const { host, alertSurface, bell, settle, cleanup } = await mountShell()
    try {
      act(() => reportAppFailure('Could not save.', 'Disk full'))
      act(() => { alertSurface()!.dispatchEvent(new MouseEvent('mouseover', { bubbles: true })) })
      settle(20000)
      expect(alertSurface()).toBeTruthy()
      act(() => { alertSurface()!.dispatchEvent(new MouseEvent('mouseout', { bubbles: true })) })
      settle(8000)
      settle(200)
      expect(alertSurface()).toBeNull()

      act(() => reportAppFailure('Could not rename.', 'Name in use'))
      const action = Array.from(alertSurface()!.querySelectorAll('button')).find((button) => button.textContent === 'View in notifications')!
      act(() => action.focus())
      settle(20000)
      expect(alertSurface()).toBeTruthy()
      expect(document.activeElement).toBe(action)

      const dismiss = alertSurface()!.querySelector<HTMLButtonElement>('[aria-label="Dismiss alert"]')!
      act(() => dismiss.focus())
      act(() => dismiss.click())
      settle(200)
      expect(alertSurface()).toBeNull()
      expect(document.activeElement).toBe(bell())

      // Neither the expired nor the dismissed alert is lost from the panel.
      expect(bell().getAttribute('aria-label')).toBe('2 alerts')
      act(() => bell().click())
      const panel = host.querySelector('[data-testid="notifications-panel"]')!
      expect(panel.querySelectorAll('time')).toHaveLength(2)
      expect(panel.textContent).toContain('Disk full')
      expect(panel.textContent).toContain('Name in use')
    } finally {
      cleanup()
      vi.useRealTimers()
    }
  })

  // David explicitly superseded composer-clearance on 2026-10-21: bottom edge, allow covering Send.
  // Prior R3 collision/R4 clearance receipts and the original test preimage remain immutable evidence.
  it('stays at the GUI bottom edge as composers grow and permits covering Send while keeping the rail alignment', async () => {
    vi.useFakeTimers()
    Object.defineProperty(window, 'innerWidth', { configurable: true, value: 1280 })
    Object.defineProperty(window, 'innerHeight', { configurable: true, value: 800 })
    useAppStore.setState({ updateChecksEnabled: false })
    const fixture = await mountShell()
    const { host, alertSurface, layoutRects, notifyResize, cleanup } = fixture
    const dock = document.createElement('div'); dock.className = 'uam-composer-dock'
    const other = document.createElement('div'); other.className = 'uam-composer-dock'
    const left = document.createElement('div'); left.className = 'uam-composer-dock'
    host.querySelector('.uam-app')!.append(dock, other, left)
    layoutRects.set(dock, new DOMRect(44, 612.85, 1192, 187.15))
    layoutRects.set(other, new DOMRect(900, 500, 336, 300))
    layoutRects.set(left, new DOMRect(44, 100, 400, 700))
    try {
      act(() => reportAppFailure('A private layout failure', 'Retained detail'))
      const surface = alertSurface()!
      const send = new DOMRect(1068, 750, 32, 32) // Prior collision is now intentionally permitted.
      const rect = surface.getBoundingClientRect()
      expect(surface.getAttribute('aria-hidden')).toBeNull()
      expect(surface.style.bottom).toBe('0px')
      expect(rect.bottom).toBe(800)
      expect(rect.right).toBe(1228)
      expect(rect.width).toBe(360)
      expect(Math.min(rect.right, send.right) - Math.max(rect.left, send.left)).toBeGreaterThan(0)
      expect(Math.min(rect.bottom, send.bottom) - Math.max(rect.top, send.top)).toBeGreaterThan(0)
      expect(fixture.observed.has(dock)).toBe(false)
      layoutRects.set(other, new DOMRect(900, 350, 336, 450))
      layoutRects.set(dock, new DOMRect(44, 492.85, 1192, 307.15))
      notifyResize()
      expect(surface.getBoundingClientRect()).toEqual(rect)
      // Horizontal placement still follows the real rail at a narrower viewport.
      Object.defineProperty(window, 'innerWidth', { configurable: true, value: 480 })
      notifyResize()
      expect(surface.getBoundingClientRect().right).toBe(428)
      expect(surface.getBoundingClientRect().width).toBe(360)
      expect(surface.getBoundingClientRect().bottom).toBe(800)
    } finally {
      cleanup()
      expect(fixture.activeObserverCount()).toBe(0)
      vi.useRealTimers()
    }
  })

  it('keeps bottom and horizontal position fixed throughout first-send form FLIP transitions', async () => {
    vi.useFakeTimers()
    Object.defineProperty(window, 'innerWidth', { configurable: true, value: 1280 })
    Object.defineProperty(window, 'innerHeight', { configurable: true, value: 800 })
    useAppStore.setState({ updateChecksEnabled: false })
    const { host, alertSurface, layoutRects, notifyResize, cleanup } = await mountShell()
    const dock = document.createElement('div'); dock.className = 'uam-composer-dock'
    const form = document.createElement('form'); dock.append(form)
    host.querySelector('.uam-app')!.append(dock)
    layoutRects.set(dock, new DOMRect(44, 612.85, 1192, 187.15))
    layoutRects.set(form, new DOMRect(172, 703, 936, 85))
    const transition = (type: string) => {
      const event = new Event(type, { bubbles: true })
      Object.defineProperty(event, 'propertyName', { value: 'transform' })
      act(() => form.dispatchEvent(event))
    }
    try {
      act(() => reportAppFailure('Notice during the first turn', 'Retained detail'))
      const surface = alertSurface()!
      const initial = surface.getBoundingClientRect()
      expect(initial.bottom).toBe(800)
      expect(initial.right).toBe(1228)
      for (const top of [250, 500, 703]) {
        layoutRects.set(form, new DOMRect(172, top, 936, 85))
        transition(top === 250 ? 'transitionrun' : top === 703 ? 'transitionend' : 'transitioncancel')
        notifyResize()
        expect(surface.style.bottom).toBe('0px')
        expect(surface.getBoundingClientRect()).toEqual(initial)
      }
    } finally { cleanup(); vi.useRealTimers() }
  })

  it('bounds bottom-edge long messages and uses the retained bell when the viewport cannot fit accessible actions', async () => {
    vi.useFakeTimers()
    Object.defineProperty(window, 'innerWidth', { configurable: true, value: 720 })
    Object.defineProperty(window, 'innerHeight', { configurable: true, value: 300 })
    useAppStore.setState({ updateChecksEnabled: false })
    const { host, alertSurface, bell, layoutRects, notifyResize, settle, cleanup } = await mountShell()
    const rail = host.querySelector('.uam-side-rail--right')!
    const dock = document.createElement('div'); dock.className = 'uam-composer-dock'
    const header = document.createElement('div'); header.className = 'uam-chat-pane__header'
    host.querySelector('.uam-app')!.append(dock, header)
    layoutRects.set(rail, new DOMRect(676, 44, 44, 256))
    layoutRects.set(bell(), new DOMRect(683, 222, 30, 30))
    layoutRects.set(dock, new DOMRect(44, 180, 632, 120))
    layoutRects.set(header, new DOMRect(44, 44, 632, 44))
    try {
      const message = 'Long notification '.repeat(100)
      act(() => reportAppFailure(message, 'Long notification detail retained'))
      const surface = alertSurface()!
      const text = surface.querySelector<HTMLElement>('[aria-label="Notification message"]')!
      expect(surface.getAttribute('aria-hidden')).toBeNull()
      expect(surface.style.bottom).toBe('0px')
      expect(surface.getBoundingClientRect().bottom).toBe(300)
      expect(surface.getBoundingClientRect().top).toBeGreaterThanOrEqual(96)
      expect(Number.parseFloat(text.style.maxHeight)).toBe(194)
      expect(text.style.overflowY).toBe('auto')
      expect(text.tabIndex).toBe(0)
      expect(text.textContent).toBe(message)
      expect([...surface.querySelectorAll('button')].every(button => !button.disabled)).toBe(true)
      act(() => text.focus())
      settle(9000)
      expect(document.activeElement).toBe(text)
      expect(alertSurface()).toBe(surface)
      // Composer growth cannot hide or lift a bottom-edge notification.
      layoutRects.set(dock, new DOMRect(44, 110, 632, 190))
      notifyResize()
      expect(surface.getAttribute('aria-hidden')).toBeNull()
      expect(surface.getBoundingClientRect().bottom).toBe(300)
      expect(document.activeElement).toBe(text)
      // An actually short viewport leaves insufficient room below the header for accessible actions.
      Object.defineProperty(window, 'innerHeight', { configurable: true, value: 120 })
      layoutRects.set(rail, new DOMRect(676, 44, 44, 76))
      layoutRects.set(bell(), new DOMRect(683, 82, 30, 30))
      notifyResize()
      expect(surface.getAttribute('aria-hidden')).toBe('true')
      expect(surface.style.visibility).toBe('hidden')
      expect(document.activeElement).toBe(bell())
      const description = document.getElementById(bell().getAttribute('aria-describedby')!)!
      expect(description.getAttribute('aria-live')).toBe('polite')
      expect(description.textContent).toContain('Saved in Notifications')
      expect(description.textContent).toContain(message)
      expect(bell().getAttribute('aria-label')).toBe('1 alert')
      // A narrow viewport cannot squeeze both actions and a readable message into the available width.
      Object.defineProperty(window, 'innerHeight', { configurable: true, value: 300 })
      for (const width of [320, 240]) {
        Object.defineProperty(window, 'innerWidth', { configurable: true, value: width })
        layoutRects.set(rail, new DOMRect(width - 44, 44, 44, 256))
        layoutRects.set(bell(), new DOMRect(width - 37, 222, 30, 30))
        notifyResize()
        expect(surface.getAttribute('aria-hidden')).toBe('true')
        expect(surface.style.bottom).toBe('0px')
        expect(bell().getAttribute('aria-label')).toBe('1 alert')
      }
      Object.defineProperty(window, 'innerWidth', { configurable: true, value: 720 })
      layoutRects.set(rail, new DOMRect(676, 44, 44, 256))
      layoutRects.set(bell(), new DOMRect(683, 222, 30, 30))
      notifyResize()
      expect(surface.getAttribute('aria-hidden')).toBeNull()
      expect(surface.getBoundingClientRect().bottom).toBe(300)
      expect(bell().hasAttribute('aria-describedby')).toBe(false)
      act(() => bell().click()); settle(200)
      expect(alertSurface()).toBeNull()
      expect(host.querySelector('[data-testid="notifications-panel"]')?.textContent).toContain('Long notification detail retained')
    } finally { cleanup(); vi.useRealTimers() }
  })

  it('never shows the transient alert over an open right-side panel and keeps the message in the bell', async () => {
    vi.useFakeTimers()
    useAppStore.setState({ updateChecksEnabled: false })
    const { host, alertSurface, bell, settle, cleanup } = await mountShell()
    try {
      act(() => reportAppFailure('Could not save.', 'Disk full'))
      expect(alertSurface()).toBeTruthy()
      act(() => bell().click())
      settle(200)
      expect(alertSurface()).toBeNull()

      act(() => reportAppFailure('Could not rename.', 'Name in use'))
      settle(200)
      expect(alertSurface()).toBeNull()
      const panel = host.querySelector('[data-testid="notifications-panel"]')!
      expect(panel.querySelectorAll('time')).toHaveLength(2)
      expect(panel.textContent).toContain('Name in use')

      act(() => (host.querySelector('button[aria-label="Check for updates"]') as HTMLButtonElement).click())
      act(() => reportAppFailure('Could not move.', 'Transport closed'))
      settle(200)
      expect(host.querySelector('[data-testid="updates-panel"]')).toBeTruthy()
      expect(alertSurface()).toBeNull()

      // The suppressed failure is not erased: it is an unread bell entry and appears in the panel.
      expect(bell().getAttribute('aria-label')).toBe('3 alerts')
      act(() => bell().click())
      expect(host.querySelector('[data-testid="notifications-panel"]')?.textContent).toContain('Transport closed')
      act(() => bell().click())
      settle(200)

      // The commit panel is also a right-side panel the alert must not cover.
      act(() => useAppStore.setState({ commitPanelOpen: true }))
      act(() => reportAppFailure('Could not copy.', 'Clipboard denied'))
      settle(200)
      expect(alertSurface()).toBeNull()
      expect(bell().getAttribute('aria-label')).toBe('4 alerts')
      act(() => useAppStore.setState({ commitPanelOpen: false }))
      settle(200)
      expect(alertSurface()).toBeNull()
    } finally {
      cleanup()
      vi.useRealTimers()
    }
  })

  it.each(['notifications', 'updates', 'commit'] as const)('announces each retained failure once while the %s panel suppresses the popup', async (panelKind) => {
    vi.useFakeTimers()
    useAppStore.setState({ updateChecksEnabled: false })
    const { host, alertSurface, bell, settle, cleanup } = await mountShell()
    let observer: MutationObserver | undefined
    try {
      const announcer = host.querySelector<HTMLElement>('[data-testid="transient-alert-announcer"]')!
      expect(announcer).toBeTruthy()
      expect(announcer.textContent).toBe('')
      expect(announcer.getAttribute('role')).toBe('status')
      expect(announcer.getAttribute('aria-live')).toBe('polite')
      expect(announcer.getAttribute('aria-atomic')).toBe('true')
      const announcementUpdates: string[] = []
      observer = new MutationObserver(() => announcementUpdates.push(announcer.textContent ?? ''))
      observer.observe(announcer, { childList: true, characterData: true, subtree: true })
      const panelToggle = panelKind === 'notifications' ? bell() : host.querySelector<HTMLButtonElement>(
        panelKind === 'updates' ? '[aria-label="Check for updates"]' : '[aria-label="Open Git/SVN commit panel"]',
      )!
      expect(panelToggle).toBeTruthy()
      await act(async () => { panelToggle.click(); await Promise.resolve() })
      settle(200)
      await act(async () => { await Promise.resolve() })
      expect(host.querySelector(`[data-testid="${panelKind}-panel"]`)?.getAttribute('data-state')).toBe('open')
      expect(announcementUpdates).toEqual([])
      const failures = [1, 2].map(index => ({ message: `${panelKind} panel failure ${index}.`, detail: `Retained ${panelKind} detail ${index}.` }))
      const expectedAnnouncements: string[] = []
      for (const [index, failure] of failures.entries()) {
        await act(async () => { reportAppFailure(failure.message, failure.detail); await Promise.resolve() })
        settle(200)
        await act(async () => { await Promise.resolve() })
        expect(alertSurface()).toBeNull()
        expect(host.querySelector(`[data-testid="${panelKind}-panel"]`)?.getAttribute('data-state')).toBe('open')
        expect(bell().getAttribute('aria-label')).toBe(`${index + 1} alert${index === 0 ? '' : 's'}`)
        if (panelKind === 'notifications') {
          const panel = host.querySelector('[data-testid="notifications-panel"]')!
          expect(panel.querySelectorAll('time')).toHaveLength(index + 1)
          expect(panel.textContent).toContain(failure.message)
          expect(panel.textContent).toContain(failure.detail)
        }
        expect(host.querySelector('[data-testid="transient-alert-announcer"]')).toBe(announcer)
        expect(host.querySelectorAll('[data-testid="transient-alert-announcer"]')).toHaveLength(1)
        expect(announcer.closest('[aria-hidden="true"]')).toBeNull()
        const expectedAnnouncement = `${failure.message} Saved in Notifications. Use the Notifications button to read it.`
        expectedAnnouncements.push(expectedAnnouncement)
        expect(announcer.textContent).toBe(expectedAnnouncement)
        expect(announcementUpdates).toEqual(expectedAnnouncements)
      }
      const lastAnnouncement = announcer.firstChild
      await act(async () => { panelToggle.click(); await Promise.resolve() })
      settle(200)
      await act(async () => { await Promise.resolve() })
      expect(host.querySelector('.uam-app')?.getAttribute('data-right-panel-open')).toBe('false')
      expect(alertSurface()).toBeNull()
      expect(host.querySelector('[data-testid="transient-alert-announcer"]')).toBe(announcer)
      expect(announcer.firstChild).toBe(lastAnnouncement)
      expect(announcementUpdates).toEqual(expectedAnnouncements)
      expect(bell().getAttribute('aria-label')).toBe('2 alerts')
      await act(async () => { bell().click(); await Promise.resolve() })
      settle(200)
      await act(async () => { await Promise.resolve() })
      const retainedPanel = host.querySelector('[data-testid="notifications-panel"]')!
      expect(retainedPanel.querySelectorAll('time')).toHaveLength(2)
      for (const failure of failures) {
        expect(retainedPanel.textContent).toContain(failure.message)
        expect(retainedPanel.textContent).toContain(failure.detail)
      }
      expect(alertSurface()).toBeNull()
      expect(announcer.firstChild).toBe(lastAnnouncement)
      expect(announcementUpdates).toEqual(expectedAnnouncements)
    } finally {
      observer?.disconnect()
      cleanup()
      vi.useRealTimers()
    }
  })

  it('navigates to attention chats while leaving modal and consumed key events owned', async () => {
    const select = vi.fn()
    const previousSelect = useAppStore.getState().setActiveSession
    useAppStore.setState({
      sessions: ['busy', 'attention'].map((id) => ({ id, name: id, folderId: null, viewMode: 'chat' as const, createdAt: new Date(), updatedAt: new Date() })),
      activeSessionId: 'busy',
      acpBindingBySessionId: { busy: { processing: true }, attention: { attentionKind: 'permission' } },
      setActiveSession: select,
    })
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    try {
      await act(async () => root.render(<AppShell />))
      const dispatch = () => window.dispatchEvent(new KeyboardEvent('keydown', { key: 'J', ctrlKey: true, shiftKey: true, bubbles: true, cancelable: true }))
      act(dispatch)
      expect(select).toHaveBeenCalledExactlyOnceWith('attention')
      select.mockClear()
      const modal = document.createElement('div')
      modal.setAttribute('aria-modal', 'true')
      document.body.appendChild(modal)
      act(dispatch)
      expect(select).not.toHaveBeenCalled()
      modal.remove()
      const consumed = new KeyboardEvent('keydown', { key: 'J', ctrlKey: true, shiftKey: true, cancelable: true })
      consumed.preventDefault()
      act(() => window.dispatchEvent(consumed))
      expect(select).not.toHaveBeenCalled()
    } finally {
      act(() => root.unmount())
      host.remove()
      useAppStore.setState({ setActiveSession: previousSelect })
    }
  })

  it('focuses the active pane input only for its exact shortcut and respects modal ownership', async () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    try {
      await act(async () => root.render(<AppShell />))
      const panes = document.createElement('div')
      panes.innerHTML = '<div class="uam-chat-pane" data-focused="false"><textarea class="uam-composer-textarea">Other</textarea></div><div class="uam-chat-pane" data-focused="true"><textarea class="uam-composer-textarea">Draft</textarea></div>'
      host.appendChild(panes)
      const input = panes.querySelectorAll('textarea')[1]
      const key = () => new KeyboardEvent('keydown', { key: 'L', ctrlKey: true, shiftKey: true, bubbles: true, cancelable: true })
      const event = key()
      act(() => host.dispatchEvent(event))
      expect(document.activeElement).toBe(input)
      expect(event.defaultPrevented).toBe(true)
      const modal = document.createElement('input')
      modal.setAttribute('aria-modal', 'true'); host.appendChild(modal); modal.focus()
      const blocked = key()
      act(() => modal.dispatchEvent(blocked))
      expect(document.activeElement).toBe(modal)
      expect(blocked.defaultPrevented).toBe(false)
      expect(input.value).toBe('Draft')
    } finally { act(() => root.unmount()); host.remove() }
  })

})
