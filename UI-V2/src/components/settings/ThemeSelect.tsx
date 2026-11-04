import { useAppStore } from '../../store/useAppStore'
import { BUILT_IN_THEMES, type StoredTheme } from '../../utils/themeStorage'
import { MenuSelect } from '../ui'

/** Theme choices shared by desktop and phone settings. */
export function ThemeSelect({ value, onChange }: { value: StoredTheme; onChange: (value: StoredTheme) => void }) {
  const customThemes = useAppStore((state) => state.customThemes)
  const options = [...BUILT_IN_THEMES.map(({ id, label }) => ({ value: id, label })), ...(customThemes ?? []).map((theme) => ({ value: theme.id, label: theme.name }))]
  return <MenuSelect label="Theme" value={value} options={options} onChange={(theme) => onChange(theme as StoredTheme)} />
}
