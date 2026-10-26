import { VcsSettingsSection } from './VcsSettingsSection'
import { StorageCleanupSettingsSection } from './StorageCleanupSettingsSection'
import { DataFolderSettingsSection, type DataFolderSettingsHandle } from './DataFolderSettingsSection'
import { TypographySettings } from './TypographySettings'
import { AppearanceDisplaySettings } from './AppearanceDisplaySettings'
import { PermissionsSettings } from './PermissionsSettings'
import { SaveButton, SettingRow, SettingsField, SettingsSection } from './SettingsPrimitives'
import { SettingsScopeBar } from './SettingsScopeBar'
import { PhonePairingPanel } from './PhonePairingPanel'
import { SETTINGS_ALL_ENVIRONMENTS } from '../../utils/settingsScope'
import { ProviderConnections } from './ProviderConnections'
import { CentralProviderSettings, type CentralProviderSettingsHandle } from './CentralProviderSettings'
import { McpServerSetup } from './McpServerSetup'
import { ComputerUseSettingRow } from './ComputerUseSettingRow'
import { ExecutionRoutingSettings, type ExecutionRoutingSettingsHandle } from './ExecutionRoutingSettings'
import { UnsavedChangesDialog } from './UnsavedChangesDialog'
import { NumberField } from './NumberField'
import { ANTIGRAVITY_ENABLED, CLAUDE_ENABLED, CODEX_ENABLED, COMPUTER_USE_ENABLED, COPILOT_ENABLED, GEMINI_ENABLED, MOBILE_COMPANION_ENABLED, OPENCODE_ENABLED, SSH_ENABLED } from '../../config/buildFeatures'
import { copyTextToClipboard } from '../../utils/copySelection'
import { forwardRef, useEffect, useImperativeHandle, useLayoutEffect, useRef, useState, type ReactNode, type KeyboardEvent as ReactKeyboardEvent } from 'react'
import {
  MAX_MEMORY_IDLE_DELAY_SECONDS,
  MAX_MEMORY_RECALL_BUDGET_BYTES,
  MIN_MEMORY_IDLE_DELAY_SECONDS,
  MIN_MEMORY_RECALL_BUDGET_BYTES,
  useAppStore,
  type CliVersionProviderState,
  type AcpModel,
  type EditorFileAssociation,
  type MemoryWorkerBinding,
    type ProviderChatDefaults,
  type ProviderAgentImportPreview,
  type UamAgentCycleShortcut,
  type ComputerUseAllowedApplication,
  type WindowsPowerShellPreference,
} from '../../store/useAppStore'
import { useTheme } from '../../hooks/useTheme'
import { sendToCEF } from '../../ipc/cefBridge'
import {
  applyDocumentTheme,
  BUILT_IN_THEMES,
  BUILT_IN_THEME_COLORS,
  normalizeCustomTheme,
  resolveDocumentTheme,
  type CustomTheme,
  type ThemeColors,
  type BuiltInTheme,
  type StoredTheme,
} from '../../utils/themeStorage'
import type { Provider } from '../../types/provider'
import type { ComputerUseBackend, ExecutionHost, Session } from '../../types/session'
import type { MemoryLevel } from '../../types/memory'
import { sanitizeVoiceInputSettings } from '../../store/cpp/sanitizers'
import type { VoiceInputSettingsRequest } from '../../store/cpp/types'
import { ProviderLogo } from '../shared/ProviderLogo'
import { useShallow } from 'zustand/react/shallow'
import { Search, ArrowDown, ArrowLeft, ArrowUp, BookOpen, Brain, Check, ChevronDown, ChevronRight, ClipboardList, Copy, Download, GripVertical, Upload, FolderOpen, Info, MemoryStick, MessageSquare, Mic, Minus, MousePointerClick, Palette, Pencil, Plus, RefreshCw, Route, Save, Server, Smartphone, Target, TerminalSquare, Trash2, X, type LucideIcon } from 'lucide-react'
import { Button, ConfirmDialog, IconButton, MenuSelect, Notice, Overlay, Presence, StatusDot, Switch, ViewportMenu, motionAllowed } from '../ui'
import { ShellActionsSettings, type ShellActionsHandle } from './ShellActionsSettings'
import { MemoryLibraryModal, type MemoryLibraryHandle } from './MemoryLibraryModal'
import { MarkdownStoreModal } from './MarkdownStoreModal'
import { StatusIndicator } from '../shared/StatusIndicator'
import {
  CLAUDE_CLI_PROVIDER_ID,
  CODEX_CLI_PROVIDER_ID,
  COPILOT_CLI_PROVIDER_ID,
  DEFAULT_PROVIDER_ID,
  GEMINI_CLI_PROVIDER_ID,
  OPENCODE_CLI_PROVIDER_ID,
  SUPPORTED_CLI_PROVIDER_IDS,
  fallbackProviderForId,
  providerCapabilities,
  providerMetadataForId,
  providerSupportsTextWorkers,
  providerShortName,
} from '../../utils/providerMetadata'
import { buildProviderReasoningOptions, buildCodexSpeedOptions, buildModelOptions, modelCatalogForDefaults, reasoningEffortForModel, titleFromModelId } from '../chat/modelOptions'
import './settings-motion.css'
import './settings-shell.css'
import { isSshHost } from '../../utils/remoteWorkspace'

const MEMORY_SETTINGS_FIELD_LABELS: Record<string, string> = {
  memoryEnabledDefault: 'Default memory enabled',
  memoryLevelDefault: 'Default memory policy',
  memoryIdleDelaySeconds: 'Memory idle delay',
  memoryRecallBudgetBytes: 'Memory recall budget',
  goalMaxLoopIterations: 'Goal loop limit',
  acpSetupInactivityTimeoutSeconds: 'Provider setup timeout',
  acpTurnOutputLimitMiB: 'Provider output limit',
  memoryWorkerBindings: 'Memory worker selections',
  permissionReviewerProviderId: 'Permission reviewer provider',
  permissionReviewerModelId: 'Permission reviewer model',
}

const MEMORY_POLICY_CHOICES: Array<{ id: MemoryLevel; label: string; detail: string }> = [
  { id: 'default', label: 'Default', detail: 'Inherit the current default.' },
  { id: 'off', label: 'Off', detail: 'Do not recall or create memories.' },
  { id: 'readOnly', label: 'Read Only', detail: 'Recall existing memories without creating new ones.' },
  { id: 'strict', label: 'Strict Creation', detail: 'Save only critical, high-confidence memories.' },
  { id: 'balanced', label: 'Balanced Creation', detail: 'Save useful medium- and high-confidence memories.' },
  { id: 'open', label: 'Remember Everything', detail: 'Save every valid, non-sensitive memory candidate.' },
]

function selectPolicyWithArrowKeys(event: ReactKeyboardEvent<HTMLDivElement>) {
  if (!['ArrowLeft', 'ArrowRight', 'ArrowUp', 'ArrowDown', 'Home', 'End'].includes(event.key)) return
  const buttons = Array.from(event.currentTarget.querySelectorAll<HTMLButtonElement>('button[role="radio"]:not(:disabled)'))
  if (!buttons.length) return
  const index = Math.max(0, buttons.indexOf(document.activeElement as HTMLButtonElement))
  const next = event.key === 'Home' ? 0 : event.key === 'End' ? buttons.length - 1
    : (index + (event.key === 'ArrowLeft' || event.key === 'ArrowUp' ? -1 : 1) + buttons.length) % buttons.length
  event.preventDefault(); buttons[next].focus(); buttons[next].click()
}

interface SettingsDraftHandle { requestLeave: (next: () => void) => void }

const VoiceInputControls = forwardRef<SettingsDraftHandle>(function VoiceInputControls(_props, ref) {
  const saved = useAppStore(useShallow((state) => ({
    mode: state.voiceInputMode ?? 'system', serverBaseUrl: state.voiceInputServerBaseUrl ?? '',
    serverEndpoint: state.voiceInputServerEndpoint ?? '/v1/audio/transcriptions', serverModel: state.voiceInputServerModel ?? 'whisper-1',
    apiKeyEnv: state.voiceInputApiKeyEnv ?? 'OPENAI_API_KEY',
  })))
  const capabilities = useAppStore((state) => state.voiceInputCapabilities)
  const saveSettings = useAppStore((state) => state.setVoiceInputSettings)
  const [draft, setDraft] = useState<VoiceInputSettingsRequest>(saved)
  const [saving, setSaving] = useState(false)
  const [error, setError] = useState('')
  const [exit, setExit] = useState<(() => void) | null>(null)
  const dirty = JSON.stringify(draft) !== JSON.stringify(saved)
  const draftRef = useRef(draft)
  const savedRef = useRef(saved)
  useEffect(() => {
    if (JSON.stringify(draftRef.current) === JSON.stringify(savedRef.current)) { setDraft(saved); draftRef.current = saved }
    savedRef.current = saved
  }, [saved])
  const edit = (next: VoiceInputSettingsRequest) => { draftRef.current = next; setDraft(next); setError('') }
  useEffect(() => {
    if (!dirty) return
    const guard = (event: BeforeUnloadEvent) => { event.preventDefault(); event.returnValue = '' }
    window.addEventListener('beforeunload', guard)
    return () => window.removeEventListener('beforeunload', guard)
  }, [dirty])
  useImperativeHandle(ref, () => ({ requestLeave: (next) => { if (saving) return; if (dirty) setExit(() => next); else next() } }))
  const projected = { voiceInputMode: draft.mode, voiceInputServerBaseUrl: draft.serverBaseUrl, voiceInputServerEndpoint: draft.serverEndpoint, voiceInputServerModel: draft.serverModel, voiceInputApiKeyEnv: draft.apiKeyEnv }
  const valid = sanitizeVoiceInputSettings(projected)
  const supported = capabilities?.[draft.mode]?.supported === true
  const validationError = draft.mode !== 'server' ? ''
    : valid.voiceInputServerBaseUrl === undefined ? 'Use HTTPS, or HTTP for localhost.'
    : valid.voiceInputServerEndpoint === undefined ? 'Enter an absolute endpoint path without a query or fragment.'
    : valid.voiceInputServerModel === undefined ? 'Enter a transcription model, up to 256 bytes.'
    : valid.voiceInputApiKeyEnv === undefined ? 'Enter an environment variable name, or leave it empty.' : ''
  const save = async (next?: () => void) => {
    if (saving || !supported || validationError) return
    setSaving(true); setError('')
    try {
      // Server configuration remains a draft until the native save succeeds.
      const payload = draft.mode === 'server' ? draft : { mode: draft.mode }
      if (!await saveSettings(payload)) { setError('Could not save voice settings. Try again.'); return }
      const current = useAppStore.getState()
      edit({ mode: current.voiceInputMode ?? draft.mode, serverBaseUrl: current.voiceInputServerBaseUrl ?? '', serverEndpoint: current.voiceInputServerEndpoint ?? '/v1/audio/transcriptions', serverModel: current.voiceInputServerModel ?? 'whisper-1', apiKeyEnv: current.voiceInputApiKeyEnv ?? 'OPENAI_API_KEY' })
      if (next) { setExit(null); next() }
    } catch { setError('Could not save voice settings. Try again.') }
    finally { setSaving(false) }
  }
  return <>
    <SettingsSection title="Dictation">
      <SettingRow title="Transcription service" status={draft.mode === 'server' ? (supported ? 'Recording is sent to your server.' : capabilities?.server?.reason || 'Server support has not been reported.') : 'Start dictation with the microphone button in the message box.'} control={
        <div role="radiogroup" aria-label="Transcription service" className="flex gap-1" onKeyDown={selectPolicyWithArrowKeys}>
          {([{ id: 'system', label: 'System' }, { id: 'server', label: 'Server' }, { id: 'local', label: 'Local AI' }] as const).map((option) =>
            <Button key={option.id} size="sm" variant={draft.mode === option.id ? 'primary' : 'secondary'} role="radio" aria-checked={draft.mode === option.id}
              disabled={saving || capabilities?.[option.id]?.supported !== true} title={capabilities?.[option.id]?.reason || (capabilities?.[option.id] ? undefined : 'Support has not been reported.')}
              onClick={() => edit({ ...draft, mode: option.id })}>{option.label}</Button>)}
        </div>
      } />
      {draft.mode === 'server' && <div className="grid gap-3 p-3">
        {([{ key: 'serverBaseUrl', label: 'Server URL' }, { key: 'serverEndpoint', label: 'Endpoint' }, { key: 'serverModel', label: 'Model' }, { key: 'apiKeyEnv', label: 'API key environment variable' }] as const).map((field) =>
          <SettingsField key={field.key} label={field.label} htmlFor={`voice-${field.key}`} hint={field.key === 'apiKeyEnv' ? 'Read from this environment variable.' : undefined}>
            <input id={`voice-${field.key}`} aria-label={field.label} className="uam-field w-full" value={draft[field.key] ?? ''} disabled={saving} autoComplete="off" spellCheck={false} onChange={(event) => edit({ ...draft, [field.key]: event.currentTarget.value })} />
          </SettingsField>)}
      </div>}
      {(error || validationError) && <p role="alert" className="px-3 text-xs" style={{ color: 'var(--error)' }}>{error || validationError}</p>}
      <div className="flex justify-end gap-2 p-3">
        <Button size="sm" disabled={saving || !dirty} onClick={() => edit(saved)}>Discard</Button>
        <SaveButton saving={saving} disabled={saving || !dirty || !supported || Boolean(validationError)} onClick={() => void save()}>Save voice settings</SaveButton>
      </div>
    </SettingsSection>
    {exit && <UnsavedChangesDialog title="Unsaved voice settings" busy={saving} error={error || validationError} onStay={() => setExit(null)}
      onDiscard={() => { const next = exit; edit(saved); setExit(null); next() }} onSave={supported && !validationError ? () => void save(exit) : undefined} />}
  </>
})

const ProviderAvailabilityControls = forwardRef<SettingsDraftHandle, { providers: Provider[]; hostId: string; hostLabel: string }>(function ProviderAvailabilityControls({ providers, hostId, hostLabel }, ref) {
  const overrides = useAppStore((state) => state.providerAvailability)
  const saveSettings = useAppStore((state) => state.setProviderAvailability)
  const saved = Object.fromEntries(providers.map((provider) => [provider.id, overrides.find((entry) => entry.providerId === provider.id && entry.executionHostId === hostId)?.enabled ?? true]))
  const [draft, setDraft] = useState(saved)
  const [saving, setSaving] = useState(false)
  const [error, setError] = useState('')
  const [exit, setExit] = useState<(() => void) | null>(null)
  const dirty = providers.some((provider) => draft[provider.id] !== saved[provider.id])
  const previousSaved = useRef(saved)
  useEffect(() => {
    setDraft((current) => Object.fromEntries(providers.map((provider) => [provider.id, current[provider.id] === previousSaved.current[provider.id] ? saved[provider.id] : current[provider.id]])))
    previousSaved.current = saved
  }, [overrides])
  useEffect(() => {
    if (!dirty) return
    const guard = (event: BeforeUnloadEvent) => { event.preventDefault(); event.returnValue = '' }
    window.addEventListener('beforeunload', guard)
    return () => window.removeEventListener('beforeunload', guard)
  }, [dirty])
  useImperativeHandle(ref, () => ({ requestLeave: (next) => { if (saving) return; if (dirty) setExit(() => next); else next() } }))
  const save = async (next?: () => void) => {
    if (saving) return
    setSaving(true); setError('')
    try {
      const results = await Promise.all(providers.filter((provider) => draft[provider.id] !== saved[provider.id]).map((provider) => saveSettings({ providerId: provider.id, executionHostId: hostId, enabled: draft[provider.id] })))
      if (results.some((ok) => !ok)) { setError('Could not save provider settings. Try again.'); return }
      const currentOverrides = useAppStore.getState().providerAvailability
      setDraft(Object.fromEntries(providers.map((provider) => [provider.id, currentOverrides.find((entry) => entry.providerId === provider.id && entry.executionHostId === hostId)?.enabled ?? true])))
      if (next) { setExit(null); next() }
    } catch { setError('Could not save provider settings. Try again.') }
    finally { setSaving(false) }
  }
  return <>
    {providers.map((provider) => <SettingRow key={provider.id} title={providerShortName(provider, provider.id)}
        control={<MenuSelect label={`${providerShortName(provider, provider.id)} availability`} value={draft[provider.id] ? 'enabled' : 'disabled'} disabled={saving}
          options={[{ value: 'enabled', label: 'Enabled' }, { value: 'disabled', label: 'Disabled' }]}
          onChange={(value) => { setDraft((current) => ({ ...current, [provider.id]: value === 'enabled' })); setError('') }} />} />)}
      {error && <p role="alert" className="px-3 text-xs" style={{ color: 'var(--error)' }}>{error}</p>}
      <div className="flex justify-end gap-2 p-3"><Button size="sm" disabled={saving || !dirty} onClick={() => { setDraft(saved); setError('') }}>Discard</Button>
        <SaveButton saving={saving} disabled={saving || !dirty} onClick={() => void save()}>Save provider settings</SaveButton></div>
    {exit && <UnsavedChangesDialog title="Unsaved provider settings" busy={saving} error={error} onStay={() => setExit(null)} onDiscard={() => { const next = exit; setExit(null); next() }} onSave={() => void save(exit)} />}
  </>
})

interface MemoryModelOption {
  disabled?: boolean
  id: string
  label: string
  detail: string
}

const PERMISSION_DEFAULT_OPTIONS: MemoryModelOption[] = [
  { id: 'off', label: 'Ask every time', detail: 'Leave every permission request for you.' },
  { id: 'acceptEdits', label: 'Accept edits', detail: 'Approve workspace file edits; ask for everything else.' },
  { id: 'aiReview', label: 'AI Review', detail: 'Use the configured isolated reviewer; failures and uncertainty return to you.' },
  { id: 'yolo', label: 'YOLO', detail: 'Approve every permission request once.' },
]

function providerDisplayName(provider?: Provider, fallbackId = '') {
  return providerShortName(provider, fallbackId)
}

function latestLocalProviderSession(sessions: Session[], providerId: string, executionHostId = 'local') {
  return sessions.reduce<Session | undefined>((latest, session) => {
    if (session.providerId !== providerId || (session.executionHostId || 'local') !== executionHostId) return latest
    if (!latest || session.updatedAt.getTime() > latest.updatedAt.getTime()) return session
    return latest
  }, undefined)
}

function cliActionErrorKey(providerId: string, executionHostId: string) {
  return `${executionHostId || 'local'}:${providerId}`
}

// Mirrors the backend rule: the host instruction file is empty or an absolute path on that host.
function remoteInstructionFileError(platform: string | undefined, rawValue: string) {
  const value = rawValue.trim()
  if (!value) return ''
  const windows = platform === 'windows' || platform === 'Windows'
  if (windows) return /^[A-Za-z]:[\\/]/.test(value) || (value.startsWith('\\\\') && value.length >= 5) ? '' : 'Use an absolute path such as C:\\Users\\you\\AGENTS.md.'
  return value.startsWith('/') ? '' : 'Use an absolute path such as /home/you/AGENTS.md.'
}

const HOST_INSTRUCTION_TEXT_MAX_BYTES = 256 * 1024

type RemoteHostOptionsDraft = { instructionFile: string; instructionText?: string | null; startupEnabled: boolean; cliUpdatesAsAdmin: boolean }

