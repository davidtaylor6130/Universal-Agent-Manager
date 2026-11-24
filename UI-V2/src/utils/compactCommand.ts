/** The trimmed instructions of a typed `/compact` command ('' for none), or null when the text is not that command. */
export function compactCommandInstructions(text: string): string | null {
  const match = /^\/compact(?:\s+([\s\S]*))?$/.exec(text.trim())
  return match ? (match[1] ?? '').trim() : null
}
