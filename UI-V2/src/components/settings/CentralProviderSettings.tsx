import { forwardRef, useCallback, useEffect, useImperativeHandle, useRef, useState, type ReactNode } from 'react'
import { ChevronRight, Plus, X } from 'lucide-react'
import { useAppStore } from '../../store/useAppStore'
import { sendToCEF } from '../../ipc/cefBridge'
import { Button, ConfirmDialog, IconButton, MenuSelect } from '../ui'
import { UnsavedChangesDialog } from './UnsavedChangesDialog'

type Agent = { id: string; description: string; mode: string; workspaceAccess: string; skills: string[]; delegates: string[]; instructions: string; builtIn: boolean }
const blankAgent = (): Agent => ({ id: '', description: '', mode: 'primary', workspaceAccess: 'write', skills: [], delegates: [], instructions: '', builtIn: false })
const field = 'uam-field w-full'
type Option = { value: string; label: string; group?: string }

/** Chosen items as removable chips plus an organised "+ Add" picker. */
function ChipPicker({ label, values, options, onChange, disabled }: { label: string; values: string[]; options: Option[]; onChange: (values: string[]) => void; disabled?: boolean }) {
  const remaining = options.filter(option => !values.includes(option.value))
  const groups = [...new Set(remaining.map(option => option.group || 'Other'))].sort()
  const labelFor = (value: string) => options.find(option => option.value === value)?.label ?? `${value} (missing)`
  return <div className="flex flex-wrap items-center gap-2">
    {values.map(value => <span key={value} className="flex items-center gap-0.5 rounded-md py-0.5 pl-2 pr-0.5 text-xs" style={{ border: '1px solid var(--border)', background: 'var(--surface)' }}>
      {labelFor(value)}
      <IconButton size="sm" icon={<X size={11} />} label={`Remove ${labelFor(value)}`} disabled={disabled} onClick={() => onChange(values.filter(item => item !== value))} />
    </span>)}
    <div style={{ minWidth: 170 }}>
      <MenuSelect label={`Add ${label}`} value="" placeholder={remaining.length ? `Add ${label}…` : `No more ${label}s`} disabled={disabled || remaining.length === 0} onChange={value => onChange([...values, value])}
        options={groups.flatMap(group => remaining.filter(option => (option.group || 'Other') === group).map(option => ({ value: option.value, label: option.label, group: groups.length > 1 ? group : undefined })))} />
    </div>
  </div>
}

function Group({ title, action, children }: { title: string; action?: ReactNode; children: ReactNode }) {
  return <section aria-label={title} className="uam-settings-group">
    <div className="flex items-center justify-between gap-3 mb-2"><h3 className="uam-settings-group__title" style={{ margin: '0 0 0 2px' }}>{title}</h3>{action}</div>
    <div className="uam-settings-group__body">{children}</div>
  </section>
}
const hint = (text: string) => <p className="text-xs" style={{ color: 'var(--text-3)' }}>{text}</p>
const label = (text: string) => <span className="text-sm" style={{ color: 'var(--text)' }}>{text}</span>

const AGENT_NAME = /^[a-z0-9][a-z0-9_-]*$/
const same = (left: unknown, right: unknown) => JSON.stringify(left) === JSON.stringify(right)

export interface CentralProviderSettingsHandle { requestLeave(next: () => void): void }

