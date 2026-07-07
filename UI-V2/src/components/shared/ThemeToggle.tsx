import { Sun, Moon } from 'lucide-react'
import { useTheme } from '../../hooks/useTheme'
import { IconButton } from '../ui'

export function ThemeToggle() {
  const { theme, toggle } = useTheme()
  const isDark = theme === 'dark'

  return (
    <IconButton
      icon={isDark ? <Sun size={16} /> : <Moon size={16} />}
      label={isDark ? 'Switch to light mode' : 'Switch to dark mode'}
      tooltipSide="right"
      onClick={toggle}
    />
  )
}
