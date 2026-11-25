import type { SoundSelection } from './synth/soundMenu'
import { TilesVisualizer } from './TilesVisualizer'
import { CustomScales } from './CustomScales'
import { GlobalDeviceSettings } from './GlobalDeviceSettings'
import type { ModeProjectTarget } from './modeNavigation'
import './DeviceSettingsPage.css'

type Props = {
  connected: boolean
  connectionMessage?: string
  onReconnect: () => void
  onEditSound: (selection: SoundSelection) => void
  onEditModeProject: (target: ModeProjectTarget) => void
}

export function DeviceSettingsPage({ connected, connectionMessage, onEditSound, onEditModeProject }: Props) {
  return <section className="device-settings-page">

    {!connected && connectionMessage &&
      <p className="snt-connection-note">{connectionMessage}</p>}
    <div className="snt-device-grid">
      <div className="snt-device-left">
          <TilesVisualizer onEditSound={onEditSound} onEditModeProject={onEditModeProject}/>
        <details className="snt-scales-details">
          <summary>Custom scales </summary>
          {connected ? <CustomScales /> : <p className="snt-muted">Connect TILES to edit custom scale slots.</p>}
        </details>
      </div>
      <aside className="snt-panel snt-global-panel">
        <GlobalDeviceSettings connected={connected} />
      </aside>
    </div>
  </section>
}
