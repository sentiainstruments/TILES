/*
 * SENTIA TILES Controller for Apple Logic Pro (Scripter MIDI FX).
 *
 * Companion MIDI FX plugin providing:
 *   1) Per-pad Auto-Assign Smart Controls: preserves untouched MPE expression
 *      (channels 2..9) via zero-latency event.send(), while splitting Touch,
 *      Strike, Press, and Tilt into Logic Learn-friendly CCs (focused channel 16
 *      CC 20..23 and per-voice channels 2..9 CC 14..17).
 *   2) Live Loops Grid Feedback: synchronizes the 5x4 visible Live Loops cell
 *      matrix back to TILES pads (notes 36..55 on TILES Logic Grid) for pad LED
 *      color/pulse state and firmware touch/sustain haptic feedback, plus page
 *      switching (CC 119) and expression-splitter toggle (CC 118).
 *   3) Automatic Mixer Bank Synchronization: monitors active mixer bank &
 *      Mackie Control bank/channel changes in Logic Pro and immediately updates
 *      the TILES hardware fader-group LED color mapping (CC 112/113/106 + per-fader
 *      CC 32..36) across SENTIA TILES DAW.
 */

var NeedsTimingInfo = true;

var MPE_FIRST_CH = 2;
var MPE_LAST_CH = 9;
var AUTO_ASSIGN_CH = 16;

var VOICE_CC_STRIKE = 14;
var VOICE_CC_PRESS = 15;
var VOICE_CC_TILT = 16;
var VOICE_CC_TOUCH = 17;

var FOCUS_CC_STRIKE = 20;
var FOCUS_CC_PRESS = 21;
var FOCUS_CC_TILT = 22;
var FOCUS_CC_TOUCH = 23;

var GRID_CC_CELL_FIRE = 24;
var GRID_CC_CELL_STOP = 25;
var GRID_CC_CELL_RECORD = 26;
var GRID_CC_CELL_DELETE = 27;
var GRID_CC_MASTER_STOP = 29;
var GRID_CC_FADER_COLOR_BASE = 32; /* Ch 16 CC 32..36 per-fader color hint */

var GRID_CC_LOG_ENABLE = 85;
var GRID_CC_LOG_FILTER = 86;
var DAW_CC_TRACK_OFFSET = 106;
var GRID_CC_FADER_COLOR = 112;
var GRID_CC_MIXER_BANK = 113;
var GRID_CC_BANK_STEP = 114;
var GRID_CC_EXPR_SPLIT = 118;
var GRID_CC_PAGE_SELECT = 119;

var MCU_NOTE_BANK_LEFT = 0x2E;
var MCU_NOTE_BANK_RIGHT = 0x2F;
var MCU_NOTE_CH_LEFT = 0x30;
var MCU_NOTE_CH_RIGHT = 0x31;

var GRID_NOTE_BASE = 36;
var GRID_NUM_CELLS = 20; /* 5 tracks x 4 scenes */
var VISIBLE_TRACKS = 5;
var MAX_TRACK_OFFSET = 59;
var MAX_MIXER_BANKS = 12;

var FADER_GROUP_COLORS = [
    { name: "Periwinkle", code: 0, ccVal: 21 },
    { name: "Teal", code: 1, ccVal: 42 },
    { name: "Amber", code: 2, ccVal: 63 },
    { name: "Gold", code: 3, ccVal: 85 },
    { name: "Coral", code: 4, ccVal: 106 },
    { name: "Magenta", code: 5, ccVal: 127 }
];

var PARAM_EXPR_MODE = 0;
var PARAM_SEND_TOUCH = 1;
var PARAM_SEND_STRIKE = 2;
var PARAM_SEND_PRESS = 3;
var PARAM_SEND_TILT = 4;
var PARAM_INVERT_TILT = 5;
var PARAM_BRIDGE_PAGE = 6;
var PARAM_LIVE_LOOPS_SYNC = 7;
var PARAM_CELL_TRACK = 8;
var PARAM_CELL_SCENE = 9;
var PARAM_CELL_STATE = 10;
var PARAM_CLEAR_GRID = 11;
var PARAM_AUTO_MIXER_BANK = 12;
var PARAM_MIXER_BANK = 13;
var PARAM_FADER_COLOR_MODE = 14;
var PARAM_MIDI_LOGGER_ENABLE = 15;
var PARAM_MIDI_LOGGER_FILTER = 16;
var PARAM_BANK_PREV = 17;
var PARAM_BANK_NEXT = 18;
var PARAM_TRACK_COLOR_MODE = 19;
var PARAM_BANK_CH_COLOR_BASE = 20; /* 20..24: Channel 1..5 Logic track colors in active bank */

