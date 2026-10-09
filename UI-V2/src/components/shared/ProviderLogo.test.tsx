import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it } from 'vitest'
import { ProviderLogo } from './ProviderLogo'
import opencodeLogo from '../../assets/provider-logos/opencode.svg'
import antigravityLogo from '../../assets/provider-logos/antigravity.svg'
import geminiLogo from '../../assets/provider-logos/gemini-cli.png'
import codexLogo from '../../assets/provider-logos/codex.svg'
import claudeLogo from '../../assets/provider-logos/claude.svg'

describe('ProviderLogo', () => {
  it.each([16, 32])('renders bundled official marks decoratively at %i pixels on black', (size) => {
    const marks = [
      ['opencode-cli', opencodeLogo], ['antigravity-cli', antigravityLogo],
      ['gemini-cli', geminiLogo], ['codex-cli', codexLogo], ['claude-cli', claudeLogo],
    ] as const
    for (const [providerId, asset] of marks) {
      const host = document.createElement('div')
      host.innerHTML = renderToStaticMarkup(<ProviderLogo providerId={providerId} size={size} style={{ background: '#000' }} />)
      const frame = host.firstElementChild as HTMLElement
      const image = frame.querySelector('img')!
      expect(frame.getAttribute('aria-hidden')).toBe('true')
      expect(frame.style.width).toBe(`${size}px`)
      expect(frame.style.height).toBe(`${size}px`)
      expect(frame.style.background).toBe('rgb(0, 0, 0)')
      expect(image.getAttribute('src')).toBe(asset)
      expect(image.getAttribute('src')).not.toMatch(/^https?:/)
      expect(image.getAttribute('alt')).toBe('')
      expect(image.draggable).toBe(false)
      expect(image.classList.contains('object-contain')).toBe(true)
      expect(image.classList.contains('uam-provider-logo--codex')).toBe(providerId === 'codex-cli')
      expect(frame.querySelectorAll('svg')).toHaveLength(0)
    }
  })
  it('retains distinct branding for all six supported providers and defaults missing IDs to Gemini', () => {
    const host = document.createElement('div')
    host.innerHTML = renderToStaticMarkup(<>
      <ProviderLogo providerId="gemini-cli" />
      <ProviderLogo providerId="codex-cli" />
      <ProviderLogo providerId="claude-cli" />
      <ProviderLogo providerId="opencode-cli" />
      <ProviderLogo providerId="copilot-cli" />
      <ProviderLogo providerId="antigravity-cli" />
      <ProviderLogo />
    </>)
    expect(host.children[0].querySelector('img')?.getAttribute('src')).toBe(geminiLogo)
    expect(host.children[1].querySelector('img')?.getAttribute('src')).toBe(codexLogo)
    expect(host.children[2].querySelector('img')?.getAttribute('src')).toBe(claudeLogo)
    expect(host.children[3].querySelector('img')?.getAttribute('src')).toBe(opencodeLogo)
    expect(host.children[4].querySelector('svg')?.getAttribute('viewBox')).toBe('0 0 24 24')
    expect(host.children[4].querySelector('img')).toBeNull()
    expect(host.children[5].querySelector('img')).not.toBeNull()
    expect(host.children[5].querySelector('img')?.getAttribute('src')).not.toBe(geminiLogo)
    expect(host.children[6].querySelector('img')?.getAttribute('src')).toBe(geminiLogo)
  })
  it('uses a neutral generic mark for unknown provider IDs', () => {
    const host = document.createElement('div')
    host.innerHTML = renderToStaticMarkup(<ProviderLogo providerId="future-provider" />)
    expect(host.querySelector('img')).toBeNull()
    expect(host.querySelector('svg')?.getAttribute('aria-hidden')).toBe('true')
    expect(host.querySelector('svg')?.getAttribute('viewBox')).toBe('0 0 24 24')
  })
})
