import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { SettingsModal } from './SettingsModal'
import { useAppStore } from '../../store/useAppStore'
import { fallbackProviderForId } from '../../utils/providerMetadata'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

describe('SettingsModal memory settings', () => {
  beforeEach(() => {
    vi.restoreAllMocks()
    window.cefQuery = undefined
    useAppStore.setState({
	  folders: [{ id: 'project', name: 'Project', parentId: null, directory: '/tmp/project', isExpanded: true, createdAt: new Date() }],
	  providerModelCatalogs: [],
      providers: [
        {
          id: 'gemini-cli',
          name: 'Gemini CLI',
          shortName: 'Gemini',
          color: '#8ab4ff',
          description: '',
          outputMode: 'cli',
          supportsCli: true,
          supportsStructured: true,
          structuredProtocol: 'gemini-acp',
        },
        {
          id: 'codex-cli',
          name: 'Codex CLI',
          shortName: 'Codex',
          color: '#22c55e',
          description: '',
          outputMode: 'cli',
          supportsCli: true,
          supportsStructured: true,
          structuredProtocol: 'codex-app-server',
        },
      ],
      sessions: [{ id: 'chat-1', name: 'Chat', viewMode: 'chat', folderId: 'project', workspaceDirectory: '/tmp/project', createdAt: new Date(), updatedAt: new Date() }],
      activeSessionId: 'chat-1',
      favoriteUamAgentIds: ['writer', 'reviewer'],
      uamAgentCycleShortcut: 'shift+tab',
      uamAgentsBySessionId: {
        'chat-1': [
          { id: 'build', description: 'Build', builtIn: true },
          { id: 'plan', description: 'Plan', builtIn: true },
          { id: 'reviewer', description: 'Review', builtIn: false },
          { id: 'writer', description: 'Write', builtIn: false },
        ],
      },
      memoryEnabledDefault: true,
      memoryLevelDefault: 'strict',
      memoryIdleDelaySeconds: 120,
      memoryRecallBudgetBytes: 4096,
      goalMaxLoopIterations: 200,
      defaultNewChatProviderId: 'codex-cli',
      providerChatDefaults: {},
      theme: 'dark',
      customThemes: [],
      workingDisplayMode: 'verbose',
      showProviderIconsInSidebar: true,
      showWorktreePathInSidebar: true,
      memoryWorkerBindings: {
        'gemini-cli': { workerProviderId: 'gemini-cli', workerModelId: '' },
      },
      cliVersionManager: {
        providers: [
          {
            providerId: 'gemini-cli',
            installedVersion: '0.38.1',
            selectedVersion: '0.38.1',
            availableVersions: [
              { version: '0.38.1', preferred: true },
              { version: '0.36.0', preferred: false },
            ],
            preferredVersion: '0.38.1',
            status: 'verified',
            message: 'Gemini CLI version is supported.',
            running: false,
            lastCommand: '',
            lastOutput: '',
          },
          {
            providerId: 'codex-cli',
            installedVersion: '0.123.0',
            selectedVersion: '0.123.0',
            availableVersions: [
              { version: '0.124.0', preferred: true },
              { version: '0.123.0', preferred: false },
            ],
            preferredVersion: '0.124.0',
            status: 'verified',
            message: 'Codex CLI version is supported.',
            running: false,
            lastCommand: '',
            lastOutput: '',
          },
        ],
      },
      memoryLastStatus: '',
      defaultEditorPresetId: 'vscode',
      editorFileAssociations: [
        {
          id: 'cpp',
          name: 'C++',
          extensions: ['.c', '.cc', '.cpp', '.cxx', '.h', '.hh', '.hpp', '.hxx'],
          editorPresetId: 'vscode',
        },
      ],
      mcpServers: [],
      executionHosts: [{ id: 'local', label: 'This computer', transport: 'local', sshAlias: '', runnerStatus: 'ready', runnerVersion: '', platform: '', architecture: '', lastSeenAt: '' }],
      markdownStoreDirectory: '/tmp/markdown-store',
      markdownStoreError: '',
      isMarkdownStoreOpen: false,
      setSettingsOpen: vi.fn(),
      setMemorySettings: vi.fn(() => Promise.resolve(true)),
      setProviderChatDefaults: vi.fn(() => Promise.resolve(true)),
      setEditorSettings: vi.fn(() => Promise.resolve(true)),
      setMcpServers: vi.fn(() => Promise.resolve({ ok: true })),
      setUamAgentPreferences: vi.fn(() => Promise.resolve(true)),
      refreshUamAgents: vi.fn(() => Promise.resolve(true)),
      browseProviderAgentImport: vi.fn(() => Promise.resolve(null)),
      previewProviderAgentImport: vi.fn(() => Promise.resolve(null)),
      importProviderAgent: vi.fn(() => Promise.resolve(true)),
      setTheme: vi.fn(),
      refreshCustomThemes: vi.fn(() => Promise.resolve(true)),
      saveCustomTheme: vi.fn((theme) => Promise.resolve(theme)),
      deleteCustomTheme: vi.fn(() => Promise.resolve(true)),
      setWorkingDisplayMode: vi.fn(),
      setSidebarSettings: vi.fn(() => Promise.resolve(true)),
      refreshCliProviderVersion: vi.fn(() => Promise.resolve(true)),
      applyCliProviderVersion: vi.fn(() => Promise.resolve(true)),
      openAllMemoryLibrary: vi.fn(async () => { useAppStore.setState({memoryLibraryScope:{scopeType:'all',folderId:'',label:'All Memory',rootPath:'',rootCount:1},memoryLibraryEntries:[],memoryLibraryLoading:false,memoryLibraryError:''}); return true }),
      closeMemoryLibrary: vi.fn(() => useAppStore.setState({memoryLibraryScope:null})),
      discoverProviderModels: vi.fn(async () => true),
      refreshMarkdownStore: vi.fn(async () => true),
      memoryLibraryScope: null,
      openGlobalMemoryLibrary: vi.fn(() => Promise.resolve(true)),
      openMemoryScanModal: vi.fn(() => Promise.resolve(true)),
    })
  })

  function renderModal() {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => {
      root.render(<SettingsModal />)
    })
    return { host, root }
  }

  function openEditorsSection(host: HTMLElement) {
    const editorsSectionButton = Array.from(host.querySelectorAll('button')).find(
      (button) => button.textContent?.includes('Editors')
    )
    expect(editorsSectionButton).toBeTruthy()

    act(() => {
      editorsSectionButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })
  }

  function openCliVersionSection(host: HTMLElement) {
    const cliSectionButton = Array.from(host.querySelectorAll('button')).find(
      (button) => button.textContent?.includes('CLI Version')
    )
    expect(cliSectionButton).toBeTruthy()

    act(() => {
      cliSectionButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })
  }

  function openRemoteHostsSection(host: HTMLElement) {
    const button = Array.from(host.querySelectorAll('button')).find(
      (candidate) => candidate.textContent?.includes('Remote Hosts')
    )
    expect(button).toBeTruthy()
    act(() => button?.click())
  }

  it('previews the exact SSH setup before explicitly installing the helper', async () => {
    const actions: string[] = []
    window.cefQuery = ({ request, onSuccess }) => {
      const parsed = JSON.parse(request) as { action: string; payload: { id: string; label: string; sshAlias: string } }
      actions.push(parsed.action)
      if (parsed.action === 'previewRemoteHost') {
        onSuccess(JSON.stringify({
          host: { id: parsed.payload.id, label: parsed.payload.label, transport: 'ssh', sshAlias: parsed.payload.sshAlias, runnerStatus: 'uninstalled', runnerVersion: '', platform: '', architecture: '', lastSeenAt: '' },
          preview: '1. Check remote platform\n2. Copy runner',
        }))
        return
      }
      onSuccess('{}')
    }
    const { host, root } = renderModal()
    openRemoteHostsSection(host)

    const label = host.querySelector('input[aria-label="Remote host display name"]') as HTMLInputElement
    const alias = host.querySelector('input[aria-label="SSH config alias"]') as HTMLInputElement
    await act(async () => {
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')?.set?.call(label, 'AI desktop')
      label.dispatchEvent(new Event('input', { bubbles: true }))
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')?.set?.call(alias, 'ai-desktop')
      alias.dispatchEvent(new Event('input', { bubbles: true }))
    })
    const preview = Array.from(host.querySelectorAll('button')).find((button) => button.textContent?.includes('Preview setup'))
    await act(async () => { preview?.click() })

    expect(actions).toEqual(['previewRemoteHost'])
    expect(host.textContent).toContain('Install helper on AI desktop?')
    expect(host.textContent).toContain('detects Ubuntu/Linux or Windows')
    expect(host.textContent).toContain('Unsupported systems stop before anything is copied')
    expect(host.textContent).toContain('Check remote platform')
    const install = Array.from(host.querySelectorAll('button')).find((button) => button.textContent?.includes('Connect and install'))
    await act(async () => { install?.click() })
    expect(actions).toEqual(['previewRemoteHost', 'installRemoteHost'])

    act(() => root.unmount())
    host.remove()
  })

  it('dismisses the setup preview so a failed compatibility check is visible', async () => {
    window.cefQuery = ({ request, onSuccess, onFailure }) => {
      const parsed = JSON.parse(request) as { action: string; payload: { id: string; label: string; sshAlias: string } }
      if (parsed.action === 'previewRemoteHost') {
        onSuccess(JSON.stringify({
          host: { id: parsed.payload.id, label: parsed.payload.label, transport: 'ssh', sshAlias: parsed.payload.sshAlias, runnerStatus: 'uninstalled', runnerVersion: '', platform: '', architecture: '', lastSeenAt: '' },
          preview: '1. Check remote platform',
        }))
        return
      }
      onFailure(500, 'This UAM build does not contain a runner for linux/s390x.')
    }
    const { host, root } = renderModal()
    openRemoteHostsSection(host)
    const alias = host.querySelector('input[aria-label="SSH config alias"]') as HTMLInputElement
    await act(async () => {
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')?.set?.call(alias, 'colima')
      alias.dispatchEvent(new Event('input', { bubbles: true }))
    })
    await act(async () => {
      Array.from(host.querySelectorAll('button')).find((button) => button.textContent?.includes('Preview setup'))?.click()
    })
    await act(async () => {
      Array.from(host.querySelectorAll('button')).find((button) => button.textContent?.includes('Connect and install'))?.click()
    })

    expect(host.textContent).not.toContain('Install helper on colima?')
    expect(host.textContent).toContain('This UAM build does not contain a runner for linux/s390x.')

    act(() => root.unmount())
    host.remove()
  })

  it('installs into a validated custom folder selected in the setup modal', async () => {
    let installPayload: Record<string, unknown> | null = null
    window.cefQuery = ({ request, onSuccess }) => {
      const parsed = JSON.parse(request) as { action: string; payload: Record<string, unknown> }
      if (parsed.action === 'previewRemoteHost') {
        onSuccess(JSON.stringify({
          host: { ...parsed.payload, transport: 'ssh', runnerStatus: 'uninstalled', runnerVersion: '', platform: '', architecture: '', lastSeenAt: '' },
          preview: 'Install UAM runner 4.5.7 at the recommended location\n1. Check remote platform',
        }))
        return
      }
      installPayload = parsed.payload
      onSuccess('{}')
    }
    const { host, root } = renderModal()
    openRemoteHostsSection(host)
    const alias = host.querySelector('input[aria-label="SSH config alias"]') as HTMLInputElement
    await act(async () => {
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')?.set?.call(alias, 'ai-desktop')
      alias.dispatchEvent(new Event('input', { bubbles: true }))
      Array.from(host.querySelectorAll('button')).find((button) => button.textContent?.includes('Preview setup'))?.click()
    })
    const customChoice = host.querySelectorAll<HTMLInputElement>('input[name="remote-helper-location"]')[1]
    act(() => customChoice.click())
    const directory = host.querySelector('input[aria-label="Remote helper folder"]') as HTMLInputElement
    act(() => {
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')?.set?.call(directory, 'helpers/uam')
      directory.dispatchEvent(new Event('input', { bubbles: true }))
    })
    expect(host.textContent).toContain('~/helpers/uam/4.9.0-alpha-11')
    await act(async () => {
      Array.from(host.querySelectorAll('button')).find((button) => button.textContent?.includes('Connect and install'))?.click()
    })
    expect(installPayload).toMatchObject({ runnerDirectory: 'helpers/uam' })

    act(() => root.unmount())
    host.remove()
  })

  it('blocks a custom helper folder that escapes the remote home directory', async () => {
    const actions: string[] = []
    window.cefQuery = ({ request, onSuccess }) => {
      const parsed = JSON.parse(request) as { action: string; payload: Record<string, unknown> }
      actions.push(parsed.action)
      if (parsed.action === 'previewRemoteHost') {
        onSuccess(JSON.stringify({
          host: { ...parsed.payload, transport: 'ssh', runnerStatus: 'uninstalled', runnerVersion: '', platform: '', architecture: '', lastSeenAt: '' },
          preview: '1. Check remote platform',
        }))
        return
      }
      onSuccess('{}')
    }
    const { host, root } = renderModal()
    openRemoteHostsSection(host)
    const alias = host.querySelector('input[aria-label="SSH config alias"]') as HTMLInputElement
    await act(async () => {
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')?.set?.call(alias, 'ai-desktop')
      alias.dispatchEvent(new Event('input', { bubbles: true }))
      Array.from(host.querySelectorAll('button')).find((button) => button.textContent?.includes('Preview setup'))?.click()
    })
    act(() => host.querySelectorAll<HTMLInputElement>('input[name="remote-helper-location"]')[1].click())
    const directory = host.querySelector('input[aria-label="Remote helper folder"]') as HTMLInputElement
    act(() => {
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')?.set?.call(directory, '../outside')
      directory.dispatchEvent(new Event('input', { bubbles: true }))
    })
    act(() => {
      directory.select()
      directory.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })
    const install = Array.from(host.querySelectorAll('button')).find((button) => button.textContent?.includes('Connect and install')) as HTMLButtonElement
    expect(install.disabled).toBe(true)
		expect(directory.value).toBe('../outside')
    expect(directory.closest('label')).toBeNull()
    expect(directory.selectionStart).toBe(0)
    expect(directory.selectionEnd).toBe('../outside'.length)
		expect(host.textContent).toContain('Remove the .. segment; the helper must stay under the remote home directory.')
    expect(actions).toEqual(['previewRemoteHost'])

    act(() => root.unmount())
    host.remove()
  })

  it('does not claim a helper was installed when removing a failed remote host', async () => {
    useAppStore.setState({
      executionHosts: [
        { id: 'local', label: 'This computer', transport: 'local', sshAlias: '', runnerStatus: 'ready', runnerVersion: '', platform: '', architecture: '', lastSeenAt: '' },
        { id: 'ssh-colima', label: 'Colima', transport: 'ssh', sshAlias: 'colima', runnerStatus: 'error', runnerVersion: '', platform: '', architecture: '', lastSeenAt: '' },
      ],
    })
    window.cefQuery = ({ onSuccess }) => onSuccess('{}')
    const { host, root } = renderModal()
    openRemoteHostsSection(host)

    const remove = host.querySelector('button[aria-label="Remove Colima"]') as HTMLButtonElement
    await act(async () => { remove.click() })

    expect(host.textContent).toContain('Any helper files on that machine were left untouched.')
    expect(host.textContent).not.toContain('The remote helper was left installed.')

    act(() => root.unmount())
    host.remove()
  })

  function openMemorySettingsSection(host: HTMLElement) {
    const memorySectionButton = Array.from(host.querySelectorAll('button')).find(
      (button) => button.textContent?.includes('Memory Settings')
    )
    expect(memorySectionButton).toBeTruthy()

    act(() => {
      memorySectionButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })
  }

  function openChatDataSection(host: HTMLElement) {
    const sectionButton = Array.from(host.querySelectorAll('button')).find(
      (button) => button.textContent?.includes('Chat Data')
    )
    expect(sectionButton).toBeTruthy()
    act(() => sectionButton?.dispatchEvent(new MouseEvent('click', { bubbles: true })))
  }

  function openMcpServersSection(host: HTMLElement) {
    const sectionButton = Array.from(host.querySelectorAll('button')).find(
      (button) => button.textContent?.includes('MCP Servers')
    )
    expect(sectionButton).toBeTruthy()
    act(() => sectionButton?.dispatchEvent(new MouseEvent('click', { bubbles: true })))
  }

  function openAgentsSection(host: HTMLElement) {
    const sectionButton = Array.from(host.querySelectorAll('button')).find(
      (button) => button.textContent?.includes('Agents')
    )
    expect(sectionButton).toBeTruthy()
    act(() => sectionButton?.dispatchEvent(new MouseEvent('click', { bubbles: true })))
  }

  it('persists the shortcut and exact ordered agent favorites', () => {
	const { host, root } = renderModal()
	openAgentsSection(host)
	expect(host.querySelector('[aria-label="Source provider"]')).toBeTruthy()

    const moveReviewerUp = host.querySelector('button[aria-label="Move reviewer up"]') as HTMLButtonElement
    act(() => moveReviewerUp.click())
    expect(useAppStore.getState().setUamAgentPreferences).toHaveBeenCalledWith({
      favoriteUamAgentIds: ['reviewer', 'writer'],
      uamAgentCycleShortcut: 'shift+tab',
    })

    const shortcut = host.querySelector('button[aria-label="Agent cycle shortcut"]') as HTMLButtonElement
    act(() => shortcut.click())
    const controlShortcut = Array.from(document.body.querySelectorAll('[role="option"]'))
      .find((option) => option.textContent?.includes('Ctrl+Shift+Tab')) as HTMLButtonElement
    act(() => controlShortcut.click())
    expect(useAppStore.getState().setUamAgentPreferences).toHaveBeenCalledWith({
      favoriteUamAgentIds: ['writer', 'reviewer'],
      uamAgentCycleShortcut: 'control+shift+tab',
    })

    act(() => root.unmount())
    host.remove()
  })

  it('requires preview and acknowledgement before importing provider-only fields', async () => {
    const previewProviderAgentImport = vi.fn(() => Promise.resolve({
      providerId: 'opencode-cli',
      sourcePath: '/tmp/reviewer.md',
      suggestedId: 'reviewer-native',
      description: 'Reviews changes',
      mode: 'subagent',
      securityFields: [],
      ignoredFields: ['model'],
      error: '',
      supported: true,
    }))
    const importProviderAgent = vi.fn(() => Promise.resolve(true))
    useAppStore.setState({ previewProviderAgentImport, importProviderAgent })
    const { host, root } = renderModal()
    openAgentsSection(host)

    const path = host.querySelector('input[aria-label="Native agent Markdown file"]') as HTMLInputElement
    await act(async () => {
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')?.set?.call(path, '/tmp/reviewer.md')
      path.dispatchEvent(new Event('input', { bubbles: true }))
    })
    const preview = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Preview') as HTMLButtonElement
    await act(async () => preview.click())

    expect(previewProviderAgentImport).toHaveBeenCalledWith('opencode-cli', '/tmp/reviewer.md')
    expect(host.textContent).toContain('Provider-only fields that will be omitted: model')
    const importButton = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Import agent') as HTMLButtonElement
    expect(importButton.disabled).toBe(true)

    const acknowledge = host.querySelector('input[aria-label="Acknowledge omitted provider fields"]') as HTMLInputElement
    act(() => acknowledge.click())
    expect(importButton.disabled).toBe(false)
    await act(async () => importButton.click())
    expect(importProviderAgent).toHaveBeenCalledWith(expect.objectContaining({
      chatId: 'chat-1',
      sourcePath: '/tmp/reviewer.md',
      canonicalId: 'reviewer-native',
      workspaceAccess: 'read',
      workspaceScope: true,
      acknowledgeIgnoredFields: true,
    }))

    act(() => root.unmount())
    host.remove()
  })

  it('rejects malformed MCP JSON locally and saves environment references', async () => {
    const { host, root } = renderModal()
    openMcpServersSection(host)
    act(() => host.querySelector('details > summary')?.dispatchEvent(new MouseEvent('click', {bubbles:true})))
    const editor = host.querySelector('textarea[aria-label="MCP server configuration"]') as HTMLTextAreaElement
    const save = host.querySelector('button[aria-label="Save MCP server configuration"]') as HTMLButtonElement

    act(() => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set?.call(editor, '{')
      editor.dispatchEvent(new Event('input', { bubbles: true }))
      save.click()
    })
    expect(host.querySelector('[role="status"]')?.textContent).toContain('valid JSON')
    expect(useAppStore.getState().setMcpServers).not.toHaveBeenCalled()

    const servers = [{
      id: 'computer-use', name: 'Computer Use', workspaceDirectory: '/tmp/project', transport: 'http',
      command: '', args: [], url: 'http://127.0.0.1:43123/mcp', environment: [],
      headers: [{ name: 'Authorization', environmentVariable: 'COMPUTER_USE_AUTH' }], enabled: true,
    }]
    act(() => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set?.call(editor, JSON.stringify(servers))
      editor.dispatchEvent(new Event('input', { bubbles: true }))
    })
    await act(async () => { save.click(); await Promise.resolve() })
    expect(useAppStore.getState().setMcpServers).toHaveBeenCalledWith(servers)
    expect(host.querySelector('[role="status"]')?.textContent).toContain('saved')

    act(() => root.unmount())
    host.remove()
  })

  it('keeps rejected MCP edits and shows the backend validation error', async () => {
    const setMcpServers = vi.fn(() => Promise.resolve({ ok: false, error: "MCP server 'Computer Use' needs an absolute executable path." }))
    useAppStore.setState({ setMcpServers })
    const { host, root } = renderModal()
    openMcpServersSection(host)
    act(() => host.querySelector('details > summary')?.dispatchEvent(new MouseEvent('click', {bubbles:true})))
    const editor = host.querySelector('textarea[aria-label="MCP server configuration"]') as HTMLTextAreaElement
    const save = host.querySelector('button[aria-label="Save MCP server configuration"]') as HTMLButtonElement
    const attempted = [{
      id: 'computer-use', name: 'Computer Use', workspaceDirectory: '/tmp/project', transport: 'stdio',
      command: 'npx', args: ['@playwright/mcp@latest'], url: '', environment: [], headers: [], enabled: true,
    }]

    act(() => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set?.call(editor, JSON.stringify(attempted))
      editor.dispatchEvent(new Event('input', { bubbles: true }))
    })
    await act(async () => { save.click(); await Promise.resolve() })
    act(() => useAppStore.setState({ mcpServers: [] }))

    expect(editor.value).toBe(JSON.stringify(attempted))
    expect(host.querySelector('[role="status"]')?.textContent).toBe("MCP server 'Computer Use' needs an absolute executable path.")

    act(() => root.unmount())
    host.remove()
  })

  it('adds isolated Playwright browser control without writing JSON', async () => {
    const { host, root } = renderModal()
    openMcpServersSection(host)
    act(() => host.querySelector<HTMLButtonElement>('[aria-label="Setup Gemini browser control"]')?.click())
    const executable = host.querySelector('input[aria-label="npx executable path"]') as HTMLInputElement
    act(() => {
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')?.set?.call(executable, '/opt/homebrew/bin/npx')
      executable.dispatchEvent(new Event('input', { bubbles: true }))
    })
    const add = host.querySelector('button[aria-label="Add Playwright browser control"]') as HTMLButtonElement
    await act(async () => { add.click(); await Promise.resolve() })

    expect(useAppStore.getState().setMcpServers).toHaveBeenCalledWith([expect.objectContaining({
      name: 'Playwright browser control',
      workspaceDirectory: '/tmp/project',
      command: '/opt/homebrew/bin/npx',
      args: ['-y', '@playwright/mcp@latest', '--isolated'],
    })])
    expect(host.querySelector('[role="status"]')?.textContent).toContain('Browser control configured')

    act(() => root.unmount())
    host.remove()
  })

  it('opens and switches sections without a forced animation or duplicate theme refresh', () => {
    const { host, root } = renderModal()
    const dialog = host.querySelector<HTMLElement>('[role="region"][aria-label="Settings"]')
    expect(dialog?.className).not.toContain('animate-slide-in')
    expect(dialog?.parentElement?.className).not.toContain('animate-fade-in')
    expect(useAppStore.getState().refreshCustomThemes).not.toHaveBeenCalled()

    openEditorsSection(host)
    expect(host.querySelector('.animate-fade-in')).toBeNull()

    act(() => root.unmount())
    host.remove()
  })

  function openMemoryStoreSection(host: HTMLElement) {
    const memoryStoreSectionButton = Array.from(host.querySelectorAll('button')).find(
      (button) => button.textContent?.includes('Memory Store')
    )
    expect(memoryStoreSectionButton).toBeTruthy()

    act(() => {
      memoryStoreSectionButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })
  }

  function openGoalLoopsSection(host: HTMLElement) {
    const button = Array.from(host.querySelectorAll('button')).find(
      (candidate) => candidate.textContent?.includes('Goal Loops')
    )
    expect(button).toBeTruthy()
    act(() => button?.dispatchEvent(new MouseEvent('click', { bubbles: true })))
  }

  function openMarkdownStoreSection(host: HTMLElement) {
    const button = Array.from(host.querySelectorAll('button')).find(
      (candidate) => candidate.textContent?.includes('Skills')
    )
    expect(button).toBeTruthy()
    act(() => button?.dispatchEvent(new MouseEvent('click', { bubbles: true })))
  }

  it('finds settings by control terms without changing pages and clears empty results', () => {
    const { host, root } = renderModal()
    const field = host.querySelector<HTMLInputElement>('[aria-label="Search settings"]')!
    const search = (value: string) => act(() => {
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(field, value)
      field.dispatchEvent(new Event('input', {bubbles:true}))
    })
    search('  ReCoMmEnDeD  ')
    expect(Array.from(host.querySelectorAll('nav button')).map(button => button.getAttribute('aria-label'))).toEqual(['CLI Version'])
    expect(host.querySelector('h2')?.textContent).toBe('Providers')
    expect(host.querySelector('nav [aria-pressed="true"]')).toBeNull()
    expect(host.querySelectorAll('h2')[1]?.textContent).toBe('Appearance')
    search('extensions')
    expect(Array.from(host.querySelectorAll('nav button')).map(button => button.getAttribute('aria-label'))).toEqual(['Editors'])
    search('no-such-setting')
    expect(host.textContent).toContain('No settings found.')
    act(() => host.querySelector<HTMLButtonElement>('[aria-label="Clear settings search"]')!.click())
    expect(host.querySelectorAll('nav button')).toHaveLength(15)
    act(() => root.unmount())
    host.remove()
  })

  it('offers Back to chats in the Settings page header', () => {
    const { host, root } = renderModal()
    const closeButton = host.querySelector('button[aria-label="Back to chats"]') as HTMLButtonElement

    expect(closeButton).toBeTruthy()
    expect(closeButton.textContent).toBe('Back to chats')
    expect(host.querySelector('[aria-modal="true"]')).toBeNull()
    expect(host.querySelector('h1')?.textContent).toBe('Settings')
    expect(Array.from(host.querySelectorAll('nav h2')).map(heading => heading.textContent)).toEqual(['General', 'Providers', 'Workspace', 'App'])
    const tab = new KeyboardEvent('keydown', {key: 'Tab', bubbles: true, cancelable: true})
    act(() => closeButton.dispatchEvent(tab))
    expect(tab.defaultPrevented).toBe(false)

    act(() => root.unmount())
    host.remove()
  })

  it('warns before discarding an unsaved MCP draft and restores focus on close', () => {
    const opener = document.createElement('button')
    document.body.appendChild(opener)
    opener.focus()
    const { host, root } = renderModal()
    expect(document.activeElement).toBe(host.querySelector('[role="region"][aria-label="Settings"]'))
    openMcpServersSection(host)
    act(() => host.querySelector('details > summary')?.dispatchEvent(new MouseEvent('click', {bubbles:true})))
    const editor = host.querySelector('textarea[aria-label="MCP server configuration"]') as HTMLTextAreaElement
    act(() => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value')?.set?.call(editor, '[{"name":"draft"}]')
      editor.dispatchEvent(new Event('input', { bubbles: true }))
      ;(host.querySelector('button[aria-label="Back to chats"]') as HTMLButtonElement).click()
    })
    expect(useAppStore.getState().setSettingsOpen).not.toHaveBeenCalled()
    expect(host.querySelector('[role="alertdialog"][aria-label="Discard unsaved MCP changes"]')).toBeTruthy()
    act(() => Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find((button) => button.textContent === 'Discard changes')?.click())
    expect(useAppStore.getState().setSettingsOpen).toHaveBeenCalledWith(false)

    act(() => root.unmount())
    expect(document.activeElement).toBe(opener)
    host.remove()
    opener.remove()
  })

  it('includes units on memory numeric settings', () => {
    const { host, root } = renderModal()
    openMemorySettingsSection(host)

    expect(host.textContent).toContain('Idle delay (seconds)')
    expect(host.textContent).toContain('Recall budget (bytes)')

    act(() => root.unmount())
    host.remove()
  })

  it('does not render native selects in settings', () => {
    const { host, root } = renderModal()

    expect(host.querySelectorAll('select')).toHaveLength(0)
    expect(host.textContent).toContain('Appearance')
    expect(host.textContent).toContain('CLI Version')
    expect(host.textContent).toContain('Memory Settings')
    expect(host.textContent).toContain('Memory Store')
    expect(host.textContent).toContain('About')
    expect(host.textContent).toContain('Theme')
    expect(host.textContent).not.toContain('Gemini memory worker')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('changes themes through the in-app menu', () => {
    const { host, root } = renderModal()
    const themeButton = host.querySelector('button[aria-label="Theme"]') as HTMLButtonElement | null

    act(() => themeButton?.click())
    expect(document.body.querySelector('[role="listbox"][aria-label="Theme"]')).toBeTruthy()

    const lightOption = Array.from(document.body.querySelectorAll('[role="option"]')).find((option) => option.textContent?.includes('Light')) as HTMLButtonElement | undefined
    act(() => lightOption?.click())

    expect(useAppStore.getState().setTheme).toHaveBeenCalledWith('light')
    expect(document.body.querySelector('[role="listbox"][aria-label="Theme"]')).toBeNull()

    act(() => root.unmount())
    host.remove()
  })

  it('toggles sidebar provider icons and worktree paths independently', () => {
    const { host, root } = renderModal()
    const providerIcons = host.querySelector<HTMLInputElement>('input[aria-label="Show provider icons in sidebar"]')
    const worktreePath = host.querySelector<HTMLInputElement>('input[aria-label="Show worktree path in sidebar"]')

    expect(providerIcons?.checked).toBe(true)
    expect(worktreePath?.checked).toBe(true)

    act(() => providerIcons?.click())
    expect(useAppStore.getState().setSidebarSettings).toHaveBeenCalledWith({
      showProviderIconsInSidebar: false,
      showWorktreePathInSidebar: true,
    })

    act(() => worktreePath?.click())
    expect(useAppStore.getState().setSidebarSettings).toHaveBeenCalledWith({
      showProviderIconsInSidebar: true,
      showWorktreePathInSidebar: false,
    })

    act(() => root.unmount())
    host.remove()
  })

  it('toggles compact working without changing sidebar settings', () => {
    const { host, root } = renderModal()
    const compactWorking = host.querySelector<HTMLInputElement>('input[aria-label="Compact working"]')

    expect(compactWorking?.checked).toBe(false)

    act(() => compactWorking?.click())
    expect(useAppStore.getState().setWorkingDisplayMode).toHaveBeenCalledWith('compact')

    act(() => root.unmount())
    host.remove()
  })

  it('updates the goal loop iteration cap', () => {
    const { host, root } = renderModal()

    openGoalLoopsSection(host)
    const decrease = host.querySelector('button[aria-label="Decrease maximum goal loop iterations"]') as HTMLButtonElement
    const output = host.querySelector('output[aria-label="Maximum goal loop iterations"]')
    const outputCeiling = host.querySelector<HTMLButtonElement>('button[aria-label="Provider turn output ceiling"]')
    expect(decrease).toBeTruthy()
    expect(output?.textContent).toBe('200')
    expect(outputCeiling?.textContent).toContain('1 GiB')
    expect(host.querySelector('select[aria-label="Provider turn output ceiling"]')).toBeNull()
    expect(host.querySelector('input[type="number"]')).toBeNull()

    act(() => {
      decrease.click()
    })

    expect(useAppStore.getState().setMemorySettings).toHaveBeenCalledWith({ goalMaxLoopIterations: 199 })

    act(() => {
      outputCeiling?.click()
    })
    const fourGiB = Array.from(document.querySelectorAll<HTMLButtonElement>('button[role="option"]')).find((option) => option.textContent?.includes('4 GiB'))
    act(() => fourGiB?.click())
    expect(useAppStore.getState().setMemorySettings).toHaveBeenCalledWith({ acpTurnOutputLimitMiB: 4096 })

    act(() => root.unmount())
    host.remove()
  })

  it('updates the default memory selectivity level', () => {
    const { host, root } = renderModal()
    openMemorySettingsSection(host)
    const group = host.querySelector('[role="radiogroup"][aria-label="Default memory level"]') as HTMLElement
    const balanced = Array.from(group.querySelectorAll('button[role="radio"]')).find((button) => button.textContent === 'Balanced') as HTMLButtonElement
    expect(group.querySelector('button[aria-checked="true"]')?.textContent).toBe('Strict')

    act(() => {
      balanced.click()
    })

    expect(useAppStore.getState().setMemorySettings).toHaveBeenCalledWith({ memoryLevelDefault: 'balanced' })
    act(() => root.unmount())
    host.remove()
  })

  it('applies a curated CLI version after an in-app confirmation', () => {
    const { host, root } = renderModal()

    openCliVersionSection(host)

    expect(host.textContent).toContain('CLI version control')
    expect(host.textContent).toContain('Gemini')
    expect(host.textContent).toContain('Codex')
    expect(host.textContent).not.toContain('0.123.0')

    const codexCliToggle = host.querySelector(
      'button[aria-label="Show Codex CLI version settings"]'
    ) as HTMLButtonElement | null
    expect(codexCliToggle).toBeTruthy()

    act(() => {
      codexCliToggle?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(host.textContent).toContain('0.123.0')
    expect(host.querySelector('button[aria-label="Refresh Codex CLI version"]')?.textContent).toBe('')

    const download = host.querySelector<HTMLButtonElement>('[aria-label="Download Codex CLI"]')!
    act(() => download.click())
    const recommended = Array.from(document.querySelectorAll<HTMLButtonElement>('[role="menuitem"]')).find(button => button.textContent?.includes('Download recommended'))!
    expect(recommended.textContent).toContain('0.124.0')
    act(() => recommended.click())
    expect(host.textContent).toContain('Install Codex 0.124.0 globally using npm?')
    expect(useAppStore.getState().applyCliProviderVersion).not.toHaveBeenCalled()
    const confirmInstall = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Install version')
    act(() => confirmInstall?.click())
    expect(useAppStore.getState().applyCliProviderVersion).toHaveBeenCalledWith('codex-cli', '0.124.0')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('disables CLI actions while a version check is running without spinning', () => {
    const providers = useAppStore.getState().cliVersionManager.providers.map((provider) =>
      provider.providerId === 'codex-cli' ? { ...provider, running: true, status: 'checking' as const } : provider
    )
    useAppStore.setState({ cliVersionManager: { providers } })
    const { host, root } = renderModal()

    openCliVersionSection(host)
    act(() => (host.querySelector('button[aria-label="Show Codex CLI version settings"]') as HTMLButtonElement).click())

    expect(host.querySelector<HTMLButtonElement>('button[aria-label="Refresh Codex CLI version"]')?.disabled).toBe(true)
    expect(host.querySelector<HTMLButtonElement>('button[aria-label="Download Codex CLI"]')?.disabled).toBe(true)
    expect(host.querySelector('.animate-spin')).toBeNull()

    act(() => root.unmount())
    host.remove()
  })

  it('keeps provider chat defaults collapsed until toggled', () => {
    const { host, root } = renderModal()

    const defaultsSectionButton = Array.from(host.querySelectorAll('button')).find(
      (button) => button.textContent?.includes('Chat Defaults')
    )
    expect(defaultsSectionButton).toBeTruthy()

    act(() => {
      defaultsSectionButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(host.textContent).toContain('Chat Defaults')
    expect(host.textContent).toContain('Gemini')
    expect(host.textContent).toContain('Codex')
    expect(host.querySelector('button[title="Codex default permissions"]')).toBeNull()

    const codexDefaultsToggle = host.querySelector(
      'button[aria-label="Show Codex chat defaults"]'
    ) as HTMLButtonElement | null
    expect(codexDefaultsToggle).toBeTruthy()

    act(() => {
      codexDefaultsToggle?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(host.textContent).toContain('Reasoning')
    expect(host.querySelector('[aria-label="AI Permission Reviewer"]')).toBeTruthy()
    expect(host.querySelector('button[title="Codex default permissions"]')).toBeTruthy()
    expect(host.querySelector('#codex-cli-defaults-panel')?.className).not.toContain('animate-fade-in')
    expect(host.querySelector('select')).toBeNull()
    const memoryLevel = host.querySelector('button[title="Codex default memory level"]') as HTMLButtonElement
    expect(memoryLevel).toBeTruthy()

    act(() => memoryLevel.click())
    const openMemory = Array.from(document.body.querySelectorAll('button[role="option"]')).find((button) => button.textContent?.includes('Save every valid')) as HTMLButtonElement
    expect(openMemory).toBeTruthy()
    act(() => openMemory.click())
    expect(useAppStore.getState().setProviderChatDefaults).toHaveBeenCalledWith(expect.objectContaining({
      providerChatDefaults: expect.objectContaining({
        'codex-cli': expect.objectContaining({ memoryLevel: 'open', memoryEnabled: true }),
      }),
    }))

    const smallModelWorkflow = Array.from(host.querySelectorAll('button')).find((button) => button.textContent?.includes('Architect + worker off')) as HTMLButtonElement
    expect(smallModelWorkflow).toBeTruthy()
    act(() => smallModelWorkflow.click())
    expect(useAppStore.getState().setProviderChatDefaults).toHaveBeenCalledWith(expect.objectContaining({
      providerChatDefaults: expect.objectContaining({
        'codex-cli': expect.objectContaining({ smallModelMode: true }),
      }),
    }))

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('keeps model refresh outside collapse and advanced preferences available', () => {
    const { host, root } = renderModal()
    act(() => host.querySelector<HTMLButtonElement>('[aria-label="Chat Defaults"]')?.click())
    const refresh = host.querySelector<HTMLButtonElement>('[aria-label="Refresh Codex models"]')!
    expect(refresh).toBeTruthy()
    expect(host.querySelector('#codex-cli-defaults-panel')).toBeNull()
    act(() => refresh.click())
    expect(useAppStore.getState().discoverProviderModels).toHaveBeenCalled()
    act(() => host.querySelector<HTMLButtonElement>('[aria-label="Show Codex chat defaults"]')?.click())
    expect(host.querySelector('[aria-label="Codex provider usage"]')).toBeNull()
    expect(refresh.closest('#codex-cli-defaults-panel')).toBeNull()
    const advanced = host.querySelector('#codex-cli-defaults-panel details') as HTMLDetailsElement
    expect(advanced.open).toBe(false)
    expect(advanced.textContent).toContain('Feature preference')
    expect(advanced.textContent).toContain('Architect + worker')
    act(() => root.unmount()); host.remove()
  })

  it('changes provider chat defaults with the keyboard', async () => {
    const { host, root } = renderModal()
    const defaultsSectionButton = Array.from(host.querySelectorAll('button')).find(
      (button) => button.textContent?.includes('Chat Defaults')
    )
    act(() => defaultsSectionButton?.click())
    act(() => host.querySelector<HTMLButtonElement>('button[aria-label="Show Codex chat defaults"]')?.click())

    const speed = host.querySelector<HTMLButtonElement>('button[title="Codex default speed"]')!
    await act(async () => {
      speed.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowDown', bubbles: true }))
      await Promise.resolve()
    })

    const selected = document.body.querySelector<HTMLButtonElement>(
      '[role="listbox"][aria-label="Codex default speed"] [role="option"][aria-selected="true"]'
    )
    expect(document.activeElement).toBe(selected)
    act(() => selected?.dispatchEvent(new KeyboardEvent('keydown', { key: 'Tab', bubbles: true, cancelable: true })))
    expect(document.body.querySelector('[role="listbox"][aria-label="Codex default speed"]')).toBeNull()

    act(() => speed.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowDown', bubbles: true })))
    const reopenedSelected = document.body.querySelector<HTMLButtonElement>(
      '[role="listbox"][aria-label="Codex default speed"] [role="option"][aria-selected="true"]'
    )
    act(() => reopenedSelected?.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowDown', bubbles: true })))
    act(() => (document.activeElement as HTMLButtonElement).click())
    expect(useAppStore.getState().setProviderChatDefaults).toHaveBeenCalledWith(expect.objectContaining({
      providerChatDefaults: expect.objectContaining({
        'codex-cli': expect.objectContaining({ serviceTier: 'fast' }),
      }),
    }))

    act(() => root.unmount())
    host.remove()
  })

  it('uses discovered provider models and model-specific defaults options', () => {
    useAppStore.setState((state) => ({
      providers: [
        ...state.providers,
        { id: 'copilot-cli', name: 'GitHub Copilot CLI', shortName: 'Copilot', color: '#8ab4ff', description: '', outputMode: 'cli', supportsCli: true, supportsStructured: true, structuredProtocol: 'copilot-acp' },
      ],
      sessions: [
        { ...state.sessions[0], id: 'chat-codex-catalog', providerId: 'codex-cli' },
        { ...state.sessions[0], id: 'chat-copilot-catalog', providerId: 'copilot-cli' },
      ],
      providerChatDefaults: {
        'codex-cli': { modelId: 'gpt-runtime-fast', reasoningEffort: 'ultra', serviceTier: 'flex', approvalMode: 'default', commandSafetyTier: 'off', memoryLevel: 'off', memoryEnabled: false, smallModelMode: false },
        'copilot-cli': { modelId: '', reasoningEffort: '', serviceTier: '', approvalMode: 'default', commandSafetyTier: 'off', memoryLevel: 'off', memoryEnabled: false, smallModelMode: false },
      },
      acpBindingBySessionId: {
        'chat-codex-catalog': {
          ...state.acpBindingBySessionId[state.sessions[0]?.id],
          availableModels: [{
            id: 'gpt-runtime-fast',
            name: 'Runtime Fast',
            description: 'Discovered Codex model',
            defaultReasoningEffort: 'medium',
            supportedReasoningEfforts: ['low', 'medium', 'high'],
            additionalSpeedTiers: ['fast'],
          }],
        },
        'chat-copilot-catalog': {
          ...state.acpBindingBySessionId[state.sessions[0]?.id],
          availableModels: [{
            id: 'copilot-runtime-model',
            name: 'Copilot Runtime',
            description: 'Discovered Copilot model',
            defaultReasoningEffort: 'medium',
            supportedReasoningEfforts: ['low', 'medium', 'max'],
            additionalSpeedTiers: [],
          }],
        },
      },
    }))

    const { host, root } = renderModal()
    act(() => Array.from(host.querySelectorAll('button')).find(
      (button) => button.textContent?.includes('Chat Defaults')
    )?.click())

    act(() => host.querySelector<HTMLButtonElement>('button[aria-label="Show Codex chat defaults"]')?.click())
    act(() => host.querySelector<HTMLButtonElement>('button[title="Codex default speed"]')?.click())
    let listbox = document.body.querySelector<HTMLElement>('[role="listbox"][aria-label="Codex default speed"]')!
    expect(listbox.textContent).toContain('Fast')
    expect(listbox.textContent).not.toContain('Use flexible service tier')
    act(() => document.body.querySelector<HTMLElement>('[role="listbox"]')?.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })))

    act(() => host.querySelector<HTMLButtonElement>('button[title="Codex default reasoning"]')?.click())
    listbox = document.body.querySelector<HTMLElement>('[role="listbox"][aria-label="Codex default reasoning"]')!
    expect(listbox.textContent).toContain('High')
    expect(listbox.textContent).not.toContain('Maximum reasoning with automatic delegation')
    act(() => document.body.querySelector<HTMLElement>('[role="listbox"]')?.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })))

    act(() => host.querySelector<HTMLButtonElement>('button[aria-label="Show Copilot chat defaults"]')?.click())
    act(() => host.querySelector<HTMLButtonElement>('button[title="Copilot default model"]')?.click())
    listbox = document.body.querySelector<HTMLElement>('[role="listbox"][aria-label="Copilot default model"]')!
    expect(listbox.textContent).toContain('Copilot Runtime')

    act(() => root.unmount())
    host.remove()
  })

  it('refreshes cached provider models manually and surfaces refresh failures', () => {
    const discoverProviderModels = vi.fn(() => Promise.resolve(true))
    useAppStore.setState((state) => ({
	  sessions: [{ ...state.sessions[0], id: 'chat-model-cache', providerId: 'codex-cli', workspaceDirectory: '/tmp/project' }],
      acpBindingBySessionId: {
        'chat-model-cache': { ...state.acpBindingBySessionId[state.sessions[0]?.id], modelsLoading: false, modelRefreshError: '' },
      },
      discoverProviderModels,
    }))
    const { host, root } = renderModal()
    const defaults = Array.from(host.querySelectorAll('button')).find((button) => button.textContent?.includes('Chat Defaults')) as HTMLButtonElement
    act(() => defaults.click())
    act(() => (host.querySelector('button[aria-label="Show Codex chat defaults"]') as HTMLButtonElement).click())
    act(() => (host.querySelector('button[aria-label="Refresh Codex models"]') as HTMLButtonElement).click())
	expect(discoverProviderModels).toHaveBeenCalledWith('chat-model-cache', 'codex-cli', '/tmp/project')

    act(() => useAppStore.setState((state) => ({ acpBindingBySessionId: { ...state.acpBindingBySessionId, 'chat-model-cache': { ...state.acpBindingBySessionId['chat-model-cache'], modelRefreshError: 'Model refresh failed.' } } })))
    expect(host.querySelector('[role="status"]')?.textContent).toContain('Model refresh failed.')

    act(() => root.unmount())
    host.remove()
  })

	it('refreshes provider models with zero chats', () => {
	  const discoverProviderModels = vi.fn(() => Promise.resolve(true))
	  useAppStore.setState({ sessions: [], acpBindingBySessionId: {}, discoverProviderModels })
	  const { host, root } = renderModal()
	  const defaults = Array.from(host.querySelectorAll('button')).find((button) => button.textContent?.includes('Chat Defaults')) as HTMLButtonElement
	  act(() => defaults.click())
	  act(() => (host.querySelector('button[aria-label="Show Codex chat defaults"]') as HTMLButtonElement).click())
	  const refresh = host.querySelector('button[aria-label="Refresh Codex models"]') as HTMLButtonElement
	  expect(refresh.disabled).toBe(false)
	  act(() => refresh.click())
	  expect(discoverProviderModels).toHaveBeenCalledWith('', 'codex-cli', '/tmp/project')
	  act(() => root.unmount())
	  host.remove()
	})

  it('switches sections through the sidebar', () => {
    const { host, root } = renderModal()

    openMemorySettingsSection(host)

    expect(host.querySelector('[aria-label="Memory Workers"]')).toBeTruthy()
    expect(host.textContent).toContain('Gemini memory worker')
    expect(host.textContent).toContain('Default')
    expect(host.textContent).not.toContain('CLI default')
    expect(host.textContent).not.toContain('Build and release information')

    openMemoryStoreSection(host)

    expect(host.querySelector('[aria-label="Search memory library"]')).toBeTruthy()
    expect(host.textContent).not.toContain('Memory Backfill')
    expect(host.textContent).not.toContain('Gemini memory worker')

    const aboutSectionButton = Array.from(host.querySelectorAll('button')).find(
      (button) => button.textContent?.includes('About')
    )
    expect(aboutSectionButton).toBeTruthy()

    act(() => {
      aboutSectionButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(host.querySelector('[aria-label="Universal Agent Manager"]')).toBeTruthy()
    expect(host.textContent).toContain('V4.9.0-alpha-11')
    expect(host.textContent).not.toContain('Gemini memory worker')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('updates the default editor through the custom menu', () => {
    const { host, root } = renderModal()
    openEditorsSection(host)

    expect(host.querySelector('[aria-label="Workspace Editors"]')).toBeTruthy()
    expect(host.querySelector('select')).toBeNull()
    expect(host.textContent).toContain('C++')
    expect(host.textContent).not.toContain('8 extensions open in VS Code')

    const defaultEditorButton = host.querySelector(
      'button[title="Default editor"]'
    ) as HTMLButtonElement | null
    expect(defaultEditorButton).toBeTruthy()
    expect(defaultEditorButton?.querySelector('.uam-menu-select__chevron')).toBeTruthy()

    act(() => {
      defaultEditorButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    const webstormOption = Array.from(document.body.querySelectorAll('button[role="option"]')).find(
      (button) => button.textContent?.includes('WebStorm')
    )
    expect(webstormOption).toBeTruthy()

    act(() => {
      webstormOption?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(useAppStore.getState().setEditorSettings).toHaveBeenCalledWith({
      defaultEditorPresetId: 'webstorm',
      editorFileAssociations: [
        {
          id: 'cpp',
          name: 'C++',
          extensions: ['.c', '.cc', '.cpp', '.cxx', '.h', '.hh', '.hpp', '.hxx'],
          editorPresetId: 'vscode',
        },
      ],
    })

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('keeps editor groups collapsed until toggled', () => {
    const { host, root } = renderModal()
    openEditorsSection(host)

    expect(host.textContent).toContain('C++')
    expect(host.textContent).not.toContain('Extensions')
    expect(host.textContent).not.toContain('8 extensions open in VS Code')

    const cppGroupToggle = host.querySelector(
      'button[aria-label="Show C++ editor group"]'
    ) as HTMLButtonElement | null
    expect(cppGroupToggle).toBeTruthy()

    act(() => {
      cppGroupToggle?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(host.querySelector('[aria-label="C++ file extensions"]')).toBeTruthy()
    expect((host.querySelector('[aria-label="C++ file extensions"]') as HTMLInputElement).value.split(' ')).toHaveLength(8)
    expect(host.querySelector('#cpp-editor-group-panel')?.className).not.toContain('animate-fade-in')
    expect(host.querySelector('button[aria-label="Delete C++ editor group"]')).toBeTruthy()
    expect(host.querySelector('button[aria-label="Add editor group"]')).toBeTruthy()

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('updates the C++ editor through the custom menu', () => {
    const { host, root } = renderModal()
    openEditorsSection(host)

    expect(host.querySelector('select')).toBeNull()

    const cppGroupToggle = host.querySelector(
      'button[aria-label="Show C++ editor group"]'
    ) as HTMLButtonElement | null
    expect(cppGroupToggle).toBeTruthy()

    act(() => {
      cppGroupToggle?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    const cppEditorButton = host.querySelector(
      'button[title="C++ editor"]'
    ) as HTMLButtonElement | null
    expect(cppEditorButton).toBeTruthy()

    act(() => {
      cppEditorButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    const clionOption = Array.from(document.body.querySelectorAll('button[role="option"]')).find(
      (button) => button.textContent?.includes('CLion')
    )
    expect(clionOption).toBeTruthy()

    act(() => {
      clionOption?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(useAppStore.getState().setEditorSettings).toHaveBeenCalledWith({
      defaultEditorPresetId: 'vscode',
      editorFileAssociations: [
        {
          id: 'cpp',
          name: 'C++',
          extensions: ['.c', '.cc', '.cpp', '.cxx', '.h', '.hh', '.hpp', '.hxx'],
          editorPresetId: 'clion',
        },
      ],
    })

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('shows the last memory worker log in the memory settings section', () => {
    useAppStore.setState({
      memoryActivity: {
        entryCount: 0,
        lastCreatedAt: '',
        lastCreatedCount: 0,
        runningCount: 0,
        lastStatus: 'Command timed out.',
        lastWorkerChatId: 'chat-1',
        lastWorkerProviderId: 'codex-cli',
        lastWorkerUpdatedAt: '2026-04-23T22:53:00.000Z',
        lastWorkerStatus: 'Command timed out.',
        lastWorkerOutput: 'codex started running commands from the transcript',
        lastWorkerError: 'Command timed out.',
        lastWorkerTimedOut: true,
        lastWorkerCanceled: false,
        lastWorkerHasExitCode: false,
        lastWorkerExitCode: 0,
      },
    })
    const { host, root } = renderModal()

    openMemorySettingsSection(host)

    expect(host.textContent).toContain('Last worker log')
    expect(host.textContent).toContain('Command timed out.')
    expect(host.textContent).toContain('codex started running commands from the transcript')
    expect(host.textContent).toContain('codex-cli')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('updates the memory worker provider through the custom menu', () => {
    const { host, root } = renderModal()

    openMemorySettingsSection(host)

    const providerButton = host.querySelector(
      'button[title="Gemini memory worker provider"]'
    ) as HTMLButtonElement | null
    expect(providerButton).toBeTruthy()

    act(() => {
      providerButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    const codexOption = Array.from(document.body.querySelectorAll('button[role="option"]')).find(
      (button) => button.textContent?.includes('Codex')
    )
    expect(codexOption).toBeTruthy()

    act(() => {
      codexOption?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(useAppStore.getState().setMemorySettings).toHaveBeenCalledWith({
      memoryWorkerBindings: {
        'gemini-cli': { workerProviderId: 'codex-cli', workerModelId: '' },
      },
    })

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('updates the memory worker model through the custom menu', () => {
    useAppStore.setState({
      memoryWorkerBindings: {
        'gemini-cli': { workerProviderId: 'gemini-cli', workerModelId: '' },
      },
    })
    const { host, root } = renderModal()

    openMemorySettingsSection(host)

    const modelButton = host.querySelector(
      'button[title="Gemini memory worker model"]'
    ) as HTMLButtonElement | null
    expect(modelButton).toBeTruthy()

    act(() => {
      modelButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    const flashOption = Array.from(document.body.querySelectorAll('button[role="option"]')).find(
      (button) => button.textContent?.includes('Prioritize speed')
    )
    expect(flashOption).toBeTruthy()

    act(() => {
      flashOption?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(useAppStore.getState().setMemorySettings).toHaveBeenCalledWith({
      memoryWorkerBindings: {
        'gemini-cli': { workerProviderId: 'gemini-cli', workerModelId: 'flash' },
      },
    })

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('uses discovered provider models for memory workers', () => {
    useAppStore.setState({
      providerModelCatalogs: [{
        providerId: 'gemini-cli',
        workspaceDirectory: '/tmp/project',
		executionHostId: 'local',
        availableModels: [{
          id: 'gemini-runtime-model',
          name: 'Gemini Runtime Model',
          description: 'Discovered from the provider',
          defaultReasoningEffort: '',
          supportedReasoningEfforts: [],
          additionalSpeedTiers: [],
        }],
        currentModelId: 'gemini-runtime-model',
        modelsLoading: false,
        modelRefreshError: '',
      }],
    })
    const { host, root } = renderModal()

    openMemorySettingsSection(host)
    act(() => {
      host.querySelector<HTMLButtonElement>('button[title="Gemini memory worker model"]')?.click()
    })

    const listbox = document.body.querySelector('[role="listbox"][aria-label="Gemini memory worker model"]')
    expect(listbox?.textContent).toContain('Gemini Runtime Model')
    expect(listbox?.textContent).not.toContain('Prioritize speed')

    act(() => root.unmount())
    host.remove()
  })

  it('opens the same all-memory library as the activity rail from settings', () => {
    const { host, root } = renderModal()

    openMemoryStoreSection(host)

    expect(host.querySelector('[aria-label="Search memory library"]')).toBeTruthy()
    expect(host.textContent).not.toContain('Open library')
    expect(useAppStore.getState().openAllMemoryLibrary).toHaveBeenCalledTimes(1)
    expect(useAppStore.getState().openGlobalMemoryLibrary).not.toHaveBeenCalled()

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('keeps Markdown Store configuration actions in their own section', () => {
    const { host, root } = renderModal()
    openMarkdownStoreSection(host)

    const saveButton = host.querySelector('button[aria-label="Save Skills directory"]') as HTMLButtonElement
    const library = host.querySelector('[role="region"][aria-label="Skills"]')

    expect(saveButton.disabled).toBe(true)
    expect(library).toBeTruthy()
    expect(host.querySelectorAll('[aria-modal="true"]')).toHaveLength(0)

    act(() => root.unmount())
    host.remove()
  })

  it('leaves Settings open when Escape belongs to the Skills modal', () => {
    useAppStore.setState({ isMarkdownStoreOpen: true })
    const { host, root } = renderModal()

    act(() => window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape' })))

    expect(useAppStore.getState().setSettingsOpen).not.toHaveBeenCalled()

    act(() => root.unmount())
    host.remove()
  })

  it('shows a rejected Skills directory without discarding the typed path', async () => {
    useAppStore.setState({
      setMarkdownStoreDirectory: vi.fn(async () => {
        useAppStore.setState({ markdownStoreError: 'That directory cannot be used.' })
        return false
      }),
    })
    const { host, root } = renderModal()
    openMarkdownStoreSection(host)

    const input = host.querySelector('input[placeholder="Skills directory"]') as HTMLInputElement
    act(() => {
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')?.set?.call(input, '/tmp/rejected-store')
      input.dispatchEvent(new Event('input', { bubbles: true }))
    })
    await act(async () => {
      (host.querySelector('button[aria-label="Save Skills directory"]') as HTMLButtonElement).click()
      await Promise.resolve()
    })

    expect((host.querySelector('input[aria-label="Skills directory"]') as HTMLInputElement | null)?.value).toBe('/tmp/rejected-store')
    expect(host.querySelector('[role="alert"]')?.textContent).toContain('That directory cannot be used.')

    act(() => root.unmount())
    host.remove()
  })

  it('distinguishes failed and cancelled Skills browsing while retaining the typed path', async () => {
    const browse = vi.fn<() => Promise<string | null>>()
      .mockImplementationOnce(async () => {
        useAppStore.setState({ markdownStoreError: 'Folder picker failed.' })
        return null
      })
      .mockImplementationOnce(async () => {
        useAppStore.setState({ markdownStoreError: '' })
        return null
      })
      .mockImplementationOnce(async () => {
        useAppStore.setState({ markdownStoreError: '' })
        return '/tmp/chosen-store'
      })
    useAppStore.setState({ browseMarkdownStoreDirectory: browse })
    const { host, root } = renderModal()
    openMarkdownStoreSection(host)

    const input = host.querySelector('input[aria-label="Skills directory"]') as HTMLInputElement
    const browseButton = host.querySelector('button[aria-label="Browse for Skills directory"]') as HTMLButtonElement
    act(() => {
      Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')?.set?.call(input, '/tmp/typed-store')
      input.dispatchEvent(new Event('input', { bubbles: true }))
    })

    await act(async () => { browseButton.click(); await Promise.resolve() })
    expect(input.value).toBe('/tmp/typed-store')
    expect(host.querySelector('[role="alert"]')?.textContent).toContain('Folder picker failed.')

    await act(async () => { browseButton.click(); await Promise.resolve() })
    expect(input.value).toBe('/tmp/typed-store')
    expect(host.querySelector('[role="alert"]')).toBeNull()

    act(() => useAppStore.setState({ markdownStoreError: 'Stale picker error.' }))
    await act(async () => { browseButton.click(); await Promise.resolve() })
    expect(input.value).toBe('/tmp/chosen-store')
    expect(host.querySelector('[role="alert"]')).toBeNull()

    act(() => root.unmount())
    host.remove()
  })

  it('describes the only supported native system dictation path', () => {
    const { host, root } = renderModal()
    const voiceSection = Array.from(host.querySelectorAll('button')).find((button) => button.textContent?.includes('Voice Input'))
    act(() => voiceSection?.click())
    expect(host.textContent).toContain('native system speech service')
    expect(host.querySelector('button[aria-label="Speech-to-text service"]')).toBeNull()
    expect(host.querySelector('input[aria-label="Voice transcription server URL"]')).toBeNull()
    expect(host.textContent).not.toContain('OpenAI-compatible server')
    act(() => root.unmount())
    host.remove()
  })

  it('guards an embedded memory draft before changing Settings pages', () => {
    const {host,root} = renderModal()
    openMemoryStoreSection(host)
    act(() => host.querySelector<HTMLButtonElement>('[aria-label="Add memory"]')?.click())
    const title = host.querySelector<HTMLInputElement>('[aria-label="Title"]')!
    act(() => { Object.getOwnPropertyDescriptor(HTMLInputElement.prototype,'value')?.set?.call(title,'Unsaved lesson'); title.dispatchEvent(new Event('input',{bubbles:true})) })
    act(() => host.querySelector<HTMLButtonElement>('[aria-label="Editors"]')?.click())
    expect(host.querySelector('[aria-label="Unsaved memory"]')).toBeTruthy()
    expect(host.querySelector('[aria-label="New memory"]')).toBeTruthy()
    act(() => Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find(button => button.textContent === 'Discard')?.click())
    expect(host.querySelector('[aria-label="Add editor group"]')).toBeTruthy()
    expect(useAppStore.getState().closeMemoryLibrary).toHaveBeenCalledTimes(1)
    act(() => root.unmount()); host.remove()
  })

  it('creates, previews, and saves a custom theme', async () => {
    const { host, root } = renderModal()
    const createButton = Array.from(host.querySelectorAll('button')).find((button) => button.getAttribute('aria-label') === 'Add theme')

    act(() => createButton?.dispatchEvent(new MouseEvent('click', { bubbles: true })))

    expect(host.querySelector('[role="radiogroup"][aria-label="Theme base"]')).toBeTruthy()
    act(() => Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find(button => button.textContent === 'Next')?.click())
    const backgroundInput = host.querySelector('input[aria-label="Background color"]') as HTMLInputElement | null
    expect(backgroundInput).toBeTruthy()
    expect(host.querySelectorAll('input[type="color"]')).toHaveLength(0)
    expect(host.querySelectorAll('select')).toHaveLength(0)
    expect(host.querySelectorAll('input[pattern="#[0-9A-Fa-f]{6}"]')).toHaveLength(12)

    const nextButton = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Next') as HTMLButtonElement
    act(() => {
      const valueSetter = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')?.set
      valueSetter?.call(backgroundInput, '#123')
      backgroundInput?.dispatchEvent(new Event('change', { bubbles: true }))
    })
    expect(backgroundInput?.getAttribute('aria-invalid')).toBe('true')
    expect(nextButton.disabled).toBe(true)
    expect(host.textContent).toContain('Every color must use #RRGGBB format.')

    act(() => {
      const valueSetter = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')?.set
      valueSetter?.call(backgroundInput, '#123456')
      backgroundInput?.dispatchEvent(new Event('change', { bubbles: true }))
    })

    expect(document.documentElement.style.getPropertyValue('--bg')).toBe('#123456')
    expect(backgroundInput?.getAttribute('aria-invalid')).toBe('false')
    expect(nextButton.disabled).toBe(false)

    act(() => nextButton.click())
    const saveButton = Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find(button => button.textContent === 'Save theme')!
    await act(async () => {
      saveButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    const saveTheme = useAppStore.getState().saveCustomTheme
    expect(saveTheme).toHaveBeenCalledWith(expect.objectContaining({
      id: 'custom:custom-theme',
      colors: expect.objectContaining({ background: '#123456' }),
    }))
    expect(useAppStore.getState().setTheme).toHaveBeenCalledWith('custom:custom-theme')

    act(() => root.unmount())
    host.remove()
  })

  it('saves a custom theme only once before the modal rerenders', async () => {
    const finishes: Array<(theme: null) => void> = []
    const save = vi.fn(() => new Promise<null>((resolve) => finishes.push(resolve)))
    useAppStore.setState({ saveCustomTheme: save })
    const { host, root } = renderModal()
    const createButton = Array.from(host.querySelectorAll('button')).find((button) => button.getAttribute('aria-label') === 'Add theme') as HTMLButtonElement
    act(() => createButton.click())
    act(() => Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find(button => button.textContent === 'Next')?.click())
    act(() => Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find(button => button.textContent === 'Next')?.click())
    const saveButton = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Save theme') as HTMLButtonElement

    act(() => {
      saveButton.click()
      saveButton.click()
    })

    expect(save).toHaveBeenCalledTimes(1)
    await act(async () => {
      finishes.forEach((finish) => finish(null))
      await Promise.resolve()
    })
    act(() => root.unmount())
    host.remove()
  })

  it('exports and imports validated theme JSON', async () => {
    const customTheme = {
      version: 1 as const,
      id: 'custom:ocean' as const,
      name: 'Ocean',
      base: 'dark' as const,
      colors: {
        background: '#101820', surface: '#17212b', surfaceUp: '#22303c', text: '#f1f5f9',
        textMuted: '#94a3b8', accent: '#38bdf8', sidebar: '#0b1219', userMessage: '#173047',
        assistantMessage: '#17212b', success: '#22c55e', warning: '#f59e0b', error: '#ef4444',
      },
    }
    useAppStore.setState({ theme: customTheme.id, customThemes: [customTheme] })
    const createObjectUrl = vi.fn(() => 'blob:theme')
    const revokeObjectUrl = vi.fn()
    Object.defineProperty(URL, 'createObjectURL', { configurable: true, value: createObjectUrl })
    Object.defineProperty(URL, 'revokeObjectURL', { configurable: true, value: revokeObjectUrl })
    const clickSpy = vi.spyOn(HTMLAnchorElement.prototype, 'click').mockImplementation(() => {})
    const { host, root } = renderModal()

    const exportButton = Array.from(host.querySelectorAll('button')).find((button) => button.getAttribute('aria-label') === 'Export theme')
    act(() => exportButton?.dispatchEvent(new MouseEvent('click', { bubbles: true })))
    expect(createObjectUrl).toHaveBeenCalledTimes(1)
    expect(clickSpy).toHaveBeenCalledTimes(1)
    expect(revokeObjectUrl).toHaveBeenCalledWith('blob:theme')

    const importInput = host.querySelector('input[aria-label="Import theme JSON"]') as HTMLInputElement | null
    const imported = { ...customTheme, id: 'custom:forest', name: 'Forest' }
    const file = new File([], 'forest.json', { type: 'application/json' })
    Object.defineProperty(file, 'text', { value: vi.fn(() => Promise.resolve(JSON.stringify(imported))) })
    Object.defineProperty(importInput, 'files', { configurable: true, value: [file] })
    await act(async () => {
      importInput?.dispatchEvent(new Event('change', { bubbles: true }))
    })

    expect(useAppStore.getState().saveCustomTheme).toHaveBeenCalledWith(imported)
    expect(useAppStore.getState().setTheme).toHaveBeenCalledWith('custom:forest')

    act(() => root.unmount())
    host.remove()
  })

  it('exports and imports portable local chat bundles with honest outcomes', async () => {
    const requests: Array<{ action: string; payload?: { currentValue?: string } }> = []
    window.cefQuery = ({ request, onSuccess }) => {
      const parsed = JSON.parse(request) as { action: string; payload?: { currentValue?: string } }
      requests.push(parsed)
      onSuccess(JSON.stringify(parsed.action === 'exportLocalChats'
        ? { cancelled: false, status: 'complete', folder: '/tmp/uam-export', totalCount: 2, exportedCount: 2, warnings: [], errors: [] }
        : { cancelled: false, status: 'degraded', folder: '/tmp/uam-import', totalCount: 2, importedCount: 1, failedCount: 1, renamedCount: 1, warnings: [], errors: ['One chat could not be saved.'], items: [] }))
    }
    const { host, root } = renderModal()
    openChatDataSection(host)

    const exportButton = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Export all chats') as HTMLButtonElement
    await act(async () => exportButton.click())
    expect(requests[0]).toMatchObject({ action: 'exportLocalChats', payload: { currentValue: '' } })
    expect(host.textContent).toContain('Exported 2 chats to /tmp/uam-export.')

    const importButton = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Import chat bundle') as HTMLButtonElement
    await act(async () => importButton.click())
    expect(requests[1]).toMatchObject({ action: 'importLocalChats', payload: { currentValue: '/tmp/uam-export' } })
    expect(host.textContent).toContain('Imported 1 of 2 chats; 1 received new local IDs. One chat could not be saved.')

    act(() => root.unmount())
    host.remove()
  })
  it('retains a blank editor name, commits on blur, and keeps failed edits guarded', async () => {
    const save = vi.fn(async () => false)
    useAppStore.setState({setEditorSettings:save})
    const {host,root} = renderModal()
    openEditorsSection(host)
    act(() => host.querySelector<HTMLButtonElement>('[aria-label="Show C++ editor group"]')!.click())
    const field = host.querySelector<HTMLInputElement>('[aria-label="C++ group name"]')!
    const typeName = (value: string) => act(() => { Object.getOwnPropertyDescriptor(HTMLInputElement.prototype,'value')!.set!.call(field,value); field.dispatchEvent(new Event('input',{bubbles:true})) })
    typeName('')
    await act(async () => field.dispatchEvent(new FocusEvent('focusout',{bubbles:true})))
    expect(save).not.toHaveBeenCalled()
    expect(field.value).toBe('')
    expect(host.querySelector('[aria-label="Hide C++ editor group"]')).toBeTruthy()
    expect(host.textContent).toContain('Enter a group name.')
    typeName('Native sources')
    expect(save).not.toHaveBeenCalled()
    await act(async () => field.dispatchEvent(new FocusEvent('focusout',{bubbles:true})))
    expect(save).toHaveBeenCalledWith(expect.objectContaining({editorFileAssociations:[expect.objectContaining({id:'cpp',name:'Native sources'})]}))
    expect(host.textContent).toContain('Editor settings could not be saved.')
    act(() => host.querySelector<HTMLButtonElement>('[aria-label="Back to chats"]')!.click())
    expect(host.querySelector('[aria-label="Unsaved editor changes"]')).toBeTruthy()
    expect(useAppStore.getState().setSettingsOpen).not.toHaveBeenCalled()
    act(() => root.unmount()); host.remove()
  })

  it('retains newer MCP JSON after an older save succeeds', async () => {
    let finish!: (value: {ok:boolean}) => void
    const save = vi.fn(() => new Promise<{ok:boolean}>(resolve => {finish=resolve}))
    useAppStore.setState({setMcpServers:save})
    const {host,root} = renderModal()
    openMcpServersSection(host)
    const editor = host.querySelector<HTMLTextAreaElement>('[aria-label="MCP server configuration"]')!
    const edit = (value:string) => act(() => { Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype,'value')!.set!.call(editor,value); editor.dispatchEvent(new Event('input',{bubbles:true})) })
    edit('[]')
    act(() => host.querySelector<HTMLButtonElement>('[aria-label="Save MCP server configuration"]')!.click())
    edit('[{"id":"newer"}]')
    await act(async () => finish({ok:true}))
    expect(editor.value).toBe('[{"id":"newer"}]')
    expect(host.textContent).toContain('newer edits remain unsaved')
    act(() => host.querySelector<HTMLButtonElement>('[aria-label="Back to chats"]')!.click())
    expect(host.querySelector('[aria-label="Discard unsaved MCP changes"]')).toBeTruthy()
    act(() => root.unmount()); host.remove()
  })

  it('shows only supported browser Setup actions and never treats a pending or rejected save as configured', async () => {
    const providers = ['gemini-cli','codex-cli','opencode-cli','claude-cli','copilot-cli'].map(fallbackProviderForId)
    let finish!: (value:{ok:boolean;error?:string}) => void
    useAppStore.setState({providers,setMcpServers:vi.fn(() => new Promise<{ok:boolean;error?:string}>(resolve => {finish=resolve}))})
    const {host,root} = renderModal()
    openMcpServersSection(host)
    expect(host.querySelectorAll('button[aria-label^="Setup "]')).toHaveLength(3)
    expect(host.querySelector('[aria-label="Setup Codex browser control"]')).toBeNull()
    expect(host.querySelector('[aria-label="Setup Claude browser control"]')).toBeNull()
    act(() => host.querySelector<HTMLButtonElement>('[aria-label="Setup Gemini browser control"]')!.click())
    const executable = host.querySelector<HTMLInputElement>('[aria-label="npx executable path"]')!
    act(() => { Object.getOwnPropertyDescriptor(HTMLInputElement.prototype,'value')!.set!.call(executable,'/usr/local/bin/npx'); executable.dispatchEvent(new Event('input',{bubbles:true})) })
    act(() => host.querySelector<HTMLButtonElement>('[aria-label="Add Playwright browser control"]')!.click())
    expect(host.textContent).toContain('Saving configuration…')
    expect(host.textContent).not.toContain('Configured')
    expect(host.querySelector<HTMLButtonElement>('[aria-label="Back to chats"]')!.disabled).toBe(true)
    await act(async () => finish({ok:false,error:'Settings disk is read-only.'}))
    expect(host.textContent).toContain('Settings disk is read-only.')
    expect(host.textContent).not.toContain('Configured')
    expect(executable.value).toBe('/usr/local/bin/npx')
    expect(host.querySelector('button[aria-label="Add Playwright browser control"]')).toBeTruthy()
    act(() => root.unmount()); host.remove()
  })

  it('keeps all five CLI rows compact and confirms a latest download before calling the real action', () => {
    const providers = ['gemini-cli','codex-cli','opencode-cli','claude-cli','copilot-cli'].map(fallbackProviderForId)
    const template = useAppStore.getState().cliVersionManager.providers[0]
    useAppStore.setState({providers,cliVersionManager:{providers:providers.map(provider => ({...template,providerId:provider.id,...(provider.id === 'codex-cli' ? {status:'known-incompatible' as const,message:'Structured chat is incompatible.',lastInstallStatus:'failed' as const} : {})}))}})
    const {host,root} = renderModal()
    openCliVersionSection(host)
    expect(host.querySelectorAll('button[aria-label^="Show "][aria-label$="CLI version settings"]')).toHaveLength(5)
    expect(host.querySelector('[aria-label="Refresh Codex CLI version"]')).toBeNull()
    const toggle = host.querySelector<HTMLButtonElement>('[aria-label="Show Codex CLI version settings"]')!
    expect(toggle.previousElementSibling?.getAttribute('role')).toBe('img')
    expect(toggle.previousElementSibling?.textContent).toBe('2')
    act(() => toggle.click())
    expect(host.querySelector('#codex-cli-cli-version-panel [aria-label="Refresh Codex CLI version"]')).toBeTruthy()
    act(() => host.querySelector<HTMLButtonElement>('[aria-label="Download Codex CLI"]')!.click())
    act(() => Array.from(document.querySelectorAll<HTMLButtonElement>('[role="menuitem"]')).find(button => button.textContent === 'Download latest')!.click())
    expect(useAppStore.getState().applyCliProviderVersion).not.toHaveBeenCalled()
    act(() => Array.from(host.querySelectorAll<HTMLButtonElement>('button')).find(button => button.textContent === 'Install version')!.click())
    expect(useAppStore.getState().applyCliProviderVersion).toHaveBeenCalledWith('codex-cli','latest')
    act(() => root.unmount()); host.remove()
  })

  it('keeps a rejected theme draft open and restores its preview only after discard', async () => {
    useAppStore.setState({saveCustomTheme:vi.fn(async () => null)})
    const {host,root} = renderModal()
    act(() => host.querySelector<HTMLButtonElement>('[aria-label="Add theme"]')!.click())
    expect(host.querySelector('[aria-label="Settings pages"]')).toBeTruthy()
    act(() => host.querySelector<HTMLButtonElement>('[aria-label="Editors"]')!.click())
    const confirmation = host.querySelector('[aria-label="Unsaved theme changes"]')!
    expect(confirmation).toBeTruthy()
    await act(async () => Array.from(confirmation.querySelectorAll<HTMLButtonElement>('button')).find(button => button.textContent === 'Save theme')!.click())
    expect(host.textContent).toContain('Theme could not be saved.')
    expect(host.querySelector('[aria-label="Theme name"]')).toBeTruthy()
    act(() => Array.from(confirmation.querySelectorAll<HTMLButtonElement>('button')).find(button => button.textContent === 'Discard')!.click())
    expect(host.querySelector('[aria-label="Theme name"]')).toBeNull()
    expect(host.querySelector('[aria-label="Add editor group"]')).toBeTruthy()
    expect(useAppStore.getState().setTheme).not.toHaveBeenCalled()
    act(() => root.unmount()); host.remove()
  })

})
