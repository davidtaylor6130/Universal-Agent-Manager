import { readFileSync } from 'node:fs'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

const boot = readFileSync('public/ui-recovery.js', 'utf8')
let observer: MutationObserverCallback
let deadline: () => void

beforeEach(() => {
  document.body.innerHTML = '<div id="root"></div><section id="uam-startup-recovery"><button id="uam-reload-interface">Reload interface</button></section>'
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
  it('starts visible so a missing boot script cannot leave an empty window', () => {
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
