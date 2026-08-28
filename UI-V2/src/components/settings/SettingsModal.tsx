import { useEffect, useRef, useState, type ReactNode } from 'react'
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
  type AcpBinding,
  type AcpProviderUsage,
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
import { BookOpen, Brain, Check, ChevronDown, ChevronRight, ClipboardList, Download, FolderOpen, Info, MemoryStick, MessageSquare, Mic, Minus, MousePointerClick, Palette, Pencil, Plus, RefreshCw, Save, Server, Target, TerminalSquare, Trash2, X, type LucideIcon } from 'lucide-react'
import { Button, IconButton, MenuSelect, Notice, Switch, ViewportMenu } from '../ui'
import { ShellActionsSettings } from './ShellActionsSettings'
import {
  DEFAULT_PROVIDER_ID,
  providerCapabilities,
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

function latestProviderUsage(sessions: Session[], providerId: string, bindings: Record<string, AcpBinding>): AcpProviderUsage | null {
  let tokenUsage: AcpProviderUsage['tokenUsage'] = null
  let rateLimits: AcpProviderUsage['rateLimits'] = null
  for (const session of sessions) {
    if (session.providerId !== providerId) continue
    const usage = bindings[session.id]?.providerUsage
    if (usage?.tokenUsage && (!tokenUsage || usage.tokenUsage.updatedAt > tokenUsage.updatedAt)) tokenUsage = usage.tokenUsage
    if (usage?.rateLimits && (!rateLimits || usage.rateLimits.updatedAt > rateLimits.updatedAt)) rateLimits = usage.rateLimits
  }
  return tokenUsage || rateLimits ? { tokenUsage, rateLimits } : null
}

function ProviderUsageSummary({ providerName, usage }: { providerName: string; usage?: AcpProviderUsage | null }) {
  const tokenUsage = usage?.tokenUsage
  const rateLimits = usage?.rateLimits
  const formatNumber = (value: number) => value.toLocaleString()
  const rateLimitLine = (label: string, window: NonNullable<AcpProviderUsage['rateLimits']>['primary']) => {
    if (!window) return null
    const duration = window.windowDurationMinutes ? ` · ${formatNumber(window.windowDurationMinutes)}-minute window` : ''
    const reset = window.resetsAt ? ` · resets ${new Date(window.resetsAt * 1000).toLocaleString()}` : ''
    return <div>{label}: {window.usedPercent}% used · {100 - window.usedPercent}% window remaining{duration}{reset}</div>
  }

  return (
    <div
      role="region"
      aria-label={`${providerName} provider usage`}
      className="grid gap-1 rounded-md border px-3 py-2"
      style={{ borderColor: 'var(--border)', background: 'var(--surface)' }}
    >
      <div className="font-medium" style={{ color: 'var(--text)' }}>Provider usage</div>
      {!tokenUsage && !rateLimits ? (
        <div style={{ color: 'var(--text-3)' }}>Not reported by provider</div>
      ) : (
        <>
          {tokenUsage && (
            <>
              <div>Thread tokens: {formatNumber(tokenUsage.total.totalTokens)} total · {formatNumber(tokenUsage.last.totalTokens)} last turn</div>
              {tokenUsage.modelContextWindow && <div>Model context: {formatNumber(tokenUsage.modelContextWindow)} tokens</div>}
            </>
          )}
          {rateLimits && (
            <>
              <div className="font-medium" style={{ color: 'var(--text)' }}>
                Account limits{rateLimits.limitName ? ` · ${rateLimits.limitName}` : ''}{rateLimits.planType ? ` · ${rateLimits.planType}` : ''}
              </div>
              {rateLimits.rateLimitReachedType && <div style={{ color: 'var(--error)' }}>Limit reached: {rateLimits.rateLimitReachedType.replace(/_/g, ' ')}</div>}
              {rateLimits.spendControlReached && <div style={{ color: 'var(--error)' }}>Spend control reached</div>}
              {rateLimits.credits && <div>Credits: {rateLimits.credits.unlimited ? 'Unlimited' : rateLimits.credits.hasCredits ? (rateLimits.credits.balance ? `${rateLimits.credits.balance} available` : 'Available') : 'None available'}</div>}
              {rateLimits.individualLimit && (
                <div>Individual limit: {rateLimits.individualLimit.used} of {rateLimits.individualLimit.limit} used · {rateLimits.individualLimit.remainingPercent}% remaining{rateLimits.individualLimit.resetsAt ? ` · resets ${new Date(rateLimits.individualLimit.resetsAt * 1000).toLocaleString()}` : ''}</div>
              )}
              {rateLimitLine('Primary limit', rateLimits.primary)}
              {rateLimitLine('Secondary limit', rateLimits.secondary)}
            </>
          )}
        </>
      )}
    </div>
  )
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
  detail: string
  icon: LucideIcon
}

const SETTINGS_SECTIONS: SettingsSection[] = [
  { id: 'appearance', label: 'Appearance', detail: 'Theme and display', icon: Palette },
  { id: 'defaults', label: 'Chat Defaults', detail: 'Provider and new-chat settings', icon: MessageSquare },
  { id: 'agents', label: 'Agents', detail: 'Favorites and composer shortcut', icon: ClipboardList },
  { id: 'cli-version', label: 'CLI Version', detail: 'Run or revert provider CLIs', icon: TerminalSquare },
  { id: 'remote-hosts', label: 'Remote Hosts', detail: 'SSH-connected UAM runners', icon: Server },
  { id: 'voice-input', label: 'Voice Input', detail: 'Speech-to-text provider', icon: Mic },
  { id: 'memory-settings', label: 'Memory Settings', detail: 'Defaults and workers', icon: Brain },
  { id: 'memory-store', label: 'Memory Store', detail: 'Library and backfill', icon: MemoryStick },
  { id: 'markdown-store', label: 'Skills', detail: 'Reusable prompts and attachments', icon: BookOpen },
  { id: 'goal-loops', label: 'Goal Loops', detail: 'Loop safety', icon: Target },
  { id: 'mcp-servers', label: 'MCP Servers', detail: 'Local workspace tools', icon: TerminalSquare },
  { id: 'editors', label: 'Editors', detail: 'Workspace launch presets', icon: Pencil },
  { id: 'shell-actions', label: 'Shell Actions', detail: 'Finder and Explorer menus', icon: MousePointerClick },
  { id: 'chat-data', label: 'Chat Data', detail: 'Local export and import', icon: Download },
  { id: 'about', label: 'About', detail: 'Version information', icon: Info },
]

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
  if (manager.status === 'known-incompatible') return 'Known incompatible — update before structured use'
  if (manager.status === 'unavailable') return 'Not installed or unavailable on PATH'
  if (manager.status === 'provider-managed') return 'Compatibility is managed by the provider'
  return 'Version has not been checked'
}

