interface LogoProps {
  size?: number
  showText?: boolean
}

export function Logo({ size = 22, showText = true }: LogoProps) {
  return (
    <div className="flex items-center gap-2 select-none">
      <img
        src="./app_icon.png"
        width={size}
        height={size}
        style={{ flexShrink: 0, borderRadius: 4, display: 'block' }}
        alt="UAM"
        draggable={false}
      />
      {showText && (
        <span
          className="text-sm font-semibold"
          style={{ color: 'var(--text)' }}
        >
          Universal Agent Manager
        </span>
      )}
    </div>
  )
}
