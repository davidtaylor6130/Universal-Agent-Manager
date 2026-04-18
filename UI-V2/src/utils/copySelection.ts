function elementForNode(node: Node | null): Element | null {
  if (!node) return null
  return node instanceof Element ? node : node.parentElement
}

function copySurfaceForNode(node: Node | null): Element | null {
  return elementForNode(node)?.closest('[data-copy-surface="chat"]') ?? null
}

export function isEditableElement(target: EventTarget | null) {
  if (!(target instanceof Element)) return false
  return Boolean(target.closest('input, textarea, select, [contenteditable=""], [contenteditable="true"]'))
}

export function selectedTextFromDocument(
  doc: Document,
  options: { requireCopySurface?: boolean } = {}
) {
  const selection = doc.getSelection()
  if (!selection || selection.isCollapsed) return ''

  if (options.requireCopySurface ?? true) {
    const anchorSurface = copySurfaceForNode(selection.anchorNode)
    const focusSurface = copySurfaceForNode(selection.focusNode)
    if (!anchorSurface || anchorSurface !== focusSurface) return ''
  }

  return selection.toString()
}

export async function copyTextToClipboard(text: string, doc: Document) {
  if (!text) return false

  const clipboard = globalThis.navigator?.clipboard
  if (clipboard?.writeText) {
    try {
      await clipboard.writeText(text)
      return true
    } catch {
      // Continue to the textarea fallback below.
    }
  }

  if (typeof doc.execCommand !== 'function') return false

  const textarea = doc.createElement('textarea')
  textarea.value = text
  textarea.setAttribute('readonly', 'true')
  textarea.style.position = 'fixed'
  textarea.style.left = '-9999px'
  textarea.style.top = '0'
  doc.body.appendChild(textarea)
  textarea.select()

  try {
    return doc.execCommand('copy')
  } finally {
    textarea.remove()
  }
}

export function installCopySelectionFallback(doc: Document = document) {
  const onKeyDown = (event: KeyboardEvent) => {
    if (event.key.toLowerCase() !== 'c') return
    if ((!event.metaKey && !event.ctrlKey) || event.altKey) return
    if (isEditableElement(event.target)) return

    const text = selectedTextFromDocument(doc)
    if (!text) return

    event.preventDefault()
    void copyTextToClipboard(text, doc)
  }

  doc.addEventListener('keydown', onKeyDown)
  return () => {
    doc.removeEventListener('keydown', onKeyDown)
  }
}
