import { useEffect, useRef, useState } from 'react'
import { Preview } from './synth/preview'
import { pageColors, fxNames, fxColors, trackColors, previewNotes, modeTrack, slotAt, slotPage, wrap, slotName, selectedSlot, selectSlot, getEffects, setEffect as setTrackEffect, effectValue, makeSelection, isSaved } from './synth/soundMenu'
import type { SoundSelection } from './synth/soundMenu'
import type { CSSProperties } from 'react'
import './TilesVisualizer.css'
import { MelodicEditor } from './MelodicEditor'
import { getMelodicPadInfo } from './melodicNoteMap'

type ColorRole = 'root' | 'fifth' | 'third' | 'note' | 'accidental'
type PreviewMode = 'press' | 'primary' | 'secondary'
type Scale = { slot: number; name: string; intervals: number[] }
type ModeId = 'melodic' | 'duet' | 'bass' | 'sequencer' | 'drums' |
  'song' | 'prism' | 'ableton' | 'volca' | 'bounce' | 'cradle' |
  'sand' | 'ripples' | 'orbits'
type ViewId = 'play' | 'modes' | 'scales' | 'patterns' | 'sounds' | 'effects'

type Mode = {
  id: ModeId
  name: string
  row: number
  col: number
  color: string
  setting?: string
  description: string
}

// Matches firmware/src/services/op_mode.c MENU_SLOTS (row and column 1-based).
// These are UI-preview colors, not measured physical LED values.
const MODES: Mode[] = [
  { id: 'melodic', name: 'Melodic', row: 1, col: 1, color: '#ff00ff', description: 'Play notes on the melodic pad grid.' },
  { id: 'duet', name: 'Duet', row: 2, col: 1, color: '#ff598c', setting: 'modes.duet', description: 'Chord pads on the left; melody on the right.' },
  { id: 'bass', name: 'Bass guitar', row: 3, col: 1, color: '#ac81ed', setting: 'modes.bass', description: 'Four strings across the rows, frets across columns.' },
  { id: 'sequencer', name: 'Sequencer', row: 1, col: 2, color: '#ff2200', setting: 'modes.sequencer', description: 'A 24-step view, with four independent lanes.' },
  { id: 'drums', name: 'Drums', row: 2, col: 2, color: '#f48a15', setting: 'modes.drums', description: 'Steps on columns 1–4; eight drum pads on 5–6.' },
  { id: 'song', name: 'Song', row: 3, col: 2, color: '#ffac18', setting: 'modes.song', description: 'A 24-slot song/looper view.' },
  { id: 'prism', name: 'Prism', row: 1, col: 3, color: '#487bff', setting: 'modes.prism', description: 'Chord roots, voicings, qualities and extensions.' },
  { id: 'ableton', name: 'Ableton', row: 1, col: 4, color: '#00bfa2', setting: 'modes.ableton', description: 'Ableton Session View clip grid.' },
  { id: 'volca', name: 'Volca', row: 2, col: 4, color: '#43c4ec', setting: 'modes.volca', description: 'Drum grid with a separate MIDI channel per drum.' },
  { id: 'bounce', name: 'Bouncing notes', row: 1, col: 5, color: '#b9ee43', setting: 'modes.bounce', description: 'Six bouncing-note lanes shown at a time.' },
  { id: 'cradle', name: "Newton's cradle", row: 2, col: 5, color: '#a3b4ce', setting: 'modes.cradle', description: 'Beat-driven cradle physics.' },
  { id: 'sand', name: 'Sand', row: 3, col: 5, color: '#e0ad5b', setting: 'modes.sand', description: 'Build melodic piles in sand.' },
  { id: 'ripples', name: 'Ripples', row: 1, col: 6, color: '#266ae8', setting: 'modes.ripples', description: 'Rings ripple from selected pads.' },
  { id: 'orbits', name: 'Orbits', row: 2, col: 6, color: '#6856c9', setting: 'modes.orbits', description: 'Planets orbit around a musical center.' },
]

