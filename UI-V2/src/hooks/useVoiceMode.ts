import { useEffect, useRef, useState } from 'react'
import { create } from 'zustand'
import { createRequestId, getSpeechOutputState, isCefContext, isCompanionContext, sendToCEF, startSpeechOutput, stopSpeechOutput } from '../ipc/cefBridge'
import type { Message } from '../types/message'
import type { DictationPushMessage } from '../store/useAppStore'

export interface VoicePreferences {
  language: string
  voiceId: string
  rate: number
  volume: number
  readReplies: boolean
  shortcut: 'v' | 'm' | 'off'
}
const defaults: VoicePreferences = { language: '', voiceId: '', rate: 1, volume: 1, readReplies: true, shortcut: 'v' }
function readPreferences(): VoicePreferences {
  try {
    const value = JSON.parse(localStorage.getItem('uam-voice-preferences') || '{}')
    return {
      language: typeof value.language === 'string' ? value.language.slice(0, 64) : '',
      voiceId: typeof value.voiceId === 'string' ? value.voiceId.slice(0, 512) : '',
      rate: typeof value.rate === 'number' && value.rate >= 0.5 && value.rate <= 2 ? value.rate : 1,
      volume: typeof value.volume === 'number' && value.volume >= 0 && value.volume <= 1 ? value.volume : 1,
      readReplies: typeof value.readReplies === 'boolean' ? value.readReplies : true,
      shortcut: value.shortcut === 'm' || value.shortcut === 'off' ? value.shortcut : 'v',
    }
  } catch { return defaults }
}
export const useVoicePreferences = create<{ preferences: VoicePreferences; save: (patch: Partial<VoicePreferences>) => boolean }>((set, get) => ({
  preferences: readPreferences(),
  save: patch => {
    const preferences = { ...get().preferences, ...patch }
    try { localStorage.setItem('uam-voice-preferences', JSON.stringify(preferences)) }
    catch { return false }
    set({ preferences })
    return true
  },
}))

interface VoiceOptions {
  contextKey: string
  enabled: boolean
  blocked: boolean
  messages: Message[]
  processing: boolean
  append: (text: string) => void
  send: () => void
  stopResponse: () => void
  navigate: (direction: number) => void
}

