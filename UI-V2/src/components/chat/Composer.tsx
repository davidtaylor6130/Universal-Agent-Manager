import { COMPUTER_USE_ENABLED, COPILOT_ENABLED } from '../../config/buildFeatures'
import { isCompanionContext } from '../../ipc/cefBridge'
// ComposerToolbar: message input toolbar with model/mode pickers and
// ComposerIcon SVG sprite. Extracted from ChatView.tsx (MO-3).
import { KeyboardEvent as ReactKeyboardEvent, RefObject, type ReactNode, type CSSProperties, useEffect, useLayoutEffect, useId, useRef, useState } from 'react'
import { Folder, Pencil, SquarePen, GitBranch, ArrowUp, SquareTerminal, Plus, Target, ClipboardList, Cpu, Brain, ShieldAlert, ShieldCheck, Sparkles, Mic, MousePointer2, Square, Paperclip, ChevronRight, CornerUpRight, ListPlus } from 'lucide-react'
import { useAppStore, type AcpBinding, type AcpConfigOption } from '../../store/useAppStore'
import type { Provider } from '../../types/provider'
import {
  COPILOT_CLI_PROVIDER_ID,
  GEMINI_CLI_PROVIDER_ID,
  normalizeCliProviderIdAlias,
  CODEX_CLI_PROVIDER_ID,
  OPENCODE_CLI_PROVIDER_ID,
  providerCapabilities,
  providerShortName,
} from '../../utils/providerMetadata'
import {
  buildProviderReasoningOptions,
  buildCodexSpeedOptions,
  CODEX_SPEED_INHERIT_ID,
  buildModelOptions,
  modelOptionFor,
  reasoningEffortForModel,
} from './modelOptions'
import { Button, IconButton, MenuSelect, Switch, Tooltip, ViewportMenu } from '../ui'
import { ProviderAllowanceIndicator } from './ProviderAllowanceIndicator'
import { MEMORY_LEVEL_OPTIONS, type MemoryLevel } from '../../types/memory'
import { ProviderLogo } from '../shared/ProviderLogo'
import { ModelPickerContent } from './ModelPickerContent'
import { CopilotAutoRoutingControl } from './CopilotAutoRoutingControl'
import './composer.css'

export type ComposerIconName = 'editor' | 'folder' | 'git-tree' | 'markdown' | 'plus' | 'send' | 'terminal'
export type DictationState = 'idle' | 'starting' | 'listening' | 'stopping'

export const PERMISSION_MODES = [
  { id: 'default', name: 'Default', description: 'Ask before commands and file changes.' },
  { id: 'acceptEdits', name: 'Accept edits', description: 'Automatically approve workspace file edits.' },
  { id: 'yolo', name: 'YOLO', description: 'Automatically approve every permission request.' },
  { id: 'aiReview', name: 'AI review', description: 'Ask a configured isolated reviewer; failures and uncertainty come back to you.' },
] as const

export type CommandSafetyTier = 'off' | 'acceptEdits' | 'aiReview' | 'yolo'

export function permissionModeForTier(tier: CommandSafetyTier): 'default' | 'acceptEdits' | 'yolo' | 'aiReview' {
  return tier === 'off' ? 'default' : tier === 'acceptEdits' ? 'acceptEdits' : tier === 'yolo' ? 'yolo' : 'aiReview'
}

