import { useTilesSettings } from './settings'

export function Lighting() {
  const keys = {
    idle: 'look.idle_baseline_percent',
    natural: 'look.natural_pad_percent',
    root: 'look.root_pad_percent',
    fifth: 'look.fifth_pad_percent',
    fifthRed: 'look.fifth_red_tint_percent',
    sustainTint: 'look.echo_sustain_tint_percent',
    echoGreen: 'look.echo_secondary_g_percent',
    echoBlue: 'look.echo_secondary_b_percent',
    flashMs: 'look.echo_flash_ms',
  }

  const { values, update, error } = useTilesSettings(keys)

  const percent = [
    ['idle', 'Idle Baseline'],
    ['natural', 'Natural Pad'],
    ['root', 'Root Pad'],
    ['fifth', 'Fifth Pad'],
    ['fifthRed', 'Fifth Red Tint'],
    ['sustainTint', 'Sustain Tint'],
    ['echoGreen', 'Echo Green'],
    ['echoBlue', 'Echo Blue'],
  ]

  return (
    <section>
      <h2>Lighting</h2>

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