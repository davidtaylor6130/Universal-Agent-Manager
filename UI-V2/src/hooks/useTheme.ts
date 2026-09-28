import { useEffect, useSyncExternalStore } from 'react'
import { useAppStore } from '../store/useAppStore'
import { applyDocumentTheme, readStoredTheme, resolveDocumentTheme, type StoredTheme, type CustomTheme } from '../utils/themeStorage'

function subscribeToSystemTheme(onChange: () => void) {
  if (typeof window.matchMedia !== 'function') return () => {}
  const media = window.matchMedia('(prefers-color-scheme: dark)')
  media.addEventListener('change', onChange)
  return () => media.removeEventListener('change', onChange)
}

export function useResolvedTheme(theme: StoredTheme, customThemes: CustomTheme[]) {
  return useSyncExternalStore(subscribeToSystemTheme, () => resolveDocumentTheme(theme, customThemes))
}

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

  const resolvedTheme = useResolvedTheme(theme, customThemes)
  const toggle = () => setTheme(resolvedTheme === 'dark' ? 'light' : 'focus')

  return { theme, resolvedTheme, toggle, setTheme }
}
