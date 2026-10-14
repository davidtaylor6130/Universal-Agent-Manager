import { forwardRef, useEffect, useImperativeHandle, useRef, useState } from 'react'
import { useShallow } from 'zustand/react/shallow'
import { Button } from '../ui'
import { useAppStore } from '../../store/useAppStore'
import { sanitizeGitBaseBranch } from '../../store/cpp/sanitizers'
import { SettingRow, SettingsSection } from './SettingsPrimitives'

export interface VcsSettingsSectionHandle { requestLeave: (next: () => void) => void }

export const VcsSettingsSection = forwardRef<VcsSettingsSectionHandle>(function VcsSettingsSection(_props, ref) {
  const saved = useAppStore(useShallow((state) => ({
    gitEnabled: state.gitEnabled, svnEnabled: state.svnEnabled,
    gitAutoWorktree: state.gitAutoWorktree, gitDefaultBaseBranch: state.gitDefaultBaseBranch,
    gitPortMethod: state.gitPortMethod, gitPullBeforeChat: state.gitPullBeforeChat, svnUpdateBeforeChat: state.svnUpdateBeforeChat,
  })))
  const saveSettings = useAppStore((state) => state.setVcsSettings)
  const [draft, setDraft] = useState(saved)
  const [baseline, setBaseline] = useState(saved)
  const [saving, setSaving] = useState(false)
  const [error, setError] = useState('')
  const [exit, setExit] = useState<(() => void) | null>(null)
  const busy = useRef(false)
  const dirty = JSON.stringify(draft) !== JSON.stringify(baseline)
  useEffect(() => { if (!saving && !dirty) { setDraft(saved); setBaseline(saved) } }, [saved, saving, dirty])
  useEffect(() => {
    if (!dirty) return
    const guard = (event: BeforeUnloadEvent) => { event.preventDefault(); event.returnValue = '' }
    window.addEventListener('beforeunload', guard)
    return () => window.removeEventListener('beforeunload', guard)
  }, [dirty])
  useImperativeHandle(ref, () => ({ requestLeave: (next) => { if (busy.current) return; if (dirty) setExit(() => next); else next() } }))
  const edit = (patch: Partial<typeof draft>) => { setDraft((current) => ({ ...current, ...patch })); setError('') }
  const base = sanitizeGitBaseBranch(draft.gitDefaultBaseBranch)
  const validation = base === undefined ? 'Use a local branch name up to 255 UTF-8 bytes without control characters.' : ''
  const save = async () => {
    if (busy.current || !dirty || validation) return
    busy.current = true; setSaving(true); setError('')
    try {
      if (!await saveSettings({ ...draft, gitDefaultBaseBranch: base! })) { setError('Could not save VCS settings. Your changes are retained.'); return }
      const current = useAppStore.getState()
      const accepted = { gitEnabled: current.gitEnabled, svnEnabled: current.svnEnabled, gitAutoWorktree: current.gitAutoWorktree,
        gitDefaultBaseBranch: current.gitDefaultBaseBranch, gitPortMethod: current.gitPortMethod, gitPullBeforeChat: current.gitPullBeforeChat, svnUpdateBeforeChat: current.svnUpdateBeforeChat }
      setDraft(accepted); setBaseline(accepted)
    } catch { setError('Could not save VCS settings. Your changes are retained.') }
    finally { busy.current = false; setSaving(false) }
  }
  const modes = (label: string, enabled: boolean, change: (enabled: boolean) => void, disabled = false) => <div className="flex gap-1" role="group" aria-label={label}>
    {[true, false].map((value) => <Button key={String(value)} size="sm" aria-pressed={enabled === value} disabled={saving || disabled}
      style={enabled === value ? { borderColor: 'var(--text)', background: 'var(--surface-high)' } : undefined}
      onClick={() => change(value)}>{value ? 'Enabled' : 'Disabled'}</Button>)}
  </div>
  return <div className="grid gap-6">
    <SettingsSection title="Git and SVN">
      <SettingRow title="Git" control={modes('Git', draft.gitEnabled, (value) => edit({ gitEnabled: value }))} />
      <SettingRow title="Automatic isolation" description="Separate worktree before each new chat." control={modes('Automatic isolation', draft.gitAutoWorktree, (value) => edit({ gitAutoWorktree: value }), !draft.gitEnabled)} />
      <SettingRow title="Default base branch" description="Empty uses HEAD. Must exist locally." control={<input className="uam-field" aria-label="Default local Git base branch" placeholder="HEAD" value={draft.gitDefaultBaseBranch} disabled={saving || !draft.gitEnabled} spellCheck={false} onChange={(event) => edit({ gitDefaultBaseBranch: event.currentTarget.value })} />} />
      <SettingRow title="Port changes" description="Merge and Squash stage changes for you to commit." control={<div className="flex gap-1" role="group" aria-label="Port method">
        {(['patch', 'merge', 'squash'] as const).map((method) => <Button key={method} size="sm" disabled={saving || !draft.gitEnabled} aria-pressed={draft.gitPortMethod === method}
          style={draft.gitPortMethod === method ? { borderColor: 'var(--text)', background: 'var(--surface-high)' } : undefined}
          onClick={() => edit({ gitPortMethod: method })}>{method.charAt(0).toUpperCase() + method.slice(1)}</Button>)}
      </div>} />
      <SettingRow title="Pull before first chat" description="Fast-forward only, on a clean tree with an upstream." control={modes('Pull before first chat', draft.gitPullBeforeChat, (value) => edit({ gitPullBeforeChat: value }), !draft.gitEnabled || !draft.gitAutoWorktree)} />
      <SettingRow title="SVN" control={modes('SVN', draft.svnEnabled, (value) => edit({ svnEnabled: value }))} />
      <SettingRow title="Update before new chat" description="Run svn update before a new chat." control={modes('Update before new chat', draft.svnUpdateBeforeChat, (value) => edit({ svnUpdateBeforeChat: value }), !draft.svnEnabled)} />
      <SettingRow title="SVN branch and merge defaults" control={<Button size="sm" disabled>Not supported</Button>} />
    </SettingsSection>
    {(error || validation) && <p role="alert" className="text-xs" style={{ color: 'var(--error)' }}>{error || validation}</p>}
    <div className="flex justify-end gap-2">
      <Button size="sm" disabled={saving || !dirty} onClick={() => { setDraft(saved); setBaseline(saved); setError('') }}>Discard</Button>
      <Button size="sm" variant="primary" disabled={saving || !dirty || Boolean(validation)} onClick={() => void save()}>{saving ? 'Saving…' : 'Save VCS settings'}</Button>
    </div>
    {exit && <div role="alert" className="flex items-center justify-between gap-3"><span>Discard unsaved VCS changes?</span><div className="flex gap-2">
      <Button size="sm" onClick={() => setExit(null)}>Keep editing</Button><Button size="sm" onClick={() => { const next = exit; setDraft(saved); setBaseline(saved); setError(''); setExit(null); next() }}>Discard and leave</Button>
    </div></div>}
  </div>
})
