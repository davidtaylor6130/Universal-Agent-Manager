import { act, createRef, type ComponentProps } from 'react'
import { createRoot } from 'react-dom/client'
import { describe, expect, it, vi } from 'vitest'
import { ComposerToolbar } from './Composer'
import { useAppStore } from '../../store/useAppStore'
import { fallbackProviderForId } from '../../utils/providerMetadata'

globalThis.IS_REACT_ACT_ENVIRONMENT = true

describe('Composer unavailable Gemini thinking', () => {
  it('exposes the saved unavailable choice and keyboard Default recovery for an alias', () => {
    window.cefQuery = undefined
    useAppStore.setState({ providerChatDefaults: {}, providerModelCatalogs: [] })
    const provider = fallbackProviderForId('gemini-cli')
    const onSelectReasoning = vi.fn()
    const noop = () => {}
    const props: ComponentProps<typeof ComposerToolbar> = {
      provider, providers: [provider], providerId: 'gemini', canChangeProvider: false, canSend: true,
      runtimeStatusLabel: 'Stopped', runtimeStatusColor: '', modelId: 'auto-gemini-3', reasoningEffort: 'high',
      featurePreference: 'provider', uamAgentId: 'build', uamAgentNextTurn: false, permissionModeId: 'default', permissionsManagedByUam: true,
      permissionControlsDisabled: false, providerModes: [], uamAgents: [], computerUseMode: false, computerUseAwaitingTarget: false,
      memoryLevel: 'default', defaultMemoryLevel: 'off', smallModelMode: false, modelOpen: false, modelMenuRef: createRef<HTMLDivElement>(),
      workspaceDirectory: '/private/fixture', executionHostId: 'local', onToggleModel: noop, onSelectProviderModel: noop, onSelectModel: noop,
      onSelectReviewerModel: noop, onSelectReasoning, onSelectSpeed: noop, onSelectConfigOption: noop, onSelectProviderMode: noop,
      onSelectUamAgent: noop, onSelectPermissionMode: noop, onToggleComputerUseMode: noop, onSelectMemoryLevel: noop, onToggleSmallModelMode: noop,
      goalArmed: false, goalActive: false, goalPaused: false, defaultGoalTokenBudget: 0, onToggleGoal: noop, onSetDefaultGoalTokenBudget: noop,
      onCancelTurn: noop, onAttachFile: noop, onOpenMarkdownStore: noop, dictationState: 'idle', dictationElapsedSeconds: 0,
      dictationAvailable: false, onToggleDictation: noop,
    }
    const host = document.createElement('div'); document.body.append(host)
    const root = createRoot(host)
    try {
      act(() => root.render(<ComposerToolbar {...props} />))
      const trigger = host.querySelector<HTMLButtonElement>('[role="combobox"][aria-label="Reasoning"]')
      expect(trigger).not.toBeNull()
      expect(trigger!.textContent).toContain('unavailable')
      act(() => trigger!.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowDown', bubbles: true })))
      const list = document.body.querySelector<HTMLElement>('[role="listbox"][aria-label="Reasoning"]')!
      expect(list.querySelector<HTMLButtonElement>('[role="option"][disabled]')?.textContent).toContain('unavailable')
      const clear = list.querySelector<HTMLButtonElement>('[role="option"]:not([disabled])')!
      expect(document.activeElement).toBe(clear)
      // Native button activation is a click; JSDOM does not synthesize it from Enter.
      act(() => clear.click())
      expect(onSelectReasoning).toHaveBeenCalledWith('')
    } finally { act(() => root.unmount()); host.remove() }
  })
})
