import { useEffect, useState } from 'react'
import { Pedal } from './pedal'
import { Cvgate } from './cvGate'
import { Expression } from './expression'
import { Midi } from './midi'
import { Lighting } from './lighting'
import { FeaturesHarmonics } from './featuresHarmonics'

const TILES_IDS = [
  [0x1209, 0x0001], // newer firmware
  [0x2e8a, 0x100a], // older firmware
]

function App() {
  const [connected, setConnected] = useState(false)
  const [error, setError] = useState('')

  async function connect() {
    try {
      setError('')
      setConnected(false)

      const devices = await window.tiles.usb.list()
      const tiles = devices.find(d =>
        TILES_IDS.some(([vid, pid]) =>
          d.vendorId === vid && d.productId === pid
        )
      )

      if (!tiles) throw new Error('SENTIA TILES not found')

      await window.tiles.usb.connect(tiles.vendorId, tiles.productId)
      setConnected(true)
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e))
    }
  }

  useEffect(() => {
    void connect()
  }, [])

  if (!connected) {
    return (
      <main>
        <h1>SENTIA TILES</h1>
        <p>{error || 'Connecting...'}</p>
        {error && <button onClick={connect}>Reconnect</button>}
      </main>
    )
  }

  return (
    <>
      <Pedal />
      <Cvgate />
      <Expression />
      <Midi />
      <Lighting />
      <FeaturesHarmonics />
    </>
  )
}

export default App