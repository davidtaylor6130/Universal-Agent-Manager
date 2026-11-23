import { createContext, useContext, useEffect, useRef, useState } from 'react'
import type { CSSProperties, ReactNode } from 'react'
import { createPortal } from 'react-dom'
import { cx } from './cx'
import { getTopmostModal } from '../../utils/modalFocus'

/** Exit animation length; keep in sync with --dur-exit in index.css. */
export const EXIT_MS = 170

export function motionAllowed(): boolean {
  // No matchMedia (jsdom), reduced motion or Motion Off (0 ms tokens, so no transitionend): change immediately.
  return typeof window !== 'undefined' && typeof window.matchMedia === 'function' &&
    !window.matchMedia('(prefers-reduced-motion: reduce)').matches &&
    document.documentElement.getAttribute('data-motion') !== 'off'
}

/** `ms` scaled by --motion-scale (Appearance > Animation speed), so exit timers end with the scaled --dur-* tokens. */
export function scaledMs(ms: number): number {
  if (typeof document === 'undefined') return ms
  const scale = parseFloat(getComputedStyle(document.documentElement).getPropertyValue('--motion-scale'))
  return Number.isFinite(scale) ? ms * scale : ms
}

/**
 * Keeps an element mounted for its exit animation after `open` turns false.
 * `closing` is true during that window so CSS can play the out-animation.
 */
export function usePresence(open: boolean, exitMs = EXIT_MS) {
  const [mounted, setMounted] = useState(open)
  useEffect(() => {
    if (open) { setMounted(true); return }
    if (!mounted) return
    if (!motionAllowed()) { setMounted(false); return }
    const timer = window.setTimeout(() => setMounted(false), scaledMs(exitMs))
    return () => window.clearTimeout(timer)
  }, [open, mounted, exitMs])
  return { mounted: open || mounted, closing: !open && mounted }
}

export interface OverlayProps {
  open: boolean
  children: ReactNode
  /** Called for a mousedown on the backdrop itself (not the panel). */
  onBackdropMouseDown?: () => void
  className?: string
  style?: CSSProperties
  zIndex?: number
  /** Dim + blur the page behind the panel. */
  dim?: boolean
  /**
   * Accessible name. With `label` or `labelledBy` the layer itself is the modal dialog (`role`, `aria-modal`,
   * name and description) and takes focus on open when its content has not. Without either, the content must
   * carry its own `role="dialog"` and `aria-modal` (existing panels do).
   */
  label?: string
  /** Id of the visible title that names the dialog. */
  labelledBy?: string
  /** Id of the text that describes the dialog. */
  describedBy?: string
  /** Defaults to dialog; alertdialog for a choice that interrupts. */
  role?: 'dialog' | 'alertdialog'
  /** Focus target on close when the element focused at open is gone (a closed menu item, a replaced step). */
  returnFocusTo?: () => HTMLElement | null | undefined
}

/** The focused element, or null when focus is on nothing (body). */
function focusedElement(): HTMLElement | null {
  if (typeof document === 'undefined') return null
  const active = document.activeElement
  return active instanceof HTMLElement && active !== document.body ? active : null
}

/**
 * Full-viewport modal layer rendered at <body>, so no ancestor transform,
 * filter or overflow can clip it. Direct children animate in and out.
 *
 * Every overlay returns focus on close to the element focused when it opened (or `returnFocusTo`), but only
 * when focus would otherwise be lost: still inside the closing layer or on the page body once the owner's own
 * cleanup has run. It never pulls focus out of another open modal, so a dialog replaced by the next step keeps
 * that step focused.
 */
