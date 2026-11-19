import { useEffect, useRef, useState, useSyncExternalStore, type MutableRefObject } from 'react'
import { AppWindow, Download, ExternalLink, Maximize2, Minimize2, Play } from 'lucide-react'
import { Button, Card, IconButton, Notice, motionAllowed } from '../ui'
import { CopyTextButton } from './StatusHelpers'
import { useAppStore } from '../../store/useAppStore'
import { useResolvedTheme } from '../../hooks/useTheme'
import { isCefContext, isCompanionContext, sendToCEF } from '../../ipc/cefBridge'
import { HTML_VIEW_SANDBOX, htmlViewFrameUrl, htmlViewRunAllowed, prepareHtmlViewDocument, rememberHtmlViewRun } from './htmlView'
import './HtmlViewCard.css'

/** Motion Off (root data-motion) or the system reduced-motion request, kept live so the frame can follow changes. */
function subscribeMotion(onChange: () => void) {
  const observer = new MutationObserver(onChange)
  observer.observe(document.documentElement, { attributes: true, attributeFilter: ['data-motion'] })
  const query = typeof window.matchMedia === 'function' ? window.matchMedia('(prefers-reduced-motion: reduce)') : null
  query?.addEventListener?.('change', onChange)
  return () => { observer.disconnect(); query?.removeEventListener?.('change', onChange) }
}

/** One frame load per document: scheme and motion are baked into the page, so a change remounts this (keyed by the card). */
function HtmlViewFrame({ title, html, scheme, stillMotion, focus }: { title: string; html: string; scheme: 'light' | 'dark'; stillMotion: boolean; focus: MutableRefObject<boolean> }) {
  const sent = useRef(false)
  const frame = useRef<HTMLIFrameElement>(null)
  // Run unmounts its button, so keyboard focus moves to the page once instead of falling to the document.
  useEffect(() => {
    if (!focus.current) return
    focus.current = false
    frame.current?.focus()
  }, [focus])
  return (
    <iframe
      ref={frame}
      className="uam-html-view__frame"
      title={title}
      src={htmlViewFrameUrl()}
      sandbox={HTML_VIEW_SANDBOX}
      referrerPolicy="no-referrer"
      style={{ colorScheme: scheme }}
      onLoad={(event) => {
        // The host replaces itself with the page; later load events belong to the page, not the host. The page cannot
        // bring the host back: Chromium refuses a sandboxed frame's navigation to this display-isolated scheme.
        if (sent.current) return
        sent.current = true
        event.currentTarget.contentWindow?.postMessage({ type: 'uam-html-view', html: prepareHtmlViewDocument(html, { scheme, stillMotion }) }, '*')
      }}
    />
  )
}

/**
 * Agent-published HTML page in a sandboxed frame. The frame has an opaque origin (no allow-same-origin)
 * and loads UAM's frame host, which runs under its own no-network policy; the page cannot reach the
 * bridge, app storage or the parent document. Only the page text is ever sent into the frame.
 * Nothing loads until the user chooses Run; that choice is remembered for this view and page only.
 */
export function HtmlViewCard({ viewId, title, html }: { viewId: string; title: string; html: string }) {
  const theme = useAppStore((s) => s.theme)
  const customThemes = useAppStore((s) => s.customThemes)
  const scheme = useResolvedTheme(theme, customThemes)
  const [expanded, setExpanded] = useState(false)
  const [failure, setFailure] = useState('')
  // Holds the page text the user approved, so a changed page under the same card asks again.
  const [approvedHtml, setApprovedHtml] = useState<string | null>(null)
  const focusFrame = useRef(false)
  const running = approvedHtml === html
  useEffect(() => {
    let current = true
    void htmlViewRunAllowed(viewId, html).then((allowed) => { if (current && allowed) setApprovedHtml(html) })
    return () => { current = false }
  }, [viewId, html])
  const stillMotion = !useSyncExternalStore(subscribeMotion, motionAllowed, () => false)
  const native = isCefContext()

  if (isCompanionContext()) {
    return <Card elevation={0} flush className="uam-html-view" role="group" aria-label={`HTML view: ${title}`}>
      <div className="uam-html-view__header"><AppWindow size={14} className="uam-html-view__icon" aria-hidden /><span className="uam-html-view__title">{title}</span></div>
      <p className="uam-html-view__placeholder">HTML view. Open this chat on the desktop to see it.</p>
    </Card>
  }

  const run = async (action: 'openHtmlView' | 'saveHtmlView', fallback: string) => {
    setFailure('')
    try {
      const response = await sendToCEF({ action, payload: { title, html } })
      if (!response.ok) setFailure(response.error || fallback)
    } catch {
      setFailure(fallback)
    }
  }

  return (
    <Card elevation={0} flush className={expanded ? 'uam-html-view uam-html-view--expanded' : 'uam-html-view'} role="group" aria-label={`HTML view: ${title}`}>
      <div className="uam-html-view__header">
        <AppWindow size={14} className="uam-html-view__icon" aria-hidden />
        <span className="uam-html-view__title" title={title}>{title}</span>
        <span className="uam-html-view__kind">HTML view</span>
        <span className="uam-html-view__actions">
          {running && <IconButton size="sm" icon={expanded ? <Minimize2 size={14} /> : <Maximize2 size={14} />} label={expanded ? 'Collapse HTML view' : 'Expand HTML view'}
            aria-expanded={expanded} onClick={() => setExpanded((value) => !value)} />}
          {native && <IconButton size="sm" icon={<ExternalLink size={14} />} label="Open in browser" tooltip="Open in the system browser"
            onClick={() => void run('openHtmlView', 'The HTML view could not be opened in the browser.')} />}
          <CopyTextButton text={html} label="Copy HTML source" title="Copy HTML source" />
          {native && <IconButton size="sm" icon={<Download size={14} />} label="Save as HTML file" tooltip="Save as .html file"
            onClick={() => void run('saveHtmlView', 'The HTML view could not be saved.')} />}
        </span>
      </div>
      {failure && <div className="uam-html-view__notice"><Notice tone="error" title="HTML view action failed" dismissLabel="Dismiss HTML view error" onDismiss={() => setFailure('')}>{failure}</Notice></div>}
      {/* The live and saved transcripts are separate trees, so the page reloads once when its turn ends. */}
      {running
        ? <HtmlViewFrame key={`${scheme}:${stillMotion}`} title={title} html={html} scheme={scheme} stillMotion={stillMotion} focus={focusFrame} />
        : <div className="uam-html-view__gate">
            <p className="uam-html-view__gate-text">This page was written by the agent and can run scripts. It stays off until you run it.</p>
            <Button size="sm" variant="secondary" leadingIcon={<Play size={14} />} aria-label={`Run HTML view: ${title}`}
              onClick={() => { void rememberHtmlViewRun(viewId, html); focusFrame.current = true; setApprovedHtml(html) }}>Run</Button>
          </div>}
    </Card>
  )
}
