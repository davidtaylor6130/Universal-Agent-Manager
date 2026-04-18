import { afterEach, describe, expect, it, vi } from 'vitest'
import {
  copyTextToClipboard,
  installCopySelectionFallback,
  isEditableElement,
  selectedTextFromDocument,
} from './copySelection'

function selectNodeText(node: Node) {
  const selection = document.getSelection()
  const range = document.createRange()
  range.selectNodeContents(node)
  selection?.removeAllRanges()
  selection?.addRange(range)
}

describe('copySelection', () => {
  afterEach(() => {
    document.body.innerHTML = ''
    document.getSelection()?.removeAllRanges()
    vi.restoreAllMocks()
  })

  it('extracts selected text inside chat copy surfaces', () => {
    const surface = document.createElement('div')
    surface.dataset.copySurface = 'chat'
    surface.textContent = 'Selected chat text'
    document.body.appendChild(surface)

    selectNodeText(surface.firstChild ?? surface)

    expect(selectedTextFromDocument(document)).toBe('Selected chat text')
  })

  it('ignores document selections outside chat copy surfaces by default', () => {
    const outside = document.createElement('div')
    outside.textContent = 'Terminal text'
    document.body.appendChild(outside)

    selectNodeText(outside.firstChild ?? outside)

    expect(selectedTextFromDocument(document)).toBe('')
    expect(selectedTextFromDocument(document, { requireCopySurface: false })).toBe('Terminal text')
  })

  it('preserves native editable-field copy handling', () => {
    const input = document.createElement('input')
    document.body.appendChild(input)

    expect(isEditableElement(input)).toBe(true)
  })

  it('copies selected chat text on keyboard shortcuts', async () => {
    const writeText = vi.fn(() => Promise.resolve())
    Object.defineProperty(globalThis.navigator, 'clipboard', {
      value: { writeText },
      configurable: true,
    })

    const surface = document.createElement('div')
    surface.dataset.copySurface = 'chat'
    surface.textContent = 'Copy me'
    document.body.appendChild(surface)
    selectNodeText(surface.firstChild ?? surface)

    const cleanup = installCopySelectionFallback(document)
    surface.dispatchEvent(new KeyboardEvent('keydown', { key: 'c', metaKey: true, bubbles: true }))
    await Promise.resolve()

    expect(writeText).toHaveBeenCalledWith('Copy me')
    cleanup()
  })

  it('falls back to execCommand when navigator clipboard is unavailable', async () => {
    Object.defineProperty(globalThis.navigator, 'clipboard', {
      value: undefined,
      configurable: true,
    })
    const execCommand = vi.fn(() => true)
    Object.defineProperty(document, 'execCommand', {
      value: execCommand,
      configurable: true,
    })

    await expect(copyTextToClipboard('Fallback text', document)).resolves.toBe(true)
    expect(execCommand).toHaveBeenCalledWith('copy')
  })
})
