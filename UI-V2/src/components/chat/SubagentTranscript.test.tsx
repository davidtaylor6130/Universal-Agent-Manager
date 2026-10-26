import { act } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, onTestFinished, vi } from 'vitest'
import { SubagentHeader, SubagentTranscript, transcriptWindowStart, TRANSCRIPT_BATCH, MAX_JUMP_PAGES, type OlderHistoryOutcome } from './SubagentTranscript'
import type { Message } from '../../types/message'
import { readFileSync } from 'node:fs'
import { resolve } from 'node:path'

// The runner cwd is UI-V2. Inspect exact source tokens; Vitest's default CSS-disable plugin empties CSS imports, including ?raw.
const indexCss = readFileSync(resolve(process.cwd(), 'src/index.css'), 'utf8')
const cardCss = readFileSync(resolve(process.cwd(), 'src/components/chat/SubagentCard.css'), 'utf8')
const groupCss = readFileSync(resolve(process.cwd(), 'src/components/chat/SubagentGroup.css'), 'utf8')
const panelCss = readFileSync(resolve(process.cwd(), 'src/components/chat/ManagedSubagentPanel.css'), 'utf8')

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
let host: HTMLDivElement
let root: Root
beforeEach(() => { host = document.createElement('div'); document.body.appendChild(host); root = createRoot(host) })
afterEach(() => { act(() => root.unmount()); host.remove(); vi.restoreAllMocks() })
const rows = (count: number, from = 0): Message[] => Array.from({ length: count }, (_, offset) => {
  const index = from + offset
  return { id: `m${index}`, sessionId: 'child', role: index % 2 ? 'assistant' : 'user', content: `Row ${index}`, createdAt: new Date(2026, 9, 21, 9, index % 60) }
})
// jsdom has no layout: a zero-height viewport would self-fill every row. Tall fixed geometry keeps fill off unless a
// test opts into a short window (`short`), and scrollTop is stored per element.
const geometry = (short = false) => {
  const positions = new WeakMap<Element, number>()
  const transcriptEl = (element: Element) => element.classList.contains('uam-subagent-transcript')
  vi.spyOn(Element.prototype, 'scrollHeight', 'get').mockImplementation(function (this: Element) { return transcriptEl(this) ? (short ? 300 : 2000) : 0 })
  vi.spyOn(Element.prototype, 'clientHeight', 'get').mockImplementation(function (this: Element) { return transcriptEl(this) ? 300 : 0 })
  vi.spyOn(Element.prototype, 'scrollTop', 'get').mockImplementation(function (this: Element) { return positions.get(this) || 0 })
  vi.spyOn(Element.prototype, 'scrollTop', 'set').mockImplementation(function (this: Element, value: number) { positions.set(this, Math.min(Math.max(0, value), Math.max(0, this.scrollHeight - this.clientHeight))) })
}
const buttons = () => [...host.querySelectorAll('button')].map(button => button.getAttribute('aria-label') || button.textContent)
const region = () => host.querySelector<HTMLElement>('[role="region"]')!
const scroll = async () => { await act(async () => { region().dispatchEvent(new Event('scroll')) }) }
const deferred = () => { let resolve!: (value: OlderHistoryOutcome) => void; const promise = new Promise<OlderHistoryOutcome>(done => { resolve = done }); return { promise, resolve } }
type Props = Partial<Parameters<typeof SubagentTranscript>[0]>
const transcript = (props: Props) => <SubagentTranscript messages={rows(4)} fallbackProviderId="codex-cli" sessionId="child" viewportKey="" contentVersion="v" label="Native conversation messages" {...props} />

