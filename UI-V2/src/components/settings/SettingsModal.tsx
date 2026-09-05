import { forwardRef, useEffect, useImperativeHandle, useRef, useState, type ReactNode } from 'react'
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
  type McpServerConfiguration,
    type ProviderChatDefaults,
  type ProviderAgentImportPreview,
  type UamAgentCycleShortcut,
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
  type StoredTheme,
} from '../../utils/themeStorage'
import type { Provider } from '../../types/provider'
import type { ExecutionHost, Session } from '../../types/session'
import { MEMORY_LEVEL_OPTIONS } from '../../types/memory'
import { ProviderLogo } from '../shared/ProviderLogo'
import { useShallow } from 'zustand/react/shallow'
import { Search, ArrowLeft, BookOpen, Brain, Check, ChevronDown, ChevronRight, ClipboardList, Copy, Download, Upload, FolderOpen, Info, MemoryStick, MessageSquare, Mic, Minus, MousePointerClick, Palette, Pencil, Plus, RefreshCw, Save, Server, Target, TerminalSquare, Trash2, X, type LucideIcon } from 'lucide-react'
import { Button, IconButton, MenuSelect, Notice, Switch, ViewportMenu } from '../ui'
import { ShellActionsSettings, type ShellActionsHandle } from './ShellActionsSettings'
import { MemoryLibraryModal, type MemoryLibraryHandle } from './MemoryLibraryModal'
import { MarkdownStoreModal } from './MarkdownStoreModal'
import { StatusIndicator } from '../shared/StatusIndicator'
import {
  DEFAULT_PROVIDER_ID,
  providerCapabilities,
  providerMetadataForId,
  providerShortName,
} from '../../utils/providerMetadata'
import { buildCodexReasoningOptions, buildCodexSpeedOptions, buildModelOptions, reasoningEffortForModel, selectedRuntimeModel, titleFromModelId } from '../chat/modelOptions'

