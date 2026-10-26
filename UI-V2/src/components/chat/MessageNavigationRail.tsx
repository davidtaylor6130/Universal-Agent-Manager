import { useEffect, useMemo, useRef, useState, type RefObject } from 'react'
import { ChevronDown, ChevronUp } from 'lucide-react'
import type { Message } from '../../types/message'
import { IconButton } from '../ui'

/** Message positions stay evenly spaced regardless of message length. */
export function messageIndexAtPointer(count: number, top: number, height: number, pointer: number): number {
  if (count <= 1 || height <= 0) return 0
  return Math.max(0, Math.min(count - 1, Math.round((pointer - top) / height * (count - 1))))
}

/** Last mark at or before a message index; marks are ascending message indices. */
export function markAtOrBefore(indices: number[], messageIndex: number): number {
  let low = 0
  let high = indices.length - 1
  while (low < high) {
    const middle = Math.ceil((low + high) / 2)
    if (indices[middle] <= messageIndex) low = middle
    else high = middle - 1
  }
  return low
}

/**
 * The message the rail treats as current, from the ascending visible message indices: the first visible user
 * message (or the one owning the topmost visible reply), but the last visible one once the transcript end is in view,
 * so the final message is selected at the bottom even when an earlier turn still shows above it.
 */
export function currentVisibleMessage(indices: number[], visible: number[], atEnd: boolean): number {
  if (atEnd) return visible[visible.length - 1]
  return visible.find((index) => indices[markAtOrBefore(indices, index)] === index) ?? visible[0]
}

type RailView = { visible: number[]; lastRendered: number }

/** The rail ends this far above the floating composer (matches its `bottom` style below). */
const RAIL_COMPOSER_GAP = 16
const ROW = '[data-message-index]'
// ponytail: fixed grace for a jump's row to appear once loading ends; long enough for a smooth scroll.
const PIN_SETTLE_MS = 2000
const RELEASE_EVENTS = ['wheel', 'touchstart', 'pointerdown', 'keydown'] as const

