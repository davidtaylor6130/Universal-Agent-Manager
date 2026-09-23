import { lazy, Suspense, useEffect } from 'react'
import { isCompanionContext } from './ipc/cefBridge'
import { TooltipProvider } from './components/ui'
import { useAppStore } from './store/useAppStore'
import { installCopySelectionFallback } from './utils/copySelection'
import { trapModalTab } from './utils/modalFocus'
import { useResolvedTheme } from './hooks/useTheme'
import { applyDocumentTheme } from './utils/themeStorage'

const CompanionShell = lazy(() => import('./components/companion/CompanionShell').then(({ CompanionShell }) => ({ default: CompanionShell })))
const AppShell = lazy(() => import('./components/layout/AppShell').then(({ AppShell }) => ({ default: AppShell })))

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
      <Suspense fallback={<div role="status" className="p-4 text-sm">Loading Universal Agent Manager…</div>}>
        {isCompanionContext() ? <CompanionShell /> : <AppShell />}
      </Suspense>
    </TooltipProvider>
  )
}
