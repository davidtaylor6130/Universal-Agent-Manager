import { useEffect, useRef } from 'react'
import { Terminal } from '@xterm/xterm'
import { FitAddon } from '@xterm/addon-fit'
import '@xterm/xterm/css/xterm.css'
import { Session } from '../../types/session'
import { useAppStore } from '../../store/useAppStore'
import { sendToCEF, isCefContext } from '../../ipc/cefBridge'
import type { CliLifecycleState } from '../../store/useAppStore'

interface CLIViewProps {
  session: Session
}

interface StartCliTerminalResponse {
  terminalId?: string
  sessionId?: string
  sourceChatId?: string
  running?: boolean
  lifecycleState?: CliLifecycleState | string
  turnState?: 'idle' | 'busy' | string
  lastError?: string
  replayData?: string
}

function binaryStringToUint8Array(data: string): Uint8Array {
  return Uint8Array.from(data, (char) => char.charCodeAt(0))
}

function decodeReplayData(data: string): Uint8Array | string {
  try {
    return binaryStringToUint8Array(atob(data))
  } catch {
    return data
  }
}

function normalizeLifecycleState(
  value: unknown,
  running: boolean,
  turnState?: string
): CliLifecycleState {
  if (
    value === 'disabled' ||
    value === 'stopped' ||
    value === 'idle' ||
    value === 'busy' ||
    value === 'shuttingDown'
  ) {
    return value
  }

  if (!running) return 'stopped'
  return turnState === 'busy' ? 'busy' : 'idle'
}

function lifecycleIsProcessing(lifecycleState: CliLifecycleState): boolean {
  return lifecycleState === 'busy' || lifecycleState === 'shuttingDown'
}

// Mock data for dev mode only
const MOCK_WELCOME = [
  '\x1b[38;5;208m┌─────────────────────────────────────────┐\x1b[0m',
  '\x1b[38;5;208m│\x1b[0m  UAM CLI — Universal Agent Manager      \x1b[38;5;208m│\x1b[0m',
  '\x1b[38;5;208m│\x1b[0m  Connected to C++ backend via CEF       \x1b[38;5;208m│\x1b[0m',
  '\x1b[38;5;208m└─────────────────────────────────────────┘\x1b[0m',
  '',
  '\x1b[38;5;245mSession:\x1b[0m ' + '\x1b[38;5;69m{SESSION_NAME}\x1b[0m',
  '',
]

