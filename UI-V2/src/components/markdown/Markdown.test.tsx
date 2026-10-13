import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { describe, expect, it, vi } from 'vitest'
import { MarkdownContent } from './Markdown'

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

function renderMarkdown(content: string): HTMLElement {
  const host = document.createElement('div')
  document.body.appendChild(host)
  const root = createRoot(host)
  act(() => {
    root.render(<MarkdownContent content={content} />)
  })
  return host
}

describe('MarkdownContent', () => {
  it('renders pipe tables and leaves malformed table text alone', () => {
    const host = renderMarkdown('| Tool | Status |\n| --- | :---: |\n| `rg` | **ok** |\n\n| Bad | Row |\n| nope |')

    expect(host.querySelectorAll('table')).toHaveLength(1)
    expect(host.querySelector('th')?.textContent).toBe('Tool')
    expect(host.querySelector('td code')?.textContent).toBe('rg')
    expect(host.querySelector('td strong')?.textContent).toBe('ok')
    expect(host.textContent).toContain('| Bad | Row |')

    host.remove()
  })

  it.each(['\n', '\r\n'])('renders fenced code blocks with an optional language label (%j)', (newline) => {
    const host = renderMarkdown('```ts\nconst x = 1\n```\n\n```plain text```\n\n```\nno label\n```'.replace(/\n/g, newline))

    expect(host.querySelector('pre code')?.textContent).toBe('const x = 1')
    expect(host.querySelector('pre span.uppercase')?.textContent).toBe('ts')
    expect(Array.from(host.querySelectorAll('pre code'), node => node.textContent)).toEqual(['const x = 1', 'plain text', 'no label'])
    expect(host.querySelectorAll('pre button[aria-label="Copy code block"]')).toHaveLength(3)

    host.remove()
  })

  it('renders inline links only for safe schemes', () => {
    const malformed = '['.repeat(40000) + '[x]('.repeat(10000)
    const host = renderMarkdown(`${malformed} **still bold** and ` + "`still code`" + `\n\nSee [docs](https://example.com) and [bad](javascript:alert(1)). [**empty**]()`)
    expect(host.querySelector('p')?.textContent).toBe(`${malformed} still bold and still code`)
    expect(Array.from(host.querySelectorAll('strong'), node => node.textContent)).toEqual(['still bold', 'empty'])
    expect(host.querySelector('code')?.textContent).toBe('still code')
    expect(host.textContent).toContain('[empty]()')

    const anchor = host.querySelector('a')
    expect(anchor?.getAttribute('href')).toBe('https://example.com')
    expect(host.textContent).toContain('bad')
    expect(host.querySelectorAll('a')).toHaveLength(1)

    host.remove()
  })
})


it('copies only fenced code from its keyboard-focusable control and exposes clipboard failure', async () => {
  const host = document.createElement('div')
  document.body.appendChild(host)
  const root = createRoot(host)
  const originalClipboard = Object.getOwnPropertyDescriptor(navigator, 'clipboard')
  const originalExec = document.execCommand
  const writeText = vi.fn().mockResolvedValue(undefined)
  Object.defineProperty(navigator, 'clipboard', { configurable: true, value: { writeText } })
  const content = 'Surrounding text\n\n```ts\nconst value = 1\n```\n\nAfter text'
  try {
    act(() => root.render(<MarkdownContent content={content} />))
    const button = host.querySelector<HTMLButtonElement>('button[aria-label="Copy code block"]')!
    expect(button).not.toBeNull()
    expect(button.type).toBe('button')
    button.focus()
    expect(document.activeElement).toBe(button)
    await act(async () => { button.click() })
    expect(writeText).toHaveBeenCalledExactlyOnceWith('const value = 1')
    expect(button.textContent).toContain('Copied')
    writeText.mockRejectedValue(new Error('Denied'))
    document.execCommand = vi.fn().mockReturnValue(false)
    await act(async () => { button.click() })
    expect(button.textContent).toContain('Copy failed')
    expect(host.querySelector('pre code')?.textContent).toBe('const value = 1')
    expect(host.querySelector('button[aria-label="Copy message"]')).toBeNull()
  } finally {
    act(() => root.unmount()); host.remove()
    if (originalClipboard) Object.defineProperty(navigator, 'clipboard', originalClipboard)
    else Reflect.deleteProperty(navigator, 'clipboard')
    document.execCommand = originalExec
  }
})


it('does not mount an empty markdown row and preserves long code without extra paragraphs', () => {
  const host = document.createElement('div')
  const root = createRoot(host)
  const longLine = 'const token = "' + 'x'.repeat(4000) + '"'
  try {
    act(() => root.render(<MarkdownContent content={' \n \t'} />))
    expect(host.children).toHaveLength(0)
    act(() => root.render(<MarkdownContent content={'```\n' + longLine + '\n```'} />))
    expect(host.querySelector('pre code')?.textContent).toBe(longLine)
    expect(host.querySelectorAll('p')).toHaveLength(0)
    expect(host.querySelectorAll('button')).toHaveLength(1)
  } finally { act(() => root.unmount()) }
})

describe('MarkdownContent chips', () => {
  it('renders file, folder and branch references as chips only when chips are enabled', () => {
    const content = 'See `src/main.cpp:3`, `src/components/`, `feature/chips` and `object.method`.\n\n```ts\nconst `src/x.ts` = 1\n```'
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    try {
      act(() => root.render(<MarkdownContent content={content} chips />))
      const chips = Array.from(host.querySelectorAll('.uam-context-chip'))
      expect(chips.map((chip) => chip.getAttribute('data-detail'))).toEqual(['src/main.cpp:3', 'src/components/', 'feature/chips'])
      expect(chips.map((chip) => chip.querySelector('.uam-context-chip__meta')?.textContent)).toEqual(['file', 'folder', 'branch'])
      expect(host.querySelector('p code')?.textContent).toBe('object.method')
      expect(host.querySelector('pre code')?.textContent).toBe('const `src/x.ts` = 1')

      act(() => root.render(<MarkdownContent content={content} />))
      expect(host.querySelectorAll('.uam-context-chip')).toHaveLength(0)
      expect(host.querySelector('p code')?.textContent).toBe('src/main.cpp:3')
    } finally {
      act(() => root.unmount())
      host.remove()
    }
  })
})
