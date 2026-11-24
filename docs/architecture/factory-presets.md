# Factory presets: the sound design (planning)

Status: **pages 2-4 built, 2026-11-23** (below): every page holds factory sounds (95; page 1's last slot is free). The owner's brief: high-quality sounds
for techno, hard techno, EDM, dubstep, pads, plucks, melodic and harmonic
playing, with a sound of our own. The slots, store and browsing are
decided in `sound-roadmap.md` ("Presets: 96 slots in 4 pages"); this note
is what goes in them and what the engines need first. Nothing here is
decided yet unless marked.

## The Sentia sound (proposal)

- **Pressure is part of every sound.** Every preset gives pressure a job
  (opens, thickens, drives, moves the wave, adds grains), slide a colour,
  bend the pitch. A preset that ignores the hand isn't finished; this is
  what no other instrument's presets do.
- **Glossy by default, grit per patch** (decided 2026-11-14): clean and
  tightly tuned, space from the plate and the ping-pong delay; drive,
  fold and crush where the genre wants them.
- **Built for the grid:** a chord on one track stays clear (pads keep
  their lows in check), basses are mono or legato with glide, leads
  glide too.
- **Named in our own words:** one or two plain words, at most 10
  characters; never an instrument, an artist, a track or a product. Two
  factory names break this today and get new ones in the preset pass:
  *hoover* (a brand) and *reese* (an artist's alias).

## The pages (proposal)

The same row is the same role on every page, so the hand learns one map:
row 1 basses, row 2 chords and stabs, row 3 leads, plucks and arps,
row 4 pads, textures and effects.

| Page | Genre | Row 1 | Row 2 | Row 3 | Row 4 |
|---|---|---|---|---|---|
| 1 | Techno, hard techno | rumble, acid, squelch, offbeat, pound, deep | dub chord, rave stab, hard stab, organ stab, howl, metal | bleep, arp, sync lead, siren, pulse, acid lead | drone, fog, wash, ritual, tunnel, riser |
| 2 | EDM | house bass, saw bass, pluck bass, sub, bounce, fm bass | supersaw chord, future chord, piano chord, stab, brass, vox chop | supersaw lead, big lead, pluck, bell pluck, arp, glide lead | trance pad, wide pad, air, sweep, riser, impact |
| 3 | Bass music, dubstep | wobble, growl, yoi, neuro, rift, sub | talk, formant stab, metal stab, horn, chop, tear | screech, laser, fm lead, sync lead, saw lead, siren | dark pad, drone, scatter, dust, noise, downlift |
| 4 | Pads, keys, melodic, harmonic | sub, round, pluck bass, solid, upright, sine | tine, keys, organ, choir, strings, chord | pluck, harp, glass, chime, kalimba, music box | cloud, shimmer, haze, air, swell, halo |

The names are working titles; the sounds get their own words when made.
Today's 20 factory sounds fill a third of it already.

## Page 2 (built 2026-11-23)

24 sounds on the new wavetable banks and three new FM voices, after the
owner's brief to follow the archetypes the best-known commercial preset
libraries are built on (studied: the big wavetable synths' and the
classic FM synths' factory banks, and techno, EDM and dubstep production
practice; internal only, nothing copied, the values ours; no product is
named anywhere a player reads). Same rows as page 1:

| Row | Sounds | The archetype |
|---|---|---|
| Basses | yoi, growl, riddim, pound, deep, punch | the vowel "yoi" scoop; the wavetable growl wobbling in eighths; riddim's mid chopped in sixteenth triplets over a whole sub; the hard-techno folded hit; the dub-techno sweep; the plucky FM bass |
| Plucks, leads | future, organ, dub, rave, screech, brass | future-bass chords (seven saws scooping up, pumped); the house organ; the dub chord into the delay; the rave stab; the gliding growl lead; FM brass |
| Pads | drift, warm, tunnel, whisper, riser, halo | the evolving wavetable pad; the warm seven-saw pad, filter opening; the dark techno pad over two bars; vowel grains; the riser (an octave in four seconds, noise rising); soft drawbars |
| Bells, keys | mallet, metal, bleep, drops, ember, keys | the FM mallet; a metallic bell; the techno bleep; water drops; a folded bell; percussive drawbars |

Levels set by render so each row sits with page 1's (within ~2 dB while
sounding). Unit 2 (`synth bench`, cycles a voice): future 437, warm 399,
growl 379, riser 374, yoi 335, drift 327, tunnel 287, rave 246, riddim
227, halo 214, screech 203, pound 191, dub 188, ember 170, whisper 168,
brass 162, punch 144, deep 143, keys 130, organ 127, drops 120, metal
108, mallet 106, bleep 106. The renders are the owner's to judge; the
names are working titles.

## Pages 3 and 4 (built 2026-11-23)

The owner asked for more in the style of the big wavetable synths'
dubstep presets, and heavy techno. Two banks came first (neuro, hard:
`synth-engines.md`, "Wavetable library"), then 48 sounds on the same
rows, levels matched by render to pages 1 and 2.

| Page | Basses | Plucks, leads | Pads, effects | Bells, percussion |
|---|---|---|---|---|
| 3: dubstep, bass music | neuro (sixteenth steps), tear (eighth-triplet gate), wub (the first wobble), rip (scoop and tear), snarl (a growl played by pressure and slide), drop (a falling sine) | laser, horn (vowel brass), chop (vocal chop), siren, zap, smash | doom, fall (downlifter), impact (boom), void, ghost (far choir), swell (the build) | glint, spark, musicbox, steel, arp, glow |
| 4: heavy techno | thump (a tuned kick), gravel (offbeat), burn (acid driven hard), grind, roll (ducked), under (the low end) | slam, alarm (octaves in sixteenths), scream, clang, gate (chopped chord), ping | dread (four-bar drone), hiss, ritual, fog, cathedral, tide | tick, anvil, glitch, gong, zip, blast |

A slot written from the computer takes the factory sound's place;
clearing it brings it back. The heaviest on the engine's cost: blast and
swell (seven copies), like super.

## Recipes by genre (what makes each work)

Techniques as widely taught in synthesis (and as read in open projects'
factory banks, to be studied: Surge XT, Vital's community tables, Helm,
Odin 2, Mutable Instruments' Plaits; internal only, nothing copied: the
values are ours, as with the engines).

- **Techno:** basses short and dry, a filter envelope doing the work, the
  low end mono; *rumble* is a long low sine through the plate, darkened
  and pumped by the kick; dub chords are a detuned saw stack through a
  band-pass, short, drowned in delay; stabs bright and percussive.
- **Hard techno:** everything driven: square and saw through overdrive,
  fold and a little crush; *pound* a distorted bass on every offbeat;
  hard stabs with a fast pitch drop; acid at high resonance into drive.
- **EDM:** the supersaw (7 detuned saws, ~20-50 cents, a high-pass under
  them, wide); future chords pitch-bending into each note, pumped; plucks
  with a fast filter envelope into a long reverb; leads in unison with
  glide.
- **Dubstep, bass music:** the wobble (an LFO on the filter or the wave
  position, synced at 1/8, 1/16, triplets); growls from FM and a moving
  formant (vowel) table through drive and fold; *yoi* a vowel sweep; the
  sub kept clean and separate from the mid bass.
- **Pads:** slow attack, unison or grains, a slow LFO on the wave, chorus
  and plate; pressure opening and thickening them (the granular sounds
  already do).
- **Plucks, keys, melodic, harmonic:** short envelopes with velocity to
  the filter; FM for bells and tines; additive (glass) frames for organs;
  chords that keep their voicing clear.

## What the engines need first

The presets above can't all be made today. In order of what they unlock:

1. **The preset store** (built 2026-11-23): presets live in flash and a
   track plays a RAM copy; ~5 KB of RAM came back.
2. **Four macros per engine** (decided direction, `sound-roadmap.md`):
   what the encoders and the app move, set per preset.
3. **Wavetable engine additions**, cheap ones first:
   - noise, a sub, a second LFO, unison up to 7: built 2026-11-23
     (`synth-engines.md`, "Wavetable extras"; *super* costs 418 cycles);
   - more wavetables: built 2026-11-23, nine banks (talk, growl, fold,
     bits, sweep and organ added), built into one of 4 RAM slots when a
     track's sound needs one (`synth-engines.md`, "Wavetable library").
4. **The drum synth** (planned): kicks for rumble and hard techno.
5. **A formant (vowel) filter** for talking and growling basses.

RAM was the limit for 3 and 5: ~55 KB is free with the wavetable
library's fourth slot (2026-11-23).

## How a preset is made

1. The recipe in a factory table (our own values).
2. Rendered on the computer through the engine's own code to a WAV
   (`scratchpad` render tool, as the granular tuning on 2026-11-23): a
   phrase per role (chord swell for pads, a line for basses, an arpeggio
   for plucks), pressure and slide moving.
3. The owner listens; tuned; then on the board (`synth bench` for its
   cost, the soak for its load).

## Open questions

1. The page map: genres by page and roles by row, or another order?
2. Which engine additions first, given the RAM (the store, then noise
   and sub, then the second LFO and the bank library, then unison 7)?
3. New names for *hoover* and *reese*.
