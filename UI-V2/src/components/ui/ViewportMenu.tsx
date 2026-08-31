import { forwardRef, useEffect, useImperativeHandle, useLayoutEffect, useRef, useState } from 'react'
import type { CSSProperties, HTMLAttributes, RefObject } from 'react'
import { createPortal } from 'react-dom'

const VIEWPORT_GAP = 8
export const VIEWPORT_MENU_Z_INDEX = 1100

export interface ViewportMenuAnchor {
  left: number
  right: number
  top: number
  bottom: number
}

export function placeViewportMenu(
  anchor: ViewportMenuAnchor,
  menu: { width: number; height: number },
  viewport: { width: number; height: number },
  side: 'top' | 'right' | 'bottom' | 'left' = 'bottom',
  align: 'start' | 'end' = 'start',
  gap = 4,
) {
  if (side === 'left' || side === 'right') {
    const preferredLeft = side === 'right' ? anchor.right + gap : anchor.left - menu.width - gap
    const alternateLeft = side === 'right' ? anchor.left - menu.width - gap : anchor.right + gap
    const preferredFits = preferredLeft >= VIEWPORT_GAP && preferredLeft + menu.width <= viewport.width - VIEWPORT_GAP
    return {
      left: Math.min(
        Math.max(preferredFits ? preferredLeft : alternateLeft, VIEWPORT_GAP),
        Math.max(VIEWPORT_GAP, viewport.width - menu.width - VIEWPORT_GAP),
      ),
      top: Math.min(
        Math.max(align === 'end' ? anchor.bottom - menu.height : anchor.top, VIEWPORT_GAP),
        Math.max(VIEWPORT_GAP, viewport.height - menu.height - VIEWPORT_GAP),
      ),
    }
  }

  const below = anchor.bottom + gap
  const above = anchor.top - menu.height - gap
  const preferredTop = side === 'bottom' ? below : above
  const alternateTop = side === 'bottom' ? above : below
  const preferredFits = preferredTop >= VIEWPORT_GAP && preferredTop + menu.height <= viewport.height - VIEWPORT_GAP
  const top = Math.min(
    Math.max(preferredFits ? preferredTop : alternateTop, VIEWPORT_GAP),
    Math.max(VIEWPORT_GAP, viewport.height - menu.height - VIEWPORT_GAP),
  )
  const alignedLeft = align === 'end' ? anchor.right - menu.width : anchor.left
  const left = Math.min(
    Math.max(alignedLeft, VIEWPORT_GAP),
    Math.max(VIEWPORT_GAP, viewport.width - menu.width - VIEWPORT_GAP),
  )
  return { left, top }
}

export interface ViewportMenuProps extends HTMLAttributes<HTMLDivElement> {
  anchorRef?: RefObject<HTMLElement>
  point?: { x: number; y: number }
  side?: 'top' | 'right' | 'bottom' | 'left'
  align?: 'start' | 'end'
  gap?: number
  onRequestClose?: () => void
  manageFocus?: boolean
}

export const ViewportMenu = forwardRef<HTMLDivElement, ViewportMenuProps>(function ViewportMenu({
  anchorRef,
  point,
  side = 'bottom',
  align = 'start',
  gap = 4,
  onRequestClose,
  manageFocus,
  style,
  ...props
}, forwardedRef) {
  const menuRef = useRef<HTMLDivElement>(null)
  const onRequestCloseRef = useRef(onRequestClose)
  onRequestCloseRef.current = onRequestClose
  const [position, setPosition] = useState<{ left: number; top: number } | null>(null)
  useImperativeHandle(forwardedRef, () => menuRef.current as HTMLDivElement)

  useLayoutEffect(() => {
    const update = () => {
      const menu = menuRef.current
      const element = anchorRef?.current
      if (!menu || (!point && !element)) return
      const rect = element?.getBoundingClientRect()
      const anchor = point
        ? { left: point.x, right: point.x, top: point.y, bottom: point.y }
        : { left: rect!.left, right: rect!.right, top: rect!.top, bottom: rect!.bottom }
      const next = placeViewportMenu(
        anchor,
        menu.getBoundingClientRect(),
        { width: window.innerWidth, height: window.innerHeight },
        side,
        align,
        gap,
      )
      setPosition((current) => current?.left === next.left && current.top === next.top ? current : next)
    }

    update()
    const resizeObserver = typeof ResizeObserver === 'undefined'
      ? null
      : new ResizeObserver(update)
    if (menuRef.current) resizeObserver?.observe(menuRef.current)
    if (anchorRef?.current) resizeObserver?.observe(anchorRef.current)
    window.addEventListener('resize', update)
    window.addEventListener('scroll', update, true)
    return () => {
      resizeObserver?.disconnect()
      window.removeEventListener('resize', update)
      window.removeEventListener('scroll', update, true)
    }
  }, [align, anchorRef, gap, point?.x, point?.y, side])

  useEffect(() => {
    const menu = menuRef.current
    if (!menu || (manageFocus ?? props.role === 'menu') === false) return
    const items = () => Array.from(menu.querySelectorAll<HTMLElement>('[role^="menuitem"]:not([disabled])'))
    items()[0]?.focus()
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.key === 'Escape' && onRequestCloseRef.current) {
        event.preventDefault()
        event.stopPropagation()
        anchorRef?.current?.focus()
        onRequestCloseRef.current()
        return
      }
      if (!['ArrowDown', 'ArrowUp', 'Home', 'End'].includes(event.key)) return
      const available = items()
      if (available.length === 0) return
      event.preventDefault()
      const current = Math.max(0, available.indexOf(document.activeElement as HTMLElement))
      const next = event.key === 'Home' ? 0
        : event.key === 'End' ? available.length - 1
          : (current + (event.key === 'ArrowDown' ? 1 : -1) + available.length) % available.length
      available[next]?.focus()
    }
    menu.addEventListener('keydown', onKeyDown)
    return () => {
      menu.removeEventListener('keydown', onKeyDown)
      if (menu.contains(document.activeElement)) anchorRef?.current?.focus()
    }
  }, [anchorRef, manageFocus, props.role])

  return createPortal(
    <div
      {...props}
      ref={menuRef}
      data-viewport-menu=""
      style={{
        ...style,
        position: 'fixed',
        zIndex: VIEWPORT_MENU_Z_INDEX,
        left: position?.left ?? 0,
        top: position?.top ?? 0,
        maxWidth: style?.maxWidth ?? 'calc(100vw - 16px)',
        maxHeight: style?.maxHeight ?? 'calc(100vh - 16px)',
        overflowY: style?.overflowY ?? 'auto',
        visibility: position ? style?.visibility : 'hidden',
      } as CSSProperties}
    />,
    document.body,
  )
})
