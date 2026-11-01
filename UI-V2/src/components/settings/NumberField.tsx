import { useEffect, useId, useState } from 'react'
import { Tooltip } from '../ui'

/** Number input that commits a clamped value on blur or Enter, never per keystroke. Escape reverts. */
export function NumberField({ label, value, min, max, step = 1, unit, onCommit }: {
  label: string
  value: number
  min: number
  max: number
  step?: number
  unit?: string
  onCommit: (value: number) => void
}) {
  const [draft, setDraft] = useState(String(value))
  const rangeId = useId()
  const range = `${min.toLocaleString()}–${max.toLocaleString()}${unit ? ` ${unit}` : ''}`
  useEffect(() => { setDraft(String(value)) }, [value])
  const parsed = Number(draft)
  const invalid = draft.trim() === '' || !Number.isFinite(parsed) || parsed < min || parsed > max
  const commit = () => {
    if (draft.trim() === '' || !Number.isFinite(parsed)) { setDraft(String(value)); return }
    const next = Math.min(max, Math.max(min, Math.round(parsed / step) * step))
    setDraft(String(next))
    if (next !== value) onCommit(next)
  }
  return <span className="relative flex items-center">
    <Tooltip label={range}>
      <input type="number" aria-label={label} aria-invalid={invalid || undefined} aria-describedby={rangeId} min={min} max={max} step={step} value={draft}
        onChange={event => setDraft(event.currentTarget.value)}
        onBlur={commit}
        onKeyDown={event => { if (event.key === 'Enter') event.currentTarget.blur(); if (event.key === 'Escape') { setDraft(String(value)); event.currentTarget.blur() } }}
        className="uam-field w-full tabular-nums" style={{ height: 34, paddingRight: unit ? 64 : undefined, ...(invalid ? { borderColor: 'var(--error)' } : {}) }} />
    </Tooltip>
    {unit && <span aria-hidden className="pointer-events-none absolute right-3 text-xs" style={{ color: 'var(--text-3)' }}>{unit}</span>}
    <span id={rangeId} className="sr-only">{range}</span>
  </span>
}
