import type { InputHTMLAttributes } from 'react'
import { cx } from './cx'

export interface SwitchProps extends Omit<InputHTMLAttributes<HTMLInputElement>, 'type'> {
  label: string
  hideLabel?: boolean
}

export function Switch({ label, hideLabel = false, className, ...props }: SwitchProps) {
  return (
    <label className={cx('uam-switch', className)}>
      <input type="checkbox" aria-label={label} {...props} />
      <span className="uam-switch__track" aria-hidden><span className="uam-switch__thumb" /></span>
      <span className={hideLabel ? 'sr-only' : 'uam-switch__label'}>{label}</span>
    </label>
  )
}
