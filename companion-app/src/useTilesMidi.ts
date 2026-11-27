import { useEffect, useState } from 'react'

// MIDI notes are tracked by channel as well as pitch for MPE/polyphony.
export function updateHeldNotes(previous: ReadonlySet<number>, data: Uint8Array): ReadonlySet<number> {
  if (data.length < 3) return previous
  const status = data[0] & 0xf0
  const channel = data[0] & 0x0f
  const note = data[1]
  const value = data[2]
  if (note > 127 || value > 127) return previous
  const key = channel * 128 + note
  if (status === 0x90 && value > 0) {
    if (previous.has(key)) return previous
    return new Set([...previous, key])
  }
  if (status === 0x80 || (status === 0x90 && value === 0)) {
    if (!previous.has(key)) return previous
    const next = new Set(previous)
    next.delete(key)
    return next
  }
  if (status === 0xb0 && (note === 120 || note === 123)) {
    return new Set([...previous].filter(k => Math.floor(k / 128) !== channel))
  }
  return previous
}

export function useTilesMidi(enabled: boolean) {
  const [ports, setPorts] = useState<MIDIInput[]>([])
  const [portId, setPortId] = useState('')
  const [held, setHeld] = useState<ReadonlySet<number>>(() => new Set())
  const [accessError, setAccessError] = useState('')
  const [inputError, setInputError] = useState('')
  const [ready, setReady] = useState(false)
  const [opened, setOpened] = useState(false)

  useEffect(() => {
    if (!enabled) return
    let cancelled = false
    let access: MIDIAccess | undefined
    setReady(false)
    setAccessError('')
    setPorts([])
    const refresh = () => {
      if (cancelled || !access) return
      const found: MIDIInput[] = []
      access.inputs.forEach(input => {
        if (input.state === 'connected' && /tiles/i.test(input.name ?? '')) found.push(input)
      })
      setPorts(found)
    }
    if (!navigator.requestMIDIAccess) {
      setAccessError('Web MIDI is unavailable in this window')
      return
    }
    navigator.requestMIDIAccess({ sysex: false }).then(result => {
      if (cancelled) return
      access = result
      refresh()
      access.addEventListener('statechange', refresh)
      setReady(true)
    }).catch(error => {
      if (!cancelled) setAccessError(String(error))
    })
    return () => {
      cancelled = true
      access?.removeEventListener('statechange', refresh)
    }
  }, [enabled])

  const input = enabled ? ports.find(port => port.id === portId) : undefined
  useEffect(() => {
    setHeld(new Set())
    setInputError('')
    setOpened(false)
    if (!input) return
    let cancelled = false
    const receive = (event: MIDIMessageEvent) => {
      const data = event.data
      if (!cancelled && data) setHeld(previous => updateHeldNotes(previous, data))
    }
    const clear = () => setHeld(new Set())
    const stateChanged = () => { if (input.state === 'disconnected') clear() }
    input.addEventListener('midimessage', receive)
    input.addEventListener('statechange', stateChanged)
    // Avoid stuck highlights if the OS suspends delivery while the window loses focus.
    window.addEventListener('blur', clear)
    input.open().then(() => {
      if (!cancelled) setOpened(true)
    }).catch(error => {
      if (!cancelled) setInputError(String(error))
    })
    return () => {
      cancelled = true
      input.removeEventListener('midimessage', receive)
      input.removeEventListener('statechange', stateChanged)
      window.removeEventListener('blur', clear)
      // This hook owns this MIDIAccess instance and input subscription.
      void input.close().catch(() => {})
    }
  }, [input])

  const activeNotes = new Set(enabled && input ? [...held].map(key => key % 128) : [])
  return { ports, portId, setPortId, activeNotes, ready, opened: !!input && opened,
    error: accessError || inputError }
}
