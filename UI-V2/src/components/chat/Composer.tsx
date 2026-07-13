// ComposerToolbar: message input toolbar with model/mode pickers and
// ComposerIcon SVG sprite. Extracted from ChatView.tsx (MO-3).

import { KeyboardEvent as ReactKeyboardEvent, RefObject, useEffect, useId, useRef, useState } from 'react'
import { Folder, SquarePen, GitBranch, ArrowUp, SquareTerminal, Plus, Settings2, Target } from 'lucide-react'
import type { AcpBinding } from '../../store/useAppStore'
import type { Goal } from '../../types/goal'
import type { Provider } from '../../types/provider'
import { ProviderLogo } from '../shared/ProviderLogo'
import {
  COPILOT_CLI_PROVIDER_ID,
  providerCapabilities,
  providerShortName,
} from '../../utils/providerMetadata'
import {
  buildCodexReasoningOptions,
  buildCodexSpeedOptions,
  buildModelOptions,
  modelOptionFor,
} from './modelOptions'

export type ComposerIconName = 'editor' | 'folder' | 'git-tree' | 'markdown' | 'plus' | 'send' | 'terminal'

export function acpRuntimeBlocksControlChanges(acp?: AcpBinding | null): boolean {
  return Boolean(
    acp?.processing ||
    acp?.lifecycleState === 'waitingPermission' ||
    acp?.lifecycleState === 'waitingUserInput'
  )
}

export function ComposerIcon({ name, size = 14 }: { name: ComposerIconName; size?: number }) {
  if (name === 'markdown') {
    return (
      <span
        aria-hidden="true"
        className="font-semibold leading-none font-mono"
        style={{ fontSize: 11, letterSpacing: 0 }}
      >
        .md
      </span>
    )
  }

  switch (name) {
    case 'folder': return <Folder size={size} aria-hidden />
    case 'editor': return <SquarePen size={size} aria-hidden />
    case 'git-tree': return <GitBranch size={size} aria-hidden />
    case 'send': return <ArrowUp size={size} aria-hidden />
    case 'terminal': return <SquareTerminal size={size} aria-hidden />
    default: return <Plus size={size} aria-hidden />
  }
}

