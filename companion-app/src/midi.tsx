import { useTilesSettings } from "./settings"

export function Midi() {
  const keys = {
    trsType: 'midi.din_trs_type',
  }

  const { values, update, error } = useTilesSettings(keys)

  return (
    <section>
      <h2>MIDI</h2>

      <label>
        DIN TRS Type
        <select
          value={values.trsType ?? ''}
          onChange={e => update('trsType', e.currentTarget.value)}
        >
          <option value="a">Type A</option>
          <option value="b">Type B</option>
        </select>
      </label>

      {error && <p>Error: {error}</p>}
    </section>
  )
}