interface MemoryModelOption {
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

function latestProviderSession(sessions: Session[], providerId: string) {
  return sessions.reduce<Session | undefined>((latest, session) => {
    if (session.providerId !== providerId) return latest
    if (!latest || session.updatedAt.getTime() > latest.updatedAt.getTime()) return session
    return latest
  }, undefined)
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

type SettingsSectionId = 'appearance' | 'defaults' | 'agents' | 'cli-version' | 'remote-hosts' | 'voice-input' | 'memory-settings' | 'memory-store' | 'markdown-store' | 'goal-loops' | 'mcp-servers' | 'editors' | 'shell-actions' | 'chat-data' | 'about'

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
  { id: 'appearance', label: 'Appearance', icon: Palette },
  { id: 'defaults', label: 'Chat Defaults', icon: MessageSquare },
  { id: 'agents', label: 'Agents', icon: ClipboardList },
  { id: 'cli-version', label: 'CLI Version', icon: TerminalSquare },
  { id: 'remote-hosts', label: 'Remote Hosts', icon: Server },
  { id: 'voice-input', label: 'Voice Input', icon: Mic },
  { id: 'memory-settings', label: 'Memory Settings', icon: Brain },
  { id: 'memory-store', label: 'Memory Store', icon: MemoryStick },
  { id: 'markdown-store', label: 'Skills', icon: BookOpen },
  { id: 'goal-loops', label: 'Goal Loops', icon: Target },
  { id: 'mcp-servers', label: 'MCP Servers', icon: TerminalSquare },
  { id: 'editors', label: 'Editors', icon: Pencil },
  { id: 'shell-actions', label: 'Shell Actions', icon: MousePointerClick },
  { id: 'chat-data', label: 'Chat Data', icon: Download },
  { id: 'about', label: 'About', icon: Info },
]

const SETTINGS_GROUPS: { label: string; sections: SettingsSectionId[] }[] = [
  { label: 'General', sections: ['appearance', 'defaults', 'voice-input'] },
  { label: 'Providers', sections: ['cli-version', 'agents', 'remote-hosts', 'mcp-servers'] },
  { label: 'Workspace', sections: ['editors', 'shell-actions', 'memory-settings', 'memory-store', 'markdown-store', 'goal-loops'] },
  { label: 'App', sections: ['chat-data', 'about'] },
]

// Terms name controls inside each page so searches work beyond sidebar titles.
const SETTINGS_SEARCH_TERMS: Record<SettingsSectionId, string> = {
  appearance: 'themes colours colors palettes dark light import export sidebar icons worktree paths compact working activity',
  defaults: 'models reasoning providers permissions safety approval yolo memory defaults codex gemini claude copilot opencode',
  agents: 'agents instructions import providers codex gemini claude copilot opencode',
  'cli-version': 'installed recommended latest download update version codex gemini claude copilot opencode',
  'remote-hosts': 'computers ssh connections hosts runners servers install',
  'voice-input': 'microphone dictation transcription speech whisper',
  'memory-settings': 'memory workers recall budgets idle models tokens processing',
  'memory-store': 'memory library lessons entries search delete',
  'markdown-store': 'skills markdown library import folder file create pin',
  'goal-loops': 'goals loops tokens budgets auto continue review',
  'mcp-servers': 'mcp computer browser control setup server config json',
  editors: 'editors extensions associations groups default vscode clion',
  'shell-actions': 'finder explorer context menus shell actions groups save',
  'chat-data': 'chat data history storage import export backup',
  about: 'version build release updates app information',
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

function SectionCard({ title, children }: { title: string; children: ReactNode }) {
  return <section aria-label={title} className="pb-4">{children}</section>
}

function ProviderDisclosureCard({ panelId, providerId, leadingIcon, title, expanded, onToggle, toggleLabel, children, actions, togglePosition = 'left' }: {
  panelId: string
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
  return <div className="py-3" style={{borderBottom:'1px solid var(--border)'}}>
    <div className="flex items-center gap-2">
      {togglePosition === 'left' && <>
      <IconButton size="sm" icon={<ChevronRight size={14} className={expanded ? 'rotate-90' : undefined}/>} label={toggleLabel} aria-expanded={expanded} aria-controls={panelId} onClick={onToggle} style={{border:'1px solid var(--border)',background:'var(--surface-up)'}}/>
      </>}
      {leadingIcon ?? (providerId ? <ProviderLogo providerId={providerId}/> : null)}
      <span className="min-w-0 flex-1 text-sm truncate">{title}</span>
      {actions}
      {togglePosition === 'right' && <>
      <IconButton size="sm" icon={<ChevronRight size={14} className={expanded ? 'rotate-90' : undefined}/>} label={toggleLabel} aria-expanded={expanded} aria-controls={panelId} onClick={onToggle} style={{border:'1px solid var(--border)',background:'var(--surface-up)'}}/>
      </>}
    </div>
    {expanded && <div id={panelId} className="mt-3 pt-3" style={{borderTop:'1px solid var(--border)'}}>{children}</div>}
  </div>
}

function cliVersionIssues(manager: CliVersionProviderState): string[] {
  const healthy = manager.status === 'verified' || manager.status === 'provider-managed'
  return [...new Set([
    !healthy ? manager.message || versionStatusText(manager) : '',
    manager.lastInstallStatus === 'failed' ? 'Last installation failed.' : '',
  ].filter(Boolean))]
}

export interface SettingsHandle {
  requestClose: () => void
  showMemory: () => void
}

export const SettingsModal = forwardRef<SettingsHandle>(function SettingsModal(_props, ref) {
  const setSettingsOpen = useAppStore((s) => s.setSettingsOpen)
  const providers = useAppStore(useShallow((s) => s.providers))
  const sessions = useAppStore(useShallow((s) => s.sessions))
  const activeSessionId = useAppStore((s) => s.activeSessionId)
  const acpBindings = useAppStore(useShallow((s) => s.acpBindingBySessionId))
  const providerModelCatalogs = useAppStore(useShallow((s) => s.providerModelCatalogs))
  const folders = useAppStore(useShallow((s) => s.folders))
  const discoverProviderModels = useAppStore((s) => s.discoverProviderModels)
  const memoryEnabledDefault = useAppStore((s) => s.memoryEnabledDefault)
  const memoryLevelDefault = useAppStore((s) => s.memoryLevelDefault)
  const memoryIdleDelaySeconds = useAppStore((s) => s.memoryIdleDelaySeconds)
  const memoryRecallBudgetBytes = useAppStore((s) => s.memoryRecallBudgetBytes)
  const goalMaxLoopIterations = useAppStore((s) => s.goalMaxLoopIterations)
  const acpSetupInactivityTimeoutSeconds = useAppStore((s) => s.acpSetupInactivityTimeoutSeconds)
  const acpTurnOutputLimitMiB = useAppStore((s) => s.acpTurnOutputLimitMiB)
  const appVersion = useAppStore((s) => s.appVersion)
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
  const markdownStoreError = useAppStore((s) => s.markdownStoreError)
  const isMarkdownStoreOpen = useAppStore((s) => s.isMarkdownStoreOpen)
  const defaultNewChatProviderId = useAppStore((s) => s.defaultNewChatProviderId)
  const providerChatDefaults = useAppStore(useShallow((s) => s.providerChatDefaults))
  const defaultEditorPresetId = useAppStore((s) => s.defaultEditorPresetId)
  const editorFileAssociations = useAppStore(useShallow((s) => s.editorFileAssociations))
  const mcpServers = useAppStore(useShallow((s) => s.mcpServers))
  const executionHosts = useAppStore(useShallow((s) => s.executionHosts))
  const favoriteUamAgentIds = useAppStore(useShallow((s) => s.favoriteUamAgentIds))
  const uamAgentCycleShortcut = useAppStore((s) => s.uamAgentCycleShortcut)
  const activeUamAgents = useAppStore(useShallow((s) => activeSessionId ? s.uamAgentsBySessionId[activeSessionId] ?? [] : []))
  const setMemorySettings = useAppStore((s) => s.setMemorySettings)
  const setProviderChatDefaults = useAppStore((s) => s.setProviderChatDefaults)
  const setEditorSettings = useAppStore((s) => s.setEditorSettings)
  const setMcpServers = useAppStore((s) => s.setMcpServers)
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
  const searchTerms = settingsSearch.toLocaleLowerCase().trim().split(/\s+/).filter(Boolean)
  const visibleSettingsGroups = SETTINGS_GROUPS.map(group => ({...group, sections: group.sections.filter(id => {
    const section = SETTINGS_SECTIONS.find(item => item.id === id)!
    const searchable = `${group.label} ${section.label} ${SETTINGS_SEARCH_TERMS[id]}`.toLocaleLowerCase()
    return searchTerms.every(term => searchable.includes(term))
  })})).filter(group => group.sections.length > 0)
  const [openMemoryMenu, setOpenMemoryMenu] = useState<string | null>(null)
  const [openEditorMenu, setOpenEditorMenu] = useState<string | null>(null)
  const [openCliVersionMenu, setOpenCliVersionMenu] = useState<string | null>(null)
  const [pendingCliInstall, setPendingCliInstall] = useState<{ providerId: string; providerName: string; version: string } | null>(null)
  const [pendingDefaultYolo, setPendingDefaultYolo] = useState<{ providerId: string; providerName: string; defaults: ProviderChatDefaults } | null>(null)
  const [expandedDefaultProviders, setExpandedDefaultProviders] = useState<Record<string, boolean>>({})
  const [expandedCliVersionProviders, setExpandedCliVersionProviders] = useState<Record<string, boolean>>({})
  const [expandedEditorGroups, setExpandedEditorGroups] = useState<Record<string, boolean>>({})
  const [markdownStoreDraftDirectory, setMarkdownStoreDraftDirectory] = useState(markdownStoreDirectory)
  const [editorAssociationsDraft, setEditorAssociationsDraft] = useState(editorFileAssociations)
  const [defaultEditorDraft, setDefaultEditorDraft] = useState(defaultEditorPresetId)
  const [mcpDraft, setMcpDraft] = useState(() => JSON.stringify(mcpServers, null, 2))
  const [mcpDraftDirty, setMcpDraftDirty] = useState(false)
  const [confirmDiscard, setConfirmDiscard] = useState(false)
  const [mcpWorkspace, setMcpWorkspace] = useState(() => {
    const active = sessions.find((session) => session.id === activeSessionId)
    return active?.workspaceSourceDirectory || active?.workspaceDirectory || folders[0]?.directory || ''
  })
  const [mcpExecutable, setMcpExecutable] = useState('')
  const [mcpSaving, setMcpSaving] = useState(false)
  const [mcpMessage, setMcpMessage] = useState('')
  const [remoteLabel, setRemoteLabel] = useState('')
  const [remoteAlias, setRemoteAlias] = useState('')
  const [remoteBusy, setRemoteBusy] = useState(false)
  const [remoteMessage, setRemoteMessage] = useState('')
  const [remotePreview, setRemotePreview] = useState<{ host: ExecutionHost; preview: string } | null>(null)
  const [remoteCustomDirectory, setRemoteCustomDirectory] = useState(false)
  const [remoteDirectory, setRemoteDirectory] = useState('uam-helper')
  const remoteDirectoryValidation = remoteCustomDirectory ? runnerDirectoryError(remoteDirectory) : ''
  const [favoriteAgentCandidate, setFavoriteAgentCandidate] = useState('')
  const [agentImportProvider, setAgentImportProvider] = useState('opencode-cli')
  const [agentImportPath, setAgentImportPath] = useState('')
  const [agentImportId, setAgentImportId] = useState('')
  const [agentImportAccess, setAgentImportAccess] = useState<'read' | 'write'>('read')
  const [agentImportWorkspaceScope, setAgentImportWorkspaceScope] = useState(true)
  const [agentImportAcknowledged, setAgentImportAcknowledged] = useState(false)
  const [agentImportPreview, setAgentImportPreview] = useState<ProviderAgentImportPreview | null>(null)
  const [agentImportBusy, setAgentImportBusy] = useState(false)
  const [agentImportMessage, setAgentImportMessage] = useState('')
  const [chatDataBusy, setChatDataBusy] = useState<'export' | 'import' | null>(null)
  const [chatDataFolder, setChatDataFolder] = useState('')
  const [chatDataMessage, setChatDataMessage] = useState<{ tone: 'success' | 'warning' | 'error' | 'info'; text: string } | null>(null)
  const [selectedSection, setSelectedSection] = useState<SettingsSectionId>('appearance')
  const memoryRef = useRef<MemoryLibraryHandle>(null)
  const shellRef = useRef<ShellActionsHandle>(null)
  const [rawEditorNames, setRawEditorNames] = useState<Record<string, string>>({})
  const [rawExtensions, setRawExtensions] = useState<Record<string, string>>({})
  const [editorExit, setEditorExit] = useState<(() => void) | null>(null)
  const [editorError, setEditorError] = useState('')
  const editorSavePending = useRef(false)
  const [editorSaving, setEditorSaving] = useState(false)
  const mcpRevision = useRef(0)
  const mcpSavePending = useRef(false)
  const [browserSetupProvider, setBrowserSetupProvider] = useState<string | null>(null)
  const [browserSetupSaved, setBrowserSetupSaved] = useState(false)
  const [themeStep, setThemeStep] = useState(0)
  const [themeSaving, setThemeSaving] = useState(false)
  const [themeExit, setThemeExit] = useState<(() => void) | null>(null)
  const [discardExit, setDiscardExit] = useState<(() => void) | null>(null)
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
    if (selectedSection === 'memory-store' && memoryRef.current) memoryRef.current.requestLeave(next)
    else if (selectedSection === 'shell-actions' && shellRef.current) shellRef.current.requestLeave(next)
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
    if (mcpSavePending.current || editorSavePending.current || remoteBusy) return
    requestThemeExit(() => requestSectionExit(() => {
      if (editorDirty) { setEditorExit(() => () => { setRawEditorNames({}); setRawExtensions({}); if (mcpDraftDirty) { setDiscardExit(() => () => setSettingsOpen(false)); setConfirmDiscard(true) } else setSettingsOpen(false) }); return }
      if (mcpDraftDirty) { setDiscardExit(() => () => setSettingsOpen(false)); setConfirmDiscard(true) }
      else setSettingsOpen(false)
    }))
  }
  const changeSection = (section: SettingsSectionId) => {
    if (section === selectedSection || mcpSavePending.current || editorSavePending.current || remoteBusy) return
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
    setRemoteBusy(true)
    setRemoteMessage('')
	setRemoteCustomDirectory(Boolean(host.runnerDirectory))
	setRemoteDirectory(host.runnerDirectory || 'uam-helper')
    const response = await sendToCEF<{ host: ExecutionHost; preview: string }>({ action: 'previewRemoteHost', payload: host })
    setRemoteBusy(false)
    if (!response.ok || !response.data) {
      setRemoteMessage(response.error || 'The remote setup preview could not be created.')
      return
    }
    setRemotePreview(response.data)
  }

  const installRemoteHost = async () => {
    if (!remotePreview || remoteBusy) return
	const runnerDirectory = remoteCustomDirectory ? remoteDirectory.trim() : ''
	if (remoteCustomDirectory && runnerDirectoryError(remoteDirectory)) return
    setRemoteBusy(true)
    setRemoteMessage('Connecting, verifying the copied helper, and starting the runner…')
    const response = await sendToCEF({ action: 'installRemoteHost', payload: { ...remotePreview.host, runnerDirectory } })
    setRemoteBusy(false)
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

  const removeRemoteHost = async (host: ExecutionHost) => {
    if (remoteBusy) return
    setRemoteBusy(true)
    setRemoteMessage('')
    const response = await sendToCEF({ action: 'removeRemoteHost', payload: { id: host.id } })
    setRemoteBusy(false)
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
    if (!mcpDraftDirty) setMcpDraft(JSON.stringify(mcpServers, null, 2))
  }, [mcpDraftDirty, mcpServers])

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
      approvalMode: saved?.approvalMode ?? 'default',
      commandSafetyTier: saved?.commandSafetyTier ?? 'off',
      memoryLevel: saved?.memoryLevel ?? memoryLevelDefault,
      memoryEnabled: saved?.memoryEnabled ?? memoryEnabledDefault,
      smallModelMode: saved?.smallModelMode ?? false,
      reasoningEffort: saved?.reasoningEffort ?? '',
      serviceTier: saved?.serviceTier ?? '',
    }
  }

  const updateProviderDefaults = (providerId: string, defaults: ProviderChatDefaults) => {
    void setProviderChatDefaults({
      providerChatDefaults: {
        ...providerChatDefaults,
        [providerId]: defaults,
      },
    })
  }

  const toggleDefaultProvider = (providerId: string) => {
    setExpandedDefaultProviders((current) => ({
      ...current,
      [providerId]: !(current[providerId] ?? false),
    }))
  }

  const toggleCliVersionProvider = (providerId: string) => {
    setOpenCliVersionMenu(null)
    setExpandedCliVersionProviders((current) => ({
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
          icon: renderOptionIcon?.(option),
        }))}
        onChange={onSelect}
      />
    )
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
            boxShadow: '0 14px 42px rgba(0, 0, 0, 0.28)',
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
                {selected && <Check size={13} style={{ color: 'var(--green)' }} aria-hidden />}
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
    if (!themeDraft || !themeDraftValid || themeSaveInFlightRef.current) return false
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
  const parseMcpDraft = (): McpServerConfiguration[] | null => {
    try {
      const parsed: unknown = JSON.parse(mcpDraft)
      if (!Array.isArray(parsed)) { setMcpMessage('MCP server configuration must be a JSON array.'); return null }
      return parsed as McpServerConfiguration[]
    } catch { setMcpMessage('Enter valid JSON.'); return null }
  }
  const saveMcpConfiguration = async (servers: McpServerConfiguration[], browserSetup = false) => {
    if (mcpSavePending.current) return
    mcpSavePending.current = true
    const submittedRevision = mcpRevision.current
    setMcpSaving(true)
    setMcpMessage('')
    setBrowserSetupSaved(false)
    try {
      const result = await setMcpServers(servers)
      if (!result.ok) { setMcpMessage(result.error || 'MCP server configuration was rejected.'); return }
      const newerEdits = submittedRevision !== mcpRevision.current
      if (!newerEdits) { setMcpDraft(JSON.stringify(servers,null,2)); setMcpDraftDirty(false) }
      if (browserSetup) setBrowserSetupSaved(true)
      setMcpMessage(newerEdits ? 'Submitted configuration saved; newer edits remain unsaved.' : browserSetup ? 'Browser control configured. Start a new local structured chat to launch the tools.' : 'MCP server configuration saved.')
    } catch { setMcpMessage('MCP server configuration could not be saved. Try again.') }
    finally { mcpSavePending.current = false; setMcpSaving(false) }
  }
  const renderMcpSave = () => <IconButton icon={<Save size={15}/>} label="Save MCP server configuration" disabled={mcpSaving} aria-busy={mcpSaving || undefined} onClick={() => { const servers = parseMcpDraft(); if (servers) void saveMcpConfiguration(servers) }}/>
  const configureBrowserControl = () => {
    const configured = parseMcpDraft()
    if (!configured || !mcpWorkspace.trim() || !mcpExecutable.trim()) return
    // Settings are workspace-wide. ACP sessions resolve these entries when they start.
    let suffix = configured.length + 1
    while (configured.some(server => server?.id === `playwright-browser-${suffix}`)) suffix += 1
    const server: McpServerConfiguration = {
      id:`playwright-browser-${suffix}`,name:'Playwright browser control',workspaceDirectory:mcpWorkspace.trim(),
      transport:'stdio',command:mcpExecutable.trim(),args:['-y','@playwright/mcp@latest','--isolated'],
      url:'',environment:[],headers:[],enabled:true,
    }
    void saveMcpConfiguration([...configured,server],true)
  }