function RemoteHostOptionsForm({ host, busy, onSave, onCheck }: { host: ExecutionHost; busy: boolean; onSave: (draft: RemoteHostOptionsDraft) => void; onCheck: (instructionFile: string) => void }) {
  const [instructionFile, setInstructionFile] = useState(host.instructionFile || '')
  // null = no inline text (the legacy instruction file applies); a string is inline text, where '' suppresses the host fragment.
  const [inlineText, setInlineText] = useState<string | null>(host.instructionText ?? null)
  const [startupEnabled, setStartupEnabled] = useState(Boolean(host.startupEnabled))
  const [cliUpdatesAsAdmin, setCliUpdatesAsAdmin] = useState(Boolean(host.cliUpdatesAsAdmin))
  const instructionError = remoteInstructionFileError(host.platform, instructionFile)
  const textBytes = new TextEncoder().encode(inlineText ?? '').length
  const textError = textBytes > HOST_INSTRUCTION_TEXT_MAX_BYTES ? 'Host instructions are over 256 KiB. Shorten them to save.' : ''
  // Clearing the box on a host still using the file keeps it in file mode instead of suppressing the file.
  const changeText = (value: string) => setInlineText(value === '' && (host.instructionText ?? null) === null ? null : value)
  const textDirty = inlineText !== (host.instructionText ?? null)
  const dirty = instructionFile.trim() !== (host.instructionFile || '').trim()
    || textDirty
    || startupEnabled !== Boolean(host.startupEnabled)
    || cliUpdatesAsAdmin !== Boolean(host.cliUpdatesAsAdmin)
  const saveReason = textError || instructionError ? 'Fix the errors above first.' : dirty ? undefined : 'No changes to save.'
  // Switch = saved preference; status = applied state on the host. Without runnerStartupApplied, keep the legacy line.
  const applied = host.runnerStartupApplied
  const appliedOn = applied === 'enabled'
  const startupMismatch = applied === 'failed' || ((applied === 'enabled' || applied === 'disabled') && appliedOn !== Boolean(host.startupEnabled))
  const startupStatus = applied ? (
    <span className="inline-flex flex-wrap items-center gap-1.5">
      <StatusDot tone={applied === 'enabled' ? 'success' : applied === 'failed' ? 'error' : applied === 'pending' ? 'info' : 'neutral'} pulse={applied === 'pending'} />
      {applied === 'enabled' && <span>Starts automatically</span>}
      {applied === 'disabled' && <span>Doesn't start automatically</span>}
      {applied === 'pending' && <span>Applying…</span>}
      {applied === 'failed' && <span style={{ color: 'var(--error)' }}>{`Couldn't apply${host.runnerStartupError ? `: ${host.runnerStartupError}` : '.'}`}</span>}
      {startupMismatch && <span style={{ color: 'var(--text-3)' }}>· Saved setting doesn't match what is applied.</span>}
    </span>
  ) : `Currently ${!host.startupStatus || host.startupStatus === 'disabled' ? 'off' : host.startupStatus} · uses login or systemd user session`
  const textHint = inlineText === null
    ? 'Not set. The instruction file below is read on this host.'
    : inlineText === ''
      ? 'Empty. No host instructions are sent, and the instruction file is skipped.'
      : 'Sent to this host. The instruction file below is skipped.'
  return (
    <form className="mt-2 grid gap-2" onSubmit={(event) => {
      event.preventDefault()
      if (!busy && dirty && !instructionError && !textError) onSave({ instructionFile, instructionText: textDirty ? inlineText : undefined, startupEnabled, cliUpdatesAsAdmin })
    }}>
      <SettingRow
        title="Start runner at login"
        description="Starts the helper when you sign in."
        status={startupStatus}
        control={<Switch hideLabel name="startupEnabled" label="Start runner at login" checked={startupEnabled} disabled={busy} onChange={(event) => setStartupEnabled(event.currentTarget.checked)} />}
      />
      {host.platform === 'linux' && (
        <SettingRow
          title="Run CLI updates with sudo"
          description="Needs passwordless sudo for npm. Chats still run as you."
          control={<Switch hideLabel name="cliUpdatesAsAdmin" label="Run CLI updates with sudo" checked={cliUpdatesAsAdmin} disabled={busy} onChange={(event) => setCliUpdatesAsAdmin(event.currentTarget.checked)} />}
        />
      )}
      <SettingsField label="Host instructions" htmlFor={`remote-host-instructions-${host.id}`} hint={textHint}>
        <textarea id={`remote-host-instructions-${host.id}`} name="instructionText" aria-label={`Host instructions on ${host.label}`} aria-invalid={Boolean(textError) || undefined} value={inlineText ?? ''} onChange={(event) => changeText(event.currentTarget.value)} disabled={busy} rows={5} spellCheck={false} placeholder="Describe how agents should work on this host." className="uam-field w-full font-mono" style={{ minHeight: 96, height: 'auto', resize: 'vertical' }} />
        {textError && <span role="alert" className="uam-set-field__hint" style={{ color: 'var(--error)' }}>{textError}</span>}
        {inlineText !== null && <div><Button size="sm" variant="ghost" type="button" disabled={busy} onClick={() => setInlineText(null)}>Use instruction file instead</Button></div>}
      </SettingsField>
      <SettingsField label="Legacy instruction file" htmlFor={`remote-host-instruction-${host.id}`} hint="Used only while the host instructions above are not set. Empty skips it.">
        <input id={`remote-host-instruction-${host.id}`} name="instructionFile" aria-label={`Instruction file on ${host.label}`} aria-invalid={Boolean(instructionError) || undefined} value={instructionFile} onChange={(event) => setInstructionFile(event.currentTarget.value)} disabled={busy} placeholder={host.platform === 'windows' ? 'C:\\Users\\you\\AGENTS.md' : '/home/you/AGENTS.md'} maxLength={4096} spellCheck={false} className="uam-field w-full font-mono" />
        {instructionError && <span role="alert" className="uam-set-field__hint" style={{ color: 'var(--error)' }}>{instructionError}</span>}
      </SettingsField>
      <div className="flex justify-end gap-2">
        <Button size="sm" variant="secondary" type="button" disabled={busy || !instructionFile.trim() || Boolean(instructionError)} onClick={() => onCheck(instructionFile)}>Check file</Button>
        <Button size="sm" variant="primary" type="submit" disabled={busy || !dirty || Boolean(instructionError) || Boolean(textError)} title={saveReason}>Save host options</Button>
      </div>
    </form>
  )
}

function workspaceKey(value: string | undefined) {
  return (value ?? '').trim().replace(/\\/g, '/').replace(/\/+$/, '').toLowerCase()
}

function runnerDirectoryError(rawValue: string) {
  const value = rawValue.trim()
  if (!value) return 'Enter a folder under the remote home directory, for example uam-helper.'
  if (rawValue !== value) return 'Remove the leading or trailing spaces.'
  if (value.length > 240) return `Shorten the folder path to 240 characters or fewer; it is currently ${value.length}.`
  if (value.startsWith('~')) return 'Do not include ~. The folder is already placed under the remote home directory.'
  if (value.startsWith('/') || value.startsWith('\\')) return 'Remove the leading slash. Enter a path relative to the remote home directory.'
  if (value.endsWith('/') || value.endsWith('\\')) return 'Remove the trailing slash.'
  if (value.includes('\\')) return 'Use / between folders instead of \\.'
  const segments = value.split('/')
  if (segments.some((segment) => segment.length === 0)) return 'Remove the empty folder segment created by //.'
  if (segments.includes('..')) return 'Remove the .. segment; the helper must stay under the remote home directory.'
  if (segments.includes('.')) return 'Remove the . segment and enter the folder name directly.'
  const invalidCharacter = Array.from(value).find((character) => !/[A-Za-z0-9._\/-]/.test(character))
  if (invalidCharacter) return `Remove “${invalidCharacter}”. Use only letters, numbers, dots, dashes, underscores, and /.`
  return ''
}

function memoryModelOptions(provider?: Provider, providerId = '', selectedModelId = '', discoveredModels: AcpModel[] = []) {
  const caps = providerCapabilities(providerId, provider)
  const baseOptions = discoveredModels.length > 0
    ? [
        { id: '', label: caps.memoryModelLabels['']?.label ?? 'Default', detail: caps.memoryModelLabels['']?.detail ?? 'Use provider settings' },
        ...discoveredModels.map((model) => ({
          id: model.id,
          label: model.name || titleFromModelId(model.id),
          detail: model.description || model.id,
        })),
      ].filter((option, index, options) => options.findIndex((candidate) => candidate.id === option.id) === index)
    : caps.memoryModelIds.map((id) => ({
        id,
        label: caps.memoryModelLabels[id]?.label ?? titleFromModelId(id),
        detail: caps.memoryModelLabels[id]?.detail ?? id,
      }))
  if (!selectedModelId || baseOptions.some((option) => option.id === selectedModelId)) return baseOptions
  return [
    ...baseOptions,
    { id: selectedModelId, label: titleFromModelId(selectedModelId), detail: selectedModelId },
  ]
}

function selectedMemoryModelLabel(options: MemoryModelOption[], modelId: string) {
  return options.find((option) => option.id === modelId)?.label ?? titleFromModelId(modelId)
}

type SettingsSectionId = `provider:${string}` | 'provider-summary' | 'permissions' | 'central-configuration' | 'appearance' | 'typography' | 'defaults' | 'agents' | 'cli-version' | 'remote-hosts' | 'routing' | 'phone' | 'voice-input' | 'memory-settings' | 'memory-store' | 'markdown-store' | 'goal-loops' | 'mcp-servers' | 'vcs' | 'editors' | 'shell-actions' | 'chat-data' | 'storage-cleanup' | 'computer-use' | 'about'

interface LocalChatBundleResult {
  cancelled: boolean
  status: 'complete' | 'degraded' | 'failed' | 'cancelled'
  folder: string
  totalCount: number
  exportedCount?: number
  importedCount?: number
  failedCount?: number
  renamedCount?: number
  warnings: string[]
  errors: string[]
}

interface SettingsSection {
  id: SettingsSectionId
  label: string
  icon: LucideIcon
}

const SETTINGS_SECTIONS: SettingsSection[] = [
  { id: 'permissions', label: 'Permissions', icon: Check },
  { id: 'appearance', label: 'Appearance', icon: Palette },
  { id: 'defaults', label: 'Chat Defaults', icon: MessageSquare },
  { id: 'agents', label: 'Agents', icon: ClipboardList },
  { id: 'provider-summary', label: 'Overview', icon: TerminalSquare },
  ...SUPPORTED_CLI_PROVIDER_IDS.map(id => ({ id: `provider:${id}` as SettingsSectionId, label: providerMetadataForId(id).shortName, icon: TerminalSquare })),
  ...(SSH_ENABLED ? [{ id: 'remote-hosts', label: 'Remote Hosts', icon: Server }, { id: 'routing', label: 'SSH Routing', icon: Route }] as SettingsSection[] : []),
  ...(MOBILE_COMPANION_ENABLED ? [{ id: 'phone', label: 'Phone', icon: Smartphone }] as SettingsSection[] : []),
  { id: 'voice-input', label: 'Voice Input', icon: Mic },
  { id: 'memory-settings', label: 'Memory Settings', icon: Brain },
  { id: 'memory-store', label: 'Memory Store', icon: MemoryStick },
  { id: 'markdown-store', label: 'Skills', icon: BookOpen },
  { id: 'goal-loops', label: 'Goal Loops', icon: Target },
  { id: 'central-configuration', label: 'Central Setup', icon: Server },
  { id: 'mcp-servers', label: 'MCP Servers', icon: TerminalSquare },
  ...(COMPUTER_USE_ENABLED ? [{ id: 'computer-use', label: 'Computer Use', icon: MousePointerClick }] as SettingsSection[] : []),
  { id: 'vcs', label: 'Version Control', icon: FolderOpen },
  { id: 'editors', label: 'Editors', icon: Pencil },
  { id: 'shell-actions', label: 'Shell Actions', icon: MousePointerClick },
  { id: 'chat-data', label: 'Chat Data', icon: Download },
  { id: 'storage-cleanup', label: 'Storage', icon: Trash2 },
    { id: 'about', label: 'About', icon: Info },
]

// Retired pages that now live inside another page; old ids still resolve (search, deep links).
const SETTINGS_SECTION_ALIASES: Partial<Record<string, SettingsSectionId>> = { typography: 'appearance' }

const SETTINGS_GROUPS: { label: string; sections: SettingsSectionId[] }[] = [
  { label: 'General', sections: ['appearance', 'defaults', ...(MOBILE_COMPANION_ENABLED ? ['phone' as const] : []), 'voice-input'] },
  { label: 'Providers', sections: ['provider-summary', ...SUPPORTED_CLI_PROVIDER_IDS.map(id => `provider:${id}` as SettingsSectionId)] },
  { label: 'Connections', sections: [...(SSH_ENABLED ? ['remote-hosts' as const, 'routing' as const] : []), 'mcp-servers'] },
  { label: 'Agent preferences', sections: ['central-configuration', 'agents', 'markdown-store', 'memory-settings', 'memory-store', 'shell-actions', 'goal-loops'] },
  { label: 'Security', sections: ['permissions', ...(COMPUTER_USE_ENABLED ? ['computer-use' as const] : [])] },
  { label: 'Workspace', sections: ['vcs', 'editors'] },
  { label: 'App', sections: ['chat-data', 'storage-cleanup', 'about'] },
]
const SETTINGS_SECTION_DESCRIPTIONS: Partial<Record<string, string>> = {
  appearance: 'Theme, sidebar, how agent activity is shown, and fonts.',
  defaults: 'Default provider and permission reviewer for new chats.',
  'voice-input': 'Dictation for the message box.',
  'cli-version': 'Installed provider CLIs and their versions.',
  ...(SSH_ENABLED ? { 'remote-hosts': 'Computers that can run workspaces over SSH.', routing: 'Which computers run new chats and which remote folders they use.' } : {}),
  'mcp-servers': 'Model Context Protocol servers available to agents.',
  ...(MOBILE_COMPANION_ENABLED ? { phone: 'Use Universal Agent Manager from your phone on the same network.' } : {}),
}


const PROVIDER_SEARCH_TERMS = [CODEX_ENABLED && 'codex', GEMINI_ENABLED && 'gemini', CLAUDE_ENABLED && 'claude', COPILOT_ENABLED && 'copilot', OPENCODE_ENABLED && 'opencode'].filter(Boolean).join(' ')
/** Native agent files UAM can import, in menu order; only providers in this build. */
const AGENT_IMPORT_PROVIDER_OPTIONS = [
  ...(OPENCODE_ENABLED ? [{ value: OPENCODE_CLI_PROVIDER_ID, label: 'OpenCode' }] : []),
  ...(COPILOT_ENABLED ? [{ value: COPILOT_CLI_PROVIDER_ID, label: 'GitHub Copilot CLI' }] : []),
  ...(GEMINI_ENABLED ? [{ value: GEMINI_CLI_PROVIDER_ID, label: 'Gemini CLI' }] : []),
  ...(CLAUDE_ENABLED ? [{ value: CLAUDE_CLI_PROVIDER_ID, label: 'Claude Code' }] : []),
]
/** Structured protocols that can host UAM control; excluded providers contribute none. */
const UAM_CONTROL_PROTOCOLS = [
  ...(CODEX_ENABLED ? ['codex-app-server'] : []), ...(CLAUDE_ENABLED ? ['claude-code-stream-json'] : []), ...(GEMINI_ENABLED ? ['gemini-acp'] : []),
  ...(OPENCODE_ENABLED ? ['opencode-acp'] : []), ...(COPILOT_ENABLED ? ['copilot-acp', 'copilot-sdk'] : []), ...(ANTIGRAVITY_ENABLED ? ['antigravity-stream-json'] : []),
]

// Terms name controls inside each page so searches work beyond sidebar titles.
const SETTINGS_SEARCH_TERMS: Partial<Record<SettingsSectionId, string>> = {
  permissions: 'privacy local network microphone speech recording accessibility authorization',
  vcs: 'git svn source control version control worktrees checkpoints commits revisions base branch merge squash patch pull',
  'storage-cleanup': 'data folder directory storage migration restart cleanup retention worktrees backup logs cache days',
  appearance: 'themes colours colors palettes dark light import export sidebar icons worktree paths compact working activity typography font fonts size text code monospace terminal message input',
  defaults: `models reasoning providers permissions safety approval yolo memory defaults ${PROVIDER_SEARCH_TERMS} reviewer`,
  agents: `agents agent store create edit delete helpers subagents instructions import providers ${PROVIDER_SEARCH_TERMS}`,
  'cli-version': `installed recommended latest download update version ${PROVIDER_SEARCH_TERMS}`,
  ...(SSH_ENABLED ? { 'remote-hosts': 'computers ssh connections hosts runners servers install', routing: 'routing ssh automatic remote folders mapping' } : {}),
  ...(MOBILE_COMPANION_ENABLED ? { phone: 'phone mobile companion token url remote access' } : {}),
  'voice-input': 'microphone dictation transcription speech whisper',
  'memory-settings': 'memory workers recall budgets idle models tokens processing',
  'memory-store': 'memory library lessons entries search delete',
  'markdown-store': 'skills markdown library import folder file create pin',
  'goal-loops': 'goals loops tokens budgets auto continue review',
  'central-configuration': `central provider setup instructions agents.md skills files defaults${SSH_ENABLED ? ' ssh' : ''}`,
  'mcp-servers': 'mcp computer browser control setup server config json',
  ...(COMPUTER_USE_ENABLED ? { 'computer-use': 'computer use screen recording accessibility permissions allowed targets applications recovery macos' } : {}),
  editors: 'editors extensions associations groups default vscode clion file types file explorer finder open powershell pwsh terminal shell',
  'shell-actions': 'finder explorer context menus shell actions groups save',
  'chat-data': 'chat data history storage import export backup',
  about: 'version build release updates app information',
}

const WINDOWS_POWERSHELL_OPTIONS: { value: WindowsPowerShellPreference; label: string }[] = [
  { value: 'auto', label: 'Automatic' },
  { value: 'windows-powershell', label: 'Windows PowerShell 5.1' },
  { value: 'pwsh', label: 'PowerShell 7' },
]
const WINDOWS_POWERSHELL_DESCRIPTIONS: Record<WindowsPowerShellPreference, string> = {
  auto: "Uses Windows PowerShell 5.1, or PowerShell 7 if it's missing.",
  'windows-powershell': 'Uses built-in Windows PowerShell 5.1.',
  pwsh: "Uses PowerShell 7; terminals won't open if it isn't installed.",
}

const EDITOR_PRESETS = [
  { id: 'vscode', label: 'VS Code' },
  { id: 'xcode', label: 'Xcode' },
  { id: 'visualstudio', label: 'Visual Studio' },
  { id: 'clion', label: 'CLion' },
  { id: 'rider', label: 'Rider' },
  { id: 'webstorm', label: 'WebStorm' },
  { id: 'pycharm', label: 'PyCharm' },
  { id: 'idea', label: 'IntelliJ IDEA' },
  { id: 'goland', label: 'GoLand' },
  { id: 'rustrover', label: 'RustRover' },
]

const THEME_COLOR_FIELDS: Array<{ key: keyof ThemeColors; label: string }> = [
  { key: 'background', label: 'Background' },
  { key: 'surface', label: 'Surface' },
  { key: 'surfaceUp', label: 'Raised surface' },
  { key: 'text', label: 'Text' },
  { key: 'textMuted', label: 'Muted text' },
  { key: 'accent', label: 'Accent' },
  { key: 'sidebar', label: 'Sidebar' },
  { key: 'userMessage', label: 'User message' },
  { key: 'assistantMessage', label: 'Assistant message' },
  { key: 'success', label: 'Success' },
  { key: 'warning', label: 'Warning' },
  { key: 'error', label: 'Error' },
]
const THEME_COLOR_PATTERN = /^#[0-9A-Fa-f]{6}$/
// Shared surface for settings dialogs: title, body, then actions right-aligned with the primary last.
const SETTINGS_DIALOG_STYLE = { background: 'var(--surface)', border: '1px solid var(--border-bright)', boxShadow: 'var(--elev-3)' }

const PAGE_EXIT_MS = 120
/**
 * Cross-fades settings pages. The previous page stays mounted for its 120ms fade-out
 * (not remounted, so its effects do not re-run); the new page fades in via .uam-settings-page.
 */
function SettingsPageSwap({ pageKey, className, children }: { pageKey: string; className: string; children: ReactNode }) {
  const [shownKey, setShownKey] = useState(pageKey)
  const [leaving, setLeaving] = useState<{ key: string; node: ReactNode } | null>(null)
  const committed = useRef({ key: pageKey, node: children })
  if (shownKey !== pageKey) {
    setShownKey(pageKey)
    if (motionAllowed()) setLeaving(committed.current)
  }
  useEffect(() => { committed.current = { key: pageKey, node: children } })
  useEffect(() => {
    if (!leaving) return
    const timer = window.setTimeout(() => setLeaving(null), PAGE_EXIT_MS)
    return () => window.clearTimeout(timer)
  }, [leaving])
  return (
    // One keyed array so the outgoing page keeps its instance (same key) instead of remounting.
    <div className="uam-settings-swap">
      {[
        leaving && <div key={leaving.key} aria-hidden className={`${className} uam-settings-page--leaving`}>{leaving.node}</div>,
        <div key={pageKey} className={className}>{children}</div>,
      ]}
    </div>
  )
}
// Preview colours for the theme grid: background, surface, text and accent from each palette in index.css.
type ThemeSwatch = { background: string; surface: string; accent: string; text: string }
const BUILT_IN_THEME_SWATCHES: Record<BuiltInTheme, ThemeSwatch> = {
  focus: { background: '#0b0c0f', surface: '#111318', accent: '#cf7538', text: '#f2f4f7' },
  dark: { background: '#0f1117', surface: '#12141b', accent: '#ff6a00', text: '#eef1f7' },
  light: { background: '#f7f8fa', surface: '#ffffff', accent: '#ff6a00', text: '#141820' },
  // Follows the OS: split preview of the light and dark palettes.
  system: { background: 'linear-gradient(135deg, #f7f8fa 50%, #0f1117 50%)', surface: 'linear-gradient(135deg, #ffffff 50%, #12141b 50%)', accent: '#ff6a00', text: 'linear-gradient(135deg, #141820 50%, #eef1f7 50%)' },
  midnight: { background: '#0b1020', surface: '#11182b', accent: '#60a5fa', text: '#e6eefc' },
  paper: { background: '#f8f7f1', surface: '#fffef9', accent: '#0f766e', text: '#26302a' },
  dusk: { background: '#211c2d', surface: '#292239', accent: '#c4a7ff', text: '#f1eafd' },
  aurora: { background: '#09252c', surface: '#0d3038', accent: '#f472b6', text: '#e1fff7' },
  contrast: { background: '#000000', surface: '#111111', accent: '#00ffff', text: '#ffffff' },
}

function customThemeId(name: string, themes: CustomTheme[]) {
  const base = name.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '').slice(0, 40) || 'theme'
  const ids = new Set(themes.map((theme) => theme.id))
  let id: CustomTheme['id'] = `custom:${base}`
  let suffix = 2
  while (ids.has(id)) id = `custom:${base}-${suffix++}`
  return id
}

function editorPresetLabel(id: string) {
  return EDITOR_PRESETS.find((preset) => preset.id === id)?.label ?? 'VS Code'
}

function extensionsText(association: EditorFileAssociation) {
  return association.extensions.join(' ')
}

function parseExtensions(value: string) {
  return Array.from(new Set(
    value
      .split(/[,\s]+/)
      .map((extension) => extension.trim().toLowerCase())
      .filter(Boolean)
      .map((extension) => extension.startsWith('.') ? extension : `.${extension}`)
  ))
}

function versionStatusText(manager: CliVersionProviderState) {
  if (manager.status === 'checking') return 'Checking installed version'
  if (manager.status === 'installing') return 'Installing selected version'
  if (manager.status === 'verified') return `Verified by UAM${manager.verifiedAt ? ` on ${manager.verifiedAt}` : ''}`
  if (manager.status === 'untested-newer') return 'Newer than UAM’s last verified build'
  if (manager.status === 'untested') return 'Not yet verified by this UAM build'
  if (manager.status === 'known-incompatible') return 'Known incompatible. Update before structured use.'
  if (manager.status === 'unavailable') return 'Not installed or unavailable on PATH'
  if (manager.status === 'provider-managed') return 'Compatibility is managed by the provider'
  return 'Version has not been checked'
}

function SectionCard({ title, description, children, hideTitle = false, flat = false, actions }: { title: string; description?: string; children: ReactNode; hideTitle?: boolean; flat?: boolean; actions?: ReactNode }) {
  // Legacy page sections render in the shared settings language (SettingsPrimitives); bodies keep their own grid content.
  return (
    <section aria-label={title} className="uam-set-section">
      {(!hideTitle || description || actions) && <header className="uam-set-section__head">
        <div className="min-w-0">
          {!hideTitle && <h3 className="uam-set-section__title">{title}</h3>}
          {description && <p className="uam-set-section__desc">{description}</p>}
        </div>
        {actions && <div className="uam-set-section__actions">{actions}</div>}
      </header>}
      <div className={flat ? 'uam-set-body uam-set-body--flat' : 'uam-set-card uam-set-body'}>{children}</div>
    </section>
  )
}

function ProviderDisclosureCard({ panelId, providerId, leadingIcon, title, expanded, onToggle, toggleLabel, children, actions, progress, togglePosition = 'left', alwaysOpen = false }: {
  /** Provider pages show their single provider flat: no disclosure, content always visible. */
  alwaysOpen?: boolean
  panelId: string
  /** Shown under the header while work runs, so it stays visible when collapsed. */
  progress?: ReactNode
  providerId?: string
  leadingIcon?: ReactNode
  title: string
  expanded: boolean
  onToggle: () => void
  toggleLabel: string
  children: ReactNode
  actions?: ReactNode
  togglePosition?: 'left' | 'right'
}) {
  if (alwaysOpen) return <>
    {actions && <div className="flex justify-end">{actions}</div>}
    <div id={panelId} className="uam-provider-flat__body">{progress}{children}</div>
  </>
  const chevron = <IconButton size="sm" className="uam-disclosure-row__toggle" icon={<ChevronRight size={14}/>} label={toggleLabel} aria-expanded={expanded} aria-controls={panelId} onClick={onToggle}/>
  return <div className="uam-disclosure-row" data-expanded={expanded}>
    <div className="uam-disclosure-row__header">
      {togglePosition === 'left' && chevron}
      {leadingIcon ?? (providerId ? <ProviderLogo providerId={providerId}/> : null)}
      {/* Clicking the name also toggles; the chevron stays the keyboard control. */}
      <span className="min-w-0 flex-1 cursor-pointer select-none truncate text-sm" onClick={onToggle}>{title}</span>
      {actions && <span className="uam-disclosure-row__actions">{actions}</span>}
      {togglePosition === 'right' && chevron}
    </div>
    {progress}
    {expanded && <div id={panelId} className="uam-disclosure-row__panel uam-reveal">{children}</div>}
  </div>
}

function cliVersionIssues(manager: CliVersionProviderState): string[] {
  const healthy = manager.status === 'verified' || manager.status === 'provider-managed'
  return [...new Set([
    !healthy ? manager.message || versionStatusText(manager) : '',
    manager.lastInstallStatus === 'failed' ? 'Last installation failed.' : '',
  ].filter(Boolean))]
}

interface PhoneAccessStatus {
  configured: boolean
  enabled: boolean
  url: string
  desiredUrl?: string
  effectiveUrl?: string
  state?: 'disabled' | 'starting' | 'ready' | 'failed'
  reason?: string
  reasonCode?: string
  generation?: number
  restartRequired?: boolean
  localAddresses?: string[]
  proxyRunning?: boolean
  loopbackRunning?: boolean
}

export interface SettingsHandle {
  requestClose: () => void
  showMemory: () => void
}

export const SettingsModal = forwardRef<SettingsHandle>(function SettingsModal(_props, ref) {
  const setSettingsOpen = useAppStore((s) => s.setSettingsOpen)
  const runtimeProviders = useAppStore(useShallow((s) => s.providers))
  const providers = SUPPORTED_CLI_PROVIDER_IDS.map(id => runtimeProviders.find(provider => provider.id === id) ?? fallbackProviderForId(id))
  const sessions = useAppStore(useShallow((s) => s.sessions))
  const activeSessionId = useAppStore((s) => s.activeSessionId)
  const acpBindings = useAppStore(useShallow((s) => s.acpBindingBySessionId))
  const providerModelCatalogs = useAppStore(useShallow((s) => s.providerModelCatalogs))
  const folders = useAppStore(useShallow((s) => s.folders))
  const localWorkspace = folders.find((folder) => (folder.executionHostId || 'local') === 'local')?.directory || ''
  const discoverProviderModels = useAppStore((s) => s.discoverProviderModels)
  const memoryEnabledDefault = useAppStore((s) => s.memoryEnabledDefault)
  const memoryLevelDefault = useAppStore((s) => s.memoryLevelDefault)
  const memorySettingsUnconfirmedFields = useAppStore(useShallow((state) => state.memorySettingsUnconfirmedFields ?? []))
  const unconfirmedSettingsLabels = Array.from(new Set(memorySettingsUnconfirmedFields)).flatMap((field) =>
    Object.prototype.hasOwnProperty.call(MEMORY_SETTINGS_FIELD_LABELS, field) ? [MEMORY_SETTINGS_FIELD_LABELS[field]] : [])
  const setSessionMemoryLevel = useAppStore((s) => s.setSessionMemoryLevel)
  const [memoryPolicySaving, setMemoryPolicySaving] = useState<Record<string, boolean>>({})
  const [memoryPolicyError, setMemoryPolicyError] = useState<Record<string, string>>({})
  const saveMemoryPolicy = async (key: string, save: () => Promise<boolean>) => {
    if (memoryPolicySaving[key]) return
    const capturedScope = useAppStore.getState().settingsScopeHostId
    const ownsFeedback = () => useAppStore.getState().settingsScopeHostId === capturedScope
    setMemoryPolicySaving((current) => ({ ...current, [key]: true }))
    setMemoryPolicyError((current) => ({ ...current, [key]: '' }))
    try {
      if (!await save() && ownsFeedback()) setMemoryPolicyError((current) => ({ ...current, [key]: 'Could not save the memory policy. Try again.' }))
    } catch { if (ownsFeedback()) setMemoryPolicyError((current) => ({ ...current, [key]: 'Could not save the memory policy. Try again.' })) }
    finally { if (ownsFeedback()) setMemoryPolicySaving((current) => ({ ...current, [key]: false })) }
  }
  const memoryIdleDelaySeconds = useAppStore((s) => s.memoryIdleDelaySeconds)
  const memoryRecallBudgetBytes = useAppStore((s) => s.memoryRecallBudgetBytes)
  const goalMaxLoopIterations = useAppStore((s) => s.goalMaxLoopIterations)
  const acpSetupInactivityTimeoutSeconds = useAppStore((s) => s.acpSetupInactivityTimeoutSeconds)
  const acpTurnOutputLimitMiB = useAppStore((s) => s.acpTurnOutputLimitMiB)
  const appVersion = useAppStore((s) => s.appVersion)
  const uiBuildId = useAppStore((s) => s.uiBuildId)
  const localVersion = uiBuildId.match(/^(\d+\.\d+\.\d+-alpha-[1-9]\d*)-/)?.[1]
  const displayedVersion = localVersion?.startsWith(`${appVersion.replace(/^v/i, '')}-alpha-`) ? `V${localVersion}` : appVersion
  const expandWorkTraces = useAppStore((s) => s.expandWorkTraces)
  const setExpandWorkTraces = useAppStore((s) => s.setExpandWorkTraces)
  const collapsibleWorkSections = useAppStore((s) => s.collapsibleWorkSections)
  const setCollapsibleWorkSections = useAppStore((s) => s.setCollapsibleWorkSections)
  const workingDisplayMode = useAppStore((s) => s.workingDisplayMode)
  const setWorkingDisplayMode = useAppStore((s) => s.setWorkingDisplayMode)
  const showProviderIconsInSidebar = useAppStore((s) => s.showProviderIconsInSidebar)
  const showWorktreePathInSidebar = useAppStore((s) => s.showWorktreePathInSidebar)
  const setSidebarSettings = useAppStore((s) => s.setSidebarSettings)
  const updateChecksEnabled = useAppStore((s) => s.updateChecksEnabled)
  const setUpdateSettings = useAppStore((s) => s.setUpdateSettings)
  const customThemes = useAppStore(useShallow((s) => s.customThemes))
  const saveCustomTheme = useAppStore((s) => s.saveCustomTheme)
  const deleteCustomTheme = useAppStore((s) => s.deleteCustomTheme)
  const memoryWorkerBindings = useAppStore(useShallow((s) => s.memoryWorkerBindings))
  const permissionReviewerProviderId = useAppStore((s) => s.permissionReviewerProviderId)
  const permissionReviewerModelId = useAppStore((s) => s.permissionReviewerModelId)
  const memoryLastStatus = useAppStore((s) => s.memoryLastStatus)
  const memoryActivity = useAppStore(useShallow((s) => s.memoryActivity))
  const cliVersionManager = useAppStore(useShallow((s) => s.cliVersionManager))
  const markdownStoreDirectory = useAppStore((s) => s.markdownStoreDirectory)
  const isMarkdownStoreOpen = useAppStore((s) => s.isMarkdownStoreOpen)
  const defaultNewChatProviderId = useAppStore((s) => (s.hostScopedSettings && s.executionHosts.some((host) => isSshHost(host) && host.id === s.settingsScopeHostId) && s.hostScopedSettingsValues?.[s.settingsScopeHostId]?.defaultNewChatProviderId) || s.defaultNewChatProviderId)
  const providerChatDefaults = useAppStore(useShallow((s) => (s.hostScopedSettings && s.executionHosts.some((host) => isSshHost(host) && host.id === s.settingsScopeHostId) && s.hostScopedSettingsValues?.[s.settingsScopeHostId]?.providerChatDefaults) || s.providerChatDefaults))
  const fileExplorerApplication = useAppStore((s) => s.fileExplorerApplication)
  const windowsPowerShell = useAppStore((s) => s.windowsPowerShell)
  const isWindows = typeof navigator !== 'undefined' && /Win/i.test(navigator.platform || navigator.userAgent)
  const [explorerDraft, setExplorerDraft] = useState('')
  const [customExplorer, setCustomExplorer] = useState(false)
  useEffect(() => { setExplorerDraft(fileExplorerApplication); setCustomExplorer(Boolean(fileExplorerApplication)) }, [fileExplorerApplication])
  const defaultEditorPresetId = useAppStore((s) => s.defaultEditorPresetId)
  const editorFileAssociations = useAppStore(useShallow((s) => s.editorFileAssociations))
  const executionHosts = useAppStore(useShallow((s) => s.executionHosts))
  const favoriteUamAgentIds = useAppStore(useShallow((s) => s.favoriteUamAgentIds))
  const uamAgentCycleShortcut = useAppStore((s) => s.uamAgentCycleShortcut)
  const computerUseAllowlistEnabled = useAppStore((s) => s.computerUseAllowlistEnabled)
  const computerUseAllowedApplications = useAppStore(useShallow((s) => s.computerUseAllowedApplications))
  const activeUamAgents = useAppStore(useShallow((s) => activeSessionId ? s.uamAgentsBySessionId[activeSessionId] ?? [] : []))
  const setMemorySettings = useAppStore((s) => s.setMemorySettings)
  const setProviderChatDefaults = useAppStore((s) => s.setProviderChatDefaults)
  const setEditorSettings = useAppStore((s) => s.setEditorSettings)
  const setSessionComputerUseBackend = useAppStore((s) => s.setSessionComputerUseBackend)
  const setSessionUamControlEnabled = useAppStore((s) => s.setSessionUamControlEnabled)
  const setUamAgentPreferences = useAppStore((s) => s.setUamAgentPreferences)
  const refreshUamAgents = useAppStore((s) => s.refreshUamAgents)
  const browseProviderAgentImport = useAppStore((s) => s.browseProviderAgentImport)
  const previewProviderAgentImport = useAppStore((s) => s.previewProviderAgentImport)
  const importProviderAgent = useAppStore((s) => s.importProviderAgent)
  const refreshCliProviderVersion = useAppStore((s) => s.refreshCliProviderVersion)
  const applyCliProviderVersion = useAppStore((s) => s.applyCliProviderVersion)
  const browseMarkdownStoreDirectory = useAppStore((s) => s.browseMarkdownStoreDirectory)
  const setMarkdownStoreDirectory = useAppStore((s) => s.setMarkdownStoreDirectory)
  const { theme, setTheme } = useTheme()
  const [settingsSearch, setSettingsSearch] = useState('')
  const navRef = useRef<HTMLElement>(null)
  const [navIndicator, setNavIndicator] = useState<{ top: number; height: number } | null>(null)
  const searchTerms = settingsSearch.toLocaleLowerCase().trim().split(/\s+/).filter(Boolean)
  const visibleSettingsGroups = SETTINGS_GROUPS.map(group => ({...group, sections: group.sections.filter(id => {
    const section = SETTINGS_SECTIONS.find(item => item.id === id)
    if (!section) return false
    const searchable = `${group.label} ${section.label} ${SETTINGS_SEARCH_TERMS[id] ?? (id.startsWith('provider:') ? 'CLI recommended latest version installation models visibility MCP setup connection' : 'providers status overview installed MCP')}`.toLocaleLowerCase()
    return searchTerms.every(term => searchable.includes(term))
  })})).filter(group => group.sections.length > 0)
  const [openMemoryMenu, setOpenMemoryMenu] = useState<string | null>(null)
  const [openEditorMenu, setOpenEditorMenu] = useState<string | null>(null)
  const [openCliVersionMenu, setOpenCliVersionMenu] = useState<string | null>(null)
  const [pendingCliInstall, setPendingCliInstall] = useState<{ providerId: string; providerName: string; version: string } | null>(null)
  const [pendingDefaultYolo, setPendingDefaultYolo] = useState<{ providerId: string; providerName: string; defaults: ProviderChatDefaults } | null>(null)
  const [expandedDefaultProviders, setExpandedDefaultProviders] = useState<Record<string, boolean>>({})
  const [expandedEditorGroups, setExpandedEditorGroups] = useState<Record<string, boolean>>({})
  const [markdownStoreDraftDirectory, setMarkdownStoreDraftDirectory] = useState(markdownStoreDirectory)
  const [editorAssociationsDraft, setEditorAssociationsDraft] = useState(editorFileAssociations)
  const [defaultEditorDraft, setDefaultEditorDraft] = useState(defaultEditorPresetId)
  const [mcpDraftDirty, setMcpDraftDirty] = useState(false)
  const [confirmDiscard, setConfirmDiscard] = useState(false)
  const [mcpSaving, setMcpSaving] = useState(false)
  const [computerUseStatus, setComputerUseStatus] = useState<{ screenRecording: { available: boolean; error: string }; accessibility: { available: boolean; error: string } } | null>(null)
  const [computerUseApps, setComputerUseApps] = useState<Array<{ identityKind: ComputerUseAllowedApplication['identityKind']; identity: string; name: string }>>([])
  const [computerUseMessage, setComputerUseMessage] = useState('')
  const [computerUseBusy, setComputerUseBusy] = useState(false)
  const [computerUseSaveBusy, setComputerUseSaveBusy] = useState(false)
  const [companionSettings, setCompanionSettings] = useState<PhoneAccessStatus | null>(null)
  const [companionBusy, setCompanionBusy] = useState(false)
  const companionInFlight = useRef(false)
  const [companionMessage, setCompanionMessage] = useState('')
  const [companionLoadError, setCompanionLoadError] = useState('')
  const [companionNotice, setCompanionNotice] = useState('')
  const [companionBusyAction, setCompanionBusyAction] = useState('')
  const [companionCopy, setCompanionCopy] = useState<{ kind: 'token' | 'url'; ok: boolean } | null>(null)
  const companionCopyTimer = useRef<number | undefined>(undefined)
  useEffect(() => () => window.clearTimeout(companionCopyTimer.current), [])
  const computerUseSaveInFlight = useRef(false)
  const computerUseOsLabel = typeof navigator !== 'undefined' && /mac/i.test(navigator.platform) ? 'macOS' : 'your operating system'
  const [remoteLabel, setRemoteLabel] = useState('')
  const [remoteAlias, setRemoteAlias] = useState('')
  const [remoteBusyHostIds, setRemoteBusyHostIds] = useState<string[]>([])
  const remoteBusy = remoteBusyHostIds.length > 0
  const isRemoteHostBusy = (hostId: string) => remoteBusyHostIds.includes(hostId)
  const setRemoteHostBusy = (hostId: string, busy: boolean) => setRemoteBusyHostIds(current => busy ? [...current.filter(id => id !== hostId), hostId] : current.filter(id => id !== hostId))
  const [remoteMessage, setRemoteMessage] = useState('')
  const [remotePreview, setRemotePreview] = useState<{ host: ExecutionHost; preview: string } | null>(null)
  const [remoteCustomDirectory, setRemoteCustomDirectory] = useState(false)
  const [remoteDirectory, setRemoteDirectory] = useState('uam-helper')
  const remoteDirectoryValidation = remoteCustomDirectory ? runnerDirectoryError(remoteDirectory) : ''
  const [favoriteAgentCandidate, setFavoriteAgentCandidate] = useState('')
  const [agentImportProvider, setAgentImportProvider] = useState(AGENT_IMPORT_PROVIDER_OPTIONS[0]?.value ?? '')
  const [agentImportPath, setAgentImportPath] = useState('')
  const [agentImportId, setAgentImportId] = useState('')
  const [agentImportAccess, setAgentImportAccess] = useState<'read' | 'write'>('read')
  const [agentImportWorkspaceScope, setAgentImportWorkspaceScope] = useState(true)
  const [agentImportAcknowledged, setAgentImportAcknowledged] = useState(false)
  const [agentImportPreview, setAgentImportPreview] = useState<ProviderAgentImportPreview | null>(null)
  const [agentImportBusy, setAgentImportBusy] = useState(false)
  const [agentImportMessage, setAgentImportMessage] = useState('')
  const agentImportRequest = useRef(0)
  const invalidateAgentImport = () => {
    agentImportRequest.current += 1
    setAgentImportPreview(null)
    setAgentImportAcknowledged(false)
    setAgentImportMessage('')
    setAgentImportBusy(false)
  }
  useEffect(() => () => { agentImportRequest.current += 1 }, [])
  const [chatDataBusy, setChatDataBusy] = useState<'export' | 'import' | null>(null)
  const [chatDataFolder, setChatDataFolder] = useState('')
  const [chatDataMessage, setChatDataMessage] = useState<{ tone: 'success' | 'warning' | 'error' | 'info'; text: string } | null>(null)
  const [selectedSection, setSelectedSection] = useState<SettingsSectionId>('appearance')
  // Measure the active nav item so one indicator (tint + line) can glide between items.
  useLayoutEffect(() => {
    const nav = navRef.current
    if (!nav) return
    const measure = () => {
      const active = nav.querySelector<HTMLElement>('[data-active="true"]')
      setNavIndicator(active ? { top: active.offsetTop, height: active.offsetHeight } : null)
    }
    measure()
    if (typeof ResizeObserver === 'undefined') return
    const observer = new ResizeObserver(measure)
    observer.observe(nav)
    return () => observer.disconnect()
  }, [selectedSection, visibleSettingsGroups.length])
  const memoryRef = useRef<MemoryLibraryHandle>(null)
  const shellRef = useRef<ShellActionsHandle>(null)
  const centralRef = useRef<CentralProviderSettingsHandle>(null)
  const voiceRef = useRef<SettingsDraftHandle>(null)
  const vcsRef = useRef<SettingsDraftHandle>(null)
  const routingRef = useRef<ExecutionRoutingSettingsHandle>(null)
  const cleanupRef = useRef<SettingsDraftHandle>(null)
  const dataFolderRef = useRef<DataFolderSettingsHandle>(null)
  const providerAvailabilityRef = useRef<SettingsDraftHandle>(null)
  const [rawEditorNames, setRawEditorNames] = useState<Record<string, string>>({})
  const [rawExtensions, setRawExtensions] = useState<Record<string, string>>({})
  const [editorExit, setEditorExit] = useState<(() => void) | null>(null)
  const [editorError, setEditorError] = useState('')
  const [openWithError, setOpenWithError] = useState('')
  const editorSavePending = useRef(false)
  const [editorSaving, setEditorSaving] = useState(false)
  const [themeStep, setThemeStep] = useState(0)
  const [themeSaving, setThemeSaving] = useState(false)
  const [themeExit, setThemeExit] = useState<(() => void) | null>(null)
  const [discardExit, setDiscardExit] = useState<(() => void) | null>(null)
  const [pendingHostRemoval, setPendingHostRemoval] = useState<ExecutionHost | null>(null)
  const [cliActionError, setCliActionError] = useState<Record<string, string>>({})
  const [themeDraft, setThemeDraft] = useState<CustomTheme | null>(null)
  const [themeMessage, setThemeMessage] = useState('')
  const [pendingDelete, setPendingDelete] = useState<{ kind: 'theme' | 'editor'; id: string; name: string } | null>(null)
  const memoryMenuRef = useRef<HTMLDivElement>(null)
  const editorMenuRef = useRef<HTMLDivElement>(null)
  const cliVersionMenuRef = useRef<HTMLDivElement>(null)
  const popupAnchorsRef = useRef(new Map<string, HTMLButtonElement>())
  const themeImportRef = useRef<HTMLInputElement>(null)
  const dialogRef = useRef<HTMLDivElement>(null)
  const openerRef = useRef(document.activeElement instanceof HTMLElement ? document.activeElement : null)

  useEffect(() => {
    if (themeDraft) dialogRef.current?.querySelector<HTMLElement>(themeStep === 0 ? '[aria-label="Theme name"]' : '[data-theme-step]')?.focus()
  }, [themeDraft?.id, themeStep])
  const themeDirty = Boolean(themeDraft && JSON.stringify(themeDraft) !== JSON.stringify(customThemes.find(item => item.id === themeDraft.id)))
  const requestThemeExit = (next: () => void) => {
    if (themeSaving) return
    if (themeDirty) setThemeExit(() => next)
    else next()
  }
  const requestSectionExit = (next: () => void) => {
    if (selectedSection === 'vcs' && vcsRef.current) vcsRef.current.requestLeave(next)
    else if (selectedSection === 'storage-cleanup') {
      const leaveCleanup = () => cleanupRef.current ? cleanupRef.current.requestLeave(next) : next()
      if (dataFolderRef.current) dataFolderRef.current.requestLeave(leaveCleanup)
      else leaveCleanup()
    }
    else if (selectedSection === 'routing' && routingRef.current) routingRef.current.requestLeave(next)
    else if (selectedSection === 'voice-input' && voiceRef.current) voiceRef.current.requestLeave(next)
    else if ((selectedSection === 'provider-summary' || selectedSection.startsWith('provider:')) && providerAvailabilityRef.current) providerAvailabilityRef.current.requestLeave(next)
    else if (selectedSection === 'memory-store' && memoryRef.current) memoryRef.current.requestLeave(next)
    else if (selectedSection === 'shell-actions' && shellRef.current) shellRef.current.requestLeave(next)
    else if ((selectedSection === 'central-configuration' || selectedSection === 'agents') && centralRef.current) centralRef.current.requestLeave(next)
    else if (selectedSection === 'mcp-servers' && mcpDraftDirty) { setDiscardExit(() => next); setConfirmDiscard(true) }
    else next()
  }
  const editorDirty = editorAssociationsDraft.some(item => {
    const saved = editorFileAssociations.find(candidate => candidate.id === item.id)
    return (rawEditorNames[item.id] !== undefined && rawEditorNames[item.id].trim() !== saved?.name)
      || (rawExtensions[item.id] !== undefined && JSON.stringify(parseExtensions(rawExtensions[item.id])) !== JSON.stringify(saved?.extensions))
  })
  useEffect(() => {
    if (!themeDirty && !mcpDraftDirty && !editorDirty) return
    const guard = (event: BeforeUnloadEvent) => { event.preventDefault(); event.returnValue = '' }
    window.addEventListener('beforeunload',guard)
    return () => window.removeEventListener('beforeunload',guard)
  }, [themeDirty,mcpDraftDirty,editorDirty])
  const requestClose = () => {
    if (mcpSaving || editorSavePending.current || remoteBusy) return
    requestThemeExit(() => requestSectionExit(() => {
      // MCP edits were already confirmed by requestSectionExit while on the MCP page.
      const mcpPending = mcpDraftDirty && selectedSection !== 'mcp-servers'
      if (editorDirty) { setEditorExit(() => () => { setRawEditorNames({}); setRawExtensions({}); if (mcpPending) { setDiscardExit(() => () => setSettingsOpen(false)); setConfirmDiscard(true) } else setSettingsOpen(false) }); return }
      if (mcpPending) { setDiscardExit(() => () => setSettingsOpen(false)); setConfirmDiscard(true) }
      else setSettingsOpen(false)
    }))
  }
  const changeSection = (requested: SettingsSectionId) => {
    const section = SETTINGS_SECTION_ALIASES[requested] ?? requested
    if (!SETTINGS_SECTIONS.some((entry) => entry.id === section) || section === selectedSection || mcpSaving || editorSavePending.current || remoteBusy) return
    requestThemeExit(() => requestSectionExit(() => {
      setThemeDraft(null)
      setSelectedSection(section)
      setOpenMemoryMenu(null)
      setOpenEditorMenu(null)
      setOpenCliVersionMenu(null)
    }))
  }

  useImperativeHandle(ref, () => ({ requestClose, showMemory: () => changeSection('memory-store') }))

  const runChatDataAction = async (operation: 'export' | 'import') => {
    if (chatDataBusy) return
    setChatDataBusy(operation)
    setChatDataMessage(null)
    const response = await sendToCEF<LocalChatBundleResult>({
      action: operation === 'export' ? 'exportLocalChats' : 'importLocalChats',
      payload: { currentValue: chatDataFolder },
    })
    setChatDataBusy(null)
    if (!response.ok || !response.data) {
      setChatDataMessage({ tone: 'error', text: response.error || `Chat ${operation} failed.` })
      return
    }
    const result = response.data
    if (result.cancelled) {
      setChatDataMessage({ tone: 'info', text: 'No folder selected.' })
      return
    }
    setChatDataFolder(result.folder)
    const detail = [...result.errors, ...result.warnings][0]
    if (operation === 'export') {
      const text = result.status === 'complete'
        ? `Exported ${result.exportedCount ?? 0} chats to ${result.folder}.`
        : `Exported ${result.exportedCount ?? 0} of ${result.totalCount} chats. ${detail || 'The bundle is incomplete.'}`
      setChatDataMessage({ tone: result.status === 'complete' ? 'success' : result.status === 'degraded' ? 'warning' : 'error', text })
      return
    }
    const text = result.status === 'complete'
      ? `Imported ${result.importedCount ?? 0} chats${result.renamedCount ? `; ${result.renamedCount} received new local IDs` : ''}.`
      : `Imported ${result.importedCount ?? 0} of ${result.totalCount} chats${result.renamedCount ? `; ${result.renamedCount} received new local IDs` : ''}. ${detail || 'Some chats could not be imported.'}`
    setChatDataMessage({ tone: result.status === 'complete' ? 'success' : result.status === 'degraded' ? 'warning' : 'error', text })
  }

  const previewRemoteHost = async (existing?: ExecutionHost) => {
    const sshAlias = (existing?.sshAlias ?? remoteAlias).trim()
    const label = (existing?.label ?? remoteLabel).trim() || sshAlias
    const portableAlias = sshAlias.toLowerCase().replace(/[^a-z0-9_-]+/g, '-').replace(/^-+|-+$/g, '')
    const host = {
      id: existing?.id ?? `ssh-${portableAlias}`,
      label,
      sshAlias,
      runnerDirectory: existing?.runnerDirectory ?? '',
    }
    if (!portableAlias || !sshAlias) {
      setRemoteMessage('Enter one exact host alias from ~/.ssh/config.')
      return
    }
    setRemoteHostBusy(host.id, true)
    setRemoteMessage('')
	setRemoteCustomDirectory(Boolean(host.runnerDirectory))
	setRemoteDirectory(host.runnerDirectory || 'uam-helper')
    const response = await sendToCEF<{ host: ExecutionHost; preview: string }>({ action: 'previewRemoteHost', payload: host })
    setRemoteHostBusy(host.id, false)
    if (!response.ok || !response.data) {
      setRemoteMessage(response.error || 'The remote setup preview could not be created.')
      return
    }
    setRemotePreview(response.data)
  }

  const installRemoteHost = async () => {
    if (!remotePreview || isRemoteHostBusy(remotePreview.host.id)) return
	const runnerDirectory = remoteCustomDirectory ? remoteDirectory.trim() : ''
	if (remoteCustomDirectory && runnerDirectoryError(remoteDirectory)) return
    setRemoteHostBusy(remotePreview.host.id, true)
    setRemoteMessage('Connecting, verifying the copied helper, and starting the runner…')
    const response = await sendToCEF({ action: 'installRemoteHost', payload: { ...remotePreview.host, runnerDirectory } })
    setRemoteHostBusy(remotePreview.host.id, false)
    if (!response.ok) {
      setRemotePreview(null)
      setRemoteMessage(response.error || 'Remote helper setup failed.')
      return
    }
    setRemoteMessage(`${remotePreview.host.label} is ready.`)
    setRemotePreview(null)
    setRemoteAlias('')
    setRemoteLabel('')
  }

  const saveRemoteHostOptions = async (host: ExecutionHost, draft: RemoteHostOptionsDraft) => {
    if (isRemoteHostBusy(host.id)) return
    setRemoteHostBusy(host.id, true)
    // instructionText is omitted unless changed, so the backend keeps the saved text.
    const response = await sendToCEF({ action: 'saveRemoteHostOptions', payload: { id: host.id, ...draft } })
    setRemoteHostBusy(host.id, false)
    setRemoteMessage(response.ok ? `${host.label} options saved.` : response.error || 'SSH host options could not be saved.')
  }

  const checkRemoteHostInstruction = (host: ExecutionHost, instructionFile: string) => {
    if (isRemoteHostBusy(host.id)) return
    setRemoteHostBusy(host.id, true)
    void sendToCEF<{ bytes: number }>({ action: 'checkRemoteHostInstruction', payload: { id: host.id, instructionFile } }).then((response) => {
      setRemoteHostBusy(host.id, false)
      setRemoteMessage(response.ok ? `Readable on ${host.label}: ${response.data?.bytes ?? 0} bytes.` : response.error || 'Host instruction file could not be read.')
    })
  }

  const removeRemoteHost = async (host: ExecutionHost) => {
    if (isRemoteHostBusy(host.id)) return
    setRemoteHostBusy(host.id, true)
    setRemoteMessage('')
    const response = await sendToCEF({ action: 'removeRemoteHost', payload: { id: host.id } })
    setRemoteHostBusy(host.id, false)
    setRemoteMessage(response.ok ? `${host.label} was removed from UAM. Any helper files on that machine were left untouched.` : response.error || 'Remote host removal failed.')
  }

  useEffect(() => {
    dialogRef.current?.focus()
    return () => openerRef.current?.focus()
  }, [])

  useEffect(() => {
    if (!themeDraft) return
    applyDocumentTheme(themeDraft.id, [themeDraft])
    return () => applyDocumentTheme(theme, customThemes)
  }, [customThemes, theme, themeDraft])

  useEffect(() => {
    const handler = (e: KeyboardEvent) => {
      if (e.defaultPrevented) return
      const dialogs = document.querySelectorAll<HTMLElement>('[aria-modal="true"]')
      const top = dialogs.item(dialogs.length - 1)
      if (top && top !== dialogRef.current && !top.hasAttribute('data-settings-owned-overlay')) return
      if (e.key === 'Tab') {
        if (!top) return
        const controls = Array.from(top.querySelectorAll<HTMLElement>('button:not(:disabled), input:not(:disabled):not([type="hidden"]), textarea:not(:disabled), select:not(:disabled), summary, [tabindex="0"]') ?? []).filter(element => !element.closest('[hidden], .hidden, [aria-hidden="true"]') && (!element.closest('details:not([open])') || element.tagName === 'SUMMARY'))
        const first = controls[0], last = controls[controls.length - 1]
        if (e.shiftKey && (document.activeElement === first || !controls.includes(document.activeElement as HTMLElement))) { e.preventDefault(); last?.focus() }
        else if (!e.shiftKey && (document.activeElement === last || !controls.includes(document.activeElement as HTMLElement))) { e.preventDefault(); first?.focus() }
        return
      }
      if (e.key !== 'Escape' || (!top && !dialogRef.current?.contains(document.activeElement))) return
      if (editorExit) { e.preventDefault(); if (!editorSaving) setEditorExit(null); return }
      if (themeExit) { e.preventDefault(); if (!themeSaving) setThemeExit(null); return }
      if (pendingDelete) { e.preventDefault(); if (!themeSaving && !editorSaving) setPendingDelete(null); return }
      if (confirmDiscard) { e.preventDefault(); setConfirmDiscard(false); dialogRef.current?.focus(); return }
      if (isMarkdownStoreOpen) return
      if (openMemoryMenu) {
        setOpenMemoryMenu(null)
        return
      }
      if (openEditorMenu) {
        setOpenEditorMenu(null)
        return
      }
      if (openCliVersionMenu) {
        setOpenCliVersionMenu(null)
        return
      }
	  if (remotePreview) {
		if (!remoteBusy) setRemotePreview(null)
		return
	  }
      if (themeDraft) { requestThemeExit(() => setThemeDraft(null)); return }
      requestClose()
    }
    window.addEventListener('keydown', handler)
    return () => window.removeEventListener('keydown', handler)
  }, [isMarkdownStoreOpen, mcpDraftDirty, openCliVersionMenu, openEditorMenu, openMemoryMenu, remotePreview, remoteBusy, selectedSection, themeDraft, themeDirty, themeSaving, themeExit, pendingDelete, editorSaving, editorExit, editorDirty, confirmDiscard, setSettingsOpen])

  useEffect(() => {
    const handler = (event: MouseEvent) => {
      const target = event.target
      if (!(target instanceof Node)) return
      if (target instanceof Element && target.closest('[data-viewport-menu]')) return
      if (!memoryMenuRef.current?.contains(target)) setOpenMemoryMenu(null)
      if (!editorMenuRef.current?.contains(target)) setOpenEditorMenu(null)
      if (!cliVersionMenuRef.current?.contains(target)) setOpenCliVersionMenu(null)
    }
    document.addEventListener('mousedown', handler)
    return () => document.removeEventListener('mousedown', handler)
  }, [])

  useEffect(() => {
    setMarkdownStoreDraftDirectory(markdownStoreDirectory)
  }, [markdownStoreDirectory])

  useEffect(() => {
    setEditorAssociationsDraft(editorFileAssociations)
  }, [editorFileAssociations])

  useEffect(() => {
    setDefaultEditorDraft(defaultEditorPresetId)
  }, [defaultEditorPresetId])


  useEffect(() => {
    if (activeSessionId) void refreshUamAgents(activeSessionId)
  }, [activeSessionId, refreshUamAgents])

  const updateMemoryBinding = (providerId: string, binding: MemoryWorkerBinding) => {
    void setMemorySettings({
      memoryWorkerBindings: {
        ...memoryWorkerBindings,
        [providerId]: binding,
      },
    })
  }

  const defaultsForProvider = (provider: Provider): ProviderChatDefaults => {
    const saved = providerChatDefaults[provider.id]
    return {
      modelId: saved?.modelId ?? '',
      reviewerModelId: saved?.reviewerModelId ?? '',
      featurePreference: saved?.featurePreference === 'provider' ? 'provider' : 'uam',
      hiddenModelIds: saved?.hiddenModelIds ?? [],
      hiddenProviderIds: saved?.hiddenProviderIds ?? [],
      approvalMode: saved?.approvalMode ?? 'default',
      commandSafetyTier: saved?.commandSafetyTier ?? 'off',
      memoryLevel: saved?.memoryLevel ?? 'default',
      memoryEnabled: saved?.memoryEnabled ?? memoryEnabledDefault,
      smallModelMode: saved?.smallModelMode ?? false,
      reasoningEffort: saved?.reasoningEffort ?? '',
      serviceTier: saved?.serviceTier ?? '',
    }
  }

  const updateProviderDefaults = (providerId: string, defaults: ProviderChatDefaults) => {
    void setProviderChatDefaults({
      providerChatDefaults: {
        [providerId]: defaults,
      },
    }, settingsScope)
  }

  const toggleDefaultProvider = (providerId: string) => {
    setExpandedDefaultProviders((current) => ({
      ...current,
      [providerId]: !(current[providerId] ?? false),
    }))
  }

  const toggleEditorGroup = (groupId: string) => {
    setOpenEditorMenu(null)
    setExpandedEditorGroups((current) => ({
      ...current,
      [groupId]: !(current[groupId] ?? false),
    }))
  }

  const renderDefaultsMenu = (
    _menuId: string,
    value: string,
    label: string,
    options: MemoryModelOption[],
    onSelect: (value: string) => void,
    renderOptionIcon?: (option: MemoryModelOption) => ReactNode
  ) => {
    return (
      <MenuSelect
        label={label}
        value={value}
        options={options.map((option) => ({
          value: option.id,
          label: option.label,
          description: option.detail,
          disabled: option.disabled,
          icon: renderOptionIcon?.(option),
        }))}
        onChange={onSelect}
      />
    )
  }

  const saveExplorer = async (path: string) => {
    if (editorSavePending.current) return
    editorSavePending.current = true
    setEditorSaving(true); setOpenWithError('')
    try {
      if (!await setEditorSettings({ defaultEditorPresetId, editorFileAssociations, fileExplorerApplication: path })) setOpenWithError('File explorer could not be saved.')
    } catch { setOpenWithError('File explorer could not be saved.') }
    finally { editorSavePending.current = false; setEditorSaving(false) }
  }
  const saveWindowsPowerShell = async (value: WindowsPowerShellPreference) => {
    if (editorSavePending.current) return
    editorSavePending.current = true
    setEditorSaving(true); setOpenWithError('')
    try {
      if (!await setEditorSettings({ defaultEditorPresetId, editorFileAssociations, windowsPowerShell: value })) setOpenWithError('Terminal shell could not be saved.')
    } catch { setOpenWithError('Terminal shell could not be saved.') }
    finally { editorSavePending.current = false; setEditorSaving(false) }
  }
  const saveEditorSettings = async (nextAssociations = editorAssociationsDraft, nextDefaultEditor = defaultEditorDraft) => {
    if (editorSavePending.current) return false
    if (nextAssociations.some(item => !item.name.trim() || !item.extensions.length)) {
      setEditorError('Each group needs a name and at least one extension.')
      return false
    }
    editorSavePending.current = true
    setEditorSaving(true)
    setEditorAssociationsDraft(nextAssociations)
    setDefaultEditorDraft(nextDefaultEditor)
    setEditorError('')
    try {
      const saved = await setEditorSettings({defaultEditorPresetId:nextDefaultEditor,editorFileAssociations:nextAssociations})
      if (!saved) setEditorError('Editor settings could not be saved. Try again.')
      return saved
    } catch {
      setEditorError('Editor settings could not be saved. Try again.')
      return false
    } finally { editorSavePending.current = false; setEditorSaving(false) }
  }
  const commitEditorField = (id: string, field: 'name' | 'extensions') => {
    const raw = field === 'name' ? rawEditorNames[id] : rawExtensions[id]
    if (raw === undefined) return
    const value = field === 'name' ? raw.trim() : parseExtensions(raw)
    if (!value.length) { setEditorError(field === 'name' ? 'Enter a group name.' : 'Enter at least one extension.'); return }
    void saveEditorSettings(editorAssociationsDraft.map(item => item.id === id ? {...item,[field]:value} : item))
  }

  const renderEditorPresetMenu = (
    menuId: string,
    value: string,
    label: string,
    onSelect: (presetId: string) => void
  ) => (
    <div className="relative">
      <button
        ref={(element) => { if (element) popupAnchorsRef.current.set(menuId, element) }}
        type="button"
        title={label}
        aria-haspopup="listbox"
        aria-expanded={openEditorMenu === menuId}
        onClick={() => setOpenEditorMenu(openEditorMenu === menuId ? null : menuId)}
        className="uam-menu-select__trigger flex w-full items-center justify-between gap-2 text-left"
        style={{
          color: 'var(--text)',
          borderRadius: 8,
          padding: '8px 10px',
        }}
      >
        <span className="truncate">{editorPresetLabel(value)}</span>
        <ChevronDown className={openEditorMenu === menuId ? 'uam-menu-select__chevron is-open' : 'uam-menu-select__chevron'} size={14} aria-hidden />
      </button>
      {openEditorMenu === menuId && (
        <ViewportMenu
          anchorRef={{ current: popupAnchorsRef.current.get(menuId) ?? null }}
          role="listbox"
          aria-label={label}
          className="animate-fade-in"
          style={{
            width: popupAnchorsRef.current.get(menuId)?.getBoundingClientRect().width,
            border: '1px solid var(--border-bright)',
            borderRadius: 10,
            background: 'var(--surface)',
            boxShadow: 'var(--elev-3)',
            padding: 6,
          }}
        >
          {EDITOR_PRESETS.map((preset) => {
            const selected = preset.id === value
            return (
              <button
                key={preset.id}
                type="button"
                role="option"
                aria-selected={selected}
                onClick={() => {
                  onSelect(preset.id)
                  setOpenEditorMenu(null)
                }}
                className={`uam-menu-select__option w-full flex items-center gap-2 text-left px-2 py-2${selected ? ' is-selected' : ''}`}
                style={{
                  borderRadius: 6,
                  color: selected ? 'var(--text)' : 'var(--text-2)',
                }}
              >
                <span className="flex-1">{preset.label}</span>
                {selected && <Check size={13} style={{ color: 'var(--success)' }} aria-hidden />}
              </button>
            )
          })}
        </ViewportMenu>
      )}
    </div>
  )

  const workerLogText = [
    memoryActivity.lastWorkerError ? `Error:\n${memoryActivity.lastWorkerError}` : '',
    memoryActivity.lastWorkerOutput ? `Output:\n${memoryActivity.lastWorkerOutput}` : '',
  ].filter(Boolean).join('\n\n')
  const hasWorkerLog = Boolean(workerLogText || memoryActivity.lastWorkerStatus)
  const workerLogIsFailure = Boolean(memoryActivity.lastWorkerTimedOut || memoryActivity.lastWorkerError || (memoryActivity.lastWorkerStatus && memoryActivity.lastWorkerStatus !== 'Memory worker completed.'))
  const workerLogMeta = [
    memoryActivity.lastWorkerProviderId || '',
    memoryActivity.lastWorkerChatId ? `chat ${memoryActivity.lastWorkerChatId}` : '',
    memoryActivity.lastWorkerHasExitCode ? `exit ${memoryActivity.lastWorkerExitCode ?? 0}` : '',
    memoryActivity.lastWorkerTimedOut ? 'timed out' : '',
    memoryActivity.lastWorkerUpdatedAt || '',
  ].filter(Boolean).join(' | ')
  const selectedCustomTheme = customThemes.find((candidate) => candidate.id === theme)
  const themeDraftValid = Boolean(
    themeDraft?.name.trim()
    && Object.values(themeDraft.colors).every((color) => THEME_COLOR_PATTERN.test(color))
  )
  const themeSaveInFlightRef = useRef(false)
  const startNewTheme = (source?: CustomTheme) => {
    const base = source?.base ?? (theme === 'light' ? 'light' : 'dark')
    const name = source ? `${source.name} Copy` : 'Custom Theme'
    setThemeDraft({
      version: 1,
      id: customThemeId(name, customThemes),
      name,
      base,
      colors: { ...(source?.colors ?? BUILT_IN_THEME_COLORS[base]) },
    })
    setThemeStep(0)
    setThemeMessage('')
  }
  const cloneCurrentTheme = () => {
    if (selectedCustomTheme) {
      startNewTheme(selectedCustomTheme)
      return
    }
    const base = resolveDocumentTheme(theme, customThemes)
    startNewTheme({ version: 1, id: 'custom:built-in', name: `${base === 'dark' ? 'Dark' : 'Light'}`, base, colors: { ...BUILT_IN_THEME_COLORS[base] } })
  }
  const saveThemeDraft = async () => {
    if (!themeDraft || !themeDirty || !themeDraftValid || themeSaveInFlightRef.current) return false
    themeSaveInFlightRef.current = true
    setThemeSaving(true)
    try {
      const saved = await saveCustomTheme(themeDraft)
      if (!saved) {
        setThemeMessage('Theme could not be saved. Check its name and colors.')
        return false
      }
      setThemeDraft(null)
      setTheme(saved.id)
      setThemeMessage('Theme saved.')
      return true
    } catch {
      setThemeMessage('Theme could not be saved. Try again.')
      return false
    } finally {
      themeSaveInFlightRef.current = false
      setThemeSaving(false)
    }
  }
  const removeSelectedTheme = async () => {
    if (!selectedCustomTheme || themeSaveInFlightRef.current) return false
    themeSaveInFlightRef.current = true
    setThemeSaving(true)
    try {
      const deleted = await deleteCustomTheme(selectedCustomTheme.id)
      setThemeMessage(deleted ? 'Theme deleted.' : 'Theme could not be deleted.')
      if (deleted) setThemeDraft(null)
      return deleted
    } catch { setThemeMessage('Theme could not be deleted. Try again.'); return false }
    finally { themeSaveInFlightRef.current = false; setThemeSaving(false) }
  }
  const importThemeFile = async (file?: File) => {
    if (!file) return
    try {
      const imported = normalizeCustomTheme(JSON.parse(await file.text()))
      if (!imported) throw new Error('invalid')
      const saved = await saveCustomTheme(imported)
      if (!saved) throw new Error('save')
      setTheme(saved.id)
      setThemeDraft(null)
      setThemeMessage('Theme imported.')
    } catch {
      setThemeMessage('Theme import failed. Choose a valid UAM theme JSON file.')
    }
  }
  const exportSelectedTheme = () => {
    if (!selectedCustomTheme) return
    const url = URL.createObjectURL(new Blob([`${JSON.stringify(selectedCustomTheme, null, 2)}\n`], { type: 'application/json' }))
    const anchor = document.createElement('a')
    anchor.href = url
    anchor.download = `${selectedCustomTheme.id.slice('custom:'.length)}.json`
    anchor.click()
    URL.revokeObjectURL(url)
    setThemeMessage('Theme exported.')
  }

  const renderAddEditor = () => (
              <Button
                leadingIcon={<Plus size={14} />}
                aria-label="Add editor group"
                size="sm"
                disabled={editorSaving}
                onClick={() => {
                  const id = `editor-group-${Date.now()}`
                  const nextAssociations = [
                    ...editorAssociationsDraft,
                    {
                      id,
                      name: 'Files',
                      extensions: ['.txt'],
                      editorPresetId: defaultEditorDraft,
                    },
                  ]
                  setExpandedEditorGroups((current) => ({ ...current, [id]: true }))
                  saveEditorSettings(nextAssociations)
                }}
              >Add group</Button>
  )

  const renderThemeEditor = () => themeDraft ? (
<div className="grid gap-4 rounded-lg p-3" style={{ border: '1px solid var(--border)', background: 'var(--surface)' }}>
                  {themeStep === 0 && <div className="grid gap-3 sm:grid-cols-2">
                    <label className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
                      <span>Name</span>
                      <input
                        aria-label="Theme name"
                        value={themeDraft.name}
                        maxLength={64}
                        onChange={(event) => setThemeDraft({ ...themeDraft, name: event.target.value })}
                        className="uam-field w-full"
                      />
                    </label>
                    <fieldset className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
                      <legend>Base</legend>
                      <div role="radiogroup" aria-label="Theme base" className="grid grid-cols-2 gap-1 rounded-md p-1" style={{ border: '1px solid var(--border)', background: 'var(--bg)' }}>
                        {(['dark', 'light'] as const).map((base) => {
                          const selected = themeDraft.base === base
                          return (
                            <button
                              key={base}
                              type="button"
                              role="radio"
                              aria-checked={selected}
                              onClick={() => setThemeDraft({ ...themeDraft, base })}
                              className="rounded px-2 py-1 capitalize transition-colors duration-150"
                              style={{ background: selected ? 'var(--accent-dim)' : 'transparent', color: selected ? 'var(--text)' : 'var(--text-2)' }}
                            >
                              {base}
                            </button>
                          )
                        })}
                      </div>
                    </fieldset>
                  </div>}
                  {themeStep === 1 && <div className="grid gap-2 sm:grid-cols-2 lg:grid-cols-3">
                    {/* CEF native popup controls crash on macOS 26; keep theme editing in-app. */}
                    {THEME_COLOR_FIELDS.map(({ key, label }) => (
                      <label key={key} className="flex items-center justify-between gap-2 rounded-md px-2 py-1.5 text-xs" style={{ border: '1px solid var(--border)', color: 'var(--text-2)' }}>
                        <span>{label}</span>
                        <span className="flex items-center gap-2">
                          <span
                            aria-hidden="true"
                            className="h-5 w-5 rounded-full transition-transform duration-150 hover:scale-110"
                            style={{ background: themeDraft.colors[key], border: '1px solid var(--border)' }}
                          />
                          <input
                            type="text"
                            inputMode="text"
                            aria-label={`${label} color`}
                            value={themeDraft.colors[key]}
                            maxLength={7}
                            pattern="#[0-9A-Fa-f]{6}"
                            spellCheck={false}
                            aria-invalid={!THEME_COLOR_PATTERN.test(themeDraft.colors[key])}
                            aria-describedby={!THEME_COLOR_PATTERN.test(themeDraft.colors[key]) ? 'theme-color-format-error' : undefined}
                            onChange={(event) => setThemeDraft({ ...themeDraft, colors: { ...themeDraft.colors, [key]: event.target.value } })}
                            className="uam-field w-24 font-mono uppercase"
                          />
                        </span>
                      </label>
                    ))}
                  </div>}
                  {!themeDraftValid && (
                    <p id="theme-color-format-error" role="alert" className="text-xs" style={{ color: 'var(--error)' }}>
                      Every color must use #RRGGBB format.
                    </p>
                  )}
                  {themeStep === 2 && <div className="grid gap-3">
                    <div className="text-sm">{themeDraft.name} · {themeDraft.base}</div>
                    <div className="grid gap-2 sm:grid-cols-2 lg:grid-cols-3">{THEME_COLOR_FIELDS.map(({key,label}) => <div key={key} className="flex items-center gap-2 text-xs"><span aria-hidden className="w-4 h-4 border" style={{background:themeDraft.colors[key]}}/>{label}<code>{themeDraft.colors[key]}</code></div>)}</div>
                  </div>}
                  <div className="flex gap-2">
                    {themeStep > 0 && <Button size="sm" disabled={themeSaving} onClick={() => setThemeStep(themeStep - 1)}>Back</Button>}
                    {themeStep < 2 ? <Button size="sm" variant="primary" disabled={themeStep === 0 ? !themeDraft.name.trim() : !themeDraftValid} onClick={() => setThemeStep(themeStep + 1)}>Next</Button> : <SaveButton saving={themeSaving} disabled={!themeDirty || !themeDraftValid || themeSaving} onClick={() => void saveThemeDraft()}>Save theme</SaveButton>}
                    <Button size="sm" disabled={themeSaving} onClick={() => requestThemeExit(() => setThemeDraft(null))}>Cancel</Button>
                  </div>
                </div>
  ) : null

  const checkComputerUsePermissions = async () => {
    setComputerUseBusy(true)
    setComputerUseMessage(`Checking ${computerUseOsLabel} permissions…`)
    const response = await sendToCEF<{ screenRecording?: { available?: boolean; error?: string }; accessibility?: { available?: boolean; error?: string } }>({ action: 'checkComputerUsePermissions' })
    setComputerUseBusy(false)
    if (!response.ok) { setComputerUseMessage(response.error || `Could not check ${computerUseOsLabel} permissions.`); return }
    setComputerUseStatus({
      screenRecording: { available: Boolean(response.data?.screenRecording?.available), error: response.data?.screenRecording?.error || '' },
      accessibility: { available: Boolean(response.data?.accessibility?.available), error: response.data?.accessibility?.error || '' },
    })
    setComputerUseMessage('Permission check complete.')
  }
  const requestComputerUsePermission = async (permission: 'screenRecording' | 'accessibility') => {
    setComputerUseBusy(true)
    const response = await sendToCEF<{ granted?: boolean; openSettingsRequired?: boolean }>({ action: 'requestComputerUsePermission', payload: { permission } })
    if (!response.ok) {
      setComputerUseBusy(false)
      setComputerUseMessage(response.error || 'Could not request this permission.')
      return
    }
    if (response.data?.granted) {
      setComputerUseBusy(false)
      setComputerUseMessage(`${permission === 'screenRecording' ? 'Screen Recording' : 'Accessibility'} permission is already granted.`)
      return
    }
    const settingsResponse = await sendToCEF<{ opened?: boolean; error?: string }>({ action: 'openComputerUseSystemSettings', payload: { permission } })
    setComputerUseBusy(false)
    setComputerUseMessage(settingsResponse.ok && settingsResponse.data?.opened !== false
      ? `Permission is not granted. Opened ${computerUseOsLabel} settings; enable it, then re-check.`
      : settingsResponse.error || settingsResponse.data?.error || `Permission is not granted. Could not open ${computerUseOsLabel} settings.`)
  }
  const openComputerUseSystemSettings = async (permission: 'screenRecording' | 'accessibility') => {
    const response = await sendToCEF({ action: 'openComputerUseSystemSettings', payload: { permission } })
    if (!response.ok) setComputerUseMessage(response.error || `Could not open ${computerUseOsLabel} settings.`)
  }
  const loadComputerUseApplications = async () => {
    const response = await sendToCEF<{ applications?: Array<{ identityKind?: string; identity?: string; name?: string }> }>({ action: 'listComputerUseApplications' })
    if (!response.ok) { setComputerUseMessage(response.error || 'Could not list applications.'); return }
    setComputerUseApps((response.data?.applications ?? []).flatMap((app) => app.identity && app.name && (app.identityKind === 'bundleId' || app.identityKind === 'executablePath') ? [{ identityKind: app.identityKind, identity: app.identity, name: app.name }] : []))
  }
  const adoptCompanionStatus = (next: PhoneAccessStatus) => {
    setCompanionSettings(previous => (previous?.generation ?? -1) > (next.generation ?? -1) ? previous : next)
  }
  const loadCompanionSettings = async (manual = false) => {
    if (manual) {
      if (companionInFlight.current) return
      companionInFlight.current = true
      setCompanionBusy(true)
      setCompanionBusyAction('reloadCompanion')
    }
    try {
      const response = await sendToCEF<PhoneAccessStatus>({ action: 'getCompanionSettings' })
      if (!response.ok || !response.data) { setCompanionLoadError(response.error || 'Could not load phone access settings.'); return }
      adoptCompanionStatus(response.data)
      setCompanionLoadError('')
    } catch { setCompanionLoadError('Could not load phone access settings. Try again.') }
    finally {
      if (manual) { companionInFlight.current = false; setCompanionBusy(false); setCompanionBusyAction('') }
    }
  }
  const changeCompanion = async (action: 'setCompanionEnabled' | 'restartCompanion' | 'repairCompanionLanAddress', payload: Record<string, unknown> = {}) => {
    if (companionInFlight.current) return
    companionInFlight.current = true
    setCompanionBusy(true)
    setCompanionBusyAction(action === 'repairCompanionLanAddress' ? `${action}:${payload.address}` : action)
    setCompanionMessage('')
    setCompanionNotice('')
    try {
      const response = await sendToCEF<PhoneAccessStatus>({ action, payload })
      if (!response.ok || !response.data) { setCompanionMessage(response.error || 'Could not update phone access.'); return }
      adoptCompanionStatus(response.data)
    } catch { setCompanionMessage('Could not update phone access. Try again.') }
    finally { companionInFlight.current = false; setCompanionBusy(false); setCompanionBusyAction('') }
  }
  const setCompanionEnabled = (enabled: boolean) => changeCompanion('setCompanionEnabled', { enabled })
  const restartApplication = async () => {
    if (companionInFlight.current) return
    companionInFlight.current = true
    setCompanionBusy(true)
    setCompanionBusyAction('restartApplication')
    setCompanionMessage('')
    setCompanionNotice('')
    try {
      const response = await sendToCEF({ action: 'restartApplication' })
      if (!response.ok) setCompanionMessage(response.error || 'Could not restart Universal Agent Manager. Close and open it again.')
      else setCompanionNotice('Restarting Universal Agent Manager…')
    } catch { setCompanionMessage('Could not restart Universal Agent Manager. Close and open it again.') }
    finally { companionInFlight.current = false; setCompanionBusy(false); setCompanionBusyAction('') }
  }
  const copyCompanion = async (kind: 'token' | 'url') => {
    if (companionInFlight.current) return
    companionInFlight.current = true
    setCompanionBusy(true)
    setCompanionBusyAction(`copy:${kind}`)
    setCompanionMessage('')
    setCompanionNotice('')
    let ok = false
    try {
      let text = companionSettings?.effectiveUrl ?? ''
      if (kind === 'token') {
        const response = await sendToCEF<{ token?: string }>({ action: 'getCompanionToken' })
        if (!response.ok || !response.data?.token) {
          setCompanionMessage(response.error || 'Phone access token is unavailable.')
          return
        }
        text = response.data.token
      }
      // CEF blocks navigator.clipboard here; route through the native bridge helper.
      ok = await copyTextToClipboard(text, document)
      if (!ok) setCompanionMessage(`Could not copy the phone access ${kind}.`)
    } catch { setCompanionMessage(`Could not copy the phone access ${kind}. Try again.`) }
    finally {
      companionInFlight.current = false
      setCompanionBusy(false)
      setCompanionBusyAction('')
      setCompanionCopy({ kind, ok })
      window.clearTimeout(companionCopyTimer.current)
      companionCopyTimer.current = window.setTimeout(() => setCompanionCopy(null), 1800)
    }
  }
  useEffect(() => {
    if (!MOBILE_COMPANION_ENABLED || selectedSection !== 'phone') return
    setCompanionMessage('')
    setCompanionNotice('')
    setCompanionCopy(null)
    let disposed = false
    let timer: number | undefined
    const poll = async () => {
      if (!disposed && !companionInFlight.current) await loadCompanionSettings()
      if (!disposed) timer = window.setTimeout(() => void poll(), 1500)
    }
    void poll()
    return () => { disposed = true; window.clearTimeout(timer) }
  }, [selectedSection])
  const saveComputerUseSettings = async (allowlistEnabled: boolean, allowedApplications: ComputerUseAllowedApplication[]) => {
    if (computerUseSaveInFlight.current) return
    const previous = { computerUseAllowlistEnabled, computerUseAllowedApplications }
    computerUseSaveInFlight.current = true
    setComputerUseSaveBusy(true)
    useAppStore.setState({ computerUseAllowlistEnabled: allowlistEnabled, computerUseAllowedApplications: allowedApplications })
    try {
      const response = await sendToCEF({ action: 'setComputerUseSettings', payload: { allowlistEnabled, allowedApplications } })
      if (!response.ok) {
        useAppStore.setState(previous)
        setComputerUseMessage(response.error || 'Could not save Computer Use settings.')
      }
    } finally {
      computerUseSaveInFlight.current = false
      setComputerUseSaveBusy(false)
    }
  }
  const copyIcon = (kind: 'token' | 'url') => companionCopy?.kind === kind
    ? companionCopy.ok ? <Check key="ok" size={14} aria-hidden className="uam-copy-pop" style={{ color: 'var(--success)' }} /> : <X key="fail" size={14} aria-hidden className="uam-copy-pop" style={{ color: 'var(--error)' }} />
    : <Copy key="idle" size={14} aria-hidden />
  const copyLabel = (kind: 'token' | 'url', idle: string) => companionCopy?.kind === kind ? companionCopy.ok ? 'Copied' : 'Copy failed' : idle
  const renderPhoneAccess = () => {
    const ready = !companionLoadError && companionSettings?.state === 'ready' && companionSettings.enabled && Boolean(companionSettings.effectiveUrl)
    const address = companionSettings?.desiredUrl || companionSettings?.url || ''
    const failed = companionSettings?.state === 'failed'
    const currentAddresses = [...new Set(companionSettings?.localAddresses ?? [])]
    let configuredHost = ''
    try { configuredHost = new URL(address).hostname } catch { /* No configured address. */ }
    const repairAddresses = configuredHost && configuredHost !== '127.0.0.1' && /^\d+\.\d+\.\d+\.\d+$/.test(configuredHost) && !currentAddresses.includes(configuredHost) ? currentAddresses : []
    const phoneFeedback = (companionLoadError || companionMessage || companionNotice) ? <>
      {companionLoadError && <p role="alert" className="text-xs" style={{ color: 'var(--error)' }}>{companionLoadError}</p>}
      {companionMessage && <p role="alert" className="text-xs" style={{ color: 'var(--error)' }}>{companionMessage}</p>}
      {companionNotice && <p role="status" className="text-xs" style={{ color: 'var(--text-2)' }}>{companionNotice}</p>}
    </> : null
    return <div className="grid gap-0">
      <SettingsSection title="Phone access">
        <SettingRow
          title="Enable phone access"
          description={companionSettings?.configured ? 'Connect your phone to this computer. Changes apply automatically.' : 'Not configured on this computer.'}
          control={<Switch hideLabel label="Enable phone access" checked={Boolean(companionSettings?.enabled)}
            disabled={!companionSettings?.configured || companionBusy} onChange={(event) => void setCompanionEnabled(event.target.checked)} />}
        />
        <SettingRow
          title={companionLoadError ? 'Phone access needs attention' : !companionSettings ? 'Checking phone access…' : ready ? 'Ready to connect' : companionSettings.state === 'starting' ? 'Updating phone access…' : failed ? 'Phone access needs attention' : 'Phone access is off'}
          description={companionLoadError ? 'Retry to check phone access on this computer.' : repairAddresses.length > 0 ? 'Choose a current network address below to repair the connection.' : companionSettings?.reason?.replace(/\bLAN\b/g, 'network') || (ready ? 'Scan the pairing code in the Universal Agent Manager iPhone app.' : 'Enable phone access to connect your iPhone.')}
          status={<span role="status" aria-live="polite" style={{ color: failed ? 'var(--error)' : 'var(--text-2)' }}>{companionSettings?.restartRequired ? 'Restart Universal Agent Manager to apply this change.' : companionBusyAction === 'setCompanionEnabled' ? 'Updating phone access…' : undefined}</span>}
          control={<div className="flex flex-wrap gap-2">
            {(failed || companionLoadError) && <Button size="sm" variant="secondary" disabled={companionBusy} onClick={() => { if (failed) void changeCompanion('restartCompanion'); else void loadCompanionSettings(true) }}>{['restartCompanion', 'reloadCompanion'].includes(companionBusyAction) ? 'Retrying…' : 'Retry phone access'}</Button>}
            {(failed || companionSettings?.restartRequired) && <Button size="sm" variant="secondary" disabled={companionBusy} onClick={() => void restartApplication()}>{companionBusyAction === 'restartApplication' ? 'Restarting Universal Agent Manager…' : 'Restart Universal Agent Manager'}</Button>}
          </div>}
        >{phoneFeedback}</SettingRow>
        {repairAddresses.length > 0 && <SettingRow title="Network address changed" description={`${configuredHost} is no longer on this computer.`}
          control={<div className="flex flex-wrap gap-2">{repairAddresses.map(localAddress => <Button key={localAddress} size="sm" variant="secondary" disabled={companionBusy} onClick={() => void changeCompanion('repairCompanionLanAddress', { address: localAddress })}>{companionBusyAction === `repairCompanionLanAddress:${localAddress}` ? 'Repairing…' : `Use ${localAddress}`}</Button>)}</div>} />}
        {address && <SettingRow title={ready ? 'Phone address' : 'Configured phone address'} description={!ready ? 'Available when phone access is ready.' : undefined}
          control={<Button size="sm" variant="ghost" leadingIcon={copyIcon('url')} aria-label="Copy phone URL" disabled={!ready || companionBusy} onClick={() => void copyCompanion('url')}>{copyLabel('url', 'Copy URL')}</Button>}>
          <code className="block truncate font-mono text-xs" title={address} style={{ color: 'var(--text-2)' }}>{ready ? companionSettings?.effectiveUrl : address}</code>
        </SettingRow>}
        <SettingRow title="Access token" description="For manual entry in the iPhone app. Pairing by QR code also configures secure certificate trust."
          control={<Button size="sm" variant="secondary" leadingIcon={copyIcon('token')} aria-label="Copy token" disabled={!companionSettings?.configured || companionBusy} onClick={() => void copyCompanion('token')}>{copyLabel('token', 'Copy token')}</Button>} />
        {address && <PhonePairingPanel url={companionSettings?.effectiveUrl || address} enabled={Boolean(ready)} />}
      </SettingsSection>
    </div>
  }

  const renderComputerUse = () => {
    const activeSession = sessions.find((session) => session.id === activeSessionId)
    const activeComputerUseBackend: ComputerUseBackend = activeSession?.computerUseBackend ?? 'auto'
    const activeComputerUseProviderAvailable = activeSession?.computerUseProviderAvailable ?? false
    const activeProvider = providers.find((provider) => provider.id === activeSession?.providerId)
    const activeUamControlSupported = Boolean(
      activeSession
      && activeProvider?.supportsStructured !== false
      && UAM_CONTROL_PROTOCOLS.includes(
        activeProvider?.structuredProtocol || providerMetadataForId(activeProvider?.id || '').structuredProtocol,
      ),
    )
    const statusRows = [
      ['Screen Recording', 'screenRecording', computerUseStatus?.screenRecording],
      ['Accessibility', 'accessibility', computerUseStatus?.accessibility],
    ] as const
    return <div className="grid gap-0">
      <SettingsSection title="Control method">
        <ComputerUseSettingRow />
        <SettingRow
          title="Computer Use backend"
          description={activeSession
            ? activeSession.computerUseEnabled
              ? 'Turn Computer Use off in this chat first.'
              : 'Applies to the active local chat.'
            : 'Open a local chat to choose a backend.'}
          control={
            <div id="computer-use-backend-settings" style={{ width: 220 }}>
              <MenuSelect
                label="Computer Use backend"
                value={activeComputerUseBackend}
                disabled={!activeSession || activeSession.computerUseEnabled || computerUseBusy}
                options={[
                  { value: 'auto', label: 'Automatic (recommended)' },
                  { value: 'provider', label: 'Provider built-in', disabled: !activeComputerUseProviderAvailable },
                  { value: 'uam', label: 'UAM controlled' },
                ]}
                onChange={(value) => {
                  if (!activeSession) return
                  setComputerUseBusy(true)
                  setComputerUseMessage('')
                  void setSessionComputerUseBackend(activeSession.id, value as ComputerUseBackend).then((result) => {
                    if (!result.ok) setComputerUseMessage(result.error || 'Could not change the Computer Use backend.')
                  }).finally(() => setComputerUseBusy(false))
                }}
              />
            </div>
          }
        />
        <SettingRow
          title="Allow UAM Control requests"
          description={activeUamControlSupported
            ? 'Agent requests appear in the chat for approval.'
            : 'Supported local structured chats only.'}
          control={
            <Switch
              hideLabel
              className="uam-switch--accent"
              label="Allow UAM Control requests"
              checked={Boolean(activeSession?.uamControlEnabled)}
              disabled={!activeUamControlSupported || computerUseBusy}
              onChange={(event) => {
                if (!activeSession) return
                setComputerUseBusy(true)
                setComputerUseMessage('')
                void setSessionUamControlEnabled(activeSession.id, event.target.checked)
                  .then((result) => {
                    if (!result) setComputerUseMessage('Could not change UAM Control. Stop the active session first, then try again.')
                  })
                  .finally(() => setComputerUseBusy(false))
              }}
            />
          }
        />
      </SettingsSection>
      <SettingsSection
        title="Readiness"
        actions={<Button size="sm" variant="secondary" disabled={computerUseBusy} onClick={() => void checkComputerUsePermissions()}>Check permissions</Button>}
      >
        {statusRows.map(([label, permission, state]) => <SettingRow
          key={permission}
          title={label}
          status={<span className="inline-flex items-center gap-1.5" style={{ color: state ? (state.available ? 'var(--success)' : 'var(--error)') : 'var(--text-3)' }}>
            <StatusDot tone={state ? (state.available ? 'success' : 'error') : 'neutral'} />
            {state ? (state.available ? 'Available' : state.error || 'Not available') : 'Not checked'}
          </span>}
          control={<div className="flex flex-wrap justify-end gap-1">
            <Button size="sm" variant="ghost" disabled={computerUseBusy} onClick={() => void openComputerUseSystemSettings(permission)}>Open settings</Button>
            <Button size="sm" variant="secondary" disabled={computerUseBusy} onClick={() => void requestComputerUsePermission(permission)}>{state?.available ? 'Re-request' : 'Request'} {label}</Button>
          </div>}
        />)}
        {computerUseMessage && <p role="status" className="px-0 py-2 text-xs" style={{ color: 'var(--text-2)' }}>{computerUseMessage}</p>}
      </SettingsSection>
      <SettingsSection
        title="Allowed targets"
        actions={<Button size="sm" variant="secondary" onClick={() => void loadComputerUseApplications()}>Refresh applications</Button>}
      >
        <SettingRow
          title="Only listed apps"
          description={computerUseAllowlistEnabled ? 'Only listed apps can be targeted.' : 'Each chat asks before targeting an app.'}
          control={<Switch hideLabel label="Restrict Computer Use to allowed applications" checked={computerUseAllowlistEnabled} disabled={computerUseSaveBusy} onChange={(event) => void saveComputerUseSettings(event.target.checked, computerUseAllowedApplications)} />}
        />
        {computerUseApps.length === 0 && <SettingRow title="No applications loaded" description="Refresh to list apps UAM can target." />}
        {computerUseApps.map((app) => {
          const allowed = computerUseAllowedApplications.some((item) => item.identityKind === app.identityKind && item.identity === app.identity)
          return <SettingRow
            key={`${app.identityKind}:${app.identity}`}
            title={app.name}
            description={<span className="font-mono">{app.identity}</span>}
            control={<Switch hideLabel label={app.name} checked={allowed} disabled={computerUseSaveBusy} onChange={() => void saveComputerUseSettings(computerUseAllowlistEnabled, allowed ? computerUseAllowedApplications.filter((item) => item.identity !== app.identity || item.identityKind !== app.identityKind) : [...computerUseAllowedApplications, { identityKind: app.identityKind, identity: app.identity }])} />}
          />
        })}
      </SettingsSection>
    </div>
  }

  const selectedSettingsSection = selectedSection
  // One computer choice for Settings: the scope bar. Provider pages use it for per-computer actions (CLI, MCP toggles).
  // Saves of defaults and MCP lists are sent per host only when the backend supports scoped settings.
  const settingsScopeHostId = useAppStore((s) => s.settingsScopeHostId)
  const setSettingsScopeHostId = useAppStore((s) => s.setSettingsScopeHostId)
  const hostScopedSettings = useAppStore((s) => s.hostScopedSettings === true)
  const hasSshHosts = SSH_ENABLED && executionHosts.some((host) => isSshHost(host))
  const storedScopeHostId = executionHosts.some((host) => isSshHost(host) && host.id === settingsScopeHostId) ? settingsScopeHostId : SETTINGS_ALL_ENVIRONMENTS
  const onProviderPage = selectedSection === 'provider-summary' || selectedSection.startsWith('provider:')
  const scopeBarVisible = hasSshHosts && (onProviderPage || selectedSection === 'defaults' || selectedSection === 'mcp-servers')
  const scopeValue = onProviderPage || hostScopedSettings ? storedScopeHostId : SETTINGS_ALL_ENVIRONMENTS
  const settingsScope = hostScopedSettings ? storedScopeHostId : SETTINGS_ALL_ENVIRONMENTS
  const effectiveProviderSettingsHostId = scopeValue === SETTINGS_ALL_ENVIRONMENTS ? 'local' : scopeValue
  const scopeBar = scopeBarVisible ? <SettingsScopeBar hosts={executionHosts} value={scopeValue} selectorEnabled={onProviderPage || hostScopedSettings} supported={hostScopedSettings} onChange={(hostId) => { if (!mcpSaving) requestSectionExit(() => setSettingsScopeHostId(hostId)) }} /> : null
  // A pending CLI install or open version menu belongs to the computer it was started for.
  useEffect(() => {
    setPendingCliInstall(null)
    setOpenCliVersionMenu(null)
    setMemoryPolicyError({})
    setMemoryPolicySaving({})
  }, [settingsScopeHostId])
  const renderSectionContent = (sectionOverride?: SettingsSectionId, scopedProviderId?: string): ReactNode => {
    const selectedSection = sectionOverride ?? selectedSettingsSection
    const availability = (ids?: string[]) => <ProviderAvailabilityControls key={`${effectiveProviderSettingsHostId}:${ids?.join(',') || 'all'}`} ref={providerAvailabilityRef} providers={providers.filter((provider) => !ids || ids.includes(provider.id))} hostId={effectiveProviderSettingsHostId} hostLabel={effectiveProviderSettingsHostId === 'local' ? 'this computer' : executionHosts.find((host) => host.id === effectiveProviderSettingsHostId)?.label || effectiveProviderSettingsHostId} />
    if (selectedSection === 'provider-summary') return <div className="grid"><SettingsSection title="Provider availability">{availability()}</SettingsSection><ProviderConnections selectedHostId={effectiveProviderSettingsHostId} onProviderSelect={id => changeSection(`provider:${id}`)} onMcpSelect={() => changeSection('mcp-servers')} onPreferencesSelect={() => changeSection('central-configuration')} /></div>
    if (selectedSection.startsWith('provider:')) {
      const providerId = selectedSection.slice('provider:'.length)
      return <div className="grid"><SettingsSection title="CLI and availability">{availability([providerId])}{renderSectionContent('cli-version', providerId)}</SettingsSection>{renderSectionContent('defaults', providerId)}<ProviderConnections providerId={providerId} selectedHostId={effectiveProviderSettingsHostId} onMcpSelect={() => changeSection('mcp-servers')} onPreferencesSelect={() => changeSection('central-configuration')} /></div>
    }
    if (selectedSection === 'permissions') return <PermissionsSettings />
    if (COMPUTER_USE_ENABLED && selectedSection === 'computer-use') return renderComputerUse()
    if (selectedSection === 'appearance') {
      const themeChoices: Array<{ value: StoredTheme; label: string; swatch: ThemeSwatch }> = [
        ...BUILT_IN_THEMES.map(({ id, label }) => ({ value: id, label, swatch: BUILT_IN_THEME_SWATCHES[id] })),
        ...customThemes.map((customTheme) => ({
          value: customTheme.id,
          label: customTheme.name,
          swatch: { background: customTheme.colors.background, surface: customTheme.colors.surface, accent: customTheme.colors.accent, text: customTheme.colors.text },
        })),
      ]
      return (
        <div className="grid gap-0">
          <SettingsSection title="Theme">
            <SettingRow
              title="Colour theme"
            >
              <div role="radiogroup" aria-label="Colour theme" className="uam-theme-grid" onKeyDown={(event) => {
                // Arrow keys move between theme tiles; Enter or Space applies the focused one.
                const step = event.key === 'ArrowRight' || event.key === 'ArrowDown' ? 1 : event.key === 'ArrowLeft' || event.key === 'ArrowUp' ? -1 : 0
                if (!step) return
                const tiles = Array.from(event.currentTarget.querySelectorAll<HTMLElement>('[role="radio"]'))
                const index = tiles.indexOf(document.activeElement as HTMLElement)
                if (index < 0) return
                event.preventDefault()
                tiles[(index + step + tiles.length) % tiles.length]?.focus()
              }}>
                {themeChoices.map((choice) => {
                  const selected = choice.value === theme
                  return (
                    <button
                      key={choice.value}
                      type="button"
                      role="radio"
                      aria-checked={selected}
                      aria-label={choice.label}
                      title={choice.label}
                      data-selected={selected || undefined}
                      onClick={() => {
                        setThemeDraft(null)
                        setTheme(choice.value)
                        setThemeMessage('')
                      }}
                      className="uam-theme-tile"
                    >
                      <span aria-hidden className="uam-theme-tile__preview" style={{ background: choice.swatch.background }}>
                        <span className="uam-theme-tile__surface" style={{ background: choice.swatch.surface }}>
                          <span className="uam-theme-tile__line" style={{ background: choice.swatch.text }} />
                          <span className="uam-theme-tile__dot" style={{ background: choice.swatch.accent }} />
                        </span>
                      </span>
                      <span className="uam-theme-tile__label">{choice.label}</span>
                    </button>
                  )
                })}
              </div>
            </SettingRow>
            <SettingRow
              title="Manage themes"
              control={
                <div className="flex flex-wrap justify-end gap-1">
                  <IconButton icon={<Plus size={15}/>} label="Add theme" onClick={() => startNewTheme()}/>
                  <IconButton icon={<Copy size={15}/>} label="Copy theme" onClick={cloneCurrentTheme}/>
                  <IconButton icon={<Pencil size={15}/>} label={selectedCustomTheme ? 'Edit theme' : 'Clone built-in theme to edit'} onClick={() => { if (selectedCustomTheme) { setThemeDraft({...selectedCustomTheme,colors:{...selectedCustomTheme.colors}}); setThemeStep(0); setThemeMessage('') } else cloneCurrentTheme() }}/>
                  <IconButton icon={<Trash2 size={15}/>} label="Delete theme" variant="danger" disabled={!selectedCustomTheme} onClick={() => selectedCustomTheme && setPendingDelete({kind:'theme',id:selectedCustomTheme.id,name:selectedCustomTheme.name})}/>
                  <IconButton icon={<Upload size={15}/>} label="Import theme" onClick={() => themeImportRef.current?.click()}/>
                  <IconButton icon={<Download size={15}/>} label="Export theme" disabled={!selectedCustomTheme} onClick={exportSelectedTheme}/>
                  <input
                    ref={themeImportRef}
                    type="file"
                    accept="application/json,.json"
                    className="hidden"
                    aria-label="Import theme JSON"
                    onChange={(event) => {
                      const file = event.currentTarget.files?.[0]
                      event.currentTarget.value = ''
                      void importThemeFile(file)
                    }}
                  />
                </div>
              }
            >
              {themeMessage && <div role="status" className="text-xs" style={{ color: themeMessage.includes('failed') || themeMessage.includes('could not') ? 'var(--error)' : 'var(--text-2)' }}>{themeMessage}</div>}
            </SettingRow>
          </SettingsSection>
          <SettingsSection title="Chats">
            <SettingRow
              title="Provider icons"
              control={
                <Switch
                  hideLabel
                  label="Show provider icons in sidebar"
                  checked={showProviderIconsInSidebar}
                  onChange={(event) => void setSidebarSettings({ showProviderIconsInSidebar: event.target.checked, showWorktreePathInSidebar })}
                />
              }
            />
            <SettingRow
              title="Worktree path"
              description="Show the worktree folder under each chat name."
              control={
                <Switch
                  hideLabel
                  label="Show worktree path in sidebar"
                  checked={showWorktreePathInSidebar}
                  onChange={(event) => void setSidebarSettings({ showProviderIconsInSidebar, showWorktreePathInSidebar: event.target.checked })}
                />
              }
            />
            <SettingRow
              title="Compact working"
              description="Condense tool activity while an agent works."
              control={<Switch hideLabel label="Compact working" checked={workingDisplayMode === 'compact'} onChange={(event) => setWorkingDisplayMode(event.target.checked ? 'compact' : 'verbose')} />}
            />
            <SettingRow
              title="Expand work traces"
              description="Open each turn's tool and reasoning trace."
              control={<Switch hideLabel label="Expand work traces" checked={expandWorkTraces} onChange={(event) => setExpandWorkTraces(event.target.checked)} />}
            />
            <SettingRow
              title="Collapsible work sections"
              control={<Switch hideLabel label="Allow work sections to collapse" checked={collapsibleWorkSections} onChange={(event) => setCollapsibleWorkSections(event.target.checked)} />}
            />
          </SettingsSection>
          <TypographySettings />
          <AppearanceDisplaySettings />
        </div>
      )
    }

    if (selectedSection === 'vcs') return <VcsSettingsSection ref={vcsRef} />
    if (selectedSection === 'storage-cleanup') return <div className="grid gap-6"><DataFolderSettingsSection ref={dataFolderRef} /><StorageCleanupSettingsSection ref={cleanupRef} /></div>

    if (selectedSection === 'defaults') {
      const reviewerProvider = providers.find((provider) => provider.id === permissionReviewerProviderId)
      const reviewerSession = reviewerProvider ? latestLocalProviderSession(sessions, reviewerProvider.id) : undefined
      const reviewerWorkspace = reviewerSession?.workspaceDirectory || localWorkspace
      const reviewerAcp = reviewerProvider
        ? (reviewerSession ? acpBindings[reviewerSession.id] : undefined)
          ?? providerModelCatalogs.find((catalog) => (catalog.executionHostId || 'local') === 'local' && catalog.providerId === reviewerProvider.id && workspaceKey(catalog.workspaceDirectory) === workspaceKey(reviewerWorkspace))
        : undefined
      const reviewerModels = reviewerProvider
        ? memoryModelOptions(reviewerProvider, reviewerProvider.id, permissionReviewerModelId, reviewerAcp?.availableModels)
        : []
      return (
        <div className="space-y-4">
          {!scopedProviderId && <SettingsSection title="Chats">
            <SettingRow title="Default provider" description="Preselected in the New chat dialog." control={renderDefaultsMenu(
                  'default-provider',
                  defaultNewChatProviderId || providers[0]?.id || DEFAULT_PROVIDER_ID,
                  'Default provider',
                  providers.map((provider) => ({
                    id: provider.id,
                    label: providerDisplayName(provider, provider.id),
                    detail: provider.name ?? provider.id,
                  })),
                  (providerId) => void setProviderChatDefaults({ defaultNewChatProviderId: providerId }, settingsScope),
                  (option) => <ProviderLogo providerId={option.id} />
                )} />
            {settingsScope === SETTINGS_ALL_ENVIRONMENTS && <>
              <SettingRow title="Reviewer provider" description="AI approves routine requests; unsure ones come to you." control={renderDefaultsMenu(
                    'permission-reviewer-provider',
                    permissionReviewerProviderId,
                    'AI permission reviewer provider',
                    [
                      { id: '', label: 'Not configured', detail: 'AI Review always falls back to you.' },
                      ...providers.filter((provider) => providerSupportsTextWorkers(provider.id)).map((provider) => ({ id: provider.id, label: providerDisplayName(provider, provider.id), detail: provider.name ?? provider.id })),
                    ],
                    (providerId) => void setMemorySettings({ permissionReviewerProviderId: providerId, permissionReviewerModelId: '' }),
                    (option) => option.id ? <ProviderLogo providerId={option.id} /> : null
                  )} />
              <SettingRow title="Reviewer model" control={renderDefaultsMenu(
                    'permission-reviewer-model',
                    permissionReviewerModelId,
                    'AI permission reviewer model',
                    reviewerModels.length > 0 ? reviewerModels : [{ id: '', label: 'Provider default', detail: reviewerProvider ? 'Use the provider default model.' : 'Choose a reviewer provider first.' }],
                    (modelId) => void setMemorySettings({ permissionReviewerModelId: modelId })
                  )} />
            </>}
          </SettingsSection>}
          {scopedProviderId && <SettingsSection title="Chat defaults">
            <div className="grid gap-4">
              <div className="space-y-3">
                {providers.filter(provider => provider.id === scopedProviderId).map((provider) => {
                  const defaults = defaultsForProvider(provider)
                  const caps = providerCapabilities(provider.id, provider)
                  const providerName = providerDisplayName(provider, provider.id)
                  const providerSession = latestLocalProviderSession(sessions, provider.id, effectiveProviderSettingsHostId)
				  const providerWorkspace = providerSession?.workspaceDirectory || folders.find(folder => (folder.executionHostId || 'local') === effectiveProviderSettingsHostId)?.directory || ''
				  const providerAcp = modelCatalogForDefaults((providerSession ? acpBindings[providerSession.id] : undefined)
				    ?? providerModelCatalogs.find((catalog) => (catalog.executionHostId || 'local') === effectiveProviderSettingsHostId && catalog.providerId === provider.id && workspaceKey(catalog.workspaceDirectory) === workspaceKey(providerWorkspace)))
                  const hasDiscoveredModels = Boolean(providerAcp?.availableModels?.some((model) => model.id.trim()) || providerAcp?.configOptions?.find((option) => option.category === 'model' || option.id === 'model')?.options.some((choice) => choice.value.trim()))
                  const modelsLoading = providerAcp?.modelsLoading ?? false
                  const modelRefreshError = providerAcp?.modelRefreshError ?? ''
                  const modelOptions = buildModelOptions(providerAcp, defaults.modelId, provider, provider.id, true)
                  const reviewerModelOptions = buildModelOptions(providerAcp, defaults.reviewerModelId || defaults.modelId, provider, provider.id, true)
                  const liveReasoningOptions = buildProviderReasoningOptions(provider.id, providerAcp, defaults.modelId, defaults.reasoningEffort)
                  const defaultReasoningOption = caps.reasoningOptions.find((option) => !option.id)
                  const reasoningOptions = defaultReasoningOption && !liveReasoningOptions.some((option) => !option.id)
                    ? [defaultReasoningOption, ...liveReasoningOptions]
                    : liveReasoningOptions
                  const speedOptions = buildCodexSpeedOptions(providerAcp, defaults.modelId, defaults.serviceTier, provider.id === CODEX_CLI_PROVIDER_ID)
                  const visibilityGroups = provider.id === OPENCODE_CLI_PROVIDER_ID
                    ? [...new Set(modelOptions.filter((option) => option.id).map((option) => option.id.split('/')[0]))]
                    : []
                  const toggleVisibility = (key: 'hiddenModelIds' | 'hiddenProviderIds', id: string) => {
                    const hidden = defaults[key] ?? []
                    updateProviderDefaults(provider.id, { ...defaults, [key]: hidden.includes(id) ? hidden.filter((value) => value !== id) : [...hidden, id] })
                  }
                  const expanded = expandedDefaultProviders[provider.id] ?? false
                  const modeOptions = [
                    { id: 'default', label: 'Default', detail: 'Use the provider default mode' },
                    { id: 'plan', label: 'Plan', detail: 'Ask the provider to plan first' },
                  ]
                  return (
                    <ProviderDisclosureCard
                      key={provider.id}
                      alwaysOpen={Boolean(scopedProviderId)}
                      panelId={`${provider.id}-defaults-panel`}
                      providerId={provider.id}
                      title={scopedProviderId ? 'Model catalogue' : providerName}
                      expanded={expanded}
                      toggleLabel={`${expanded ? 'Hide' : 'Show'} ${providerName} chat defaults`}
                      onToggle={() => toggleDefaultProvider(provider.id)}
                      actions={provider.supportsStructured === false ? undefined : <IconButton icon={<RefreshCw size={14}/>} label={`Refresh ${providerName} models`} disabled={!providerWorkspace || modelsLoading} onClick={() => void (effectiveProviderSettingsHostId === 'local' ? discoverProviderModels(providerSession?.id ?? '', provider.id, providerWorkspace) : discoverProviderModels(providerSession?.id ?? '', provider.id, providerWorkspace, effectiveProviderSettingsHostId))} />}
                    >
                      <div className="grid gap-3 text-xs" style={{ color: 'var(--text-2)' }}>
                        {provider.supportsStructured === false && <p>Terminal only. Models are managed in the CLI.</p>}
                        {!modelOptions.some(option => option.id) && provider.supportsStructured !== false && <p>Visibility appears once models are discovered.</p>}
                        {modelOptions.some((option) => option.id) && (
                          <details className="rounded-lg" style={{ border: '1px solid var(--border)' }}>
                            <summary className="flex cursor-pointer items-center justify-between px-3 py-2" style={{ color: 'var(--text)' }}>
                              <span>Model visibility</span>
                              <span style={{ color: 'var(--text-3)' }}>{(defaults.hiddenProviderIds?.length ?? 0) + (defaults.hiddenModelIds?.length ?? 0) ? [visibilityGroups.length && defaults.hiddenProviderIds?.length ? `${defaults.hiddenProviderIds.length} providers` : '', defaults.hiddenModelIds?.length ? `${defaults.hiddenModelIds.length} models` : ''].filter(Boolean).join(', ') + ' hidden' : 'All shown'}</span>
                            </summary>
                            <div className="grid gap-1 px-2 pb-2">
                              {!visibilityGroups.length && modelOptions.filter((option) => option.id).map((option) => (
                                <label key={option.id} className="flex cursor-pointer items-center gap-2 rounded-md px-2 py-1.5" style={{ background: 'var(--bg)' }}>
                                  <input type="checkbox" checked={!defaults.hiddenModelIds?.includes(option.id)} onChange={() => toggleVisibility('hiddenModelIds', option.id)} />
                                  <span className="truncate" style={{ color: defaults.hiddenModelIds?.includes(option.id) ? 'var(--text-3)' : 'var(--text)' }}>{option.label}</span>
                                </label>
                              ))}
                              {visibilityGroups.map((group) => {
                                const groupModels = modelOptions.filter((option) => option.id && option.id.split('/')[0] === group)
                                const groupShown = !defaults.hiddenProviderIds?.includes(group)
                                const shownCount = groupModels.filter((option) => !defaults.hiddenModelIds?.includes(option.id)).length
                                return (
                                  <details key={group} className="rounded-md" style={{ background: 'var(--bg)' }}>
                                    <summary className="flex cursor-pointer items-center gap-2 px-2 py-1.5">
                                      <input type="checkbox" aria-label={`Show ${group}`} checked={groupShown} onClick={(event) => event.stopPropagation()} onChange={() => toggleVisibility('hiddenProviderIds', group)} />
                                      <span className="min-w-0 flex-1 truncate font-medium" style={{ color: groupShown ? 'var(--text)' : 'var(--text-3)' }}>{group}</span>
                                      <span style={{ color: 'var(--text-3)' }}>{groupShown ? `${shownCount}/${groupModels.length}` : 'hidden'}</span>
                                    </summary>
                                    <div className="grid gap-0.5 pb-1.5 pl-8 pr-2" style={{ opacity: groupShown ? 1 : 0.5 }}>
                                      {groupModels.map((option) => (
                                        <label key={option.id} className="flex cursor-pointer items-center gap-2 py-1">
                                          <input type="checkbox" disabled={!groupShown} checked={!defaults.hiddenModelIds?.includes(option.id)} onChange={() => toggleVisibility('hiddenModelIds', option.id)} />
                                          <span className="truncate">{option.label}</span>
                                        </label>
                                      ))}
                                    </div>
                                  </details>
                                )
                              })}
                              <div className="px-1 pt-1" style={{ color: 'var(--text-3)' }}>Hidden models stay available in existing chats.</div>
                            </div>
                          </details>
                        )}
                        <div className="grid gap-2" style={{gridTemplateColumns:"repeat(auto-fit,minmax(170px,1fr))"}}>
                          <div className="grid gap-1">
                            <div>Model</div>
                            {renderDefaultsMenu(
                              `${provider.id}:model`,
                              defaults.modelId,
                              `${providerName} default model`,
                              modelOptions,
                              (modelId) => updateProviderDefaults(provider.id, {
                                ...defaults,
                                modelId,
                                reasoningEffort: reasoningEffortForModel(providerAcp, modelId, defaults.reasoningEffort),
                              })
                            )}
                          </div>
                          {reasoningOptions.length > 0 && (
                            <div className="grid gap-1">
                              <div>Reasoning</div>
                              {renderDefaultsMenu(
                                `${provider.id}:reasoning`,
                                defaults.reasoningEffort,
                                `${providerName} default reasoning`,
                                reasoningOptions,
                                (reasoningEffort) => updateProviderDefaults(provider.id, { ...defaults, reasoningEffort })
                              )}
                            </div>
                          )}
                          {caps.hasServiceTier && speedOptions.length > 0 && (
                            <div className="grid gap-1">
                              <div>Speed</div>
                              {renderDefaultsMenu(
                                `${provider.id}:speed`,
                                defaults.serviceTier,
                                `${providerName} default speed`,
                                speedOptions,
                                (serviceTier) => updateProviderDefaults(provider.id, { ...defaults, serviceTier })
                              )}
                            </div>
                          )}
                        </div>
                        <div className="grid gap-2" style={{gridTemplateColumns:"repeat(auto-fit,minmax(170px,1fr))"}}>
                          <div className="grid gap-1">
							<div>Provider mode</div>
                            {renderDefaultsMenu(
                              `${provider.id}:mode`,
                              defaults.approvalMode,
							  `${providerName} default provider mode`,
                              modeOptions,
                              (approvalMode) => updateProviderDefaults(provider.id, { ...defaults, approvalMode })
                            )}
                          </div>

                          <div className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
                            <div>Permissions</div>
                            {caps.structuredPermissionControl === 'uam'
                              ? renderDefaultsMenu(
                                  `${provider.id}:permissions`,
                                  defaults.commandSafetyTier,
                                  `${providerName} default permissions`,
                                  PERMISSION_DEFAULT_OPTIONS.filter((option) => option.id !== 'acceptEdits' || caps.hasAcceptEditsMode),
                                  (commandSafetyTier) => {
                                    const nextDefaults = { ...defaults, commandSafetyTier: commandSafetyTier as ProviderChatDefaults['commandSafetyTier'] }
                                    if (commandSafetyTier === 'yolo' && defaults.commandSafetyTier !== 'yolo') {
                                      setPendingDefaultYolo({ providerId: provider.id, providerName, defaults: nextDefaults })
                                      return
                                    }
                                    updateProviderDefaults(provider.id, nextDefaults)
                                  }
                                )
                              : <div className="rounded-md border px-2 py-2" style={{ borderColor: 'var(--border)' }}>Provider managed</div>}
                          </div>
                          <div className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
                            <div>Memory</div>
                            <MenuSelect label={`${providerName} default memory level`} value={defaults.memoryLevel ?? 'default'} disabled={memoryPolicySaving[provider.id]}
                              options={MEMORY_POLICY_CHOICES.map((option) => ({ value: option.id, label: option.label, description: option.id === 'default' ? 'Use the current app default.' : option.detail }))}
                              onChange={(memoryLevel) => void saveMemoryPolicy(provider.id, () => setProviderChatDefaults({ providerChatDefaults: {
                                [provider.id]: { ...defaults, memoryLevel: memoryLevel as MemoryLevel,
                                  memoryEnabled: memoryLevel === 'default' ? memoryEnabledDefault : memoryLevel !== 'off' },
                              } }, settingsScope))} />
                            {memoryPolicyError[provider.id] && <p role="alert" style={{ color: 'var(--error)' }}>{memoryPolicyError[provider.id]}</p>}
                          </div>
                        </div>
                        {pendingDefaultYolo?.providerId === provider.id && (
                          <Notice
                            tone="warning"
                            title="Use YOLO by default?"
                            dismissLabel={`Dismiss ${providerName} default YOLO warning`}
                            onDismiss={() => setPendingDefaultYolo(null)}
                            actions={(
                              <>
                                <Button size="sm" onClick={() => setPendingDefaultYolo(null)}>Cancel</Button>
                                <Button size="sm" variant="danger" onClick={() => {
                                  const pending = pendingDefaultYolo
                                  setPendingDefaultYolo(null)
                                  updateProviderDefaults(pending.providerId, pending.defaults)
                                }}>Use YOLO</Button>
                              </>
                            )}
                          >
                            New {pendingDefaultYolo.providerName} chats will automatically approve {COMPUTER_USE_ENABLED ? 'computer use, commands' : 'commands'}, file changes, and every other permission request.
                          </Notice>
                        )}
                        <details className="uam-skill-folder">
                          <summary className="uam-skill-folder__summary" style={{ fontSize: 'var(--fs-sm)' }}><ChevronRight size={13} aria-hidden className="uam-skill-folder__chevron" />Advanced</summary>
                          <div className="grid gap-3">
                        <div className="grid gap-1">
                          <div>Feature preference</div>
                          {renderDefaultsMenu(
                            `${provider.id}:feature-preference`,
                            defaults.featurePreference ?? 'uam',
                            `${providerName} feature preference`,
                            [
                              { id: 'uam', label: 'Prefer UAM', detail: 'Use UAM agents, reviewed goals, and permission mediation.' },
                              { id: 'provider', label: `Prefer ${providerName}`, detail: 'Use provider-native features when available.' },
                            ],
                            (featurePreference) => updateProviderDefaults(provider.id, { ...defaults, featurePreference: featurePreference === 'provider' ? 'provider' : 'uam' })
                          )}
                        </div>
                        <div className="grid gap-1">
                          <div>Goal reviewer model</div>
                          {renderDefaultsMenu(
                            `${provider.id}:goal-reviewer`,
                            defaults.reviewerModelId || defaults.modelId,
                            `${providerName} goal reviewer model`,
                            reviewerModelOptions,
                            (reviewerModelId) => updateProviderDefaults(provider.id, { ...defaults, reviewerModelId })
                          )}
                          <span style={{ color: 'var(--text-3)' }}>Used for architecture and read-only review.</span>
                        </div>
                        <div className="grid gap-1">
                          <Button
                            variant={defaults.smallModelMode ? 'primary' : 'secondary'}
                            size="sm"
                            aria-pressed={defaults.smallModelMode}
                            onClick={() => updateProviderDefaults(provider.id, { ...defaults, smallModelMode: !defaults.smallModelMode })}
                          >
                            {defaults.smallModelMode ? 'Architect + worker on' : 'Architect + worker off'}
                          </Button>
                          <span style={{ color: 'var(--text-3)' }}>Reviewer plans and checks each step; the model implements it.</span>
                        </div>
                          </div>
                        </details>
                        {modelRefreshError && !modelsLoading ? (
                          <Notice
                            key={`${provider.id}:${providerWorkspace}:${modelRefreshError}`}
                            tone="error"
                            title="Model refresh failed"
                            dismissLabel={`Dismiss ${providerName} model refresh error`}
                          >
                            {modelRefreshError}
                          </Notice>
                        ) : (
                          <span role="status" className="text-xs" style={{ color: 'var(--text-3)' }}>
                            {modelsLoading ? 'Refreshing models…' : !providerWorkspace ? 'Add a workspace to refresh models' : hasDiscoveredModels ? 'Model catalog available' : 'Using provider defaults; refresh to discover models'}
                          </span>
                        )}
                      </div>
                    </ProviderDisclosureCard>
                  )
                })}
              </div>
            </div>
          </SettingsSection>}
        </div>
      )
    }

    if (MOBILE_COMPANION_ENABLED && selectedSection === 'phone') return <div className="space-y-4">{renderPhoneAccess()}</div>

    if (selectedSection === 'agents') {
      const selectableCustomAgents = activeUamAgents
        .filter((agent) => !agent.builtIn && !favoriteUamAgentIds.includes(agent.id))
        .sort((left, right) => left.id.localeCompare(right.id))
      const selectedCandidate = selectableCustomAgents.some((agent) => agent.id === favoriteAgentCandidate)
        ? favoriteAgentCandidate
        : selectableCustomAgents[0]?.id ?? ''
      const updatePreferences = (favoriteIds: string[], shortcut: UamAgentCycleShortcut = uamAgentCycleShortcut) => {
        void setUamAgentPreferences({ favoriteUamAgentIds: favoriteIds, uamAgentCycleShortcut: shortcut })
      }
      const moveFavorite = (index: number, offset: -1 | 1) => {
        const target = index + offset
        if (target < 0 || target >= favoriteUamAgentIds.length) return
        const next = [...favoriteUamAgentIds]
        ;[next[index], next[target]] = [next[target], next[index]]
        updatePreferences(next)
      }
      const previewImport = async () => {
        if (agentImportBusy || !agentImportPath.trim()) return
        const requestId = ++agentImportRequest.current
        setAgentImportPreview(null)
        setAgentImportBusy(true)
        setAgentImportMessage('')
        const preview = await previewProviderAgentImport(agentImportProvider, agentImportPath.trim())
        if (requestId !== agentImportRequest.current) return
        setAgentImportPreview(preview)
        setAgentImportId(preview?.suggestedId ?? '')
        setAgentImportAcknowledged(false)
        setAgentImportMessage(preview ? '' : 'Preview failed.')
        setAgentImportBusy(false)
      }
      // Only a workspace-scoped import needs a chat; a global import is admitted without one.
      const importNeedsChat = agentImportWorkspaceScope && !activeSessionId
      const runImport = async () => {
        if (agentImportBusy || importNeedsChat || !agentImportPreview?.supported) return
        const requestId = ++agentImportRequest.current
        setAgentImportBusy(true)
        const imported = await importProviderAgent({
          chatId: activeSessionId ?? '',
          providerId: agentImportProvider,
          sourcePath: agentImportPath.trim(),
          canonicalId: agentImportId.trim(),
          workspaceAccess: agentImportAccess,
          workspaceScope: agentImportWorkspaceScope,
          acknowledgeIgnoredFields: agentImportAcknowledged,
        })
        if (requestId !== agentImportRequest.current) return
        setAgentImportMessage(imported ? `Imported ${agentImportId.trim()}.` : 'Import failed. The source or target may have changed; preview it again.')
        if (imported) setAgentImportPreview(null)
        setAgentImportBusy(false)
      }
      return (
        <div className="space-y-4">
          <SettingsSection
            title="Agent cycle"
            actions={
              <>
                <div className="w-[220px] max-w-full">
                  <MenuSelect
                    label="Favourite agent"
                    value={selectedCandidate}
                    options={selectableCustomAgents.length > 0
                      ? selectableCustomAgents.map((agent) => ({ value: agent.id, label: agent.id, description: agent.description }))
                      : [{ value: '', label: activeSessionId ? 'No more selectable custom agents' : 'Open a chat to list workspace agents' }]}
                    onChange={setFavoriteAgentCandidate}
                    disabled={selectableCustomAgents.length === 0}
                  />
                </div>
                <Button
                  size="sm"
                  variant="secondary"
                  leadingIcon={<Plus size={14} />}
                  disabled={!selectedCandidate}
                  onClick={() => {
                    updatePreferences([...favoriteUamAgentIds, selectedCandidate])
                    setFavoriteAgentCandidate('')
                  }}
                >Add favourite</Button>
              </>
            }
          >
            <SettingRow
              title="Cycle shortcut"
              description="Moves to the next agent in the composer."
              htmlFor="agent-cycle-shortcut"
              control={
                <MenuSelect
                  id="agent-cycle-shortcut"
                  label="Agent cycle shortcut"
                  value={uamAgentCycleShortcut}
                  options={[
                    { value: 'shift+tab', label: 'Shift+Tab' },
                    { value: 'control+shift+tab', label: 'Ctrl+Shift+Tab' },
                    { value: 'alt+shift+tab', label: 'Alt+Shift+Tab' },
                    { value: 'meta+shift+tab', label: 'Command+Shift+Tab' },
                    { value: 'disabled', label: 'Disabled' },
                  ]}
                  onChange={(value) => updatePreferences(favoriteUamAgentIds, value as UamAgentCycleShortcut)}
                />
              }
            />
            {favoriteUamAgentIds.length === 0 && (
              <SettingRow title="No custom favourites" description="The cycle is Build, then Plan." />
            )}
            {favoriteUamAgentIds.map((agentId, index) => {
              const available = activeUamAgents.some((agent) => agent.id === agentId)
              return (
                <SettingRow
                  key={agentId}
                  icon={<GripVertical size={14} aria-hidden />}
                  title={<span className="font-mono">{agentId}</span>}
                  description={`Position ${index + 1} in the cycle`}
                  status={available ? undefined : <><StatusDot tone="warning" /> Unavailable in this workspace</>}
                  control={
                    <div className="flex items-center gap-1">
                      <IconButton icon={<ArrowUp size={14} />} label={`Move ${agentId} up`} disabled={index === 0} onClick={() => moveFavorite(index, -1)} />
                      <IconButton icon={<ArrowDown size={14} />} label={`Move ${agentId} down`} disabled={index === favoriteUamAgentIds.length - 1} onClick={() => moveFavorite(index, 1)} />
                      <IconButton icon={<X size={14} />} label={`Remove ${agentId}`} variant="danger" onClick={() => updatePreferences(favoriteUamAgentIds.filter((id) => id !== agentId))} />
                    </div>
                  }
                />
              )
            })}
          </SettingsSection>

          {AGENT_IMPORT_PROVIDER_OPTIONS.length > 0 && <SettingsSection title="Import from a provider">
            <SettingRow
              title="Source provider"
              htmlFor="agent-import-source-provider"
              control={
                <MenuSelect
                  id="agent-import-source-provider"
                  label="Source provider"
                  value={agentImportProvider}
                  options={AGENT_IMPORT_PROVIDER_OPTIONS}
                  onChange={(value) => {
                    setAgentImportProvider(value)
                    invalidateAgentImport()
                  }}
                />
              }
            />
            <div className="grid gap-3">
              <SettingsField label="Native Markdown file" htmlFor="agent-import-path">
                <div className="flex gap-2">
                  <input
                    id="agent-import-path"
                    aria-label="Native agent Markdown file"
                    value={agentImportPath}
                    onChange={(event) => {
                      setAgentImportPath(event.target.value)
                      invalidateAgentImport()
                    }}
                    className="uam-field min-w-0 flex-1 font-mono"
                  />
                  <Button size="sm" variant="secondary" onClick={() => {
                    invalidateAgentImport()
                    const requestId = agentImportRequest.current
                    void browseProviderAgentImport(agentImportPath).then((path) => {
                      if (!path || requestId !== agentImportRequest.current) return
                      setAgentImportPath(path)
                    })
                  }}>Browse</Button>
                  <Button size="sm" variant={agentImportPath.trim() ? 'primary' : 'secondary'} aria-busy={agentImportBusy || undefined} disabled={agentImportBusy || !agentImportPath.trim()} onClick={() => void previewImport()}>Preview</Button>
                </div>
              </SettingsField>

              {agentImportPreview && (
                <div className="grid gap-3 rounded-md p-3 text-xs" style={{ border: '1px solid var(--border)', background: 'var(--surface-up)', color: 'var(--text-2)' }}>
                  <div className="text-sm font-medium" style={{ color: 'var(--text)' }}>
                    {agentImportPreview.description || agentImportPreview.suggestedId || 'Provider agent'}
                    <span className="font-normal" style={{ color: 'var(--text-3)' }}> · {agentImportPreview.mode || 'unknown mode'}</span>
                  </div>
                  {agentImportPreview.securityFields.length > 0 && (
                    <div role="alert" style={{ color: 'var(--error)' }}>Blocked security/tool fields: {agentImportPreview.securityFields.join(', ')}</div>
                  )}
                  {agentImportPreview.ignoredFields.length > 0 && (
                    <div>Provider-only fields that will be omitted: {agentImportPreview.ignoredFields.join(', ')}</div>
                  )}
                  {agentImportPreview.error && <div role="alert" style={{ color: 'var(--error)' }}>{agentImportPreview.error}</div>}
                  {agentImportPreview.supported && (
                    <>
                      <SettingsField label="UAM agent ID" htmlFor="agent-import-id">
                        <input
                          id="agent-import-id"
                          aria-label="Imported UAM agent ID"
                          value={agentImportId}
                          onChange={(event) => setAgentImportId(event.target.value)}
                          className="uam-field font-mono"
                        />
                      </SettingsField>
                      <div className="grid gap-3 sm:grid-cols-2">
                        <SettingsField label="Workspace access" htmlFor="agent-import-access">
                          <MenuSelect
                            id="agent-import-access"
                            label="Workspace access"
                            value={agentImportAccess}
                            options={[
                              { value: 'read', label: 'Read only', description: 'The imported agent cannot approve writes.' },
                              { value: 'write', label: 'Read and write', description: 'The imported agent may request workspace changes.' },
                            ]}
                            onChange={(value) => setAgentImportAccess(value === 'write' ? 'write' : 'read')}
                          />
                        </SettingsField>
                        <SettingsField label="Install scope" htmlFor="agent-import-scope">
                          <MenuSelect
                            id="agent-import-scope"
                            label="Install scope"
                            value={agentImportWorkspaceScope ? 'workspace' : 'global'}
                            options={[
                              { value: 'workspace', label: 'This workspace' },
                              { value: 'global', label: 'All workspaces' },
                            ]}
                            onChange={(value) => setAgentImportWorkspaceScope(value === 'workspace')}
                          />
                        </SettingsField>
                      </div>
                      {agentImportPreview.ignoredFields.length > 0 && (
                        <label className="flex items-start gap-2" style={{ color: 'var(--text-2)' }}>
                          <input
                            type="checkbox"
                            aria-label="Acknowledge omitted provider fields"
                            checked={agentImportAcknowledged}
                            onChange={(event) => setAgentImportAcknowledged(event.target.checked)}
                          />
                          <span>I understand the listed provider-only fields will not be copied.</span>
                        </label>
                      )}
                      <div className="flex justify-end">
                        <Button
                          size="sm"
                          variant="primary"
                          aria-busy={agentImportBusy || undefined}
                          disabled={agentImportBusy || importNeedsChat || !agentImportId.trim() || (agentImportPreview.ignoredFields.length > 0 && !agentImportAcknowledged)}
                          onClick={() => void runImport()}
                        >Import agent</Button>
                      </div>
                    </>
                  )}
                </div>
              )}
              {agentImportMessage && <div role="status" className="text-xs" style={{ color: agentImportMessage.startsWith('Imported') ? 'var(--success)' : 'var(--error)' }}>{agentImportMessage}</div>}
              {importNeedsChat && <div className="text-xs" style={{ color: 'var(--text-3)' }}>Open a workspace chat to import into this workspace, or choose All workspaces.</div>}
            </div>
          </SettingsSection>}
        </div>
      )
    }

    if (selectedSection === 'cli-version') {
      const managers = (effectiveProviderSettingsHostId === 'local' ? cliVersionManager.providers : (cliVersionManager.remoteProviders ?? []).filter(manager => manager.executionHostId === effectiveProviderSettingsHostId)).filter(manager => !scopedProviderId || manager.providerId === scopedProviderId)
      return (
        <div ref={cliVersionMenuRef} className="grid gap-3">
          {scopedProviderId && hasSshHosts && scopeValue === SETTINGS_ALL_ENVIRONMENTS && <p className="text-xs" style={{ color: 'var(--text-3)' }}>Choose a computer above to manage its CLI version.</p>}
          {managers.length === 0 && <p className="text-xs" style={{ color: 'var(--text-3)' }}>No CLI installation information is available. Check the provider on this host.</p>}
          {managers.map((manager) => {
            const cliVersionProvider = providers.find((provider) => provider.id === manager.providerId)
            const providerName = providerDisplayName(cliVersionProvider, manager.providerId)
            const actionErrorKey = cliActionErrorKey(manager.providerId, manager.executionHostId || effectiveProviderSettingsHostId)
            const installHostName = manager.executionHostId && manager.executionHostId !== 'local'
              ? manager.executionHostName || executionHosts.find((host) => host.id === manager.executionHostId)?.label || manager.executionHostId
              : 'this computer'
            const issues = [...cliVersionIssues(manager), ...(cliActionError[actionErrorKey] ? [cliActionError[actionErrorKey]] : [])]
            const requestInstall = (version: string) => {
              setOpenCliVersionMenu(null)
              setPendingCliInstall({providerId:manager.providerId,providerName,version})
            }
            return (
              <div key={manager.providerId} className="grid gap-3">
                {manager.running && <div role="progressbar" aria-label={`${providerName} CLI task running`} className="uam-settings-progress" />}
                <div className="flex items-start justify-between gap-3">
                  <div className="grid gap-1 text-xs" style={{color:'var(--warning)'}}>{issues.map(issue => <div key={issue}>{issue}</div>)}</div>
                  <div className="flex items-center gap-1">
                    <StatusIndicator issues={issues} okLabel={manager.status === 'provider-managed' ? 'Provider managed' : 'CLI verified'}/>
                    <IconButton icon={<RefreshCw size={15}/>} label={`Refresh ${providerName} CLI version`} disabled={manager.running} onClick={async () => {
                      setCliActionError(current => ({...current,[actionErrorKey]:''}))
                      try { if (!await (manager.executionHostId && manager.executionHostId !== 'local' ? refreshCliProviderVersion(manager.providerId, manager.executionHostId) : refreshCliProviderVersion(manager.providerId))) setCliActionError(current => ({...current,[actionErrorKey]:'Version check could not start.'})) }
                      catch { setCliActionError(current => ({...current,[actionErrorKey]:'Version check could not start.'})) }
                    }}/>
                    {manager.canInstall !== false && <IconButton ref={element => { if (element) popupAnchorsRef.current.set(`${manager.providerId}:download`,element) }} icon={<Download size={15}/>} label={`Download ${providerName} CLI`} aria-haspopup="menu" aria-expanded={openCliVersionMenu === `${manager.providerId}:download`} disabled={manager.running} onClick={() => setOpenCliVersionMenu(current => current === `${manager.providerId}:download` ? null : `${manager.providerId}:download`)}/>}
                    {openCliVersionMenu === `${manager.providerId}:download` && <ViewportMenu anchorRef={{current:popupAnchorsRef.current.get(`${manager.providerId}:download`) ?? null}} role="menu" aria-label={`${providerName} download choices`} className="grid gap-1 p-2" style={{background:'var(--surface)',border:'1px solid var(--border)',borderRadius:8}}>
                      <Button size="sm" role="menuitem" disabled={!manager.preferredVersion} onClick={() => requestInstall(manager.preferredVersion)}>Download recommended{manager.preferredVersion ? ` ${manager.preferredVersion}` : ''}</Button>
                      <Button size="sm" role="menuitem" onClick={() => requestInstall('latest')}>Download latest</Button>
                    </ViewportMenu>}
                  </div>
                </div>
                <div className="grid grid-cols-2 gap-3 text-sm">
                  <div><div className="text-xs" style={{color:'var(--text-3)'}}>Installed</div>{manager.installedVersion || 'Not detected'}</div>
                  <div><div className="text-xs" style={{color:'var(--text-3)'}}>Recommended</div>{manager.preferredVersion || 'Unavailable'}</div>
                </div>
                {pendingCliInstall?.providerId === manager.providerId && <div className="flex flex-wrap items-center gap-3 text-xs" role="group" aria-label="Confirm CLI installation">
                  <span>Install {pendingCliInstall.providerName} {pendingCliInstall.version} on {installHostName} using {manager.installMethod || 'npm'}?</span>
                  <Button size="sm" onClick={() => setPendingCliInstall(null)}>Cancel</Button>
                  <Button size="sm" disabled={manager.running} onClick={async () => {
                    const pending = pendingCliInstall
                    setPendingCliInstall(null)
                    setCliActionError(current => ({...current,[actionErrorKey]:''}))
                    try { if (!await (manager.executionHostId && manager.executionHostId !== 'local' ? applyCliProviderVersion(pending.providerId,pending.version,manager.executionHostId) : applyCliProviderVersion(pending.providerId,pending.version))) setCliActionError(current => ({...current,[actionErrorKey]:'Installation could not start. Check provider output.'})) }
                    catch { setCliActionError(current => ({...current,[actionErrorKey]:'Installation could not start. Check provider output.'})) }
                  }}>Install version</Button>
                </div>}
                {(manager.lastCommand || manager.lastOutput) && (
                  <div className="grid gap-2">
                    {manager.lastCommand && (
                      <code className="text-xs rounded-md px-2 py-1" style={{ background: 'var(--surface-up)', color: 'var(--text)' }}>
                        {manager.lastCommand}
                      </code>
                    )}
                    <pre
                      className="text-[11px] leading-5 overflow-auto rounded-lg px-3 py-2"
                      style={{
                        maxHeight: 180,
                        background: 'var(--surface-up)',
                        color: 'var(--text-2)',
                        border: '1px solid var(--border)',
                        whiteSpace: 'pre-wrap',
                        wordBreak: 'break-word',
                        fontFamily: 'var(--font-mono)',
                      }}
                    >
                      {manager.lastOutput || 'No output captured yet.'}
                    </pre>
                  </div>
                )}
              </div>
            )
          })}
        </div>
      )
    }

    if (SSH_ENABLED && selectedSection === 'routing') return <ExecutionRoutingSettings ref={routingRef} variant="page" />

    if (SSH_ENABLED && selectedSection === 'remote-hosts') {
      const remoteHosts = executionHosts.filter((host) => host.id !== 'local')
      const statusColor = (status?: string) => status === 'ready' ? 'var(--green)' : status === 'error' ? 'var(--red)' : 'var(--text-3)'
      return (
        <div className="space-y-4">
          {COMPUTER_USE_ENABLED && <details className="text-xs" style={{ color: 'var(--text-3)' }}>
            <summary className="flex cursor-pointer items-center gap-2" style={{ color: 'var(--text-2)' }}>
              <Info size={14} aria-hidden />Remote Computer Use is off
            </summary>
            <p className="mt-1.5 pl-[22px]">Remote chats can't see or control the screen.</p>
          </details>}

          <SettingsSection
            title="Add a host"
            actions={
              <Button size="sm" variant="primary" leadingIcon={<Server size={14} />} aria-busy={remoteBusy || undefined} disabled={remoteBusy || !remoteAlias.trim()} onClick={() => void previewRemoteHost()}>
                Preview setup
              </Button>
            }
          >
            <div className="grid gap-3 sm:grid-cols-2">
              <SettingsField label="Display name" htmlFor="remote-host-name">
                <input
                  id="remote-host-name"
                  aria-label="Remote host display name"
                  value={remoteLabel}
                  maxLength={128}
                  placeholder="AI desktop"
                  className="uam-field"
                  onChange={(event) => { setRemoteLabel(event.target.value); setRemotePreview(null); setRemoteMessage('') }}
                />
              </SettingsField>
              <SettingsField label="SSH config alias" htmlFor="remote-host-alias" hint="The Host name from your ~/.ssh/config.">
                <input
                  id="remote-host-alias"
                  aria-label="SSH config alias"
                  value={remoteAlias}
                  maxLength={255}
                  placeholder="ai-desktop"
                  spellCheck={false}
                  className="uam-field font-mono"
                  onChange={(event) => { setRemoteAlias(event.target.value); setRemotePreview(null); setRemoteMessage('') }}
                />
              </SettingsField>
            </div>
            {remoteMessage && <div role="status" className="text-xs" style={{ color: remoteMessage.endsWith('ready.') || remoteMessage.includes('removed') ? 'var(--success)' : 'var(--text-2)' }}>{remoteMessage}</div>}
          </SettingsSection>

          <SettingsSection title="Configured hosts">
            {remoteHosts.length === 0 && <SettingRow title="No remote hosts yet" />}
            {remoteHosts.map((host) => (
              <SettingRow
                key={host.id}
                icon={<Server size={15} aria-hidden />}
                title={host.label}
                description={<span className="font-mono">{host.sshAlias}</span>}
                status={(
                  <span className="inline-flex flex-wrap items-center gap-1.5">
                    <StatusDot tone={host.runnerStatus === 'ready' ? 'success' : host.runnerStatus === 'error' ? 'error' : 'neutral'} />
                    <span>{host.runnerStatus}</span>
                    {(host.runnerVersion || host.platform) && <span style={{ color: 'var(--text-3)' }}>· {[host.runnerVersion ? `runner ${host.runnerVersion}` : '', host.platform ? `${host.platform} ${host.architecture}` : ''].filter(Boolean).join(' · ')}</span>}
                  </span>
                )}
                control={(
                  <div className="flex items-center gap-1">
                    <IconButton icon={<RefreshCw size={14} />} label={`Reinstall helper on ${host.label}`} disabled={isRemoteHostBusy(host.id)} onClick={() => void previewRemoteHost(host)} />
                    <IconButton icon={<Trash2 size={14} />} label={`Remove ${host.label}`} variant="danger" disabled={isRemoteHostBusy(host.id)} onClick={() => setPendingHostRemoval(host)} />
                  </div>
                )}
              >
                <div className="grid gap-3">
                  {isRemoteHostBusy(host.id) && <div role="progressbar" aria-label={`Remote host task running on ${host.label}`} className="uam-settings-progress" />}
                  <div className="font-mono text-xs" style={{ color: 'var(--text-3)' }}>Helper: home / {host.runnerDirectory || (host.platform === 'windows' ? '.uam/runner' : '.local/share/uam/runner')}</div>
                  <details>
                    <summary className="cursor-pointer text-xs font-medium" style={{ color: 'var(--text)' }}>Host options</summary>
                    <RemoteHostOptionsForm
                      host={host}
                      busy={isRemoteHostBusy(host.id)}
                      onSave={(draft) => void saveRemoteHostOptions(host, draft)}
                      onCheck={(instructionFile) => checkRemoteHostInstruction(host, instructionFile)}
                    />
                  </details>
                </div>
              </SettingRow>
            ))}
          </SettingsSection>
        </div>
      )
    }

    if (selectedSection === 'memory-settings') {
      return (
        <div className="grid gap-0">
          <SettingsSection title="Memory defaults">
            <SettingRow
              title="Default memory level"
              description={MEMORY_POLICY_CHOICES.find((option) => option.id === memoryLevelDefault)?.detail}
              control={
                <div role="radiogroup" aria-label="Default memory level" className="flex flex-wrap gap-1" onKeyDown={selectPolicyWithArrowKeys}>
                  {MEMORY_POLICY_CHOICES.filter((option) => option.id !== 'default').map((option) => {
                    const selected = option.id === memoryLevelDefault
                    return <button key={option.id} type="button" role="radio" aria-checked={selected} disabled={memoryPolicySaving.app} onClick={() => void saveMemoryPolicy('app', () => setMemorySettings({ memoryLevelDefault: option.id }))} className="uam-segment-button rounded-md px-3 py-2 text-xs" style={{ border: '1px solid var(--border)', background: selected ? 'var(--accent-dim)' : 'transparent', color: selected ? 'var(--accent)' : 'var(--text-2)' }}>{option.label}</button>
                  })}
                </div>
              }
            />
            {memoryPolicyError.app && <p role="alert" className="px-3 text-xs" style={{ color: 'var(--error)' }}>{memoryPolicyError.app}</p>}
            {sessions.find((session) => session.id === activeSessionId) && <SettingRow title="Current chat memory" description="Default follows the provider policy."
              status={memoryPolicySaving.chat ? 'Saving…' : undefined}
              control={<MenuSelect label="Current chat memory policy" value={sessions.find((session) => session.id === activeSessionId)?.memoryLevel ?? 'default'} disabled={memoryPolicySaving.chat}
                options={MEMORY_POLICY_CHOICES.map((option) => ({ value: option.id, label: option.label, description: option.detail }))}
                onChange={(level) => { if (activeSessionId) void saveMemoryPolicy('chat', () => setSessionMemoryLevel(activeSessionId, level as MemoryLevel)) }} />}>
              {memoryPolicyError.chat && <p role="alert" className="text-xs" style={{ color: 'var(--error)' }}>{memoryPolicyError.chat}</p>}
            </SettingRow>}
            <SettingRow
              title="Idle delay (seconds)"
              description="Idle time before a chat's memory is processed."
              htmlFor="memory-idle-delay"
              control={
                <input
                  id="memory-idle-delay"
                  type="number"
                  className="uam-field"
                  style={{ width: 120 }}
                  min={MIN_MEMORY_IDLE_DELAY_SECONDS}
                  max={MAX_MEMORY_IDLE_DELAY_SECONDS}
                  value={memoryIdleDelaySeconds}
                  onChange={(event) => void setMemorySettings({ memoryIdleDelaySeconds: Number(event.currentTarget.value) })}
                />
              }
            />
            <SettingRow
              title="Recall budget (bytes)"
              description="Most saved memory added to each prompt."
              htmlFor="memory-recall-budget"
              control={
                <input
                  id="memory-recall-budget"
                  type="number"
                  className="uam-field"
                  style={{ width: 120 }}
                  min={MIN_MEMORY_RECALL_BUDGET_BYTES}
                  max={MAX_MEMORY_RECALL_BUDGET_BYTES}
                  step={256}
                  value={memoryRecallBudgetBytes}
                  onChange={(event) => void setMemorySettings({ memoryRecallBudgetBytes: Number(event.currentTarget.value) })}
                />
              }
            />
          </SettingsSection>

          <SettingsSection title="Memory workers">
            <div ref={memoryMenuRef}>
              {providers.filter((provider) => providerSupportsTextWorkers(provider.id)).map((provider) => {
                const binding = memoryWorkerBindings[provider.id] ?? { workerProviderId: provider.id, workerModelId: '' }
                const workerProvider = providers.find((candidate) => candidate.id === binding.workerProviderId) ?? provider
                const workerSession = latestLocalProviderSession(sessions, binding.workerProviderId)
                const workerWorkspace = workerSession?.workspaceDirectory || localWorkspace
                const workerAcp = (workerSession ? acpBindings[workerSession.id] : undefined)
                  ?? providerModelCatalogs.find((catalog) => (catalog.executionHostId || 'local') === 'local' && catalog.providerId === binding.workerProviderId && workspaceKey(catalog.workspaceDirectory) === workspaceKey(workerWorkspace))
                const providerMenuId = `${provider.id}:provider`
                const modelMenuId = `${provider.id}:model`
                const modelOptions = memoryModelOptions(workerProvider, binding.workerProviderId, binding.workerModelId, workerAcp?.availableModels)
                return (
                  <SettingRow
                    key={provider.id}
                    icon={<ProviderLogo providerId={provider.id} />}
                    title={`${providerDisplayName(provider, provider.id)} chats`}
                    control={
                    <div className="grid grid-cols-2 gap-2" style={{ width: 380 }}>
                      <div className="relative">
                        <button
                          ref={(element) => { if (element) popupAnchorsRef.current.set(providerMenuId, element) }}
                          type="button"
                          title={`${providerDisplayName(provider, provider.id)} memory worker provider`}
                          aria-haspopup="listbox"
                          aria-expanded={openMemoryMenu === providerMenuId}
                          onClick={() => setOpenMemoryMenu(openMemoryMenu === providerMenuId ? null : providerMenuId)}
                          className="uam-menu-select__trigger flex w-full items-center justify-between gap-2 rounded-md px-3 py-2 text-left text-sm"
                        >
                          <span className="flex min-w-0 items-center gap-2 truncate">
                            <ProviderLogo providerId={binding.workerProviderId} />
                            <span className="truncate">{providerDisplayName(workerProvider, binding.workerProviderId)}</span>
                          </span>
                          <ChevronDown className={openMemoryMenu === providerMenuId ? 'uam-menu-select__chevron is-open' : 'uam-menu-select__chevron'} size={14} aria-hidden />
                        </button>
                        {openMemoryMenu === providerMenuId && (
                          <ViewportMenu
                            anchorRef={{ current: popupAnchorsRef.current.get(providerMenuId) ?? null }}
                            role="listbox"
                            aria-label={`${providerDisplayName(provider, provider.id)} memory worker provider`}
                            style={{
                              width: popupAnchorsRef.current.get(providerMenuId)?.getBoundingClientRect().width,
                              border: '1px solid var(--border-bright)',
                              borderRadius: 10,
                              background: 'var(--surface)',
                              boxShadow: 'var(--elev-3)',
                              padding: 6,
                            }}
                          >
                            {providers.filter((candidate) => providerSupportsTextWorkers(candidate.id)).map((candidate) => {
                              const selected = candidate.id === binding.workerProviderId
                              return (
                                <button
                                  key={candidate.id}
                                  type="button"
                                  role="option"
                                  aria-selected={selected}
                                  onClick={() => {
                                    updateMemoryBinding(provider.id, {
                                      workerProviderId: candidate.id,
                                      workerModelId: '',
                                    })
                                    setOpenMemoryMenu(null)
                                  }}
                                  className="w-full flex items-center gap-2 text-left px-2 py-2"
                                  style={{
                                    borderRadius: 6,
                                    background: selected ? 'var(--accent-dim)' : 'transparent',
                                    color: selected ? 'var(--text)' : 'var(--text-2)',
                                  }}
                                >
                                  <ProviderLogo providerId={candidate.id} />
                                  <span className="flex-1">{providerDisplayName(candidate, candidate.id)}</span>
                                  {selected && <Check size={13} style={{ color: 'var(--success)' }} aria-hidden />}
                                </button>
                              )
                            })}
                          </ViewportMenu>
                        )}
                      </div>

                      <div className="relative">
                        <button
                          ref={(element) => { if (element) popupAnchorsRef.current.set(modelMenuId, element) }}
                          type="button"
                          title={`${providerDisplayName(provider, provider.id)} memory worker model`}
                          aria-haspopup="listbox"
                          aria-expanded={openMemoryMenu === modelMenuId}
                          onClick={() => setOpenMemoryMenu(openMemoryMenu === modelMenuId ? null : modelMenuId)}
                          className="uam-menu-select__trigger flex w-full items-center justify-between gap-2 rounded-md px-3 py-2 text-left text-sm"
                        >
                          <span className="truncate">{selectedMemoryModelLabel(modelOptions, binding.workerModelId)}</span>
                          <ChevronDown className={openMemoryMenu === modelMenuId ? 'uam-menu-select__chevron is-open' : 'uam-menu-select__chevron'} size={14} aria-hidden />
                        </button>
                        {openMemoryMenu === modelMenuId && (
                          <ViewportMenu
                            anchorRef={{ current: popupAnchorsRef.current.get(modelMenuId) ?? null }}
                            role="listbox"
                            aria-label={`${providerDisplayName(provider, provider.id)} memory worker model`}
                            style={{
                              width: popupAnchorsRef.current.get(modelMenuId)?.getBoundingClientRect().width,
                              border: '1px solid var(--border-bright)',
                              borderRadius: 10,
                              background: 'var(--surface)',
                              boxShadow: 'var(--elev-3)',
                              padding: 6,
                            }}
                          >
                            {modelOptions.map((option) => {
                              const selected = option.id === binding.workerModelId
                              return (
                                <button
                                  key={option.id || 'default'}
                                  type="button"
                                  role="option"
                                  aria-selected={selected}
                                  onClick={() => {
                                    updateMemoryBinding(provider.id, {
                                      ...binding,
                                      workerModelId: option.id,
                                    })
                                    setOpenMemoryMenu(null)
                                  }}
                                  className="w-full grid gap-0.5 text-left px-2 py-2"
                                  style={{
                                    borderRadius: 6,
                                    background: selected ? 'var(--accent-dim)' : 'transparent',
                                    color: selected ? 'var(--text)' : 'var(--text-2)',
                                  }}
                                >
                                  <span className="flex items-center gap-2">
                                    <span className="flex-1">{option.label}</span>
                                    {selected && <Check size={13} style={{ color: 'var(--success)' }} aria-hidden />}
                                  </span>
                                  <span className="text-[11px]" style={{ color: 'var(--text-3)' }}>{option.detail}</span>
                                </button>
                              )
                            })}
                          </ViewportMenu>
                        )}
                      </div>
                    </div>
                    }
                  />
                )
              })}
            </div>
            {memoryLastStatus && <SettingRow title="Last run" description={memoryLastStatus} />}

            {hasWorkerLog && (
              <details className="mt-3 text-xs" style={{ color: 'var(--text-2)' }}>
                <summary className="cursor-pointer" style={{ color: workerLogIsFailure ? 'var(--error)' : 'var(--text)' }}>Last worker log</summary>
                {memoryActivity.lastWorkerStatus && <p className="mt-2 text-[11px]" style={{ color: 'var(--text-3)' }}>{memoryActivity.lastWorkerStatus}</p>}
                {workerLogMeta && <p className="mt-0.5 text-[11px]" style={{ color: 'var(--text-3)' }}>{workerLogMeta}</p>}
                <pre
                  className="mt-2 rounded-lg px-3 py-2 text-[11px] leading-5 overflow-auto"
                  style={{
                    background: 'var(--bg)',
                    border: '1px solid var(--border)',
                    color: 'var(--text-2)',
                    maxHeight: 180,
                    whiteSpace: 'pre-wrap',
                    wordBreak: 'break-word',
                    fontFamily: 'var(--font-mono)',
                  }}
                >
                  {workerLogText || 'No worker output was captured.'}
                </pre>
              </details>
            )}
          </SettingsSection>

        </div>
      )
    }


    if (selectedSection === 'voice-input') return <VoiceInputControls ref={voiceRef} />

    if (selectedSection === 'memory-store') return <MemoryLibraryModal embedded ref={memoryRef}/>
    if (selectedSection === 'markdown-store' && !markdownStoreDirectory) return <div className="mx-auto w-full" style={{maxWidth:800}}>
      <SettingsSection title="Skills folder">
        <div className="grid gap-3 p-3 text-xs" style={{color:'var(--text-2)'}}>
          <p>Skills are Markdown files in one folder.</p>
          <div><Button size="sm" variant="primary" leadingIcon={<FolderOpen size={14} aria-hidden/>} onClick={() => void browseMarkdownStoreDirectory(markdownStoreDraftDirectory).then(selected => { if (selected) void setMarkdownStoreDirectory(selected) })}>Choose folder…</Button></div>
          <div className="flex items-center gap-2">
            <input aria-label="Skills directory" value={markdownStoreDraftDirectory} onChange={event => setMarkdownStoreDraftDirectory(event.target.value)} placeholder="Or type a folder path" className="uam-field min-w-0 flex-1"/>
            <Button size="sm" disabled={!markdownStoreDraftDirectory.trim()} onClick={() => void setMarkdownStoreDirectory(markdownStoreDraftDirectory)}>Use path</Button>
          </div>
        </div>
      </SettingsSection>
    </div>
    if (selectedSection === 'markdown-store') return <div className="h-full min-h-0 flex flex-col gap-3">
      <details className="uam-skill-folder shrink-0 order-last">
        <summary className="uam-skill-folder__summary" style={{fontSize:'var(--fs-xs)',color:'var(--text-3)'}}><ChevronRight size={12} className="uam-skill-folder__chevron" aria-hidden/><span>Enter folder path manually</span></summary>
        <div className="flex items-center gap-2 pt-2 pl-6">
          <input aria-label="Skills directory" value={markdownStoreDraftDirectory} onChange={event => setMarkdownStoreDraftDirectory(event.target.value)} placeholder="Skills directory" className="uam-field min-w-0 flex-1"/>
          <IconButton icon={<FolderOpen size={15}/>} label="Browse for Skills directory" onClick={() => void browseMarkdownStoreDirectory(markdownStoreDraftDirectory).then(selected => { if (selected) setMarkdownStoreDraftDirectory(selected) })}/>
          <IconButton icon={<Save size={15}/>} label="Save Skills directory" disabled={!markdownStoreDraftDirectory.trim() || markdownStoreDraftDirectory.trim() === markdownStoreDirectory} onClick={() => void setMarkdownStoreDirectory(markdownStoreDraftDirectory)}/>
        </div>
      </details>
      <MarkdownStoreModal embedded/>
    </div>

    if (selectedSection === 'goal-loops') {
      return (
        <SettingsSection title="Goal loop safety">
          <SettingRow
            title="Maximum iterations"
            description="Loops a goal may run before it stops (1–200)."
            control={
              <div className="flex w-fit items-center overflow-hidden rounded-lg" style={{ background: 'var(--surface)', border: '1px solid var(--border)' }}>
                <IconButton icon={<Minus size={14} />} label="Decrease maximum goal loop iterations" size="sm" disabled={goalMaxLoopIterations <= 1} onClick={() => void setMemorySettings({ goalMaxLoopIterations: Math.max(1, goalMaxLoopIterations - 1) })} />
                <output aria-label="Maximum goal loop iterations" className="min-w-16 px-3 text-center tabular-nums" style={{ color: 'var(--text)' }}>{goalMaxLoopIterations}</output>
                <IconButton icon={<Plus size={14} />} label="Increase maximum goal loop iterations" size="sm" disabled={goalMaxLoopIterations >= 200} onClick={() => void setMemorySettings({ goalMaxLoopIterations: Math.min(200, goalMaxLoopIterations + 1) })} />
              </div>
            }
          />
          <SettingRow
            title="Provider setup timeout"
            description="Restarts a provider after this long with no setup activity."
            htmlFor="acp-setup-timeout"
            control={
              <MenuSelect
                id="acp-setup-timeout"
                label="ACP setup inactivity timeout"
                value={String(acpSetupInactivityTimeoutSeconds)}
                options={[
                  { value: '60', label: '1 minute' },
                  { value: '300', label: '5 minutes' },
                  { value: '600', label: '10 minutes' },
                  { value: '1200', label: '20 minutes' },
                  { value: '1800', label: '30 minutes' },
                  { value: '3600', label: '1 hour' },
                ]}
                onChange={(value) => void setMemorySettings({ acpSetupInactivityTimeoutSeconds: Number(value) })}
              />
            }
          />
          <SettingRow
            title="Turn output limit"
            description="Largest output per provider turn, excluding startup history."
            control={
              <MenuSelect
                label="Provider turn output ceiling"
                value={String(acpTurnOutputLimitMiB)}
                options={[
                  { value: '256', label: '256 MiB' },
                  { value: '512', label: '512 MiB' },
                  { value: '1024', label: '1 GiB' },
                  { value: '2048', label: '2 GiB' },
                  { value: '4096', label: '4 GiB' },
                ]}
                onChange={(value) => void setMemorySettings({ acpTurnOutputLimitMiB: Number(value) })}
              />
            }
          />
        </SettingsSection>
      )
    }

    if (selectedSection === 'editors') {
      return (
        <div className="grid gap-0">
          <SettingsSection title="Open with">
            <SettingRow
              title="Default editor"
              description="Opens files outside every file type group."
              control={renderEditorPresetMenu(
                'default-editor',
                defaultEditorDraft,
                'Default editor',
                (presetId) => saveEditorSettings(editorAssociationsDraft, presetId)
              )}
            />
          </SettingsSection>

          <SettingsSection title="File types" actions={renderAddEditor()}>
            <div ref={editorMenuRef}>
              {editorAssociationsDraft.map((association) => {
                const groupName = association.name.trim() || 'File group'
                const expanded = expandedEditorGroups[association.id] ?? false
                const panelId = `${association.id}-editor-group-panel`
                return (
                  <SettingRow
                    key={association.id}
                    icon={<IconButton size="sm" icon={<ChevronRight size={14} style={{ transform: expanded ? 'rotate(90deg)' : undefined, transition: 'transform var(--dur-fast) var(--ease)' }} />} label={`${expanded ? 'Hide' : 'Show'} ${groupName} editor group`} aria-expanded={expanded} aria-controls={panelId} onClick={() => toggleEditorGroup(association.id)} />}
                    title={groupName}
                    description={<span className="font-mono">{extensionsText(association) || 'No extensions set'}</span>}
                    control={<div className="flex items-center gap-2">
                      <span className="text-xs" style={{ color: 'var(--text-3)' }}>Opens in {editorPresetLabel(association.editorPresetId)}</span>
                      <IconButton icon={<Trash2 size={14}/>} label={`Delete ${groupName} editor group`} variant="danger" size="sm" disabled={editorSaving || editorAssociationsDraft.length <= 1} onClick={() => setPendingDelete({kind:'editor',id:association.id,name:groupName})}/>
                    </div>}
                  >
                    {expanded && <div id={panelId} className="uam-reveal grid grid-cols-3 gap-2">
                      <label className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
                        Name
                        <input
                          className="uam-field"
                          aria-label={`${groupName} group name`}
                          placeholder="Group name"
                          value={rawEditorNames[association.id] ?? association.name}
                          disabled={editorSaving}
                          aria-invalid={rawEditorNames[association.id] !== undefined && !rawEditorNames[association.id].trim()}
                          onChange={event => { const name = event.currentTarget.value; setRawEditorNames(current => ({...current,[association.id]:name})) }}
                          onBlur={() => commitEditorField(association.id,'name')}
                          onKeyDown={event => { if (event.key === 'Enter') event.currentTarget.blur() }}
                        />
                      </label>
                      <label className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
                        Extensions
                        <input
                          className="uam-field font-mono"
                          aria-label={`${groupName} file extensions`}
                          placeholder=".ts .tsx"
                          value={rawExtensions[association.id] ?? extensionsText(association)}
                          disabled={editorSaving}
                          aria-invalid={rawExtensions[association.id] !== undefined && !parseExtensions(rawExtensions[association.id]).length}
                          onChange={event => { const extensions = event.currentTarget.value; setRawExtensions(current => ({...current,[association.id]:extensions})) }}
                          onBlur={() => commitEditorField(association.id,'extensions')}
                          onKeyDown={event => { if (event.key === 'Enter') event.currentTarget.blur() }}
                        />
                      </label>
                      <div className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
                        Editor
                        {renderEditorPresetMenu(
                          `${association.id}:editor`,
                          association.editorPresetId,
                          `${groupName} editor`,
                          (presetId) => {
                            const nextAssociations = editorAssociationsDraft.map((item) =>
                              item.id === association.id ? { ...item, editorPresetId: presetId } : item
                            )
                            saveEditorSettings(nextAssociations)
                          }
                        )}
                      </div>
                    </div>}
                  </SettingRow>
                )
              })}
              {editorAssociationsDraft.length === 0 && <SettingRow title="No file types" description="Add a group to choose an editor for a set of extensions." />}
            </div>
            {editorError && <p role="alert" className="pt-2 text-xs" style={{color:'var(--error)'}}>{editorError}</p>}
          </SettingsSection>

          <SettingsSection title="Open with">
            <SettingRow
              title="File explorer"
              description={customExplorer ? 'A custom application opens folders.' : 'Your system file manager opens folders.'}
              control={<MenuSelect label="File explorer" disabled={editorSaving} value={customExplorer ? 'custom' : 'system'} options={[{ value: 'system', label: 'System default' }, { value: 'custom', label: 'Custom application' }]} onChange={(value) => { setCustomExplorer(value === 'custom'); if (value === 'system') void saveExplorer('') }} />}
            >
              {customExplorer && <div className="flex gap-2">
                <input aria-label="File explorer application path" value={explorerDraft} onChange={(event) => setExplorerDraft(event.target.value)} className="uam-field min-w-0 flex-1 font-mono" placeholder="/Applications/Path Finder.app" />
                <Button size="sm" onClick={async () => { const response = await sendToCEF<{ selectedPath: string }>({ action: 'browseFolderDirectory', payload: { currentValue: explorerDraft, application: true } }); if (!response.ok) setOpenWithError(response.error || 'Application picker could not be opened.'); else if (response.data?.selectedPath) setExplorerDraft(response.data.selectedPath) }}>Browse</Button>
                <Button size="sm" variant="primary" disabled={!explorerDraft.trim() || explorerDraft.trim() === fileExplorerApplication || editorSaving} onClick={() => void saveExplorer(explorerDraft.trim())}>Save</Button>
              </div>}
            </SettingRow>
            {isWindows && <SettingRow
              title="Terminal shell"
              description={WINDOWS_POWERSHELL_DESCRIPTIONS[windowsPowerShell]}
              control={<MenuSelect label="Terminal shell" disabled={editorSaving} value={windowsPowerShell} options={WINDOWS_POWERSHELL_OPTIONS} onChange={(value) => void saveWindowsPowerShell(value as WindowsPowerShellPreference)} />}
            />}
            {openWithError && <p role="alert" className="pt-2 text-xs" style={{color:'var(--error)'}}>{openWithError}</p>}
          </SettingsSection>
        </div>
      )
    }

    if (selectedSection === 'shell-actions') {
      return <ShellActionsSettings ref={shellRef}/>
    }

    if (selectedSection === 'central-configuration') return <CentralProviderSettings ref={centralRef} />

    if (selectedSection === 'mcp-servers') return <McpServerSetup key={settingsScope} onBusyChange={setMcpSaving} onDirtyChange={setMcpDraftDirty} scopeExecutionHostId={settingsScope} />

    if (selectedSection === 'chat-data') {
      return (
        <div className="grid gap-0">
          <SettingsSection title="Chat bundle">
            <SettingRow
              title="Export all chats"
              description="Writes to a new or empty folder."
              status={chatDataFolder ? <span className="break-all font-mono">Last folder: {chatDataFolder}</span> : undefined}
              control={
                <Button
                  size="sm"
                  leadingIcon={<Download size={14} />}
                  aria-busy={chatDataBusy === 'export' || undefined}
                  disabled={chatDataBusy !== null}
                  onClick={() => void runChatDataAction('export')}
                >
                  Export all chats
                </Button>
              }
            />
            <SettingRow
              title="Import a chat bundle"
              description="Clashing chats are copied with new IDs."
              control={
                <Button
                  size="sm"
                  variant="secondary"
                  leadingIcon={<FolderOpen size={14} />}
                  aria-busy={chatDataBusy === 'import' || undefined}
                  disabled={chatDataBusy !== null}
                  onClick={() => void runChatDataAction('import')}
                >
                  Import chat bundle
                </Button>
              }
            />
          </SettingsSection>
          {chatDataMessage && (
            <div className="pt-1">
              <Notice
                tone={chatDataMessage.tone}
                title="Chat data"
                dismissLabel="Dismiss chat data result"
                onDismiss={() => setChatDataMessage(null)}
              >
                {chatDataMessage.text}
              </Notice>
            </div>
          )}
        </div>
      )
    }

    return (
      <div className="grid gap-0">
        <SettingsSection title="Updates">
          <SettingRow title="Version" control={<span style={{ color: 'var(--text)' }}>{displayedVersion}</span>} />
          <SettingRow
            title="Check for updates automatically"
            control={<Switch hideLabel label="Automatically check for updates" checked={updateChecksEnabled} onChange={(event) => { void setUpdateSettings({ updateChecksEnabled: event.target.checked }) }} />}
          />
        </SettingsSection>
      </div>
    )
  }

  return (
    <div className="uam-settings-motion flex h-full min-h-0 min-w-0" style={{background:'var(--bg)',color:'var(--text)'}}>
      <div ref={dialogRef} role="region" aria-label="Settings" tabIndex={-1} className="w-full h-full min-h-0 flex flex-col overflow-hidden">
        <div className="grid flex-1 min-h-0" style={{gridTemplateColumns:'200px minmax(0,1fr)'}}>
          <aside className="min-h-0 flex flex-col px-3 py-3" style={{borderRight:'1px solid var(--border)',background:'var(--sidebar-bg)'}}>
            <Button size="sm" variant="ghost" className="self-start mb-5" style={{paddingInline:8}} aria-label="Back to chats" leadingIcon={<ArrowLeft size={15} aria-hidden/>} disabled={mcpSaving || editorSaving || remoteBusy || themeSaving} onClick={requestClose}>Back to chats</Button>
            <h1 className="text-lg font-semibold px-2 mb-3">Settings</h1>
            <div className="uam-search-field flex items-center gap-2 px-2 rounded-md mb-5" style={{border:'1px solid var(--border)',background:'var(--surface)'}}>
              <Search size={14} aria-hidden style={{color:'var(--text-3)',flexShrink:0}}/>
              <input type="text" role="searchbox" aria-label="Search settings" placeholder="Search settings" value={settingsSearch} onChange={event => setSettingsSearch(event.target.value)} className="min-w-0 w-full py-2 text-xs" style={{background:'transparent',border:0,color:'var(--text)'}}/>
              {settingsSearch && <IconButton size="sm" icon={<X size={12}/>} label="Clear settings search" onClick={() => setSettingsSearch('')}/>}
            </div>
            <nav ref={navRef} aria-label="Settings pages" className="uam-settings-nav min-h-0 overflow-y-auto flex-1">
              {navIndicator && <span aria-hidden className="uam-settings-nav__indicator" style={{ transform: `translateY(${navIndicator.top}px)`, height: navIndicator.height }} />}
              {visibleSettingsGroups.length === 0 && <p role="status" className="px-2 text-xs" style={{color:'var(--text-3)'}}>No settings found.</p>}
              {visibleSettingsGroups.map(group => <div key={group.label} className="mb-5 last:mb-0">
                <h2 className="px-2 mb-1 text-xs font-medium" style={{color:'var(--text-3)'}}>{group.label}</h2>
                {group.sections.map(id => {
                  const section = SETTINGS_SECTIONS.find(item => item.id === id)!
                  const active = selectedSection === section.id
                  const SectionIcon = section.icon
                  return <button key={section.id} type="button" aria-pressed={active} aria-label={section.label} onClick={() => changeSection(section.id)} className="uam-settings-nav-item" data-active={active}>{section.id.startsWith('provider:') ? <ProviderLogo providerId={section.id.slice('provider:'.length)} size={15}/> : <SectionIcon size={15} className="shrink-0" aria-hidden/>}<span className="text-sm">{section.label}</span></button>
                })}
              </div>)}
            </nav>
          </aside>
          <div className="min-w-0 min-h-0 flex flex-col overflow-hidden">
            {unconfirmedSettingsLabels.length > 0 && <div data-memory-settings-unconfirmed className="shrink-0 px-6 pt-6">
              <Notice tone="warning" title="Unconfirmed settings changes" dismissible={false} dismissLabel="Unconfirmed settings changes cannot be dismissed">
                <p>Displayed choices are unconfirmed drafts and may differ from the running app.</p>
                <ul aria-label="Unconfirmed settings fields" className="list-disc pl-4">
                  {unconfirmedSettingsLabels.map((label) => <li key={label}>{label}</li>)}
                </ul>
              </Notice>
            </div>}
            <div className="w-full min-h-0 flex-1 flex flex-col">
              {themeDraft ? <div className="overflow-y-auto">
                <div className="shrink-0 flex items-center justify-between gap-3 mb-5"><h2 tabIndex={-1} data-theme-step className="text-lg font-semibold">{['Theme name and base','Theme palette','Review theme'][themeStep]}</h2><IconButton icon={<X size={16}/>} label="Close theme editor" disabled={themeSaving} onClick={() => requestThemeExit(() => setThemeDraft(null))}/></div>
                {renderThemeEditor()}
                {themeMessage && <p role="status" className="text-xs mt-3">{themeMessage}</p>}
              </div> : <>
                {(() => {
                  const fullHeight = selectedSection === 'memory-store' || selectedSection === 'markdown-store'
                  const header = <div className="uam-settings-page-header w-full">
                  <div key={selectedSection} className="uam-reveal min-w-0">
                    <h2 className="uam-settings-page-header__title">{selectedSection === 'cli-version' ? 'CLI version control' : SETTINGS_SECTIONS.find(section => section.id === selectedSection)?.label}</h2>
                    {SETTINGS_SECTION_DESCRIPTIONS[selectedSection] && <p className="uam-settings-page-header__description">{SETTINGS_SECTION_DESCRIPTIONS[selectedSection]}</p>}
                  </div>
                  <div className="flex items-center gap-2">
                    <div id="settings-page-actions" className="flex items-center gap-2"/>
                  </div>
                </div>
                  if (fullHeight) return <div className="flex-1 min-h-0 flex flex-col p-6">{header}{scopeBar}<SettingsPageSwap pageKey={selectedSection} className="uam-settings-page flex-1 min-h-0 overflow-hidden mt-5">{renderSectionContent()}</SettingsPageSwap></div>
                  return <div className="uam-settings-scroll"><div className="uam-settings-frame" style={selectedSection === 'phone' ? { margin: '0 auto' } : undefined}>{header}{scopeBar}<SettingsPageSwap pageKey={selectedSection} className="uam-settings-page uam-settings-page--narrow">{renderSectionContent()}</SettingsPageSwap></div></div>
                })()}
              </>}
            </div>
          </div>
        </div>
      </div>
      <Presence open={Boolean(editorExit)}>
        {editorExit && <Overlay open zIndex={80}>
          <div role="alertdialog" aria-modal="true" data-settings-owned-overlay aria-label="Unsaved editor changes" className="w-full max-w-md rounded-xl" style={SETTINGS_DIALOG_STYLE}>
            <div className="px-5 pt-5 text-sm font-semibold" style={{ color: 'var(--text)' }}>Save editor changes?</div>
            <div className="grid gap-3 px-5 pt-1.5 pb-5 text-sm" style={{ color: 'var(--text-2)' }}>
              <p>Your file type changes have not been saved.</p>
              {editorError && <p role="alert" className="text-xs" style={{ color: 'var(--error)' }}>{editorError}</p>}
            </div>
            <div className="flex justify-end gap-2 px-5 pb-5">
              <Button size="sm" disabled={editorSaving} onClick={() => { const next=editorExit; setEditorExit(null); next() }}>Discard</Button>
              <Button size="sm" autoFocus disabled={editorSaving} onClick={() => setEditorExit(null)}>Stay</Button>
              <Button size="sm" variant="primary" disabled={editorSaving} onClick={async () => {
                const next = editorAssociationsDraft.map(item => ({...item,name:rawEditorNames[item.id]?.trim() ?? item.name,extensions:rawExtensions[item.id] === undefined ? item.extensions : parseExtensions(rawExtensions[item.id])}))
                if (await saveEditorSettings(next)) { const leave=editorExit; setEditorExit(null); leave() }
              }}>Save</Button>
            </div>
          </div>
        </Overlay>}
      </Presence>
      <Presence open={Boolean(themeExit)}>
        {themeExit && <Overlay open zIndex={80}>
          <div role="alertdialog" aria-modal="true" data-settings-owned-overlay aria-label="Unsaved theme changes" className="w-full max-w-md rounded-xl" style={SETTINGS_DIALOG_STYLE}>
            <div className="px-5 pt-5 text-sm font-semibold" style={{ color: 'var(--text)' }}>Save theme changes?</div>
            <div className="grid gap-3 px-5 pt-1.5 pb-5 text-sm" style={{ color: 'var(--text-2)' }}>
              <p>Your theme edits have not been saved.</p>
              {themeMessage && <p role="status" className="text-xs">{themeMessage}</p>}
            </div>
            <div className="flex justify-end gap-2 px-5 pb-5">
              <Button size="sm" disabled={themeSaving} onClick={() => { const next=themeExit; setThemeDraft(null); setThemeExit(null); next() }}>Discard</Button>
              <Button size="sm" autoFocus disabled={themeSaving} onClick={() => setThemeExit(null)}>Stay</Button>
              <Button size="sm" variant="primary" disabled={!themeDirty || !themeDraftValid || themeSaving} aria-busy={themeSaving || undefined} onClick={async () => { if (await saveThemeDraft()) { const next=themeExit; setThemeExit(null); next() } }}>Save theme</Button>
            </div>
          </div>
        </Overlay>}
      </Presence>
      {SSH_ENABLED && <Presence open={Boolean(remotePreview)}>
        {remotePreview && <Overlay open zIndex={70} onBackdropMouseDown={() => { if (!remoteBusy) setRemotePreview(null) }}>
          <div role="dialog" aria-modal="true" data-settings-owned-overlay aria-label="Remote helper setup" className="w-full max-w-lg rounded-xl" style={SETTINGS_DIALOG_STYLE}>
            <div className="px-5 pt-5">
              <div className="text-sm font-semibold" style={{ color: 'var(--text)' }}>Install helper on {remotePreview.host.label}?</div>
              <div className="mt-1 text-xs" style={{ color: 'var(--text-3)' }}>SSH alias: <code>{remotePreview.host.sshAlias}</code></div>
            </div>
            <div className="grid gap-4 px-5 pt-4 pb-5 text-sm" style={{ color: 'var(--text-2)' }}>
  			  <p>UAM uses your existing OpenSSH authentication, detects Ubuntu/Linux or Windows, copies the matching helper, verifies its SHA-256 checksum, and starts it. Unsupported systems stop before anything is copied.</p>
  			  <fieldset className="grid gap-2">
  				<legend className="mb-1 text-xs font-medium" style={{ color: 'var(--text)' }}>Install location</legend>
  				<label className="flex cursor-pointer items-start gap-2 rounded-lg p-3" style={{ border: `1px solid ${remoteCustomDirectory ? 'var(--border)' : 'var(--accent)'}` }}>
  				  <input type="radio" name="remote-helper-location" checked={!remoteCustomDirectory} onChange={() => setRemoteCustomDirectory(false)} />
  				  <span><span className="block text-xs font-medium" style={{ color: 'var(--text)' }}>Recommended private location</span><span className="mt-1 block text-[11px]" style={{ color: 'var(--text-3)' }}>Linux: ~/.local/share/uam/runner/{appVersion.replace(/^v/i, '')}<br />Windows: %USERPROFILE%\.uam\runner\{appVersion.replace(/^v/i, '')}</span></span>
  				</label>
  				<div className="rounded-lg p-3" style={{ border: `1px solid ${remoteCustomDirectory ? 'var(--accent)' : 'var(--border)'}` }}>
  				  <label className="flex cursor-pointer items-start gap-2">
  					<input type="radio" name="remote-helper-location" checked={remoteCustomDirectory} onChange={() => setRemoteCustomDirectory(true)} />
  					<span className="block text-xs font-medium" style={{ color: 'var(--text)' }}>Custom folder under the remote home directory</span>
  				  </label>
  				  <div className="ml-6">
  					<input aria-label="Remote helper folder" aria-invalid={remoteCustomDirectory && Boolean(remoteDirectoryValidation)} aria-describedby={remoteDirectoryValidation ? 'remote-helper-folder-help remote-helper-folder-error' : 'remote-helper-folder-help'} value={remoteDirectory} disabled={!remoteCustomDirectory} onChange={(event) => setRemoteDirectory(event.currentTarget.value)} placeholder="uam-helper" spellCheck={false} className="uam-field mt-2 w-full font-mono" style={remoteDirectoryValidation ? { borderColor: 'var(--error)' } : undefined} />
  					<span id="remote-helper-folder-help" className="mt-1 block text-[11px]" style={{ color: 'var(--text-3)' }}>Relative path only. Use letters, numbers, dots, dashes, underscores, and /.</span>
  					{remoteDirectoryValidation && <span id="remote-helper-folder-error" role="alert" className="mt-1 block text-[11px]" style={{ color: 'var(--error)' }}>{remoteDirectoryValidation}</span>}
  					{remoteCustomDirectory && !remoteDirectoryValidation && <span className="mt-1 block break-all font-mono text-[11px]" style={{ color: 'var(--accent)' }}>Linux: ~/{remoteDirectory.trim()}/{appVersion.replace(/^v/i, '')}<br />Windows: %USERPROFILE%\{remoteDirectory.trim().replace(/\//g, '\\')}\{appVersion.replace(/^v/i, '')}</span>}
  				  </div>
  				</div>
  			  </fieldset>
  			  <pre className="max-h-32 overflow-auto whitespace-pre-wrap rounded p-2 text-[11px]" style={{ background: 'var(--bg)', color: 'var(--text-3)', border: '1px solid var(--border)' }}>{remotePreview.preview.split('\n').filter((line) => !line.startsWith('Install UAM runner')).join('\n')}</pre>
            </div>
            <div className="flex justify-end gap-2 px-5 pb-5">
              <Button size="sm" disabled={remoteBusy} onClick={() => setRemotePreview(null)}>Cancel</Button>
              <Button size="sm" variant="primary" aria-busy={remoteBusy || undefined} disabled={remoteBusy || Boolean(remoteDirectoryValidation)} onClick={() => void installRemoteHost()}>Connect and install</Button>
            </div>
          </div>
        </Overlay>}
      </Presence>}
      <Presence open={confirmDiscard}>
        {confirmDiscard && <Overlay open zIndex={70}>
          <div role="alertdialog" aria-modal="true" data-settings-owned-overlay aria-label="Discard unsaved MCP changes" className="w-full max-w-sm rounded-xl" style={SETTINGS_DIALOG_STYLE}>
            <div className="px-5 pt-5 text-sm font-semibold" style={{ color: 'var(--text)' }}>Discard MCP changes?</div>
            <p className="px-5 pt-1.5 pb-5 text-sm" style={{ color: 'var(--text-2)' }}>The MCP server configuration has unsaved changes.</p>
            <div className="flex justify-end gap-2 px-5 pb-5">
              <Button size="sm" onClick={() => { setConfirmDiscard(false); dialogRef.current?.focus() }}>Keep editing</Button>
              <Button size="sm" variant="danger" onClick={() => { setMcpDraftDirty(false); setConfirmDiscard(false); discardExit?.(); setDiscardExit(null) }}>Discard changes</Button>
            </div>
          </div>
        </Overlay>}
      </Presence>
      <Presence open={Boolean(pendingDelete)}>
        {pendingDelete && <Overlay open zIndex={60} onBackdropMouseDown={() => setPendingDelete(null)}>
          <div role="alertdialog" aria-modal="true" data-settings-owned-overlay aria-label={`Delete ${pendingDelete.name}`} className="w-full max-w-md rounded-xl" style={SETTINGS_DIALOG_STYLE}>
            <div className="px-5 pt-5 text-sm font-semibold" style={{ color: 'var(--text)' }}>Delete {pendingDelete.kind === 'theme' ? 'theme' : 'editor group'}?</div>
            <div className="grid gap-3 px-5 pt-1.5 pb-5 text-sm" style={{ color: 'var(--text-2)' }}>
              <p>“{pendingDelete.name}” will be permanently deleted. This cannot be undone or restored.</p>
              {(pendingDelete.kind === 'theme' ? themeMessage : editorError) && <p role="status" className="text-xs">{pendingDelete.kind === 'theme' ? themeMessage : editorError}</p>}
            </div>
            <div className="flex justify-end gap-2 px-5 pb-5">
              <Button size="sm" disabled={themeSaving || editorSaving} onClick={() => setPendingDelete(null)}>Cancel</Button>
              <Button size="sm" variant="danger" disabled={themeSaving || editorSaving} onClick={async () => {
                const deleted = pendingDelete.kind === 'theme' ? await removeSelectedTheme() : await saveEditorSettings(editorAssociationsDraft.filter(item => item.id !== pendingDelete.id))
                if (deleted) setPendingDelete(null)
              }}>Delete permanently</Button>
            </div>
          </div>
        </Overlay>}
      </Presence>
      {SSH_ENABLED && <ConfirmDialog open={pendingHostRemoval !== null} title={`Remove ${pendingHostRemoval?.label ?? 'host'}?`} confirmLabel="Remove host" busy={isRemoteHostBusy(pendingHostRemoval?.id ?? '')}
        onCancel={() => setPendingHostRemoval(null)} onConfirm={() => { const host = pendingHostRemoval; if (host) void removeRemoteHost(host).then(() => setPendingHostRemoval(null)) }}>
        This removes the saved host configuration. Any helper files on that machine are left untouched.
      </ConfirmDialog>}
    </div>
  )
})
