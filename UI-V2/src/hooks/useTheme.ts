import { useEffect } from 'react'
import { useAppStore } from '../store/useAppStore'

export function useTheme() {
  const { theme, setTheme } = useAppStore()

  // Sync from localStorage on mount
  useEffect(() => {
    const stored = localStorage.getItem('uam-theme') as 'dark' | 'light' | null
    if (stored && stored !== theme) {
      setTheme(stored)
    }
    // Apply to HTML element
    document.documentElement.setAttribute('data-theme', stored ?? theme)
  }, []) // eslint-disable-line react-hooks/exhaustive-deps

  const toggle = () => setTheme(theme === 'dark' ? 'light' : 'dark')

  return { theme, toggle, setTheme }
}
