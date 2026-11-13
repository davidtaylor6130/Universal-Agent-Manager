import { readFileSync } from 'node:fs'
import { resolve } from 'node:path'
import { act } from 'react'
import { createRoot } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { HtmlViewCard } from './HtmlViewCard'
import { HTML_VIEW_SANDBOX, prepareHtmlViewDocument } from './htmlView'
import { sanitizeTurnEvent } from '../../store/cpp/sanitizers'
import { PersistedMessageContent } from './MessageBlocks'
import type { Message } from '../../types/message'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true

const root = resolve(__dirname, '../../../..')
const frameHost = readFileSync(resolve(root, 'UI-V2/public/html-view-frame.html'), 'utf8')
const nativePolicy = /kContentSecurityPolicy =\s*((?:\s*"[^"]*")+);/.exec(readFileSync(resolve(root, 'src/common/chat/html_view.h'), 'utf8'))![1]
  .match(/"([^"]*)"/g)!.map((part) => part.slice(1, -1)).join('')

function mount(node: React.ReactNode) {
  const host = document.createElement('div')
  document.body.appendChild(host)
  const view = createRoot(host)
  act(() => view.render(node))
  return { host, unmount: () => { act(() => view.unmount()); host.remove() } }
}

const stored = new Map<string, string>()
beforeEach(() => {
  stored.clear()
  Object.defineProperty(globalThis, 'localStorage', { configurable: true, value: {
    getItem: (key: string) => stored.get(key) ?? null,
    setItem: (key: string, value: string) => stored.set(key, value),
  } })
})

afterEach(() => {
  delete (window as { cefQuery?: unknown }).cefQuery
})

/** Mounts a card and chooses Run, as the user does before any page loads. */
function mountRunning(node: React.ReactNode) {
  const mounted = mount(node)
  act(() => mounted.host.querySelector<HTMLButtonElement>('.uam-html-view__gate button')!.click())
  return mounted
}

describe('HtmlViewCard run gate', () => {
  it('loads nothing until the user chooses Run', () => {
    const { host, unmount } = mount(<HtmlViewCard viewId="html-view:a" title="Chart" html="<script>x()</script>" />)
    expect(host.querySelector('iframe')).toBeNull()
    expect(host.querySelector('.uam-html-view')!.innerHTML).not.toContain('<script>')
    const run = host.querySelector<HTMLButtonElement>('button[aria-label="Run HTML view: Chart"]')!
    expect(run.textContent).toBe('Run')
    act(() => run.click())
    expect(host.querySelector('iframe')).not.toBeNull()
    unmount()
  })

  it('remembers Run for that view and page only', async () => {
    const settle = () => act(async () => { await new Promise((resolve) => setTimeout(resolve, 20)) })
    const first = mountRunning(<HtmlViewCard viewId="html-view:a" title="Chart" html="<p>one</p>" />)
    expect(document.activeElement).toBe(first.host.querySelector('iframe')) // Focus moves to the page, not the document body.
    await settle()
    first.unmount()
    const again = mount(<HtmlViewCard viewId="html-view:a" title="Chart" html="<p>one</p>" />)
    await settle()
    expect(again.host.querySelector('iframe')).not.toBeNull()
    again.unmount()
    for (const [viewId, html] of [['html-view:b', '<p>one</p>'], ['html-view:a', '<p>two</p>'], ['', '<p>one</p>']]) {
      const other = mount(<HtmlViewCard viewId={viewId} title="Chart" html={html} />)
      await settle()
      expect(other.host.querySelector('iframe')).toBeNull()
      other.unmount()
    }
  })

  it('asks again when the page changes under the same card', async () => {
    const host = document.createElement('div')
    document.body.appendChild(host)
    const view = createRoot(host)
    act(() => view.render(<HtmlViewCard viewId="html-view:a" title="Chart" html="<p>one</p>" />))
    act(() => host.querySelector<HTMLButtonElement>('.uam-html-view__gate button')!.click())
    expect(host.querySelector('iframe')).not.toBeNull()
    act(() => view.render(<HtmlViewCard viewId="html-view:a" title="Chart" html="<p>two</p>" />))
    expect(host.querySelector('iframe')).toBeNull()
    act(() => view.unmount())
    host.remove()
  })
})

