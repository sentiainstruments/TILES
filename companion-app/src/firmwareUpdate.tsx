import { useEffect, useState, type ChangeEvent, type CSSProperties } from 'react'

type Uf2Summary = { name: string; bytes: number; blocks: number; family: string }

const MAGIC_0 = 0x0a324655
const MAGIC_1 = 0x9e5d5157
const MAGIC_END = 0x0ab16f30
const RP2350_FAMILY = 0xe48bff59

function inspectUf2(name: string, buffer: ArrayBuffer): Uf2Summary {
  if (!/\.uf2$/i.test(name)) throw new Error('Select a .uf2 firmware file.')
  if (!buffer.byteLength || buffer.byteLength > 16 * 1024 * 1024 || buffer.byteLength % 512 !== 0) {
    throw new Error('UF2 must contain complete 512-byte blocks and be 16 MB or smaller.')
  }

  const data = new DataView(buffer)
  const total = buffer.byteLength / 512
  const seen = new Set<number>()
  for (let index = 0; index < total; index++) {
    const base = index * 512
    const word = (at: number) => data.getUint32(base + at, true)
    if (word(0) !== MAGIC_0 || word(4) !== MAGIC_1 || word(508) !== MAGIC_END) {
      throw new Error(`Invalid UF2 header/footer in block ${index + 1}.`)
    }
    if (!(word(8) & 0x2000) || word(28) !== RP2350_FAMILY) {
      throw new Error('UF2 file does not identify the RP2350 chip family.')
    }
    const payload = word(16)
    if (payload < 1 || payload > 476 || word(12) < 0x10000000 ||
        word(12) + payload > 0x12000000 || word(24) !== total ||
        word(20) >= total || seen.has(word(20))) {
      throw new Error('UF2 contains invalid addresses, payload size, or block numbering.')
    }
    seen.add(word(20))
  }
  return { name, bytes: buffer.byteLength, blocks: total, family: 'RP2350' }
}

export function FirmwareUpdate() {
  const [file, setFile] = useState<Uf2Summary | null>(null)
  const [error, setError] = useState('')
  const [info, setInfo] = useState<Record<string, string>>({})
  const [loading, setLoading] = useState(true)

  useEffect(() => {
    let cancelled = false
    void window.tiles.usb.command('INFO', true).then(rows => {
      if (cancelled) return
      setInfo(Object.fromEntries(rows.filter(row => row.includes('=')).map(row => {
        const at = row.indexOf('=')
        return [row.slice(0, at), row.slice(at + 1)]
      })))
    }).catch(() => {
      if (!cancelled) setInfo({})
    }).finally(() => {
      if (!cancelled) setLoading(false)
    })
    return () => { cancelled = true }
  }, [])

  async function chooseFile(event: ChangeEvent<HTMLInputElement>) {
    setError('')
    setFile(null)
    const selected = event.currentTarget.files?.[0]
    if (!selected) return
    try {
      setFile(inspectUf2(selected.name, await selected.arrayBuffer()))
    } catch (cause) {
      setError(cause instanceof Error ? cause.message : String(cause))
    }
  }

  const card: CSSProperties = {
    border: '1px solid var(--border)', borderRadius: 12,
    padding: 22, display: 'grid', gap: 12,
  }
  return (
    <section style={{ maxWidth: 780, margin: '0 auto', padding: 28, display: 'grid', gap: 20, textAlign: 'left' }}>
      <header>
        <h2>Firmware Update</h2>
        <p>Inspect local firmware files for SENTIA TILES.</p>
      </header>
      <section style={card}>
        <h3>1. Instrument</h3>
        <p>{loading ? 'Reading device information…' : info.simulator === '1' ? 'Virtual TILES (simulator)' : info.firmware ? 'TILES connected' : 'Device information unavailable'}</p>
        <p>Current firmware: <strong>{info.firmware ?? 'Unknown'}</strong></p>
        <p>Unit: <strong>{info.unit ?? 'Unknown'}</strong></p>
      </section>
      <section style={card}>
        <h3>2. Select a .uf2 file</h3>
        <p>Use a trusted file built for the correct TILES unit. This checks the UF2 container, not its authenticity or device-specific firmware compatibility.</p>
        <input type="file" accept=".uf2" aria-label="Select TILES UF2 firmware" onChange={e => void chooseFile(e)} />
        {file && <>
          <p><strong>{file.name}</strong></p>
          <p>{(file.bytes / 1024).toFixed(1)} KB · {file.blocks} blocks · {file.family}</p>
        </>}
        {error && <p role="alert" style={{ color: '#e57373' }}>{error}</p>}
      </section>
      <section style={card}>
        <h3>3. Install</h3>
        <button type="button" disabled>Install Firmware (coming next)</button>
        <p>Flashing is intentionally disabled until the update workflow can be verified on physical hardware. Selecting a file does not reboot or modify the instrument.</p>
      </section>
    </section>
  )
}
