import fixture from '../../fixtures/tiles-settings.json'

type Setting = {
  id: number
  key: string
  type: string
  default: string | number | boolean
  value: string
  persist: boolean
  min?: number
  max?: number
  values?: string[]
}
type Scale = {
  slot: number
  name: string
  intervals: number[]
  pack: string
  item: string
  version: number
}

export const mockDevice = {
  vendorId: 0x1209,
  productId: 0x0001,
  productName: 'SENTIA TILES (Simulator)',
  manufacturerName: 'Sentia Instruments',
}

const VERSIONS = ['0.2.0', '0.3.6', 'alpha.6.5'] as const
type Version = (typeof VERSIONS)[number]
let firmware: Version = 'alpha.6.5'
let settings = new Map<string, Setting>()
const scales = new Map<number, Scale>()

const toWire = (value: string | number | boolean) =>
  typeof value === 'boolean' ? (value ? '1' : '0') : String(value)

function loadVersion(next: Version) {
  firmware = next
  const list = next === 'alpha.6.5'
    ? [...fixture.settings, ...fixture.alphaSettings]
    : next === '0.3.6'
      ? fixture.settings
      : [...fixture.settings.filter(s => !s.key.startsWith('color.')), ...fixture.legacySettings]
  settings = new Map<string, Setting>(list.map((s): [string, Setting] => [s.key, {
    ...s,
    value: toWire(s.default),
  }]))
  scales.clear()
}
loadVersion(firmware)

function schemaLine(setting: Setting) {
  const parts = [
    `id=${setting.id}`,
    `key=${setting.key}`,
    `type=${setting.type}`,
  ]
  if (setting.values) parts.push(`values=${setting.values.join('|')}`)
  if (setting.min !== undefined) parts.push(`min=${setting.min}`)
  if (setting.max !== undefined) parts.push(`max=${setting.max}`)
  parts.push(`default=${toWire(setting.default)}`)
  if (!setting.persist) parts.push('persist=0')
  return parts.join(' ')
}

function validate(setting: Setting, value: string): string | null {
  if (setting.type === 'bool')
    return ['0', '1'].includes(value) ? null : 'ERR bad-value'
  if (setting.type === 'enum')
    return setting.values?.includes(value) ? null : 'ERR out-of-range'
  if (setting.type === 'color')
    return /^(?:[0-9a-fA-F]{6}|none)$/.test(value) ? null : 'ERR bad-value'
  if (['int', 'uint', 'float'].includes(setting.type)) {
    if (!value || !/^[+-]?(?:\d+\.?\d*|\.\d+)$/.test(value))
      return 'ERR bad-value'
    const num = Number(value)
    if (!Number.isFinite(num) || (setting.type !== 'float' && !Number.isInteger(num)))
      return 'ERR bad-value'
    if (setting.type === 'uint' && num < 0) return 'ERR out-of-range'
    if ((setting.min !== undefined && num < setting.min) ||
        (setting.max !== undefined && num > setting.max))
      return 'ERR out-of-range'
    return null
  }
  return 'ERR bad-value'
}

const validToken = (s: string, allowEmpty = false) =>
  (allowEmpty && s.length === 0) || (s.length > 0 && s.length <= 16 && /^[!-~]+$/.test(s))

function parseSlot(raw: string) {
  if (!/^[1-9]$/.test(raw ?? '')) return null
  return Number(raw)
}

function scaleLine(scale: Scale) {
  let value = `${scale.slot} ${scale.name} ${scale.intervals.join(',')}`
  if (scale.pack) value += ` pack=${scale.pack}`
  if (scale.item) value += ` item=${scale.item}`
  return `${value} version=${scale.version}`
}

function putScale(parts: string[]): string {
  const slot = parseSlot(parts[0])
  if (slot === null) return 'ERR bad-slot'
  const name = parts[1] ?? ''
  if (!validToken(name)) return 'ERR bad-name'
  const rawIntervals = parts[2] ?? ''
  if (!/^\d+(,\d+)*$/.test(rawIntervals)) return 'ERR bad-intervals'
  const intervals = rawIntervals.split(',').map(Number)
  if (!intervals.length || intervals.length > 12 || intervals[0] !== 0 ||
      intervals.some((n, i) => n > 11 || (i > 0 && n <= intervals[i - 1])))
    return 'ERR bad-intervals'

  const fields: Record<string, string> = {}
  for (const field of parts.slice(3)) {
    const index = field.indexOf('=')
    const key = field.slice(0, index)
    if (index < 1 || !['pack', 'item', 'version'].includes(key) || key in fields)
      return 'ERR bad-field'
    fields[key] = field.slice(index + 1)
  }
  const pack = fields.pack ?? ''
  const item = fields.item ?? ''
  const version = fields.version === undefined ? 0 : Number(fields.version)
  if (!validToken(pack, true) || !validToken(item, true)) return 'ERR bad-name'
  if (fields.version !== undefined &&
      (!/^\d+$/.test(fields.version) || !Number.isInteger(version) || version > 65535))
    return 'ERR bad-field'

  scales.set(slot, {slot, name, intervals, pack, item, version})
  return 'OK'
}

