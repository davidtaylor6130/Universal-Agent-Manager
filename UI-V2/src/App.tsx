import { useEffect } from 'react'
import { AppShell } from './components/layout/AppShell'
import { useAppStore } from './store/useAppStore'
import { installCopySelectionFallback } from './utils/copySelection'
import { applyDocumentTheme } from './utils/themeStorage'

export default function App() {
  const theme = useAppStore((s) => s.theme)

  // Sync data-theme attribute when theme changes.
  useEffect(() => {
    applyDocumentTheme(theme)
  }, [theme])

  useEffect(() => installCopySelectionFallback(), [])

  return <AppShell />
}
