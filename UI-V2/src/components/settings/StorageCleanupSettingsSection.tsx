import { forwardRef, useEffect, useImperativeHandle, useRef, useState } from 'react'
import { useShallow } from 'zustand/react/shallow'
import { Button, ConfirmDialog, SegmentedControl, Switch } from '../ui'
import { NumberField } from './NumberField'
import { useAppStore } from '../../store/useAppStore'
import { reportAppFailure } from '../../utils/appFailure'
import { SaveButton, SettingRow, SettingsSection } from './SettingsPrimitives'

export interface StorageCleanupSettingsSectionHandle { requestLeave: (next: () => void) => void }

export const StorageCleanupSettingsSection = forwardRef<StorageCleanupSettingsSectionHandle>(function StorageCleanupSettingsSection(_props, ref) {
  const saved = useAppStore(useShallow((state) => ({ enabled: state.worktreeCleanupEnabled, worktree: String(state.worktreeRetentionDays),
    logs: String(state.backupLogRetentionDays), cache: String(state.httpCacheClearIntervalDays), periodic: state.httpCacheClearIntervalDays > 0 })))
  const saveVcs = useAppStore((state) => state.setVcsSettings)
  const saveStorage = useAppStore((state) => state.setStorageSettings)
  const [draft, setDraft] = useState(saved)
  const [baseline, setBaseline] = useState(saved)
  const [saving, setSaving] = useState<'worktree' | 'storage' | null>(null)
  const [exit, setExit] = useState<(() => void) | null>(null)
  const busy = useRef(false)
  const worktreeDirty = draft.enabled !== baseline.enabled || draft.worktree !== baseline.worktree
  const storageDirty = draft.logs !== baseline.logs || draft.cache !== baseline.cache || draft.periodic !== baseline.periodic
  const dirty = worktreeDirty || storageDirty
  useEffect(() => {
    if (saving) return
    if (!worktreeDirty) { setDraft((value) => ({ ...value, enabled: saved.enabled, worktree: saved.worktree })); setBaseline((value) => ({ ...value, enabled: saved.enabled, worktree: saved.worktree })) }
    if (!storageDirty) { setDraft((value) => ({ ...value, logs: saved.logs, cache: saved.cache, periodic: saved.periodic })); setBaseline((value) => ({ ...value, logs: saved.logs, cache: saved.cache, periodic: saved.periodic })) }
  }, [saved, saving, worktreeDirty, storageDirty])
  useEffect(() => {
    if (!dirty) return
    const guard = (event: BeforeUnloadEvent) => { event.preventDefault(); event.returnValue = '' }
    window.addEventListener('beforeunload', guard)
    return () => window.removeEventListener('beforeunload', guard)
  }, [dirty])
  useImperativeHandle(ref, () => ({ requestLeave: (next) => { if (busy.current) return; if (dirty) setExit(() => next); else next() } }))
  const edit = (patch: Partial<typeof draft>) => { setDraft((value) => ({ ...value, ...patch })) }
  const days = (value: string, minimum: number) => /^\d+$/.test(value) && Number.isSafeInteger(Number(value)) && Number(value) >= minimum && Number(value) <= 3650
  const worktreeValid = days(draft.worktree, 1)
  const storageValid = days(draft.logs, 0) && (!draft.periodic || days(draft.cache, 1))
  const save = async (group: 'worktree' | 'storage') => {
    if (busy.current || (group === 'worktree' ? !worktreeDirty || !worktreeValid : !storageDirty || !storageValid)) return
    busy.current = true; setSaving(group)
    try {
      const current = useAppStore.getState()
      const ok = group === 'worktree'
        ? await saveVcs({ gitEnabled: current.gitEnabled, svnEnabled: current.svnEnabled, worktreeCleanupEnabled: draft.enabled, worktreeRetentionDays: Number(draft.worktree) })
        : await saveStorage({ backupLogRetentionDays: Number(draft.logs), httpCacheClearIntervalDays: draft.periodic ? Number(draft.cache) : 0 })
      if (!ok) { reportAppFailure(`Could not save ${group === 'worktree' ? 'worktree settings' : 'backup and cache policy'}.`, 'Your changes are retained.'); return }
      const accepted = useAppStore.getState()
      const patch = group === 'worktree' ? { enabled: accepted.worktreeCleanupEnabled, worktree: String(accepted.worktreeRetentionDays) }
        : { logs: String(accepted.backupLogRetentionDays), cache: String(accepted.httpCacheClearIntervalDays), periodic: accepted.httpCacheClearIntervalDays > 0 }
      setDraft((value) => ({ ...value, ...patch })); setBaseline((value) => ({ ...value, ...patch }))
    } catch { reportAppFailure('Could not save cleanup policy.', 'Your changes are retained.') }
    finally { busy.current = false; setSaving(null) }
  }
  const modes = (label: string, enabled: boolean, change: (value: boolean) => void) => <SegmentedControl aria-label={label} disabled={Boolean(saving)} value={enabled ? 'periodic' : 'disabled'}
    options={[{ value: 'periodic', label: 'Periodic' }, { value: 'disabled', label: 'Disabled' }]} onChange={(value) => change(value === 'periodic')} />
  // ponytail: NumberField clamps on commit, so the alerts below only show for values typed into the draft by other paths.
  const number = (label: string, value: string, minimum: number, change: (value: string) => void, disabled = false) => <fieldset disabled={Boolean(saving) || disabled} className="m-0 min-w-0 border-0 p-0"><NumberField label={label} value={Number(value)} min={minimum} max={3650} unit="days" onCommit={(next) => change(String(next))} /></fieldset>
  const discard = (group: 'worktree' | 'storage') => {
    const patch = group === 'worktree' ? { enabled: saved.enabled, worktree: saved.worktree } : { logs: saved.logs, cache: saved.cache, periodic: saved.periodic }
    setDraft((value) => ({ ...value, ...patch })); setBaseline((value) => ({ ...value, ...patch }))
  }
  return <div className="grid gap-6">
    <SettingsSection title="Worktrees">
      <SettingRow title="Worktree cleanup" description="Removes old worktrees only after safety checks." control={<Switch hideLabel label="Worktree cleanup" checked={draft.enabled} disabled={Boolean(saving)} onChange={(event) => edit({ enabled: event.currentTarget.checked })} />} />
      <SettingRow title="Worktree age" description="Branches and commits are kept." control={number('Worktree retention age', draft.worktree, 1, (value) => edit({ worktree: value }), !draft.enabled)} />
      {!worktreeValid && <p role="alert" className="px-3 text-xs" style={{ color: 'var(--error)' }}>Worktree age must be a whole number from 1 to 3650.</p>}
      <div className="flex justify-end gap-2 p-3"><Button size="sm" disabled={Boolean(saving) || !worktreeDirty} onClick={() => discard('worktree')}>Discard worktree changes</Button><SaveButton saving={saving === 'worktree'} disabled={Boolean(saving) || !worktreeDirty || !worktreeValid} onClick={() => void save('worktree')}>Save worktree settings</SaveButton></div>
    </SettingsSection>
    <SettingsSection title="Logs and cache">
      <SettingRow title="Backup log retention" description="Current logs are kept. 0 keeps all backups." control={number('Backup log retention', draft.logs, 0, (value) => edit({ logs: value }))} />
      <SettingRow title="HTTP cache clearing" description="Profiles and sign-in data are kept." control={modes('HTTP cache clearing', draft.periodic, (value) => edit({ periodic: value, cache: value ? (Number(draft.cache) > 0 ? draft.cache : '30') : '0' }))} />
      <SettingRow title="Cache interval" description="Starts counting without clearing at once." control={number('HTTP cache interval', draft.cache, 1, (value) => edit({ cache: value }), !draft.periodic)} />
      {!storageValid && <p role="alert" className="px-3 text-xs" style={{ color: 'var(--error)' }}>Backup days must be a whole number from 0 to 3650; periodic cache days from 1 to 3650.</p>}
      <div className="flex justify-end gap-2 p-3"><Button size="sm" disabled={Boolean(saving) || !storageDirty} onClick={() => discard('storage')}>Discard backup and cache changes</Button><SaveButton saving={saving === 'storage'} disabled={Boolean(saving) || !storageDirty || !storageValid} onClick={() => void save('storage')}>Save backup and cache policy</SaveButton></div>
    </SettingsSection>
    <ConfirmDialog open={Boolean(exit)} title="Discard unsaved cleanup changes?" confirmLabel="Discard and leave" cancelLabel="Keep editing"
      onCancel={() => setExit(null)}
      onConfirm={() => { const next = exit; setDraft(saved); setBaseline(saved); setExit(null); next?.() }}>
      Your unsaved worktree, backup and cache changes will be lost.
    </ConfirmDialog>
  </div>
})