/** Shared instructions, default skills and default agent (part="defaults"), or the agent store (part="agents"). Installed natively at every provider launch, local and SSH. */
export const CentralProviderSettings = forwardRef<CentralProviderSettingsHandle, { part?: 'defaults' | 'agents' }>(function CentralProviderSettings({ part = 'defaults' }, ref) {
  const saved = useAppStore(state => state.centralProviderConfiguration)
  const skills = useAppStore(state => state.markdownStoreEntries)
  const [instructionFiles, setInstructionFiles] = useState(saved.instructionFiles)
  const [defaultAgents, setDefaultAgents] = useState(saved.defaultAgents ?? [])
  const [defaultSkills, setDefaultSkills] = useState(saved.defaultSkills ?? [])
  const [defaultAgentId, setDefaultAgentId] = useState(saved.defaultAgentId)
  const [agents, setAgents] = useState<Agent[]>([])
  const [agentErrors, setAgentErrors] = useState<string[]>([])
  const [editing, setEditing] = useState<{ draft: Agent; isNew: boolean } | null>(null)
  const [busy, setBusy] = useState(false)
  const [message, setMessage] = useState<{ tone: 'ok' | 'error'; text: string } | null>(null)
  const [pendingDelete, setPendingDelete] = useState<string | null>(null)
  const [pendingExit, setPendingExit] = useState<(() => void) | null>(null)
  const editingOriginal = useRef<Agent | null>(null)

  const loadAgents = useCallback(async () => {
    const response = await sendToCEF<{ agents: Agent[]; errors: string[] }>({ action: 'listAgentDefinitions', payload: {} })
    if (response.ok && response.data) { setAgents(response.data.agents ?? []); setAgentErrors(response.data.errors ?? []) }
  }, [])
  useEffect(() => { void loadAgents(); void useAppStore.getState().refreshMarkdownStore() }, [loadAgents])

  // Success notes fade after a few seconds; errors stay until the next action.
  useEffect(() => {
    if (message?.tone !== 'ok') return
    const timer = window.setTimeout(() => setMessage(null), 4000)
    return () => window.clearTimeout(timer)
  }, [message])

  const run = async (action: () => Promise<{ ok: boolean; error?: string }>, success: string) => {
    setBusy(true); setMessage(null)
    try { const result = await action(); setMessage(result.ok ? { tone: 'ok', text: success } : { tone: 'error', text: result.error || 'Could not save.' }); return result.ok }
    catch { setMessage({ tone: 'error', text: 'Could not save. Try again.' }); return false }
    finally { setBusy(false) }
  }
  const startEditing = (next: { draft: Agent; isNew: boolean } | null) => { editingOriginal.current = next ? { ...next.draft } : null; setEditing(next); setMessage(null) }
  const draftId = editing ? editing.draft.id.trim().toLowerCase() : ''
  const nameError = !editing?.isNew ? '' : !draftId ? 'Enter a name.' : !AGENT_NAME.test(draftId) ? 'Use lowercase letters, numbers, - or _.' : agents.some(agent => agent.id === draftId) ? `An agent named ${draftId} already exists.` : ''
  const agentDirty = Boolean(editing && !same(editing.draft, editingOriginal.current))
  const saveAgent = async () => {
    if (!editing || nameError) return false
    const draft = { ...editing.draft, id: draftId, description: editing.draft.description.trim() }
    if (!await run(() => sendToCEF({ action: 'saveAgentDefinition', payload: draft }), `Agent ${draft.id} saved.`)) return false
    startEditing(null); setMessage({ tone: 'ok', text: `Agent ${draft.id} saved.` }); await loadAgents()
    return true
  }
  const deleteAgent = async (id: string) => {
    if (await run(() => sendToCEF({ action: 'deleteAgentDefinition', payload: { id } }), `Agent ${id} deleted.`)) {
      if (editing?.draft.id === id) startEditing(null)
      setPendingDelete(null)
      await loadAgents()
    }
  }
  const defaultsDirty = !same(instructionFiles, saved.instructionFiles) || !same(defaultSkills, saved.defaultSkills ?? []) || !same(defaultAgents, saved.defaultAgents ?? []) || defaultAgentId !== saved.defaultAgentId
  const saveDefaults = () => run(() => {
    const store = useAppStore.getState()
    return store.setCentralProviderConfiguration({ ...store.centralProviderConfiguration, enabled: true, instructionFiles, defaultSkills, defaultAgents, defaultAgentId })
  }, 'Saved. New and restarted chats get this setup.')
  const undoDefaults = () => { setInstructionFiles(saved.instructionFiles); setDefaultSkills(saved.defaultSkills ?? []); setDefaultAgents(saved.defaultAgents ?? []); setDefaultAgentId(saved.defaultAgentId); setMessage(null) }
  const dirty = part === 'defaults' ? defaultsDirty : agentDirty
  useImperativeHandle(ref, () => ({ requestLeave(next) {
    if (busy) return
    if (dirty) setPendingExit(() => next)
    else next()
  } }))
  useEffect(() => {
    if (!dirty) return
    const guard = (event: BeforeUnloadEvent) => { event.preventDefault(); event.returnValue = '' }
    window.addEventListener('beforeunload', guard)
    return () => window.removeEventListener('beforeunload', guard)
  }, [dirty])
  const primaryAgents = agents.filter(agent => agent.mode !== 'subagent')
  const skillName = (entry: typeof skills[number]) => entry.commandName || entry.title
  const skillOptions: Option[] = skills.map(entry => ({ value: skillName(entry), label: entry.title, group: entry.group || 'Ungrouped' }))
  const agentOptions: Option[] = agents.filter(agent => !agent.builtIn).map(agent => ({ value: agent.id, label: agent.id, group: agent.mode === 'subagent' ? 'Helpers' : 'Chat agents' }))
  const addInstructionFile = async () => {
    const path = await useAppStore.getState().browseProviderAgentImport(instructionFiles[instructionFiles.length - 1] ?? '')
    if (path && !instructionFiles.includes(path)) setInstructionFiles([...instructionFiles, path])
  }

  const status = <p role="status" aria-live="polite" className="uam-reveal text-xs" key={message?.text} style={{ color: message?.tone === 'error' ? 'var(--red)' : 'var(--text-2)' }}>{message?.text}</p>
  const leaveDialog = pendingExit && <UnsavedChangesDialog
    title={part === 'defaults' ? 'Save central setup changes?' : 'Save agent changes?'}
    busy={busy}
    error={message?.tone === 'error' ? message.text : undefined}
    onStay={() => setPendingExit(null)}
    onDiscard={() => { if (part === 'defaults') undoDefaults(); else startEditing(null); const next = pendingExit; setPendingExit(null); next() }}
    onSave={(part === 'defaults' || !nameError) ? async () => { if (await (part === 'defaults' ? saveDefaults() : saveAgent())) { const next = pendingExit; setPendingExit(null); next() } } : undefined}
  />

  if (part === 'defaults') return <div style={{ color: 'var(--text)' }}>
    <Group title="Shared instructions">
      {hint('AGENTS.md files on this computer. Their contents go to every agent on every provider, not into your messages.')}
      {instructionFiles.length === 0 && <p className="text-xs" style={{ color: 'var(--text-2)' }}>No instruction files chosen.</p>}
      {instructionFiles.map(path => <div key={path} className="flex items-center gap-2 rounded-md py-1 pl-2.5 pr-1 text-xs" style={{ border: '1px solid var(--border)', background: 'var(--bg)' }}>
        <span className="flex-1 min-w-0 break-all font-mono">{path}</span>
        <IconButton size="sm" icon={<X size={12} />} label={`Remove ${path}`} disabled={busy} onClick={() => setInstructionFiles(instructionFiles.filter(item => item !== path))} />
      </div>)}
      <Button size="sm" className="justify-self-start" leadingIcon={<Plus size={13} aria-hidden />} disabled={busy} onClick={() => void addInstructionFile()}>Choose file…</Button>
      {saved.instructions && hint('Older typed instructions are still applied.')}
    </Group>

    <Group title="Defaults for every chat">
      <div className="grid gap-1.5">
        {label('Default agent for new chats')}
        <MenuSelect label="Default agent for new chats" value={defaultAgentId} disabled={busy} onChange={setDefaultAgentId} options={[
          ...primaryAgents.map(agent => ({ value: agent.id, label: agent.id, description: agent.description })),
          ...(primaryAgents.some(agent => agent.id === defaultAgentId) ? [] : [{ value: defaultAgentId, label: `${defaultAgentId} (not found)` }]),
        ]} />
      </div>
      <div className="grid gap-1.5">
        {label('Default skills')}
        {hint('Skills every agent always has, from the Skills store. Agents also get skills listed on the agent itself.')}
        {skills.length === 0 ? <p className="text-xs" style={{ color: 'var(--text-2)' }}>The Skills store is empty. Add skills there first.</p> : <ChipPicker label="skill" values={defaultSkills} options={skillOptions} onChange={setDefaultSkills} disabled={busy} />}
      </div>
      <div className="grid gap-1.5">
        {label('Default agents')}
        {hint('Agents from the agent store (Settings › Agents) every provider gets. Build, Plan and the chat\'s own agent are always included.')}
        <ChipPicker label="agent" values={defaultAgents} options={agentOptions} onChange={setDefaultAgents} disabled={busy} />
      </div>
    </Group>

    <div className="flex items-center gap-2">
      <Button size="sm" variant="primary" loading={busy} disabled={!defaultsDirty} onClick={() => void saveDefaults()}>Save</Button>
      {defaultsDirty && <Button size="sm" variant="ghost" disabled={busy} onClick={undoDefaults}>Undo changes</Button>}
      {!message && defaultsDirty ? <span className="text-xs" style={{ color: 'var(--text-3)' }}>Unsaved changes</span> : status}
    </div>
    {leaveDialog}
  </div>

  return <div style={{ color: 'var(--text)' }}>
    <Group title="Agent store" action={<Button size="sm" leadingIcon={<Plus size={13} aria-hidden />} disabled={busy || Boolean(editing)} onClick={() => startEditing({ draft: blankAgent(), isNew: true })}>Add agent</Button>}>
      {hint('Saved to the UAM agent store and installed natively for every provider. Click an agent to edit it.')}
      {agentErrors.map(error => <p key={error} role="alert" className="text-xs" style={{ color: 'var(--red)' }}>{error}</p>)}
      {agents.length === 0 && !editing && <p className="text-xs" style={{ color: 'var(--text-2)' }}>No agents yet.</p>}
      {[...agents, ...(editing?.isNew ? [editing.draft] : [])].map((agent, index) => {
        const isOpen = editing ? (editing.isNew ? index === agents.length : editing.draft.id === agent.id) : false
        const draft = isOpen && editing ? editing.draft : agent
        const set = (change: Partial<Agent>) => editing && setEditing({ ...editing, draft: { ...editing.draft, ...change } })
        return <div key={isOpen && editing?.isNew ? 'new' : agent.id} className="rounded-lg" style={{ border: '1px solid var(--border)' }}>
          <div className="flex items-center gap-3 px-3 py-2 text-sm">
            <button type="button" className="flex flex-1 min-w-0 items-center gap-2 text-left disabled:opacity-50" aria-expanded={isOpen} disabled={busy || (Boolean(editing) && !isOpen)} title={editing && !isOpen ? 'Finish editing the open agent first' : undefined} onClick={() => isOpen && agentDirty ? setPendingExit(() => () => undefined) : startEditing(isOpen ? null : { draft: { ...agent }, isNew: false })}>
              <ChevronRight size={14} aria-hidden className="shrink-0" style={{ color: 'var(--text-3)', transform: isOpen ? 'rotate(90deg)' : undefined, transition: 'transform var(--dur-base) var(--ease-out-soft)' }} />
              <strong className="shrink-0">{draft.id || 'New agent'}</strong> <span className="truncate text-xs" style={{ color: 'var(--text-3)' }}>{agent.builtIn ? 'Built in · read only' : agent.description}</span>
            </button>
            {!agent.builtIn && !(isOpen && editing?.isNew) && <Button size="sm" variant="ghost" disabled={busy} aria-label={`Delete agent ${agent.id}`} onClick={() => setPendingDelete(agent.id)}>Delete</Button>}
          </div>
          {isOpen && <div className="grid gap-3 px-3 pb-3 text-sm">
            <label className="grid gap-1"><span>Name</span><input className={field} autoFocus={editing?.isNew} value={draft.id} placeholder="reviewer" aria-invalid={Boolean(isOpen && nameError && draft.id)} disabled={busy || agent.builtIn || !editing?.isNew} onChange={event => set({ id: event.currentTarget.value })} style={isOpen && nameError && draft.id ? { borderColor: 'var(--red)' } : undefined} /><span className="text-xs" style={{ color: isOpen && nameError && draft.id ? 'var(--red)' : 'var(--text-3)' }}>{isOpen && nameError && draft.id ? nameError : 'Lowercase letters, numbers, - or _. Cannot be changed after saving.'}</span></label>
            <label className="grid gap-1"><span>Description</span><input className={field} value={draft.description} placeholder="What this agent does" disabled={busy || agent.builtIn} onChange={event => set({ description: event.currentTarget.value })} /></label>
            <div className="grid grid-cols-2 gap-3">
              <div className="grid gap-1"><span>Used as</span><MenuSelect label="Used as" value={draft.mode} disabled={busy || agent.builtIn} onChange={mode => set({ mode })} options={[{ value: 'primary', label: 'Chat agent' }, { value: 'subagent', label: 'Helper (delegated to)' }, { value: 'both', label: 'Both' }]} /></div>
              <div className="grid gap-1"><span>Workspace access</span><MenuSelect label="Workspace access" value={draft.workspaceAccess} disabled={busy || agent.builtIn} onChange={workspaceAccess => set({ workspaceAccess })} options={[{ value: 'write', label: 'Read and write' }, { value: 'read', label: 'Read only' }]} /></div>
            </div>
            <div className="grid gap-1"><span>Skills for this agent</span><ChipPicker label="skill" values={draft.skills} options={skillOptions} onChange={skills => set({ skills })} disabled={busy || agent.builtIn} /></div>
            <label className="grid gap-1"><span>Instructions</span><textarea className={`${field} uam-field--area font-mono text-xs`} rows={10} placeholder="How the agent should work…" value={draft.instructions} disabled={busy || agent.builtIn} onChange={event => set({ instructions: event.currentTarget.value })} /></label>
            {!agent.builtIn && <div className="flex items-center gap-2"><Button size="sm" variant="primary" loading={busy} disabled={Boolean(nameError) || (!editing?.isNew && !agentDirty)} onClick={() => void saveAgent()}>{editing?.isNew ? 'Create agent' : 'Save agent'}</Button><Button size="sm" variant="ghost" disabled={busy} onClick={() => startEditing(null)}>Cancel</Button></div>}
          </div>}
        </div>
      })}
      {status}
    </Group>
    <ConfirmDialog open={pendingDelete !== null} title={`Delete agent ${pendingDelete ?? ''}?`} confirmLabel="Delete agent" busy={busy} error={message?.tone === 'error' ? message.text : undefined} onCancel={() => setPendingDelete(null)} onConfirm={() => { if (pendingDelete) void deleteAgent(pendingDelete) }}>
      It is removed from the agent store. This cannot be undone.
    </ConfirmDialog>
    {leaveDialog}
  </div>
})