  const renderAddEditor = () => (
              <IconButton
                icon={<Plus size={15} />}
                label="Add editor group"
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
              />
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
                        className="px-2 py-1.5"
                        style={{ border: '1px solid var(--border)', borderRadius: 6, background: 'var(--bg)', color: 'var(--text)' }}
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
                            className="w-20 rounded px-2 py-1 font-mono uppercase transition-colors duration-150 focus:outline-none"
                            style={{ border: '1px solid var(--border)', background: 'var(--bg)', color: 'var(--text)' }}
                          />
                        </span>
                      </label>
                    ))}
                  </div>}
                  {!themeDraftValid && (
                    <p id="theme-color-format-error" role="alert" className="text-xs" style={{ color: 'var(--red)' }}>
                      Every color must use #RRGGBB format.
                    </p>
                  )}
                  {themeStep === 2 && <div className="grid gap-3">
                    <div className="text-sm">{themeDraft.name} · {themeDraft.base}</div>
                    <div className="grid gap-2 sm:grid-cols-2 lg:grid-cols-3">{THEME_COLOR_FIELDS.map(({key,label}) => <div key={key} className="flex items-center gap-2 text-xs"><span aria-hidden className="w-4 h-4 border" style={{background:themeDraft.colors[key]}}/>{label}<code>{themeDraft.colors[key]}</code></div>)}</div>
                  </div>}
                  <div className="flex gap-2">
                    {themeStep > 0 && <Button size="sm" disabled={themeSaving} onClick={() => setThemeStep(themeStep - 1)}>Back</Button>}
                    {themeStep < 2 ? <Button size="sm" disabled={themeStep === 0 ? !themeDraft.name.trim() : !themeDraftValid} onClick={() => setThemeStep(themeStep + 1)}>Next</Button> : <Button size="sm" variant="primary" disabled={!themeDraftValid || themeSaving} aria-busy={themeSaving || undefined} onClick={() => void saveThemeDraft()}>Save theme</Button>}
                    <Button size="sm" disabled={themeSaving} onClick={() => requestThemeExit(() => setThemeDraft(null))}>Cancel</Button>
                  </div>
                </div>
  ) : null

  const renderSectionContent = () => {
    if (selectedSection === 'appearance') {
      const themeOptions: Array<{ value: StoredTheme; label: string }> = [
        ...BUILT_IN_THEMES.map(({ id, label }) => ({ value: id, label })),
        ...customThemes.map((customTheme) => ({ value: customTheme.id, label: customTheme.name })),
      ]
      return (
        <div className="space-y-4">
          <SectionCard
            title="Appearance"
          >
            <div className="flex flex-wrap items-end gap-2">
              <div className="grid gap-1 text-xs min-w-40 flex-1" style={{ color: 'var(--text-2)' }}>
                <span>Theme</span>
                <MenuSelect
                  label="Theme"
                  value={theme}
                  options={themeOptions}
                  onChange={(value) => {
                    setThemeDraft(null)
                    setTheme(value as StoredTheme)
                    setThemeMessage('')
                  }}
                />
              </div>
              <div className="flex flex-wrap gap-1">
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
              {themeMessage && <div role="status" className="text-xs" style={{ color: themeMessage.includes('failed') || themeMessage.includes('could not') ? 'var(--red)' : 'var(--text-2)' }}>{themeMessage}</div>}
            </div>
          </SectionCard>
          <SectionCard title="Sidebar">
            <div className="grid gap-3">
              <Switch
                label="Show provider icons in sidebar"
                checked={showProviderIconsInSidebar}
                onChange={(event) => void setSidebarSettings({ showProviderIconsInSidebar: event.target.checked, showWorktreePathInSidebar })}
              />
              <Switch
                label="Show worktree path in sidebar"
                checked={showWorktreePathInSidebar}
                onChange={(event) => void setSidebarSettings({ showProviderIconsInSidebar, showWorktreePathInSidebar: event.target.checked })}
              />
            </div>
          </SectionCard>
          <SectionCard title="Chat activity">
            <Switch
              label="Compact working"
              checked={workingDisplayMode === 'compact'}
              onChange={(event) => setWorkingDisplayMode(event.target.checked ? 'compact' : 'verbose')}
            />
          </SectionCard>
        </div>
      )
    }

    if (selectedSection === 'defaults') {
      const reviewerProvider = providers.find((provider) => provider.id === permissionReviewerProviderId)
      const reviewerSession = reviewerProvider ? latestProviderSession(sessions, reviewerProvider.id) : undefined
      const reviewerWorkspace = reviewerSession?.workspaceDirectory || folders[0]?.directory || ''
      const reviewerAcp = reviewerProvider
        ? (reviewerSession ? acpBindings[reviewerSession.id] : undefined)
          ?? providerModelCatalogs.find((catalog) => catalog.providerId === reviewerProvider.id && workspaceKey(catalog.workspaceDirectory) === workspaceKey(reviewerWorkspace))
        : undefined
      const reviewerModels = reviewerProvider
        ? memoryModelOptions(reviewerProvider, reviewerProvider.id, permissionReviewerModelId, reviewerAcp?.availableModels)
        : []
      return (
        <div className="space-y-4">
          <SectionCard
            title="AI Permission Reviewer"
          >
            <div className="grid gap-3 sm:grid-cols-2 text-xs" style={{ color: 'var(--text-2)' }}>
              <div className="grid gap-1">
                <div>Reviewer provider</div>
                {renderDefaultsMenu(
                  'permission-reviewer-provider',
                  permissionReviewerProviderId,
                  'AI permission reviewer provider',
                  [
                    { id: '', label: 'Not configured', detail: 'AI Review always falls back to you.' },
                    ...providers.map((provider) => ({ id: provider.id, label: providerDisplayName(provider, provider.id), detail: provider.name ?? provider.id })),
                  ],
                  (providerId) => void setMemorySettings({ permissionReviewerProviderId: providerId, permissionReviewerModelId: '' }),
                  (option) => option.id ? <ProviderLogo providerId={option.id} /> : null
                )}
              </div>
              <div className="grid gap-1">
                <div>Reviewer model</div>
                {renderDefaultsMenu(
                  'permission-reviewer-model',
                  permissionReviewerModelId,
                  'AI permission reviewer model',
                  reviewerModels.length > 0 ? reviewerModels : [{ id: '', label: 'Provider default', detail: reviewerProvider ? 'Use the provider default model.' : 'Choose a reviewer provider first.' }],
                  (modelId) => void setMemorySettings({ permissionReviewerModelId: modelId })
                )}
              </div>
            </div>
          </SectionCard>
          <SectionCard
            title="New Chat Defaults"
          >
            <div className="grid gap-4">
              <div className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
                <div>Default provider</div>
                {renderDefaultsMenu(
                  'default-provider',
                  defaultNewChatProviderId || providers[0]?.id || DEFAULT_PROVIDER_ID,
                  'Default provider',
                  providers.map((provider) => ({
                    id: provider.id,
                    label: providerDisplayName(provider, provider.id),
                    detail: provider.name ?? provider.id,
                  })),
                  (providerId) => void setProviderChatDefaults({ defaultNewChatProviderId: providerId }),
                  (option) => <ProviderLogo providerId={option.id} />
                )}
              </div>

              <div className="space-y-3">
                {providers.map((provider) => {
                  const defaults = defaultsForProvider(provider)
                  const caps = providerCapabilities(provider.id, provider)
                  const providerName = providerDisplayName(provider, provider.id)
                  const providerSession = latestProviderSession(sessions, provider.id)
				  const providerWorkspace = providerSession?.workspaceDirectory || folders[0]?.directory || ''
				  const providerAcp = (providerSession ? acpBindings[providerSession.id] : undefined)
				    ?? providerModelCatalogs.find((catalog) => catalog.providerId === provider.id && workspaceKey(catalog.workspaceDirectory) === workspaceKey(providerWorkspace))
                  const modelsLoading = providerAcp?.modelsLoading ?? false
                  const modelRefreshError = providerAcp?.modelRefreshError ?? ''
                  const modelOptions = buildModelOptions(providerAcp, defaults.modelId, provider, provider.id, true)
                  const reviewerModelOptions = buildModelOptions(providerAcp, defaults.reviewerModelId || defaults.modelId, provider, provider.id, true)
                  const runtimeSupportsReasoning = (selectedRuntimeModel(providerAcp, defaults.modelId)?.supportedReasoningEfforts?.length ?? 0) > 0
                  const liveReasoningOptions = caps.hasReasoningEffort || runtimeSupportsReasoning
                    ? buildCodexReasoningOptions(providerAcp, defaults.modelId, defaults.reasoningEffort, caps.reasoningOptions.map((option) => option.id))
                    : []
                  const defaultReasoningOption = caps.reasoningOptions.find((option) => !option.id)
                  const reasoningOptions = defaultReasoningOption && !liveReasoningOptions.some((option) => !option.id)
                    ? [defaultReasoningOption, ...liveReasoningOptions]
                    : liveReasoningOptions
                  const speedOptions = buildCodexSpeedOptions(providerAcp, defaults.modelId, defaults.serviceTier)
                  const expanded = expandedDefaultProviders[provider.id] ?? false
                  const modeOptions = [
                    { id: 'default', label: 'Default', detail: 'Use the provider default mode' },
                    { id: 'plan', label: 'Plan', detail: 'Ask the provider to plan first' },
                  ]
                  return (
                    <ProviderDisclosureCard
                      key={provider.id}
                      panelId={`${provider.id}-defaults-panel`}
                      providerId={provider.id}
                      title={providerName}
                      expanded={expanded}
                      toggleLabel={`${expanded ? 'Hide' : 'Show'} ${providerName} chat defaults`}
                      onToggle={() => toggleDefaultProvider(provider.id)}
                      actions={<IconButton icon={<RefreshCw size={14}/>} label={`Refresh ${providerName} models`} disabled={!providerWorkspace || modelsLoading} onClick={() => void discoverProviderModels(providerSession?.id ?? '', provider.id, providerWorkspace)} />}
                    >
                      <div className="grid gap-3 text-xs" style={{ color: 'var(--text-2)' }}>
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
                          {(caps.hasReasoningEffort || runtimeSupportsReasoning) && (
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
                          {caps.hasServiceTier && (
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
                            {renderDefaultsMenu(
                              `${provider.id}:memory`,
                              defaults.memoryLevel ?? 'off',
                              `${providerName} default memory level`,
                              MEMORY_LEVEL_OPTIONS,
                              (memoryLevel) => updateProviderDefaults(provider.id, {
                                ...defaults,
                                memoryLevel: memoryLevel as ProviderChatDefaults['memoryLevel'],
                                memoryEnabled: memoryLevel !== 'off',
                              })
                            )}
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
                            New {pendingDefaultYolo.providerName} chats will automatically approve computer use, commands, file changes, and every other permission request.
                          </Notice>
                        )}
                        <details>
                          <summary className="cursor-pointer py-2">Advanced</summary>
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
                          <span style={{ color: 'var(--text-3)' }}>Used for architecture and read-only review; the normal model remains the worker.</span>
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
                          <span style={{ color: 'var(--text-3)' }}>The reviewer model plans and checks one step at a time; the normal model implements it.</span>
                        </div>
                          </div>
                        </details>
                        <div className="flex items-center gap-2">
                          <span role="status" className="text-xs" style={{ color: modelRefreshError ? 'var(--red)' : 'var(--text-3)' }}>
							{modelsLoading ? 'Refreshing models…' : modelRefreshError || (!providerWorkspace ? 'Add a workspace to refresh models' : providerAcp?.availableModels?.length ? 'Model catalog available' : 'Using provider defaults; refresh to discover models')}
                          </span>
                        </div>
                      </div>
                    </ProviderDisclosureCard>
                  )
                })}
              </div>
            </div>
          </SectionCard>
        </div>
      )
    }

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
        if (!agentImportPath.trim()) return
        setAgentImportBusy(true)
        setAgentImportMessage('')
        const preview = await previewProviderAgentImport(agentImportProvider, agentImportPath.trim())
        setAgentImportPreview(preview)
        setAgentImportId(preview?.suggestedId ?? '')
        setAgentImportAcknowledged(false)
        setAgentImportMessage(preview ? '' : 'Preview failed.')
        setAgentImportBusy(false)
      }
      const runImport = async () => {
        if (!activeSessionId || !agentImportPreview?.supported) return
        setAgentImportBusy(true)
        const imported = await importProviderAgent({
          chatId: activeSessionId,
          providerId: agentImportProvider,
          sourcePath: agentImportPath.trim(),
          canonicalId: agentImportId.trim(),
          workspaceAccess: agentImportAccess,
          workspaceScope: agentImportWorkspaceScope,
          acknowledgeIgnoredFields: agentImportAcknowledged,
        })
        setAgentImportMessage(imported ? `Imported ${agentImportId.trim()}.` : 'Import failed. The source or target may have changed; preview it again.')
        if (imported) setAgentImportPreview(null)
        setAgentImportBusy(false)
      }
      return (
        <div className="space-y-4">
          <SectionCard
            title="Composer agents"
          >
            <div className="grid gap-4">
              <label className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
                <span>Agent cycle shortcut</span>
                <MenuSelect
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
              </label>

              <div className="grid gap-2">
                <div className="text-xs" style={{ color: 'var(--text-2)' }}>Ordered favorites</div>
                {favoriteUamAgentIds.length === 0 ? (
                  <div className="text-xs" style={{ color: 'var(--text-3)' }}>No custom favorites. The cycle is Build, then Plan.</div>
                ) : favoriteUamAgentIds.map((agentId, index) => {
                  const available = activeUamAgents.some((agent) => agent.id === agentId)
                  return (
                    <div key={agentId} className="flex items-center gap-2 rounded-md px-2 py-2" style={{ border: '1px solid var(--border)' }}>
                      <span className="min-w-0 flex-1 truncate text-sm">{agentId}</span>
                      {!available && <span className="text-[11px]" style={{ color: 'var(--text-3)' }}>Unavailable here</span>}
                      <Button size="sm" disabled={index === 0} onClick={() => moveFavorite(index, -1)} aria-label={`Move ${agentId} up`}>Up</Button>
                      <Button size="sm" disabled={index === favoriteUamAgentIds.length - 1} onClick={() => moveFavorite(index, 1)} aria-label={`Move ${agentId} down`}>Down</Button>
                      <Button size="sm" variant="danger" onClick={() => updatePreferences(favoriteUamAgentIds.filter((id) => id !== agentId))} aria-label={`Remove ${agentId}`}>Remove</Button>
                    </div>
                  )
                })}
              </div>

              <div className="grid gap-2 sm:grid-cols-[minmax(0,1fr)_auto]">
                <MenuSelect
                  label="Favorite UAM agent"
                  value={selectedCandidate}
                  options={selectableCustomAgents.length > 0
                    ? selectableCustomAgents.map((agent) => ({ value: agent.id, label: agent.id, description: agent.description }))
                    : [{ value: '', label: activeSessionId ? 'No more selectable custom agents' : 'Open a chat to list workspace agents' }]}
                  onChange={setFavoriteAgentCandidate}
                  disabled={selectableCustomAgents.length === 0}
                />
                <Button
                  size="sm"
                  disabled={!selectedCandidate}
                  onClick={() => {
                    updatePreferences([...favoriteUamAgentIds, selectedCandidate])
                    setFavoriteAgentCandidate('')
                  }}
                >Add favorite</Button>
              </div>
            </div>
          </SectionCard>
          <SectionCard
            title="Import provider agent"
          >
            <div className="grid gap-3">
              <MenuSelect
                label="Source provider"
                value={agentImportProvider}
                options={[
                  { value: 'opencode-cli', label: 'OpenCode' },
                  { value: 'copilot-cli', label: 'GitHub Copilot CLI' },
                  { value: 'gemini-cli', label: 'Gemini CLI' },
				  { value: 'claude-cli', label: 'Claude Code' },
                ]}
                onChange={(value) => {
                  setAgentImportProvider(value)
                  setAgentImportPreview(null)
                  setAgentImportMessage('')
                }}
              />
              <label className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
                <span>Native Markdown file</span>
                <span className="flex gap-2">
                  <input
                    aria-label="Native agent Markdown file"
                    value={agentImportPath}
                    onChange={(event) => {
                      setAgentImportPath(event.target.value)
                      setAgentImportPreview(null)
                      setAgentImportMessage('')
                    }}
                    className="min-w-0 flex-1 rounded px-2 py-1.5 focus:outline-none"
                    style={{ border: '1px solid var(--border)', background: 'var(--bg)', color: 'var(--text)' }}
                  />
                  <Button size="sm" onClick={() => void browseProviderAgentImport(agentImportPath).then((path) => {
                    if (!path) return
                    setAgentImportPath(path)
                    setAgentImportPreview(null)
                    setAgentImportMessage('')
                  })}>Browse</Button>
                  <Button size="sm" aria-busy={agentImportBusy || undefined} disabled={agentImportBusy || !agentImportPath.trim()} onClick={() => void previewImport()}>Preview</Button>
                </span>
              </label>

              {agentImportPreview && (
                <div className="grid gap-2 rounded-md border p-3 text-xs" style={{ borderColor: 'var(--border)', background: 'var(--bg)' }}>
                  <div><strong>{agentImportPreview.description || agentImportPreview.suggestedId || 'Provider agent'}</strong> · {agentImportPreview.mode || 'unknown mode'}</div>
                  {agentImportPreview.securityFields.length > 0 && (
                    <div role="alert" style={{ color: 'var(--red)' }}>Blocked security/tool fields: {agentImportPreview.securityFields.join(', ')}</div>
                  )}
                  {agentImportPreview.ignoredFields.length > 0 && (
                    <div style={{ color: 'var(--text-2)' }}>Provider-only fields that will be omitted: {agentImportPreview.ignoredFields.join(', ')}</div>
                  )}
                  {agentImportPreview.error && <div role="alert" style={{ color: 'var(--red)' }}>{agentImportPreview.error}</div>}
                  {agentImportPreview.supported && (
                    <>
                      <label className="grid gap-1">
                        <span>UAM agent ID</span>
                        <input
                          aria-label="Imported UAM agent ID"
                          value={agentImportId}
                          onChange={(event) => setAgentImportId(event.target.value)}
                          className="rounded px-2 py-1.5 focus:outline-none"
                          style={{ border: '1px solid var(--border)', background: 'var(--surface)', color: 'var(--text)' }}
                        />
                      </label>
                      <div className="grid gap-2 sm:grid-cols-2">
                        <MenuSelect
                          label="Workspace access"
                          value={agentImportAccess}
                          options={[
                            { value: 'read', label: 'Read only', description: 'The imported agent cannot approve writes.' },
                            { value: 'write', label: 'Read and write', description: 'The imported agent may request workspace changes.' },
                          ]}
                          onChange={(value) => setAgentImportAccess(value === 'write' ? 'write' : 'read')}
                        />
                        <MenuSelect
                          label="Install scope"
                          value={agentImportWorkspaceScope ? 'workspace' : 'global'}
                          options={[
                            { value: 'workspace', label: 'This workspace' },
                            { value: 'global', label: 'All workspaces' },
                          ]}
                          onChange={(value) => setAgentImportWorkspaceScope(value === 'workspace')}
                        />
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
                      <Button
                        size="sm"
                        variant="primary"
                        aria-busy={agentImportBusy || undefined}
                        disabled={agentImportBusy || !activeSessionId || !agentImportId.trim() || (agentImportPreview.ignoredFields.length > 0 && !agentImportAcknowledged)}
                        onClick={() => void runImport()}
                      >Import agent</Button>
                    </>
                  )}
                </div>
              )}
              {agentImportMessage && <div role="status" className="text-xs" style={{ color: agentImportMessage.startsWith('Imported') ? 'var(--green)' : 'var(--red)' }}>{agentImportMessage}</div>}
              {!activeSessionId && <div className="text-xs" style={{ color: 'var(--text-3)' }}>Open a workspace chat before importing.</div>}
            </div>
          </SectionCard>
        </div>
      )
    }

    if (selectedSection === 'cli-version') {
      return (
        <div className="space-y-4">
          <SectionCard
            title="Provider CLIs"
          >
            <div ref={cliVersionMenuRef} className="space-y-3">
              {cliVersionManager.providers.length === 0 && (
                <div className="text-xs" style={{ color: 'var(--text-3)' }}>
                  No managed CLI providers are available in this build.
                </div>
              )}
              {cliVersionManager.providers.map((manager) => {
                const cliVersionProvider = providers.find((provider) => provider.id === manager.providerId)
                const providerName = providerDisplayName(cliVersionProvider, manager.providerId)
                const expanded = expandedCliVersionProviders[manager.providerId] ?? false
                const issues = [...cliVersionIssues(manager), ...(cliActionError[manager.providerId] ? [cliActionError[manager.providerId]] : [])]
                const requestInstall = (version: string) => {
                  setOpenCliVersionMenu(null)
                  setExpandedCliVersionProviders(current => ({...current,[manager.providerId]:true}))
                  setPendingCliInstall({providerId:manager.providerId,providerName,version})
                }
                return (
                  <ProviderDisclosureCard
                    key={manager.providerId}
                    togglePosition="right"
                    panelId={`${manager.providerId}-cli-version-panel`}
                    providerId={manager.providerId}
                    title={providerName}
                    expanded={expanded}
                    toggleLabel={`${expanded ? 'Hide' : 'Show'} ${providerName} CLI version settings`}
                    onToggle={() => toggleCliVersionProvider(manager.providerId)}
                    actions={<StatusIndicator issues={issues} okLabel={manager.status === 'provider-managed' ? 'Provider managed' : 'CLI verified'}/>}
                  >
                    <div className="flex items-start justify-between gap-3">
                    <div className="grid gap-1 text-xs" style={{color:'var(--yellow)'}}>{issues.map(issue => <div key={issue}>{issue}</div>)}</div>
                      <div className="flex items-center gap-1">
                      <IconButton icon={<RefreshCw size={15}/>} label={`Refresh ${providerName} CLI version`} disabled={manager.running} onClick={async () => {
                        setCliActionError(current => ({...current,[manager.providerId]:''}))
                        try { if (!await refreshCliProviderVersion(manager.providerId)) setCliActionError(current => ({...current,[manager.providerId]:'Version check could not start.'})) }
                        catch { setCliActionError(current => ({...current,[manager.providerId]:'Version check could not start.'})) }
                      }}/>
                      <IconButton ref={element => { if (element) popupAnchorsRef.current.set(`${manager.providerId}:download`,element) }} icon={<Download size={15}/>} label={`Download ${providerName} CLI`} aria-haspopup="menu" aria-expanded={openCliVersionMenu === `${manager.providerId}:download`} disabled={manager.running} onClick={() => setOpenCliVersionMenu(current => current === `${manager.providerId}:download` ? null : `${manager.providerId}:download`)}/>
                      {openCliVersionMenu === `${manager.providerId}:download` && <ViewportMenu anchorRef={{current:popupAnchorsRef.current.get(`${manager.providerId}:download`) ?? null}} role="menu" aria-label={`${providerName} download choices`} className="grid gap-1 p-2" style={{background:'var(--surface)',border:'1px solid var(--border)',borderRadius:8}}>
                        <Button size="sm" role="menuitem" disabled={!manager.preferredVersion} onClick={() => requestInstall(manager.preferredVersion)}>Download recommended{manager.preferredVersion ? ` ${manager.preferredVersion}` : ''}</Button>
                        <Button size="sm" role="menuitem" onClick={() => requestInstall('latest')}>Download latest</Button>
                      </ViewportMenu>}
                      </div>
                    </div>
                    <div className="grid grid-cols-2 gap-3 mt-3 text-sm">
                      <div><div className="text-xs" style={{color:'var(--text-3)'}}>Installed</div>{manager.installedVersion || 'Not detected'}</div>
                      <div><div className="text-xs" style={{color:'var(--text-3)'}}>Recommended</div>{manager.preferredVersion || 'Unavailable'}</div>
                    </div>
                    {pendingCliInstall?.providerId === manager.providerId && <div className="flex flex-wrap items-center gap-3 mt-3 text-xs" role="group" aria-label="Confirm CLI installation">
                      <span>Install {pendingCliInstall.providerName} {pendingCliInstall.version} globally using {manager.installMethod || 'npm'}?</span>
                      <Button size="sm" onClick={() => setPendingCliInstall(null)}>Cancel</Button>
                      <Button size="sm" disabled={manager.running} onClick={async () => {
                        const pending = pendingCliInstall
                        setPendingCliInstall(null)
                        setCliActionError(current => ({...current,[manager.providerId]:''}))
                        try { if (!await applyCliProviderVersion(pending.providerId,pending.version)) setCliActionError(current => ({...current,[manager.providerId]:'Installation could not start. Check provider output.'})) }
                        catch { setCliActionError(current => ({...current,[manager.providerId]:'Installation could not start. Check provider output.'})) }
                      }}>Install version</Button>
                    </div>}

                    {(manager.lastCommand || manager.lastOutput) && (
                      <div className="grid gap-2 mt-3">
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
                  </ProviderDisclosureCard>
                )
              })}
            </div>
          </SectionCard>
        </div>
      )
    }

    if (selectedSection === 'remote-hosts') {
      const remoteHosts = executionHosts.filter((host) => host.id !== 'local')
      return (
        <div className="space-y-4">
          <SectionCard
            title="Remote execution hosts"
          >
            <div className="grid gap-4">
              <Notice tone="info" title="Remote Computer Use is disabled" dismissLabel="Dismiss remote Computer Use notice">
                Remote chats retain structured chat, permissions, cancellation, and provider controls. Screen observation and input stay disabled because UAM cannot safely supervise a remote desktop.
              </Notice>
              <div className="grid gap-3 sm:grid-cols-2">
                <label className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
                  Display name
                  <input
                    aria-label="Remote host display name"
                    value={remoteLabel}
                    maxLength={128}
                    placeholder="AI desktop"
                    onChange={(event) => { setRemoteLabel(event.target.value); setRemotePreview(null); setRemoteMessage('') }}
                    className="rounded-lg px-3 py-2 outline-none"
                    style={{ color: 'var(--text)', background: 'var(--bg)', border: '1px solid var(--border)' }}
                  />
                </label>
                <label className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
                  SSH config alias
                  <input
                    aria-label="SSH config alias"
                    value={remoteAlias}
                    maxLength={255}
                    placeholder="ai-desktop"
                    spellCheck={false}
                    onChange={(event) => { setRemoteAlias(event.target.value); setRemotePreview(null); setRemoteMessage('') }}
                    className="rounded-lg px-3 py-2 font-mono outline-none"
                    style={{ color: 'var(--text)', background: 'var(--bg)', border: '1px solid var(--border)' }}
                  />
                </label>
              </div>
              <div className="flex justify-end">
                <Button size="sm" leadingIcon={<Server size={14} />} aria-busy={remoteBusy || undefined} disabled={remoteBusy || !remoteAlias.trim()} onClick={() => void previewRemoteHost()}>
                  Preview setup
                </Button>
              </div>
              {remoteMessage && <div role="status" className="text-xs" style={{ color: remoteMessage.endsWith('ready.') || remoteMessage.includes('removed') ? 'var(--green)' : 'var(--text-2)' }}>{remoteMessage}</div>}
            </div>
          </SectionCard>

          <SectionCard title="Configured hosts">
            <div className="grid gap-3">
              {remoteHosts.length === 0 && <div className="text-xs" style={{ color: 'var(--text-3)' }}>No remote hosts configured.</div>}
              {remoteHosts.map((host) => (
                <div key={host.id} className="flex items-start justify-between gap-4 rounded-lg p-3" style={{ border: '1px solid var(--border)', background: 'var(--bg)' }}>
                  <div className="min-w-0 text-xs">
                    <div className="font-medium" style={{ color: 'var(--text)' }}>{host.label}</div>
                    <div className="mt-1 font-mono" style={{ color: 'var(--text-2)' }}>{host.sshAlias}</div>
                    <div className="mt-1" style={{ color: host.runnerStatus === 'ready' ? 'var(--green)' : host.runnerStatus === 'error' ? 'var(--red)' : 'var(--text-3)' }}>
                      {host.runnerStatus}{host.runnerVersion ? ` · runner ${host.runnerVersion}` : ''}{host.platform ? ` · ${host.platform} ${host.architecture}` : ''}
                    </div>
					<div className="mt-1" style={{ color: 'var(--text-3)' }}>Helper: home / {host.runnerDirectory || (host.platform === 'windows' ? '.uam/runner' : '.local/share/uam/runner')}</div>
                  </div>
                  <div className="flex shrink-0 gap-1">
                    <IconButton icon={<RefreshCw size={14} />} label={`Reinstall helper on ${host.label}`} disabled={remoteBusy} onClick={() => void previewRemoteHost(host)} />
                    <IconButton icon={<Trash2 size={14} />} label={`Remove ${host.label}`} variant="danger" disabled={remoteBusy} onClick={() => void removeRemoteHost(host)} />
                  </div>
                </div>
              ))}
            </div>
          </SectionCard>
        </div>
      )
    }

    if (selectedSection === 'memory-settings') {
      return (
        <div className="space-y-4">
          <SectionCard
            title="Memory Defaults"
          >
            <div className="space-y-3">
              <div>
                <div className="mb-2 flex items-end justify-between gap-4">
                  <div className="text-sm" style={{ color: 'var(--text)' }}>Default memory level</div>
                  <div className="text-xs" style={{ color: 'var(--text-3)' }}>{MEMORY_LEVEL_OPTIONS.find((option) => option.id === memoryLevelDefault)?.detail}</div>
                </div>
                <div role="radiogroup" aria-label="Default memory level" className="grid grid-cols-4 overflow-hidden rounded-lg" style={{ border: '1px solid var(--border)' }}>
                  {MEMORY_LEVEL_OPTIONS.map((option) => {
                    const selected = option.id === memoryLevelDefault
                    return <button key={option.id} type="button" role="radio" aria-checked={selected} onClick={() => void setMemorySettings({ memoryLevelDefault: option.id })} className="uam-segment-button px-3 py-2 text-xs" style={{ border: 0, borderRight: option.id === 'open' ? 0 : '1px solid var(--border)', background: selected ? 'var(--accent-dim)' : 'transparent', color: selected ? 'var(--accent)' : 'var(--text-2)' }}>{option.label}</button>
                  })}
                </div>
              </div>

              <div className="grid grid-cols-2 gap-3">
                <label className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
                  Idle delay (seconds)
                  <input
                    type="number"
                    min={MIN_MEMORY_IDLE_DELAY_SECONDS}
                    max={MAX_MEMORY_IDLE_DELAY_SECONDS}
                    value={memoryIdleDelaySeconds}
                    onChange={(event) => void setMemorySettings({ memoryIdleDelaySeconds: Number(event.currentTarget.value) })}
                    style={{
                      background: 'var(--surface)',
                      color: 'var(--text)',
                      border: '1px solid var(--border)',
                      borderRadius: 8,
                      padding: '8px 10px',
                    }}
                  />
                </label>

                <label className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
                  Recall budget (bytes)
                  <input
                    type="number"
                    min={MIN_MEMORY_RECALL_BUDGET_BYTES}
                    max={MAX_MEMORY_RECALL_BUDGET_BYTES}
                    step={256}
                    value={memoryRecallBudgetBytes}
                    onChange={(event) => void setMemorySettings({ memoryRecallBudgetBytes: Number(event.currentTarget.value) })}
                    style={{
                      background: 'var(--surface)',
                      color: 'var(--text)',
                      border: '1px solid var(--border)',
                      borderRadius: 8,
                      padding: '8px 10px',
                    }}
                  />
                </label>
              </div>
            </div>
          </SectionCard>

          <SectionCard
            title="Memory Workers"
          >
            <div ref={memoryMenuRef} className="space-y-3">
              {providers.map((provider) => {
                const binding = memoryWorkerBindings[provider.id] ?? { workerProviderId: provider.id, workerModelId: '' }
                const workerProvider = providers.find((candidate) => candidate.id === binding.workerProviderId) ?? provider
                const workerSession = latestProviderSession(sessions, binding.workerProviderId)
                const workerWorkspace = workerSession?.workspaceDirectory || folders[0]?.directory || ''
                const workerAcp = (workerSession ? acpBindings[workerSession.id] : undefined)
                  ?? providerModelCatalogs.find((catalog) => catalog.providerId === binding.workerProviderId && workspaceKey(catalog.workspaceDirectory) === workspaceKey(workerWorkspace))
                const providerMenuId = `${provider.id}:provider`
                const modelMenuId = `${provider.id}:model`
                const modelOptions = memoryModelOptions(workerProvider, binding.workerProviderId, binding.workerModelId, workerAcp?.availableModels)
                return (
                  <div
                    key={provider.id}
                    className="grid gap-2 rounded-lg p-3 text-xs"
                    style={{
                      color: 'var(--text-2)',
                      background: 'var(--surface)',
                      border: '1px solid var(--border)',
                    }}
                  >
                    <div>{providerDisplayName(provider, provider.id)} memory worker</div>
                    <div className="grid grid-cols-2 gap-2">
                      <div className="relative">
                        <button
                          ref={(element) => { if (element) popupAnchorsRef.current.set(providerMenuId, element) }}
                          type="button"
                          title={`${providerDisplayName(provider, provider.id)} memory worker provider`}
                          aria-haspopup="listbox"
                          aria-expanded={openMemoryMenu === providerMenuId}
                          onClick={() => setOpenMemoryMenu(openMemoryMenu === providerMenuId ? null : providerMenuId)}
                          className="w-full text-left"
                          style={{
                            background: 'var(--surface-up)',
                            color: 'var(--text)',
                            border: '1px solid var(--border)',
                            borderRadius: 8,
                            padding: '8px 10px',
                          }}
                        >
                          <span className="inline-flex items-center gap-2">
                            <ProviderLogo providerId={binding.workerProviderId} />
                            <span>{providerDisplayName(workerProvider, binding.workerProviderId)}</span>
                          </span>
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
                              boxShadow: '0 14px 42px rgba(0, 0, 0, 0.28)',
                              padding: 6,
                            }}
                          >
                            {providers.map((candidate) => {
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
                                  {selected && <Check size={13} style={{ color: 'var(--green)' }} aria-hidden />}
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
                          className="w-full text-left"
                          style={{
                            background: 'var(--surface-up)',
                            color: 'var(--text)',
                            border: '1px solid var(--border)',
                            borderRadius: 8,
                            padding: '8px 10px',
                          }}
                        >
                          {selectedMemoryModelLabel(modelOptions, binding.workerModelId)}
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
                              boxShadow: '0 14px 42px rgba(0, 0, 0, 0.28)',
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
                                    {selected && <Check size={13} style={{ color: 'var(--green)' }} aria-hidden />}
                                  </span>
                                  <span className="text-[11px]" style={{ color: 'var(--text-3)' }}>{option.detail}</span>
                                </button>
                              )
                            })}
                          </ViewportMenu>
                        )}
                      </div>
                    </div>
                  </div>
                )
              })}
            </div>

            {memoryLastStatus && (
              <div className="text-xs mt-3" style={{ color: 'var(--text-3)' }}>{memoryLastStatus}</div>
            )}

            {hasWorkerLog && (
              <div
                className="mt-3 rounded-lg overflow-hidden"
                style={{
                  background: workerLogIsFailure ? 'color-mix(in srgb, var(--red) 7%, var(--surface))' : 'var(--surface)',
                  border: `1px solid ${workerLogIsFailure ? 'color-mix(in srgb, var(--red) 35%, var(--border))' : 'var(--border)'}`,
                }}
              >
                <div className="px-3 py-2" style={{ borderBottom: '1px solid var(--border)' }}>
                  <div className="text-xs font-semibold" style={{ color: workerLogIsFailure ? 'var(--red)' : 'var(--text)' }}>
                    Last worker log
                  </div>
                  {memoryActivity.lastWorkerStatus && (
                    <div className="text-[11px] mt-0.5" style={{ color: 'var(--text-3)' }}>
                      {memoryActivity.lastWorkerStatus}
                    </div>
                  )}
                  {workerLogMeta && (
                    <div className="text-[11px] mt-0.5" style={{ color: 'var(--text-3)' }}>
                      {workerLogMeta}
                    </div>
                  )}
                </div>
                <pre
                  className="px-3 py-2 text-[11px] leading-5 overflow-auto"
                  style={{
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
            )}
          </SectionCard>

        </div>
      )
    }

    if (selectedSection === 'voice-input') {
      return (
        <SectionCard title="Voice Input">
          <div role="status" className="rounded-lg p-3 text-xs" style={{ color: 'var(--text-2)', background: 'var(--surface)', border: '1px solid var(--border)' }}>
            Dictation stays on the native system speech service. Start it with the microphone button in the composer.
          </div>
        </SectionCard>
      )
    }

    if (selectedSection === 'memory-store') return <MemoryLibraryModal embedded ref={memoryRef}/>
    if (selectedSection === 'markdown-store') return <div className="h-full min-h-0 flex flex-col gap-3">
      <details className="shrink-0">
        <summary className="cursor-pointer text-xs py-2">Skills folder</summary>
        <div className="flex items-center gap-2">
          <input aria-label="Skills directory" value={markdownStoreDraftDirectory} onChange={event => setMarkdownStoreDraftDirectory(event.target.value)} placeholder="Skills directory" className="min-w-0 flex-1 text-xs" style={{border:'1px solid var(--border)',borderRadius:8,background:'var(--bg)',color:'var(--text)',padding:'8px 10px'}}/>
          <IconButton icon={<FolderOpen size={15}/>} label="Browse for Skills directory" onClick={() => void browseMarkdownStoreDirectory(markdownStoreDraftDirectory).then(selected => { if (selected) setMarkdownStoreDraftDirectory(selected) })}/>
          <IconButton icon={<Save size={15}/>} label="Save Skills directory" disabled={!markdownStoreDraftDirectory.trim() || markdownStoreDraftDirectory.trim() === markdownStoreDirectory} onClick={() => void setMarkdownStoreDirectory(markdownStoreDraftDirectory)}/>
        </div>
      </details>
      <MarkdownStoreModal embedded/>
    </div>

    if (selectedSection === 'goal-loops') {
      return (
        <SectionCard title="Goal Loop Safety">
          <div className="grid gap-4 text-xs" style={{ color: 'var(--text-2)' }}>
            <div className="grid gap-1">
              <span>Maximum iterations</span>
              <div className="flex w-fit items-center overflow-hidden rounded-lg" style={{ background: 'var(--surface)', border: '1px solid var(--border)' }}>
			    <IconButton icon={<Minus size={14} />} label="Decrease maximum goal loop iterations" size="sm" disabled={goalMaxLoopIterations <= 1} onClick={() => void setMemorySettings({ goalMaxLoopIterations: Math.max(1, goalMaxLoopIterations - 1) })} />
			    <output aria-label="Maximum goal loop iterations" className="min-w-16 px-3 text-center tabular-nums" style={{ color: 'var(--text)' }}>{goalMaxLoopIterations}</output>
			    <IconButton icon={<Plus size={14} />} label="Increase maximum goal loop iterations" size="sm" disabled={goalMaxLoopIterations >= 200} onClick={() => void setMemorySettings({ goalMaxLoopIterations: Math.min(200, goalMaxLoopIterations + 1) })} />
              </div>
			  <span style={{ color: 'var(--text-3)' }}>Always bounded between 1 and 200 iterations.</span>
            </div>
            <div className="grid w-72 gap-1">
              <span>ACP setup inactivity timeout</span>
              <MenuSelect
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
              <span style={{ color: 'var(--text-3)' }}>Restarts an ACP provider only after no setup activity for this long.</span>
            </div>
            <div className="grid w-72 gap-1">
              <span>Provider turn output ceiling</span>
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
              <span style={{ color: 'var(--text-3)' }}>Resets for every worker or review turn; startup history is excluded.</span>
            </div>
          </div>
        </SectionCard>
      )
    }

    if (selectedSection === 'editors') {
      return (
        <div className="space-y-4">
          <SectionCard
            title="Workspace Editors"
          >
            <div ref={editorMenuRef} className="grid gap-3">
              <div className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
                <div>Default editor</div>
                {renderEditorPresetMenu(
                  'default-editor',
                  defaultEditorDraft,
                  'Default editor',
                  (presetId) => saveEditorSettings(editorAssociationsDraft, presetId)
                )}
              </div>

              <div className="space-y-3">
                {editorAssociationsDraft.map((association) => {
                  const groupName = association.name.trim() || 'File group'
                  const expanded = expandedEditorGroups[association.id] ?? false
                  return (
                    <ProviderDisclosureCard
                      key={association.id}
                      panelId={`${association.id}-editor-group-panel`}
                      title={groupName}
                      actions={<IconButton icon={<Trash2 size={14}/>} label={`Delete ${groupName} editor group`} variant="danger" size="sm" style={{border:'1px solid var(--border)',background:'var(--surface-up)'}} disabled={editorSaving || editorAssociationsDraft.length <= 1} onClick={() => setPendingDelete({kind:'editor',id:association.id,name:groupName})}/>}
                      expanded={expanded}
                      toggleLabel={`${expanded ? 'Hide' : 'Show'} ${groupName} editor group`}
                      onToggle={() => toggleEditorGroup(association.id)}
                    >
                      <div className="grid gap-2">
                        <div className="grid grid-cols-3 gap-2">
                          <label className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
                            <input
                              aria-label={`${groupName} group name`}
                              placeholder="Group name"
                              value={rawEditorNames[association.id] ?? association.name}
                              disabled={editorSaving}
                              aria-invalid={rawEditorNames[association.id] !== undefined && !rawEditorNames[association.id].trim()}
                              onChange={event => { const name = event.currentTarget.value; setRawEditorNames(current => ({...current,[association.id]:name})) }}
                              onBlur={() => commitEditorField(association.id,'name')}
                              onKeyDown={event => { if (event.key === 'Enter') event.currentTarget.blur() }}
                              style={{ background: 'var(--surface-up)', color: 'var(--text)', border: '1px solid var(--border)', borderRadius: 8, padding: '8px 10px' }}
                            />
                          </label>
                          <label className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
                            <input
                              aria-label={`${groupName} file extensions`}
                              placeholder=".ts .tsx"
                              value={rawExtensions[association.id] ?? extensionsText(association)}
                              disabled={editorSaving}
                              aria-invalid={rawExtensions[association.id] !== undefined && !parseExtensions(rawExtensions[association.id]).length}
                              onChange={event => { const extensions = event.currentTarget.value; setRawExtensions(current => ({...current,[association.id]:extensions})) }}
                              onBlur={() => commitEditorField(association.id,'extensions')}
                              onKeyDown={event => { if (event.key === 'Enter') event.currentTarget.blur() }}
                              style={{ background: 'var(--surface-up)', color: 'var(--text)', border: '1px solid var(--border)', borderRadius: 8, padding: '8px 10px' }}
                            />
                          </label>
                          <div className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
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
                        </div>
                      </div>
                    </ProviderDisclosureCard>
                  )
                })}
              </div>

              {editorError && <p role="alert" className="text-xs" style={{color:'var(--red)'}}>{editorError}</p>}

            </div>
          </SectionCard>
        </div>
      )
    }

    if (selectedSection === 'shell-actions') {
      return <ShellActionsSettings ref={shellRef}/>
    }

    if (selectedSection === 'mcp-servers') {
      const browserConfigured = mcpServers.some(server => server.enabled && server.transport === 'stdio' && workspaceKey(server.workspaceDirectory) === workspaceKey(mcpWorkspace) && server.args.some(arg => /^@playwright\/mcp(?:@|$)/.test(arg)))
      return <div className="grid gap-4">
        <div className="text-sm">Browser control</div>
        <input aria-label="Browser control workspace directory" placeholder="Workspace directory" value={mcpWorkspace} disabled={mcpSaving} onChange={event => { setMcpWorkspace(event.currentTarget.value); setBrowserSetupSaved(false) }} className="rounded-lg px-3 py-2 text-sm" style={{color:'var(--text)',background:'var(--bg)',border:'1px solid var(--border)'}}/>
        <div>
          {providers.map(provider => {
            const supported = provider.supportsStructured !== false && ['gemini-acp','opencode-acp','copilot-acp'].includes(provider.structuredProtocol || providerMetadataForId(provider.id).structuredProtocol)
            return <div key={provider.id} className="flex items-center gap-2 py-3 text-sm" style={{borderBottom:'1px solid var(--border)'}}>
              <ProviderLogo providerId={provider.id}/><span className="flex-1">{providerDisplayName(provider,provider.id)}</span>
              {!supported ? <span style={{color:'var(--text-3)'}}>Unavailable</span> : mcpSaving ? <span>Saving…</span> : browserConfigured ? <span>Configured</span> : <Button size="sm" aria-label={`Setup ${providerDisplayName(provider,provider.id)} browser control`} onClick={() => { setBrowserSetupProvider(provider.id); setBrowserSetupSaved(false); setMcpMessage('') }}>Setup</Button>}
            </div>
          })}
        </div>
        {browserSetupProvider && <div className="grid gap-3">
          <p className="text-xs">This configures browser tools for all supported providers in this workspace. The provider downloads and starts the tools in a new local structured chat.</p>
          {!browserSetupSaved && <>
            <label className="grid gap-1 text-xs">npx executable
              <span className="flex gap-2"><input aria-label="npx executable path" placeholder="Absolute npx path" value={mcpExecutable} disabled={mcpSaving} onChange={event => setMcpExecutable(event.currentTarget.value)} className="min-w-0 flex-1 rounded-lg px-3 py-2" style={{color:'var(--text)',background:'var(--bg)',border:'1px solid var(--border)'}}/>
                <Button size="sm" disabled={mcpSaving} onClick={() => void browseProviderAgentImport(mcpExecutable).then(path => path && setMcpExecutable(path))}>Browse</Button>
              </span>
            </label>
            <div className="flex gap-2"><Button size="sm" disabled={mcpSaving} onClick={() => setBrowserSetupProvider(null)}>Cancel</Button><Button size="sm" aria-label="Add Playwright browser control" aria-busy={mcpSaving || undefined} disabled={mcpSaving || !mcpWorkspace.trim() || !mcpExecutable.trim()} onClick={configureBrowserControl}>{mcpSaving ? 'Saving configuration…' : 'Save configuration'}</Button></div>
          </>}
          {browserSetupSaved && <Button size="sm" onClick={() => setBrowserSetupProvider(null)}>Done</Button>}
        </div>}
        {mcpMessage && <p role="status" className="text-xs">{mcpMessage}</p>}
        <details>
          <summary className="cursor-pointer py-2 text-sm">Advanced JSON</summary>
          <textarea aria-label="MCP server configuration" value={mcpDraft} onChange={event => { mcpRevision.current += 1; setMcpDraft(event.target.value); setMcpDraftDirty(true); setMcpMessage(''); setBrowserSetupSaved(false) }} spellCheck={false} rows={16} className="w-full resize-y rounded-lg px-3 py-2 font-mono text-xs" style={{color:'var(--text)',background:'var(--bg)',border:'1px solid var(--border)'}}/>
          <p className="text-xs">Use absolute workspace and executable paths. HTTP and SSE must use localhost. Reference secrets by environment-variable name. Changes apply to the next session.</p>
        </details>
      </div>
    }

    if (selectedSection === 'chat-data') {
      return (
        <div className="space-y-4">
          <SectionCard
            title="Portable local chat bundle"
          >
            <div className="grid gap-4">
              <p className="text-xs leading-5" style={{ color: 'var(--text-2)' }}>
                Export writes a readable versioned folder with a manifest and canonical chat JSON files. Choose a new or empty folder. Import validates the complete bundle first and gives conflicting chats new local IDs.
              </p>
              {chatDataMessage && (
                <Notice
                  tone={chatDataMessage.tone}
                  title="Chat data"
                  dismissLabel="Dismiss chat data result"
                  onDismiss={() => setChatDataMessage(null)}
                >
                  {chatDataMessage.text}
                </Notice>
              )}
              <div className="flex flex-wrap gap-2">
                <Button
                  size="sm"
                  leadingIcon={<Download size={14} />}
                  aria-busy={chatDataBusy === 'export' || undefined}
                  disabled={chatDataBusy !== null}
                  onClick={() => void runChatDataAction('export')}
                >
                  Export all chats
                </Button>
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
              </div>
              {chatDataFolder && <div className="text-xs" style={{ color: 'var(--text-3)', overflowWrap: 'anywhere' }}>Last folder: {chatDataFolder}</div>}
            </div>
          </SectionCard>
        </div>
      )
    }

    return (
      <div className="space-y-4">
        <SectionCard
          title="Universal Agent Manager"
        >
          <div className="grid gap-3 text-xs">
            <div className="flex justify-between gap-3">
              <span style={{ color: 'var(--text-3)' }}>Application</span>
              <span style={{ color: 'var(--text)' }}>Universal Agent Manager</span>
            </div>
            <div className="flex justify-between gap-3">
              <span style={{ color: 'var(--text-3)' }}>Version</span>
              <span style={{ color: 'var(--text)' }}>{appVersion}</span>
            </div>
          </div>
        </SectionCard>
        <SectionCard
          title="Update checks"
        >
          <Switch label="Automatically check for updates" checked={updateChecksEnabled} onChange={(event) => { void setUpdateSettings({ updateChecksEnabled: event.target.checked }) }} />
        </SectionCard>
      </div>
    )
  }

  return (
    <div className="flex h-full min-h-0 min-w-0" style={{background:'var(--bg)',color:'var(--text)'}}>
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
            <nav aria-label="Settings pages" className="space-y-5 min-h-0 overflow-y-auto flex-1">
              {visibleSettingsGroups.length === 0 && <p role="status" className="px-2 text-xs" style={{color:'var(--text-3)'}}>No settings found.</p>}
              {visibleSettingsGroups.map(group => <div key={group.label}>
                <h2 className="px-2 mb-1 text-xs font-medium" style={{color:'var(--text-3)'}}>{group.label}</h2>
                {group.sections.map(id => {
                  const section = SETTINGS_SECTIONS.find(item => item.id === id)!
                  const active = selectedSection === section.id
                  const SectionIcon = section.icon
                  return <button key={section.id} type="button" aria-pressed={active} aria-label={section.label} onClick={() => changeSection(section.id)} className="uam-choice-button w-full flex items-center gap-2 text-left px-2 py-2" style={{background:active?'var(--surface-up)':'transparent',border:'none',color:active?'var(--text)':'var(--text-2)'}}><SectionIcon size={15} className="shrink-0" aria-hidden/><span className="text-sm">{section.label}</span></button>
                })}
              </div>)}
            </nav>
          </aside>
          <div className="min-w-0 min-h-0 flex flex-col p-5 md:p-6 overflow-hidden">
            <div className="w-full mx-auto min-h-0 flex-1 flex flex-col" style={{maxWidth: !themeDraft && (selectedSection === 'memory-store' || selectedSection === 'markdown-store') ? 1040 : 800}}>
              {themeDraft ? <div className="overflow-y-auto">
                <div className="shrink-0 flex items-center justify-between gap-3 mb-5"><h2 tabIndex={-1} data-theme-step className="text-lg font-semibold">{['Theme name and base','Theme palette','Review theme'][themeStep]}</h2><IconButton icon={<X size={16}/>} label="Close theme editor" disabled={themeSaving} onClick={() => requestThemeExit(() => setThemeDraft(null))}/></div>
                {renderThemeEditor()}
                {themeMessage && <p role="status" className="text-xs mt-3">{themeMessage}</p>}
              </div> : <>
                <div className="flex items-center justify-between gap-3 mb-5">
                  <h2 className="text-lg font-semibold">{selectedSection === 'cli-version' ? 'CLI version control' : SETTINGS_SECTIONS.find(section => section.id === selectedSection)?.label}</h2>
                  <div className="flex items-center gap-2">
                    <div id="settings-page-actions" className="flex items-center gap-2"/>
                    {selectedSection === 'editors' && renderAddEditor()}
                    {selectedSection === 'mcp-servers' && renderMcpSave()}
                  </div>
                </div>
                <div key={selectedSection} className={`flex-1 min-h-0 ${selectedSection === 'memory-store' || selectedSection === 'markdown-store' ? 'overflow-hidden' : 'overflow-y-auto'}`}>{renderSectionContent()}</div>
              </>}
            </div>
          </div>
        </div>
      </div>
      {editorExit && <div className="fixed inset-0 z-[80] flex items-center justify-center p-4" style={{background:'rgba(0,0,0,.5)'}}>
        <div role="alertdialog" aria-modal="true" data-settings-owned-overlay aria-label="Unsaved editor changes" className="max-w-md p-5 space-y-4 rounded-xl" style={{background:'var(--surface)',border:'1px solid var(--border)'}}>
          <h2 className="text-sm font-semibold">Save editor changes?</h2>
          {editorError && <p role="alert" className="text-xs">{editorError}</p>}
          <div className="flex gap-2"><Button autoFocus disabled={editorSaving} onClick={() => setEditorExit(null)}>Stay</Button><Button disabled={editorSaving} onClick={() => { const next=editorExit; setEditorExit(null); next() }}>Discard</Button><Button disabled={editorSaving} onClick={async () => {
            const next = editorAssociationsDraft.map(item => ({...item,name:rawEditorNames[item.id]?.trim() ?? item.name,extensions:rawExtensions[item.id] === undefined ? item.extensions : parseExtensions(rawExtensions[item.id])}))
            if (await saveEditorSettings(next)) { const leave=editorExit; setEditorExit(null); leave() }
          }}>Save</Button></div>
        </div>
      </div>}
      {themeExit && <div className="fixed inset-0 z-[80] flex items-center justify-center p-4" style={{background:'rgba(0,0,0,.5)'}}>
        <div role="alertdialog" aria-modal="true" data-settings-owned-overlay aria-label="Unsaved theme changes" className="max-w-md rounded-xl p-5 space-y-4" style={{background:'var(--surface)',border:'1px solid var(--border)'}}>
          <h2 className="text-sm font-semibold">Save theme changes?</h2>
          <div className="flex gap-2"><Button autoFocus disabled={themeSaving} onClick={() => setThemeExit(null)}>Stay</Button><Button disabled={themeSaving} onClick={() => { const next=themeExit; setThemeDraft(null); setThemeExit(null); next() }}>Discard</Button><Button disabled={!themeDraftValid || themeSaving} aria-busy={themeSaving || undefined} onClick={async () => { if (await saveThemeDraft()) { const next=themeExit; setThemeExit(null); next() } }}>Save theme</Button></div>
          {themeMessage && <p role="status" className="text-xs">{themeMessage}</p>}
        </div>
      </div>}
      {remotePreview && (
		<div className="fixed inset-0 z-[70] flex items-center justify-center p-4 animate-fade-in" style={{ background: 'rgba(0,0,0,.5)' }} onClick={(event) => { if (event.target === event.currentTarget && !remoteBusy) setRemotePreview(null) }}>
		  <div role="dialog" aria-modal="true" data-settings-owned-overlay aria-label="Remote helper setup" className="w-full max-w-lg rounded-xl animate-slide-in" style={{ background: 'var(--surface)', border: '1px solid var(--border-bright)', boxShadow: 'var(--elev-3)' }}>
			<div className="px-5 py-4" style={{ borderBottom: '1px solid var(--border)' }}>
			  <div className="text-sm font-semibold" style={{ color: 'var(--text)' }}>Install helper on {remotePreview.host.label}?</div>
			  <div className="mt-1 text-xs" style={{ color: 'var(--text-3)' }}>SSH alias: <code>{remotePreview.host.sshAlias}</code></div>
			</div>
			<div className="grid gap-4 p-5 text-sm" style={{ color: 'var(--text-2)' }}>
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
					<input aria-label="Remote helper folder" aria-invalid={remoteCustomDirectory && Boolean(remoteDirectoryValidation)} aria-describedby={remoteDirectoryValidation ? 'remote-helper-folder-help remote-helper-folder-error' : 'remote-helper-folder-help'} value={remoteDirectory} disabled={!remoteCustomDirectory} onChange={(event) => setRemoteDirectory(event.currentTarget.value)} placeholder="uam-helper" spellCheck={false} className="mt-2 w-full rounded-lg px-3 py-2 font-mono text-xs outline-none" style={{ color: 'var(--text)', background: 'var(--bg)', border: `1px solid ${remoteDirectoryValidation ? 'var(--red)' : 'var(--border)'}` }} />
					<span id="remote-helper-folder-help" className="mt-1 block text-[11px]" style={{ color: 'var(--text-3)' }}>Relative path only. Use letters, numbers, dots, dashes, underscores, and /.</span>
					{remoteDirectoryValidation && <span id="remote-helper-folder-error" role="alert" className="mt-1 block text-[11px]" style={{ color: 'var(--red)' }}>{remoteDirectoryValidation}</span>}
					{remoteCustomDirectory && !remoteDirectoryValidation && <span className="mt-1 block break-all font-mono text-[11px]" style={{ color: 'var(--accent)' }}>Linux: ~/{remoteDirectory.trim()}/{appVersion.replace(/^v/i, '')}<br />Windows: %USERPROFILE%\{remoteDirectory.trim().replace(/\//g, '\\')}\{appVersion.replace(/^v/i, '')}</span>}
				  </div>
				</div>
			  </fieldset>
			  <pre className="max-h-32 overflow-auto whitespace-pre-wrap rounded p-2 text-[11px]" style={{ background: 'var(--bg)', color: 'var(--text-3)', border: '1px solid var(--border)' }}>{remotePreview.preview.split('\n').filter((line) => !line.startsWith('Install UAM runner')).join('\n')}</pre>
			</div>
			<div className="flex justify-end gap-2 px-5 py-4" style={{ borderTop: '1px solid var(--border)' }}>
			  <Button size="sm" disabled={remoteBusy} onClick={() => setRemotePreview(null)}>Cancel</Button>
			  <Button size="sm" variant="primary" aria-busy={remoteBusy || undefined} disabled={remoteBusy || Boolean(remoteDirectoryValidation)} onClick={() => void installRemoteHost()}>Connect and install</Button>
			</div>
		  </div>
		</div>
	  )}
      {confirmDiscard && (
        <div className="fixed inset-0 z-[70] flex items-center justify-center p-4" style={{ background: 'rgba(0,0,0,.45)' }}>
          <div role="alertdialog" aria-modal="true" data-settings-owned-overlay aria-label="Discard unsaved MCP changes" className="w-full max-w-sm rounded-xl" style={{ background: 'var(--surface)', border: '1px solid var(--border-bright)', boxShadow: 'var(--elev-3)' }}>
            <div className="px-5 py-4 text-sm font-semibold" style={{ color: 'var(--text)', borderBottom: '1px solid var(--border)' }}>Discard MCP changes?</div>
            <p className="p-5 text-sm" style={{ color: 'var(--text-2)' }}>The MCP server configuration has unsaved changes.</p>
            <div className="flex justify-end gap-2 px-5 py-4" style={{ borderTop: '1px solid var(--border)' }}>
              <Button size="sm" onClick={() => { setConfirmDiscard(false); dialogRef.current?.focus() }}>Keep editing</Button>
              <Button size="sm" variant="danger" onClick={() => { setMcpDraftDirty(false); setConfirmDiscard(false); discardExit?.(); setDiscardExit(null) }}>Discard changes</Button>
            </div>
          </div>
        </div>
      )}
      {pendingDelete && (
        <div className="fixed inset-0 z-[60] flex items-center justify-center p-4 animate-fade-in" style={{ background: 'rgba(0,0,0,.35)' }} onClick={(event) => { if (event.target === event.currentTarget) setPendingDelete(null) }}>
          <div role="alertdialog" aria-modal="true" data-settings-owned-overlay aria-label={`Delete ${pendingDelete.name}`} className="w-full max-w-md rounded-xl animate-slide-in" style={{ background: 'var(--surface)', border: '1px solid var(--border-bright)', boxShadow: 'var(--elev-3)' }}>
            <div className="px-5 py-4 text-sm font-semibold" style={{ color: 'var(--text)', borderBottom: '1px solid var(--border)' }}>Delete {pendingDelete.kind === 'theme' ? 'theme' : 'editor group'}?</div>
            <div className="p-5 text-sm" style={{ color: 'var(--text-2)' }}>“{pendingDelete.name}” will be permanently deleted. This cannot be undone or restored.</div>
            <div className="flex justify-end gap-2 px-5 py-4" style={{ borderTop: '1px solid var(--border)' }}>
              {(pendingDelete.kind === 'theme' ? themeMessage : editorError) && <span role="status" className="text-xs">{pendingDelete.kind === 'theme' ? themeMessage : editorError}</span>}
              <Button size="sm" disabled={themeSaving || editorSaving} onClick={() => setPendingDelete(null)}>Cancel</Button>
              <Button size="sm" variant="danger" disabled={themeSaving || editorSaving} onClick={async () => {
                const deleted = pendingDelete.kind === 'theme' ? await removeSelectedTheme() : await saveEditorSettings(editorAssociationsDraft.filter(item => item.id !== pendingDelete.id))
                if (deleted) setPendingDelete(null)
              }}>Delete permanently</Button>
            </div>
          </div>
        </div>
      )}
    </div>
  )
})
