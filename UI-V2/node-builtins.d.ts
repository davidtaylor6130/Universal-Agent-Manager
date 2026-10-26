// The only Node built-ins vite.config.ts uses; the project does not install @types/node.
declare module 'node:fs' {
  export function rmSync(path: string, options?: { force?: boolean }): void
}
declare module 'node:path' {
  export function join(...paths: string[]): string
}
