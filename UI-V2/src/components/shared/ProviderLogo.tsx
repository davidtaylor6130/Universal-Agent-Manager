import type { CSSProperties } from 'react'
import { Bot } from 'lucide-react'
import codexLogo from '../../assets/provider-logos/codex.svg'
import claudeLogo from '../../assets/provider-logos/claude.svg'
import copilotLogo from '../../assets/provider-logos/copilot.svg'
import opencodeLogo from '../../assets/provider-logos/opencode.svg'
import antigravityLogo from '../../assets/provider-logos/antigravity.svg'
import geminiLogo from '../../assets/provider-logos/gemini-cli.png'
import {
  ANTIGRAVITY_CLI_PROVIDER_ID,
  CLAUDE_CLI_PROVIDER_ID,
  CODEX_CLI_PROVIDER_ID,
  COPILOT_CLI_PROVIDER_ID,
  DEFAULT_PROVIDER_ID,
  OPENCODE_CLI_PROVIDER_ID,
} from '../../utils/providerMetadata'

interface ProviderLogoProps {
  providerId?: string
  size?: number
  style?: CSSProperties
  className?: string
}

export function ProviderLogo({ providerId, size = 16, style, className }: ProviderLogoProps) {
  const codex = providerId === CODEX_CLI_PROVIDER_ID
  const claude = providerId === CLAUDE_CLI_PROVIDER_ID
  const opencode = providerId === OPENCODE_CLI_PROVIDER_ID
  const copilot = providerId === COPILOT_CLI_PROVIDER_ID
  const gemini = !providerId || providerId === DEFAULT_PROVIDER_ID
  const logoSrc = codex ? codexLogo : claude ? claudeLogo : opencode ? opencodeLogo : copilot ? copilotLogo
    : providerId === ANTIGRAVITY_CLI_PROVIDER_ID ? antigravityLogo : gemini ? geminiLogo : ''

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
          className={`block h-full w-full object-contain${codex ? ' uam-provider-logo--codex' : copilot ? ' uam-provider-logo--copilot' : ''}`}
          draggable={false}
        />
      ) : (
        <Bot aria-hidden="true" className="h-full w-full" style={{ color: 'var(--text-3)' }} />
      )}
    </span>
  )
}
