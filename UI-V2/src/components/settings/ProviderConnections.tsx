import { useEffect, useRef, useState, type MouseEvent as ReactMouseEvent } from 'react'
import { useShallow } from 'zustand/react/shallow'
import { useAppStore } from '../../store/useAppStore'
import { SUPPORTED_CLI_PROVIDER_IDS, providerMetadataForId } from '../../utils/providerMetadata'
import { ProviderLogo } from '../shared/ProviderLogo'
import { sendToCEF } from '../../ipc/cefBridge'
import { Check, Minus, RefreshCw } from 'lucide-react'
import { Button, IconButton, Notice, StatusDot, ViewportMenu, usePresence, type StatusTone } from '../ui'
import './provider-settings.css'

interface McpStatus {
  executionHostId: string
  connectedProofAvailable: boolean
  handshakeProofAvailable?: boolean
  computerUseBuiltInAvailable?: boolean
  providers: Array<{ providerId: string; status: 'disabled' | 'missing' | 'configured-unverified' | 'failed' | 'handshake-confirmed' | 'connected'; detail: string; activeSessionCount: number; configuredSessionCount: number; uamControlEnabled?: boolean; sharedSetupEnabled?: boolean; mcpServers?: Array<{ id: string; name: string; enabled: boolean }> }>
}

/** More server columns than this collapse into one "MCP servers" column with a popover. */
const MAX_SERVER_COLUMNS = 3

interface Props {
  providerId?: string
  selectedHostId?: string
  onHostChange?: (id: string) => void
  onProviderSelect?: (id: string) => void
  onMcpSelect: () => void
  onPreferencesSelect: () => void
}