export function ComposerToolbar({
  acp,
  provider,
  providers,
  providerId,
  providerName,
  canSend,
  modelId,
  session,
  reasoningEffort,
  serviceTier,
  approvalModeId,
  autoApproveCommands,
  commandSafetyTier,
  memoryEnabled,
  canChangeProvider,
  providerOpen,
  modelOpen,
  reasoningOpen,
  speedOpen,
  settingsOpen,
  providerMenuRef,
  modelMenuRef,
  reasoningMenuRef,
  speedMenuRef,
  settingsMenuRef,
  onToggleProvider,
  onToggleModel,
  onToggleReasoning,
  onToggleSpeed,
  onToggleSettings,
  onSelectProvider,
  onSelectModel,
  onSelectReasoning,
  onSelectSpeed,
  onTogglePlan,
  onToggleAcceptEdits,
  onToggleYolo,
  onSetCommandSafetyTier,
  onToggleMemory,
  goalArmed,
  goalActive,
  goalPaused,
  defaultGoalTokenBudget,
  onToggleGoal,
  onSetDefaultGoalTokenBudget,
  onStopRuntime,
  onAttachFile,
  onOpenMarkdownStore,
}: {
  acp?: AcpBinding
  provider: Provider
  providers: Provider[]
  providerId: string
  providerName: string
  canSend: boolean
  modelId?: string
  session: { id: string }
  reasoningEffort?: string
  serviceTier?: string
  approvalModeId?: string
  autoApproveCommands: boolean
  commandSafetyTier: 'low' | 'medium' | 'high'
  memoryEnabled: boolean
  canChangeProvider: boolean
  providerOpen: boolean
  modelOpen: boolean
  reasoningOpen: boolean
  speedOpen: boolean
  settingsOpen: boolean
  providerMenuRef: RefObject<HTMLDivElement>
  modelMenuRef: RefObject<HTMLDivElement>
  reasoningMenuRef: RefObject<HTMLDivElement>
  speedMenuRef: RefObject<HTMLDivElement>
  settingsMenuRef: RefObject<HTMLDivElement>
  onToggleProvider: () => void
  onToggleModel: () => void
  onToggleReasoning: () => void
  onToggleSpeed: () => void
  onToggleSettings: () => void
  onSelectProvider: (providerId: string) => void
  onSelectModel: (modelId: string) => void
  onSelectReasoning: (reasoningEffort: string) => void
  onSelectSpeed: (serviceTier: string) => void
  onTogglePlan: () => void
  onToggleAcceptEdits: () => void
  onToggleYolo: () => void
  onSetCommandSafetyTier: (tier: 'low' | 'medium' | 'high') => void
  onToggleMemory: () => void
  goalArmed: boolean
  goalActive: boolean
  goalPaused: boolean
  defaultGoalTokenBudget: number
  onToggleGoal: () => void
  onSetDefaultGoalTokenBudget: (value: number) => void
  onStopRuntime: () => void
  onAttachFile: () => void
  onOpenMarkdownStore: () => void
}) {
  const caps = providerCapabilities(providerId, provider)
  const modelOptions = buildModelOptions(acp, modelId ?? '', provider, providerId)
  const currentModel = modelOptionFor(modelOptions, modelId)
  const reasoningOptions = caps.hasReasoningEffort ? buildCodexReasoningOptions(acp, currentModel.id, reasoningEffort ?? '') : []
  const speedOptions = caps.hasServiceTier ? buildCodexSpeedOptions(acp, currentModel.id, serviceTier ?? '') : []
  const currentReasoning = modelOptionFor(reasoningOptions, reasoningEffort)
  const currentSpeed = modelOptionFor(speedOptions, serviceTier)
  const providerOptions = providers.length > 0 ? providers : [provider]
  const modelDisabled = acpRuntimeBlocksControlChanges(acp)
  const planActive = approvalModeId === 'plan'
  const acceptEditsActive = approvalModeId === 'acceptEdits'
  const yoloActive = autoApproveCommands
  const hasRuntimeModes = Boolean(acp?.running && acp.availableModes.length > 0)
  const planAvailable = !hasRuntimeModes || acp?.availableModes.some((mode) => mode.id === 'plan')
  const acceptEditsAvailable = caps.hasAcceptEditsMode && (!hasRuntimeModes || acp?.availableModes.some((mode) => mode.id === 'acceptEdits'))
  const yoloAvailable = true
  const planDisabled = Boolean(modelDisabled || !planAvailable)
  const acceptEditsDisabled = Boolean(modelDisabled || !acceptEditsAvailable)
  const yoloDisabled = false
  const memoryDisabled = Boolean(modelDisabled)
  const autoLabel = caps.autoLabel
  const modeLabel = planActive ? 'Plan' : acceptEditsActive ? 'Accept Edits' : 'Default'
  const running = Boolean(acp?.processing)
  // Secondary mode controls (plan / accept-edits / auto / memory / markdown) are
  // consolidated behind one "Options" popover to keep the toolbar quiet.
  const [optionsOpen, setOptionsOpen] = useState(false)
  const [modelFocusIndex, setModelFocusIndex] = useState(0)
  const optionsRef = useRef<HTMLDivElement>(null)
  const modelTriggerRef = useRef<HTMLButtonElement>(null)
  const modelWasOpenRef = useRef(false)
  const modelOptionRefs = useRef<Array<HTMLButtonElement | null>>([])
  const modelListId = useId()

  useEffect(() => {
    if (!modelOpen) return
    const selectedIndex = Math.max(0, modelOptions.findIndex((option) => option.id === currentModel.id))
    const focusIsOutsideMenu = !modelOptionRefs.current.includes(document.activeElement as HTMLButtonElement)
    if (focusIsOutsideMenu && modelFocusIndex !== selectedIndex) {
      setModelFocusIndex(selectedIndex)
      return
    }
    const option = modelOptionRefs.current[modelFocusIndex]
    option?.focus()
    option?.scrollIntoView?.({ block: 'nearest' })
  }, [currentModel.id, modelFocusIndex, modelOpen])

  useEffect(() => {
    const wasOpen = modelWasOpenRef.current
    modelWasOpenRef.current = modelOpen
    if (wasOpen && !modelOpen) modelTriggerRef.current?.focus()
  }, [modelOpen])

  const onModelKeyDown = (event: ReactKeyboardEvent<HTMLDivElement>) => {
    if (event.key === 'Escape') {
      event.preventDefault()
      onToggleModel()
      modelTriggerRef.current?.focus()
      return
    }
    if (event.key === 'Enter') {
      event.preventDefault()
      onSelectModel(modelOptions[modelFocusIndex]?.id ?? currentModel.id)
      modelTriggerRef.current?.focus()
      return
    }
    if (event.key !== 'ArrowDown' && event.key !== 'ArrowUp' && event.key !== 'Home' && event.key !== 'End') return
    event.preventDefault()
    if (event.key === 'Home') setModelFocusIndex(0)
    else if (event.key === 'End') setModelFocusIndex(modelOptions.length - 1)
    else setModelFocusIndex((index) => (index + (event.key === 'ArrowDown' ? 1 : -1) + modelOptions.length) % modelOptions.length)
  }
  useEffect(() => {
    if (!optionsOpen) return
    const onDown = (e: MouseEvent) => {
      if (optionsRef.current && !optionsRef.current.contains(e.target as Node)) setOptionsOpen(false)
    }
    const onKey = (e: KeyboardEvent) => { if (e.key === 'Escape') setOptionsOpen(false) }
    document.addEventListener('mousedown', onDown)
    document.addEventListener('keydown', onKey)
    return () => {
      document.removeEventListener('mousedown', onDown)
      document.removeEventListener('keydown', onKey)
    }
  }, [optionsOpen])
  const chipStyle = {
    height: 26,
    borderRadius: 6,
    border: '1px solid var(--border)',
    background: 'color-mix(in srgb, var(--surface) 72%, var(--bg))',
    color: 'var(--text-2)',
  }
  const iconChipStyle = {
    ...chipStyle,
    width: 30,
    justifyContent: 'center',
  }

  return (
    <div
      className="flex items-center gap-2 flex-wrap px-2 py-2 text-xs"
      style={{
        borderTop: '1px solid var(--border)',
        color: 'var(--text-2)',
      }}
    >
      {/* Provider selector moved to the workspace row above the input. */}
      {caps.hasReasoningEffort && (
        <div ref={reasoningMenuRef} className="relative">
          <button
            type="button"
            title="Select Codex reasoning"
            onClick={onToggleReasoning}
            disabled={modelDisabled}
            className="inline-flex items-center gap-1.5 px-2"
            style={{
              ...chipStyle,
              color: reasoningOpen ? 'var(--text)' : 'var(--text-2)',
              borderColor: reasoningOpen ? 'var(--border-bright)' : 'var(--border)',
              opacity: modelDisabled ? 0.55 : 1,
            }}
          >
            <span>Reasoning</span>
            <span style={{ color: 'var(--text)' }}>{currentReasoning.shortLabel}</span>
          </button>
          {reasoningOpen && !modelDisabled && (
            <div
              className="absolute left-0"
              style={{
                bottom: 32,
                width: 250,
                zIndex: 40,
                border: '1px solid var(--border-bright)',
                borderRadius: 8,
                background: 'var(--surface)',
                boxShadow: '0 14px 42px rgba(0, 0, 0, 0.28)',
                padding: 6,
              }}
            >
              <div className="px-2 py-1 text-[11px]" style={{ color: 'var(--text-3)' }}>Reasoning</div>
              {reasoningOptions.map((option) => {
                const selected = option.id === currentReasoning.id
                return (
                  <button
                    key={option.id || 'default'}
                    type="button"
                    onClick={() => onSelectReasoning(option.id)}
                    className="w-full grid gap-0.5 text-left px-2 py-2"
                    style={{
                      borderRadius: 6,
                      background: selected ? 'var(--accent-dim)' : 'transparent',
                      color: selected ? 'var(--text)' : 'var(--text-2)',
                    }}
                  >
                    <span className="flex items-center gap-2">
                      <span className="flex-1">{option.label}</span>
                      {selected && <span style={{ color: 'var(--green)', fontSize: 10 }}>●</span>}
                    </span>
                    <span className="text-[11px]" style={{ color: 'var(--text-3)' }}>{option.detail}</span>
                  </button>
                )
              })}
            </div>
          )}
        </div>
      )}
      {caps.hasServiceTier && (
        <div ref={speedMenuRef} className="relative">
          <button
            type="button"
            title="Select Codex speed"
            onClick={onToggleSpeed}
            disabled={modelDisabled}
            className="inline-flex items-center gap-1.5 px-2"
            style={{
              ...chipStyle,
              color: speedOpen ? 'var(--text)' : 'var(--text-2)',
              borderColor: speedOpen ? 'var(--border-bright)' : 'var(--border)',
              opacity: modelDisabled ? 0.55 : 1,
            }}
          >
            <span>Speed</span>
            <span style={{ color: 'var(--text)' }}>{currentSpeed.shortLabel}</span>
          </button>
          {speedOpen && !modelDisabled && (
            <div
              className="absolute left-0"
              style={{
                bottom: 32,
                width: 230,
                zIndex: 40,
                border: '1px solid var(--border-bright)',
                borderRadius: 8,
                background: 'var(--surface)',
                boxShadow: '0 14px 42px rgba(0, 0, 0, 0.28)',
                padding: 6,
              }}
            >
              <div className="px-2 py-1 text-[11px]" style={{ color: 'var(--text-3)' }}>Speed</div>
              {speedOptions.map((option) => {
                const selected = option.id === currentSpeed.id
                return (
                  <button
                    key={option.id || 'default'}
                    type="button"
                    onClick={() => onSelectSpeed(option.id)}
                    className="w-full grid gap-0.5 text-left px-2 py-2"
                    style={{
                      borderRadius: 6,
                      background: selected ? 'var(--accent-dim)' : 'transparent',
                      color: selected ? 'var(--text)' : 'var(--text-2)',
                    }}
                  >
                    <span className="flex items-center gap-2">
                      <span className="flex-1">{option.label}</span>
                      {selected && <span style={{ color: 'var(--green)', fontSize: 10 }}>●</span>}
                    </span>
                    <span className="text-[11px]" style={{ color: 'var(--text-3)' }}>{option.detail}</span>
                  </button>
                )
              })}
            </div>
          )}
        </div>
      )}
      <div ref={optionsRef} className="relative">
        <button
          type="button"
          title="Add files, goal, and options"
          aria-label="Options"
          aria-expanded={optionsOpen}
          onClick={() => setOptionsOpen((v) => !v)}
          className="inline-flex items-center"
          style={{
            ...iconChipStyle,
            color: optionsOpen ? 'var(--text)' : 'var(--text-2)',
            borderColor: optionsOpen ? 'var(--border-bright)' : 'var(--border)',
          }}
        >
          <Plus size={16} aria-hidden />
        </button>
        {optionsOpen && (
          <div
            className="absolute left-0 flex flex-col gap-1.5"
            style={{
              bottom: 34, minWidth: 210, zIndex: 40, padding: 8,
              border: '1px solid var(--border-bright)', borderRadius: 8,
              background: 'var(--surface)', boxShadow: 'var(--elev-3)',
            }}
          >
            <button
              type="button"
              title="Attach files to the next message"
              onClick={() => { setOptionsOpen(false); onAttachFile() }}
              className="inline-flex items-center gap-1.5 px-2 w-full justify-start"
              style={{ ...chipStyle }}
            >
              <Plus size={13} aria-hidden />
              <span>Attach files</span>
            </button>
            <button
              type="button"
              title={goalActive ? 'Pause goal mode' : goalPaused ? 'Resume goal mode' : goalArmed ? 'Next message will become the goal' : 'Use the next message as a goal'}
              aria-pressed={goalActive || goalArmed}
              onClick={() => { setOptionsOpen(false); onToggleGoal() }}
              disabled={modelDisabled}
              className="inline-flex items-center gap-1.5 px-2 w-full justify-start"
              style={{ ...chipStyle, borderColor: goalActive || goalArmed ? 'color-mix(in srgb, var(--purple) 55%, var(--border))' : 'var(--border)', background: goalActive || goalArmed ? 'color-mix(in srgb, var(--purple) 16%, var(--surface))' : chipStyle.background, color: goalActive || goalArmed ? 'var(--text)' : 'var(--text-2)', opacity: modelDisabled ? 0.55 : 1 }}
            >
              <Target size={13} aria-hidden style={{ color: goalActive || goalArmed ? 'var(--purple)' : 'var(--text-3)' }} />
              <span>{goalArmed ? 'Goal: next message' : 'Goal'}</span>
            </button>
            <div className="mt-1 border-t" style={{ borderColor: 'var(--border)' }} />
            <div className="px-1 pb-0.5 text-[11px] font-medium uppercase tracking-wide" style={{ color: 'var(--text-3)' }}>Mode</div>
            <button
              type="button"
              title={planAvailable ? 'Toggle planning mode. Claude Plan is read-only and will not edit files.' : 'Planning mode unavailable'}
              aria-pressed={planActive}
              onClick={onTogglePlan}
              disabled={planDisabled}
              className="inline-flex items-center gap-1.5 px-2 w-full justify-start"
              style={{ ...chipStyle, borderColor: planActive ? 'color-mix(in srgb, var(--accent) 55%, var(--border))' : 'var(--border)', background: planActive ? 'var(--accent-dim)' : chipStyle.background, color: planActive ? 'var(--text)' : 'var(--text-2)', opacity: planDisabled ? 0.55 : 1 }}
            >
              <span style={{ color: planActive ? 'var(--accent)' : 'var(--text-3)', fontSize: 10 }}>●</span>
              <span>Plan</span>
            </button>
            {caps.hasAcceptEditsMode && (
              <button
                type="button"
                title={acceptEditsAvailable ? 'Toggle Accept Edits mode. Claude can edit workspace files without prompting.' : 'Accept Edits mode unavailable'}
                aria-pressed={acceptEditsActive}
                onClick={onToggleAcceptEdits}
                disabled={acceptEditsDisabled}
                className="inline-flex items-center gap-1.5 px-2 w-full justify-start"
                style={{ ...chipStyle, borderColor: acceptEditsActive ? 'color-mix(in srgb, var(--green) 52%, var(--border))' : 'var(--border)', background: acceptEditsActive ? 'color-mix(in srgb, var(--green) 14%, var(--surface))' : chipStyle.background, color: acceptEditsActive ? 'var(--text)' : 'var(--text-2)', opacity: acceptEditsDisabled ? 0.55 : 1 }}
              >
                <span style={{ color: acceptEditsActive ? 'var(--green)' : 'var(--text-3)', fontSize: 10 }}>●</span>
                <span>{caps.acceptEditsLabel ?? 'Accept Edits'}</span>
              </button>
            )}
            <button
              type="button"
              title={yoloAvailable ? `Toggle ${autoLabel} mode` : `${autoLabel} mode unavailable`}
              aria-pressed={yoloActive}
              onClick={onToggleYolo}
              disabled={yoloDisabled}
              className="inline-flex items-center gap-1.5 px-2 w-full justify-start"
              style={{ ...chipStyle, borderColor: yoloActive ? 'color-mix(in srgb, var(--yellow) 55%, var(--border))' : 'var(--border)', background: yoloActive ? 'color-mix(in srgb, var(--yellow) 16%, var(--surface))' : chipStyle.background, color: yoloActive ? 'var(--text)' : 'var(--text-2)', opacity: yoloDisabled ? 0.55 : 1 }}
            >
              <span style={{ color: yoloActive ? 'var(--yellow)' : 'var(--text-3)', fontSize: 10 }}>●</span>
              <span>{autoLabel}</span>
            </button>
            <div className="mt-1 border-t" style={{ borderColor: 'var(--border)' }} />
            <button
              type="button"
              title="Toggle memory"
              aria-pressed={memoryEnabled}
              onClick={onToggleMemory}
              disabled={memoryDisabled}
              className="inline-flex items-center gap-1.5 px-2 w-full justify-start"
              style={{ ...chipStyle, borderColor: memoryEnabled ? 'color-mix(in srgb, var(--green) 50%, var(--border))' : 'var(--border)', background: memoryEnabled ? 'color-mix(in srgb, var(--green) 14%, var(--surface))' : chipStyle.background, color: memoryEnabled ? 'var(--text)' : 'var(--text-2)', opacity: memoryDisabled ? 0.55 : 1 }}
            >
              <span style={{ color: memoryEnabled ? 'var(--green)' : 'var(--text-3)', fontSize: 10 }}>●</span>
              <span>Memory</span>
            </button>
            <button
              type="button"
              title="Open Markdown Store"
              onClick={onOpenMarkdownStore}
              className="inline-flex items-center gap-1.5 px-2 w-full justify-start"
              style={{ ...chipStyle }}
            >
              <ComposerIcon name="markdown" />
              <span>Markdown store</span>
            </button>
          </div>
        )}
      </div>
      {goalArmed && (
        <span className="inline-flex items-center gap-1.5 px-2 rounded-md" style={{ height: 26, background: 'color-mix(in srgb, var(--purple) 16%, var(--surface))', color: 'var(--text)', border: '1px solid color-mix(in srgb, var(--purple) 45%, var(--border))' }}>
          <Target size={12} aria-hidden style={{ color: 'var(--purple)' }} />
          <span>Goal: next message</span>
        </span>
      )}
      <div className="ml-auto flex items-center gap-2">
        <div ref={modelMenuRef} className="relative">
          <button
            ref={modelTriggerRef}
            type="button"
            title="Select model"
            onClick={onToggleModel}
            disabled={modelDisabled}
            aria-haspopup="listbox"
            aria-expanded={modelOpen}
            aria-controls={modelOpen ? modelListId : undefined}
            className="inline-flex items-center gap-1.5 px-2"
            style={{
              ...chipStyle,
              color: modelOpen ? 'var(--text)' : 'var(--text-2)',
              borderColor: modelOpen ? 'var(--border-bright)' : 'var(--border)',
              opacity: modelDisabled ? 0.55 : 1,
            }}
          >
            <span style={{ color: 'var(--text-3)' }}>Model</span>
            <span style={{ color: 'var(--text)' }}>{currentModel.shortLabel}</span>
          </button>
          {modelOpen && !modelDisabled && (
            <div
              id={modelListId}
              role="listbox"
              aria-label="Model"
              onKeyDown={onModelKeyDown}
              className="absolute right-0"
              style={{
                bottom: 32,
                width: 260,
                zIndex: 40,
                border: '1px solid var(--border-bright)',
                borderRadius: 8,
                background: 'var(--surface)',
                boxShadow: 'var(--elev-3)',
                padding: 6,
              }}
            >
              <div className="px-2 py-1 text-[11px]" style={{ color: 'var(--text-3)' }}>Model</div>
              <div style={{ maxHeight: 520, overflowY: 'auto' }}>
                {modelOptions.map((option, index) => {
                  const selected = option.id === currentModel.id
                  const hideRepeatedCopilotDetail = providerId === COPILOT_CLI_PROVIDER_ID && option.detail.trim().toLowerCase() === option.label.trim().toLowerCase()
                  return (
                    <button
                      key={option.id || 'default'}
                      ref={(element) => { modelOptionRefs.current[index] = element }}
                      type="button"
                      role="option"
                      aria-selected={selected}
                      tabIndex={index === modelFocusIndex ? 0 : -1}
                      onFocus={() => setModelFocusIndex(index)}
                      onMouseEnter={() => setModelFocusIndex(index)}
                      onClick={() => {
                        onSelectModel(option.id)
                        modelTriggerRef.current?.focus()
                      }}
                      className="w-full grid gap-0.5 text-left px-2 py-2"
                      style={{
                        minHeight: 52,
                        borderRadius: 6,
                        background: selected ? 'var(--accent-dim)' : 'transparent',
                        color: selected ? 'var(--text)' : 'var(--text-2)',
                        outline: index === modelFocusIndex ? '1px solid var(--accent)' : 'none',
                      }}
                    >
                      <span className="flex items-center gap-2">
                        <span className="flex-1">{option.label}</span>
                        {selected && <span style={{ color: 'var(--green)', fontSize: 10 }}>●</span>}
                      </span>
                      {!hideRepeatedCopilotDetail && <span className="text-[11px]" style={{ color: 'var(--text-3)' }}>{option.detail}</span>}
                    </button>
                  )
                })}
              </div>
            </div>
          )}
        </div>
        <div ref={settingsMenuRef} className="relative">
          <button
            type="button"
            title="Settings"
            onClick={onToggleSettings}
            className="inline-flex items-center gap-1.5 px-2"
            style={{
              ...chipStyle,
              color: settingsOpen ? 'var(--text)' : 'var(--text-2)',
              borderColor: settingsOpen ? 'var(--border-bright)' : 'var(--border)',
            }}
          >
            <Settings2 size={14} aria-hidden />
          </button>
          {settingsOpen && (
            <div
              className="absolute right-0"
              style={{
                bottom: 32,
                width: 250,
                zIndex: 40,
                border: '1px solid var(--border-bright)',
                borderRadius: 8,
                background: 'var(--surface)',
                boxShadow: '0 14px 42px rgba(0, 0, 0, 0.28)',
                padding: 10,
              }}
            >
              <div className="text-xs font-semibold mb-2" style={{ color: 'var(--text)' }}>Chat settings</div>
              <div className="grid gap-2 text-[11px]" style={{ color: 'var(--text-2)' }}>
                <div className="flex justify-between gap-3">
                  <span style={{ color: 'var(--text-3)' }}>Provider</span>
                  <span>{providerName}</span>
                </div>
                <div className="flex justify-between gap-3">
                  <span style={{ color: 'var(--text-3)' }}>Model</span>
                  <span>{currentModel.label}</span>
                </div>
                {caps.hasReasoningEffort && (
                  <div className="flex justify-between gap-3">
                    <span style={{ color: 'var(--text-3)' }}>Reasoning</span>
                    <span>{currentReasoning.label}</span>
                  </div>
                )}
                {caps.hasServiceTier && (
                  <div className="flex justify-between gap-3">
                    <span style={{ color: 'var(--text-3)' }}>Speed</span>
                    <span>{currentSpeed.label}</span>
                  </div>
                )}
                <div className="flex justify-between gap-3">
                  <span style={{ color: 'var(--text-3)' }}>Mode</span>
                  <span>{modeLabel}</span>
                </div>
                <label className="grid gap-1">
                  <span style={{ color: 'var(--text-3)' }}>Command safety</span>
                  <select
                    aria-label="Command safety tier"
                    value={commandSafetyTier}
                    onChange={(event) => onSetCommandSafetyTier(event.target.value as 'low' | 'medium' | 'high')}
                    className="w-full px-2 py-1 text-xs"
                    style={{ border: '1px solid var(--border)', borderRadius: 6, background: 'var(--bg)', color: 'var(--text)' }}
                  >
                    <option value="low">Low</option>
                    <option value="medium">Medium</option>
                    <option value="high">High</option>
                  </select>
                </label>
                <div className="flex justify-between gap-3">
                  <span style={{ color: 'var(--text-3)' }}>Memory</span>
                  <span>{memoryEnabled ? 'On' : 'Off'}</span>
                </div>
                <label className="grid gap-1">
                  <span style={{ color: 'var(--text-3)' }}>Goal token budget</span>
                  <input
                    type="number"
                    min={0}
                    value={defaultGoalTokenBudget || ''}
                    placeholder="Unlimited"
                    onChange={(event) => onSetDefaultGoalTokenBudget(parseInt(event.target.value || '0', 10))}
                    className="w-full px-2 py-1 text-xs"
                    style={{
                      border: '1px solid var(--border)',
                      borderRadius: 6,
                      background: 'var(--bg)',
                      color: 'var(--text)',
                      outline: 'none',
                    }}
                  />
                </label>
              </div>
            </div>
          )}
        </div>
        <button
          type={running ? 'button' : 'submit'}
          title={running ? 'Stop runtime' : 'Send prompt'}
          disabled={!running && !canSend}
          onClick={running ? onStopRuntime : undefined}
          className="h-[30px] w-[34px] text-xs font-semibold inline-flex items-center justify-center"
          style={{
            borderRadius: 7,
            border: running
              ? '1px solid color-mix(in srgb, var(--red) 46%, var(--border-bright))'
              : '1px solid color-mix(in srgb, var(--accent) 64%, var(--border-bright))',
            background: running ? 'color-mix(in srgb, var(--red) 14%, var(--surface))' : canSend ? 'var(--accent)' : 'var(--surface-up)',
            color: running ? 'var(--red)' : canSend ? '#fff' : 'var(--text-3)',
            boxShadow: !running && canSend ? '0 8px 18px color-mix(in srgb, var(--accent) 20%, transparent)' : 'none',
          }}
        >
          {running ? (
            <span aria-hidden="true" style={{ width: 9, height: 9, borderRadius: 2, background: 'currentColor' }} />
          ) : (
            <ComposerIcon name="send" size={15} />
          )}
        </button>
      </div>
    </div>
  )
}
