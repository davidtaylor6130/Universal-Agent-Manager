import { useEffect } from 'react'
import { useAppStore } from '../store/useAppStore'
import { applyDocumentTheme, readStoredTheme, resolveDocumentTheme } from '../utils/themeStorage'

export function useTheme() {
  const theme = useAppStore((s) => s.theme)
  const setTheme = useAppStore((s) => s.setTheme)
  const customThemes = useAppStore((s) => s.customThemes)

  // Sync from localStorage on mount
  useEffect(() => {
    const stored = readStoredTheme()
    if (stored && stored !== theme) {
      setTheme(stored)
    }
    // Apply to HTML element
    applyDocumentTheme(stored ?? theme, customThemes)
  }, [customThemes, setTheme, theme])

  const resolvedTheme = resolveDocumentTheme(theme, customThemes)
  const toggle = () => setTheme(resolvedTheme === 'dark' ? 'light' : 'focus')

  return { theme, resolvedTheme, toggle, setTheme }
}
