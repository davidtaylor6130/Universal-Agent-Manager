import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import './index.css'
import { isCefContext } from './ipc/cefBridge'
import { applyDocumentTheme, readStoredTheme } from './utils/themeStorage'

// Apply persisted theme before first render to prevent flash
applyDocumentTheme(readStoredTheme() ?? 'dark')

const root = createRoot(document.getElementById('root')!)

function BrowserLaunchGuard() {
  const openedFromFile = window.location.protocol === 'file:'

  return (
    <StrictMode>
      <div className="browser-guard">
        <div className="browser-guard__panel">
          <div className="browser-guard__eyebrow">Native shell required</div>
          <h1 className="browser-guard__title">This UI is not meant to run inside Chrome.</h1>
          <p className="browser-guard__body">
            UAM removes browser tabs and the URL bar by hosting the React UI inside the
            native CEF desktop window. Opening the bundled HTML directly will always show
            browser chrome.
          </p>
          <div className="browser-guard__callout">
            {openedFromFile
              ? 'You opened the packaged UI file directly.'
              : 'You launched the React frontend in browser mode.'}
          </div>
          <div className="browser-guard__steps">
            <div>
              <span className="browser-guard__step-label">macOS bundle</span>
              <code>open Builds/universal_agent_manager.app</code>
            </div>
            <div>
              <span className="browser-guard__step-label">Launcher script</span>
              <code>./run_uam.sh</code>
            </div>
            <div>
              <span className="browser-guard__step-label">Windows binary</span>
              <code>Builds\\Release\\universal_agent_manager.exe</code>
            </div>
          </div>
        </div>
      </div>
    </StrictMode>
  )
}

if (isCefContext()) {
  void import('./App').then(({ default: App }) => {
    root.render(
      <StrictMode>
        <App />
      </StrictMode>
    )
  })
} else {
  root.render(<BrowserLaunchGuard />)
}