const BUTTONS = [
  { symbol: '−', name: 'Minus', hint: 'Octave down / mode-specific action' },
  { symbol: '+', name: 'Plus', hint: 'Octave up / mode-specific action' },
  { symbol: '△', name: 'Triangle', hint: 'Open or close the mode menu' },
  { symbol: '◇', name: 'Diamond', hint: 'Transport / sequencer pattern bank' },
  { symbol: '□', name: 'Square', hint: 'Pitch-bend toggle / Feel menu (hold on hardware)' },
  { symbol: '○', name: 'Circle', hint: 'Shift (click to arm a combo in the preview)' },
] as const

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
const DEFAULT_PAD_COLORS: Record<string, string> = Object.fromEntries(
  Array.from({ length: 24 }, (_, i) => [String(i + 1).padStart(2, '0'), 'none']),
)

function rgb(r: number, g: number, b: number): string {
  const hex = (n: number) => Math.round(Math.max(0, Math.min(100, n)) * 2.55)
    .toString(16).padStart(2, '0')
  return `#${hex(r)}${hex(g)}${hex(b)}`
}

function legacyColor(role: ColorRole, look: Record<string, number>): string {
  const root = look['look.root_pad_percent']
  const fifth = look['look.fifth_pad_percent']
  const tint = look['look.fifth_red_tint_percent']
  const natural = look['look.natural_pad_percent']
  if (role === 'root') return rgb(root, 0, root)
  if (role === 'fifth') return rgb(fifth * tint / 100, 0, fifth)
  return role === 'accidental' ? '#000000' : rgb(natural, natural, natural)
}

function modernColor(role: ColorRole, colors: Record<ColorRole, string>): string {
  const value = colors[role] === 'none' ? colors.note : colors[role]
  return /^[0-9a-f]{6}$/i.test(value) ? `#${value}` : '#000000'
}

