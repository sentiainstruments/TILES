import { useEffect, useState } from 'react'

const keys = {
  enabled: 'cv_gate.enabled',
  voltsPerSemitone: 'cv_gate.pitch.volts_per_semitone',
  referenceNote: 'cv_gate.pitch.reference_note',
  pitchZeroTrim: 'cv_gate.pitch.zero_trim_volts',
  pitchGainTrim: 'cv_gate.pitch.gain_trim',
  pressureFullScale: 'cv_gate.pressure.full_scale_volts',
  pressureZeroTrim: 'cv_gate.pressure.zero_trim_volts',
  pressureGainTrim: 'cv_gate.pressure.gain_trim',
} as const

type Settings = Record<keyof typeof keys, string>

export function Cvgate() {
  const [settings, setSettings] = useState<Settings>({
    enabled: '',
    voltsPerSemitone: '',
    referenceNote: '',
    pitchZeroTrim: '',
    pitchGainTrim: '',
    pressureFullScale: '',
    pressureZeroTrim: '',
    pressureGainTrim: '',
  })
  const [error, setError] = useState('')

  async function get(key: string) {
    const [value] = await window.tiles.usb.command(`GET ${key}`)
    if (value.startsWith('ERR ')) throw new Error(value)
    return value
  }

  async function update(name: keyof Settings, value: string) {
    try {
      const [response] = await window.tiles.usb.command(`SET ${keys[name]} ${value}`)
      if (response !== 'OK') throw new Error(response)
      setSettings(s => ({ ...s, [name]: value }))
      setError('')
    } catch (e) {
      setError(String(e))
    }
  }

  useEffect(() => {
    Promise.all(Object.values(keys).map(get))
      .then(values => setSettings(
        Object.fromEntries(Object.keys(keys).map((key, i) => [key, values[i]])) as Settings
      ))
      .catch(e => setError(String(e)))
  }, [])

  return (
    <section>
      <h2>CV / Gate</h2>

      <label>
        Enabled
        <input
          type="checkbox"
          checked={settings.enabled === '1'}
          onChange={e => update('enabled', e.currentTarget.checked ? '1' : '0')}
        />
      </label>

      <h3>Pitch CV</h3>

      <label>
        Volts per Semitone
        <input
          type="number"
          step="0.001"
          min="0.001"
          max="1"
          value={settings.voltsPerSemitone}
          onChange={e => update('voltsPerSemitone', e.currentTarget.value)}
        />
      </label>

      <label>
        Reference Note
        <input
          type="number"
          min="0"
          max="127"
          value={settings.referenceNote}
          onChange={e => update('referenceNote', e.currentTarget.value)}
        />
      </label>

      <label>
        Zero Trim Volts
        <input
          type="number"
          step="0.01"
          min="-2.5"
          max="2.5"
          value={settings.pitchZeroTrim}
          onChange={e => update('pitchZeroTrim', e.currentTarget.value)}
        />
      </label>

      <label>
        Gain Trim
        <input
          type="number"
          step="0.01"
          min="0.5"
          max="2"
          value={settings.pitchGainTrim}
          onChange={e => update('pitchGainTrim', e.currentTarget.value)}
        />
      </label>

      <h3>Pressure CV</h3>

      <label>
        Full Scale Volts
        <input
          type="number"
          step="0.1"
          min="0.1"
          max="10"
          value={settings.pressureFullScale}
          onChange={e => update('pressureFullScale', e.currentTarget.value)}
        />
      </label>

      <label>
        Zero Trim Volts
        <input
          type="number"
          step="0.01"
          min="-2.5"
          max="2.5"
          value={settings.pressureZeroTrim}
          onChange={e => update('pressureZeroTrim', e.currentTarget.value)}
        />
      </label>

      <label>
        Gain Trim
        <input
          type="number"
          step="0.01"
          min="0.5"
          max="2"
          value={settings.pressureGainTrim}
          onChange={e => update('pressureGainTrim', e.currentTarget.value)}
        />
      </label>

      {error && <p>Error: {error}</p>}
    </section>
  )
}