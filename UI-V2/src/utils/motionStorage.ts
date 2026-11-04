export type MotionSpeed = 'default' | 'fast' | 'slow' | 'off'

export const MOTION_SPEEDS: ReadonlyArray<{ id: MotionSpeed; label: string }> = [
  { id: 'default', label: 'Default' },
  { id: 'fast', label: 'Fast' },
  { id: 'slow', label: 'Slow' },
  { id: 'off', label: 'Off' },
]

export function readStoredMotion(): MotionSpeed {
  try {
    if (typeof localStorage === 'undefined' || typeof localStorage.getItem !== 'function') return 'default'
    const value = localStorage.getItem('uam-motion')
    return MOTION_SPEEDS.some((speed) => speed.id === value) ? value as MotionSpeed : 'default'
  } catch {
    return 'default'
  }
}

export function writeStoredMotion(speed: MotionSpeed): void {
  try {
    if (typeof localStorage !== 'undefined' && typeof localStorage.setItem === 'function') {
      localStorage.setItem('uam-motion', speed)
    }
  } catch {
    // Storage may be unavailable in restricted launch contexts.
  }
}

/** Sets data-motion on the root; index.css scales the --dur-* tokens from it. Default leaves no attribute. */
export function applyMotionSpeed(speed: MotionSpeed): void {
  if (typeof document === 'undefined' || !document.documentElement) return
  if (speed === 'default') document.documentElement.removeAttribute('data-motion')
  else document.documentElement.setAttribute('data-motion', speed)
}

/** Smooth programmatic scrolling unless Appearance > Motion is Off or the system asks for reduced motion. */
export function motionScrollBehavior(): ScrollBehavior {
  if (typeof document === 'undefined') return 'auto'
  const reduced = typeof window.matchMedia === 'function' && window.matchMedia('(prefers-reduced-motion: reduce)').matches
  return reduced || document.documentElement.getAttribute('data-motion') === 'off' ? 'auto' : 'smooth'
}

/** Aligns `row` to the top of its scrolling `region` with motionScrollBehavior(). Returns true while a smooth scroll
 * owns the region; `onEnd` then runs once, on `scrollend` or after two frames if nothing moved (a row already in
 * place, or the region already at its end, never fires `scrollend`). */
export function alignRowStart(region: HTMLElement, row: HTMLElement, onEnd: () => void): boolean {
  const behavior = motionScrollBehavior()
  const from = region.scrollTop
  row.scrollIntoView?.({ block: 'start', behavior })
  // A scroll that already landed was instant (Motion Off, or smooth not honoured); nothing to hold.
  if (behavior !== 'smooth' || region.scrollTop !== from) return false
  let done = false
  const end = () => {
    if (done) return
    done = true
    region.removeEventListener('scrollend', end)
    onEnd()
  }
  region.addEventListener('scrollend', end)
  requestAnimationFrame(() => requestAnimationFrame(() => { if (region.scrollTop === from) end() }))
  return true
}
