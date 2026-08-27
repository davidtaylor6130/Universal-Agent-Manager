export type BuiltInTheme = 'focus' | 'dark' | 'light' | 'system' | 'midnight' | 'paper' | 'dusk' | 'aurora' | 'contrast'
export type StoredTheme = BuiltInTheme | `custom:${string}`
export type ResolvedTheme = 'dark' | 'light'

export interface ThemeColors {
  background: string
  surface: string
  surfaceUp: string
  text: string
  textMuted: string
  accent: string
  sidebar: string
  userMessage: string
  assistantMessage: string
  success: string
  warning: string
  error: string
}

export interface CustomTheme {
  version: 1
  id: `custom:${string}`
  name: string
  base: ResolvedTheme
  colors: ThemeColors
}

export const BUILT_IN_THEMES: ReadonlyArray<{ id: BuiltInTheme; label: string; base: ResolvedTheme }> = [
  { id: 'focus', label: 'Focus', base: 'dark' },
  { id: 'dark', label: 'OG', base: 'dark' },
  { id: 'light', label: 'Light', base: 'light' },
  { id: 'system', label: 'System', base: 'light' },
  { id: 'midnight', label: 'Midnight', base: 'dark' },
  { id: 'paper', label: 'Paper', base: 'light' },
  { id: 'dusk', label: 'Dusk', base: 'dark' },
  { id: 'aurora', label: 'Aurora', base: 'dark' },
  { id: 'contrast', label: 'High Contrast', base: 'dark' },
]

export const BUILT_IN_THEME_COLORS: Record<ResolvedTheme, ThemeColors> = {
  dark: {
    background: '#0f1117', surface: '#12141b', surfaceUp: '#191c25', text: '#eef1f7', textMuted: '#9aa4b6',
    accent: '#ff6a00', sidebar: '#11131a', userMessage: '#211811', assistantMessage: '#12141b',
    success: '#4ade80', warning: '#facc15', error: '#f87171',
  },
  light: {
    background: '#f7f8fa', surface: '#ffffff', surfaceUp: '#f2f4f7', text: '#141820', textMuted: '#596273',
    accent: '#ff6a00', sidebar: '#fbfcfe', userMessage: '#fff3eb', assistantMessage: '#ffffff',
    success: '#16a34a', warning: '#eab308', error: '#ef4444',
  },
}

const CUSTOM_THEME_ID = /^custom:[a-z0-9-]{1,48}$/
const HEX_COLOR = /^#[0-9a-fA-F]{6}$/
const CUSTOM_CSS_PROPERTIES: Record<keyof ThemeColors, string> = {
  background: '--bg',
  surface: '--surface',
  surfaceUp: '--surface-up',
  text: '--text',
  textMuted: '--text-2',
  accent: '--accent',
  sidebar: '--sidebar-bg',
  userMessage: '--message-user-bg',
  assistantMessage: '--message-assistant-bg',
  success: '--green',
  warning: '--yellow',
  error: '--red',
}

const DERIVED_CSS_PROPERTIES = [
  '--window-bg', '--surface-high', '--border', '--border-bright', '--text-2', '--text-3', '--accent-dim',
  '--accent-glow', '--sidebar-item-hover', '--sidebar-item-active', '--success-dim', '--warning-dim',
  '--error-dim', '--term-bg', '--term-fg', '--term-cursor', '--term-selection',
]

export function normalizeStoredTheme(value: unknown): StoredTheme | null {
  if (value === 'mono') return 'focus'
  if (BUILT_IN_THEMES.some((theme) => theme.id === value)) return value as BuiltInTheme
  return typeof value === 'string' && CUSTOM_THEME_ID.test(value) ? value as StoredTheme : null
}

