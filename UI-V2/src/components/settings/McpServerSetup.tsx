import { useEffect, useState, type ReactNode } from 'react'
import { useAppStore, type McpServerConfiguration } from '../../store/useAppStore'
import { ChevronRight, Plus, X } from 'lucide-react'
import { Button, IconButton, MenuSelect, Switch } from '../ui'

type Reference = McpServerConfiguration['environment'][number]
type JsonEntry = { type: 'builtin' | 'local' | 'remote'; enabled?: boolean; command?: string[]; environment?: Record<string, string>; url?: string; headers?: Record<string, string>; sse?: boolean; workspace?: string; host?: string }
const BUILTIN = 'uam-mcp'
const absolutePath = (value: string) => /^(\/|[a-z]:[\\/]|\\\\)/i.test(value)
const refsToJson = (refs: Reference[]) => refs.length ? Object.fromEntries(refs.map(ref => [ref.name, ref.value !== undefined ? ref.value : `{env:${ref.environmentVariable}}`])) : undefined
const refsFromJson = (value: unknown, label: string): Reference[] => {
  if (value === undefined) return []
  if (!value || typeof value !== 'object' || Array.isArray(value)) throw Error(`${label} must be an object of "NAME": "value" or "NAME": "{env:VARIABLE}".`)
  return Object.entries(value).map(([name, text]) => {
    const match = typeof text === 'string' ? /^\{env:([A-Za-z_][A-Za-z0-9_]*)\}$/.exec(text) : null
    if (typeof text !== 'string') throw Error(`${label} "${name}" must be text.`)
    return match ? { name, environmentVariable: match[1] } : { name, environmentVariable: '', value: text }
  })
}

/** OpenCode-style document; the form edits the same data. */
export function toJsonText(servers: McpServerConfiguration[], uamEnabled: boolean) {
  const mcp: Record<string, JsonEntry> = { [BUILTIN]: { type: 'builtin', enabled: uamEnabled } }
  for (const server of servers) mcp[server.name] = {
    type: server.transport === 'stdio' ? 'local' : 'remote',
    ...(server.transport === 'stdio' ? { command: [server.command, ...server.args], environment: refsToJson(server.environment) } : { url: server.url, headers: refsToJson(server.headers), sse: server.transport === 'sse' || undefined }),
    enabled: server.enabled,
    workspace: server.workspaceDirectory || undefined,
    host: server.executionHostId || undefined,
  }
  return JSON.stringify({ mcp }, null, 2)
}

export function fromJsonText(text: string, previous: McpServerConfiguration[]): { servers: McpServerConfiguration[]; uamEnabled: boolean } {
  let parsed: unknown
  try { parsed = JSON.parse(text) } catch { throw Error('The JSON is not valid. Check for a missing comma or quote.') }
  const mcp = (parsed as { mcp?: unknown })?.mcp
  if (!mcp || typeof mcp !== 'object' || Array.isArray(mcp)) throw Error('Put servers inside { "mcp": { ... } }.')
  let uamEnabled = true
  const servers = Object.entries(mcp as Record<string, JsonEntry>).flatMap(([name, entry]): McpServerConfiguration[] => {
    if (!entry || typeof entry !== 'object') throw Error(`"${name}" must be an object.`)
    if (name === BUILTIN) { uamEnabled = entry.enabled !== false; return [] }
    if (entry.type !== 'local' && entry.type !== 'remote') throw Error(`"${name}" needs "type": "local" or "remote".`)
    const workspaceDirectory = entry.workspace ?? ''
    const executionHostId = entry.host ?? ''
    const id = previous.find(server => server.name === name && server.workspaceDirectory === workspaceDirectory && server.executionHostId === executionHostId)?.id ?? crypto.randomUUID()
    const base = { id, name, workspaceDirectory, executionHostId, enabled: entry.enabled !== false, command: '', args: [], url: '', environment: [], headers: [] }
    if (entry.type === 'local') {
      if (!Array.isArray(entry.command) || !entry.command.length || entry.command.some(part => typeof part !== 'string')) throw Error(`"${name}" needs "command": ["/path/to/program", "argument", ...].`)
      return [{ ...base, transport: 'stdio', command: entry.command[0], args: entry.command.slice(1), environment: refsFromJson(entry.environment, `"${name}" environment`) }]
    }
    if (typeof entry.url !== 'string') throw Error(`"${name}" needs "url".`)
    return [{ ...base, transport: entry.sse ? 'sse' : 'http', url: entry.url, headers: refsFromJson(entry.headers, `"${name}" headers`) }]
  })
  return { servers, uamEnabled }
}

