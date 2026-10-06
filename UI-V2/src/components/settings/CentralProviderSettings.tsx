import { useCallback, useEffect, useState } from 'react'
import { useAppStore } from '../../store/useAppStore'
import { sendToCEF } from '../../ipc/cefBridge'
import { Button } from '../ui'

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
    {values.map(value => <span key={value} className="flex items-center gap-1 rounded-md px-2 py-1 text-xs" style={{ border: '1px solid var(--border)', background: 'var(--surface)' }}>
      {labelFor(value)}
      <button type="button" aria-label={`Remove ${labelFor(value)}`} disabled={disabled} onClick={() => onChange(values.filter(item => item !== value))} style={{ color: 'var(--text-3)' }}>×</button>
    </span>)}
    <select aria-label={`Add ${label}`} className="uam-field text-xs" style={{ width: 'auto' }} value="" disabled={disabled || remaining.length === 0} onChange={event => { const value = event.currentTarget.value; if (value) onChange([...values, value]) }}>
      <option value="">{remaining.length ? `+ Add ${label}` : `No more ${label}s`}</option>
      {groups.map(group => <optgroup key={group} label={group}>{remaining.filter(option => (option.group || 'Other') === group).map(option => <option key={option.value} value={option.value}>{option.label}</option>)}</optgroup>)}
    </select>
  </div>
}

