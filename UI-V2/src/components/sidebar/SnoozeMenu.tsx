import { useState } from 'react'
import type { FormEvent } from 'react'
import { AlarmClockOff } from 'lucide-react'
import { useAppStore } from '../../store/useAppStore'
import { Button } from '../ui'
import { useShallow } from 'zustand/react/shallow'
import { runForEach, type BulkTarget } from '../../utils/runForEach'

const MINUTE = 60_000
const HOUR = 60 * MINUTE
const DAY = 24 * HOUR
export const SNOOZE_UNIT_MS = { minutes: MINUTE, hours: HOUR, days: DAY } as const
export type SnoozeUnit = keyof typeof SNOOZE_UNIT_MS

export function snoozePresets(now = new Date()) {
  const tomorrowMorning = new Date(now); tomorrowMorning.setDate(tomorrowMorning.getDate() + 1); tomorrowMorning.setHours(9, 0, 0, 0)
  const at = (ms: number) => now.getTime() + ms
  return [
    { label: '5 minutes', until: at(5 * MINUTE) },
    { label: '10 minutes', until: at(10 * MINUTE) },
    { label: '15 minutes', until: at(15 * MINUTE) },
    { label: '30 minutes', until: at(30 * MINUTE) },
    { label: '1 hour', until: at(HOUR) },
    { label: '3 hours', until: at(3 * HOUR) },
    { label: 'Tomorrow morning', until: tomorrowMorning.getTime() },
    { label: '1 day', until: at(DAY) },
    { label: '2 days', until: at(2 * DAY) },
    { label: '1 week', until: at(7 * DAY) },
  ]
}

/** Wake time for a whole-number amount from now, or null when the amount is not usable. */
export function customSnoozeUntil(amount: number, unit: SnoozeUnit, now = Date.now()) {
  if (!Number.isSafeInteger(amount) || amount < 1) return null
  const until = now + amount * SNOOZE_UNIT_MS[unit]
  return Number.isSafeInteger(until) ? until : null
}

const awaitsUser = (binding: ReturnType<typeof useAppStore.getState>['acpBindingBySessionId'][string] | undefined) =>
  !!((binding?.attentionKind && binding.attentionKind !== 'error') || binding?.pendingPermission || binding?.pendingUserInput || binding?.queuedPrompts?.length)
const BLOCKED_TEXT = 'Respond to pending requests or queued messages first.'

