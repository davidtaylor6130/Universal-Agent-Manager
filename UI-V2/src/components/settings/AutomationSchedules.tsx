import { useCallback, useEffect, useState } from 'react'
import { Pencil, Plus, X } from 'lucide-react'
import { useShallow } from 'zustand/react/shallow'
import { sendToCEF } from '../../ipc/cefBridge'
import { useAppStore } from '../../store/useAppStore'
import { reportAppFailure } from '../../utils/appFailure'
import { Button, Chip, ConfirmDialog, IconButton, MenuSelect, Overlay, Presence, Switch } from '../ui'
import { SettingRow, SettingsField, SettingsLoading, SettingsSection } from './SettingsPrimitives'

export type ScheduleTrigger = { kind: 'interval'; minutes: number } | { kind: 'weekly'; days: number[]; time: string }

export interface ScheduleRequest {
  prompt: string
  workspaceId: string
  providerId: string
  modelId?: string
  title?: string
  maxIterations: number
  gitPermission: string
}

export interface AutomationSchedule {
  id: string
  name: string
  paused: boolean
  trusted: boolean
  trigger: ScheduleTrigger
  missedPolicy: 'skip' | 'runOnce'
  request: ScheduleRequest
  /** Epoch milliseconds; 0 when not yet known. */
  nextRunMs: number
  lastRunMs: number
  lastResult: '' | 'started' | 'awaitingApproval' | 'starting' | 'failed'
  lastError: string
  lastSkipped: number
}

const DAY_NAMES = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat']
const DAY_LABELS = ['Sunday', 'Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday']
// Monday first, as a week reads in most calendars.
const DAY_ORDER = [1, 2, 3, 4, 5, 6, 0]

const REPEAT_OPTIONS = [
  { value: '15', label: 'Every 15 minutes' },
  { value: '30', label: 'Every 30 minutes' },
  { value: '60', label: 'Every hour' },
  { value: '120', label: 'Every 2 hours' },
  { value: '240', label: 'Every 4 hours' },
  { value: '360', label: 'Every 6 hours' },
  { value: '720', label: 'Every 12 hours' },
  { value: '1440', label: 'Every day' },
  { value: '10080', label: 'Every 7 days' },
  { value: 'weekly', label: 'On chosen days at a time' },
]

const MISSED_OPTIONS = [
  { value: 'skip', label: 'Skip them' },
  { value: 'runOnce', label: 'Run once' },
]

const GIT_OPTIONS = [
  { value: 'commit', label: 'Commit to its branch' },
  { value: 'pr', label: 'Open a pull request' },
]

export function describeTrigger(trigger: ScheduleTrigger): string {
  if (trigger.kind === 'interval') {
    const preset = REPEAT_OPTIONS.find(option => option.value === String(trigger.minutes))
    if (preset) return preset.label
    return trigger.minutes % 60 === 0 ? `Every ${trigger.minutes / 60} hours` : `Every ${trigger.minutes} minutes`
  }
  const days = [...trigger.days].sort((left, right) => DAY_ORDER.indexOf(left) - DAY_ORDER.indexOf(right))
  if (days.length === 7) return `Every day at ${trigger.time}`
  if (days.length === 5 && [1, 2, 3, 4, 5].every(day => days.includes(day))) return `Weekdays at ${trigger.time}`
  return `${days.map(day => DAY_NAMES[day]).join(', ')} at ${trigger.time}`
}

const formatWhen = (ms: number) => new Date(ms).toLocaleString(undefined, { weekday: 'short', day: 'numeric', month: 'short', hour: '2-digit', minute: '2-digit' })

function describeLastRun(schedule: AutomationSchedule): string {
  const skipped = schedule.lastSkipped > 0 ? ` · Skipped ${schedule.lastSkipped} missed ${schedule.lastSkipped === 1 ? 'run' : 'runs'}` : ''
  if (!schedule.lastRunMs) return skipped.replace(/^ · /, '')
  const result = schedule.lastResult === 'started' ? 'started a chat'
    : schedule.lastResult === 'awaitingApproval' ? 'chat waiting for approval'
      : schedule.lastResult === 'starting' ? 'starting a chat'
        : schedule.lastResult === 'failed' ? `failed: ${schedule.lastError || 'no chat was started.'}` : ''
  return `Last ${formatWhen(schedule.lastRunMs)}${result ? `, ${result}` : ''}${skipped}`
}

function describeStatus(schedule: AutomationSchedule): string {
  const next = !schedule.paused && schedule.nextRunMs > 0 ? `Next ${formatWhen(schedule.nextRunMs)}` : ''
  return [next, describeLastRun(schedule)].filter(Boolean).join(' · ')
}

