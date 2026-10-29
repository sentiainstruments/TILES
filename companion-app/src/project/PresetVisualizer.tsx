import { useState, type CSSProperties } from 'react'
import type { CustomScale } from './model'
import '../TilesVisualizer.css'

type Role = 'root' | 'third' | 'fifth' | 'note' | 'accidental'
type Props = {
  settings: Record<string, string>
  scales: CustomScale[]
  previewScaleSlot: number | null
}
const BUTTONS = ['−', '+', '△', '◇', '□', '○']
const COLOR_FALLBACK: Record<Role, string> = {
  root: '660066', third: 'none', fifth: '240066', note: '363636', accidental: '000000',
}

function toColor(value: string) {
  return /^[A-Fa-f0-9]{6}$/.test(value) ? `#${value}` : '#08080C'
}

function roleForPad(pad: number, intervals?: number[]): Role {
  // Visual approximation, not an exact emulator of alternative layouts.
  const row = Math.floor((pad - 1) / 6)
  const col = (pad - 1) % 6
  const step = (3 - row) * 6 + col
  const note = intervals?.length ? intervals[step % intervals.length] : step % 12
  const third = intervals?.includes(4) ? 4 : intervals?.includes(3) ? 3 : 4
  if (note === 0) return 'root'
  if (note === 7) return 'fifth'
  if (note === third) return 'third'
  if ([1, 3, 6, 8, 10].includes(note)) return 'accidental'
  return 'note'
}

export function PresetVisualizer({ settings, scales, previewScaleSlot }: Props) {
  const [selectedPad, setSelectedPad] = useState<number | null>(null)
  const [pressPreview, setPressPreview] = useState(false)
  const scale = scales.find(s => s.slot === previewScaleSlot)
  const custom = settings['color.custom_pads'] === '1'
  const intensityRaw = Number(settings['look.led_intensity_percent'] ?? '100')
  const intensity = Number.isFinite(intensityRaw)
    ? Math.max(0, Math.min(100, intensityRaw)) / 100 : 1
  const previewColor = (pad: number) => {
    if (selectedPad === pad && pressPreview) return '#FFFFFF'
    const perPad = settings[`color.pad.${String(pad).padStart(2, '0')}`]
    if (custom && perPad && perPad !== 'none') return toColor(perPad)
    const role = roleForPad(pad, scale?.intervals)
    for (const candidate of [role, 'note'] as Role[]) {
      const color = settings[`color.${candidate}`] ?? COLOR_FALLBACK[candidate]
      if (color !== 'none') return toColor(color)
    }
    return '#08080C'
  }
  const dim = (color: string) => `#${[1, 3, 5].map(offset =>
    Math.round(parseInt(color.slice(offset, offset + 2), 16) * intensity)
      .toString(16).padStart(2, '0')).join('')}`

  return (
    <section className="tiles-visualizer" style={{ padding: 0 }}>
      <div className="tiles-visualizer-heading">
        <h2>Instrument preview</h2>
        <span>24 pads · {settings['layout.melodic'] ?? 'rows'} layout</span>
      </div>
      <div className="tiles-device">
        <div className="tiles-device-header">
          <span className="tiles-brand">SENTIA / TILES</span>
          <div className="tiles-buttons">
            {BUTTONS.map((label, i) => <span className="tiles-function-button" key={i}>{label}</span>)}
          </div>
        </div>
        <div className="tiles-pad-grid">
          {Array.from({ length: 24 }, (_, index) => {
            const pad = index + 1
            const color = dim(previewColor(pad))
            return <button key={pad} type="button"
              className={`tiles-pad ${selectedPad === pad ? 'is-active' : ''}`}
              style={{ '--pad-color': color } as CSSProperties}
              aria-label={`Preview pad ${pad}`} aria-pressed={selectedPad === pad}
              title={`Pad ${pad} · ${roleForPad(pad, scale?.intervals)}`}
              onClick={() => setSelectedPad(selectedPad === pad ? null : pad)}>
              <span>{pad}</span>
            </button>
          })}
        </div>
      </div>
      <div className="preset-preview-footer">
        <label><input type="checkbox" checked={pressPreview}
          onChange={e => setPressPreview(e.currentTarget.checked)} /> Show selected pad as pressed</label>
        <small>Preview only. Alternate layouts and animated effects are not yet rendered faithfully.
          This screen never changes the device until you press Apply.</small>
      </div>
    </section>
  )
}
