import type { CSSProperties } from 'react'
import codexLogo from '../../assets/provider-logos/codex.svg'
import claudeLogo from '../../assets/provider-logos/claude.svg'

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

export function ProviderLogo({ providerId, size = 16, style, className }: ProviderLogoProps) {
  const codex = providerId === 'codex-cli'
  const claude = providerId === 'claude-cli'
  const logoSrc = codex ? codexLogo : claude ? claudeLogo : ''

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
        <span style={{ color: '#8ab4ff' }} className="block h-full w-full">
          <GeminiMark />
        </span>
      )}
    </span>
  )
}
