// Curated, lazily-rendered icon library helpers. Kept free of lucide imports so the main bundle stays small.
export const SUGGESTED_LUCIDE_ICONS = [
  'folder', 'folder-open', 'file-text', 'code', 'terminal', 'bot', 'sparkles', 'book-open',
  'bookmark', 'briefcase', 'globe', 'rocket', 'flask-conical', 'lightbulb', 'star', 'heart',
  'zap', 'shield', 'wrench', 'database', 'cloud', 'package', 'layers', 'target',
  'compass', 'pen-line', 'message-square', 'notebook', 'palette', 'music', 'camera', 'gamepad-2',
  'coffee', 'home', 'server', 'cpu', 'git-branch', 'flag', 'map', 'shopping-cart',
] as const

const RECENT_KEY = 'uam.recentLucideIcons'
const RECENT_LIMIT = 8

export function readRecentLucideIcons(): string[] {
  try {
    const parsed: unknown = JSON.parse(window.localStorage.getItem(RECENT_KEY) ?? '[]')
    return Array.isArray(parsed) ? parsed.filter((name): name is string => typeof name === 'string').slice(0, RECENT_LIMIT) : []
  } catch {
    return []
  }
}

export function rememberRecentLucideIcon(name: string): void {
  try {
    const next = [name, ...readRecentLucideIcons().filter((entry) => entry !== name)].slice(0, RECENT_LIMIT)
    window.localStorage.setItem(RECENT_KEY, JSON.stringify(next))
  } catch {
    // Storage may be unavailable (private mode); recents are a convenience only.
  }
}
