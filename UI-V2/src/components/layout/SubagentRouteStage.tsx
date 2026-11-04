import { useLayoutEffect, useRef, type ReactNode } from 'react'

/** Route keys: a chat is its session ID; a child view is `${parentId}>${childId}`. */
export const subagentStageKey = (parentId: string, childId?: string) => childId ? `${parentId}>${childId}` : parentId

/** +1 pushes a child over its parent, -1 pops back to the parent, 0 is any unrelated change (no stage motion). */
export function subagentStageDirection(previous: string, next: string): 1 | -1 | 0 {
  if (previous === next) return 0
  const parentOf = (key: string) => key.includes('>') ? key.slice(0, key.indexOf('>')) : null
  if (parentOf(next) !== null && (parentOf(next) === previous || parentOf(next) === parentOf(previous))) return 1
  if (parentOf(previous) !== null && parentOf(previous) === next) return -1
  return 0
}

// Spring response (undamped period) per second of the shared motion token: push follows --dur-enter, Back follows the
// shorter --dur-exit, so Appearance → Motion (Fast/Slow/Off) and reduced motion scale it like every other transition.
// Default tokens give 0.42 s push / 0.30 s Back; at ζ 0.9 the view is ~95 % home after about one token duration.
const RESPONSE_PER_TOKEN_S = 1.75
const DAMPING = 0.9
const PARALLAX = 0.3
const MAX_FLIGHT_MS = 2000

export type SpringState = { p: number; v: number }

/** One semi-implicit Euler step toward rest at 0. `p` is the remaining travel fraction (1 = off-stage). */
export function stepSpring(state: SpringState, dt: number, response: number): SpringState {
  const omega = 2 * Math.PI / response
  const v = state.v + (-omega * omega * state.p - 2 * DAMPING * omega * state.v) * dt
  return { p: state.p + v * dt, v }
}

export const springSettled = ({ p, v }: SpringState) => Math.abs(p) < 0.0005 && Math.abs(v) < 0.01

const motionHeld = () => document.documentElement.getAttribute('data-motion') === 'off' ||
  Boolean(window.matchMedia?.('(prefers-reduced-motion: reduce)').matches)

const seconds = (value = '') => {
  const amount = Number.parseFloat(value)
  return Number.isFinite(amount) && amount > 0 ? (value.trim().endsWith('ms') ? amount / 1000 : amount) : 0
}

/** Spring response for this move from the stage's resolved `--dur-enter, --dur-exit`; 0 means change without motion. */
export function stageResponse(stage: HTMLElement, direction: 1 | -1) {
  if (motionHeld()) return 0
  const [enter, exit] = getComputedStyle(stage).transitionDuration.split(',')
  return seconds(direction === 1 ? enter : exit) * RESPONSE_PER_TOKEN_S
}

// Content a DOM copy cannot reproduce faithfully (blank canvas, reloading frames/media): such moves change instantly.
const UNSUPPORTED = 'canvas, iframe, video, audio, object, embed'
// In-tree fixed/open layers would be repositioned by a transformed ancestor; while one is open the stage does not move.
export const STAGE_OVERLAY = 'dialog[open], [role="dialog"], [role="alertdialog"], [role="menu"], [role="listbox"], .uam-overlay, .tool-details-backdrop, .fixed, [style*="position: fixed"]'
const CONTROL = 'button, input, textarea, select, a[href], summary, [contenteditable], [role="button"], [role="link"], [role="slider"], [role="checkbox"], [role="switch"], [role="tab"], [role="menuitem"], [role="option"], [role="textbox"]'
const STRIPPED = ['id', 'name', 'for', 'form', 'href', 'autofocus', 'tabindex', 'contenteditable', 'draggable', 'popover', 'data-testid', 'data-message-index', 'data-message-navigation']
// ponytail: very large outgoing trees change instantly instead of paying a long clone; raise if long chats should glide.
const MAX_COPIED_ELEMENTS = 6000