/** Owns native input/playback IDs for the selected chat. Playback pauses input until it ends. */
export function useVoiceMode(options: VoiceOptions) {
  const preferences = useVoicePreferences(s => s.preferences)
  const latest = useRef({ ...options, preferences })
  latest.current = { ...options, preferences }
  const available = isCefContext() && !isCompanionContext() && options.enabled
  const [active, setActive] = useState(false)
  const [speaking, setSpeaking] = useState(false)
  const [status, setStatus] = useState('')
  const [error, setError] = useState('')
  const controls = useRef({ toggle: () => {}, stop: () => {}, read: (_text: string) => {}, replies: () => {} })

  useEffect(() => {
    setActive(false); setSpeaking(false); setStatus(''); setError('')
    let alive = true, listening = false, mode = false, paused = false
    let inputId = '', speechId = '', timer = 0, poll = 0, emptyEnds = 0
    let hadFinal = false, endWaiter: (() => void) | undefined
    const seen = new Set<string>()
    let queuedReply = ''
    const fail = (message: string) => { stop(); if (alive) setError(message) }
    const stop = () => {
      mode = false
      paused = false
      window.clearTimeout(timer)
      window.clearTimeout(poll)
      const ownedInput = inputId, ownedSpeech = speechId
      listening = false; queuedReply = ''
      endWaiter?.(); endWaiter = undefined
      if (alive) { setStatus(ownedInput || ownedSpeech ? 'Stopping voice mode' : ''); setActive(Boolean(ownedInput)); setSpeaking(Boolean(ownedSpeech)) }
      const finish = () => { if (alive && !inputId && !speechId) { setActive(false); setSpeaking(false); setStatus('') } }
      if (ownedInput) void sendToCEF({ action: 'stopDictation', payload: { dictationId: ownedInput } }).then(response => {
        if (inputId !== ownedInput) return
        if (response.ok) inputId = ''
        else if (alive) setError(response.error || 'Voice input could not stop. Try Stop again.')
        finish()
      })
      if (ownedSpeech) void stopSpeechOutput(ownedSpeech).then(response => {
        if (speechId !== ownedSpeech) return
        if (response.ok) speechId = ''
        else if (alive) setError(response.error || 'Speech playback could not stop. Try Stop again.')
        finish()
      })
      finish()
    }
    const start = async () => {
      if (!alive || !mode || paused || listening || inputId || speechId) return
      if (!available || latest.current.blocked || navigator.onLine === false) { fail('Voice input is unavailable.'); return }
      inputId = createRequestId('voice-input'); listening = true; hadFinal = false
      const id = inputId
      setStatus('Starting voice input')
      const response = await sendToCEF<{ started: boolean }>({ action: 'startDictation', payload: { dictationId: id, locale: latest.current.preferences.language || navigator.language || '' } })
      if (!alive || inputId !== id) { void sendToCEF({ action: 'stopDictation', payload: { dictationId: id } }); return }
      if (!response.ok || !response.data?.started) { fail(response.error || 'Voice input could not start.'); return }
      setStatus('Listening')
    }
    const rearm = () => { window.clearTimeout(timer); if (mode && !paused) timer = window.setTimeout(() => void start(), 250) }
    const read = async (text: string) => {
      if (!alive || !available || !text.trim() || speechId || latest.current.blocked) return
      if (new TextEncoder().encode(text).length > 65536) { setError('Response is too long to read aloud.'); return }
      speechId = createRequestId('voice-output')
      const id = speechId
      paused = true; setSpeaking(true); setStatus('Finishing voice input')
      if (listening) {
        const id = inputId
        let timeout = 0
        const ended = new Promise<boolean>(resolve => {
          endWaiter = () => { window.clearTimeout(timeout); resolve(true) }
          timeout = window.setTimeout(() => { endWaiter = undefined; resolve(false) }, 3000)
        })
        const stopped = await sendToCEF({ action: 'stopDictation', payload: { dictationId: id } })
        if (!stopped.ok) { endWaiter?.(); fail(stopped.error || 'Voice input could not stop.'); return }
        if (!await ended) { fail('Voice input did not finish. Try again.'); return }
      }
      if (!alive || !paused || speechId !== id) return
      setSpeaking(true); setStatus('Reading response'); setError('')
      const { voiceId, language, rate, volume } = latest.current.preferences
      const response = await startSpeechOutput({ speechId: id, text, voiceId, language, rate, volume })
      if (!alive || !paused || speechId !== id) { void stopSpeechOutput(id); return }
      if (!response.ok || !response.data?.started) { fail(response.error || 'Speech playback could not start.'); return }
      const check = async () => {
        const state = await getSpeechOutputState()
        if (!alive || !paused || speechId !== id) return
        if (!state.ok || !state.data || state.data.speechId !== id) { fail(state.error || 'Speech playback is no longer available.'); return }
        if (state.data.error) { fail(state.data.error); return }
        if (state.data.speaking) { poll = window.setTimeout(() => void check(), 250); return }
        speechId = ''; paused = false; setSpeaking(false); setStatus(mode ? 'Listening' : '')
        if (queuedReply) { const reply = queuedReply; queuedReply = ''; void read(reply) } else rearm()
      }
      poll = window.setTimeout(() => void check(), 250)
    }
    const toggle = () => {
      if (mode || inputId || speechId) { stop(); return }
      if (!available || latest.current.blocked) return
      mode = true; emptyEnds = 0; setActive(true); setError('')
      latest.current.messages.forEach(message => seen.add(message.id))
      void start()
    }
    const onInput = (event: Event) => {
      const message = (event as CustomEvent<DictationPushMessage>).detail
      if (!alive || !message || message.dictationId !== inputId || !listening) return
      if (message.event === 'error') { fail(message.message || 'Voice input failed.'); return }
      if (message.event === 'final') {
        const text = message.text.trim()
        hadFinal ||= Boolean(text)
        if (paused) { if (text) latest.current.append(text); return }
        const command = text.toLocaleLowerCase().replace(/[.!?]+$/, '')
        if (command === 'stop voice mode') stop()
        else if (command === 'send message') latest.current.send()
        else if (command === 'stop response') latest.current.stopResponse()
        else if (command === 'next chat') latest.current.navigate(1)
        else if (command === 'previous chat') latest.current.navigate(-1)
        else if (text) latest.current.append(text)
        return
      }
      if (message.event !== 'end') return
      listening = false; inputId = ''
      endWaiter?.(); endWaiter = undefined
      if (paused) return
      emptyEnds = hadFinal ? 0 : emptyEnds + 1
      if (emptyEnds >= 3) { fail('Voice input ended without speech. Start voice mode again.'); return }
      rearm()
    }
    const onKey = (event: KeyboardEvent) => {
      if (latest.current.preferences.shortcut !== 'off' && (event.ctrlKey || event.metaKey) && event.shiftKey && !event.altKey && event.key.toLowerCase() === latest.current.preferences.shortcut && !event.repeat) { event.preventDefault(); toggle() }
    }
    const onOffline = () => fail('Voice mode stopped because the connection is offline.')
    const replies = () => {
      if (!mode || !latest.current.preferences.readReplies || latest.current.processing) return
      const replies = latest.current.messages.filter(message => !seen.has(message.id) && message.role === 'assistant' && !message.isStreaming && !message.interrupted && message.content.trim())
      replies.forEach(message => seen.add(message.id))
      const reply = replies[replies.length - 1]
      if (reply) { if (speechId) queuedReply = reply.content; else void read(reply.content) }
    }
    controls.current = { toggle, stop, read: text => void read(text), replies }
    window.addEventListener('uam-dictation', onInput)
    window.addEventListener('keydown', onKey)
    window.addEventListener('offline', onOffline)
    return () => { alive = false; stop(); window.removeEventListener('uam-dictation', onInput); window.removeEventListener('keydown', onKey); window.removeEventListener('offline', onOffline) }
  }, [options.contextKey, available])

  useEffect(() => { controls.current.replies() }, [options.messages, options.processing, preferences.readReplies])
  useEffect(() => { if (options.blocked) controls.current.stop() }, [options.blocked])
  return { active, speaking, status, error, available, toggle: () => controls.current.toggle(), stop: () => controls.current.stop(), read: (text: string) => controls.current.read(text) }
}
