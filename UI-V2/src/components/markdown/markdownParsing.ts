// Pure (JSX-free) helpers for the hand-rolled markdown renderer.
// Kept separate from the React components so they can be unit-tested directly.

import { parseFileReference, type FileReference } from '../../utils/fileReferences'
import { CLAUDE_ENABLED, CODEX_ENABLED } from '../../config/buildFeatures'

export function safeHref(url: string): string | undefined {
  return /^(https?:|mailto:)/i.test(url) ? url : undefined
}

// Models (notably Gemini) write symbols as inline LaTeX, e.g. `$\rightarrow$`. This renderer has no math
// engine, so symbol-only spans become their Unicode character.
// ponytail: symbol table, not a math renderer; add KaTeX if models start sending real formulas.
const LATEX_SYMBOLS: Record<string, string> = {
  rightarrow: '→', to: '→', leftarrow: '←', gets: '←', leftrightarrow: '↔', uparrow: '↑', downarrow: '↓',
  Rightarrow: '⇒', implies: '⇒', Leftarrow: '⇐', Leftrightarrow: '⇔', iff: '⇔', mapsto: '↦',
  longrightarrow: '⟶', longleftarrow: '⟵', Longrightarrow: '⟹',
  times: '×', cdot: '·', div: '÷', pm: '±', mp: '∓', neq: '≠', ne: '≠', leq: '≤', le: '≤', geq: '≥', ge: '≥',
  approx: '≈', sim: '∼', equiv: '≡', propto: '∝', ll: '≪', gg: '≫', infty: '∞', circ: '∘', degree: '°',
  checkmark: '✓', ldots: '…', dots: '…', cdots: '⋯', in: '∈', notin: '∉', subset: '⊂', subseteq: '⊆',
  cup: '∪', cap: '∩', emptyset: '∅', forall: '∀', exists: '∃', neg: '¬', land: '∧', lor: '∨', sum: '∑', prod: '∏',
  partial: '∂', nabla: '∇', sqrt: '√', alpha: 'α', beta: 'β', gamma: 'γ', delta: 'δ', epsilon: 'ε', theta: 'θ',
  lambda: 'λ', mu: 'μ', pi: 'π', sigma: 'σ', tau: 'τ', phi: 'φ', omega: 'ω', Delta: 'Δ', Sigma: 'Σ', Omega: 'Ω',
}

/** Replaces `$…$` spans made only of known LaTeX symbol commands; anything else is left exactly as written. */
export function replaceInlineLatexSymbols(text: string): string {
  if (!text.includes('$') || !text.includes('\\')) return text
  // As in Markdown math: no space after the opening $, and no digit after the closing $ (so "$5 \to $10" stays text).
  return text.replace(/\$(?!\s)([^$\n]{1,120}?)\\?\$(?!\d)/g, (span, body: string) => {
    if (!/\\[A-Za-z]/.test(body)) return span
    let known = true
    const rendered = body
      .replace(/\^\s*\{?\\circ\}?/g, '°')
      .replace(/\\([A-Za-z]+)/g, (command, name: string) => {
        const symbol = LATEX_SYMBOLS[name]
        if (symbol === undefined) known = false
        return symbol ?? command
      })
      .replace(/[{}]/g, '')
    // Commands with arguments (\frac, \text…) or unknown macros mean this is a real formula; keep the source.
    return known && !/[\\^_]/.test(rendered) ? rendered.trim() : span
  })
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
