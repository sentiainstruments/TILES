import { useState } from 'react'
import {
  CURVE_PRESETS,
  ResponseCurveEditor,
} from './controls/ResponseCurveEditor'
import type { ResponseCurve } from './controls/ResponseCurveEditor'
import { SchemaSettings } from './SchemaSettings'

type GlobalProps = {
  connected: boolean
}

export function GlobalDeviceSettings({ connected }: GlobalProps) {
  const [velocity, setVelocity] = useState<ResponseCurve>({
    ...CURVE_PRESETS.linear,
  })

  const [aftertouch, setAftertouch] = useState<ResponseCurve>({
    ...CURVE_PRESETS.linear,
  })

  return (
    <div className="snt-global-settings">

      <div className="snt-panel-title-row">
        <h2>Global Settings</h2>
      </div>

      {connected ? (
        <SchemaSettings />
      ) : (
        <p className="snt-system-message">
          Connect TILES to edit device settings.
        </p>
      )}

      <section className="snt-settings-section">
        <div className="snt-section-label">
          <h3>Response curves</h3>
        </div>

        <div className="snt-response-curve-grid">
          <ResponseCurveEditor
            label="Velocity curve"
            description="Initial strike response"
            value={velocity}
            onChange={setVelocity}
          />

          <ResponseCurveEditor
            label="Aftertouch curve"
            description="Continuous pressure response"
            value={aftertouch}
            onChange={setAftertouch}
          />
        </div>
      </section>

    </div>
  )
}