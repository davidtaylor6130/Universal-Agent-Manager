// ComposerToolbar: message input toolbar with model/mode pickers and
// ComposerIcon SVG sprite. Extracted from ChatView.tsx (MO-3).

import { KeyboardEvent as ReactKeyboardEvent, RefObject, type ReactNode, useEffect, useLayoutEffect, useId, useRef, useState } from 'react'
import { Folder, SquarePen, GitBranch, ArrowUp, SquareTerminal, Plus, Target, ClipboardList, Cpu, Brain, Shield, ShieldAlert, ShieldCheck, Sparkles, Mic, MousePointer2, Square, X, Check } from 'lucide-react'
import type { AcpBinding, AcpConfigOption } from '../../store/useAppStore'
import type { Goal } from '../../types/goal'
import type { Provider } from '../../types/provider'
import {
  COPILOT_CLI_PROVIDER_ID,
  OPENCODE_CLI_PROVIDER_ID,
  providerCapabilities,
  providerShortName,
} from '../../utils/providerMetadata'
import {
  buildCodexReasoningOptions,
  buildCodexSpeedOptions,
  CODEX_SPEED_INHERIT_ID,
  buildModelOptions,
  modelOptionFor,
  reasoningEffortForModel,
  selectedRuntimeModel,
} from './modelOptions'
import { MenuSelect, ViewportMenu } from '../ui'
import { MEMORY_LEVEL_OPTIONS, type MemoryLevel } from '../../types/memory'
import { ProviderLogo } from '../shared/ProviderLogo'
import './composer.css'

export type ComposerIconName = 'editor' | 'folder' | 'git-tree' | 'markdown' | 'plus' | 'send' | 'terminal'
export type DictationState = 'idle' | 'starting' | 'listening' | 'stopping'

export const PERMISSION_MODES = [
  { id: 'default', name: 'Default', description: 'Ask before commands and file changes.' },
  { id: 'acceptEdits', name: 'Accept Edits', description: 'Automatically approve workspace file edits.' },
  { id: 'yolo', name: 'YOLO', description: 'Automatically approve every permission request.' },
  { id: 'aiReview', name: 'AI Review', description: 'Ask a configured isolated reviewer; failures and uncertainty come back to you.' },
] as const

export type CommandSafetyTier = 'off' | 'acceptEdits' | 'aiReview' | 'yolo'

export function permissionModeForTier(tier: CommandSafetyTier): 'default' | 'acceptEdits' | 'yolo' | 'aiReview' {
  return tier === 'off' ? 'default' : tier === 'acceptEdits' ? 'acceptEdits' : tier === 'yolo' ? 'yolo' : 'aiReview'
}

export function acpRuntimeBlocksControlChanges(acp?: AcpBinding | null): boolean {
  return Boolean(
    acp?.processing ||
    acp?.lifecycleState === 'waitingPermission' ||
    acp?.lifecycleState === 'waitingUserInput'
  )
}

