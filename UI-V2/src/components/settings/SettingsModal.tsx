import { useEffect, useRef, useState, type ReactNode } from 'react'
import { useAppStore, type MemoryWorkerBinding } from '../../store/useAppStore'
import { ThemeToggle } from '../shared/ThemeToggle'
import { useTheme } from '../../hooks/useTheme'
import type { Provider } from '../../types/provider'
import { ProviderLogo } from '../shared/ProviderLogo'

interface MemoryModelOption {
  id: string
  label: string
  detail: string
}

const GEMINI_MEMORY_MODELS: MemoryModelOption[] = [
  { id: '', label: 'CLI default', detail: 'Use Gemini CLI settings' },
  { id: 'auto-gemini-3', label: 'Auto 3', detail: 'Gemini 3 routing' },
  { id: 'auto-gemini-2.5', label: 'Auto 2.5', detail: 'Gemini 2.5 routing' },
  { id: 'pro', label: 'Pro', detail: 'Prioritize capability' },
  { id: 'flash', label: 'Flash', detail: 'Prioritize speed' },
  { id: 'flash-lite', label: 'Flash Lite', detail: 'Fastest option' },
]

const CODEX_MEMORY_MODELS: MemoryModelOption[] = [
  { id: '', label: 'CLI default', detail: 'Use Codex CLI settings' },
  { id: 'gpt-5.4', label: 'GPT-5.4', detail: 'Frontier coding model' },
  { id: 'gpt-5.4-mini', label: 'GPT-5.4 Mini', detail: 'Smaller fast model' },
  { id: 'gpt-5.2', label: 'GPT-5.2', detail: 'Balanced coding model' },
]

function providerDisplayName(provider?: Provider, fallbackId = '') {
  if (provider?.shortName?.trim()) return provider.shortName.trim()
  if (provider?.name?.trim()) return provider.name.trim()
  if (fallbackId === 'codex-cli') return 'Codex'
  if (fallbackId === 'claude-cli') return 'Claude'
  return 'Gemini'
}

function titleFromModelId(modelId: string) {
  const source = modelId.split('/').pop() ?? modelId
  return source
    .split(/[-_.]+/)
    .filter(Boolean)
    .map((part) => part.charAt(0).toUpperCase() + part.slice(1))
    .join(' ') || modelId
}

function isCodexProvider(provider?: Provider, providerId = '') {
  return providerId === 'codex-cli' || provider?.structuredProtocol === 'codex-app-server'
}

function isClaudeProvider(provider?: Provider, providerId = '') {
  return providerId === 'claude-cli' || provider?.structuredProtocol === 'claude-code-stream-json'
}

function memoryModelOptions(provider?: Provider, providerId = '', selectedModelId = '') {
  const baseOptions = isCodexProvider(provider, providerId)
    ? CODEX_MEMORY_MODELS
    : isClaudeProvider(provider, providerId)
      ? [
          { id: '', label: 'CLI default', detail: 'Use Claude Code settings' },
          { id: 'sonnet', label: 'Sonnet', detail: 'Latest Sonnet alias' },
          { id: 'opus', label: 'Opus', detail: 'Latest Opus alias' },
        ]
      : GEMINI_MEMORY_MODELS
  if (!selectedModelId || baseOptions.some((option) => option.id === selectedModelId)) return baseOptions
  return [
    ...baseOptions,
    { id: selectedModelId, label: titleFromModelId(selectedModelId), detail: selectedModelId },
  ]
}

function selectedMemoryModelLabel(options: MemoryModelOption[], modelId: string) {
  return options.find((option) => option.id === modelId)?.label ?? titleFromModelId(modelId)
}

type SettingsSectionId = 'appearance' | 'memory' | 'about'

interface SettingsSection {
  id: SettingsSectionId
  label: string
  detail: string
}

const SETTINGS_SECTIONS: SettingsSection[] = [
  { id: 'appearance', label: 'Appearance', detail: 'Theme and display' },
  { id: 'memory', label: 'Memory', detail: 'Defaults and workers' },
  { id: 'about', label: 'About', detail: 'Version information' },
]

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

