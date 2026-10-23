import { useId } from 'react'
import type { ButtonHTMLAttributes } from 'react'
import { ChevronRight, CircleDashed } from 'lucide-react'
import { ProviderLogo } from '../shared/ProviderLogo'
import { providerMetadataForId, SUPPORTED_CLI_PROVIDER_IDS } from '../../utils/providerMetadata'
import { Button, Tooltip } from '../ui'
import './SubagentCard.css'

type Props = Omit<ButtonHTMLAttributes<HTMLButtonElement>, 'title' | 'children'> & {
  title: string
  status: string
  summary?: string
  providerId?: string
  working?: boolean
  elapsed?: string
}

/** Presentation only: identity, navigation and control remain with the owning row. */
export function SubagentCard({ title, status, summary, providerId, working = false, elapsed = 'Timing unavailable', className = '', ...button }: Props) {
  const id = useId()
  const description = summary || 'No summary reported'
  const provider = providerId && SUPPORTED_CLI_PROVIDER_IDS.includes(providerId) ? providerMetadataForId(providerId).name : 'Provider unavailable'
  const knownTime = /^\d+(?:s|m \d+s|h \d+m)$/.test(elapsed)
  return <Tooltip label={`${provider} · ${title} · ${status}\n${description}\n${elapsed}`}>
    <Button type="button" {...button} variant="ghost" size="sm" className={`uam-subagent-card ${className}`} aria-describedby={`${id}-provider ${id}-status ${id}-summary ${id}-time`}
      leadingIcon={<span className="uam-subagent-card__provider" data-provider-id={providerId || 'unknown'}><ProviderLogo providerId={providerId || 'unknown'} size={24} /><span className="sr-only" id={`${id}-provider`}>{provider}</span></span>}
      trailingIcon={<span className="uam-subagent-card__tail"><span id={`${id}-time`} className="uam-subagent-card__time" aria-label={knownTime ? `Elapsed ${elapsed}` : elapsed}>{knownTime ? elapsed : '—'}</span><ChevronRight size={14} aria-hidden /></span>}>
      <span className="uam-subagent-card__headline"><span className="uam-subagent-card__title">{title}</span><span id={`${id}-status`} className="uam-subagent-card__status">{working && <CircleDashed size={12} className="uam-working-ring" aria-hidden />}<span className="uam-subagent-card__status-label">{status}</span></span></span>
      <span id={`${id}-summary`} className="uam-subagent-card__summary">{description}</span>
    </Button>
  </Tooltip>
}