describe('subagent transcript window', () => {
  it('anchors by absolute index so prepends and appends keep the same first row', () => {
    expect(transcriptWindowStart(50, 0, null)).toBe(50 - TRANSCRIPT_BATCH)
    expect(transcriptWindowStart(10, 0, null)).toBe(0)
    expect(transcriptWindowStart(50, 0, 12)).toBe(12)
    expect(transcriptWindowStart(70, 0, 12)).toBe(12)        // appended rows
    expect(transcriptWindowStart(70, 80, 100)).toBe(20)      // 20 older rows prepended: same absolute row 100
    expect(transcriptWindowStart(50, 0, -5)).toBe(0)
  })

  it('renders continuous canonical rows with the navigation rail and no page buttons or composer', () => {
    geometry()
    act(() => root.render(transcript({ messages: rows(50), label: 'Stored conversation messages' })))
    expect(host.querySelector('.uam-chat-viewport[data-message-navigation] .uam-chat-transcript .uam-chat-content .uam-message-list')).not.toBeNull()
    expect(host.querySelector('[role="slider"][aria-label="Navigate user messages"]')).not.toBeNull()
    expect(host.querySelectorAll('[data-message-index]').length).toBe(TRANSCRIPT_BATCH)
    expect(buttons()).not.toEqual(expect.arrayContaining(['Earlier messages', 'Later messages', 'Load earlier history']))
    expect(host.querySelector('textarea, form')).toBeNull()
  })

  it('reveals every loaded older row by scrolling up, never dropping rows', async () => {
    geometry()
    act(() => root.render(transcript({ messages: rows(45), label: 'Stored conversation messages' })))
    expect(host.querySelectorAll('[data-message-index]').length).toBe(TRANSCRIPT_BATCH)
    for (let i = 0; i < 4; i++) { region().scrollTop = 0; await scroll() }
    expect(host.querySelectorAll('[data-message-index]').length).toBe(45)
    expect(host.querySelector('[data-message-index="0"]')).not.toBeNull()
  })

  it('smooth-scrolls a rail jump and holds reveal-older until the scroll ends, or jumps instantly with Motion Off', async () => {
    geometry()
    const scrollIntoView = vi.fn()
    const original = Element.prototype.scrollIntoView
    Element.prototype.scrollIntoView = scrollIntoView
    onTestFinished(() => { Element.prototype.scrollIntoView = original })
    act(() => root.render(transcript({ messages: rows(45), label: 'Stored conversation messages' })))
    const jump = () => act(() => { host.querySelector('[role="slider"]')!.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowUp', bubbles: true })) })
    jump()
    expect(scrollIntoView).toHaveBeenLastCalledWith({ block: 'start', behavior: 'smooth' })
    region().scrollTop = 0; await scroll()
    expect(host.querySelectorAll('[data-message-index]').length).toBe(TRANSCRIPT_BATCH)
    await act(async () => { region().dispatchEvent(new Event('scrollend')) })
    expect(host.querySelectorAll('[data-message-index]').length).toBeGreaterThan(TRANSCRIPT_BATCH)

    document.documentElement.setAttribute('data-motion', 'off')
    try {
      jump()
      expect(scrollIntoView).toHaveBeenLastCalledWith({ block: 'start', behavior: 'auto' })
    } finally {
      document.documentElement.removeAttribute('data-motion')
    }
  })

  it('keeps one older read in flight, reports failure with Retry, and retries only on explicit intent', async () => {
    geometry(true)
    const reads: Array<ReturnType<typeof deferred>> = []
    const loadOlder = vi.fn(() => { const next = deferred(); reads.push(next); return next.promise })
    await act(async () => root.render(transcript({ nativeStartIndex: 40, loadOlder })))
    await scroll(); await scroll()
    expect(loadOlder).toHaveBeenCalledTimes(1)
    await act(async () => reads[0].resolve('failed'))
    expect(host.textContent).toContain('Earlier messages could not be loaded. Loaded messages are still shown.')
    expect(host.textContent).toContain('Row 3')
    // The failed position is held: re-renders and layout fill do not loop.
    await act(async () => root.render(transcript({ nativeStartIndex: 40, loadOlder })))
    expect(loadOlder).toHaveBeenCalledTimes(1)
    await act(async () => host.querySelector<HTMLButtonElement>('.uam-subagent-transcript__meta button')!.click())
    expect(loadOlder).toHaveBeenCalledTimes(2)
    expect(host.textContent).toContain('Loading earlier messages…')
  })

  it('treats an unchanged read as held until the reader scrolls up again', async () => {
    geometry(true)
    const loadOlder = vi.fn().mockResolvedValue('unchanged')
    await act(async () => root.render(transcript({ nativeStartIndex: 40, loadOlder })))
    const automatic = loadOlder.mock.calls.length
    expect(automatic).toBeLessThanOrEqual(1)
    await act(async () => root.render(transcript({ nativeStartIndex: 40, loadOlder })))
    expect(loadOlder).toHaveBeenCalledTimes(automatic)
    await scroll()
    expect(loadOlder).toHaveBeenCalledTimes(automatic + 1)
    expect(host.textContent).not.toContain('could not be loaded')
  })

  it('clears a held failure when Refresh changes the recovery epoch at the same history position', async () => {
    geometry(true)
    const loadOlder = vi.fn().mockResolvedValue('failed')
    await act(async () => root.render(transcript({ nativeStartIndex: 40, loadOlder, recoveryEpoch: 0 })))
    await scroll()
    const before = loadOlder.mock.calls.length
    expect(host.textContent).toContain('could not be loaded')
    await act(async () => root.render(transcript({ nativeStartIndex: 40, loadOlder, recoveryEpoch: 1 })))
    expect(loadOlder.mock.calls.length).toBeGreaterThan(before) // the short window refills itself once more
  })

  it('ignores an older read that settles after the view unmounts', async () => {
    geometry(true)
    const read = deferred()
    const loadOlder = vi.fn(() => read.promise)
    await act(async () => root.render(transcript({ nativeStartIndex: 40, loadOlder })))
    await scroll()
    act(() => root.render(<div />))
    await act(async () => read.resolve('failed'))
    expect(host.textContent).toBe('')
  })

  it('flags any changed row (equal length edit or tool update) as new activity off the tail', async () => {
    geometry()
    const history = rows(30)
    act(() => root.render(transcript({ messages: history, contentVersion: history })))
    region().scrollTop = 600
    await scroll()
    expect(host.textContent).not.toContain('New activity')
    const edited = history.map((row, index) => index === 3 ? { ...row, content: 'Row 3!', toolCalls: [] } : row)
    act(() => root.render(transcript({ messages: edited, contentVersion: edited })))
    expect(host.textContent).toContain('New activity · Show latest')
  })

  it('does not flag a prepend that arrives with a new native start index', async () => {
    geometry()
    const history = rows(30, 40)
    act(() => root.render(transcript({ messages: history, nativeStartIndex: 40, contentVersion: history })))
    region().scrollTop = 600
    await scroll()
    const prepended = [...rows(20, 20), ...history]
    act(() => root.render(transcript({ messages: prepended, nativeStartIndex: 20, contentVersion: prepended })))
    expect(host.textContent).not.toContain('New activity')
  })

  it('offers full-history rail marks only with a read path and starts no read from showing them', async () => {
    geometry()
    const loadOlder = vi.fn().mockResolvedValue('unchanged')
    act(() => root.render(transcript({ messages: rows(4, 100), nativeStartIndex: 100, userMessageIndices: [0, 50, 100, 102] })))
    expect(host.querySelector('[role="slider"]')!.getAttribute('aria-valuetext') ?? '').not.toContain('of 4')
    await act(async () => root.render(transcript({ messages: rows(4, 100), nativeStartIndex: 100, userMessageIndices: [0, 50, 100, 102], loadOlder })))
    expect(host.querySelector('[role="slider"]')!.getAttribute('aria-valuetext')).toContain('of 4')
    expect(loadOlder.mock.calls.length).toBeLessThanOrEqual(1) // at most the short-window fill, never one per mark
    expect(MAX_JUMP_PAGES).toBe(10)
  })
})

describe('subagent header', () => {
  it('is the only bar: Back, child identity with its parent, then Refresh; no close or view toggles', () => {
    const onBack = vi.fn(), onRefresh = vi.fn()
    act(() => root.render(<SubagentHeader title="Reviewer" parentName="Main chat" detail="Running" onBack={onBack} onRefresh={onRefresh} />))
    expect(host.querySelectorAll('nav[aria-label="Subagent lineage"]')).toHaveLength(1)
    expect(host.textContent).toContain('Subagent of Main chat · Running')
    expect(buttons()).toEqual(['Back to parent', 'Refresh conversation'])
    act(() => host.querySelector<HTMLButtonElement>('[aria-label="Refresh conversation"]')!.click())
    expect(onRefresh).toHaveBeenCalledTimes(1)
    expect(onBack).not.toHaveBeenCalled()
  })

  it('offers Close only to an orphan with no parent to return to', () => {
    // The existing view-close callback is now the single Back to chats return, without a separate X.
    const onClose = vi.fn()
    act(() => root.render(<SubagentHeader title="Orphan" onClose={onClose} />))
    expect(host.textContent).toContain('Parent unavailable')
    const returnControl = host.querySelector<HTMLButtonElement>('nav button')!
    expect(returnControl.disabled).toBe(false)
    expect(buttons()).toEqual(['Back to chats'])
    expect(host.querySelector('[aria-label="Close subagent view"]')).toBeNull()
    act(() => returnControl.click())
    expect(onClose).toHaveBeenCalledTimes(1)
  })
})

describe('agent surface tone', () => {
  const luminance = (hex: string) => {
    const digits = hex.length === 4 ? [...hex.slice(1)].map(digit => digit + digit) : [hex.slice(1, 3), hex.slice(3, 5), hex.slice(5, 7)]
    const [r, g, b] = digits.map(pair => { const c = parseInt(pair, 16) / 255; return c <= 0.03928 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4 })
    return 0.2126 * r + 0.7152 * g + 0.0722 * b
  }
  it('rests agent surfaces darker than the transcript in light themes (Paper included) and lighter in dark themes', () => {
    const themes = [...indexCss.matchAll(/([^{}]+)\{([^{}]*)\}/g)].flatMap(([, selector, body]) => {
      const bg = /--bg:\s*(#[0-9a-f]{3,6})\b/i.exec(body)?.[1], up = /--surface-up:\s*(#[0-9a-f]{3,6})\b/i.exec(body)?.[1]
      return bg && up ? [{ selector: selector.trim(), bg: luminance(bg), up: luminance(up) }] : []
    })
    expect(themes.some(theme => theme.selector.includes('paper'))).toBe(true)
    for (const theme of themes) {
      if (theme.bg > 0.5) expect(theme.up, theme.selector).toBeLessThan(theme.bg)
      else expect(theme.up, theme.selector).toBeGreaterThan(theme.bg)
    }
    expect(cardCss).toMatch(/\.uam-btn\.uam-subagent-card \{[^}]*background: var\(--surface-up\)/)
    expect(groupCss).toMatch(/summary:hover \{[^}]*background: var\(--surface-up\)/)
    expect(panelCss).toMatch(/summary:hover \{[^}]*background: var\(--surface-up\)/)
  })
})

describe('single return control without an orphan Close X', () => {
  it('returns an orphan to chats using only its existing view-exit callback, preserves normal parent Back, and disables an unavailable return', () => {
    const onClose = vi.fn(), onBack = vi.fn(), onRefresh = vi.fn()
    act(() => root.render(<SubagentHeader title="Owned orphan" onClose={onClose} onRefresh={onRefresh} />))
    expect(host.querySelectorAll('nav[aria-label="Subagent lineage"]')).toHaveLength(1)
    expect(host.textContent).toContain('Parent unavailable')
    expect(host.textContent).not.toContain('Subagent of')
    const returnToChats = [...host.querySelectorAll<HTMLButtonElement>('nav button')].find(button => button.textContent === 'Back to chats')
    expect(returnToChats, 'An orphan must have one enabled view-exit return instead of fake parent Back plus X').toBeDefined()
    expect(returnToChats!.disabled).toBe(false)
    expect(buttons()).toEqual(['Back to chats', 'Refresh conversation'])
    expect(host.querySelector('[aria-label="Close subagent view"], svg.lucide-x')).toBeNull()
    act(() => returnToChats!.click())
    expect(onClose).toHaveBeenCalledTimes(1)
    expect(onBack).not.toHaveBeenCalled()
    expect(onRefresh).not.toHaveBeenCalled()

    act(() => root.render(<SubagentHeader title="No return destination" />))
    expect(host.textContent).toContain('Parent unavailable')
    expect(buttons()).toEqual(['Back'])
    const unavailable = host.querySelector<HTMLButtonElement>('nav button')!
    expect(unavailable.disabled).toBe(true)
    act(() => unavailable.click())
    expect(onClose).toHaveBeenCalledTimes(1)
    expect(onBack).not.toHaveBeenCalled()

    act(() => root.render(<SubagentHeader title="Known child" parentName="Exact parent" onBack={onBack} onClose={onClose} onRefresh={onRefresh} />))
    expect(buttons()).toEqual(['Back to parent', 'Refresh conversation'])
    expect(host.textContent).toContain('Subagent of Exact parent')
    expect(host.querySelector('[aria-label="Close subagent view"], svg.lucide-x')).toBeNull()
    act(() => host.querySelector<HTMLButtonElement>('nav button')!.click())
    expect(onBack).toHaveBeenCalledTimes(1)
    expect(onClose).toHaveBeenCalledTimes(1)
    expect(onRefresh).not.toHaveBeenCalled()
  })
})
