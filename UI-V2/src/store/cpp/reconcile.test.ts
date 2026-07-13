import { describe, expect, it } from 'vitest'
import type { CppMessage } from './types'
import { reconcileCppMessages } from './reconcile'

const message: CppMessage = {
  role: 'user',
  content: 'Use the attached file.',
  createdAt: '2026-01-01T00:00:00.000Z',
}

describe('reconcileCppMessages attachments', () => {
  it('keeps an ordinary attachment-only update', () => {
    const existing = reconcileCppMessages('chat-1', undefined, [message])
    const attachment = {
      id: 'file-1',
      name: 'diagram.png',
      type: 'image/png',
      size: 42,
      path: '/tmp/diagram.png',
    }

    const reconciled = reconcileCppMessages('chat-1', existing, [{
      ...message,
      attachments: [attachment],
    }])

    expect(reconciled).not.toBe(existing)
    expect(reconciled[0].attachments).toEqual([attachment])
  })

  it('keeps a markdown-store attachment-only update', () => {
    const existing = reconcileCppMessages('chat-1', undefined, [message])
    const filePath = '/tmp/skill-builder.uam'

    const reconciled = reconcileCppMessages('chat-1', existing, [{
      ...message,
      markdownStoreFiles: [filePath],
    }])

    expect(reconciled).not.toBe(existing)
    expect(reconciled[0].attachments).toEqual([{
      id: filePath,
      name: 'skill-builder.uam',
      type: 'markdown-store',
      size: 0,
      path: filePath,
    }])
  })
})
