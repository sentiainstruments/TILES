import type { SoundSelection } from './synth/soundMenu'
import { SynthWorkspace } from './synth/SynthWorkspace'
import { useEffect, useState } from 'react'
import { ProjectWorkspace } from './project/ProjectWorkspace'
import { DeviceSettingsPage } from './DeviceSettingsPage'

const TILES_IDS = [
  [0x1209, 0x0001],
  [0x2e8a, 0x100a],
]

function App() {
  const [connected, setConnected] = useState(false)
  const [error, setError] = useState('')
  const [simFirmware, setSimFirmware] = useState<string | null>(null)
  const [page, setPage] = useState<'workspace' | 'device' | 'synth'>('workspace')

  const [soundSelection, setSoundSelection] = useState<SoundSelection | null>(null)
  function editSound(selection: SoundSelection) {
    setSoundSelection(selection)
    setPage('synth')
  }

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
      <button type="button" aria-current={page === 'synth' ? 'page' : undefined}
        onClick={() => setPage('synth')}>Synth Workspace</button>
      <span style={{ marginLeft: 'auto', fontSize: 12, alignSelf: 'center' }}>
        {connected ? simFirmware ? `Simulator ${simFirmware}` : 'TILES connected' : 'Offline'}
      </span>
    </nav>
    {/* Keep the workspace mounted while switching tabs so edits remain staged. */}
    <div style={{ display: page === 'workspace' ? 'block' : 'none' }}>
      <ProjectWorkspace />
    </div>
    {page === 'device' && <section>
      {connected && simFirmware && <div className="snt-firmware-selector">
        <span>Virtual TILES · firmware {simFirmware}</span>
        <label>Simulated firmware{' '}
          <select value={simFirmware} onChange={e => void changeSimFirmware(e.currentTarget.value)}>
            <option value="0.2.0">0.2.0 — legacy</option>
            <option value="0.3.6">0.3.6 — main</option>
            <option value="alpha.6.5">Alpha 6.5 — previous</option>
            <option value="alpha.6.9">Alpha 6.9 — official main</option>
          </select>
        </label>
        <small>Changing simulated firmware resets the virtual board, not saved projects.</small>
      </div>}
      {connected && error && <p role="alert" className="snt-error" style={{ margin: 16 }}>{error}</p>}
      <div key={simFirmware ?? 'real-device'}>
        <DeviceSettingsPage connected={connected} connectionMessage={error}
          onReconnect={() => { void connect() }} onEditSound={editSound} />
      </div>
    </section>}
    <div style={{ display: page === 'synth' ? 'block' : 'none' }}>
      <SynthWorkspace connected={connected} simulated={simFirmware !== null} active={page === 'synth'} soundSelection={soundSelection} />
    </div>
  </main>
}
export default App
