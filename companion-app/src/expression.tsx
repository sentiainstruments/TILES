import { useTilesSettings } from "./settings"

export function Expression() {
  const keys = {
    mpe: 'expression.mpe_enabled',
    pitchBend: 'expression.pitch_bend_sensitivity',
    aftertouch: 'expression.aftertouch_sensitivity',
  }

  const { values, update, error } = useTilesSettings(keys)

  return (
    <section>
      <h2>Expression</h2>

      <label>
        MPE Enabled
        <input
          type="checkbox"
          checked={values.mpe === '1'}
          onChange={e => update('mpe', e.currentTarget.checked ? '1' : '0')}
        />
      </label>

      <label>
        Pitch Bend Sensitivity
        <input
          type="number"
          min="0.001"
          max="1"
          step="0.001"
          value={values.pitchBend ?? ''}
          onChange={e => update('pitchBend', e.currentTarget.value)}
        />
      </label>

      <label>
        Aftertouch Sensitivity
        <input
          type="number"
          min="1"
          max="65535"
          value={values.aftertouch ?? ''}
          onChange={e => update('aftertouch', e.currentTarget.value)}
        />
      </label>

      {error && <p>Error: {error}</p>}
    </section>
  )
}