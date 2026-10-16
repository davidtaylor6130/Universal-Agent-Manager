import { forwardRef, useCallback, useEffect, useImperativeHandle, useRef, useState } from 'react'
import { Bot, FileText, Pencil, Plus, Trash2, X } from 'lucide-react'
import { useAppStore } from '../../store/useAppStore'
import { sendToCEF } from '../../ipc/cefBridge'
import { Button, ConfirmDialog, IconButton, MenuSelect, Overlay, Presence } from '../ui'
import { SettingRow, SettingsField, SettingsLoading, SettingsSection } from './SettingsPrimitives'
import './provider-settings.css'
import { UnsavedChangesDialog } from './UnsavedChangesDialog'

const AGENT_NAME = /^[a-z0-9][a-z0-9_-]*$/
const same = (left: unknown, right: unknown) => JSON.stringify(left) === JSON.stringify(right)

type Agent = { id: string; description: string; mode: string; workspaceAccess: string; skills: string[]; delegates: string[]; instructions: string; builtIn: boolean }
type InstructionEditor = { path: string; content: string; original: string; loading: boolean; readFailed: boolean; error: string }
const blankAgent = (): Agent => ({ id: '', description: '', mode: 'primary', workspaceAccess: 'write', skills: [], delegates: [], instructions: '', builtIn: false })
const field = 'uam-field w-full'
type Option = { value: string; label: string; group?: string }
const USED_AS = [
  { value: 'primary', label: 'Chat agent', detail: 'Pick it as the agent for a chat.' },
  { value: 'subagent', label: 'Helper', detail: 'Other agents can delegate work to it.' },
  { value: 'both', label: 'Both', detail: 'A chat agent that helpers can also use.' },
]
const fileName = (path: string) => path.split(/[\\/]/).pop() || path

/** Chosen items as removable chips. */
function ChipList({ values, options, onChange, label, disabled }: { values: string[]; options: Option[]; onChange: (values: string[]) => void; label: string; disabled?: boolean }) {
  const labelFor = (value: string) => options.find(option => option.value === value)?.label ?? `${value} (missing)`
  return <div className="uam-central-chips">
    {values.map(value => <span key={value} className="uam-central-chip">
      {labelFor(value)}
      <IconButton size="sm" icon={<X size={12} />} label={`Remove ${labelFor(value)} from ${label}`} disabled={disabled} onClick={() => onChange(values.filter(item => item !== value))} />
    </span>)}
  </div>
}

/** Organised "Add" picker for the items not chosen yet. */
function ChipAdd({ label, values, options, onChange, disabled }: { label: string; values: string[]; options: Option[]; onChange: (values: string[]) => void; disabled?: boolean }) {
  const remaining = options.filter(option => !values.includes(option.value))
  return <MenuSelect
    label={`Add ${label}`}
    value=""
    disabled={disabled || remaining.length === 0}
    options={[
      { value: '', label: remaining.length ? `Add ${label}…` : `No more ${label}s` },
      ...remaining.map(option => ({ value: option.value, label: option.label, description: option.group })),
    ]}
    onChange={value => { if (value) onChange([...values, value]) }}
  />
}

