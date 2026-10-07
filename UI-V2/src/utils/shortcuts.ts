/** App-wide keyboard shortcuts: ⌘ on macOS, Ctrl elsewhere. */
const IS_MAC = typeof navigator !== 'undefined' && /Mac|iPhone|iPad/i.test(navigator.platform || navigator.userAgent)

export type AppShortcut = 'newChat' | 'search' | 'settings' | 'sidebar' | 'nextAttention' | 'undoOrganization'

const SHORTCUT_KEYS: Record<AppShortcut, string> = {
  newChat: 'n',
  search: 'k',
  settings: ',',
  sidebar: 'b',
  nextAttention: 'j',
  undoOrganization: 'z',
}

/** Tooltip hint, e.g. "⌘N" or "Ctrl+N". */
export function shortcutLabel(shortcut: AppShortcut): string {
  const key = SHORTCUT_KEYS[shortcut].toUpperCase()
  const shift = shortcut === 'nextAttention' ? IS_MAC ? '⇧' : 'Shift+' : ''
  return IS_MAC ? `⌘${shift}${key}` : `Ctrl+${shift}${key}`
}

export function matchShortcut(event: Pick<KeyboardEvent, 'key' | 'metaKey' | 'ctrlKey' | 'altKey' | 'shiftKey'>): AppShortcut | null {
  const mod = IS_MAC ? event.metaKey && !event.ctrlKey : event.ctrlKey && !event.metaKey
  if (!mod || event.altKey) return null
  if (event.shiftKey) return event.key.toLowerCase() === SHORTCUT_KEYS.nextAttention ? 'nextAttention' : null
  const key = event.key.toLowerCase()
  return (Object.keys(SHORTCUT_KEYS) as AppShortcut[]).find((shortcut) => shortcut !== 'nextAttention' && SHORTCUT_KEYS[shortcut] === key) ?? null
}

/** Human label for the OS file manager used by shell actions. */
export const FILE_MANAGER_NAME = IS_MAC ? 'Finder' : typeof navigator !== 'undefined' && /Win/i.test(navigator.platform || navigator.userAgent) ? 'Explorer' : 'File manager'
