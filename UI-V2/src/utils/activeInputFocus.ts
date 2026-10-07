export const FOCUS_ACTIVE_INPUT_EVENT = 'uam-focus-active-input'

/** Focus only an existing input in the currently focused visible chat pane. */
export function focusActiveInput(doc: Document = document): boolean {
  if (doc.querySelector('[aria-modal="true"]')) return false
  const panes = Array.from(doc.querySelectorAll<HTMLElement>('.uam-chat-pane[data-focused="true"]')).filter((pane) => {
    for (let element: HTMLElement | null = pane; element; element = element.parentElement) {
      if (element.matches('[hidden], [inert], [aria-hidden="true"]')) return false
      const style = doc.defaultView?.getComputedStyle(element)
      if (style?.display === 'none' || style?.visibility === 'hidden') return false
    }
    return true
  })
  if (panes.length !== 1) return false
  const composer = panes[0].querySelector<HTMLTextAreaElement>('textarea.uam-composer-textarea')
  if (composer) {
    if (composer.disabled || composer.readOnly) return false
    composer.focus()
    return doc.activeElement === composer
  }
  const terminal = panes[0].querySelector<HTMLElement>('[data-uam-terminal]')
  if (!terminal) return false
  return !terminal.dispatchEvent(new Event(FOCUS_ACTIVE_INPUT_EVENT, { cancelable: true }))
}
