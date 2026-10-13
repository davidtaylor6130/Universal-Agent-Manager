import { forwardRef, useEffect, useImperativeHandle, useRef, useState } from 'react'
import { useShallow } from 'zustand/react/shallow'
import { Button } from '../ui'
import { useAppStore } from '../../store/useAppStore'
import { SettingRow, SettingsSection } from './SettingsPrimitives'

export interface StorageCleanupSettingsSectionHandle { requestLeave: (next: () => void) => void }

export const StorageCleanupSettingsSection = forwardRef<StorageCleanupSettingsSectionHandle>(function StorageCleanupSettingsSection(_props, ref) {
  const saved = useAppStore(useShallow((state) => ({ enabled: state.worktreeCleanupEnabled, worktree: String(state.worktreeRetentionDays),
    logs: String(state.backupLogRetentionDays), cache: String(state.httpCacheClearIntervalDays), periodic: state.httpCacheClearIntervalDays > 0 })))
  const saveVcs = useAppStore((state) => state.setVcsSettings)
  const saveStorage = useAppStore((state) => state.setStorageSettings)
  const [draft, setDraft] = useState(saved)
  const [baseline, setBaseline] = useState(saved)
  const [saving, setSaving] = useState<'worktree' | 'storage' | null>(null)
  const [error, setError] = useState('')
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
  const edit = (patch: Partial<typeof draft>) => { setDraft((value) => ({ ...value, ...patch })); setError('') }
  const days = (value: string, minimum: number) => /^\d+$/.test(value) && Number.isSafeInteger(Number(value)) && Number(value) >= minimum && Number(value) <= 3650
  const worktreeValid = days(draft.worktree, 1)
  const storageValid = days(draft.logs, 0) && (!draft.periodic || days(draft.cache, 1))
  const save = async (group: 'worktree' | 'storage') => {
    if (busy.current || (group === 'worktree' ? !worktreeDirty || !worktreeValid : !storageDirty || !storageValid)) return
    busy.current = true; setSaving(group); setError('')
    try {
      const current = useAppStore.getState()
      const ok = group === 'worktree'
        ? await saveVcs({ gitEnabled: current.gitEnabled, svnEnabled: current.svnEnabled, worktreeCleanupEnabled: draft.enabled, worktreeRetentionDays: Number(draft.worktree) })
        : await saveStorage({ backupLogRetentionDays: Number(draft.logs), httpCacheClearIntervalDays: draft.periodic ? Number(draft.cache) : 0 })
      if (!ok) { setError(`Could not save ${group === 'worktree' ? 'worktree settings' : 'backup and cache policy'}. Your changes are retained.`); return }
      const accepted = useAppStore.getState()
      const patch = group === 'worktree' ? { enabled: accepted.worktreeCleanupEnabled, worktree: String(accepted.worktreeRetentionDays) }
        : { logs: String(accepted.backupLogRetentionDays), cache: String(accepted.httpCacheClearIntervalDays), periodic: accepted.httpCacheClearIntervalDays > 0 }
      setDraft((value) => ({ ...value, ...patch })); setBaseline((value) => ({ ...value, ...patch }))
    } catch { setError('Could not save cleanup policy. Your changes are retained.') }
    finally { busy.current = false; setSaving(null) }
  }
  const modes = (label: string, enabled: boolean, choices: [string, string], change: (value: boolean) => void) => <div className="flex gap-1" role="group" aria-label={label}>
    {[true, false].map((value, index) => <Button key={String(value)} size="sm" aria-pressed={enabled === value} disabled={Boolean(saving)}
      style={enabled === value ? { borderColor: 'var(--text)', background: 'var(--surface-high)' } : undefined} onClick={() => change(value)}>{choices[index]}</Button>)}
  </div>
  const number = (label: string, value: string, minimum: number, change: (value: string) => void, disabled = false) => <div className="flex items-center gap-2"><input className="uam-input" type="number" aria-label={label} value={value} min={minimum} max={3650} step={1} disabled={Boolean(saving) || disabled} onChange={(event) => change(event.currentTarget.value)} /><span>days</span></div>
  const discard = (group: 'worktree' | 'storage') => {
    const patch = group === 'worktree' ? { enabled: saved.enabled, worktree: saved.worktree } : { logs: saved.logs, cache: saved.cache, periodic: saved.periodic }
    setDraft((value) => ({ ...value, ...patch })); setBaseline((value) => ({ ...value, ...patch })); setError('')
  }
  return <div className="grid gap-6">
    <SettingsSection title="Worktree cleanup">
      <SettingRow title="Worktree cleanup" description="Remove eligible old worktrees when Git is enabled, after ownership, activity and change checks." control={modes('Worktree cleanup', draft.enabled, ['Enabled', 'Disabled'], (value) => edit({ enabled: value }))} />
      <SettingRow title="Worktree age" description="Branches and their commits are retained. Use 1–3650 days." control={number('Worktree retention age', draft.worktree, 1, (value) => edit({ worktree: value }), !draft.enabled)} />
      {!worktreeValid && <p role="alert" className="px-3 text-xs" style={{ color: 'var(--error)' }}>Worktree age must be a whole number from 1 to 3650.</p>}
      <div className="flex justify-end gap-2 p-3"><Button size="sm" disabled={Boolean(saving) || !worktreeDirty} onClick={() => discard('worktree')}>Discard worktree changes</Button><Button size="sm" variant="primary" disabled={Boolean(saving) || !worktreeDirty || !worktreeValid} onClick={() => void save('worktree')}>{saving === 'worktree' ? 'Saving…' : 'Save worktree settings'}</Button></div>
    </SettingsSection>
    <SettingsSection title="Backup logs and browser cache">
      <SettingRow title="Backup log retention" description="Current app, CEF and crash-recovery logs stay. Zero keeps all backups." control={number('Backup log retention', draft.logs, 0, (value) => edit({ logs: value }))} />
      <SettingRow title="HTTP cache clearing" description="Clear the HTTP cache periodically. Profiles and sign-in data stay." control={modes('HTTP cache clearing', draft.periodic, ['Periodic', 'Disabled'], (value) => edit({ periodic: value, cache: value ? (Number(draft.cache) > 0 ? draft.cache : '30') : '0' }))} />
      <SettingRow title="Cache interval" description="Enabling starts the interval without clearing immediately. Use 1–3650 days." control={number('HTTP cache interval', draft.cache, 1, (value) => edit({ cache: value }), !draft.periodic)} />
      {!storageValid && <p role="alert" className="px-3 text-xs" style={{ color: 'var(--error)' }}>Backup days must be a whole number from 0 to 3650; periodic cache days from 1 to 3650.</p>}
      <div className="flex justify-end gap-2 p-3"><Button size="sm" disabled={Boolean(saving) || !storageDirty} onClick={() => discard('storage')}>Discard backup and cache changes</Button><Button size="sm" variant="primary" disabled={Boolean(saving) || !storageDirty || !storageValid} onClick={() => void save('storage')}>{saving === 'storage' ? 'Saving…' : 'Save backup and cache policy'}</Button></div>
    </SettingsSection>
    {error && <p role="alert" className="text-xs" style={{ color: 'var(--error)' }}>{error}</p>}
    {exit && <div role="alert" className="flex items-center justify-between gap-3"><span>Discard unsaved cleanup changes?</span><div className="flex gap-2"><Button size="sm" onClick={() => setExit(null)}>Keep editing</Button><Button size="sm" onClick={() => { const next = exit; setDraft(saved); setBaseline(saved); setError(''); setExit(null); next() }}>Discard and leave</Button></div></div>}
  </div>
})