function SectionCard(
  { title, description, children }: { title: string; description?: string; children: ReactNode }
) {
  return (
    <section
      className="pb-5"
      style={{
        borderBottom: '1px solid var(--border)',
      }}
    >
      <div className="mb-4">
        <div className="text-sm font-semibold" style={{ color: 'var(--text)' }}>{title}</div>
        {description && (
          <div className="text-xs mt-1" style={{ color: 'var(--text-3)' }}>{description}</div>
        )}
      </div>
      {children}
    </section>
  )
}

function ProviderDisclosureCard(
  {
    panelId,
    providerId,
    leadingIcon,
    title,
    expanded,
    onToggle,
    toggleLabel,
    children,
  }: {
    panelId: string
    providerId?: string
    leadingIcon?: ReactNode
    title: string
    expanded: boolean
    onToggle: () => void
    toggleLabel: string
    children: ReactNode
  }
) {
  return (
    <div
      className="rounded-lg p-3"
      style={{ background: 'var(--surface)', border: '1px solid var(--border)' }}
    >
      <button
        type="button"
        aria-expanded={expanded}
        aria-controls={panelId}
        aria-label={toggleLabel}
        title={toggleLabel}
        onClick={onToggle}
        className="w-full flex items-center justify-between gap-3 text-left"
        style={{ color: 'var(--text)', background: 'transparent', border: 'none', padding: 0, cursor: 'pointer' }}
      >
        <span className="inline-flex min-w-0 items-center gap-2 text-sm">
          {leadingIcon ?? (providerId ? <ProviderLogo providerId={providerId} /> : null)}
          <span className="truncate">{title}</span>
        </span>
        <span
          aria-hidden="true"
          className="inline-flex h-6 w-6 shrink-0 items-center justify-center rounded-md text-sm transition-colors duration-150"
          style={{
            background: expanded ? 'var(--accent-dim)' : 'var(--surface-up)',
            color: 'var(--text-2)',
            border: '1px solid var(--border)',
          }}
        >
          <ChevronRight size={14} className={`transition-transform duration-150${expanded ? ' rotate-90' : ''}`} />
        </span>
      </button>

      {expanded && (
        <div
          id={panelId}
          className="mt-3 pt-3 animate-fade-in"
          style={{ borderTop: '1px solid var(--border)' }}
        >
          {children}
        </div>
      )}
    </div>
  )
}

