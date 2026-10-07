import { useTilesSettings } from "./settings"

export function FeaturesHarmonics() {
  const keys = {
    enabled: 'features.melodic_harmonics',
    armMs: 'features.harmonics.arm_ms',
    confirmMs: 'features.harmonics.confirm_ms',
    pressDepth: 'features.harmonics.press_depth',
  }

  const { values, update, error } = useTilesSettings(keys)

  return (
    <section>
      <h2>Harmonics</h2>

      <label>
        Melodic Harmonics
        <input
          type="checkbox"
          checked={values.enabled === '1'}
          onChange={e => update('enabled', e.currentTarget.checked ? '1' : '0')}
        />
      </label>

      <label>
        Arm Time (ms)
        <input
          type="number"
          min="0"
          max="1000"
          value={values.armMs ?? ''}
          onChange={e => update('armMs', e.currentTarget.value)}
        />
      </label>

      <label>
        Confirm Time (ms)
        <input
          type="number"
          min="0"
          max="500"
          value={values.confirmMs ?? ''}
          onChange={e => update('confirmMs', e.currentTarget.value)}
        />
      </label>

      <label>
        Press Depth
        <input
          type="number"
          min="1"
          max="1000"
          value={values.pressDepth ?? ''}
          onChange={e => update('pressDepth', e.currentTarget.value)}
        />
      </label>

      {error && <p>Error: {error}</p>}
    </section>
  )
}