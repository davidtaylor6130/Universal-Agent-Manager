import { create } from 'zustand'
import { createJSONStorage, persist } from 'zustand/middleware'

export interface FavoriteModel { providerId: string; modelId: string }

/** Ignore malformed or excessive browser preferences before using model keys. */
export function normalizeFavoriteModels(value: unknown): FavoriteModel[] {
  if (!Array.isArray(value)) return []
  const result: FavoriteModel[] = []
  const seen = new Set<string>()
  for (const entry of value.slice(0, 200)) {
    if (!entry || typeof entry !== 'object' || typeof entry.providerId !== 'string' || typeof entry.modelId !== 'string') continue
    if (!entry.providerId || entry.providerId.length > 160 || entry.modelId.length > 240) continue
    const key = JSON.stringify([entry.providerId, entry.modelId])
    if (seen.has(key)) continue
    seen.add(key)
    result.push({ providerId: entry.providerId, modelId: entry.modelId })
  }
  return result
}

export const useModelPickerStore = create<{
  favorites: FavoriteModel[]
  toggleFavorite: (providerId: string, modelId: string) => void
}>()(persist((set) => ({
  favorites: [],
  toggleFavorite: (providerId, modelId) => set((state) => ({
    favorites: state.favorites.some((entry) => entry.providerId === providerId && entry.modelId === modelId)
      ? state.favorites.filter((entry) => entry.providerId !== providerId || entry.modelId !== modelId)
      : normalizeFavoriteModels([...state.favorites, { providerId, modelId }]),
  })),
}), {
  name: 'uam-model-favorites-v1',
  storage: createJSONStorage(() => ({
    getItem: (key) => { try { return window.localStorage.getItem(key) } catch { return null } },
    setItem: (key, value) => { try { window.localStorage.setItem(key, value) } catch (error) { console.error('[Models] Could not save favorites.', error) } },
    removeItem: (key) => { try { window.localStorage.removeItem(key) } catch (error) { console.error('[Models] Could not remove favorites.', error) } },
  })),
  partialize: (state) => ({ favorites: state.favorites }),
  merge: (stored, current) => ({ ...current, favorites: normalizeFavoriteModels((stored as { favorites?: unknown } | undefined)?.favorites) }),
}))