var MAX_TOTAL_TRACKS = 64;
var DEFAULT_BANK_TRACK_PALETTE = [
    [2, 4, 0, 5, 1], /* Bank 1: Amber, Coral, Periwinkle, Magenta, Teal */
    [3, 1, 0, 2, 5], /* Bank 2: Gold, Teal, Periwinkle, Amber, Magenta */
    [1, 0, 5, 3, 4], /* Bank 3: Teal, Periwinkle, Magenta, Gold, Coral */
    [4, 2, 3, 1, 0], /* Bank 4: Coral, Amber, Gold, Teal, Periwinkle */
    [5, 0, 1, 4, 2], /* Bank 5: Magenta, Periwinkle, Teal, Coral, Amber */
    [0, 3, 2, 5, 1]  /* Bank 6: Periwinkle, Gold, Amber, Magenta, Teal */
];

var PluginParameters = [
    {
        name: "Auto-Assign Expression",
        type: "menu",
        valueStrings: [
            "MPE + Focused & Per-Voice",
            "MPE + Focused (Ch 16 Only)",
            "MPE + Per-Voice (Ch 2-9 Only)",
            "MPE Passthrough Only"
        ],
        defaultValue: 0
    },
    {
        name: "Auto-Assign Touch (CC 23 / 17)",
        type: "checkbox",
        defaultValue: 1
    },
    {
        name: "Auto-Assign Strike (CC 20 / 14)",
        type: "checkbox",
        defaultValue: 1
    },
    {
        name: "Auto-Assign Press (CC 21 / 15)",
        type: "checkbox",
        defaultValue: 1
    },
    {
        name: "Auto-Assign Tilt (CC 22 / 16)",
        type: "checkbox",
        defaultValue: 1
    },
    {
        name: "Invert Tilt Axis",
        type: "checkbox",
        defaultValue: 0
    },
    {
        name: "Bridge Page (CC 119)",
        type: "menu",
        valueStrings: ["Live Loops", "Smart Controls", "Mixer"],
        defaultValue: 0
    },
    {
        name: "Live Loops Cell Feedback",
        type: "checkbox",
        defaultValue: 1
    },
    {
        name: "Feedback Cell Track (1-5)",
        type: "lin",
        minValue: 1,
        maxValue: 5,
        numberOfSteps: 4,
        defaultValue: 1
    },
    {
        name: "Feedback Cell Scene (1-4)",
        type: "lin",
        minValue: 1,
        maxValue: 4,
        numberOfSteps: 3,
        defaultValue: 1
    },
    {
        name: "Feedback Cell State",
        type: "menu",
        valueStrings: ["Empty (Off)", "Queued (Blink)", "Playing (Pulse + Haptic)"],
        defaultValue: 0
    },
    {
        name: "Clear Live Loops Feedback",
        type: "momentary",
        defaultValue: 0
    },
    {
        name: "Auto-Sync Mixer Bank LEDs",
        type: "checkbox",
        defaultValue: 1
    },
    {
        name: "Active Mixer Bank (Fader Group)",
        type: "menu",
        valueStrings: [
            "Group 1 (Tracks 1-5)",
            "Group 2 (Tracks 6-10)",
            "Group 3 (Tracks 11-15)",
            "Group 4 (Tracks 16-20)",
            "Group 5 (Tracks 21-25)",
            "Group 6 (Tracks 26-30)",
            "Group 7 (Tracks 31-35)",
            "Group 8 (Tracks 36-40)",
            "Group 9 (Tracks 41-45)",
            "Group 10 (Tracks 46-50)",
            "Group 11 (Tracks 51-55)",
            "Group 12 (Tracks 56-60)"
        ],
        defaultValue: 0
    },
    {
        name: "Fader Group LED Color",
        type: "menu",
        valueStrings: [
            "Auto by Active Bank",
            "Periwinkle",
            "Teal",
            "Amber",
            "Gold",
            "Coral",
            "Magenta"
        ],
        defaultValue: 0
    },
    {
        name: "MIDILogger System Console (CC 85)",
        type: "checkbox",
        defaultValue: 0
    },
    {
        name: "MIDILogger Filter (CC 86)",
        type: "menu",
        valueStrings: [
            "All Bridge Traffic",
            "MPE + Expression Only",
            "Haptic + SysEx Only",
            "Live Loops Grid Only",
            "Mackie Control (MCU)"
        ],
        defaultValue: 0
    },
    {
        name: "Mixer Bank Previous (-5 Ch)",
        type: "momentary",
        defaultValue: 0
    },
    {
        name: "Mixer Bank Next (+5 Ch)",
        type: "momentary",
        defaultValue: 0
    },
    {
        name: "Bank LED Color Source",
        type: "menu",
        valueStrings: [
            "Bank Group Palette",
            "Logic Track Colors (Per-Channel)"
        ],
        defaultValue: 0
    },
    {
        name: "Bank Ch 1 Logic Color",
        type: "menu",
        valueStrings: ["Track Auto", "Periwinkle", "Teal", "Amber", "Gold", "Coral", "Magenta"],
        defaultValue: 0
    },
    {
        name: "Bank Ch 2 Logic Color",
        type: "menu",
        valueStrings: ["Track Auto", "Periwinkle", "Teal", "Amber", "Gold", "Coral", "Magenta"],
        defaultValue: 0
    },
    {
        name: "Bank Ch 3 Logic Color",
        type: "menu",
        valueStrings: ["Track Auto", "Periwinkle", "Teal", "Amber", "Gold", "Coral", "Magenta"],
        defaultValue: 0
    },
    {
        name: "Bank Ch 4 Logic Color",
        type: "menu",
        valueStrings: ["Track Auto", "Periwinkle", "Teal", "Amber", "Gold", "Coral", "Magenta"],
        defaultValue: 0
    },
    {
        name: "Bank Ch 5 Logic Color",
        type: "menu",
        valueStrings: ["Track Auto", "Periwinkle", "Teal", "Amber", "Gold", "Coral", "Magenta"],
        defaultValue: 0
    }
];

