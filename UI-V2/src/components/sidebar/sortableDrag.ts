import { createContext } from 'react'
import type { PointerEvent as ReactPointerEvent } from 'react'
import { motionAllowed, scaledMs } from '../ui/Overlay'

/**
 * Pointer-driven sortable rows: the dragged row lifts and follows the pointer while its siblings glide aside
 * to open the gap where it will land. Rows carry `data-sort-id`; their sortable siblings are the other
 * `[data-sort-id]` children of the same parent. Styles are written straight to the DOM so a drag never
 * re-renders the sidebar; React only hears about drop-target changes and the final drop.
 */

export interface SortRow { id: string; top: number; height: number }

/**
 * Order and per-row offsets when the pointer is at `probeY` (same coordinates as `rows`). The pointer, not the
 * dragged row's centre, picks the slot, so a tall expanded row moves past a sibling as soon as its handle does.
 */
export function sortPreview(rows: readonly SortRow[], draggedId: string, probeY: number) {
  const others = rows.filter((row) => row.id !== draggedId)
  const order = others.map((row) => row.id)
  order.splice(others.filter((row) => row.top + row.height / 2 < probeY).length, 0, draggedId)
  const byId = new Map(rows.map((row) => [row.id, row]))
  const offsets = new Map<string, number>()
  let top = rows[0]?.top ?? 0
  for (const id of order) {
    const row = byId.get(id)!
    offsets.set(id, top - row.top)
    top += row.height
  }
  return { order, offsets }
}

/**
 * Applies a saved manual order to a list's default order. Ids the user has not placed yet (new pins, chats newly
 * active) and ids `returned` reports as back since the save stay on top in their default order; placed ids follow in
 * the saved order. An empty save is the default.
 */
export function applyManualOrder(ids: readonly string[], saved: readonly string[], returned?: (id: string) => boolean): readonly string[] {
  if (saved.length === 0) return ids
  const rank = new Map(saved.map((id, index) => [id, index]))
  const isPlaced = (id: string) => rank.has(id) && !returned?.(id)
  const fresh = ids.filter((id) => !isPlaced(id))
  const placed = ids.filter(isPlaced).sort((a, b) => rank.get(a)! - rank.get(b)!)
  return [...fresh, ...placed]
}

export type ChatSortList = 'pinned' | 'active'
/** Native keeps this many ids per saved list (settings_normalization.h); longer lists are not sortable. */
export const MAX_SORTED_CHATS = 2000
/** Lets a chat row in Pinned or Active start a drag or a keyboard move without the sidebar passing callbacks per row. */
export const ChatSortContext = createContext<{
  start: (list: ChatSortList, sessionId: string, event: ReactPointerEvent<HTMLElement>) => void
  move: (list: ChatSortList, sessionId: string, direction: -1 | 1) => void
} | null>(null)

export interface SortableDrop {
  /** Sibling ids in their new order (unchanged when dropped on a target). */
  order: string[]
  changed: boolean
  /** Drop target from `targetAt`, or null for a reorder among siblings. */
  target: string | null
}

export interface SortableDragOptions {
  /** Pointer travel before a press becomes a drag, so ordinary clicks still open rows. */
  threshold?: number
  /** A drop target under the pointer (for example a collection), which replaces sorting while hovered. */
  targetAt?: (x: number, y: number) => string | null
  onTargetChange?: (target: string | null) => void
  onStart?: (id: string) => void
  /** Resolves true once the change is saved; false animates the row back. */
  onDrop: (drop: SortableDrop) => Promise<boolean> | boolean
  onCancel?: (id: string) => void
}

const AUTO_SCROLL_EDGE = 36
const AUTO_SCROLL_MAX = 14
/** A save that has not answered by then glides the row back, so a stalled bridge never leaves it lifted. */
const SAVE_TIMEOUT_MS = 5000
let busy = false

/** True while a drag runs or its drop is still settling; other reorder paths wait for it. */
export const isSorting = () => busy

const sortId = (element: Element) => (element as HTMLElement).dataset.sortId ?? ''
const siblingsOf = (row: HTMLElement) => Array.from(row.parentElement?.children ?? []).filter((element): element is HTMLElement => element instanceof HTMLElement && element.dataset.sortId !== undefined)

