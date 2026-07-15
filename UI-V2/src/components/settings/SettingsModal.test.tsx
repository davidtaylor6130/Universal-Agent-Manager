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
      memoryIdleDelaySeconds: 120,
      memoryRecallBudgetBytes: 4096,
      goalMaxLoopIterations: 200,
      theme: 'dark',
      customThemes: [],
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
      setSettingsOpen: vi.fn(),
      setMemorySettings: vi.fn(() => Promise.resolve(true)),
      setEditorSettings: vi.fn(() => Promise.resolve(true)),
      setTheme: vi.fn(),
      refreshCustomThemes: vi.fn(() => Promise.resolve(true)),
      saveCustomTheme: vi.fn((theme) => Promise.resolve(theme)),
      deleteCustomTheme: vi.fn(() => Promise.resolve(true)),
      refreshCliProviderVersion: vi.fn(() => Promise.resolve(true)),
      applyCliProviderVersion: vi.fn(() => Promise.resolve(true)),
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

  function openMemoryStoreSection(host: HTMLElement) {
    const memoryStoreSectionButton = Array.from(host.querySelectorAll('button')).find(
      (button) => button.textContent?.includes('Memory Store') && button.textContent?.includes('Library and backfill')
    )
    expect(memoryStoreSectionButton).toBeTruthy()

    act(() => {
      memoryStoreSectionButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })
  }

  it('keeps modal dismissal visually secondary', () => {
    const { host, root } = renderModal()
    const closeButton = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Close') as HTMLButtonElement

    expect(closeButton.className).toContain('uam-btn--secondary')
    expect(closeButton.className).not.toContain('uam-btn--block')
    expect(closeButton.parentElement?.className).toContain('justify-end')

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
    expect(host.querySelector('[role="listbox"][aria-label="Theme"]')).toBeTruthy()

    const lightOption = Array.from(host.querySelectorAll('[role="option"]')).find((option) => option.textContent?.includes('Light')) as HTMLButtonElement | undefined
    act(() => lightOption?.click())

    expect(useAppStore.getState().setTheme).toHaveBeenCalledWith('light')
    expect(host.querySelector('[role="listbox"][aria-label="Theme"]')).toBeNull()

    act(() => root.unmount())
    host.remove()
  })

  it('updates the goal loop iteration cap', () => {
    const { host, root } = renderModal()

    openMemorySettingsSection(host)
    const input = Array.from(host.querySelectorAll('input')).find(
      (candidate) => candidate.parentElement?.textContent?.includes('Maximum iterations')
    )
    expect(input).toBeTruthy()

    act(() => {
      const valueSetter = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')?.set
      valueSetter?.call(input, '0')
      input?.dispatchEvent(new Event('change', { bubbles: true }))
    })

    expect(useAppStore.getState().setMemorySettings).toHaveBeenCalledWith({ goalMaxLoopIterations: 0 })

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

    const targetVersionButton = host.querySelector(
      'button[title="Codex target version"]'
    ) as HTMLButtonElement | null
    expect(targetVersionButton).toBeTruthy()

    act(() => {
      targetVersionButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    const nextVersionOption = Array.from(host.querySelectorAll('button[role="option"]')).find(
      (button) => button.textContent?.includes('0.124.0')
    )
    expect(nextVersionOption).toBeTruthy()

    act(() => {
      nextVersionOption?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    const applyButton = Array.from(host.querySelectorAll('button')).find(
      (button) => button.textContent === 'Apply' && !(button as HTMLButtonElement).disabled
    ) as HTMLButtonElement | undefined
    expect(applyButton).toBeTruthy()
    expect(applyButton?.disabled).toBe(false)

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

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('switches sections through the sidebar', () => {
    const { host, root } = renderModal()

    openMemorySettingsSection(host)

    expect(host.textContent).toContain('Memory Workers')
    expect(host.textContent).toContain('Gemini memory worker')
    expect(host.textContent).toContain('CLI default')
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
    expect(host.textContent).toContain('V4.3.0')
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

    const webstormOption = Array.from(host.querySelectorAll('button[role="option"]')).find(
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

    const clionOption = Array.from(host.querySelectorAll('button[role="option"]')).find(
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

    const codexOption = Array.from(host.querySelectorAll('button[role="option"]')).find(
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

    const flashOption = Array.from(host.querySelectorAll('button[role="option"]')).find(
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

  it('opens the global memory library from the memory store section', () => {
    const { host, root } = renderModal()

    openMemoryStoreSection(host)

    const openLibraryButton = Array.from(host.querySelectorAll('button')).find(
      (button) => button.textContent?.includes('Open library')
    )
    expect(openLibraryButton).toBeTruthy()

    act(() => {
      openLibraryButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(useAppStore.getState().openGlobalMemoryLibrary).toHaveBeenCalledTimes(1)

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('keeps secondary memory-store actions visually secondary', () => {
    const { host, root } = renderModal()
    openMemoryStoreSection(host)

    const saveButton = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Save') as HTMLButtonElement
    const openStoreButton = Array.from(host.querySelectorAll('button')).find((button) => button.textContent === 'Open store') as HTMLButtonElement

    expect(saveButton.disabled).toBe(true)
    expect(openStoreButton.className).toContain('uam-btn--secondary')

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
    expect(modalShell?.style.maxHeight).toBe('calc(100vh - 2rem)')

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
