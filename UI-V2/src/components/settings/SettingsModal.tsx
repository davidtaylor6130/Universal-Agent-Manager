import { useEffect, useRef, useState, type ReactNode } from 'react'
import {
  MAX_MEMORY_IDLE_DELAY_SECONDS,
  MAX_MEMORY_RECALL_BUDGET_BYTES,
  MIN_MEMORY_IDLE_DELAY_SECONDS,
  MIN_MEMORY_RECALL_BUDGET_BYTES,
  useAppStore,
  type CliVersionProviderState,
  type EditorFileAssociation,
  type MemoryWorkerBinding,
  type ProviderChatDefaults,
} from '../../store/useAppStore'
import { useTheme } from '../../hooks/useTheme'
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
import { MEMORY_LEVEL_OPTIONS } from '../../types/memory'
import { ProviderLogo } from '../shared/ProviderLogo'
import { useShallow } from 'zustand/react/shallow'
import { BookOpen, Brain, Check, ChevronDown, ChevronRight, Download, FolderOpen, Info, MemoryStick, MessageSquare, Mic, Minus, MousePointerClick, Palette, Pencil, Plus, RefreshCw, Save, Target, TerminalSquare, Trash2, X, type LucideIcon } from 'lucide-react'
import { Button, IconButton, MenuSelect, Switch, ViewportMenu } from '../ui'
import { ShellActionsSettings } from './ShellActionsSettings'
import {
  DEFAULT_PROVIDER_ID,
  providerCapabilities,
  providerShortName,
} from '../../utils/providerMetadata'
import { titleFromModelId } from '../chat/modelOptions'

interface MemoryModelOption {
  id: string
  label: string
  detail: string
}

function providerDisplayName(provider?: Provider, fallbackId = '') {
  return providerShortName(provider, fallbackId)
}

