import { describe, expect, it } from 'vitest'
import { isPotentialTableRow, parseTableSeparator, safeHref, splitTableRow } from './markdownParsing'

describe('markdownParsing', () => {
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
