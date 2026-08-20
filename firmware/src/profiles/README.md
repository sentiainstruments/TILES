# profiles/

Everything that's *configuration*, not *code*: feature flags (which
subsystems are enabled), power budgets per demo/bring-up profile
(SAFE_BRINGUP, SENSOR_TEST, USB_DEMO_SAFE, USB_DEMO_VALIDATED_1P5A,
FULL_DEMO_EXTERNAL, MANUFACTURING_TEST), note/scale layouts, button
bindings, and lighting/haptic themes.

This is the layer the companion app edits and downloads. On-device,
`storage/` persists it; here is just the schema/defaults and the runtime
struct the rest of the firmware reads from. A device-side "next scale" or
"toggle haptics" button action mutates the active profile in place — it
never bypasses this layer to poke `services/` directly.