function problem(server: McpServerConfiguration): string {
  const label = server.name.trim() || 'Unnamed server'
  if (!server.name.trim()) return 'Every server needs a name.'
  if ([BUILTIN, 'uam-control', 'uam-computer'].includes(server.name.trim())) return `"${label}" is reserved for a built-in UAM service.`
  if (server.transport === 'stdio' && !absolutePath(server.command.trim())) return `${label}: program path must be a full path, such as /usr/local/bin/npx.`
  if (server.transport !== 'stdio') {
    try {
      const url = new URL(server.url.trim())
      if (!['http:', 'https:'].includes(url.protocol) || !url.hostname || url.username || url.password) throw Error()
    } catch { return `${label}: enter an http:// or https:// URL, such as http://main.homelab.com:9001/mcp.` }
  }
  if (server.workspaceDirectory.trim() && !absolutePath(server.workspaceDirectory.trim())) return `${label}: workspace must be a full path, or empty for all workspaces.`
  const refs = server.transport === 'stdio' ? server.environment : server.headers
  if (refs.some(ref => !ref.name.trim() || (ref.value !== undefined ? !ref.value || /[\r\n]/.test(ref.value) : !/^[A-Za-z_][A-Za-z0-9_]*$/.test(ref.environmentVariable.trim())))) return `${label}: each variable needs a name and a value (or a valid source variable).`
  return ''
}

const newServer = (name: string): McpServerConfiguration => ({ id: crypto.randomUUID(), name, workspaceDirectory: '', executionHostId: '', transport: 'stdio', command: '', args: [], url: '', environment: [], headers: [], enabled: true })

