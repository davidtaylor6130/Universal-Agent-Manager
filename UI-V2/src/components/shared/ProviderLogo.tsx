import type { CSSProperties } from 'react'
import codexLogo from '../../assets/provider-logos/codex.svg'
import claudeLogo from '../../assets/provider-logos/claude.svg'
import opencodeLogo from '../../assets/provider-logos/opencode.svg'
import {
  CLAUDE_CLI_PROVIDER_ID,
  CODEX_CLI_PROVIDER_ID,
  COPILOT_CLI_PROVIDER_ID,
  OPENCODE_CLI_PROVIDER_ID,
} from '../../utils/providerMetadata'

interface ProviderLogoProps {
  providerId?: string
  size?: number
  style?: CSSProperties
  className?: string
}

function GeminiMark() {
  return (
    <svg viewBox="0 0 24 24" aria-hidden="true" className="block h-full w-full">
      <path
        d="M12 2.5l1.85 5.65 5.65 1.85-5.65 1.85L12 17.5l-1.85-5.65-5.65-1.85 5.65-1.85L12 2.5z"
        fill="currentColor"
      />
      <path
        d="M17.2 14.7l.95 2.9 2.9.95-2.9.95-.95 2.9-.95-2.9-2.9-.95 2.9-.95.95-2.9z"
        fill="currentColor"
        opacity="0.78"
      />
    </svg>
  )
}

function CopilotMark() {
  return (
    <svg viewBox="0 0 24 24" aria-hidden="true" className="block h-full w-full">
      <path
        d="M6.2 9.6c.45-3.15 2.62-5.1 5.8-5.1s5.35 1.95 5.8 5.1c1.18.38 2 1.45 2 2.74v3.06c0 1.74-1.4 3.1-3.14 3.1h-.9c-.72 0-1.32-.5-1.46-1.16a8.1 8.1 0 0 1-4.6 0c-.14.66-.74 1.16-1.46 1.16h-.9A3.08 3.08 0 0 1 4.2 15.4v-3.06c0-1.29.82-2.36 2-2.74Zm1.66-.24c1.3-.14 2.84.18 4.14 1.06 1.3-.88 2.84-1.2 4.14-1.06-.5-2.02-1.94-3.2-4.14-3.2s-3.64 1.18-4.14 3.2Zm-.54 1.76c-.86 0-1.52.54-1.52 1.28v3c0 .84.66 1.5 1.54 1.5h.74v-3.16c0-.46.38-.84.84-.84.48 0 .86.38.86.84v1.96c1.36.68 3.08.68 4.44 0v-1.96c0-.46.38-.84.86-.84.46 0 .84.38.84.84v3.16h.74c.88 0 1.54-.66 1.54-1.5v-3c0-.74-.66-1.28-1.52-1.28-1.3 0-2.7.4-3.98 1.4a1.1 1.1 0 0 1-1.38 0c-1.28-1-2.68-1.4-3.98-1.4Z"
        fill="currentColor"
      />
    </svg>
  )
}

export function ProviderLogo({ providerId, size = 16, style, className }: ProviderLogoProps) {
  const codex = providerId === CODEX_CLI_PROVIDER_ID
  const claude = providerId === CLAUDE_CLI_PROVIDER_ID
  const opencode = providerId === OPENCODE_CLI_PROVIDER_ID
  const copilot = providerId === COPILOT_CLI_PROVIDER_ID
  const logoSrc = codex ? codexLogo : claude ? claudeLogo : opencode ? opencodeLogo : ''

  return (
    <span
      aria-hidden="true"
      className={className ?? 'inline-flex items-center justify-center shrink-0'}
      style={{
        width: size,
        height: size,
        ...style,
      }}
    >
      {logoSrc ? (
        <img
          src={logoSrc}
          alt=""
          className="block h-full w-full object-contain"
          draggable={false}
        />
      ) : (
        <span style={{ color: copilot ? '#22c55e' : '#8ab4ff' }} className="block h-full w-full">
          {copilot ? <CopilotMark /> : <GeminiMark />}
        </span>
      )}
    </span>
  )
}
