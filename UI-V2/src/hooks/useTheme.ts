import { useEffect } from 'react'
import { useAppStore } from '../store/useAppStore'
import { applyDocumentTheme, readStoredTheme } from '../utils/themeStorage'

export function useTheme() {
  const theme = useAppStore((s) => s.theme)
  const setTheme = useAppStore((s) => s.setTheme)

  // Sync from localStorage on mount
  useEffect(() => {
    const stored = readStoredTheme()
    if (stored && stored !== theme) {
      setTheme(stored)
    }
    // Apply to HTML element
    applyDocumentTheme(stored ?? theme)
  }, []) // eslint-disable-line react-hooks/exhaustive-deps

  const toggle = () => setTheme(theme === 'dark' ? 'light' : 'dark')

  return { theme, toggle, setTheme }
}
