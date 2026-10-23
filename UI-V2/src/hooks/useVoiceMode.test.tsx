import { act } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, beforeEach, expect, it, vi } from 'vitest'
import { useVoiceMode, useVoicePreferences } from './useVoiceMode'
import type { Message } from '../types/message'

(globalThis as typeof globalThis & { IS_REACT_ACT_ENVIRONMENT?: boolean }).IS_REACT_ACT_ENVIRONMENT = true
let host: HTMLDivElement, root: Root
let voice: ReturnType<typeof useVoiceMode>
let requests: { action: string; payload: Record<string, unknown> }[]
let messages: Message[], contextKey: string, processing: boolean
const append = vi.fn(), send = vi.fn(), stopResponse = vi.fn(), navigate = vi.fn()
function Harness() {
  voice = useVoiceMode({ contextKey, enabled: true, blocked: false, messages, processing, append, send, stopResponse, navigate })
  return <span>{voice.status}{voice.error}</span>
}
async function render() { await act(async () => { root.render(<Harness/>) }) }
async function push(event: string, text = '', id = requests.filter(request => request.action === 'startDictation').slice(-1)[0]?.payload.dictationId) {
  await act(async () => { window.dispatchEvent(new CustomEvent('uam-dictation', { detail: { type: 'dictation', dictationId: id, event, text, message: text } })) })
}
async function tick() { await act(async () => { await vi.advanceTimersByTimeAsync(250) }) }
const reply = (id: string, content: string, isStreaming = false): Message => ({ id, content, isStreaming, role: 'assistant', sessionId: 'chat', createdAt: new Date() })

beforeEach(() => {
  vi.useFakeTimers(); vi.clearAllMocks()
  messages = []; contextKey = 'chat:provider:runtime'; processing = false; requests = []
  useVoicePreferences.setState({ preferences: { language: 'en-GB', voiceId: 'installed', rate: 1.2, volume: 0.5, readReplies: true, shortcut: 'v' } })
  host = document.createElement('div'); document.body.appendChild(host); root = createRoot(host)
  window.cefQuery = ({ request, onSuccess }) => {
    const parsed = JSON.parse(request); requests.push(parsed)
    if (parsed.action === 'startDictation') onSuccess(JSON.stringify({ started: true }))
    else if (parsed.action === 'stopDictation') { onSuccess(JSON.stringify({ stopped: true })); window.dispatchEvent(new CustomEvent('uam-dictation', { detail: { type: 'dictation', dictationId: parsed.payload.dictationId, event: 'end' } })) }
    else if (parsed.action === 'startSpeechOutput') onSuccess(JSON.stringify({ started: true, speechId: parsed.payload.speechId }))
    else if (parsed.action === 'getSpeechOutputState') onSuccess(JSON.stringify({ speechId: requests.filter(request => request.action === 'startSpeechOutput').slice(-1)[0]?.payload.speechId, speaking: false, error: '' }))
    else onSuccess(JSON.stringify({ stopped: true }))
  }
})
afterEach(() => { act(() => root.unmount()); host.remove(); delete window.cefQuery; vi.useRealTimers(); vi.restoreAllMocks(); window.history.replaceState({}, '', '/'); })

it('rearms native input and routes only exact safe commands, preserving ordinary speech', async () => {
  await render(); await act(async () => voice.toggle())
  const first = requests[0].payload.dictationId
  await push('final', 'send message', 'foreign')
  expect(send).not.toHaveBeenCalled()
  await push('final', 'Please send message later.')
  expect(append).toHaveBeenCalledWith('Please send message later.')
  await push('end'); await tick()
  expect(requests.filter(request => request.action === 'startDictation')).toHaveLength(2)
  expect(requests.filter(request => request.action === 'startDictation')[1].payload.dictationId).not.toBe(first)
  await push('final', 'Send message.'); await push('final', 'Stop response'); await push('final', 'Next chat'); await push('final', 'Previous chat')
  expect(send).toHaveBeenCalledOnce(); expect(stopResponse).toHaveBeenCalledOnce(); expect(navigate.mock.calls).toEqual([[1], [-1]])
  await push('final', 'Stop voice mode')
  expect(voice.active).toBe(false); await tick()
  expect(requests.filter(request => request.action === 'startDictation')).toHaveLength(2)
})