export function MessageNavigationRail({ userMessageIndices, startIndex, messages, currentIndex, viewportRef, busy, onNavigate }: {
  userMessageIndices?: number[]
  startIndex: number
  messages: Message[]
  currentIndex: number
  viewportRef: RefObject<HTMLDivElement>
  busy: boolean
  onNavigate: (index: number) => void
}) {
  const indices = useMemo(() => userMessageIndices ?? messages.flatMap((message, index) => message.role === 'user' ? [startIndex + index] : []), [userMessageIndices, messages, startIndex])
  const positions = useMemo(() => new Map(indices.map((messageIndex, position) => [messageIndex, position])), [indices])
  const count = indices.length
  const [previewIndex, setPreviewIndex] = useState<number | null>(null)
  const [view, setView] = useState<RailView>({ visible: [], lastRendered: -1 })
  // A rail jump owns the selection until the reader scrolls, or its row has been on screen and left it;
  // otherwise a target that cannot reach the top (near the end) would hand the selection straight back.
  const [pin, setPin] = useState<{ index: number; seen: boolean } | null>(null)
  const railRef = useRef<HTMLDivElement>(null)
  const hasMarks = count > 0
  // A jump whose row never shows (refused read, page budget spent) stops holding the selection.
  useEffect(() => {
    if (!pin || pin.seen || busy) return
    const timer = window.setTimeout(() => setPin((prior) => prior && !prior.seen ? null : prior), PIN_SETTLE_MS)
    return () => window.clearTimeout(timer)
  }, [pin, busy])
  // Visibility comes from IntersectionObserver, so scrolling never reads layout; only this rail re-renders.
  useEffect(() => {
    const viewport = viewportRef.current
    if (!viewport || typeof IntersectionObserver === 'undefined') return
    const observed = new Map<Element, number>()
    const intersecting = new Set<Element>()
    let frame: number | null = null
    const publish = () => {
      const visible = [...intersecting].map((element) => observed.get(element)!).sort((a, b) => a - b)
      // ponytail: keeps the last lit marks while only non-message rows show, instead of flickering to the caller's position.
      if (visible.length === 0) return
      const lastRendered = Math.max(...observed.values())
      setView((prior) => prior.lastRendered === lastRendered && prior.visible.length === visible.length && prior.visible.every((index, at) => index === visible[at])
        ? prior : { visible, lastRendered })
      setPin((prior) => !prior ? prior : visible.includes(prior.index) ? (prior.seen ? prior : { ...prior, seen: true }) : prior.seen ? null : prior)
    }
    const onEntries = (entries: IntersectionObserverEntry[]) => {
      for (const entry of entries) {
        if (entry.isIntersecting && observed.has(entry.target)) intersecting.add(entry.target)
        else intersecting.delete(entry.target)
      }
      publish()
    }
    let observer = new IntersectionObserver(onEntries, { root: viewport })
    // Rows under the floating composer are not on screen. The rail's own bottom follows the composer height,
    // so the inset is read only when the rail resizes (composer growth or window resize), never while scrolling.
    let inset = 0
    const rail = railRef.current
    const resize = rail && typeof ResizeObserver !== 'undefined' ? new ResizeObserver(() => {
      const next = Math.max(0, (parseFloat(getComputedStyle(rail).bottom) || 0) - RAIL_COMPOSER_GAP)
      if (next === inset) return
      inset = next
      observer.disconnect()
      intersecting.clear()
      observer = new IntersectionObserver(onEntries, { root: viewport, rootMargin: `0px 0px -${inset}px 0px` })
      for (const element of observed.keys()) observer.observe(element)
    }) : null
    if (rail) resize?.observe(rail)
    // Rows mount, unmount and renumber as history pages in, so the observed set follows the DOM.
    const scan = () => {
      frame = null
      const rows = new Map<Element, number>()
      for (const element of viewport.querySelectorAll<HTMLElement>(ROW)) {
        const index = Number(element.dataset.messageIndex)
        if (Number.isSafeInteger(index)) rows.set(element, index)
      }
      // Unmounted rows and rows that lost their index stop counting.
      for (const element of observed.keys()) {
        if (!rows.has(element)) { observer.unobserve(element); observed.delete(element); intersecting.delete(element) }
      }
      for (const [element, index] of rows) {
        if (!observed.has(element)) observer.observe(element)
        observed.set(element, index)
      }
      publish()
    }
    // Streaming text changes do not add or remove rows, so they never schedule a scan.
    const touchesRows = (node: Node) => node instanceof Element && (node.matches(ROW) || node.querySelector(ROW) !== null)
    const mutations = new MutationObserver((records) => {
      if (frame === null && records.some((record) => record.type === 'attributes' ||
        [...record.addedNodes].some(touchesRows) || [...record.removedNodes].some(touchesRows))) frame = requestAnimationFrame(scan)
    })
    const release = () => setPin(null)
    for (const type of RELEASE_EVENTS) viewport.addEventListener(type, release, { passive: true })
    mutations.observe(viewport, { childList: true, subtree: true, attributes: true, attributeFilter: ['data-message-index'] })
    scan()
    return () => {
      for (const type of RELEASE_EVENTS) viewport.removeEventListener(type, release)
      mutations.disconnect()
      resize?.disconnect()
      observer.disconnect()
      if (frame !== null) cancelAnimationFrame(frame)
    }
  }, [viewportRef, hasMarks])
  if (count === 0) return null
  // The transcript end only counts once the final user message is loaded, not at the end of an older page.
  const atEnd = view.visible.length > 0 && view.visible[view.visible.length - 1] >= view.lastRendered && view.lastRendered >= indices[count - 1]
  // Without visibility tracking the caller's position stands in.
  const current = view.visible.length ? pin?.index ?? currentVisibleMessage(indices, view.visible, atEnd) : currentIndex
  const currentUserIndex = markAtOrBefore(indices, current)
  const navigate = (position: number) => {
    setPin({ index: indices[position], seen: false })
    onNavigate(indices[position])
  }
  const index = Math.max(0, Math.min(count - 1, previewIndex ?? currentUserIndex))
  const message = messages[indices[index] - startIndex]
  const label = `${!userMessageIndices && startIndex > 0 ? 'Loaded user' : 'User'} message ${index + 1} of ${count}`
  const preview = message ? message.content.slice(0, 160) : null
  // The visual preview is aria-hidden; the slider value carries the same text so screen readers get it once.
  const valueText = preview === null ? `${label}. Not loaded yet. Scroll up or click to load this part of the chat.`
    : preview.trim() ? `${label}: ${preview.replace(/\s+/g, ' ').trim()}` : label
  const markTop = (position: number) => `${count === 1 ? 50 : position / (count - 1) * 100}%`
  const lit = new Set(view.visible.flatMap((messageIndex) => positions.has(messageIndex) ? [positions.get(messageIndex)!] : []))
  const step = (delta: -1 | 1) => {
    const target = currentUserIndex + delta
    const unavailable = busy || target < 0 || target >= count
    // aria-disabled rather than disabled: Chromium drops focus from a button that becomes disabled mid-jump.
    return <IconButton className="conversation-rail-step" size="sm" tooltipSide="right"
      icon={delta < 0 ? <ChevronUp size={14} /> : <ChevronDown size={14} />}
      label={delta < 0 ? 'Previous user message' : 'Next user message'}
      aria-disabled={unavailable}
      onClick={() => { if (!unavailable) navigate(target) }} />
  }
  // Inset 4px keeps expanded marks (6 + 48 wide) within the 64px gutter, ending at 58px.
  return <div ref={railRef} className="conversation-rail absolute left-1 top-4 z-10 flex w-5 flex-col justify-center gap-0.5" style={{ bottom: `calc(var(--composer-dock-h, 0px) + ${RAIL_COMPOSER_GAP}px)` }}>
    {step(-1)}
    <div role="slider" tabIndex={busy ? -1 : 0} aria-label="Navigate user messages" aria-orientation="vertical"
      aria-valuemin={1} aria-valuemax={count} aria-valuenow={index + 1} aria-valuetext={valueText} aria-disabled={busy}
      className="conversation-rail-track relative min-h-0 w-full cursor-pointer outline-none focus-visible:ring-1 focus-visible:ring-[var(--accent)]"
      style={{ height: Math.max(1, count - 1) * 8, maxHeight: '100%' }}
      onMouseMove={(event) => {
        const rect = event.currentTarget.getBoundingClientRect()
        setPreviewIndex(messageIndexAtPointer(count, rect.top, rect.height, event.clientY))
      }}
      onMouseLeave={(event) => {
        if (event.currentTarget !== document.activeElement) setPreviewIndex(null)
      }}
      onFocus={() => setPreviewIndex(index)}
      onBlur={() => setPreviewIndex(null)}
      onClick={(event) => {
        if (busy) return
        const rect = event.currentTarget.getBoundingClientRect()
        navigate(messageIndexAtPointer(count, rect.top, rect.height, event.clientY))
      }}
      onKeyDown={(event) => {
        if (busy) return
        const deltas: Record<string, number> = { ArrowUp: -1, ArrowDown: 1, PageUp: -10, PageDown: 10 }
        const next = event.key === 'Home' ? 0 : event.key === 'End' ? count - 1
          : event.key in deltas ? Math.max(0, Math.min(count - 1, index + deltas[event.key])) : null
        if (next === null && event.key !== 'Enter' && event.key !== ' ') return
        event.preventDefault()
        const target = next ?? index
        setPreviewIndex(target)
        navigate(target)
      }}>
      <span aria-hidden style={{ position: 'absolute', top: count === 1 ? '50%' : 0, bottom: count === 1 ? undefined : -1, left: 6, width: 14, height: count === 1 ? 1.5 : undefined,
        background: count === 1 ? 'var(--text-3)' : undefined,
        backgroundImage: count > 1 ? 'linear-gradient(to bottom, var(--text-3) 1.5px, transparent 1.5px)' : undefined,
        backgroundSize: count > 1 ? `100% calc((100% - 1px) / ${count - 1})` : undefined }} />
      {[...lit].map((position) => <span key={position} aria-hidden data-rail-visible={position} className="conversation-rail-lit pointer-events-none"
        style={{ position: 'absolute', top: markTop(position), left: 6, width: 14, height: 1.5, background: 'var(--text)' }} />)}
      {previewIndex !== null && [-2, -1, 1, 2].map(distance => {
        const neighbor = index + distance
        if (neighbor < 0 || neighbor >= count) return null
        return <span key={neighbor} aria-hidden data-rail-neighbor={neighbor} className="pointer-events-none motion-reduce:transition-none" style={{ position: 'absolute', top: markTop(neighbor), left: 6, width: 48, height: 1.5, background: lit.has(neighbor) ? 'var(--text)' : 'var(--text-3)', transformOrigin: 'left', transition: 'transform var(--dur-fast) var(--ease)', transform: `scaleX(${Math.abs(distance) === 1 ? 2 / 3 : 5 / 12})` }} />
      })}
      <span aria-hidden data-rail-target={index} className="pointer-events-none motion-reduce:transition-none" style={{ position: 'absolute', top: markTop(index), left: 6, width: 48, height: 4.5, background: 'var(--accent)', transformOrigin: 'left', transition: 'transform var(--dur-fast) var(--ease)', transform: `scaleX(${previewIndex === null ? 22 / 48 : 1})` }} />
      {previewIndex !== null && <div className="conversation-rail-preview" aria-hidden style={{ top: markTop(index) }}>
        <div className="conversation-rail-preview__label">{label}</div>
        {preview !== null
          ? <div className="conversation-rail-preview__text">{preview}</div>
          : <>
            <div className="conversation-rail-preview__title">Not loaded yet</div>
            <div className="conversation-rail-preview__hint">Scroll up or click to load this part of the chat.</div>
          </>}
      </div>}
    </div>
    {step(1)}
  </div>
}