describe('HtmlViewCard isolation', () => {
  it('hidden runtime check uses the same frame host and sandbox as the card, with no script of its own', () => {
    const probe = new DOMParser().parseFromString(readFileSync(resolve(root, 'UI-V2/public/html-view-probe.html'), 'utf8'), 'text/html')
    const frame = probe.querySelector('iframe')!
    expect(frame.getAttribute('sandbox')).toBe(HTML_VIEW_SANDBOX)
    expect(frame.getAttribute('src')).toBe('html-view-frame.html')
    expect(frame.getAttribute('referrerpolicy')).toBe('no-referrer')
    expect(frame.style.height).toBe('360px') // The card's inline height, so the page sees the same viewport.
    expect(probe.querySelectorAll('script, iframe:not([sandbox])')).toHaveLength(0)
  })

  it('loads the frame host in an opaque-origin sandbox instead of srcdoc', () => {
    const { host, unmount } = mountRunning(<HtmlViewCard viewId="html-view:test" title="Chart" html="<p>hi</p>" />)
    const frame = host.querySelector('iframe')!
    expect(frame.getAttribute('sandbox')).toBe('allow-scripts')
    expect(HTML_VIEW_SANDBOX).not.toContain('allow-same-origin')
    for (const token of ['allow-same-origin', 'allow-top-navigation', 'allow-popups', 'allow-forms', 'allow-modals'])
      expect(frame.getAttribute('sandbox')).not.toContain(token)
    expect(frame.hasAttribute('srcdoc')).toBe(false)
    expect(new URL(frame.src).pathname).toBe('/html-view-frame.html')
    expect(frame.getAttribute('referrerpolicy')).toBe('no-referrer')
    expect(frame.title).toBe('Chart')
    unmount()
  })

  it('sends only the page text into the frame, once', () => {
    const { host, unmount } = mountRunning(<HtmlViewCard viewId="html-view:test" title="Chart" html="<!doctype html><p>hi</p>" />)
    const frame = host.querySelector('iframe')!
    const post = vi.fn()
    Object.defineProperty(frame, 'contentWindow', { value: { postMessage: post } })
    act(() => { frame.dispatchEvent(new Event('load')) })
    act(() => { frame.dispatchEvent(new Event('load')) })
    expect(post).toHaveBeenCalledTimes(1)
    const [message, target] = post.mock.calls[0]
    expect(Object.keys(message).sort()).toEqual(['html', 'type'])
    expect(message.type).toBe('uam-html-view')
    expect(message.html.startsWith('<!doctype html><meta name="color-scheme"')).toBe(true)
    expect(target).toBe('*') // An opaque origin cannot be named; the payload carries no secrets.
    unmount()
  })

  it('ships a frame host whose policy blocks network, frames and same-origin access', () => {
    const meta = /<meta http-equiv="Content-Security-Policy" content="([^"]+)">/.exec(frameHost)![1]
    expect(meta).toBe(nativePolicy)
    for (const directive of ["default-src 'none'", "connect-src 'none'", "img-src data: blob:", "frame-src 'none'", "form-action 'none'", "base-uri 'none'"])
      expect(meta).toContain(directive)
    expect(meta).not.toMatch(/'self'|https?:/)
    // The host accepts a page only from its embedder and never calls back into it.
    expect(frameHost).toContain('event.source !== parent')
    expect(frameHost).not.toMatch(/cefQuery|postMessage|localStorage|fetch\(/)
  })

  it('removes WebRTC natively from every frame and turns network prediction off', () => {
    // No policy directive or switch stops these in this CEF, and V8 is unavailable here: the probe in html-views.md proved the behaviour.
    const security = readFileSync(resolve(root, 'src/cef/uam_cef_security.h'), 'utf8')
    const app = readFileSync(resolve(root, 'src/cef/uam_cef_app.cpp'), 'utf8')
    const renderer = readFileSync(resolve(root, 'src/cef/uam_cef_renderer_app.cpp'), 'utf8')
    for (const name of ['"RTCPeerConnection"', '"webkitRTCPeerConnection"', '"RTCIceTransport"']) expect(security).toContain(name)
    const hook = renderer.slice(renderer.indexOf('void UamCefRendererApp::OnContextCreated'), renderer.indexOf('void UamCefRendererApp::OnContextReleased'))
    expect(hook).toContain('kRendererGlobalsRemoved) (void)global->DeleteValue(name)')
    expect(hook.indexOf('DeleteValue')).toBeLessThan(hook.indexOf('IsTrustedRendererFrame'))
    expect(security).toContain('kNetworkPredictionNever = 2')
    expect(app).toContain('SetPreference(uam::cef::kNetworkPredictionPreference')
  })

  it('keeps the app policy frame allowance limited to the frame host', () => {
    const security = readFileSync(resolve(root, 'src/cef/uam_cef_security.h'), 'utf8')
    expect(security).toContain("frame-src uam://app/html-view-frame.html\"")
    expect(security).toContain('return is_main_frame && IsTrustedUiUrl(frame_url, trusted_index_url);')
  })
})

