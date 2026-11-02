import { VcsSettingsSection } from './VcsSettingsSection'
import { StorageCleanupSettingsSection } from './StorageCleanupSettingsSection'
import { NativeHistorySettings } from './NativeHistorySettings'
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
import { forwardRef, useEffect, useImperativeHandle, useLayoutEffect, useRef, useState, type ReactNode } from 'react'
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
import { Search, ArrowDown, ArrowLeft, ArrowUp, BookOpen, Brain, Check, ChevronRight, ClipboardList, Copy, Download, GripVertical, Upload, FolderOpen, Info, MemoryStick, MessageSquare, Mic, Minus, MousePointerClick, Palette, Pencil, Plus, RefreshCw, Route, Save, Server, Smartphone, Target, TerminalSquare, Trash2, TriangleAlert, X, type LucideIcon } from 'lucide-react'
import { Button, Card, ConfirmDialog, Disclosure, EXIT_MS, IconButton, MenuItem, MenuSelect, Notice, Overlay, Presence, ProgressFill, RowButton, SegmentedControl, StatusDot, Swap, Switch, ViewportMenu, cx, motionAllowed, scaledMs, Reveal } from '../ui'
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
import { reportAppFailure, reportAppNotice } from '../../utils/appFailure'

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
  { id: 'readOnly', label: 'Read only', detail: 'Recall existing memories without creating new ones.' },
  { id: 'strict', label: 'Strict creation', detail: 'Save only critical, high-confidence memories.' },
  { id: 'balanced', label: 'Balanced creation', detail: 'Save useful medium- and high-confidence memories.' },
  { id: 'open', label: 'Remember everything', detail: 'Save every valid, non-sensitive memory candidate.' },
]

interface StatusMessage { tone: 'success' | 'error' | 'info'; text: string }

/** Status text whose tone is stored with the message, shown with a tick or cross for success and failure. */
function StatusMessage({ message, className = 'text-xs' }: { message: StatusMessage | null; className?: string }) {
  if (!message) return null
  const color = message.tone === 'error' ? 'var(--error)' : message.tone === 'success' ? 'var(--success)' : 'var(--text-2)'
  return <div role="status" className={cx('inline-flex items-center gap-1', className)} style={{ color }}>
    {message.tone === 'success' && <Check size={13} aria-hidden />}
    {message.tone === 'error' && <X size={13} aria-hidden />}
    {message.text}
  </div>
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
  const [exit, setExit] = useState<(() => void) | null>(null)
  const dirty = JSON.stringify(draft) !== JSON.stringify(saved)
  const draftRef = useRef(draft)
  const savedRef = useRef(saved)
  useEffect(() => {
    if (JSON.stringify(draftRef.current) === JSON.stringify(savedRef.current)) { setDraft(saved); draftRef.current = saved }
    savedRef.current = saved
  }, [saved])
  const edit = (next: VoiceInputSettingsRequest) => { draftRef.current = next; setDraft(next) }
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
    setSaving(true)
    try {
      // Server configuration remains a draft until the native save succeeds.
      const payload = draft.mode === 'server' ? draft : { mode: draft.mode }
      if (!await saveSettings(payload)) { reportAppFailure('Could not save voice settings.', 'Try again.'); return }
      const current = useAppStore.getState()
      edit({ mode: current.voiceInputMode ?? draft.mode, serverBaseUrl: current.voiceInputServerBaseUrl ?? '', serverEndpoint: current.voiceInputServerEndpoint ?? '/v1/audio/transcriptions', serverModel: current.voiceInputServerModel ?? 'whisper-1', apiKeyEnv: current.voiceInputApiKeyEnv ?? 'OPENAI_API_KEY' })
      if (next) { setExit(null); next() }
    } catch { reportAppFailure('Could not save voice settings.', 'Try again.') }
    finally { setSaving(false) }
  }
  const voiceModes = [{ id: 'system', label: 'System' }, { id: 'server', label: 'Server' }, { id: 'local', label: 'Local AI' }] as const
  const unavailableVoiceReasons = voiceModes
    .filter((option) => capabilities?.[option.id]?.supported !== true)
    .map((option) => capabilities?.[option.id]?.reason || (capabilities?.[option.id] ? undefined : 'Support has not been reported.'))
    .filter((reason): reason is string => Boolean(reason))
  return <>
    <SettingsSection title="Dictation" actions={<>
        <Button size="sm" disabled={saving || !dirty} onClick={() => edit(saved)}>Discard</Button>
        <SaveButton saving={saving} disabled={saving || !dirty || !supported || Boolean(validationError)} onClick={() => void save()}>Save voice settings</SaveButton>
      </>}>
      <SettingRow title="Transcription service" status={draft.mode === 'server' ? (supported ? 'Recording is sent to your server.' : capabilities?.server?.reason || 'Server support has not been reported.') : 'Start dictation with the microphone button in the message box.'} control={
        <div className="grid justify-items-end gap-1">
          <SegmentedControl aria-label="Transcription service" value={draft.mode} disabled={saving}
            options={voiceModes.map((option) => ({ value: option.id, label: option.label, disabled: capabilities?.[option.id]?.supported !== true }))}
            onChange={(mode) => edit({ ...draft, mode })} />
          {/* Visible reasons: a disabled radio cannot show its own tooltip. */}
          <Reveal open={unavailableVoiceReasons.length > 0}>{unavailableVoiceReasons.map((reason, index) => <p key={index} className="text-xs" style={{ color: 'var(--text-3)' }}>{reason}</p>)}</Reveal>
        </div>
      } />
      <Reveal open={draft.mode === 'server'} className="grid gap-3 p-3">
        {([{ key: 'serverBaseUrl', label: 'Server URL' }, { key: 'serverEndpoint', label: 'Endpoint' }, { key: 'serverModel', label: 'Model' }, { key: 'apiKeyEnv', label: 'API key environment variable' }] as const).map((field) =>
          <SettingsField key={field.key} label={field.label} htmlFor={`voice-${field.key}`}>
            <input id={`voice-${field.key}`} aria-label={field.label} className="uam-field w-full" value={draft[field.key] ?? ''} disabled={saving} autoComplete="off" spellCheck={false} onChange={(event) => edit({ ...draft, [field.key]: event.currentTarget.value })} />
          </SettingsField>)}
      </Reveal>
      <Reveal open={Boolean(validationError)} className="px-3"><Notice tone="error" dismissible={false} dismissLabel="Dismiss">{validationError}</Notice></Reveal>
    </SettingsSection>
    <Presence open={Boolean(exit)}>{exit && <UnsavedChangesDialog title="Unsaved voice settings" busy={saving} error={validationError} onStay={() => setExit(null)}
      onDiscard={() => { const next = exit; edit(saved); setExit(null); next() }} onSave={supported && !validationError ? () => void save(exit) : undefined} />}</Presence>
  </>
})

const ProviderAvailabilityControls = forwardRef<SettingsDraftHandle, { title: string; providers: Provider[]; hostId: string }>(function ProviderAvailabilityControls({ title, providers, hostId }, ref) {
  const overrides = useAppStore((state) => state.providerAvailability)
  const saveSettings = useAppStore((state) => state.setProviderAvailability)
  const saved = Object.fromEntries(providers.map((provider) => [provider.id, overrides.find((entry) => entry.providerId === provider.id && entry.executionHostId === hostId)?.enabled ?? true]))
  const [draft, setDraft] = useState(saved)
  const [saving, setSaving] = useState(false)
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
    setSaving(true)
    try {
      const results = await Promise.all(providers.filter((provider) => draft[provider.id] !== saved[provider.id]).map((provider) => saveSettings({ providerId: provider.id, executionHostId: hostId, enabled: draft[provider.id] })))
      if (results.some((ok) => !ok)) { reportAppFailure('Could not save provider settings.', 'Try again.'); return }
      const currentOverrides = useAppStore.getState().providerAvailability
      setDraft(Object.fromEntries(providers.map((provider) => [provider.id, currentOverrides.find((entry) => entry.providerId === provider.id && entry.executionHostId === hostId)?.enabled ?? true])))
      if (next) { setExit(null); next() }
    } catch { reportAppFailure('Could not save provider settings.', 'Try again.') }
    finally { setSaving(false) }
  }
  return <>
    <SettingsSection title={title} actions={<>
        <Button size="sm" disabled={saving || !dirty} onClick={() => setDraft(saved)}>Discard</Button>
        <SaveButton saving={saving} disabled={saving || !dirty} onClick={() => void save()}>Save provider settings</SaveButton>
      </>}>
      {providers.map((provider) => <SettingRow key={provider.id} title={providerShortName(provider, provider.id)}
        control={<Switch hideLabel label={`${providerShortName(provider, provider.id)} available`} checked={draft[provider.id]} disabled={saving}
          onChange={(event) => { const enabled = event.currentTarget.checked; setDraft((current) => ({ ...current, [provider.id]: enabled })) }} />} />)}
    </SettingsSection>
    <Presence open={Boolean(exit)}>{exit && <UnsavedChangesDialog title="Unsaved provider settings" busy={saving} onStay={() => setExit(null)} onDiscard={() => { const next = exit; setExit(null); next() }} onSave={() => void save(exit)} />}</Presence>
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
  { id: 'aiReview', label: 'AI review', detail: 'An AI reviewer decides. Failures and doubts return to you.' },
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
      <Reveal as="span" open={applied === 'enabled'}><span>Starts automatically</span></Reveal>
      <Reveal as="span" open={applied === 'disabled'}><span>Doesn't start automatically</span></Reveal>
      <Reveal as="span" open={applied === 'pending'}><span>Applying…</span></Reveal>
      <Reveal as="span" open={applied === 'failed'}><span className="inline-flex items-center gap-1" style={{ color: 'var(--error)' }}><X size={12} aria-hidden />{`Couldn't apply${host.runnerStartupError ? `: ${host.runnerStartupError}` : '.'}`}</span></Reveal>
      <Reveal as="span" open={startupMismatch}><span style={{ color: 'var(--text-3)' }}>· Saved setting doesn't match what is applied.</span></Reveal>
    </span>
  ) : `Currently ${!host.startupStatus || host.startupStatus === 'disabled' ? 'off' : host.startupStatus} · starts with your login session`
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
        title="Start helper at login"
        status={startupStatus}
        control={<Switch hideLabel name="startupEnabled" label="Start helper at login" checked={startupEnabled} disabled={busy} onChange={(event) => setStartupEnabled(event.currentTarget.checked)} />}
      />
      <Reveal open={host.platform === 'linux'}>
        <SettingRow
          title="Run CLI updates with sudo"
          description="Needs passwordless sudo for npm. Chats still run as you."
          control={<Switch hideLabel name="cliUpdatesAsAdmin" label="Run CLI updates with sudo" checked={cliUpdatesAsAdmin} disabled={busy} onChange={(event) => setCliUpdatesAsAdmin(event.currentTarget.checked)} />}
        />
      </Reveal>
      <SettingsField label="Host instructions" htmlFor={`remote-host-instructions-${host.id}`} hint={textHint}>
        <textarea id={`remote-host-instructions-${host.id}`} name="instructionText" aria-label={`Host instructions on ${host.label}`} aria-invalid={Boolean(textError) || undefined} value={inlineText ?? ''} onChange={(event) => changeText(event.currentTarget.value)} disabled={busy} rows={5} spellCheck={false} placeholder="Describe how agents should work on this host." className="uam-field w-full" style={{ minHeight: 96, height: 'auto', resize: 'vertical' }} />
        <Reveal open={Boolean(textError)}><span role="alert" className="uam-set-field__hint" style={{ color: 'var(--error)' }}>{textError}</span></Reveal>
        <Reveal open={inlineText !== null}><div><Button size="sm" variant="ghost" type="button" disabled={busy} onClick={() => setInlineText(null)}>Use instruction file instead</Button></div></Reveal>
      </SettingsField>
      <SettingsField label="Instruction file (fallback)" htmlFor={`remote-host-instruction-${host.id}`} hint="Used only while the host instructions above are not set. Empty skips it.">
        <input id={`remote-host-instruction-${host.id}`} name="instructionFile" aria-label={`Instruction file on ${host.label}`} aria-invalid={Boolean(instructionError) || undefined} value={instructionFile} onChange={(event) => setInstructionFile(event.currentTarget.value)} disabled={busy} placeholder={host.platform === 'windows' ? 'C:\\Users\\you\\AGENTS.md' : '/home/you/AGENTS.md'} maxLength={4096} spellCheck={false} className="uam-field w-full font-mono" />
        <Reveal open={Boolean(instructionError)}><span role="alert" className="uam-set-field__hint" style={{ color: 'var(--error)' }}>{instructionError}</span></Reveal>
      </SettingsField>
      <div className="flex items-center justify-end gap-2">
        <Reveal as="span" open={Boolean(textError || instructionError)} className="mr-auto"><span id={`remote-host-save-reason-${host.id}`} className="text-xs" style={{ color: 'var(--text-3)' }}>{saveReason}</span></Reveal>
        <Button size="sm" variant="secondary" type="button" disabled={busy || !instructionFile.trim() || Boolean(instructionError)} onClick={() => onCheck(instructionFile)}>Check file</Button>
        <Button size="sm" variant="primary" type="submit" disabled={busy || !dirty || Boolean(instructionError) || Boolean(textError)} aria-describedby={textError || instructionError ? `remote-host-save-reason-${host.id}` : undefined}>Save host options</Button>
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
  { id: 'defaults', label: 'Chat defaults', icon: MessageSquare },
  { id: 'agents', label: 'Agents', icon: ClipboardList },
  { id: 'provider-summary', label: 'Overview', icon: TerminalSquare },
  ...SUPPORTED_CLI_PROVIDER_IDS.map(id => ({ id: `provider:${id}` as SettingsSectionId, label: providerMetadataForId(id).shortName, icon: TerminalSquare })),
  ...(SSH_ENABLED ? [{ id: 'remote-hosts', label: 'Remote hosts', icon: Server }, { id: 'routing', label: 'SSH routing', icon: Route }] as SettingsSection[] : []),
  ...(MOBILE_COMPANION_ENABLED ? [{ id: 'phone', label: 'Phone', icon: Smartphone }] as SettingsSection[] : []),
  { id: 'voice-input', label: 'Voice input', icon: Mic },
  { id: 'memory-settings', label: 'Memory settings', icon: Brain },
  { id: 'memory-store', label: 'Memory store', icon: MemoryStick },
  { id: 'markdown-store', label: 'Skills', icon: BookOpen },
  { id: 'goal-loops', label: 'Goal loops', icon: Target },
  { id: 'central-configuration', label: 'Central setup', icon: Server },
  { id: 'mcp-servers', label: 'MCP servers', icon: TerminalSquare },
  ...(COMPUTER_USE_ENABLED ? [{ id: 'computer-use', label: 'Computer use', icon: MousePointerClick }] as SettingsSection[] : []),
  { id: 'vcs', label: 'Version control', icon: FolderOpen },
  { id: 'editors', label: 'Editors', icon: Pencil },
  { id: 'shell-actions', label: 'Shell actions', icon: MousePointerClick },
  { id: 'chat-data', label: 'Chat data', icon: Download },
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
  'chat-data': 'chat data native provider history discovery storage import export backup',
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

