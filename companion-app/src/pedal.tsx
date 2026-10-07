import { useEffect, useState } from 'react'

type PedalSettings = {
  mode: string
  polarity: string
  sustainStyle: string
}

export function Pedal() {
  const [settings, setSettings] = useState<PedalSettings>({
    mode: '',
    polarity: '',
    sustainStyle: '',
  })

  const [error, setError] = useState('')

  async function get(key: string) {
    const [response] = await window.tiles.usb.command(`GET ${key}`)
    if (response.startsWith('ERR ')) throw new Error(response)
    return response
  }

  async function set(key: string, value: string) {
    const [response] = await window.tiles.usb.command(`SET ${key} ${value}`)
    if (response !== 'OK') throw new Error(response)
  }

  useEffect(() => {
    Promise.all([
      get('pedal.mode'),
      get('pedal.polarity'),
      get('pedal.sustain_style'),
    ])
      .then(([mode, polarity, sustainStyle]) =>
        setSettings({ mode, polarity, sustainStyle }),
      )
      .catch(error => setError(String(error)))
  }, [])

  async function update(key: keyof PedalSettings, firmwareKey: string, value: string) {
    try {
      await set(firmwareKey, value)
      setSettings(current => ({ ...current, [key]: value }))
      setError('')
    } catch (error) {
      setError(String(error))
    }
  }

  return (
    <section>
      <h2>Pedal</h2>

      <label>
        Mode
        <select
          value={settings.mode}
          onChange={e =>
            update('mode', 'pedal.mode', e.currentTarget.value)
          }
        >
          <option value="sustain">Sustain</option>
          <option value="expression">Expression</option>
        </select>
      </label>

      <label>
        Polarity
        <select
          value={settings.polarity}
          onChange={e =>
            update('polarity', 'pedal.polarity', e.currentTarget.value)
          }
        >
          <option value="normally_open">Normally Open</option>
          <option value="normally_closed">Normally Closed</option>
        </select>
      </label>

      <label>
        Sustain Style
        <select
          value={settings.sustainStyle}
          onChange={e =>
            update('sustainStyle', 'pedal.sustain_style', e.currentTarget.value)
          }
        >
          <option value="synth">Synth</option>
          <option value="hold">Hold</option>
        </select>
      </label>

      {error && <p>{error}</p>}
    </section>
  )
}