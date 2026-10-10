import { useState } from 'react'
import { GitBranch, FolderGit2 } from 'lucide-react'
import { Switch } from '../ui'
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
  return <div>
    {([
      { key: 'gitEnabled', name: 'Git', icon: GitBranch, enabled: gitEnabled, detail: 'File changes, commits, new worktrees and turn checkpoints.' },
      { key: 'svnEnabled', name: 'SVN', icon: FolderGit2, enabled: svnEnabled, detail: 'File changes, revisions and commits.' },
    ] as const).map(({ key, name, icon: Icon, enabled, detail }) => <div key={key} className="flex items-center gap-3 border-b py-4" style={{ borderColor: 'var(--border)' }}>
      <Icon size={18} aria-hidden />
      <div className="flex-1"><div className="text-sm font-medium">{name}</div><p className="mt-1 text-xs" style={{ color: 'var(--text-2)' }}>{detail}</p></div>
      <Switch label={`Enable ${name}`} hideLabel checked={enabled} disabled={saving} onChange={() => { void update(key, !enabled) }} />
    </div>)}
    <p className="mt-3 text-xs" style={{ color: 'var(--text-2)' }}>Existing worktree locations and cleanup are preserved. Turning Git off makes new chats use the workspace directly.</p>
    {error && <p role="alert" className="mt-3 text-sm" style={{ color: 'var(--red)' }}>{error}</p>}
    {saving && <p role="status" className="mt-3 text-xs">Saving…</p>}
  </div>
}
