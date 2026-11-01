import { forwardRef, useCallback, useEffect, useImperativeHandle, useRef, useState } from 'react'
import { Bot, FileText, Pencil, Plus, X } from 'lucide-react'
import { useAppStore } from '../../store/useAppStore'
import { sendToCEF } from '../../ipc/cefBridge'
import { Button, ConfirmDialog, IconButton, MenuSelect, Notice, Overlay, Presence, usePresence, Reveal } from '../ui'
import { SaveButton, SettingRow, SettingsField, SettingsLoading, SettingsSection } from './SettingsPrimitives'
import './provider-settings.css'
import { UnsavedChangesDialog } from './UnsavedChangesDialog'
import { reportAppFailure, reportAppNotice } from '../../utils/appFailure'

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
  const [saving, setSaving] = useState<'defaults' | 'agent' | null>(null)
  const [pendingDelete, setPendingDelete] = useState<string | null>(null)
  const [pendingExit, setPendingExit] = useState<{ next: () => void } | null>(null)
  const [agentExitPrompt, setAgentExitPrompt] = useState(false)
  const editingOriginal = useRef<Agent | null>(null)
  const [instructionEditor, setInstructionEditor] = useState<InstructionEditor | null>(null)
  const instructionRequest = useRef(0)
  const instructionOperation = useRef(false)
  const instructionTrigger = useRef<HTMLButtonElement | null>(null)
  const instructionTextarea = useRef<HTMLTextAreaElement | null>(null)
  const instructionRetry = useRef<HTMLButtonElement | null>(null)
  const [instructionFocusRequest, setInstructionFocusRequest] = useState(0)
  const instructionPresence = usePresence(instructionEditor !== null)
  const lastInstructionEditor = useRef<InstructionEditor | null>(null)
  if (instructionEditor) lastInstructionEditor.current = instructionEditor
  const shownEditor = instructionEditor ?? lastInstructionEditor.current
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

  const run = async (action: () => Promise<{ ok: boolean; error?: string }>, success: string, save: 'defaults' | 'agent' | null = null) => {
    setBusy(true); setSaving(save)
    try {
      const result = await action()
      if (result.ok) reportAppNotice(success, 'success')
      else reportAppFailure(result.error || 'Could not save.', 'Your changes are kept. Try again.')
      return result.ok
    }
    catch { reportAppFailure('Could not save.', 'Your changes are kept. Try again.'); return false }
    finally { setBusy(false); setSaving(null) }
  }
  const startEditing = (next: { draft: Agent; isNew: boolean } | null) => { editingOriginal.current = next ? { ...next.draft } : null; setAgentExitPrompt(false); setEditing(next) }
  const draftId = editing ? editing.draft.id.trim().toLowerCase() : ''
  const nameError = !editing?.isNew ? '' : !draftId ? 'Enter a name.' : !AGENT_NAME.test(draftId) ? 'Use lowercase letters, numbers, - or _.' : agents.some(agent => agent.id === draftId) ? `An agent named ${draftId} already exists.` : ''
  const agentDirty = Boolean(editing && !same(editing.draft, editingOriginal.current))
  const saveAgent = async () => {
    if (!editing || nameError) return false
    const draft = { ...editing.draft, id: draftId, description: editing.draft.description.trim() }
    if (!await run(() => sendToCEF({ action: 'saveAgentDefinition', payload: draft }), `Agent ${draft.id} saved.`, 'agent')) return false
    startEditing(null); await loadAgents()
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
  }, 'Saved. New and restarted chats get this setup.', 'defaults')
  const undoDefaults = () => { setInstructionFiles(saved.instructionFiles); setDefaultSkills(saved.defaultSkills ?? []); setDefaultAgents(saved.defaultAgents ?? []); setDefaultAgentId(saved.defaultAgentId) }
  const dirty = defaultsDirty || agentDirty || instructionDirty
  useImperativeHandle(ref, () => ({ requestLeave(next) {
    if (busy || instructionOperation.current) return
    if (dirty) setPendingExit({ next })
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
  // Unsaved agent edits ask inside the editor footer; a dialog must not open on top of the editor overlay.
  const closeEditor = () => { if (busy) return; if (agentExitPrompt) setAgentExitPrompt(false); else if (agentDirty) setAgentExitPrompt(true); else startEditing(null) }

  if (!agentsLoaded) return <SettingsLoading label="Loading agents" rows={3} />

  const leaveDialog = <Presence open={pendingExit !== null}>{pendingExit && <UnsavedChangesDialog
    title="Save central setup changes?"
    busy={busy} error={instructionDirty && instructionEditor?.error ? instructionEditor.error : undefined}
    onStay={() => setPendingExit(null)}
    onDiscard={() => { undoDefaults(); setInstructionEditor(null); startEditing(null); const next = pendingExit.next; setPendingExit(null); next() }}
    onSave={!nameError ? async () => {
      if (instructionDirty && !await saveInstructionFile()) return
      if (defaultsDirty && !await saveDefaults()) return
      if (agentDirty && !await saveAgent()) return
      const next = pendingExit.next; setPendingExit(null); next()
    } : undefined}
  />}</Presence>
  return <div className="grid gap-6" style={{ color: 'var(--text)' }}>
    {leaveDialog}
    <ConfirmDialog open={pendingDelete !== null} title="Delete agent?" confirmLabel="Delete" busy={busy} onCancel={() => setPendingDelete(null)} onConfirm={() => { if (pendingDelete) void deleteAgent(pendingDelete) }}>This removes the agent from the UAM agent store.</ConfirmDialog>
    <SettingsSection title="Shared instructions"
      actions={<Button size="sm" leadingIcon={<Plus size={14} />} disabled={busy} onClick={() => void addInstructionFile()}>Choose file…</Button>}>
      <Reveal open={instructionFiles.length === 0}><SettingRow title="No instruction files" description="Choose an AGENTS.md file to share." /></Reveal>
      {instructionFiles.map(path => <SettingRow key={path} icon={<FileText size={15} aria-hidden />} title={fileName(path)}
        description={<span className="uam-prov-detail">{path}</span>}
        control={<>
          <IconButton size="sm" icon={<Pencil size={14} />} label={`Edit ${fileName(path)}`} disabled={busy || instructionDirty || !saved.instructionFiles.includes(path)} onClick={event => { instructionTrigger.current = event.currentTarget; void openInstructionFile(path) }} />
          <IconButton size="sm" icon={<X size={14} />} label={`Remove ${fileName(path)}`} disabled={busy || instructionEditor?.path === path} onClick={() => setInstructionFiles(instructionFiles.filter(item => item !== path))} />
        </>}>
        {instructionPresence.mounted && shownEditor?.path === path && <div className="uam-central-editor uam-presence grid gap-3" data-state={instructionPresence.closing ? 'closed' : 'open'}>
          <Reveal open={shownEditor.loading}><SettingsLoading label="Opening instruction file" rows={3} /></Reveal>
          <Reveal open={!shownEditor.loading} className="grid gap-3"><>
            <Reveal open={!!(shownEditor.error)}><Notice key={shownEditor.error} tone="error" dismissLabel="Dismiss">{shownEditor.error}</Notice></Reveal>
            {!shownEditor.readFailed && <SettingsField label={fileName(path)}>
              <textarea ref={instructionTextarea} aria-label="Instruction file contents" className={field} rows={12} spellCheck={false} disabled={busy} style={{ minHeight: 180, resize: 'vertical', fontFamily: 'var(--font-mono)' }} value={shownEditor.content} onChange={event => setInstructionEditor({ ...shownEditor, content: event.currentTarget.value })} />
            </SettingsField>}
            <div className="flex justify-end gap-2">
              <Button size="sm" disabled={busy} onClick={() => setInstructionEditor(null)}>{instructionDirty ? 'Discard file changes' : 'Close file'}</Button>
              <Reveal open={shownEditor.readFailed}><Button ref={instructionRetry} size="sm" disabled={busy} onClick={() => void openInstructionFile(path)}>Retry</Button></Reveal>
              <Button size="sm" variant="primary" disabled={busy || !instructionDirty} onClick={() => void saveInstructionFile()}>Save file</Button>
            </div>
          </></Reveal>
        </div>}
      </SettingRow>)}
      <Reveal open={!!(saved.instructions)}><SettingRow title="Older typed instructions" description="Still applied. Choosing files does not remove them." /></Reveal>
    </SettingsSection>

    <SettingsSection title="Defaults">
      <SettingRow title="Default agent for new chats"
        control={<MenuSelect label="Default agent for new chats" value={defaultAgentId} disabled={busy} options={defaultAgentChoices} onChange={setDefaultAgentId} />} />
      <Reveal open={skills.length === 0}><p className="uam-prov-sub">No skills yet. Add skills in the Skills page first.</p></Reveal>
      <Reveal open={skills.length > 0}><SettingRow title="Skills on every agent" description={defaultSkills.length ? undefined : 'None chosen.'}
          control={<ChipAdd label="skill" values={defaultSkills} options={skillOptions} onChange={setDefaultSkills} disabled={busy} />}>
          <Reveal open={defaultSkills.length > 0}><ChipList label="default skills" values={defaultSkills} options={skillOptions} onChange={setDefaultSkills} disabled={busy} /></Reveal>
        </SettingRow></Reveal>
      <SettingRow title="Agents on every provider" description={defaultAgents.length ? undefined : 'None chosen.'}
        control={<ChipAdd label="agent" values={defaultAgents} options={agentOptions} onChange={setDefaultAgents} disabled={busy} />}>
        <Reveal open={defaultAgents.length > 0}><ChipList label="default agents" values={defaultAgents} options={agentOptions} onChange={setDefaultAgents} disabled={busy} /></Reveal>
      </SettingRow>
    </SettingsSection>

    <div className="flex items-center justify-end gap-3">
      <SaveButton saving={saving === 'defaults'} loading={busy} disabled={busy || (saved.enabled && !defaultsDirty)} onClick={() => void saveDefaults()}>Save</SaveButton>
    </div>

    <SettingsSection title="Agent store"
      actions={<Button size="sm" leadingIcon={<Plus size={14} />} disabled={busy} onClick={() => startEditing({ draft: blankAgent(), isNew: true })}>Add agent</Button>}>
      <Reveal open={agentErrors.length > 0}>{agentErrors.map(error => <Notice key={error} tone="error" title="Some agents could not be read" dismissLabel="Dismiss">{error}</Notice>)}</Reveal>
      <Reveal open={agents.length === 0}><p className="uam-prov-sub">No agents yet. Add one to give every provider a helper.</p></Reveal>
      {agents.map(agent => {
        const usedAs = USED_AS.find(option => option.value === agent.mode)
        return <SettingRow key={agent.id} icon={<Bot size={15} aria-hidden />}
          title={agent.id}
          description={agent.builtIn ? 'Built in · read only' : agent.description || 'No description yet'}
          status={usedAs?.label}
          control={<>
            <IconButton size="sm" icon={<Pencil size={14} />} label={agent.builtIn ? `View ${agent.id}` : `Edit ${agent.id}`} disabled={busy} onClick={() => startEditing({ draft: { ...agent }, isNew: false })} />
            {!agent.builtIn && <IconButton size="sm" icon={<X size={14} />} label={`Delete agent ${agent.id}`} disabled={busy} onClick={() => setPendingDelete(agent.id)} />}
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
                <span className="text-sm font-semibold">{isNew ? 'New agent' : builtIn ? draft.id : `Edit ${draft.id}`}</span>
                {!builtIn && <p className="uam-prov-sub">Changes apply to new chats.</p>}
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
                  <Reveal open={draft.skills.length > 0}><ChipList label="this agent" values={draft.skills} options={skillOptions} onChange={skills => set({ skills })} disabled={busy || builtIn} /></Reveal>
                  <ChipAdd label="skill" values={draft.skills} options={skillOptions} onChange={skills => set({ skills })} disabled={busy || builtIn} />
                </div>
              </SettingsField>
              <SettingsField label="Instructions">
                <textarea className={field} rows={10} style={{ minHeight: 160, height: "auto", resize: "vertical", fontFamily: "var(--font-mono)", fontSize: "var(--fs-sm)" }} value={draft.instructions} disabled={busy || builtIn} onChange={event => set({ instructions: event.currentTarget.value })} />
              </SettingsField>
            </div>
            <footer className="uam-central-dialog__foot">
              <Reveal open={builtIn} className="flex items-center justify-end gap-2">
                <Button size="sm" onClick={closeEditor}>Close</Button>
              </Reveal>
              <Reveal open={!builtIn && agentExitPrompt} className="flex min-w-0 flex-1 items-center justify-end gap-2">
                <p className="mr-auto text-sm" role="status">Save agent changes? Your changes have not been saved.</p>
                <Button size="sm" onClick={() => setAgentExitPrompt(false)}>Keep editing</Button>
                <Button size="sm" variant="secondary" onClick={() => startEditing(null)}>Discard</Button>
                <SaveButton saving={saving === 'agent'} loading={busy} disabled={busy || Boolean(nameError)} onClick={() => void saveAgent()}>Save agent</SaveButton>
              </Reveal>
              <Reveal open={!builtIn && !agentExitPrompt} className="flex items-center justify-end gap-2">
                <Button size="sm" disabled={busy} onClick={closeEditor}>Cancel</Button>
                <SaveButton saving={saving === 'agent'} loading={busy} disabled={busy || !agentDirty || Boolean(nameError)} onClick={() => void saveAgent()}>Save agent</SaveButton>
              </Reveal>
            </footer>
          </div>
        })()}
      </Overlay>
    </Presence>
  </div>
})
