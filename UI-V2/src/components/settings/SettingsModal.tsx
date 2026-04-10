import { useEffect } from 'react'
import { useAppStore } from '../../store/useAppStore'
import { ThemeToggle } from '../shared/ThemeToggle'
import { useTheme } from '../../hooks/useTheme'

export function SettingsModal() {
  const { setSettingsOpen } = useAppStore()
  const { theme } = useTheme()

  useEffect(() => {
    const handler = (e: KeyboardEvent) => {
      if (e.key === 'Escape') setSettingsOpen(false)
    }
    window.addEventListener('keydown', handler)
    return () => window.removeEventListener('keydown', handler)
  }, [setSettingsOpen])

  return (
    <div
      className="fixed inset-0 z-50 flex items-center justify-center animate-fade-in"
      style={{ background: 'rgba(0,0,0,0.55)', backdropFilter: 'blur(4px)' }}
      onClick={(e) => { if (e.target === e.currentTarget) setSettingsOpen(false) }}
    >
      <div
        className="rounded-xl shadow-2xl w-full max-w-sm mx-4 animate-slide-in"
        style={{
          background: 'var(--surface)',
          border: '1px solid var(--border-bright)',
        }}
      >
        {/* Header */}
        <div
          className="flex items-center justify-between px-5 py-4"
          style={{ borderBottom: '1px solid var(--border)' }}
        >
          <span className="text-sm font-semibold" style={{ color: 'var(--text)' }}>
            Settings
          </span>
          <button
            onClick={() => setSettingsOpen(false)}
            style={{
              background: 'transparent',
              color: 'var(--text-3)',
              border: 'none',
              cursor: 'pointer',
              fontFamily: 'inherit',
              fontSize: 12,
            }}
          >
            ✕
          </button>
        </div>

        <div className="p-5 space-y-4">
          {/* Theme row */}
          <div className="flex items-center justify-between">
            <div>
              <div className="text-sm" style={{ color: 'var(--text)' }}>
                Appearance
              </div>
              <div className="text-xs mt-0.5" style={{ color: 'var(--text-3)' }}>
                {theme === 'dark' ? 'Dark mode' : 'Light mode'} active
              </div>
            </div>
            <ThemeToggle />
          </div>

          {/* Divider */}
          <div style={{ borderTop: '1px solid var(--border)' }} />

          {/* Version info */}
          <div>
            <div className="text-xs" style={{ color: 'var(--text-3)' }}>UAM UI-V2</div>
            <div className="text-xs mt-0.5" style={{ color: 'var(--text-3)', opacity: 0.6 }}>
              Stage 1 — UI preview build
            </div>
          </div>
        </div>

        <div
          className="px-5 py-4"
          style={{ borderTop: '1px solid var(--border)' }}
        >
          <button
            onClick={() => setSettingsOpen(false)}
            className="w-full py-1.5 rounded-md text-xs font-medium transition-opacity duration-150"
            style={{
              background: 'var(--accent)',
              color: '#fff',
              border: 'none',
              cursor: 'pointer',
              fontFamily: 'inherit',
            }}
            onMouseEnter={(e) => { e.currentTarget.style.opacity = '0.88' }}
            onMouseLeave={(e) => { e.currentTarget.style.opacity = '1' }}
          >
            Close
          </button>
        </div>
      </div>
    </div>
  )
}
