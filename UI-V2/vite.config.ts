import { defineConfig } from 'vitest/config'
import react from '@vitejs/plugin-react'

const buildOutDir =
  (globalThis as typeof globalThis & { process?: { env?: Record<string, string | undefined> } }).process?.env
    ?.VITE_UAM_OUT_DIR ?? 'dist'

export default defineConfig({
  plugins: [react()],
  base: './',   // relative paths so file:// serving works in CEF
  test: {
    environment: 'jsdom',
    globals: true,
    include: ['src/**/*.test.{ts,tsx}'],
  },
  build: {
    outDir: buildOutDir,
    emptyOutDir: true,
    assetsDir: 'assets',
  },
})
