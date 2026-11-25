"""Turns an audio file into what a TILES drum slot holds, the way the
computer side of a hardware sampler does before sending: 16-bit at 48 kHz
(played at its own pitch with no resampling on the device), mono or
stereo, silence trimmed, capped at 2 s mono / 1 s stereo with a fade.
Plain Python (no numpy); used by tiles_control.py `sample put`.
Format and caps: firmware/src/audio/sample_slot.h."""

import array
import math
import operator
import os
import random
import re
import struct
import zlib

RATE = 48000
MAX_FRAMES = {1: 96000, 2: 48000}
NAME_MAX = 23
SILENCE = 10 ** (-60 / 20)  # trimmed below -60 dBFS
END_FADE_S = 0.010          # a cut at the cap fades over this
NORMALIZE_PEAK = 10 ** (-0.3 / 20)


class SampleError(Exception):
    pass


def read_wav(path):
    """(rate, bits, [channel samples as floats in -1..1]). PCM 8/16/24/32-bit
    and 32/64-bit float, plain or WAVE_FORMAT_EXTENSIBLE."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise SampleError("not a WAV file (export it as WAV)")
    fmt = pcm = None
    pos = 12
    while pos + 8 <= len(data):
        cid = data[pos:pos + 4]
        size = struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            fmt = body
        elif cid == b"data":
            pcm = body
        pos += 8 + size + (size & 1)
    if fmt is None or pcm is None:
        raise SampleError("WAV file without fmt/data chunks")
    tag, channels, rate, _, block, bits = struct.unpack("<HHIIHH", fmt[:16])
    if tag == 0xFFFE and len(fmt) >= 26:
        tag = struct.unpack("<H", fmt[24:26])[0]
    frames = len(pcm) // block
    pcm = pcm[:frames * block]
    if tag == 1 and bits == 16:
        flat = array.array("h", pcm)
        scale = 1 / 32768
    elif tag == 1 and bits == 32:
        flat = array.array("i", pcm)
        scale = 1 / 2147483648
    elif tag == 1 and bits == 24:
        flat = [int.from_bytes(pcm[i:i + 3], "little", signed=True) for i in range(0, len(pcm), 3)]
        scale = 1 / 8388608
    elif tag == 1 and bits == 8:
        flat = [b - 128 for b in pcm]
        scale = 1 / 128
    elif tag == 3 and bits == 32:
        flat = array.array("f", pcm)
        scale = 1.0
    elif tag == 3 and bits == 64:
        flat = array.array("d", pcm)
        scale = 1.0
    else:
        raise SampleError(f"unsupported WAV encoding (format {tag}, {bits}-bit)")
    chans = [[v * scale for v in flat[c::channels]] for c in range(channels)]
    return rate, (bits if tag == 1 else 32), chans


def _kernel(t, fc, width):
    """Blackman-windowed sinc, low-pass at fc (of the input's Nyquist)."""
    if abs(t) >= width:
        return 0.0
    s = fc if t == 0 else math.sin(math.pi * fc * t) / (math.pi * t)
    x = math.pi * t / width
    return s * (0.42 + 0.5 * math.cos(x) + 0.08 * math.cos(2 * x))


def resample(x, rate_in, rate_out, half_taps=16, phases=512):
    """Band-limited resampling: a windowed sinc, its phases cached."""
    if rate_in == rate_out:
        return list(x)
    step = rate_in / rate_out
    fc = min(1.0, rate_out / rate_in) * 0.94
    width = int(math.ceil(half_taps / min(1.0, rate_out / rate_in)))
    pad = [0.0] * (width + 1)
    xp = pad + list(x) + pad
    n_out = int(len(x) / step)
    taps = 2 * width
    cache = {}
    out = [0.0] * n_out
    for i in range(n_out):
        c = i * step
        base = int(c)
        ph = int((c - base) * phases + 0.5)
        if ph == phases:
            ph = 0
            base += 1
        vec = cache.get(ph)
        if vec is None:
            f = ph / phases
            vec = cache[ph] = [_kernel(f + width - 1 - j, fc, width) for j in range(taps)]
        s = base + 2  # input base - width + 1, past the padding
        out[i] = sum(map(operator.mul, vec, xp[s:s + taps]))
    return out


def safe_name(text):
    name = re.sub(r"[^A-Za-z0-9_.-]+", "-", text).strip("-")[:NAME_MAX]
    return name or "sample"


def prepare(path, mono=False, normalize=False, keep_silence=False, name=None):
    """(channels, frames, audio bytes, name, notes): the slot's contents."""
    rate, bits, chans = read_wav(path)
    notes = [f"{rate / 1000:g} kHz {bits}-bit {'mono' if len(chans) == 1 else f'{len(chans)}-ch'}"]
    if not chans or not chans[0]:
        raise SampleError("the file holds no audio")
    if len(chans) > 2:
        chans = chans[:2]
        notes.append("first two channels")
    changed = bits != 16 or rate != RATE
    if len(chans) == 2:
        if mono:
            chans = [[(a + b) * 0.5 for a, b in zip(*chans)]]
            notes.append("mixed to mono")
            changed = True
        elif chans[0] == chans[1]:
            chans = chans[:1]
            notes.append("both sides identical: stored mono")
    channels = len(chans)

    if not keep_silence:
        loud = [i for i in range(len(chans[0])) if any(abs(c[i]) > SILENCE for c in chans)]
        if not loud:
            raise SampleError("the file is silent (below -60 dBFS)")
        first, last = loud[0], loud[-1] + 1
        trimmed_ms = (first + len(chans[0]) - last) * 1000 / rate
        if trimmed_ms >= 1:
            notes.append(f"silence trimmed ({trimmed_ms:.0f} ms)")
        chans = [c[first:last] for c in chans]

    cap = MAX_FRAMES[channels]
    # Only what can be kept is resampled.
    keep_in = int(math.ceil(cap * rate / RATE)) + 64
    chans = [c[:keep_in] for c in chans]
    if rate != RATE:
        chans = [resample(c, rate, RATE) for c in chans]
        notes.append("resampled to 48 kHz")
    frames = len(chans[0])
    if frames > cap:
        fade = int(END_FADE_S * RATE)
        chans = [c[:cap] for c in chans]
        for c in chans:
            for i in range(fade):
                c[cap - fade + i] *= 1 - (i + 1) / fade
        frames = cap
        hint = " (--mono keeps 2 s)" if channels == 2 else ""
        notes.append(f"cut to {cap / RATE:g} s with a fade{hint}")
        changed = True
    if frames == 0:
        raise SampleError("nothing left after trimming")

    if normalize:
        peak = max(max(abs(v) for v in c) for c in chans)
        if peak > 0:
            gain = NORMALIZE_PEAK / peak
            chans = [[v * gain for v in c] for c in chans]
            notes.append(f"normalized ({20 * math.log10(gain):+.1f} dB to -0.3 dBFS)")
            changed = True

    if array.array("h", [1]).tobytes() != b"\x01\x00":
        raise SampleError("this computer isn't little-endian; not supported")
    out = array.array("h", bytes(frames * channels * 2))
    rnd = random.Random(1)
    for ch, c in enumerate(chans):
        for i, v in enumerate(c):
            x = v * 32768
            if changed:
                x += rnd.random() - rnd.random()  # TPDF dither down to 16 bits
            q = int(math.floor(x + 0.5))
            out[i * channels + ch] = 32767 if q > 32767 else -32768 if q < -32768 else q
    name = safe_name(name or os.path.splitext(os.path.basename(path))[0])
    return channels, frames, out.tobytes(), name, notes


def crc32(data):
    return zlib.crc32(data) & 0xFFFFFFFF
