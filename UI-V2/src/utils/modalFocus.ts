const FOCUSABLE = 'a[href], button:not([disabled]), input:not([disabled]), select:not([disabled]), textarea:not([disabled]), [tabindex]:not([tabindex="-1"])'

/** The highest overlay layer owns keyboard input; later dialogs win ties. */
export function getTopmostModal(): HTMLElement | null {
  let top: HTMLElement | null = null
  let topLayer = -Infinity
  for (const dialog of document.querySelectorAll<HTMLElement>('[aria-modal="true"]')) {
    if (dialog.closest('[inert], [aria-hidden="true"], [data-state="closed"]')) continue
    let layer = 0
    for (let parent: HTMLElement | null = dialog; parent; parent = parent.parentElement) {
      const zIndex = Number.parseInt(window.getComputedStyle(parent).zIndex, 10)
      if (Number.isFinite(zIndex)) layer = Math.max(layer, zIndex)
    }
    if (layer >= topLayer) {
      top = dialog
      topLayer = layer
    }
  }
  return top
}

export function trapModalTab(event: KeyboardEvent) {
  if (event.key !== 'Tab' || event.defaultPrevented) return
  if ((document.activeElement as HTMLElement | null)?.closest('[data-viewport-menu]')) return

  const dialog = getTopmostModal()
  if (!dialog) return

  const focusable = Array.from(dialog.querySelectorAll<HTMLElement>(FOCUSABLE))
    .filter((element) => !element.closest('[inert], [aria-hidden="true"]'))
  if (focusable.length === 0) {
    event.preventDefault()
    dialog.focus()
    return
  }

  const current = focusable.indexOf(document.activeElement as HTMLElement)
  const next = event.shiftKey
    ? current <= 0 ? focusable.length - 1 : current - 1
    : current < 0 || current === focusable.length - 1 ? 0 : current + 1
  event.preventDefault()
  focusable[next].focus()
}
