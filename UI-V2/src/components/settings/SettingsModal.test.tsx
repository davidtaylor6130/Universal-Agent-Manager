import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { SettingsModal } from './SettingsModal'
import { useAppStore } from '../../store/useAppStore'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

describe('SettingsModal memory settings', () => {
  beforeEach(() => {
    vi.restoreAllMocks()
    useAppStore.setState({
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
            status: 'supported',
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
            status: 'supported',
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
      markdownStoreDirectory: '/tmp/markdown-store',
      markdownStoreError: '',
      isMarkdownStoreOpen: false,
      setSettingsOpen: vi.fn(),
      setMemorySettings: vi.fn(() => Promise.resolve(true)),
      setProviderChatDefaults: vi.fn(() => Promise.resolve(true)),
      setEditorSettings: vi.fn(() => Promise.resolve(true)),
      setTheme: vi.fn(),
      refreshCustomThemes: vi.fn(() => Promise.resolve(true)),
      saveCustomTheme: vi.fn((theme) => Promise.resolve(theme)),
      deleteCustomTheme: vi.fn(() => Promise.resolve(true)),
      setWorkingDisplayMode: vi.fn(),
      setSidebarSettings: vi.fn(() => Promise.resolve(true)),
      refreshCliProviderVersion: vi.fn(() => Promise.resolve(true)),
      applyCliProviderVersion: vi.fn(() => Promise.resolve(true)),
      openAllMemoryLibrary: vi.fn(() => Promise.resolve(true)),
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
      (button) => button.textContent?.includes('Editors') && button.textContent?.includes('Workspace launch presets')
    )
    expect(editorsSectionButton).toBeTruthy()

    act(() => {
      editorsSectionButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })
  }

  function openCliVersionSection(host: HTMLElement) {
    const cliSectionButton = Array.from(host.querySelectorAll('button')).find(
      (button) => button.textContent?.includes('CLI Version') && button.textContent?.includes('Run or revert provider CLIs')
    )
    expect(cliSectionButton).toBeTruthy()

    act(() => {
      cliSectionButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })
  }

  function openMemorySettingsSection(host: HTMLElement) {
    const memorySectionButton = Array.from(host.querySelectorAll('button')).find(
      (button) => button.textContent?.includes('Memory Settings') && button.textContent?.includes('Defaults and workers')
    )
    expect(memorySectionButton).toBeTruthy()

    act(() => {
      memorySectionButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })
  }

  it('opens and switches sections without a forced animation or duplicate theme refresh', () => {
    const { host, root } = renderModal()
    const dialog = host.querySelector<HTMLElement>('[role="dialog"][aria-label="Settings"]')
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
      (button) => button.textContent?.includes('Memory Store') && button.textContent?.includes('Library and backfill')
    )
    expect(memoryStoreSectionButton).toBeTruthy()

    act(() => {
      memoryStoreSectionButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })
  }

  function openGoalLoopsSection(host: HTMLElement) {
    const button = Array.from(host.querySelectorAll('button')).find(
      (candidate) => candidate.textContent?.includes('Goal Loops') && candidate.textContent?.includes('Loop safety')
    )
    expect(button).toBeTruthy()
    act(() => button?.dispatchEvent(new MouseEvent('click', { bubbles: true })))
  }

  function openMarkdownStoreSection(host: HTMLElement) {
    const button = Array.from(host.querySelectorAll('button')).find(
      (candidate) => candidate.textContent?.includes('Skills') && candidate.textContent?.includes('Reusable prompts and attachments')
    )
    expect(button).toBeTruthy()
    act(() => button?.dispatchEvent(new MouseEvent('click', { bubbles: true })))
  }

  it('uses an icon-first modal dismissal', () => {
    const { host, root } = renderModal()
    const closeButton = host.querySelector('button[aria-label="Close settings"]') as HTMLButtonElement

    expect(closeButton).toBeTruthy()
    expect(closeButton.textContent).toBe('')

    act(() => root.unmount())
    host.remove()
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
    expect(decrease).toBeTruthy()
    expect(output?.textContent).toBe('200')
    expect(host.querySelector('input[type="number"]')).toBeNull()

    act(() => {
      decrease.click()
    })

    expect(useAppStore.getState().setMemorySettings).toHaveBeenCalledWith({ goalMaxLoopIterations: 199 })

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

  it('applies a curated CLI version after confirmation', () => {
    const confirmSpy = vi.spyOn(window, 'confirm').mockReturnValue(true)
    const { host, root } = renderModal()

    openCliVersionSection(host)

    expect(host.textContent).toContain('Provider CLIs')
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

    const targetVersionButton = host.querySelector(
      'button[title="Codex target version"]'
    ) as HTMLButtonElement | null
    expect(targetVersionButton).toBeTruthy()

    act(() => {
      targetVersionButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    const nextVersionOption = Array.from(document.body.querySelectorAll('button[role="option"]')).find(
      (button) => button.textContent?.includes('0.124.0')
    )
    expect(nextVersionOption).toBeTruthy()

    act(() => {
      nextVersionOption?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    const applyButton = host.querySelector('button[aria-label="Apply Codex CLI version"]') as HTMLButtonElement
    expect(applyButton).toBeTruthy()
    expect(applyButton?.disabled).toBe(false)
    expect(applyButton.textContent).toBe('')

    act(() => {
      applyButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(confirmSpy).toHaveBeenCalledWith('Install Codex 0.124.0?')
    expect(useAppStore.getState().applyCliProviderVersion).toHaveBeenCalledWith('codex-cli', '0.124.0')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('animates CLI actions while a version check is running', () => {
    const providers = useAppStore.getState().cliVersionManager.providers.map((provider) =>
      provider.providerId === 'codex-cli' ? { ...provider, running: true, status: 'checking' as const } : provider
    )
    useAppStore.setState({ cliVersionManager: { providers } })
    const { host, root } = renderModal()

    openCliVersionSection(host)
    act(() => (host.querySelector('button[aria-label="Show Codex CLI version settings"]') as HTMLButtonElement).click())

    expect(host.querySelector('button[aria-label="Refresh Codex CLI version"] svg')?.classList.contains('animate-spin')).toBe(true)
    expect(host.querySelector('button[aria-label="Installing Codex CLI version"] svg')?.classList.contains('animate-spin')).toBe(true)

    act(() => root.unmount())
    host.remove()
  })

  it('keeps provider chat defaults collapsed until toggled', () => {
    const { host, root } = renderModal()

    const defaultsSectionButton = Array.from(host.querySelectorAll('button')).find(
      (button) => button.textContent?.includes('Chat Defaults') && button.textContent?.includes('Provider and new-chat settings')
    )
    expect(defaultsSectionButton).toBeTruthy()

    act(() => {
      defaultsSectionButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(host.textContent).toContain('New Chat Defaults')
    expect(host.textContent).toContain('Gemini')
    expect(host.textContent).toContain('Codex')
    expect(host.textContent).not.toContain('Auto approve off')

    const codexDefaultsToggle = host.querySelector(
      'button[aria-label="Show Codex chat defaults"]'
    ) as HTMLButtonElement | null
    expect(codexDefaultsToggle).toBeTruthy()

    act(() => {
      codexDefaultsToggle?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(host.textContent).toContain('Reasoning')
    expect(host.textContent).toContain('Auto approve off')
    expect(host.querySelector('#codex-cli-defaults-panel')?.className).toContain('animate-fade-in')
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

    const smallModelWorkflow = Array.from(host.querySelectorAll('button')).find((button) => button.textContent?.includes('Small-model workflow off')) as HTMLButtonElement
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

  it('changes provider chat defaults with the keyboard', async () => {
    const { host, root } = renderModal()
    const defaultsSectionButton = Array.from(host.querySelectorAll('button')).find(
      (button) => button.textContent?.includes('Chat Defaults') && button.textContent?.includes('Provider and new-chat settings')
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
        'codex-cli': { modelId: 'gpt-runtime-fast', reasoningEffort: 'ultra', serviceTier: 'flex', approvalMode: 'default', memoryLevel: 'off', memoryEnabled: false, autoApproveCommands: false, smallModelMode: false },
        'copilot-cli': { modelId: '', reasoningEffort: '', serviceTier: '', approvalMode: 'default', memoryLevel: 'off', memoryEnabled: false, autoApproveCommands: false, smallModelMode: false },
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
      (button) => button.textContent?.includes('Chat Defaults') && button.textContent?.includes('Provider and new-chat settings')
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
      sessions: [{ ...state.sessions[0], id: 'chat-model-cache', providerId: 'codex-cli' }],
      acpBindingBySessionId: {
        'chat-model-cache': { ...state.acpBindingBySessionId[state.sessions[0]?.id], modelsLoading: false, modelRefreshError: '' },
      },
      discoverProviderModels,
    }))
    const { host, root } = renderModal()
    const defaults = Array.from(host.querySelectorAll('button')).find((button) => button.textContent?.includes('Chat Defaults') && button.textContent?.includes('Provider and new-chat settings')) as HTMLButtonElement
    act(() => defaults.click())
    act(() => (host.querySelector('button[aria-label="Show Codex chat defaults"]') as HTMLButtonElement).click())
    act(() => (host.querySelector('button[aria-label="Refresh Codex models"]') as HTMLButtonElement).click())
    expect(discoverProviderModels).toHaveBeenCalledWith('chat-model-cache')

    act(() => useAppStore.setState((state) => ({ acpBindingBySessionId: { ...state.acpBindingBySessionId, 'chat-model-cache': { ...state.acpBindingBySessionId['chat-model-cache'], modelRefreshError: 'Model refresh failed.' } } })))
    expect(host.querySelector('[role="status"]')?.textContent).toContain('Model refresh failed.')

    act(() => root.unmount())
    host.remove()
  })

  it('switches sections through the sidebar', () => {
    const { host, root } = renderModal()

    openMemorySettingsSection(host)

    expect(host.textContent).toContain('Memory Workers')
    expect(host.textContent).toContain('Gemini memory worker')
    expect(host.textContent).toContain('Default')
    expect(host.textContent).not.toContain('CLI default')
    expect(host.textContent).not.toContain('Build and release information')

    openMemoryStoreSection(host)

    expect(host.textContent).toContain('Memory Library')
    expect(host.textContent).toContain('Memory Backfill')
    expect(host.textContent).not.toContain('Gemini memory worker')

    const aboutSectionButton = Array.from(host.querySelectorAll('button')).find(
      (button) => button.textContent?.includes('About') && button.textContent?.includes('Version information')
    )
    expect(aboutSectionButton).toBeTruthy()

    act(() => {
      aboutSectionButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(host.textContent).toContain('Build and release information')
    expect(host.textContent).toContain('V4.5.3')
    expect(host.textContent).not.toContain('Gemini memory worker')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('updates the default editor through the custom menu', () => {
    const { host, root } = renderModal()
    openEditorsSection(host)

    expect(host.textContent).toContain('Workspace Editors')
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

    expect(host.textContent).toContain('Extensions')
    expect(host.textContent).toContain('8 extensions open in VS Code')
    expect(host.querySelector('#cpp-editor-group-panel')?.className).toContain('animate-fade-in')
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

  it('opens the same all-memory library as the activity rail from settings', () => {
    const { host, root } = renderModal()

    openMemoryStoreSection(host)

    const openLibraryButton = Array.from(host.querySelectorAll('button')).find(
      (button) => button.textContent?.includes('Open library')
    )
    expect(openLibraryButton).toBeTruthy()

    act(() => {
      openLibraryButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

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
    const openStoreButton = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Open store') as HTMLButtonElement

    expect(saveButton.disabled).toBe(true)
    expect(openStoreButton).toBeTruthy()

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

  it('configures an OpenAI-compatible voice transcription server without storing a secret', () => {
    useAppStore.setState({ voiceInputCapabilities: { system: { supported: true, reason: '' }, local: { supported: false, reason: 'Coming soon.' }, server: { supported: true, reason: '' } } })
    const { host, root } = renderModal()
    const voiceSection = Array.from(host.querySelectorAll('button')).find((button) => button.textContent?.includes('Voice Input'))
    act(() => voiceSection?.click())
    const service = host.querySelector<HTMLButtonElement>('button[aria-label="Speech-to-text service"]')!
    act(() => service.click())
    const server = Array.from(document.body.querySelectorAll<HTMLButtonElement>('[role="option"]')).find((option) => option.textContent?.includes('OpenAI-compatible server'))
    act(() => server?.click())
    expect(host.querySelector('input[aria-label="Voice transcription server URL"]')).toBeTruthy()
    expect(host.querySelector('input[aria-label="Voice transcription credential environment variable"]')).toBeTruthy()
    expect(host.textContent).toContain('does not store the secret')
    act(() => root.unmount())
    host.remove()
  })

  it('opens the scan current chats flow from the memory store section', () => {
    const { host, root } = renderModal()

    openMemoryStoreSection(host)

    const scanButton = Array.from(host.querySelectorAll('button')).find(
      (button) => button.textContent?.includes('Scan Current Chats')
    )
    expect(scanButton).toBeTruthy()

    act(() => {
      scanButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(useAppStore.getState().openMemoryScanModal).toHaveBeenCalledTimes(1)

    const modalShell = host.querySelector('.rounded-2xl.shadow-2xl') as HTMLDivElement | null
    expect(modalShell).toBeTruthy()
    expect(modalShell?.className).toContain('flex-col')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('creates, previews, and saves a custom theme', async () => {
    const { host, root } = renderModal()
    const createButton = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Create')

    act(() => createButton?.dispatchEvent(new MouseEvent('click', { bubbles: true })))

    const backgroundInput = host.querySelector('input[aria-label="Background color"]') as HTMLInputElement | null
    expect(backgroundInput).toBeTruthy()
    expect(host.querySelectorAll('input[type="color"]')).toHaveLength(0)
    expect(host.querySelectorAll('select')).toHaveLength(0)
    expect(host.querySelector('[role="radiogroup"][aria-label="Theme base"]')).toBeTruthy()
    expect(host.querySelectorAll('input[pattern="#[0-9A-Fa-f]{6}"]')).toHaveLength(12)

    const saveButton = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Save theme') as HTMLButtonElement | undefined
    act(() => {
      const valueSetter = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')?.set
      valueSetter?.call(backgroundInput, '#123')
      backgroundInput?.dispatchEvent(new Event('change', { bubbles: true }))
    })
    expect(backgroundInput?.getAttribute('aria-invalid')).toBe('true')
    expect(saveButton?.disabled).toBe(true)
    expect(host.textContent).toContain('Every color must use #RRGGBB format.')

    act(() => {
      const valueSetter = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')?.set
      valueSetter?.call(backgroundInput, '#123456')
      backgroundInput?.dispatchEvent(new Event('change', { bubbles: true }))
    })

    expect(document.documentElement.style.getPropertyValue('--bg')).toBe('#123456')
    expect(backgroundInput?.getAttribute('aria-invalid')).toBe('false')
    expect(saveButton?.disabled).toBe(false)

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
    const createButton = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Create') as HTMLButtonElement
    act(() => createButton.click())
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

    const exportButton = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Export JSON')
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
})
