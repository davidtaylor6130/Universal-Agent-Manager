import type { TimerNotice } from '../store/cpp/types'
import { reportAppFailure, reportAppNotice } from './appFailure'

// Highest timer notice serial already reported. The native serial counts up for the whole app run.
// ponytail: module memory, so a reload of the interface (not an app restart) reports the retained notices once more.
let reported = 0

/** Sends each new scheduled follow-up or automation schedule notice to the notifications panel once. */
export function reportTimerNotices(notices: readonly TimerNotice[], chatName: (chatId: string) => string | undefined): void {
  for (const notice of notices) {
    if (notice.serial <= reported) continue
    reported = notice.serial
    const name = notice.chatId ? chatName(notice.chatId) : undefined
    const message = name ? `Chat “${name}”: ${notice.message}` : notice.message
    if (notice.tone === 'warning') reportAppFailure(message, '')
    else reportAppNotice(message, notice.tone)
  }
}

export function resetTimerNoticesForTests(): void {
  reported = 0
}
