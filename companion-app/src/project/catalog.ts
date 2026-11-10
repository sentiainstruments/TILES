import fixture from '../../fixtures/tiles-settings.json'

export type SettingDescriptor = {
  key: string
  type: string
  min?: number
  max?: number
  values?: string[]
  default: string
}

export const toWire = (value: string | number | boolean): string =>
  typeof value === 'boolean' ? (value ? '1' : '0') : String(value)

/** Base + current board-bringup settings; used for offline editing only. */
type FixtureSetting = {
  key: string
  type: string
  min?: number
  max?: number
  values?: string[]
  default: string | number | boolean
}

export const settingCatalog: SettingDescriptor[] = (
  [...fixture.settings, ...fixture.alphaSettings] as FixtureSetting[]
).map(setting => ({
  key: setting.key,
  type: setting.type,
  min: setting.min,
  max: setting.max,
  values: setting.values ? [...setting.values] : undefined,
  default: toWire(setting.default),
}))

export const defaultSettings: Record<string, string> = Object.fromEntries(
  settingCatalog.map(setting => [setting.key, setting.default]),
)

export function parseSchema(rows: string[]): Map<string, SettingDescriptor> {
  const map = new Map<string, SettingDescriptor>()
  for (const row of rows) {
    const items = Object.fromEntries(row.split(/\s+/).map(x => {
      const at = x.indexOf('=')
      return [x.slice(0, at), x.slice(at + 1)]
    }))
    if (!items.key || !/^[a-z][a-z0-9_.]+$/.test(items.key)) continue
    map.set(items.key, {
      key: items.key,
      type: items.type,
      min: items.min === undefined ? undefined : Number(items.min),
      max: items.max === undefined ? undefined : Number(items.max),
      values: items.values?.split('|'),
      default: items.default ?? '',
    })
  }
  return map
}

export function parseList(rows: string[]): Map<string, string> {
  return new Map(rows.filter(row => row.includes('=') && !row.startsWith('ERR '))
    .map(row => [row.slice(0, row.indexOf('=')), row.slice(row.indexOf('=') + 1)]))
}

export function validForSchema(setting: SettingDescriptor, value: string): boolean {
  if (/\s|[\r\n]/.test(value)) return false
  if (setting.type === 'bool') return value === '0' || value === '1'
  if (setting.type === 'enum') return setting.values?.includes(value) ?? false
  if (setting.type === 'color') return /^(?:[A-Fa-f0-9]{6}|none)$/.test(value)
  if (['uint', 'int', 'float'].includes(setting.type)) {
    const n = Number(value)
    return value !== '' && Number.isFinite(n) &&
      (setting.type === 'float' || Number.isInteger(n)) &&
      (setting.type !== 'uint' || n >= 0) &&
      (setting.min === undefined || n >= setting.min) &&
      (setting.max === undefined || n <= setting.max)
  }
  return false
}

export function equivalentValue(a: string, b: string, type: string): boolean {
  if (type === 'color') return a.toLowerCase() === b.toLowerCase()
  if (type === 'float' || type === 'int' || type === 'uint') {
    return Number(a) === Number(b)
  }
  return a === b
}

export const groups = [
  { title: 'Lighting', prefix: 'color.' },
  { title: 'Appearance & effects', prefix: 'look.' },
  { title: 'Musical layout', prefix: 'layout.' },
  { title: 'Expression & MPE', prefix: 'expression.' },
  { title: 'Mode menu', prefix: 'modes.' },
  { title: 'MIDI', prefix: 'midi.' },
  { title: 'Pedal', prefix: 'pedal.' },
  { title: 'Sequencer', prefix: 'seq.' },
  { title: 'Harmonics', prefix: 'features.' },
  { title: 'CV & gate', prefix: 'cv_gate.' },
] as const

export function niceLabel(key: string): string {
  return key.split('.').slice(1).join(' ').replace(/[_.]/g, ' ')
    .replace(/\b\w/g, char => char.toUpperCase())
}
