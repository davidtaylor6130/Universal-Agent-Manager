/** Frame host shipped with the UI; UAM serves it under its own no-network policy (see uam_cef_security.h). */
export const HTML_VIEW_FRAME = 'html-view-frame.html'
/** Sandbox tokens for agent HTML views. Never add allow-same-origin: it would expose the UAM origin and bridge. */
export const HTML_VIEW_SANDBOX = 'allow-scripts'

export function htmlViewFrameUrl(base = typeof document === 'undefined' ? 'http://localhost/' : document.baseURI) {
  return new URL(HTML_VIEW_FRAME, base).href
}

/** Adds the app's colour scheme and, when motion is off, a stop for CSS animation, after any doctype. */
export function prepareHtmlViewDocument(html: string, options: { scheme: 'light' | 'dark'; stillMotion: boolean }) {
  const head = `<meta name="color-scheme" content="${options.scheme}">` + (options.stillMotion
    ? '<style>*,*::before,*::after{animation-duration:0s!important;animation-iteration-count:1!important;transition-duration:0s!important;scroll-behavior:auto!important}</style>'
    : '')
  const doctype = /^\s*<!doctype[^>]*>/i.exec(html)
  return doctype ? doctype[0] + head + html.slice(doctype[0].length) : head + html
}


const runStorageKey = 'uam-html-view-runs-v1'
// Keeps the newest choices only; older views simply ask again.
const maxRememberedRuns = 500

/** SHA-256 of the page text, so a remembered Run covers this exact page and never a different one under the same id. */
async function runKey(viewId: string, html: string) {
  const digest = new Uint8Array(await crypto.subtle.digest('SHA-256', new TextEncoder().encode(html)))
  return `${viewId}:${Array.from(digest, (byte) => byte.toString(16).padStart(2, '0')).join('')}`
}

function readRuns(): string[] {
  try {
    const value = JSON.parse(localStorage.getItem(runStorageKey) ?? '[]')
    return Array.isArray(value) ? value.filter((entry): entry is string => typeof entry === 'string') : []
  } catch {
    return []
  }
}

/** True only after the user chose Run for this view and page; there is no global allow. */
export async function htmlViewRunAllowed(viewId: string, html: string) {
  if (!viewId) return false
  try {
    return readRuns().includes(await runKey(viewId, html))
  } catch {
    return false
  }
}

export async function rememberHtmlViewRun(viewId: string, html: string) {
  if (!viewId) return
  try {
    const key = await runKey(viewId, html)
    localStorage.setItem(runStorageKey, JSON.stringify([...readRuns().filter((entry) => entry !== key), key].slice(-maxRememberedRuns)))
  } catch {
    // The view still runs now; it asks again next time.
  }
}
