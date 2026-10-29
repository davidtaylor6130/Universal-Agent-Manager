import { describe, expect, it } from 'vitest'
import { inlineChipFor, isPotentialTableRow, parseTableSeparator, replaceInlineLatexSymbols, safeHref, splitTableRow } from './markdownParsing'

describe('markdownParsing', () => {
  describe('replaceInlineLatexSymbols', () => {
    it('renders symbol-only inline LaTeX as Unicode', () => {
      expect(replaceInlineLatexSymbols('Parse $\\rightarrow$ render')).toBe('Parse → render')
      expect(replaceInlineLatexSymbols('Parse $\\rightarrow\\$ render')).toBe('Parse → render')
      expect(replaceInlineLatexSymbols('$x \\leq y$ and $a \\times b$')).toBe('x ≤ y and a × b')
      expect(replaceInlineLatexSymbols('It is $25^\\circ$C and $\\alpha \\to \\beta$')).toBe('It is 25°C and α → β')
    })

    it('leaves money, real formulas and unknown commands exactly as written', () => {
      for (const text of ['Costs $5 and $10', 'From $5 \\to $10 today', '$\\frac{a}{b}$', '$\\unknown$', '$x^2$', '$ \\to $',
        'no math \\rightarrow here', 'map $src \\to $dst', '$\\sqrt{x+1}$']) {
        expect(replaceInlineLatexSymbols(text)).toBe(text)
      }
    })

    it('does not let a dollar amount swallow a later symbol span', () => {
      expect(replaceInlineLatexSymbols('Saves $5 per run $\\rightarrow$ cheaper')).toBe('Saves $5 per run → cheaper')
    })

    it('maps the Greek variants to their LaTeX glyphs', () => {
      expect(replaceInlineLatexSymbols('$\\epsilon$ $\\varepsilon$ $\\phi$ $\\varphi$')).toBe('ϵ ε ϕ φ')
    })
  })

  describe('inlineChipFor', () => {
    const known = ['release-train', 'origin/base']

    it('classifies file paths, bare source filenames and line references', () => {
      expect(inlineChipFor('src/components/Foo.tsx', known)).toEqual({ kind: 'file', name: 'Foo.tsx', reference: { path: 'src/components/Foo.tsx' } })
      expect(inlineChipFor('C:\\work\\main.cpp', known)).toMatchObject({ kind: 'file', name: 'main.cpp' })
      expect(inlineChipFor('./scripts/run.sh', known)).toMatchObject({ kind: 'file', name: 'run.sh' })
      expect(inlineChipFor('index.ts', known)).toEqual({ kind: 'file', name: 'index.ts', reference: { path: 'index.ts' } })
      expect(inlineChipFor('src/main.cpp:12-20', known)).toEqual({ kind: 'file', name: 'main.cpp', reference: { path: 'src/main.cpp', line: 12, endLine: 20 } })
    })

    it('classifies directories and branches, including the chat known branches', () => {
      expect(inlineChipFor('src/components/', known)).toEqual({ kind: 'directory', name: 'components' })
      expect(inlineChipFor('feature/chips', known)).toEqual({ kind: 'branch', name: 'feature/chips' })
      expect(inlineChipFor('claude/v5ui-assistant-chips', known)).toEqual({ kind: 'branch', name: 'claude/v5ui-assistant-chips' })
      expect(inlineChipFor('main', known)).toEqual({ kind: 'branch', name: 'main' })
      expect(inlineChipFor('release-train', known)).toEqual({ kind: 'branch', name: 'release-train' })
      expect(inlineChipFor('origin/base', known)).toEqual({ kind: 'branch', name: 'origin/base' })
      expect(inlineChipFor('origin/base', [])).toBeNull()
    })

    it('leaves ordinary code, URLs and version-like text as plain code', () => {
      for (const value of ['object.method', 'npm run build', 'foo()', '1.2', 'v2', '/', 'https://example.com/docs/', 'a/b.x)', '*.ts', 'x.unknownext', '']) {
        expect(inlineChipFor(value, known), value).toBeNull()
      }
    })
  })

  describe('safeHref', () => {
    it('allows http, https and mailto urls', () => {
      expect(safeHref('https://example.com')).toBe('https://example.com')
      expect(safeHref('http://example.com')).toBe('http://example.com')
      expect(safeHref('mailto:a@b.com')).toBe('mailto:a@b.com')
    })

    it('rejects unsafe schemes', () => {
      expect(safeHref('javascript:alert(1)')).toBeUndefined()
      expect(safeHref('/relative/path')).toBeUndefined()
      expect(safeHref('ftp://example.com')).toBeUndefined()
    })
  })

  describe('splitTableRow', () => {
    it('splits cells and trims surrounding pipes and whitespace', () => {
      expect(splitTableRow('| Tool | Status |')).toEqual(['Tool', 'Status'])
    })

    it('honors escaped pipes inside a cell', () => {
      expect(splitTableRow('| a \\| b | c |')).toEqual(['a | b', 'c'])
    })
  })

  describe('parseTableSeparator', () => {
    it('returns alignments for a valid separator row', () => {
      expect(parseTableSeparator('| --- | :---: | ---: |', 3)).toEqual(['left', 'center', 'right'])
    })

    it('returns null when the cell count does not match', () => {
      expect(parseTableSeparator('| --- | --- |', 3)).toBeNull()
    })

    it('returns null for a non-separator row', () => {
      expect(parseTableSeparator('| Tool | Status |', 2)).toBeNull()
    })
  })

  describe('isPotentialTableRow', () => {
    it('detects rows with at least two pipe-separated cells', () => {
      expect(isPotentialTableRow('| a | b |')).toBe(true)
      expect(isPotentialTableRow('a | b')).toBe(true)
    })

    it('rejects rows without a pipe', () => {
      expect(isPotentialTableRow('plain text')).toBe(false)
    })
  })
})
