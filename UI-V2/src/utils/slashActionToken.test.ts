import { describe, expect, it } from 'vitest'
import { mentionActionToken, replaceSlashAction, slashActionToken } from './slashActionToken'

describe('slash action token', () => {
  it('finds actions at the start or after whitespace and preserves surrounding text', () => {
    expect(slashActionToken('/mem keep this', 3)).toMatchObject({ command: 'mem', commandStart: 0, end: 4 })
    const middle = slashActionToken('before /mem after', 10)!
    expect(middle).toMatchObject({ command: 'mem', commandStart: 7, end: 11 })
    expect(replaceSlashAction('before /mem after', middle, '/memory ')).toBe('before /memory  after')
  })

  it('does not treat an embedded slash or path as an action', () => {
    expect(slashActionToken('word/permission', 8)).toBeNull()
    expect(slashActionToken('/tmp/file', 4)).toBeNull()
  })

  it('uses the cursor token for sub-palette filtering and whole-action replacement', () => {
    const token = slashActionToken('before /permission yo after', 20)!
    expect(token).toMatchObject({ command: 'permission', query: 'yo', queryStart: 19, end: 21 })
    expect(replaceSlashAction('before /permission yo after', token, '', true)).toBe('before  after')
  })
})

describe('mention action token', () => {
  it('finds a plain @ token at the caret and ignores email addresses and selections', () => {
    expect(mentionActionToken('Read @docs/a.md now', 15)).toEqual({ query: 'docs/a.md', start: 5, end: 15, quoted: false })
    expect(mentionActionToken('Check @', 7)).toEqual({ query: '', start: 6, end: 7, quoted: false })
    expect(mentionActionToken('mail me@example.com', 19)).toBeNull()
    expect(mentionActionToken('Read @docs', 6, 9)).toBeNull()
  })

  it('handles quoted paths: open, closed, multi-line and caret positions', () => {
    expect(mentionActionToken('@"my file.md" next', 6)).toEqual({ query: 'my file.md', start: 0, end: 13, quoted: true })
    expect(mentionActionToken('Read @"draft one', 16)).toEqual({ query: 'draft one', start: 5, end: 16, quoted: true })
    expect(mentionActionToken('@"draft\nnext line', 18)).toBeNull()
    expect(mentionActionToken('@"draft\nnext line', 4)).toEqual({ query: 'draft', start: 0, end: 7, quoted: true })
    // Caret after the closing quote or before the opening quote is not inside the mention.
    expect(mentionActionToken('@"x" more', 4)).toBeNull()
    expect(mentionActionToken('@"x"', 0)).toBeNull()
    expect(mentionActionToken('@"x"', 1)).toBeNull()
    // A quote preceded by a word is not a mention.
    expect(mentionActionToken('say@"x"', 5)).toBeNull()
  })
})
