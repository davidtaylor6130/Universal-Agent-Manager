import { act } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { GeminiChildBrowser } from './GeminiChildBrowser'
import { useAppStore } from '../../store/useAppStore'

const sendToCEF = vi.hoisted(() => vi.fn())
vi.mock('../../ipc/cefBridge', async importOriginal => ({ ...(await importOriginal<object>()), sendToCEF }))
;(globalThis as { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

const reference = 'native-agent-reference'
const child = { reference, preview: 'Audit the parser', startedAt: '2026-11-01T10:00:00.000Z', updatedAt: '2026-11-01T10:05:00.000Z', messages: 2 }
const listing = (children: unknown[], rest = {}) => ({ ok: true, data: { chatId: 'parent', children, unreadable: 0, truncated: false, ...rest } })
const settle = () => act(async () => { await Promise.resolve(); await Promise.resolve(); await Promise.resolve() })

describe('GeminiChildBrowser', () => {
  const openSubAgentSession = vi.fn()
  let root: Root | null = null
  let host: HTMLDivElement

  const mount = async (onClose: () => void = vi.fn()) => {
    host = document.createElement('div')
    document.body.append(host)
    root = createRoot(host)
    await act(async () => { root?.render(<GeminiChildBrowser chatId="parent" onClose={onClose} />) })
    await settle()
  }
  const unmount = async () => {
    await act(async () => { root?.unmount() })
    root = null
    host?.remove()
  }
  const button = (label: string | RegExp) => [...document.querySelectorAll<HTMLButtonElement>('button')].find(item => {
    const name = item.getAttribute('aria-label') || item.textContent || ''
    return typeof label === 'string' ? name === label : label.test(name)
  })
  const text = () => document.body.textContent || ''

  beforeEach(() => {
    sendToCEF.mockReset()
    openSubAgentSession.mockReset()
    useAppStore.setState({ openSubAgentSession })
  })
  afterEach(unmount)

  it('opens the chosen row by its exact reference and closes', async () => {
    sendToCEF.mockResolvedValue(listing([child]))
    openSubAgentSession.mockResolvedValue('opened-child')
    const onClose = vi.fn()
    await mount(onClose)
    await act(async () => { button(/^Open subagent conversation: Audit the parser, .*, 2 messages$/)?.click() })
    await settle()
    expect(onClose).toHaveBeenCalledOnce()
    expect(sendToCEF).toHaveBeenCalledWith({ action: 'listGeminiChildConversations', payload: { chatId: 'parent' } })
    expect(openSubAgentSession.mock.calls[0].slice(0, 2)).toEqual(['parent', reference])
  })

  it('keeps the dialog open and explains a refused open', async () => {
    sendToCEF.mockResolvedValue(listing([child]))
    openSubAgentSession.mockImplementation(async (_a, _b, _c, _d, onFailure) => { onFailure('The exact child is unavailable.'); return null })
    const onClose = vi.fn()
    await mount(onClose)
    await act(async () => { button(/Open subagent conversation/)?.click() })
    await settle()
    expect(text()).toContain('The exact child is unavailable.')
    expect(onClose).not.toHaveBeenCalled()
    expect(document.activeElement?.getAttribute('role')).toBe('dialog')
  })

  it('lets Escape dismiss a slow open and then withholds navigation', async () => {
    sendToCEF.mockResolvedValue(listing([child]))
    let finish: (value: string | null) => void = () => {}
    openSubAgentSession.mockImplementation(() => new Promise(resolve => { finish = resolve }))
    const onClose = vi.fn()
    await mount(onClose)
    await act(async () => { button(/Open subagent conversation/)?.click() })
    const stillWanted = openSubAgentSession.mock.calls[0][5] as () => boolean
    expect(stillWanted()).toBe(true)
    await act(async () => { window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true })) })
    expect(onClose).toHaveBeenCalledOnce()
    expect(stillWanted()).toBe(false)
    await act(async () => { finish('opened-child') })
    expect(onClose).toHaveBeenCalledOnce()
  })

  it('shows empty and partial results, then retries a failed read', async () => {
    sendToCEF.mockResolvedValueOnce(listing([]))
    await mount()
    expect(text()).toContain('No subagent conversations are saved for this chat.')
    await unmount()

    sendToCEF.mockResolvedValueOnce(listing([], { unreadable: 1 }))
    await mount()
    expect(text()).toContain('1 saved journal could not be read.')
    expect(text()).not.toContain('No subagent conversations are saved')
    await unmount()

    sendToCEF.mockResolvedValueOnce(listing([child], { unreadable: 2, truncated: true }))
    await mount()
    expect(text()).toContain('2 saved journals could not be read. Not every saved conversation')
    await unmount()

    sendToCEF.mockResolvedValueOnce({ ok: false, error: 'Update this host.' }).mockResolvedValueOnce(listing([child]))
    await mount()
    expect(text()).toContain('Update this host.')
    await act(async () => { button('Retry')?.click() })
    await settle()
    expect(button(/Open subagent conversation/)).toBeTruthy()
  })

  it('rejects a listing for another chat and closes on Escape', async () => {
    sendToCEF.mockResolvedValue({ ok: true, data: { chatId: 'other', children: [child], unreadable: 0, truncated: false } })
    const onClose = vi.fn()
    await mount(onClose)
    expect(text()).toContain('Subagent conversations could not be read.')
    expect(button(/Open subagent conversation/)).toBeUndefined()
    await act(async () => { window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true })) })
    expect(onClose).toHaveBeenCalledOnce()
  })
})
