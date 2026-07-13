import { useEffect } from 'react'
import { useAppStore } from '../store/useAppStore'
import { applyDocumentTheme, readStoredTheme } from '../utils/themeStorage'

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

  const toggle = () => setTheme(theme === 'dark' ? 'light' : 'dark')

  return { theme, toggle, setTheme }
}