it('reads only new completed replies, pauses native input, and rearms after owned playback ends', async () => {
  messages = [reply('old', 'Old response')]
  await render(); await act(async () => voice.toggle())
  messages = [...messages, reply('new', 'New response', true)]; processing = true; await render()
  expect(requests.some(request => request.action === 'startSpeechOutput')).toBe(false)
  messages = [messages[0], reply('new', 'New response')]; processing = false; await render()
  const actions = requests.map(request => request.action)
  expect(actions.indexOf('stopDictation')).toBeLessThan(actions.indexOf('startSpeechOutput'))
  expect(requests.find(request => request.action === 'startSpeechOutput')?.payload).toMatchObject({ text: 'New response', language: 'en-GB', voiceId: 'installed', rate: 1.2, volume: 0.5 })
  expect(voice.speaking).toBe(true)
  await tick(); expect(voice.speaking).toBe(false)
  await tick(); expect(requests.filter(request => request.action === 'startDictation')).toHaveLength(2)
  await render(); expect(requests.filter(request => request.action === 'startSpeechOutput')).toHaveLength(1)
})

it('stops owned playback and rejects late input after context changes', async () => {
  await render(); await act(async () => voice.toggle())
  const oldInput = requests[0].payload.dictationId
  await act(async () => voice.read('Read this'))
  const ownedSpeech = requests.find(request => request.action === 'startSpeechOutput')?.payload.speechId
  contextKey = 'different-chat'; await render()
  expect(requests.find(request => request.action === 'stopSpeechOutput')?.payload.speechId).toBe(ownedSpeech)
  await push('final', 'wrong chat', oldInput); await tick()
  expect(append).not.toHaveBeenCalled(); expect(voice.active).toBe(false); expect(voice.speaking).toBe(false)
})

it('stops on permission errors and offline events without an automatic retry loop', async () => {
  await render(); await act(async () => voice.toggle())
  await push('error', 'Microphone permission denied')
  expect(voice.active).toBe(false); expect(voice.error).toBe('Microphone permission denied')
  await tick(); expect(requests.filter(request => request.action === 'startDictation')).toHaveLength(1)
  await act(async () => voice.toggle()); await act(async () => { window.dispatchEvent(new Event('offline')) })
  expect(voice.active).toBe(false); expect(voice.error).toContain('offline')
})

it('uses the configured activation shortcut and disables native voice on companion clients', async () => {
  await render()
  await act(async () => { window.dispatchEvent(new KeyboardEvent('keydown', { key: 'v', ctrlKey: true, shiftKey: true })) })
  expect(voice.active).toBe(true)
  await act(async () => voice.stop())
  window.history.replaceState({}, '', '/companion'); await render()
  await act(async () => voice.toggle()); await act(async () => voice.read('No playback'))
  expect(voice.available).toBe(false)
  expect(requests.filter(request => request.action === 'startDictation')).toHaveLength(1)
  expect(requests.filter(request => request.action === 'startSpeechOutput')).toHaveLength(0)
})

it('retains playback ownership when Stop fails, allowing an explicit retry', async () => {
  vi.spyOn(console, 'error').mockImplementation(() => {})
  const native = window.cefQuery!
  let rejectStop = true
  window.cefQuery = args => {
    if (JSON.parse(args.request).action === 'stopSpeechOutput' && rejectStop) {
      requests.push(JSON.parse(args.request)); rejectStop = false; args.onFailure(503, 'Playback stop failed'); return
    }
    native(args)
  }
  await render(); await act(async () => voice.read('Response'))
  const id = requests.find(request => request.action === 'startSpeechOutput')?.payload.speechId
  await act(async () => voice.stop())
  expect(voice.speaking).toBe(true); expect(voice.error).toBe('Playback stop failed')
  await act(async () => voice.stop())
  expect(voice.speaking).toBe(false)
  expect(requests.filter(request => request.action === 'stopSpeechOutput').map(request => request.payload.speechId)).toEqual([id, id])
})

it('cancels a late native start after the controller has stopped', async () => {
  let complete: (() => void) | undefined
  const native = window.cefQuery!
  window.cefQuery = args => {
    const parsed = JSON.parse(args.request)
    if (parsed.action === 'startDictation') { requests.push(parsed); complete = () => args.onSuccess(JSON.stringify({ started: true })); return }
    native(args)
  }
  await render(); await act(async () => voice.toggle()); await act(async () => voice.stop())
  await act(async () => complete?.()); await tick()
  expect(voice.active).toBe(false)
  expect(requests.filter(request => request.action === 'startDictation')).toHaveLength(1)
  expect(requests.filter(request => request.action === 'stopDictation').every(request => request.payload.dictationId === requests[0].payload.dictationId)).toBe(true)
})
