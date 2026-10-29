import { useEffect, useState } from 'react'
import type { CSSProperties } from 'react'
import './TilesVisualizer.css'

type ColorRole = 'root' | 'fifth' | 'third' | 'note' | 'accidental'
type PreviewMode = 'press' | 'primary' | 'secondary'
type Scale = { slot: number; name: string; intervals: number[] }
const BUTTONS = ['−', '+', '△', '◇', '□', '○']
const DEFAULT_COLORS: Record<ColorRole, string> = {
  root: '660066', fifth: '240066', third: 'none', note: '363636', accidental: '000000',
}
const DEFAULT_LOOK: Record<string, number> = {
  'look.idle_baseline_percent': 50,
  'look.natural_pad_percent': 21,
  'look.root_pad_percent': 40,
  'look.fifth_pad_percent': 40,
  'look.fifth_red_tint_percent': 35,
  'look.echo_sustain_tint_percent': 35,
  'look.echo_secondary_g_percent': 18,
  'look.echo_secondary_b_percent': 12,
  'look.echo_flash_ms': 180,
  'look.led_intensity_percent': 100,
}
const DEFAULT_PAD_COLORS = Object.fromEntries(
  Array.from({ length: 24 }, (_, i) => [String(i + 1).padStart(2, '0'), 'none']),
)

function rgb(r: number, g: number, b: number) {
  const hex = (n: number) => Math.round(Math.max(0, Math.min(100, n)) * 2.55)
    .toString(16).padStart(2, '0')
  return `#${hex(r)}${hex(g)}${hex(b)}`
}

// A preview approximation of the melodic grid: pad 19 is the lowest note.
// The chosen custom scale is preview-only; no firmware command selects a scale.
function getPadRole(pad: number, intervals?: number[]): ColorRole {
  const row = Math.floor((pad - 1) / 6)
  const col = (pad - 1) % 6
  const degree = (3 - row) * 6 + col
  const semitone = intervals?.length
    ? intervals[degree % intervals.length] : degree % 12
  const third = intervals?.includes(4) ? 4 : intervals?.includes(3) ? 3 : 4
  if (semitone === 0) return 'root'
  if (semitone === 7) return 'fifth'
  if (semitone === third) return 'third'
  if ([1, 3, 6, 8, 10].includes(semitone)) return 'accidental'
  return 'note'
}

function legacyColor(role: ColorRole, look: Record<string, number>) {
  const root = look['look.root_pad_percent']
  const fifth = look['look.fifth_pad_percent']
  const tint = look['look.fifth_red_tint_percent']
  const natural = look['look.natural_pad_percent']
  if (role === 'root') return rgb(root, 0, root)
  if (role === 'fifth') return rgb(fifth * tint / 100, 0, fifth)
  return role === 'accidental' ? '#000000' : rgb(natural, natural, natural)
}

function modernColor(role: ColorRole, colors: Record<ColorRole, string>) {
  const value = colors[role] === 'none' ? colors.note : colors[role]
  return /^[0-9a-f]{6}$/i.test(value) ? `#${value}` : '#000000'
}

function previewColor(mode: PreviewMode, look: Record<string, number>) {
  if (mode === 'primary') return rgb(look['look.echo_sustain_tint_percent'], 100, look['look.echo_sustain_tint_percent'])
  if (mode === 'secondary') return rgb(100, look['look.echo_secondary_g_percent'], look['look.echo_secondary_b_percent'])
  const baseline = look['look.idle_baseline_percent']
  const level = baseline + (100 - baseline) * 0.35
  return rgb(level, level, level)
}

function parseScales(rows: string[]): Scale[] {
  return rows.flatMap(row => {
    const match = row.match(/^([1-9]) (\S+) (\d+(?:,\d+)*)/)
    return match ? [{ slot: Number(match[1]), name: match[2], intervals: match[3].split(',').map(Number) }] : []
  })
}

