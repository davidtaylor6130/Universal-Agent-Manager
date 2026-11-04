import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, describe, expect, it, vi } from 'vitest'
import { Button } from './Button'
import { IconButton } from './IconButton'
import { Tooltip } from './Tooltip'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

describe('disabledReason (opt-in)', () => {
  let host: HTMLDivElement | undefined
  afterEach(() => { host?.remove(); host = undefined })

  function render(node: React.ReactElement) {
    host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(node))
    return { root, button: host.querySelector('button') as HTMLButtonElement }
  }

  it('keeps a disabled Button focusable, marks it aria-disabled, blocks clicks and describes the reason', () => {
    const onClick = vi.fn()
    const { button } = render(<Button disabled disabledReason="Answer the permission request first" onClick={onClick}>Model</Button>)
    expect(button.hasAttribute('disabled')).toBe(false)
    expect(button.getAttribute('aria-disabled')).toBe('true')
    button.click()
    expect(onClick).not.toHaveBeenCalled()
    const describedBy = button.getAttribute('aria-describedby')
    expect(describedBy).toBeTruthy()
    expect(document.getElementById(describedBy!)?.textContent).toBe('Answer the permission request first')
    expect(button.querySelector('.uam-btn__label')?.textContent).toBe('Model')
  })

  it('keeps native disabled and no description when disabledReason is not set', () => {
    const { button } = render(<Button disabled>Model</Button>)
    expect(button.hasAttribute('disabled')).toBe(true)
    expect(button.hasAttribute('aria-disabled')).toBe(false)
    expect(button.hasAttribute('aria-describedby')).toBe(false)
  })

  it('leaves an enabled Button untouched even when disabledReason is set', () => {
    const onClick = vi.fn()
    const { button } = render(<Button disabledReason="Nope" onClick={onClick}>Go</Button>)
    expect(button.hasAttribute('aria-disabled')).toBe(false)
    button.click()
    expect(onClick).toHaveBeenCalledTimes(1)
  })

  it('applies the same behaviour to IconButton and keeps its accessible name', () => {
    const onClick = vi.fn()
    const { button } = render(<IconButton icon={<span />} label="Microphone" disabled disabledReason="Dictation needs the desktop app" onClick={onClick} />)
    expect(button.hasAttribute('disabled')).toBe(false)
    expect(button.getAttribute('aria-disabled')).toBe('true')
    expect(button.getAttribute('aria-label')).toBe('Microphone')
    button.click()
    expect(onClick).not.toHaveBeenCalled()
    expect(document.getElementById(button.getAttribute('aria-describedby')!)?.textContent).toBe('Dictation needs the desktop app')
  })

  it('keeps the plain native disabled IconButton when disabledReason is not set', () => {
    const { button } = render(<IconButton icon={<span />} label="Microphone" disabled />)
    expect(button.hasAttribute('disabled')).toBe(true)
    expect(button.hasAttribute('aria-disabled')).toBe(false)
  })
})

describe('Tooltip', () => {
  it('renders children without a wrapper when there is no label', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<Tooltip><button>Plain</button></Tooltip>))
    expect(host.innerHTML).toBe('<button>Plain</button>')
    act(() => root.unmount())
    host.remove()
  })
})

describe('Button asLink', () => {
  it('uses the inline text-link look instead of a variant and size', () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const root = createRoot(host)
    act(() => root.render(<Button asLink variant="danger" size="lg">Add folder</Button>))
    const button = host.querySelector('button') as HTMLButtonElement
    expect(button.classList.contains('uam-btn--link')).toBe(true)
    expect(button.classList.contains('uam-btn--danger')).toBe(false)
    expect(button.classList.contains('uam-btn--lg')).toBe(false)
    expect(button.querySelector('.uam-btn__label')?.textContent).toBe('Add folder')
    act(() => root.unmount()); host.remove()
  })
})
