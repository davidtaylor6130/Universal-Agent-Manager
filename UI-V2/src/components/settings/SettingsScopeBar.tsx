import { MenuSelect, Tooltip } from '../ui'
import type { ExecutionHost } from '../../types/session'
import { SETTINGS_ALL_ENVIRONMENTS } from '../../utils/settingsScope'

/**
 * Which computer the settings on this page apply to. Only SSH hosts are offered;
 * "This computer" is always covered by "All environments". Disabled until the backend
 * advertises per-computer settings, and the scope then stays "All environments".
 */
export function SettingsScopeBar({ hosts, value, supported, onChange }: {
  hosts: ExecutionHost[]
  /** Effective scope: SETTINGS_ALL_ENVIRONMENTS or a host id. */
  value: string
  supported: boolean
  onChange: (hostId: string) => void
}) {
  const sshHosts = hosts.filter((host) => host.transport === 'ssh')
  const selected = sshHosts.find((host) => host.id === value)
  const options = [
    { value: SETTINGS_ALL_ENVIRONMENTS, label: 'All environments' },
    ...sshHosts.map((host) => ({ value: host.id, label: host.label, description: host.sshAlias })),
  ]
  return (
    <div className="mb-4 grid gap-1.5">
      <div className="flex flex-wrap items-center gap-3">
        <label htmlFor="settings-scope" className="text-xs font-medium" style={{ color: 'var(--text-2)' }}>Applying settings for</label>
        <Tooltip label={supported ? undefined : 'Per-computer settings need an update to UAM'}>
          <span className="inline-flex">
            <MenuSelect id="settings-scope" label="Applying settings for" value={supported ? value : SETTINGS_ALL_ENVIRONMENTS} options={options} disabled={!supported} onChange={onChange} />
          </span>
        </Tooltip>
      </div>
      {supported && selected && <p className="text-xs" style={{ color: 'var(--text-3)' }}>Changes here apply only to {selected.label}.</p>}
    </div>
  )
}
