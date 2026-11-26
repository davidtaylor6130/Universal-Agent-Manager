import type { CSSProperties } from 'react'
import './sidebar.css'

/**
 * Painted size of a sidebar icon control inside its 24px hit area (`.uam-sb-iconbtn` in sidebar.css).
 * With `.uam-sb-iconbtn--inline` it is also the control's footprint in the row's flow.
 */
export const sidebarIconPaint = (px: number) => ({ '--sb-paint': `${px}px` }) as CSSProperties
