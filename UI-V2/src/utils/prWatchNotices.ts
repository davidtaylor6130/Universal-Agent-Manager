import type { PullRequestWatch } from '../store/cpp/types'
import { reportAppFailure, reportAppNotice } from './appFailure'

// Highest notice serial already reported per chat and pull request.
// ponytail: module memory, so a reload of the interface (not an app restart) reports the latest notice once more.
const reported = new Map<string, number>()

/** Sends each new pull request watch notice to the notifications panel once. */
export function reportPullRequestWatchNotices(watches: readonly PullRequestWatch[], chatName: (chatId: string) => string | undefined): void {
  for (const watch of watches) {
    if (!watch.notice) continue
    const key = `${watch.chatId}\n${watch.repository}#${watch.number}`
    if ((reported.get(key) ?? 0) >= watch.notice.serial) continue
    reported.set(key, watch.notice.serial)
    const name = chatName(watch.chatId)
    const message = name ? `${watch.notice.message} (chat “${name}”)` : watch.notice.message
    if (watch.notice.tone === 'warning') reportAppFailure(message, '')
    else reportAppNotice(message, watch.notice.tone)
  }
}

export function resetPullRequestWatchNoticesForTests(): void {
  reported.clear()
}
