import { act } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { usePassiveHistoryWindow, type PassiveHistoryConfig } from './usePassiveHistoryWindow'
import { SubagentTranscript } from './SubagentTranscript'
import { useAppStore } from '../../store/useAppStore'
import type { Message } from '../../types/message'

;(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
let host: HTMLDivElement, root: Root, cef: Window['cefQuery'], config: PassiveHistoryConfig, currentOwner: string
let requests: { payload: Record<string, number | string>; finish: (override?: unknown) => void; done: boolean }[]
const book: Message[] = Array.from({ length: 800 }, (_, index) => ({ id: `owned-${index}`, sessionId: 'passive-owned',
  role: index % 2 ? 'assistant' : 'user', content: `Owned raw row ${index}`, createdAt: new Date(1792630800000 + index * 1000) }))
let previousAlignment: PropertyDescriptor | undefined
let geometryObserver: MutationObserver
const rowHeight = (row: Element) => 40 + ((row.textContent?.length ?? 0) % 3) * 4
const region = () => host.querySelector<HTMLElement>('[role="region"]')!
const row = (index: number) => host.querySelector<HTMLElement>(`[data-message-index="${index}"]`)
const firstVisible = () => [...region().querySelectorAll<HTMLElement>('[data-message-index]')].find(item => item.getBoundingClientRect().bottom > 100)!
const relative = (element: Element) => element.getBoundingClientRect().top - region().getBoundingClientRect().top
const complete = async (override?: unknown) => { const pending = requests.find(item => !item.done)!; expect(pending).toBeDefined(); await act(async () => pending.finish(override)) }
function Harness() {
  const history = usePassiveHistoryWindow(config)
  return <><button onClick={history.invalidate}>Refresh fixture</button><SubagentTranscript messages={config.messages}
    nativeStartIndex={config.first} userMessageIndices={Array.from({ length: 400 }, (_, i) => i * 2)} fallbackProviderId="codex-cli"
    sessionId="passive-owned" viewportKey="" contentVersion={config.messages} label="Owned window" scopedView={history.view} /></>
}
const mount = async () => { await act(async () => root.render(<Harness key={config.owner} />)) }
const clickMark = async (last = false) => { const rail = host.querySelector<HTMLElement>('[role="slider"]')!; await act(async () => rail.dispatchEvent(new MouseEvent('click', { bubbles: true, clientY: last ? 220 : 20 }))) }
const scrollTo = async (top: number) => { await act(async () => { region().scrollTop = top; region().dispatchEvent(new Event('scroll')) }) }
beforeEach(() => {
  cef = window.cefQuery; requests = []; host = document.createElement('div'); document.body.append(host); root = createRoot(host)
  currentOwner = `pane:${Math.random()}:child-birth:parent-birth:digest-v1`
  const owner = currentOwner
  config = { enabled: true, owner, childId: 'passive-owned', digest: 'owned-v1', total: 800,
    messages: book.slice(750), first: 750, current: () => currentOwner === owner }
  window.cefQuery = ({ request, onSuccess }) => {
    const envelope = JSON.parse(request)
    const pending = { payload: envelope.payload, done: false, finish: (override?: unknown) => {
      if (pending.done) return
      pending.done = true
      const end = Math.min(800, Number(envelope.payload.before))
      const first = Math.max(0, end - Number(envelope.payload.limit))
      onSuccess(JSON.stringify(override ?? { ok: true, data: { chatId: 'passive-owned', messagesDigest: 'owned-v1',
        startIndex: first, totalCount: 800, messages: book.slice(first, end).map(message => ({
          role: message.role, content: message.content, createdAt: message.createdAt.toISOString() })) } }))
    } }
    requests.push(pending)
  }
  const positions = new WeakMap<Element, number>()
  const owned = (element: Element) => element.classList.contains('uam-subagent-transcript')
  const rows = (element: Element) => [...element.querySelectorAll('[data-message-index]')]
  let generation = 0
  const layouts = new WeakMap<Element, { generation: number; total: number; offsets: Map<Element, number>; heights: Map<Element, number> }>()
  geometryObserver = new MutationObserver(() => { generation += 1 })
  geometryObserver.observe(host, { childList: true, subtree: true, characterData: true, attributes: true, attributeFilter: ['data-message-index'] })
  const layout = (container: Element) => {
    // React layout effects can read geometry before the observer callback runs.
    if (geometryObserver.takeRecords().length) generation += 1
    const cached = layouts.get(container)
    if (host.contains(container) && cached?.generation === generation) return cached
    const offsets = new Map<Element, number>(), heights = new Map<Element, number>()
    let total = 0
    for (const item of rows(container)) {
      const height = rowHeight(item)
      offsets.set(item, total); heights.set(item, height); total += height
    }
    const measured = { generation, total, offsets, heights }
    layouts.set(container, measured)
    return measured
  }
  const offset = (element: Element, container: Element) => { const measured = layout(container); return measured.offsets.get(element) ?? measured.total }
  const rect = (top: number, height: number) => ({ top, bottom: top + height, height, width: 400, left: 0, right: 400, x: 0, y: top, toJSON: () => ({}) }) as DOMRect
  vi.spyOn(Element.prototype, 'scrollHeight', 'get').mockImplementation(function (this: Element) { return owned(this) ? layout(this).total : 0 })
  vi.spyOn(Element.prototype, 'clientHeight', 'get').mockImplementation(function (this: Element) { return owned(this) ? 280 : 0 })
  vi.spyOn(Element.prototype, 'scrollTop', 'get').mockImplementation(function (this: Element) { return positions.get(this) ?? 0 })
  vi.spyOn(Element.prototype, 'scrollTop', 'set').mockImplementation(function (this: Element, top: number) { positions.set(this, Math.max(0, Math.min(top, this.scrollHeight - this.clientHeight))) })
  vi.spyOn(Element.prototype, 'getBoundingClientRect').mockImplementation(function (this: Element) {
    if (owned(this)) return rect(100, 280)
    if (this.getAttribute('role') === 'slider') return rect(20, 200)
    const container = this.closest('.uam-subagent-transcript')
    if (!container) return rect(0, 0)
    const measured = layout(container)
    return rect(100 + (measured.offsets.get(this) ?? measured.total) - container.scrollTop, measured.heights.get(this) ?? rowHeight(this))
  })
  previousAlignment = Object.getOwnPropertyDescriptor(Element.prototype, 'scrollIntoView')
  Object.defineProperty(Element.prototype, 'scrollIntoView', { configurable: true, value: function (this: Element) {
    const container = this.closest('.uam-subagent-transcript'); if (container) container.scrollTop = offset(this, container)
  } })
})
afterEach(async () => {
  await act(async () => { root.unmount(); for (const pending of requests) pending.finish() })
  geometryObserver.disconnect()
  host.remove(); window.cefQuery = cef
  if (previousAlignment) Object.defineProperty(Element.prototype, 'scrollIntoView', previousAlignment)
  else Reflect.deleteProperty(Element.prototype, 'scrollIntoView')
  vi.restoreAllMocks()
})

describe('passive history geometry fixture', () => {
  it('reflects synchronous text, membership, order and detached changes while reading current scroll position', () => {
    const container = document.createElement('div')
    container.className = 'uam-subagent-transcript'
    const items = Array.from({ length: 8 }, (_, index) => {
      const item = document.createElement('div')
      item.dataset.messageIndex = String(index)
      item.textContent = index === 0 ? 'a' : index === 1 ? 'bb' : 'ccc'
      container.append(item)
      return item
    })
    host.append(container)
    const [first, second, third] = items
    expect(container.scrollHeight).toBe(332)
    expect(first.getBoundingClientRect().height).toBe(44)
    expect(second.getBoundingClientRect().top).toBe(144)
    // All mutations and reads are synchronous: no observer callback is awaited.
    first.textContent = 'ab'
    expect(container.scrollHeight).toBe(336)
    expect(second.getBoundingClientRect().top).toBe(148)
    second.removeAttribute('data-message-index')
    expect(container.scrollHeight).toBe(288)
    expect(third.getBoundingClientRect().top).toBe(148)
    expect(second.getBoundingClientRect().top).toBe(388)
    expect(second.getBoundingClientRect().height).toBe(48)
    second.dataset.messageIndex = '1'
    container.insertBefore(third, first)
    expect(container.scrollHeight).toBe(336)
    expect(third.getBoundingClientRect().top).toBe(100)
    expect(first.getBoundingClientRect().top).toBe(140)
    expect(second.getBoundingClientRect().top).toBe(188)
    container.scrollTop = 40
    expect(first.getBoundingClientRect().top).toBe(100)
    expect(second.getBoundingClientRect().top).toBe(148)
    container.remove()
    first.textContent = 'abcdef'
    expect(container.scrollHeight).toBe(328)
    expect(second.getBoundingClientRect().top).toBe(140)
  })
})

describe('owned passive target windows, source-shaped controls UNRUN', () => {
  it('uses one target request, aligns exact raw row and never mutates the shared store tail or runtime', async () => {
    const store = useAppStore.getState(); await mount(); await clickMark()
    expect(requests).toHaveLength(1)
    expect(requests[0].payload).toMatchObject({ chatId: 'passive-owned', limit: 50, before: 26, expectedMessagesDigest: 'owned-v1', targetMessageIndex: 0 })
    await complete(); expect(row(0)?.textContent).toContain('Owned raw row 0'); expect(relative(row(0)!)).toBe(0)
    expect(useAppStore.getState()).toBe(store); expect(host.querySelector('textarea, input, form')).toBeNull()
  })
  it('reads onward across the 300-row cap and back with the SAME visible raw row, text and relative offset', async () => {
    await mount(); await clickMark(); await complete()
    for (let page = 0; page < 3; page += 1) {
      await scrollTo(region().scrollHeight - region().clientHeight - 10)
      const anchor = firstVisible(); const index = Number(anchor.dataset.messageIndex); const text = anchor.querySelector('article')?.textContent; const top = relative(anchor)
      expect(text).toBe(book[index].content)
      expect(requests.at(-1)?.payload.limit).toBeLessThanOrEqual(100)
      await complete()
      expect(row(index)?.querySelector('article')?.textContent).toBe(text); expect(relative(row(index)!)).toBe(top)
      expect(host.querySelectorAll('[data-message-index]').length).toBeLessThanOrEqual(300)
      expect(host.textContent).not.toContain('New activity')
    }
    expect(row(0)).toBeNull(); expect(row(325)).not.toBeNull()
    await scrollTo(0); const anchor = firstVisible(); const index = Number(anchor.dataset.messageIndex); const top = relative(anchor); const text = anchor.querySelector('article')?.textContent
    expect(text).toBe(book[index].content)
    await complete(); expect(row(index)?.querySelector('article')?.textContent).toBe(text); expect(relative(row(index)!)).toBe(top)
  })
  it('Show latest explicitly reads the current digest-checked book tail rather than the around-window bottom', async () => {
    await mount(); await clickMark(); await complete()
    const latest = [...host.querySelectorAll('button')].find(item => item.textContent === 'Show latest')!
    expect(latest).toBeDefined(); await act(async () => latest.click())
    expect(requests.at(-1)?.payload.targetMessageIndex).toBe(799); await complete(); expect(row(799)).not.toBeNull()
    expect(region().scrollTop).toBe(region().scrollHeight - region().clientHeight)
  })
  it('restores the absolute raw reading row and offset on same-owner remount using one bounded reread', async () => {
    await mount()
    expect(row(400)).toBeNull()
    await act(async () => host.querySelector<HTMLElement>('[role="slider"]')!.dispatchEvent(new MouseEvent('click', { bubbles: true, clientY: 120 })))
    expect(requests.at(-1)?.payload.targetMessageIndex).toBe(400)
    await complete(); await scrollTo(400)
    const anchor = firstVisible(); const index = Number(anchor.dataset.messageIndex); const top = relative(anchor)
    await act(async () => root.render(<div />)); await mount()
    expect(requests.at(-1)?.payload.targetMessageIndex).toBe(index); await complete()
    expect(relative(row(index)!)).toBe(top)
  })
  it('keeps one pending read under double intent and rejects a stale owner success', async () => {
    await mount(); await clickMark(); await clickMark(); expect(requests).toHaveLength(1)
    currentOwner = 'new-child-birth'; await complete(); expect(row(0)).toBeNull(); expect(row(798)).not.toBeNull()
  })
  it('Refresh invalidates held success without replacing available rows or replaying a request', async () => {
    await mount(); await clickMark(); await act(async () => host.querySelector<HTMLButtonElement>('button')!.click())
    await complete(); expect(row(0)).toBeNull(); expect(row(798)).not.toBeNull(); expect(requests).toHaveLength(1)
  })
  it.each(['refusal', 'unchanged', 'digest', 'total', 'row'])('retains rows on %s and retries only through the explicit history action', async kind => {
    await mount(); await clickMark()
    const data = { chatId: 'passive-owned', messagesDigest: kind === 'digest' ? 'changed' : 'owned-v1', startIndex: 0,
      totalCount: kind === 'total' ? 801 : 800, unchanged: kind === 'unchanged', messages: [{ role: kind === 'row' ? 'alien' : 'user', content: 'Owned raw row 0', createdAt: book[0].createdAt.toISOString() }] }
    await complete(kind === 'refusal' ? { ok: false, error: 'Read refused' } : { ok: true, data })
    expect(row(798)).not.toBeNull(); expect(row(0)).toBeNull(); expect(requests).toHaveLength(1)
    const retry = [...host.querySelectorAll('button')].find(item => item.textContent === 'Retry history')!
    expect(retry).toBeDefined(); await act(async () => retry.click()); expect(requests).toHaveLength(2)
    await complete(); expect(row(0)).not.toBeNull()
  })
})


describe('passive window reviewed reading guards', () => {
  it('reactive Show latest reflects leaving the initial stored tail before any target request', async () => {
    await mount()
    expect(requests).toHaveLength(0)
    expect(row(798)?.querySelector('article')?.textContent).toBe(book[798].content)
    expect([...host.querySelectorAll('button')].find(item => item.textContent === 'Show latest')).toBeUndefined()
    const top = region().scrollHeight - region().clientHeight - 100
    expect(top).toBeGreaterThan(320)
    await scrollTo(top)
    expect(requests).toHaveLength(0)
    expect([...host.querySelectorAll('button')].find(item => item.textContent === 'Show latest')).toBeDefined()
  })

  it('reactive Show latest reflects leaving the real book tail without another read', async () => {
    await mount(); await clickMark(); await complete()
    const latest = [...host.querySelectorAll('button')].find(item => item.textContent === 'Show latest')!
    expect(latest).toBeDefined(); await act(async () => latest.click()); await complete()
    expect(row(799)).not.toBeNull()
    expect([...host.querySelectorAll('button')].find(item => item.textContent === 'Show latest')).toBeUndefined()
    const count = requests.length
    const top = region().scrollHeight - region().clientHeight - 100
    expect(top).toBeGreaterThan(320)
    await scrollTo(top)
    expect(requests).toHaveLength(count)
    expect([...host.querySelectorAll('button')].find(item => item.textContent === 'Show latest')).toBeDefined()
  })

  it('requires an actual visible anchor before trimming a bounded reading window', async () => {
    const shared = useAppStore.getState()
    await mount(); await clickMark(); await complete()
    for (let page = 0; page < 3; page += 1) {
      await scrollTo(region().scrollHeight - region().clientHeight - 10)
      await complete()
    }
    expect(row(0)).toBeNull()
    const retainedFirst = row(26); const retainedLast = row(325)
    expect(retainedFirst).not.toBeNull(); expect(retainedLast).not.toBeNull()
    const geometry = vi.mocked(Element.prototype.getBoundingClientRect).getMockImplementation()!
    expect(geometry).toBeDefined()
    vi.mocked(Element.prototype.getBoundingClientRect).mockImplementation(function (this: Element) {
      const box = geometry.call(this)
      return this.hasAttribute('data-message-index') ? { ...box, y: -1000, top: -1000, bottom: -900 } as DOMRect : box
    })
    const count = requests.length
    await scrollTo(0)
    const pending = requests.find(item => !item.done)
    if (pending) await complete()
    expect(row(0), 'without a visible DOM witness a trim must retain the prior reading window').toBeNull()
    expect(row(26)).toBe(retainedFirst); expect(row(325)).toBe(retainedLast)
    expect(requests.length).toBeLessThanOrEqual(count + 1)
    expect(useAppStore.getState()).toBe(shared)
  })

  it.each([undefined, 'legacy-unknown-time'])('accepts canonical rows without reliable timestamps (%s)', async timestamp => {
    const shared = useAppStore.getState()
    await mount(); await clickMark()
    expect(requests).toHaveLength(1)
    const messages = book.slice(0, 26).map((message, index) => ({ role: message.role, content: message.content,
      createdAt: index === 0 ? timestamp : message.createdAt.toISOString() }))
    await complete({ ok: true, data: { chatId: 'passive-owned', messagesDigest: 'owned-v1', startIndex: 0,
      totalCount: 800, messages } })
    expect(row(0), 'an optional timestamp must not reject otherwise canonical readable history').not.toBeNull()
    expect(row(0)?.textContent).toContain('Owned raw row 0')
    expect([...host.querySelectorAll('button')].find(item => item.textContent === 'Retry history')).toBeUndefined()
    expect(useAppStore.getState()).toBe(shared)
  })
})

describe('passive local reading recovery', () => {
  it.each(['beginning', 'end'] as const)('does not offer an impossible retry at the absolute %s without visible row geometry', async edge => {
    const shared = useAppStore.getState()
    // Both rail marks must require a bounded read; the default tail already contains the end mark.
    config = { ...config, first: 350, messages: book.slice(350, 400) }
    await mount(); await clickMark(edge === 'end'); await complete()
    const target = edge === 'beginning' ? 0 : 798
    expect(requests).toHaveLength(1)
    expect(requests[0].payload.targetMessageIndex).toBe(target)
    expect(requests.every(request => request.done)).toBe(true)
    const retained = row(target)
    expect(retained?.querySelector('article')?.textContent).toBe(book[target].content)
    // The beginning is fully revealed, and the end includes the actual final raw message.
    expect(row(edge === 'beginning' ? 0 : 799)).not.toBeNull()
    const geometry = vi.mocked(Element.prototype.getBoundingClientRect).getMockImplementation()!
    vi.mocked(Element.prototype.getBoundingClientRect).mockImplementation(function (this: Element) {
      const box = geometry.call(this)
      return this.hasAttribute('data-message-index') ? { ...box, y: -1000, top: -1000, bottom: -900 } as DOMRect : box
    })
    await scrollTo(edge === 'beginning' ? 0 : region().scrollHeight - region().clientHeight)
    expect(row(target)).toBe(retained)
    expect(host.textContent, 'A known history edge must not fabricate anchor recovery').not.toContain('The visible reading position is unavailable.')
    expect([...host.querySelectorAll('button')].find(item => item.textContent === 'Retry history')).toBeUndefined()
    expect(requests).toHaveLength(1)
    expect(useAppStore.getState()).toBe(shared)
    expect(host.querySelector('textarea, input, form')).toBeNull()
  })

  it('retains explicit anchor recovery when an adjacent page actually exists', async () => {
    const shared = useAppStore.getState()
    await mount()
    await act(async () => host.querySelector<HTMLElement>('[role="slider"]')!.dispatchEvent(new MouseEvent('click', { bubbles: true, clientY: 120 })))
    expect(requests.at(-1)?.payload.targetMessageIndex).toBe(400)
    await complete()
    const retained = row(400)
    expect(retained?.querySelector('article')?.textContent).toBe(book[400].content)
    const geometry = vi.mocked(Element.prototype.getBoundingClientRect).getMockImplementation()!
    vi.mocked(Element.prototype.getBoundingClientRect).mockImplementation(function (this: Element) {
      const box = geometry.call(this)
      return this.hasAttribute('data-message-index') ? { ...box, y: -1000, top: -1000, bottom: -900 } as DOMRect : box
    })
    await scrollTo(0)
    expect(host.textContent).toContain('The visible reading position is unavailable.')
    const retry = [...host.querySelectorAll('button')].find(item => item.textContent === 'Retry history')
    expect(retry).toBeDefined()
    expect(requests).toHaveLength(1)
    vi.mocked(Element.prototype.getBoundingClientRect).mockImplementation(geometry)
    await act(async () => retry!.click())
    expect(requests).toHaveLength(2)
    expect(requests[1].payload).toMatchObject({ chatId: 'passive-owned', limit: 100, before: 376 })
    await complete()
    expect(row(400)).toBe(retained)
    expect(host.textContent).not.toContain('The visible reading position is unavailable.')
    expect(useAppStore.getState()).toBe(shared)
  })

  it.each(['unavailable retry', 'in-window rail recovery'])('keeps retained initial history truthful after missing visible anchor: %s', async mode => {
    const shared = useAppStore.getState()
    await mount()
    expect(requests).toHaveLength(0)
    const geometry = vi.mocked(Element.prototype.getBoundingClientRect).getMockImplementation()!
    expect(geometry).toBeDefined()
    vi.mocked(Element.prototype.getBoundingClientRect).mockImplementation(function (this: Element) {
      const box = geometry.call(this)
      return this.hasAttribute('data-message-index') ? { ...box, y: -1000, top: -1000, bottom: -900 } as DOMRect : box
    })
    await scrollTo(0)
    expect(host.textContent).toContain('The visible reading position is unavailable.')
    expect(row(798)?.querySelector('article')?.textContent).toBe(book[798].content)
    expect(requests).toHaveLength(0)
    if (mode === 'unavailable retry') {
      expect([...host.querySelectorAll('button')].find(item => item.textContent === 'Retry history')).toBeUndefined()
    } else {
      vi.mocked(Element.prototype.getBoundingClientRect).mockImplementation(geometry)
      const rail = host.querySelector<HTMLElement>('[role="slider"]')!
      const box = rail.getBoundingClientRect()
      await act(async () => rail.dispatchEvent(new MouseEvent('click', { bubbles: true, clientY: box.top + box.height * (750 / 798) })))
      expect(host.textContent).not.toContain('The visible reading position is unavailable.')
      expect(row(750)?.querySelector('article')?.textContent).toBe(book[750].content)
      expect(relative(row(750)!)).toBe(0)
      expect(requests).toHaveLength(0)
    }
    expect(useAppStore.getState()).toBe(shared)
  })
})
