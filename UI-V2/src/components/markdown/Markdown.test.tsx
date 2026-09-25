import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { describe, expect, it } from 'vitest'
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
    expect(host.querySelector('pre div')?.textContent).toBe('ts')
    expect(Array.from(host.querySelectorAll('pre code'), node => node.textContent)).toEqual(['const x = 1', 'plain text', 'no label'])
    expect(host.querySelectorAll('pre div')).toHaveLength(1)

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
