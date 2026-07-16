import { forwardRef, useImperativeHandle, useLayoutEffect, useRef, useState } from 'react'
import type { CSSProperties, HTMLAttributes, RefObject } from 'react'
import { createPortal } from 'react-dom'

const VIEWPORT_GAP = 8

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
  side: 'top' | 'bottom' = 'bottom',
  align: 'start' | 'end' = 'start',
  gap = 4,
) {
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
  side?: 'top' | 'bottom'
  align?: 'start' | 'end'
  gap?: number
}

export const ViewportMenu = forwardRef<HTMLDivElement, ViewportMenuProps>(function ViewportMenu({
  anchorRef,
  point,
  side = 'bottom',
  align = 'start',
  gap = 4,
  style,
  ...props
}, forwardedRef) {
  const menuRef = useRef<HTMLDivElement>(null)
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
    window.addEventListener('resize', update)
    window.addEventListener('scroll', update, true)
    return () => {
      window.removeEventListener('resize', update)
      window.removeEventListener('scroll', update, true)
    }
  }, [align, anchorRef, gap, point?.x, point?.y, side])

  return createPortal(
    <div
      {...props}
      ref={menuRef}
      data-viewport-menu=""
      style={{
        ...style,
        position: 'fixed',
        zIndex: 1000,
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