export function providerConfigVariantOptions(acp: AcpBinding | undefined, providerId: string): AcpConfigOption[] {
  if (providerId !== OPENCODE_CLI_PROVIDER_ID) return []
  return (acp?.configOptions ?? []).filter((option) =>
    ['effort', 'thought_level'].includes(option.id.toLowerCase()) && option.options.length > 0
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
  if (id === 'aiReview') return <Sparkles size={size} />
  if (id === 'yolo') return <ShieldAlert size={size} />
  if (id === 'acceptEdits') return <SquarePen size={size} />
  if (id === 'plan') return <ClipboardList size={size} />
  return <ShieldCheck size={size} />
}

/** Compact composer controls reuse the shared keyboard-accessible option menu. */
function ComposerChoice({ icon, chipLabel, ...props }: Parameters<typeof MenuSelect>[0] & { icon: ReactNode; chipLabel?: string }) {
  return <div className="uam-composer-choice" data-mode-chip={chipLabel}>
    <MenuSelect {...props} onChange={(value) => { if (!props.disabled) props.onChange(value) }} options={props.options.map((option) => ({ ...option, icon: option.icon ?? icon }))} />
  </div>
}

function ActiveModeChip({ label, compactLabel = label, icon, onClear }: { label: string; compactLabel?: string; icon: ReactNode; onClear?: () => void }) {
  return onClear
    ? <ComposerChoice label={label} chipLabel={label} value="on" icon={icon} options={[{ value: 'on', label: compactLabel }, { value: 'off', label: 'Off' }]} onChange={(value) => { if (value === 'off') onClear() }} />
    : <span data-mode-chip={label} className="inline-flex h-[26px] shrink-0 items-center gap-1.5 px-2 text-xs" title={label}>{icon}<span>{compactLabel}</span></span>
}

/** Selected agent precedes the draft without becoming part of submitted text. */
export function ComposerAgentSelector({ agents, agentId, nextTurn, onSelectUamAgent }: {
  agents: Array<{ id: string; name: string; description?: string }>
  agentId: string
  nextTurn: boolean
  onSelectUamAgent: (agentId: string) => void
}) {
  const agentRef = useRef<HTMLDivElement>(null)
  useLayoutEffect(() => {
    const element = agentRef.current
    const row = element?.parentElement
    if (!element || !row) return
    const measure = () => row.style.setProperty('--agent-prefix-width', `${element.getBoundingClientRect().width}px`)
    measure()
    const observer = typeof ResizeObserver === 'undefined' ? null : new ResizeObserver(measure)
    observer?.observe(element)
    return () => {
      observer?.disconnect()
      row.style.removeProperty('--agent-prefix-width')
    }
  }, [agentId, agents])
  const selected = agents.find((agent) => agent.id === agentId)
  const name = selected?.name ?? agentId
  const hue = Array.from(agentId).reduce((hash, character) => (hash * 31 + character.codePointAt(0)!) % 360, 0)
  return <div ref={agentRef} className="uam-composer-agent" style={{ color: `hsl(${hue} 70% 72%)` }}>
    <MenuSelect
      label={`${nextTurn ? 'Next turn agent' : 'UAM agent'}: ${name}`}
      value={agentId}
      options={(selected ? agents : [{ id: agentId, name }, ...agents]).map((agent) => ({ value: agent.id, label: agent.name, description: agent.description }))}
      onChange={onSelectUamAgent}
    />
    <span className="uam-composer-agent-separator" aria-hidden />
  </div>
}

export function ComposerToolbar({
  acp,
  provider,
  providers,
  providerId,
  canChangeProvider,
  canSend,
  runtimeStatusLabel,
  runtimeStatusColor,
  modelId,
  reviewerModelId,
  includeDefaultModel,
  session,
  reasoningEffort,
  serviceTier,
  serviceTierExplicit,
  providerModeId,
  featurePreference,
  uamAgentId,
  uamAgentNextTurn,
  permissionModeId,
  permissionsManagedByUam,
  permissionControlsDisabled,
  providerModes,
  uamAgents,
  computerUseMode,
  memoryLevel,
  defaultMemoryLevel,
  memoryChipVisible,
  smallModelMode,
  modelOpen,
  modelMenuRef,
  onToggleModel,
  onSelectProvider,
  onSelectModel,
  onSelectReviewerModel,
  onSelectReasoning,
  onSelectSpeed,
  onSelectConfigOption,
  onSelectProviderMode,
  onSelectUamAgent,
  onSelectPermissionMode,
  onToggleComputerUseMode,
  onOpenComputerUse,
  onSelectMemoryLevel,
  onClearMemoryLevel,
  onToggleSmallModelMode,
  goalArmed,
  goalActive,
  goalPaused,
  defaultGoalTokenBudget,
  onToggleGoal,
  onSetDefaultGoalTokenBudget,
  onStopRuntime,
  onAttachFile,
  onOpenMarkdownStore,
  workspaceControl,
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
  canChangeProvider: boolean
  canSend: boolean
  runtimeStatusLabel: string
  runtimeStatusColor: string
  modelId?: string
  reviewerModelId?: string
  includeDefaultModel?: boolean
  session: { id: string }
  reasoningEffort?: string
  serviceTier?: string
  serviceTierExplicit?: boolean
  providerModeId?: string
  featurePreference: 'uam' | 'provider'
  uamAgentId: string
  uamAgentNextTurn: boolean
  permissionModeId: string
  permissionsManagedByUam: boolean
  permissionControlsDisabled: boolean
  providerModes: Array<{ id: string; name: string; description?: string }>
  uamAgents: Array<{ id: string; name: string; description?: string }>
  computerUseMode: boolean
  memoryLevel: MemoryLevel
  defaultMemoryLevel: MemoryLevel
  memoryChipVisible: boolean
  smallModelMode: boolean
  modelOpen: boolean
  modelMenuRef: RefObject<HTMLDivElement>
  onToggleModel: () => void
  onSelectProvider: (providerId: string) => void
  onSelectModel: (modelId: string) => void
  onSelectReviewerModel: (modelId: string) => void
  onSelectReasoning: (reasoningEffort: string) => void
  onSelectSpeed: (serviceTier: string) => void
  onSelectConfigOption: (configId: string, value: string) => void
  onSelectProviderMode: (modeId: string) => void
  onSelectUamAgent: (agentId: string) => void
  onSelectPermissionMode: (modeId: string) => void
  onToggleComputerUseMode: () => void
  onOpenComputerUse: () => void
  onSelectMemoryLevel: (level: MemoryLevel) => void
  onClearMemoryLevel: () => void
  onToggleSmallModelMode: () => void
  goalArmed: boolean
  goalActive: boolean
  goalPaused: boolean
  defaultGoalTokenBudget: number
  onToggleGoal: () => void
  onSetDefaultGoalTokenBudget: (value: number) => void
  onStopRuntime: () => void
  onAttachFile: () => void
  onOpenMarkdownStore: () => void
  workspaceControl?: ReactNode
  dictationState: DictationState
  dictationError?: string
  dictationElapsedSeconds: number
  dictationAvailable: boolean
  onToggleDictation: () => void
}) {
  const caps = providerCapabilities(providerId, provider)
  const modelOptions = buildModelOptions(acp, modelId ?? '', provider, providerId, includeDefaultModel)
  const currentModel = modelOptionFor(modelOptions, modelId)
  const reviewerModelOptions = buildModelOptions(acp, reviewerModelId ?? modelId ?? '', provider, providerId, includeDefaultModel)
  const currentReviewerModel = modelOptionFor(reviewerModelOptions, reviewerModelId || modelId)
  const runtimeSupportsReasoning = (selectedRuntimeModel(acp, currentModel.id)?.supportedReasoningEfforts?.length ?? 0) > 0
  const reasoningOptions = caps.hasReasoningEffort || runtimeSupportsReasoning
    ? buildCodexReasoningOptions(
        acp,
        currentModel.id,
        reasoningEffort ?? '',
        providerId === COPILOT_CLI_PROVIDER_ID ? caps.reasoningOptions.map((option) => option.id) : undefined
      )
    : []
  const hasReasoningEffort = reasoningOptions.length > 0
  const speedExplicit = serviceTierExplicit ?? (serviceTier ?? '') !== ''
  const speedOptions = caps.hasServiceTier ? buildCodexSpeedOptions(acp, currentModel.id, serviceTier ?? '') : []
  const currentReasoning = modelOptionFor(
    reasoningOptions,
    reasoningEffortForModel(acp, currentModel.id, reasoningEffort, providerId === COPILOT_CLI_PROVIDER_ID)
  )
  const currentSpeed = modelOptionFor(speedOptions, speedExplicit ? serviceTier : CODEX_SPEED_INHERIT_ID)
  const variantOptions = providerConfigVariantOptions(acp, providerId)
  const goalPairLocked = featurePreference === 'uam' && smallModelMode && goalActive
  const modelDisabled = acpRuntimeBlocksControlChanges(acp) || goalPairLocked
  const architectModelsInOptions = featurePreference === 'uam' && smallModelMode
  const selectorDisabled = architectModelsInOptions || (modelDisabled && !canChangeProvider)
  const providerName = providerShortName(provider, providerId)
  const providerPlanActive = providerModeId === 'plan'
  const memoryDisabled = false
  const permissionModes = permissionsManagedByUam
    ? PERMISSION_MODES.filter((mode) => mode.id !== 'acceptEdits' || caps.hasAcceptEditsMode)
    : [{ id: 'provider', name: 'Provider managed', description: 'Respond to permission prompts in the provider interface.' }]
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
  const optionsMenuId = useId()
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
      if (!modelOptionRefs.current.includes(document.activeElement as HTMLButtonElement)) return
      if (modelDisabled) return
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
    document.addEventListener('mousedown', onDown)
    return () => {
      document.removeEventListener('mousedown', onDown)
    }
  }, [optionsOpen])
  const chipStyle = {
    height: 26,
    borderRadius: 6,
    border: '1px solid transparent',
    background: 'color-mix(in srgb, var(--surface-up) 72%, transparent)',
    color: 'var(--text-2)',
  }
  const iconChipStyle = {
    ...chipStyle,
    width: 30,
    justifyContent: 'center',
  }

  return (
    <div
      className="uam-composer-toolbar flex items-center gap-2 px-2 py-2 text-xs"
      style={{
        borderTop: '1px solid var(--border)',
        color: 'var(--text-2)',
      }}
    >
      <div ref={optionsRef} className="relative shrink-0">
        <button
          ref={optionsTriggerRef}
          type="button"
          title="Add files, goal, and options"
          aria-label="Options"
          aria-haspopup="menu"
          aria-expanded={optionsOpen}
          aria-controls={optionsOpen ? optionsMenuId : undefined}
          onClick={() => setOptionsOpen((v) => !v)}
          className="uam-composer-action inline-flex items-center"
          style={{
            ...iconChipStyle,
            color: optionsOpen ? 'var(--text)' : 'var(--text-2)',
            background: optionsOpen ? 'var(--surface-high)' : iconChipStyle.background,
          }}
        >
          <Plus size={16} aria-hidden />
        </button>
        {optionsOpen && (
          <ViewportMenu
            ref={optionsMenuRef}
            anchorRef={optionsTriggerRef}
            side="top"
            id={optionsMenuId}
            role="menu"
            aria-label="Composer options"
            onRequestClose={() => setOptionsOpen(false)}
            className="flex flex-col gap-1.5 animate-fade-in"
            style={{
              minWidth: 210, padding: 8,
              border: '1px solid var(--border-bright)', borderRadius: 8,
              background: 'var(--surface)', boxShadow: 'var(--elev-3)',
            }}
          >
            <button
              type="button"
              role="menuitem"
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
              role="menuitem"
              title={goalActive ? 'Pause goal mode' : goalPaused ? 'Resume goal mode' : goalArmed ? 'Next message will become the goal' : 'Use the next message as a goal'}
              aria-pressed={goalActive || goalArmed}
              onClick={() => { setOptionsOpen(false); onToggleGoal() }}
              className="uam-choice-button inline-flex items-center gap-1.5 px-2 w-full justify-start"
              style={{ ...chipStyle, borderColor: goalActive || goalArmed ? 'color-mix(in srgb, var(--purple) 55%, var(--border))' : 'var(--border)', background: goalActive || goalArmed ? 'color-mix(in srgb, var(--purple) 16%, var(--surface))' : chipStyle.background, color: goalActive || goalArmed ? 'var(--text)' : 'var(--text-2)', opacity: modelDisabled ? 0.55 : 1 }}
            >
              <Target size={13} aria-hidden style={{ color: goalActive || goalArmed ? 'var(--purple)' : 'var(--text-3)' }} />
              <span>{goalArmed ? 'Goal: next message' : 'Goal'}</span>
            </button>
            <label className="grid gap-1 px-1 py-1 text-xs">
              <span style={{ color: 'var(--text-3)' }}>Goal token budget</span>
              <input
                type="number"
                min={0}
                value={defaultGoalTokenBudget || ''}
                placeholder="Unlimited"
                onChange={(event) => onSetDefaultGoalTokenBudget(parseInt(event.target.value || '0', 10))}
                className="w-full px-2 py-1 text-xs"
                style={{ border: '1px solid var(--border)', borderRadius: 6, background: 'var(--bg)', color: 'var(--text)', outline: 'none' }}
              />
            </label>
            <button
              type="button"
              role="menuitem"
              title="Configure computer use"
              aria-haspopup="dialog"
              aria-pressed={computerUseMode}
              onClick={() => { setOptionsOpen(false); onOpenComputerUse() }}
              className="uam-choice-button inline-flex items-center gap-1.5 px-2 w-full justify-start"
              style={{ ...chipStyle, borderColor: computerUseMode ? 'color-mix(in srgb, var(--accent) 55%, var(--border))' : 'var(--border)', background: computerUseMode ? 'color-mix(in srgb, var(--accent) 16%, var(--surface))' : chipStyle.background, color: computerUseMode ? 'var(--text)' : 'var(--text-2)' }}
            >
              <MousePointer2 size={13} aria-hidden style={{ color: computerUseMode ? 'var(--accent)' : 'var(--text-3)' }} />
              <span>Computer use…</span>
            </button>
            {(hasReasoningEffort || caps.hasServiceTier || variantOptions.length > 0) && (
              <>
                <div className="mt-1 border-t" style={{ borderColor: 'var(--border)' }} />
                <div className="px-1 pb-0.5 text-xs font-medium uppercase tracking-wide" style={{ color: 'var(--text-3)' }}>Model</div>
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
                {variantOptions.map((option) => (
                  <div key={option.id} className="grid gap-1">
                    <div className="px-1 text-xs" style={{ color: 'var(--text-3)' }}>{option.name || option.id}</div>
                    <MenuSelect
                      label={option.name || option.id}
                      value={option.currentValue}
                      options={option.options.map((choice) => ({ value: choice.value, label: choice.name || choice.value, description: choice.description }))}
                      onChange={(value) => onSelectConfigOption(option.id, value)}
                      disabled={modelDisabled}
                    />
                  </div>
                ))}
              </>
            )}
            {featurePreference === 'uam' && <>
			  <div className="mt-1 border-t" style={{ borderColor: 'var(--border)' }} />
			  <div className="px-1 pb-0.5 text-xs font-medium uppercase tracking-wide" style={{ color: 'var(--text-3)' }}>UAM agent</div>
			  <MenuSelect
				label={uamAgentNextTurn ? "Next turn agent" : "UAM agent"}
				value={uamAgentId}
				options={uamAgents.map((agent) => ({
				  value: agent.id,
				  label: agent.name,
				  description: agent.description,
				  icon: permissionModeIcon(agent.id === 'plan' ? 'plan' : 'default'),
				}))}
				onChange={onSelectUamAgent}
			  />
			</>}
			{featurePreference === 'provider' && providerModes.length > 0 && <>
			  <div className="mt-1 border-t" style={{ borderColor: 'var(--border)' }} />
			  <div className="px-1 pb-0.5 text-xs font-medium uppercase tracking-wide" style={{ color: 'var(--text-3)' }}>Provider mode</div>
			  <MenuSelect
				label="Provider mode"
				value={providerModeId ?? 'default'}
				options={providerModes.map((mode) => ({
				  value: mode.id,
				  label: mode.name,
				  description: mode.description,
				  icon: permissionModeIcon(mode.id),
				}))}
				onChange={onSelectProviderMode}
				disabled={modelDisabled}
			  />
			</>}
			<div className="mt-1 border-t" style={{ borderColor: 'var(--border)' }} />
            <div className="px-1 pb-0.5 text-xs font-medium uppercase tracking-wide" style={{ color: 'var(--text-3)' }}>Permissions</div>
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
              disabled={!permissionsManagedByUam || permissionControlsDisabled}
            />
            <div className="mt-1 border-t" style={{ borderColor: 'var(--border)' }} />
            <MenuSelect
              label="Memory"
              value={memoryLevel}
              options={MEMORY_LEVEL_OPTIONS.map((option) => ({ value: option.id, label: `Memory ${option.label}`, description: option.detail }))}
              onChange={(level) => onSelectMemoryLevel(level as MemoryLevel)}
              disabled={memoryDisabled}
            />
            {featurePreference === 'uam' && <button
              type="button"
              role="menuitem"
              title="Plan first, then run and review one verified step at a time"
              aria-pressed={smallModelMode}
              onClick={onToggleSmallModelMode}
              disabled={modelDisabled}
              className="uam-choice-button inline-flex items-center gap-1.5 px-2 w-full justify-start"
              style={{ ...chipStyle, borderColor: smallModelMode ? 'color-mix(in srgb, var(--accent) 55%, var(--border))' : 'var(--border)', background: smallModelMode ? 'var(--accent-dim)' : chipStyle.background, color: smallModelMode ? 'var(--text)' : 'var(--text-2)', opacity: modelDisabled ? 0.55 : 1 }}
            >
              <Cpu size={13} aria-hidden />
              <span>Architect + worker {smallModelMode ? 'on' : 'off'}</span>
            </button>}
            {featurePreference === 'uam' && smallModelMode && (
              <div className="grid gap-2">
                <div className="grid gap-1">
                  <div className="px-1 text-xs" style={{ color: 'var(--text-3)' }}>{goalPairLocked ? 'Worker model · locked for active Goal' : 'Worker model'}</div>
                  <MenuSelect
                    label="Worker model"
                    value={currentModel.id}
                    options={modelOptions.map((option) => ({ value: option.id, label: option.label, description: option.detail }))}
                    onChange={onSelectModel}
                    disabled={modelDisabled}
                  />
                </div>
                <div className="grid gap-1">
                  <div className="px-1 text-xs" style={{ color: 'var(--text-3)' }}>{goalPairLocked ? 'Reviewer model · locked for active Goal' : 'Reviewer model'}</div>
                <MenuSelect
                  label="Reviewer model"
                  value={currentReviewerModel.id}
                  options={reviewerModelOptions.map((option) => ({ value: option.id, label: option.label, description: option.detail }))}
                  onChange={onSelectReviewerModel}
                  disabled={modelDisabled}
                />
                </div>
              </div>
            )}
            <button
              type="button"
              role="menuitem"
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
      {workspaceControl}
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
      <div className="uam-composer-status-chips flex min-w-0 flex-1 items-center gap-2 overflow-x-auto">
        {goalArmed && <ActiveModeChip label="Goal: next message" compactLabel="Goal" icon={<Target size={12} aria-hidden style={{ color: 'var(--purple)' }} />} onClear={onToggleGoal} />}
        {computerUseMode && <ActiveModeChip label="Computer use" compactLabel="Computer" icon={<MousePointer2 size={12} aria-hidden style={{ color: 'var(--accent)' }} />} onClear={onToggleComputerUseMode} />}
        {featurePreference === 'provider' && providerPlanActive && <ComposerChoice label="Provider mode" chipLabel="Provider Plan" value={providerModeId ?? 'default'} icon={<ClipboardList size={12} aria-hidden />} options={providerModes.map((mode) => ({ value: mode.id, label: mode.name, description: mode.description }))} onChange={onSelectProviderMode} disabled={modelDisabled} />}
        <ComposerChoice
          label="Permissions"
          chipLabel={permissionModeId === 'default' ? 'Permissions: Default' : permissionModes.find((mode) => mode.id === permissionModeId)?.name}
          value={permissionModeId}
          icon={permissionModeIcon(permissionModeId, 12)}
          options={permissionModes.map((mode) => ({ value: mode.id, label: mode.name, description: mode.description, icon: permissionModeIcon(mode.id, 12) }))}
          onChange={onSelectPermissionMode}
          disabled={!permissionsManagedByUam || permissionControlsDisabled}
        />
        {memoryChipVisible && <ComposerChoice
          label="Memory"
          chipLabel={`Memory ${MEMORY_LEVEL_OPTIONS.find((option) => option.id === memoryLevel)?.label ?? memoryLevel}`}
          value={memoryLevel}
          icon={<Brain size={12} aria-hidden />}
          options={[...MEMORY_LEVEL_OPTIONS.map((option) => ({ value: option.id, label: option.label, description: option.detail })), { value: 'reset', label: 'Use default', description: MEMORY_LEVEL_OPTIONS.find((option) => option.id === defaultMemoryLevel)?.label }]}
          onChange={(level) => level === 'reset' ? onClearMemoryLevel() : onSelectMemoryLevel(level as MemoryLevel)}
          disabled={memoryDisabled}
        />}
        {featurePreference === 'uam' && smallModelMode && <ActiveModeChip label="Architect + worker" compactLabel="Architect" icon={<Cpu size={12} aria-hidden style={{ color: 'var(--accent)' }} />} onClear={modelDisabled ? undefined : onToggleSmallModelMode} />}
        {featurePreference === 'uam' && smallModelMode && <ActiveModeChip label={`Reviewer: ${currentReviewerModel.label}`} compactLabel={`Review: ${currentReviewerModel.shortLabel}`} icon={<Sparkles size={12} aria-hidden style={{ color: 'var(--purple)' }} />} />}
        {hasReasoningEffort && <ComposerChoice label="Reasoning" chipLabel={`Reasoning: ${currentReasoning.label}`} value={currentReasoning.id} icon={<Cpu size={12} aria-hidden />} options={reasoningOptions.map((option) => ({ value: option.id, label: option.label, description: option.detail }))} onChange={onSelectReasoning} disabled={modelDisabled} />}
        {variantOptions.map((option) => <ComposerChoice key={option.id} label={option.name || option.id} value={option.currentValue} icon={<Cpu size={12} aria-hidden />} options={option.options.map((choice) => ({ value: choice.value, label: choice.name || choice.value, description: choice.description }))} onChange={(value) => onSelectConfigOption(option.id, value)} disabled={modelDisabled} />)}
        {speedExplicit && <ComposerChoice label="Speed" chipLabel={`Speed: ${currentSpeed.label}`} value={currentSpeed.id} icon={<Sparkles size={12} aria-hidden />} options={speedOptions.map((option) => ({ value: option.id, label: option.label, description: option.detail }))} onChange={onSelectSpeed} disabled={modelDisabled} />}

      </div>
      <div className="flex shrink-0 items-center gap-2">
        <div ref={modelMenuRef} className="relative">
          <button
            ref={modelTriggerRef}
            type="button"
            title={`${architectModelsInOptions ? 'Worker model is managed in Options' : 'Model'} · ${providerName} · ${currentModel.label} · ${runtimeStatusLabel}`}
            aria-label={architectModelsInOptions ? 'Worker model is managed in Options' : 'Select provider and model'}
            onClick={onToggleModel}
            disabled={selectorDisabled}
            aria-haspopup="menu"
            aria-expanded={modelOpen}
            aria-controls={modelOpen ? modelListId : undefined}
            onKeyDown={onModelTriggerKeyDown}
            className="uam-provider-model-trigger uam-composer-action inline-flex min-w-0 max-w-[150px] items-center gap-1.5 px-2"
            style={{
              ...chipStyle,
              borderRadius: 7,
              color: modelOpen ? 'var(--text)' : 'var(--text-2)',
              background: modelOpen ? 'var(--surface-high)' : chipStyle.background,
              opacity: selectorDisabled ? 0.55 : 1,
            }}
          >
            <ProviderLogo providerId={providerId} />
            <span className="truncate" style={{ color: 'var(--text)' }}>{currentModel.shortLabel}</span>
            <span aria-hidden className="h-1.5 w-1.5 shrink-0 rounded-full" style={{ background: runtimeStatusColor }} />
          </button>
          {modelOpen && !selectorDisabled && (
            <ViewportMenu
              anchorRef={modelTriggerRef}
              side="top"
              align="end"
              id={modelListId}
              role="menu"
              aria-label="Provider and model"
              manageFocus={false}
              onKeyDown={onModelKeyDown}
              className="animate-fade-in"
              style={{
                width: 280,
                border: '1px solid var(--border-bright)',
                borderRadius: 8,
                background: 'var(--surface)',
                boxShadow: 'var(--elev-3)',
                padding: 6,
              }}
            >
              {providers.length > 1 && (
                <div role="group" aria-label="Provider">
                  <div className="px-2 py-1 text-xs" style={{ color: 'var(--text-3)' }}>Provider</div>
                  {providers.map((candidate) => {
                    const selected = candidate.id === providerId
                    const disabled = !selected && !canChangeProvider
                    return (
                      <button
                        key={candidate.id}
                        type="button"
                        role="menuitemradio"
                        aria-checked={selected}
                        disabled={disabled}
                        onClick={() => onSelectProvider(candidate.id)}
                        className={`uam-menu-select__option flex w-full items-center gap-2 rounded-md px-2 py-2 text-left${selected ? ' is-selected' : ''}`}
                        style={{ color: selected ? 'var(--text)' : 'var(--text-2)', opacity: disabled ? 0.5 : 1 }}
                      >
                        <ProviderLogo providerId={candidate.id} />
                        <span className="flex-1">{providerShortName(candidate, candidate.id)}</span>
                        {selected && <Check size={13} aria-hidden style={{ color: 'var(--accent)' }} />}
                      </button>
                    )
                  })}
                  <div className="my-1 border-t" style={{ borderColor: 'var(--border)' }} />
                </div>
              )}
              <div className="px-2 py-1 text-xs" style={{ color: 'var(--text-3)' }}>{featurePreference === 'uam' && smallModelMode ? 'Worker model' : 'Model'}</div>
              {acp?.modelsLoading && <div role="status" className="px-2 py-1 text-xs" style={{ color: 'var(--accent)' }}>Discovering models…</div>}
              <div data-testid="model-options" style={{ maxHeight: 520, overflowY: 'auto' }}>
                {modelOptions.map((option, index) => {
                  const selected = option.id === currentModel.id
                  const hideRepeatedCopilotDetail = providerId === COPILOT_CLI_PROVIDER_ID && option.detail.trim().toLowerCase() === option.label.trim().toLowerCase()
                  return (
                    <button
                      key={option.id || 'default'}
                      ref={(element) => { modelOptionRefs.current[index] = element }}
                      type="button"
                      role="menuitemradio"
                      aria-checked={selected}
                      disabled={modelDisabled}
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
                        opacity: modelDisabled ? 0.5 : 1,
                      }}
                    >
                      <span className="flex items-center gap-2">
                        <span className="flex-1">{option.label}</span>
                        {selected && <span style={{ color: 'var(--green)', fontSize: 10 }}>●</span>}
                      </span>
                      {!hideRepeatedCopilotDetail && <span className="text-xs" style={{ color: 'var(--text-3)' }}>{option.detail}</span>}
                    </button>
                  )
                })}
              </div>
              <div className="mt-1 flex items-center gap-2 border-t px-2 pt-2 text-xs" style={{ borderColor: 'var(--border)', color: 'var(--text-3)' }}>
                <span aria-hidden className="h-1.5 w-1.5 rounded-full" style={{ background: runtimeStatusColor }} />
                <span>{runtimeStatusLabel}</span>
                {acp?.agentInfo?.title && <span className="ml-auto truncate">{acp.agentInfo.title}</span>}
              </div>
            </ViewportMenu>
          )}
        </div>
        <button type="button" title={dictationLabel} aria-label={dictationLabel} aria-pressed="false" data-dictation-state="idle" onClick={onToggleDictation} disabled={!dictationAvailable} className="uam-composer-action uam-composer-secondary-control uam-dictation-button h-[30px] text-xs font-semibold inline-flex items-center justify-center gap-1.5 px-2" style={{ borderRadius: 7, opacity: dictationAvailable ? 1 : 0.55 }}>
          <Mic size={15} aria-hidden />
        </button>
      </div>
      </>}
        {running && !canSend ? (
          <button type="button" title="Stop runtime" aria-label="Stop runtime" onClick={onStopRuntime} className="uam-composer-action h-[30px] w-[34px] shrink-0 text-xs font-semibold inline-flex items-center justify-center" style={{ borderRadius: 7, border: '1px solid color-mix(in srgb, var(--red) 46%, var(--border-bright))', background: 'color-mix(in srgb, var(--red) 14%, var(--surface))', color: 'var(--red)' }}><Square size={11} fill="currentColor" aria-hidden /></button>
        ) : (
        <button
          type="submit"
          title={running ? 'Queue prompt' : 'Send prompt'}
          aria-label={running ? 'Queue prompt' : 'Send prompt'}
          disabled={!canSend}
          className="uam-composer-action h-[30px] w-[34px] shrink-0 text-xs font-semibold inline-flex items-center justify-center"
          style={{
            borderRadius: 7,
            border: '1px solid color-mix(in srgb, var(--accent) 64%, var(--border-bright))',
            background: canSend ? 'var(--accent)' : 'var(--surface-up)',
            color: canSend ? '#fff' : 'var(--text-3)',
          }}
        >
          <ComposerIcon name="send" size={15} />
        </button>
        )}
    </div>
  )
}