export function normalizeCustomTheme(value: unknown): CustomTheme | null {
  if (!value || typeof value !== 'object') return null
  const raw = value as Record<string, unknown>
  if (raw.version !== 1) return null
  const id = normalizeStoredTheme(raw.id)
  if (!id?.startsWith('custom:') || typeof raw.name !== 'string' || !raw.name.trim() || raw.name.trim().length > 64) return null
  if (raw.base !== 'dark' && raw.base !== 'light') return null
  if (!raw.colors || typeof raw.colors !== 'object') return null
  const rawColors = raw.colors as Record<string, unknown>
  const colors = {} as ThemeColors
  for (const key of Object.keys(CUSTOM_CSS_PROPERTIES) as Array<keyof ThemeColors>) {
    const color = rawColors[key]
    if (typeof color !== 'string' || !HEX_COLOR.test(color)) return null
    colors[key] = color.toLowerCase()
  }
  return { version: 1, id: id as CustomTheme['id'], name: raw.name.trim(), base: raw.base, colors }
}

export function readStoredTheme(): StoredTheme | null {
  try {
    if (typeof localStorage === 'undefined' || typeof localStorage.getItem !== 'function') {
      return null
    }

    return normalizeStoredTheme(localStorage.getItem('uam-theme'))
  } catch {
    return null
  }
}

export function writeStoredTheme(theme: StoredTheme): void {
  try {
    if (typeof localStorage !== 'undefined' && typeof localStorage.setItem === 'function') {
      localStorage.setItem('uam-theme', theme)
    }
  } catch {
    // Storage may be unavailable in restricted launch contexts.
  }
}

export function resolveDocumentTheme(theme: StoredTheme, customThemes: CustomTheme[] = []): ResolvedTheme {
  if (theme.startsWith('custom:')) return customThemes.find((candidate) => candidate.id === theme)?.base ?? 'dark'
  if (theme === 'dark' || theme === 'light') return theme
  if (theme !== 'system') return BUILT_IN_THEMES.find((candidate) => candidate.id === theme)?.base ?? 'dark'
  if (typeof window === 'undefined' || typeof window.matchMedia !== 'function') {
    return 'light'
  }
  return window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light'
}

export function applyDocumentTheme(theme: StoredTheme, customThemes: CustomTheme[] = []): void {
  if (typeof document !== 'undefined' && document.documentElement) {
    const root = document.documentElement
    for (const property of Object.values(CUSTOM_CSS_PROPERTIES)) root.style.removeProperty(property)
    for (const property of DERIVED_CSS_PROPERTIES) root.style.removeProperty(property)
    const customTheme = customThemes.find((candidate) => candidate.id === theme)
    root.setAttribute('data-theme', customTheme?.base ?? (theme === 'system' ? resolveDocumentTheme(theme) : theme))
    if (customTheme) {
      for (const [key, property] of Object.entries(CUSTOM_CSS_PROPERTIES) as Array<[keyof ThemeColors, string]>) {
        root.style.setProperty(property, customTheme.colors[key])
      }
      const { background, surfaceUp, text, textMuted, accent, success, warning, error } = customTheme.colors
      const derived: Record<string, string> = {
        '--window-bg': background,
        '--surface-high': `color-mix(in srgb, ${surfaceUp} 92%, ${text})`,
        '--border': `color-mix(in srgb, ${text} 9%, transparent)`,
        '--border-bright': `color-mix(in srgb, ${text} 15%, transparent)`,
        '--text-2': `color-mix(in srgb, ${textMuted} 72%, ${text})`,
        '--text-3': `color-mix(in srgb, ${textMuted} 82%, ${text})`,
        '--accent-dim': `color-mix(in srgb, ${accent} 13%, transparent)`,
        '--accent-glow': `color-mix(in srgb, ${accent} 22%, transparent)`,
        '--sidebar-item-hover': `color-mix(in srgb, ${text} 5%, transparent)`,
        '--sidebar-item-active': `color-mix(in srgb, ${accent} 14%, transparent)`,
        '--success-dim': `color-mix(in srgb, ${success} 14%, transparent)`,
        '--warning-dim': `color-mix(in srgb, ${warning} 16%, transparent)`,
        '--error-dim': `color-mix(in srgb, ${error} 15%, transparent)`,
        '--term-bg': background,
        '--term-fg': text,
        '--term-cursor': accent,
        '--term-selection': `color-mix(in srgb, ${accent} 28%, transparent)`,
      }
      for (const [property, value] of Object.entries(derived)) root.style.setProperty(property, value)
    }
  }
}
