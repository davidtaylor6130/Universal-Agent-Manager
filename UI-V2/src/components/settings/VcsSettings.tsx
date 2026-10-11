import { useState } from 'react'
import { GitBranch, FolderGit2 } from 'lucide-react'
import { StatusDot, Switch } from '../ui'
import { useAppStore } from '../../store/useAppStore'

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
  return <div className="uam-vcs-settings">
    <section aria-label="Version control systems" className="uam-settings-group">
      <h3 className="uam-settings-group__title">Systems</h3>
      <div className="uam-settings-group__body">
        {([
          { key: 'gitEnabled', name: 'Git', icon: GitBranch, enabled: gitEnabled, detail: 'File changes, commits, new worktrees and turn checkpoints.' },
          { key: 'svnEnabled', name: 'SVN', icon: FolderGit2, enabled: svnEnabled, detail: 'File changes, revisions and commits.' },
        ] as const).map(({ key, name, icon: Icon, enabled, detail }) => <div key={key} className="flex items-center gap-3">
          <Icon size={16} aria-hidden style={{ color: 'var(--text-3)' }} />
          <div className="min-w-0 flex-1">
            <div className="flex items-center gap-2 text-sm font-medium">
              {name}
              <span className="flex items-center gap-1.5 text-xs font-normal" style={{ color: enabled ? 'var(--success)' : 'var(--text-3)' }}>
                <StatusDot tone={enabled ? 'success' : 'neutral'} size={6} />{enabled ? 'On' : 'Off'}
              </span>
            </div>
            <p className="mt-0.5 text-xs" style={{ color: 'var(--text-3)' }}>{detail}</p>
          </div>
          <Switch label={`Enable ${name}`} hideLabel checked={enabled} disabled={saving} onChange={() => { void update(key, !enabled) }} />
        </div>)}
      </div>
    </section>
    <p className="text-xs" style={{ color: 'var(--text-3)' }}>Existing worktree locations and cleanup are preserved. Turning Git off makes new chats use the workspace directly.</p>
    {error && <p role="alert" className="mt-3 text-xs" style={{ color: 'var(--error)' }}>{error}</p>}
    {saving && <p role="status" className="mt-3 text-xs" style={{ color: 'var(--text-3)' }}>Saving…</p>}
  </div>
}
