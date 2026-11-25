import { useId } from 'react'
import type { ReactNode } from 'react'
import { StatusIndicator } from './StatusIndicator'
import { StatusDot, type StatusTone } from '../ui'
import { blockDisabledClick } from '../ui/Button'

export interface SelectionGridOption {
  id: string
  label: string
  icon: ReactNode
  disabled?: boolean
  /** Why the option cannot be chosen or needs attention; shown as the warning mark and read as its description. */
  issues?: string[]
  /** Secondary mono line, such as an SSH alias. */
  detail?: string
  tone?: StatusTone
  /** Words for the `tone` dot (e.g. "Offline"), so the state is not conveyed by colour alone. */
  status?: string
}

/** The text a screen reader hears after the option's name: detail, status and every issue. */
export function selectionOptionDescription(option: Pick<SelectionGridOption, 'detail' | 'status' | 'issues'>): string {
  const issues = [...new Set((option.issues ?? []).map((issue) => issue.trim()).filter(Boolean))]
  return [option.detail, option.status, ...issues]
    .map((part) => part?.trim() ?? '')
    .filter(Boolean)
    .map((part) => /[.!?]$/.test(part) ? part : `${part}.`)
    .join(' ')
}

/**
 * Icon and name choices shared by host, provider and setup workflows. `detail` (mono), `tone` with `status`, and
 * `issues` are optional and are also the option's accessible description. An option disabled with issues stays
 * focusable (aria-disabled) so keyboard users can hear why, like Button's `disabledReason`.
 */
export function SelectionGrid({ label, options, value, onChange }: {
  label: string
  options: SelectionGridOption[]
  value: string
  onChange: (id: string) => void
}) {
  const idPrefix = useId()
  return <div role="group" aria-label={label} className="uam-selection-grid">
    {/* ui: raw button kept — multi-line selection card (icon, label, detail, status); no matching primitive */}
    {options.map((option, index) => {
      const description = selectionOptionDescription(option)
      const descriptionId = `${idPrefix}-${index}`
      const explained = Boolean(option.disabled && option.issues?.some((issue) => issue.trim()))
      return <button type="button" key={option.id}
        aria-label={option.label} aria-pressed={option.id === value}
        aria-describedby={description ? descriptionId : undefined}
        disabled={option.disabled && !explained}
        aria-disabled={explained || undefined}
        onClick={explained ? blockDisabledClick : () => onChange(option.id)} className="uam-selection-option">
        <span className="uam-selection-icon">{option.icon}</span>
        <span className="flex max-w-full items-center gap-1.5 truncate">
          {option.tone && <StatusDot tone={option.tone} size={6} />}
          <span className="truncate">{option.label}</span>
        </span>
        {option.detail && <span className="max-w-full truncate font-mono" style={{ fontSize: 'var(--fs-xs)', color: 'var(--text-3)' }}>{option.detail}</span>}
        {!!option.issues?.length && <span className="uam-selection-status"><StatusIndicator issues={option.issues} focusable={false} /></span>}
        {description && <span id={descriptionId} hidden>{description}</span>}
      </button>
    })}
  </div>
}