/** Same-size static box for a control: shows where it was without a label, value, draft text or working affordance. */
function placeholder(control: HTMLElement) {
  const style = getComputedStyle(control)
  const box = document.createElement('span')
  box.className = 'uam-subagent-stage__placeholder'
  Object.assign(box.style, {
    display: style.display === 'none' ? 'none' : style.display.startsWith('inline') ? 'inline-block' : 'block',
    position: style.position === 'fixed' || style.position === 'sticky' ? 'absolute' : style.position,
    top: style.top, right: style.right, bottom: style.bottom, left: style.left,
    width: `${control.offsetWidth}px`, height: `${control.offsetHeight}px`, margin: style.margin, flex: style.flex,
    verticalAlign: style.verticalAlign, borderRadius: style.borderRadius, border: style.border, backgroundColor: style.backgroundColor,
  })
  return box
}

type Frozen = { ghost: HTMLElement; restoreScroll: () => void }

/**
 * Static visual approximation of the outgoing view (not a pixel snapshot): an aria-hidden, inert DOM copy with every
 * control replaced by a same-size static box, identifiers and inline handlers removed, and scroll offsets restored.
 * React listeners are never cloned and the live React tree unmounts normally, so no reader, effect, draft, focus target
 * or delivery path exists in the copy. Returns null when the content cannot be copied faithfully; the move is then instant.
 */
export function freezeOutgoing(source: HTMLElement): Frozen | null {
  if (source.querySelector(`${UNSUPPORTED}, ${STAGE_OVERLAY}`) || source.getElementsByTagName('*').length > MAX_COPIED_ELEMENTS) return null
  const ghost = source.cloneNode(true) as HTMLElement
  const from = [source, ...source.querySelectorAll<HTMLElement>('*')]
  const to = [ghost, ...ghost.querySelectorAll<HTMLElement>('*')]
  const offsets: Array<[HTMLElement, number, number]> = []
  const controls: Array<[HTMLElement, HTMLElement]> = []
  from.forEach((element, index) => {
    if (element.scrollTop || element.scrollLeft) offsets.push([to[index], element.scrollTop, element.scrollLeft])
    const outer = element.parentElement?.closest(CONTROL)
    if (index > 0 && element.matches(CONTROL) && !(outer && source.contains(outer))) controls.push([to[index], placeholder(element)])
  })
  for (const [copy, box] of controls) copy.replaceWith(box)
  for (const element of [ghost, ...ghost.querySelectorAll<HTMLElement>('*')]) {
    for (const name of element.getAttributeNames()) if (name.startsWith('on') || STRIPPED.includes(name)) element.removeAttribute(name)
  }
  ghost.setAttribute('aria-hidden', 'true')
  ghost.setAttribute('inert', '')
  ghost.dataset.subagentStageCopy = ''
  return { ghost, restoreScroll: () => offsets.forEach(([element, top, left]) => { element.scrollTop = top; element.scrollLeft = left }) }
}

type Flight = { direction: 1 | -1; response: number; state: SpringState; ghost: HTMLElement; frame: number | null; last: number; started: number }

/**
 * Parent/child route stage for one pane. Open slides the child in from the trailing edge over the parent, which recedes
 * with parallax; Back slides the child out to the trailing edge while the parent returns. A damped spring drives both,
 * reversing mid-flight keeps position and velocity, and the live view stays interactive throughout. Motion Off, reduced
 * motion, unrelated route changes, open in-tree overlays and uncopyable content allocate no copy and request no frame.
 */
