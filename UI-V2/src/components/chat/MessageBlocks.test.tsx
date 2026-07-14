import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { describe, expect, it } from 'vitest'
import { AttachmentList } from './MessageBlocks'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

describe('AttachmentList', () => {
  it('renders Markdown Store context separately from ordinary files', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<AttachmentList attachments={[
      { id: '/tmp/review.uam', name: 'review.uam', type: 'markdown-store', size: 0, path: '/tmp/review.uam' },
      { id: 'diagram', name: 'diagram.png', type: 'image', size: 10, path: '/tmp/diagram.png' },
    ]} />))

    const markdownContext = host.querySelector('[aria-label="Markdown Store context"]')
    const files = host.querySelector('[aria-label="File attachments"]')
    expect(markdownContext?.textContent).toContain('Markdown Storereview.uam')
    expect(markdownContext?.textContent).not.toContain('/tmp/review.uam')
    expect(files?.textContent).toContain('/tmp/diagram.pngimage')

    act(() => root.unmount())
    host.remove()
  })
})
