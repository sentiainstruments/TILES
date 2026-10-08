# Content packs: scales, layouts, modes and games

TILES content will be sold in packs ("we will sell these layouts in packs
so keep that in mind"). This note is the plan for how a pack gets from the
store onto a device. Only the first piece is built: custom scales in the
content store (firmware 0.2.7). It builds on decision 4 in
[`control-software.md`](control-software.md): the app distributes, and
the device never talks to a store.

## Two kinds of content

| Kind | Examples | What the device does with it | How it gets there |
|---|---|---|---|
| **Data** | scales, layouts (pad → note maps, a mode's settings), colour schemes | interprets it with code it already has | the app pushes it into the content store |
| **Code** | a new game, a new mode (a different sequencer, an arpeggiator) | runs it | it is already in the firmware; a pack *unlocks* it |

A pack can mix both, e.g. "Japan": five scales, a layout, and a koto-style
strum mode.

### Data content (scales built; layouts next)

- Lives in the **content store** (`firmware/src/profiles/content.h`): its own
  power-cut-safe flash region, a list of typed records. Scales are type 1;
  a layout becomes a new type without changing the format, and a firmware
  that doesn't know a type keeps it untouched.
- Every item carries `pack`, `item` (stable text ids, at most 16
  characters, lowercase by convention, never reused) and `version` (raised
  whenever the item's content changes). The device stores and lists them
  (`CONTENT LIST`, `SCALES`) but never interprets them. That's what lets
  the app tell what came from where, and offer an update when the catalog
  has a newer version.
- **The device doesn't check ownership of data.** A scale is a handful of
  numbers anyone could type in by hand; protecting that on the device would
  cost a lot and buy nothing. The app (with the store's account) decides
  what a user may push.
- **The app is the library; the device holds a working set.** The
  picker has 9 custom scale slots. A user who owns 40 scales picks which 9
  are on the device; the app swaps them in and out. Layouts will work the
  same way, with whatever slot count their UI allows.

### Code content (planned, not built)

The RP2350 runs code from flash, and the firmware has no way to load code
at runtime. Options:

1. **Every firmware contains every mode and game; a pack unlocks
   them on one device** (recommended). An unlock is an *entitlement*: a
   small record, signed with Sentia's private key, saying "chip ID X may
   use items A, B, C". It is stored in the content store as its own record
   type; the firmware checks the signature against a public key built into
   it, at boot and when one is pushed, and only then shows those modes on
   the device. The chip ID is the unit's USB serial number (already unique
   per RP2350), so an entitlement copied to another unit does nothing.
   Ed25519 verification is a few milliseconds on the RP2350.
2. Loadable code modules: a module format, relocation, an API boundary
   that must stay stable for years, and new ways to crash the instrument
   on stage. Not worth it.
3. A custom firmware build per customer: unmanageable.

Where entitlements come from: the store's server signs one when a pack is
bought (it needs the unit's chip ID: the app reads it over USB and sends
it with the purchase). The app pushes it; it can also re-push it after a
`CONTENT CLEAR` or a repair, since the server can re-issue it.

**Threat model, honestly:** this stops casual sharing (copying a file to a
friend's unit does nothing). It does not stop someone who builds and
flashes a modified firmware with the check removed. Only RP2350 secure boot
(the chip runs only Sentia-signed firmware) closes that, and it also locks
out every unofficial firmware, forever, on that chip (it's OTP). That's a
product decision to make before shipping, not an engineering default.

## What's built, what's next

| Piece | State |
|---|---|
| Content store: typed records, own flash region, power-cut safe, keeps unknown types, read-only on a newer format | ✅ 0.2.7 |
| Custom scales: push / pull / delete over USB, with pack, item, version | ✅ 0.2.7 (`SCALES`, `SCALE PUT/GET/DELETE`) |
| Colour schemes and per-pad colours | ✅ 0.2.7, as settings (`color.*`): schemes live in the app, the device holds the active one |
| Layout record type (first: a pad → note map for melodic mode) | ❌ next; needs "what is a layout" settled with Sam |
| Entitlement record type + signature check + locking modes/games | ❌ planned (option 1 above) |
| More room: the store is one ~4 KB kv payload; layouts may need more | ❌ grow it when layouts land: several records per sector set, or a multi-sector slot |
| Store server: catalog, accounts, purchases, signing entitlements | ❌ app/server side (`companion-app/BRIEF.md`, phase 3) |

## Rules to keep

- Pack and item ids are permanent; a renamed pack keeps its old id.
- Item `version` only goes up.
- The device enforces its own limits whatever a pack says (scale shape,
  LED ceiling, CV range), so a bad or malicious file can't push an unsafe
  value.
- A firmware update must never drop content: new record types are added,
  existing ones keep their format (or get a new type number), and an
  unknown blob format makes the store read-only rather than erasing it.
