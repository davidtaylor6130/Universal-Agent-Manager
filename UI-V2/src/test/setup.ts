// jsdom has no ResizeObserver; Radix Tooltip/Popper needs one once a tooltip opens.
if (typeof globalThis.ResizeObserver === 'undefined') {
  globalThis.ResizeObserver = class {
    observe() {}
    unobserve() {}
    disconnect() {}
  }
}
