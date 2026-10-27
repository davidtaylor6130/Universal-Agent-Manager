import { useEffect, useRef, useState } from 'react'
import type { KeyboardEvent as ReactKeyboardEvent, ReactNode } from 'react'
import { ChevronRight } from 'lucide-react'
import { ViewportMenu, scaledMs } from '../ui'

const HOVER_OPEN_DELAY = 150 // hover intent: sweeping across rows must not flash submenus
const HOVER_CLOSE_DELAY = 120
const EXIT_MS = 170 // matches --dur-exit; the submenu stays mounted while it fades out

type Phase = 'closed' | 'open' | 'closing'

/** Submenu row for chat/folder context menus: hover-intent open, ArrowRight/ArrowLeft and Escape, edge-aware placement. */
export function RowSubmenu({ label, ariaLabel, icon, children }: { label: string; ariaLabel: string; icon: ReactNode; children: ReactNode }) {
  const [phase, setPhase] = useState<Phase>('closed')
  const triggerRef = useRef<HTMLButtonElement>(null)
  const timerRef = useRef<ReturnType<typeof setTimeout> | null>(null)
  const clearTimer = () => {
    if (timerRef.current) clearTimeout(timerRef.current)
    timerRef.current = null
  }
  const later = (ms: number, action: () => void) => {
    clearTimer()
    timerRef.current = setTimeout(action, ms)
  }
  const open = () => { clearTimer(); setPhase('open') }
  const close = (restoreFocus = false) => {
    clearTimer()
    if (restoreFocus) triggerRef.current?.focus()
    if (phase === 'closed') return
    setPhase('closing')
    timerRef.current = setTimeout(() => setPhase('closed'), scaledMs(EXIT_MS))
  }
  const scheduleClose = () => later(HOVER_CLOSE_DELAY, () => close())

  useEffect(() => () => clearTimer(), [])

  const onTriggerKeyDown = (event: ReactKeyboardEvent<HTMLButtonElement>) => {
    if (event.key !== 'ArrowRight' && event.key !== 'Enter' && event.key !== ' ') return
    event.preventDefault()
    event.stopPropagation()
    open()
  }
  const onSubmenuKeyDown = (event: ReactKeyboardEvent<HTMLDivElement>) => {
    if (event.key !== 'ArrowLeft') return
    event.preventDefault()
    event.stopPropagation()
    close(true)
  }

  return (
    <>
      <button
        ref={triggerRef}
        type="button"
        role="menuitem"
        aria-haspopup="menu"
        aria-expanded={phase === 'open'}
        className="uam-sb-menu-item"
        onClick={open}
        onKeyDown={onTriggerKeyDown}
        onMouseEnter={() => { if (phase === 'open') clearTimer(); else later(HOVER_OPEN_DELAY, open) }}
        onMouseLeave={() => { if (phase === 'closed') clearTimer(); else scheduleClose() }}
      >
        {icon}
        <span className="flex-1">{label}</span>
        <ChevronRight size={13} aria-hidden />
      </button>
      {phase !== 'closed' && (
        <ViewportMenu
          anchorRef={triggerRef}
          side="right"
          role="menu"
          aria-label={ariaLabel}
          data-state={phase}
          className="uam-sb-submenu rounded-md py-1"
          style={{ minWidth: 170, background: 'var(--surface-up)', border: '1px solid var(--border-bright)', boxShadow: 'var(--elev-2)' }}
          onRequestClose={() => close(true)}
          onKeyDown={onSubmenuKeyDown}
          onMouseEnter={clearTimer}
          onMouseLeave={scheduleClose}
        >
          {children}
        </ViewportMenu>
      )}
    </>
  )
}
