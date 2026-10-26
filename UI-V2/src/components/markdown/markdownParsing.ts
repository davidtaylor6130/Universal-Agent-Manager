// Pure (JSX-free) helpers for the hand-rolled markdown renderer.
// Kept separate from the React components so they can be unit-tested directly.

import { parseFileReference, type FileReference } from '../../utils/fileReferences'
import { CLAUDE_ENABLED, CODEX_ENABLED } from '../../config/buildFeatures'

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

export interface InlineChip {
  kind: 'file' | 'directory' | 'branch'
  name: string
  /** Set for files: the parsed path plus any :line / #L range, used for the preview. */
  reference?: FileReference
}

const SOURCE_FILE = /\.(?:[cm]?[jt]sx?|[ch](?:pp|xx)?|cs|py|rs|go|java|swift|kt|json|ya?ml|toml|md|txt|css|scss|html|xml|sh|ps1|bat|sql|vue|svelte)$/i
// Agent branch prefixes are listed only for providers this build includes.
const BRANCH_NAME = new RegExp(`^(?:(?:${['feature', 'fix', ...(CLAUDE_ENABLED ? ['claude'] : []), ...(CODEX_ENABLED ? ['codex'] : []), 'uam'].join('|')})\\/[\\w./-]+|main|master|develop)$`)
// Path-ish characters only: anything with spaces, operators or brackets is ordinary code.
const PATH_CHARS = /^[\w./\\:#@+~%-]+$/

/** Classifies an inline code span as a file, folder or branch reference; null keeps it as plain code. */
export function inlineChipFor(value: string, knownBranches: readonly string[]): InlineChip | null {
  const text = value.trim()
  if (!text || text.length > 300 || !PATH_CHARS.test(text) || text.includes('://')) return null
  if (/[\\/]$/.test(text)) {
    const name = text.replace(/[\\/]+$/, '').split(/[\\/]/).pop() ?? ''
    return name && name !== '.' && name !== '..' ? { kind: 'directory', name } : null
  }
  const reference = parseFileReference(text)
  if (reference) {
    const name = reference.path.split(/[\\/]/).pop() ?? ''
    const isFile = /[\\/]/.test(reference.path) ? /\.[a-z\d_-]+$/i.test(name) : SOURCE_FILE.test(name)
    if (isFile) return { kind: 'file', name, reference }
  }
  return knownBranches.includes(text) || BRANCH_NAME.test(text) ? { kind: 'branch', name: text } : null
}