function scrollParent(element: HTMLElement): HTMLElement | null {
  for (let node = element.parentElement; node; node = node.parentElement) {
    // Only an ancestor that actually overflows can scroll; skip the style lookup for the rest.
    if (node.scrollHeight > node.clientHeight && /(auto|scroll)/.test(getComputedStyle(node).overflowY)) return node
  }
  return null
}

function clearStyles(rows: readonly HTMLElement[]) {
  for (const row of rows) {
    row.style.transform = ''
    row.style.transition = ''
    row.style.boxShadow = ''
    row.style.zIndex = ''
    row.style.position = ''
    row.style.pointerEvents = ''
    row.style.background = ''
    row.style.borderRadius = ''
    row.style.opacity = ''
    delete row.dataset.sortDragging
  }
}

/** Waits for the DOM to show `order`, then returns, so held transforms can drop without a jump. */
function afterOrder(rows: readonly HTMLElement[], order: readonly string[], done: (settled: boolean) => void) {
  let frames = 0
  const check = () => {
    const live = rows[0]?.parentElement ? siblingsOf(rows[0]).map(sortId) : []
    if (order.every((id, index) => live[index] === id)) return done(true)
    if (++frames > 20) return done(false)
    requestAnimationFrame(check)
  }
  check()
}

/**
 * The release that ends a drag, or follows an Escape-cancelled one, is not a click on the row under it.
 * The browser fires that click in the same task as pointerup, so clicks re-arm right after the release.
 */
function swallowNextClick(doc: Document, released: boolean) {
  const win = doc.defaultView ?? window
  const swallow = (event: Event) => { event.stopPropagation(); event.preventDefault(); clear() }
  const clear = () => {
    doc.removeEventListener('click', swallow, true)
    doc.removeEventListener('pointerup', clearSoon, true)
    doc.removeEventListener('pointerdown', clear, true)
    doc.removeEventListener('keydown', onKey, true)
  }
  // Key repeat or a lone modifier is not a new action; anything else (Enter on a button) re-arms clicks.
  const onKey = (event: KeyboardEvent) => { if (!event.repeat && !['Shift', 'Control', 'Alt', 'Meta', 'Escape'].includes(event.key)) clear() }
  const clearSoon = () => win.setTimeout(clear, 0)
  doc.addEventListener('click', swallow, true)
  // A release outside the window produces no click; the next press or key re-arms clicks at once.
  doc.addEventListener('pointerdown', clear, true)
  doc.addEventListener('keydown', onKey, true)
  if (released) clearSoon()
  else doc.addEventListener('pointerup', clearSoon, true)
}