export function Overlay({ open, children, onBackdropMouseDown, className, style, zIndex = 70, dim = true, label, labelledBy, describedBy, role = 'dialog', returnFocusTo }: OverlayProps) {
  // Inside <Presence>, the parent's exit window closes this overlay so its out-animation plays.
  const presenceOpen = useContext(OverlayStateContext) !== 'closed'
  open = open && presenceOpen
  const { mounted, closing } = usePresence(open)
  // Keep the last open content on screen while it animates out.
  const lastChildren = useRef(children)
  if (open) lastChildren.current = children
  const layerRef = useRef<HTMLDivElement>(null)
  const ownsDialog = Boolean(label || labelledBy)
  const ownsDialogRef = useRef(ownsDialog)
  ownsDialogRef.current = ownsDialog
  const returnFocusToRef = useRef(returnFocusTo)
  returnFocusToRef.current = returnFocusTo
  // Read during render, before the content's effects (or autoFocus) move focus into the dialog.
  const openerRef = useRef<HTMLElement | null>(null)
  const wasOpenRef = useRef(false)
  // Counts open-effect runs: StrictMode's mount check runs effect, cleanup, effect while still open, and the
  // restore queued by that cleanup must not pull focus back out of the dialog.
  const openRunRef = useRef(0)
  if (open !== wasOpenRef.current) {
    if (open) openerRef.current = focusedElement()
    wasOpenRef.current = open
  }

  useEffect(() => {
    if (!open) return
    const run = ++openRunRef.current
    const layer = layerRef.current
    if (ownsDialogRef.current && layer && !layer.contains(document.activeElement) && getTopmostModal() === layer) layer.focus()
    const opener = openerRef.current
    return () => {
      // A fallback: queued so an owner's own restore (its cleanup runs after this child's, such as a chat's Goals
      // trigger or a row it names) and any dialog mounted in the same commit (the next step) are already in place.
      queueMicrotask(() => {
        if (openRunRef.current !== run) return
        const active = focusedElement()
        if (active && !layer?.contains(active)) return
        const target = [opener, returnFocusToRef.current?.()].find((element): element is HTMLElement =>
          Boolean(element?.isConnected) && element !== document.body && !layer?.contains(element ?? null))
        if (!target) return
        // A dialog that replaced this one keeps focus; a parent dialog gets it back on its own control.
        const topmost = getTopmostModal()
        if (topmost && topmost !== layer && !topmost.contains(target)) return
        target.focus()
      })
    }
  }, [open])

  if (!mounted || typeof document === 'undefined') return null
  return createPortal(
    <div
      ref={layerRef}
      className={cx('uam-overlay fixed inset-0 flex items-center justify-center p-4', dim && 'uam-overlay--dim', className)}
      data-state={closing ? 'closed' : 'open'}
      role={ownsDialog ? role : undefined}
      aria-modal={ownsDialog ? 'true' : undefined}
      aria-label={ownsDialog ? label : undefined}
      aria-labelledby={ownsDialog ? labelledBy : undefined}
      aria-describedby={ownsDialog ? describedBy : undefined}
      tabIndex={ownsDialog ? -1 : undefined}
      style={{ zIndex, ...style }}
      onMouseDown={(event) => {
        if (event.target === event.currentTarget) onBackdropMouseDown?.()
      }}
    >
      {open ? children : lastChildren.current}
    </div>,
    document.body,
  )
}

const OverlayStateContext = createContext<'open' | 'closed'>('open')

/** 'closed' while a modal wrapped in <Presence> plays its exit animation. */
export function useOverlayState() {
  return useContext(OverlayStateContext)
}

/**
 * Keeps conditionally rendered modals mounted long enough to animate out.
 * The modal's root should carry `data-state={useOverlayState()}`.
 */
export function Presence({ open, children }: { open: boolean; children: ReactNode }) {
  const { mounted, closing } = usePresence(open)
  const lastChildren = useRef(children)
  if (open) lastChildren.current = children
  if (!mounted) return null
  return (
    <OverlayStateContext.Provider value={closing ? 'closed' : 'open'}>
      {open ? children : lastChildren.current}
    </OverlayStateContext.Provider>
  )
}
