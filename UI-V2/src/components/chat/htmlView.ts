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

