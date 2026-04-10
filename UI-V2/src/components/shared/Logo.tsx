interface LogoProps {
  size?: number
  showText?: boolean
}

export function Logo({ size = 22, showText = true }: LogoProps) {
  return (
    <div className="flex items-center gap-2 select-none">
      {/* Icon mark */}
      <svg
        width={size}
        height={size}
        viewBox="0 0 24 24"
        fill="none"
        xmlns="http://www.w3.org/2000/svg"
        style={{ flexShrink: 0 }}
      >
        {/* Outer ring */}
        <circle
          cx="12"
          cy="12"
          r="10.5"
          stroke="var(--accent)"
          strokeWidth="1.5"
          opacity="0.8"
        />
        {/* Inner cross-hairs */}
        <circle
          cx="12"
          cy="12"
          r="3.5"
          fill="var(--accent)"
        />
        {/* Top node */}
        <circle cx="12" cy="3.5" r="1.5" fill="var(--blue)" />
        {/* Right node */}
        <circle cx="20.5" cy="12" r="1.5" fill="var(--blue)" />
        {/* Bottom node */}
        <circle cx="12" cy="20.5" r="1.5" fill="var(--blue)" />
        {/* Left node */}
        <circle cx="3.5" cy="12" r="1.5" fill="var(--blue)" />
        {/* Connector lines */}
        <line x1="12" y1="5" x2="12" y2="8.5" stroke="var(--accent)" strokeWidth="1" opacity="0.5" />
        <line x1="19" y1="12" x2="15.5" y2="12" stroke="var(--accent)" strokeWidth="1" opacity="0.5" />
        <line x1="12" y1="19" x2="12" y2="15.5" stroke="var(--accent)" strokeWidth="1" opacity="0.5" />
        <line x1="5" y1="12" x2="8.5" y2="12" stroke="var(--accent)" strokeWidth="1" opacity="0.5" />
      </svg>

      {showText && (
        <span
          className="text-sm font-semibold tracking-wider"
          style={{ color: 'var(--text)', letterSpacing: '0.12em' }}
        >
          UAM
        </span>
      )}
    </div>
  )
}
