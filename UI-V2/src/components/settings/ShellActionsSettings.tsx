import { forwardRef, useEffect, useImperativeHandle, useRef, useState } from 'react'
import { createPortal } from 'react-dom'
import { BookOpen, Save, X, ChevronRight, File, Files, Folder, FolderOpen, FolderTree, Plus, Sparkles, Trash2 } from 'lucide-react'
import { useAppStore, type ShellAction } from '../../store/useAppStore'
import { DEFAULT_PROVIDER_ID, providerRuntimeDescription } from '../../utils/providerMetadata'
import { buildModelOptions } from '../chat/modelOptions'
import { ProviderLogo } from '../shared/ProviderLogo'
import { Button, IconButton, MenuSelect, Notice, Switch } from '../ui'
import { StatusIndicator } from '../shared/StatusIndicator'

function newAction(): ShellAction {
  const token = globalThis.crypto?.randomUUID?.() ?? `${Date.now()}-${Math.random().toString(16).slice(2)}`
  return {
    id: `action-${token}`,
    label: 'New action',
    skillPath: '',
    providerId: '',
    modelId: '',
    groupPath: [],
    acceptsFiles: true,
    acceptsFolders: true,
    enabled: true,
    openWorkspace: true,
  }
}

function groupPathText(action: ShellAction) {
  return action.groupPath.join(' / ')
}

function parseGroupPath(value: string) {
  return value.split('/').map((segment) => segment.trim()).filter(Boolean)
}

export interface ShellActionsHandle { requestLeave(next: () => void): void }

