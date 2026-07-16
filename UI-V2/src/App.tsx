import { useEffect } from 'react'
import { AppShell } from './components/layout/AppShell'
import { TooltipProvider } from './components/ui'
import { useAppStore } from './store/useAppStore'
import { installCopySelectionFallback } from './utils/copySelection'
import { trapModalTab } from './utils/modalFocus'
import { applyDocumentTheme } from './utils/themeStorage'

export default function App() {
  const theme = useAppStore((s) => s.theme)

  // Sync data-theme attribute when theme changes.
  useEffect(() => {
    applyDocumentTheme(theme)
  }, [theme])

  useEffect(() => installCopySelectionFallback(), [])

  useEffect(() => {
    document.addEventListener('keydown', trapModalTab, true)
    return () => document.removeEventListener('keydown', trapModalTab, true)
  }, [])

  return (
    <TooltipProvider>
      <AppShell />
    </TooltipProvider>
  )
}
