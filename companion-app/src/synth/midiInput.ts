// Local Web MIDI input only. Never sends USB commands or writes presets.
export type TilesMidiHandlers = {
  status: (message: string) => void;
  reset: () => void;
  noteOn: (channel: number, note: number, velocity: number) => void;
  noteOff: (channel: number, note: number) => void;
  pressure: (channel: number, value: number) => void;
  allNotesOff: (channel: number) => void;
};

// Reserve 0–127 for the on-screen/computer keyboard. MIDI channels are 0–15.
export function midiVoiceId(channel: number, note: number): number {
  return 128 + channel * 128 + note;
}

export function handleMidi(data: Uint8Array, h: TilesMidiHandlers): void {
  if (data.length < 2) return;
  const kind = data[0] & 0xf0;
  const channel = data[0] & 0x0f;
  const a = data[1] & 0x7f;
  const b = (data[2] ?? 0) & 0x7f;
  if (kind === 0x90 && data.length >= 3 && b > 0) h.noteOn(channel, a, b / 127);
  else if ((kind === 0x80 || kind === 0x90) && data.length >= 3) h.noteOff(channel, a);
  else if (kind === 0xd0) h.pressure(channel, a / 127);
  else if (kind === 0xb0 && data.length >= 3 && (a === 120 || a === 123)) h.allNotesOff(channel);
}

// Prefer the currently selected physical TILES port; reconnect after USB hotplug.
// Only one instrument is listened to at a time, avoiding cross-device channel collisions.
export function connectTilesMidi(h: TilesMidiHandlers): () => void {
  let disposed = false;
  let access: MIDIAccess | undefined;
  let input: MIDIInput | undefined;
  const onMessage = (event: MIDIMessageEvent) => {
    if (!disposed && event.data) handleMidi(event.data, h);
  };
  const detach = () => {
    if (input) {
      input.removeEventListener('midimessage', onMessage);
      input = undefined;
      h.reset();
    }
  };
  const refresh = () => {
    if (disposed || !access) return;
    if (input?.state === 'connected') return;
    detach();
    const candidates: MIDIInput[] = [];
    access.inputs.forEach(port => {
      if (port.state === 'connected' && /sentia\s+tiles/i.test(port.name ?? '')) candidates.push(port);
    });
    input = candidates[0];
    if (!input) {
      h.status('MIDI: waiting for TILES');
      return;
    }
    input.addEventListener('midimessage', onMessage);
    h.status(`MIDI: ${input.name ?? 'SENTIA TILES'}`);
  };
  if (typeof navigator.requestMIDIAccess !== 'function') {
    h.status('MIDI: unavailable in this renderer');
    return () => { disposed = true; };
  }
  h.status('MIDI: connecting…');
  void navigator.requestMIDIAccess().then(result => {
    if (disposed) return;
    access = result;
    access.addEventListener('statechange', refresh);
    refresh();
  }).catch(error => {
    if (!disposed) h.status(`MIDI: ${String(error)}`);
  });
  return () => {
    disposed = true;
    access?.removeEventListener('statechange', refresh);
    detach();
  };
}
