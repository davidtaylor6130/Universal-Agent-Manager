import { afterEach, expect, it, vi } from 'vitest'
import { companionPlatformAction } from './companionPlatform'

afterEach(() => vi.unstubAllGlobals())

it('uses the phone clipboard and reports denied access', async () => {
  const writeText = vi.fn().mockResolvedValue(undefined)
  vi.stubGlobal('navigator', { clipboard: { writeText } })
  expect(await companionPlatformAction('writeClipboardText', { text: 'Phone text' })).toMatchObject({ ok: true })
  expect(writeText).toHaveBeenCalledWith('Phone text')
  writeText.mockRejectedValue(new Error('denied'))
  expect(await companionPlatformAction('writeClipboardText', { text: 'Phone text' })).toMatchObject({ ok: false, error: expect.stringContaining('clipboard') })
})

it('leaves backend features on the shared bridge', async () => {
  expect(await companionPlatformAction('createSession', {})).toBeUndefined()
})

it('delivers phone dictation through the shared chat event protocol', async () => {
  let speech: any
  const events: unknown[] = []
  const receive = (event: Event) => events.push((event as CustomEvent).detail)
  window.addEventListener('uam-dictation', receive)
  class Recognition {
    start = vi.fn()
    stop = () => this.onend?.()
    onend?: () => void
    onresult?: (event: unknown) => void
    constructor() { speech = this }
  }
  vi.stubGlobal('SpeechRecognition', Recognition)
  try {
    expect(await companionPlatformAction('startDictation', { dictationId: 'phone', locale: 'en-GB' })).toMatchObject({ ok: true })
    speech.onresult({ resultIndex: 0, results: [{ isFinal: true, 0: { transcript: 'Hello' } }] })
    expect(events).toContainEqual({ type: 'dictation', dictationId: 'phone', event: 'final', text: 'Hello' })
    await companionPlatformAction('stopDictation', {})
    expect(events).toContainEqual({ type: 'dictation', dictationId: 'phone', event: 'stopped' })
  } finally { await companionPlatformAction('stopDictation', {}); window.removeEventListener('uam-dictation', receive) }
})
