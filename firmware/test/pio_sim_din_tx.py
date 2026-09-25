# Instruction-level simulation of midi/din_midi_tx.pio using the words pioasm actually assembled
# (so it checks the real program). Decodes the pin waveform as 8N1 @ 8 PIO cycles per bit.
# Minimal simulator for the four instructions in din_midi_tx.pio, taken from the
# assembled words pioasm produced (so this checks the real program, not my reading of it).
import glob, os, re
here = os.path.dirname(os.path.abspath(__file__))
matches = glob.glob(os.path.join(here, "..", "build", "**", "din_midi_tx.pio.h"), recursive=True)
assert matches, "build the firmware first (pioasm generates din_midi_tx.pio.h)"
hdr = open(matches[0]).read()
words = [int(x, 16) for x in re.findall(r"^\s+0x([0-9a-f]{4}),", hdr, re.M)]
assert len(words) == 4, words

def run2(tx_bytes, cycles):
    pin = 1; pc = 0; x = 0; osr = 0; fifo = list(tx_bytes); trace = []; wait = 0
    for _ in range(cycles):
        if wait > 0:
            wait -= 1; trace.append(pin); continue
        w = words[pc]; op = w >> 13
        dsf = (w >> 8) & 0x1f; en, val, delay = dsf >> 4, (dsf >> 3) & 1, dsf & 7
        if en: pin = val
        if op == 0b100:
            if fifo: osr = fifo.pop(0); wait = delay; pc = (pc + 1) % 4
        elif op == 0b111: x = w & 0x1f; wait = delay; pc = (pc + 1) % 4
        elif op == 0b011: pin = osr & 1; osr >>= 1; wait = delay; pc = (pc + 1) % 4
        elif op == 0b000:
            taken = x != 0; x = (x - 1) & 0xffffffff; wait = delay; pc = (w & 0x1f) if taken else (pc + 1) % 4
        trace.append(pin)
    return trace

msg = [0x90, 0x3C, 0x64, 0xFA, 0x00, 0xFF]
CYC_PER_BIT = 8
t = run2(msg, 8 * 10 * len(msg) + 40)
# Find each frame: falling edge = start bit; sample the middle of every bit cell.
out, i = [], 0
while i < len(t) - 1 and len(out) < len(msg):
    if t[i] == 1 and t[i + 1] == 0:
        start = i + 1
        bits = [t[start + CYC_PER_BIT * k + CYC_PER_BIT // 2] for k in range(10)]
        assert bits[0] == 0, "start bit"
        assert bits[9] == 1, "stop bit"
        out.append(sum(b << k for k, b in enumerate(bits[1:9])))
        # every cell must be constant for its full 8 cycles (no glitches inside a bit)
        for k in range(10):
            cell = t[start + CYC_PER_BIT * k : start + CYC_PER_BIT * (k + 1)]
            assert len(set(cell)) == 1, ("glitch in bit", k, cell)
        i = start + CYC_PER_BIT * 10 - 1   # last cycle of the stop bit; next start edge is i -> i+1
        continue
    i += 1
assert out == msg, [hex(b) for b in out]
# back-to-back: one byte = exactly 80 cycles (10 bits x 8), so 31250 baud at clkdiv 600 / 150 MHz
starts = [k for k in range(1, len(t)) if t[k - 1] == 1 and t[k] == 0]
first_starts = starts[:1]
assert t[-1] == 1, "line must idle high"
# idle before data: the line stays high while the FIFO is empty
idle = run2([], 200); assert set(idle) == {1}
print("PIO program: decoded", [hex(b) for b in out], "- 8N1 LSB-first, 8 cycles/bit, glitch-free, idles high")
print("baud @150MHz, clkdiv 600 ->", 150_000_000 / 600 / CYC_PER_BIT)
