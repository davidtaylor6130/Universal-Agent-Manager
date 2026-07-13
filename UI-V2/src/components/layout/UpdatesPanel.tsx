import { ArrowUpCircle, ExternalLink, RefreshCw, X } from 'lucide-react'
import type { UpdateMonitor } from '../../hooks/useUpdateMonitor'
import { Button, IconButton } from '../ui'

export function UpdatesPanel({ monitor, onClose }: { monitor: UpdateMonitor; onClose: () => void }) {
  const checked = monitor.lastCheckedAt ? new Date(monitor.lastCheckedAt) : null
  const checkedLabel = checked && !Number.isNaN(checked.getTime())
    ? `Last checked ${checked.toLocaleString()}`
    : 'Not checked yet'

  return (
    <aside
      aria-label="Updates"
      data-testid="updates-panel"
      className="flex h-full w-[360px] shrink-0 flex-col overflow-hidden"
      style={{ background: 'var(--surface)', borderLeft: '1px solid var(--border)' }}
    >
      <header className="flex items-center justify-between gap-3 px-4 py-3" style={{ borderBottom: '1px solid var(--border)' }}>
        <div className="min-w-0">
          <div className="flex items-center gap-2 text-sm font-semibold" style={{ color: 'var(--text)' }}>
            <ArrowUpCircle size={16} /> Updates
          </div>
          <div className="mt-0.5 text-[11px]" style={{ color: 'var(--text-3)' }}>{checkedLabel}</div>
        </div>
        <IconButton icon={<X size={16} />} label="Close updates" onClick={onClose} />
      </header>

      <div className="flex-1 overflow-y-auto p-4">
        {monitor.error && (
          <div role="alert" className="mb-3 rounded-lg p-3 text-xs" style={{ color: 'var(--red)', border: '1px solid var(--red)', background: 'var(--surface-up)' }}>
            {monitor.error}
          </div>
        )}

        {monitor.updates.length === 0 ? (
          <div className="grid place-items-center gap-2 rounded-xl px-4 py-10 text-center" style={{ border: '1px solid var(--border)', color: 'var(--text-3)' }}>
            <ArrowUpCircle size={24} />
            <div className="text-sm" style={{ color: 'var(--text-2)' }}>Everything is up to date</div>
            <div className="text-xs">UAM and detected provider CLIs have no newer known release.</div>
          </div>
        ) : (
          <div className="grid gap-3">
            {monitor.updates.map((update) => (
              <article key={update.id} className="grid gap-3 rounded-xl p-3" style={{ background: 'var(--surface-up)', border: '1px solid var(--border)' }}>
                <div className="flex items-start justify-between gap-3">
                  <div className="min-w-0">
                    <div className="truncate text-sm font-semibold" style={{ color: 'var(--text)' }}>{update.name}</div>
                    <div className="mt-1 text-xs" style={{ color: 'var(--text-3)' }}>
                      {update.currentVersion} <span aria-hidden>→</span> <strong style={{ color: 'var(--green)' }}>{update.latestVersion}</strong>
                    </div>
                  </div>
                  <IconButton
                    icon={<X size={14} />}
                    label={`Dismiss ${update.name} ${update.latestVersion}`}
                    onClick={() => monitor.dismiss(update.id, update.latestVersion)}
                  />
                </div>
                <div className="flex flex-wrap gap-2">
                  {update.providerId && update.installable ? (
                    <Button size="sm" variant="primary" disabled={monitor.providerChecksRunning} onClick={() => { void monitor.applyCliProviderVersion(update.providerId!, update.latestVersion) }}>
                      Update now
                    </Button>
                  ) : (
                    <Button size="sm" variant="primary" onClick={() => window.open(update.url, '_blank', 'noopener')}>
                      View release <ExternalLink size={13} />
                    </Button>
                  )}
                  {update.providerId && (
                    <Button size="sm" variant="secondary" onClick={() => window.open(update.url, '_blank', 'noopener')}>
                      Instructions <ExternalLink size={13} />
                    </Button>
                  )}
                </div>
              </article>
            ))}
          </div>
        )}
      </div>

      <footer className="flex items-center justify-between gap-2 p-3" style={{ borderTop: '1px solid var(--border)' }}>
        <Button size="sm" variant="secondary" leadingIcon={<RefreshCw size={14} className={monitor.checking ? 'animate-spin' : ''} />} disabled={monitor.checking} onClick={() => { void monitor.checkNow() }}>
          {monitor.checking ? 'Checking…' : 'Check now'}
        </Button>
        {monitor.updates.length > 0 && <Button size="sm" variant="ghost" onClick={monitor.dismissAll}>Dismiss all</Button>}
      </footer>
    </aside>
  )
}
