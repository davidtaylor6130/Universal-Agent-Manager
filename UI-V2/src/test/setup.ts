// jsdom has no ResizeObserver; Radix Tooltip/Popper needs one once a tooltip opens.
if (typeof globalThis.ResizeObserver === 'undefined') {
  globalThis.ResizeObserver = class {
    observe() {}
    unobserve() {}
    disconnect() {}
  }
}

// Formatting follows the machine locale (Windows, for example, may default to en-GB), so tests pin
// the default to en-US; an explicit locale argument still wins. ponytail: TZ is not pinned.
const TEST_LOCALE = 'en-US'
const pinLocale = (locales: unknown) => locales === undefined || (Array.isArray(locales) && locales.length === 0) ? TEST_LOCALE : locales
for (const name of ['Collator', 'DateTimeFormat', 'DisplayNames', 'ListFormat', 'NumberFormat', 'PluralRules', 'RelativeTimeFormat', 'Segmenter']) {
  const original = (Intl as unknown as Record<string, ((...args: unknown[]) => unknown) | undefined>)[name]
  if (!original) continue
  Object.defineProperty(Intl, name, { configurable: true, writable: true, value: new Proxy(original, {
    construct: (target, [locales, ...rest]) => Reflect.construct(target as unknown as new (...args: unknown[]) => object, [pinLocale(locales), ...rest]),
    apply: (target, self, [locales, ...rest]) => Reflect.apply(target, self, [pinLocale(locales), ...rest]),
  }) })
}
const pinMethod = (proto: object, method: string, localesIndex = 0) => {
  const original = (proto as Record<string, (...args: unknown[]) => unknown>)[method]
  Object.defineProperty(proto, method, { configurable: true, writable: true, value: function (this: unknown, ...args: unknown[]) {
    args[localesIndex] = pinLocale(args[localesIndex])
    return original.apply(this, args)
  } })
}
pinMethod(Number.prototype, 'toLocaleString')
pinMethod(BigInt.prototype, 'toLocaleString')
for (const method of ['toLocaleString', 'toLocaleDateString', 'toLocaleTimeString']) pinMethod(Date.prototype, method)
pinMethod(String.prototype, 'localeCompare', 1)
