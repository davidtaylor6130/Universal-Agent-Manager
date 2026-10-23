/** Browser device services used by the shared GUI instead of the host computer. */
interface SpeechResult {
  isFinal: boolean
  0: { transcript: string }
}
interface SpeechRecognitionInstance {
  lang: string
  continuous: boolean
  interimResults: boolean
  onresult: ((event: { resultIndex: number; results: ArrayLike<SpeechResult> }) => void) | null
  onerror: ((event: { error: string }) => void) | null
  onend: (() => void) | null
  start(): void
  stop(): void
}
type SpeechConstructor = new () => SpeechRecognitionInstance
let recognition: SpeechRecognitionInstance | null = null

export function browserDictationAvailable(): boolean {
  const device = window as Window & { SpeechRecognition?: SpeechConstructor; webkitSpeechRecognition?: SpeechConstructor }
  return Boolean(device.SpeechRecognition || device.webkitSpeechRecognition)
}

export async function companionPlatformAction(action: string, payload: unknown): Promise<{ ok: boolean; data?: unknown; error?: string } | undefined> {
  const values = (payload ?? {}) as { text?: string; locale?: string; dictationId?: string }
  if (action === 'writeClipboardText') {
    try { await navigator.clipboard.writeText(values.text ?? ''); return { ok: true, data: {} } }
    catch { return { ok: false, error: 'Your browser could not copy this text. Allow clipboard access and try again.' } }
  }
  if (action === 'stopDictation') {
    recognition?.stop()
    return { ok: true, data: { stopped: true } }
  }
  if (action !== 'startDictation') return undefined
  const device = window as Window & { SpeechRecognition?: SpeechConstructor; webkitSpeechRecognition?: SpeechConstructor }
  const Recognition = device.SpeechRecognition ?? device.webkitSpeechRecognition
  if (!Recognition) return { ok: false, error: 'Speech recognition is unavailable in this browser. Use your keyboard microphone.' }
  if (recognition) return { ok: false, error: 'Dictation is already running.' }
  const emit = (event: string, detail: object = {}) => window.dispatchEvent(new CustomEvent('uam-dictation', { detail: { type: 'dictation', dictationId: values.dictationId, event, ...detail } }))
  const current = new Recognition()
  current.lang = values.locale || navigator.language
  current.continuous = true
  current.interimResults = true
  current.onresult = (event) => {
    let interim = ''
    for (let index = event.resultIndex; index < event.results.length; index += 1) {
      const result = event.results[index]
      if (result.isFinal) emit('final', { text: result[0].transcript })
      else interim += result[0].transcript
    }
    emit('interim', { text: interim })
  }
  current.onerror = (event) => emit('error', { message: event.error === 'not-allowed' ? 'Allow microphone access in your browser to use dictation.' : `Dictation failed: ${event.error}.` })
  current.onend = () => { if (recognition === current) recognition = null; emit('stopped') }
  recognition = current
  try { current.start(); return { ok: true, data: { started: true } } }
  catch { recognition = null; return { ok: false, error: 'Could not start dictation. Try again.' } }
}
