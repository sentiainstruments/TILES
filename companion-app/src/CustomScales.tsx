import { useCallback, useEffect, useState } from 'react'

type Scale = { slot: number; name: string; intervals: string; metadata: string }

function parseScale(line: string): Scale | null {
  const match = line.match(/^([1-9]) (\S+) (\d+(?:,\d+)*)(.*)$/)
  return match ? {
    slot: Number(match[1]),
    name: match[2],
    intervals: match[3],
    metadata: match[4].trim(),
  } : null
}

export function CustomScales() {
  const [supported, setSupported] = useState<boolean | null>(null)
  const [scales, setScales] = useState<Scale[]>([])
  const [slot, setSlot] = useState(1)
  const [name, setName] = useState('')
  const [intervals, setIntervals] = useState('0,2,4,5,7,9,11')
  const [message, setMessage] = useState('')
  const [busy, setBusy] = useState(false)

  const refresh = useCallback(async () => {
    const lines = await window.tiles.usb.command('SCALES', true)
    if (lines.some(line => line.startsWith('ERR '))) {
      setSupported(false)
      return
    }
    setSupported(true)
    setScales(lines.map(parseScale).filter((s): s is Scale => s !== null))
  }, [])

  useEffect(() => {
    void refresh().catch(reason => setMessage(String(reason)))
  }, [refresh])

  useEffect(() => {
    const scale = scales.find(s => s.slot === slot)
    setName(scale?.name.replace(/_/g, ' ') ?? '')
    setIntervals(scale?.intervals ?? '0,2,4,5,7,9,11')
  }, [scales, slot])

  async function save() {
    const safeName = name.trim().replace(/\s+/g, '_')
    if (!/^[!-~]{1,16}$/.test(safeName)) {
      setMessage('Name must be 1–16 ASCII characters (spaces become underscores).')
      return
    }
    const values = intervals.split(',').map(v => Number(v.trim()))
    if (!values.length || values.length > 12 || values[0] !== 0 ||
        values.some((v, i) => !Number.isInteger(v) || v > 11 || v < 0 ||
          (i > 0 && v <= values[i - 1]))) {
      setMessage('Use 1–12 ascending semitone offsets from 0 to 11, starting with 0.')
      return
    }
    setBusy(true)
    try {
      const [response] = await window.tiles.usb.command(
        `SCALE PUT ${slot} ${safeName} ${values.join(',')}`,
      )
      if (response !== 'OK') throw new Error(response)
      await refresh()
      window.dispatchEvent(new Event('tiles:scales-changed'))
      setMessage(`Saved custom scale to slot ${slot}.`)
    } catch (reason) {
      setMessage(`Couldn't save: ${String(reason)}`)
    } finally {
      setBusy(false)
    }
  }

  async function remove() {
    setBusy(true)
    try {
      const [response] = await window.tiles.usb.command(`SCALE DELETE ${slot}`)
      if (response !== 'OK') throw new Error(response)
      await refresh()
      window.dispatchEvent(new Event('tiles:scales-changed'))
      setMessage(`Cleared slot ${slot}.`)
    } catch (reason) {
      setMessage(`Couldn't delete: ${String(reason)}`)
    } finally {
      setBusy(false)
    }
  }

  return (
    <section>
      <h2>Custom scales</h2>
      {supported === null && <p>Checking custom scale support…</p>}
      {supported === false && <p>This firmware does not expose custom scales over USB.</p>}
      {supported === true && <>
        <p>Manage nine scale slots. To preview one, use the selector above the virtual instrument.</p>
        <label>Slot{' '}
          <select value={slot} onChange={e => setSlot(Number(e.currentTarget.value))}>
            {Array.from({ length: 9 }, (_, i) => (
              <option key={i} value={i + 1}>
                {i + 1}{scales.some(s => s.slot === i + 1) ? ` — ${scales.find(s => s.slot === i + 1)?.name.replace(/_/g, ' ')}` : ' — Empty'}
              </option>
            ))}
          </select>
        </label>
        <label>Name
          <input value={name} maxLength={16} placeholder="My scale"
            onChange={e => setName(e.currentTarget.value)} />
        </label>
        <label>Intervals (semitones above root)
          <input value={intervals} placeholder="0,2,4,5,7,9,11"
            onChange={e => setIntervals(e.currentTarget.value)} />
        </label>
        <button type="button" disabled={busy} onClick={() => void save()}>Save scale</button>
        <button type="button" disabled={busy || !scales.some(s => s.slot === slot)}
          onClick={() => void remove()}>Delete scale</button>
        {scales.some(s => s.slot === slot && !!s.metadata) &&
          <p>Saving here replaces the slot's existing pack metadata.</p>}
      </>}
      {message && <p role="status">{message}</p>}
    </section>
  )
}
