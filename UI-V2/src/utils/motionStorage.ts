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
