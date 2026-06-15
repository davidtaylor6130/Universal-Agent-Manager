/** @type {import('tailwindcss').Config} */
export default {
  content: [
    "./index.html",
    "./src/**/*.{js,ts,jsx,tsx}",
  ],
  theme: {
    extend: {
      fontFamily: {
        mono: ['"JetBrains Mono"', 'monospace'],
        sans: ['"JetBrains Mono"', 'monospace'],
      },
      colors: {
        // Theme-aware via CSS variables
        'app-bg':            'var(--bg)',
        'app-surface':       'var(--surface)',
        'app-surface-up':    'var(--surface-up)',
        'app-surface-high':  'var(--surface-high)',
        'app-border':        'var(--border)',
        'app-border-bright': 'var(--border-bright)',
        'app-text':          'var(--text)',
        'app-text-2':        'var(--text-2)',
        'app-text-3':        'var(--text-3)',
        'app-accent':        'var(--accent)',
        'app-accent-dim':    'var(--accent-dim)',
        'app-blue':          'var(--blue)',
        'app-blue-dim':      'var(--blue-dim)',
        'app-green':         'var(--green)',
        'app-red':           'var(--red)',
      },
      animation: {
        'blink': 'blink 1.1s step-end infinite',
        'fade-in': 'fadeIn 0.15s ease-out',
        'slide-in': 'slideIn 0.18s ease-out',
        'step-appear': 'stepAppear 0.2s ease-out',
      },
      keyframes: {
        blink: {
          '0%, 100%': { opacity: '1' },
          '50%': { opacity: '0' },
        },
        fadeIn: {
          from: { opacity: '0' },
          to: { opacity: '1' },
        },
        slideIn: {
          from: { opacity: '0', transform: 'translateY(6px)' },
          to: { opacity: '1', transform: 'translateY(0)' },
        },
        stepAppear: {
          from: { opacity: '0', transform: 'translateX(-8px)' },
          to: { opacity: '1', transform: 'translateX(0)' },
        },
      },
    },
  },
  plugins: [],
}
