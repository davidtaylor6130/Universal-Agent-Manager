import { createContext, useContext, useEffect, useRef, useState } from 'react'
import type { CSSProperties, ReactNode } from 'react'
import { createPortal } from 'react-dom'
import { cx } from './cx'

/** Exit animation length; keep in sync with --dur-exit in index.css. */
export const EXIT_MS = 170

function motionAllowed(): boolean {
  // No matchMedia (jsdom) or reduced motion: unmount immediately.
  return typeof window !== 'undefined' && typeof window.matchMedia === 'function' &&
    !window.matchMedia('(prefers-reduced-motion: reduce)').matches
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
    const timer = window.setTimeout(() => setMounted(false), exitMs)
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
}

/**
 * Full-viewport modal layer rendered at <body>, so no ancestor transform,
 * filter or overflow can clip it. Direct children animate in and out.
 */
export function Overlay({ open, children, onBackdropMouseDown, className, style, zIndex = 70, dim = true }: OverlayProps) {
  const { mounted, closing } = usePresence(open)
  // Keep the last open content on screen while it animates out.
  const lastChildren = useRef(children)
  if (open) lastChildren.current = children
  if (!mounted || typeof document === 'undefined') return null
  return createPortal(
    <div
      className={cx('uam-overlay fixed inset-0 flex items-center justify-center p-4', dim && 'uam-overlay--dim', className)}
      data-state={closing ? 'closed' : 'open'}
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
