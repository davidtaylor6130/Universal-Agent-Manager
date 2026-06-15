// ComposerToolbar: message input toolbar with model/mode pickers and
// ComposerIcon SVG sprite. Extracted from ChatView.tsx (MO-3).

import { RefObject } from 'react'
import type { AcpBinding } from '../../store/useAppStore'
import type { Goal } from '../../types/goal'
import type { Provider } from '../../types/provider'
import { ProviderLogo } from '../shared/ProviderLogo'
import {
  isClaudeProvider,
  isCodexProvider,
  isCopilotProvider,
  isOpenCodeProvider,
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
        className="font-semibold leading-none"
        style={{ fontSize: 11, letterSpacing: 0 }}
      >
        .md
      </span>
    )
  }

  if (name === 'folder') {
    return (
      <svg width={size} height={size} viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.7" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
        <path d="M2.5 4.2h4l1.1 1.4h5.9v6.2a1 1 0 0 1-1 1h-10a1 1 0 0 1-1-1V5.2a1 1 0 0 1 1-1Z" />
      </svg>
    )
  }

  if (name === 'editor') {
    return (
      <svg width={size} height={size} viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.7" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
        <rect x="2.5" y="3" width="11" height="8.5" rx="1.2" />
        <path d="M5.5 13h5" />
        <path d="M8 11.5V13" />
        <path d="m5.6 6.1 1.2 1.2-1.2 1.2" />
        <path d="M8.2 8.6h2.2" />
      </svg>
    )
  }

  if (name === 'git-tree') {
    return (
      <svg width={size} height={size} viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.7" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
        <circle cx="4" cy="3.5" r="1.6" />
        <circle cx="12" cy="8" r="1.6" />
        <circle cx="4" cy="12.5" r="1.6" />
        <path d="M4 5.1v5.8" />
        <path d="M5.6 3.5h1.7A2.7 2.7 0 0 1 10 6.2V8h.4" />
      </svg>
    )
  }

  if (name === 'send') {
    return (
      <svg width={size} height={size} viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
        <path d="M2.5 8 13 3.2 10.4 13 7.5 9.2 2.5 8Z" />
        <path d="m7.5 9.2 2.2-2.4" />
      </svg>
    )
  }

  if (name === 'terminal') {
    return (
      <svg width={size} height={size} viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.7" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
        <rect x="2" y="3" width="12" height="10" rx="1.5" />
        <path d="m4.7 7.3 2 1.1-2 1.1" />
        <path d="M8 10h4" />
      </svg>
    )
  }

  return (
    <svg width={size} height={size} viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" aria-hidden="true">
      <path d="M8 3.2v9.6" />
      <path d="M3.2 8h9.6" />
    </svg>
  )
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
  const modelOptions = buildModelOptions(acp, modelId ?? '', provider, providerId)
  const currentModel = modelOptionFor(modelOptions, modelId)
  const codexProvider = isCodexProvider(provider, providerId)
  const reasoningOptions = codexProvider ? buildCodexReasoningOptions(acp, currentModel.id, reasoningEffort ?? '') : []
  const speedOptions = codexProvider ? buildCodexSpeedOptions(acp, currentModel.id, serviceTier ?? '') : []
  const currentReasoning = modelOptionFor(reasoningOptions, reasoningEffort)
  const currentSpeed = modelOptionFor(speedOptions, serviceTier)
  const providerOptions = providers.length > 0 ? providers : [provider]
  const modelDisabled = acpRuntimeBlocksControlChanges(acp)
  const planActive = approvalModeId === 'plan'
  const acceptEditsActive = approvalModeId === 'acceptEdits'
  const yoloActive = autoApproveCommands
  const claudeProvider = isClaudeProvider(provider, providerId)
  const hasRuntimeModes = Boolean(acp?.running && acp.availableModes.length > 0)
  const planAvailable = !hasRuntimeModes || acp?.availableModes.some((mode) => mode.id === 'plan')
  const acceptEditsAvailable = claudeProvider && (!hasRuntimeModes || acp?.availableModes.some((mode) => mode.id === 'acceptEdits'))
  const yoloAvailable = true
  const planDisabled = Boolean(modelDisabled || !planAvailable)
  const acceptEditsDisabled = Boolean(modelDisabled || !acceptEditsAvailable)
  const yoloDisabled = false
  const memoryDisabled = Boolean(modelDisabled)
  const autoLabel = claudeProvider ? 'Auto' : 'Yolo'
  const modeLabel = planActive ? 'Plan' : acceptEditsActive ? 'Accept Edits' : 'Default'
  const running = Boolean(acp?.processing)
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
      {(providerOptions.length > 1 || providerId !== providerOptions[0]?.id) && (
      <div ref={providerMenuRef} className="relative">
        <button
          type="button"
          title="Select provider"
          onClick={onToggleProvider}
          className="inline-flex items-center gap-1.5 px-2"
          style={{
            ...chipStyle,
            color: providerOpen ? 'var(--text)' : 'var(--text-2)',
            borderColor: providerOpen ? 'var(--border-bright)' : 'var(--border)',
          }}
        >
          <ProviderLogo providerId={providerId} />
          <span>{providerName}</span>
        </button>
        {providerOpen && (
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
            <div className="px-2 py-1 text-[11px]" style={{ color: 'var(--text-3)' }}>Provider</div>
            {providerOptions.map((candidate) => {
              const candidateName = providerShortName(candidate, candidate.id)
              const selected = candidate.id === providerId
              const disabled = !selected && !canChangeProvider
              return (
                <button
                  key={candidate.id}
                  type="button"
                  onClick={() => {
                    if (disabled) return
                    onSelectProvider(candidate.id)
                  }}
                  disabled={disabled}
                  className="w-full flex items-center gap-2 text-left px-2 py-2"
                  style={{
                    borderRadius: 6,
                    background: selected ? 'var(--accent-dim)' : 'transparent',
                    color: selected ? 'var(--text)' : 'var(--text-2)',
                    opacity: disabled ? 0.5 : 1,
                  }}
                >
                  <ProviderLogo providerId={candidate.id} />
                  <span className="flex-1">{candidateName}</span>
                  {selected && <span style={{ color: 'var(--green)', fontSize: 10 }}>●</span>}
                </button>
              )
            })}
          </div>
        )}
      </div>
      )}
      <div ref={modelMenuRef} className="relative">
        <button
          type="button"
          title="Select model"
          onClick={onToggleModel}
          disabled={modelDisabled}
          className="inline-flex items-center gap-1.5 px-2"
          style={{
            ...chipStyle,
            color: modelOpen ? 'var(--text)' : 'var(--text-2)',
            borderColor: modelOpen ? 'var(--border-bright)' : 'var(--border)',
            opacity: modelDisabled ? 0.55 : 1,
          }}
        >
          <span>Model</span>
          <span style={{ color: 'var(--text)' }}>{currentModel.shortLabel}</span>
        </button>
        {modelOpen && !modelDisabled && (
          <div
            className="absolute left-0"
            style={{
              bottom: 32,
              width: 260,
              zIndex: 40,
              border: '1px solid var(--border-bright)',
              borderRadius: 8,
              background: 'var(--surface)',
              boxShadow: '0 14px 42px rgba(0, 0, 0, 0.28)',
              padding: 6,
            }}
          >
            <div className="px-2 py-1 text-[11px]" style={{ color: 'var(--text-3)' }}>Model</div>
            {modelOptions.map((option) => {
              const selected = option.id === currentModel.id
              return (
                <button
                  key={option.id || 'default'}
                  type="button"
                  onClick={() => onSelectModel(option.id)}
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
      {codexProvider && (
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
      {codexProvider && (
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
      <button
        type="button"
        title={planAvailable ? 'Toggle planning mode. Claude Plan is read-only and will not edit files.' : 'Planning mode unavailable'}
        aria-pressed={planActive}
        onClick={onTogglePlan}
        disabled={planDisabled}
        className="inline-flex items-center gap-1.5 px-2"
        style={{
          ...chipStyle,
          borderColor: planActive ? 'color-mix(in srgb, var(--accent) 55%, var(--border))' : 'var(--border)',
          background: planActive ? 'var(--accent-dim)' : chipStyle.background,
          color: planActive ? 'var(--text)' : 'var(--text-2)',
          opacity: planDisabled ? 0.55 : 1,
        }}
      >
        <span style={{ color: planActive ? 'var(--accent)' : 'var(--text-3)', fontSize: 10 }}>●</span>
        <span>Plan</span>
      </button>
      {claudeProvider && (
        <button
          type="button"
          title={acceptEditsAvailable ? 'Toggle Accept Edits mode. Claude can edit workspace files without prompting.' : 'Accept Edits mode unavailable'}
          aria-pressed={acceptEditsActive}
          onClick={onToggleAcceptEdits}
          disabled={acceptEditsDisabled}
          className="inline-flex items-center gap-1.5 px-2"
          style={{
            ...chipStyle,
            borderColor: acceptEditsActive ? 'color-mix(in srgb, var(--green) 52%, var(--border))' : 'var(--border)',
            background: acceptEditsActive ? 'color-mix(in srgb, var(--green) 14%, var(--surface))' : chipStyle.background,
            color: acceptEditsActive ? 'var(--text)' : 'var(--text-2)',
            opacity: acceptEditsDisabled ? 0.55 : 1,
          }}
        >
          <span style={{ color: acceptEditsActive ? 'var(--green)' : 'var(--text-3)', fontSize: 10 }}>●</span>
          <span>Accept Edits</span>
        </button>
      )}
      <button
        type="button"
        title={yoloAvailable ? `Toggle ${autoLabel} mode` : `${autoLabel} mode unavailable`}
        aria-pressed={yoloActive}
        onClick={onToggleYolo}
        disabled={yoloDisabled}
        className="inline-flex items-center gap-1.5 px-2"
        style={{
          ...chipStyle,
          borderColor: yoloActive ? 'color-mix(in srgb, var(--yellow) 55%, var(--border))' : 'var(--border)',
          background: yoloActive ? 'color-mix(in srgb, var(--yellow) 16%, var(--surface))' : chipStyle.background,
          color: yoloActive ? 'var(--text)' : 'var(--text-2)',
          opacity: yoloDisabled ? 0.55 : 1,
        }}
      >
        <span style={{ color: yoloActive ? 'var(--yellow)' : 'var(--text-3)', fontSize: 10 }}>●</span>
        <span>{autoLabel}</span>
      </button>
      <button
        type="button"
        title="Toggle memory"
        aria-pressed={memoryEnabled}
        onClick={onToggleMemory}
        disabled={memoryDisabled}
        className="inline-flex items-center gap-1.5 px-2"
        style={{
          ...chipStyle,
          borderColor: memoryEnabled ? 'color-mix(in srgb, var(--green) 50%, var(--border))' : 'var(--border)',
          background: memoryEnabled ? 'color-mix(in srgb, var(--green) 14%, var(--surface))' : chipStyle.background,
          color: memoryEnabled ? 'var(--text)' : 'var(--text-2)',
          opacity: memoryDisabled ? 0.55 : 1,
        }}
      >
        <span style={{ color: memoryEnabled ? 'var(--green)' : 'var(--text-3)', fontSize: 10 }}>●</span>
        <span>Memory</span>
      </button>
      <button
        type="button"
        title={goalActive ? 'Pause goal mode' : goalPaused ? 'Resume goal mode' : goalArmed ? 'Next message will become the goal' : 'Use the next message as a goal'}
        aria-pressed={goalActive || goalArmed}
        onClick={onToggleGoal}
        disabled={modelDisabled}
        className="inline-flex items-center gap-1.5 px-2"
        style={{
          ...chipStyle,
          borderColor: goalActive || goalArmed ? 'color-mix(in srgb, var(--purple) 55%, var(--border))' : 'var(--border)',
          background: goalActive || goalArmed ? 'color-mix(in srgb, var(--purple) 16%, var(--surface))' : chipStyle.background,
          color: goalActive || goalArmed ? 'var(--text)' : 'var(--text-2)',
          opacity: modelDisabled ? 0.55 : 1,
        }}
      >
        <span style={{ color: goalActive || goalArmed ? 'var(--purple)' : 'var(--text-3)', fontSize: 10 }}>●</span>
        <span>{goalArmed ? 'Goal Next' : 'Goal'}</span>
      </button>
      <button
        type="button"
        title="Attach files"
        onClick={onAttachFile}
        className="inline-flex items-center"
        style={iconChipStyle}
      >
        <ComposerIcon name="plus" />
      </button>
      <button
        type="button"
        title="Open Markdown Store"
        onClick={onOpenMarkdownStore}
        className="inline-flex items-center"
        style={iconChipStyle}
      >
        <ComposerIcon name="markdown" />
      </button>
      <div className="ml-auto flex items-center gap-2">
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
            <span>⚙</span>
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
                {codexProvider && (
                  <div className="flex justify-between gap-3">
                    <span style={{ color: 'var(--text-3)' }}>Reasoning</span>
                    <span>{currentReasoning.label}</span>
                  </div>
                )}
                {codexProvider && (
                  <div className="flex justify-between gap-3">
                    <span style={{ color: 'var(--text-3)' }}>Speed</span>
                    <span>{currentSpeed.label}</span>
                  </div>
                )}
                <div className="flex justify-between gap-3">
                  <span style={{ color: 'var(--text-3)' }}>Mode</span>
                  <span>{modeLabel}</span>
                </div>
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