// ponytail: matches the 120ms leave animation in settings-shell.css; scaled by Motion speed at use.
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
    const timer = window.setTimeout(() => setLeaving(null), scaledMs(PAGE_EXIT_MS))
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
  if (manager.status === 'known-incompatible') return 'Known incompatible. Update before using it in chats.'
  if (manager.status === 'unavailable') return 'Not installed or unavailable on PATH'
  if (manager.status === 'provider-managed') return 'Compatibility is managed by the provider'
  return 'Version has not been checked'
}

/**
 * Keyed list rows: added rows slide in (uam-presence), removed rows hold their last content for the exit length
 * while they slide out, and reordered rows glide to their new place (FLIP). Reduced motion and Motion Off change at once.
 */
export function AnimatedRows<T>({ items, keyOf, render }: { items: T[]; keyOf: (item: T) => string; render: (item: T) => ReactNode }) {
  const [held, setHeld] = useState<{ key: string; item: T; exiting: boolean }[]>(() => items.map((item) => ({ key: keyOf(item), item, exiting: false })))
  const keys = items.map(keyOf)
  const order = keys.join('\n')
  const exitingKeys = held.filter((row) => row.exiting).map((row) => row.key).join('\n')
  const current = new Map(items.map((item) => [keyOf(item), item]))
  const nodes = useRef(new Map<string, HTMLDivElement>())
  const tops = useRef(new Map<string, number>())

  useEffect(() => {
    setHeld((prev) => {
      const animate = motionAllowed()
      const next = prev.map((row) => ({ ...row, exiting: !keys.includes(row.key) })).filter((row) => animate || !row.exiting)
      keys.forEach((key, index) => {
        if (!next.some((row) => row.key === key)) next.splice(index, 0, { key, item: items[index], exiting: false })
      })
      return next
    })
    // ponytail: keys-only dependency; item content is read live from `current` at render.
  }, [order])

  useEffect(() => {
    if (!exitingKeys) return
    const timer = window.setTimeout(() => setHeld((prev) => prev.filter((row) => !row.exiting)), scaledMs(EXIT_MS))
    return () => window.clearTimeout(timer)
  }, [exitingKeys])

  useLayoutEffect(() => {
    const before = tops.current
    const animate = motionAllowed()
    nodes.current.forEach((node, key) => {
      const previous = before.get(key)
      if (previous === undefined || !animate || typeof node.animate !== 'function') return
      const delta = previous - node.offsetTop
      const style = getComputedStyle(node)
      // --dur-base is `calc(180ms * var(--motion-scale, 1))`; the first literal is the base, scaledMs applies the scale.
      const base = /([\d.]+)(ms|s)\b/.exec(style.getPropertyValue('--dur-base'))
      const durMs = base ? parseFloat(base[1]) * (base[2] === 's' ? 1000 : 1) : 180
      if (delta) node.animate([{ transform: `translateY(${delta}px)` }, { transform: 'translateY(0)' }], { duration: scaledMs(durMs), easing: style.getPropertyValue('--ease').trim() || 'ease' })
    })
    tops.current = new Map([...nodes.current].map(([key, node]) => [key, node.offsetTop]))
  })

  return <>{held.map((row) => <div
    key={row.key}
    ref={(node) => { if (node) nodes.current.set(row.key, node); else nodes.current.delete(row.key) }}
    className="uam-presence"
    data-state={row.exiting ? 'closed' : 'open'}
    aria-hidden={row.exiting || undefined}
  >{render(current.get(row.key) ?? row.item)}</div>)}</>
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
  allowKeychainPrompts?: boolean
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
  const saveMemoryPolicy = async (key: string, save: () => Promise<boolean>) => {
    if (memoryPolicySaving[key]) return
    const capturedScope = useAppStore.getState().settingsScopeHostId
    const ownsFeedback = () => useAppStore.getState().settingsScopeHostId === capturedScope
    setMemoryPolicySaving((current) => ({ ...current, [key]: true }))
    try {
      if (!await save() && ownsFeedback()) reportAppFailure('Could not save the memory policy.', 'Try again.')
    } catch { if (ownsFeedback()) reportAppFailure('Could not save the memory policy.', 'Try again.') }
    finally { if (ownsFeedback()) setMemoryPolicySaving((current) => ({ ...current, [key]: false })) }
  }
  const memoryIdleDelaySeconds = useAppStore((s) => s.memoryIdleDelaySeconds)
  const memoryRecallBudgetBytes = useAppStore((s) => s.memoryRecallBudgetBytes)
  const goalMaxLoopIterations = useAppStore((s) => s.goalMaxLoopIterations)
  const goalTokenBudget = useAppStore((s) => s.goalTokenBudget)
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
  const automaticProviderUpdates = useAppStore((s) => s.automaticProviderUpdates)
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
  const [openCliVersionMenu, setOpenCliVersionMenu] = useState<string | null>(null)
  const [pendingCliInstall, setPendingCliInstall] = useState<{ providerId: string; providerName: string; version: string } | null>(null)
  const [pendingDefaultYolo, setPendingDefaultYolo] = useState<{ providerId: string; providerName: string; defaults: ProviderChatDefaults } | null>(null)
  const [expandedEditorGroups, setExpandedEditorGroups] = useState<Record<string, boolean>>({})
  const [editorAssociationsDraft, setEditorAssociationsDraft] = useState(editorFileAssociations)
  const [defaultEditorDraft, setDefaultEditorDraft] = useState(defaultEditorPresetId)
  const [mcpDraftDirty, setMcpDraftDirty] = useState(false)
  const [importFiltersDirty, setImportFiltersDirty] = useState(false)
  const [confirmDiscard, setConfirmDiscard] = useState(false)
  const [mcpSaving, setMcpSaving] = useState(false)
  const [computerUseStatus, setComputerUseStatus] = useState<{ screenRecording: { available: boolean; error: string }; accessibility: { available: boolean; error: string } } | null>(null)
  const [computerUseApps, setComputerUseApps] = useState<Array<{ identityKind: ComputerUseAllowedApplication['identityKind']; identity: string; name: string }>>([])
  const [computerUseBusy, setComputerUseBusy] = useState(false)
  const [computerUseSaveBusy, setComputerUseSaveBusy] = useState(false)
  const [companionSettings, setCompanionSettings] = useState<PhoneAccessStatus | null>(null)
  const [companionBusy, setCompanionBusy] = useState(false)
  const companionInFlight = useRef(false)
  const [companionLoadError, setCompanionLoadError] = useState('')
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
  const [remoteMessage, setRemoteMessage] = useState<StatusMessage | null>(null)
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
  const agentImportRequest = useRef(0)
  const invalidateAgentImport = () => {
    agentImportRequest.current += 1
    setAgentImportPreview(null)
    setAgentImportAcknowledged(false)
    setAgentImportBusy(false)
  }
  useEffect(() => () => { agentImportRequest.current += 1 }, [])
  const [chatDataBusy, setChatDataBusy] = useState<'export' | 'import' | null>(null)
  const [chatDataFolder, setChatDataFolder] = useState('')
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
  // True when leaving also drops unsaved MCP changes; the editor dialog then covers both in one prompt.
  const [editorExitMcp, setEditorExitMcp] = useState(false)
  const editorSavePending = useRef(false)
  const [editorSaving, setEditorSaving] = useState(false)
  const [themeStep, setThemeStep] = useState(0)
  const [themeSaving, setThemeSaving] = useState(false)
  const [themeExit, setThemeExit] = useState<(() => void) | null>(null)
  const [discardExit, setDiscardExit] = useState<(() => void) | null>(null)
  const [pendingHostRemoval, setPendingHostRemoval] = useState<ExecutionHost | null>(null)
  const [themeDraft, setThemeDraft] = useState<CustomTheme | null>(null)
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
    else if ((selectedSection === 'mcp-servers' && mcpDraftDirty) || (selectedSection === 'chat-data' && importFiltersDirty)) { setDiscardExit(() => next); setConfirmDiscard(true) }
    else next()
  }
  const editorDirty = editorAssociationsDraft.some(item => {
    const saved = editorFileAssociations.find(candidate => candidate.id === item.id)
    return (rawEditorNames[item.id] !== undefined && rawEditorNames[item.id].trim() !== saved?.name)
      || (rawExtensions[item.id] !== undefined && JSON.stringify(parseExtensions(rawExtensions[item.id])) !== JSON.stringify(saved?.extensions))
  })
  useEffect(() => {
    if (!themeDirty && !mcpDraftDirty && !editorDirty && !importFiltersDirty) return
    const guard = (event: BeforeUnloadEvent) => { event.preventDefault(); event.returnValue = '' }
    window.addEventListener('beforeunload', guard)
    return () => window.removeEventListener('beforeunload', guard)
  }, [themeDirty, mcpDraftDirty, editorDirty, importFiltersDirty])
  const requestClose = () => {
    if (mcpSaving || editorSavePending.current || remoteBusy) return
    requestThemeExit(() => requestSectionExit(() => {
      // MCP edits were already confirmed by requestSectionExit while on the MCP page.
      const mcpPending = mcpDraftDirty && selectedSection !== 'mcp-servers'
      if (editorDirty) { setEditorExitMcp(mcpPending); setEditorExit(() => () => { setRawEditorNames({}); setRawExtensions({}); setSettingsOpen(false) }); return }
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
      setOpenCliVersionMenu(null)
    }))
  }

  useImperativeHandle(ref, () => ({ requestClose, showMemory: () => changeSection('memory-store') }))

  const runChatDataAction = async (operation: 'export' | 'import') => {
    if (chatDataBusy) return
    setChatDataBusy(operation)
    const response = await sendToCEF<LocalChatBundleResult>({
      action: operation === 'export' ? 'exportLocalChats' : 'importLocalChats',
      payload: { currentValue: chatDataFolder },
    })
    setChatDataBusy(null)
    if (!response.ok || !response.data) {
      reportAppFailure(response.error || `Chat ${operation} failed.`, 'Check the chat data folder and try again.')
      return
    }
    const result = response.data
    if (result.cancelled) {
      reportAppNotice('No folder selected.')
      return
    }
    setChatDataFolder(result.folder)
    const detail = [...result.errors, ...result.warnings][0]
    if (operation === 'export') {
      const text = result.status === 'complete'
        ? `Exported ${result.exportedCount ?? 0} chats to ${result.folder}.`
        : `Exported ${result.exportedCount ?? 0} of ${result.totalCount} chats. ${detail || 'The bundle is incomplete.'}`
      if (result.status === 'complete') reportAppNotice(text, 'success')
      else reportAppFailure(text, 'Check the chat data folder and try again.')
      return
    }
    const text = result.status === 'complete'
      ? `Imported ${result.importedCount ?? 0} chats${result.renamedCount ? `; ${result.renamedCount} were given new IDs` : ''}.`
      : `Imported ${result.importedCount ?? 0} of ${result.totalCount} chats${result.renamedCount ? `; ${result.renamedCount} were given new IDs` : ''}. ${detail || 'Some chats could not be imported.'}`
    if (result.status === 'complete') reportAppNotice(text, 'success')
    else reportAppFailure(text, 'Check the chat data folder and try again.')
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
      reportAppFailure('Enter one exact host alias from ~/.ssh/config.', 'Check the SSH alias and try again.')
      return
    }
    setRemoteHostBusy(host.id, true)
    setRemoteMessage(null)
    setRemoteCustomDirectory(Boolean(host.runnerDirectory))
    setRemoteDirectory(host.runnerDirectory || 'uam-helper')
    const response = await sendToCEF<{ host: ExecutionHost; preview: string }>({ action: 'previewRemoteHost', payload: host })
    setRemoteHostBusy(host.id, false)
    if (!response.ok || !response.data) {
      reportAppFailure(response.error || 'The remote setup preview could not be created.', 'Check the SSH alias and try again.')
      return
    }
    setRemotePreview(response.data)
  }

  const installRemoteHost = async () => {
    if (!remotePreview || isRemoteHostBusy(remotePreview.host.id)) return
    const runnerDirectory = remoteCustomDirectory ? remoteDirectory.trim() : ''
    if (remoteCustomDirectory && runnerDirectoryError(remoteDirectory)) return
    setRemoteHostBusy(remotePreview.host.id, true)
    const response = await sendToCEF({ action: 'installRemoteHost', payload: { ...remotePreview.host, runnerDirectory } })
    setRemoteHostBusy(remotePreview.host.id, false)
    setRemoteMessage(null)
    if (!response.ok) {
      setRemotePreview(null)
      reportAppFailure(response.error || 'Remote helper setup failed.', 'Check the SSH connection and try again.')
      return
    }
    reportAppNotice(`${remotePreview.host.label} is ready.`, 'success')
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
    if (response.ok) reportAppNotice(`${host.label} options saved.`, 'success')
    else reportAppFailure(response.error || 'SSH host options could not be saved.', 'Try again.')
  }

  const checkRemoteHostInstruction = (host: ExecutionHost, instructionFile: string) => {
    if (isRemoteHostBusy(host.id)) return
    setRemoteHostBusy(host.id, true)
    void sendToCEF<{ bytes: number }>({ action: 'checkRemoteHostInstruction', payload: { id: host.id, instructionFile } }).then((response) => {
      setRemoteHostBusy(host.id, false)
      if (response.ok) reportAppNotice(`Readable on ${host.label}: ${response.data?.bytes ?? 0} bytes.`, 'success')
      else reportAppFailure(response.error || 'Host instruction file could not be read.', 'Check the instruction file path and try again.')
    })
  }

  const removeRemoteHost = async (host: ExecutionHost) => {
    if (isRemoteHostBusy(host.id)) return
    setRemoteHostBusy(host.id, true)
    setRemoteMessage(null)
    const response = await sendToCEF({ action: 'removeRemoteHost', payload: { id: host.id } })
    setRemoteHostBusy(host.id, false)
    if (response.ok) reportAppNotice(`${host.label} was removed from UAM. Any helper files on that machine were left untouched.`, 'success')
    else reportAppFailure(response.error || 'Remote host removal failed.', 'Try again.')
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
  }, [isMarkdownStoreOpen, mcpDraftDirty, openCliVersionMenu, openMemoryMenu, remotePreview, remoteBusy, selectedSection, themeDraft, themeDirty, themeSaving, themeExit, pendingDelete, editorSaving, editorExit, editorDirty, confirmDiscard, setSettingsOpen])

  useEffect(() => {
    const handler = (event: MouseEvent) => {
      const target = event.target
      if (!(target instanceof Node)) return
      if (target instanceof Element && target.closest('[data-viewport-menu]')) return
      if (!memoryMenuRef.current?.contains(target)) setOpenMemoryMenu(null)
      if (!cliVersionMenuRef.current?.contains(target)) setOpenCliVersionMenu(null)
    }
    document.addEventListener('mousedown', handler)
    return () => document.removeEventListener('mousedown', handler)
  }, [])

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

  const toggleEditorGroup = (groupId: string) => {
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
    setEditorSaving(true)
    try {
      if (!await setEditorSettings({ defaultEditorPresetId, editorFileAssociations, fileExplorerApplication: path })) reportAppFailure('File explorer could not be saved.', 'Try again.')
    } catch { reportAppFailure('File explorer could not be saved.', 'Try again.') }
    finally { editorSavePending.current = false; setEditorSaving(false) }
  }
  const saveWindowsPowerShell = async (value: WindowsPowerShellPreference) => {
    if (editorSavePending.current) return
    editorSavePending.current = true
    setEditorSaving(true)
    try {
      if (!await setEditorSettings({ defaultEditorPresetId, editorFileAssociations, windowsPowerShell: value })) reportAppFailure('Terminal shell could not be saved.', 'Try again.')
    } catch { reportAppFailure('Terminal shell could not be saved.', 'Try again.') }
    finally { editorSavePending.current = false; setEditorSaving(false) }
  }
  const saveEditorSettings = async (nextAssociations = editorAssociationsDraft, nextDefaultEditor = defaultEditorDraft) => {
    if (editorSavePending.current) return false
    if (nextAssociations.some(item => !item.name.trim() || !item.extensions.length)) {
      reportAppFailure('Each group needs a name and at least one extension.', 'Fix the file type group before saving.')
      return false
    }
    editorSavePending.current = true
    setEditorSaving(true)
    setEditorAssociationsDraft(nextAssociations)
    setDefaultEditorDraft(nextDefaultEditor)
    try {
      const saved = await setEditorSettings({defaultEditorPresetId:nextDefaultEditor,editorFileAssociations:nextAssociations})
      if (!saved) reportAppFailure('Editor settings could not be saved.', 'Try again.')
      return saved
    } catch {
      reportAppFailure('Editor settings could not be saved.', 'Try again.')
      return false
    } finally { editorSavePending.current = false; setEditorSaving(false) }
  }
  const commitEditorField = (id: string, field: 'name' | 'extensions') => {
    const raw = field === 'name' ? rawEditorNames[id] : rawExtensions[id]
    if (raw === undefined) return
    const value = field === 'name' ? raw.trim() : parseExtensions(raw)
    if (!value.length) { reportAppFailure(field === 'name' ? 'Enter a group name.' : 'Enter at least one extension.', 'The change was not saved.'); return }
    void saveEditorSettings(editorAssociationsDraft.map(item => item.id === id ? {...item,[field]:value} : item))
  }

  const renderEditorPresetMenu = (
    value: string,
    label: string,
    onSelect: (presetId: string) => void
  ) => (
    <MenuSelect label={label} value={value} options={EDITOR_PRESETS.map((preset) => ({ value: preset.id, label: preset.label }))} onChange={onSelect} />
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
    const name = source ? `${source.name} copy` : 'Custom theme'
    setThemeDraft({
      version: 1,
      id: customThemeId(name, customThemes),
      name,
      base,
      colors: { ...(source?.colors ?? BUILT_IN_THEME_COLORS[base]) },
    })
    setThemeStep(0)
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
        reportAppFailure('Theme could not be saved.', 'Check its name and colors.')
        return false
      }
      setThemeDraft(null)
      setTheme(saved.id)
      reportAppNotice('Theme saved.', 'success')
      return true
    } catch {
      reportAppFailure('Theme could not be saved.', 'Try again.')
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
      if (deleted) reportAppNotice('Theme deleted.', 'success')
      else reportAppFailure('Theme could not be deleted.', 'Try again.')
      if (deleted) setThemeDraft(null)
      return deleted
    } catch { reportAppFailure('Theme could not be deleted.', 'Try again.'); return false }
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
      reportAppNotice('Theme imported.', 'success')
    } catch {
      reportAppFailure('Theme import failed.', 'Choose a valid UAM theme JSON file.')
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
    reportAppNotice('Theme exported.', 'success')
  }

  const renderAddEditor = () => (
    <Button
      leadingIcon={<Plus size={14} />}
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
    <Card elevation={0} className="grid gap-4 p-3">
      <Swap swapKey={String(themeStep)}>
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
          <SegmentedControl aria-label="Theme base" value={themeDraft.base} disabled={themeSaving}
            options={[{ value: 'dark', label: 'Dark' }, { value: 'light', label: 'Light' }]}
            onChange={(base) => setThemeDraft({ ...themeDraft, base })} />
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
                className="h-5 w-5 rounded-full"
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
      <Reveal open={!themeDraftValid}>
        <Notice tone="error" dismissible={false} dismissLabel="Dismiss"><span id="theme-color-format-error">Every color must use #RRGGBB format.</span></Notice>
      </Reveal>
      {themeStep === 2 && <div className="grid gap-3">
        <div className="text-sm">{themeDraft.name} · {themeDraft.base}</div>
        <div className="grid gap-2 sm:grid-cols-2 lg:grid-cols-3">{THEME_COLOR_FIELDS.map(({key,label}) => <div key={key} className="flex items-center gap-2 text-xs"><span aria-hidden className="w-4 h-4 border" style={{background:themeDraft.colors[key]}}/>{label}</div>)}</div>
      </div>}
      </Swap>
      <div className="flex gap-2">
        <Reveal open={themeStep > 0}><Button size="sm" disabled={themeSaving} onClick={() => setThemeStep(themeStep - 1)}>Back</Button></Reveal>
        <Swap swapKey={themeStep < 2 ? 'next' : 'save'}>{themeStep < 2 ? <Button size="sm" variant="primary" disabled={themeStep === 0 ? !themeDraft.name.trim() : !themeDraftValid} onClick={() => setThemeStep(themeStep + 1)}>Next</Button> : <SaveButton saving={themeSaving} disabled={!themeDirty || !themeDraftValid || themeSaving} onClick={() => void saveThemeDraft()}>Save theme</SaveButton>}</Swap>
        <Button size="sm" disabled={themeSaving} onClick={() => requestThemeExit(() => setThemeDraft(null))}>Cancel</Button>
      </div>
    </Card>
  ) : null

  const checkComputerUsePermissions = async () => {
    setComputerUseBusy(true)
    const response = await sendToCEF<{ screenRecording?: { available?: boolean; error?: string }; accessibility?: { available?: boolean; error?: string } }>({ action: 'checkComputerUsePermissions' })
    setComputerUseBusy(false)
    if (!response.ok) { reportAppFailure(response.error || `Could not check ${computerUseOsLabel} permissions.`, 'Try again.'); return }
    setComputerUseStatus({
      screenRecording: { available: Boolean(response.data?.screenRecording?.available), error: response.data?.screenRecording?.error || '' },
      accessibility: { available: Boolean(response.data?.accessibility?.available), error: response.data?.accessibility?.error || '' },
    })
  }
  const requestComputerUsePermission = async (permission: 'screenRecording' | 'accessibility') => {
    setComputerUseBusy(true)
    const response = await sendToCEF<{ granted?: boolean; openSettingsRequired?: boolean }>({ action: 'requestComputerUsePermission', payload: { permission } })
    if (!response.ok) {
      setComputerUseBusy(false)
      reportAppFailure(response.error || 'Could not request this permission.', 'Try again.')
      return
    }
    if (response.data?.granted) {
      setComputerUseBusy(false)
      reportAppNotice(`${permission === 'screenRecording' ? 'Screen Recording' : 'Accessibility'} permission is already granted.`, 'success')
      return
    }
    const settingsResponse = await sendToCEF<{ opened?: boolean; error?: string }>({ action: 'openComputerUseSystemSettings', payload: { permission } })
    setComputerUseBusy(false)
    if (settingsResponse.ok && settingsResponse.data?.opened !== false) reportAppNotice(`Permission is not granted. Opened ${computerUseOsLabel} settings; enable it, then re-check.`)
    else reportAppFailure(settingsResponse.error || settingsResponse.data?.error || `Permission is not granted. Could not open ${computerUseOsLabel} settings.`, 'Open the settings manually, then re-check.')
  }
  const openComputerUseSystemSettings = async (permission: 'screenRecording' | 'accessibility') => {
    const response = await sendToCEF({ action: 'openComputerUseSystemSettings', payload: { permission } })
    if (!response.ok) reportAppFailure(response.error || `Could not open ${computerUseOsLabel} settings.`, 'Try again.')
  }
  const loadComputerUseApplications = async () => {
    const response = await sendToCEF<{ applications?: Array<{ identityKind?: string; identity?: string; name?: string }> }>({ action: 'listComputerUseApplications' })
    if (!response.ok) { reportAppFailure(response.error || 'Could not list applications.', 'Try again.'); return }
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
  const changeCompanion = async (action: 'setCompanionEnabled' | 'setCompanionKeychainPrompts' | 'restartCompanion' | 'repairCompanionLanAddress', payload: Record<string, unknown> = {}) => {
    if (companionInFlight.current) return
    companionInFlight.current = true
    setCompanionBusy(true)
    setCompanionBusyAction(action === 'repairCompanionLanAddress' ? `${action}:${payload.address}` : action)
    try {
      const response = await sendToCEF<PhoneAccessStatus>({ action, payload })
      if (!response.ok || !response.data) { reportAppFailure(response.error || 'Could not update phone access.', 'Try again.'); return }
      adoptCompanionStatus(response.data)
    } catch { reportAppFailure('Could not update phone access.', 'Try again.') }
    finally { companionInFlight.current = false; setCompanionBusy(false); setCompanionBusyAction('') }
  }
  const setCompanionEnabled = (enabled: boolean) => changeCompanion('setCompanionEnabled', { enabled })
  const restartApplication = async () => {
    if (companionInFlight.current) return
    companionInFlight.current = true
    setCompanionBusy(true)
    setCompanionBusyAction('restartApplication')
    try {
      const response = await sendToCEF({ action: 'restartApplication' })
      if (!response.ok) reportAppFailure(response.error || 'Could not restart Universal Agent Manager.', 'Close and open it again.')
      else reportAppNotice('Restarting Universal Agent Manager…')
    } catch { reportAppFailure('Could not restart Universal Agent Manager.', 'Close and open it again.') }
    finally { companionInFlight.current = false; setCompanionBusy(false); setCompanionBusyAction('') }
  }
  const copyCompanion = async (kind: 'token' | 'url') => {
    if (companionInFlight.current) return
    companionInFlight.current = true
    setCompanionBusy(true)
    setCompanionBusyAction(`copy:${kind}`)
    let ok = false
    try {
      let text = companionSettings?.effectiveUrl ?? ''
      if (kind === 'token') {
        const response = await sendToCEF<{ token?: string }>({ action: 'getCompanionToken' })
        if (!response.ok || !response.data?.token) {
          reportAppFailure(response.error || 'Phone access token is unavailable.', 'Try again.')
          return
        }
        text = response.data.token
      }
      // CEF blocks navigator.clipboard here; route through the native bridge helper.
      ok = await copyTextToClipboard(text, document)
      if (!ok) reportAppFailure(`Could not copy the phone access ${kind}.`, 'Try again.')
    } catch { reportAppFailure(`Could not copy the phone access ${kind}.`, 'Try again.') }
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
        reportAppFailure(response.error || 'Could not save Computer Use settings.', 'Try again.')
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
    // Load failures persist until a retry succeeds; action results are reported as app events.
    const phoneFeedback = <Reveal open={Boolean(companionLoadError)}><Notice tone="error" dismissible={false} dismissLabel="Dismiss">{companionLoadError}</Notice></Reveal>
    return <div className="grid gap-0">
      <div className="uam-set-card">
        <SettingRow
          title="Enable phone access"
          description={companionSettings?.configured ? 'Connect your phone to this computer. Changes apply automatically.' : 'Not configured on this computer.'}
          control={<Switch hideLabel label="Enable phone access" checked={Boolean(companionSettings?.enabled)}
            disabled={!companionSettings?.configured || companionBusy} onChange={(event) => void setCompanionEnabled(event.target.checked)} />}
        />
        <SettingRow
          title="Connection"
          description={companionLoadError ? undefined : repairAddresses.length > 0 ? 'Choose a current network address below to repair the connection.' : companionSettings?.reason?.replace(/\bLAN\b/g, 'network')}
          status={!companionSettings?.enabled && !failed && !companionLoadError && !companionSettings?.restartRequired && companionBusyAction !== 'setCompanionEnabled' && companionSettings ? undefined : <span role="status" aria-live="polite" className="inline-flex items-center gap-1" style={{ color: failed || companionLoadError ? 'var(--error)' : 'var(--text-2)' }}>{failed || companionLoadError ? <X size={13} aria-hidden /> : null}{companionSettings?.restartRequired ? 'Restart Universal Agent Manager to apply this change.' : companionBusyAction === 'setCompanionEnabled' ? 'Updating…' : companionLoadError ? 'Needs attention' : !companionSettings ? 'Checking…' : ready ? 'Ready to connect' : companionSettings.state === 'starting' ? 'Updating…' : failed ? 'Needs attention' : 'Off'}</span>}
          control={<div className="flex flex-wrap gap-2">
            <Reveal open={Boolean(failed || companionLoadError)}><Button size="sm" variant="secondary" disabled={companionBusy} onClick={() => { if (failed) void changeCompanion('restartCompanion'); else void loadCompanionSettings(true) }}>{['restartCompanion', 'reloadCompanion'].includes(companionBusyAction) ? 'Retrying…' : 'Retry phone access'}</Button></Reveal>
            <Reveal open={Boolean(failed || companionSettings?.restartRequired)}><Button size="sm" variant="secondary" disabled={companionBusy} onClick={() => void restartApplication()}>{companionBusyAction === 'restartApplication' ? 'Restarting Universal Agent Manager…' : 'Restart Universal Agent Manager'}</Button></Reveal>
          </div>}
        >{phoneFeedback}</SettingRow>
        <Reveal open={repairAddresses.length > 0}>{repairAddresses.length > 0 && <SettingRow title="Network address changed" description={`${configuredHost} is no longer on this computer.`}
          control={<div className="flex flex-wrap gap-2">{repairAddresses.map(localAddress => <Button key={localAddress} size="sm" variant="secondary" disabled={companionBusy} onClick={() => void changeCompanion('repairCompanionLanAddress', { address: localAddress })}>{companionBusyAction === `repairCompanionLanAddress:${localAddress}` ? 'Repairing…' : `Use ${localAddress}`}</Button>)}</div>} />}</Reveal>
        <Reveal open={Boolean(address)}><SettingRow title={ready ? 'Phone address' : 'Configured phone address'} description={!ready ? 'Available when phone access is ready.' : undefined}
          control={<Button size="sm" variant="ghost" leadingIcon={copyIcon('url')} aria-label="Copy phone URL" disabled={!ready || companionBusy} onClick={() => void copyCompanion('url')}>{copyLabel('url', 'Copy URL')}</Button>}>
          <code className="block truncate font-mono text-xs" title={address} style={{ color: 'var(--text-2)' }}>{ready ? companionSettings?.effectiveUrl : address}</code>
        </SettingRow></Reveal>
        <Reveal open={typeof companionSettings?.allowKeychainPrompts === 'boolean'}><SettingRow
          title="Allow Keychain prompts from phone"
          description="When on, phone refreshes can show the macOS Keychain dialog on this computer. When off, they never do."
          status={companionBusyAction === 'setCompanionKeychainPrompts' ? <span role="status" aria-live="polite" style={{ color: 'var(--text-2)' }}>Saving…</span> : undefined}
          control={<Switch hideLabel label="Allow Keychain prompts from phone" checked={companionSettings?.allowKeychainPrompts === true}
            disabled={!companionSettings?.configured || companionBusy} onChange={(event) => void changeCompanion('setCompanionKeychainPrompts', { allowed: event.target.checked })} />}
        /></Reveal>
        <SettingRow title="Access token" description="For manual entry in the iPhone app. QR pairing also sets up certificate trust."
          control={<Button size="sm" variant="secondary" leadingIcon={copyIcon('token')} aria-label="Copy token" disabled={!companionSettings?.configured || companionBusy} onClick={() => void copyCompanion('token')}>{copyLabel('token', 'Copy token')}</Button>} />
        <Reveal open={Boolean(address)}><PhonePairingPanel url={companionSettings?.effectiveUrl || address} enabled={Boolean(ready)} /></Reveal>
      </div>
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
          title="Computer use backend"
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
                  void setSessionComputerUseBackend(activeSession.id, value as ComputerUseBackend).then((result) => {
                    if (!result.ok) reportAppFailure(result.error || 'Could not change the Computer Use backend.', 'Try again.')
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
            : 'Available only for supported local chats.'}
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
                void setSessionUamControlEnabled(activeSession.id, event.target.checked)
                  .then((result) => {
                    if (!result) reportAppFailure('Could not change UAM Control.', 'Stop the active session first, then try again.')
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
            {state ? (state.available ? <Check size={13} aria-hidden /> : <X size={13} aria-hidden />) : <StatusDot tone="neutral" />}
            {state ? (state.available ? 'Available' : state.error || 'Not available') : 'Not checked'}
          </span>}
          control={<div className="flex flex-wrap justify-end gap-1">
            <Button size="sm" variant="ghost" disabled={computerUseBusy} onClick={() => void openComputerUseSystemSettings(permission)}>Open settings</Button>
            <Button size="sm" variant="secondary" disabled={computerUseBusy} onClick={() => void requestComputerUsePermission(permission)}>{state?.available ? 'Re-request' : 'Request'} {label}</Button>
          </div>}
        />)}
      </SettingsSection>
      <SettingsSection
        title="Allowed targets"
        actions={<Button size="sm" variant="secondary" onClick={() => void loadComputerUseApplications()}>Refresh applications</Button>}
      >
        <SettingRow
          title="Only listed apps"
          description={computerUseAllowlistEnabled ? 'Chats can target only the apps checked below.' :'Each chat asks before targeting an app.'}
          control={<Switch hideLabel label="Restrict Computer Use to allowed applications" checked={computerUseAllowlistEnabled} disabled={computerUseSaveBusy} onChange={(event) => void saveComputerUseSettings(event.target.checked, computerUseAllowedApplications)} />}
        />
        <Reveal open={computerUseApps.length === 0}><SettingRow title={<span style={{ color: 'var(--text-3)' }}>Refresh to list apps UAM can target.</span>} /></Reveal>
        <AnimatedRows items={computerUseApps} keyOf={(app) => `${app.identityKind}:${app.identity}`} render={(app) => {
          const allowed = computerUseAllowedApplications.some((item) => item.identityKind === app.identityKind && item.identity === app.identity)
          return <SettingRow
            title={<span title={app.identity}>{app.name}</span>}
            control={<Switch hideLabel label={app.name} checked={allowed} disabled={computerUseSaveBusy} onChange={() => void saveComputerUseSettings(computerUseAllowlistEnabled, allowed ? computerUseAllowedApplications.filter((item) => item.identity !== app.identity || item.identityKind !== app.identityKind) : [...computerUseAllowedApplications, { identityKind: app.identityKind, identity: app.identity }])} />}
          />
        }} />
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
  const scopeBar = <Reveal open={scopeBarVisible}><SettingsScopeBar hosts={executionHosts} value={scopeValue} selectorEnabled={onProviderPage || hostScopedSettings} supported={hostScopedSettings} onChange={(hostId) => { if (!mcpSaving) requestSectionExit(() => setSettingsScopeHostId(hostId)) }} /></Reveal>
  // A pending CLI install or open version menu belongs to the computer it was started for.
  useEffect(() => {
    setPendingCliInstall(null)
    setOpenCliVersionMenu(null)
    setMemoryPolicySaving({})
  }, [settingsScopeHostId])
  const renderSectionContent = (sectionOverride?: SettingsSectionId, scopedProviderId?: string): ReactNode => {
    const selectedSection = sectionOverride ?? selectedSettingsSection
    const availability = (title: string, ids?: string[]) => <ProviderAvailabilityControls key={`${effectiveProviderSettingsHostId}:${ids?.join(',') || 'all'}`} ref={providerAvailabilityRef} title={title} providers={providers.filter((provider) => !ids || ids.includes(provider.id))} hostId={effectiveProviderSettingsHostId} />
    if (selectedSection === 'provider-summary') return <div className="grid">{availability('Provider availability')}<ProviderConnections selectedHostId={effectiveProviderSettingsHostId} onProviderSelect={id => changeSection(`provider:${id}`)} onMcpSelect={() => changeSection('mcp-servers')} onPreferencesSelect={() => changeSection('central-configuration')} /></div>
    if (selectedSection.startsWith('provider:')) {
      const providerId = selectedSection.slice('provider:'.length)
      return <div className="grid">{availability('Availability', [providerId])}{renderSectionContent('cli-version', providerId)}{renderSectionContent('defaults', providerId)}<ProviderConnections providerId={providerId} selectedHostId={effectiveProviderSettingsHostId} onMcpSelect={() => changeSection('mcp-servers')} onPreferencesSelect={() => changeSection('central-configuration')} /></div>
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
                  // ui: raw button kept — role=radio theme tile with a swatch preview; no primitive has that shape.
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
                  <IconButton icon={<Pencil size={15}/>} label={selectedCustomTheme ? 'Edit theme' : 'Clone built-in theme to edit'} onClick={() => { if (selectedCustomTheme) { setThemeDraft({...selectedCustomTheme,colors:{...selectedCustomTheme.colors}}); setThemeStep(0) } else cloneCurrentTheme() }}/>
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
            />
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
    if (selectedSection === 'storage-cleanup') return <div className="grid gap-0"><DataFolderSettingsSection ref={dataFolderRef} /><StorageCleanupSettingsSection ref={cleanupRef} /></div>

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
        <div className="grid gap-0">
          {!scopedProviderId && <div className="uam-set-card">
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
            <Reveal open={settingsScope === SETTINGS_ALL_ENVIRONMENTS}>
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
            </Reveal>
          </div>}
          {scopedProviderId && <div>
            <div className="grid gap-0">
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
                  const refreshFailed = Boolean(modelRefreshError && !modelsLoading)
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
                  const modeOptions = [
                    { id: 'default', label: 'Default', detail: 'Use the provider default mode' },
                    { id: 'plan', label: 'Plan', detail: 'Ask the provider to plan first' },
                  ]
                  return (
                    <SettingsSection
                      key={provider.id}
                      title="Defaults"
                      actions={provider.supportsStructured === false ? undefined : <IconButton icon={<RefreshCw size={14}/>} label={`Refresh ${providerName} models`} disabled={!providerWorkspace || modelsLoading} onClick={() => void (effectiveProviderSettingsHostId === 'local' ? discoverProviderModels(providerSession?.id ?? '', provider.id, providerWorkspace) : discoverProviderModels(providerSession?.id ?? '', provider.id, providerWorkspace, effectiveProviderSettingsHostId))} />}
                    >
                      <div id={`${provider.id}-defaults-panel`} className="grid gap-0 text-xs" style={{ color: 'var(--text-2)' }}>
                        <Reveal open={provider.supportsStructured === false}><SettingRow title="Terminal only. Models are managed in the CLI." /></Reveal>
                        <Reveal open={!modelOptions.some(option => option.id) && provider.supportsStructured !== false}><SettingRow title="Model visibility" description="Appears once models are discovered." /></Reveal>
                        <Reveal open={modelOptions.some((option) => option.id)}>
                          <Disclosure summary={<span className="flex min-w-0 flex-1 items-center justify-between gap-2"><span>Model visibility</span><span style={{ color: 'var(--text-3)' }}>{(defaults.hiddenProviderIds?.length ?? 0) + (defaults.hiddenModelIds?.length ?? 0) ? [visibilityGroups.length && defaults.hiddenProviderIds?.length ? `${defaults.hiddenProviderIds.length} providers` : '', defaults.hiddenModelIds?.length ? `${defaults.hiddenModelIds.length} models` : ''].filter(Boolean).join(', ') + ' hidden' : 'All shown'}</span></span>}>
                            <div className="grid gap-1">
                              {!visibilityGroups.length && modelOptions.filter((option) => option.id).map((option) => (
                                <div key={option.id} className="flex items-center justify-between gap-2 py-1.5" style={{ borderTop: '1px solid var(--border)' }}>
                                  <span className="truncate" style={{ color: defaults.hiddenModelIds?.includes(option.id) ? 'var(--text-3)' : 'var(--text)' }}>{option.label}</span>
                                  <Switch hideLabel label={option.label} checked={!defaults.hiddenModelIds?.includes(option.id)} onChange={() => toggleVisibility('hiddenModelIds', option.id)} />
                                </div>
                              ))}
                              {visibilityGroups.map((group) => {
                                const groupModels = modelOptions.filter((option) => option.id && option.id.split('/')[0] === group)
                                const groupShown = !defaults.hiddenProviderIds?.includes(group)
                                const shownCount = groupModels.filter((option) => !defaults.hiddenModelIds?.includes(option.id)).length
                                return (
                                  // ui: the group switch cannot sit inside the disclosure button, so it is a sibling of the Disclosure.
                                  <div key={group} className="flex items-center gap-2 py-1.5" style={{ borderTop: '1px solid var(--border)' }}>
                                    <Switch hideLabel label={`Show ${group}`} checked={groupShown} onChange={() => toggleVisibility('hiddenProviderIds', group)} />
                                    <Disclosure className="min-w-0 flex-1" summary={<span className="flex min-w-0 flex-1 items-center gap-2"><span className="min-w-0 flex-1 truncate font-medium" style={{ color: groupShown ? 'var(--text)' : 'var(--text-3)' }}>{group}</span><span style={{ color: 'var(--text-3)' }}>{groupShown ? `${shownCount}/${groupModels.length}` : 'hidden'}</span></span>}>
                                      <div className="grid gap-0.5 pb-1.5 pl-6 pr-2" style={{ opacity: groupShown ? 1 : 0.5 }}>
                                        {groupModels.map((option) => (
                                          <div key={option.id} className="flex items-center justify-between gap-2 py-1">
                                            <span className="truncate">{option.label}</span>
                                            <Switch hideLabel label={option.label} disabled={!groupShown} checked={!defaults.hiddenModelIds?.includes(option.id)} onChange={() => toggleVisibility('hiddenModelIds', option.id)} />
                                          </div>
                                        ))}
                                      </div>
                                    </Disclosure>
                                  </div>
                                )
                              })}
                              <div className="px-1 pt-1" style={{ color: 'var(--text-3)' }}>Hidden models stay available in existing chats.</div>
                            </div>
                          </Disclosure>
                        </Reveal>
                        <SettingRow title="Model"control={renderDefaultsMenu(
                          `${provider.id}:model`,
                          defaults.modelId,
                          `${providerName} default model`,
                          modelOptions,
                          (modelId) => updateProviderDefaults(provider.id, {
                            ...defaults,
                            modelId,
                            reasoningEffort: reasoningEffortForModel(providerAcp, modelId, defaults.reasoningEffort),
                          })
                        )} />
                        <Reveal open={reasoningOptions.length > 0}><SettingRow title="Reasoning" control={renderDefaultsMenu(
                          `${provider.id}:reasoning`,
                          defaults.reasoningEffort,
                          `${providerName} default reasoning`,
                          reasoningOptions,
                          (reasoningEffort) => updateProviderDefaults(provider.id, { ...defaults, reasoningEffort })
                        )} /></Reveal>
                        <Reveal open={caps.hasServiceTier && speedOptions.length > 0}><SettingRow title="Speed" control={renderDefaultsMenu(
                          `${provider.id}:speed`,
                          defaults.serviceTier,
                          `${providerName} default speed`,
                          speedOptions,
                          (serviceTier) => updateProviderDefaults(provider.id, { ...defaults, serviceTier })
                        )} /></Reveal>
                        <SettingRow title="Provider mode" control={renderDefaultsMenu(
                          `${provider.id}:mode`,
                          defaults.approvalMode,
                          `${providerName} default provider mode`,
                          modeOptions,
                          (approvalMode) => updateProviderDefaults(provider.id, { ...defaults, approvalMode })
                        )} />
                        <SettingRow title="Permissions" control={caps.structuredPermissionControl === 'uam'
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
                          : <span style={{ color: 'var(--text-3)' }}>Provider managed</span>} />
                        <SettingRow title="Memory" control={<MenuSelect label={`${providerName} default memory level`} value={defaults.memoryLevel ?? 'default'} disabled={memoryPolicySaving[provider.id]}
                          options={MEMORY_POLICY_CHOICES.map((option) => ({ value: option.id, label: option.label, description: option.id === 'default' ? 'Use the current app default.' : option.detail }))}
                          onChange={(memoryLevel) => void saveMemoryPolicy(provider.id, () => setProviderChatDefaults({ providerChatDefaults: {
                            [provider.id]: { ...defaults, memoryLevel: memoryLevel as MemoryLevel,
                              memoryEnabled: memoryLevel === 'default' ? memoryEnabledDefault : memoryLevel !== 'off' },
                          } }, settingsScope))} />} />
                        <Disclosure summary="Advanced">
                          <div className="grid gap-3">
                            <SettingRow title="Feature preference" control={renderDefaultsMenu(
                              `${provider.id}:feature-preference`,
                              defaults.featurePreference ?? 'uam',
                              `${providerName} feature preference`,
                              [
                                { id: 'uam', label: 'Prefer UAM', detail: 'Use UAM agents, reviewed goals, and approvals.' },
                                { id: 'provider', label: `Prefer ${providerName}`, detail: 'Use provider-native features when available.' },
                              ],
                              (featurePreference) => updateProviderDefaults(provider.id, { ...defaults, featurePreference: featurePreference === 'provider' ? 'provider' : 'uam' })
                            )} />
                            <SettingRow title="Goal reviewer model" description="Used for architecture and read-only review." control={renderDefaultsMenu(
                              `${provider.id}:goal-reviewer`,
                              defaults.reviewerModelId || defaults.modelId,
                              `${providerName} goal reviewer model`,
                              reviewerModelOptions,
                              (reviewerModelId) => updateProviderDefaults(provider.id, { ...defaults, reviewerModelId })
                            )} />
                            <SettingRow
                              title="Architect + worker"
                              description="Reviewer plans and checks each step; the model implements it."
                              control={<Switch hideLabel label="Architect + worker" checked={Boolean(defaults.smallModelMode)} onChange={() => updateProviderDefaults(provider.id, { ...defaults, smallModelMode: !defaults.smallModelMode })} />}
                            />
                          </div>
                        </Disclosure>
                        <Reveal open={refreshFailed}>
                          <Notice
                            key={`${provider.id}:${providerWorkspace}:${modelRefreshError}`}
                            tone="error"
                            title="Model refresh failed"
                            dismissLabel={`Dismiss ${providerName} model refresh error`}
                          >
                            {modelRefreshError}
                          </Notice>
                        </Reveal>
                        <Reveal open={!refreshFailed}>
                          <SettingRow title="Models" status={
                            <span role="status" className="text-xs" style={{ color: 'var(--text-3)' }}>
                              <ProgressFill label={`Refreshing ${providerName} models`} indeterminate active={modelsLoading}>{modelsLoading ? 'Refreshing models…' : !providerWorkspace ? 'Add a workspace to refresh models' : hasDiscoveredModels ? <><Check size={13} aria-hidden style={{ color: 'var(--success)', verticalAlign: '-2px', marginRight: 4 }} />Model catalog available</> : 'Using provider defaults; refresh to discover models'}</ProgressFill>
                            </span>
                          } />
                        </Reveal>
                      </div>
                    </SettingsSection>
                  )
                })}
            </div>
          </div>}
        </div>
      )
    }

    if (MOBILE_COMPANION_ENABLED && selectedSection === 'phone') return <div className="grid gap-0">{renderPhoneAccess()}</div>

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
        const preview = await previewProviderAgentImport(agentImportProvider, agentImportPath.trim())
        if (requestId !== agentImportRequest.current) return
        setAgentImportPreview(preview)
        setAgentImportId(preview?.suggestedId ?? '')
        setAgentImportAcknowledged(false)
        if (!preview) reportAppFailure('Preview failed.', agentImportPath.trim())
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
        if (imported) reportAppNotice(`Imported ${agentImportId.trim()}.`, 'success')
        else reportAppFailure('Import failed.', 'The source or target may have changed; preview it again.')
        if (imported) setAgentImportPreview(null)
        setAgentImportBusy(false)
      }
      return (
        <div className="grid gap-0">
          <SettingsSection
            title="Agent cycle"
            actions={
              <>
                {selectableCustomAgents.length > 0 && <div className="w-[220px] max-w-full">
                  <MenuSelect
                    label="Favourite agent"
                    value={selectedCandidate}
                    options={selectableCustomAgents.map((agent) => ({ value: agent.id, label: agent.id, description: agent.description }))}
                    onChange={setFavoriteAgentCandidate}
                  />
                </div>}
                <Button
                  size="sm"
                  variant="secondary"
                  leadingIcon={<Plus size={14} />}
                  disabled={!selectedCandidate}
                  disabledReason={selectedCandidate ? undefined : activeSessionId ? 'All custom agents are already favourites' : 'Open a chat to list workspace agents'}
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
              description={favoriteUamAgentIds.length === 0 ? 'Moves to the next agent: Build, then Plan.' : 'Moves to the next agent in the composer.'}
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
            <AnimatedRows items={favoriteUamAgentIds} keyOf={(agentId) => agentId} render={(agentId) => {
              const index = favoriteUamAgentIds.indexOf(agentId)
              const available = activeUamAgents.some((agent) => agent.id === agentId)
              return (
                <SettingRow
                  icon={<GripVertical size={14} aria-hidden />}
                  title={<span className="font-mono">{agentId}</span>}
                  status={available ? undefined : <><StatusDot tone="warning" /> Unavailable in this workspace</>}
                  control={
                    <div className="flex items-center gap-1">
                      <span className="mr-2 text-xs" style={{ color: 'var(--text-3)' }}>{index + 1}</span>
                      <IconButton icon={<ArrowUp size={14} />} label={`Move ${agentId} up`} disabled={index === 0} onClick={() => moveFavorite(index, -1)} />
                      <IconButton icon={<ArrowDown size={14} />} label={`Move ${agentId} down`} disabled={index === favoriteUamAgentIds.length - 1} onClick={() => moveFavorite(index, 1)} />
                      <IconButton icon={<X size={14} />} label={`Remove ${agentId}`} variant="danger" onClick={() => updatePreferences(favoriteUamAgentIds.filter((id) => id !== agentId))} />
                    </div>
                  }
                />
              )
            }} />
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

              <Reveal open={Boolean(agentImportPreview)} className="grid gap-3">
                {agentImportPreview && (
                <>
                  <SettingRow
                    title={agentImportPreview.description || agentImportPreview.suggestedId || 'Provider agent'}
                    description={agentImportPreview.mode || 'Unknown mode'}
                  />
                  <Reveal open={agentImportPreview.securityFields.length > 0}>
                    <Notice tone="error" dismissible={false} dismissLabel="Dismiss">Blocked security/tool fields: {agentImportPreview.securityFields.join(', ')}</Notice>
                  </Reveal>
                  <Reveal open={agentImportPreview.ignoredFields.length > 0}>
                    <Notice tone="warning" dismissible={false} dismissLabel="Dismiss">Provider-only fields that will be omitted: {agentImportPreview.ignoredFields.join(', ')}</Notice>
                  </Reveal>
                  <Reveal open={Boolean(agentImportPreview.error)}><Notice tone="error" dismissible={false} dismissLabel="Dismiss">{agentImportPreview.error}</Notice></Reveal>
                  <Reveal open={agentImportPreview.supported}>
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
                      <Reveal open={agentImportPreview.ignoredFields.length > 0}>
                        <SettingRow
                          title="Omit listed fields"
                          description="Copies the rest of the agent. Listed fields are not copied."
                          control={<Switch hideLabel label="Acknowledge omitted provider fields" checked={agentImportAcknowledged} onChange={(event) => setAgentImportAcknowledged(event.target.checked)} />}
                        />
                      </Reveal>
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
                  </Reveal>
                </>
              )}
              </Reveal>
              <Reveal open={importNeedsChat}><Notice tone="info" dismissible={false} dismissLabel="Dismiss">Open a workspace chat to import into this workspace, or choose All workspaces.</Notice></Reveal>
            </div>
          </SettingsSection>}
        </div>
      )
    }

    if (selectedSection === 'cli-version') {
      const managers = (effectiveProviderSettingsHostId === 'local' ? cliVersionManager.providers : (cliVersionManager.remoteProviders ?? []).filter(manager => manager.executionHostId === effectiveProviderSettingsHostId)).filter(manager => !scopedProviderId || manager.providerId === scopedProviderId)
      return (
        <div ref={cliVersionMenuRef} className="grid gap-0">
          <Reveal open={Boolean(scopedProviderId && hasSshHosts && scopeValue === SETTINGS_ALL_ENVIRONMENTS)}><Notice tone="info" dismissible={false} dismissLabel="Dismiss">Choose a computer above to manage its CLI version.</Notice></Reveal>
          <Reveal open={managers.length === 0}><Notice tone="info" dismissible={false} dismissLabel="Dismiss">No CLI installation information is available. Check the provider on this host.</Notice></Reveal>
          {managers.map((manager) => {
            const cliVersionProvider = providers.find((provider) => provider.id === manager.providerId)
            const providerName = providerDisplayName(cliVersionProvider, manager.providerId)
            const installHostName = manager.executionHostId && manager.executionHostId !== 'local'
              ? manager.executionHostName || executionHosts.find((host) => host.id === manager.executionHostId)?.label || manager.executionHostId
              : 'this computer'
            const issues = cliVersionIssues(manager)
            const requestInstall = (version: string) => {
              setOpenCliVersionMenu(null)
              setPendingCliInstall({providerId:manager.providerId,providerName,version})
            }
            const rows = (
              <>
                <Reveal open={issues.length > 0}>{issues.map(issue => <SettingRow key={issue} title="Version" status={<span className="inline-flex items-center gap-1.5"><TriangleAlert size={13} aria-hidden style={{ color: 'var(--warning)' }} />{issue}</span>} />)}</Reveal>
                <SettingRow title="Installed" control={<span>{manager.installedVersion || 'Not detected'}</span>} />
                <SettingRow title="Recommended" control={<span>{manager.preferredVersion || 'Unavailable'}</span>} />
                <Reveal open={pendingCliInstall?.providerId === manager.providerId}>{pendingCliInstall?.providerId === manager.providerId && <div role="group" aria-label="Confirm CLI installation">
                  <SettingRow
                    title={`Install ${pendingCliInstall.providerName} ${pendingCliInstall.version} on ${installHostName} using ${manager.installMethod || 'npm'}?`}
                    control={
                      <div className="flex items-center gap-2">
                        <Button size="sm" onClick={() => setPendingCliInstall(null)}>Cancel</Button>
                        <Button size="sm" disabled={manager.running} onClick={async () => {
                          const pending = pendingCliInstall
                          setPendingCliInstall(null)
                          try { if (!await (manager.executionHostId && manager.executionHostId !== 'local' ? applyCliProviderVersion(pending.providerId,pending.version,manager.executionHostId) : applyCliProviderVersion(pending.providerId,pending.version))) reportAppFailure('Installation could not start.', 'Check provider output.') }
                          catch { reportAppFailure('Installation could not start.', 'Check provider output.') }
                        }}>Install version</Button>
                      </div>
                    }
                  />
                </div>}
                </Reveal>
                <Reveal open={Boolean(manager.lastCommand || manager.lastOutput)}>
                  <Disclosure summary="Last command output">
                    <div className="grid gap-2">
                      <Reveal open={Boolean(manager.lastCommand)}>
                        <code className="text-xs rounded-md px-2 py-1" style={{ background: 'var(--surface-up)', color: 'var(--text)' }}>
                          {manager.lastCommand}
                        </code>
                      </Reveal>
                      <pre
                        className="leading-5 overflow-auto rounded-lg px-3 py-2"
                        style={{
                          fontSize: 'var(--fs-xs)',
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
                  </Disclosure>
                </Reveal>
              </>
            )
            return (
              <SettingsSection
                key={manager.providerId}
                title={scopedProviderId ? 'CLI version' : providerName}
                actions={
                  <>
                    <StatusIndicator issues={issues} okLabel={manager.status === 'provider-managed' ? 'Provider managed' : 'CLI verified'}/>
                    <IconButton icon={<RefreshCw size={15}/>} label={`Refresh ${providerName} CLI version`} disabled={manager.running} onClick={async () => {
                      try { if (!await (manager.executionHostId && manager.executionHostId !== 'local' ? refreshCliProviderVersion(manager.providerId, manager.executionHostId) : refreshCliProviderVersion(manager.providerId))) reportAppFailure('Version check could not start.', 'Try again.') }
                      catch { reportAppFailure('Version check could not start.', 'Try again.') }
                    }}/>
                    <Reveal open={manager.canInstall !== false}><IconButton ref={element => { if (element) popupAnchorsRef.current.set(`${manager.providerId}:download`,element) }} icon={<Download size={15}/>} label={`Download ${providerName} CLI`} aria-haspopup="menu" aria-expanded={openCliVersionMenu === `${manager.providerId}:download`} disabled={manager.running} onClick={() => setOpenCliVersionMenu(current => current === `${manager.providerId}:download` ? null : `${manager.providerId}:download`)}/></Reveal>
                    {openCliVersionMenu === `${manager.providerId}:download` && <ViewportMenu anchorRef={{current:popupAnchorsRef.current.get(`${manager.providerId}:download`) ?? null}} role="menu" aria-label={`${providerName} download choices`} className="grid gap-1 p-2" style={{background:'var(--surface)',border:'1px solid var(--border)',borderRadius:'var(--r-md)'}}>
                      <MenuItem label={`Download recommended${manager.preferredVersion ? ` ${manager.preferredVersion}` : ''}`} disabled={!manager.preferredVersion} onSelect={() => requestInstall(manager.preferredVersion)} />
                      <MenuItem label="Download latest" onSelect={() => requestInstall('latest')} />
                    </ViewportMenu>}
                  </>
                }
              >
                <ProgressFill label={`${providerName} CLI task running`} indeterminate active={manager.running}>{rows}</ProgressFill>
              </SettingsSection>
            )
          })}
          {!scopedProviderId && <SettingRow title="Install provider updates automatically" description="Active sessions delay installation." control={<Switch hideLabel label="Install provider updates automatically" checked={automaticProviderUpdates} onChange={(event) => { void setUpdateSettings({ automaticProviderUpdates: event.target.checked }) }} />} />}
        </div>
      )
    }

    if (SSH_ENABLED && selectedSection === 'routing') return <ExecutionRoutingSettings ref={routingRef} variant="page" onAddComputer={() => changeSection('remote-hosts')} />

    if (SSH_ENABLED && selectedSection === 'remote-hosts') {
      const remoteHosts = executionHosts.filter((host) => host.id !== 'local')
      const addHostSection = (
            <SettingsSection
              title="Add a host"
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
                    onChange={(event) => { setRemoteLabel(event.target.value); setRemotePreview(null); setRemoteMessage(null) }}
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
                    onChange={(event) => { setRemoteAlias(event.target.value); setRemotePreview(null); setRemoteMessage(null) }}
                  />
                </SettingsField>
              </div>
              <Reveal open={Boolean(remoteMessage)}><StatusMessage message={remoteMessage} /></Reveal>
              <div className="flex justify-end pt-3">
                <Button size="sm" variant="primary" leadingIcon={<Server size={14} />} aria-busy={remoteBusy || undefined} disabled={remoteBusy || !remoteAlias.trim()} onClick={() => void previewRemoteHost()}>
                  Preview setup
                </Button>
              </div>
            </SettingsSection>
      )
      return (
        <div className="grid gap-0">
          {COMPUTER_USE_ENABLED && <p className="text-xs" style={{ color: 'var(--text-3)' }}>Remote Computer Use is off. Remote chats can't see or control the screen.</p>}

          <ProgressFill label="Remote host setup running" indeterminate active={remoteBusy}>{addHostSection}</ProgressFill>
          <SettingsSection title="Configured hosts">
            <Reveal open={remoteHosts.length === 0}><SettingRow title="No remote hosts yet" /></Reveal>
            <AnimatedRows items={remoteHosts} keyOf={(host) => host.id} render={(host) => (
              <SettingRow
                icon={<Server size={15} aria-hidden />}
                title={host.label}
                description={<span className="font-mono">{host.sshAlias}</span>}
                status={(
                  <span className="inline-flex flex-wrap items-center gap-1.5">
                    {host.runnerStatus === 'ready' ? <Check size={13} aria-hidden style={{ color: 'var(--success)' }} /> : host.runnerStatus === 'error' ? <X size={13} aria-hidden style={{ color: 'var(--error)' }} /> : <StatusDot tone="neutral" />}
                    <span>{host.runnerStatus === 'ready' ? 'Ready' : host.runnerStatus === 'error' ? 'Error' : host.runnerStatus}</span>
                    <Reveal as="span" open={Boolean(host.runnerVersion || host.platform)}><span style={{ color: 'var(--text-3)' }}>· {[host.runnerVersion ? `helper ${host.runnerVersion}` : '', host.platform ? `${host.platform} ${host.architecture}` : ''].filter(Boolean).join(' · ')}</span></Reveal>
                  </span>
                )}
                control={(
                  <div className="flex items-center gap-1">
                    <IconButton icon={<RefreshCw size={14} />} label={`Reinstall helper on ${host.label}`} disabled={isRemoteHostBusy(host.id)} onClick={() => void previewRemoteHost(host)} />
                    <IconButton icon={<Trash2 size={14} />} label={`Remove ${host.label}`} variant="danger" disabled={isRemoteHostBusy(host.id)} onClick={() => setPendingHostRemoval(host)} />
                  </div>
                )}
              >
                {(() => {
                  const options = (
                    <Disclosure summary="Host options">
                      <div className="grid gap-3">
                        <div className="font-mono text-xs" style={{ color: 'var(--text-3)' }}>Helper: home / {host.runnerDirectory || (host.platform === 'windows' ? '.uam/runner' : '.local/share/uam/runner')}</div>
                        <RemoteHostOptionsForm
                          host={host}
                          busy={isRemoteHostBusy(host.id)}
                          onSave={(draft) => void saveRemoteHostOptions(host, draft)}
                          onCheck={(instructionFile) => checkRemoteHostInstruction(host, instructionFile)}
                        />
                      </div>
                    </Disclosure>
                  )
                  return <ProgressFill label={`Remote host task running on ${host.label}`} indeterminate active={isRemoteHostBusy(host.id)}>{options}</ProgressFill>
                })()}
              </SettingRow>
            )} />
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
                <MenuSelect
                  label="Default memory level"
                  value={memoryLevelDefault}
                  disabled={memoryPolicySaving.app}
                  options={MEMORY_POLICY_CHOICES.filter((option) => option.id !== 'default').map((option) => ({ value: option.id, label: option.label, description: option.detail }))}
                  onChange={(level) => void saveMemoryPolicy('app', () => setMemorySettings({ memoryLevelDefault: level as MemoryLevel }))}
                />
              }
            />
            
            <Reveal open={Boolean(sessions.find((session) => session.id === activeSessionId))}><SettingRow title="Current chat memory" description="Default follows the provider policy."
              status={memoryPolicySaving.chat ? 'Saving…' : undefined}
              control={<MenuSelect label="Current chat memory policy" value={sessions.find((session) => session.id === activeSessionId)?.memoryLevel ?? 'default'} disabled={memoryPolicySaving.chat}
                options={MEMORY_POLICY_CHOICES.map((option) => ({ value: option.id, label: option.label, description: option.detail }))}
                onChange={(level) => { if (activeSessionId) void saveMemoryPolicy('chat', () => setSessionMemoryLevel(activeSessionId, level as MemoryLevel)) }} />}>

            </SettingRow></Reveal>
            <SettingRow
              title="Idle delay"
              description="Idle time before a chat's memory is processed."
              control={
                <NumberField label="Idle delay (seconds)" value={memoryIdleDelaySeconds} min={MIN_MEMORY_IDLE_DELAY_SECONDS} max={MAX_MEMORY_IDLE_DELAY_SECONDS} unit="seconds"
                  onCommit={(value) => void setMemorySettings({ memoryIdleDelaySeconds: value })} />
              }
            />
            <SettingRow
              title="Recall budget"
              description="Most saved memory added to each prompt."
              control={
                <NumberField label="Recall budget (bytes)" value={memoryRecallBudgetBytes} min={MIN_MEMORY_RECALL_BUDGET_BYTES} max={MAX_MEMORY_RECALL_BUDGET_BYTES} step={256} unit="bytes"
                  onCommit={(value) => void setMemorySettings({ memoryRecallBudgetBytes: value })} />
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
                const modelOptions = memoryModelOptions(workerProvider, binding.workerProviderId, binding.workerModelId, workerAcp?.availableModels)
                return (
                  <SettingRow
                    key={provider.id}
                    icon={<ProviderLogo providerId={provider.id} />}
                    title={`${providerDisplayName(provider, provider.id)} chats`}
                    control={
                    <div className="grid grid-cols-2 gap-2" style={{ width: 380 }}>
                      <MenuSelect label={`${providerDisplayName(provider, provider.id)} memory worker provider`} value={binding.workerProviderId}
                        options={providers.filter((candidate) => providerSupportsTextWorkers(candidate.id)).map((candidate) => ({ value: candidate.id, label: providerDisplayName(candidate, candidate.id), icon: <ProviderLogo providerId={candidate.id} /> }))}
                        onChange={(workerProviderId) => updateMemoryBinding(provider.id, { workerProviderId, workerModelId: '' })} />
                      <MenuSelect label={`${providerDisplayName(provider, provider.id)} memory worker model`} value={binding.workerModelId}
                        options={modelOptions.map((option) => ({ value: option.id, label: option.label, description: option.detail }))}
                        onChange={(workerModelId) => updateMemoryBinding(provider.id, { ...binding, workerModelId })} />
                    </div>
                    }
                  />
                )
              })}
            </div>
            <Reveal open={Boolean(memoryLastStatus)}><SettingRow title="Last run" description={memoryLastStatus || undefined} /></Reveal>

            <Reveal open={hasWorkerLog}>
              <Disclosure className="mt-3 text-xs" summary={<span className="inline-flex items-center gap-1" style={{ color: workerLogIsFailure ? 'var(--error)' : 'var(--text)' }}>{workerLogIsFailure && <X size={13} aria-hidden />}Last worker log</span>}>
                <div style={{ color: 'var(--text-2)' }}>
                <Reveal open={Boolean(memoryActivity.lastWorkerStatus)}><p className="mt-2" style={{ color: 'var(--text-3)', fontSize: 'var(--fs-xs)' }}>{memoryActivity.lastWorkerStatus}</p></Reveal>
                <Reveal open={Boolean(workerLogMeta)}><p className="mt-0.5" style={{ color: 'var(--text-3)', fontSize: 'var(--fs-xs)' }}>{workerLogMeta}</p></Reveal>
                <pre
                  className="mt-2 rounded-lg px-3 py-2 leading-5 overflow-auto"
                  style={{
                    fontSize: 'var(--fs-xs)',
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
                </div>
              </Disclosure>
            </Reveal>
          </SettingsSection>

        </div>
      )
    }


    if (selectedSection === 'voice-input') return <VoiceInputControls ref={voiceRef} />

    if (selectedSection === 'memory-store') return <MemoryLibraryModal embedded ref={memoryRef}/>
    // The folder setting (choose or type a path) lives inside the Skills card, so the page is just the library.
    if (selectedSection === 'markdown-store') return <MarkdownStoreModal embedded/>

    if (selectedSection === 'goal-loops') {
      return (
        <SettingsSection title="Goal limits">
          <SettingRow title="Goal token budget" description="Maximum tokens for a new goal; 0 is unlimited." control={<MenuSelect label="Goal token budget" value={String(goalTokenBudget)} options={[{ value: '0', label: 'Unlimited' }, ...[10000, 25000, 50000, 100000, 200000, 500000].map((value) => ({ value: String(value), label: value.toLocaleString() }))]} onChange={(value) => void setMemorySettings({ goalTokenBudget: Number(value) })} />} />
          <SettingRow
            title="Maximum iterations"
            description="Loops a goal may run before it stops."
            control={<MenuSelect label="Maximum goal loop iterations" value={String(goalMaxLoopIterations)} options={Array.from({ length: 200 }, (_, index) => ({ value: String(index + 1), label: String(index + 1) }))} onChange={(value) => void setMemorySettings({ goalMaxLoopIterations: Number(value) })} />}
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
          <SettingsSection title="File types" actions={renderAddEditor()}>
            <SettingRow
              title="Default editor"
              description="Opens files outside every file type group."
              control={renderEditorPresetMenu(
                defaultEditorDraft,
                'Default editor',
                (presetId) => saveEditorSettings(editorAssociationsDraft, presetId)
              )}
            />
            <div ref={editorMenuRef}>
              <AnimatedRows items={editorAssociationsDraft} keyOf={(association) => association.id} render={(association) => {
                const groupName = association.name.trim() || 'File group'
                const expanded = expandedEditorGroups[association.id] ?? false
                const panelId = `${association.id}-editor-group-panel`
                return (
                  <SettingRow
                    icon={<IconButton size="sm" icon={<ChevronRight size={14} style={{ transform: expanded ? 'rotate(90deg)' : undefined, transition: 'transform var(--dur-fast) var(--ease)' }} />} label={`${expanded ? 'Hide' : 'Show'} ${groupName} editor group`} aria-expanded={expanded} aria-controls={panelId} onClick={() => toggleEditorGroup(association.id)} />}
                    title={groupName}
                    description={<span className="font-mono">{extensionsText(association) || 'No extensions set'}</span>}
                    control={<div className="flex items-center gap-2">
                      <span className="text-xs" style={{ color: 'var(--text-3)' }}>Opens in {editorPresetLabel(association.editorPresetId)}</span>
                      <IconButton icon={<Trash2 size={14}/>} label={`Delete ${groupName} editor group`} variant="danger" size="sm" disabled={editorSaving || editorAssociationsDraft.length <= 1} onClick={() => setPendingDelete({kind:'editor',id:association.id,name:groupName})}/>
                    </div>}
                  >
                    <Reveal open={expanded} id={panelId} className="grid grid-cols-3 gap-2">
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
                    </Reveal>
                  </SettingRow>
                )
              }} />
              <Reveal open={editorAssociationsDraft.length === 0}><SettingRow title={<span style={{ color: 'var(--text-3)' }}>No file types. Add a group to choose an editor for a set of extensions.</span>} control={renderAddEditor()} /></Reveal>
            </div>
          </SettingsSection>

          <SettingsSection title="Folders and terminal">
            <SettingRow
              title="File explorer"
              description={customExplorer ? 'A custom application opens folders.' : 'Your system file manager opens folders.'}
              control={<MenuSelect label="File explorer" disabled={editorSaving} value={customExplorer ? 'custom' : 'system'} options={[{ value: 'system', label: 'System default' }, { value: 'custom', label: 'Custom application' }]} onChange={(value) => { setCustomExplorer(value === 'custom'); if (value === 'system') void saveExplorer('') }} />}
            >
              <Reveal open={customExplorer}><div className="flex gap-2">
                <input aria-label="File explorer application path" value={explorerDraft} onChange={(event) => setExplorerDraft(event.target.value)} className="uam-field min-w-0 flex-1 font-mono" placeholder="/Applications/Path Finder.app" />
                <Button size="sm" onClick={async () => { const response = await sendToCEF<{ selectedPath: string }>({ action: 'browseFolderDirectory', payload: { currentValue: explorerDraft, application: true } }); if (!response.ok) reportAppFailure(response.error || 'Application picker could not be opened.', 'Try again.'); else if (response.data?.selectedPath) setExplorerDraft(response.data.selectedPath) }}>Browse</Button>
                <Button size="sm" variant="primary" disabled={!explorerDraft.trim() || explorerDraft.trim() === fileExplorerApplication || editorSaving} onClick={() => void saveExplorer(explorerDraft.trim())}>Save</Button>
              </div></Reveal>
            </SettingRow>
            {isWindows && <SettingRow
              title="Terminal shell"
              description={WINDOWS_POWERSHELL_DESCRIPTIONS[windowsPowerShell]}
              control={<MenuSelect label="Terminal shell" disabled={editorSaving} value={windowsPowerShell} options={WINDOWS_POWERSHELL_OPTIONS} onChange={(value) => void saveWindowsPowerShell(value as WindowsPowerShellPreference)} />}
            />}
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
          <NativeHistorySettings onFiltersDirtyChange={setImportFiltersDirty} />
          <SettingsSection title="Chat bundle">
            <SettingRow
              title="Export"
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
              title="Import"
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
            <Button size="sm" variant="ghost" className="self-start mb-5" style={{paddingInline:'var(--sp-2)'}} aria-label="Back to chats" leadingIcon={<ArrowLeft size={15} aria-hidden/>} disabled={mcpSaving || editorSaving || remoteBusy || themeSaving} onClick={requestClose}>Back to chats</Button>
            <h1 className="text-lg font-semibold px-2 mb-3">Settings</h1>
            <div className="uam-search-field flex items-center gap-2 px-2 rounded-md mb-5" style={{border:'1px solid var(--border)',background:'var(--surface)'}}>
              <Search size={14} aria-hidden style={{color:'var(--text-3)',flexShrink:0}}/>
              <input type="text" role="searchbox" aria-label="Search settings" placeholder="Search settings" value={settingsSearch} onChange={event => setSettingsSearch(event.target.value)} className="min-w-0 w-full py-2 text-xs" style={{background:'transparent',border:0,color:'var(--text)'}}/>
              <Reveal open={Boolean(settingsSearch)}><IconButton size="sm" icon={<X size={12}/>} label="Clear settings search" onClick={() => setSettingsSearch('')}/></Reveal>
            </div>
            <nav ref={navRef} aria-label="Settings pages" className="uam-settings-nav min-h-0 overflow-y-auto flex-1">
              {navIndicator && <span aria-hidden className="uam-settings-nav__indicator" style={{ transform: `translateY(${navIndicator.top}px)`, height: navIndicator.height }} />}
              <Reveal open={visibleSettingsGroups.length === 0}><p role="status" className="px-2 text-xs" style={{color:'var(--text-3)'}}>No settings found.</p></Reveal>
              {visibleSettingsGroups.map(group => <div key={group.label} className="mb-5 last:mb-0">
                <h2 className="px-2 mb-1 text-[length:var(--fs-xs)] font-semibold uppercase tracking-[0.08em]" style={{color:'var(--text-3)'}}>{group.label}</h2>
                {group.sections.map(id => {
                  const section = SETTINGS_SECTIONS.find(item => item.id === id)!
                  const active = selectedSection === section.id
                  const SectionIcon = section.icon
                  return <RowButton key={section.id} type="button" aria-pressed={active} aria-label={section.label} onClick={() => changeSection(section.id)} className="uam-settings-nav-item" data-active={active}>{section.id.startsWith('provider:') ? <ProviderLogo providerId={section.id.slice('provider:'.length)} size={15}/> : <SectionIcon size={15} className="shrink-0" aria-hidden/>}<span className="text-sm">{section.label}</span></RowButton>
                })}
              </div>)}
            </nav>
          </aside>
          <div className="min-w-0 min-h-0 flex flex-col overflow-hidden">
            <Reveal open={unconfirmedSettingsLabels.length > 0} className="shrink-0 px-6 pt-6"><div data-memory-settings-unconfirmed>
              <Notice tone="warning" title="Unconfirmed settings changes" dismissible={false} dismissLabel="Unconfirmed settings changes cannot be dismissed">
                <p>Displayed choices are unconfirmed drafts and may differ from the running app.</p>
                <ul aria-label="Unconfirmed settings fields" className="list-disc pl-4">
                  {unconfirmedSettingsLabels.map((label) => <li key={label}>{label}</li>)}
                </ul>
              </Notice>
            </div></Reveal>
            <div className="w-full min-h-0 flex-1 flex flex-col">
              <Swap swapKey={themeDraft ? 'theme' : 'page'} className="flex-1 min-h-0 flex flex-col">{themeDraft ? <div className="overflow-y-auto">
                <div className="shrink-0 flex items-center justify-between gap-3 mb-5"><h2 tabIndex={-1} data-theme-step className="text-lg font-semibold">{['Theme name and base','Theme palette','Review theme'][themeStep]}</h2><IconButton icon={<X size={16}/>} label="Close theme editor" disabled={themeSaving} onClick={() => requestThemeExit(() => setThemeDraft(null))}/></div>
                {renderThemeEditor()}
              </div> : <>
                {(() => {
                  const fullHeight = selectedSection === 'memory-store' || selectedSection === 'markdown-store'
                  const header = <div className="uam-settings-page-header w-full">
                  <div key={selectedSection} className="uam-reveal min-w-0">
                    <h2 className="uam-settings-page-header__title">{selectedSection === 'cli-version' ? 'CLI version control' : SETTINGS_SECTIONS.find(section => section.id === selectedSection)?.label}</h2>
                  </div>
                </div>
                  if (fullHeight) return <div className="flex-1 min-h-0 flex flex-col p-6"><div className="uam-settings-frame flex-1 min-h-0 flex flex-col">{header}{scopeBar}<SettingsPageSwap pageKey={selectedSection} className="uam-settings-page flex-1 min-h-0 overflow-hidden mt-5">{renderSectionContent()}</SettingsPageSwap></div></div>
                  return <div className="uam-settings-scroll"><div className="uam-settings-frame" style={selectedSection === 'phone' ? { margin: '0 auto' } : undefined}>{header}{scopeBar}<SettingsPageSwap pageKey={selectedSection} className="uam-settings-page uam-settings-page--narrow">{renderSectionContent()}</SettingsPageSwap></div></div>
                })()}
              </>}</Swap>
            </div>
          </div>
        </div>
      </div>
      <ConfirmDialog open={Boolean(editorExit)} tone="primary" title="Save editor changes?" label="Unsaved editor changes" confirmLabel="Save" cancelLabel="Stay" busy={editorSaving}
        secondaryAction={{ label: 'Discard', onClick: () => { const next = editorExit; setEditorExit(null); next?.() } }}
        onCancel={() => setEditorExit(null)}
        onConfirm={async () => {
          const next = editorAssociationsDraft.map(item => ({...item,name:rawEditorNames[item.id]?.trim() ?? item.name,extensions:rawExtensions[item.id] === undefined ? item.extensions : parseExtensions(rawExtensions[item.id])}))
          // With MCP changes also unsaved, saving keeps Settings open; the MCP changes are then offered on the next close.
          if (await saveEditorSettings(next)) { const leave=editorExit; setEditorExit(null); if (!editorExitMcp) leave?.() }
        }}>
        <p>Your file type changes have not been saved.</p>
        {editorExitMcp && <p>MCP server changes have not been saved either. Discard drops both; Save keeps Settings open.</p>}
      </ConfirmDialog>
      <ConfirmDialog open={Boolean(themeExit)} tone="primary" title="Save theme changes?" label="Unsaved theme changes" settingsOwned
        confirmLabel="Save theme" cancelLabel="Stay" busy={themeSaving} confirmDisabled={!themeDirty || !themeDraftValid}
        secondaryAction={{ label: 'Discard', onClick: () => { const next = themeExit; setThemeDraft(null); setThemeExit(null); next?.() } }}
        onCancel={() => setThemeExit(null)}
        onConfirm={async () => { if (await saveThemeDraft()) { const next = themeExit; setThemeExit(null); next?.() } }}>
        <p>Your theme edits have not been saved.</p>
      </ConfirmDialog>
      {SSH_ENABLED && <Presence open={Boolean(remotePreview)}>
        {/* ponytail: accepted exception (audit 3071). This dialog keeps its own panel: the SSH helper preview needs a wider body and a scrollable pre block that the shared ConfirmDialog surface does not provide. */}
        {remotePreview && <Overlay open zIndex={70} onBackdropMouseDown={() => { if (!remoteBusy) setRemotePreview(null) }}>
          <div role="dialog" aria-modal="true" data-settings-owned-overlay aria-label="Remote helper setup" className="w-full max-w-lg rounded-xl" style={SETTINGS_DIALOG_STYLE}>
            <div className="px-5 pt-5">
              <div className="text-sm font-semibold" style={{ color: 'var(--text)' }}>Install helper on {remotePreview.host.label}?</div>
              <div className="mt-1 text-xs" style={{ color: 'var(--text-3)' }}>SSH alias: <code>{remotePreview.host.sshAlias}</code></div>
            </div>
            <div className="grid gap-4 px-5 pt-4 pb-5 text-sm" style={{ color: 'var(--text-2)' }}>
  			  <p>Uses your existing SSH login, copies the matching helper, checks its SHA-256, and starts it. Unsupported systems stop before copying.</p>
  			  <fieldset className="grid gap-2">
  				<legend className="mb-1 text-xs font-medium" style={{ color: 'var(--text)' }}>Install location</legend>
  				<SegmentedControl
  				  aria-label="Install location"
  				  value={remoteCustomDirectory ? 'custom' : 'private'}
  				  disabled={remoteBusy}
  				  onChange={(value) => setRemoteCustomDirectory(value === 'custom')}
  				  options={[{ value: 'private', label: 'Recommended location' }, { value: 'custom', label: 'Custom folder' }]}
  				/>
  				<Reveal open={!remoteCustomDirectory}><span className="block text-[length:var(--fs-xs)]" style={{ color: 'var(--text-3)' }}>Linux: ~/.local/share/uam/runner/{appVersion.replace(/^v/i, '')}<br />Windows: %USERPROFILE%\.uam\runner\{appVersion.replace(/^v/i, '')}</span></Reveal>
  				<Reveal open={remoteCustomDirectory} className="grid gap-1">
  				  <input aria-label="Remote helper folder" aria-invalid={Boolean(remoteDirectoryValidation)} aria-describedby={remoteDirectoryValidation ? 'remote-helper-folder-help remote-helper-folder-error' : 'remote-helper-folder-help'} value={remoteDirectory} onChange={(event) => setRemoteDirectory(event.currentTarget.value)} placeholder="uam-helper" spellCheck={false} className="uam-field w-full font-mono" style={remoteDirectoryValidation ? { borderColor: 'var(--error)' } : undefined} />
  				  <span id="remote-helper-folder-help" className="block text-[length:var(--fs-xs)]" style={{ color: 'var(--text-3)' }}>Relative path only. Use letters, numbers, dots, dashes, underscores, and /.</span>
  				  <Reveal open={Boolean(remoteDirectoryValidation)}><span id="remote-helper-folder-error" role="alert" className="block text-[length:var(--fs-xs)]" style={{ color: 'var(--error)' }}>{remoteDirectoryValidation}</span></Reveal>
  				  <Reveal open={!remoteDirectoryValidation}><span className="block break-all font-mono text-[length:var(--fs-xs)]" style={{ color: 'var(--accent)' }}>Linux: ~/{remoteDirectory.trim()}/{appVersion.replace(/^v/i, '')}<br />Windows: %USERPROFILE%\{remoteDirectory.trim().replace(/\//g, '\\')}\{appVersion.replace(/^v/i, '')}</span></Reveal>
  				</Reveal>
  			  </fieldset>
  			  <pre className="max-h-32 overflow-auto whitespace-pre-wrap rounded p-2 text-[length:var(--fs-xs)]" style={{ background: 'var(--bg)', color: 'var(--text-3)', border: '1px solid var(--border)' }}>{remotePreview.preview.split('\n').filter((line) => !line.startsWith('Install UAM runner')).join('\n')}</pre>
            </div>
            <div className="flex justify-end gap-2 px-5 pb-5">
              <Button size="sm" disabled={remoteBusy} onClick={() => setRemotePreview(null)}>Cancel</Button>
              <Button size="sm" variant="primary" aria-busy={remoteBusy || undefined} disabled={remoteBusy || Boolean(remoteDirectoryValidation)} onClick={() => void installRemoteHost()}>Connect and install</Button>
            </div>
          </div>
        </Overlay>}
      </Presence>}
      <ConfirmDialog open={confirmDiscard} tone="danger" title={selectedSection === 'chat-data' ? 'Discard import filter changes?' : 'Discard MCP changes?'} label={selectedSection === 'chat-data' ? 'Discard unsaved import filters' : 'Discard unsaved MCP changes'}
        cancelLabel="Keep editing" confirmLabel="Discard changes"
        onCancel={() => { setConfirmDiscard(false); dialogRef.current?.focus() }}
        onConfirm={() => { setMcpDraftDirty(false); setImportFiltersDirty(false); setConfirmDiscard(false); discardExit?.(); setDiscardExit(null) }}>
        <p>{selectedSection === 'chat-data' ? 'The import filters have unsaved changes.' : 'The MCP server configuration has unsaved changes.'}</p>
      </ConfirmDialog>
      <ConfirmDialog open={Boolean(pendingDelete)} title={`Delete ${pendingDelete?.kind === 'theme' ? 'theme' : 'editor group'}?`} label={pendingDelete ? `Delete ${pendingDelete.name}` : undefined}
        confirmLabel="Delete permanently" busy={themeSaving || editorSaving}
        onCancel={() => setPendingDelete(null)}
        onConfirm={async () => {
          if (!pendingDelete) return
          const deleted = pendingDelete.kind === 'theme' ? await removeSelectedTheme() : await saveEditorSettings(editorAssociationsDraft.filter(item => item.id !== pendingDelete.id))
          if (deleted) setPendingDelete(null)
        }}>
        <p>“{pendingDelete?.name}” will be permanently deleted. This cannot be undone or restored.</p>
      </ConfirmDialog>
      <Presence open={Boolean(pendingDefaultYolo)}>{pendingDefaultYolo && <ConfirmDialog open title="Use YOLO by default?" confirmLabel="Use YOLO"
        onCancel={() => setPendingDefaultYolo(null)} onConfirm={() => { const pending = pendingDefaultYolo; setPendingDefaultYolo(null); updateProviderDefaults(pending.providerId, pending.defaults) }}>
        New {pendingDefaultYolo.providerName} chats will automatically approve {COMPUTER_USE_ENABLED ? 'computer use, commands' : 'commands'}, file changes, and every other permission request.
      </ConfirmDialog>}</Presence>
      {SSH_ENABLED && <ConfirmDialog open={pendingHostRemoval !== null} title={`Remove ${pendingHostRemoval?.label ?? 'host'}?`} confirmLabel="Remove host" busy={isRemoteHostBusy(pendingHostRemoval?.id ?? '')}
        onCancel={() => setPendingHostRemoval(null)} onConfirm={() => { const host = pendingHostRemoval; if (host) void removeRemoteHost(host).then(() => setPendingHostRemoval(null)) }}>
        This removes the saved host configuration. Any helper files on that machine are left untouched.
      </ConfirmDialog>}
    </div>
  )
})
