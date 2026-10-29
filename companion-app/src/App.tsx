import { useEffect, useState } from 'react'
import { Pedal } from './pedal'
import { Cvgate } from './cvGate'
import { Expression } from './expression'
import { Midi } from './midi'
import { Lighting } from './lighting'
import { FeaturesHarmonics } from './featuresHarmonics'
import { TilesVisualizer } from './TilesVisualizer'
import { CustomScales } from './CustomScales'
import { FirmwareControls } from './FirmwareControls'
import { ProjectWorkspace } from './project/ProjectWorkspace'

const TILES_IDS = [
  [0x1209, 0x0001],
  [0x2e8a, 0x100a],
]

function App() {
  const [connected, setConnected] = useState(false)
  const [error, setError] = useState('')
  const [simFirmware, setSimFirmware] = useState<string | null>(null)
  const [page, setPage] = useState<'workspace' | 'device'>('workspace')

  async function connect() {
    try {
      setError('')
      setConnected(false)
      const devices = await window.tiles.usb.list()
      const tiles = devices.find(device =>
        TILES_IDS.some(([vid, pid]) => device.vendorId === vid && device.productId === pid))
      if (!tiles) throw new Error('SENTIA TILES not found. You can still edit projects offline.')
      await window.tiles.usb.connect(tiles.vendorId, tiles.productId)
      const info = await window.tiles.usb.command('INFO', true)
      const firmware = info.find(line => line.startsWith('firmware='))?.slice(9) ?? 'unknown'
      setSimFirmware(info.includes('simulator=1') ? firmware : null)
      setConnected(true)
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : String(reason))
    }
  }

  async function changeSimFirmware(next: string) {
    try {
      setError('')
      const [reply] = await window.tiles.usb.command(`SIM VERSION ${next}`)
      if (reply !== 'OK') throw new Error(reply)
      setSimFirmware(next)
    } catch (reason) {
      setError(String(reason))
    }
  }

  useEffect(() => { void connect() }, [])

  return <main>
    <nav style={{ display: 'flex', gap: 12, padding: '14px 22px',
      borderBottom: '1px solid var(--border)', textAlign: 'left' }} aria-label="Companion app">
      <button type="button" aria-current={page === 'workspace' ? 'page' : undefined}
        onClick={() => setPage('workspace')}>Project Workspace</button>
      <button type="button" aria-current={page === 'device' ? 'page' : undefined}
        onClick={() => setPage('device')}>Device Settings</button>
      <span style={{ marginLeft: 'auto', fontSize: 12, alignSelf: 'center' }}>
        {connected ? simFirmware ? `Simulator ${simFirmware}` : 'TILES connected' : 'Offline'}
      </span>
    </nav>
    {/* Keep the workspace mounted while switching tabs so edits remain staged. */}
    <div style={{ display: page === 'workspace' ? 'block' : 'none' }}>
      <ProjectWorkspace />
    </div>
    {page === 'device' && <section>
      {!connected ? <div style={{ padding: 32 }}>
        <h2>Device not connected</h2><p>{error}</p>
        <button onClick={() => void connect()}>Reconnect</button>
      </div> : <>
        {simFirmware && <div style={{ padding: 18 }}>
          <h2>Virtual TILES · firmware {simFirmware}</h2>
          <label>Simulated firmware{' '}
            <select value={simFirmware} onChange={e => void changeSimFirmware(e.currentTarget.value)}>
              <option value="0.2.0">0.2.0 — legacy</option>
              <option value="0.3.6">0.3.6 — main</option>
              <option value="alpha.6.5">Alpha 6.5 — board-bringup</option>
            </select>
          </label>
          <p>Switching firmware versions resets the simulated device, not saved project files.</p>
        </div>}
        {error && <p role="alert">{error}</p>}
        <div key={simFirmware ?? 'real-device'}>
          <TilesVisualizer />
          <Lighting />
          <CustomScales />
          <FirmwareControls />
          <Pedal />
          <Cvgate />
          <Expression />
          <Midi />
          <FeaturesHarmonics />
        </div>
      </>}
    </section>}
  </main>
}
export default App
