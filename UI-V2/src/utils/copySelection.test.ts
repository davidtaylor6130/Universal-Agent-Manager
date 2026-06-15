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
    delete window.cefQuery
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

  it('extracts selected text outside chat copy surfaces by default', () => {
    const outside = document.createElement('div')
    outside.textContent = 'Terminal text'
    document.body.appendChild(outside)

    selectNodeText(outside.firstChild ?? outside)

    expect(selectedTextFromDocument(document)).toBe('Terminal text')
    expect(selectedTextFromDocument(document, { requireCopySurface: false })).toBe('Terminal text')
  })

  it('can still require chat copy surfaces when requested', () => {
    const outside = document.createElement('div')
    outside.textContent = 'Terminal text'
    document.body.appendChild(outside)

    selectNodeText(outside.firstChild ?? outside)

    expect(selectedTextFromDocument(document, { requireCopySurface: true })).toBe('')
  })

  it('preserves native editable-field copy handling', () => {
    const input = document.createElement('input')
    document.body.appendChild(input)

    expect(isEditableElement(input)).toBe(true)
  })

  it('does not intercept editable-field copy shortcuts', async () => {
    const writeText = vi.fn(() => Promise.resolve())
    Object.defineProperty(globalThis.navigator, 'clipboard', {
      value: { writeText },
      configurable: true,
    })

    const input = document.createElement('input')
    input.value = 'Native copy text'
    document.body.appendChild(input)

    const cleanup = installCopySelectionFallback(document)
    input.dispatchEvent(new KeyboardEvent('keydown', { key: 'c', metaKey: true, bubbles: true }))
    await Promise.resolve()

    expect(writeText).not.toHaveBeenCalled()
    cleanup()
  })

  it('copies selected app text on keyboard shortcuts', async () => {
    const writeText = vi.fn(() => Promise.resolve())
    Object.defineProperty(globalThis.navigator, 'clipboard', {
      value: { writeText },
      configurable: true,
    })

    const text = document.createElement('div')
    text.textContent = 'Copy me'
    document.body.appendChild(text)
    selectNodeText(text.firstChild ?? text)

    const cleanup = installCopySelectionFallback(document)
    text.dispatchEvent(new KeyboardEvent('keydown', { key: 'c', metaKey: true, bubbles: true }))
    await Promise.resolve()

    expect(writeText).toHaveBeenCalledWith('Copy me')
    cleanup()
  })

  it('prefers the CEF clipboard bridge when available', async () => {
    const writeText = vi.fn(() => Promise.resolve())
    Object.defineProperty(globalThis.navigator, 'clipboard', {
      value: { writeText },
      configurable: true,
    })
    window.cefQuery = vi.fn(({ request, onSuccess }) => {
      const parsed = JSON.parse(request)
      expect(parsed.action).toBe('writeClipboardText')
      expect(parsed.payload.text).toBe('CEF text')
      onSuccess('{"copied":true}')
    })

    await expect(copyTextToClipboard('CEF text', document)).resolves.toBe(true)
    expect(window.cefQuery).toHaveBeenCalled()
    expect(writeText).not.toHaveBeenCalled()
  })

  it('writes selected app text during browser copy events', () => {
    const text = document.createElement('div')
    text.textContent = 'Diagnostics text'
    document.body.appendChild(text)
    selectNodeText(text.firstChild ?? text)

    const clipboardData = { setData: vi.fn() }
    const event = new Event('copy', { bubbles: true, cancelable: true }) as ClipboardEvent
    Object.defineProperty(event, 'clipboardData', {
      value: clipboardData,
    })

    const cleanup = installCopySelectionFallback(document)
    const prevented = !text.dispatchEvent(event)

    expect(prevented).toBe(true)
    expect(clipboardData.setData).toHaveBeenCalledWith('text/plain', 'Diagnostics text')
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
