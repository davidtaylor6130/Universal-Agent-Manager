import type { InputHTMLAttributes } from 'react'
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
      <span className="uam-switch__track" aria-hidden><span className="uam-switch__thumb" /></span>
      {description && !hideLabel
        ? <span className="uam-switch__text"><span className="uam-switch__label">{label}</span><span className="uam-switch__description">{description}</span></span>
        : <span className={hideLabel ? 'sr-only' : 'uam-switch__label'}>{label}</span>}
    </label>
  )
}
