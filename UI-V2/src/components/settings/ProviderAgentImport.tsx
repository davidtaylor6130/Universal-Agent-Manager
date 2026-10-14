import { useRef, useState } from 'react'
import { Bot } from 'lucide-react'
import { sendToCEF } from '../../ipc/cefBridge'
import type { ProviderAgentCandidate, ProviderAgentImportResult } from '../../store/cpp/types'
import { Button, MenuSelect, Switch } from '../ui'
import { SettingRow, SettingsLoading, SettingsSection } from './SettingsPrimitives'

const PROVIDERS = [
  { value: 'opencode-cli', label: 'OpenCode' },
  { value: 'copilot-cli', label: 'GitHub Copilot CLI' },
  { value: 'gemini-cli', label: 'Gemini CLI' },
  { value: 'claude-cli', label: 'Claude Code' },
]

const listAgents = (providerId: string) =>
  sendToCEF<{ agents: ProviderAgentCandidate[] }>({ action: 'listProviderAgents', payload: { providerId } })

/** Pick agents already defined in a provider's own configuration and import the ticked ones. Shown only when the backend advertises providerAgentImport. */
export function ProviderAgentImport() {
  const [providerId, setProviderId] = useState(PROVIDERS[0].value)
  const [agents, setAgents] = useState<ProviderAgentCandidate[] | null>(null)
  const [selected, setSelected] = useState<string[]>([])
  const [busy, setBusy] = useState<'find' | 'import' | null>(null)
  const [error, setError] = useState('')
  const [result, setResult] = useState<ProviderAgentImportResult | null>(null)
  // Bumped on every request so a slow response never overwrites a newer one (for example after a provider change).
  const request = useRef(0)
  const providerLabel = PROVIDERS.find(provider => provider.value === providerId)?.label ?? providerId
  const allSelected = agents !== null && agents.length > 0 && agents.every(agent => selected.includes(agent.id))

  const reset = () => { request.current += 1; setAgents(null); setSelected([]); setError(''); setResult(null); setBusy(null) }
  const changeProvider = (value: string) => { setProviderId(value); reset() }
  const findAgents = async () => {
    const id = ++request.current
    setBusy('find'); setError(''); setResult(null); setSelected([])
    const response = await listAgents(providerId)
    if (id !== request.current) return
    if (response.ok && response.data) setAgents(response.data.agents ?? [])
    else { setAgents(null); setError(response.error || `Could not look for agents in ${providerLabel}.`) }
    setBusy(null)
  }
  const importSelected = async () => {
    if (busy || selected.length === 0) return
    const id = ++request.current
    setBusy('import'); setError(''); setResult(null)
    const response = await sendToCEF<ProviderAgentImportResult>({ action: 'importProviderAgents', payload: { providerId, agentIds: selected } })
    if (id !== request.current) return
    if (!response.ok || !response.data) { setError(response.error || 'Import failed. Try again.'); setBusy(null); return }
    const data = response.data
    setResult(data)
    // Failed agents stay ticked so the user can retry them.
    setSelected((data.failed ?? []).map(item => item.id))
    const refreshed = await listAgents(providerId)
    if (id !== request.current) return
    if (refreshed.ok && refreshed.data) setAgents(refreshed.data.agents ?? [])
    setBusy(null)
  }
  const toggle = (agentId: string) => setSelected(current => current.includes(agentId) ? current.filter(item => item !== agentId) : [...current, agentId])
  const toggleAll = () => setSelected(agents && !allSelected ? agents.map(agent => agent.id) : [])

  return <>
    <SettingsSection title="Import from a provider"
      actions={<Button size="sm" variant={agents === null ? 'primary' : 'secondary'} loading={busy === 'find'} disabled={busy !== null} onClick={() => void findAgents()}>Find agents</Button>}>
      <SettingRow title="Provider" htmlFor="provider-agent-import-provider"
        control={<MenuSelect id="provider-agent-import-provider" label="Provider" value={providerId} disabled={busy !== null} options={PROVIDERS} onChange={changeProvider} />} />
      {error && <SettingRow title="Could not read agents" description={<span role="alert" style={{ color: 'var(--error)' }}>{error}</span>} />}
    </SettingsSection>

    {busy === 'find' && <SettingsLoading label="Looking for agents" rows={3} />}

    {busy !== 'find' && agents !== null && <SettingsSection title={`Agents in ${providerLabel}`}
      actions={<>
        {agents.length > 0 && <Button size="sm" variant="ghost" disabled={busy !== null} onClick={toggleAll}>{allSelected ? 'Clear selection' : 'Select all'}</Button>}
        <Button size="sm" variant="primary" loading={busy === 'import'} disabled={busy !== null || selected.length === 0} onClick={() => void importSelected()}>
          {selected.length > 0 ? `Import ${selected.length}` : 'Import'}
        </Button>
      </>}>
      {agents.length === 0 && <SettingRow title="No agents found" description="No agent files on this computer." />}
      {agents.map(agent => <SettingRow key={agent.id} icon={<Bot size={15} aria-hidden />} title={agent.name}
        description={<>
          <div>{agent.description || 'No description'}</div>
          <div className="font-mono" style={{ color: 'var(--text-3)' }}>{agent.sourcePath}</div>
        </>}
        status={agent.alreadyImported ? 'Already in UAM' : undefined}
        control={<Switch hideLabel label={`Import ${agent.name}`} checked={selected.includes(agent.id)} disabled={busy !== null} onChange={() => toggle(agent.id)} />} />)}
      {result && <SettingRow title={result.imported === 1 ? 'Imported 1 agent' : `Imported ${result.imported} agents`}
        description={result.failed?.length ? 'Failed agents stay ticked.' : undefined}>
        {result.failed?.map(item => <div key={item.id} className="text-xs" style={{ color: 'var(--error)' }}><span className="font-mono">{item.id}</span>: {item.error}</div>)}
      </SettingRow>}
    </SettingsSection>}
  </>
}
