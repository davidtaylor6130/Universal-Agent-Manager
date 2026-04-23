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
      memoryWorkerBindings: {
        'gemini-cli': { workerProviderId: 'gemini-cli', workerModelId: '' },
      },
      memoryLastStatus: '',
      setSettingsOpen: vi.fn(),
      setMemorySettings: vi.fn(() => Promise.resolve(true)),
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

  it('does not render native selects for memory worker controls', () => {
    const { host, root } = renderModal()

    expect(host.querySelector('select')).toBeNull()
    expect(host.textContent).toContain('Appearance')
    expect(host.textContent).toContain('Memory')
    expect(host.textContent).toContain('About')
    expect(host.textContent).toContain('Theme')
    expect(host.textContent).not.toContain('Gemini memory worker')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('switches sections through the sidebar', () => {
    const { host, root } = renderModal()

    const memorySectionButton = Array.from(host.querySelectorAll('button')).find(
      (button) => button.textContent?.includes('Memory') && button.textContent?.includes('Defaults and workers')
    )
    expect(memorySectionButton).toBeTruthy()

    act(() => {
      memorySectionButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(host.textContent).toContain('Memory Workers')
    expect(host.textContent).toContain('Gemini memory worker')
    expect(host.textContent).toContain('CLI default')
    expect(host.textContent).not.toContain('Build and release information')

    const aboutSectionButton = Array.from(host.querySelectorAll('button')).find(
      (button) => button.textContent?.includes('About') && button.textContent?.includes('Version information')
    )
    expect(aboutSectionButton).toBeTruthy()

    act(() => {
      aboutSectionButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

    expect(host.textContent).toContain('Build and release information')
    expect(host.textContent).toContain('V2.0.1')
    expect(host.textContent).not.toContain('Gemini memory worker')

    act(() => {
      root.unmount()
    })
    host.remove()
  })

  it('updates the memory worker provider through the custom menu', () => {
    const { host, root } = renderModal()

    const memorySectionButton = Array.from(host.querySelectorAll('button')).find(
      (button) => button.textContent?.includes('Memory') && button.textContent?.includes('Defaults and workers')
    )

    act(() => {
      memorySectionButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

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

    const memorySectionButton = Array.from(host.querySelectorAll('button')).find(
      (button) => button.textContent?.includes('Memory') && button.textContent?.includes('Defaults and workers')
    )

    act(() => {
      memorySectionButton?.dispatchEvent(new MouseEvent('click', { bubbles: true }))
    })

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
})