var cfgExprMode = 0;
var cfgSendTouch = true;
var cfgSendStrike = true;
var cfgSendPress = true;
var cfgSendTilt = true;
var cfgInvertTilt = false;
var cfgLiveLoopsSync = true;
var cfgAutoMixerBankSync = true;
var cfgColorMode = 0; /* 0 = Auto by Active Bank, 1..6 = explicit palette */
var cfgTrackColorMode = 0; /* 0 = Bank Group Palette, 1 = Logic Track Colors (Per-Channel) */
var cfgMidiLoggerEnabled = false;
var cfgMidiLoggerFilter = 0; /* 0 = All, 1 = MPE, 2 = Haptic, 3 = Grid, 4 = MCU */

var selTrack = 1;
var selScene = 1;
var activeMixerBank = 0;
var activeTrackOffset = 0;
var activeColorIndex = 0;
var isSyncingParameters = false;

var trackColorOverrides = [];
for (var tc = 0; tc < MAX_TOTAL_TRACKS; tc++) {
    trackColorOverrides.push(-1); /* -1 = use active bank / scheme default */
}

var focusedChannel = MPE_FIRST_CH;
var activeVoiceNotes = {};
var cellStates = [];
for (var c = 0; c < GRID_NUM_CELLS; c++) {
    cellStates.push(0); /* 0 = off, 1 = queued/triggered, 2 = playing */
}
var wasPlaying = false;

