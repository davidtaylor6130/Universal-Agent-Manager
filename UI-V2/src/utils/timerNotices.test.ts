import { afterEach, describe, expect, it, vi } from 'vitest'
import { reportTimerNotices, resetTimerNoticesForTests } from './timerNotices'
import { reportAppFailure, reportAppNotice } from './appFailure'

vi.mock('./appFailure', () => ({ reportAppFailure: vi.fn(), reportAppNotice: vi.fn() }))

afterEach(() => {
  resetTimerNoticesForTests()
  vi.clearAllMocks()
})

describe('timer notices', () => {
  it('reports each notice once, naming its chat', () => {
    const notices = [
      { serial: 1, title: 'Follow-up dropped', message: 'A scheduled follow-up was dropped.', tone: 'warning' as const, chatId: 'chat-1' },
      { serial: 2, title: 'Scheduled run did not start', message: 'Schedule “Nightly” did not start a chat: That workspace no longer exists.', tone: 'warning' as const },
    ]
    reportTimerNotices(notices, (chatId) => chatId === 'chat-1' ? 'Parser work' : undefined)
    reportTimerNotices(notices, () => undefined)
    expect(reportAppFailure).toHaveBeenCalledTimes(2)
    expect(reportAppFailure).toHaveBeenCalledWith('Chat “Parser work”: A scheduled follow-up was dropped.', '', { title: 'Follow-up dropped' })
    expect(reportAppFailure).toHaveBeenCalledWith('Schedule “Nightly” did not start a chat: That workspace no longer exists.', '', { title: 'Scheduled run did not start' })
    reportTimerNotices([...notices, { serial: 3, message: 'Done.', tone: 'info' }], () => undefined)
    expect(reportAppNotice).toHaveBeenCalledWith('Done.', 'info')
  })
})
