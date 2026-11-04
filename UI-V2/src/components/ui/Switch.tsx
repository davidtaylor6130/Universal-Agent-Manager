import type { InputHTMLAttributes } from 'react'
import { Check, X } from 'lucide-react'
import { cx } from './cx'

export interface SwitchProps extends Omit<InputHTMLAttributes<HTMLInputElement>, 'type'> {
  label: string
  hideLabel?: boolean
  /** Secondary line under the label. */
  description?: string
}

export function Switch({ label, hideLabel = false, description, className, ...props }: SwitchProps) {
  return (
    <label className={cx('uam-switch', className)}>
      <input type="checkbox" aria-label={label} {...props} />
      {/* Both icons ride in the thumb; inside Settings CSS shows the tick when checked and the X when not (so uncontrolled switches swap too); elsewhere they stay hidden. */}
      <span className="uam-switch__track" aria-hidden>
        <span className="uam-switch__thumb">
          <Check className="uam-switch__icon uam-switch__icon--on" size={9} strokeWidth={3} />
          <X className="uam-switch__icon uam-switch__icon--off" size={9} strokeWidth={3} />
        </span>
      </span>
      {description && !hideLabel
        ? <span className="uam-switch__text"><span className="uam-switch__label">{label}</span><span className="uam-switch__description">{description}</span></span>
        : <span className={hideLabel ? 'sr-only' : 'uam-switch__label'}>{label}</span>}
    </label>
  )
}
