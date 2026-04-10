import { useState, useEffect } from 'react'
import { Session } from '../../types/session'
import { useAppStore } from '../../store/useAppStore'
import { AgentStep } from '../../mock/mockData'
import { ProviderChiplets } from '../input/ProviderChiplets'
import { InputBar } from '../input/InputBar'
import { StreamingCursor } from '../chat/StreamingCursor'

interface CodingAgentViewProps {
  session: Session
}

type StepStatus = AgentStep['status']

const STATUS_ICON: Record<StepStatus, React.ReactNode> = {
  done:    <span style={{ color: 'var(--green)', fontSize: 12 }}>✓</span>,
  running: <span style={{ color: 'var(--accent)', fontSize: 12 }}>▶</span>,
  pending: <span style={{ color: 'var(--text-3)', fontSize: 12 }}>○</span>,
  error:   <span style={{ color: 'var(--red)', fontSize: 12 }}>✕</span>,
}

const STATUS_COLOR: Record<StepStatus, string> = {
  done:    'var(--text-2)',
  running: 'var(--text)',
  pending: 'var(--text-3)',
  error:   'var(--red)',
}

function StepRow({ step, index }: { step: AgentStep; index: number }) {
  const [expanded, setExpanded] = useState(step.status === 'running')

  return (
    <div
      className="animate-step-appear"
      style={{ animationDelay: `${index * 60}ms`, animationFillMode: 'both' }}
    >
      <div
        className="flex items-start gap-2.5 px-4 py-2 rounded-md mx-2 transition-colors duration-100"
        style={{
          background: step.status === 'running' ? 'var(--accent-dim)' : 'transparent',
          border: step.status === 'running' ? '1px solid var(--accent)20' : '1px solid transparent',
          cursor: step.output ? 'pointer' : 'default',
        }}
        onClick={() => step.output && setExpanded((v) => !v)}
      >
        {/* Status icon */}
        <div className="flex-shrink-0 mt-0.5 w-4 text-center">
          {step.status === 'running' ? (
            <span style={{ color: 'var(--accent)', fontSize: 12, animation: 'blink 1.2s ease-in-out infinite' }}>▶</span>
          ) : (
            STATUS_ICON[step.status]
          )}
        </div>

        {/* Content */}
        <div className="flex-1 min-w-0">
          <div className="flex items-center gap-2">
            <span
              className="text-sm"
              style={{
                color: STATUS_COLOR[step.status],
                fontWeight: step.status === 'running' ? 500 : 400,
              }}
            >
              {step.label}
            </span>
            {step.status === 'running' && <StreamingCursor />}
            {step.durationMs && step.status === 'done' && (
              <span className="text-xs" style={{ color: 'var(--text-3)', marginLeft: 'auto' }}>
                {step.durationMs}ms
              </span>
            )}
          </div>

          {/* Output — shown when expanded */}
          {step.output && expanded && (
            <div
              className="mt-1.5 text-xs rounded p-2"
              style={{
                background: 'var(--surface-high)',
                color: 'var(--text-2)',
                fontFamily: 'inherit',
                lineHeight: 1.6,
                whiteSpace: 'pre-wrap',
              }}
            >
              {step.output}
            </div>
          )}

          {/* Expand hint */}
          {step.output && !expanded && (
            <div className="text-xs mt-0.5" style={{ color: 'var(--text-3)' }}>
              Click to expand
            </div>
          )}
        </div>
      </div>
    </div>
  )
}

export function CodingAgentView({ session }: CodingAgentViewProps) {
  const { agentSteps, sendMessage } = useAppStore()
  const steps = agentSteps[session.id] ?? []
  const [agentMessage, setAgentMessage] = useState('')
  const isRunning = steps.some((s) => s.status === 'running')

  // Mock "agent is typing a summary" message
  useEffect(() => {
    if (steps.length === 0) return
    const fullMsg = `Working on: **${session.name}**\n\nI've analyzed the codebase and identified the areas that need refactoring. I'm currently rewriting the token storage layer to use HttpOnly cookies instead of localStorage.`
    setAgentMessage(fullMsg)
  }, [session.id, session.name, steps.length])

  const doneCount = steps.filter((s) => s.status === 'done').length
  const totalCount = steps.length

  return (
    <div className="flex flex-col h-full overflow-hidden">
      {/* Provider chiplets — above content for coding agent */}
      <div
        className="flex-shrink-0 px-4 py-2"
        style={{ borderBottom: '1px solid var(--border)', background: 'var(--surface)' }}
      >
        <ProviderChiplets sessionId={session.id} />
      </div>

      {/* Main scroll area */}
      <div className="flex-1 overflow-y-auto py-3">
        {/* Agent message / summary */}
        {agentMessage && (
          <div className="mx-4 mb-4 p-3 rounded-lg text-sm" style={{
            background: 'var(--surface-up)',
            border: '1px solid var(--border)',
            color: 'var(--text-2)',
            lineHeight: 1.6,
          }}>
            <div className="flex items-center gap-2 mb-2">
              <span style={{ color: 'var(--accent)', fontSize: 11 }}>◈</span>
              <span className="text-xs font-medium" style={{ color: 'var(--accent)' }}>Agent</span>
              {isRunning && (
                <span className="text-xs ml-auto" style={{ color: 'var(--text-3)' }}>
                  {doneCount}/{totalCount} steps
                </span>
              )}
            </div>
            <div style={{ whiteSpace: 'pre-wrap' }}>{agentMessage}</div>
          </div>
        )}

        {/* Steps */}
        {steps.length > 0 ? (
          <div className="space-y-0.5">
            <div className="px-4 mb-2">
              <div className="flex items-center gap-2">
                <span className="text-xs font-medium uppercase tracking-wider" style={{ color: 'var(--text-3)', fontSize: 10, letterSpacing: '0.08em' }}>
                  Execution plan
                </span>
                <div className="flex-1" style={{ borderTop: '1px solid var(--border)' }} />
                <span className="text-xs" style={{ color: 'var(--text-3)' }}>
                  {doneCount}/{totalCount}
                </span>
              </div>
            </div>
            {steps.map((step, i) => (
              <StepRow key={step.id} step={step} index={i} />
            ))}
          </div>
        ) : (
          <div className="flex flex-col items-center justify-center h-full" style={{ color: 'var(--text-3)' }}>
            <div style={{ fontSize: 28, opacity: 0.2, marginBottom: 10 }}>⚙</div>
            <div className="text-sm">Ready to start</div>
            <div className="text-xs mt-1" style={{ opacity: 0.6 }}>
              Describe what you want the agent to do
            </div>
          </div>
        )}
      </div>

      {/* Input bar */}
      <InputBar
        onSend={(content) => sendMessage(session.id, content)}
        disabled={isRunning}
        placeholder={isRunning ? 'Agent is running…' : 'Instruct the agent…'}
      />
    </div>
  )
}
