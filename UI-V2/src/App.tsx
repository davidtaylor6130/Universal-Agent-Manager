import { useEffect } from 'react'
import { AppShell } from './components/layout/AppShell'
import { useAppStore } from './store/useAppStore'

export default function App() {
  const { theme } = useAppStore()

  // Sync data-theme attribute when theme changes.
  useEffect(() => {
    document.documentElement.setAttribute('data-theme', theme)
  }, [theme])

  return <AppShell />
}
