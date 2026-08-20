# test/

Host-buildable unit tests (no hardware required) for logic that can be
isolated from the Pico SDK runtime — pad table integrity (all 24 touch/
Hall/LED/haptic/FPC routes unique, per `SENTIA_FIRMWARE_CODEX_START.md`),
scale/note mapping, voice allocation, calibration math, protocol framing
in `usb_vendor/`. Hardware-dependent code stays covered by the
`diagnostics/` manufacturing test commands instead.
