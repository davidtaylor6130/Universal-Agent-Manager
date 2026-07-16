const FOCUSABLE = 'a[href], button:not([disabled]), input:not([disabled]), select:not([disabled]), textarea:not([disabled]), [tabindex]:not([tabindex="-1"])'

export function trapModalTab(event: KeyboardEvent) {
  if (event.key !== 'Tab' || event.defaultPrevented) return

  const dialogs = document.querySelectorAll<HTMLElement>('[aria-modal="true"]')
  const dialog = dialogs.item(dialogs.length - 1)
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
