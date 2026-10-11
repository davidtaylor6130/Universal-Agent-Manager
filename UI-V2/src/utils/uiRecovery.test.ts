import { readFileSync } from 'node:fs'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

const boot = readFileSync('public/ui-recovery.js', 'utf8')
const html = readFileSync('index.html', 'utf8')
let initiallyHidden: boolean
let observer: MutationObserverCallback
let deadline: () => void

beforeEach(() => {
  document.body.innerHTML = new DOMParser().parseFromString(html, 'text/html').body.innerHTML
  initiallyHidden = document.getElementById('uam-startup-recovery')!.hidden
  vi.stubGlobal('MutationObserver', class {
    constructor(callback: MutationObserverCallback) { observer = callback }
    observe() {}
  })
  vi.spyOn(window, 'setTimeout').mockImplementation((callback) => { deadline = callback as () => void; return 1 })
  vi.spyOn(window, 'clearTimeout').mockImplementation(() => {})
  // The production classic script must work without React, CEF, or module imports.
  new Function(boot)()
})
afterEach(() => { vi.restoreAllMocks(); vi.unstubAllGlobals(); document.body.innerHTML = '' })

function changed() { observer([], {} as MutationObserver) }

describe('independent startup recovery', () => {
  it('keeps recovery in the HTML if its script is missing, then hides it while startup is healthy', () => {
    expect(initiallyHidden).toBe(false)
    expect(document.getElementById('uam-startup-recovery')!.hidden).toBe(true)
  })
  it.each(['error', 'unhandledrejection'])('shows recovery for a startup %s', (type) => {
    window.dispatchEvent(type === 'error' ? new ErrorEvent(type, { error: new Error('Startup failed') }) : new Event(type))
    expect(document.getElementById('uam-startup-recovery')!.hidden).toBe(false)
  })
  it('hides after mount and returns when React clears its root', () => {
    const root = document.getElementById('root')!
    const fallback = document.getElementById('uam-startup-recovery')!
    root.innerHTML = '<main>Chat</main>'
    changed()
    expect(fallback.hidden).toBe(true)
    root.replaceChildren()
    changed()
    expect(fallback.hidden).toBe(false)
  })
  it('shows recovery after the startup deadline when no UI mounts', () => {
    const fallback = document.getElementById('uam-startup-recovery')!
    fallback.hidden = true
    deadline()
    expect(fallback.hidden).toBe(false)
  })
  it('does not obscure a mounted app when an unrelated promise rejects', () => {
    document.getElementById('root')!.innerHTML = '<main>Chat</main>'
    changed()
    window.dispatchEvent(new Event('unhandledrejection'))
    expect(document.getElementById('uam-startup-recovery')!.hidden).toBe(true)
  })
})
