import { forwardRef, useEffect, useImperativeHandle, useRef, useState } from 'react'
import { useShallow } from 'zustand/react/shallow'
import { Button, ConfirmDialog, Notice, SegmentedControl, Switch, usePresence } from '../ui'
import { useAppStore } from '../../store/useAppStore'
import { reportAppFailure, reportAppNotice } from '../../utils/appFailure'
import { sanitizeGitBaseBranch } from '../../store/cpp/sanitizers'
import { SaveButton, SettingRow, SettingsSection } from './SettingsPrimitives'

export interface VcsSettingsSectionHandle { requestLeave: (next: () => void) => void }

export const VcsSettingsSection = forwardRef<VcsSettingsSectionHandle>(function VcsSettingsSection(_props, ref) {
  const saved = useAppStore(useShallow((state) => ({
    gitEnabled: state.gitEnabled, svnEnabled: state.svnEnabled,
    gitSnapshotSvn: state.gitSnapshotSvn, gitSnapshotPlainFolders: state.gitSnapshotPlainFolders,
    gitAutoWorktree: state.gitAutoWorktree, gitDefaultBaseBranch: state.gitDefaultBaseBranch,
    gitPortMethod: state.gitPortMethod, gitPullBeforeChat: state.gitPullBeforeChat, svnUpdateBeforeChat: state.svnUpdateBeforeChat,
  })))
  const saveSettings = useAppStore((state) => state.setVcsSettings)
  const [draft, setDraft] = useState(saved)
  const [baseline, setBaseline] = useState(saved)
  const [saving, setSaving] = useState(false)
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
  const edit = (patch: Partial<typeof draft>) => { setDraft((current) => ({ ...current, ...patch })) }
  const base = sanitizeGitBaseBranch(draft.gitDefaultBaseBranch)
  const validation = base === undefined ? 'Use a local branch name up to 255 UTF-8 bytes without control characters.' : ''
  const validationNotice = usePresence(Boolean(validation))
  const shownValidation = useRef(validation)
  if (validation) shownValidation.current = validation
  const save = async () => {
    if (busy.current || !dirty || validation) return
    busy.current = true; setSaving(true)
    try {
      if (!await saveSettings({ ...draft, gitDefaultBaseBranch: base! })) { reportAppFailure('Could not save VCS settings.', 'Your changes are retained.'); return }
      const current = useAppStore.getState()
      const accepted = { gitEnabled: current.gitEnabled, svnEnabled: current.svnEnabled,
        gitSnapshotSvn: current.gitSnapshotSvn, gitSnapshotPlainFolders: current.gitSnapshotPlainFolders, gitAutoWorktree: current.gitAutoWorktree,
        gitDefaultBaseBranch: current.gitDefaultBaseBranch, gitPortMethod: current.gitPortMethod, gitPullBeforeChat: current.gitPullBeforeChat, svnUpdateBeforeChat: current.svnUpdateBeforeChat }
      setDraft(accepted); setBaseline(accepted); setExit(null)
      reportAppNotice('VCS settings saved.', 'success')
    } catch { reportAppFailure('Could not save VCS settings.', 'Your changes are retained.') }
    finally { busy.current = false; setSaving(false) }
  }
  const toggle = (label: string, checked: boolean, change: (checked: boolean) => void, disabled = false) =>
    <Switch hideLabel label={label} checked={checked} disabled={saving || disabled} onChange={(event) => change(event.currentTarget.checked)} />
  return <div className="grid gap-6">
    <SettingsSection title="Git">
      <SettingRow title="Enable Git" control={toggle('Enable Git', draft.gitEnabled, (value) => edit({ gitEnabled: value }))} />
      <SettingRow title="Automatic isolation" description="Separate worktree before each new chat." control={toggle('Automatic isolation', draft.gitAutoWorktree, (value) => edit({ gitAutoWorktree: value }), !draft.gitEnabled)} />
      <SettingRow title="Default base branch" description="Empty uses HEAD. Must exist locally." control={<input className="uam-field" aria-label="Default local Git base branch" placeholder="HEAD" value={draft.gitDefaultBaseBranch} disabled={saving || !draft.gitEnabled} spellCheck={false} onChange={(event) => edit({ gitDefaultBaseBranch: event.currentTarget.value })} />} />
      <SettingRow title="Port changes" description="Merge and Squash stage changes for you to commit."
        control={<SegmentedControl aria-label="Port method" disabled={saving || !draft.gitEnabled} value={draft.gitPortMethod}
          options={[{ value: 'patch', label: 'Patch' }, { value: 'merge', label: 'Merge' }, { value: 'squash', label: 'Squash' }]}
          onChange={(method) => edit({ gitPortMethod: method })} />} />
      <SettingRow title="Pull before first chat" description="Fast-forward only, on a clean tree with an upstream." control={toggle('Pull before first chat', draft.gitPullBeforeChat, (value) => edit({ gitPullBeforeChat: value }), !draft.gitEnabled || !draft.gitAutoWorktree)} />
      <SettingRow title="Enable local Git files" description="Off means new chats edit the original folder directly." control={toggle('Enable local Git files', draft.gitSnapshotPlainFolders, (value) => edit({ gitSnapshotPlainFolders: value }))} />
    </SettingsSection>
    <SettingsSection title="SVN">
      <SettingRow title="Enable SVN" control={toggle('Enable SVN', draft.svnEnabled, (value) => edit({ svnEnabled: value }))} />
      <SettingRow title="Update before new chat" description="Run svn update before a new chat." control={toggle('Update before new chat', draft.svnUpdateBeforeChat, (value) => edit({ svnUpdateBeforeChat: value }), !draft.svnEnabled)} />
      <SettingRow title="Local snapshot Git" description="Off means new chats edit the original working copy." control={toggle('Local snapshot Git for SVN', draft.gitSnapshotSvn, (value) => edit({ gitSnapshotSvn: value }))} />
    </SettingsSection>
    {validationNotice.mounted && <div className="uam-settings-presence" data-state={validationNotice.closing ? 'closed' : 'open'}><Notice tone="error" dismissible={false} dismissLabel="Dismiss">{shownValidation.current}</Notice></div>}
    <div className="flex justify-end gap-2">
      <Button size="sm" disabled={saving || !dirty} onClick={() => { setDraft(saved); setBaseline(saved) }}>Discard</Button>
      <SaveButton saving={saving} disabled={saving || !dirty || Boolean(validation)} onClick={() => void save()}>Save VCS settings</SaveButton>
    </div>
    <ConfirmDialog open={Boolean(exit)} busy={saving} title="Discard unsaved VCS changes?" confirmLabel="Discard and leave" cancelLabel="Keep editing"
      onCancel={() => setExit(null)}
      onConfirm={() => { const next = exit; setDraft(saved); setBaseline(saved); setExit(null); next?.() }}>
      Your unsaved Git, SVN and VCS changes will be lost.
    </ConfirmDialog>
  </div>
})