/** Provider controls show effective next-launch configuration; connection status needs live evidence. */
export function ProviderConnections({ providerId, selectedHostId, onHostChange, onProviderSelect, onMcpSelect, onPreferencesSelect }: Props) {
  const hosts = useAppStore(useShallow(state => state.executionHosts))
  const runtimeProviders = useAppStore(useShallow(state => state.providers))
  const versions = useAppStore(state => state.cliVersionManager)
  const central = useAppStore(state => state.centralProviderConfiguration)
  const servers = useAppStore(useShallow(state => state.mcpServers))
  const computerUseEnabled = useAppStore(state => state.computerUseEnabled)
  const [ownHostId, setOwnHostId] = useState('local')
  const hostId = selectedHostId ?? ownHostId
  const setHostId = onHostChange ?? setOwnHostId
  const [busy, setBusy] = useState(false)
  /** The one switch whose save is in flight; only it shows the pending state. */
  const [pendingKey, setPendingKey] = useState('')
  const [error, setError] = useState('')
  const [status, setStatus] = useState<McpStatus | null>(null)
  const [statusError, setStatusError] = useState('')
  const [refresh, setRefresh] = useState(0)
  const requestGeneration = useRef(0)
  const busyRef = useRef(false)
  const ids = providerId ? [providerId] : SUPPORTED_CLI_PROVIDER_IDS
  const activeHostId = hosts.some(host => host.id === hostId) || hostId === 'local' ? hostId : 'local'
  const currentHost = useRef(activeHostId)
  currentHost.current = activeHostId
  const hostVersions = activeHostId === 'local' ? versions.providers : (versions.remoteProviders ?? []).filter(version => version.executionHostId === activeHostId)
  const configurationSignature = JSON.stringify([
    central.enabled, central.uamControlEnabled,
    (central.mcpOverrides ?? []).filter(override => override.executionHostId === activeHostId && ids.includes(override.providerId)),
    servers.map(server => [server.id, server.name, server.enabled, server.executionHostId, server.workspaceDirectory]),
    runtimeProviders.map(provider => [provider.id, provider.supportsStructured]),
  ])
  useEffect(() => setError(''), [activeHostId])
  const validStatus = (value: McpStatus | undefined, expectedHost: string): value is McpStatus => value?.executionHostId === expectedHost && Array.isArray(value.providers)
  useEffect(() => {
    const generation = ++requestGeneration.current
    setStatus(previous => previous?.executionHostId === activeHostId ? previous : null); setStatusError('')
    void sendToCEF<McpStatus>({ action: 'getProviderMcpStatus', payload: { executionHostId: activeHostId } }).then(result => {
      if (generation !== requestGeneration.current || currentHost.current !== activeHostId) return
      if (result.ok && validStatus(result.data, activeHostId)) setStatus(result.data)
      else { setStatus(null); setStatusError(result.error || 'Connection checks are unavailable in this build.') }
    }).catch(() => { if (generation === requestGeneration.current) { setStatus(null); setStatusError('Could not check provider MCP setup.') } })
    return () => { requestGeneration.current++ }
  }, [activeHostId, refresh, configurationSignature])
  useEffect(() => {
    const timer = window.setInterval(() => { if (!busyRef.current) setRefresh(value => value + 1) }, 30_000)
    return () => window.clearInterval(timer)
  }, [])
  const enable = async () => {
    setBusy(true); busyRef.current = true; setError('')
    try {
      const store = useAppStore.getState()
      const result = await store.setCentralProviderConfiguration({ ...store.centralProviderConfiguration, uamControlEnabled: true })
      if (result.ok) setRefresh(value => value + 1)
      if (!result.ok) setError(result.error || 'Could not enable UAM MCP.')
    } catch { setError('Could not enable UAM MCP. Try again.') }
    finally { setBusy(false); busyRef.current = false }
  }
  const setConfiguration = async (id: string, serverId: string, enabled: boolean) => {
    const generation = ++requestGeneration.current
    const selectedHost = activeHostId
    setBusy(true); busyRef.current = true; setError(''); setPendingKey(`${id}:${serverId}`)
    try {
      const result = await sendToCEF<McpStatus>({ action: 'setProviderMcpConfiguration', payload: { executionHostId: selectedHost, providerId: id, serverId, enabled } })
      if (generation !== requestGeneration.current || currentHost.current !== selectedHost) return
      const row = result.data?.providers?.find(provider => provider.providerId === id)
      const actual = serverId === 'uam-mcp' ? row?.uamControlEnabled : serverId === 'shared-setup' ? row?.sharedSetupEnabled : row?.mcpServers?.find(server => server.id === serverId)?.enabled
      if (result.ok && validStatus(result.data, selectedHost) && actual === enabled) setStatus(result.data)
      else setError(result.error || 'The provider MCP change could not be verified. Refresh and try again.')
    } catch { if (generation === requestGeneration.current) setError('Could not save provider MCP configuration. Try again.') }
    finally { setBusy(false); busyRef.current = false; setPendingKey('') }
  }
  /** Configuration switch. Unknown (undefined) renders as a dashed, disabled switch. Only the saving switch is pending; the rest stay steady. */
  const checkbox = (label: string, checked: boolean | undefined, onChange?: () => void, pendingId = '') => <button type="button" role="checkbox" className="uam-prov-switch" aria-label={checked === undefined ? `${label} (unknown)` : label} aria-checked={checked === undefined ? 'mixed' : checked} data-pending={pendingId !== '' && pendingKey === pendingId || undefined} disabled={(pendingId !== '' && pendingKey === pendingId) || checked === undefined || !onChange} title={checked === undefined ? 'Not known yet' : checked ? 'On. Click to turn off.' : 'Off. Click to turn on.'} onClick={onChange}/>
  /** Read-only availability glyph: state is shown, not set here. */
  const collapseServers = servers.length > MAX_SERVER_COLUMNS
  const [menuId, setMenuId] = useState<string | null>(null)
  const menuPresence = usePresence(menuId !== null)
  const lastMenuId = useRef<string | null>(null)
  if (menuId) lastMenuId.current = menuId
  const shownMenuId = menuId ?? lastMenuId.current
  const menuAnchor = useRef<HTMLElement | null>(null)
  const menuRef = useRef<HTMLDivElement | null>(null)
  useEffect(() => setMenuId(null), [activeHostId])
  useEffect(() => {
    if (!menuId) return
    const onPointerDown = (event: MouseEvent) => {
      const target = event.target as Node
      if (!menuRef.current?.contains(target) && !menuAnchor.current?.contains(target)) setMenuId(null)
    }
    document.addEventListener('mousedown', onPointerDown)
    return () => document.removeEventListener('mousedown', onPointerDown)
  }, [menuId])
  const openMenu = (id: string, anchor: HTMLElement) => {
    menuAnchor.current = anchor
    setMenuId(current => current === id ? null : id)
  }
  const serverEnabled = (health: McpStatus['providers'][number] | undefined, serverId: string) => health?.mcpServers?.find(item => item.id === serverId)?.enabled
  /** Compact count of enabled servers; the popover lists each one with its switch. */
  const mcpChip = (id: string, terminalOnly: boolean, health: McpStatus['providers'][number] | undefined) => {
    const flags = servers.map(server => serverEnabled(health, server.id))
    const on = flags.filter(flag => flag === true).length
    const known = flags.some(flag => flag !== undefined)
    const label = terminalOnly ? 'Terminal only' : known ? `${on} of ${servers.length} on` : 'Unknown'
    const open = menuId === id
    return <button type="button" className="uam-prov-mcp-chip" aria-haspopup="dialog" aria-expanded={open} aria-label={`${providerMetadataForId(id).shortName} MCP servers, ${label}`}
      disabled={terminalOnly || servers.length === 0} onClick={event => openMenu(id, event.currentTarget)}>
      <span className="uam-prov-meter" aria-hidden>{flags.map((flag, index) => <i key={index} data-state={flag === undefined ? 'unknown' : flag ? 'on' : 'off'} />)}</span>
      <span className="tabular-nums">{label}</span>
    </button>
  }
  const glyph = (label: string, available: boolean | undefined, title: string) => <button type="button" role="checkbox" disabled className="uam-prov-avail" data-state={available === undefined ? 'unknown' : available ? 'on' : 'off'} aria-label={available === undefined ? `${label} (unknown)` : label} aria-checked={available === undefined ? 'mixed' : available} title={title}>{available ? <Check size={13}/> : <Minus size={13}/>}</button>
  return <div className="space-y-3">
    <div className="flex items-center gap-2 flex-wrap">
      <div role="tablist" aria-label="Provider settings host" className="uam-prov-seg">
        {[{ id: 'local', label: 'This computer' }, ...hosts.filter(host => host.id !== 'local')].map(host => <button type="button" key={host.id} role="tab" className="uam-prov-seg__item" aria-label={host.label} aria-selected={host.id === activeHostId} onClick={() => setHostId(host.id)}>{host.label}</button>)}
      </div>
      <IconButton size="sm" variant="ghost" label="Refresh provider connections" tooltip="Check connections again" disabled={busy} onClick={() => setRefresh(value => value + 1)} icon={<RefreshCw size={14} className={!status && !statusError ? 'uam-prov-spin' : undefined}/>}/>
      <div className="ml-auto flex gap-1">
        <Button size="sm" variant="ghost" onClick={onPreferencesSelect}>Shared agent preferences</Button>
        <Button size="sm" onClick={onMcpSelect}>Manage MCP servers</Button>
      </div>
    </div>
    {!central.uamControlEnabled && <Notice tone="error" title="UAM MCP is off" dismissLabel="Dismiss UAM MCP warning" actions={<Button size="sm" disabled={busy} onClick={() => void enable()}>Enable UAM MCP</Button>}>New provider sessions will run without UAM tools. Existing sessions keep their current configuration.</Notice>}
    {error && <p role="alert" className="uam-prov-sub" style={{ color: 'var(--error)' }}>{error}</p>}
    {statusError && <p role="status" className="uam-prov-sub">{statusError}</p>}
    {!status && !statusError && <p role="status" className="uam-prov-sub">Checking connections…</p>}
    <div className="uam-prov-card overflow-x-auto">
      <table className="uam-prov-matrix w-full text-xs text-left" aria-label="Provider connection overview">
        <thead><tr style={{ borderBottom: '1px solid var(--border)' }}>{['Provider', 'Status', 'UAM MCP', 'Computer Use', 'Shared setup'].map(label => <th key={label} className={`uam-prov-label uam-prov-th px-2 py-2${label === 'Provider' ? ' uam-prov-sticky uam-prov-th--provider' : ''} font-medium`}>{label}</th>)}{collapseServers
          ? <th className="uam-prov-label uam-prov-th px-2 py-2 font-medium">MCP servers</th>
          : servers.map(server => <th key={server.id} title={[server.executionHostId, server.workspaceDirectory].filter(Boolean).join(' · ')} className="uam-prov-label uam-prov-th px-2 py-2 font-medium">{server.name}</th>)}</tr></thead>
        <tbody>{ids.map(id => {
          const metadata = providerMetadataForId(id)
          const runtime = runtimeProviders.find(provider => provider.id === id)
          const terminalOnly = runtime?.supportsStructured === undefined ? metadata.structuredProtocol === 'none' : !runtime.supportsStructured
          const version = hostVersions.find(item => item.providerId === id)
          const health = status?.executionHostId === activeHostId ? status.providers.find(item => item.providerId === id) : undefined
          const healthLabel = health?.status === 'connected' && status?.connectedProofAvailable ? 'Connected' : health?.status === 'failed' ? 'Failed' : health?.status === 'missing' ? 'Not configured' : health?.status === 'disabled' ? 'Disabled' : 'Unknown'
          const healthTone: StatusTone = terminalOnly ? 'neutral' : healthLabel === 'Connected' ? 'success' : healthLabel === 'Failed' ? 'error' : healthLabel === 'Not configured' ? 'warning' : 'neutral'
          const cli = version?.installedVersion || (version?.status === 'checking' ? 'Checking' : version?.status === 'unavailable' ? 'Not installed' : 'Not checked')
          const uamOn = health?.uamControlEnabled === undefined ? undefined : !terminalOnly && health.uamControlEnabled
          const computerUse = status?.executionHostId === activeHostId && typeof status.computerUseBuiltInAvailable === 'boolean' ? !terminalOnly && status.computerUseBuiltInAvailable : undefined
          return <tr key={id} style={{ borderTop: '1px solid var(--border)' }}>
            <td className="uam-prov-sticky uam-prov-provider-cell px-3 py-2"><button type="button" className="uam-prov-provider" onClick={() => onProviderSelect?.(id)} disabled={!onProviderSelect}><ProviderLogo providerId={id}/><span className="grid min-w-0"><span className="uam-prov-provider__name">{metadata.shortName}</span><span className="uam-prov-detail whitespace-nowrap">{cli}</span></span></button></td>
            <td className="px-2 py-2"><span className="uam-prov-status" data-tone={healthTone} title={health?.detail}><StatusDot tone={healthTone}/>{terminalOnly ? 'Terminal only' : healthLabel}</span>
              {!terminalOnly && health?.status === 'failed' && health.detail && <div className="uam-prov-detail uam-prov-detail--clip mt-0.5" title={health.detail}>{health.detail}</div>}</td>
            <td className="uam-prov-glyph-cell px-1 py-2">{glyph(`${metadata.shortName} UAM MCP`, uamOn, terminalOnly ? 'Not used in terminal mode' : uamOn ? 'Always on. Managed in the UAM MCP service section.' : uamOn === false ? 'Off. Turn UAM MCP on in the UAM MCP service section.' : 'Not known yet')}</td>
            <td className="uam-prov-glyph-cell px-1 py-2">{glyph(`${metadata.shortName} Computer Use`, computerUse === undefined ? undefined : computerUse && computerUseEnabled !== false, computerUseEnabled === false && computerUse ? 'Off. Turn Computer Use on in the Built in section.' : computerUse ? 'Available. Chat permissions are set in the composer.' : computerUse === false ? 'Not available for this provider' : 'Not known yet')}</td>
            <td className="uam-prov-glyph-cell px-1 py-2">{checkbox(`${metadata.shortName} Shared setup`, health?.sharedSetupEnabled, terminalOnly ? undefined : () => void setConfiguration(id, 'shared-setup', !health?.sharedSetupEnabled), `${id}:shared-setup`)}</td>
            {collapseServers
              ? <td className="px-2 py-2">{mcpChip(id, terminalOnly, health)}</td>
              : servers.map(server => {
                const enabled = serverEnabled(health, server.id)
                return <td key={server.id} className="uam-prov-glyph-cell px-1 py-2">{checkbox(`${metadata.shortName} ${server.name}`, enabled, terminalOnly ? undefined : () => void setConfiguration(id, server.id, !enabled), `${id}:${server.id}`)}</td>
              })}
          </tr>
        })}</tbody>
      </table>
    </div>
    {menuPresence.mounted && shownMenuId && (() => {
      const id = shownMenuId
      const metadata = providerMetadataForId(id)
      const runtime = runtimeProviders.find(provider => provider.id === id)
      const terminalOnly = runtime?.supportsStructured === undefined ? metadata.structuredProtocol === 'none' : !runtime.supportsStructured
      const health = status?.executionHostId === activeHostId ? status.providers.find(item => item.providerId === id) : undefined
      return <ViewportMenu ref={menuRef} anchorRef={menuAnchor} align="end" role="dialog" aria-label={`${metadata.shortName} MCP servers`} data-state={menuPresence.closing ? 'closed' : 'open'}
        className="uam-prov-pop" onKeyDown={event => { if (event.key === 'Escape') { event.preventDefault(); event.stopPropagation(); setMenuId(null); menuAnchor.current?.focus() } }}>
        <div className="uam-prov-label px-1 pb-1">MCP servers for {metadata.shortName}</div>
        {servers.map(server => {
          const enabled = serverEnabled(health, server.id)
          return <div key={server.id} className="uam-prov-pop__row">
            <span className="min-w-0 grid"><span className="truncate text-sm" style={{ color: 'var(--text)' }}>{server.name}</span>{server.workspaceDirectory && <span className="uam-prov-detail truncate">{server.workspaceDirectory}</span>}</span>
            {checkbox(`${metadata.shortName} ${server.name}`, enabled, terminalOnly ? undefined : () => void setConfiguration(id, server.id, !enabled), `${id}:${server.id}`)}
          </div>
        })}
      </ViewportMenu>
    })()}
  </div>
}