export function acpRuntimeBlocksControlChanges(acp?: AcpBinding | null): boolean {
  return Boolean(
    acp?.processing ||
    (acp?.pendingDirectPrompts?.length ?? 0) > 0 ||
    (acp?.unsentDirectPrompts?.length ?? 0) > 0 ||
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
        style={{ fontSize: 'var(--fs-xs)', letterSpacing: 0 }}
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

/** Menu on/off row: the label and icon toggle the shared Switch; the Switch input is the menu item (role menuitemcheckbox) and its accessible name. */
function ComposerToggleRow({ id, icon, label, title, checked, disabled, onChange }: {
  id: string
  icon: ReactNode
  label: string
  title?: string
  checked: boolean
  disabled?: boolean
  onChange: () => void
}) {
  return <div className="uam-cmenu__item uam-cmenu__toggle" title={title}>
    <label htmlFor={id} className="uam-cmenu__toggle-label">{icon}<span className="uam-cmenu__text"><span>{label}</span></span></label>
    <Switch id={id} hideLabel label={label} role="menuitemcheckbox" checked={checked} disabled={disabled} onChange={onChange} />
  </div>
}

function ActiveModeChip({ label, compactLabel = label, icon, onClear }: { label: string; compactLabel?: string; icon: ReactNode; onClear?: () => void }) {
  return onClear
    ? <ComposerChoice label={label} chipLabel={label} value="on" icon={icon} options={[{ value: 'on', label: compactLabel }, { value: 'off', label: 'Off' }]} onChange={(value) => { if (value === 'off') onClear() }} />
    : <Tooltip label={label}><span data-mode-chip={label} className="inline-flex h-[26px] shrink-0 items-center gap-1.5 px-2 text-xs">{icon}<span>{compactLabel}</span></span></Tooltip>
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
    const textarea = row.querySelector('textarea')
    const followScroll = () => { element.style.transform = `translateY(${-(textarea?.scrollTop ?? 0)}px)` }
    followScroll()
    textarea?.addEventListener('scroll', followScroll, { passive: true })
    const measure = () => row.style.setProperty('--agent-prefix-width', `${element.getBoundingClientRect().width}px`)
    measure()
    const observer = typeof ResizeObserver === 'undefined' ? null : new ResizeObserver(measure)
    observer?.observe(element)
    return () => {
      observer?.disconnect()
      textarea?.removeEventListener('scroll', followScroll)
      row.style.removeProperty('--agent-prefix-width')
    }
  }, [agentId, agents])
  const selected = agents.find((agent) => agent.id === agentId)
  const name = selected?.name ?? agentId
  const hue = Array.from(agentId).reduce((hash, character) => (hash * 31 + character.codePointAt(0)!) % 360, 0)
  return <div ref={agentRef} className="uam-composer-agent" style={{ '--agent-hue': hue } as CSSProperties}>
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
  draftPending = false,
  acp,
  provider,
  providers,
  providerId,
  canChangeProvider,
  providerLockReason,
  providerSwitchUnlocked,
  canSend,
  editing = false,
  sendLabel,
  holdSend = false,
  runtimeStatusLabel,
  runtimeStatusColor,
  modelId,
  reviewerModelId,
  includeDefaultModel,
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
  computerUseAwaitingTarget,
  memoryLevel,
  defaultMemoryLevel,
  smallModelMode,
  modelOpen,
  modelMenuRef,
  onToggleModel,
  onSelectProviderModel,
  onRefreshModels,
  workspaceDirectory,
  executionHostId,
  onSelectModel,
  onSelectReviewerModel,
  onSelectReasoning,
  onSelectSpeed,
  onSelectConfigOption,
  onSelectProviderMode,
  onSelectUamAgent,
  onSelectPermissionMode,
  onToggleComputerUseMode,
  onSelectMemoryLevel,
  onToggleSmallModelMode,
  goalArmed,
  goalActive,
  goalPaused,
  onToggleGoal,
  onCancelTurn,
  onSendAlternate,
  onAttachFile,
  onOpenMarkdownStore,
  dictationState,
  dictationError,
  dictationElapsedSeconds,
  dictationAvailable,
  onToggleDictation,
}: {
  /** Composer holds unsent text; shown as a quiet "Draft" label, never as a sent state. */
  draftPending?: boolean
  acp?: AcpBinding
  provider: Provider
  providers: Provider[]
  providerId: string
  canChangeProvider: boolean
  providerLockReason?: string
  /** A disconnected recovery blocks model changes but must still reach the provider-switch warning. */
  providerSwitchUnlocked?: boolean
  canSend: boolean
  /** Composer is editing a sent message: the send button saves the edit. */
  editing?: boolean
  /** Overrides the send button's label, e.g. while it submits question answers. */
  sendLabel?: string
  /** Always show the (possibly disabled) send button, with Cancel turn beside it while the turn runs, e.g. while a question fills the composer. */
  holdSend?: boolean
  runtimeStatusLabel: string
  runtimeStatusColor: string
  modelId?: string
  reviewerModelId?: string
  includeDefaultModel?: boolean
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
  computerUseAwaitingTarget: boolean
  memoryLevel: MemoryLevel
  defaultMemoryLevel: MemoryLevel
  smallModelMode: boolean
  modelOpen: boolean
  modelMenuRef: RefObject<HTMLDivElement>
  onToggleModel: () => void
  onSelectProviderModel: (providerId: string, modelId: string) => void
  onRefreshModels?: () => Promise<boolean>
  workspaceDirectory: string
  executionHostId: string
  onSelectModel: (modelId: string) => void
  onSelectReviewerModel: (modelId: string) => void
  onSelectReasoning: (reasoningEffort: string) => void
  onSelectSpeed: (serviceTier: string) => void
  onSelectConfigOption: (configId: string, value: string) => void
  onSelectProviderMode: (modeId: string) => void
  onSelectUamAgent: (agentId: string) => void
  onSelectPermissionMode: (modeId: string) => void
  onToggleComputerUseMode: () => void
  onSelectMemoryLevel: (level: MemoryLevel) => void
  onToggleSmallModelMode: () => void
  goalArmed: boolean
  goalActive: boolean
  goalPaused: boolean
  onToggleGoal: () => void
  onCancelTurn: () => void
  /** Sends with the action the active-turn preference did not choose (Queue or Steer). */
  onSendAlternate?: () => void
  onAttachFile: () => void
  onOpenMarkdownStore: () => void
  dictationState: DictationState
  dictationError?: string
  dictationElapsedSeconds: number
  dictationAvailable: boolean
  onToggleDictation: () => void
}) {
  const modelVisibility = useAppStore((state) => state.providerChatDefaults[providerId])
  const caps = providerCapabilities(providerId, provider)
  const modelOptions = buildModelOptions(acp, modelId ?? '', provider, providerId, includeDefaultModel, modelVisibility)
  const currentModel = modelOptionFor(modelOptions, modelId)
  const reviewerModelOptions = buildModelOptions(acp, reviewerModelId ?? modelId ?? '', provider, providerId, includeDefaultModel, modelVisibility)
  const currentReviewerModel = modelOptionFor(reviewerModelOptions, reviewerModelId || modelId)
  const canonicalProviderId = normalizeCliProviderIdAlias(providerId)
  const reasoningOptions = buildProviderReasoningOptions(providerId, acp,
    canonicalProviderId === GEMINI_CLI_PROVIDER_ID ? modelId ?? '' : currentModel.id, reasoningEffort ?? '')
  const hasReasoningEffort = reasoningOptions.length > 0
  const speedExplicit = serviceTierExplicit ?? (serviceTier ?? '') !== ''
  const speedOptions = caps.hasServiceTier ? buildCodexSpeedOptions(acp, currentModel.id, serviceTier ?? '', providerId === CODEX_CLI_PROVIDER_ID) : []
  const currentReasoning = modelOptionFor(
    reasoningOptions,
    canonicalProviderId === GEMINI_CLI_PROVIDER_ID ? reasoningEffort : reasoningEffortForModel(acp, currentModel.id, reasoningEffort, canonicalProviderId === COPILOT_CLI_PROVIDER_ID)
  )
  const currentSpeed = modelOptionFor(speedOptions, speedExplicit ? serviceTier : CODEX_SPEED_INHERIT_ID)
  const variantOptions = providerConfigVariantOptions(acp, providerId)
  const goalPairLocked = featurePreference === 'uam' && smallModelMode && goalActive
  const modelDisabled = acpRuntimeBlocksControlChanges(acp) || goalPairLocked
  const architectModelsInOptions = featurePreference === 'uam' && smallModelMode
  const selectorDisabled = architectModelsInOptions || (modelDisabled && !canChangeProvider)
  const modelBlockReason = architectModelsInOptions ? 'Worker model is managed in Options'
    : goalPairLocked ? 'Goal mode locks the model pair'
    : acp?.lifecycleState === 'waitingPermission' ? 'Waiting for a permission answer'
    : acp?.lifecycleState === 'waitingUserInput' ? 'Waiting for your answer'
    : 'Finish the current turn to change models'
  const providerName = providerShortName(provider, providerId)
  const providerPlanActive = providerModeId === 'plan'
  const memoryDisabled = false
  const permissionModes = permissionsManagedByUam
    ? PERMISSION_MODES.filter((mode) => mode.id !== 'acceptEdits' || caps.hasAcceptEditsMode)
    : [{ id: 'provider', name: 'Ask in provider', description: 'Respond to permission prompts in the provider interface.' }]
  const running = Boolean(acp?.processing)
  const steerPreferred = useAppStore((state) => state.steerActiveTurnOnSend)
  // Saving an edit and submitting a composer question keep their own Send action.
  const alternateSendEligible = !editing && !holdSend
  const steerByDefault = running && steerPreferred && alternateSendEligible
  // Providers without native steering stop the running turn and send the steer first; say so before Send.
  const steerInterrupts = provider.supportsNativeSteering !== true
  const steerLabel = steerInterrupts ? 'Interrupt and steer' : 'Steer active turn'
  const steerDetail = steerInterrupts ? 'Stops the running turn, then sends this first; queued prompts keep their order.' : 'Sends this to the running turn at its next safe point; queued prompts keep their order.'
  const alternateSendLabel = steerByDefault ? 'Queue for next turn instead' : `${steerLabel} instead`
  const alternateSendTitle = steerByDefault ? 'Queue for next turn instead: runs after the current turn finishes.' : `${alternateSendLabel}: ${steerDetail}`
  const alternateSendAvailable = running && canSend && alternateSendEligible
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
  const optionsRef = useRef<HTMLDivElement>(null)
  const optionsTriggerRef = useRef<HTMLButtonElement>(null)
  const optionsMenuRef = useRef<HTMLDivElement>(null)
  const optionsMenuId = useId()
  const modelTriggerRef = useRef<HTMLButtonElement>(null)
  const modelWasOpenRef = useRef(false)
  const modelListId = useId()
  const goalToggleId = useId()
  const computerUseToggleId = useId()
  const architectToggleId = useId()

  useEffect(() => {
    if (!modelOpen && modelWasOpenRef.current) modelTriggerRef.current?.focus()
    modelWasOpenRef.current = modelOpen
  }, [modelOpen])

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
  const memoryOn = memoryLevel !== 'off'
  // Turning memory back on uses the default level; a default of "off" falls back to Strict.
  const memoryRestoreLevel: MemoryLevel = defaultMemoryLevel === 'off' ? 'strict' : defaultMemoryLevel
  const iconChipStyle = {
    ...chipStyle,
    width: 26,
    justifyContent: 'center',
  }

  return (
    <div
      className="uam-composer-toolbar flex items-center gap-2 px-2 py-1.5 text-xs"
      style={{
        borderTop: '1px solid var(--border)',
        color: 'var(--text-2)',
      }}
    >
      <div ref={optionsRef} className="relative shrink-0" style={isCompanionContext() ? { display: 'none' } : undefined}>
        <IconButton
          ref={optionsTriggerRef}
          icon={<Plus size={16} aria-hidden />}
          label="Options"
          tooltip="Add files, goal, and options"
          size="sm"
          aria-haspopup="menu"
          aria-expanded={optionsOpen}
          aria-controls={optionsOpen ? optionsMenuId : undefined}
          onClick={() => setOptionsOpen((v) => !v)}
          className="uam-composer-action uam-composer-plus inline-flex items-center"
          style={{
            ...iconChipStyle,
            color: optionsOpen ? 'var(--text)' : 'var(--text-2)',
            background: optionsOpen ? 'var(--surface-high)' : iconChipStyle.background,
          }}
        />
        {optionsOpen && (
          <ViewportMenu
            ref={optionsMenuRef}
            anchorRef={optionsTriggerRef}
            side="top"
            id={optionsMenuId}
            role="menu"
            aria-label="Composer options"
            onRequestClose={() => setOptionsOpen(false)}
            className="uam-cmenu"
          >
            {!isCompanionContext() && <>
              {/* ui: raw button kept — uam-cmenu__item row look and classes; MenuItem would swap them for uam-menu-item */}
              <button type="button" role="menuitem" className="uam-cmenu__item" onClick={() => { setOptionsOpen(false); onAttachFile() }}>
                <Paperclip size={15} aria-hidden className="uam-cmenu__icon" />
                <span className="uam-cmenu__text"><span>Attach files</span></span>
              </button>
              {/* ui: raw button kept — uam-cmenu__item row look and classes; MenuItem would swap them for uam-menu-item */}
              <button type="button" role="menuitem" className="uam-cmenu__item" onClick={() => { setOptionsOpen(false); onOpenMarkdownStore() }}>
                <span className="uam-cmenu__icon"><ComposerIcon name="markdown" /></span>
                <span className="uam-cmenu__text"><span>Skills</span></span>
                <ChevronRight size={14} aria-hidden className="uam-cmenu__trail" />
              </button>
              <div className="uam-cmenu__divider" />
            </>}
            <div className="uam-cmenu__section">Modes</div>
            <ComposerToggleRow
              id={goalToggleId}
              icon={<Target size={15} aria-hidden className="uam-cmenu__icon" style={{ color: goalActive || goalArmed ? 'var(--purple)' : undefined }} />}
              label={goalArmed ? 'Goal: next message' : 'Goal'}
              checked={goalActive || goalArmed}
              disabled={modelDisabled}
              onChange={() => { setOptionsOpen(false); onToggleGoal() }}
            />
            {COMPUTER_USE_ENABLED && !isCompanionContext() && <ComposerToggleRow
              id={computerUseToggleId}
              icon={<MousePointer2 size={15} aria-hidden className="uam-cmenu__icon" style={{ color: computerUseMode ? 'var(--accent)' : undefined }} />}
              label="Computer use…"
              checked={computerUseMode}
              onChange={() => { setOptionsOpen(false); onToggleComputerUseMode() }}
            />}
            {featurePreference === 'uam' && <ComposerToggleRow
              id={architectToggleId}
              title="Plan first, then run and review one verified step at a time"
              icon={<Cpu size={15} aria-hidden className="uam-cmenu__icon" style={{ color: smallModelMode ? 'var(--accent)' : undefined }} />}
              label="Architect + worker"
              checked={smallModelMode}
              disabled={modelDisabled}
              onChange={onToggleSmallModelMode}
            />}
            {featurePreference === 'uam' && smallModelMode && <>
              <div className="uam-cmenu__setting uam-cmenu__setting--nested">
                <span>{goalPairLocked ? 'Worker · locked' : 'Worker model'}</span>
                <MenuSelect
                  label="Worker model"
                  value={currentModel.id}
                  options={modelOptions.map((option) => ({ value: option.id, label: option.label, description: option.detail }))}
                  onChange={onSelectModel}
                  disabled={modelDisabled}
                />
              </div>
              <div className="uam-cmenu__setting uam-cmenu__setting--nested">
                <span>{goalPairLocked ? 'Reviewer · locked' : 'Reviewer model'}</span>
                <MenuSelect
                  label="Reviewer model"
                  value={currentReviewerModel.id}
                  options={reviewerModelOptions.map((option) => ({ value: option.id, label: option.label, description: option.detail }))}
                  onChange={onSelectReviewerModel}
                  disabled={modelDisabled}
                />
              </div>
            </>}
            <div className="uam-cmenu__divider" />
            <div className="uam-cmenu__section">Chat settings</div>
            {hasReasoningEffort && (
              <div className="uam-cmenu__setting">
                <span>Reasoning</span>
                <MenuSelect
                  label="Reasoning"
                  value={currentReasoning.id}
                  options={reasoningOptions.map((option) => ({ value: option.id, label: option.label, description: option.detail, disabled: option.disabled }))}
                  onChange={onSelectReasoning}
                  disabled={modelDisabled}
                />
              </div>
            )}
            {caps.hasServiceTier && speedOptions.length > 0 && (
              <div className="uam-cmenu__setting">
                <span>Speed</span>
                <MenuSelect
                  label="Speed"
                  value={currentSpeed.id}
                  options={speedOptions.map((option) => ({ value: option.id, label: option.label, description: option.detail }))}
                  onChange={onSelectSpeed}
                  disabled={modelDisabled}
                />
              </div>
            )}
            {COPILOT_ENABLED && canonicalProviderId === COPILOT_CLI_PROVIDER_ID && acp && <CopilotAutoRoutingControl acp={acp} disabled={modelDisabled} />}
            {variantOptions.map((option) => (
              <div key={option.id} className="uam-cmenu__setting">
                <span className="truncate">{option.name || option.id}</span>
                <MenuSelect
                  label={option.name || option.id}
                  value={option.currentValue}
                  options={option.options.map((choice) => ({ value: choice.value, label: choice.name || choice.value, description: choice.description }))}
                  onChange={(value) => onSelectConfigOption(option.id, value)}
                  disabled={modelDisabled}
                />
              </div>
            ))}
            {featurePreference === 'uam' && (
              <div className="uam-cmenu__setting">
                <span>{uamAgentNextTurn ? 'Next turn' : 'Agent'}</span>
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
              </div>
            )}
            {featurePreference === 'provider' && providerModes.length > 0 && (
              <div className="uam-cmenu__setting">
                <span>Mode</span>
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
              </div>
            )}
            <div className="uam-cmenu__setting">
              <span>Permissions</span>
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
            </div>
            <div className="uam-cmenu__setting">
              <span>Memory</span>
              <MenuSelect
                label="Memory"
                value={memoryLevel}
                options={MEMORY_LEVEL_OPTIONS.map((option) => ({ value: option.id, label: option.label, description: option.detail }))}
                onChange={(level) => onSelectMemoryLevel(level as MemoryLevel)}
                disabled={memoryDisabled}
              />
            </div>
          </ViewportMenu>
        )}
      </div>
      {/* ui: raw button kept — Button wraps children in a label span, which would break the wave and status flex row */}
      {dictationActive ? (
        <Tooltip label={dictationError || dictationLabel}><button
          type="button"
          aria-label={dictationLabel}
          aria-pressed={dictationState !== 'idle'}
          aria-busy={dictationState === 'starting' || dictationState === 'stopping'}
          data-dictation-state={dictationVisualState}
          onClick={onToggleDictation}
          disabled={!dictationAvailable || dictationState === 'starting' || dictationState === 'stopping'}
          className="uam-composer-action uam-dictation-button flex h-[26px] min-w-0 flex-1 items-center justify-center gap-3 px-3 text-xs font-semibold animate-fade-in"
          style={{ borderRadius: 6, opacity: !dictationAvailable || dictationState === 'starting' || dictationState === 'stopping' ? 0.55 : 1 }}
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
        </button></Tooltip>
      ) : <>
      <fieldset style={{ border: 0, padding: 0, margin: 0 }} className="animate-fade-in uam-composer-status-chips flex min-w-0 flex-1 items-center gap-2 overflow-x-auto">
        {goalArmed && <ActiveModeChip label="Goal: next message" compactLabel="Goal" icon={<Target size={12} aria-hidden style={{ color: 'var(--purple)' }} />} onClear={onToggleGoal} />}
        {COMPUTER_USE_ENABLED && computerUseMode && <ActiveModeChip label={computerUseAwaitingTarget ? 'Computer use: awaiting target approval' : 'Computer use'} compactLabel="Computer" icon={<MousePointer2 size={12} aria-hidden style={{ color: 'var(--accent)' }} />} onClear={onToggleComputerUseMode} />}
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
        <Tooltip label="Use memories in this chat"><Button
          type="button"
          variant="ghost"
          size="sm"
          leadingIcon={<Brain size={12} aria-hidden />}
          data-mode-chip={memoryOn ? 'Memory On' : 'Memory Off'}
          aria-pressed={memoryOn}
          aria-label={memoryOn ? 'Memory on' : 'Memory off'}
          onClick={() => onSelectMemoryLevel(memoryOn ? 'off' : memoryRestoreLevel)}
          disabled={memoryDisabled}
          className="uam-composer-action inline-flex h-[26px] min-w-0 shrink-0 items-center gap-1.5 px-2 text-xs"
          style={{ ...chipStyle, color: memoryOn ? 'var(--text)' : 'var(--text-3)' }}
        >
          <span>{memoryOn ? `Memory On · ${MEMORY_LEVEL_OPTIONS.find((option) => option.id === memoryLevel)?.label ?? memoryLevel}` : 'Memory Off'}</span>
        </Button></Tooltip>
        {featurePreference === 'uam' && smallModelMode && <ActiveModeChip label="Architect + worker" compactLabel="Architect" icon={<Cpu size={12} aria-hidden style={{ color: 'var(--accent)' }} />} onClear={modelDisabled ? undefined : onToggleSmallModelMode} />}
        {featurePreference === 'uam' && smallModelMode && <ActiveModeChip label={`Reviewer: ${currentReviewerModel.label}`} compactLabel={`Review: ${currentReviewerModel.shortLabel}`} icon={<Sparkles size={12} aria-hidden style={{ color: 'var(--purple)' }} />} />}
        {hasReasoningEffort && <ComposerChoice label="Reasoning" chipLabel={`Reasoning: ${currentReasoning.label}`} value={currentReasoning.id} icon={<Cpu size={12} aria-hidden />} options={reasoningOptions.map((option) => ({ value: option.id, label: option.label, description: option.detail, disabled: option.disabled }))} onChange={onSelectReasoning} disabled={modelDisabled} />}
        {variantOptions.map((option) => <ComposerChoice key={option.id} label={option.name || option.id} value={option.currentValue} icon={<Cpu size={12} aria-hidden />} options={option.options.map((choice) => ({ value: choice.value, label: choice.name || choice.value, description: choice.description }))} onChange={(value) => onSelectConfigOption(option.id, value)} disabled={modelDisabled} />)}
        {speedExplicit && <ComposerChoice label="Speed" chipLabel={`Speed: ${currentSpeed.label}`} value={currentSpeed.id} icon={<Sparkles size={12} aria-hidden />} options={speedOptions.map((option) => ({ value: option.id, label: option.label, description: option.detail }))} onChange={onSelectSpeed} disabled={modelDisabled} />}

      </fieldset>
      <div className="flex shrink-0 items-center gap-2">
        {draftPending && <Tooltip label="You have unsent text in the composer"><span data-draft-indicator className="text-xs" style={{ color: 'var(--text-3)' }}>Draft</span></Tooltip>}
        <ProviderAllowanceIndicator providerId={providerId} sourceProviderId={acp?.providerId} rateLimits={acp?.providerUsage?.rateLimits} running={Boolean(acp?.running)} />
        <div ref={modelMenuRef} className="relative">
          <Tooltip label={selectorDisabled ? undefined : `Model · ${providerName} · ${currentModel.label} · ${runtimeStatusLabel}`}><Button
            ref={modelTriggerRef}
            type="button"
            variant="ghost"
            size="sm"
            leadingIcon={<ProviderLogo providerId={providerId} />}
            trailingIcon={<span aria-hidden className="h-1.5 w-1.5 shrink-0 rounded-full" style={{ background: runtimeStatusColor }} />}
            disabledReason={selectorDisabled ? `${modelBlockReason} · ${providerName} · ${currentModel.label} · ${runtimeStatusLabel}` : undefined}
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
              borderRadius: 6,
              color: modelOpen ? 'var(--text)' : 'var(--text-2)',
              background: modelOpen ? 'var(--surface-high)' : chipStyle.background,
              opacity: selectorDisabled ? 0.55 : 1,
            }}
          >
            <span className="truncate" style={{ color: 'var(--text)' }}>{currentModel.shortLabel}</span>
          </Button></Tooltip>
          {modelOpen && !selectorDisabled && (
            <ViewportMenu
              anchorRef={modelTriggerRef}
              side="top"
              align="end"
              id={modelListId}
              role="menu"
              aria-label="Provider and model"
              manageFocus={false}
              className="uam-composer-model-popover"
              style={{
                width: 360,
                border: '1px solid var(--border-bright)',
                borderRadius: 8,
                background: 'var(--surface)',
                boxShadow: 'var(--elev-3)',
                padding: 0,
              }}
            >
              <ModelPickerContent
                providers={providers.some((candidate) => candidate.id === providerId) ? providers : [...providers, provider]} providerId={providerId} currentModelId={currentModel.id}
                activeOptions={modelOptions} activeLoading={acp?.modelsLoading} activeError={acp?.modelRefreshError}
                onRefreshActive={onRefreshModels}
                modelDisabled={modelDisabled} canChangeProvider={canChangeProvider} providerLockReason={providerLockReason} providerSwitchUnlocked={providerSwitchUnlocked}
                workspaceDirectory={workspaceDirectory} executionHostId={executionHostId}
                onClose={() => { onToggleModel(); modelTriggerRef.current?.focus() }}
                onSelect={(selectedProviderId, selectedModelId) => {
                  if (selectedProviderId === providerId) onSelectModel(selectedModelId)
                  else onSelectProviderModel(selectedProviderId, selectedModelId)
                  modelTriggerRef.current?.focus()
                }}
              />
              <div className="flex items-center gap-2 border-t px-2 py-2 text-xs" style={{ borderColor: 'var(--border)', color: 'var(--text-3)' }}>
                <span aria-hidden className="h-1.5 w-1.5 rounded-full" style={{ background: runtimeStatusColor }} />
                <span>{runtimeStatusLabel}</span>
                {acp?.agentInfo?.title && <span className="ml-auto truncate">{acp.agentInfo.title}</span>}
              </div>
            </ViewportMenu>
          )}
        </div>
        <IconButton
          icon={<Mic size={15} aria-hidden />}
          label={dictationLabel}
          tooltip={isCompanionContext() ? `${dictationLabel} · or use your keyboard microphone` : undefined}
          size="sm"
          aria-pressed="false"
          data-dictation-state="idle"
          onClick={onToggleDictation}
          disabled={!dictationAvailable}
          disabledReason={dictationAvailable ? undefined : 'Dictation requires the desktop app'}
          className="uam-composer-action uam-composer-secondary-control uam-dictation-button h-[26px] w-[26px] text-xs font-semibold inline-flex items-center justify-center p-0"
          style={{ borderRadius: 6, opacity: dictationAvailable ? 1 : 0.55 }}
        />
      </div>
      </>}
        {/* Space stays reserved in every state, ahead of Cancel and Send, so swapping them does not shift the toolbar; the closed state fades out and leaves the tab order. */}
        {onSendAlternate && !holdSend && <IconButton
          icon={steerByDefault ? <ListPlus size={15} aria-hidden /> : <CornerUpRight size={15} aria-hidden />}
          label={alternateSendLabel}
          title={alternateSendAvailable ? alternateSendTitle : undefined}
          tooltip={false}
          size="sm"
          aria-hidden={alternateSendAvailable ? undefined : true}
          tabIndex={alternateSendAvailable ? undefined : -1}
          disabled={!alternateSendAvailable}
          data-state={alternateSendAvailable ? 'open' : 'closed'}
          onClick={onSendAlternate}
          className="uam-composer-action uam-composer-secondary-control uam-composer-alternate-send h-[26px] w-[26px] shrink-0 inline-flex items-center justify-center p-0"
          style={{ borderRadius: 6 }}
        />}
        {running && !editing && (holdSend || !canSend) && (
          <IconButton title="Cancel turn" icon={<Square size={11} fill="currentColor" aria-hidden />} label="Cancel turn" tooltip={false} onClick={onCancelTurn} className="uam-composer-action h-8 w-8 shrink-0 text-xs font-semibold inline-flex items-center justify-center" style={{ width: 32, height: 32, borderRadius: 8, border: '1px solid color-mix(in srgb, var(--red) 46%, var(--border-bright))', background: 'color-mix(in srgb, var(--red) 14%, var(--surface))', color: 'var(--red)' }} />
        )}
        {(holdSend || editing || !running || canSend) && <ComposerSendButton canSend={canSend} running={running} editing={editing} steer={steerByDefault} label={sendLabel ?? (steerByDefault ? steerLabel : undefined)} title={steerByDefault ? `${steerLabel}: ${steerDetail}` : undefined} />}
    </div>
  )
}

/** Composer send button; while editing a message it saves the edit with a pencil icon, and a preferred steer shows the steer icon. */
export function ComposerSendButton({ canSend, running = false, editing = false, steer = false, label: labelOverride, title }: { canSend: boolean; running?: boolean; editing?: boolean; steer?: boolean; label?: string; title?: string }) {
  const label = labelOverride ?? (editing ? 'Save edit' : running ? 'Queue prompt' : 'Send prompt')
  return (
    <IconButton
      type="submit"
      title={title ?? label}
      icon={editing ? <Pencil size={15} aria-hidden /> : steer ? <CornerUpRight size={15} aria-hidden /> : <ComposerIcon name="send" size={15} />}
      label={label}
      tooltip={false}
      disabled={!canSend}
      className="uam-composer-action h-8 w-8 shrink-0 text-xs font-semibold inline-flex items-center justify-center"
      style={{
        width: 32,
        height: 32,
        borderRadius: 8,
        border: '1px solid color-mix(in srgb, var(--accent) 64%, var(--border-bright))',
        background: canSend ? 'var(--accent)' : 'var(--surface-up)',
        color: canSend ? 'var(--on-accent)' : 'var(--text-3)',
      }}
    />
  )
}
