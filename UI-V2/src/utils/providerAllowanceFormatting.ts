const creditFormat = new Intl.NumberFormat(undefined, { maximumFractionDigits: 2 })

/** Provider credits do not imply a currency; preserve values that are not plain numbers. */
export function formatCreditBalance(balance: string): string {
  const value = balance.trim()
  if (!/^[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:e[+-]?\d+)?$/i.test(value)) return balance
  const number = Number(value)
  return Number.isFinite(number) ? creditFormat.format(number) : balance
}
