import { applyTypography, useTypographyStore } from './store/typographyStore'
import { StrictMode } from 'react'
import { InterfaceErrorBoundary } from './components/shared/InterfaceErrorBoundary'
import { createRoot } from 'react-dom/client'
import './index.css'
import { isCefContext } from './ipc/cefBridge'
import { applyDocumentTheme, readStoredTheme } from './utils/themeStorage'
import { applyAppearance, readAppearance } from './utils/appearancePreferences'
import { applyMotionSpeed, readStoredMotion } from './utils/motionStorage'

// Apply persisted theme and motion speed before first render to prevent flash
applyDocumentTheme(readStoredTheme() ?? 'focus')
applyAppearance(readAppearance())
applyMotionSpeed(readStoredMotion())
applyTypography(useTypographyStore.getState().preferences)

const root = createRoot(document.getElementById('root')!)

function BrowserLaunchGuard() {
  const openedFromFile = window.location.protocol === 'file:'

  return (
    <StrictMode>
      <div className="browser-guard">
        <div className="browser-guard__panel">
          <h1 className="browser-guard__title">This UI is not meant to run inside Chrome.</h1>
          <p className="browser-guard__body">
            Open UAM from the desktop app. Browser mode shows browser tabs and the URL bar.
          </p>
          <div className="browser-guard__callout">
            {openedFromFile
              ? 'You opened the packaged UI file directly.'
              : 'You launched the React frontend in browser mode.'}
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
        <InterfaceErrorBoundary><App /></InterfaceErrorBoundary>
      </StrictMode>
    )
  }).catch((error: unknown) => {
    console.error('[UI startup]', error)
    document.getElementById('uam-startup-recovery')?.removeAttribute('hidden')
  })
} else {
  root.render(<BrowserLaunchGuard />)
}
