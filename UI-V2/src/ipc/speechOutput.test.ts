import { afterEach, expect, it, vi } from 'vitest'
import { getSpeechOutputState, listSpeechVoices, startSpeechOutput, stopSpeechOutput } from './cefBridge'

afterEach(() => { delete window.cefQuery; vi.restoreAllMocks() })

it('routes typed speech requests and preserves native playback identity and failures', async () => {
  const requests: { action: string; payload?: unknown }[] = []
  const responses = [
    { voices: [{ id: 'voice-1', name: 'Installed voice', language: 'en-GB' }] },
    { started: true, speechId: 'owned-playback' },
    { speechId: 'owned-playback', speaking: false, error: '' },
    { stopped: false },
  ]
  window.cefQuery = ({ request, onSuccess, onFailure }) => {
    requests.push(JSON.parse(request))
    const response = responses.shift()
    if (response) onSuccess(JSON.stringify(response))
    else onFailure(409, 'Stop voice input before starting speech playback.')
  }
  const options = { speechId: 'owned-playback', text: 'Reply', voiceId: 'voice-1', language: 'en-GB', rate: 1.25, volume: 0.5 }
  expect((await listSpeechVoices()).data?.voices[0].id).toBe('voice-1')
  expect((await startSpeechOutput(options)).data).toEqual({ started: true, speechId: 'owned-playback' })
  expect((await getSpeechOutputState()).data?.speaking).toBe(false)
  expect((await stopSpeechOutput('foreign-playback')).data?.stopped).toBe(false)
  vi.spyOn(console, 'error').mockImplementation(() => {})
  expect(await startSpeechOutput(options)).toMatchObject({ ok: false, error: 'Stop voice input before starting speech playback.' })
  expect(requests.map(({ action }) => action)).toEqual(['listSpeechVoices', 'startSpeechOutput', 'getSpeechOutputState', 'stopSpeechOutput', 'startSpeechOutput'])
  expect(requests[1].payload).toEqual(options)
  expect(requests[3].payload).toEqual({ speechId: 'foreign-playback' })
})
