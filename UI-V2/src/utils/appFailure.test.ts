import { afterEach, describe, expect, it } from 'vitest'
import { APP_FAILURE_EVENT, failureDetail, isRawBridgeError, noticeText, reportAppFailure, reportAppNotice, type AppFailure } from './appFailure'

const notices: AppFailure[] = []
const collect = (event: Event) => notices.push((event as CustomEvent<AppFailure>).detail)
afterEach(() => { window.removeEventListener(APP_FAILURE_EVENT, collect); notices.length = 0 })

describe('one-line notices', () => {
  it('keeps the first line as the message and moves the rest to the detail', () => {
    expect(noticeText('Update failed.\n  at install (updater.ts:12)\n\n2. Restart', 'Exit code 1')).toEqual({
      message: 'Update failed.',
      detail: 'at install (updater.ts:12) 2. Restart · Exit code 1',
    })
    expect(noticeText('  Saved   theme. ')).toEqual({ message: 'Saved theme.', detail: '' })
    // A cause that only repeats the message adds nothing.
    expect(noticeText('Could not change Computer Use.', 'Could not change Computer Use.')).toEqual({ message: 'Could not change Computer Use.', detail: '' })
  })

  it('applies to failures and notices raised through the shared path', () => {
    window.addEventListener(APP_FAILURE_EVENT, collect)
    reportAppFailure('Could not save.\nENOSPC: no space left on device', 'Try again.')
    reportAppNotice('Imported 2 chats.\nOne was renamed.', 'success')
    expect(notices.map(({ message, detail, tone }) => [message, detail, tone])).toEqual([
      ['Could not save.', 'ENOSPC: no space left on device · Try again.', undefined],
      ['Imported 2 chats.', 'One was renamed.', 'success'],
    ])
  })

  it('builds the detail from a backend cause and next step, and recognises raw bridge text', () => {
    expect(failureDetail('The chat is busy.', 'Try again.')).toBe('The chat is busy. Try again.')
    expect(failureDetail(new Error('Connection lost.'))).toBe('Connection lost.')
    expect(failureDetail(undefined, 'Try again.')).toBe('Try again.')
    expect(isRawBridgeError('Bridge request failed: TypeError: x is undefined')).toBe(true)
    expect(isRawBridgeError('The chat could not be saved.')).toBe(false)
  })
})
