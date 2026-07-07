/** @type {import('tailwindcss').Config} */
export default {
  content: [
    "./index.html",
    "./src/**/*.{js,ts,jsx,tsx}",
  ],
  theme: {
    extend: {
      fontFamily: {
        // UI text is proportional sans; mono reserved for code/CLI/ids.
        sans: ['var(--font-sans)'],
        mono: ['var(--font-mono)'],
      },
      fontSize: {
        xs:   ['var(--fs-xs)',   { lineHeight: '1.4' }],
        sm:   ['var(--fs-sm)',   { lineHeight: '1.45' }],
        base: ['var(--fs-base)', { lineHeight: '1.55' }],
        md:   ['var(--fs-md)',   { lineHeight: '1.5' }],
        lg:   ['var(--fs-lg)',   { lineHeight: '1.4' }],
        xl:   ['var(--fs-xl)',   { lineHeight: '1.3' }],
        '2xl':['var(--fs-2xl)',  { lineHeight: '1.25' }],
      },
      borderRadius: {
        sm:   'var(--r-sm)',
        md:   'var(--r-md)',
        lg:   'var(--r-lg)',
        full: 'var(--r-full)',
      },
      boxShadow: {
        'elev-1': 'var(--elev-1)',
        'elev-2': 'var(--elev-2)',
        'elev-3': 'var(--elev-3)',
      },
      transitionTimingFunction: {
        app: 'var(--ease)',
      },
      transitionDuration: {
        fast: 'var(--dur-fast)',
        base: 'var(--dur-base)',
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
        // Semantic
        'app-success':       'var(--success)',
        'app-success-dim':   'var(--success-dim)',
        'app-warning':       'var(--warning)',
        'app-warning-dim':   'var(--warning-dim)',
        'app-error':         'var(--error)',
        'app-error-dim':     'var(--error-dim)',
        'app-info':          'var(--info)',
        'app-info-dim':      'var(--info-dim)',
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
