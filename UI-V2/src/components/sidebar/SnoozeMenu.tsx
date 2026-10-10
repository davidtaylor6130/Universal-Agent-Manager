import { useState } from 'react'
import { Clock } from 'lucide-react'
import { useAppStore } from '../../store/useAppStore'
import { useShallow } from 'zustand/react/shallow'

export function snoozePresets(now = new Date()) {
  const evening = new Date(now); evening.setHours(18, 0, 0, 0)
  const tomorrow = new Date(now); tomorrow.setDate(tomorrow.getDate() + 1); tomorrow.setHours(9, 0, 0, 0)
  const nextWeek = new Date(now); nextWeek.setDate(nextWeek.getDate() + ((8 - nextWeek.getDay()) % 7 || 7)); nextWeek.setHours(9, 0, 0, 0)
  return [
    { label: '1 hour', until: now.getTime() + 3_600_000 },
    { label: '3 hours', until: now.getTime() + 10_800_000 },
    ...(evening.getTime() - now.getTime() > 3_600_000 ? [{ label: 'This evening', until: evening.getTime() }] : []),
    { label: 'Tomorrow', until: tomorrow.getTime() },
    { label: 'Next week', until: nextWeek.getTime() },
  ]
}

export function SnoozeMenu({ chatId, familyIds, untilMs = 0 }: { chatId: string; familyIds: string[]; untilMs?: number }) {
  const save = useAppStore((state) => state.setChatSnoozedUntil)
  const blocked = useAppStore(useShallow((state) => familyIds.some((id) => {
    const binding = state.acpBindingBySessionId[id]
    return !!((binding?.attentionKind && binding.attentionKind !== 'error') || binding?.pendingPermission || binding?.pendingUserInput || binding?.queuedPrompts?.length)
  })))
  const [expanded, setExpanded] = useState(false)
  const [custom, setCustom] = useState('')
  const [saving, setSaving] = useState(false)
  const [feedback, setFeedback] = useState<{ error: boolean; text: string } | null>(null)
  const snoozed = untilMs > Date.now()
  const update = async (until: number) => {
    if (saving) return
    if (until !== 0 && (!Number.isSafeInteger(until) || until <= Date.now())) {
      setFeedback({ error: true, text: 'Choose a future wake time.' }); return
    }
    setSaving(true); setFeedback(null)
    try {
      const result = await save(chatId, until)
      setFeedback(result.ok ? { error: false, text: until === 0 ? 'Chat awake.' : `Snoozed until ${new Date(until).toLocaleString()}.` } : { error: true, text: result.error || 'Could not save. Try again.' })
      if (result.ok) setExpanded(false)
    } catch { setFeedback({ error: true, text: 'Could not save. Try again.' }) }
    finally { setSaving(false) }
  }
  const style = { background: 'transparent', color: 'var(--text-2)', border: 'none', fontFamily: 'inherit' }
  const buttonClass = 'flex w-full items-center gap-2 px-3 py-1.5 text-left text-sm disabled:opacity-50 hover:bg-[var(--surface-hover)]'
  return <>
    <button type="button" role="menuitem" className={buttonClass} style={style} disabled={saving || (!snoozed && blocked)}
      aria-expanded={!snoozed ? expanded : undefined} title={!snoozed && blocked ? 'Respond to pending requests or queued messages first.' : undefined}
      onClick={() => { if (snoozed) void update(0); else setExpanded((value) => !value) }}>
      <Clock size={13} aria-hidden />{saving ? 'Saving…' : snoozed ? 'Wake chat' : 'Snooze'}
    </button>
    {snoozed && <p className="px-3 py-1 text-xs" style={{ color: 'var(--text-2)' }}>Until {new Date(untilMs).toLocaleString()}</p>}
    {expanded && !snoozed && <div role="group" aria-label="Wake time">
      {snoozePresets().map((preset) => <button key={preset.label} type="button" role="menuitem" className={buttonClass} style={style} disabled={saving || blocked}
        onClick={() => { void update(preset.until) }}>{preset.label}<span className="ml-auto text-xs">{new Date(preset.until).toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' })}</span></button>)}
      <div className="px-3 py-1.5"><label className="text-xs">Custom wake time<input aria-label="Custom wake time" type="datetime-local" value={custom} disabled={saving || blocked} onChange={(event) => setCustom(event.target.value)}
        className="mt-1 block w-full text-xs" style={{ background: 'var(--surface)', color: 'var(--text)', border: '1px solid var(--border)' }} /></label>
        <button type="button" className={buttonClass} style={style} disabled={saving || blocked || !custom} onClick={() => { void update(new Date(custom).getTime()) }}>Set wake time</button></div>
    </div>}
    {feedback && <p role={feedback.error ? 'alert' : 'status'} className="px-3 py-1 text-xs" style={{ color: feedback.error ? 'var(--red)' : 'var(--text-2)' }}>{feedback.text}</p>}
  </>
}