interface Draft {
  name: string
  prompt: string
  workspaceId: string
  providerId: string
  modelId: string
  repeat: string
  days: number[]
  time: string
  missedPolicy: 'skip' | 'runOnce'
  trusted: boolean
  gitPermission: string
  maxIterations: string
}

const emptyDraft = (workspaceId: string, providerId: string): Draft => ({
  name: '', prompt: '', workspaceId, providerId, modelId: '', repeat: '1440', days: [1, 2, 3, 4, 5], time: '09:00',
  missedPolicy: 'skip', trusted: false, gitPermission: 'commit', maxIterations: '5',
})

const draftFrom = (schedule: AutomationSchedule): Draft => ({
  name: schedule.name,
  prompt: schedule.request.prompt,
  workspaceId: schedule.request.workspaceId,
  providerId: schedule.request.providerId,
  modelId: schedule.request.modelId ?? '',
  repeat: schedule.trigger.kind === 'interval' ? String(schedule.trigger.minutes) : 'weekly',
  days: schedule.trigger.kind === 'weekly' ? schedule.trigger.days : [1, 2, 3, 4, 5],
  time: schedule.trigger.kind === 'weekly' ? schedule.trigger.time : '09:00',
  missedPolicy: schedule.missedPolicy,
  trusted: schedule.trusted,
  gitPermission: schedule.request.gitPermission,
  maxIterations: String(schedule.request.maxIterations),
})

function specFrom(draft: Draft) {
  const trigger: ScheduleTrigger = draft.repeat === 'weekly'
    ? { kind: 'weekly', days: [...draft.days].sort(), time: draft.time }
    : { kind: 'interval', minutes: Number(draft.repeat) }
  return {
    name: draft.name.trim(),
    trusted: draft.trusted,
    trigger,
    missedPolicy: draft.missedPolicy,
    request: {
      prompt: draft.prompt.trim(), workspaceId: draft.workspaceId, providerId: draft.providerId, modelId: draft.modelId.trim(),
      title: draft.name.trim(), maxIterations: Number(draft.maxIterations), gitPermission: draft.gitPermission,
    },
  }
}

const iterationsValid = (value: string) => /^\d+$/.test(value) && Number(value) >= 1 && Number(value) <= 200

