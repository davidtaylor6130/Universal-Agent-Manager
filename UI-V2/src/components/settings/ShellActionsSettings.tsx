import { useEffect, useState } from 'react'
import { Plus, Trash2 } from 'lucide-react'
import { useAppStore, type ShellAction } from '../../store/useAppStore'
import { Button, IconButton } from '../ui'

function newAction(): ShellAction {
  const token = globalThis.crypto?.randomUUID?.() ?? `${Date.now()}-${Math.random().toString(16).slice(2)}`
  return {
    id: `action-${token}`,
    label: 'New action',
    skillPath: '',
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
  const refreshMarkdownStore = useAppStore((s) => s.refreshMarkdownStore)
  const setShellActions = useAppStore((s) => s.setShellActions)
  const applyShellActions = useAppStore((s) => s.applyShellActions)
  const [actions, setActions] = useState(savedActions)
  const [applying, setApplying] = useState(false)
  const [error, setError] = useState('')

  useEffect(() => setActions(savedActions), [savedActions])
  useEffect(() => { void refreshMarkdownStore() }, [refreshMarkdownStore])

  const update = (id: string, patch: Partial<ShellAction>) => {
    setActions((current) => current.map((action) => action.id === id ? { ...action, ...patch } : action))
  }

  const apply = async () => {
    const invalid = actions.find((action) => !action.label.trim() || (!action.acceptsFiles && !action.acceptsFolders) || (action.enabled && !action.openWorkspace && !action.skillPath))
    if (invalid) {
      setError('Each action needs a label and input type; enabled skill actions also need a skill.')
      return
    }
    setError('')
    setApplying(true)
    const saved = await setShellActions(actions)
    if (saved) await applyShellActions()
    setApplying(false)
  }

  return (
    <div className="space-y-4">
      <div className="rounded-xl p-4 grid gap-2" style={{ border: '1px solid var(--border)', background: 'var(--surface-up)' }}>
        <div className="text-sm font-medium" style={{ color: 'var(--text)' }}>Finder / Explorer actions</div>
        <p className="text-xs" style={{ color: 'var(--text-3)' }}>
          Add per-user context-menu actions for files, folders, or both. Apply replaces only Universal Agent Manager entries.
        </p>
      </div>

      {actions.map((action) => (
        <fieldset key={action.id} className="rounded-xl p-4 grid gap-3" style={{ border: '1px solid var(--border)' }}>
          <legend className="sr-only">Shell action {action.label}</legend>
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
            <label className="flex items-center gap-2 text-xs mt-5" style={{ color: 'var(--text-2)' }}>
              <input type="checkbox" checked={action.enabled} onChange={(event) => update(action.id, { enabled: event.target.checked })} />
              Enabled
            </label>
            <IconButton icon={<Trash2 size={15} />} label={`Remove ${action.label}`} onClick={() => setActions((current) => current.filter((item) => item.id !== action.id))} />
          </div>

          <div className="grid sm:grid-cols-2 gap-3">
            <label className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
              Action
              <select
                aria-label={`Action for ${action.label}`}
                value={action.openWorkspace ? 'workspace' : 'skill'}
                onChange={(event) => update(action.id, { openWorkspace: event.target.value === 'workspace' })}
                className="rounded-md px-3 py-2 text-sm"
                style={{ background: 'var(--bg)', border: '1px solid var(--border)', color: 'var(--text)' }}
              >
                <option value="workspace">Open as Workspace</option>
                <option value="skill">Run skill</option>
              </select>
            </label>
            <label className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
              Available for
              <select
                aria-label={`Input type for ${action.label}`}
                value={action.acceptsFiles && action.acceptsFolders ? 'both' : action.acceptsFolders ? 'folders' : 'files'}
                onChange={(event) => update(action.id, {
                  acceptsFiles: event.target.value !== 'folders',
                  acceptsFolders: event.target.value !== 'files',
                })}
                className="rounded-md px-3 py-2 text-sm"
                style={{ background: 'var(--bg)', border: '1px solid var(--border)', color: 'var(--text)' }}
              >
                <option value="both">Files and folders</option>
                <option value="files">Files only</option>
                <option value="folders">Folders only</option>
              </select>
            </label>
          </div>

          {!action.openWorkspace && (
            <label className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
              Skill
              <select
                aria-label={`Skill for ${action.label}`}
                value={action.skillPath}
                onChange={(event) => update(action.id, { skillPath: event.target.value })}
                className="rounded-md px-3 py-2 text-sm"
                style={{ background: 'var(--bg)', border: '1px solid var(--border)', color: 'var(--text)' }}
              >
                <option value="">Select a Markdown Store skill</option>
                {markdownEntries.map((entry) => <option key={entry.filePath} value={entry.filePath}>{entry.title}</option>)}
              </select>
            </label>
          )}
        </fieldset>
      ))}

      <div className="flex items-center justify-between gap-3">
        <Button variant="secondary" onClick={() => setActions((current) => [...current, newAction()])}>
          <Plus size={14} /> Add action
        </Button>
        <Button onClick={() => void apply()} disabled={applying}>{applying ? 'Applying…' : 'Apply'}</Button>
      </div>
      {(error || notification) && <div role="status" className="text-xs" style={{ color: error ? 'var(--red)' : 'var(--text-2)' }}>{error || notification}</div>}
    </div>
  )
}
