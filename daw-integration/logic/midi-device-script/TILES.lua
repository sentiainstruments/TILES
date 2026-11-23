-- SENTIA TILES Logic Pro MIDI Device Script descriptor
-- Install directory: ~/Music/Audio Music Apps/MIDI Device Scripts/Sentia/TILES/

return {
    name = "SENTIA TILES",
    manufacturer = "Sentia Instruments",
    model = "TILES",
    version = "1.0.0",

    ports = {
        { name = "SENTIA TILES 1 MIDI", direction = "in", mpe = true },
        { name = "SENTIA TILES 1 DAW", direction = "duplex" },
        { name = "TILES Logic MPE", direction = "duplex", mpe = true },
        { name = "TILES Logic MCU", direction = "duplex" },
        { name = "TILES Logic Grid", direction = "duplex" },
        { name = "TILES Logic Display", direction = "out" },
    },

    transport = {
        play = { channel = 1, cc = 102 },
        stop = { channel = 1, cc = 103 },
        record = { channel = 1, cc = 104 },
    },

    live_loops = {
        columns = 5,
        rows = 4,
        channel = 16,
        cell_fire_cc = 24,
        cell_stop_cc = 25,
        cell_record_cc = 26,
        cell_delete_cc = 27,
        scene_fire_cc = 28,
        master_stop_cc = 29,
        end_capture_cc = 30,
    },

    expression_auto_assign = {
        focused_channel = 16,
        strike_cc = 20,
        press_cc = 21,
        tilt_cc = 22,
        touch_cc = 23,
        per_voice_channels = { 2, 3, 4, 5, 6, 7, 8, 9 },
        per_voice_strike_cc = 14,
        per_voice_press_cc = 15,
        per_voice_tilt_cc = 16,
        per_voice_touch_cc = 17,
    },
}
