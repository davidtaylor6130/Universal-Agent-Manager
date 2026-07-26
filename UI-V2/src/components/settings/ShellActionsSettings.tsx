import { useEffect, useRef, useState } from 'react'
import { BookOpen, Check, ChevronRight, File, Files, Folder, FolderOpen, MousePointerClick, Plus, Sparkles, Trash2 } from 'lucide-react'
import { useAppStore, type ShellAction } from '../../store/useAppStore'
import { DEFAULT_PROVIDER_ID, providerRuntimeDescription } from '../../utils/providerMetadata'
import { buildModelOptions } from '../chat/modelOptions'
import { ProviderLogo } from '../shared/ProviderLogo'
import { Button, IconButton, MenuSelect, Switch } from '../ui'

function newAction(): ShellAction {
  const token = globalThis.crypto?.randomUUID?.() ?? `${Date.now()}-${Math.random().toString(16).slice(2)}`
  return {
    id: `action-${token}`,
    label: 'New action',
    skillPath: '',
    providerId: '',
    modelId: '',
    acceptsFiles: true,
    acceptsFolders: true,
    enabled: true,
    openWorkspace: true,
  }
}

export function ShellActionsSettings() {
  const savedActions = useAppStore((s) => s.shellActions)
  const notification = useAppStore((s) => s.shellActionNotification)
  const markdownEntries = useAppStore((s) => s.markdownStoreEntries)
  const providers = useAppStore((s) => s.providers)
  const defaultNewChatProviderId = useAppStore((s) => s.defaultNewChatProviderId)
  const refreshMarkdownStore = useAppStore((s) => s.refreshMarkdownStore)
  const setShellActions = useAppStore((s) => s.setShellActions)
  const applyShellActions = useAppStore((s) => s.applyShellActions)
  const [actions, setActions] = useState(savedActions)
  const [applying, setApplying] = useState(false)
  const applyingRef = useRef(false)
  const [error, setError] = useState('')
  const [expandedActions, setExpandedActions] = useState<Record<string, boolean>>({})

  useEffect(() => setActions(savedActions), [savedActions])
  useEffect(() => { void refreshMarkdownStore() }, [refreshMarkdownStore])

  const update = (id: string, patch: Partial<ShellAction>) => {
    setActions((current) => current.map((action) => action.id === id ? { ...action, ...patch } : action))
  }

  const apply = async () => {
    if (applyingRef.current) return
    const invalid = actions.find((action) => !action.label.trim() || (!action.acceptsFiles && !action.acceptsFolders) || (action.enabled && !action.openWorkspace && !action.skillPath))
    if (invalid) {
      setError('Each action needs a label and input type; enabled skill actions also need a skill.')
      return
    }
    setError('')
    applyingRef.current = true
    setApplying(true)
    try {
      const saved = await setShellActions(actions)
      if (saved) await applyShellActions()
    } finally {
      applyingRef.current = false
      setApplying(false)
    }
  }

  return (
    <div className="space-y-4">
      <div className="rounded-xl p-4 grid gap-2" style={{ border: '1px solid var(--border)', background: 'var(--surface-up)' }}>
        <div className="flex items-center gap-2 text-sm font-medium" style={{ color: 'var(--text)' }}>
          <MousePointerClick size={16} aria-hidden style={{ color: 'var(--accent)' }} />
          Finder / Explorer actions
        </div>
        <p className="text-xs" style={{ color: 'var(--text-3)' }}>
          Add per-user context-menu actions for files, folders, or both. Apply replaces only Universal Agent Manager entries.
        </p>
      </div>

      {actions.map((action) => {
        const providerId = action.providerId || defaultNewChatProviderId || providers[0]?.id || DEFAULT_PROVIDER_ID
        const provider = providers.find((candidate) => candidate.id === providerId) ?? providers[0]
        const modelOptions = buildModelOptions(undefined, action.modelId, provider, providerId)
        const expanded = expandedActions[action.id] ?? true
        return (
        <fieldset key={action.id} className="overflow-hidden rounded-xl transition-opacity duration-150" style={{ border: '1px solid var(--border)', opacity: action.enabled ? 1 : 0.58 }}>
          <legend className="sr-only">Shell action {action.label}</legend>
          <div className="flex items-center gap-2 px-3 py-2" style={{ background: 'var(--surface-up)', borderBottom: expanded ? '1px solid var(--border)' : 0 }}>
            <IconButton size="sm" icon={<ChevronRight size={14} className="transition-transform duration-150" style={{ transform: expanded ? 'rotate(90deg)' : 'none' }} />} label={expanded ? `Collapse ${action.label}` : `Expand ${action.label}`} onClick={() => setExpandedActions((current) => ({ ...current, [action.id]: !expanded }))} />
            <span className="min-w-0 flex-1 truncate text-sm font-medium" style={{ color: 'var(--text)' }}>{action.label || 'Untitled action'}</span>
            <Switch hideLabel label={`Enable ${action.label}`} checked={action.enabled} onChange={(event) => update(action.id, { enabled: event.target.checked })} />
            <IconButton icon={<Trash2 size={15} />} label={`Remove ${action.label}`} variant="danger" onClick={() => { if (window.confirm(`Delete shell action “${action.label}”?`)) setActions((current) => current.filter((item) => item.id !== action.id)) }} />
          </div>
          {expanded && <div className="grid gap-3 p-4 animate-fade-in">
          <div className="flex gap-3 items-center">
            <label className="grid gap-1 flex-1 text-xs" style={{ color: 'var(--text-2)' }}>
              Label
              <input
                aria-label={`Label for ${action.label}`}
                value={action.label}
                onChange={(event) => update(action.id, { label: event.target.value })}
                className="rounded-md px-3 py-2 text-sm"
                style={{ background: 'var(--bg)', border: '1px solid var(--border)', color: 'var(--text)' }}
              />
            </label>
          </div>

          <div className="grid sm:grid-cols-2 gap-3">
            <div className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
              <span>Provider</span>
              <MenuSelect
                label={`Provider for ${action.label}`}
                value={action.providerId}
                onChange={(value) => update(action.id, { providerId: value, modelId: '' })}
                options={[
                  { value: '', label: 'New chat default', description: 'Use the provider selected in Chat defaults.' },
                  ...providers.map((candidate) => ({
                    value: candidate.id,
                    label: candidate.shortName || candidate.name,
                    description: providerRuntimeDescription(candidate, candidate.id),
                    icon: <ProviderLogo providerId={candidate.id} />,
                  })),
                ]}
              />
            </div>
            <div className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
              <span>Model</span>
              <MenuSelect
                label={`Model for ${action.label}`}
                value={action.modelId}
                onChange={(value) => update(action.id, { modelId: value })}
                options={[
                  { value: '', label: 'Provider default', description: 'Use the saved default for this provider.' },
                  ...modelOptions.filter((option) => option.id).map((option) => ({
                    value: option.id,
                    label: option.label,
                    description: option.detail,
                  })),
                ]}
              />
            </div>
          </div>

          <div className="grid sm:grid-cols-2 gap-3">
            <div className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
              <span>Action</span>
              <MenuSelect
                value={action.openWorkspace ? 'workspace' : 'skill'}
                label={`Action for ${action.label}`}
                onChange={(value) => update(action.id, { openWorkspace: value === 'workspace' })}
                options={[
                  { value: 'workspace', label: 'Open as Workspace', description: 'Start a chat with the selected items.', icon: <FolderOpen size={15} /> },
                  { value: 'skill', label: 'Run skill', description: 'Apply a skill to the selection.', icon: <Sparkles size={15} /> },
                ]}
              />
            </div>
            <div className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
              <span>Available for</span>
              <MenuSelect
                label={`Input type for ${action.label}`}
                value={action.acceptsFiles && action.acceptsFolders ? 'both' : action.acceptsFolders ? 'folders' : 'files'}
                onChange={(value) => update(action.id, {
                  acceptsFiles: value !== 'folders',
                  acceptsFolders: value !== 'files',
                })}
                options={[
                  { value: 'both', label: 'Files and folders', icon: <Files size={15} /> },
                  { value: 'files', label: 'Files only', icon: <File size={15} /> },
                  { value: 'folders', label: 'Folders only', icon: <Folder size={15} /> },
                ]}
              />
            </div>
          </div>

          {!action.openWorkspace && (
            <div className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
              <span>Skill</span>
              <MenuSelect
                label={`Skill for ${action.label}`}
                value={action.skillPath}
                onChange={(value) => update(action.id, { skillPath: value })}
                options={[
                  { value: '', label: 'Select a skill', icon: <BookOpen size={15} /> },
                  ...markdownEntries.map((entry) => ({ value: entry.filePath, label: entry.title, icon: <BookOpen size={15} /> })),
                ]}
              />
            </div>
          )}
          </div>}
        </fieldset>
        )
      })}

      <div className="flex items-center justify-between gap-3">
        <IconButton icon={<Plus size={16} />} label="Add shell action" variant="solid" onClick={() => setActions((current) => [...current, newAction()])} />
        <Button variant="primary" leadingIcon={<Check size={14} />} loading={applying} onClick={() => void apply()}>Apply</Button>
      </div>
      {(error || notification) && <div role="status" className="text-xs" style={{ color: error ? 'var(--red)' : 'var(--text-2)' }}>{error || notification}</div>}
    </div>
  )
}
