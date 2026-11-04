import { create } from 'zustand'
import { createJSONStorage, persist } from 'zustand/middleware'

export const SANS_FONTS = {
  system: { label: 'System', family: '-apple-system, BlinkMacSystemFont, "Segoe UI", system-ui, Roboto, "Helvetica Neue", Arial, sans-serif' },
  arial: { label: 'Arial', family: 'Arial, sans-serif' },
  segoe: { label: 'Segoe UI', family: '"Segoe UI", system-ui, sans-serif' },
}
export const MONO_FONTS = {
  jetbrains: { label: 'JetBrains Mono', family: '"JetBrains Mono", ui-monospace, "SF Mono", Menlo, monospace' },
  system: { label: 'System monospace', family: 'ui-monospace, "SF Mono", Menlo, Consolas, monospace' },
  consolas: { label: 'Consolas', family: 'Consolas, "SF Mono", Menlo, monospace' },
}

export interface TypographyPreferences {
  interfaceFont: keyof typeof SANS_FONTS
  interfaceSize: number
  promptFont: keyof typeof SANS_FONTS
  promptSize: number
  codeFont: keyof typeof MONO_FONTS
  codeSize: number
  terminalFont: keyof typeof MONO_FONTS
  terminalSize: number
}

export const DEFAULT_TYPOGRAPHY: TypographyPreferences = {
  interfaceFont: 'system', interfaceSize: 13,
  promptFont: 'system', promptSize: 13,
  codeFont: 'jetbrains', codeSize: 12,
  terminalFont: 'jetbrains', terminalSize: 13,
}

function size(value: unknown, fallback: number, min: number, max: number): number {
  return typeof value === 'number' && Number.isFinite(value) ? Math.min(max, Math.max(min, Math.round(value))) : fallback
}

/** Bound stored preferences before applying them to CSS or xterm. */
export function normalizeTypography(value: unknown): TypographyPreferences {
  const input = value && typeof value === 'object' ? value as Partial<TypographyPreferences> : {}
  const sans = (font: unknown): keyof typeof SANS_FONTS => typeof font === 'string' && Object.prototype.hasOwnProperty.call(SANS_FONTS, font) ? font as keyof typeof SANS_FONTS : 'system'
  const mono = (font: unknown): keyof typeof MONO_FONTS => typeof font === 'string' && Object.prototype.hasOwnProperty.call(MONO_FONTS, font) ? font as keyof typeof MONO_FONTS : 'jetbrains'
  return {
    interfaceFont: sans(input.interfaceFont), interfaceSize: size(input.interfaceSize, 13, 12, 20),
    promptFont: sans(input.promptFont), promptSize: size(input.promptSize, 13, 12, 20),
    codeFont: mono(input.codeFont), codeSize: size(input.codeSize, 12, 10, 18),
    terminalFont: mono(input.terminalFont), terminalSize: size(input.terminalSize, 13, 8, 20),
  }
}

interface TypographyState {
  preferences: TypographyPreferences
  update: (patch: Partial<TypographyPreferences>) => void
  reset: () => void
}

export const useTypographyStore = create<TypographyState>()(persist((set) => ({
  preferences: DEFAULT_TYPOGRAPHY,
  update: (patch) => set((state) => ({ preferences: normalizeTypography({ ...state.preferences, ...patch }) })),
  reset: () => set({ preferences: { ...DEFAULT_TYPOGRAPHY } }),
}), {
  name: 'uam-typography-v1',
  storage: createJSONStorage(() => ({
    getItem: (key) => {
      try { return window.localStorage?.getItem(key) ?? null }
      catch (error) { console.error('[Typography] Could not read preferences.', error); return null }
    },
    setItem: (key, value) => {
      try { window.localStorage?.setItem(key, value) }
      catch (error) { console.error('[Typography] Could not save preferences.', error) }
    },
    removeItem: (key) => {
      try { window.localStorage?.removeItem(key) }
      catch (error) { console.error('[Typography] Could not remove preferences.', error) }
    },
  })),
  partialize: (state) => ({ preferences: state.preferences }),
  merge: (stored, current) => ({ ...current, preferences: normalizeTypography((stored as Partial<TypographyState> | undefined)?.preferences) }),
}))

export function applyTypography(preferences: TypographyPreferences, root = document.documentElement): void {
  const style = root.style
  style.setProperty('--font-sans', SANS_FONTS[preferences.interfaceFont].family)
  style.setProperty('--font-mono', MONO_FONTS[preferences.codeFont].family)
  style.setProperty('--font-prompt', SANS_FONTS[preferences.promptFont].family)
  style.setProperty('--fs-prompt', `${preferences.promptSize}px`)
  style.setProperty('--fs-code', `${preferences.codeSize}px`)
  const scale = preferences.interfaceSize / DEFAULT_TYPOGRAPHY.interfaceSize
  for (const [name, baseline] of Object.entries({ xs: 11, sm: 12, base: 13, md: 14, lg: 16, xl: 20, '2xl': 24 })) {
    style.setProperty(`--fs-${name}`, `${baseline * scale}px`)
  }
}