export function SettingsModal() {
  const {
    setSettingsOpen,
    providers,
    memoryEnabledDefault,
    memoryIdleDelaySeconds,
    memoryRecallBudgetBytes,
    memoryWorkerBindings,
    memoryLastStatus,
    setMemorySettings,
  } = useAppStore()
  const { theme } = useTheme()
  const [openMemoryMenu, setOpenMemoryMenu] = useState<string | null>(null)
  const [selectedSection, setSelectedSection] = useState<SettingsSectionId>('appearance')
  const memoryMenuRef = useRef<HTMLDivElement>(null)

  useEffect(() => {
    const handler = (e: KeyboardEvent) => {
      if (e.key !== 'Escape') return
      if (openMemoryMenu) {
        setOpenMemoryMenu(null)
        return
      }
      setSettingsOpen(false)
    }
    window.addEventListener('keydown', handler)
    return () => window.removeEventListener('keydown', handler)
  }, [openMemoryMenu, setSettingsOpen])

  useEffect(() => {
    const handler = (event: MouseEvent) => {
      const target = event.target
      if (!(target instanceof Node)) return
      if (!memoryMenuRef.current?.contains(target)) setOpenMemoryMenu(null)
    }
    document.addEventListener('mousedown', handler)
    return () => document.removeEventListener('mousedown', handler)
  }, [])

  const updateMemoryBinding = (providerId: string, binding: MemoryWorkerBinding) => {
    void setMemorySettings({
      memoryWorkerBindings: {
        ...memoryWorkerBindings,
        [providerId]: binding,
      },
    })
  }

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
                  {theme === 'dark' ? 'Dark mode' : 'Light mode'} active
                </div>
              </div>
              <ThemeToggle />
            </div>
          </SectionCard>
        </div>
      )
    }

    if (selectedSection === 'memory') {
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
                <button
                  type="button"
                  onClick={() => void setMemorySettings({ memoryEnabledDefault: !memoryEnabledDefault })}
                  className="px-2 py-1 rounded-md text-xs"
                  style={{
                    background: memoryEnabledDefault ? 'var(--accent-dim)' : 'var(--surface)',
                    color: 'var(--text)',
                    border: '1px solid var(--border)',
                  }}
                >
                  {memoryEnabledDefault ? 'On' : 'Off'}
                </button>
              </div>

              <div className="grid grid-cols-2 gap-3">
                <label className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
                  Idle delay
                  <input
                    type="number"
                    min={30}
                    max={3600}
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
                    min={512}
                    max={8192}
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
                                  {selected && <span style={{ color: 'var(--green)', fontSize: 10 }}>●</span>}
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
                                    {selected && <span style={{ color: 'var(--green)', fontSize: 10 }}>●</span>}
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
              <span style={{ color: 'var(--text)' }}>V2.0.1</span>
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
        className="rounded-2xl shadow-2xl w-full max-w-5xl mx-4 animate-slide-in overflow-hidden"
        style={{
          background: 'var(--surface)',
          border: '1px solid var(--border-bright)',
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
          <button
            onClick={() => setSettingsOpen(false)}
            style={{
              background: 'transparent',
              color: 'var(--text-3)',
              border: 'none',
              cursor: 'pointer',
              fontFamily: 'inherit',
              fontSize: 12,
            }}
          >
            ✕
          </button>
        </div>

        <div className="grid md:grid-cols-[220px_minmax(0,1fr)] min-h-[560px]">
          <aside
            className="p-4"
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

          <div className="p-5 md:p-6 overflow-y-auto">
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
          <button
            onClick={() => setSettingsOpen(false)}
            className="w-full py-1.5 rounded-md text-xs font-medium transition-opacity duration-150"
            style={{
              background: 'var(--accent)',
              color: '#fff',
              border: 'none',
              cursor: 'pointer',
              fontFamily: 'inherit',
            }}
            onMouseEnter={(e) => { e.currentTarget.style.opacity = '0.88' }}
            onMouseLeave={(e) => { e.currentTarget.style.opacity = '1' }}
          >
            Close
          </button>
        </div>
      </div>
    </div>
  )
}