function previewColor(mode: PreviewMode, look: Record<string, number>): string {
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

function dimColor(hex: string, multiplier: number): string {
  if (!/^#[\da-f]{6}$/i.test(hex)) return hex
  return '#' + [1, 3, 5].map(i => Math.round(
    parseInt(hex.slice(i, i + 2), 16) * Math.max(0, Math.min(multiplier, 1)),
  ).toString(16).padStart(2, '0')).join('')
}

function padInModeMenu(pad: number): Mode | undefined {
  const row = Math.floor((pad - 1) / 6) + 1
  const col = ((pad - 1) % 6) + 1
  return MODES.find(mode => mode.row === row && mode.col === col)
}

function modePadPreview(mode: ModeId, pad: number, selected: Set<number>): {
  color: string
  text: string
} {
  const col = (pad - 1) % 6
  const row = Math.floor((pad - 1) / 6)
  const active = selected.has(pad)
  switch (mode) {
    case 'duet':
      return { color: col < 2 ? '#c65389' : '#7b438d', text: col < 2 ? `C${row + 1}` : `${pad}` }
    case 'bass':
      return { color: ['#ad8cee', '#9574e4', '#7c65cb', '#6454ae'][row], text: `${col + 1}` }
    case 'sequencer':
      return { color: active ? '#f2243a' : '#4a1319', text: String(pad) }
    case 'drums':
    case 'volca':
      return col < 4
        ? { color: active ? '#ff7529' : '#543020', text: String(row * 4 + col + 1) }
        : { color: mode === 'volca' ? '#53e3eb' : '#e3b139', text: `D${row * 2 + col - 3}` }
    case 'song':
      return { color: active ? '#ffd264' : '#6a4310', text: String(pad) }
    case 'prism':
      return { color: ['#3764ff', '#537cfa', '#6f95f8', '#ca70c5', '#59c5fb', '#b4b8ff'][col], text:
        col < 3 ? `${pad}` : (col === 3 ? 'V' : col === 4 ? 'Q' : 'E') }
    case 'ableton':
      return { color: active ? '#45ffc2' : (row + col) % 2 ? '#126e6a' : '#168e84', text: `T${col + 1}` }
    case 'bounce':
      return { color: row === 3 || active ? '#b5ef4b' : dimColor('#a8dd4a', 0.2 + 0.17 * row), text: `${pad}` }
    case 'cradle':
      return { color: active ? '#f3f5ff' : dimColor('#b9c9ea', 0.3 + col * .1), text: `${pad}` }
    case 'sand':
      return { color: active ? '#ffcf79' : dimColor('#d9ab65', 0.2 + (3 - row) * .15), text: `${pad}` }
    case 'ripples':
      return { color: active ? '#c9ebff' : dimColor('#3e83ee', 0.18 + Math.abs(col - 2.5) * .12), text: `${pad}` }
    case 'orbits':
      return { color: active ? '#ddd4ff' : dimColor('#7b64de', 0.2 + ((row + col) % 4) * .13), text: `${pad}` }
    default:
      return { color: '#555555', text: String(pad) }
  }
}

export function TilesVisualizer({ onEditSound }: { onEditSound?: (selection: SoundSelection) => void } = {}) {
  const [standalonePreview, setStandalonePreview] = useState(true)
  const [soundTrack, setSoundTrack] = useState(2)
  const [soundPage, setSoundPage] = useState(0)
  const [soundRevision, setSoundRevision] = useState(0)
  const [soundMessage, setSoundMessage] = useState('Select a lit pad to audition a local draft.')
  const audition = useRef<Preview | null>(null)
  const auditionTimer = useRef<number | undefined>(undefined)
  const auditionGeneration = useRef(0)
  function stopAudition() {
    auditionGeneration.current++
    window.clearTimeout(auditionTimer.current)
    audition.current?.panic()
  }
  useEffect(() => () => {
    auditionGeneration.current++
    window.clearTimeout(auditionTimer.current)
    audition.current?.close()
    audition.current = null
  }, [])
  async function auditionSound(track: number, row: number) {
    stopAudition()
    const generation = auditionGeneration.current
    const selection = makeSelection(track)
    if (!selection) return
    const player = audition.current ?? (audition.current = new Preview())
    try {
      await player.enable()
      if (generation !== auditionGeneration.current) return
      const note = previewNotes[row]
      player.noteOn(note, selection.record, 100 / 127)
      auditionTimer.current = window.setTimeout(() => player.noteOff(note), 600)
      setSoundMessage(`${slotName(selection.slot)} · local audio approximation`)
    } catch (error) {
      if (generation === auditionGeneration.current) setSoundMessage(`Audio unavailable: ${String(error)}`)
    }
  }

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
  const [previewPressed, setPreviewPressed] = useState(false)
  const [activeMode, setActiveMode] = useState<ModeId>('melodic')
  const [view, setView] = useState<ViewId>('play')
  const [modeSettings, setModeSettings] = useState<Record<string, string>>({})
  const [shiftArmed, setShiftArmed] = useState(false)
  const [octave, setOctave] = useState(0)
  const [running, setRunning] = useState(false)
  const [pitchBend, setPitchBend] = useState(true)
  const [mpe, setMpe] = useState(true)
  const [, setDrumPage] = useState(1)
  const [selectedPads, setSelectedPads] = useState<Set<number>>(new Set())

  useEffect(() => {
    let cancelled = false
    async function loadSettings() {
      try {
        const lines = await window.tiles.usb.command('LIST', true)
        if (cancelled) return
        const lookup = new Map<string, string>(lines.filter(l => l.includes('='))
          .map(l => [l.slice(0, l.indexOf('=')), l.slice(l.indexOf('=') + 1)] as [string, string]))
        setModern(lookup.has('color.root'))
        setColors(prev => Object.fromEntries(
          Object.keys(prev).map(role => [role, lookup.get(`color.${role}`) ?? prev[role as ColorRole]]),
        ) as Record<ColorRole, string>)
        setPadColors(prev => Object.fromEntries(
          Object.keys(prev).map(pad => [pad, lookup.get(`color.pad.${pad}`) ?? 'none']),
        ))
        setCustomPads(lookup.get('color.custom_pads') === '1')
        setLayout(lookup.get('layout.melodic') ?? 'rows')
        setEffect(lookup.get('look.play_effect') ?? 'off')
        setMpe(lookup.get('expression.mpe_enabled') !== '0')
        setModeSettings(Object.fromEntries(
          [...lookup.entries()].filter(([key]) => key.startsWith('modes.')),
        ))
        setLook(prev => Object.fromEntries(Object.entries(prev).map(([key, value]) =>
          [key, Number(lookup.get(key) ?? value)]),
        ))
      } catch (reason) { console.warn('Could not read TILES settings', reason) }
    }
    async function loadScales() {
      try {
        const rows = await window.tiles.usb.command('SCALES', true)
        if (cancelled) return
        const current = parseScales(rows)
        setScales(current)
        setScaleChoice(previous =>
          previous === 'chromatic' || current.some(s => String(s.slot) === previous)
            ? previous : 'chromatic')
      } catch (reason) { console.warn('Could not read custom scales', reason) }
    }
    function changed(event: Event) {
      const { key, value } = (event as CustomEvent<{key: string; value: string}>).detail
      if (key in DEFAULT_LOOK) {
        const number = Number(value)
        if (Number.isFinite(number)) setLook(prev => ({ ...prev, [key]: number }))
      } else if (key === 'layout.melodic') setLayout(value)
      else if (key === 'look.play_effect') setEffect(value)
      else if (key === 'color.custom_pads') setCustomPads(value === '1')
      else if (key === 'expression.mpe_enabled') setMpe(value === '1')
      else if (key.startsWith('modes.')) {
        setModeSettings(prev => ({ ...prev, [key]: value }))
      } else if (key.startsWith('color.pad.')) {
        setPadColors(prev => ({ ...prev, [key.slice(10)]: value }))
      } else if (key.startsWith('color.')) {
        const role = key.slice(6) as ColorRole
        if (role in DEFAULT_COLORS) setColors(prev => ({ ...prev, [role]: value }))
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

  function modeEnabled(mode: Mode): boolean {
    return !mode.setting || modeSettings[mode.setting] !== '0'
  }

  useEffect(() => {
    const selected = MODES.find(candidate => candidate.id === activeMode)
    if (selected?.setting && modeSettings[selected.setting] === '0') {
      setActiveMode('melodic')
      setView('play')
      setSelectedPads(new Set())
    }
  }, [activeMode, modeSettings])

  const mode = MODES.find(m => m.id === activeMode) ?? MODES[0]
  const intervals = scales.find(s => String(s.slot) === scaleChoice)?.intervals

  const soundView = view === 'sounds' || view === 'effects'
  const chosenSlot = selectedSlot(soundTrack)
  // The revision triggers a repaint of the app-session simulator store.
  const soundFx = getEffects(soundTrack)
  function openSoundView(next: 'sounds' | 'effects') {
    stopAudition()
    if (view === next) { setView('play'); return }
    const t = modeTrack(activeMode)
    setSoundTrack(t)
    setSoundPage(slotPage(selectedSlot(t)))
    setView(next)
    setSoundMessage('Select a lit pad to audition a local draft.')
  }
  function handleButton(index: number) {
    if (index === 5) {
      // Circle is a held modifier on hardware. A click arms the *next*
      // combo in this mouse-operated preview, rather than faking a hold.
      setShiftArmed(current => !current)
      return
    }
    if (soundView && (index === 0 || index === 1)) {
      stopAudition()
      const delta = index === 1 ? 1 : -1
      if (view === 'effects' || shiftArmed) {
        const t = wrap(soundTrack - 1 + delta, 4) + 1
        setSoundTrack(t)
        setSoundPage(slotPage(selectedSlot(t)))
      } else setSoundPage(p => wrap(p + delta, 4))
      setShiftArmed(false)
      return
    }
    if (soundView && index === 2) stopAudition()
    if (index === 2) {
      if (shiftArmed) {
        setView(activeMode === 'drums' || activeMode === 'volca' ? 'play' : 'scales')
        if (activeMode === 'drums' || activeMode === 'volca')
          setDrumPage(p => p === 1 ? 2 : 1)
      } else {
        setView(current => current === 'modes' ? 'play' : 'modes')
      }
    } else if (index === 3) {
      if (standalonePreview && activeMode !== 'sequencer') {
        openSoundView(shiftArmed ? 'effects' : 'sounds')
      } else if (shiftArmed) {
        setView('play') // capture needs separate firmware-backed implementation
      } else if (activeMode === 'sequencer') {
        setView(current => current === 'patterns' ? 'play' : 'patterns')
      } else if (activeMode === 'drums' || activeMode === 'volca') {
        setDrumPage(p => p === 1 ? 2 : 1)
        setView('play')
      } else {
        setRunning(prev => !prev)
      }
    } else if (index === 4) {
      if (shiftArmed) setMpe(prev => !prev)
      else setPitchBend(prev => !prev)
    } else if (index === 0 || index === 1) {
      if (activeMode === 'sequencer' || activeMode === 'drums' || activeMode === 'volca') {
        setRunning(index === 1)
      } else {
        setOctave(prev => Math.max(-3, Math.min(3, prev + (index === 1 ? 1 : -1))))
      }
    }
    setShiftArmed(false)
  }

  function handlePad(pad: number) {
    if (view === 'sounds') {
      const slot = slotAt(soundPage, pad)
      if (selectSlot(soundTrack, slot)) {
        setSoundRevision(n => n + 1)
        void auditionSound(soundTrack, Math.floor((pad - 1) / 6))
      }
      return
    }
    if (view === 'effects') {
      setTrackEffect(soundTrack, (pad - 1) % 6, effectValue(pad))
      setSoundRevision(n => n + 1)
      return
    }
    if (view === 'modes') {
      const picked = padInModeMenu(pad)
      if (picked && modeEnabled(picked)) {
        setActiveMode(picked.id)
        setView('play')
        setSelectedPads(new Set())
        setActivePad(null)
      }
      return
    }
    if (view === 'scales') {
      // Actual built-in scale picker positions are not available to the
      // app through the USB settings protocol; use the dropdown instead.
      return
    }
    if (view === 'patterns') {
      const lane = Math.floor((pad - 1) / 6)
      setSelectedPads(previous => {
        const next = new Set([...previous].filter(item =>
          Math.floor((item - 1) / 6) !== lane))
        next.add(pad)
        return next
      })
      return
    }
    if (activeMode === 'melodic' || activeMode === 'duet' || activeMode === 'bass') {
      setActivePad(prev => prev === pad ? null : pad)
    } else {
      setSelectedPads(prev => {
        const next = new Set(prev)
        if (next.has(pad)) next.delete(pad)
        else next.add(pad)
        return next
      })
    }
  }

  const note = view === 'sounds'
    ? '− / + changes the preset page. Click ○ then − / + to switch tracks. Press ◇ again to close. Select a pad to audition, then open the selected sound in Synth Workspace.'
    : view === 'effects'
      ? 'Each column is an effect; top row = full, bottom row = off. − / + switches tracks. Click ○ then ◇ to close. Values are local draft data, not live DSP effects.'
      : view === 'modes' 
    ? 'Press △ to close the mode menu, or click a lit pad to select a mode. A disabled mode is dark. Mode selection here changes only the app preview; it does not change the real TILES mode.'
    : view === 'scales'
      ? '○ then △ opens the scale picker on TILES. Built-in picker-pad mappings are not modeled here. The scale dropdown above changes this preview only.'
      : view === 'patterns'
        ? '◇ opens the sequencer pattern bank. This grid is a visual placeholder: the real instrument stores six alternatives per lane. Press ◇ again to return.'
        : `Previewing ${mode.name}: ${mode.description} ${activeMode === 'melodic' ? 'Click a pad to edit its individual color; enable Animate selected pad to show its pressed state.' : 'These pads do not send MIDI or change the physical instrument.'} Layout: ${layout}; lighting effect: ${effect}.`

  return (
    <section className="tiles-visualizer">
      <div className="tiles-visualizer-heading">
        <h2>Instrument Preview</h2>
        <span>24 Pads · {modern ? 'RGB' : 'Legacy'} · Interactive simulation</span>
      </div>
      <div className="tiles-preview-controls">
        <label>Instrument preview{' '}
          <select value={standalonePreview ? 'standalone' : 'controller'} onChange={e => {
            stopAudition(); setStandalonePreview(e.currentTarget.value === 'standalone'); setView('play'); setShiftArmed(false)
          }}>
            <option value="standalone">Standalone (Sound Menu)</option>
            <option value="controller">Controller (MIDI)</option>
          </select>
        </label>
        <label>Mode{' '}
          <select aria-label="Preview mode" value={activeMode}
            onChange={event => {
              const next = MODES.find(item => item.id === event.currentTarget.value)
              if (!next || !modeEnabled(next)) return
              stopAudition()
              setActiveMode(next.id)
              setView('play')
              setActivePad(null)
              setSelectedPads(new Set())
            }}>
            {MODES.map(item => <option key={item.id} value={item.id} disabled={!modeEnabled(item)}>
              {item.name}
            </option>)}
          </select>
        </label>
        <label>Preview scale{' '}
          <select value={scaleChoice} onChange={e => setScaleChoice(e.currentTarget.value)}>
            <option value="chromatic">Chromatic (C)</option>
            {scales.map(scale =>
              <option key={scale.slot} value={String(scale.slot)}>
                Slot {scale.slot}: {scale.name.replace(/_/g, ' ')}
              </option>)}
          </select>
        </label>
        <label>Pad animation{' '}
          <select value={previewMode}
            onChange={e => setPreviewMode(e.currentTarget.value as PreviewMode)}>
            <option value="press">Press</option>
            <option value="primary">Primary echo</option>
            <option value="secondary">Secondary echo</option>
          </select>
        </label>
        <label><input type="checkbox" checked={previewPressed}
          onChange={event => setPreviewPressed(event.currentTarget.checked)} />
          Animate selected pad
        </label>
      </div>
      <div className="tiles-mode-status" aria-live="polite">
        <span className="tiles-mode-dot" style={{ background: mode.color }} />
        <strong>{view === 'sounds' ? 'Sound Menu' : view === 'effects' ? 'Effects Menu' : view === 'modes' ? 'Mode menu' : view === 'scales' ? 'Scale picker preview'
          : view === 'patterns' ? 'Pattern bank preview' : mode.name}</strong>
        {soundView && <span style={{ color: trackColors[soundTrack - 1] }}>Track {soundTrack} · {view === 'sounds' ? `Page ${soundPage + 1}/4` : 'FX'}</span>}
        <span>Octave {octave >= 0 ? `+${octave}` : octave}</span>
        <span>{running ? 'Transport: playing' : 'Transport: stopped'}</span>
        <span>{pitchBend ? 'Bend on' : 'Bend off'} · {mpe ? 'MPE on' : 'MPE off'}</span>
        {shiftArmed && <strong className="tiles-shift-label">○ Shift armed — choose another button</strong>}
      </div>
      <div className={`tiles-preview-workspace ${activeMode === 'melodic' && view === 'play' ? 'with-melodic-editor' : ''}`}>
        <div className="tiles-device-and-notes">
      <div className={`tiles-device ${soundView ? 'tiles-sound-device' : ''}`} style={soundView ? { '--sound-track-color': trackColors[soundTrack-1] } as CSSProperties : undefined}>
        <div className="tiles-device-header">
          <span className="tiles-brand">SENTIA / TILES</span>
          <div className="tiles-buttons">
            {BUTTONS.map((button, index) => (
              <button key={button.symbol} type="button"
                className={`tiles-function-button ${(index === 2 && view === 'modes') || (index === 3 && soundView) ? 'is-selected' : ''} ${index === 5 && shiftArmed ? 'is-selected' : ''}`}
                onClick={() => handleButton(index)}
                title={index === 3 && standalonePreview && activeMode !== 'sequencer' ? 'Sound Menu / circle + diamond: effects' : button.hint} aria-label={index === 3 && standalonePreview && activeMode !== 'sequencer' ? 'Diamond: Sound Menu' : `${button.name}: ${button.hint}`}
                aria-pressed={(index === 2 && view === 'modes') || (index === 3 && soundView) || (index === 5 && shiftArmed)}>
                {button.symbol}
              </button>
            ))}
          </div>
        </div>
        <div className="tiles-pad-grid">
          {Array.from({ length: 24 }, (_, index) => {
            const pad = index + 1
            const padInfo = getMelodicPadInfo(pad, layout, intervals, octave)
            const role = padInfo.role
            const override = padColors[String(pad).padStart(2, '0')]
            const isActive = activePad === pad
            const menuMode = padInModeMenu(pad)
            const enabled = menuMode ? modeEnabled(menuMode) : false
            let baseColor = ''
            let label = String(pad)
            let selected = isActive
            if (view === 'sounds') {
              const slot = slotAt(soundPage, pad)
              label = slotName(slot) || 'Empty'
              selected = slot === chosenSlot && Boolean(slotName(slot))
              baseColor = slotName(slot) ? pageColors[soundPage][Math.floor(index / 6)] : '#242735'
            } else if (view === 'effects') {
              const col = index % 6
              const threshold = effectValue(pad)
              const amount = soundFx[col]
              label = `${fxNames[col]} ${Math.round(threshold*100)}%`
              selected = threshold === 0 ? amount === 0 : amount >= threshold - .001
              baseColor = fxColors[col]
            } else if (view === 'modes') {
              baseColor = enabled && menuMode ? menuMode.color : '#131625'
              label = enabled && menuMode ? menuMode.name : ''
              selected = Boolean(menuMode && menuMode.id === activeMode)
            } else if (view === 'scales') {
              baseColor = pad <= 9 ? '#663b87' : '#141824'
              label = pad <= 9 ? `S${pad}` : ''
            } else if (view === 'patterns') {
              selected = selectedPads.has(pad)
              baseColor = selected ? '#ff89f6' : '#842c62'
              label = `P${(pad - 1) % 6 + 1}`
            } else if (activeMode === 'melodic') {
              label = padInfo.noteLabel
              const idle = !padInfo.isActiveNote ? '#000000' : modern
                ? customPads && override && override !== 'none'
                  ? `#${override}` : modernColor(role, colors)
                : legacyColor(role, look)
              baseColor = isActive && previewPressed ? previewColor(previewMode, look) : idle
            } else {
              const result = modePadPreview(activeMode, pad, selectedPads)
              baseColor = selectedPads.has(pad) ? dimColor(result.color, 1) : result.color
              label = result.text
              selected = selectedPads.has(pad)
            }
            const color = dimColor(baseColor, look['look.led_intensity_percent'] / 100)
            return (
              <button key={pad} type="button"
                className={`tiles-pad ${selected ? 'is-active' : ''} ${view === 'play' && activeMode === 'melodic' && selected ? 'is-edit-selected' : ''} ${view === 'modes' && selected ? 'is-current-mode' : ''} ${view === 'modes' && !enabled ? 'is-disabled-preview' : ''}`}
                style={{ '--pad-color': color } as CSSProperties}
                disabled={(view === 'modes' && !enabled) || (view === 'sounds' && !slotName(slotAt(soundPage,pad)))}
                title={view === 'sounds' ? `Slot ${slotAt(soundPage,pad)} · ${label}` : view === 'effects' ? label : view === 'modes' ? (enabled && menuMode ? menuMode.name : 'Unavailable')
                  : activeMode === 'melodic' ? `Pad ${pad} · ${padInfo.noteLabel} · ${role}`
                  : `Pad ${pad} · ${mode.name}`}
                aria-label={view === 'sounds' ? `Preset ${slotAt(soundPage,pad)}: ${label}` : view === 'effects' ? label : view === 'modes' ? `Select ${menuMode?.name ?? 'unavailable mode'}` : `Preview pad ${pad} in ${mode.name}`}
                aria-pressed={selected}
                onClick={() => handlePad(pad)}>
                <span>{label}</span>
                {view === 'sounds' && <small>{slotAt(soundPage,pad)}{isSaved(slotAt(soundPage,pad)) ? ' · EDIT' : ''}</small>}
              </button>
            )
          })}
        </div>
      </div>
      {soundView && <div className="tiles-sound-toolbar" data-revision={soundRevision}>
        <div className="tiles-sound-nav">
          <label>Track <select value={soundTrack} onChange={e => {
            stopAudition(); const t = Number(e.currentTarget.value); setSoundTrack(t); setSoundPage(slotPage(selectedSlot(t)))
          }}>{[1,2,3,4].map(t => <option key={t} value={t}>Track {t}</option>)}</select></label>
          {view === 'sounds' && <label>Page <select value={soundPage} onChange={e => { stopAudition(); setSoundPage(Number(e.currentTarget.value)) }}>
            {[0,1,2,3].map(p => <option key={p} value={p}>{p+1} · {p*24+1}–{p*24+24}</option>)}
          </select></label>}
          <button type="button" onClick={() => openSoundView(view === 'sounds' ? 'effects' : 'sounds')}>{view === 'sounds' ? 'Effects' : 'Sounds'}</button>
        </div>
        <strong>Track {soundTrack} · Slot {chosenSlot} · {slotName(chosenSlot)}</strong>
        {view === 'effects' && <div className="tiles-fx-readout">{fxNames.map((fx,i) => <span key={fx}>{fx} {Math.round(soundFx[i]*100)}%</span>)}</div>}
        <small role="status">{soundMessage}</small>
        <button type="button" disabled={!onEditSound || !slotName(chosenSlot)} onClick={() => {
          const selection = makeSelection(soundTrack)
          if (!selection || !onEditSound) return
          if (!window.confirm('Open this local sound draft in Synth Workspace? This replaces the current unsaved editor draft.')) return
          stopAudition(); onEditSound(selection)
        }}>Edit selected sound in Synth Workspace</button>
        <small>Development firmware layout · factory-inspired drafts, not exact factory records · no hardware commands</small>
      </div>}
      <p className="tiles-preview-note">{note}</p>

        </div>
        {activeMode === 'melodic' && view === 'play' &&
          <MelodicEditor selectedPad={activePad} onSelectPad={setActivePad} />}
      </div>
    </section>
  )
}