export function SettingsModal() {
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
  const openMarkdownStore = useAppStore((s) => s.openMarkdownStore)
  const openAllMemoryLibrary = useAppStore((s) => s.openAllMemoryLibrary)
  const openMemoryScanModal = useAppStore((s) => s.openMemoryScanModal)
  const { theme, setTheme } = useTheme()
  const [openMemoryMenu, setOpenMemoryMenu] = useState<string | null>(null)
  const [openEditorMenu, setOpenEditorMenu] = useState<string | null>(null)
  const [openCliVersionMenu, setOpenCliVersionMenu] = useState<string | null>(null)
  const [selectedCliVersions, setSelectedCliVersions] = useState<Record<string, string>>({})
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

  const requestClose = () => {
    if (mcpDraftDirty) {
      setConfirmDiscard(true)
      return
    }
    setSettingsOpen(false)
  }

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
    }
    if (!portableAlias || !sshAlias) {
      setRemoteMessage('Enter one exact host alias from ~/.ssh/config.')
      return
    }
    setRemoteBusy(true)
    setRemoteMessage('')
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
    setRemoteBusy(true)
    setRemoteMessage('Connecting, verifying the copied helper, and starting the runner…')
    const response = await sendToCEF({ action: 'installRemoteHost', payload: remotePreview.host })
    setRemoteBusy(false)
    if (!response.ok) {
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
    setSelectedCliVersions((current) => {
      const next = { ...current }
      for (const provider of cliVersionManager.providers) {
        if (!next[provider.providerId]) {
          next[provider.providerId] = provider.selectedVersion || provider.preferredVersion || ''
        }
      }
      return next
    })
  }, [cliVersionManager.providers])

  useEffect(() => {
    const handler = (e: KeyboardEvent) => {
      if (e.key !== 'Escape') return
      if (e.defaultPrevented) return
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
      requestClose()
    }
    window.addEventListener('keydown', handler)
    return () => window.removeEventListener('keydown', handler)
  }, [isMarkdownStoreOpen, mcpDraftDirty, openCliVersionMenu, openEditorMenu, openMemoryMenu, setSettingsOpen])

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

  const saveEditorSettings = (nextAssociations = editorAssociationsDraft, nextDefaultEditor = defaultEditorDraft) => {
    setEditorAssociationsDraft(nextAssociations)
    setDefaultEditorDraft(nextDefaultEditor)
    void setEditorSettings({
      defaultEditorPresetId: nextDefaultEditor,
      editorFileAssociations: nextAssociations,
    })
  }

  const renderCliVersionMenu = (
    menuId: string,
    value: string,
    label: string,
    options: MemoryModelOption[],
    disabled: boolean,
    onSelect: (value: string) => void
  ) => {
    const selectedOption = options.find((option) => option.id === value) ?? (value ? { id: value, label: value, detail: 'Current selection' } : undefined)
    return (
      <div className="relative">
        <button
          ref={(element) => { if (element) popupAnchorsRef.current.set(menuId, element) }}
          type="button"
          title={label}
          aria-haspopup="listbox"
          aria-expanded={openCliVersionMenu === menuId}
          disabled={disabled}
          onClick={() => setOpenCliVersionMenu(openCliVersionMenu === menuId ? null : menuId)}
          className="uam-menu-select__trigger flex w-full items-center justify-between gap-2 text-left disabled:opacity-50"
          style={{
            color: 'var(--text)',
            borderRadius: 8,
            padding: '8px 10px',
          }}
        >
          <span className="truncate">{selectedOption?.label ?? 'No versions available'}</span>
          {!disabled && <ChevronDown className={openCliVersionMenu === menuId ? 'uam-menu-select__chevron is-open' : 'uam-menu-select__chevron'} size={14} aria-hidden />}
        </button>
        {!disabled && openCliVersionMenu === menuId && (
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
            {options.map((option) => {
              const selected = option.id === value
              return (
                <button
                  key={option.id}
                  type="button"
                  role="option"
                  aria-selected={selected}
                  onClick={() => {
                    onSelect(option.id)
                    setOpenCliVersionMenu(null)
                  }}
                  className={`uam-menu-select__option w-full grid gap-0.5 text-left px-2 py-2${selected ? ' is-selected' : ''}`}
                  style={{
                    borderRadius: 6,
                    color: selected ? 'var(--text)' : 'var(--text-2)',
                  }}
                >
                  <span className="flex items-center gap-2">
                    <span className="flex-1">{option.label}</span>
                    {selected && <Check size={13} style={{ color: 'var(--green)' }} aria-hidden />}
                  </span>
                  {option.detail && (
                    <span className="text-[11px]" style={{ color: 'var(--text-3)' }}>
                      {option.detail}
                    </span>
                  )}
                </button>
              )
            })}
          </ViewportMenu>
        )}
      </div>
    )
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
    if (!themeDraft || !themeDraftValid || themeSaveInFlightRef.current) return
    themeSaveInFlightRef.current = true
    try {
      const saved = await saveCustomTheme(themeDraft)
      if (!saved) {
        setThemeMessage('Theme could not be saved. Check its name and colors.')
        return
      }
      setThemeDraft(saved)
      setTheme(saved.id)
      setThemeMessage('Theme saved.')
    } finally {
      themeSaveInFlightRef.current = false
    }
  }
  const removeSelectedTheme = async () => {
    if (!selectedCustomTheme) return
    const deleted = await deleteCustomTheme(selectedCustomTheme.id)
    setThemeDraft(null)
    setThemeMessage(deleted ? 'Theme deleted.' : 'Theme could not be deleted.')
  }
  const importThemeFile = async (file?: File) => {
    if (!file) return
    try {
      const imported = normalizeCustomTheme(JSON.parse(await file.text()))
      if (!imported) throw new Error('invalid')
      const saved = await saveCustomTheme(imported)
      if (!saved) throw new Error('save')
      setTheme(saved.id)
      setThemeDraft(saved)
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
            description="Choose a built-in theme or create a validated custom palette."
          >
            <div className="grid gap-4">
              <div className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
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
              <div className="flex flex-wrap gap-2">
                <Button size="sm" onClick={() => startNewTheme()}>Create</Button>
                <Button size="sm" onClick={cloneCurrentTheme}>Clone</Button>
                <Button size="sm" disabled={!selectedCustomTheme} onClick={() => setThemeDraft(selectedCustomTheme ? { ...selectedCustomTheme, colors: { ...selectedCustomTheme.colors } } : null)}>Edit</Button>
                <Button size="sm" variant="danger" disabled={!selectedCustomTheme} onClick={() => selectedCustomTheme && setPendingDelete({ kind: 'theme', id: selectedCustomTheme.id, name: selectedCustomTheme.name })}>Delete</Button>
                <Button size="sm" onClick={() => themeImportRef.current?.click()}>Import JSON</Button>
                <Button size="sm" disabled={!selectedCustomTheme} onClick={exportSelectedTheme}>Export JSON</Button>
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
              {themeDraft && (
                <div className="grid gap-4 rounded-lg p-3" style={{ border: '1px solid var(--border)', background: 'var(--surface)' }}>
                  <div className="grid gap-3 sm:grid-cols-2">
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
                  </div>
                  <div className="grid gap-2 sm:grid-cols-2 lg:grid-cols-3">
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
                  </div>
                  {!themeDraftValid && (
                    <p id="theme-color-format-error" role="alert" className="text-xs" style={{ color: 'var(--red)' }}>
                      Every color must use #RRGGBB format.
                    </p>
                  )}
                  <div className="flex gap-2">
                    <Button size="sm" variant="primary" disabled={!themeDraftValid} onClick={() => void saveThemeDraft()}>Save theme</Button>
                    <Button size="sm" onClick={() => setThemeDraft(null)}>Cancel preview</Button>
                  </div>
                </div>
              )}
              {themeMessage && <div role="status" className="text-xs" style={{ color: themeMessage.includes('failed') || themeMessage.includes('could not') ? 'var(--red)' : 'var(--text-2)' }}>{themeMessage}</div>}
            </div>
          </SectionCard>
          <SectionCard title="Sidebar" description="Choose the context shown beside each chat.">
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
          <SectionCard title="Chat activity" description="Choose how reasoning and tool activity is presented.">
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
            description="Optional. AI Review runs this provider in an isolated text-only worker. If it is unavailable or uncertain, you decide."
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
            description="Choose the provider preselected for new chats and the defaults each provider applies."
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
                  const providerUsage = latestProviderUsage(sessions, provider.id, acpBindings)
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
                    >
                      <div className="grid gap-3 text-xs" style={{ color: 'var(--text-2)' }}>
                        <ProviderUsageSummary providerName={providerName} usage={providerUsage} />
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
                        <div className="grid grid-cols-2 gap-2">
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
                        <div className="grid grid-cols-2 gap-2">
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
                        <div className="flex items-center gap-2">
						  <IconButton icon={<RefreshCw size={14} className={modelsLoading ? 'animate-spin' : ''} />} label={`Refresh ${providerName} models`} disabled={!providerWorkspace || modelsLoading} onClick={() => void discoverProviderModels(providerSession?.id ?? '', provider.id, providerWorkspace)} />
                          <span role="status" className="text-xs" style={{ color: modelRefreshError ? 'var(--red)' : 'var(--text-3)' }}>
							{modelsLoading ? 'Refreshing models…' : modelRefreshError || (providerWorkspace ? 'Cached models are ready' : 'Add a workspace to refresh models')}
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
            description="Build and Plan always come first. The configured shortcut then follows these favorites in the saved order, skipping agents unavailable in the current workspace."
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
			description="Copy one OpenCode, Copilot CLI, Gemini CLI, or Claude Code Markdown agent into UAM. Provider tool and security settings are never silently translated."
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
                  <Button size="sm" loading={agentImportBusy} disabled={!agentImportPath.trim()} onClick={() => void previewImport()}>Preview</Button>
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
                        loading={agentImportBusy}
                        disabled={!activeSessionId || !agentImportId.trim() || (agentImportPreview.ignoredFields.length > 0 && !agentImportAcknowledged)}
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
            description="Check, run, or revert each provider CLI to a curated supported version."
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
                const selectedCliVersion = selectedCliVersions[manager.providerId] || manager.selectedVersion || manager.preferredVersion || ''
                const cliVersionChanged = Boolean(selectedCliVersion && selectedCliVersion !== manager.installedVersion)
                const canApplyCliVersion = Boolean(selectedCliVersion && cliVersionChanged && !manager.running)
                const expanded = expandedCliVersionProviders[manager.providerId] ?? false
                const versionOptions = manager.availableVersions.map((option) => ({
                  id: option.version,
                  label: option.version,
                  detail: option.preferred ? 'Preferred version' : 'Curated version',
                }))
                if (selectedCliVersion && !versionOptions.some((option) => option.id === selectedCliVersion)) {
                  versionOptions.push({ id: selectedCliVersion, label: selectedCliVersion, detail: 'Current selection' })
                }
                return (
                  <ProviderDisclosureCard
                    key={manager.providerId}
                    panelId={`${manager.providerId}-cli-version-panel`}
                    providerId={manager.providerId}
                    title={providerName}
                    expanded={expanded}
                    toggleLabel={`${expanded ? 'Hide' : 'Show'} ${providerName} CLI version settings`}
                    onToggle={() => toggleCliVersionProvider(manager.providerId)}
                  >
                    <div className="flex items-start justify-between gap-4">
                      <div className="min-w-0">
                        <div className="text-xs" style={{ color: 'var(--text-3)' }}>
                          {versionStatusText(manager)}
                        </div>
                        {manager.message && (
                          <div className="text-xs mt-1" style={{ color: manager.status === 'known-incompatible' || manager.status === 'unavailable' ? 'var(--red)' : 'var(--text-2)' }}>
                            {manager.message}
                          </div>
                        )}
                      </div>
                      <IconButton
                        icon={<RefreshCw size={15} className={manager.running && manager.status === 'checking' ? 'animate-spin' : undefined} />}
                        label={`Refresh ${providerName} CLI version`}
                        disabled={manager.running}
                        onClick={() => void refreshCliProviderVersion(manager.providerId)}
                      />
                    </div>

                    <div className="grid grid-cols-2 gap-3 mt-3">
                      <div>
                        <div className="text-[11px]" style={{ color: 'var(--text-3)' }}>Installed</div>
                        <div className="text-sm mt-1" style={{ color: 'var(--text)' }}>
                          {manager.installedVersion || 'Unknown'}
                        </div>
                      </div>
                      <div>
                        <div className="text-[11px]" style={{ color: 'var(--text-3)' }}>Preferred</div>
                        <div className="text-sm mt-1" style={{ color: 'var(--text)' }}>
                          {manager.preferredVersion || 'Not configured'}
                        </div>
                      </div>
                    </div>

                    <div className="grid gap-2 mt-3">
                      <div className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
                        <div>Target version</div>
                        {renderCliVersionMenu(
                          `${manager.providerId}:cli-version`,
                          selectedCliVersion,
                          `${providerName} target version`,
                          versionOptions,
                          manager.running || versionOptions.length === 0,
                          (nextVersion) => setSelectedCliVersions((current) => ({ ...current, [manager.providerId]: nextVersion }))
                        )}
                      </div>
                      <div className="flex items-center justify-between gap-3">
                        <div className="text-xs" style={{ color: 'var(--text-3)' }}>
                          Applies with npm globally for this provider.
                        </div>
                        <IconButton
                          icon={manager.running ? <RefreshCw size={15} className="animate-spin" /> : <Download size={15} />}
                          label={manager.running ? `Installing ${providerName} CLI version` : `Apply ${providerName} CLI version`}
                          variant={canApplyCliVersion ? 'solid' : 'ghost'}
                          disabled={!canApplyCliVersion}
                          onClick={() => setPendingCliInstall({ providerId: manager.providerId, providerName, version: selectedCliVersion })}
                        />
                      </div>
                      {pendingCliInstall?.providerId === manager.providerId && (
                        <Notice
                          tone="warning"
                          title="Install provider CLI?"
                          dismissLabel={`Dismiss ${providerName} CLI install warning`}
                          onDismiss={() => setPendingCliInstall(null)}
                          actions={(
                            <>
                              <Button size="sm" onClick={() => setPendingCliInstall(null)}>Cancel</Button>
                              <Button size="sm" variant="danger" onClick={() => {
                                const pending = pendingCliInstall
                                setPendingCliInstall(null)
                                void applyCliProviderVersion(pending.providerId, pending.version)
                              }}>Install version</Button>
                            </>
                          )}
                        >
                          Install {pendingCliInstall.providerName} {pendingCliInstall.version} globally with npm?
                        </Notice>
                      )}
                    </div>

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
            description="Connect UAM to another computer through one existing SSH config alias. Credentials stay with OpenSSH; UAM copies and checksum-verifies its headless helper."
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
                <Button size="sm" leadingIcon={<Server size={14} />} loading={remoteBusy} disabled={!remoteAlias.trim()} onClick={() => void previewRemoteHost()}>
                  Preview setup
                </Button>
              </div>
              {remotePreview && (
                <Notice
                  tone="warning"
                  title={`Install helper on ${remotePreview.host.label}?`}
                  dismissLabel="Dismiss remote host setup preview"
                  onDismiss={() => setRemotePreview(null)}
                  actions={<>
                    <Button size="sm" onClick={() => setRemotePreview(null)}>Cancel</Button>
                    <Button size="sm" variant="primary" loading={remoteBusy} onClick={() => void installRemoteHost()}>Connect and install</Button>
                  </>}
                >
                  <div className="grid gap-2">
                    <span>This uses your existing OpenSSH authentication. It first verifies that the host matches this build’s macOS architecture, then creates a private versioned directory under ~/.local/share/uam/runner, copies the helper, verifies SHA-256, and starts it.</span>
                    <pre className="max-h-44 overflow-auto whitespace-pre-wrap rounded p-2 text-[11px]" style={{ background: 'var(--bg)', color: 'var(--text-2)', border: '1px solid var(--border)' }}>{remotePreview.preview}</pre>
                  </div>
                </Notice>
              )}
              {remoteMessage && <div role="status" className="text-xs" style={{ color: remoteMessage.endsWith('ready.') || remoteMessage.includes('removed') ? 'var(--green)' : 'var(--text-2)' }}>{remoteMessage}</div>}
            </div>
          </SectionCard>

          <SectionCard title="Configured hosts" description="New Chat shows Runs on after the first remote host is ready.">
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
            description="Control how new chats use memory and how much background context is retained."
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
            description="Choose which provider and model should handle memory work for each provider."
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
        <SectionCard title="Voice Input" description="Use the operating system speech recognition service.">
          <div role="status" className="rounded-lg p-3 text-xs" style={{ color: 'var(--text-2)', background: 'var(--surface)', border: '1px solid var(--border)' }}>
            Dictation stays on the native system speech service. Start it with the microphone button in the composer.
          </div>
        </SectionCard>
      )
    }

    if (selectedSection === 'memory-store') {
      return (
        <div className="space-y-4">
          <SectionCard
            title="Memory Library"
            description="Browse global and project memory files without leaving the app."
          >
            <div className="flex items-center justify-between gap-4">
              <div>
                <div className="text-sm" style={{ color: 'var(--text)' }}>
                  All memory
                </div>
                <div className="text-xs mt-0.5" style={{ color: 'var(--text-3)' }}>
                  Browse global memory alongside collections and workspace folders.
                </div>
              </div>
              <Button
                variant="primary"
                size="sm"
                onClick={() => void openAllMemoryLibrary()}
              >
                Open library
              </Button>
            </div>
          </SectionCard>

          <SectionCard
            title="Memory Backfill"
            description="Scan existing chats to extract durable memories from older history."
          >
            <div className="flex items-center justify-between gap-4">
              <div>
                <div className="text-sm" style={{ color: 'var(--text)' }}>
                  Scan current chats
                </div>
                <div className="text-xs mt-0.5" style={{ color: 'var(--text-3)' }}>
                  Choose chats and queue a one-off backfill scan for memory extraction.
                </div>
              </div>
              <Button
                variant="primary"
                size="sm"
                onClick={() => void openMemoryScanModal()}
              >
                Scan Current Chats
              </Button>
            </div>
          </SectionCard>
        </div>
      )
    }

    if (selectedSection === 'markdown-store') {
      return (
        <SectionCard title="Skills" description="Publish and attach reusable `.uam` files from one shared directory.">
          <div className="grid gap-3">
            <div className="flex items-center gap-2">
              <input aria-label="Skills directory" value={markdownStoreDraftDirectory} onChange={(event) => setMarkdownStoreDraftDirectory(event.target.value)} placeholder="Skills directory" className="min-w-0 flex-1 text-xs" style={{ border: '1px solid var(--border)', borderRadius: 8, background: 'var(--bg)', color: 'var(--text)', padding: '8px 10px', outline: 'none' }} />
              <IconButton icon={<FolderOpen size={15} />} label="Browse for Skills directory" onClick={() => { void browseMarkdownStoreDirectory(markdownStoreDraftDirectory).then((selected) => { if (selected) setMarkdownStoreDraftDirectory(selected) }) }} />
              <IconButton variant="solid" icon={<Save size={15} />} label="Save Skills directory" disabled={!markdownStoreDraftDirectory.trim() || markdownStoreDraftDirectory.trim() === markdownStoreDirectory} onClick={() => void setMarkdownStoreDirectory(markdownStoreDraftDirectory)} />
            </div>
            {markdownStoreError && <div role="alert" className="text-xs" style={{ color: 'var(--red)' }}>{markdownStoreError}</div>}
            <div className="flex items-center justify-between gap-4">
              <span className="text-xs" style={{ color: 'var(--text-3)' }}>Favorite entries become composer slash commands; other entries can be attached from the store.</span>
              <Button variant="primary" size="sm" leadingIcon={<BookOpen size={14} />} onClick={() => void openMarkdownStore()}>Open store</Button>
            </div>
          </div>
        </SectionCard>
      )
    }

    if (selectedSection === 'goal-loops') {
      return (
        <SectionCard title="Goal Loop Safety" description="Limit UAM-managed review loops. Provider-native goals continue to use provider controls.">
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
            description="Choose which IDE opens when the chat workspace editor button is pressed."
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
                      expanded={expanded}
                      toggleLabel={`${expanded ? 'Hide' : 'Show'} ${groupName} editor group`}
                      onToggle={() => toggleEditorGroup(association.id)}
                    >
                      <div className="grid gap-2">
                        <div className="grid grid-cols-3 gap-2">
                          <label className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
                            Group
                            <input
                              value={association.name}
                              onChange={(event) => {
                                const nextAssociations = editorAssociationsDraft.map((item) =>
                                  item.id === association.id ? { ...item, name: event.currentTarget.value } : item
                                )
                                saveEditorSettings(nextAssociations)
                              }}
                              style={{ background: 'var(--surface-up)', color: 'var(--text)', border: '1px solid var(--border)', borderRadius: 8, padding: '8px 10px' }}
                            />
                          </label>
                          <label className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
                            Extensions
                            <input
                              value={extensionsText(association)}
                              onChange={(event) => {
                                const nextAssociations = editorAssociationsDraft.map((item) =>
                                  item.id === association.id ? { ...item, extensions: parseExtensions(event.currentTarget.value) } : item
                                )
                                saveEditorSettings(nextAssociations)
                              }}
                              style={{ background: 'var(--surface-up)', color: 'var(--text)', border: '1px solid var(--border)', borderRadius: 8, padding: '8px 10px' }}
                            />
                          </label>
                          <div className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
                            <div>Editor</div>
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
                        <div className="flex items-center justify-between gap-3">
                          <div className="text-xs" style={{ color: 'var(--text-3)' }}>
                            {association.extensions.length} extension{association.extensions.length === 1 ? '' : 's'} open in {editorPresetLabel(association.editorPresetId)}
                          </div>
                          <IconButton
                            icon={<Trash2 size={14} />}
                            label={`Delete ${groupName} editor group`}
                            variant="danger"
                            size="sm"
                            disabled={editorAssociationsDraft.length <= 1}
                            onClick={() => setPendingDelete({ kind: 'editor', id: association.id, name: groupName })}
                          />
                        </div>
                      </div>
                    </ProviderDisclosureCard>
                  )
                })}
              </div>

              <IconButton
                icon={<Plus size={15} />}
                label="Add editor group"
                size="sm"
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
            </div>
          </SectionCard>
        </div>
      )
    }

    if (selectedSection === 'shell-actions') {
      return <ShellActionsSettings />
    }

    if (selectedSection === 'mcp-servers') {
      return (
        <SectionCard title="Local MCP servers" description="Connect each workspace to local tools such as computer use. Providers run the tools; UAM keeps permissions, activity, and cancellation in the normal chat flow.">
          <div className="grid gap-3">
            <div className="grid gap-3 rounded-lg border p-3" style={{ borderColor: 'var(--border)', background: 'var(--surface)' }}>
              <div>
                <div className="text-sm font-medium" style={{ color: 'var(--text)' }}>Browser control</div>
                <div className="mt-1 text-xs leading-5" style={{ color: 'var(--text-3)' }}>
                  Add Microsoft Playwright MCP in an isolated browser profile. Gemini, OpenCode, and Copilot structured chats can use it after starting a new provider session.
                </div>
              </div>
              <label className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
                Workspace directory
                <input
                  aria-label="Browser control workspace directory"
                  value={mcpWorkspace}
                  onChange={(event) => setMcpWorkspace(event.currentTarget.value)}
                  className="rounded-lg px-3 py-2 outline-none"
                  style={{ color: 'var(--text)', background: 'var(--bg)', border: '1px solid var(--border)' }}
                />
              </label>
              <label className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
                npx executable
                <div className="flex gap-2">
                  <input
                    aria-label="npx executable path"
                    value={mcpExecutable}
                    onChange={(event) => setMcpExecutable(event.currentTarget.value)}
                    placeholder="Absolute path from which npx / where npx"
                    className="min-w-0 flex-1 rounded-lg px-3 py-2 outline-none"
                    style={{ color: 'var(--text)', background: 'var(--bg)', border: '1px solid var(--border)' }}
                  />
                  <Button size="sm" onClick={() => void browseProviderAgentImport(mcpExecutable).then((path) => path && setMcpExecutable(path))}>Browse</Button>
                </div>
              </label>
              <div className="flex justify-end">
                <Button
                  aria-label="Add Playwright browser control"
                  variant="primary"
                  size="sm"
                  leadingIcon={<MousePointerClick size={14} />}
                  loading={mcpSaving}
                  disabled={!mcpWorkspace.trim() || !mcpExecutable.trim()}
                  onClick={() => {
                    let configured: McpServerConfiguration[]
                    try {
                      const parsed = JSON.parse(mcpDraft)
                      if (!Array.isArray(parsed)) throw new Error()
                      configured = parsed as McpServerConfiguration[]
                    } catch {
                      setMcpMessage('Fix the advanced JSON before adding browser control.')
                      return
                    }
                    let suffix = configured.length + 1
                    while (configured.some((server) => server.id === `playwright-browser-${suffix}`)) suffix += 1
                    const server: McpServerConfiguration = {
                      id: `playwright-browser-${suffix}`,
                      name: 'Playwright browser control',
                      workspaceDirectory: mcpWorkspace.trim(),
                      transport: 'stdio',
                      command: mcpExecutable.trim(),
                      args: ['-y', '@playwright/mcp@latest', '--isolated'],
                      url: '',
                      environment: [],
                      headers: [],
                      enabled: true,
                    }
                    const next = [...configured, server]
                    setMcpSaving(true)
                    setMcpMessage('')
                    void setMcpServers(next).then((result) => {
                      setMcpSaving(false)
                      if (result.ok) {
                        setMcpDraft(JSON.stringify(next, null, 2))
                        setMcpDraftDirty(false)
                      }
                      setMcpMessage(result.ok ? 'Browser control saved. Start a new structured provider session to use it.' : result.error || 'Browser control configuration was rejected.')
                    })
                  }}
                >Add browser control</Button>
              </div>
            </div>
            <label className="grid gap-2 text-xs" style={{ color: 'var(--text-2)' }}>
              Advanced server configuration (JSON array)
              <textarea
                aria-label="MCP server configuration"
                value={mcpDraft}
                onChange={(event) => { setMcpDraft(event.target.value); setMcpDraftDirty(true); setMcpMessage('') }}
                spellCheck={false}
                rows={16}
                className="w-full resize-y rounded-lg px-3 py-2 font-mono text-xs outline-none"
                style={{ color: 'var(--text)', background: 'var(--bg)', border: '1px solid var(--border)' }}
              />
            </label>
            <div className="text-xs leading-5" style={{ color: 'var(--text-3)' }}>
              Use an absolute workspaceDirectory. stdio requires an absolute command path. http and sse are localhost-only. Put secret values in environment variables and store only their names here. Changes apply to the next provider session.
            </div>
            {mcpMessage && <div role="status" className="text-xs" style={{ color: mcpMessage.includes('saved') ? 'var(--green)' : 'var(--red)' }}>{mcpMessage}</div>}
            <div className="flex justify-end">
              <IconButton variant="solid" icon={<Save size={15} />} label="Save MCP server configuration" disabled={mcpSaving} onClick={() => {
                let parsed: unknown
                try {
                  parsed = JSON.parse(mcpDraft)
                } catch {
                  setMcpMessage('Enter valid JSON.')
                  return
                }
                if (!Array.isArray(parsed)) {
                  setMcpMessage('MCP server configuration must be a JSON array.')
                  return
                }
                setMcpSaving(true)
                setMcpMessage('')
                void setMcpServers(parsed as McpServerConfiguration[]).then((result) => {
                  setMcpSaving(false)
                  if (result.ok) setMcpDraftDirty(false)
                  setMcpMessage(result.ok ? 'MCP server configuration saved.' : result.error || 'MCP server configuration was rejected.')
                })
              }} />
            </div>
          </div>
        </SectionCard>
      )
    }

    if (selectedSection === 'chat-data') {
      return (
        <div className="space-y-4">
          <SectionCard
            title="Portable local chat bundle"
            description="Move your complete local chat history between UAM installations without changing or deleting existing chats."
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
                  loading={chatDataBusy === 'export'}
                  disabled={chatDataBusy !== null}
                  onClick={() => void runChatDataAction('export')}
                >
                  Export all chats
                </Button>
                <Button
                  size="sm"
                  variant="secondary"
                  leadingIcon={<FolderOpen size={14} />}
                  loading={chatDataBusy === 'import'}
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
          description="Build and release information for the current application."
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
          description="Check UAM releases and installed provider CLIs at most once every 24 hours."
        >
          <Switch label="Automatically check for updates" checked={updateChecksEnabled} onChange={(event) => { void setUpdateSettings({ updateChecksEnabled: event.target.checked }) }} />
        </SectionCard>
      </div>
    )
  }

  return (
    <div
      className="fixed inset-0 z-50 flex items-center justify-center"
      style={{ background: 'rgba(0,0,0,0.55)', backdropFilter: 'blur(4px)' }}
      onClick={(e) => { if (e.target === e.currentTarget) requestClose() }}
    >
      <div
        ref={dialogRef}
        role="dialog"
        aria-modal="true"
        aria-label="Settings"
        tabIndex={-1}
        className="rounded-2xl shadow-2xl w-full max-w-5xl mx-4 overflow-hidden flex flex-col"
        style={{
          background: 'var(--surface)',
          border: '1px solid var(--border-bright)',
          height: 'min(760px, calc(100vh - 2rem))',
        }}
      >
        {/* Header */}
        <div
          className="flex items-center justify-between px-5 py-4"
          style={{ borderBottom: '1px solid var(--border)' }}
        >
          <span className="text-sm font-semibold" style={{ color: 'var(--text)' }}>
            Settings
          </span>
          <IconButton
            icon={<X size={16} />}
            label="Close settings"
            onClick={requestClose}
          />
        </div>

        <div className="grid md:grid-cols-[220px_minmax(0,1fr)] min-h-[560px] flex-1 min-h-0">
          <aside
            className="p-4 overflow-y-auto"
            style={{
              background: 'color-mix(in srgb, var(--surface-up) 68%, var(--surface))',
              borderRight: '1px solid var(--border)',
            }}
          >
            <div className="text-[11px] font-semibold uppercase tracking-[0.16em] mb-3" style={{ color: 'var(--text-3)' }}>
              Preferences
            </div>
            <div className="space-y-1">
              {SETTINGS_SECTIONS.map((section) => {
                const active = section.id === selectedSection
                const SectionIcon = section.icon
                return (
                  <button
                    key={section.id}
                    type="button"
                    aria-pressed={active}
                    onClick={() => {
                      setSelectedSection(section.id)
                      setOpenMemoryMenu(null)
                      setOpenEditorMenu(null)
                      setOpenCliVersionMenu(null)
                    }}
                    className="uam-choice-button w-full text-left px-3 py-2.5 rounded-xl"
                    style={{
                      background: active ? 'var(--surface)' : 'transparent',
                      border: active ? '1px solid var(--border-bright)' : '1px solid transparent',
                      boxShadow: active ? '0 8px 20px rgba(0, 0, 0, 0.08)' : 'none',
                    }}
                  >
                    <div className="flex items-center gap-2.5">
                      <SectionIcon size={15} aria-hidden style={{ color: active ? 'var(--accent)' : 'var(--text-3)' }} />
                      <div className="min-w-0"><div className="text-sm font-medium" style={{ color: active ? 'var(--text)' : 'var(--text-2)' }}>{section.label}</div><div className="truncate text-[11px] mt-0.5" style={{ color: 'var(--text-3)' }}>{section.detail}</div></div>
                    </div>
                  </button>
                )
              })}
            </div>
          </aside>

          <div className="p-5 md:p-6 overflow-y-auto min-h-0">
            <div className="mb-5">
              <div className="text-lg font-semibold" style={{ color: 'var(--text)' }}>
                {SETTINGS_SECTIONS.find((section) => section.id === selectedSection)?.label}
              </div>
              <div className="text-xs mt-1" style={{ color: 'var(--text-3)' }}>
                {SETTINGS_SECTIONS.find((section) => section.id === selectedSection)?.detail}
              </div>
            </div>
            <div key={selectedSection}>
              {renderSectionContent()}
            </div>
          </div>
        </div>

      </div>
      {confirmDiscard && (
        <div className="fixed inset-0 z-[70] flex items-center justify-center p-4" style={{ background: 'rgba(0,0,0,.45)' }}>
          <div role="alertdialog" aria-modal="true" aria-label="Discard unsaved MCP changes" className="w-full max-w-sm rounded-xl" style={{ background: 'var(--surface)', border: '1px solid var(--border-bright)', boxShadow: 'var(--elev-3)' }}>
            <div className="px-5 py-4 text-sm font-semibold" style={{ color: 'var(--text)', borderBottom: '1px solid var(--border)' }}>Discard MCP changes?</div>
            <p className="p-5 text-sm" style={{ color: 'var(--text-2)' }}>The MCP server configuration has unsaved changes.</p>
            <div className="flex justify-end gap-2 px-5 py-4" style={{ borderTop: '1px solid var(--border)' }}>
              <Button size="sm" onClick={() => { setConfirmDiscard(false); dialogRef.current?.focus() }}>Keep editing</Button>
              <Button size="sm" variant="danger" onClick={() => setSettingsOpen(false)}>Discard changes</Button>
            </div>
          </div>
        </div>
      )}
      {pendingDelete && (
        <div className="fixed inset-0 z-[60] flex items-center justify-center p-4 animate-fade-in" style={{ background: 'rgba(0,0,0,.35)' }} onClick={(event) => { if (event.target === event.currentTarget) setPendingDelete(null) }}>
          <div role="alertdialog" aria-modal="true" aria-label={`Delete ${pendingDelete.name}`} className="w-full max-w-md rounded-xl animate-slide-in" style={{ background: 'var(--surface)', border: '1px solid var(--border-bright)', boxShadow: 'var(--elev-3)' }}>
            <div className="px-5 py-4 text-sm font-semibold" style={{ color: 'var(--text)', borderBottom: '1px solid var(--border)' }}>Delete {pendingDelete.kind === 'theme' ? 'theme' : 'editor group'}?</div>
            <div className="p-5 text-sm" style={{ color: 'var(--text-2)' }}>“{pendingDelete.name}” will be permanently deleted. This cannot be undone or restored.</div>
            <div className="flex justify-end gap-2 px-5 py-4" style={{ borderTop: '1px solid var(--border)' }}>
              <Button size="sm" onClick={() => setPendingDelete(null)}>Cancel</Button>
              <Button size="sm" variant="danger" onClick={() => {
                if (pendingDelete.kind === 'theme') void removeSelectedTheme()
                else saveEditorSettings(editorAssociationsDraft.filter((item) => item.id !== pendingDelete.id))
                setPendingDelete(null)
              }}>Delete permanently</Button>
            </div>
          </div>
        </div>
      )}
    </div>
  )
}