/** Automation schedules: stored automation-API chat requests that UAM runs on local timers, listed with their last and next run. */
export function AutomationSchedules() {
  const folders = useAppStore(useShallow(state => state.folders))
  const providers = useAppStore(useShallow(state => state.providers))
  // A provider that answers its own permission prompts cannot be held to a Git permission, so it is not offered.
  const providerOptions = providers.filter(provider => provider.supportsStructured && provider.structuredPermissionControl !== 'provider')
    .map(provider => ({ value: provider.id, label: provider.name }))
  const workspaceOptions = folders.map(folder => ({ value: folder.id, label: folder.name }))
  const [schedules, setSchedules] = useState<AutomationSchedule[] | null>(null)
  const [editing, setEditing] = useState<AutomationSchedule | 'new' | null>(null)
  const [draft, setDraft] = useState<Draft>(() => emptyDraft('', ''))
  const [busy, setBusy] = useState(false)
  const [deleting, setDeleting] = useState<AutomationSchedule | null>(null)
  const [pausing, setPausing] = useState<string[]>([])
  const [loadError, setLoadError] = useState('')

  const load = useCallback(async (reportFailure: boolean) => {
    const response = await sendToCEF<{ schedules: AutomationSchedule[] }>({ action: 'getAutomationSchedules' })
    if (response.ok && response.data) {
      setSchedules(Array.isArray(response.data.schedules) ? response.data.schedules : [])
      setLoadError('')
    } else {
      setSchedules(current => current ?? [])
      // Only the first load decides that schedules are unavailable; a failed background refresh keeps the shown list.
      if (reportFailure) setLoadError(response.error || 'Could not load automation schedules.')
      if (reportFailure) reportAppFailure(`Could not load automation schedules. ${response.error || 'Try again.'}`, '')
    }
  }, [])

  useEffect(() => {
    void load(true)
    // Runs happen while the page is open; their last and next times are re-read every 30 seconds.
    const timer = window.setInterval(() => { void load(false) }, 30_000)
    return () => window.clearInterval(timer)
  }, [load])

  const replace = (next: AutomationSchedule) => setSchedules(current => {
    const list = current ?? []
    return list.some(value => value.id === next.id) ? list.map(value => value.id === next.id ? next : value) : [...list, next]
  })
  const open = (schedule: AutomationSchedule | 'new') => {
    setDraft(schedule === 'new' ? emptyDraft(workspaceOptions[0]?.value ?? '', providerOptions[0]?.value ?? '') : draftFrom(schedule))
    setEditing(schedule)
  }
  const close = () => { if (!busy) setEditing(null) }
  const update = (patch: Partial<Draft>) => setDraft(current => ({ ...current, ...patch }))
  const save = async () => {
    setBusy(true)
    const id = editing !== null && editing !== 'new' ? editing.id : ''
    const response = await sendToCEF<{ schedule: AutomationSchedule }>({ action: 'saveAutomationSchedule', payload: { id, schedule: specFrom(draft) } })
    setBusy(false)
    if (!response.ok || !response.data) {
      // The dialog stays open with the draft, so the user can correct it.
      reportAppFailure(`The schedule was not saved. ${response.error || 'Try again.'}`, '')
      return
    }
    replace(response.data.schedule)
    setEditing(null)
  }
  const setPaused = async (schedule: AutomationSchedule, paused: boolean) => {
    setPausing(current => [...current, schedule.id])
    const response = await sendToCEF<{ schedule: AutomationSchedule }>({ action: 'setAutomationSchedulePaused', payload: { id: schedule.id, paused } })
    setPausing(current => current.filter(id => id !== schedule.id))
    if (!response.ok || !response.data) {
      reportAppFailure(`The schedule was not ${paused ? 'paused' : 'resumed'}. ${response.error || 'Try again.'}`, '')
      return
    }
    replace(response.data.schedule)
  }
  const remove = async (schedule: AutomationSchedule) => {
    setDeleting(null)
    const response = await sendToCEF({ action: 'deleteAutomationSchedule', payload: { id: schedule.id } })
    if (!response.ok) { reportAppFailure(`The schedule was not deleted. ${response.error || 'Try again.'}`, ''); return }
    setSchedules(current => current?.filter(value => value.id !== schedule.id) ?? [])
  }

  const weekly = draft.repeat === 'weekly'
  const valid = draft.name.trim() !== '' && draft.prompt.trim() !== '' && draft.workspaceId !== '' && draft.providerId !== '' && iterationsValid(draft.maxIterations) &&
    (!weekly || (draft.days.length > 0 && /^([01]\d|2[0-3]):[0-5]\d$/.test(draft.time)))
  // An edit that changes nothing keeps Save disabled, as every settings save does.
  const unchanged = editing !== null && editing !== 'new' && JSON.stringify(specFrom(draft)) === JSON.stringify(specFrom(draftFrom(editing)))
  const workspaceName = (id: string) => folders.find(folder => folder.id === id)?.name ?? 'Missing workspace'

  return <>
    <SettingsSection title="Schedules" actions={<Button size="sm" variant="secondary" leadingIcon={<Plus size={14} aria-hidden />} disabled={loadError !== ''} onClick={() => open('new')}>New schedule</Button>}>
      {schedules === null && <SettingsLoading label="Loading automation schedules" rows={2} />}
      {schedules !== null && loadError !== '' && schedules.length === 0 && <SettingRow title="Schedules unavailable" status={loadError} />}
      {schedules !== null && loadError === '' && schedules.length === 0 && <SettingRow title={<span style={{ color: 'var(--text-3)' }}>No schedules; they run while UAM is open, even with Phone access off.</span>} />}
      {schedules?.map(schedule => <SettingRow key={schedule.id} title={schedule.name}
        description={`${describeTrigger(schedule.trigger)} · ${workspaceName(schedule.request.workspaceId)} · ${schedule.trusted ? 'Starts straight away' : 'Waits for approval'}`}
        status={describeStatus(schedule) || undefined}
        control={<div className="flex items-center gap-1">
          <Switch hideLabel label={`Run ${schedule.name} on schedule`} checked={!schedule.paused} disabled={pausing.includes(schedule.id)} onChange={event => void setPaused(schedule, !event.currentTarget.checked)} />
          <IconButton size="sm" icon={<Pencil size={14} />} label={`Edit ${schedule.name}`} onClick={() => open(schedule)} />
          <IconButton size="sm" icon={<X size={14} />} label={`Delete ${schedule.name}`} onClick={() => setDeleting(schedule)} />
        </div>} />)}
    </SettingsSection>

    <Presence open={editing !== null}>
      <Overlay open zIndex={60} onBackdropMouseDown={close}>
        <div role="dialog" aria-modal="true" data-settings-owned-overlay aria-label={editing === 'new' ? 'New schedule' : 'Edit schedule'} tabIndex={-1} className="uam-mcp-dialog uam-central-dialog"
          onKeyDown={event => { if (event.key === 'Escape') { event.preventDefault(); event.stopPropagation(); close() } }}>
          <header className="uam-central-dialog__head">
            <span className="text-sm font-semibold">{editing === 'new' ? 'New schedule' : 'Edit schedule'}</span>
            <IconButton size="sm" icon={<X size={14} />} label="Close schedule dialog" disabled={busy} onClick={close} />
          </header>
          <div className="uam-central-dialog__body">
            <SettingsField label="Name">
              <input autoFocus aria-label="Schedule name" className="uam-field" value={draft.name} placeholder="Nightly triage" maxLength={80} onChange={event => update({ name: event.currentTarget.value })} />
            </SettingsField>
            <SettingsField label="Prompt">
              <textarea aria-label="Schedule prompt" className="uam-field" rows={4} value={draft.prompt} placeholder="Summarise yesterday's failing tests and fix the first one."
                onChange={event => update({ prompt: event.currentTarget.value })} />
            </SettingsField>
            <SettingsField label="Workspace">
              <MenuSelect label="Workspace" value={draft.workspaceId} options={workspaceOptions} placeholder="Choose a workspace" onChange={value => update({ workspaceId: value })} />
            </SettingsField>
            <SettingsField label="Provider">
              <MenuSelect label="Provider" value={draft.providerId} options={providerOptions} placeholder="Choose a provider" onChange={value => update({ providerId: value })} />
            </SettingsField>
            <SettingsField label="Model (optional)">
              <input aria-label="Model" className="uam-field" value={draft.modelId} placeholder="Provider default" maxLength={256} onChange={event => update({ modelId: event.currentTarget.value })} />
            </SettingsField>
            <SettingsField label="Repeat">
              <MenuSelect label="Repeat" value={draft.repeat} options={REPEAT_OPTIONS} onChange={value => update({ repeat: value })} />
            </SettingsField>
            {weekly && <SettingsField label="Days and time">
              <div className="flex flex-wrap items-center gap-1" role="group" aria-label="Days">
                {DAY_ORDER.map(day => <Chip key={day} size="sm" selected={draft.days.includes(day)} aria-label={DAY_LABELS[day]}
                  onClick={() => update({ days: draft.days.includes(day) ? draft.days.filter(value => value !== day) : [...draft.days, day] })}>{DAY_NAMES[day]}</Chip>)}
                <input type="time" aria-label="Time" className="uam-field" style={{ width: 120 }} value={draft.time} onChange={event => update({ time: event.currentTarget.value })} />
              </div>
            </SettingsField>}
            <SettingsField label="Runs missed while UAM was closed">
              <MenuSelect label="Runs missed while UAM was closed" value={draft.missedPolicy} options={MISSED_OPTIONS} onChange={value => update({ missedPolicy: value === 'runOnce' ? 'runOnce' : 'skip' })} />
            </SettingsField>
            <SettingRow title="Start chats straight away" description="Off: each chat waits in Active until you approve it."
              control={<Switch hideLabel label="Start chats straight away" checked={draft.trusted} onChange={event => update({ trusted: event.currentTarget.checked })} />} />
            <SettingsField label="Git access">
              <MenuSelect label="Git access" value={draft.gitPermission} options={GIT_OPTIONS} onChange={value => update({ gitPermission: value })} />
            </SettingsField>
            <SettingsField label="Most goal loop iterations per chat">
              <input aria-label="Most goal loop iterations per chat" className="uam-field" type="number" min={1} max={200} step={1} value={draft.maxIterations}
                aria-invalid={!iterationsValid(draft.maxIterations) || undefined} onChange={event => update({ maxIterations: event.currentTarget.value })} />
            </SettingsField>
            <div className="flex justify-end gap-2">
              <Button size="sm" variant="ghost" disabled={busy} onClick={close}>Cancel</Button>
              <Button size="sm" disabled={busy || !valid || unchanged} onClick={() => void save()}>{busy ? 'Saving…' : 'Save schedule'}</Button>
            </div>
          </div>
        </div>
      </Overlay>
    </Presence>

    <ConfirmDialog open={deleting !== null} tone="danger" title="Delete this schedule?" label="Delete automation schedule" confirmLabel="Delete schedule"
      onCancel={() => setDeleting(null)} onConfirm={() => { if (deleting) void remove(deleting) }}>
      {deleting?.name} stops running. Chats it already started are kept.
    </ConfirmDialog>
  </>
}