/** One list of MCP servers, edited as a form or as JSON, sent to every provider on launch (local and SSH). */
export function McpServerSetup({ onBusyChange, onDirtyChange }: { onBusyChange: (busy: boolean) => void; onDirtyChange: (dirty: boolean) => void }) {
  const savedServers = useAppStore(state => state.mcpServers)
  const savedUam = useAppStore(state => state.centralProviderConfiguration.uamControlEnabled)
  const hosts = useAppStore(state => state.executionHosts)
  const active = useAppStore(state => state.sessions.find(session => session.id === state.activeSessionId))
  const [servers, setServers] = useState(savedServers)
  const [uamEnabled, setUamEnabled] = useState(savedUam)
  const [view, setView] = useState<'form' | 'json'>('form')
  const [jsonText, setJsonText] = useState('')
  const [open, setOpen] = useState<string | null>(null)
  const [message, setMessage] = useState('')
  const [busy, setBusy] = useState(false)
  const dirty = view === 'json' ? jsonText !== toJsonText(savedServers, savedUam) : toJsonText(servers, uamEnabled) !== toJsonText(savedServers, savedUam)
  useEffect(() => onDirtyChange(dirty), [dirty, onDirtyChange])
  useEffect(() => { if (!dirty) { setServers(savedServers); setUamEnabled(savedUam) } }, [savedServers, savedUam])

  const update = (id: string, change: Partial<McpServerConfiguration>) => { setServers(list => list.map(server => server.id === id ? { ...server, ...change } : server)); setMessage('') }
  const switchView = (next: 'form' | 'json') => {
    if (next === view) return
    if (next === 'json') { setJsonText(toJsonText(servers, uamEnabled)); setView('json'); return }
    try { const parsed = fromJsonText(jsonText, servers); setServers(parsed.servers); setUamEnabled(parsed.uamEnabled); setView('form'); setMessage('') }
    catch (error) { setMessage((error as Error).message) }
  }
  const save = async () => {
    let next = servers, nextUam = uamEnabled
    if (view === 'json') {
      try { ({ servers: next, uamEnabled: nextUam } = fromJsonText(jsonText, servers)) } catch (error) { setMessage((error as Error).message); return }
    }
    next = next.map(server => ({ ...server, name: server.name.trim(), command: server.command.trim(), url: server.url.trim(), workspaceDirectory: server.workspaceDirectory.trim(), args: server.args.filter(arg => arg.length > 0) }))
    const issue = next.map(problem).find(Boolean)
    if (issue) { setMessage(issue); return }
    const keys = next.map(server => `${server.name.toLowerCase()}|${server.workspaceDirectory}|${server.executionHostId}`)
    if (new Set(keys).size !== keys.length) { setMessage('Two servers share the same name. Rename one.'); return }
    setBusy(true); onBusyChange(true); setMessage('')
    try {
      const store = useAppStore.getState()
      const result = await store.setMcpServers(next)
      if (!result.ok) { setMessage(result.error || 'Could not save MCP servers.'); return }
      if (nextUam !== store.centralProviderConfiguration.uamControlEnabled) {
        const central = await store.setCentralProviderConfiguration({ ...store.centralProviderConfiguration, uamControlEnabled: nextUam })
        if (!central.ok) { setMessage(central.error || 'Servers saved, but the UAM MCP setting was not.'); return }
      }
      setServers(next); setUamEnabled(nextUam)
      if (view === 'json') setJsonText(toJsonText(next, nextUam))
      setMessage('Saved. Gemini, Codex, Claude Code, OpenCode and Copilot get these servers the next time a chat or CLI starts, locally and over SSH.')
    } catch { setMessage('Could not save MCP servers. Try again.') }
    finally { setBusy(false); onBusyChange(false) }
  }

  const text = (label: string, value: string, onChange: (value: string) => void, placeholder = '', help = '') => <label className="grid gap-1 text-sm">
    <span>{label}</span>
    <input className="uam-field w-full" value={value} placeholder={placeholder} disabled={busy} onChange={event => onChange(event.currentTarget.value)} />
    {help && <span className="text-xs" style={{ color: 'var(--text-3)' }}>{help}</span>}
  </label>
  const toggle = (label: string, checked: boolean, onChange: (checked: boolean) => void, disabled = false) => <Switch label={label} hideLabel checked={checked} disabled={busy || disabled} onChange={event => onChange(event.currentTarget.checked)} />
  const builtinRow = (name: string, detail: string, control: ReactNode) => <div className="uam-settings-row flex items-center gap-3 text-sm">
    <div className="flex-1 min-w-0"><strong>{name}</strong> <span className="text-xs" style={{ color: 'var(--text-3)' }}>Built in · cannot be deleted</span><p className="text-xs" style={{ color: 'var(--text-3)' }}>{detail}</p></div>{control}
  </div>

  return <div className="space-y-4" style={{ color: 'var(--text)' }}>
    <div className="flex items-center justify-between gap-3">
      <p className="text-sm">Add a server once. Every provider gets it, locally and over SSH.</p>
      <div role="tablist" aria-label="Editor view" className="flex gap-1">
        <Button size="sm" role="tab" aria-selected={view === 'form'} variant={view === 'form' ? 'primary' : 'secondary'} disabled={busy} onClick={() => switchView('form')}>Form</Button>
        <Button size="sm" role="tab" aria-selected={view === 'json'} variant={view === 'json' ? 'primary' : 'secondary'} disabled={busy} onClick={() => switchView('json')}>JSON</Button>
      </div>
    </div>

    {view === 'json' ? <div className="grid gap-2">
      <textarea aria-label="MCP JSON" className="w-full resize-y rounded-lg px-3 py-2 font-mono text-xs" rows={20} spellCheck={false} value={jsonText} disabled={busy} onChange={event => { setJsonText(event.currentTarget.value); setMessage('') }} style={{ color: 'var(--text)', background: 'var(--bg)', border: '1px solid var(--border)' }} />
      <p className="text-xs" style={{ color: 'var(--text-3)' }}>Same layout as OpenCode. <code>local</code> runs a program: <code>command</code> is the program path followed by its arguments. <code>remote</code> connects to a <code>url</code>. Values are plain text, or <code>{'"{env:VARIABLE}"'}</code> to read from the machine's environment. Optional <code>workspace</code> and <code>host</code> limit where a server is used. <code>uam-mcp</code> is built in: only <code>enabled</code> can change.</p>
    </div> : <div className="grid gap-2">
      {builtinRow('UAM MCP service', 'Lets agents ask you questions, manage goals and use UAM skills. Applies to new chats.', toggle('UAM MCP service enabled', uamEnabled, value => { setUamEnabled(value); setMessage('') }))}
      {builtinRow('UAM Computer Use', active ? `Lets agents see and control apps you allow. Off by default; switched per chat. Current chat: ${active.name}.` : 'Lets agents see and control apps you allow. Off by default; open a chat to switch it on.',
        toggle('UAM Computer Use for current chat', Boolean(active?.computerUseEnabled), value => {
          if (!active) return
          void useAppStore.getState().setSessionComputerUseEnabled(active.id, value).then(result => setMessage(result.ok ? `Computer Use ${value ? 'on' : 'off'} for ${active.name}.` : result.error || 'Could not change Computer Use.'))
        }, !active))}

      {servers.map(server => {
        const expanded = open === server.id
        const refs = server.transport === 'stdio' ? server.environment : server.headers
        const setRefs = (values: Reference[]) => update(server.id, server.transport === 'stdio' ? { environment: values } : { headers: values })
        return <div key={server.id} className="rounded-lg" style={{ border: '1px solid var(--border)' }}>
          <div className="flex items-center gap-3 px-3 py-2 text-sm">
            <button type="button" className="flex flex-1 min-w-0 items-center gap-2 text-left" aria-expanded={expanded} onClick={() => setOpen(expanded ? null : server.id)}>
              <ChevronRight size={14} aria-hidden className="shrink-0" style={{ color: 'var(--text-3)', transform: expanded ? 'rotate(90deg)' : undefined, transition: 'transform var(--dur-base) var(--ease-out-soft)' }} />
              <strong className="shrink-0">{server.name || 'New server'}</strong>
              <span className="truncate text-xs" style={{ color: 'var(--text-3)' }}>{server.transport === 'stdio' ? [server.command, ...server.args].join(' ') || 'Program not set' : server.url || 'URL not set'}</span>
            </button>
            {toggle(`${server.name || 'Server'} enabled`, server.enabled, enabled => update(server.id, { enabled }))}
            <Button size="sm" variant="ghost" disabled={busy} aria-label={`Delete ${server.name || 'server'}`} onClick={() => { setServers(list => list.filter(item => item.id !== server.id)); setMessage('') }}>Delete</Button>
          </div>
          {expanded && <div className="grid gap-3 px-3 pb-3">
            {text('Name', server.name, name => update(server.id, { name }), 'playwright', 'Short name shown to the agent.')}
            <div className="grid gap-1 text-sm"><span>Connection type</span>
              <MenuSelect label="Connection type" value={server.transport} disabled={busy} onChange={transport => update(server.id, { transport: transport as McpServerConfiguration['transport'] })} options={[
                { value: 'stdio', label: 'Local program', description: 'Runs on the same computer as the agent' },
                { value: 'http', label: 'Remote URL (HTTP)' },
                { value: 'sse', label: 'Remote URL (SSE)', description: 'Older servers' },
              ]} />
            </div>
            {server.transport === 'stdio' ? <>
              {text('Program path', server.command, command => update(server.id, { command }), '/usr/local/bin/npx', 'Full path to the program. On SSH this is the path on the remote machine.')}
              <div className="grid gap-1 text-sm"><span>Arguments</span>
                {server.args.map((arg, index) => <div key={index} className="flex gap-2">
                  <input aria-label={`Argument ${index + 1}`} className="uam-field flex-1" value={arg} disabled={busy} onChange={event => { const value = event.currentTarget.value; update(server.id, { args: server.args.map((item, i) => i === index ? value : item) }) }} />
                  <IconButton size="sm" icon={<X size={13} />} disabled={busy} label={`Remove argument ${index + 1}`} onClick={() => update(server.id, { args: server.args.filter((_, i) => i !== index) })} />
                </div>)}
                <Button size="sm" className="justify-self-start" leadingIcon={<Plus size={13} aria-hidden />} disabled={busy} onClick={() => update(server.id, { args: [...server.args, ''] })}>Add argument</Button>
                <span className="text-xs" style={{ color: 'var(--text-3)' }}>One value per box, no quotes. Example: <code>-y</code> then <code>@playwright/mcp@latest</code>.</span>
              </div>
            </> : text('URL', server.url, url => update(server.id, { url }), 'http://main.homelab.com:9001/mcp', 'Any http:// or https:// address. On SSH, localhost means the remote machine.')}
            <div className="grid gap-1 text-sm"><span>{server.transport === 'stdio' ? 'Environment variables' : 'Headers'}</span>
              {refs.map((ref, index) => <div key={index} className="flex gap-2 items-center">
                <input aria-label={`Variable name ${index + 1}`} className="uam-field flex-1" placeholder={server.transport === 'stdio' ? 'API_TOKEN' : 'Authorization'} value={ref.name} disabled={busy} onChange={event => { const name = event.currentTarget.value; setRefs(refs.map((item, i) => i === index ? { ...item, name } : item)) }} />
                <div style={{ width: 150 }}><MenuSelect label={`Value source ${index + 1}`} value={ref.value !== undefined ? 'value' : 'variable'} disabled={busy} onChange={source => { const literal = source === 'value'; setRefs(refs.map((item, i) => i === index ? { name: item.name, environmentVariable: '', ...(literal ? { value: '' } : {}) } : item)) }} options={[{ value: 'value', label: '= typed value' }, { value: 'variable', label: 'from variable' }]} /></div>
                {ref.value !== undefined
                  ? <input aria-label={`Value ${index + 1}`} className="uam-field flex-1" placeholder="http://main.homelab.com:8081" value={ref.value} disabled={busy} onChange={event => { const value = event.currentTarget.value; setRefs(refs.map((item, i) => i === index ? { ...item, value } : item)) }} />
                  : <input aria-label={`Source variable ${index + 1}`} className="uam-field flex-1" placeholder="MY_TOKEN" value={ref.environmentVariable} disabled={busy} onChange={event => { const environmentVariable = event.currentTarget.value; setRefs(refs.map((item, i) => i === index ? { ...item, environmentVariable } : item)) }} />}
                <IconButton size="sm" icon={<X size={13} />} disabled={busy} label={`Remove variable ${index + 1}`} onClick={() => setRefs(refs.filter((_, i) => i !== index))} />
              </div>)}
              <Button size="sm" className="justify-self-start" leadingIcon={<Plus size={13} aria-hidden />} disabled={busy} onClick={() => setRefs([...refs, { name: '', environmentVariable: '', value: '' }])}>Add {server.transport === 'stdio' ? 'variable' : 'header'}</Button>
              <span className="text-xs" style={{ color: 'var(--text-3)' }}>Type a value, or choose "from variable" to read it from that machine's environment. Typed values are saved in UAM settings as plain text.</span>
            </div>
            <details className="uam-skill-folder"><summary className="uam-skill-folder__summary text-sm"><ChevronRight size={14} aria-hidden className="uam-skill-folder__chevron" />Limit where it runs (optional)</summary>
              <div className="grid gap-3 pt-2">
                {text('Only in this workspace', server.workspaceDirectory, workspaceDirectory => update(server.id, { workspaceDirectory }), 'Empty = every workspace')}
                <div className="grid gap-1 text-sm"><span>Only on this machine</span>
                  <MenuSelect label="Only on this machine" value={server.executionHostId ?? ''} disabled={busy} onChange={executionHostId => update(server.id, { executionHostId })} options={[
                    { value: '', label: 'Every machine, including SSH' },
                    ...hosts.map(host => ({ value: host.id, label: host.label })),
                    ...(server.executionHostId && !hosts.some(host => host.id === server.executionHostId) ? [{ value: server.executionHostId, label: `${server.executionHostId} (unavailable)` }] : []),
                  ]} />
                </div>
              </div>
            </details>
          </div>}
        </div>
      })}
      <Button size="sm" className="justify-self-start" leadingIcon={<Plus size={13} aria-hidden />} disabled={busy} onClick={() => { const server = newServer(''); setServers(list => [...list, server]); setOpen(server.id); setMessage('') }}>Add server</Button>
    </div>}

    <div className="flex items-center gap-2">
      <Button size="sm" variant={dirty ? 'primary' : 'secondary'} loading={busy} disabled={!dirty} onClick={() => void save()}>Save</Button>
      {dirty && <Button size="sm" disabled={busy} onClick={() => { setServers(savedServers); setUamEnabled(savedUam); setJsonText(toJsonText(savedServers, savedUam)); setMessage('') }}>Undo changes</Button>}
      <span className="text-xs" style={{ color: 'var(--text-3)' }}>Running chats keep their old setup until restarted.</span>
    </div>
    {message && <p role="status" className="text-sm">{message}</p>}
  </div>
}
