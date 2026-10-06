import { useRef, useState } from 'react'
import { useAppStore, type McpServerConfiguration } from '../../store/useAppStore'
import { SSH_ENABLED } from '../../config/buildFeatures'
import { Button } from '../ui'

type Reference = McpServerConfiguration['environment'][number]
const emptyServer = (): McpServerConfiguration => ({ id: crypto.randomUUID(), name: '', workspaceDirectory: '', executionHostId: '', transport: 'stdio', command: '', args: [], url: '', environment: [], headers: [], enabled: true })
const absolutePath = (value: string) => /^(\/|[a-z]:[\\/]|\\\\)/i.test(value)

/** Edit shared launch configuration without writing individual provider configuration files. */
export function McpServerSetup({ disabled = false, onBusyChange }: { disabled?: boolean; onBusyChange: (busy: boolean) => void }) {
  const servers = useAppStore(state => state.mcpServers)
  const hosts = useAppStore(state => state.executionHosts)
  const central = useAppStore(state => state.centralProviderConfiguration)
  const active = useAppStore(state => state.sessions.find(session => session.id === state.activeSessionId))
  const [draft, setDraft] = useState<McpServerConfiguration | null>(null)
  const [original, setOriginal] = useState<McpServerConfiguration | null>(null)
  const [step, setStep] = useState(0)
  const [args, setArgs] = useState('')
  const [message, setMessage] = useState('')
  const [removal, setRemoval] = useState<McpServerConfiguration | null>(null)
  const [busy, setBusy] = useState(false)
  const pending = useRef(false)
  const locked = busy || disabled
  const change = (update: Partial<McpServerConfiguration>) => { if (draft) setDraft({ ...draft, ...update }); setMessage('') }
  const run = async (action: () => Promise<{ ok: boolean; error?: string }>, success: string) => {
    if (pending.current || disabled) return false
    pending.current = true; setBusy(true); onBusyChange(true); setMessage('')
    try {
      const result = await action()
      setMessage(result.ok ? success : result.error || 'Could not save MCP settings.')
      return result.ok
    } catch { setMessage('Could not save MCP settings. Try again.'); return false }
    finally { pending.current = false; setBusy(false); onBusyChange(false) }
  }
  const start = (server?: McpServerConfiguration) => {
    setOriginal(server ? structuredClone(server) : null)
    setDraft(server ? structuredClone(server) : emptyServer()); setArgs(server?.args.join('\n') || ''); setStep(0); setMessage('')
  }
  const validate = () => {
    if (!draft) return false
    if (!draft.name.trim()) { setMessage('Enter a server name.'); return false }
    if (['uam-control', 'uam-computer'].includes(draft.name.trim())) { setMessage('This name is reserved for the built-in UAM service.'); return false }
    if (draft.transport === 'stdio' && !absolutePath(draft.command.trim())) { setMessage('Enter an absolute executable path on the computer that runs the provider.'); return false }
    if (draft.transport !== 'stdio') {
      try {
        const url = new URL(draft.url.trim())
        if (!['http:', 'https:'].includes(url.protocol) || !['localhost', '127.0.0.1', '[::1]'].includes(url.hostname) || url.username || url.password) throw Error()
      } catch { setMessage('Enter a localhost HTTP or HTTPS URL, such as http://localhost:3000/mcp.'); return false }
    }
    if (draft.workspaceDirectory.trim() && !absolutePath(draft.workspaceDirectory.trim())) { setMessage('Enter an absolute workspace path, or choose all workspaces.'); return false }
    const references = draft.transport === 'stdio' ? draft.environment : draft.headers
    if (references.some(ref => !ref.name.trim() || !/^[A-Za-z_][A-Za-z0-9_]*$/.test(ref.environmentVariable.trim()))) { setMessage('Each secret reference needs a name and a valid environment variable.'); return false }
    if (new Set(references.map(ref => draft.transport === 'stdio' ? ref.name.trim() : ref.name.trim().toLowerCase())).size !== references.length) { setMessage('Use each environment or header name once.'); return false }
    return true
  }
  const save = async () => {
    if (!draft || !validate()) return
    const next: McpServerConfiguration = { ...draft, name: draft.name.trim(), command: draft.transport === 'stdio' ? draft.command.trim() : '', args: draft.transport === 'stdio' ? args.split('\n').filter(arg => arg.length > 0) : [], url: draft.transport === 'stdio' ? '' : draft.url.trim(), workspaceDirectory: draft.workspaceDirectory.trim(), environment: draft.transport === 'stdio' ? draft.environment.map(trimReference) : [], headers: draft.transport === 'stdio' ? [] : draft.headers.map(trimReference) }
    const current = useAppStore.getState().mcpServers
    if (original && JSON.stringify(current.find(server => server.id === original.id)) !== JSON.stringify(original)) { setMessage('This server changed while you were editing. Cancel and reopen it.'); return }
    if (current.some(server => server.id !== next.id && server.name.toLowerCase() === next.name.toLowerCase() && server.workspaceDirectory === next.workspaceDirectory && (server.executionHostId || '') === (next.executionHostId || ''))) { setMessage('A server with this name already exists in this scope. Edit it or choose another name.'); return }
    if (await run(() => useAppStore.getState().setMcpServers([...current.filter(server => server.id !== next.id), next]), 'Server saved for all five providers. Start a new session or restart a running session to apply it.')) setDraft(null)
  }
  const field = (label: string, value: string, onChange: (value: string) => void, placeholder = '') => <label className="grid gap-1 text-sm">{label}<input className="uam-field w-full" value={value} placeholder={placeholder} disabled={locked} onChange={event => onChange(event.currentTarget.value)} /></label>
  const references = draft?.transport === 'stdio' ? draft.environment : draft?.headers || []
  const updateReferences = (values: Reference[]) => change(draft?.transport === 'stdio' ? { environment: values } : { headers: values })
  return <div className="space-y-5" style={{ color: 'var(--text)' }}>
    <p className="text-sm">Set up an MCP server once for Gemini, Codex, Claude Code, OpenCode and Copilot. Settings apply to Chat and CLI View, including SSH, on the next launch.</p>
    <div className="border-b pb-4 space-y-3" style={{ borderColor: 'var(--border)' }}>
      <strong>UAM MCP service</strong>
      <p className="text-sm">Built in. UAM manages the connection for questions, goals, skills and approved actions. No command or URL is needed.</p>
      <label className="flex items-center gap-2 text-sm"><input type="checkbox" checked={central.uamControlEnabled} disabled={locked} onChange={event => {
        const enabled = event.currentTarget.checked
        void run(() => { const store = useAppStore.getState(); return store.setCentralProviderConfiguration({ ...store.centralProviderConfiguration, uamControlEnabled: enabled }) }, 'UAM tools default saved. Existing chats retain their setting.')
      }} />Enable UAM MCP service for new chats</label>
      {active && <div className="flex items-center justify-between gap-3 text-sm"><span>Current chat: {active.name} · UAM tools {active.uamControlEnabled ? 'enabled' : 'disabled'}</span><Button size="sm" disabled={locked} onClick={() => void run(async () => ({ ok: await useAppStore.getState().setSessionUamControlEnabled(active.id, !active.uamControlEnabled), error: 'Stop the current session before changing its UAM tools setting.' }), 'Current chat UAM tools setting saved.')}>{active.uamControlEnabled ? 'Disable' : 'Enable'} for current chat</Button></div>}
      <p className="text-xs">Enabled is a configuration setting, not a live connection check.</p>
    </div>
    {disabled && <p className="text-sm">Save your advanced JSON edits before using the setup wizard.</p>}
    {!draft && <>
      <div className="flex items-center justify-between gap-3"><strong>Shared MCP servers</strong><Button size="sm" disabled={locked} onClick={() => start()}>Add MCP server</Button></div>
      {removal && <div role="group" aria-label="Confirm server removal" className="space-y-2 text-sm"><p>Remove {removal.name} from shared settings? Running sessions retain their configuration.</p><div className="flex gap-2"><Button size="sm" disabled={locked} onClick={() => setRemoval(null)}>Keep server</Button><Button size="sm" variant="danger" disabled={locked} onClick={() => void run(() => { const store = useAppStore.getState(); return store.setMcpServers(store.mcpServers.filter(item => item.id !== removal.id)) }, 'Server removed from shared settings.').then(ok => { if (ok) setRemoval(null) })}>Confirm removal</Button></div></div>}
      {servers.length === 0 && <p className="text-sm">No shared servers configured. Add a server to use it across providers.</p>}
      {servers.map(server => <div key={server.id} className="uam-settings-row flex flex-wrap items-center gap-3 text-sm">
        <div className="flex-1 min-w-0"><strong>{server.name}</strong><p className="text-xs break-all">{server.transport} · {server.workspaceDirectory || 'All workspaces'} · {server.executionHostId ? hosts.find(host => host.id === server.executionHostId)?.label || server.executionHostId : 'All hosts, including SSH'}</p><span className="text-xs">{server.enabled ? 'Saved · applies on next launch' : 'Disabled'}</span></div>
        <Button size="sm" disabled={locked} onClick={() => start(server)}>Edit {server.name}</Button>
        <Button size="sm" disabled={locked} onClick={() => void run(() => { const store = useAppStore.getState(); return store.setMcpServers(store.mcpServers.map(item => item.id === server.id ? { ...item, enabled: !item.enabled } : item)) }, 'Server setting saved. Applies on the next launch.')}>{server.enabled ? 'Disable' : 'Enable'} {server.name}</Button>
        <Button size="sm" variant="danger" disabled={locked} onClick={() => setRemoval(server)}>Remove {server.name}</Button>
      </div>)}
    </>}
    {draft && <div className="space-y-4 border-t pt-4" style={{ borderColor: 'var(--border)' }}>
      <h3 className="font-semibold">{original ? 'Edit' : 'Add'} MCP server · Step {step + 1} of 3: {['Connection', 'Scope and credentials', 'Review'][step]}</h3>
      {step === 0 && <>
        {field('Server name', draft.name, name => change({ name }), 'documentation')}
        <label className="grid gap-1 text-sm">Connection type<select className="uam-field w-full" value={draft.transport} disabled={locked} onChange={event => change({ transport: event.currentTarget.value as McpServerConfiguration['transport'] })}><option value="stdio">Local command (stdio)</option><option value="http">HTTP</option><option value="sse">SSE</option></select></label>
        {draft.transport === 'stdio' ? <>{field('Executable path', draft.command, command => change({ command }), '/usr/local/bin/npx or C:\\tools\\server.exe')}<label className="grid gap-1 text-sm">Arguments, one per line<textarea className="uam-field w-full" rows={4} value={args} disabled={locked} onChange={event => setArgs(event.currentTarget.value)} placeholder={'-y\n@playwright/mcp@latest'} /></label><p className="text-xs">Each line is one argument. Do not add shell quotes around paths with spaces.</p></> : <>{field('Server URL', draft.url, url => change({ url }), 'http://localhost:3000/mcp')}<p className="text-xs">UAM currently accepts localhost HTTP/SSE servers. On SSH, localhost means the remote host.</p></>}
      </>}
      {step === 1 && <>
        <label className="grid gap-1 text-sm">Execution host<select className="uam-field w-full" value={draft.executionHostId || ''} disabled={locked} onChange={event => change({ executionHostId: event.currentTarget.value })}><option value="">All hosts{SSH_ENABLED ? ', including SSH' : ''}</option>{hosts.filter(host => SSH_ENABLED || host.transport === 'local').map(host => <option key={host.id} value={host.id}>{host.label}</option>)}{draft.executionHostId && !hosts.some(host => host.id === draft.executionHostId) && <option value={draft.executionHostId}>{draft.executionHostId} (unavailable)</option>}</select></label>
        {field('Workspace path (leave empty for all workspaces)', draft.workspaceDirectory, workspaceDirectory => change({ workspaceDirectory }))}
        <p className="text-sm">Commands, workspace paths and secret variables belong to the computer that runs the provider. Use a host-specific entry when local and SSH paths differ.</p>
        <strong className="text-sm">{draft.transport === 'stdio' ? 'Environment variables' : 'HTTP headers'}</strong>
        <p className="text-xs">Reference an existing environment variable. UAM does not copy secret values to SSH hosts.</p>
        {references.map((reference, index) => <div key={index} className="flex flex-wrap gap-2 items-end">{field(`${draft.transport === 'stdio' ? 'Environment' : 'Header'} name ${index + 1}`, reference.name, name => updateReferences(references.map((item, i) => i === index ? { ...item, name } : item)))}{field(`Secret variable ${index + 1}`, reference.environmentVariable, environmentVariable => updateReferences(references.map((item, i) => i === index ? { ...item, environmentVariable } : item)), 'MCP_API_TOKEN')}<Button size="sm" disabled={locked} onClick={() => updateReferences(references.filter((_, i) => i !== index))}>Remove reference {index + 1}</Button></div>)}
        <Button size="sm" disabled={locked} onClick={() => updateReferences([...references, { name: '', environmentVariable: '' }])}>Add {draft.transport === 'stdio' ? 'environment variable' : 'header'}</Button>
      </>}
      {step === 2 && <div className="space-y-2 text-sm"><p><strong>{draft.name}</strong> · {draft.transport}</p><p className="break-all">{draft.transport === 'stdio' ? draft.command : draft.url}</p>{draft.transport === 'stdio' && <pre className="whitespace-pre-wrap">{args}</pre>}<p>{draft.workspaceDirectory || 'All workspaces'} · {draft.executionHostId ? hosts.find(host => host.id === draft.executionHostId)?.label || draft.executionHostId : 'All hosts, including SSH'}</p><p>Providers: Gemini, Codex, Claude Code, OpenCode and Copilot. Chat and CLI View.</p><p>{references.length} secret reference(s). Values are resolved on the executing host.</p><label className="flex items-center gap-2"><input type="checkbox" checked={draft.enabled} disabled={locked} onChange={event => change({ enabled: event.currentTarget.checked })} />Enable server</label><p>Save to apply on the next launch. Restart running sessions. Saving does not test the connection.</p></div>}
      <div className="flex gap-2"><Button size="sm" disabled={locked} onClick={() => { setDraft(null); setMessage('') }}>Cancel setup</Button>{step > 0 && <Button size="sm" disabled={locked} onClick={() => { setStep(step - 1); setMessage('') }}>Back</Button>}{step < 2 ? <Button size="sm" disabled={locked} onClick={() => { if (validate()) { setStep(step + 1); setMessage('') } }}>Next</Button> : <Button size="sm" loading={busy} disabled={disabled} onClick={() => void save()}>Save for all providers</Button>}</div>
    </div>}
    {message && <p role="status" className="text-sm">{message}</p>}
  </div>
}
function trimReference(reference: Reference): Reference { return { name: reference.name.trim(), environmentVariable: reference.environmentVariable.trim() } }