export function TilesVisualizer() {
  const [colors, setColors] = useState(DEFAULT_COLORS)
  const [padColors, setPadColors] = useState<Record<string, string>>(DEFAULT_PAD_COLORS)
  const [customPads, setCustomPads] = useState(false)
  const [look, setLook] = useState(DEFAULT_LOOK)
  const [modern, setModern] = useState(false)
  const [layout, setLayout] = useState('rows')
  const [effect, setEffect] = useState('off')
  const [scales, setScales] = useState<Scale[]>([])
  const [scaleChoice, setScaleChoice] = useState('chromatic')
  const [activePad, setActivePad] = useState<number | null>(null)
  const [previewMode, setPreviewMode] = useState<PreviewMode>('press')

  useEffect(() => {
    let cancelled = false
    async function loadSettings() {
      try {
        const lines = await window.tiles.usb.command('LIST', true)
        if (cancelled) return
        const lookup = new Map<string, string>(lines.filter(l => l.includes('='))
          .map((l): [string, string] => [l.slice(0, l.indexOf('=')), l.slice(l.indexOf('=') + 1)]))
        setModern(lookup.has('color.root'))
        setColors(prev => Object.fromEntries(
          Object.keys(prev).map(role => [role, lookup.get(`color.${role}`) ?? prev[role as ColorRole]])
        ) as Record<ColorRole, string>)
        setPadColors(prev => Object.fromEntries(
          Object.keys(prev).map(pad => [pad, lookup.get(`color.pad.${pad}`) ?? 'none'])
        ))
        setCustomPads(lookup.get('color.custom_pads') === '1')
        setLayout(lookup.get('layout.melodic') ?? 'rows')
        setEffect(lookup.get('look.play_effect') ?? 'off')
        setLook(prev => Object.fromEntries(Object.entries(prev).map(([key, value]) =>
          [key, Number(lookup.get(key) ?? value)]
        )))
      } catch (reason) { console.warn('Could not read simulator settings', reason) }
    }
    async function loadScales() {
      try {
        const rows = await window.tiles.usb.command('SCALES', true)
        if (!cancelled) {
          const current = parseScales(rows)
          setScales(current)
          setScaleChoice(previous =>
            previous === 'chromatic' || current.some(s => String(s.slot) === previous)
              ? previous : 'chromatic')
        }
      } catch (reason) { console.warn('Could not read custom scales', reason) }
    }
    function changed(event: Event) {
      const { key, value } = (event as CustomEvent<{key: string; value: string}>).detail
      if (key in DEFAULT_LOOK) {
        const number = Number(value)
        if (Number.isFinite(number)) setLook(prev => ({ ...prev, [key]: number }))
      } else if (key === 'layout.melodic') {
        setLayout(value)
      } else if (key === 'look.play_effect') {
        setEffect(value)
      } else if (key === 'color.custom_pads') {
        setCustomPads(value === '1')
      } else if (key.startsWith('color.pad.')) {
        setPadColors(prev => ({ ...prev, [key.slice(10)]: value }))
      } else if (key.startsWith('color.')) {
        const role = key.slice(6) as ColorRole
        if (role in DEFAULT_COLORS)
          setColors(prev => ({ ...prev, [role]: value }))
      }
    }
    window.addEventListener('tiles:setting-changed', changed)
    window.addEventListener('tiles:scales-changed', loadScales)
    void loadSettings()
    void loadScales()
    return () => {
      cancelled = true
      window.removeEventListener('tiles:setting-changed', changed)
      window.removeEventListener('tiles:scales-changed', loadScales)
    }
  }, [])

  const intervals = scales.find(s => String(s.slot) === scaleChoice)?.intervals

  return (
    <section className="tiles-visualizer">
      <div className="tiles-visualizer-heading">
        <h2>Instrument Preview</h2>
        <span>24 Pads · {modern ? 'RGB scheme' : 'Legacy lighting'} · Simulated</span>
      </div>
      <div className="tiles-preview-controls">
        <label>Preview scale{' '}
          <select value={scaleChoice} onChange={e => setScaleChoice(e.currentTarget.value)}>
            <option value="chromatic">Chromatic (C)</option>
            {scales.map(scale => (
              <option key={scale.slot} value={scale.slot}>
                Slot {scale.slot}: {scale.name.replace(/_/g, ' ')}
              </option>
            ))}
          </select>
        </label>
        <label>Selected-pad animation{' '}
          <select value={previewMode}
            onChange={e => setPreviewMode(e.currentTarget.value as PreviewMode)}>
            <option value="press">Press (35%)</option>
            <option value="primary">Primary echo</option>
            <option value="secondary">Secondary echo</option>
          </select>
        </label>
      </div>
      <div className="tiles-device">
        <div className="tiles-device-header">
          <span className="tiles-brand">SENTIA / TILES</span>
          <div className="tiles-buttons">
            {BUTTONS.map((button, index) => (
              <span key={index} className="tiles-function-button">{button}</span>
            ))}
          </div>
        </div>
        <div className="tiles-pad-grid">
          {Array.from({ length: 24 }, (_, index) => {
            const pad = index + 1
            const role = getPadRole(pad, intervals)
            const override = padColors[String(pad).padStart(2, '0')]
            const isActive = activePad === pad
            const idle = modern
              ? customPads && override && override !== 'none'
                ? `#${override}`
                : modernColor(role, colors)
              : legacyColor(role, look)
            const baseColor = isActive ? previewColor(previewMode, look) : idle
            const brightness = look['look.led_intensity_percent'] / 100
            const color = /^#[0-9a-f]{6}$/i.test(baseColor)
              ? '#' + [1, 3, 5].map(offset => Math.round(
                  parseInt(baseColor.slice(offset, offset + 2), 16) * brightness,
                ).toString(16).padStart(2, '0')).join('')
              : baseColor
            return (
              <button key={pad} type="button"
                className={`tiles-pad ${isActive ? 'is-active' : ''}`}
                style={{ '--pad-color': color } as CSSProperties}
                title={`Pad ${pad} · ${role}`}
                aria-label={`Preview pad ${pad}`}
                aria-pressed={isActive}
                onClick={() => setActivePad(isActive ? null : pad)}>
                <span>{pad}</span>
              </button>
            )
          })}
        </div>
      </div>
      <p className="tiles-preview-note">
        Visual approximation only. Layout setting: {layout}; effect setting: {effect}.
        Non-row layouts and animated effects are not yet represented geometrically
        in this preview. Selecting scales or pads here does not send MIDI.
      </p>
    </section>
  )
}
