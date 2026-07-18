// ComposerToolbar: message input toolbar with model/mode pickers and
// ComposerIcon SVG sprite. Extracted from ChatView.tsx (MO-3).

import { KeyboardEvent as ReactKeyboardEvent, RefObject, type ReactNode, useEffect, useId, useRef, useState } from 'react'
import { Folder, SquarePen, GitBranch, ArrowUp, SquareTerminal, Plus, Settings2, Target, ClipboardList, Cpu, Shield, ShieldAlert, ShieldCheck, Sparkles, Mic, Square, X } from 'lucide-react'
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
  selectedRuntimeModel,
} from './modelOptions'
import { MenuSelect, ViewportMenu } from '../ui'
import { MEMORY_LEVEL_OPTIONS, type MemoryLevel } from '../../types/memory'

export type ComposerIconName = 'editor' | 'folder' | 'git-tree' | 'markdown' | 'plus' | 'send' | 'terminal'
export type DictationState = 'idle' | 'starting' | 'listening' | 'stopping'

export const COMMAND_SAFETY_TIERS = [
  { id: 'low', label: 'Low', detail: 'Warn about a smaller set of risky commands.' },
  { id: 'medium', label: 'Medium', detail: 'Balanced command warnings.' },
  { id: 'high', label: 'High', detail: 'Warn before more potentially risky commands.' },
] as const

export const PERMISSION_MODES = [
  { id: 'default', name: 'Default', description: 'Ask before commands and file changes.' },
  { id: 'acceptEdits', name: 'Accept Edits', description: 'Automatically approve workspace file edits.' },
  { id: 'yolo', name: 'YOLO', description: 'Automatically approve every permission request.' },
  { id: 'auto', name: 'Auto Decide', description: 'Automatically approve requests allowed by command safety.' },
] as const

export type CommandSafetyTier = 'off' | 'acceptEdits' | 'low' | 'medium' | 'high' | 'yolo'

export function permissionModeForTier(tier: CommandSafetyTier): 'default' | 'acceptEdits' | 'yolo' | 'auto' {
  return tier === 'off' ? 'default' : tier === 'acceptEdits' ? 'acceptEdits' : tier === 'yolo' ? 'yolo' : 'auto'
}

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

export function permissionModeIcon(id: string, size = 14) {
  if (id === 'auto') return <Sparkles size={size} />
  if (id === 'yolo') return <ShieldAlert size={size} />
  if (id === 'acceptEdits') return <SquarePen size={size} />
  if (id === 'plan') return <ClipboardList size={size} />
  return <ShieldCheck size={size} />
}

