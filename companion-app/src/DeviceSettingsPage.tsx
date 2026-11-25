import type { SoundSelection } from './synth/soundMenu'
import { TilesVisualizer } from './TilesVisualizer'
import { CustomScales } from './CustomScales'
import { GlobalDeviceSettings } from './GlobalDeviceSettings'
import './DeviceSettingsPage.css'

type Props = {
  connected: boolean
  connectionMessage?: string
  onReconnect: () => void
  onEditSound: (selection: SoundSelection) => void
}

export function DeviceSettingsPage({ connected, connectionMessage, onEditSound }: Props) {
  return <section className="device-settings-page">

    {!connected && connectionMessage &&
      <p className="snt-connection-note">{connectionMessage}</p>}
    <div className="snt-device-grid">
      <div className="snt-device-left">
        <div className="snt-panel snt-preview-panel">
          <TilesVisualizer onEditSound={onEditSound} />
        </div>
        <details className="snt-scales-details">
          <summary>Custom scales <span>Manage scale slots</span></summary>
          {connected ? <CustomScales /> : <p className="snt-muted">Connect TILES to edit custom scale slots.</p>}
        </details>
      </div>
      <aside className="snt-panel snt-global-panel">
        <GlobalDeviceSettings connected={connected} />
      </aside>
    </div>
  </section>
}
