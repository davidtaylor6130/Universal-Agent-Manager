export type StoredTheme = 'dark' | 'light' | 'system'
export type ResolvedTheme = 'dark' | 'light'

export function normalizeStoredTheme(value: unknown): StoredTheme | null {
  return value === 'dark' || value === 'light' || value === 'system' ? value : null
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

export function resolveDocumentTheme(theme: StoredTheme): ResolvedTheme {
  if (theme !== 'system') return theme
  if (typeof window === 'undefined' || typeof window.matchMedia !== 'function') {
    return 'light'
  }
  return window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light'
}

export function applyDocumentTheme(theme: StoredTheme): void {
  if (typeof document !== 'undefined' && document.documentElement) {
    document.documentElement.setAttribute('data-theme', resolveDocumentTheme(theme))
  }
}
