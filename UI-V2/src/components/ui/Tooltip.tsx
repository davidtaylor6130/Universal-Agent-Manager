import * as RadixTooltip from '@radix-ui/react-tooltip'
import type { ReactNode } from 'react'

export interface TooltipProps {
  /** The tooltip text. When empty/undefined the trigger renders without a tooltip. */
  label?: ReactNode
  children: ReactNode
  side?: 'top' | 'right' | 'bottom' | 'left'
  align?: 'start' | 'center' | 'end'
  /** Keyboard shortcut hint rendered as a mono chip after the label. */
  shortcut?: string
}

/**
 * Styled, accessible tooltip. Wrap any control that needs a label — icon
 * buttons, truncated text, technical fields. Prefer this over native title=.
 */
export function Tooltip({ label, children, side = 'top', align = 'center', shortcut }: TooltipProps) {
  if (label === undefined || label === null || label === '') {
    return <>{children}</>
  }
  return (
    // Each tooltip owns its timing, including standalone renders and tests.
    <RadixTooltip.Provider delayDuration={350} skipDelayDuration={200}>
    <RadixTooltip.Root>
      <RadixTooltip.Trigger asChild>{children}</RadixTooltip.Trigger>
      <RadixTooltip.Portal>
        <RadixTooltip.Content
          side={side}
          align={align}
          sideOffset={6}
          className="uam-tooltip"
        >
          <span>{label}</span>
          {shortcut ? <kbd className="uam-tooltip__kbd">{shortcut}</kbd> : null}
          <RadixTooltip.Arrow className="uam-tooltip__arrow" width={10} height={5} />
        </RadixTooltip.Content>
      </RadixTooltip.Portal>
    </RadixTooltip.Root>
    </RadixTooltip.Provider>
  )
}
