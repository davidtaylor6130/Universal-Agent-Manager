import { useEffect, useLayoutEffect, useRef, useState, type ReactNode } from 'react'
import { Copy, Minus, Square, X } from 'lucide-react'
import { isCefContext, isCompanionContext, sendToCEF } from '../../ipc/cefBridge'
import { reportAppFailure } from '../../utils/appFailure'
import { IconButton } from '../ui'
import './WindowChrome.css'

type WindowState = {
  platform: 'macos' | 'windows'
  maximized: boolean
  fullscreen: boolean
}
type WindowCommand = 'minimize' | 'toggleMaximize' | 'close'

function initialWindowState(): WindowState | null {
  if (!isCefContext() || isCompanionContext()) return null
  const platform = /Mac/i.test(navigator.platform) ? 'macos' : /Win/i.test(navigator.platform) ? 'windows' : null
  return platform ? { platform, maximized: false, fullscreen: false } : null
}

function isWindowState(value: unknown): value is WindowState {
  if (!value || typeof value !== 'object') return false
  const state = value as Partial<WindowState>
  return (state.platform === 'macos' || state.platform === 'windows')
    && typeof state.maximized === 'boolean' && typeof state.fullscreen === 'boolean'
}

export function useWindowChrome() {
  const [state, setState] = useState(initialWindowState)
  const mounted = useRef(false)
  useEffect(() => {
    if (!state) return
    mounted.current = true
    let inFlight = false
    let refreshAgain = false
    let settledTimer: ReturnType<typeof setTimeout> | undefined
    const refresh = async () => {
      if (inFlight) { refreshAgain = true; return }
      inFlight = true
      do {
        refreshAgain = false
        const response = await sendToCEF({ action: 'getWindowChrome' }, false)
        if (!mounted.current) break
        if (response.ok && isWindowState(response.data)) {
          const next = response.data
          setState(current => current?.platform === next.platform && current.maximized === next.maximized
            && current.fullscreen === next.fullscreen ? current : next)
        }
      } while (refreshAgain)
      inFlight = false
    }
    const onResize = () => {
      void refresh()
      clearTimeout(settledTimer)
      settledTimer = setTimeout(refresh, 350)
    }
    void refresh()
    window.addEventListener('resize', onResize)
    window.addEventListener('focus', refresh)
    window.addEventListener('uam-window-changed', refresh)
    document.addEventListener('fullscreenchange', refresh)
    return () => {
      mounted.current = false
      clearTimeout(settledTimer)
      window.removeEventListener('resize', onResize)
      window.removeEventListener('focus', refresh)
      window.removeEventListener('uam-window-changed', refresh)
      document.removeEventListener('fullscreenchange', refresh)
    }
    // Platform is fixed for this window; updates do not replace its subscriptions.
  }, [state?.platform])

  const command = async (value: WindowCommand) => {
    const response = await sendToCEF({ action: 'windowCommand', payload: { command: value } }, false)
    if (!response.ok) reportAppFailure('Window action failed.', response.error ?? 'Try the window action again.')
    else if (mounted.current && isWindowState(response.data)) setState(response.data)
  }
  return { state, command }
}

/** Root render/import recovery still needs the controls of a frameless window. */
export function WindowRecovery({ children }: { children: ReactNode }) {
  const chrome = useWindowChrome()
  return (
    <div className={chrome.state ? 'uam-app uam-window-recovery' : undefined} data-window-platform={chrome.state?.platform} data-window-fullscreen={chrome.state?.fullscreen ? 'true' : undefined}>
      {chrome.state && <WindowChrome state={chrome.state} command={chrome.command} />}
      {children}
    </div>
  )
}

export function WindowChrome({ state, command }: { state: WindowState; command: (value: WindowCommand) => Promise<void> }) {
  const [pending, setPending] = useState(false)
  const stripRef = useRef<HTMLDivElement>(null)
  useLayoutEffect(() => {
    const shell = stripRef.current?.closest<HTMLElement>('.uam-app')
    const panel = shell?.querySelector<HTMLElement>('.uam-layout-toolbar')?.parentElement
    if (!shell || !panel) return
    // Follow the existing panel animations and resize handles without rerendering chats.
    const measure = () => {
      const bounds = panel.getBoundingClientRect()
      if (!bounds.width) return
      shell.style.setProperty('--uam-layout-left', `${bounds.left}px`)
      shell.style.setProperty('--uam-layout-right', `${window.innerWidth - bounds.right}px`)
    }
    measure()
    const observer = new ResizeObserver(measure)
    observer.observe(panel)
    shell.querySelectorAll('.uam-shell-panel').forEach(element => observer.observe(element))
    window.addEventListener('resize', measure)
    return () => { observer.disconnect(); window.removeEventListener('resize', measure) }
  }, [])
  const run = async (value: WindowCommand) => {
    if (pending) return
    setPending(true)
    try { await command(value) } finally { setPending(false) }
  }
  return (
    <div ref={stripRef} className="uam-window-chrome" data-testid="window-chrome">
      {state.platform === 'windows' && !state.fullscreen && (
        <div className="uam-window-captions" role="group" aria-label="Window controls">
          <IconButton className="uam-window-caption" icon={<Minus size={12} />} label="Minimize window" disabled={pending} tooltipSide="bottom" onClick={() => void run('minimize')} />
          <IconButton className="uam-window-caption" icon={state.maximized ? <Copy size={12} /> : <Square size={12} />} label={state.maximized ? 'Restore window' : 'Maximize window'} disabled={pending} tooltipSide="bottom" onClick={() => void run('toggleMaximize')} />
          <IconButton className="uam-window-caption uam-window-caption--close" icon={<X size={14} />} label="Close window" disabled={pending} tooltipSide="bottom" onClick={() => void run('close')} />
        </div>
      )}
    </div>
  )
}