/** Snooze items for a chat, or for every selected chat row when `targets` lists more than one. Rendered inside the Snooze submenu and the Active-row snooze popup. */
export function SnoozeMenu({ chatId, familyIds, untilMs = 0, onSnoozed, targets }: { chatId: string; familyIds: string[]; untilMs?: number; onSnoozed?: () => void; targets?: BulkTarget[] }) {
  const save = useAppStore((state) => state.setChatSnoozedUntil)
  const bulk = targets && targets.length > 1 ? targets : null
  // A row is blocked while any chat in its branch family is waiting for the user.
  const blockedIds = useAppStore(useShallow((state) => bulk
    ? bulk.filter(({ id }) => state.sessions.some((chat) => (chat.id === id || (chat.branchRootChatId || chat.parentChatId || chat.id) === id) && awaitsUser(state.acpBindingBySessionId[chat.id]))).map(({ id }) => id)
    : familyIds.some((id) => awaitsUser(state.acpBindingBySessionId[id])) ? [chatId] : []))
  const untilByRow = useAppStore(useShallow((state) => bulk ? bulk.map(({ id }) => state.sessions.find((chat) => chat.id === id)?.snoozedUntilMs ?? 0) : [untilMs]))
  const blocked = blockedIds.length === (bulk?.length ?? 1)
  const [customOpen, setCustomOpen] = useState(false)
  const [amount, setAmount] = useState('1')
  const [unit, setUnit] = useState<SnoozeUnit>('hours')
  const [saving, setSaving] = useState(false)
  const [feedback, setFeedback] = useState<{ error: boolean; text: string } | null>(null)
  const snoozedCount = untilByRow.filter((until) => until > Date.now()).length
  const snoozed = snoozedCount > 0
  const update = async (until: number) => {
    if (saving) return
    if (until !== 0 && (!Number.isSafeInteger(until) || until <= Date.now())) {
      setFeedback({ error: true, text: 'Choose a future wake time.' }); return
    }
    setSaving(true); setFeedback(null)
    try {
      if (bulk) {
        const chosen = until === 0 ? bulk.filter((_, index) => untilByRow[index] > Date.now()) : bulk
        const done = await runForEach(chosen, async ({ id }) => {
          if (until !== 0 && blockedIds.includes(id)) return BLOCKED_TEXT
          const result = await save(id, until)
          return result.ok ? null : result.error || 'Could not save. Try again.'
        }, until === 0 ? 'Could not wake chats' : 'Could not snooze chats')
        const failed = chosen.length - done.length
        setFeedback(failed > 0
          ? { error: true, text: `${failed} of ${chosen.length} chats could not be ${until === 0 ? 'woken' : 'snoozed'}.` }
          : { error: false, text: until === 0 ? `${done.length} chats awake.` : `${done.length} chats snoozed until ${new Date(until).toLocaleString()}.` })
        if (failed === 0 && until !== 0) onSnoozed?.()
        return
      }
      const result = await save(chatId, until)
      setFeedback(result.ok ? { error: false, text: until === 0 ? 'Chat awake.' : `Snoozed until ${new Date(until).toLocaleString()}.` } : { error: true, text: result.error || 'Could not save. Try again.' })
      if (result.ok && until !== 0) onSnoozed?.()
    } catch { setFeedback({ error: true, text: 'Could not save. Try again.' }) }
    finally { setSaving(false) }
  }
  const submitCustom = (event: FormEvent) => {
    event.preventDefault()
    const until = customSnoozeUntil(Number(amount), unit)
    if (until === null) { setFeedback({ error: true, text: 'Enter a whole number of minutes, hours or days.' }); return }
    void update(until)
  }
  const itemClass = 'uam-sb-menu-item'
  const presetsBlocked = saving || blocked
  return <>
    {snoozed && <button type="button" role="menuitem" className={itemClass} disabled={saving} onClick={() => { void update(0) }}>
      <AlarmClockOff size={13} aria-hidden />Wake now
    </button>}
    {snoozed && <p className="px-3 pb-1 text-xs" style={{ color: 'var(--text-3)' }}>{bulk ? `${snoozedCount} of ${bulk.length} chats snoozed` : `Snoozed until ${new Date(untilMs).toLocaleString()}`}</p>}
    {blocked && !snoozed && <p className="px-3 pb-1 text-xs" style={{ color: 'var(--text-3)' }}>{BLOCKED_TEXT}</p>}
    {bulk && !blocked && blockedIds.length > 0 && <p className="px-3 pb-1 text-xs" style={{ color: 'var(--text-3)' }}>{blockedIds.length} of {bulk.length} chats need a response first and stay awake.</p>}
    {snoozePresets().map((preset) => <button key={preset.label} type="button" role="menuitem" className={itemClass} disabled={presetsBlocked}
      onClick={() => { void update(preset.until) }}>{preset.label}<span className="ml-auto text-xs" style={{ color: 'var(--text-3)' }}>{new Date(preset.until).toLocaleString([], { weekday: 'short', hour: 'numeric', minute: '2-digit' })}</span></button>)}
    <button type="button" role="menuitem" className={itemClass} aria-expanded={customOpen} disabled={saving} onClick={() => setCustomOpen((open) => !open)}>Custom…</button>
    {customOpen && <form aria-label="Custom snooze" noValidate className="uam-reveal flex items-center gap-1.5 px-3 py-1.5" onSubmit={submitCustom}>
      <input aria-label="Snooze amount" type="number" min={1} step={1} inputMode="numeric" value={amount} disabled={presetsBlocked}
        onChange={(event) => setAmount(event.target.value)} className="w-16 px-2 py-1 text-xs" style={{ background: 'var(--surface)', color: 'var(--text)', border: '1px solid var(--border)', borderRadius: 'var(--r-sm)' }} />
      <select aria-label="Snooze unit" value={unit} disabled={presetsBlocked} onChange={(event) => setUnit(event.target.value as SnoozeUnit)}
        className="px-1.5 py-1 text-xs" style={{ background: 'var(--surface)', color: 'var(--text)', border: '1px solid var(--border)', borderRadius: 'var(--r-sm)' }}>
        <option value="minutes">minutes</option>
        <option value="hours">hours</option>
        <option value="days">days</option>
      </select>
      <Button type="submit" size="sm" variant="primary" className="ml-auto" disabled={presetsBlocked}>Snooze</Button>
    </form>}
    {feedback && <p role={feedback.error ? 'alert' : 'status'} className="px-3 py-1 text-xs" style={{ color: feedback.error ? 'var(--error)' : 'var(--text-2)' }}>{feedback.text}</p>}
  </>
}
