// Pure (JSX-free) helpers for the hand-rolled markdown renderer.
// Kept separate from the React components so they can be unit-tested directly.

export function safeHref(url: string): string | undefined {
  return /^(https?:|mailto:)/i.test(url) ? url : undefined
}

export function splitTableRow(line: string): string[] {
  const trimmed = line.trim().replace(/^\|/, '').replace(/\|$/, '')
  const cells: string[] = []
  let cell = ''
  let escaped = false

  for (const char of trimmed) {
    if (escaped) {
      cell += char
      escaped = false
      continue
    }

    if (char === '\\') {
      escaped = true
      continue
    }

    if (char === '|') {
      cells.push(cell.trim())
      cell = ''
      continue
    }

    cell += char
  }

  if (escaped) {
    cell += '\\'
  }
  cells.push(cell.trim())
  return cells
}

export function parseTableSeparator(line: string, expectedCells: number): Array<'left' | 'center' | 'right'> | null {
  const cells = splitTableRow(line)
  if (cells.length !== expectedCells) return null

  const alignments: Array<'left' | 'center' | 'right'> = []
  for (const cell of cells) {
    const normalized = cell.replace(/\s+/g, '')
    if (!/^:?-{3,}:?$/.test(normalized)) return null
    if (normalized.startsWith(':') && normalized.endsWith(':')) alignments.push('center')
    else if (normalized.endsWith(':')) alignments.push('right')
    else alignments.push('left')
  }

  return alignments
}

export function isPotentialTableRow(line: string): boolean {
  return line.includes('|') && splitTableRow(line).length >= 2
}