function ActiveModeChip({ label, icon, onClear }: { label: string; icon: ReactNode; onClear?: () => void }) {
  return <button type="button" aria-label={onClear ? `Disable ${label}` : label} onClick={onClear} className="uam-choice-button inline-flex h-[26px] items-center gap-1.5 rounded-md px-2 text-[11px]" style={{ border: '1px solid var(--border-bright)', background: 'var(--surface-up)', color: 'var(--text-2)' }}>{icon}<span>{label}</span>{onClear && <X size={11} aria-hidden style={{ color: 'var(--text-3)' }} />}</button>
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
  permissionModeId,
  agentModes,
  commandSafetyTier,
  memoryLevel,
  defaultMemoryLevel,
  memoryChipVisible,
  canChangeProvider,
  providerOpen,
  modelOpen,
  settingsOpen,
  providerMenuRef,
  modelMenuRef,
  settingsMenuRef,
  onToggleProvider,
  onToggleModel,
  onToggleSettings,
  onSelectProvider,
  onSelectModel,
  onSelectReasoning,
  onSelectSpeed,
  onSelectAgentMode,
  onSelectPermissionMode,
  onSetCommandSafetyTier,
  onSelectMemoryLevel,
  onClearMemoryLevel,
  goalArmed,
  goalActive,
  goalPaused,
  defaultGoalTokenBudget,
  onToggleGoal,
  onSetDefaultGoalTokenBudget,
  onStopRuntime,
  onAttachFile,
  onOpenMarkdownStore,
  dictationState,
  dictationError,
  dictationElapsedSeconds,
  dictationAvailable,
  onToggleDictation,
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
  permissionModeId: string
  agentModes: Array<{ id: string; name: string; description?: string }>
  commandSafetyTier: CommandSafetyTier
  memoryLevel: MemoryLevel
  defaultMemoryLevel: MemoryLevel
  memoryChipVisible: boolean
  canChangeProvider: boolean
  providerOpen: boolean
  modelOpen: boolean
  settingsOpen: boolean
  providerMenuRef: RefObject<HTMLDivElement>
  modelMenuRef: RefObject<HTMLDivElement>
  settingsMenuRef: RefObject<HTMLDivElement>
  onToggleProvider: () => void
  onToggleModel: () => void
  onToggleSettings: () => void
  onSelectProvider: (providerId: string) => void
  onSelectModel: (modelId: string) => void
  onSelectReasoning: (reasoningEffort: string) => void
  onSelectSpeed: (serviceTier: string) => void
  onSelectAgentMode: (modeId: string) => void
  onSelectPermissionMode: (modeId: string) => void
  onSetCommandSafetyTier: (tier: 'low' | 'medium' | 'high') => void
  onSelectMemoryLevel: (level: MemoryLevel) => void
  onClearMemoryLevel: () => void
  goalArmed: boolean
  goalActive: boolean
  goalPaused: boolean
  defaultGoalTokenBudget: number
  onToggleGoal: () => void
  onSetDefaultGoalTokenBudget: (value: number) => void
  onStopRuntime: () => void
  onAttachFile: () => void
  onOpenMarkdownStore: () => void
  dictationState: DictationState
  dictationError?: string
  dictationElapsedSeconds: number
  dictationAvailable: boolean
  onToggleDictation: () => void
}) {
  const caps = providerCapabilities(providerId, provider)
  const modelOptions = buildModelOptions(acp, modelId ?? '', provider, providerId)
  const currentModel = modelOptionFor(modelOptions, modelId)
  const runtimeSupportsReasoning = (selectedRuntimeModel(acp, currentModel.id)?.supportedReasoningEfforts?.length ?? 0) > 0
  const reasoningOptions = caps.hasReasoningEffort || runtimeSupportsReasoning ? buildCodexReasoningOptions(acp, currentModel.id, reasoningEffort ?? '') : []
  const hasReasoningEffort = reasoningOptions.length > 0
  const speedOptions = caps.hasServiceTier ? buildCodexSpeedOptions(acp, currentModel.id, serviceTier ?? '') : []
  const currentReasoning = modelOptionFor(reasoningOptions, reasoningEffort)
  const currentSpeed = modelOptionFor(speedOptions, serviceTier)
  const providerOptions = providers.length > 0 ? providers : [provider]
  const modelDisabled = acpRuntimeBlocksControlChanges(acp)
  const planActive = approvalModeId === 'plan'
  const memoryDisabled = false
  const permissionModes = PERMISSION_MODES.filter((mode) => mode.id !== 'acceptEdits' || caps.hasAcceptEditsMode)
  const permissionMode = permissionModes.find((mode) => mode.id === permissionModeId) ?? permissionModes[0]
  const running = Boolean(acp?.processing)
  const dictationVisualState = dictationError ? 'error' : dictationState
  const dictationLabel = !dictationAvailable
    ? 'Dictation requires the desktop app'
    : dictationError
      ? dictationState === 'idle' ? 'Retry dictation' : 'Dictation error'
      : dictationState === 'starting'
        ? 'Starting dictation'
        : dictationState === 'listening'
          ? 'Stop dictation'
          : dictationState === 'stopping'
            ? 'Finishing dictation'
            : 'Start dictation'
  const dictationActive = dictationState !== 'idle' || Boolean(dictationError)
  const dictationStatusText = dictationError || (dictationState === 'starting'
    ? 'Starting dictation…'
    : dictationState === 'stopping'
      ? 'Finishing dictation…'
      : 'Listening…')
  // Secondary mode controls (plan / accept-edits / auto / memory / markdown) are
  // consolidated behind one "Options" popover to keep the toolbar quiet.
  const [optionsOpen, setOptionsOpen] = useState(false)
  const [modelFocusIndex, setModelFocusIndex] = useState(0)
  const optionsRef = useRef<HTMLDivElement>(null)
  const optionsTriggerRef = useRef<HTMLButtonElement>(null)
  const optionsMenuRef = useRef<HTMLDivElement>(null)
  const modelTriggerRef = useRef<HTMLButtonElement>(null)
  const settingsTriggerRef = useRef<HTMLButtonElement>(null)
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
    if (modelOptions.length === 0 || (event.key !== 'ArrowDown' && event.key !== 'ArrowUp' && event.key !== 'Home' && event.key !== 'End')) return
    event.preventDefault()
    if (event.key === 'Home') setModelFocusIndex(0)
    else if (event.key === 'End') setModelFocusIndex(modelOptions.length - 1)
    else setModelFocusIndex((index) => (index + (event.key === 'ArrowDown' ? 1 : -1) + modelOptions.length) % modelOptions.length)
  }
  const onModelTriggerKeyDown = (event: ReactKeyboardEvent<HTMLButtonElement>) => {
    if (event.key !== 'ArrowDown' && event.key !== 'ArrowUp') return
    event.preventDefault()
    if (!modelOpen) onToggleModel()
  }
  useEffect(() => {
    if (!optionsOpen) return
    const onDown = (e: MouseEvent) => {
      const target = e.target as Node
      if (target instanceof Element && target.closest('[data-viewport-menu]')) return
      if (!optionsRef.current?.contains(target) && !optionsMenuRef.current?.contains(target)) setOptionsOpen(false)
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
      <div ref={optionsRef} className="relative">
        <button
          ref={optionsTriggerRef}
          type="button"
          title="Add files, goal, and options"
          aria-label="Options"
          aria-expanded={optionsOpen}
          onClick={() => setOptionsOpen((v) => !v)}
          className="uam-composer-action inline-flex items-center"
          style={{
            ...iconChipStyle,
            color: optionsOpen ? 'var(--text)' : 'var(--text-2)',
            borderColor: optionsOpen ? 'var(--border-bright)' : 'var(--border)',
          }}
        >
          <Plus size={16} aria-hidden />
        </button>
        {optionsOpen && (
          <ViewportMenu
            ref={optionsMenuRef}
            anchorRef={optionsTriggerRef}
            side="top"
            className="flex flex-col gap-1.5 animate-fade-in"
            style={{
              minWidth: 210, padding: 8,
              border: '1px solid var(--border-bright)', borderRadius: 8,
              background: 'var(--surface)', boxShadow: 'var(--elev-3)',
            }}
          >
            <button
              type="button"
              title="Attach files to the next message"
              onClick={() => { setOptionsOpen(false); onAttachFile() }}
              className="uam-choice-button inline-flex items-center gap-1.5 px-2 w-full justify-start"
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
              className="uam-choice-button inline-flex items-center gap-1.5 px-2 w-full justify-start"
              style={{ ...chipStyle, borderColor: goalActive || goalArmed ? 'color-mix(in srgb, var(--purple) 55%, var(--border))' : 'var(--border)', background: goalActive || goalArmed ? 'color-mix(in srgb, var(--purple) 16%, var(--surface))' : chipStyle.background, color: goalActive || goalArmed ? 'var(--text)' : 'var(--text-2)', opacity: modelDisabled ? 0.55 : 1 }}
            >
              <Target size={13} aria-hidden style={{ color: goalActive || goalArmed ? 'var(--purple)' : 'var(--text-3)' }} />
              <span>{goalArmed ? 'Goal: next message' : 'Goal'}</span>
            </button>
            {(hasReasoningEffort || caps.hasServiceTier) && (
              <>
                <div className="mt-1 border-t" style={{ borderColor: 'var(--border)' }} />
                <div className="px-1 pb-0.5 text-[11px] font-medium uppercase tracking-wide" style={{ color: 'var(--text-3)' }}>Model</div>
                {hasReasoningEffort && (
                  <MenuSelect
                    label="Reasoning"
                    value={currentReasoning.id}
                    options={reasoningOptions.map((option) => ({ value: option.id, label: option.label, description: option.detail }))}
                    onChange={onSelectReasoning}
                    disabled={modelDisabled}
                  />
                )}
                {caps.hasServiceTier && (
                  <MenuSelect
                    label="Speed"
                    value={currentSpeed.id}
                    options={speedOptions.map((option) => ({ value: option.id, label: option.label, description: option.detail }))}
                    onChange={onSelectSpeed}
                    disabled={modelDisabled}
                  />
                )}
              </>
            )}
            <div className="mt-1 border-t" style={{ borderColor: 'var(--border)' }} />
            <div className="px-1 pb-0.5 text-[11px] font-medium uppercase tracking-wide" style={{ color: 'var(--text-3)' }}>Agent</div>
            <MenuSelect
              label="Agent mode"
              value={approvalModeId ?? 'default'}
              options={agentModes.map((mode) => ({
                value: mode.id,
                label: mode.name,
                description: mode.description,
                icon: permissionModeIcon(mode.id),
              }))}
              onChange={onSelectAgentMode}
            />
            <div className="mt-1 border-t" style={{ borderColor: 'var(--border)' }} />
            <div className="px-1 pb-0.5 text-[11px] font-medium uppercase tracking-wide" style={{ color: 'var(--text-3)' }}>Permissions</div>
            <MenuSelect
              label="Permissions"
              value={permissionModeId}
              options={permissionModes.map((mode) => ({
                value: mode.id,
                label: mode.name,
                description: mode.description,
                icon: permissionModeIcon(mode.id),
              }))}
              onChange={onSelectPermissionMode}
            />
            {permissionModeId === 'auto' && (
              <MenuSelect
                label="Auto Decide safety"
                value={commandSafetyTier}
                onChange={(value) => onSetCommandSafetyTier(value as 'low' | 'medium' | 'high')}
                options={COMMAND_SAFETY_TIERS.map((tier) => ({
                  value: tier.id,
                  label: tier.label,
                  description: tier.detail,
                  icon: tier.id === 'low' ? <Shield size={14} /> : tier.id === 'medium' ? <ShieldCheck size={14} /> : <ShieldAlert size={14} />,
                }))}
              />
            )}
            <div className="mt-1 border-t" style={{ borderColor: 'var(--border)' }} />
            <MenuSelect
              label="Memory"
              value={memoryLevel}
              options={MEMORY_LEVEL_OPTIONS.map((option) => ({ value: option.id, label: `Memory ${option.label}`, description: option.detail }))}
              onChange={(level) => onSelectMemoryLevel(level as MemoryLevel)}
              disabled={memoryDisabled}
            />
            <button
              type="button"
              title="Open Skills"
              onClick={() => { setOptionsOpen(false); onOpenMarkdownStore() }}
              className="uam-choice-button inline-flex items-center gap-1.5 px-2 w-full justify-start"
              style={{ ...chipStyle }}
            >
              <ComposerIcon name="markdown" />
              <span>Skills</span>
            </button>
          </ViewportMenu>
        )}
      </div>
      {dictationActive ? (
        <button
          type="button"
          title={dictationError || dictationLabel}
          aria-label={dictationLabel}
          aria-pressed={dictationState !== 'idle'}
          aria-busy={dictationState === 'starting' || dictationState === 'stopping'}
          data-dictation-state={dictationVisualState}
          onClick={onToggleDictation}
          disabled={!dictationAvailable || dictationState === 'starting' || dictationState === 'stopping'}
          className="uam-composer-action uam-dictation-button flex h-[30px] min-w-0 flex-1 items-center justify-center gap-3 px-3 text-xs font-semibold animate-fade-in"
          style={{ borderRadius: 7, opacity: !dictationAvailable || dictationState === 'starting' || dictationState === 'stopping' ? 0.55 : 1 }}
        >
          {dictationState === 'listening' ? (
            <span className="uam-dictation-listening-indicator uam-dictation-wave uam-dictation-wave--wide" aria-hidden>
              <i /><i /><i /><i /><i /><i /><i />
            </span>
          ) : <Mic size={15} aria-hidden />}
          <span>{dictationStatusText}</span>
          {dictationState === 'listening' && (
            <span className="tabular-nums">{Math.floor(dictationElapsedSeconds / 60)}:{String(dictationElapsedSeconds % 60).padStart(2, '0')}</span>
          )}
        </button>
      ) : <>
      {goalArmed && <ActiveModeChip label="Goal: next message" icon={<Target size={12} aria-hidden style={{ color: 'var(--purple)' }} />} onClear={onToggleGoal} />}
      {planActive && <ActiveModeChip label="Plan" icon={<ClipboardList size={12} aria-hidden style={{ color: 'var(--accent)' }} />} onClear={() => onSelectAgentMode('default')} />}
      {permissionModeId !== 'default' && <ActiveModeChip label={permissionModeId === 'auto' ? `Auto Decide: ${COMMAND_SAFETY_TIERS.find((tier) => tier.id === commandSafetyTier)?.label ?? 'Medium'}` : permissionMode.name} icon={permissionModeIcon(permissionModeId, 12)} onClear={() => onSelectPermissionMode('default')} />}
      {memoryChipVisible && <ActiveModeChip label={`Memory ${MEMORY_LEVEL_OPTIONS.find((option) => option.id === memoryLevel)?.label ?? memoryLevel}`} icon={<span aria-hidden>●</span>} onClear={onClearMemoryLevel} />}
      {hasReasoningEffort && <ActiveModeChip label={`Reasoning: ${currentReasoning.label}`} icon={<Cpu size={12} aria-hidden />} />}
      {serviceTier && <ActiveModeChip label={`Speed: ${currentSpeed.label}`} icon={<Sparkles size={12} aria-hidden />} onClear={() => onSelectSpeed('')} />}
      <div className="ml-auto flex items-center gap-2">
        <div ref={modelMenuRef} className="relative">
          <button
            ref={modelTriggerRef}
            type="button"
            title="Select model"
            aria-label="Select model"
            onClick={onToggleModel}
            disabled={modelDisabled}
            aria-haspopup="listbox"
            aria-expanded={modelOpen}
            aria-controls={modelOpen ? modelListId : undefined}
            onKeyDown={onModelTriggerKeyDown}
            className="uam-composer-action inline-flex items-center gap-1.5 px-2"
            style={{
              ...chipStyle,
              borderRadius: 7,
              color: modelOpen ? 'var(--text)' : 'var(--text-2)',
              borderColor: 'var(--border-bright)',
              opacity: modelDisabled ? 0.55 : 1,
            }}
          >
            <span style={{ color: 'var(--text)' }}>{currentModel.shortLabel}</span>
          </button>
          {modelOpen && !modelDisabled && (
            <ViewportMenu
              anchorRef={modelTriggerRef}
              side="top"
              align="end"
              id={modelListId}
              role="listbox"
              aria-label="Model"
              onKeyDown={onModelKeyDown}
              className="animate-fade-in"
              style={{
                width: 260,
                border: '1px solid var(--border-bright)',
                borderRadius: 8,
                background: 'var(--surface)',
                boxShadow: 'var(--elev-3)',
                padding: 6,
              }}
            >
              <div className="px-2 py-1 text-[11px]" style={{ color: 'var(--text-3)' }}>Model</div>
              {acp?.modelsLoading && <div role="status" className="px-2 py-1 text-[11px]" style={{ color: 'var(--accent)' }}>Discovering models…</div>}
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
                      className={`uam-menu-select__option w-full grid gap-0.5 text-left px-2 py-2${selected ? ' is-selected' : ''}`}
                      style={{
                        minHeight: 52,
                        borderRadius: 6,
                        color: selected ? 'var(--text)' : 'var(--text-2)',
                        boxShadow: index === modelFocusIndex ? 'inset 0 0 0 1px var(--accent)' : 'none',
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
            </ViewportMenu>
          )}
        </div>
        <div ref={settingsMenuRef} className="relative">
          <button
            ref={settingsTriggerRef}
            type="button"
            title="Settings"
            aria-label="Chat settings"
            aria-expanded={settingsOpen}
            onClick={onToggleSettings}
            className="uam-composer-action inline-flex items-center gap-1.5 px-2"
            style={{
              ...chipStyle,
              color: settingsOpen ? 'var(--text)' : 'var(--text-2)',
              borderColor: settingsOpen ? 'var(--border-bright)' : 'var(--border)',
            }}
          >
            <Settings2 size={14} aria-hidden />
          </button>
          {settingsOpen && (
            <ViewportMenu
              anchorRef={settingsTriggerRef}
              side="top"
              align="end"
              className="animate-fade-in"
              style={{
                width: 250,
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
                {hasReasoningEffort && (
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
                  <span style={{ color: 'var(--text-3)' }}>Memory</span>
                  <span>{MEMORY_LEVEL_OPTIONS.find((option) => option.id === memoryLevel)?.label ?? 'Strict'}</span>
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
            </ViewportMenu>
          )}
        </div>
        <button type="button" title={dictationLabel} aria-label={dictationLabel} aria-pressed="false" data-dictation-state="idle" onClick={onToggleDictation} disabled={!dictationAvailable} className="uam-composer-action uam-dictation-button h-[30px] text-xs font-semibold inline-flex items-center justify-center gap-1.5 px-2" style={{ borderRadius: 7, opacity: dictationAvailable ? 1 : 0.55 }}>
          <Mic size={15} aria-hidden />
        </button>
      </div>
      </>}
        {running && !canSend ? (
          <button type="button" title="Stop runtime" aria-label="Stop runtime" onClick={onStopRuntime} className="uam-composer-action h-[30px] w-[34px] text-xs font-semibold inline-flex items-center justify-center" style={{ borderRadius: 7, border: '1px solid color-mix(in srgb, var(--red) 46%, var(--border-bright))', background: 'color-mix(in srgb, var(--red) 14%, var(--surface))', color: 'var(--red)' }}><Square size={11} fill="currentColor" aria-hidden /></button>
        ) : (
        <button
          type="submit"
          title={running ? 'Queue prompt' : 'Send prompt'}
          aria-label={running ? 'Queue prompt' : 'Send prompt'}
          disabled={!canSend}
          className="uam-composer-action h-[30px] w-[34px] text-xs font-semibold inline-flex items-center justify-center"
          style={{
            borderRadius: 7,
            border: '1px solid color-mix(in srgb, var(--accent) 64%, var(--border-bright))',
            background: canSend ? 'var(--accent)' : 'var(--surface-up)',
            color: canSend ? '#fff' : 'var(--text-3)',
            boxShadow: canSend ? '0 8px 18px color-mix(in srgb, var(--accent) 20%, transparent)' : 'none',
          }}
        >
          <ComposerIcon name="send" size={15} />
        </button>
        )}
    </div>
  )
}
