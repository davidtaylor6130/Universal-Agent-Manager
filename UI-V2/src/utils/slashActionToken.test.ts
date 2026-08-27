import { describe, expect, it } from 'vitest'
import { replaceSlashAction, slashActionToken } from './slashActionToken'

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