/** Starts tracking a press on a sortable row's handle; it becomes a drag only after the threshold. */
export function startSortableDrag(down: ReactPointerEvent<HTMLElement>, options: SortableDragOptions) {
  // A macOS Ctrl-click opens the context menu rather than starting a drag.
  if (busy || down.button !== 0 || down.ctrlKey || down.nativeEvent.isPrimary === false) return
  // React bubbles presses from a row's portalled menu or dialog up to the row; only presses on the row itself drag it.
  if (!down.currentTarget.contains(down.target as Node)) return
  if ((down.target as Element).closest('input, textarea, select, button, a, [contenteditable="true"]')) return
  const row = down.currentTarget.closest<HTMLElement>('[data-sort-id]')
  if (!row) return
  const id = sortId(row)
  const doc = row.ownerDocument
  const win = doc.defaultView ?? window
  const threshold = options.threshold ?? 4
  const startX = down.clientX
  const startY = down.clientY
  let pointerX = startX
  let pointerY = startY
  let started = false
  let rows: HTMLElement[] = []
  let measured: SortRow[] = []
  let scroller: HTMLElement | null = null
  let startScroll = 0
  let target: string | null = null
  let order: string[] = []
  let offsets = new Map<string, number>()
  let frame = 0
  let layoutKey = ''
  const shifted = new Map<HTMLElement, number>()
  let cancelledByKey = false

  const scrollTop = () => scroller?.scrollTop ?? 0
  const update = () => {
    // Hit-test before writing styles so the pointer move costs one style pass, not two.
    const nextTarget = options.targetAt?.(pointerX, pointerY) ?? null
    if (nextTarget !== target) {
      target = nextTarget
      // Let the highlighted target show through the lifted row.
      row.style.opacity = target === null ? '' : '0.85'
      options.onTargetChange?.(target)
    }
    row.style.transform = `translate3d(0, ${pointerY - startY + scrollTop() - startScroll}px, 0) scale(1.02)`
    const preview = target === null
      ? sortPreview(measured, id, pointerY + scrollTop())
      : { order: measured.map((entry) => entry.id), offsets: new Map<string, number>() }
    const key = `${target === null}:${preview.order.join('\n')}`
    order = preview.order
    offsets = preview.offsets
    // Siblings only move when the gap does.
    if (key === layoutKey) return
    layoutKey = key
    // Only rows whose offset changes are written, so a long list costs little per move.
    for (const sibling of rows) {
      if (sibling === row) continue
      const next = preview.offsets.get(sortId(sibling)) ?? 0
      if ((shifted.get(sibling) ?? 0) === next) continue
      shifted.set(sibling, next)
      sibling.style.transform = next ? `translate3d(0, ${next}px, 0)` : ''
    }
  }
  const autoScroll = () => {
    frame = 0
    if (!scroller || !started) return
    const box = scroller.getBoundingClientRect()
    const speed = pointerY < box.top + AUTO_SCROLL_EDGE
      ? -Math.ceil(((box.top + AUTO_SCROLL_EDGE - pointerY) / AUTO_SCROLL_EDGE) * AUTO_SCROLL_MAX)
      : pointerY > box.bottom - AUTO_SCROLL_EDGE
        ? Math.ceil(((pointerY - box.bottom + AUTO_SCROLL_EDGE) / AUTO_SCROLL_EDGE) * AUTO_SCROLL_MAX)
        : 0
    if (!speed) return
    const before = scroller.scrollTop
    scroller.scrollTop = before + Math.max(-AUTO_SCROLL_MAX, Math.min(AUTO_SCROLL_MAX, speed))
    if (scroller.scrollTop !== before) update()
    frame = requestAnimationFrame(autoScroll)
  }

  const begin = () => {
    started = true
    busy = true
    rows = siblingsOf(row)
    scroller = scrollParent(row)
    startScroll = scrollTop()
    measured = rows.map((element) => {
      const box = element.getBoundingClientRect()
      return { id: sortId(element), top: box.top + startScroll, height: box.height }
    })
    // One attribute gives every sibling its glide (sidebar.css) instead of a style write per row.
    row.parentElement?.setAttribute('data-sorting', '')
    row.dataset.sortDragging = ''
    row.style.position = 'relative'
    row.style.zIndex = '20'
    row.style.pointerEvents = 'none'
    row.style.background = 'var(--surface)'
    row.style.borderRadius = '6px'
    row.style.boxShadow = 'var(--elev-2), 0 0 0 1px var(--border)'
    row.style.transition = 'box-shadow var(--dur-fast) var(--ease)'
    doc.body.style.cursor = 'grabbing'
    // Wheel or trackpad scrolling mid-drag keeps the lifted row under the pointer.
    scroller?.addEventListener('scroll', update)
    options.onStart?.(id)
    update()
  }

  const detach = () => {
    doc.removeEventListener('pointermove', move, true)
    doc.removeEventListener('pointerup', up, true)
    doc.removeEventListener('pointercancel', cancel, true)
    doc.removeEventListener('keydown', key, true)
    doc.removeEventListener('dragstart', preventDefault, true)
    win.removeEventListener('blur', cancel)
    scroller?.removeEventListener('scroll', update)
    if (frame) cancelAnimationFrame(frame)
    frame = 0
    doc.body.style.cursor = ''
  }
  const finish = () => {
    // Dropping the glide and the transforms in one pass changes nothing on screen when the DOM already matches.
    row.parentElement?.removeAttribute('data-sorting')
    clearStyles([row, ...shifted.keys()])
    busy = false
  }
  /** Glides every row back to where it started, then hands styles back to React. */
  const settleBack = () => {
    row.style.transition = 'transform var(--dur-base) var(--ease-out-soft), box-shadow var(--dur-base) var(--ease)'
    row.style.transform = ''
    row.style.boxShadow = ''
    for (const element of shifted.keys()) element.style.transform = ''
    if (motionAllowed()) win.setTimeout(finish, scaledMs(180) + 20)
    else finish()
  }

  const move = (event: PointerEvent) => {
    // The release was lost (outside the window, a system gesture): end rather than start a phantom drag.
    if (!(event.buttons & 1)) { cancel(); return }
    pointerX = event.clientX
    pointerY = event.clientY
    if (!started) {
      if (Math.hypot(pointerX - startX, pointerY - startY) < threshold) return
      begin()
    }
    event.preventDefault()
    update()
    if (!frame) frame = requestAnimationFrame(autoScroll)
  }
  const cancel = () => {
    detach()
    if (!started) return
    // Escape leaves the button held; blur and pointercancel never produce a click.
    if (cancelledByKey) swallowNextClick(doc, false)
    if (target !== null) options.onTargetChange?.(null)
    settleBack()
    options.onCancel?.(id)
  }
  const key = (event: KeyboardEvent) => {
    if (event.key !== 'Escape' || !started) return
    event.preventDefault()
    event.stopPropagation()
    cancelledByKey = true
    cancel()
  }
  const up = (event: PointerEvent) => {
    detach()
    if (!started) return
    pointerX = event.clientX
    pointerY = event.clientY
    update()
    swallowNextClick(doc, true)
    const dropTarget = target
    if (dropTarget !== null) options.onTargetChange?.(null)
    const changed = dropTarget !== null || order.some((entry, index) => entry !== measured[index]?.id)
    const slot = offsets.get(id) ?? 0
    // Settle the lifted row into its slot while the change saves.
    row.style.transition = 'transform var(--dur-base) var(--ease-out-soft), box-shadow var(--dur-base) var(--ease)'
    row.style.transform = `translate3d(0, ${slot}px, 0)`
    row.style.boxShadow = ''
    // The settling row no longer blocks clicks on whatever sits under it.
    row.style.pointerEvents = ''
    if (!changed) { settleBack(); return }
    let settled = false
    const timeout = win.setTimeout(() => { settled = true; settleBack() }, SAVE_TIMEOUT_MS)
    void Promise.resolve(options.onDrop({ order, changed, target: dropTarget })).catch(() => false).then((saved) => {
      win.clearTimeout(timeout)
      if (settled) return
      settled = true
      if (!saved) { settleBack(); return }
      // A row that moved elsewhere (into a collection) is gone from this list; one that stayed (opened in a pane) glides home.
      if (dropTarget !== null) { if (row.isConnected && siblingsOf(row).length === rows.length) settleBack(); else finish(); return }
      afterOrder(rows, order, (shown) => {
        if (!shown) { settleBack(); return }
        // The DOM now matches what the transforms showed, so dropping them changes nothing on screen.
        row.style.transition = 'none'
        finish()
      })
    })
  }
  const preventDefault = (event: Event) => event.preventDefault()

  doc.addEventListener('pointermove', move, true)
  doc.addEventListener('pointerup', up, true)
  doc.addEventListener('pointercancel', cancel, true)
  doc.addEventListener('keydown', key, true)
  doc.addEventListener('dragstart', preventDefault, true)
  win.addEventListener('blur', cancel)
}

/** Glides `row` and its siblings from their old positions after a keyboard move commits; reduced motion moves them at once. */
export function glideAfter(row: HTMLElement | undefined, commit: () => Promise<boolean>) {
  const parent = row?.parentElement
  const before = new Map((row ? siblingsOf(row) : []).map((element) => [sortId(element), element.getBoundingClientRect().top]))
  return commit().then((saved) => {
    if (!saved || !parent || !motionAllowed() || typeof HTMLElement.prototype.animate !== 'function') return saved
    requestAnimationFrame(() => {
      for (const element of parent.children as HTMLCollectionOf<HTMLElement>) {
        const top = before.get(sortId(element))
        const dy = top === undefined ? 0 : top - element.getBoundingClientRect().top
        // Web Animations cannot read CSS variables; these mirror --dur-base and --ease-out-soft.
        if (dy) element.animate([{ transform: `translateY(${dy}px)` }, { transform: 'translateY(0)' }], { duration: scaledMs(180), easing: 'cubic-bezier(0.22, 1, 0.36, 1)' })
      }
    })
    return saved
  })
}