/** Shared instructions, agents, default skills and default agent. Installed natively at every provider launch, local and SSH. */
export function CentralProviderSettings() {
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
  const [message, setMessage] = useState('')

  const loadAgents = useCallback(async () => {
    const response = await sendToCEF<{ agents: Agent[]; errors: string[] }>({ action: 'listAgentDefinitions', payload: {} })
    if (response.ok && response.data) { setAgents(response.data.agents); setAgentErrors(response.data.errors) }
  }, [])
  useEffect(() => { void loadAgents(); void useAppStore.getState().refreshMarkdownStore() }, [loadAgents])

  const run = async (action: () => Promise<{ ok: boolean; error?: string }>, success: string) => {
    setBusy(true); setMessage('')
    try { const result = await action(); setMessage(result.ok ? success : result.error || 'Could not save.'); return result.ok }
    catch { setMessage('Could not save. Try again.'); return false }
    finally { setBusy(false) }
  }
  const saveAgent = async () => {
    if (!editing) return
    const draft = { ...editing.draft, id: editing.draft.id.trim().toLowerCase(), description: editing.draft.description.trim() }
    if (editing.isNew && agents.some(agent => agent.id === draft.id)) { setMessage(`An agent named ${draft.id} already exists.`); return }
    if (await run(() => sendToCEF({ action: 'saveAgentDefinition', payload: draft }), `Agent ${draft.id} saved.`)) { setEditing(null); await loadAgents() }
  }
  const deleteAgent = async (id: string) => {
    if (await run(() => sendToCEF({ action: 'deleteAgentDefinition', payload: { id } }), `Agent ${id} deleted.`)) {
      if (editing?.draft.id === id) setEditing(null)
      await loadAgents()
    }
  }
  const saveDefaults = () => run(() => {
    const store = useAppStore.getState()
    return store.setCentralProviderConfiguration({ ...store.centralProviderConfiguration, enabled: true, instructionFiles, defaultSkills, defaultAgents, defaultAgentId })
  }, 'Saved. New chats and restarted chats get this setup on every provider, locally and over SSH.')
  const primaryAgents = agents.filter(agent => agent.mode !== 'subagent')
  const skillName = (entry: typeof skills[number]) => entry.commandName || entry.title
  const skillOptions: Option[] = skills.map(entry => ({ value: skillName(entry), label: entry.title, group: entry.group || 'Ungrouped' }))
  const agentOptions: Option[] = agents.filter(agent => !agent.builtIn).map(agent => ({ value: agent.id, label: agent.id, group: agent.mode === 'subagent' ? 'Helpers' : 'Chat agents' }))
  const addInstructionFile = async () => {
    const path = await useAppStore.getState().browseProviderAgentImport(instructionFiles[instructionFiles.length - 1] ?? '')
    if (path && !instructionFiles.includes(path)) setInstructionFiles([...instructionFiles, path])
  }

  return <div className="space-y-6" style={{ color: 'var(--text)' }}>
    <section className="grid gap-2">
      <strong>Shared instructions (AGENTS.md)</strong>
      <p className="text-xs" style={{ color: 'var(--text-3)' }}>Pick AGENTS.md files on this computer. Their contents go to every agent on every provider, not into your messages.</p>
      {instructionFiles.map(path => <div key={path} className="flex items-center gap-2 rounded-md px-2 py-1 text-xs" style={{ border: '1px solid var(--border)', background: 'var(--surface)' }}>
        <span className="flex-1 min-w-0 break-all font-mono">{path}</span>
        <button type="button" aria-label={`Remove ${path}`} disabled={busy} onClick={() => setInstructionFiles(instructionFiles.filter(item => item !== path))} style={{ color: 'var(--text-3)' }}>×</button>
      </div>)}
      <Button size="sm" className="self-start" disabled={busy} onClick={() => void addInstructionFile()}>+ Choose file…</Button>
      {saved.instructions && <p className="text-xs" style={{ color: 'var(--text-3)' }}>Older typed instructions are still applied.</p>}
    </section>

    <section className="grid gap-2">
      <strong>Default agent for new chats</strong>
      <select aria-label="Default agent for new chats" className={field} value={defaultAgentId} disabled={busy} onChange={event => setDefaultAgentId(event.currentTarget.value)}>
        {primaryAgents.map(agent => <option key={agent.id} value={agent.id}>{agent.id} — {agent.description}</option>)}
        {!primaryAgents.some(agent => agent.id === defaultAgentId) && <option value={defaultAgentId}>{defaultAgentId} (not found)</option>}
      </select>
    </section>

    <section className="grid gap-2">
      <strong>Default skills</strong>
      <p className="text-xs" style={{ color: 'var(--text-3)' }}>Skills every agent always has. Pick from the Skills store. Agents also get any skills listed on the agent itself.</p>
      {skills.length === 0 ? <p className="text-sm">The Skills store is empty. Add skills there first.</p> : <ChipPicker label="skill" values={defaultSkills} options={skillOptions} onChange={setDefaultSkills} disabled={busy} />}
    </section>

    <section className="grid gap-2">
      <strong>Default agents</strong>
      <p className="text-xs" style={{ color: 'var(--text-3)' }}>Agents from the UAM agent store every provider gets. Build, Plan and the chat's own agent are always included.</p>
      <ChipPicker label="agent" values={defaultAgents} options={agentOptions} onChange={setDefaultAgents} disabled={busy} />
    </section>

    <div className="flex items-center gap-2"><Button size="sm" variant="primary" loading={busy} onClick={() => void saveDefaults()}>Save</Button></div>

    <section className="grid gap-2">
      <div className="flex items-center justify-between"><strong>Agent store</strong><Button size="sm" disabled={busy || Boolean(editing)} onClick={() => setEditing({ draft: blankAgent(), isNew: true })}>+ Add agent</Button></div>
      <p className="text-xs" style={{ color: 'var(--text-3)' }}>Saved to the UAM agent store and installed natively for every provider. Click an agent to edit it.</p>
      {agentErrors.map(error => <p key={error} role="alert" className="text-xs" style={{ color: 'var(--red)' }}>{error}</p>)}
      {[...agents, ...(editing?.isNew ? [editing.draft] : [])].map((agent, index) => {
        const isOpen = editing ? (editing.isNew ? index === agents.length : editing.draft.id === agent.id) : false
        const draft = isOpen && editing ? editing.draft : agent
        const set = (change: Partial<Agent>) => editing && setEditing({ ...editing, draft: { ...editing.draft, ...change } })
        return <div key={isOpen && editing?.isNew ? 'new' : agent.id} className="rounded-lg" style={{ border: '1px solid var(--border)' }}>
          <div className="flex items-center gap-3 px-3 py-2 text-sm">
            <button type="button" className="flex-1 min-w-0 text-left" aria-expanded={isOpen} disabled={busy || (Boolean(editing) && !isOpen)} onClick={() => setEditing(isOpen ? null : { draft: { ...agent }, isNew: false })}>
              <strong>{isOpen ? '▾' : '▸'} {draft.id || 'New agent'}</strong> <span className="text-xs" style={{ color: 'var(--text-3)' }}>{agent.builtIn ? 'Built in · read only' : agent.description}</span>
            </button>
            {!agent.builtIn && !(isOpen && editing?.isNew) && <Button size="sm" variant="danger" disabled={busy} aria-label={`Delete agent ${agent.id}`} onClick={() => void deleteAgent(agent.id)}>Delete</Button>}
          </div>
          {isOpen && <div className="grid gap-3 px-3 pb-3 text-sm">
            <label className="grid gap-1"><span>Name</span><input className={field} value={draft.id} placeholder="reviewer" disabled={busy || agent.builtIn || !editing?.isNew} onChange={event => set({ id: event.currentTarget.value })} /><span className="text-xs" style={{ color: 'var(--text-3)' }}>Lowercase letters, numbers, - or _. Cannot be changed after saving.</span></label>
            <label className="grid gap-1"><span>Description</span><input className={field} value={draft.description} placeholder="Reviews diffs for bugs" disabled={busy || agent.builtIn} onChange={event => set({ description: event.currentTarget.value })} /></label>
            <div className="grid grid-cols-2 gap-3">
              <label className="grid gap-1"><span>Used as</span><select className={field} value={draft.mode} disabled={busy || agent.builtIn} onChange={event => set({ mode: event.currentTarget.value })}><option value="primary">Chat agent</option><option value="subagent">Helper (delegated to)</option><option value="both">Both</option></select></label>
              <label className="grid gap-1"><span>Workspace access</span><select className={field} value={draft.workspaceAccess} disabled={busy || agent.builtIn} onChange={event => set({ workspaceAccess: event.currentTarget.value })}><option value="write">Read and write</option><option value="read">Read only</option></select></label>
            </div>
            <div className="grid gap-1"><span>Skills for this agent</span><ChipPicker label="skill" values={draft.skills} options={skillOptions} onChange={skills => set({ skills })} disabled={busy || agent.builtIn} /></div>
            <label className="grid gap-1"><span>Instructions</span><textarea className={`${field} font-mono text-xs`} rows={10} value={draft.instructions} disabled={busy || agent.builtIn} onChange={event => set({ instructions: event.currentTarget.value })} /></label>
            {!agent.builtIn && <div className="flex gap-2"><Button size="sm" variant="primary" loading={busy} onClick={() => void saveAgent()}>Save agent</Button><Button size="sm" disabled={busy} onClick={() => setEditing(null)}>Cancel</Button></div>}
          </div>}
        </div>
      })}
    </section>
    {message && <p role="status" className="text-sm">{message}</p>}
  </div>
}
