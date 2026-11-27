import { useEffect, useId, useRef, useState } from 'react'
import type { PointerEvent as ReactPointerEvent, KeyboardEvent as ReactKeyboardEvent } from 'react'
import { createPortal } from 'react-dom'
import './HardwareAudioMonitor.css'

type Session = { context: AudioContext; stream: MediaStream | null; gain: GainNode | null; frame: number }
const errorText = (e: unknown) => e instanceof Error ? `${e.name}: ${e.message}` : String(e)
const isTiles = (d: MediaDeviceInfo) => d.kind === 'audioinput' && /sentia.*tiles/i.test(d.label)
const dispose = (s: Session | null) => {
  if (!s) return
  cancelAnimationFrame(s.frame)
  s.stream?.getTracks().forEach(t => t.stop())
  if (s.context.state !== 'closed') void s.context.close().catch(() => {})
}

export function HardwareAudioMonitor({ onActiveChange }: { onActiveChange: (active: boolean) => void }) {
  // Keep the audio session mounted even when the floating controls are hidden.
  const panelId = useId()
  const [panelOpen, setPanelOpen] = useState(false)
  const [position, setPosition] = useState<{ x: number; y: number } | null>(null)
  const panelRef = useRef<HTMLElement>(null)
  const triggerRef = useRef<HTMLButtonElement>(null)
  const closeRef = useRef<HTMLButtonElement>(null)
  const drag = useRef<{ id: number; dx: number; dy: number } | null>(null)

  function clampPosition(x: number, y: number) {
    const rect = panelRef.current?.getBoundingClientRect()
    const w = rect?.width ?? 390, h = rect?.height ?? 280
    return {
      x: Math.max(8, Math.min(x, window.innerWidth - w - 8)),
      y: Math.max(8, Math.min(y, window.innerHeight - h - 8)),
    }
  }
  function closePanel() {
    drag.current = null
    setPanelOpen(false)
    triggerRef.current?.focus({ preventScroll: true })
  }
  useEffect(() => {
    if (!panelOpen) return
    const panel = panelRef.current
    if (!panel) return
    const fit = () => {
      const rect = panel.getBoundingClientRect()
      const next = clampPosition(rect.left, rect.top)
      setPosition(old => old?.x === next.x && old?.y === next.y ? old : next)
    }
    fit()
    closeRef.current?.focus({ preventScroll: true })
    window.addEventListener('resize', fit)
    const observer = new ResizeObserver(fit)
    observer.observe(panel)
    return () => {
      observer.disconnect()
      window.removeEventListener('resize', fit)
      drag.current = null
    }
  }, [panelOpen])

  function beginDrag(event: ReactPointerEvent<HTMLDivElement>) {
    if (event.button !== 0 || drag.current) return
    const rect = panelRef.current?.getBoundingClientRect()
    if (!rect) return
    drag.current = { id: event.pointerId, dx: event.clientX - rect.left, dy: event.clientY - rect.top }
    event.currentTarget.setPointerCapture(event.pointerId)
    event.preventDefault()
  }
  function moveDrag(event: ReactPointerEvent<HTMLDivElement>) {
    const currentDrag = drag.current
    if (!currentDrag || currentDrag.id !== event.pointerId) return
    setPosition(clampPosition(event.clientX - currentDrag.dx, event.clientY - currentDrag.dy))
  }
  function endDrag(event: ReactPointerEvent<HTMLDivElement>) {
    if (drag.current?.id !== event.pointerId) return
    drag.current = null
    if (event.currentTarget.hasPointerCapture(event.pointerId)) event.currentTarget.releasePointerCapture(event.pointerId)
  }
  function moveByKey(event: ReactKeyboardEvent<HTMLDivElement>) {
    const delta = { ArrowLeft: [-1, 0], ArrowRight: [1, 0], ArrowUp: [0, -1], ArrowDown: [0, 1] }[event.key as 'ArrowLeft' | 'ArrowRight' | 'ArrowUp' | 'ArrowDown']
    if (!delta) return
    const rect = panelRef.current?.getBoundingClientRect()
    if (!rect) return
    event.preventDefault()
    const step = event.shiftKey ? 30 : 10
    setPosition(clampPosition(rect.left + delta[0] * step, rect.top + delta[1] * step))
  }

  const [devices, setDevices] = useState<MediaDeviceInfo[]>([])
  const [deviceId, setDeviceId] = useState('')
  const [state, setState] = useState<'off' | 'starting' | 'on'>('off')
  const [volume, setVolume] = useState(50)
  const [levels, setLevels] = useState<[number, number]>([0, 0])
  const [detail, setDetail] = useState('')
  const [error, setError] = useState('')
  const [discovering, setDiscovering] = useState(false)
  const current = useRef<Session | null>(null)
  const generation = useRef(0)
  const mounted = useRef(false)
  const callback = useRef(onActiveChange)
  callback.current = onActiveChange
  const volumeRef = useRef(volume)
  volumeRef.current = volume

  async function scan() {
    const all = await navigator.mediaDevices.enumerateDevices()
    const tiles = all.filter(isTiles)
    if (!mounted.current) return tiles
    setDevices(tiles)
    setDeviceId(old => tiles.some(d => d.deviceId === old) ? old : tiles.find(d => d.deviceId !== 'default' && d.deviceId !== 'communications')?.deviceId ?? tiles[0]?.deviceId ?? '')
    return tiles
  }

  function stop(reason = '') {
    generation.current++
    const old = current.current
    current.current = null
    dispose(old)
    if (mounted.current) {
      setState('off'); setLevels([0, 0]); setDetail(''); setError(reason)
      callback.current(false)
    }
  }

  useEffect(() => {
    mounted.current = true
    const changed = () => { void scan().then(list => {
      const track = current.current?.stream?.getAudioTracks()[0]
      if (track && !list.some(d => d.deviceId === track.getSettings().deviceId) && track.readyState === 'ended') stop('TILES audio disconnected.')
    }).catch(e => { if (mounted.current) setError(errorText(e)) }) }
    if (navigator.mediaDevices) {
      changed(); navigator.mediaDevices.addEventListener('devicechange', changed)
    } else setError('Audio input is unavailable in this Electron renderer.')
    return () => {
      mounted.current = false; generation.current++
      navigator.mediaDevices?.removeEventListener('devicechange', changed)
      dispose(current.current); current.current = null
      callback.current(false)
    }
  }, [])

  // Chromium may hide labels until audio-input permission is granted.
  // This explicit button briefly opens the default input for enumeration only;
  // it is never connected to speakers and its tracks are always released.
  async function discover() {
    if (discovering || state !== 'off') return
    setDiscovering(true); setError('')
    let temporary: MediaStream | null = null
    try {
      const found = await scan()
      if (!found.length) {
        temporary = await navigator.mediaDevices.getUserMedia({ audio: true, video: false })
        const next = await scan()
        if (!next.length && mounted.current) setError('No SENTIA TILES audio input found. Check standalone mode and macOS microphone permissions.')
      }
    } catch (e) { if (mounted.current) setError(errorText(e)) }
    finally {
      temporary?.getTracks().forEach(t => t.stop())
      if (mounted.current) setDiscovering(false)
    }
  }

  async function start() {
    if (state !== 'off' || !devices.some(d => d.deviceId === deviceId && isTiles(d))) return
    const token = ++generation.current
    setState('starting'); setError(''); callback.current(true)
    let session: Session | null = null
    try {
      // Resume directly from the user gesture before waiting for capture.
      const context = new AudioContext()
      session = { context, stream: null, gain: null, frame: 0 }
      current.current = session
      await context.resume()
      if (generation.current !== token) return
      const stream = await navigator.mediaDevices.getUserMedia({ video: false, audio: {
        deviceId: { exact: deviceId }, channelCount: { ideal: 2 }, sampleRate: { ideal: 48000 },
        echoCancellation: false, noiseSuppression: false, autoGainControl: false,
      } })
      if (generation.current !== token || !mounted.current) { stream.getTracks().forEach(t => t.stop()); return }
      session.stream = stream
      const track = stream.getAudioTracks()[0]
      if (!track || !/sentia.*tiles/i.test(track.label)) throw new Error('Refusing to monitor a non-TILES input.')
      const settings = track.getSettings()
      const input = context.createMediaStreamSource(stream)
      const gain = context.createGain(); session.gain = gain
      gain.gain.value = volumeRef.current / 100
      input.connect(gain); gain.connect(context.destination)
      const splitter = context.createChannelSplitter(2)
      input.connect(splitter)
      const analysers = [context.createAnalyser(), context.createAnalyser()]
      for (let i = 0; i < 2; i++) { analysers[i].fftSize = 1024; splitter.connect(analysers[i], i) }
      const buffers = analysers.map(a => new Float32Array(a.fftSize))
      let last = 0
      const tick = (now: number) => {
        if (generation.current !== token || !session) return
        if (now - last >= 65) {
          last = now
          const meter = analysers.map((a, i) => {
            a.getFloatTimeDomainData(buffers[i]); let peak = 0
            for (const sample of buffers[i]) peak = Math.max(peak, Math.abs(sample))
            const db = 20 * Math.log10(Math.max(peak, .00001))
            return Math.max(0, Math.min(100, (db + 60) / 60 * 100))
          })
          setLevels([meter[0], meter[1]])
        }
        session.frame = requestAnimationFrame(tick)
      }
      track.addEventListener('ended', () => { if (generation.current === token) stop('TILES audio disconnected.') }, { once: true })
      setDetail(`${settings.sampleRate ?? context.sampleRate} Hz · ${settings.channelCount === 1 ? 'Mono' : settings.channelCount === 2 ? 'Stereo' : 'Channels not reported'}`)
      setState('on'); session.frame = requestAnimationFrame(tick)
    } catch (e) {
      if (generation.current === token) stop(errorText(e))
      else dispose(session)
    }
  }

  function changeVolume(value: number) {
    setVolume(value)
    const session = current.current
    if (session?.gain) session.gain.gain.setTargetAtTime(value / 100, session.context.currentTime, .025)
  }

  return <>
    <button ref={triggerRef} type="button" className="snt-hw-floating-trigger"
      aria-haspopup="dialog" aria-expanded={panelOpen} aria-controls={panelOpen ? panelId : undefined}
      title={state === 'on' ? 'Hardware audio is monitoring' : 'Open hardware audio monitor'}
      onClick={() => panelOpen ? closePanel() : setPanelOpen(true)}>
      <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" strokeWidth="1.7" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true"><path d="M3 14v-2a9 9 0 0 1 18 0v2M3 14h3a2 2 0 0 1 2 2v3H6a3 3 0 0 1-3-3v-2Zm18 0h-3a2 2 0 0 0-2 2v3h2a3 3 0 0 0 3-3v-2Z" /></svg>
      <span>Monitor</span><span className={`snt-hw-live-dot ${state === 'on' ? 'is-live' : state === 'starting' ? 'is-starting' : ''}`} aria-label={state === 'on' ? 'Monitoring active' : state === 'starting' ? 'Opening audio' : 'Monitoring off'} />
    </button>
    {panelOpen && createPortal(<section ref={panelRef} id={panelId} className="snt-hw-monitor snt-hw-floating-panel"
      role="dialog" aria-modal="false" aria-labelledby={`${panelId}-title`}
      style={position ? { left: position.x, top: position.y } : { right: 16, top: 90 }}
      onKeyDown={event => { if (event.key === 'Escape') { event.stopPropagation(); closePanel() } }}>
      <div className="snt-hw-floating-titlebar">
        <div className="snt-hw-drag-handle" tabIndex={0} role="button" aria-label="Move hardware audio panel with arrow keys" title="Drag to move; arrow keys also move the panel"
          onPointerDown={beginDrag} onPointerMove={moveDrag} onPointerUp={endDrag} onPointerCancel={endDrag} onLostPointerCapture={endDrag} onKeyDown={moveByKey}>
          <span aria-hidden="true" className="snt-hw-grip">⠿</span><strong id={`${panelId}-title`}>Hardware audio</strong>
        </div>
        <button ref={closeRef} className="snt-hw-close" type="button" onClick={closePanel} title="Hide panel (keep monitoring active)" aria-label="Hide hardware audio panel">×</button>
      </div>
    <div className="snt-hw-monitor-heading">
      <span role="status">{state === 'on' ? 'Monitoring' : state === 'starting' ? 'Opening stream…' : 'Off'}</span>
      <button type="button" onClick={() => state === 'off' ? void start() : stop()}
        disabled={state === 'off' && (!deviceId || discovering)}>{state === 'off' ? 'Monitor TILES' : 'Stop monitoring'}</button>
    </div>
    <div className="snt-hw-monitor-input">
      <select aria-label="TILES audio input" value={deviceId} disabled={state !== 'off' || discovering} onChange={e => setDeviceId(e.currentTarget.value)}>
        <option value="">Select TILES input</option>
        {devices.map(d => <option key={d.deviceId} value={d.deviceId}>{d.label}</option>)}
      </select>
      <button type="button" disabled={state !== 'off' || discovering} onClick={() => void discover()} title="If labels are hidden, briefly request audio-input permission without monitoring the default microphone">{discovering ? 'Finding…' : 'Find audio input'}</button>
    </div>
    {detail && <small>{detail} · Default Mac output</small>}
    {state === 'on' && <>
      <div className="snt-hw-meter" aria-label="Left and right input peak meters">
        {levels.map((level, i) => <div key={i}><span>{i === 0 ? 'L' : 'R'}</span><div className="snt-hw-meter-track"><div style={{ width: `${level}%` }} /></div></div>)}
      </div>
      <label className="snt-hw-volume">Volume <input type="range" min="0" max="100" value={volume} onChange={e => changeVolume(Number(e.currentTarget.value))} /><span>{volume}%</span></label>
    </>}
    {error && <small className="snt-hw-error" role="alert">{error}</small>}
    {state === 'off' && !error && <small>Listen to the onboard synth and drum sampler through your Mac. Local synth playback is paused while monitoring.</small>}
    </section>, document.body)}
  </>
}
