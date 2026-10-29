import { useTilesSettings } from './settings'

// Colour scheme (firmware 0.2.7+): one colour per note role, "RRGGBB" or
// "none" (no highlight; the pad shows the next role that applies). The
// colour is what the pad shows before the power ceiling, so brightness is
// part of it. See shared/protocol/README.md, "Colour schemes and pad colours".
const SCHEME_ROLES = [
  ['root', 'Root'],
  ['fifth', 'Fifth'],
  ['third', 'Third'],
  ['note', 'Scale Note'],
  ['accidental', 'Accidental'],
]

// What a role gets when it's switched back on (the device defaults; third has none).
const ON_COLOR: Record<string, string> = {
  root: '660066',
  fifth: '240066',
  third: '663300',
  note: '363636',
  accidental: '000000',
}

export function Lighting() {
  const keys = {
    idle: 'look.idle_baseline_percent',
    sustainTint: 'look.echo_sustain_tint_percent',
    echoGreen: 'look.echo_secondary_g_percent',
    echoBlue: 'look.echo_secondary_b_percent',
    flashMs: 'look.echo_flash_ms',
  }

  const { values, update, error } = useTilesSettings(keys)

  const scheme = useTilesSettings({
    root: 'color.root',
    fifth: 'color.fifth',
    third: 'color.third',
    note: 'color.note',
    accidental: 'color.accidental',
  })

  const percent = [
    ['idle', 'Idle Baseline'],
    ['sustainTint', 'Sustain Tint'],
    ['echoGreen', 'Echo Green'],
    ['echoBlue', 'Echo Blue'],
  ]

  return (
    <section>
      <h2>Lighting</h2>

      <h3>Colour scheme</h3>
      {SCHEME_ROLES.map(([name, label]) => {
        const value = scheme.values[name] ?? ''
        const off = value === 'none'
        return (
          <label key={name}>
            {label}
            <input
              type="color"
              value={off || !value ? '#000000' : `#${value.toLowerCase()}`}
              disabled={off}
              onChange={e => scheme.update(name, e.currentTarget.value.slice(1).toUpperCase())}
            />
            <input
              type="checkbox"
              checked={off}
              onChange={e => scheme.update(name, e.currentTarget.checked ? 'none' : ON_COLOR[name])}
            />
            off
          </label>
        )
      })}
      {scheme.error && <p>Error: {scheme.error}</p>}

      {percent.map(([name, label]) => (
        <label key={name}>
          {label} %
          <input
            type="range"
            min="0"
            max="100"
            value={values[name] ?? 0}
            onChange={e => update(name, e.currentTarget.value)}
          />
          {values[name] ?? 0}%
        </label>
      ))}

      <label>
        Echo Flash (ms)
        <input
          type="number"
          min="0"
          max="2000"
          value={values.flashMs ?? ''}
          onChange={e => update('flashMs', e.currentTarget.value)}
        />
      </label>

      {error && <p>Error: {error}</p>}
    </section>
  )
}