function memoryModelOptions(provider?: Provider, providerId = '', selectedModelId = '') {
  const caps = providerCapabilities(providerId, provider)
  const baseOptions = caps.memoryModelIds.map((id) => ({
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

type SettingsSectionId = 'appearance' | 'defaults' | 'cli-version' | 'voice-input' | 'memory-settings' | 'memory-store' | 'markdown-store' | 'goal-loops' | 'editors' | 'shell-actions' | 'about'

interface SettingsSection {
  id: SettingsSectionId
  label: string
  detail: string
  icon: LucideIcon
}

const SETTINGS_SECTIONS: SettingsSection[] = [
  { id: 'appearance', label: 'Appearance', detail: 'Theme and display', icon: Palette },
  { id: 'defaults', label: 'Chat Defaults', detail: 'Provider and new-chat settings', icon: MessageSquare },
  { id: 'cli-version', label: 'CLI Version', detail: 'Run or revert provider CLIs', icon: TerminalSquare },
  { id: 'voice-input', label: 'Voice Input', detail: 'Speech-to-text provider', icon: Mic },
  { id: 'memory-settings', label: 'Memory Settings', detail: 'Defaults and workers', icon: Brain },
  { id: 'memory-store', label: 'Memory Store', detail: 'Library and backfill', icon: MemoryStick },
  { id: 'markdown-store', label: 'Skills', detail: 'Reusable prompts and attachments', icon: BookOpen },
  { id: 'goal-loops', label: 'Goal Loops', detail: 'Loop safety', icon: Target },
  { id: 'editors', label: 'Editors', detail: 'Workspace launch presets', icon: Pencil },
  { id: 'shell-actions', label: 'Shell Actions', detail: 'Finder and Explorer menus', icon: MousePointerClick },
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
  if (manager.status === 'supported') return 'Installed version is supported'
  if (manager.status === 'unsupported') return 'Installed version is not in the curated list'
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
  const acpBindings = useAppStore(useShallow((s) => s.acpBindingBySessionId))
  const discoverProviderModels = useAppStore((s) => s.discoverProviderModels)
  const memoryEnabledDefault = useAppStore((s) => s.memoryEnabledDefault)
  const memoryLevelDefault = useAppStore((s) => s.memoryLevelDefault)
  const memoryIdleDelaySeconds = useAppStore((s) => s.memoryIdleDelaySeconds)
  const memoryRecallBudgetBytes = useAppStore((s) => s.memoryRecallBudgetBytes)
  const goalMaxLoopIterations = useAppStore((s) => s.goalMaxLoopIterations)
  const appVersion = useAppStore((s) => s.appVersion)
  const showProviderIconsInSidebar = useAppStore((s) => s.showProviderIconsInSidebar)
  const showWorktreePathInSidebar = useAppStore((s) => s.showWorktreePathInSidebar)
  const setSidebarSettings = useAppStore((s) => s.setSidebarSettings)
  const updateChecksEnabled = useAppStore((s) => s.updateChecksEnabled)
  const setUpdateSettings = useAppStore((s) => s.setUpdateSettings)
  const customThemes = useAppStore(useShallow((s) => s.customThemes))
  const refreshCustomThemes = useAppStore((s) => s.refreshCustomThemes)
  const saveCustomTheme = useAppStore((s) => s.saveCustomTheme)
  const deleteCustomTheme = useAppStore((s) => s.deleteCustomTheme)
  const memoryWorkerBindings = useAppStore(useShallow((s) => s.memoryWorkerBindings))
  const memoryLastStatus = useAppStore((s) => s.memoryLastStatus)
  const memoryActivity = useAppStore(useShallow((s) => s.memoryActivity))
  const cliVersionManager = useAppStore(useShallow((s) => s.cliVersionManager))
  const markdownStoreDirectory = useAppStore((s) => s.markdownStoreDirectory)
  const savedVoiceMode = useAppStore((s) => s.voiceInputMode)
  const savedVoiceServerBaseUrl = useAppStore((s) => s.voiceInputServerBaseUrl)
  const savedVoiceServerEndpoint = useAppStore((s) => s.voiceInputServerEndpoint)
  const savedVoiceServerModel = useAppStore((s) => s.voiceInputServerModel)
  const savedVoiceCredentialEnv = useAppStore((s) => s.voiceInputApiKeyEnv)
  const voiceCapabilities = useAppStore((s) => s.voiceInputCapabilities)
  const defaultNewChatProviderId = useAppStore((s) => s.defaultNewChatProviderId)
  const providerChatDefaults = useAppStore(useShallow((s) => s.providerChatDefaults))
  const defaultEditorPresetId = useAppStore((s) => s.defaultEditorPresetId)
  const editorFileAssociations = useAppStore(useShallow((s) => s.editorFileAssociations))
  const setMemorySettings = useAppStore((s) => s.setMemorySettings)
  const setProviderChatDefaults = useAppStore((s) => s.setProviderChatDefaults)
  const setEditorSettings = useAppStore((s) => s.setEditorSettings)
  const refreshCliProviderVersion = useAppStore((s) => s.refreshCliProviderVersion)
  const applyCliProviderVersion = useAppStore((s) => s.applyCliProviderVersion)
  const browseMarkdownStoreDirectory = useAppStore((s) => s.browseMarkdownStoreDirectory)
  const setMarkdownStoreDirectory = useAppStore((s) => s.setMarkdownStoreDirectory)
  const setVoiceInputSettings = useAppStore((s) => s.setVoiceInputSettings)
  const openMarkdownStore = useAppStore((s) => s.openMarkdownStore)
  const openGlobalMemoryLibrary = useAppStore((s) => s.openGlobalMemoryLibrary)
  const openMemoryScanModal = useAppStore((s) => s.openMemoryScanModal)
  const { theme, setTheme } = useTheme()
  const [openMemoryMenu, setOpenMemoryMenu] = useState<string | null>(null)
  const [openEditorMenu, setOpenEditorMenu] = useState<string | null>(null)
  const [openDefaultsMenu, setOpenDefaultsMenu] = useState<string | null>(null)
  const [openCliVersionMenu, setOpenCliVersionMenu] = useState<string | null>(null)
  const [selectedCliVersions, setSelectedCliVersions] = useState<Record<string, string>>({})
  const [expandedDefaultProviders, setExpandedDefaultProviders] = useState<Record<string, boolean>>({})
  const [expandedCliVersionProviders, setExpandedCliVersionProviders] = useState<Record<string, boolean>>({})
  const [expandedEditorGroups, setExpandedEditorGroups] = useState<Record<string, boolean>>({})
  const [markdownStoreDraftDirectory, setMarkdownStoreDraftDirectory] = useState(markdownStoreDirectory)
  const [editorAssociationsDraft, setEditorAssociationsDraft] = useState(editorFileAssociations)
  const [defaultEditorDraft, setDefaultEditorDraft] = useState(defaultEditorPresetId)
  const [selectedSection, setSelectedSection] = useState<SettingsSectionId>('appearance')
  const [themeDraft, setThemeDraft] = useState<CustomTheme | null>(null)
  const [themeMessage, setThemeMessage] = useState('')
  const [voiceMode, setVoiceMode] = useState(savedVoiceMode)
  const [voiceServerUrl, setVoiceServerUrl] = useState(savedVoiceServerBaseUrl)
  const [voiceServerEndpoint, setVoiceServerEndpoint] = useState(savedVoiceServerEndpoint)
  const [voiceServerModel, setVoiceServerModel] = useState(savedVoiceServerModel)
  const [voiceCredentialEnv, setVoiceCredentialEnv] = useState(savedVoiceCredentialEnv)
  const [voiceSaving, setVoiceSaving] = useState(false)
  const [voiceMessage, setVoiceMessage] = useState('')
  const [pendingDelete, setPendingDelete] = useState<{ kind: 'theme' | 'editor'; id: string; name: string } | null>(null)
  const memoryMenuRef = useRef<HTMLDivElement>(null)
  const editorMenuRef = useRef<HTMLDivElement>(null)
  const defaultsMenuRef = useRef<HTMLDivElement>(null)
  const cliVersionMenuRef = useRef<HTMLDivElement>(null)
  const popupAnchorsRef = useRef(new Map<string, HTMLButtonElement>())
  const themeImportRef = useRef<HTMLInputElement>(null)

  useEffect(() => {
    void refreshCustomThemes()
  }, [refreshCustomThemes])

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
      if (openMemoryMenu) {
        setOpenMemoryMenu(null)
        return
      }
      if (openEditorMenu) {
        setOpenEditorMenu(null)
        return
      }
      if (openDefaultsMenu) {
        setOpenDefaultsMenu(null)
        return
      }
      if (openCliVersionMenu) {
        setOpenCliVersionMenu(null)
        return
      }
      setSettingsOpen(false)
    }
    window.addEventListener('keydown', handler)
    return () => window.removeEventListener('keydown', handler)
  }, [openCliVersionMenu, openDefaultsMenu, openEditorMenu, openMemoryMenu, setSettingsOpen])

  useEffect(() => {
    const handler = (event: MouseEvent) => {
      const target = event.target
      if (!(target instanceof Node)) return
      if (target instanceof Element && target.closest('[data-viewport-menu]')) return
      if (!memoryMenuRef.current?.contains(target)) setOpenMemoryMenu(null)
      if (!editorMenuRef.current?.contains(target)) setOpenEditorMenu(null)
      if (!defaultsMenuRef.current?.contains(target)) setOpenDefaultsMenu(null)
      if (!cliVersionMenuRef.current?.contains(target)) setOpenCliVersionMenu(null)
    }
    document.addEventListener('mousedown', handler)
    return () => document.removeEventListener('mousedown', handler)
  }, [])

  useEffect(() => {
    setMarkdownStoreDraftDirectory(markdownStoreDirectory)
  }, [markdownStoreDirectory])

  useEffect(() => {
    setVoiceMode(savedVoiceMode)
    setVoiceServerUrl(savedVoiceServerBaseUrl)
    setVoiceServerEndpoint(savedVoiceServerEndpoint)
    setVoiceServerModel(savedVoiceServerModel)
    setVoiceCredentialEnv(savedVoiceCredentialEnv)
  }, [savedVoiceCredentialEnv, savedVoiceMode, savedVoiceServerBaseUrl, savedVoiceServerEndpoint, savedVoiceServerModel])

  useEffect(() => {
    setEditorAssociationsDraft(editorFileAssociations)
  }, [editorFileAssociations])

  useEffect(() => {
    setDefaultEditorDraft(defaultEditorPresetId)
  }, [defaultEditorPresetId])

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
      approvalMode: saved?.approvalMode ?? 'default',
      autoApproveCommands: saved?.autoApproveCommands ?? false,
      memoryLevel: saved?.memoryLevel ?? memoryLevelDefault,
      memoryEnabled: saved?.memoryEnabled ?? memoryEnabledDefault,
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
    setOpenDefaultsMenu(null)
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
    menuId: string,
    value: string,
    label: string,
    options: MemoryModelOption[],
    onSelect: (value: string) => void,
    renderOptionIcon?: (option: MemoryModelOption) => ReactNode
  ) => {
    const selectedOption = options.find((option) => option.id === value) ?? options[0]
    return (
      <div className="relative">
        <button
          ref={(element) => { if (element) popupAnchorsRef.current.set(menuId, element) }}
          type="button"
          title={label}
          aria-haspopup="listbox"
          aria-expanded={openDefaultsMenu === menuId}
          onClick={() => setOpenDefaultsMenu(openDefaultsMenu === menuId ? null : menuId)}
          className="uam-menu-select__trigger flex w-full items-center justify-between gap-2 text-left"
          style={{
            color: 'var(--text)',
            borderRadius: 8,
            padding: '8px 10px',
          }}
        >
          <span className="inline-flex items-center gap-2 min-w-0">
            {selectedOption ? renderOptionIcon?.(selectedOption) : null}
            <span className="truncate">{selectedOption?.label ?? titleFromModelId(value)}</span>
          </span>
          <ChevronDown className={openDefaultsMenu === menuId ? 'uam-menu-select__chevron is-open' : 'uam-menu-select__chevron'} size={14} aria-hidden />
        </button>
        {openDefaultsMenu === menuId && (
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
                  key={option.id || 'default'}
                  type="button"
                  role="option"
                  aria-selected={selected}
                  onClick={() => {
                    onSelect(option.id)
                    setOpenDefaultsMenu(null)
                  }}
                  className={`uam-menu-select__option w-full flex items-center gap-2 text-left px-2 py-2${selected ? ' is-selected' : ''}`}
                  style={{
                    borderRadius: 6,
                    color: selected ? 'var(--text)' : 'var(--text-2)',
                  }}
                >
                  {renderOptionIcon?.(option)}
                  <span className="flex-1 min-w-0">
                    <span className="block truncate">{option.label}</span>
                    {option.detail && (
                      <span className="block truncate text-[11px]" style={{ color: 'var(--text-3)' }}>
                        {option.detail}
                      </span>
                    )}
                  </span>
                  {selected && <Check size={13} style={{ color: 'var(--green)' }} aria-hidden />}
                </button>
              )
            })}
          </ViewportMenu>
        )}
      </div>
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
    const base = resolveDocumentTheme(theme)
    startNewTheme({ version: 1, id: 'custom:built-in', name: `${base === 'dark' ? 'Dark' : 'Light'}`, base, colors: { ...BUILT_IN_THEME_COLORS[base] } })
  }
  const saveThemeDraft = async () => {
    if (!themeDraft || !themeDraftValid) return
    const saved = await saveCustomTheme(themeDraft)
    if (!saved) {
      setThemeMessage('Theme could not be saved. Check its name and colors.')
      return
    }
    setThemeDraft(saved)
    setTheme(saved.id)
    setThemeMessage('Theme saved.')
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
        </div>
      )
    }

    if (selectedSection === 'defaults') {
      return (
        <div className="space-y-4">
          <SectionCard
            title="New Chat Defaults"
            description="Choose the provider preselected for new chats and the defaults each provider applies."
          >
            <div ref={defaultsMenuRef} className="grid gap-4">
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
                  const modelOptions = memoryModelOptions(provider, provider.id, defaults.modelId)
                  const caps = providerCapabilities(provider.id, provider)
                  const providerName = providerDisplayName(provider, provider.id)
                  const providerSession = sessions.find((session) => session.providerId === provider.id)
                  const modelsLoading = providerSession ? acpBindings[providerSession.id]?.modelsLoading : false
                  const modelRefreshError = providerSession ? acpBindings[providerSession.id]?.modelRefreshError : ''
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
                        <div className="grid grid-cols-2 gap-2">
                          <div className="grid gap-1">
                            <div>Model</div>
                            {renderDefaultsMenu(
                              `${provider.id}:model`,
                              defaults.modelId,
                              `${providerName} default model`,
                              modelOptions,
                              (modelId) => updateProviderDefaults(provider.id, { ...defaults, modelId })
                            )}
                          </div>
                          <div className="grid gap-1">
                            <div>Agent</div>
                            {renderDefaultsMenu(
                              `${provider.id}:mode`,
                              defaults.approvalMode,
                              `${providerName} default agent`,
                              modeOptions,
                              (approvalMode) => updateProviderDefaults(provider.id, { ...defaults, approvalMode })
                            )}
                          </div>
                          {caps.hasReasoningEffort && (
                            <div className="grid gap-1">
                              <div>Reasoning</div>
                              {renderDefaultsMenu(
                                `${provider.id}:reasoning`,
                                defaults.reasoningEffort,
                                `${providerName} default reasoning`,
                                caps.reasoningOptions,
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
                                caps.speedOptions,
                                (serviceTier) => updateProviderDefaults(provider.id, { ...defaults, serviceTier })
                              )}
                            </div>
                          )}
                        </div>
                        <div className="grid grid-cols-2 gap-2">
                          <Button
                            variant={defaults.autoApproveCommands ? 'primary' : 'secondary'}
                            size="sm"
                            onClick={() => updateProviderDefaults(provider.id, { ...defaults, autoApproveCommands: !defaults.autoApproveCommands })}
                          >
                            {defaults.autoApproveCommands ? 'Auto approve on' : 'Auto approve off'}
                          </Button>
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
                        <div className="flex items-center gap-2">
                          <IconButton icon={<RefreshCw size={14} className={modelsLoading ? 'animate-spin' : ''} />} label={`Refresh ${providerName} models`} disabled={!providerSession || modelsLoading} onClick={() => providerSession && void discoverProviderModels(providerSession.id)} />
                          <span role="status" className="text-xs" style={{ color: modelRefreshError ? 'var(--red)' : 'var(--text-3)' }}>
                            {modelsLoading ? 'Refreshing models…' : modelRefreshError || (providerSession ? 'Cached models are ready' : 'Open a chat to refresh models')}
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
                          <div className="text-xs mt-1" style={{ color: manager.status === 'unsupported' ? 'var(--red)' : 'var(--text-3)' }}>
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
                          onClick={() => {
                            if (!window.confirm(`Install ${providerName} ${selectedCliVersion}?`)) return
                            void applyCliProviderVersion(manager.providerId, selectedCliVersion)
                          }}
                        />
                      </div>
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
                const providerMenuId = `${provider.id}:provider`
                const modelMenuId = `${provider.id}:model`
                const modelOptions = memoryModelOptions(workerProvider, binding.workerProviderId, binding.workerModelId)
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
      let serverUrlError = ''
      if (voiceServerUrl.trim()) {
        try {
          const url = new URL(voiceServerUrl)
          if (url.protocol !== 'https:' && !['localhost', '127.0.0.1', '::1'].includes(url.hostname)) serverUrlError = 'Use HTTPS unless the server runs on localhost.'
        } catch {
          serverUrlError = 'Enter a valid server URL.'
        }
      }
      const voiceDirty = voiceMode !== savedVoiceMode || voiceServerUrl !== savedVoiceServerBaseUrl || voiceServerEndpoint !== savedVoiceServerEndpoint || voiceServerModel !== savedVoiceServerModel || voiceCredentialEnv !== savedVoiceCredentialEnv
      const selectedCapability = voiceCapabilities[voiceMode]
      const serverInvalid = voiceMode === 'server' && (!!serverUrlError || !voiceServerUrl.trim() || !voiceServerEndpoint.trim() || !voiceServerModel.trim() || !voiceCredentialEnv.trim())
      return (
        <SectionCard title="Voice Input" description="Choose where recorded audio is transcribed. Audio is only sent to the selected service.">
          <div className="grid gap-4">
            <MenuSelect
              label="Speech-to-text service"
              value={voiceMode}
              options={[
                { value: 'system', label: 'System speech recognition', description: voiceCapabilities.system.reason || 'Use the operating system speech service.' },
                { value: 'local', label: 'Local AI model · Coming soon', description: voiceCapabilities.local.reason || 'Requires a compatible local model and hardware check.' },
                { value: 'server', label: 'OpenAI-compatible server', description: voiceCapabilities.server.reason || 'Send recorded audio to an audio transcription API.' },
              ]}
              onChange={(value) => {
                const next = value as typeof voiceMode
                if (voiceCapabilities[next].supported) setVoiceMode(next)
                else setVoiceMessage(voiceCapabilities[next].reason || 'This speech-to-text service is unavailable.')
              }}
            />
            {voiceMode === 'system' && <div role="status" className="rounded-lg p-3 text-xs animate-fade-in" style={{ color: 'var(--text-2)', background: 'var(--surface)', border: '1px solid var(--border)' }}>Uses the current on-device/system dictation service. Choose a server for AI speech-to-text.</div>}
            {voiceMode === 'local' && <div role="status" className="rounded-lg p-3 text-xs animate-fade-in" style={{ color: 'var(--text-2)', background: 'var(--surface)', border: '1px solid var(--border)' }}>Local AI transcription is coming soon. UAM will enable it only after confirming compatible hardware and an installed model.</div>}
            {voiceMode === 'server' && (
              <div className="grid gap-3 rounded-lg p-3 animate-fade-in" style={{ background: 'var(--surface)', border: '1px solid var(--border)' }}>
                <label className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>Server base URL<input aria-label="Voice transcription server URL" value={voiceServerUrl} onChange={(event) => setVoiceServerUrl(event.target.value)} placeholder="https://api.example.com" className="rounded-lg px-3 py-2 outline-none transition-colors focus:border-[var(--accent)]" style={{ color: 'var(--text)', background: 'var(--bg)', border: `1px solid ${serverUrlError ? 'var(--red)' : 'var(--border)'}` }} />{serverUrlError && <span role="alert" style={{ color: 'var(--red)' }}>{serverUrlError}</span>}</label>
                <label className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>Transcription endpoint<input aria-label="Voice transcription endpoint" value={voiceServerEndpoint} onChange={(event) => setVoiceServerEndpoint(event.target.value)} placeholder="/v1/audio/transcriptions" className="rounded-lg px-3 py-2 font-mono outline-none" style={{ color: 'var(--text)', background: 'var(--bg)', border: '1px solid var(--border)' }} /></label>
                <label className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>Model<input aria-label="Voice transcription model" value={voiceServerModel} onChange={(event) => setVoiceServerModel(event.target.value)} placeholder="whisper-1" className="rounded-lg px-3 py-2 outline-none" style={{ color: 'var(--text)', background: 'var(--bg)', border: '1px solid var(--border)' }} /></label>
                <label className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>Credential environment variable<input aria-label="Voice transcription credential environment variable" value={voiceCredentialEnv} onChange={(event) => setVoiceCredentialEnv(event.target.value.replace(/[^A-Za-z0-9_]/g, '').toUpperCase())} placeholder="OPENAI_API_KEY" className="rounded-lg px-3 py-2 font-mono outline-none" style={{ color: 'var(--text)', background: 'var(--bg)', border: '1px solid var(--border)' }} /><span style={{ color: 'var(--text-3)' }}>UAM reads this environment variable; it does not store the secret.</span></label>
              </div>
            )}
            {voiceMessage && <div role="status" className="text-xs" style={{ color: voiceMessage.includes('saved') ? 'var(--green)' : 'var(--red)' }}>{voiceMessage}</div>}
            <div className="flex justify-end"><IconButton variant="solid" icon={<Save size={15} />} label="Save voice input settings" disabled={voiceSaving || !voiceDirty || !selectedCapability.supported || serverInvalid} onClick={() => {
              setVoiceSaving(true)
              setVoiceMessage('')
              void setVoiceInputSettings({ voiceInputMode: voiceMode, voiceInputServerBaseUrl: voiceServerUrl.trim(), voiceInputServerEndpoint: voiceServerEndpoint.trim(), voiceInputServerModel: voiceServerModel.trim(), voiceInputApiKeyEnv: voiceCredentialEnv.trim() }).then((saved) => {
                setVoiceSaving(false)
                setVoiceMessage(saved ? 'Voice input settings saved.' : 'Voice input settings could not be saved.')
              })
            }} /></div>
          </div>
        </SectionCard>
      )
    }

    if (selectedSection === 'memory-store') {
      return (
        <div className="space-y-4">
          <SectionCard
            title="Memory Library"
            description="Browse, add, delete, and reveal global memory files without leaving the app."
          >
            <div className="flex items-center justify-between gap-4">
              <div>
                <div className="text-sm" style={{ color: 'var(--text)' }}>
                  Global memory browser
                </div>
                <div className="text-xs mt-0.5" style={{ color: 'var(--text-3)' }}>
                  Open the file-backed library for app-wide durable memories.
                </div>
              </div>
              <Button
                variant="primary"
                size="sm"
                onClick={() => void openGlobalMemoryLibrary()}
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
              <input value={markdownStoreDraftDirectory} onChange={(event) => setMarkdownStoreDraftDirectory(event.target.value)} placeholder="Skills directory" className="min-w-0 flex-1 text-xs" style={{ border: '1px solid var(--border)', borderRadius: 8, background: 'var(--bg)', color: 'var(--text)', padding: '8px 10px', outline: 'none' }} />
              <IconButton icon={<FolderOpen size={15} />} label="Browse for Skills directory" onClick={() => { void browseMarkdownStoreDirectory(markdownStoreDraftDirectory).then((selected) => { if (selected) setMarkdownStoreDraftDirectory(selected) }) }} />
              <IconButton variant="solid" icon={<Save size={15} />} label="Save Skills directory" disabled={!markdownStoreDraftDirectory.trim() || markdownStoreDraftDirectory.trim() === markdownStoreDirectory} onClick={() => void setMarkdownStoreDirectory(markdownStoreDraftDirectory)} />
            </div>
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
          <div className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
            <span>Maximum iterations</span>
            <div className="flex w-fit items-center overflow-hidden rounded-lg" style={{ background: 'var(--surface)', border: '1px solid var(--border)' }}>
              <IconButton icon={<Minus size={14} />} label="Decrease maximum goal loop iterations" size="sm" disabled={goalMaxLoopIterations === 0} onClick={() => void setMemorySettings({ goalMaxLoopIterations: Math.max(0, goalMaxLoopIterations - 1) })} />
              <output aria-label="Maximum goal loop iterations" className="min-w-16 px-3 text-center tabular-nums" style={{ color: 'var(--text)' }}>{goalMaxLoopIterations || 'Unlimited'}</output>
              <IconButton icon={<Plus size={14} />} label="Increase maximum goal loop iterations" size="sm" onClick={() => void setMemorySettings({ goalMaxLoopIterations: goalMaxLoopIterations + 1 })} />
            </div>
            <span style={{ color: 'var(--text-3)' }}>Decrease to 0 for unlimited iterations.</span>
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
      className="fixed inset-0 z-50 flex items-center justify-center animate-fade-in"
      style={{ background: 'rgba(0,0,0,0.55)', backdropFilter: 'blur(4px)' }}
      onClick={(e) => { if (e.target === e.currentTarget) setSettingsOpen(false) }}
    >
      <div
        role="dialog"
        aria-modal="true"
        aria-label="Settings"
        tabIndex={-1}
        className="rounded-2xl shadow-2xl w-full max-w-5xl mx-4 animate-slide-in overflow-hidden flex flex-col"
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
            onClick={() => setSettingsOpen(false)}
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
                      setOpenDefaultsMenu(null)
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
            <div key={selectedSection} className="animate-fade-in">
              {renderSectionContent()}
            </div>
          </div>
        </div>

      </div>
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
