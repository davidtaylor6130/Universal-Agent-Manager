import { act } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { useAppStore } from '../../store/useAppStore'
import { ComputePoolsSettings } from './ComputePoolsSettings'

vi.stubGlobal('ResizeObserver', class { observe() {} unobserve() {} disconnect() {} })
;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

const pool = { id: 'p1', name: 'InferDeck', limit: 2, members: [{ providerId: 'opencode-cli', modelId: 'qwen3.8' }] }
const setComputePools = vi.fn(async () => true)

const button = (name: string) => Array.from(document.querySelectorAll<HTMLButtonElement>('button')).find((item) => (item.getAttribute('aria-label') || item.textContent?.trim()) === name)!
const input = (name: string) => document.querySelector<HTMLInputElement>(`input[aria-label="${name}"]`)!
const click = async (name: string) => { await act(async () => { button(name).click() }) }
const type = (name: string, value: string) => act(() => {
  Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(input(name), value)
  input(name).dispatchEvent(new Event('input', { bubbles: true }))
})

describe('ComputePoolsSettings', () => {
  let host: HTMLDivElement
  let root: Root
  const show = () => { host = document.createElement('div'); document.body.appendChild(host); root = createRoot(host); act(() => root.render(<ComputePoolsSettings />)) }

  beforeEach(() => {
    setComputePools.mockClear()
    useAppStore.setState({ computePools: [], computePoolUsage: {}, setComputePools })
  })
  afterEach(() => { act(() => root.unmount()); host.remove() })

  it('shows the empty sentence and adds a trimmed pool through the editor', async () => {
    show()
    expect(document.body.textContent).toContain('No pools yet. Without one, chats run with no limit.')
    await click('Add pool')
    expect(document.activeElement).toBe(input('Pool name'))
    expect(button('Save pool').disabled).toBe(true)
    type('Pool name', '算'.repeat(40))
    expect(button('Save pool').disabled).toBe(true)
    type('Pool name', '  Local GPU ')
    expect(button('Save pool').disabled).toBe(false)
    await click('Save pool')
    const saved = (setComputePools.mock.calls[0] as unknown[])[0] as typeof pool[]
    expect(saved).toHaveLength(1)
    expect(saved[0].name).toBe('Local GPU')
    expect(saved[0].limit).toBe(2)
  })

  it('shows live use and waiting counts on the pool row', () => {
    useAppStore.setState({ computePools: [pool], computePoolUsage: { p1: { inUse: 2, waiting: 1 } } })
    show()
    expect(document.body.textContent).toContain('2/2 in use · 1 waiting')
  })

  it('refuses a limit outside the range', async () => {
    useAppStore.setState({ computePools: [pool] })
    show()
    await click('Edit InferDeck')
    type('Chats running at once', '0')
    expect(button('Save pool').disabled).toBe(true)
  })

  it('leaves the editor open when Escape closes one of its menus', async () => {
    useAppStore.setState({ computePools: [pool] })
    show()
    await click('Edit InferDeck')
    const dialog = document.querySelector('[role="dialog"]')!
    const menu = document.createElement('div')
    menu.setAttribute('data-viewport-menu', '')
    dialog.appendChild(menu)
    act(() => { menu.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })) })
    expect(document.querySelector('[role="dialog"]')).toBeTruthy()
    expect(document.body.textContent).not.toContain('Your changes have not been saved.')
    menu.remove()
  })

  it('removes a pool only after confirming', async () => {
    useAppStore.setState({ computePools: [pool] })
    show()
    await click('Delete InferDeck')
    expect(setComputePools).not.toHaveBeenCalled()
    await click('Delete')
    await vi.waitFor(() => expect(setComputePools).toHaveBeenCalledWith([]))
  })
})
