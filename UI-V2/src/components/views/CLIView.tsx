import { useEffect, useRef, useState } from 'react'
import { useShallow } from 'zustand/react/shallow'
import { Terminal } from '@xterm/xterm'
import { FitAddon } from '@xterm/addon-fit'
import '@xterm/xterm/css/xterm.css'
import { AlertTriangle, X } from 'lucide-react'
import { Session } from '../../types/session'
import { useAppStore } from '../../store/useAppStore'
import { sendToCEF, isCefContext, createRequestId } from '../../ipc/cefBridge'
import { normalizeCliLifecycleState, cliLifecycleIsProcessing } from '../../store/cpp/reconcile'
import type { CliLifecycleState } from '../../store/useAppStore'
import { COPILOT_CLI_PROVIDER_ID, DEFAULT_PROVIDER_ID } from '../../utils/providerMetadata'
import { resolveDocumentTheme } from '../../utils/themeStorage'
import { useResolvedTheme } from '../../hooks/useTheme'

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
  pendingSteer?: boolean
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

function terminalTheme(isDark: boolean) {
  return {
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
  }
}

export function CLIView({ session }: CLIViewProps) {
  const terminalRef = useRef<HTMLDivElement>(null)
  const termInstanceRef = useRef<Terminal | null>(null)
  const fitAddonRef = useRef<FitAddon | null>(null)
  const theme = useAppStore((s) => s.theme)
  const customThemes = useAppStore((s) => s.customThemes)
  const resolvedTheme = useResolvedTheme(theme, customThemes)
  const providers = useAppStore((s) => s.providers)
  const copilotVersionStatus = useAppStore((s) =>
    s.cliVersionManager.providers.find((provider) => provider.providerId === COPILOT_CLI_PROVIDER_ID)?.status ?? 'unknown'
  )
  const cliBinding = useAppStore(useShallow((s) => {
    const binding = s.cliBindingBySessionId[session.id]
    return { terminalId: binding?.terminalId, running: binding?.running, lastError: binding?.lastError }
  }))
  const setCliBinding = useAppStore((s) => s.setCliBinding)
  const refreshCliProviderVersion = useAppStore((s) => s.refreshCliProviderVersion)
  const setSettingsOpen = useAppStore((s) => s.setSettingsOpen)
  const [terminalStartAttempt, setTerminalStartAttempt] = useState(0)
  const [dismissedTerminalError, setDismissedTerminalError] = useState('')
  const copilotCompatibilityRetryPendingRef = useRef(false)
  const currentProviderId = session.providerId?.trim() || DEFAULT_PROVIDER_ID
  const providerSupported = providers.some((provider) => provider.id === currentProviderId)
  const visibleTerminalError =
    cliBinding?.lastError && cliBinding.lastError !== dismissedTerminalError
      ? cliBinding.lastError
      : ''
  const unsupportedProviderMessage = `Provider '${currentProviderId}' is not supported in this build. Switch this chat to Gemini CLI to use terminal mode.`

  useEffect(() => {
    copilotCompatibilityRetryPendingRef.current = false
    setDismissedTerminalError('')
  }, [currentProviderId, session.id])

  useEffect(() => {
    const compatibilityCheckFinished =
      copilotVersionStatus !== 'unknown' && copilotVersionStatus !== 'checking' && copilotVersionStatus !== 'installing'
    if (compatibilityCheckFinished && copilotCompatibilityRetryPendingRef.current) {
      copilotCompatibilityRetryPendingRef.current = false
      setTerminalStartAttempt((attempt) => attempt + 1)
    }
  }, [copilotVersionStatus])

  useEffect(() => {
    if (!cliBinding?.lastError && dismissedTerminalError) {
      setDismissedTerminalError('')
    }
  }, [cliBinding?.lastError, dismissedTerminalError])

  useEffect(() => {
    if (!providerSupported) return
    if (!terminalRef.current) return

    // Replay once on attachment; live output reaches xterm through uam-cli-output.
    const state = useAppStore.getState()
    const cliTranscript = state.cliTranscriptBySessionId[session.id]
    const effectiveTerminalId = state.cliBindingBySessionId[session.id]?.terminalId || cliTranscript?.terminalId || ''
    const isDark = resolveDocumentTheme(state.theme, state.customThemes) === 'dark'

    const term = new Terminal({
      fontFamily: '"JetBrains Mono", monospace',
      fontSize: 13,
      lineHeight: 1,
      cursorBlink: true,
      cursorStyle: 'block',
      theme: terminalTheme(isDark),
      minimumContrastRatio: 4.5,
      allowTransparency: true,
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
        const attachmentId = createRequestId('terminal-view')
        let cancelled = false
        let resizeAnimationFrame: number | null = null
        let bestKnownTerminalId = effectiveTerminalId
        let bestKnownBoundChatId = session.id
        const retryCopilotAfterCompatibilityCheck = (error: string) => {
          if (
            currentProviderId !== COPILOT_CLI_PROVIDER_ID ||
            !error.startsWith('Checking GitHub Copilot CLI compatibility.')
          ) {
            return
          }
          copilotCompatibilityRetryPendingRef.current = true
          const status = useAppStore.getState().cliVersionManager.providers.find(
            (provider) => provider.providerId === COPILOT_CLI_PROVIDER_ID
          )?.status
          if (status !== 'unknown' && status !== 'checking' && status !== 'installing') {
            copilotCompatibilityRetryPendingRef.current = false
            setTerminalStartAttempt((attempt) => attempt + 1)
          }
        }

        const detachTerminal = (terminalId = bestKnownTerminalId, chatId = bestKnownBoundChatId) => {
          sendToCEF({
            action: 'stopCliTerminal',
            payload: {
              chatId,
              terminalId,
              attachmentId,
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
          const sourceChatMatch = sourceChatId === session.id
          const terminalMatch = Boolean(binding?.terminalId) && Boolean(terminalId) && binding?.terminalId === terminalId

          if (!cancelled && (sessionMatch || sourceChatMatch || terminalMatch) && data) {
            term.write(binaryStringToUint8Array(data))
          }
        }
        window.addEventListener('uam-cli-output', onCliOutput)

      // Production path — request the C++ backend to start/attach a PTY.
      sendToCEF<StartCliTerminalResponse>({
        action: 'startCliTerminal',
        payload: {
          chatId: session.id,
          terminalId: effectiveTerminalId,
          attachmentId,
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
            retryCopilotAfterCompatibilityCheck(resp.error ?? '')
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
            retryCopilotAfterCompatibilityCheck(data.lastError ?? '')
            const lifecycleState = normalizeCliLifecycleState(data.lifecycleState, running, data.turnState)
            const processing = cliLifecycleIsProcessing(lifecycleState)
          setCliBinding(session.id, {
            terminalId: data.terminalId ?? cliBinding?.terminalId ?? '',
            boundChatId: data.sourceChatId ?? session.id,
            running,
            lifecycleState,
            processing,
            active: lifecycleState === 'idle' && running,
            pendingSteer: Boolean(data.pendingSteer),
            turnState: lifecycleState === 'unknown' ? 'unknown' : processing ? 'busy' : 'idle',
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
          void sendToCEF({
            action: 'writeCliInput',
            payload: { chatId: binding?.boundChatId ?? session.id, terminalId: binding?.terminalId ?? '', data },
          }).then((response) => {
            if (!cancelled && !response.ok && useAppStore.getState().cliBindingBySessionId[session.id]?.terminalId === binding?.terminalId) {
              setCliBinding(session.id, { lastError: response.error || 'Terminal input could not be delivered.' })
            }
          })
        })

        // xterm emits only when the character grid changes, including refits outside the observer.
        const onResize = term.onResize(({ rows, cols }) => {
          if (cancelled) return
          const binding = useAppStore.getState().cliBindingBySessionId[session.id]
          void sendToCEF({
            action: 'resizeCliTerminal',
            payload: {
              chatId: binding?.boundChatId ?? session.id,
              terminalId: binding?.terminalId ?? '',
              rows,
              cols,
            },
          })
        })
        const resizeObserver = new ResizeObserver(() => {
          if (resizeAnimationFrame !== null) cancelAnimationFrame(resizeAnimationFrame)
          resizeAnimationFrame = requestAnimationFrame(() => {
            resizeAnimationFrame = null
            if (!cancelled) fitAddon.fit()
          })
        })
        if (terminalRef.current) resizeObserver.observe(terminalRef.current)

        return () => {
          cancelled = true
          onData.dispose()
          onResize.dispose()
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

    term.writeln('Open this chat in the desktop app to use the provider terminal.')
    return () => {
      termInstanceRef.current = null
      fitAddonRef.current = null
      term.dispose()
    }
  }, [currentProviderId, providerSupported, session.id, terminalStartAttempt]) // Re-init per session, provider, support, and explicit retry

  // Refit and recolor the live terminal on theme change.
  useEffect(() => {
    if (termInstanceRef.current?.options) {
      termInstanceRef.current.options.theme = terminalTheme(resolvedTheme === 'dark')
    }
    requestAnimationFrame(() => fitAddonRef.current?.fit())
  }, [resolvedTheme])

  if (!providerSupported) {
    return (
      <div className="flex flex-col h-full overflow-hidden">
        <div
          className="mx-4 mt-4 flex items-start gap-3 rounded-md border px-3 py-2 text-xs"
          style={{
            borderColor: 'color-mix(in srgb, var(--yellow) 45%, var(--border))',
            background: 'color-mix(in srgb, var(--yellow) 10%, var(--surface))',
            color: 'var(--text)',
          }}
        >
          <AlertTriangle size={14} style={{ flexShrink: 0, marginTop: 2, color: 'var(--yellow)' }} aria-hidden />
          <span>{unsupportedProviderMessage}</span>
        </div>
      </div>
    )
  }

  return (
    <div className="flex flex-col h-full overflow-hidden">
      {!!visibleTerminalError && (
        <div
          className="flex-shrink-0 px-3 py-2 text-xs"
          style={{
            borderBottom: '1px solid var(--border)',
            background: 'rgba(239, 68, 68, 0.10)',
            color: 'var(--red)',
          }}
        >
          <div className="flex items-start gap-2">
            <span className="min-w-0 flex-1">{visibleTerminalError}</span>
            {!cliBinding?.running && (
              <button
                type="button"
                aria-label="Retry terminal start"
                onClick={() => {
                  setDismissedTerminalError('')
                  setCliBinding(session.id, { lastError: '' })
                  setTerminalStartAttempt((attempt) => attempt + 1)
                }}
                className="rounded px-2 py-0.5 font-medium"
                style={{ color: 'inherit', border: '1px solid currentColor' }}
              >
                Retry
              </button>
            )}
            <button
              type="button"
              aria-label="Check provider CLI"
              onClick={() => void refreshCliProviderVersion(currentProviderId)}
              className="rounded px-2 py-0.5 font-medium"
              style={{ color: 'inherit', border: '1px solid currentColor' }}
            >
              Check CLI
            </button>
            <button
              type="button"
              aria-label="Open CLI settings"
              onClick={() => setSettingsOpen(true)}
              className="rounded px-2 py-0.5 font-medium"
              style={{ color: 'inherit', border: '1px solid currentColor' }}
            >
              Settings
            </button>
            <button
              type="button"
              aria-label="Dismiss terminal error"
              title="Dismiss"
              onClick={() => setDismissedTerminalError(visibleTerminalError)}
              className="inline-flex h-5 w-5 flex-shrink-0 items-center justify-center rounded"
              style={{ color: 'inherit' }}
            >
              <X size={13} aria-hidden />
            </button>
          </div>
        </div>
      )}

      {/* Terminal area */}
      <div
        className="flex-1 overflow-hidden"
        style={{ background: 'var(--term-bg)', padding: '12px 18px 18px' }}
      >
        <div ref={terminalRef} className="h-full" />
      </div>

    </div>
  )
}