describe('HtmlViewCard presentation', () => {
  it('offers keyboard-reachable labelled actions and expands to the pane', () => {
    window.cefQuery = vi.fn() as unknown as typeof window.cefQuery
    const { host, unmount } = mountRunning(<HtmlViewCard viewId="html-view:test" title="Chart" html="<p>hi</p>" />)
    expect(host.querySelector('[role="group"]')!.getAttribute('aria-label')).toBe('HTML view: Chart')
    const labels = [...host.querySelectorAll('button')].map((button) => button.getAttribute('aria-label'))
    expect(labels).toEqual(['Expand HTML view', 'Open in browser', 'Copy HTML source', 'Save as HTML file'])
    const expand = host.querySelector<HTMLButtonElement>('button[aria-label="Expand HTML view"]')!
    expect(expand.getAttribute('aria-expanded')).toBe('false')
    act(() => expand.click())
    expect(host.querySelector('.uam-html-view--expanded')).not.toBeNull()
    expect(host.querySelector('button[aria-label="Collapse HTML view"]')!.getAttribute('aria-expanded')).toBe('true')
    unmount()
  })

  it('hides native-only actions outside the desktop app', () => {
    const { host, unmount } = mountRunning(<HtmlViewCard viewId="html-view:test" title="Chart" html="<p>hi</p>" />)
    const labels = [...host.querySelectorAll('button')].map((button) => button.getAttribute('aria-label'))
    expect(labels).toEqual(['Expand HTML view', 'Copy HTML source'])
    unmount()
  })

  it('stops CSS motion when motion is off and keeps standards mode', () => {
    const html = prepareHtmlViewDocument('<!DOCTYPE html><style>p{animation:spin 1s infinite}</style>', { scheme: 'light', stillMotion: true })
    expect(html.startsWith('<!DOCTYPE html><meta name="color-scheme" content="light"><style>')).toBe(true)
    expect(html).toContain('animation-duration:0s!important')
    expect(prepareHtmlViewDocument('<p>x</p>', { scheme: 'dark', stillMotion: false })).toBe('<meta name="color-scheme" content="dark"><p>x</p>')
  })

  it('reloads the frame with the new setting when motion changes', async () => {
    const matchMedia = window.matchMedia
    window.matchMedia = ((query: string) => ({ matches: false, media: query, addEventListener() {}, removeEventListener() {} })) as unknown as typeof window.matchMedia
    try {
      const { host, unmount } = mountRunning(<HtmlViewCard viewId="html-view:test" title="Chart" html="<p>hi</p>" />)
      const before = host.querySelector('iframe')!
      await act(async () => { document.documentElement.setAttribute('data-motion', 'off') })
      const after = host.querySelector('iframe')!
      expect(after).not.toBe(before)
      const post = vi.fn()
      Object.defineProperty(after, 'contentWindow', { value: { postMessage: post } })
      act(() => { after.dispatchEvent(new Event('load')) })
      expect(post.mock.calls[0][0].html).toContain('animation-duration:0s!important')
      unmount()
    } finally {
      window.matchMedia = matchMedia
      document.documentElement.removeAttribute('data-motion')
    }
  })

  it('renders a persisted html_view block in its transcript position', () => {
    const message = {
      id: 'm1', role: 'assistant', content: 'Done', createdAt: '2026-11-02T00:00:00Z',
      blocks: [{ type: 'html_view', text: '<p>chart</p>', title: 'Sales', requestId: 'html-view:a' }, { type: 'assistant_text', text: 'Done' }],
    } as unknown as Message
    const { host, unmount } = mount(<PersistedMessageContent message={message} onSelectTool={() => {}} />)
    const card = host.querySelector('.uam-html-view')!
    expect(card.textContent).toContain('Sales')
    expect(card.compareDocumentPosition(host.querySelector('.conversation-trace__text')!) & Node.DOCUMENT_POSITION_FOLLOWING).toBeTruthy()
    unmount()
  })

  it('sanitizes html_view events and drops ones without an identity or page', () => {
    expect(sanitizeTurnEvent({ type: 'html_view', text: '<p/>', requestId: 'html-view:a', title: 'T', extra: 1 })).toEqual({ type: 'html_view', text: '<p/>', requestId: 'html-view:a', title: 'T' })
    expect(sanitizeTurnEvent({ type: 'html_view', text: '<p/>', requestId: 'html-view:a' })).toEqual({ type: 'html_view', text: '<p/>', requestId: 'html-view:a', title: 'HTML view' })
    expect(sanitizeTurnEvent({ type: 'html_view', text: '<p/>' })).toBeNull()
    expect(sanitizeTurnEvent({ type: 'html_view', requestId: 'html-view:a' })).toBeNull()
  })
})