export function CLIView({ session }: CLIViewProps) {
  const terminalRef = useRef<HTMLDivElement>(null)
  const termInstanceRef = useRef<Terminal | null>(null)
  const fitAddonRef = useRef<FitAddon | null>(null)
  const theme = useAppStore((s) => s.theme)
  const providers = useAppStore((s) => s.providers)
  const cliBindingBySessionId = useAppStore((s) => s.cliBindingBySessionId)
  const cliTranscriptBySessionId = useAppStore((s) => s.cliTranscriptBySessionId)
  const setCliBinding = useAppStore((s) => s.setCliBinding)
  const cliBinding = cliBindingBySessionId[session.id]
  const cliTranscript = cliTranscriptBySessionId[session.id]
  const currentProviderId = session.providerId?.trim() || 'gemini-cli'
  const providerSupported = providers.some((provider) => provider.id === currentProviderId)
  const unsupportedProviderMessage = `Provider '${currentProviderId}' is not supported in this build. Switch this chat to Gemini CLI to use terminal mode.`

  useEffect(() => {
    if (!providerSupported) return
    if (!terminalRef.current) return

    const isDark = document.documentElement.getAttribute('data-theme') !== 'light'

    const term = new Terminal({
      fontFamily: '"JetBrains Mono", monospace',
      fontSize: 13,
      lineHeight: 1.5,
      cursorBlink: true,
      cursorStyle: 'block',
      theme: {
        background:   isDark ? '#0b0b0e' : '#f0f0f5',
        foreground:   isDark ? '#e6e6ef' : '#111118',
        cursor:       '#f97316',
        cursorAccent: isDark ? '#0b0b0e' : '#f0f0f5',
        selectionBackground: 'rgba(249,115,22,0.30)',
        black:        isDark ? '#1a1a24' : '#2a2a3a',
        red:          '#f87171',
        green:        isDark ? '#4ade80' : '#16a34a',
        yellow:       '#facc15',
        blue:         '#60a5fa',
        magenta:      '#c084fc',
        cyan:         '#22d3ee',
        white:        isDark ? '#e6e6ef' : '#111118',
        brightBlack:  '#6b6b88',
        brightRed:    '#fb923c',
        brightGreen:  isDark ? '#86efac' : '#15803d',
        brightYellow: '#fde047',
        brightBlue:   '#93c5fd',
        brightMagenta:'#d8b4fe',
        brightCyan:   '#67e8f9',
        brightWhite:  isDark ? '#ffffff' : '#000000',
      },
      allowTransparency: true,
      convertEol: true,
      scrollback: 5000,
    })

    const fitAddon = new FitAddon()
    term.loadAddon(fitAddon)
    term.open(terminalRef.current)
    fitAddon.fit()

    if (cliTranscript?.content) {
      term.write(binaryStringToUint8Array(cliTranscript.content))
    }

	    termInstanceRef.current = term
	    fitAddonRef.current = fitAddon

	    if (isCefContext()) {
	      let cancelled = false
	      let resizeAnimationFrame: number | null = null
	      let bestKnownTerminalId = cliBinding?.terminalId ?? cliTranscript?.terminalId ?? ''
	      let bestKnownBoundChatId = session.id

	      const detachTerminal = (terminalId = bestKnownTerminalId, chatId = bestKnownBoundChatId) => {
	        sendToCEF({
	          action: 'stopCliTerminal',
	          payload: {
	            chatId,
	            terminalId,
	          },
	        }).catch((e) => console.error('[CEF] stopCliTerminal error:', e))
	      }

	      // Receive PTY output pushed from C++ via window.uamPush → CustomEvent.
	      const onCliOutput = (e: Event) => {
	        const { sessionId, sourceChatId, terminalId, data } = (e as CustomEvent<{
	          sessionId?: string
          sourceChatId?: string
          terminalId?: string
          data: string
        }>).detail
        const binding = useAppStore.getState().cliBindingBySessionId[session.id]
	        const sessionMatch = sessionId === session.id
	        const terminalMatch = Boolean(binding?.terminalId) && Boolean(terminalId) && binding?.terminalId === terminalId

	        if (!cancelled && (sessionMatch || terminalMatch) && data) {
	          term.write(binaryStringToUint8Array(data))
	        }
	      }
	      window.addEventListener('uam-cli-output', onCliOutput)

      // Production path — request the C++ backend to start/attach a PTY.
      sendToCEF<StartCliTerminalResponse>({
        action: 'startCliTerminal',
        payload: {
          chatId: session.id,
          terminalId: cliBinding?.terminalId ?? cliTranscript?.terminalId ?? '',
          rows: term.rows,
          cols: term.cols,
	        },
	      }).then((resp) => {
	        const data = resp.ok ? resp.data : undefined
	        const returnedTerminalId = data?.terminalId ?? ''
	        const returnedBoundChatId = data?.sourceChatId ?? session.id
	        if (returnedTerminalId) {
	          bestKnownTerminalId = returnedTerminalId
	        }
	        bestKnownBoundChatId = returnedBoundChatId

	        if (cancelled) {
	          if (returnedTerminalId || bestKnownTerminalId) {
	            detachTerminal(returnedTerminalId || bestKnownTerminalId, returnedBoundChatId)
	          }
	          return
	        }

	        if (!resp.ok) {
	          setCliBinding(session.id, {
	            running: false,
            processing: false,
            active: false,
            lifecycleState: 'stopped',
            turnState: 'idle',
            lastError: resp.error ?? 'Failed to start provider terminal.',
	          })
	          return
	        }

	        if (data) {
	          const running = Boolean(data.running)
	          const lifecycleState = normalizeLifecycleState(data.lifecycleState, running, data.turnState)
	          const processing = lifecycleIsProcessing(lifecycleState)
          setCliBinding(session.id, {
            terminalId: data.terminalId ?? cliBinding?.terminalId ?? '',
            boundChatId: data.sourceChatId ?? session.id,
            running,
            lifecycleState,
            processing,
            active: lifecycleState === 'idle' && running,
            turnState: processing ? 'busy' : 'idle',
            lastError: data.lastError ?? '',
          })

          if (!cliTranscript?.content && data.replayData) {
            term.write(decodeReplayData(data.replayData))
          }
        }
	      }).catch((e) => {
	        console.error('[CEF] startCliTerminal error:', e)
	        if (cancelled) {
	          return
	        }
	        setCliBinding(session.id, {
	          running: false,
          processing: false,
          active: false,
          lifecycleState: 'stopped',
          turnState: 'idle',
          lastError: 'Failed to start provider terminal.',
        })
      })

	      // Forward xterm.js keystrokes → C++ PTY via cefQuery.
	      const onData = term.onData((data) => {
	        if (cancelled) return
	        const binding = useAppStore.getState().cliBindingBySessionId[session.id]
	        sendToCEF({
	          action: 'writeCliInput',
          payload: { chatId: binding?.boundChatId ?? session.id, terminalId: binding?.terminalId ?? '', data },
        })
      })

	      // Resize observer — notify C++ of terminal dimension changes.
	      const resizeObserver = new ResizeObserver(() => {
	        if (resizeAnimationFrame !== null) {
	          cancelAnimationFrame(resizeAnimationFrame)
	        }
	        resizeAnimationFrame = requestAnimationFrame(() => {
	          resizeAnimationFrame = null
	          if (cancelled) return
	          fitAddon.fit()
	          const binding = useAppStore.getState().cliBindingBySessionId[session.id]
	          sendToCEF({
            action: 'resizeCliTerminal',
            payload: {
              chatId: binding?.boundChatId ?? session.id,
              terminalId: binding?.terminalId ?? '',
              rows: term.rows,
              cols: term.cols,
            },
          }).catch((e) => console.error('[CEF] resizeCliTerminal error:', e))
        })
      })
	      if (terminalRef.current) resizeObserver.observe(terminalRef.current)

	      return () => {
	        cancelled = true
	        onData.dispose()
	        window.removeEventListener('uam-cli-output', onCliOutput)
	        resizeObserver.disconnect()
	        if (resizeAnimationFrame !== null) {
	          cancelAnimationFrame(resizeAnimationFrame)
	          resizeAnimationFrame = null
	        }
	        const binding = useAppStore.getState().cliBindingBySessionId[session.id]
	        bestKnownTerminalId = binding?.terminalId ?? bestKnownTerminalId
	        bestKnownBoundChatId = binding?.boundChatId ?? bestKnownBoundChatId
	        detachTerminal()
	        if (termInstanceRef.current === term) {
	          termInstanceRef.current = null
	        }
	        if (fitAddonRef.current === fitAddon) {
	          fitAddonRef.current = null
	        }
	        term.dispose()
	      }
	    }

    // ----- Dev/mock path -----
    const welcome = MOCK_WELCOME.map((line) =>
      line.replace('{SESSION_NAME}', session.name)
    )
    welcome.forEach((line) => term.writeln(line))

    let lineBuffer = ''
    const MOCK_OUTPUTS: Record<string, string> = {
      ls: '\x1b[34mbin\x1b[0m  \x1b[34mbuild\x1b[0m  \x1b[32mCMakeLists.txt\x1b[0m  \x1b[32mREADME.md\x1b[0m  \x1b[34msrc\x1b[0m  \x1b[34mtests\x1b[0m',
      pwd: '/Users/user/projects/uam',
      'uname -a': 'Darwin macbook.local 25.3.0 Darwin Kernel Version 25.3.0',
      help: '\x1b[38;5;208mAvailable: \x1b[0mls, pwd, uname, clear, help',
      clear: '\x1b[2J\x1b[H',
    }

    term.onData((data) => {
      if (data === '\r') {
        term.write('\r\n')
        const cmd = lineBuffer.trim()
        lineBuffer = ''
        if (cmd) {
          const out = MOCK_OUTPUTS[cmd]
          if (out === '\x1b[2J\x1b[H') {
            term.write(out)
          } else if (out) {
            term.writeln(out)
          } else {
            term.writeln(`\x1b[31mbash: ${cmd}: command not found\x1b[0m`)
          }
        }
        term.write('\x1b[38;5;245m$ \x1b[0m')
      } else if (data === '\x7f') {
        if (lineBuffer.length > 0) {
          lineBuffer = lineBuffer.slice(0, -1)
          term.write('\b \b')
        }
      } else if (data >= ' ') {
        lineBuffer += data
        term.write(data)
      }
    })

    const resizeObserver = new ResizeObserver(() => {
      requestAnimationFrame(() => fitAddon.fit())
    })
    if (terminalRef.current) resizeObserver.observe(terminalRef.current)

    return () => {
      resizeObserver.disconnect()
      term.dispose()
    }
  }, [cliBinding?.terminalId, cliTranscript?.content, providerSupported, session.id]) // Re-init per session and provider support

  // Refit on theme change
  useEffect(() => {
    requestAnimationFrame(() => fitAddonRef.current?.fit())
  }, [theme])

  if (!providerSupported) {
    return (
      <div className="flex flex-col h-full overflow-hidden">
        <div
          className="mx-4 mt-4 rounded-md border px-3 py-2 text-xs"
          style={{
            borderColor: 'color-mix(in srgb, var(--yellow) 45%, var(--border))',
            background: 'color-mix(in srgb, var(--yellow) 10%, var(--surface))',
            color: 'var(--text)',
          }}
        >
          {unsupportedProviderMessage}
        </div>
      </div>
    )
  }

  return (
    <div className="flex flex-col h-full overflow-hidden">
      {!!cliBinding?.lastError && (
        <div
          className="flex-shrink-0 px-3 py-2 text-xs"
          style={{
            borderBottom: '1px solid var(--border)',
            background: 'rgba(239, 68, 68, 0.10)',
            color: 'var(--red)',
          }}
        >
          {cliBinding.lastError}
        </div>
      )}

      {/* Terminal area */}
      <div
        className="flex-1 overflow-hidden"
        style={{ background: 'var(--term-bg)', padding: '8px 4px 4px' }}
      >
        <div ref={terminalRef} className="h-full" />
      </div>
    </div>
  )
}