function handleScale(parts: string[]): string[] {
  const action = parts[0]
  const slot = parseSlot(parts[1])
  if (!['GET', 'PUT', 'DELETE'].includes(action ?? '')) return ['ERR unsupported-command']
  if (slot === null) return ['ERR bad-slot']
  if (action === 'PUT') return [putScale(parts.slice(1))]
  if (action === 'DELETE') {
    scales.delete(slot)
    return ['OK']
  }
  const value = scales.get(slot)
  return value ? [scaleLine(value)] : ['ERR empty']
}

/**
 * An in-memory, firmware-versioned protocol simulator.
 * Multiline responses omit the terminal OK because the real USB adapter
 * consumes it before returning lines to the renderer.
 */
export async function mockCommand(command: string, _multiLine = false): Promise<string[]> {
  const parts = command.trim().split(/\s+/)
  const [action, key, ...rest] = parts
  console.log('[SIM]', command)

  // Simulator-only command. Never send it to physical hardware.
  if (action === 'SIM' && key === 'VERSION') {
    if (!VERSIONS.includes(rest[0] as Version)) return ['ERR bad-value']
    loadVersion(rest[0] as Version)
    return ['OK']
  }
  if (action === 'INFO') {
    return [
      'simulator=1',
      'unit=2',
      `firmware=${firmware}`,
      `settings=${settings.size}`,
      'power=usb_only',
      'power.led_ceiling_percent=50',
      'store.loaded=1',
      'store.pending=0',
      `content.storage=${firmware !== '0.2.0' ? 'ok' : 'none'}`,
      `content.scales=${scales.size}/9`,
      'drums.storage=mock',
    ]
  }
  if (action === 'LIST') return [...settings.values()].map(s => `${s.key}=${s.value}`)
  if (action === 'SCHEMA') return [...settings.values()].map(schemaLine)
  if (action === 'GET') {
    const setting = settings.get(key)
    return setting ? [setting.value] : ['ERR unknown-key']
  }
  if (action === 'SET') {
    const setting = settings.get(key)
    if (!setting) return ['ERR unknown-key']
    const value = rest.join(' ')
    const error = validate(setting, value)
    if (error) return [error]
    setting.value = setting.type === 'color' && value !== 'none'
      ? value.toUpperCase() : value
    return ['OK']
  }
  if (action === 'RESET') {
    if (key === 'ALL') {
      for (const s of settings.values()) s.value = toWire(s.default)
      return ['OK']
    }
    const setting = settings.get(key)
    if (!setting) return ['ERR unknown-key']
    setting.value = toWire(setting.default)
    return ['OK']
  }
  if (action === 'SAVE') return ['OK']
  if (action === 'REBOOT' && key === 'APP') {
    for (const s of settings.values())
      if (!s.persist) s.value = toWire(s.default)
    return ['OK']
  }
  if (action === 'REBOOT' && key === 'BOOTSEL')
    return ['ERR unsupported-command']

  if (firmware !== '0.2.0') {
    if (action === 'SCALES')
      return [...scales.values()].sort((a, b) => a.slot - b.slot).map(scaleLine)
    if (action === 'SCALE') return handleScale([key, ...rest])
    if (action === 'CONTENT' && key === 'LIST') {
      return [...scales.values()].sort((a, b) => a.slot - b.slot).map(s => {
        const bytes = 6 + s.intervals.length + s.name.length + s.pack.length + s.item.length
        return `scale ${s.slot} bytes=${bytes} version=${s.version}${s.pack ? ` pack=${s.pack}` : ''}${s.item ? ` item=${s.item}` : ''}`
      })
    }
    if (action === 'CONTENT' && key === 'CLEAR') {
      scales.clear()
      return ['OK']
    }
    if (action === 'TEST' && key === 'OFF') return ['OK']
    if (action === 'TEST' && key === 'MODE' &&
        ['melodic', 'duet', 'sequencer', 'bass', 'song', 'ableton', 'drums', 'volca', 'prism', 'bounce', 'ripples', 'sand', 'orbits', 'cradle'].includes(rest[0]))
      return ['OK']
  }
  return ['ERR unsupported-command']
}
