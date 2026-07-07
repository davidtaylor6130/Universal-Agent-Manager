/** Tiny classname joiner — filters out falsy values. Avoids a clsx dependency. */
export function cx(...parts: Array<string | false | null | undefined>): string {
  return parts.filter(Boolean).join(' ')
}