/** Shared instructions, default agent, default skills and agents, and the agent store. Installed natively at every provider launch, local and SSH. */
export interface CentralProviderSettingsHandle { requestLeave(next: () => void): void }
export const CentralProviderSettings = forwardRef<CentralProviderSettingsHandle>(function CentralProviderSettings(_props, ref) {
  const saved = useAppStore(state => state.centralProviderConfiguration)
  const skills = useAppStore(state => state.markdownStoreEntries)
  const [instructionFiles, setInstructionFiles] = useState(saved.instructionFiles ?? [])
  const [defaultAgents, setDefaultAgents] = useState(saved.defaultAgents ?? [])
  const [defaultSkills, setDefaultSkills] = useState(saved.defaultSkills ?? [])
  const [defaultAgentId, setDefaultAgentId] = useState(saved.defaultAgentId)
  const [agents, setAgents] = useState<Agent[]>([])
  const [agentsLoaded, setAgentsLoaded] = useState(false)
  const [agentErrors, setAgentErrors] = useState<string[]>([])
  const [editing, setEditing] = useState<{ draft: Agent; isNew: boolean } | null>(null)
  const [busy, setBusy] = useState(false)
  const [message, setMessage] = useState<{ tone: 'ok' | 'error'; text: string } | null>(null)
  const [pendingDelete, setPendingDelete] = useState<string | null>(null)
  const [pendingExit, setPendingExit] = useState<{ next: () => void; scope: 'all' | 'agent' } | null>(null)
  const editingOriginal = useRef<Agent | null>(null)
  const [instructionEditor, setInstructionEditor] = useState<InstructionEditor | null>(null)
  const instructionRequest = useRef(0)
  const instructionOperation = useRef(false)
  const instructionTrigger = useRef<HTMLButtonElement | null>(null)
  const instructionTextarea = useRef<HTMLTextAreaElement | null>(null)
  const instructionRetry = useRef<HTMLButtonElement | null>(null)
  const [instructionFocusRequest, setInstructionFocusRequest] = useState(0)
  useEffect(() => () => { ++instructionRequest.current }, [])
  useEffect(() => {
    if (!instructionEditor) instructionTrigger.current?.focus()
  }, [instructionEditor?.path])
  useEffect(() => {
    if (instructionFocusRequest && !pendingExit && !editing && !pendingDelete) (instructionTextarea.current || instructionRetry.current)?.focus()
  }, [instructionFocusRequest])
  const instructionDirty = Boolean(instructionEditor && instructionEditor.content !== instructionEditor.original)

  const loadAgents = useCallback(async () => {
    const response = await sendToCEF<{ agents: Agent[]; errors: string[] }>({ action: 'listAgentDefinitions', payload: {} })
    if (response.ok && response.data) { setAgents(response.data.agents ?? []); setAgentErrors(response.data.errors ?? []) }
    else setAgentErrors([response.error || 'Could not load the agent store.'])
    setAgentsLoaded(true)
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
  const dirty = defaultsDirty || agentDirty || instructionDirty
  useImperativeHandle(ref, () => ({ requestLeave(next) {
    if (busy || instructionOperation.current) return
    if (dirty) setPendingExit({ next, scope: 'all' })
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
  const defaultAgentChoices = [
    ...primaryAgents.map(agent => ({ value: agent.id, label: agent.id, description: agent.description })),
    ...(primaryAgents.some(agent => agent.id === defaultAgentId) ? [] : [{ value: defaultAgentId, label: `${defaultAgentId} (not found)` }]),
  ]
  const addInstructionFile = async () => {
    const path = await useAppStore.getState().browseProviderAgentImport(instructionFiles[instructionFiles.length - 1] ?? '')
    if (path && !instructionFiles.includes(path)) setInstructionFiles([...instructionFiles, path])
  }
  const openInstructionFile = async (path: string) => {
    if (busy || instructionOperation.current || instructionDirty || !saved.instructionFiles.includes(path)) return
    const request = ++instructionRequest.current
    instructionOperation.current = true
    setBusy(true)
    setInstructionEditor({ path, content: '', original: '', loading: true, readFailed: false, error: '' })
    try {
      const response = await sendToCEF<{ path: string; content: string }>({ action: 'readCentralInstructionFile', payload: { path } })
      if (request !== instructionRequest.current) return
      if (!response.ok || response.data?.path !== path || typeof response.data.content !== 'string') {
        setInstructionEditor({ path, content: '', original: '', loading: false, readFailed: true, error: response.error || 'Could not open this instruction file.' })
      } else setInstructionEditor({ path, content: response.data.content, original: response.data.content, loading: false, readFailed: false, error: '' })
    } catch {
      if (request === instructionRequest.current) setInstructionEditor({ path, content: '', original: '', loading: false, readFailed: true, error: 'Could not open this instruction file.' })
    } finally {
      if (request === instructionRequest.current) { instructionOperation.current = false; setBusy(false); setInstructionFocusRequest(current => current + 1) }
    }
  }
  const saveInstructionFile = async () => {
    if (busy || instructionOperation.current || !instructionEditor || !instructionDirty) return false
    const draft = instructionEditor
    if (!useAppStore.getState().centralProviderConfiguration.instructionFiles.includes(draft.path)) {
      setInstructionEditor({ ...draft, error: 'This file is no longer in Central Setup. Your changes are retained.' })
      return false
    }
    const request = ++instructionRequest.current
    instructionOperation.current = true
    setBusy(true)
    setInstructionEditor({ ...draft, error: '' })
    try {
      const response = await sendToCEF<{ path: string; content: string }>({ action: 'saveCentralInstructionFile', payload: { path: draft.path, content: draft.content, expectedContent: draft.original } })
      if (request !== instructionRequest.current) return false
      if (!response.ok || response.data?.path !== draft.path || response.data.content !== draft.content) {
        setInstructionEditor({ ...draft, error: response.error || 'Could not confirm the save. Your changes are retained.' })
        return false
      }
      setInstructionEditor({ ...draft, original: draft.content, error: '' })
      return true
    } catch {
      if (request === instructionRequest.current) setInstructionEditor({ ...draft, error: 'Could not save this file. Your changes are retained.' })
      return false
    } finally {
      if (request === instructionRequest.current) { instructionOperation.current = false; setBusy(false); setInstructionFocusRequest(current => current + 1) }
    }
  }
  const closeEditor = () => { if (busy) return; if (agentDirty) setPendingExit({ next: () => startEditing(null), scope: 'agent' }); else startEditing(null) }

  if (!agentsLoaded) return <SettingsLoading label="Loading agents" rows={3} />

  const leaveDialog = pendingExit && <UnsavedChangesDialog
    title={pendingExit.scope === 'agent' ? 'Save agent changes?' : 'Save central setup changes?'}
    busy={busy} error={pendingExit.scope === 'all' && instructionDirty && instructionEditor?.error ? instructionEditor.error : message?.tone === 'error' ? message.text : undefined}
    onStay={() => setPendingExit(null)}
    onDiscard={() => { if (pendingExit.scope === 'all') { undoDefaults(); setInstructionEditor(null) }; startEditing(null); const next = pendingExit.next; setPendingExit(null); next() }}
    onSave={!nameError ? async () => {
      if (pendingExit.scope === 'all' && instructionDirty && !await saveInstructionFile()) return
      if (pendingExit.scope === 'all' && defaultsDirty && !await saveDefaults()) return
      if (agentDirty && !await saveAgent()) return
      const next = pendingExit.next; setPendingExit(null); next()
    } : undefined}
  />
  return <div className="grid gap-6" style={{ color: 'var(--text)' }}>
    {leaveDialog}
    <ConfirmDialog open={pendingDelete !== null} title="Delete agent?" confirmLabel="Delete" busy={busy} error={message?.tone === 'error' ? message.text : undefined} onCancel={() => setPendingDelete(null)} onConfirm={() => { if (pendingDelete) void deleteAgent(pendingDelete) }}>This removes the agent from the UAM agent store.</ConfirmDialog>
    <SettingsSection title="Shared instructions" description="Sent to every agent, not into your messages."
      actions={<Button size="sm" leadingIcon={<Plus size={14} />} disabled={busy} onClick={() => void addInstructionFile()}>Choose file…</Button>}>
      {instructionFiles.length === 0 && <SettingRow title="No instruction files" description="Choose an AGENTS.md file to share." />}
      {instructionFiles.map(path => <SettingRow key={path} icon={<FileText size={15} aria-hidden />} title={fileName(path)}
        description={<span className="uam-prov-detail">{path}</span>}
        control={<>
          <IconButton size="sm" icon={<Pencil size={14} />} label={`Edit ${fileName(path)}`} disabled={busy || instructionDirty || !saved.instructionFiles.includes(path)} onClick={event => { instructionTrigger.current = event.currentTarget; void openInstructionFile(path) }} />
          <IconButton size="sm" icon={<X size={14} />} label={`Remove ${fileName(path)}`} disabled={busy || instructionEditor?.path === path} onClick={() => setInstructionFiles(instructionFiles.filter(item => item !== path))} />
        </>}>
        {instructionEditor?.path === path && <div className="uam-central-editor grid gap-3">
          {instructionEditor.loading ? <SettingsLoading label="Opening instruction file" rows={3} /> : <>
            {instructionEditor.error && <p role="alert" className="text-xs" style={{ color: 'var(--error)' }}>{instructionEditor.error}</p>}
            {!instructionEditor.readFailed && <SettingsField label={fileName(path)}>
              <textarea ref={instructionTextarea} aria-label="Instruction file contents" className={field} rows={12} spellCheck={false} disabled={busy} style={{ minHeight: 180, resize: 'vertical', fontFamily: 'var(--font-mono)' }} value={instructionEditor.content} onChange={event => setInstructionEditor({ ...instructionEditor, content: event.currentTarget.value })} />
            </SettingsField>}
            <div className="flex justify-end gap-2">
              <Button size="sm" disabled={busy} onClick={() => setInstructionEditor(null)}>{instructionDirty ? 'Discard file changes' : 'Close file'}</Button>
              {instructionEditor.readFailed && <Button ref={instructionRetry} size="sm" disabled={busy} onClick={() => void openInstructionFile(path)}>Retry</Button>}
              <Button size="sm" variant="primary" aria-label="Save instruction file" disabled={busy || !instructionDirty} onClick={() => void saveInstructionFile()}>Save file</Button>
            </div>
          </>}
        </div>}
      </SettingRow>)}
      {saved.instructions && <SettingRow title="Older typed instructions" description="Still applied. Choosing files does not remove them." />}
    </SettingsSection>

    <SettingsSection title="Defaults">
      <SettingRow title="Default agent for new chats"
        control={<MenuSelect label="Default agent for new chats" value={defaultAgentId} disabled={busy} options={defaultAgentChoices} onChange={setDefaultAgentId} />} />
      {skills.length === 0
        ? <SettingRow title="No skills yet" description="Add skills in the Skills page first." />
        : <SettingRow title="Skills on every agent" description={defaultSkills.length ? undefined : 'None chosen.'}
          control={<ChipAdd label="skill" values={defaultSkills} options={skillOptions} onChange={setDefaultSkills} disabled={busy} />}>
          {defaultSkills.length > 0 && <ChipList label="default skills" values={defaultSkills} options={skillOptions} onChange={setDefaultSkills} disabled={busy} />}
        </SettingRow>}
      <SettingRow title="Agents on every provider" description={defaultAgents.length ? undefined : 'None chosen.'}
        control={<ChipAdd label="agent" values={defaultAgents} options={agentOptions} onChange={setDefaultAgents} disabled={busy} />}>
        {defaultAgents.length > 0 && <ChipList label="default agents" values={defaultAgents} options={agentOptions} onChange={setDefaultAgents} disabled={busy} />}
      </SettingRow>
    </SettingsSection>

    <div className="flex items-center justify-end gap-3">
      {message && !editing && <span role="status" className="uam-set-section__desc mr-auto">{message.text}</span>}
      <Button size="sm" variant="primary" loading={busy} disabled={busy || (saved.enabled && !defaultsDirty)} onClick={() => void saveDefaults()}>Save</Button>
    </div>

    <SettingsSection title="Agent store" description="Installed for every provider. Built-in agents are read only."
      actions={<Button size="sm" leadingIcon={<Plus size={14} />} disabled={busy} onClick={() => startEditing({ draft: blankAgent(), isNew: true })}>Add agent</Button>}>
      {agentErrors.map(error => <SettingRow key={error} title="Some agents could not be read" description={<span style={{ color: 'var(--error)' }}>{error}</span>} />)}
      {agents.length === 0 && <SettingRow title="No agents yet" description="Add one to give every provider a helper." />}
      {agents.map(agent => {
        const usedAs = USED_AS.find(option => option.value === agent.mode)
        return <SettingRow key={agent.id} icon={<Bot size={15} aria-hidden />}
          title={agent.id}
          description={agent.builtIn ? 'Built in · read only' : agent.description || 'No description yet'}
          status={usedAs?.label}
          control={<>
            <IconButton size="sm" icon={<Pencil size={14} />} label={agent.builtIn ? `View ${agent.id}` : `Edit ${agent.id}`} disabled={busy} onClick={() => startEditing({ draft: { ...agent }, isNew: false })} />
            {!agent.builtIn && <IconButton variant="danger" size="sm" icon={<Trash2 size={14} />} label={`Delete agent ${agent.id}`} disabled={busy} onClick={() => setPendingDelete(agent.id)} />}
          </>} />
      })}
    </SettingsSection>

    <Presence open={editing !== null}>
      <Overlay open zIndex={60} onBackdropMouseDown={closeEditor}>
        {editing && (() => {
          const { draft, isNew } = editing
          const builtIn = draft.builtIn
          const set = (change: Partial<Agent>) => setEditing(current => current && { ...current, draft: { ...current.draft, ...change } })
          return <div role="dialog" aria-modal="true" aria-label={isNew ? 'New agent' : `Edit ${draft.id}`} tabIndex={-1} className="uam-mcp-dialog uam-central-dialog"
            onKeyDown={event => { if (event.key === 'Escape') { event.preventDefault(); event.stopPropagation(); closeEditor() } }}>
            <header className="uam-central-dialog__head">
              <div className="min-w-0">
                <strong className="text-sm">{isNew ? 'New agent' : builtIn ? draft.id : `Edit ${draft.id}`}</strong>
                <p className="uam-prov-sub">{builtIn ? 'Built in, read only.' : 'Changes apply to new chats.'}</p>
              </div>
              <IconButton size="sm" icon={<X size={14} />} label="Close agent editor" disabled={busy} onClick={closeEditor} />
            </header>
            <div className="uam-central-dialog__body">
              <SettingsField label="Name" hint="Lowercase letters, numbers, - or _. Cannot be changed after saving.">
                <input className={field} aria-label="Agent name" value={draft.id} placeholder="reviewer" disabled={busy || builtIn || !isNew} onChange={event => set({ id: event.currentTarget.value })} />
              </SettingsField>
              <SettingsField label="Description">
                <input className={field} aria-label="Agent description" value={draft.description} placeholder="Reviews diffs for bugs" disabled={busy || builtIn} onChange={event => set({ description: event.currentTarget.value })} />
              </SettingsField>
              <div className="grid gap-3 sm:grid-cols-2">
                <SettingsField label="Used as">
                  <MenuSelect label="Used as" value={draft.mode} disabled={busy || builtIn} options={USED_AS.map(option => ({ value: option.value, label: option.label, description: option.detail }))} onChange={mode => set({ mode })} />
                </SettingsField>
                <SettingsField label="Workspace access">
                  <MenuSelect label="Workspace access" value={draft.workspaceAccess} disabled={busy || builtIn} options={[{ value: 'write', label: 'Read and write' }, { value: 'read', label: 'Read only' }]} onChange={workspaceAccess => set({ workspaceAccess })} />
                </SettingsField>
              </div>
              <SettingsField label="Skills for this agent">
                <div className="grid gap-2">
                  {draft.skills.length > 0 && <ChipList label="this agent" values={draft.skills} options={skillOptions} onChange={skills => set({ skills })} disabled={busy || builtIn} />}
                  <ChipAdd label="skill" values={draft.skills} options={skillOptions} onChange={skills => set({ skills })} disabled={busy || builtIn} />
                </div>
              </SettingsField>
              <SettingsField label="Instructions">
                <textarea className={field} rows={10} style={{ minHeight: 160, height: "auto", resize: "vertical", fontFamily: "var(--font-mono)", fontSize: "var(--fs-sm)" }} value={draft.instructions} disabled={busy || builtIn} onChange={event => set({ instructions: event.currentTarget.value })} />
              </SettingsField>
            </div>
            <footer className="uam-central-dialog__foot">
              {message && <span role="status" className="uam-set-section__desc mr-auto">{message.text}</span>}
              {builtIn
                ? <Button size="sm" onClick={closeEditor}>Close</Button>
                : <>
                  <Button size="sm" disabled={busy} onClick={closeEditor}>Cancel</Button>
                  <Button size="sm" variant="primary" loading={busy} disabled={busy || !agentDirty || Boolean(nameError)} onClick={() => void saveAgent()}>Save agent</Button>
                </>}
            </footer>
          </div>
        })()}
      </Overlay>
    </Presence>
  </div>
})
