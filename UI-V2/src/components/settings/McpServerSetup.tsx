import { useEffect, useRef, useState, type ReactNode } from 'react'
import { useAppStore, type McpServerConfiguration } from '../../store/useAppStore'
import { COMPUTER_USE_ENABLED } from '../../config/buildFeatures'
import { sendToCEF } from '../../ipc/cefBridge'
import { Button } from '../ui'

type Reference = McpServerConfiguration['environment'][number]
type JsonEntry = { type: 'builtin' | 'local' | 'remote'; name?: string; uamId?: string; enabled?: boolean; command?: string[]; environment?: Record<string, string>; url?: string; headers?: Record<string, string>; sse?: boolean; workspace?: string; host?: string }
const BUILTIN = 'uam-mcp'
const hasSubstitution = (value: string) => /\{(?:env|file):/.test(value)
const absolutePath = (value: string) => /^(\/|[a-z]:[\\/]|\\\\)/i.test(value)
const refsToJson = (refs: Reference[]) => refs.length ? Object.fromEntries(refs.map(ref => [ref.name, ref.value !== undefined ? ref.value : `{env:${ref.environmentVariable}}`])) : undefined
const refsFromJson = (value: unknown, label: string): Reference[] => {
  if (value === undefined) return []
  if (!value || typeof value !== 'object' || Array.isArray(value)) throw Error(`${label} must be an object of "NAME": "value" or "NAME": "{env:VARIABLE}".`)
  return Object.entries(value).map(([name, text]) => {
    const match = typeof text === 'string' ? /^\{env:([A-Za-z_][A-Za-z0-9_]*)\}$/.exec(text) : null
    if (typeof text !== 'string') throw Error(`${label} "${name}" must be text.`)
    if (!match && hasSubstitution(text)) throw Error(`${label}: substitutions need manual configuration. Use a whole "{env:VARIABLE}" reference or enter a literal value.`)
    return match ? { name, environmentVariable: match[1] } : { name, environmentVariable: '', value: text }
  })
}

/** OpenCode-style editor document. uamId retains local server identity across name and scope edits. */
export function toJsonText(servers: McpServerConfiguration[], uamEnabled: boolean) {
  const mcp: Record<string, JsonEntry> = Object.assign(Object.create(null), { [BUILTIN]: { type: 'builtin', enabled: uamEnabled } })
  for (const server of servers) {
    let key = server.name
    let suffix = 2
    while (Object.prototype.hasOwnProperty.call(mcp, key)) key = `${server.name} (${suffix++})`
    mcp[key] = {
    uamId: server.id,
    ...(key !== server.name ? { name: server.name } : {}),
    type: server.transport === 'stdio' ? 'local' : 'remote',
    ...(server.transport === 'stdio' ? { command: [server.command, ...server.args], environment: refsToJson(server.environment) } : { url: server.url, headers: refsToJson(server.headers), sse: server.transport === 'sse' || undefined }),
    enabled: server.enabled,
    workspace: server.workspaceDirectory || undefined,
    host: server.executionHostId || undefined,
    }
  }
  return JSON.stringify({ mcp }, null, 2)
}

export function fromJsonText(text: string, previous: McpServerConfiguration[]): { servers: McpServerConfiguration[]; uamEnabled: boolean } {
  let parsed: unknown
  try { parsed = JSON.parse(text) } catch { throw Error('The JSON is not valid. Check for a missing comma or quote.') }
  const mcp = (parsed as { mcp?: unknown })?.mcp
  if (!mcp || typeof mcp !== 'object' || Array.isArray(mcp)) throw Error('Put servers inside { "mcp": { ... } }.')
  let uamEnabled = true
  const editorIds = new Set<string>()
  const serverIds = new Set<string>()
  const servers = Object.entries(mcp as Record<string, JsonEntry>).flatMap(([key, entry]): McpServerConfiguration[] => {
    const name = entry?.name ?? key
    if (typeof name !== 'string') throw Error('Server names must be text.')
    if (!entry || typeof entry !== 'object' || Array.isArray(entry)) throw Error(`"${name}" must be an object.`)
    for (const field of ['workspace', 'host'] as const) {
      if (entry[field] !== undefined && typeof entry[field] !== 'string') throw Error(`"${name}": ${field} must be text.`)
    }
    for (const field of ['enabled', 'sse'] as const) {
      if (entry[field] !== undefined && typeof entry[field] !== 'boolean') throw Error(`"${name}": ${field} must be boolean.`)
    }
    if (key === BUILTIN) {
      if (entry.type !== 'builtin') throw Error('"uam-mcp" is reserved for the built-in UAM service. Rename the external server before importing.')
      uamEnabled = entry.enabled !== false
      return []
    }
    if (entry.uamId !== undefined) {
      if (typeof entry.uamId !== 'string' || !/^[^\s\x00-\x1f\x7f]{1,128}$/.test(entry.uamId)) throw Error(`"${name}": uamId must be a valid server identifier.`)
      if (editorIds.has(entry.uamId)) throw Error(`"${name}": duplicate uamId. Each server needs its own editor identifier.`)
      editorIds.add(entry.uamId)
    }
    if (entry.type !== 'local' && entry.type !== 'remote') throw Error(`"${name}" needs "type": "local" or "remote".`)
    for (const field of ['oauth', 'timeout'] as const) {
      if (field in entry) throw Error(`"${name}": ${field === 'oauth' ? 'OAuth' : field} settings need manual configuration; this editor cannot preserve them.`)
    }
    const workspaceDirectory = entry.workspace ?? ''
    const executionHostId = entry.host ?? ''
    const known = entry.uamId !== undefined ? previous.find(server => server.id === entry.uamId) : undefined
    const id = known?.id ?? previous.find(server => server.name === name && server.workspaceDirectory === workspaceDirectory && server.executionHostId === executionHostId)?.id ?? crypto.randomUUID()
    if (serverIds.has(id)) throw Error(`"${name}": duplicate server identity. Each server needs its own editor identifier.`)
    serverIds.add(id)
    const base = { id, name, workspaceDirectory, executionHostId, enabled: entry.enabled !== false, command: '', args: [], url: '', environment: [], headers: [] }
    if (entry.type === 'local') {
      if (!Array.isArray(entry.command) || !entry.command.length || entry.command.some(part => typeof part !== 'string')) throw Error(`"${name}" needs "command": ["/path/to/program", "argument", ...].`)
      if (entry.command.some(hasSubstitution)) throw Error(`"${name}": command substitutions need manual configuration. Enter the program path and literal arguments.`)
      return [{ ...base, transport: 'stdio', command: entry.command[0], args: entry.command.slice(1), environment: refsFromJson(entry.environment, `"${name}" environment`) }]
    }
    if (typeof entry.url !== 'string') throw Error(`"${name}" needs "url".`)
    if (hasSubstitution(entry.url)) throw Error(`"${name}": URL substitutions need manual configuration. Enter a literal URL.`)
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

/** Import adds definitions without replacing existing servers or changing UAM's built-in service. */
export function mergeMcpImport(text: string, previous: McpServerConfiguration[]): McpServerConfiguration[] {
  // Imported metadata cannot adopt an existing server or its provider overrides.
  const incoming = fromJsonText(text, []).servers.map(server => ({ ...server, name: server.name.trim() }))
  const keys = new Set(previous.map(server => `${server.name.trim().toLowerCase()}|${server.workspaceDirectory}|${server.executionHostId}`))
  for (const server of incoming) {
    const issue = problem(server)
    if (issue) throw Error(issue)
    const key = `${server.name.toLowerCase()}|${server.workspaceDirectory}|${server.executionHostId}`
    if (keys.has(key)) throw Error(`A server named "${server.name}" already exists for this workspace and machine. Rename it before importing.`)
    keys.add(key)
  }
  if (!incoming.length) throw Error('This configuration contains no servers to import.')
  return [...previous, ...incoming]
}

/** One list of MCP servers, edited as a form or as JSON, sent to every provider on launch (local and SSH). */
export function McpServerSetup({ onBusyChange, onDirtyChange }: { onBusyChange: (busy: boolean) => void; onDirtyChange: (dirty: boolean) => void }) {
  const savedServers = useAppStore(state => state.mcpServers)
  const savedUam = useAppStore(state => state.centralProviderConfiguration.uamControlEnabled)
  const hosts = useAppStore(state => state.executionHosts)
  const [servers, setServers] = useState(savedServers)
  const [uamEnabled, setUamEnabled] = useState(savedUam)
  const [view, setView] = useState<'form' | 'json'>('form')
  const [jsonText, setJsonText] = useState('')
  const [open, setOpen] = useState<string | null>(null)
  const [message, setMessage] = useState('')
  const [busy, setBusy] = useState(false)
  const [setup, setSetup] = useState<'choose' | 'import' | 'guided' | null>(null)
  const [importText, setImportText] = useState('')
  const [guidedName, setGuidedName] = useState('')
  const [guidedTransport, setGuidedTransport] = useState<McpServerConfiguration['transport']>('stdio')
  const [importPath, setImportPath] = useState('')
  const [importSources, setImportSources] = useState<string[]>([])
  const [importWarnings, setImportWarnings] = useState<string[]>([])
  const importGeneration = useRef(0)
  useEffect(() => () => { importGeneration.current++ }, [])
  const dirty = view === 'json' ? jsonText !== toJsonText(savedServers, savedUam) : toJsonText(servers, uamEnabled) !== toJsonText(savedServers, savedUam)
  const setupDirty = setup === 'import' ? Boolean(importText) : setup === 'guided' && Boolean(guidedName)
  useEffect(() => onDirtyChange(dirty || setupDirty), [dirty, setupDirty, onDirtyChange])
  useEffect(() => { if (!dirty) { setServers(savedServers); setUamEnabled(savedUam) } }, [savedServers, savedUam])

  const update = (id: string, change: Partial<McpServerConfiguration>) => { setServers(list => list.map(server => server.id === id ? { ...server, ...change } : server)); setMessage('') }
  const switchView = (next: 'form' | 'json') => {
    if (next === view) return
    if (next === 'json') { setJsonText(toJsonText(servers, uamEnabled)); setView('json'); return }
    try { const parsed = fromJsonText(jsonText, servers); setServers(parsed.servers); setUamEnabled(parsed.uamEnabled); setView('form'); setMessage('') }
    catch (error) { setMessage((error as Error).message) }
  }
  const readOpenCode = async () => {
    const generation = ++importGeneration.current
    setBusy(true); onBusyChange(true); setMessage(''); setImportText(''); setImportSources([]); setImportWarnings([])
    try {
      const response = await sendToCEF<{ servers: McpServerConfiguration[]; warnings: string[]; sources: string[] }>({ action: 'previewOpenCodeMcpImport', payload: { executionHostId: 'local', ...(importPath.trim() ? { sourcePath: importPath.trim() } : {}) } })
      if (generation !== importGeneration.current) return
      if (!response.ok || !response.data) { setMessage(response.error || 'Could not read OpenCode configuration.'); return }
      setImportSources(response.data.sources)
      setImportWarnings(response.data.warnings)
      setImportText(response.data.servers.length ? toJsonText(response.data.servers, uamEnabled) : '')
      setMessage(response.data.servers.length ? `Loaded ${response.data.servers.length} server${response.data.servers.length === 1 ? '' : 's'}. Review the configuration below before importing.` : 'No external MCP servers found in the configuration.')
    } catch { if (generation === importGeneration.current) setMessage('Could not read OpenCode configuration. Try again.') }
    finally { if (generation === importGeneration.current) { setBusy(false); onBusyChange(false) } }
  }
  const save = async () => {
    if (setup !== null) { setMessage('Finish or cancel setup before saving.'); return }
    let next = servers, nextUam = uamEnabled
    if (view === 'json') {
      try { ({ servers: next, uamEnabled: nextUam } = fromJsonText(jsonText, servers)) } catch (error) { setMessage((error as Error).message); return }
    }
    next = next.map(server => ({ ...server, name: server.name.trim(), command: server.command.trim(), url: server.url.trim(), workspaceDirectory: server.workspaceDirectory.trim() }))
    const issue = next.map(problem).find(Boolean)
    if (issue) { setMessage(issue); return }
    const keys = next.map(server => `${server.name.toLowerCase()}|${server.workspaceDirectory}|${server.executionHostId}`)
    if (new Set(keys).size !== keys.length) { setMessage('Two servers share the same name. Rename one.'); return }
    setBusy(true); onBusyChange(true); setMessage('')
    try {
      const store = useAppStore.getState()
      const result = await store.setMcpServers(next)
      if (!result.ok) { setMessage(result.error || 'Could not save MCP servers.'); return }
      const currentStore = useAppStore.getState()
      if (nextUam !== currentStore.centralProviderConfiguration.uamControlEnabled) {
        const central = await currentStore.setCentralProviderConfiguration({ ...currentStore.centralProviderConfiguration, uamControlEnabled: nextUam })
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
  const toggle = (label: string, checked: boolean, onChange: (checked: boolean) => void, disabled = false) => <label className="flex items-center gap-2 text-xs"><input type="checkbox" aria-label={label} checked={checked} disabled={busy || disabled} onChange={event => onChange(event.currentTarget.checked)} />{checked ? 'On' : 'Off'}</label>
  const builtinRow = (name: string, detail: string, control: ReactNode) => <div className="uam-settings-row flex items-center gap-3 text-sm">
    <div className="flex-1 min-w-0"><strong>{name}</strong> <span className="text-xs" style={{ color: 'var(--text-3)' }}>Built in · cannot be deleted</span><p className="text-xs" style={{ color: 'var(--text-3)' }}>{detail}</p></div>{control}
  </div>

  const warningNotice = importWarnings.length > 0 ? <p role="status" aria-label="Import warnings" className="text-sm">{importWarnings.join(' ')}</p> : null

  return <div className="space-y-4" style={{ color: 'var(--text)' }}>
    {setup && <section aria-label="MCP setup" className="grid gap-3 border-b pb-4" style={{ borderColor: 'var(--border)' }}>
      <div className="flex items-center justify-between"><strong>{setup === 'choose' ? 'How would you like to set up this server?' : setup === 'import' ? 'Import MCP configuration' : 'Step 1 of 2: name and connection'}</strong><Button size="sm" disabled={busy} onClick={() => { setSetup(null); setMessage(''); setImportWarnings([]) }}>Cancel setup</Button></div>
      {setup === 'choose' && <div className="flex flex-wrap gap-2">
        <Button size="sm" onClick={() => { setSetup('import'); setImportText(''); setImportSources([]); setMessage('') }}>Import configuration</Button>
        <Button size="sm" onClick={() => { setSetup('guided'); setGuidedName(''); setGuidedTransport('stdio') }}>Step by step</Button>
        <Button size="sm" onClick={() => { const server = newServer(''); setServers(list => [...list, server]); setOpen(server.id); setSetup(null) }}>Manual form</Button>
        <Button size="sm" onClick={() => { switchView('json'); setSetup(null) }}>Edit JSON</Button>
      </div>}
      {setup === 'import' && <>
        <p className="text-sm">Read OpenCode's global configuration, or paste its MCP section. Existing servers stay unchanged.</p>
        {text('OpenCode configuration file or directory (optional)', importPath, setImportPath, 'Empty: read global OpenCode configuration')}
        <Button size="sm" className="self-start" disabled={busy} loading={busy} onClick={() => void readOpenCode()}>Read this computer's OpenCode configuration</Button>
        <textarea aria-label="Import MCP configuration" className="uam-field w-full resize-y font-mono text-xs" style={{ height: 'auto', padding: '8px 9px' }} rows={8} disabled={busy} value={importText} onChange={event => { setImportText(event.currentTarget.value); setImportSources([]); setMessage('') }} />
        {importSources.length > 0 && <div className="text-xs break-all">{importSources.map(source => <div key={source}>{source}</div>)}</div>}
        {message && <p role="status" className="text-sm">{message}</p>}
        {warningNotice}
        <Button size="sm" className="self-start" disabled={busy || !importText.trim()} onClick={() => { try { setServers(mergeMcpImport(importText, servers)); setSetup(null); setMessage('Imported into the draft. Review the servers, then Save.'); } catch (error) { setMessage(error instanceof Error ? error.message : 'Could not import configuration.') } }}>Review import</Button>
      </>}
      {setup === 'guided' && <>
        {text('Server name', guidedName, setGuidedName, 'playwright')}
        <label className="grid gap-1 text-sm"><span>How does this server connect?</span><select aria-label="Setup connection type" className="uam-field" value={guidedTransport} onChange={event => setGuidedTransport(event.currentTarget.value as McpServerConfiguration['transport'])}><option value="stdio">Run a program on the agent's machine</option><option value="http">Connect to an HTTP URL</option><option value="sse">Connect to an SSE URL</option></select></label>
        <Button size="sm" className="self-start" disabled={!guidedName.trim()} onClick={() => { const server = { ...newServer(guidedName.trim()), transport: guidedTransport }; setServers(list => [...list, server]); setOpen(server.id); setSetup(null); setMessage('Step 2 of 2: enter the connection details below, then Save.'); }}>Next: connection details</Button>
      </>}
    </section>}
    <div className="flex items-center justify-between gap-3">
      <p className="text-sm">Manage shared MCP servers for supported providers, locally and over SSH.</p>
      <div role="tablist" aria-label="Editor view" className="flex gap-1">
        <Button size="sm" role="tab" aria-selected={view === 'form'} variant={view === 'form' ? 'primary' : 'secondary'} disabled={busy || setup !== null} onClick={() => switchView('form')}>Form</Button>
        <Button size="sm" role="tab" aria-selected={view === 'json'} variant={view === 'json' ? 'primary' : 'secondary'} disabled={busy || setup !== null} onClick={() => switchView('json')}>JSON</Button>
      </div>
    </div>

    {view === 'json' ? <div className="grid gap-2">
      <textarea aria-label="MCP JSON" className="w-full resize-y rounded-lg px-3 py-2 font-mono text-xs" rows={20} spellCheck={false} value={jsonText} disabled={busy} onChange={event => { setJsonText(event.currentTarget.value); setMessage('') }} style={{ color: 'var(--text)', background: 'var(--bg)', border: '1px solid var(--border)' }} />
      <p className="text-xs" style={{ color: 'var(--text-3)' }}>Same layout as OpenCode. <code>local</code> runs a program: <code>command</code> is the program path followed by its arguments. <code>remote</code> connects to a <code>url</code>. Values are plain text, or <code>{'"{env:VARIABLE}"'}</code> to read from the machine's environment. Optional <code>workspace</code> and <code>host</code> limit where a server is used. <code>uam-mcp</code> is built in: only <code>enabled</code> can change.</p>
    </div> : <div className="grid gap-2">
      {builtinRow('UAM MCP service', 'Lets agents ask you questions, manage goals and use UAM skills. Applies to new chats.', toggle('UAM MCP service enabled', uamEnabled, value => { setUamEnabled(value); setMessage('') }))}
      {builtinRow('UAM Computer Use', 'Built-in MCP server. Control chat permissions in the chat composer.',
        <span className="text-xs">{COMPUTER_USE_ENABLED ? 'Available in this build' : 'Unavailable in this build'}</span>)}

      {servers.map(server => {
        const expanded = open === server.id
        const refs = server.transport === 'stdio' ? server.environment : server.headers
        const setRefs = (values: Reference[]) => update(server.id, server.transport === 'stdio' ? { environment: values } : { headers: values })
        return <div key={server.id} className="rounded-lg" style={{ border: '1px solid var(--border)' }}>
          <div className="flex items-center gap-3 px-3 py-2 text-sm">
            <button type="button" className="flex-1 min-w-0 text-left" aria-expanded={expanded} onClick={() => setOpen(expanded ? null : server.id)}>
              <strong>{expanded ? '▾' : '▸'} {server.name || 'New server'}</strong>
              <span className="ml-2 text-xs break-all" style={{ color: 'var(--text-3)' }}>{server.transport === 'stdio' ? [server.command, ...server.args].join(' ') || 'Program not set' : server.url || 'URL not set'}</span>
            </button>
            {toggle(`${server.name || 'Server'} enabled`, server.enabled, enabled => update(server.id, { enabled }))}
            <Button size="sm" variant="danger" disabled={busy} aria-label={`Delete ${server.name || 'server'}`} onClick={() => { setServers(list => list.filter(item => item.id !== server.id)); setMessage('') }}>Delete</Button>
          </div>
          {expanded && <div className="grid gap-3 px-3 pb-3">
            {text('Name', server.name, name => update(server.id, { name }), 'playwright', 'Short name shown to the agent.')}
            <label className="grid gap-1 text-sm"><span>Connection type</span>
              <select className="uam-field w-full" value={server.transport} disabled={busy} onChange={event => update(server.id, { transport: event.currentTarget.value as McpServerConfiguration['transport'] })}>
                <option value="stdio">Local program (runs on the same computer as the agent)</option>
                <option value="http">Remote URL (HTTP)</option>
                <option value="sse">Remote URL (SSE, older servers)</option>
              </select>
            </label>
            {server.transport === 'stdio' ? <>
              {text('Program path', server.command, command => update(server.id, { command }), '/usr/local/bin/npx', 'Full path to the program. On SSH this is the path on the remote machine.')}
              <div className="grid gap-1 text-sm"><span>Arguments</span>
                {server.args.map((arg, index) => <div key={index} className="flex gap-2">
                  <input aria-label={`Argument ${index + 1}`} className="uam-field flex-1" value={arg} disabled={busy} onChange={event => { const value = event.currentTarget.value; update(server.id, { args: server.args.map((item, i) => i === index ? value : item) }) }} />
                  <Button size="sm" disabled={busy} aria-label={`Remove argument ${index + 1}`} onClick={() => update(server.id, { args: server.args.filter((_, i) => i !== index) })}>−</Button>
                </div>)}
                <Button size="sm" className="self-start" disabled={busy} onClick={() => update(server.id, { args: [...server.args, ''] })}>+ Add argument</Button>
                <span className="text-xs" style={{ color: 'var(--text-3)' }}>One value per box, no quotes. Example: <code>-y</code> then <code>@playwright/mcp@latest</code>.</span>
              </div>
            </> : text('URL', server.url, url => update(server.id, { url }), 'http://main.homelab.com:9001/mcp', 'Any http:// or https:// address. On SSH, localhost means the remote machine.')}
            <div className="grid gap-1 text-sm"><span>{server.transport === 'stdio' ? 'Environment variables' : 'Headers'}</span>
              {refs.map((ref, index) => <div key={index} className="flex gap-2 items-center">
                <input aria-label={`Variable name ${index + 1}`} className="uam-field flex-1" placeholder={server.transport === 'stdio' ? 'API_TOKEN' : 'Authorization'} value={ref.name} disabled={busy} onChange={event => { const name = event.currentTarget.value; setRefs(refs.map((item, i) => i === index ? { ...item, name } : item)) }} />
                <select aria-label={`Value source ${index + 1}`} className="uam-field text-xs" style={{ width: 'auto' }} value={ref.value !== undefined ? 'value' : 'variable'} disabled={busy} onChange={event => { const literal = event.currentTarget.value === 'value'; setRefs(refs.map((item, i) => i === index ? { name: item.name, environmentVariable: '', ...(literal ? { value: '' } : {}) } : item)) }}>
                  <option value="value">=</option><option value="variable">from variable</option>
                </select>
                {ref.value !== undefined
                  ? <input aria-label={`Value ${index + 1}`} className="uam-field flex-1" placeholder="http://main.homelab.com:8081" value={ref.value} disabled={busy} onChange={event => { const value = event.currentTarget.value; setRefs(refs.map((item, i) => i === index ? { ...item, value } : item)) }} />
                  : <input aria-label={`Source variable ${index + 1}`} className="uam-field flex-1" placeholder="MY_TOKEN" value={ref.environmentVariable} disabled={busy} onChange={event => { const environmentVariable = event.currentTarget.value; setRefs(refs.map((item, i) => i === index ? { ...item, environmentVariable } : item)) }} />}
                <Button size="sm" disabled={busy} aria-label={`Remove variable ${index + 1}`} onClick={() => setRefs(refs.filter((_, i) => i !== index))}>−</Button>
              </div>)}
              <Button size="sm" className="self-start" disabled={busy} onClick={() => setRefs([...refs, { name: '', environmentVariable: '', value: '' }])}>+ Add {server.transport === 'stdio' ? 'variable' : 'header'}</Button>
              <span className="text-xs" style={{ color: 'var(--text-3)' }}>Type a value, or choose "from variable" to read it from that machine's environment. Typed values are saved in UAM settings as plain text.</span>
            </div>
            <details><summary className="cursor-pointer text-sm">Limit where it runs (optional)</summary>
              <div className="grid gap-3 pt-2">
                {text('Only in this workspace', server.workspaceDirectory, workspaceDirectory => update(server.id, { workspaceDirectory }), 'Empty = every workspace')}
                <label className="grid gap-1 text-sm"><span>Only on this machine</span>
                  <select className="uam-field w-full" value={server.executionHostId} disabled={busy} onChange={event => update(server.id, { executionHostId: event.currentTarget.value })}>
                    <option value="">Every machine, including SSH</option>
                    {hosts.map(host => <option key={host.id} value={host.id}>{host.label}</option>)}
                    {server.executionHostId && !hosts.some(host => host.id === server.executionHostId) && <option value={server.executionHostId}>{server.executionHostId} (unavailable)</option>}
                  </select>
                </label>
              </div>
            </details>
          </div>}
        </div>
      })}
      <Button size="sm" className="self-start" disabled={busy} onClick={() => { setSetup('choose'); setMessage('') }}>+ Add server</Button>
    </div>}

    <div className="flex items-center gap-2">
      <Button size="sm" variant={dirty ? 'primary' : 'secondary'} loading={busy} disabled={!dirty || setup !== null} title={setup !== null ? 'Finish or cancel setup before saving.' : undefined} onClick={() => void save()}>Save</Button>
      {dirty && <Button size="sm" disabled={busy} onClick={() => { setServers(savedServers); setUamEnabled(savedUam); setJsonText(toJsonText(savedServers, savedUam)); setMessage('') }}>Undo changes</Button>}
      <span className="text-xs" style={{ color: 'var(--text-3)' }}>{setup !== null ? 'Finish or cancel setup before saving.' : 'Running chats keep their old setup until restarted.'}</span>
    </div>
    {message && setup !== 'import' && <p role="status" className="text-sm">{message}</p>}
    {setup !== 'import' && warningNotice}
  </div>
}
