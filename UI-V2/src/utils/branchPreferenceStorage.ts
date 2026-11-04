const storageKey = 'uam-preferred-branches-v1'
// Keeps the newest preferences only; entries for deleted chats age out rather than being removed on delete.
const maxPreferences = 200

type Preferences = Record<string, string>

function key(parentChatId: string, messageIndex: number): string {
  return `${parentChatId}:${messageIndex}`
}

function familyKey(rootChatId: string): string {
  return `family:${rootChatId}`
}

function read(): Preferences {
  try {
    const value = JSON.parse(localStorage.getItem(storageKey) ?? '{}')
    if (!value || typeof value !== 'object' || Array.isArray(value)) return {}
    return Object.fromEntries(Object.entries(value).filter((entry): entry is [string, string] => typeof entry[1] === 'string'))
  } catch {
    return {}
  }
}

/** Moves the preference to the newest position and keeps only the newest entries, so storage stays bounded. */
function remember(preferenceKey: string, chatId: string): void {
  try {
    const preferences = read()
    delete preferences[preferenceKey]
    preferences[preferenceKey] = chatId
    localStorage.setItem(storageKey, JSON.stringify(Object.fromEntries(Object.entries(preferences).slice(-maxPreferences))))
  } catch {
    // Navigation and selection still work when persistence is unavailable.
  }
}

export function setPreferredBranch(parentChatId: string, messageIndex: number, chatId: string): void {
  remember(key(parentChatId, messageIndex), chatId)
}

export function preferredBranch(parentChatId: string, messageIndex: number, availableChatIds: string[]): string | null {
  const preferences = read()
  const preferenceKey = key(parentChatId, messageIndex)
  const chatId = preferences[preferenceKey]
  if (!chatId) return null
  if (availableChatIds.includes(chatId)) return chatId
  delete preferences[preferenceKey]
  try {
    localStorage.setItem(storageKey, JSON.stringify(preferences))
  } catch {
    // Ignore storage failures.
  }
  return availableChatIds[0] ?? null
}

export function setPreferredFamilyChat(rootChatId: string, chatId: string): void {
  remember(familyKey(rootChatId), chatId)
}

export function preferredFamilyChat(rootChatId: string, availableChatIds: string[]): string | null {
  const preferences = read()
  const preferenceKey = familyKey(rootChatId)
  const chatId = preferences[preferenceKey]
  if (!chatId) return null
  if (availableChatIds.includes(chatId)) return chatId
  delete preferences[preferenceKey]
  try {
    localStorage.setItem(storageKey, JSON.stringify(preferences))
  } catch {
    // Ignore storage failures.
  }
  return null
}