export const ShellActionsSettings = forwardRef<ShellActionsHandle>(function ShellActionsSettings(_props, ref) {
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
  const [appliedActions, setAppliedActions] = useState(savedActions)
  const [pendingExit, setPendingExit] = useState<(() => void) | null>(null)
  const [newGroupActionId, setNewGroupActionId] = useState('')
  const [newGroupName, setNewGroupName] = useState('')
  const [toolbarTarget, setToolbarTarget] = useState<HTMLElement | null>(null)
  const dirty = JSON.stringify(actions) !== JSON.stringify(appliedActions)
  const dirtyRef = useRef(dirty)
  dirtyRef.current = dirty
  useImperativeHandle(ref, () => ({ requestLeave(next) {
    if (applyingRef.current) return
    if (dirty) setPendingExit(() => next)
    else next()
  } }), [dirty])
  useEffect(() => { setToolbarTarget(document.getElementById('settings-page-actions')) }, [])
  useEffect(() => {
    if (!dirty) return
    const guard = (event: BeforeUnloadEvent) => { event.preventDefault(); event.returnValue = '' }
    window.addEventListener('beforeunload', guard)
    return () => window.removeEventListener('beforeunload', guard)
  }, [dirty])
  const [pendingDeleteActionId, setPendingDeleteActionId] = useState('')

  useEffect(() => {
    if (!dirtyRef.current && !applyingRef.current) { setActions(savedActions); setAppliedActions(savedActions) }
  }, [savedActions])
  useEffect(() => { void refreshMarkdownStore() }, [refreshMarkdownStore])

  const update = (id: string, patch: Partial<ShellAction>) => {
    setActions((current) => current.map((action) => action.id === id ? { ...action, ...patch } : action))
  }

  const apply = async () => {
    if (applyingRef.current) return false
    const prepared = actions.map(action => ({ ...action, label: action.label.trim() }))
    const invalid = prepared.find((action) => !action.label.trim() || (!action.acceptsFiles && !action.acceptsFolders) || (action.enabled && !action.openWorkspace && !action.skillPath))
    if (invalid) {
      setError('Each action needs a label and input type; enabled skill actions also need a skill.')
      return false
    }
    setError('')
    applyingRef.current = true
    setApplying(true)
    try {
      const saved = await setShellActions(prepared)
      if (!saved) { setError(useAppStore.getState().shellActionNotification || 'Could not save shell actions.'); return false }
      const applied = await applyShellActions()
      if (!applied) { setError(useAppStore.getState().shellActionNotification || 'Could not apply shell actions. Save again to retry.'); return false }
      setActions(prepared)
      setAppliedActions(prepared)
      return true
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : 'Could not save shell actions. Try again.')
      return false
    } finally {
      applyingRef.current = false
      setApplying(false)
    }
  }

  const groupOptions = Array.from(new Set([
    ...actions.map(groupPathText),
    ...markdownEntries.map(entry => entry.group || ''),
  ].flatMap(path => { const parts = parseGroupPath(path); return parts.map((_, index) => parts.slice(0, index + 1).join(' / ')) }))).filter(Boolean).sort()
  const toolbar = <div className="flex items-center gap-2">
    {dirty && <StatusIndicator issues={['Unsaved shell action changes']} />}
    <IconButton icon={<Plus size={16} />} label="Add shell action" disabled={applying} onClick={() => setActions(current => [newAction(), ...current])} />
    <IconButton icon={<Save size={16} />} label="Save shell actions" variant="solid" disabled={applying || !dirty} onClick={() => void apply()} />
  </div>

  return (
    <div className="space-y-4">
      {toolbarTarget ? createPortal(toolbar, toolbarTarget) : toolbar}
      {pendingExit && <div className="fixed inset-0 z-[80] flex items-center justify-center bg-black/50" onClick={event => { if (event.target === event.currentTarget && !applying) setPendingExit(null) }}>
        <div role="alertdialog" aria-modal="true" aria-label="Unsaved shell actions" tabIndex={-1} className="rounded-xl p-5 space-y-4 max-w-sm" style={{background:'var(--surface)',border:'1px solid var(--border-bright)'}} onKeyDown={event => { if(event.key === 'Escape') { event.preventDefault(); event.stopPropagation(); if(!applying) setPendingExit(null) } }}>
          <h3 className="text-sm font-semibold">Save shell actions before leaving?</h3>
          {error && <p role="alert" className="text-xs" style={{color:'var(--red)'}}>{error}</p>}
          <div className="flex justify-end gap-2"><Button autoFocus disabled={applying} onClick={() => setPendingExit(null)}>Go back</Button><Button variant="primary" disabled={applying} onClick={async () => { if(await apply()) { const next=pendingExit; setPendingExit(null); next() } }}>Save and leave</Button></div>
        </div>
      </div>}
      {newGroupActionId && <div className="fixed inset-0 z-[80] flex items-center justify-center bg-black/50">
        <form role="dialog" aria-modal="true" aria-label="New shell action group" className="rounded-xl p-5 space-y-4 w-80" style={{background:'var(--surface)',border:'1px solid var(--border-bright)'}} onKeyDown={event => { if(event.key==='Escape') { event.preventDefault(); event.stopPropagation(); setNewGroupActionId('') } }} onSubmit={event => { event.preventDefault(); if(!parseGroupPath(newGroupName).length) return; update(newGroupActionId,{groupPath:parseGroupPath(newGroupName)}); setNewGroupActionId('') }}>
          <div className="flex items-center justify-between"><h3 className="text-sm font-semibold">New group</h3><IconButton icon={<X size={16}/>} label="Close new group" onClick={() => setNewGroupActionId('')} /></div>
          <input autoFocus aria-label="Group name" placeholder="Group name" value={newGroupName} onChange={event => setNewGroupName(event.target.value)} className="w-full rounded-md px-3 py-2 text-sm" style={{background:'var(--bg)',color:'var(--text)',border:'1px solid var(--border)'}} />
          <Button type="submit" variant="primary" block disabled={!parseGroupPath(newGroupName).length}>Create group</Button>
        </form>
      </div>}
      {actions.map((action) => {
        const providerId = action.providerId || defaultNewChatProviderId || providers[0]?.id || DEFAULT_PROVIDER_ID
        const provider = providers.find((candidate) => candidate.id === providerId) ?? providers[0]
        const modelOptions = buildModelOptions(undefined, action.modelId, provider, providerId)
        const expanded = expandedActions[action.id] ?? true
        return (
        <fieldset disabled={applying} key={action.id} className="overflow-hidden rounded-xl transition-opacity duration-150" style={{ border: '1px solid var(--border)', opacity: action.enabled ? 1 : 0.58 }}>
          <legend className="sr-only">Shell action {action.label}</legend>
          <div className="flex items-center gap-2 px-3 py-2" style={{ background: 'var(--surface-up)', borderBottom: expanded ? '1px solid var(--border)' : 0 }}>
            <IconButton size="sm" variant="solid" icon={<ChevronRight size={14} className="transition-transform duration-150" style={{ transform: expanded ? 'rotate(90deg)' : 'none' }} />} label={expanded ? `Collapse ${action.label}` : `Expand ${action.label}`} onClick={() => setExpandedActions((current) => ({ ...current, [action.id]: !expanded }))} />
            <span className="min-w-0 flex-1 truncate text-sm font-medium" style={{ color: 'var(--text)' }}>{action.label || 'Untitled action'}</span>
            {JSON.stringify(action) !== JSON.stringify(appliedActions.find(item => item.id === action.id)) && <StatusIndicator issues={['This action has unsaved changes']} />}
            <Switch hideLabel label={`Enable ${action.label}`} checked={action.enabled} onChange={(event) => update(action.id, { enabled: event.target.checked })} />
            <IconButton icon={<Trash2 size={15} />} label={`Remove ${action.label}`} variant="solid" className="hover:!text-[var(--red)]" onClick={() => setPendingDeleteActionId(action.id)} />
          </div>
          {pendingDeleteActionId === action.id && (
            <div className="p-3">
              <Notice
                tone="warning"
                title="Delete shell action?"
                dismissLabel={`Dismiss delete ${action.label} warning`}
                onDismiss={() => setPendingDeleteActionId('')}
                actions={(
                  <>
                    <Button size="sm" onClick={() => setPendingDeleteActionId('')}>Cancel</Button>
                    <Button size="sm" variant="danger" onClick={() => {
                      setActions((current) => current.filter((item) => item.id !== action.id))
                      setPendingDeleteActionId('')
                    }}>Delete action</Button>
                  </>
                )}
              >
                Delete “{action.label}”? This change takes effect when you save.
              </Notice>
            </div>
          )}
          {expanded && <div className="grid gap-3 p-4 sm:grid-cols-2">
          <div className="flex gap-3 items-center">
            <label className="grid gap-1 flex-1 text-xs" style={{ color: 'var(--text-2)' }}>
              <input
                aria-label={`Label for ${action.label}`}
                placeholder="Action name"
                value={action.label}
                onChange={(event) => update(action.id, { label: event.target.value })}
                className="rounded-md px-3 py-2 text-sm"
                style={{ background: 'var(--bg)', border: '1px solid var(--border)', color: 'var(--text)' }}
              />
            </label>
          </div>

          <MenuSelect label={`Group for ${action.label}`} value={groupPathText(action)} onChange={value => {
            if(value==='__new__') { setNewGroupActionId(action.id); setNewGroupName('') }
            else update(action.id,{groupPath:parseGroupPath(value)})
          }} options={[{value:'',label:'No group',icon:<FolderTree size={15}/>},...groupOptions.map(group => ({value:group,label:group,icon:<FolderTree size={15}/> })),{value:'__new__',label:'New group…',icon:<Plus size={15}/> }]} />

          <div className="grid sm:grid-cols-2 gap-3 sm:col-span-2">
            <div className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
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

          <div className="grid sm:grid-cols-2 gap-3 sm:col-span-2">
            <div className="grid gap-1 text-xs" style={{ color: 'var(--text-2)' }}>
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

      {(error || notification) && <div role="status" className="flex items-center justify-between gap-2 text-xs" style={{ color: error ? 'var(--red)' : 'var(--text-2)' }}><span>{error || notification}</span><IconButton icon={<X size={14}/>} label="Dismiss shell action message" onClick={() => {setError(''); void useAppStore.getState().dismissShellActionNotification()}} /></div>}
    </div>
  )
})
