export interface SlashActionToken {
  command: string
  query: string
  commandStart: number
  queryStart: number
  end: number
}

const subPaletteCommands = new Set(['permission', 'memory', 'reasoning', 'speed', 'variants'])

function tokenBounds(value: string, start: number, end: number): [number, number] {
  let left = Math.min(Math.max(0, start), value.length)
  let right = Math.min(Math.max(left, end), value.length)
  while (left > 0 && !/\s/.test(value[left - 1])) left -= 1
  while (right < value.length && !/\s/.test(value[right])) right += 1
  return [left, right]
}

export function slashActionToken(value: string, selectionStart: number, selectionEnd = selectionStart): SlashActionToken | null {
  const [activeStart, activeEnd] = tokenBounds(value, selectionStart, selectionEnd)
  const active = value.slice(activeStart, activeEnd)
  if (active.startsWith('/') && !active.slice(1).includes('/')) {
    return { command: active.slice(1).toLowerCase(), query: active.slice(1).toLowerCase(), commandStart: activeStart, queryStart: activeStart + 1, end: activeEnd }
  }

  let scan = activeStart
  while (scan > 0) {
    while (scan > 0 && /\s/.test(value[scan - 1])) scan -= 1
    const [, previousEnd] = tokenBounds(value, scan, scan)
    let previousStart = scan
    while (previousStart > 0 && !/\s/.test(value[previousStart - 1])) previousStart -= 1
    const previous = value.slice(previousStart, previousEnd)
    if (previous.startsWith('/')) {
      const command = previous.slice(1).toLowerCase()
      if (!subPaletteCommands.has(command) || previous.slice(1).includes('/')) return null
      return {
        command,
        query: value.slice(previousEnd, activeEnd).trim().toLowerCase(),
        commandStart: previousStart,
        queryStart: activeStart,
        end: activeEnd,
      }
    }
    scan = previousStart
  }
  return null
}

export function replaceSlashAction(value: string, token: SlashActionToken, replacement: string, wholeAction = false): string {
  const start = wholeAction ? token.commandStart : token.queryStart - (token.commandStart === token.queryStart - 1 ? 1 : 0)
  return `${value.slice(0, start)}${replacement}${value.slice(token.end)}`
}

export interface MentionActionToken {
  query: string
  start: number
  end: number
  quoted: boolean
}

/** The `@` token at the caret, if any, including a quoted `@"path with spaces"` span. */
export function mentionActionToken(value: string, selectionStart: number, selectionEnd = selectionStart): MentionActionToken | null {
  if (selectionStart !== selectionEnd) return null
  const caret = Math.min(Math.max(0, selectionStart), value.length)
  // The caret must sit after the opening quote; lastIndexOf clamps a negative start to 0.
  const quoteStart = caret >= 2 ? value.lastIndexOf('@"', caret - 2) : -1
  if (quoteStart >= 0 && (quoteStart === 0 || /\s/.test(value[quoteStart - 1])) && !value.slice(quoteStart + 2, caret).includes('"') && !value.slice(quoteStart, caret).includes('\n')) {
    const close = value.indexOf('"', caret)
    const lineEnd = value.indexOf('\n', caret)
    const end = close >= 0 && (lineEnd < 0 || close < lineEnd) ? close + 1 : lineEnd >= 0 ? lineEnd : value.length
    return { query: value.slice(quoteStart + 2, close >= 0 && end === close + 1 ? close : end), start: quoteStart, end, quoted: true }
  }
  const [start, end] = tokenBounds(value, caret, caret)
  const token = value.slice(start, end)
  if (!token.startsWith('@') || token.startsWith('@"')) return null
  return { query: token.slice(1), start, end, quoted: false }
}
