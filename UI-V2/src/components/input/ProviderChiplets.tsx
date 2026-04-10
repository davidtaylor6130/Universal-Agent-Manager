import { useAppStore } from '../../store/useAppStore'

interface ProviderChipletsProps {
  sessionId: string
}

export function ProviderChiplets({ sessionId }: ProviderChipletsProps) {
  const { providers, features, activeProviderId, setActiveProvider, toggleFeature } =
    useAppStore()

  const currentProviderId = activeProviderId[sessionId] ?? providers[0]?.id
  const currentProvider = providers.find((p) => p.id === currentProviderId) ?? providers[0]

  return (
    <div
      className="flex items-center gap-1.5 flex-wrap"
      style={{ minHeight: 26 }}
    >
      {/* Provider selector */}
      <div
        className="relative"
        style={{ display: 'inline-block' }}
      >
        <select
          value={currentProviderId}
          onChange={(e) => setActiveProvider(sessionId, e.target.value)}
          className="appearance-none text-xs rounded-full pl-2.5 pr-6 py-1 transition-all duration-150 cursor-pointer outline-none"
          style={{
            background: `${currentProvider?.color ?? 'var(--accent)'}18`,
            border: `1px solid ${currentProvider?.color ?? 'var(--accent)'}44`,
            color: currentProvider?.color ?? 'var(--accent)',
            fontFamily: 'inherit',
            fontWeight: 500,
          }}
        >
          {providers.map((p) => (
            <option key={p.id} value={p.id} style={{ background: 'var(--surface-up)', color: 'var(--text)' }}>
              {p.shortName}
            </option>
          ))}
        </select>
        {/* Dropdown arrow */}
        <span
          className="pointer-events-none absolute right-2 top-1/2 -translate-y-1/2 text-xs"
          style={{ color: currentProvider?.color ?? 'var(--accent)', fontSize: 8 }}
        >
          ▾
        </span>
      </div>

      {/* Divider */}
      <div
        className="flex-shrink-0 self-stretch"
        style={{ width: 1, background: 'var(--border)' }}
      />

      {/* Feature chiplets */}
      {features.map((feat) => (
        <button
          key={feat.id}
          onClick={() => toggleFeature(feat.id)}
          title={feat.label}
          className="flex items-center gap-1 text-xs rounded-full px-2.5 py-1 transition-all duration-150"
          style={{
            background: feat.enabled ? 'var(--accent-dim)' : 'transparent',
            border: feat.enabled ? '1px solid var(--accent)' : '1px solid var(--border)',
            color: feat.enabled ? 'var(--accent)' : 'var(--text-3)',
            cursor: 'pointer',
            fontFamily: 'inherit',
            fontWeight: feat.enabled ? 500 : 400,
          }}
          onMouseEnter={(e) => {
            if (!feat.enabled) {
              e.currentTarget.style.borderColor = 'var(--border-bright)'
              e.currentTarget.style.color = 'var(--text-2)'
            }
          }}
          onMouseLeave={(e) => {
            if (!feat.enabled) {
              e.currentTarget.style.borderColor = 'var(--border)'
              e.currentTarget.style.color = 'var(--text-3)'
            }
          }}
        >
          <span style={{ fontSize: 9 }}>{feat.icon}</span>
          {feat.label}
        </button>
      ))}
    </div>
  )
}