function clamp(v, lo, hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

function emitCC(ch, cc, val) {
    var msg = new ControlChange();
    msg.channel = ch;
    msg.number = cc;
    msg.value = clamp(Math.round(val), 0, 127);
    msg.send();
}

function emitGridNote(padIndex0, velocity) {
    var note = new NoteOn();
    note.channel = 1;
    note.pitch = GRID_NOTE_BASE + padIndex0;
    note.velocity = clamp(Math.round(velocity), 0, 127);
    note.send();
}

function ccValToColorCode(val) {
    if (val <= 31) return 0;
    if (val <= 52) return 1;
    if (val <= 73) return 2;
    if (val <= 95) return 3;
    if (val <= 116) return 4;
    return 5;
}

function resolveFaderColorIndex(bankIdx) {
    if (cfgColorMode > 0) {
        return (cfgColorMode - 1) % FADER_GROUP_COLORS.length;
    }
    return bankIdx % FADER_GROUP_COLORS.length;
}

function resolveTrackColorCode(absTrack, col) {
    if (absTrack >= 0 && absTrack < MAX_TOTAL_TRACKS && trackColorOverrides[absTrack] >= 0) {
        return trackColorOverrides[absTrack] % FADER_GROUP_COLORS.length;
    }
    if (cfgColorMode > 0) {
        return (cfgColorMode - 1) % FADER_GROUP_COLORS.length;
    }
    if (cfgTrackColorMode === 1) {
        var schemeRow = DEFAULT_BANK_TRACK_PALETTE[activeMixerBank % DEFAULT_BANK_TRACK_PALETTE.length];
        return schemeRow[col % VISIBLE_TRACKS];
    }
    return resolveFaderColorIndex(activeMixerBank);
}

function syncBankUiParameters() {
    if (typeof SetParameter !== "function" || isSyncingParameters) return;
    isSyncingParameters = true;
    try {
        SetParameter(PARAM_MIXER_BANK, activeMixerBank);
        for (var col = 0; col < VISIBLE_TRACKS; col++) {
            var absTrack = activeTrackOffset + col;
            var overrideVal = (absTrack < MAX_TOTAL_TRACKS && trackColorOverrides[absTrack] >= 0)
                ? (trackColorOverrides[absTrack] + 1)
                : 0;
            SetParameter(PARAM_BANK_CH_COLOR_BASE + col, overrideVal);
        }
    } finally {
        isSyncingParameters = false;
    }
}

function pushActiveFaderGroupColors() {
    var colorIdx = resolveFaderColorIndex(activeMixerBank);
    activeColorIndex = colorIdx;
    var bankPalette = FADER_GROUP_COLORS[colorIdx];

    /* Update bridge fader-group palette + track window immediately. */
    emitCC(1, GRID_CC_MIXER_BANK, activeMixerBank);
    emitCC(1, DAW_CC_TRACK_OFFSET, activeTrackOffset);
    emitCC(1, GRID_CC_FADER_COLOR, bankPalette.code);

    /* Emit per-track color hint CCs (Ch 16 CC 32..36) for visible 5-fader bank. */
    var colorNames = [];
    for (var col = 0; col < VISIBLE_TRACKS; col++) {
        var absTrack = activeTrackOffset + col;
        var colCode = resolveTrackColorCode(absTrack, col);
        var colPalette = FADER_GROUP_COLORS[colCode];
        emitCC(AUTO_ASSIGN_CH, GRID_CC_FADER_COLOR_BASE + col, colPalette.ccVal);
        colorNames.push("T" + (absTrack + 1) + ":" + colPalette.name);
    }

    if (typeof Trace === "function") {
        Trace("[TILES_Controller] Bank " + (activeMixerBank + 1) + " LEDs -> " + colorNames.join(", "));
    }
}

function syncMixerBankFromTrackOffset(nextOffset, activateMixerPage) {
    activeTrackOffset = clamp(Math.round(nextOffset), 0, MAX_TRACK_OFFSET);
    activeMixerBank = clamp(Math.floor(activeTrackOffset / VISIBLE_TRACKS), 0, MAX_MIXER_BANKS - 1);

    if (activateMixerPage) {
        emitCC(1, GRID_CC_PAGE_SELECT, 2);
    }
    syncBankUiParameters();
    pushActiveFaderGroupColors();
}

function syncMixerBankIndex(nextBank, activateMixerPage) {
    var bank = clamp(Math.round(nextBank), 0, MAX_MIXER_BANKS - 1);
    var offset = clamp(bank * VISIBLE_TRACKS, 0, MAX_TRACK_OFFSET);
    activeMixerBank = bank;
    activeTrackOffset = offset;

    if (activateMixerPage) {
        emitCC(1, GRID_CC_PAGE_SELECT, 2);
    }
    syncBankUiParameters();
    pushActiveFaderGroupColors();
}

function emitCellFeedback(padIndex0, state) {
    if (padIndex0 < 0 || padIndex0 >= GRID_NUM_CELLS) return;
    cellStates[padIndex0] = state;
    if (!cfgLiveLoopsSync) return;

    if (state === 2) {
        emitGridNote(padIndex0, 127);
    } else if (state === 1) {
        emitGridNote(padIndex0, 32);
    } else {
        emitGridNote(padIndex0, 0);
    }
}

function clearAllCellFeedback() {
    for (var i = 0; i < GRID_NUM_CELLS; i++) {
        if (cellStates[i] !== 0) {
            cellStates[i] = 0;
            if (cfgLiveLoopsSync) {
                emitGridNote(i, 0);
            }
        }
    }
}

function pad1ToVisibleIndex(pad1) {
    if (pad1 < 1 || pad1 > 24) return -1;
    var col0 = (pad1 - 1) % 6;
    var row0 = Math.floor((pad1 - 1) / 6);
    if (col0 >= 5 || row0 >= 4) return -1;
    return row0 * 5 + col0;
}

function sendFocusedAndVoiceCC(voiceCh, voiceCC, focusCC, value) {
    if (cfgExprMode === 3) return;
    if (cfgExprMode === 0 || cfgExprMode === 2) {
        emitCC(voiceCh, voiceCC, value);
    }
    if ((cfgExprMode === 0 || cfgExprMode === 1) && voiceCh === focusedChannel) {
        emitCC(AUTO_ASSIGN_CH, focusCC, value);
    }
}

function ParameterChanged(param, value) {
    if (param === PARAM_EXPR_MODE) {
        cfgExprMode = Math.round(value);
        emitCC(1, GRID_CC_EXPR_SPLIT, cfgExprMode === 3 ? 0 : 127);
    } else if (param === PARAM_SEND_TOUCH) {
        cfgSendTouch = value >= 0.5;
    } else if (param === PARAM_SEND_STRIKE) {
        cfgSendStrike = value >= 0.5;
    } else if (param === PARAM_SEND_PRESS) {
        cfgSendPress = value >= 0.5;
    } else if (param === PARAM_SEND_TILT) {
        cfgSendTilt = value >= 0.5;
    } else if (param === PARAM_INVERT_TILT) {
        cfgInvertTilt = value >= 0.5;
    } else if (param === PARAM_BRIDGE_PAGE) {
        var page = clamp(Math.round(value), 0, 2);
        emitCC(1, GRID_CC_PAGE_SELECT, page);
        if (page === 2 && cfgAutoMixerBankSync) {
            pushActiveFaderGroupColors();
        }
    } else if (param === PARAM_LIVE_LOOPS_SYNC) {
        cfgLiveLoopsSync = value >= 0.5;
        if (!cfgLiveLoopsSync) {
            clearAllCellFeedback();
        }
    } else if (param === PARAM_CELL_TRACK) {
        selTrack = clamp(Math.round(value), 1, 5);
    } else if (param === PARAM_CELL_SCENE) {
        selScene = clamp(Math.round(value), 1, 4);
    } else if (param === PARAM_CELL_STATE) {
        var state = clamp(Math.round(value), 0, 2);
        var idx = (selScene - 1) * 5 + (selTrack - 1);
        emitCellFeedback(idx, state);
    } else if (param === PARAM_CLEAR_GRID && value >= 0.5) {
        clearAllCellFeedback();
    } else if (param === PARAM_AUTO_MIXER_BANK) {
        cfgAutoMixerBankSync = value >= 0.5;
        if (cfgAutoMixerBankSync) {
            pushActiveFaderGroupColors();
        }
    } else if (param === PARAM_MIXER_BANK) {
        syncMixerBankIndex(Math.round(value), cfgAutoMixerBankSync);
    } else if (param === PARAM_FADER_COLOR_MODE) {
        cfgColorMode = clamp(Math.round(value), 0, FADER_GROUP_COLORS.length);
        pushActiveFaderGroupColors();
    } else if (param === PARAM_MIDI_LOGGER_ENABLE) {
        cfgMidiLoggerEnabled = value >= 0.5;
        emitCC(1, GRID_CC_LOG_FILTER, cfgMidiLoggerFilter);
        emitCC(1, GRID_CC_LOG_ENABLE, cfgMidiLoggerEnabled ? 127 : 0);
        if (typeof Trace === "function") {
            Trace("[TILES_Controller] MIDILogger console " + (cfgMidiLoggerEnabled ? "ENABLED" : "DISABLED") + " (CC 85)");
        }
    } else if (param === PARAM_MIDI_LOGGER_FILTER) {
        cfgMidiLoggerFilter = clamp(Math.round(value), 0, 4);
        emitCC(1, GRID_CC_LOG_FILTER, cfgMidiLoggerFilter);
    } else if (param === PARAM_BANK_PREV && value >= 0.5) {
        syncMixerBankIndex(activeMixerBank - 1, cfgAutoMixerBankSync);
    } else if (param === PARAM_BANK_NEXT && value >= 0.5) {
        syncMixerBankIndex(activeMixerBank + 1, cfgAutoMixerBankSync);
    } else if (param === PARAM_TRACK_COLOR_MODE) {
        cfgTrackColorMode = clamp(Math.round(value), 0, 1);
        pushActiveFaderGroupColors();
    } else if (param >= PARAM_BANK_CH_COLOR_BASE && param < PARAM_BANK_CH_COLOR_BASE + VISIBLE_TRACKS) {
        if (isSyncingParameters) return;
        var col = param - PARAM_BANK_CH_COLOR_BASE;
        var absTrack = activeTrackOffset + col;
        var sel = clamp(Math.round(value), 0, FADER_GROUP_COLORS.length);
        if (absTrack < MAX_TOTAL_TRACKS) {
            trackColorOverrides[absTrack] = (sel === 0) ? -1 : (sel - 1);
        }
        pushActiveFaderGroupColors();
    }
}

function handleMpeExpression(event) {
    var ch = event.channel;
    if (ch < MPE_FIRST_CH || ch > MPE_LAST_CH) return;

    if (event instanceof NoteOn && event.velocity > 0) {
        focusedChannel = ch;
        activeVoiceNotes[ch] = (activeVoiceNotes[ch] || 0) + 1;
        if (cfgSendTouch) {
            sendFocusedAndVoiceCC(ch, VOICE_CC_TOUCH, FOCUS_CC_TOUCH, 127);
        }
        if (cfgSendStrike) {
            sendFocusedAndVoiceCC(ch, VOICE_CC_STRIKE, FOCUS_CC_STRIKE, event.velocity);
        }
    } else if (event instanceof NoteOff || (event instanceof NoteOn && event.velocity === 0)) {
        if (activeVoiceNotes[ch] && activeVoiceNotes[ch] > 0) {
            activeVoiceNotes[ch] -= 1;
        }
        if (!activeVoiceNotes[ch] || activeVoiceNotes[ch] <= 0) {
            activeVoiceNotes[ch] = 0;
            if (cfgSendTouch) {
                sendFocusedAndVoiceCC(ch, VOICE_CC_TOUCH, FOCUS_CC_TOUCH, 0);
            }
        }
    } else if (event instanceof ChannelPressure) {
        if (cfgSendPress) {
            sendFocusedAndVoiceCC(ch, VOICE_CC_PRESS, FOCUS_CC_PRESS, event.value);
        }
    } else if (event instanceof PitchBend) {
        if (cfgSendTilt) {
            /* Logic Scripter PitchBend.value is signed -8192..8191; scale cleanly to 0..127. */
            var norm = clamp(Math.floor((event.value + 8192) / 128), 0, 127);
            if (cfgInvertTilt) {
                norm = 127 - norm;
            }
            sendFocusedAndVoiceCC(ch, VOICE_CC_TILT, FOCUS_CC_TILT, norm);
        }
    }
}

function handleGridControl(event) {
    if (!cfgLiveLoopsSync) return;

    if (event instanceof ControlChange && event.channel === AUTO_ASSIGN_CH) {
        var idx = pad1ToVisibleIndex(event.value);
        if (event.number === GRID_CC_CELL_FIRE && idx >= 0) {
            var nextState = cellStates[idx] === 2 ? 0 : 2;
            emitCellFeedback(idx, nextState);
        } else if ((event.number === GRID_CC_CELL_STOP || event.number === GRID_CC_CELL_DELETE) && idx >= 0) {
            emitCellFeedback(idx, 0);
        } else if (event.number === GRID_CC_CELL_RECORD && idx >= 0) {
            emitCellFeedback(idx, 1);
        } else if (event.number === GRID_CC_MASTER_STOP && event.value > 0) {
            clearAllCellFeedback();
        }
    }
}

function handleMixerBankSync(event) {
    if (!cfgAutoMixerBankSync) return;

    if (event instanceof NoteOn && event.velocity > 0 && event.channel === 1) {
        if (event.pitch === MCU_NOTE_BANK_LEFT) {
            syncMixerBankFromTrackOffset(activeTrackOffset - 8, false);
        } else if (event.pitch === MCU_NOTE_BANK_RIGHT) {
            syncMixerBankFromTrackOffset(activeTrackOffset + 8, false);
        } else if (event.pitch === MCU_NOTE_CH_LEFT) {
            syncMixerBankFromTrackOffset(activeTrackOffset - 1, false);
        } else if (event.pitch === MCU_NOTE_CH_RIGHT) {
            syncMixerBankFromTrackOffset(activeTrackOffset + 1, false);
        }
    } else if (event instanceof ControlChange && event.channel === AUTO_ASSIGN_CH) {
        if (event.number >= GRID_CC_FADER_COLOR_BASE && event.number < GRID_CC_FADER_COLOR_BASE + VISIBLE_TRACKS) {
            var colorCol = event.number - GRID_CC_FADER_COLOR_BASE;
            var absTr = activeTrackOffset + colorCol;
            var decodedCode = ccValToColorCode(event.value);
            if (absTr < MAX_TOTAL_TRACKS) {
                trackColorOverrides[absTr] = decodedCode;
            }
            syncBankUiParameters();
            pushActiveFaderGroupColors();
        }
    } else if (event instanceof ControlChange && event.channel === 1) {
        if (event.number === GRID_CC_MIXER_BANK) {
            syncMixerBankIndex(event.value, false);
        } else if (event.number === GRID_CC_BANK_STEP) {
            var delta = event.value < 64 ? -VISIBLE_TRACKS : VISIBLE_TRACKS;
            syncMixerBankFromTrackOffset(activeTrackOffset + delta, false);
        } else if (event.number === DAW_CC_TRACK_OFFSET) {
            syncMixerBankFromTrackOffset(event.value, false);
        }
    }
}

function HandleMIDI(event) {
    /* Pass original MIDI and 14-bit MPE stream downstream untouched first. */
    event.send();

    handleMpeExpression(event);
    handleGridControl(event);
    handleMixerBankSync(event);
}

function ProcessMIDI() {
    var info = GetTimingInfo();
    if (wasPlaying && !info.playing && cfgLiveLoopsSync) {
        /* Keep loaded clip state intact unless explicitly stopped; reset active voices. */
        activeVoiceNotes = {};
    }
    wasPlaying = info.playing;
}

function Reset() {
    activeVoiceNotes = {};
    activeMixerBank = 0;
    activeTrackOffset = 0;
    activeColorIndex = resolveFaderColorIndex(0);
    if (cfgSendTouch && cfgExprMode !== 3) {
        emitCC(AUTO_ASSIGN_CH, FOCUS_CC_TOUCH, 0);
    }
    clearAllCellFeedback();
    syncBankUiParameters();
    if (cfgAutoMixerBankSync) {
        pushActiveFaderGroupColors();
    }
}