export function SubagentRouteStage({ routeKey, children }: { routeKey: string; children: ReactNode }) {
  const stage = useRef<HTMLDivElement>(null)
  const layer = useRef<HTMLDivElement>(null)
  const committedKey = useRef(routeKey)
  // ponytail: render-phase ref write; only the commit of this render's key reads it (during that commit's deletion pass).
  const renderedKey = useRef(routeKey)
  renderedKey.current = routeKey
  const flight = useRef<Flight | null>(null)
  const outgoing = useRef<{ frozen: Frozen; response: number } | null>(null)
  const disposed = useRef(false)

  const paint = (current: Flight) => {
    const width = stage.current?.clientWidth ?? 0
    const travel = current.state.p
    const live = layer.current
    if (current.direction === 1) {
      if (live) live.style.transform = `translate3d(${travel * width}px,0,0)`
      current.ghost.style.transform = `translate3d(${-(1 - travel) * PARALLAX * width}px,0,0)`
    } else {
      if (live) live.style.transform = `translate3d(${-travel * PARALLAX * width}px,0,0)`
      current.ghost.style.transform = `translate3d(${(1 - travel) * width}px,0,0)`
    }
  }
  const land = () => {
    const current = flight.current
    if (!current) return
    if (current.frame !== null) cancelAnimationFrame(current.frame)
    current.ghost.remove()
    if (layer.current) { layer.current.style.transform = ''; layer.current.style.boxShadow = '' }
    flight.current = null
  }

  useLayoutEffect(() => {
    disposed.current = false
    return () => { disposed.current = true; land() }
  }, [])

  // Runs in React's deletion pass while the outgoing view is still attached and scrolled. Every refusal is decided
  // before cloneNode, so instant changes allocate nothing.
  const capture = (element: HTMLElement | null) => {
    const direction = subagentStageDirection(committedKey.current, renderedKey.current)
    if (disposed.current || !element || !stage.current || !direction) return
    const response = stageResponse(stage.current, direction)
    if (!response || layer.current?.querySelector(STAGE_OVERLAY)) return
    const frozen = freezeOutgoing(element)
    if (frozen) outgoing.current = { frozen, response }
  }

  useLayoutEffect(() => {
    const previous = committedKey.current
    committedKey.current = routeKey
    const captured = outgoing.current
    outgoing.current = null
    const direction = subagentStageDirection(previous, routeKey)
    const prior = flight.current
    // Reversal continues from the exact on-screen position and velocity; anything else starts a fresh push or pop.
    const state: SpringState = prior && direction === -prior.direction ? { p: 1 - prior.state.p, v: -prior.state.v } : { p: 1, v: 0 }
    land()
    // The incoming view may have opened an overlay or motion may have been turned off since capture.
    if (!captured || !direction || !stage.current || !layer.current || motionHeld() || layer.current.querySelector(STAGE_OVERLAY)) return
    const { frozen: { ghost, restoreScroll }, response } = captured
    Object.assign(ghost.style, { position: 'absolute', inset: '0', pointerEvents: 'none', zIndex: direction === 1 ? '0' : '2', willChange: 'transform' })
    if (direction === -1) ghost.style.boxShadow = 'var(--elev-3)'
    else layer.current.style.boxShadow = 'var(--elev-3)'
    stage.current.appendChild(ghost)
    restoreScroll()
    const now = performance.now()
    const current: Flight = { direction, response, state, ghost, frame: null, last: now, started: now }
    flight.current = current
    paint(current)
    const tick = (time: number) => {
      if (disposed.current || flight.current !== current) return
      // Turning motion off, an overlay opening in the live view, or a stalled flight lands immediately.
      if (motionHeld() || layer.current?.querySelector(STAGE_OVERLAY) || time - current.started > MAX_FLIGHT_MS) { land(); return }
      // ponytail: frame dt capped at 1/30 s so a stalled frame cannot overshoot; sub-step if 30 Hz ever looks loose.
      current.state = stepSpring(current.state, Math.min(Math.max(0, time - current.last) / 1000, 1 / 30), response)
      current.last = time
      if (springSettled(current.state)) { land(); return }
      paint(current)
      current.frame = requestAnimationFrame(tick)
    }
    current.frame = requestAnimationFrame(tick)
  }, [routeKey])

  // transition-property none: the duration list only resolves the shared motion tokens for stageResponse.
  return <div ref={stage} className="relative h-full"
    style={{ overflow: 'clip', isolation: 'isolate', transitionProperty: 'none', transitionDuration: 'var(--dur-enter), var(--dur-exit)' }}>
    <div ref={layer} className="relative h-full" style={{ zIndex: 1, background: 'var(--bg)' }}>
      <OutgoingCapture key={routeKey} onLeave={capture}>{children}</OutgoingCapture>
    </div>
  </div>
}

function OutgoingCapture({ onLeave, children }: { onLeave: (element: HTMLElement | null) => void; children: ReactNode }) {
  const root = useRef<HTMLDivElement>(null)
  const leave = useRef(onLeave)
  leave.current = onLeave
  useLayoutEffect(() => { const element = root.current; return () => leave.current(element) }, [])
  return <div ref={root} className="h-full">{children}</div>
}
