import { useState } from 'react'
import { GitBranch, FolderGit2 } from 'lucide-react'
import { StatusDot, Switch } from '../ui'
import { useAppStore } from '../../store/useAppStore'
import { SettingRow, SettingsSection } from './SettingsPrimitives'

export function VcsSettings() {
  const gitEnabled = useAppStore((state) => state.gitEnabled)
  const svnEnabled = useAppStore((state) => state.svnEnabled)
  const save = useAppStore((state) => state.setVcsSettings)
  const [saving, setSaving] = useState(false)
  const [error, setError] = useState('')
  const update = async (key: 'gitEnabled' | 'svnEnabled', enabled: boolean) => {
    if (saving) return
    setSaving(true)
    setError('')
    try {
      if (!await save({ gitEnabled, svnEnabled, [key]: enabled })) setError('Could not save. Try again.')
    } catch { setError('Could not save. Try again.') }
    finally { setSaving(false) }
  }
  return <div className="grid gap-6">
    <SettingsSection title="Version control systems" description="Turn on the systems UAM tracks in each workspace. Existing worktree locations and cleanup are preserved. Turning Git off makes new chats use the workspace directly.">
      {([
        { key: 'gitEnabled', name: 'Git', icon: GitBranch, enabled: gitEnabled, detail: 'File changes, commits, new worktrees and turn checkpoints.' },
        { key: 'svnEnabled', name: 'SVN', icon: FolderGit2, enabled: svnEnabled, detail: 'File changes, revisions and commits.' },
      ] as const).map(({ key, name, icon: Icon, enabled, detail }) => <SettingRow key={key}
        icon={<Icon size={16} aria-hidden />}
        title={name}
        description={detail}
        status={<span className="flex items-center gap-1.5" style={{ color: enabled ? 'var(--success)' : 'var(--text-3)' }}><StatusDot tone={enabled ? 'success' : 'neutral'} size={6} />{enabled ? 'Tracking changes' : 'Off'}</span>}
        control={<Switch label={`Enable ${name}`} hideLabel checked={enabled} disabled={saving} onChange={() => { void update(key, !enabled) }} />} />)}
      {error && <SettingRow title="Not saved" description={<span role="alert" style={{ color: 'var(--error)' }}>{error}</span>} />}
      {saving && <SettingRow title="Saving…" description={<span role="status">Saving…</span>} />}
    </SettingsSection>
  </div>
}
