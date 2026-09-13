import { useEffect } from 'react'
import { CompanionShell } from './components/companion/CompanionShell'
import { isCompanionContext } from './ipc/cefBridge'
import { AppShell } from './components/layout/AppShell'
import { TooltipProvider } from './components/ui'
import { useAppStore } from './store/useAppStore'
import { installCopySelectionFallback } from './utils/copySelection'
import { trapModalTab } from './utils/modalFocus'
import { useResolvedTheme } from './hooks/useTheme'
import { applyDocumentTheme } from './utils/themeStorage'

export default function App() {
  const theme = useAppStore((s) => s.theme)
  const customThemes = useAppStore((s) => s.customThemes)

  const resolvedTheme = useResolvedTheme(theme, customThemes)

  // Sync data-theme when the selected palette or OS preference changes.
  useEffect(() => {
    applyDocumentTheme(theme, customThemes)
  }, [customThemes, theme, resolvedTheme])

  useEffect(() => installCopySelectionFallback(), [])

  useEffect(() => {
    document.addEventListener('keydown', trapModalTab, true)
    return () => document.removeEventListener('keydown', trapModalTab, true)
  }, [])

  return (
    <TooltipProvider>
      {isCompanionContext() ? <CompanionShell /> : <AppShell />}
    </TooltipProvider>
  )
}
