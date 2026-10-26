export type ChatWidth = 'comfortable' | 'wide' | 'full'
export type DiffPalette = 'default' | 'colour-blind'

export interface AppearancePreferences {
  chatWidth: ChatWidth
  highContrast: boolean
  diffPalette: DiffPalette
  /** Show reported context/output token limits in the model picker. */
  modelTokenLimits: boolean
}

export const DEFAULT_APPEARANCE: AppearancePreferences = { chatWidth: 'comfortable', highContrast: false, diffPalette: 'default', modelTokenLimits: true }

const STORAGE_KEY = 'uam-appearance'

/** Bound stored values to known options; anything else falls back to the default. */
export function normalizeAppearance(value: unknown): AppearancePreferences {
  const input = value && typeof value === 'object' ? value as Partial<Record<keyof AppearancePreferences, unknown>> : {}
  return {
    chatWidth: input.chatWidth === 'wide' || input.chatWidth === 'full' ? input.chatWidth : 'comfortable',
    highContrast: input.highContrast === true,
    diffPalette: input.diffPalette === 'colour-blind' ? 'colour-blind' : 'default',
    modelTokenLimits: input.modelTokenLimits !== false,
  }
}

export function readAppearance(): AppearancePreferences {
  try {
    if (typeof localStorage === 'undefined' || typeof localStorage.getItem !== 'function') return DEFAULT_APPEARANCE
    const raw = localStorage.getItem(STORAGE_KEY)
    return normalizeAppearance(raw ? JSON.parse(raw) : null)
  } catch {
    return DEFAULT_APPEARANCE
  }
}

export function writeAppearance(preferences: AppearancePreferences): void {
  try {
    if (typeof localStorage !== 'undefined' && typeof localStorage.setItem === 'function') {
      localStorage.setItem(STORAGE_KEY, JSON.stringify(preferences))
    }
  } catch {
    // Storage may be unavailable in restricted launch contexts.
  }
}

/** Sets root data attributes read by CSS; defaults remove the attribute so the base styles apply. */
export function applyAppearance(preferences: AppearancePreferences, root: HTMLElement = document.documentElement): void {
  const set = (name: string, value: string | null) => {
    if (value === null) root.removeAttribute(name)
    else if (root.getAttribute(name) !== value) root.setAttribute(name, value)
  }
  set('data-chat-width', preferences.chatWidth === 'comfortable' ? null : preferences.chatWidth)
  set('data-contrast', preferences.highContrast ? 'more' : null)
  set('data-diff-palette', preferences.diffPalette === 'default' ? null : preferences.diffPalette)
}
