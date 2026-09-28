import { readFileSync } from 'node:fs'
import { expect, it } from 'vitest'

const source = (path: string) => readFileSync(new URL(path, import.meta.url), 'utf8')
const companionStyles = source('./companion.css')

it('gives companion chat, collection, and folder rows 44px touch targets', () => {
  const style = document.createElement('style')
  style.textContent = companionStyles
  document.head.append(style)

  const companion = document.createElement('div')
  companion.className = 'uam-companion'
  companion.innerHTML = `<div class="uam-companion-list">
    <div data-session-id="chat"></div>
    <div data-testid="collection-header-project"><button aria-label="Collapse Websites"></button></div>
    <div data-testid="folder-header-project"></div>
  </div>`
  document.body.append(companion)

  try {
    for (const target of companion.querySelectorAll<HTMLElement>(
      '[data-session-id], [data-testid^="collection-header-"], [data-testid^="folder-header-"]',
    )) {
      expect(getComputedStyle(target).minHeight).toBe('44px')
    }
    const disclosure = companion.querySelector<HTMLButtonElement>('[aria-label="Collapse Websites"]')!
    expect(getComputedStyle(disclosure).minWidth).toBe('44px')
    expect(getComputedStyle(disclosure).minHeight).toBe('44px')
    expect(getComputedStyle(disclosure).margin).toBe('-15px')

    const desktopRow = document.createElement('div')
    desktopRow.dataset.sessionId = 'desktop-chat'
    document.body.append(desktopRow)
    expect(getComputedStyle(desktopRow).minHeight).not.toBe('44px')
    desktopRow.remove()
  } finally {
    companion.remove()
    style.remove()
  }
})
