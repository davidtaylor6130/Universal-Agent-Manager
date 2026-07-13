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
import { ThemeToggle } from '../shared/ThemeToggle'
import { useTheme } from '../../hooks/useTheme'
import type { Provider } from '../../types/provider'
import { ProviderLogo } from '../shared/ProviderLogo'
import { useShallow } from 'zustand/react/shallow'
import { ChevronDown, ChevronRight, X, Check } from 'lucide-react'
import { Button, IconButton } from '../ui'
import { ShellActionsSettings } from './ShellActionsSettings'
import {
  DEFAULT_PROVIDER_ID,
  providerCapabilities,
  providerShortName,
} from '../../utils/providerMetadata'

interface MemoryModelOption {
  id: string
  label: string
  detail: string
}

function providerDisplayName(provider?: Provider, fallbackId = '') {
  return providerShortName(provider, fallbackId)
}

function titleFromModelId(modelId: string) {
  const source = modelId.split('/').pop() ?? modelId
  return source
    .split(/[-_.]+/)
    .filter(Boolean)
    .map((part) => part.charAt(0).toUpperCase() + part.slice(1))
    .join(' ') || modelId
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

type SettingsSectionId = 'appearance' | 'defaults' | 'cli-version' | 'memory-settings' | 'memory-store' | 'editors' | 'shell-actions' | 'about'

interface SettingsSection {
  id: SettingsSectionId
  label: string
  detail: string
}

const SETTINGS_SECTIONS: SettingsSection[] = [
  { id: 'appearance', label: 'Appearance', detail: 'Theme and display' },
  { id: 'defaults', label: 'Chat Defaults', detail: 'Provider and new-chat settings' },
  { id: 'cli-version', label: 'CLI Version', detail: 'Run or revert provider CLIs' },
  { id: 'memory-settings', label: 'Memory Settings', detail: 'Defaults and workers' },
  { id: 'memory-store', label: 'Memory Store', detail: 'Library and backfill' },
  { id: 'editors', label: 'Editors', detail: 'Workspace launch presets' },
  { id: 'shell-actions', label: 'Shell Actions', detail: 'Finder and Explorer menus' },
  { id: 'about', label: 'About', detail: 'Version information' },
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
      className="rounded-xl p-4"
      style={{
        background: 'color-mix(in srgb, var(--surface-up) 78%, var(--surface))',
        border: '1px solid var(--border)',
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
          className="inline-flex h-6 w-6 shrink-0 items-center justify-center rounded-md text-sm"
          style={{
            background: expanded ? 'var(--accent-dim)' : 'var(--surface-up)',
            color: 'var(--text-2)',
            border: '1px solid var(--border)',
          }}
        >
          {expanded ? <ChevronDown size={14} /> : <ChevronRight size={14} />}
        </span>
      </button>

      {expanded && (
        <div
          id={panelId}
          className="mt-3 pt-3"
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
  const memoryEnabledDefault = useAppStore((s) => s.memoryEnabledDefault)
  const memoryIdleDelaySeconds = useAppStore((s) => s.memoryIdleDelaySeconds)
  const memoryRecallBudgetBytes = useAppStore((s) => s.memoryRecallBudgetBytes)
  const memoryWorkerBindings = useAppStore(useShallow((s) => s.memoryWorkerBindings))
  const memoryLastStatus = useAppStore((s) => s.memoryLastStatus)
  const memoryActivity = useAppStore(useShallow((s) => s.memoryActivity))
  const cliVersionManager = useAppStore(useShallow((s) => s.cliVersionManager))
  const markdownStoreDirectory = useAppStore((s) => s.markdownStoreDirectory)
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
  const openMarkdownStore = useAppStore((s) => s.openMarkdownStore)
  const openGlobalMemoryLibrary = useAppStore((s) => s.openGlobalMemoryLibrary)
  const openMemoryScanModal = useAppStore((s) => s.openMemoryScanModal)
  const { theme } = useTheme()
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
  const memoryMenuRef = useRef<HTMLDivElement>(null)
  const editorMenuRef = useRef<HTMLDivElement>(null)
  const defaultsMenuRef = useRef<HTMLDivElement>(null)
  const cliVersionMenuRef = useRef<HTMLDivElement>(null)

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
          type="button"
          title={label}
          aria-haspopup="listbox"
          aria-expanded={openDefaultsMenu === menuId}
          onClick={() => setOpenDefaultsMenu(openDefaultsMenu === menuId ? null : menuId)}
          className="w-full text-left"
          style={{
            background: 'var(--surface-up)',
            color: 'var(--text)',
            border: '1px solid var(--border)',
            borderRadius: 8,
            padding: '8px 10px',
          }}
        >
          <span className="inline-flex items-center gap-2 min-w-0">
            {selectedOption ? renderOptionIcon?.(selectedOption) : null}
            <span className="truncate">{selectedOption?.label ?? titleFromModelId(value)}</span>
          </span>
        </button>
        {openDefaultsMenu === menuId && (
          <div
            role="listbox"
            aria-label={label}
            className="absolute left-0 right-0"
            style={{
              top: 38,
              zIndex: 70,
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
                  className="w-full flex items-center gap-2 text-left px-2 py-2"
                  style={{
                    borderRadius: 6,
                    background: selected ? 'var(--accent-dim)' : 'transparent',
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
          </div>
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
          type="button"
          title={label}
          aria-haspopup="listbox"
          aria-expanded={openCliVersionMenu === menuId}
          disabled={disabled}
          onClick={() => setOpenCliVersionMenu(openCliVersionMenu === menuId ? null : menuId)}
          className="w-full text-left disabled:opacity-50"
          style={{
            background: 'var(--surface-up)',
            color: 'var(--text)',
            border: '1px solid var(--border)',
            borderRadius: 8,
            padding: '8px 10px',
            cursor: disabled ? 'default' : 'pointer',
          }}
        >
          <span className="truncate">{selectedOption?.label ?? 'No versions available'}</span>
        </button>
        {!disabled && openCliVersionMenu === menuId && (
          <div
            role="listbox"
            aria-label={label}
            className="absolute left-0 right-0"
            style={{
              top: 38,
              zIndex: 70,
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
                  {option.detail && (
                    <span className="text-[11px]" style={{ color: 'var(--text-3)' }}>
                      {option.detail}
                    </span>
                  )}
                </button>
              )
            })}
          </div>
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
        type="button"
        title={label}
        aria-haspopup="listbox"
        aria-expanded={openEditorMenu === menuId}
        onClick={() => setOpenEditorMenu(openEditorMenu === menuId ? null : menuId)}
        className="w-full text-left"
        style={{
          background: 'var(--surface-up)',
          color: 'var(--text)',
          border: '1px solid var(--border)',
          borderRadius: 8,
          padding: '8px 10px',
        }}
      >
        {editorPresetLabel(value)}
      </button>
      {openEditorMenu === menuId && (
        <div
          role="listbox"
          aria-label={label}
          className="absolute left-0 right-0"
          style={{
            top: 38,
            zIndex: 60,
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
                className="w-full flex items-center gap-2 text-left px-2 py-2"
                style={{
                  borderRadius: 6,
                  background: selected ? 'var(--accent-dim)' : 'transparent',
                  color: selected ? 'var(--text)' : 'var(--text-2)',
                }}
              >
                <span className="flex-1">{preset.label}</span>
                {selected && <Check size={13} style={{ color: 'var(--green)' }} aria-hidden />}
              </button>
            )
          })}
        </div>
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
  const renderSectionContent = () => {
    if (selectedSection === 'appearance') {
      return (
        <div className="space-y-4">
          <SectionCard
            title="Appearance"
            description="Choose how Universal Agent Manager looks across the app."
          >
            <div className="flex items-center justify-between gap-4">
              <div>
                <div className="text-sm" style={{ color: 'var(--text)' }}>
                  Theme
                </div>
                <div className="text-xs mt-0.5" style={{ color: 'var(--text-3)' }}>
                  {theme === 'system' ? 'System theme' : theme === 'dark' ? 'Dark mode' : 'Light mode'} active
                </div>
              </div>
              <ThemeToggle />
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
                  const expanded = expandedDefaultProviders[provider.id] ?? false
                  const modeOptions = [
                    { id: 'default', label: 'Default', detail: 'Use the provider default mode' },
                    { id: 'plan', label: 'Plan', detail: 'Ask the provider to plan first' },
                    ...(caps.hasAcceptEditsMode ? [{ id: 'acceptEdits', label: 'Accept Edits', detail: 'Auto-approve workspace file edits' }] : []),
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
                            <div>Mode</div>
                            {renderDefaultsMenu(
                              `${provider.id}:mode`,
                              defaults.approvalMode,
                              `${providerName} default mode`,
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
                        <div className="flex flex-wrap gap-2">
                          <Button
                            variant={defaults.autoApproveCommands ? 'primary' : 'secondary'}
                            size="sm"
                            onClick={() => updateProviderDefaults(provider.id, { ...defaults, autoApproveCommands: !defaults.autoApproveCommands })}
                          >
                            {defaults.autoApproveCommands ? 'Auto approve on' : 'Auto approve off'}
                          </Button>
                          <Button
                            variant={defaults.memoryEnabled ? 'primary' : 'secondary'}
                            size="sm"
                            onClick={() => updateProviderDefaults(provider.id, { ...defaults, memoryEnabled: !defaults.memoryEnabled })}
                          >
                            {defaults.memoryEnabled ? 'Memory on' : 'Memory off'}
                          </Button>
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
                      <Button
                        variant="secondary"
                        size="sm"
                        disabled={manager.running}
                        onClick={() => void refreshCliProviderVersion(manager.providerId)}
                      >
                        Refresh
                      </Button>
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
                        <Button
                          variant={canApplyCliVersion ? 'primary' : 'secondary'}
                          size="sm"
                          disabled={!canApplyCliVersion}
                          onClick={() => {
                            if (!window.confirm(`Install ${providerName} ${selectedCliVersion}?`)) return
                            void applyCliProviderVersion(manager.providerId, selectedCliVersion)
                          }}
                        >
                          {manager.running ? 'Running' : 'Apply'}
                        </Button>
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
              <div className="flex items-center justify-between gap-4">
                <div>
                  <div className="text-sm" style={{ color: 'var(--text)' }}>Memory</div>
                  <div className="text-xs mt-0.5" style={{ color: 'var(--text-3)' }}>
                    New chats default {memoryEnabledDefault ? 'on' : 'off'}
                  </div>
                </div>
                <Button
                  variant={memoryEnabledDefault ? 'primary' : 'secondary'}
                  size="sm"
                  onClick={() => void setMemorySettings({ memoryEnabledDefault: !memoryEnabledDefault })}
                >
                  {memoryEnabledDefault ? 'On' : 'Off'}
                </Button>
              </div>

              <div className="grid grid-cols-2 gap-3">
                <label className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
                  Idle delay
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
                  Recall budget bytes
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
                          <div
                            role="listbox"
                            aria-label={`${providerDisplayName(provider, provider.id)} memory worker provider`}
                            className="absolute left-0 right-0"
                            style={{
                              top: 38,
                              zIndex: 60,
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
                          </div>
                        )}
                      </div>

                      <div className="relative">
                        <button
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
                          <div
                            role="listbox"
                            aria-label={`${providerDisplayName(provider, provider.id)} memory worker model`}
                            className="absolute left-0 right-0"
                            style={{
                              top: 38,
                              zIndex: 60,
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
                          </div>
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
            title="Markdown Store"
            description="Publish and attach internal `.uam` markdown files from a shared directory."
          >
            <div className="grid gap-3">
              <div className="flex items-center gap-2">
                <input
                  value={markdownStoreDraftDirectory}
                  onChange={(event) => setMarkdownStoreDraftDirectory(event.target.value)}
                  placeholder="Markdown Store directory"
                  className="min-w-0 flex-1 text-xs"
                  style={{
                    border: '1px solid var(--border)',
                    borderRadius: 8,
                    background: 'var(--bg)',
                    color: 'var(--text)',
                    padding: '8px 10px',
                    outline: 'none',
                  }}
                />
                <Button
                  variant="secondary"
                  size="sm"
                  onClick={() => {
                    void browseMarkdownStoreDirectory(markdownStoreDraftDirectory).then((selected) => {
                      if (selected) setMarkdownStoreDraftDirectory(selected)
                    })
                  }}
                >
                  Browse
                </Button>
                <Button
                  variant="primary"
                  size="sm"
                  onClick={() => void setMarkdownStoreDirectory(markdownStoreDraftDirectory)}
                >
                  Save
                </Button>
              </div>
              <div className="flex items-center justify-between gap-4">
                <div className="text-xs" style={{ color: 'var(--text-3)' }}>
                  Attach entries to chats as file path references.
                </div>
                <Button
                  variant="primary"
                  size="sm"
                  onClick={() => void openMarkdownStore()}
                >
                  Open store
                </Button>
              </div>
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
                          <Button
                            variant="secondary"
                            size="sm"
                            disabled={editorAssociationsDraft.length <= 1}
                            onClick={() => saveEditorSettings(editorAssociationsDraft.filter((item) => item.id !== association.id))}
                          >
                            Delete
                          </Button>
                        </div>
                      </div>
                    </ProviderDisclosureCard>
                  )
                })}
              </div>

              <Button
                variant="secondary"
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
              >
                Add group
              </Button>
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
              <span style={{ color: 'var(--text)' }}>V4.1.0</span>
            </div>
          </div>
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
        className="rounded-2xl shadow-2xl w-full max-w-5xl mx-4 animate-slide-in overflow-hidden flex flex-col"
        style={{
          background: 'var(--surface)',
          border: '1px solid var(--border-bright)',
          maxHeight: 'calc(100vh - 2rem)',
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
                    className="w-full text-left px-3 py-2.5 rounded-xl transition-colors"
                    style={{
                      background: active ? 'var(--surface)' : 'transparent',
                      border: active ? '1px solid var(--border-bright)' : '1px solid transparent',
                      boxShadow: active ? '0 8px 20px rgba(0, 0, 0, 0.08)' : 'none',
                    }}
                  >
                    <div className="text-sm font-medium" style={{ color: active ? 'var(--text)' : 'var(--text-2)' }}>
                      {section.label}
                    </div>
                    <div className="text-[11px] mt-0.5" style={{ color: 'var(--text-3)' }}>
                      {section.detail}
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
            {renderSectionContent()}
          </div>
        </div>

        <div
          className="px-5 py-4"
          style={{ borderTop: '1px solid var(--border)' }}
        >
          <Button
            variant="primary"
            block
            size="md"
            onClick={() => setSettingsOpen(false)}
          >
            Close
          </Button>
        </div>
      </div>
    </div>
  )
}
