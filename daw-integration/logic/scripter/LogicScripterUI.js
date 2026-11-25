/*
 * SENTIA TILES Live Loops, Mixer & 24-Pad Hardware Mapper UI for Apple Logic Pro (Scripter MIDI FX).
 *
 * Provides dual runtime support:
 *   1) Native Apple Logic Pro Scripter MIDI FX runtime (PluginParameters,
 *      HandleMIDI, ParameterChanged, ProcessMIDI, Reset) with zero-latency
 *      MPE/MIDI passthrough and Learn pulse emission for per-pad MIDI mapping.
 *   2) Interactive HTML/CSS interface (LogicScripterUI) featuring CSS grids
 *      that mirror the TILES 24-pad hardware layout (top navigation keys +
 *      6x4 pad matrix), Live Loops bank status, current Mixer page mapping,
 *      and per-pad MIDI Learn/CC configuration in the Sentia visual style
 *      (Exo 2 + Sora, Ink/Paper/Periwinkle/Magenta palette).
 */

var NeedsTimingInfo = true;

var PAGE_LIVE_LOOPS = 0;
var PAGE_SMART_CONTROLS = 1;
var PAGE_MIXER = 2;

var NUM_PADS = 24;
var NUM_COLS = 6;
var MAX_TRACKS = 64;
var VISIBLE_TRACKS = 5;
var NUM_SCENES = 4;
var MAX_TRACK_OFFSET = 59;

var GRID_NOTE_BASE = 36;
var GRID_CC_CELL_FIRE = 24;
var GRID_CC_CELL_STOP = 25;
var GRID_CC_CELL_RECORD = 26;
var GRID_CC_CELL_DELETE = 27;
var GRID_CC_SCENE_FIRE = 28;
var GRID_CC_MASTER_STOP = 29;
var GRID_CC_END_CAPTURE = 30;

var DAW_CC_TRACK_OFFSET = 106;
var GRID_CC_BANK_STEP = 114;
var GRID_CC_EXPR_SPLIT = 118;
var GRID_CC_PAGE_SELECT = 119;

var MCU_NOTE_ARM_BASE = 0x00;
var MCU_NOTE_SOLO_BASE = 0x08;
var MCU_NOTE_MUTE_BASE = 0x10;
var MCU_NOTE_SEL_BASE = 0x18;
var MCU_NOTE_CYCLE = 0x56;
var MCU_NOTE_PLAY = 0x5E;

var PARAM_PAGE = 0;
var PARAM_TRACK_OFFSET = 1;
var PARAM_BANK_STEP = 2;
var PARAM_EXPR_SPLIT = 3;
var PARAM_SELECTED_PAD = 4;
var PARAM_PAD_LEARN_CC = 5;
var PARAM_PAD_LEARN_CH = 6;
var PARAM_PAD_LEARN_PULSE = 7;
var PARAM_CELL_STATE = 8;
var PARAM_DUMP_HTML = 9;

var PluginParameters = [
    {
        name: "Bridge View Page (CC 119)",
        type: "menu",
        valueStrings: ["Live Loops", "Smart Controls", "Mixer"],
        defaultValue: 0
    },
    {
        name: "Live Loops Track Offset (0-59)",
        type: "lin",
        minValue: 0,
        maxValue: MAX_TRACK_OFFSET,
        numberOfSteps: MAX_TRACK_OFFSET,
        defaultValue: 0
    },
    {
        name: "Mixer Bank Step (CC 114)",
        type: "menu",
        valueStrings: ["Ready", "Step Left (-5 Tracks)", "Step Right (+5 Tracks)"],
        defaultValue: 0
    },
    {
        name: "Auto-Assign Split (CC 118)",
        type: "checkbox",
        defaultValue: 1
    },
    {
        name: "Selected Hardware Pad (1-24)",
        type: "lin",
        minValue: 1,
        maxValue: NUM_PADS,
        numberOfSteps: NUM_PADS - 1,
        defaultValue: 1
    },
    {
        name: "Pad Map CC Number (0-127)",
        type: "lin",
        minValue: 0,
        maxValue: 127,
        numberOfSteps: 127,
        defaultValue: GRID_CC_CELL_FIRE
    },
    {
        name: "Pad Map MIDI Channel (1-16)",
        type: "lin",
        minValue: 1,
        maxValue: 16,
        numberOfSteps: 15,
        defaultValue: 16
    },
    {
        name: "Send Selected Pad Learn Pulse",
        type: "momentary",
        defaultValue: 0
    },
    {
        name: "Selected Cell Bank Status",
        type: "menu",
        valueStrings: ["Empty", "Queued", "Playing", "Recording"],
        defaultValue: 0
    },
    {
        name: "Trace HTML/CSS Interface",
        type: "momentary",
        defaultValue: 0
    }
];

function defaultPadMapping(pad1, page) {
    var col0 = (pad1 - 1) % NUM_COLS;
    var row0 = Math.floor((pad1 - 1) / NUM_COLS);

    if (page === PAGE_SMART_CONTROLS) {
        if (col0 < 4) {
            var macroNum = (row0 % 2) * 4 + col0 + 1;
            var dir = row0 < 2 ? "+6" : "-6";
            return {
                label: "Macro " + macroNum + " " + dir,
                channel: 16,
                cc: 101 + macroNum,
                note: GRID_NOTE_BASE + (pad1 - 1),
                exprAxis: "Press CC 21"
            };
        }
        if (col0 === 4) {
            var assignLabels = ["Plug-In", "Pan", "Send", "Track"];
            return {
                label: "MCU " + assignLabels[row0],
                channel: 1,
                cc: 0,
                note: 0x2B - row0,
                exprAxis: "Mode Switch"
            };
        }
        var scCol6 = ["Next Page", "Bank -", "Bank +", "Expr Split"];
        return {
            label: scCol6[row0],
            channel: 1,
            cc: row0 === 3 ? GRID_CC_EXPR_SPLIT : GRID_CC_PAGE_SELECT,
            note: GRID_NOTE_BASE + (pad1 - 1),
            exprAxis: "Control"
        };
    }

    if (page === PAGE_MIXER) {
        if (col0 < VISIBLE_TRACKS) {
            var rowLabels = ["Mute", "Solo", "Rec Arm", "Select"];
            var mcuBases = [MCU_NOTE_MUTE_BASE, MCU_NOTE_SOLO_BASE, MCU_NOTE_ARM_BASE, MCU_NOTE_SEL_BASE];
            return {
                label: "T" + (col0 + 1) + " " + rowLabels[row0],
                channel: 1,
                cc: 20 + col0,
                note: mcuBases[row0] + col0,
                exprAxis: "MCU Strip"
            };
        }
        var mixCol6 = ["Next Page", "Ch Left", "Ch Right", "Cycle"];
        return {
            label: mixCol6[row0],
            channel: 1,
            cc: row0 === 0 ? GRID_CC_PAGE_SELECT : GRID_CC_BANK_STEP,
            note: row0 === 3 ? MCU_NOTE_CYCLE : (0x2F + row0),
            exprAxis: "Bank Nav"
        };
    }

    if (col0 < VISIBLE_TRACKS) {
        var visIdx = row0 * VISIBLE_TRACKS + col0;
        return {
            label: "T" + (col0 + 1) + "·S" + (row0 + 1),
            channel: 16,
            cc: GRID_CC_CELL_FIRE,
            note: GRID_NOTE_BASE + visIdx,
            exprAxis: "Touch/Strike"
        };
    }

    return {
        label: "Scene " + (row0 + 1),
        channel: 16,
        cc: GRID_CC_SCENE_FIRE,
        note: GRID_NOTE_BASE + 20 + row0,
        exprAxis: "Trigger"
    };
}

function createPadMappings() {
    var maps = [];
    for (var p = 1; p <= NUM_PADS; p++) {
        maps.push({
            pad: p,
            custom: false,
            label: "",
            channel: 16,
            cc: GRID_CC_CELL_FIRE,
            note: GRID_NOTE_BASE + (p - 1),
            exprAxis: "Strike CC 20"
        });
    }
    return maps;
}

function createDefaultState() {
    var cells = [];
    for (var s = 0; s < NUM_SCENES; s++) {
        var row = [];
        for (var t = 0; t < VISIBLE_TRACKS; t++) {
            row.push(0); /* 0 = empty, 1 = queued, 2 = playing, 3 = recording */
        }
        cells.push(row);
    }

    var strips = [];
    for (var i = 0; i < MAX_TRACKS; i++) {
        strips.push({
            muted: false,
            soloed: false,
            recArmed: false,
            selected: i === 0
        });
    }

    return {
        page: PAGE_LIVE_LOOPS,
        trackOffset: 0,
        mcuBank: 0,
        exprSplit: true,
        cycleActive: false,
        playing: false,
        activeScene: -1,
        selectedPad: 1,
        lastLearnPulsePad: 0,
        inspectTrack: 1,
        inspectScene: 1,
        cells: cells,
        strips: strips,
        padMappings: createPadMappings()
    };
}

var uiState = createDefaultState();
var mountedRoot = null;
var wasPlaying = false;

function clamp(v, lo, hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

function pageName(page) {
    if (page === PAGE_MIXER) return "Mixer";
    if (page === PAGE_SMART_CONTROLS) return "Smart Controls";
    return "Live Loops";
}

function cellStateLabel(st) {
    if (st === 1) return "Queued";
    if (st === 2) return "Playing";
    if (st === 3) return "Recording";
    return "Empty";
}

function cellStateClass(st) {
    if (st === 1) return "is-queued";
    if (st === 2) return "is-playing";
    if (st === 3) return "is-recording";
    return "is-empty";
}

function pad1ToVisibleCoords(pad1) {
    if (pad1 < 1 || pad1 > 24) return null;
    var col0 = (pad1 - 1) % NUM_COLS;
    var row0 = Math.floor((pad1 - 1) / NUM_COLS);
    if (col0 >= VISIBLE_TRACKS || row0 >= NUM_SCENES) return null;
    return { track: col0, scene: row0 };
}

function getEffectivePadMapping(pad1) {
    var clampedPad = clamp(Math.round(pad1), 1, NUM_PADS);
    var customEntry = uiState.padMappings[clampedPad - 1];
    var base = defaultPadMapping(clampedPad, uiState.page);

    if (uiState.page === PAGE_LIVE_LOOPS && ((clampedPad - 1) % NUM_COLS) < VISIBLE_TRACKS) {
        var col0 = (clampedPad - 1) % NUM_COLS;
        var row0 = Math.floor((clampedPad - 1) / NUM_COLS);
        base.label = "T" + (uiState.trackOffset + col0 + 1) + "·S" + (row0 + 1);
    } else if (uiState.page === PAGE_MIXER && ((clampedPad - 1) % NUM_COLS) < VISIBLE_TRACKS) {
        var mCol0 = (clampedPad - 1) % NUM_COLS;
        var mRow0 = Math.floor((clampedPad - 1) / NUM_COLS);
        var roles = ["Mute", "Solo", "Arm", "Sel"];
        base.label = "T" + (uiState.trackOffset + mCol0 + 1) + " " + roles[mRow0];
    }

    if (customEntry && customEntry.custom) {
        return {
            pad: clampedPad,
            custom: true,
            label: customEntry.label || base.label,
            channel: customEntry.channel,
            cc: customEntry.cc,
            note: customEntry.note,
            exprAxis: customEntry.exprAxis || base.exprAxis
        };
    }

    return {
        pad: clampedPad,
        custom: false,
        label: base.label,
        channel: base.channel,
        cc: base.cc,
        note: base.note,
        exprAxis: base.exprAxis
    };
}

function setPadCustomMapping(pad1, cc, channel, exprAxis) {
    var clampedPad = clamp(Math.round(pad1), 1, NUM_PADS);
    var entry = uiState.padMappings[clampedPad - 1];
    var current = getEffectivePadMapping(clampedPad);
    entry.custom = true;
    entry.cc = cc !== undefined ? clamp(Math.round(cc), 0, 127) : current.cc;
    entry.channel = channel !== undefined ? clamp(Math.round(channel), 1, 16) : current.channel;
    entry.note = current.note;
    entry.exprAxis = exprAxis !== undefined ? String(exprAxis) : current.exprAxis;
}

function resetPadCustomMapping(pad1) {
    var clampedPad = clamp(Math.round(pad1), 1, NUM_PADS);
    uiState.padMappings[clampedPad - 1].custom = false;
}

function selectPad(pad1) {
    uiState.selectedPad = clamp(Math.round(pad1), 1, NUM_PADS);
    var coords = pad1ToVisibleCoords(uiState.selectedPad);
    if (coords) {
        uiState.inspectTrack = coords.track + 1;
        uiState.inspectScene = coords.scene + 1;
    }
}

function emitScripterCC(channel, cc, value) {
    if (typeof ControlChange === "undefined") return;
    var msg = new ControlChange();
    msg.channel = channel;
    msg.number = cc;
    msg.value = clamp(Math.round(value), 0, 127);
    msg.send();
}

function emitLearnPulse(pad1) {
    var clampedPad = clamp(Math.round(pad1), 1, NUM_PADS);
    var map = getEffectivePadMapping(clampedPad);
    uiState.lastLearnPulsePad = clampedPad;
    emitScripterCC(map.channel, map.cc, 127);
}

function escapeHtml(text) {
    return String(text)
        .replace(/&/g, "&amp;")
        .replace(/</g, "&lt;")
        .replace(/>/g, "&gt;")
        .replace(/"/g, "&quot;");
}

function setTrackOffset(nextOffset) {
    var clamped = clamp(Math.round(nextOffset), 0, MAX_TRACK_OFFSET);
    uiState.trackOffset = clamped;
    uiState.mcuBank = Math.floor(clamped / 8);
}

function stepBank(delta) {
    setTrackOffset(uiState.trackOffset + delta);
}

function renderCSS() {
    return [
        ":root {",
        "  --tiles-ink: #171d3a;",
        "  --tiles-ink-light: #e6e9f6;",
        "  --tiles-ink-2: #5a6088;",
        "  --tiles-paper: #ffffff;",
        "  --tiles-bg: #eef0f7;",
        "  --tiles-line: #d9ddf0;",
        "  --tiles-periwinkle: #94abf9;",
        "  --tiles-magenta: #ff00ff;",
        "}",
        ".tiles-ui {",
        "  box-sizing: border-box;",
        "  width: 100%;",
        "  max-width: 980px;",
        "  margin: 0 auto;",
        "  padding: 20px 24px 32px;",
        "  background: var(--tiles-paper);",
        "  color: var(--tiles-ink);",
        "  font-family: 'Sora', -apple-system, BlinkMacSystemFont, sans-serif;",
        "  font-size: 14px;",
        "  line-height: 1.45;",
        "}",
        ".tiles-ui *, .tiles-ui *::before, .tiles-ui *::after {",
        "  box-sizing: border-box;",
        "}",
        ".tiles-ui-head {",
        "  display: flex;",
        "  flex-wrap: wrap;",
        "  align-items: flex-end;",
        "  justify-content: space-between;",
        "  gap: 12px;",
        "  padding-bottom: 12px;",
        "  border-bottom: 1.5px solid var(--tiles-ink);",
        "}",
        ".tiles-ui-title {",
        "  margin: 0;",
        "  font-family: 'Exo 2', sans-serif;",
        "  font-weight: 700;",
        "  font-size: 22px;",
        "  letter-spacing: 0.02em;",
        "  color: var(--tiles-ink);",
        "}",
        ".tiles-ui-sub {",
        "  font-size: 12px;",
        "  text-transform: uppercase;",
        "  letter-spacing: 0.08em;",
        "  color: var(--tiles-ink-2);",
        "}",
        ".tiles-page-tabs {",
        "  display: inline-flex;",
        "  gap: 6px;",
        "}",
        ".tiles-tab {",
        "  appearance: none;",
        "  background: var(--tiles-bg);",
        "  color: var(--tiles-ink);",
        "  border: 1.5px solid var(--tiles-line);",
        "  padding: 6px 12px;",
        "  font-family: 'Sora', sans-serif;",
        "  font-size: 12px;",
        "  font-weight: 600;",
        "  text-transform: uppercase;",
        "  letter-spacing: 0.06em;",
        "  cursor: pointer;",
        "}",
        ".tiles-tab.is-active {",
        "  background: var(--tiles-ink);",
        "  border-color: var(--tiles-ink);",
        "  color: var(--tiles-paper);",
        "}",
        ".tiles-tab.is-active .tiles-tab-dot {",
        "  background: var(--tiles-magenta);",
        "}",
        ".tiles-tab-dot {",
        "  display: inline-block;",
        "  width: 7px;",
        "  height: 7px;",
        "  margin-right: 6px;",
        "  background: var(--tiles-ink-2);",
        "}",
        ".tiles-status-strip {",
        "  display: grid;",
        "  grid-template-columns: repeat(auto-fit, minmax(170px, 1fr));",
        "  gap: 12px;",
        "  padding: 14px 0;",
        "  border-bottom: 1px solid var(--tiles-line);",
        "}",
        ".tiles-metric-label {",
        "  font-size: 12px;",
        "  text-transform: uppercase;",
        "  letter-spacing: 0.08em;",
        "  color: var(--tiles-ink-2);",
        "}",
        ".tiles-metric-val {",
        "  font-family: 'Exo 2', sans-serif;",
        "  font-weight: 700;",
        "  font-size: 18px;",
        "  color: var(--tiles-ink);",
        "}",
        ".tiles-bank-bar {",
        "  display: flex;",
        "  align-items: center;",
        "  gap: 8px;",
        "  margin-top: 4px;",
        "}",
        ".tiles-btn {",
        "  appearance: none;",
        "  background: var(--tiles-paper);",
        "  color: var(--tiles-ink);",
        "  border: 1.5px solid var(--tiles-ink);",
        "  padding: 5px 11px;",
        "  font-family: 'Exo 2', sans-serif;",
        "  font-weight: 600;",
        "  font-size: 12px;",
        "  cursor: pointer;",
        "}",
        ".tiles-btn:hover {",
        "  background: var(--tiles-bg);",
        "}",
        ".tiles-btn.is-primary {",
        "  background: var(--tiles-ink);",
        "  color: var(--tiles-ink-light);",
        "}",
        ".tiles-btn.is-primary:hover {",
        "  border-color: var(--tiles-magenta);",
        "}",
        ".tiles-section {",
        "  padding-top: 20px;",
        "}",
        ".tiles-section-head {",
        "  display: flex;",
        "  align-items: baseline;",
        "  justify-content: space-between;",
        "  flex-wrap: wrap;",
        "  gap: 8px;",
        "  padding-bottom: 6px;",
        "  margin-bottom: 12px;",
        "  border-bottom: 1.5px solid var(--tiles-ink);",
        "}",
        ".tiles-section-title {",
        "  margin: 0;",
        "  font-family: 'Exo 2', sans-serif;",
        "  font-weight: 700;",
        "  font-size: 16px;",
        "}",
        ".tiles-section-meta {",
        "  font-size: 12px;",
        "  color: var(--tiles-ink-2);",
        "}",
        "/* Interactive TILES 24-pad hardware face CSS grid */",
        ".tiles-hw-layout {",
        "  display: grid;",
        "  grid-template-columns: minmax(0, 1.75fr) minmax(250px, 1fr);",
        "  gap: 18px;",
        "  align-items: start;",
        "}",
        "@media (max-width: 780px) {",
        "  .tiles-hw-layout {",
        "    grid-template-columns: 1fr;",
        "  }",
        "}",
        ".tiles-hw-surface {",
        "  padding: 14px;",
        "  background: var(--tiles-ink);",
        "  border: 1.5px solid var(--tiles-ink);",
        "  color: var(--tiles-ink-light);",
        "}",
        ".tiles-hw-keys {",
        "  display: grid;",
        "  grid-template-columns: repeat(6, minmax(0, 1fr));",
        "  gap: 8px;",
        "  padding-bottom: 10px;",
        "  margin-bottom: 10px;",
        "  border-bottom: 1px solid rgba(201, 207, 238, 0.2);",
        "}",
        ".tiles-hw-key {",
        "  appearance: none;",
        "  display: flex;",
        "  flex-direction: column;",
        "  align-items: center;",
        "  justify-content: center;",
        "  min-height: 38px;",
        "  padding: 4px 6px;",
        "  background: rgba(230, 233, 246, 0.08);",
        "  border: 1px solid rgba(201, 207, 238, 0.25);",
        "  color: var(--tiles-ink-light);",
        "  font-family: 'Sora', sans-serif;",
        "  font-size: 12px;",
        "  cursor: pointer;",
        "}",
        ".tiles-hw-key:hover {",
        "  border-color: var(--tiles-periwinkle);",
        "}",
        ".tiles-hw-key-glyph {",
        "  font-family: 'Exo 2', sans-serif;",
        "  font-weight: 700;",
        "  font-size: 13px;",
        "  line-height: 1;",
        "}",
        ".tiles-hw-key-name {",
        "  font-size: 10px;",
        "  color: #9ca3c6;",
        "  text-transform: uppercase;",
        "  letter-spacing: 0.05em;",
        "}",
        ".tiles-hw-grid-24 {",
        "  display: grid;",
        "  grid-template-columns: repeat(6, minmax(0, 1fr));",
        "  grid-template-rows: repeat(4, minmax(74px, auto));",
        "  gap: 8px;",
        "}",
        ".tiles-hw-pad {",
        "  appearance: none;",
        "  position: relative;",
        "  display: flex;",
        "  flex-direction: column;",
        "  justify-content: space-between;",
        "  text-align: left;",
        "  padding: 7px 8px;",
        "  background: #232a4f;",
        "  border: 1.5px solid rgba(201, 207, 238, 0.22);",
        "  border-radius: 6px;",
        "  color: var(--tiles-ink-light);",
        "  cursor: pointer;",
        "  transition: border-color 140ms ease, background-color 140ms ease;",
        "}",
        ".tiles-hw-pad:hover {",
        "  border-color: var(--tiles-periwinkle);",
        "}",
        ".tiles-hw-pad.is-active-pad {",
        "  background: #2a3464;",
        "  border-color: var(--tiles-periwinkle);",
        "}",
        ".tiles-hw-pad.is-selected {",
        "  border-color: var(--tiles-magenta);",
        "  box-shadow: inset 0 0 0 1px var(--tiles-magenta);",
        "}",
        ".tiles-hw-pad-top {",
        "  display: flex;",
        "  justify-content: space-between;",
        "  align-items: center;",
        "  width: 100%;",
        "}",
        ".tiles-hw-pad-num {",
        "  font-family: 'Exo 2', sans-serif;",
        "  font-weight: 700;",
        "  font-size: 12px;",
        "  color: #9ca3c6;",
        "}",
        ".tiles-hw-pad.is-selected .tiles-hw-pad-num {",
        "  color: var(--tiles-magenta);",
        "}",
        ".tiles-hw-pad-cc {",
        "  font-family: 'Exo 2', sans-serif;",
        "  font-weight: 600;",
        "  font-size: 11px;",
        "  color: var(--tiles-periwinkle);",
        "}",
        ".tiles-hw-pad-fn {",
        "  font-size: 12px;",
        "  font-weight: 600;",
        "  color: var(--tiles-ink-light);",
        "  white-space: nowrap;",
        "  overflow: hidden;",
        "  text-overflow: ellipsis;",
        "}",
        ".tiles-hw-pad-sub {",
        "  font-size: 11px;",
        "  color: #9ca3c6;",
        "}",
        ".tiles-mapper-panel {",
        "  padding: 14px;",
        "  background: var(--tiles-bg);",
        "  border: 1.5px solid var(--tiles-line);",
        "}",
        ".tiles-mapper-title {",
        "  margin: 0 0 10px;",
        "  font-family: 'Exo 2', sans-serif;",
        "  font-weight: 700;",
        "  font-size: 15px;",
        "  border-bottom: 1px solid var(--tiles-ink);",
        "  padding-bottom: 6px;",
        "}",
        ".tiles-mapper-row {",
        "  display: flex;",
        "  flex-direction: column;",
        "  gap: 4px;",
        "  margin-bottom: 10px;",
        "}",
        ".tiles-mapper-row label {",
        "  font-size: 12px;",
        "  text-transform: uppercase;",
        "  letter-spacing: 0.06em;",
        "  color: var(--tiles-ink-2);",
        "}",
        ".tiles-input, .tiles-select {",
        "  width: 100%;",
        "  padding: 6px 8px;",
        "  border: 1.5px solid var(--tiles-line);",
        "  background: var(--tiles-paper);",
        "  color: var(--tiles-ink);",
        "  font-family: 'Exo 2', sans-serif;",
        "  font-weight: 600;",
        "  font-size: 13px;",
        "}",
        ".tiles-mapper-actions {",
        "  display: flex;",
        "  flex-wrap: wrap;",
        "  gap: 8px;",
        "  margin-top: 10px;",
        "}",
        "/* Live Loops 5x4 Matrix */",
        ".tiles-matrix {",
        "  display: grid;",
        "  grid-template-columns: repeat(6, minmax(0, 1fr));",
        "  gap: 8px;",
        "}",
        ".tiles-col-head {",
        "  font-family: 'Exo 2', sans-serif;",
        "  font-weight: 700;",
        "  font-size: 12px;",
        "  text-transform: uppercase;",
        "  letter-spacing: 0.06em;",
        "  color: var(--tiles-ink-2);",
        "  padding-bottom: 4px;",
        "  border-bottom: 1px solid var(--tiles-line);",
        "}",
        ".tiles-pad {",
        "  display: flex;",
        "  flex-direction: column;",
        "  justify-content: space-between;",
        "  min-height: 68px;",
        "  padding: 8px 9px;",
        "  background: var(--tiles-bg);",
        "  border: 1.5px solid var(--tiles-line);",
        "  color: var(--tiles-ink);",
        "  cursor: pointer;",
        "}",
        ".tiles-pad.is-queued {",
        "  border-color: var(--tiles-periwinkle);",
        "  background: #edf1fe;",
        "}",
        ".tiles-pad.is-playing {",
        "  background: var(--tiles-ink);",
        "  border-color: var(--tiles-magenta);",
        "  color: var(--tiles-ink-light);",
        "}",
        ".tiles-pad.is-recording {",
        "  border-color: var(--tiles-magenta);",
        "  background: #fff0ff;",
        "}",
        ".tiles-pad.is-scene {",
        "  background: var(--tiles-paper);",
        "  border-color: var(--tiles-ink-2);",
        "}",
        ".tiles-pad.is-scene.is-active {",
        "  background: var(--tiles-ink);",
        "  border-color: var(--tiles-magenta);",
        "  color: var(--tiles-ink-light);",
        "}",
        ".tiles-pad-num {",
        "  font-family: 'Exo 2', sans-serif;",
        "  font-weight: 700;",
        "  font-size: 12px;",
        "}",
        ".tiles-pad-state {",
        "  font-size: 12px;",
        "  font-weight: 600;",
        "}",
        ".tiles-pad-note {",
        "  font-size: 12px;",
        "  color: var(--tiles-ink-2);",
        "}",
        ".tiles-pad.is-playing .tiles-pad-note, .tiles-pad.is-scene.is-active .tiles-pad-note {",
        "  color: var(--tiles-periwinkle);",
        "}",
        ".tiles-mixer-table {",
        "  width: 100%;",
        "  border-collapse: collapse;",
        "}",
        ".tiles-mixer-table th, .tiles-mixer-table td {",
        "  text-align: left;",
        "  padding: 8px 10px;",
        "  border-bottom: 1px solid var(--tiles-line);",
        "  font-size: 13px;",
        "}",
        ".tiles-mixer-table th {",
        "  font-family: 'Exo 2', sans-serif;",
        "  font-weight: 700;",
        "  font-size: 12px;",
        "  text-transform: uppercase;",
        "  letter-spacing: 0.06em;",
        "  color: var(--tiles-ink-2);",
        "}",
        ".tiles-flag {",
        "  display: inline-block;",
        "  min-width: 44px;",
        "  padding: 2px 6px;",
        "  border: 1px solid var(--tiles-line);",
        "  background: var(--tiles-bg);",
        "  font-family: 'Exo 2', sans-serif;",
        "  font-weight: 600;",
        "  font-size: 12px;",
        "  text-align: center;",
        "}",
        ".tiles-flag.is-on {",
        "  border-color: var(--tiles-ink);",
        "  background: var(--tiles-periwinkle);",
        "  color: var(--tiles-ink);",
        "}",
        ".tiles-flag.is-sel {",
        "  border-color: var(--tiles-magenta);",
        "  background: var(--tiles-ink);",
        "  color: var(--tiles-magenta);",
        "}",
        ".tiles-col6-row {",
        "  margin-top: 10px;",
        "  display: grid;",
        "  grid-template-columns: repeat(4, minmax(0, 1fr));",
        "  gap: 8px;",
        "}",
        ".tiles-col6-item {",
        "  padding: 8px 10px;",
        "  border: 1px solid var(--tiles-line);",
        "  background: var(--tiles-bg);",
        "  font-size: 12px;",
        "}",
        ".tiles-col6-item strong {",
        "  display: block;",
        "  font-family: 'Exo 2', sans-serif;",
        "  font-size: 13px;",
        "}"
    ].join("\n");
}

function renderHardware24PadGrid(st) {
    var html = [];
    var selMap = getEffectivePadMapping(st.selectedPad);

    html.push('  <div class="tiles-section">');
    html.push('    <div class="tiles-section-head">');
    html.push('      <h2 class="tiles-section-title">1. TILES 24-Pad Hardware MIDI Mapper</h2>');
    html.push('      <span class="tiles-section-meta">Click any pad (1–24) to inspect or remap MIDI Learn CCs for Logic Pro</span>');
    html.push('    </div>');
    html.push('    <div class="tiles-hw-layout">');

    /* Left: 6x4 TILES hardware layout + top function keys */
    html.push('      <div class="tiles-hw-surface" aria-label="SENTIA TILES 24-Pad Hardware Grid">');
    html.push('        <div class="tiles-hw-keys">');
    html.push('          <button type="button" class="tiles-hw-key" data-action="bank-prev"><span class="tiles-hw-key-glyph">&minus;</span><span class="tiles-hw-key-name">Bank -5</span></button>');
    html.push('          <button type="button" class="tiles-hw-key" data-action="bank-next"><span class="tiles-hw-key-glyph">+</span><span class="tiles-hw-key-name">Bank +5</span></button>');
    html.push('          <button type="button" class="tiles-hw-key" data-action="set-page" data-page="0"><span class="tiles-hw-key-glyph">&#9651;</span><span class="tiles-hw-key-name">Loops</span></button>');
    html.push('          <button type="button" class="tiles-hw-key" data-action="set-page" data-page="1"><span class="tiles-hw-key-glyph">&#9671;</span><span class="tiles-hw-key-name">Smart</span></button>');
    html.push('          <button type="button" class="tiles-hw-key" data-action="set-page" data-page="2"><span class="tiles-hw-key-glyph">&#9633;</span><span class="tiles-hw-key-name">Mixer</span></button>');
    html.push('          <button type="button" class="tiles-hw-key" data-action="send-learn-pulse"><span class="tiles-hw-key-glyph">&#9675;</span><span class="tiles-hw-key-name">Learn</span></button>');
    html.push('        </div>');

    html.push('        <div class="tiles-hw-grid-24">');
    for (var pad = 1; pad <= NUM_PADS; pad++) {
        var map = getEffectivePadMapping(pad);
        var isSel = st.selectedPad === pad;
        var col0 = (pad - 1) % NUM_COLS;
        var row0 = Math.floor((pad - 1) / NUM_COLS);
        var activeCell = (st.page === PAGE_LIVE_LOOPS && col0 < VISIBLE_TRACKS && st.cells[row0] && st.cells[row0][col0] === 2);

        html.push(
            '          <button type="button" class="tiles-hw-pad' +
            (activeCell ? " is-active-pad" : "") +
            (isSel ? " is-selected" : "") +
            '" data-action="select-pad" data-pad="' + pad + '">' +
            '<span class="tiles-hw-pad-top">' +
            '<span class="tiles-hw-pad-num">' + pad + '</span>' +
            '<span class="tiles-hw-pad-cc">CC ' + map.cc + '</span>' +
            '</span>' +
            '<span class="tiles-hw-pad-fn">' + escapeHtml(map.label) + '</span>' +
            '<span class="tiles-hw-pad-sub">Ch ' + map.channel + (map.custom ? ' · Custom' : '') + '</span>' +
            '</button>'
        );
    }
    html.push('        </div>');
    html.push('      </div>');

    /* Right: Selected pad MIDI Mapping Inspector */
    html.push('      <div class="tiles-mapper-panel" data-selected-pad="' + selMap.pad + '">');
    html.push('        <h3 class="tiles-mapper-title">Pad ' + selMap.pad + ' MIDI Mapping (' + escapeHtml(selMap.label) + ')</h3>');
    html.push('        <div class="tiles-mapper-row">');
    html.push('          <label for="tiles-map-cc">Learn CC Number (0–127)</label>');
    html.push('          <input id="tiles-map-cc" class="tiles-input" type="number" min="0" max="127" value="' + selMap.cc + '" data-field="pad-cc">');
    html.push('        </div>');
    html.push('        <div class="tiles-mapper-row">');
    html.push('          <label for="tiles-map-ch">MIDI Channel (1–16)</label>');
    html.push('          <input id="tiles-map-ch" class="tiles-input" type="number" min="1" max="16" value="' + selMap.channel + '" data-field="pad-ch">');
    html.push('        </div>');
    html.push('        <div class="tiles-mapper-row">');
    html.push('          <label for="tiles-map-axis">Expression Axis</label>');
    html.push('          <select id="tiles-map-axis" class="tiles-select" data-field="pad-axis">');
    var axes = ["Touch/Strike", "Strike CC 20", "Press CC 21", "Tilt CC 22", "Touch CC 23", "MCU Strip", "Mode Switch", "Trigger", "Control"];
    for (var a = 0; a < axes.length; a++) {
        var selectedAttr = selMap.exprAxis === axes[a] ? " selected" : "";
        html.push('            <option value="' + escapeHtml(axes[a]) + '"' + selectedAttr + '>' + escapeHtml(axes[a]) + '</option>');
    }
    html.push('          </select>');
    html.push('        </div>');
    html.push('        <div class="tiles-metric-label">Last Learn Pulse: ' + (st.lastLearnPulsePad > 0 ? ('Pad ' + st.lastLearnPulsePad) : 'None') + '</div>');
    html.push('        <div class="tiles-mapper-actions">');
    html.push('          <button type="button" class="tiles-btn is-primary" data-action="send-learn-pulse">Send Cmd+L Learn Pulse</button>');
    html.push('          <button type="button" class="tiles-btn" data-action="reset-pad-map">Reset Pad</button>');
    html.push('        </div>');
    html.push('      </div>');

    html.push('    </div>');
    html.push('  </div>');
    return html.join("\n");
}

function renderMarkup(state) {
    var st = state || uiState;
    var firstTrack = st.trackOffset + 1;
    var lastTrack = st.trackOffset + VISIBLE_TRACKS;
    var mcuFirst = st.mcuBank * 8 + 1;
    var mcuLast = st.mcuBank * 8 + 8;

    var html = [];
    html.push('<section class="tiles-ui" data-page="' + st.page + '">');
    html.push('  <header class="tiles-ui-head">');
    html.push('    <div>');
    html.push('      <h1 class="tiles-ui-title">SENTIA TILES</h1>');
    html.push('      <div class="tiles-ui-sub">Logic Pro Scripter Bank, Mixer &amp; 24-Pad Mapper</div>');
    html.push('    </div>');
    html.push('    <div class="tiles-page-tabs" role="tablist" aria-label="Bridge Page">');

    var pages = [
        { id: PAGE_LIVE_LOOPS, label: "Live Loops" },
        { id: PAGE_SMART_CONTROLS, label: "Smart Controls" },
        { id: PAGE_MIXER, label: "Mixer" }
    ];
    for (var p = 0; p < pages.length; p++) {
        var isCur = st.page === pages[p].id;
        html.push(
            '      <button type="button" class="tiles-tab' + (isCur ? " is-active" : "") +
            '" data-action="set-page" data-page="' + pages[p].id + '">' +
            '<span class="tiles-tab-dot"></span>' + escapeHtml(pages[p].label) + "</button>"
        );
    }
    html.push('    </div>');
    html.push('  </header>');

    html.push('  <div class="tiles-status-strip">');
    html.push('    <div>');
    html.push('      <div class="tiles-metric-label">Current Page (CC 119)</div>');
    html.push('      <div class="tiles-metric-val">' + escapeHtml(pageName(st.page)) + '</div>');
    html.push('    </div>');
    html.push('    <div>');
    html.push('      <div class="tiles-metric-label">Live Loops Track Window</div>');
    html.push('      <div class="tiles-metric-val">T' + firstTrack + ' – T' + lastTrack + '</div>');
    html.push('      <div class="tiles-bank-bar">');
    html.push('        <button type="button" class="tiles-btn" data-action="bank-prev">-5 Tracks</button>');
    html.push('        <button type="button" class="tiles-btn" data-action="bank-next">+5 Tracks</button>');
    html.push('      </div>');
    html.push('    </div>');
    html.push('    <div>');
    html.push('      <div class="tiles-metric-label">MCU 8-Channel Bank</div>');
    html.push('      <div class="tiles-metric-val">Bank ' + (st.mcuBank + 1) + ' (Ch ' + mcuFirst + '–' + mcuLast + ')</div>');
    html.push('    </div>');
    html.push('    <div>');
    html.push('      <div class="tiles-metric-label">Auto-Assign Split / Cycle</div>');
    html.push('      <div class="tiles-metric-val">' + (st.exprSplit ? "MPE + CC" : "MPE Only") + ' / ' + (st.cycleActive ? "Cycle On" : "Cycle Off") + '</div>');
    html.push('    </div>');
    html.push('  </div>');

    /* Section 1: Interactive 24-Pad Hardware Layout CSS Grid + Mapper */
    html.push(renderHardware24PadGrid(st));

    /* Section 2: Live Loops 5x4 Bank Status + Column 6 Scenes */
    html.push('  <div class="tiles-section">');
    html.push('    <div class="tiles-section-head">');
    html.push('      <h2 class="tiles-section-title">2. Live Loops Bank Status (5x4 Matrix)</h2>');
    html.push('      <span class="tiles-section-meta">TILES Logic Grid · Ch 16 CC 24–30 · Notes 36–55</span>');
    html.push('    </div>');
    html.push('    <div class="tiles-matrix">');

    for (var col = 0; col < VISIBLE_TRACKS; col++) {
        html.push('      <div class="tiles-col-head">Track ' + (firstTrack + col) + '</div>');
    }
    html.push('      <div class="tiles-col-head">Col 6 Scene</div>');

    for (var row = 0; row < NUM_SCENES; row++) {
        for (var tr = 0; tr < VISIBLE_TRACKS; tr++) {
            var padNum = row * NUM_COLS + tr + 1;
            var visIdx = row * VISIBLE_TRACKS + tr;
            var cellSt = st.cells[row] ? st.cells[row][tr] : 0;
            html.push(
                '      <div class="tiles-pad ' + cellStateClass(cellSt) + '" data-action="select-pad" data-pad="' + padNum + '" data-track="' + (tr + 1) + '" data-scene="' + (row + 1) + '">' +
                '<span class="tiles-pad-num">Pad ' + padNum + '</span>' +
                '<span class="tiles-pad-state">' + escapeHtml(cellStateLabel(cellSt)) + '</span>' +
                '<span class="tiles-pad-note">Note ' + (GRID_NOTE_BASE + visIdx) + '</span>' +
                '</div>'
            );
        }
        var scenePadNum = row * NUM_COLS + 6;
        var sceneActive = st.activeScene === row;
        html.push(
            '      <div class="tiles-pad is-scene' + (sceneActive ? " is-active" : "") + '" data-action="select-pad" data-pad="' + scenePadNum + '">' +
            '<span class="tiles-pad-num">Pad ' + scenePadNum + '</span>' +
            '<span class="tiles-pad-state">Scene ' + (row + 1) + '</span>' +
            '<span class="tiles-pad-note">CC 28 = ' + (row + 1) + '</span>' +
            '</div>'
        );
    }
    html.push('    </div>');
    html.push('  </div>');

    /* Section 3: Current Mixer Page Mapping */
    html.push('  <div class="tiles-section">');
    html.push('    <div class="tiles-section-head">');
    html.push('      <h2 class="tiles-section-title">3. Current Mixer Page Mapping (' + escapeHtml(pageName(st.page)) + ')</h2>');
    html.push('      <span class="tiles-section-meta">TILES Logic MCU · Cross-Bank Aligned (abs_track % 8)</span>');
    html.push('    </div>');
    html.push('    <table class="tiles-mixer-table">');
    html.push('      <thead>');
    html.push('        <tr>');
    html.push('          <th>Column</th>');
    html.push('          <th>Logic Track</th>');
    html.push('          <th>MCU Bank / Slot</th>');
    html.push('          <th>Row 1 Mute (0x10)</th>');
    html.push('          <th>Row 2 Solo (0x08)</th>');
    html.push('          <th>Row 3 Rec Arm (0x00)</th>');
    html.push('          <th>Row 4 Select (0x18)</th>');
    html.push('        </tr>');
    html.push('      </thead>');
    html.push('      <tbody>');

    for (var m = 0; m < VISIBLE_TRACKS; m++) {
        var absTrack = st.trackOffset + m;
        var strip = st.strips[absTrack] || { muted: false, soloed: false, recArmed: false, selected: false };
        var bankIdx = Math.floor(absTrack / 8) + 1;
        var slotIdx = absTrack % 8;
        html.push('        <tr>');
        html.push('          <td>Col ' + (m + 1) + '</td>');
        html.push('          <td><strong>Track ' + (absTrack + 1) + '</strong></td>');
        html.push('          <td>Bank ' + bankIdx + ' · Slot ' + (slotIdx + 1) + '</td>');
        html.push('          <td><span class="tiles-flag' + (strip.muted ? " is-on" : "") + '">Pad ' + (m + 1) + (strip.muted ? " ON" : "") + '</span></td>');
        html.push('          <td><span class="tiles-flag' + (strip.soloed ? " is-on" : "") + '">Pad ' + (m + 7) + (strip.soloed ? " ON" : "") + '</span></td>');
        html.push('          <td><span class="tiles-flag' + (strip.recArmed ? " is-on" : "") + '">Pad ' + (m + 13) + (strip.recArmed ? " ON" : "") + '</span></td>');
        html.push('          <td><span class="tiles-flag' + (strip.selected ? " is-sel" : "") + '">Pad ' + (m + 19) + (strip.selected ? " SEL" : "") + '</span></td>');
        html.push('        </tr>');
    }

    html.push('      </tbody>');
    html.push('    </table>');
    html.push('    <div class="tiles-col6-row">');
    html.push('      <div class="tiles-col6-item"><strong>Pad 6 (Col 6, Row 1)</strong>Next Page (Live Loops &lt;-&gt; Mixer)</div>');
    html.push('      <div class="tiles-col6-item"><strong>Pad 12 (Col 6, Row 2)</strong>MCU Channel Left (-1 Track · 0x30)</div>');
    html.push('      <div class="tiles-col6-item"><strong>Pad 18 (Col 6, Row 3)</strong>MCU Channel Right (+1 Track · 0x31)</div>');
    html.push('      <div class="tiles-col6-item"><strong>Pad 24 (Col 6, Row 4)</strong>Logic Cycle Toggle (0x56 · ' + (st.cycleActive ? "On" : "Off") + ')</div>');
    html.push('    </div>');
    html.push('  </div>');
    html.push('</section>');
    return html.join("\n");
}

function renderHTML(state) {
    return [
        "<!DOCTYPE html>",
        '<html lang="en">',
        "<head>",
        '<meta charset="utf-8">',
        '<meta name="viewport" content="width=device-width, initial-scale=1">',
        "<title>SENTIA TILES — Logic Pro 24-Pad Mapper &amp; Bank UI</title>",
        "<style>",
        renderCSS(),
        "</style>",
        "</head>",
        '<body style="margin:0;background:#eef0f7;">',
        renderMarkup(state),
        "</body>",
        "</html>"
    ].join("\n");
}

function refreshDOM() {
    if (typeof document === "undefined" || !mountedRoot) return;
    mountedRoot.innerHTML = renderMarkup(uiState);
}

function applyMidiMessage(status, data1, data2) {
    var msgType = status & 0xF0;
    var channel = (status & 0x0F) + 1;

    if (msgType === 0xB0) {
        if (data1 === GRID_CC_PAGE_SELECT && channel === 1) {
            if (data2 === 127) {
                uiState.page = (uiState.page + 1) % 3;
            } else {
                uiState.page = clamp(data2, 0, 2);
            }
        } else if (data1 === GRID_CC_EXPR_SPLIT && channel === 1) {
            uiState.exprSplit = data2 > 0;
        } else if (data1 === DAW_CC_TRACK_OFFSET && channel === 1) {
            setTrackOffset(data2);
        } else if (data1 === GRID_CC_BANK_STEP && channel === 1) {
            stepBank(data2 < 64 ? -VISIBLE_TRACKS : VISIBLE_TRACKS);
        } else if (channel === 16) {
            var coords = pad1ToVisibleCoords(data2);
            if (data1 === GRID_CC_CELL_FIRE && coords) {
                var cur = uiState.cells[coords.scene][coords.track];
                uiState.cells[coords.scene][coords.track] = cur === 2 ? 0 : 2;
            } else if ((data1 === GRID_CC_CELL_STOP || data1 === GRID_CC_CELL_DELETE) && coords) {
                uiState.cells[coords.scene][coords.track] = 0;
            } else if (data1 === GRID_CC_CELL_RECORD && coords) {
                uiState.cells[coords.scene][coords.track] = 3;
            } else if (data1 === GRID_CC_SCENE_FIRE && data2 >= 1 && data2 <= NUM_SCENES) {
                uiState.activeScene = data2 - 1;
                for (var t = 0; t < VISIBLE_TRACKS; t++) {
                    if (uiState.cells[uiState.activeScene][t] > 0) {
                        uiState.cells[uiState.activeScene][t] = 2;
                    }
                }
            } else if (data1 === GRID_CC_MASTER_STOP && data2 > 0) {
                uiState.activeScene = -1;
                for (var s = 0; s < NUM_SCENES; s++) {
                    for (var c = 0; c < VISIBLE_TRACKS; c++) {
                        uiState.cells[s][c] = 0;
                    }
                }
            } else if (data1 === GRID_CC_END_CAPTURE && data2 > 0) {
                for (var r = 0; r < NUM_SCENES; r++) {
                    for (var k = 0; k < VISIBLE_TRACKS; k++) {
                        if (uiState.cells[r][k] === 3) {
                            uiState.cells[r][k] = 2;
                        }
                    }
                }
            }
        }
    } else if (msgType === 0x90 && channel === 1) {
        if (data1 >= GRID_NOTE_BASE && data1 < GRID_NOTE_BASE + VISIBLE_TRACKS * NUM_SCENES) {
            var idx = data1 - GRID_NOTE_BASE;
            var sc = Math.floor(idx / VISIBLE_TRACKS);
            var tr = idx % VISIBLE_TRACKS;
            if (data2 === 0) {
                uiState.cells[sc][tr] = 0;
            } else if (data2 < 64) {
                uiState.cells[sc][tr] = 1;
            } else {
                uiState.cells[sc][tr] = 2;
            }
        } else if (data1 >= MCU_NOTE_ARM_BASE && data1 < MCU_NOTE_ARM_BASE + 8) {
            var armAbs = uiState.mcuBank * 8 + (data1 - MCU_NOTE_ARM_BASE);
            if (armAbs < MAX_TRACKS) uiState.strips[armAbs].recArmed = data2 > 0;
        } else if (data1 >= MCU_NOTE_SOLO_BASE && data1 < MCU_NOTE_SOLO_BASE + 8) {
            var soloAbs = uiState.mcuBank * 8 + (data1 - MCU_NOTE_SOLO_BASE);
            if (soloAbs < MAX_TRACKS) uiState.strips[soloAbs].soloed = data2 > 0;
        } else if (data1 >= MCU_NOTE_MUTE_BASE && data1 < MCU_NOTE_MUTE_BASE + 8) {
            var muteAbs = uiState.mcuBank * 8 + (data1 - MCU_NOTE_MUTE_BASE);
            if (muteAbs < MAX_TRACKS) uiState.strips[muteAbs].muted = data2 > 0;
        } else if (data1 >= MCU_NOTE_SEL_BASE && data1 < MCU_NOTE_SEL_BASE + 8) {
            var selAbs = uiState.mcuBank * 8 + (data1 - MCU_NOTE_SEL_BASE);
            if (selAbs < MAX_TRACKS) uiState.strips[selAbs].selected = data2 > 0;
        } else if (data1 === MCU_NOTE_CYCLE) {
            uiState.cycleActive = data2 > 0;
        } else if (data1 === MCU_NOTE_PLAY) {
            uiState.playing = data2 > 0;
        }
    }

    refreshDOM();
}

/* Logic Pro Scripter callbacks */
function ParameterChanged(param, value) {
    if (param === PARAM_PAGE) {
        uiState.page = clamp(Math.round(value), 0, 2);
        emitScripterCC(1, GRID_CC_PAGE_SELECT, uiState.page);
    } else if (param === PARAM_TRACK_OFFSET) {
        setTrackOffset(value);
        emitScripterCC(1, DAW_CC_TRACK_OFFSET, uiState.trackOffset);
    } else if (param === PARAM_BANK_STEP) {
        var choice = Math.round(value);
        if (choice === 1) {
            stepBank(-VISIBLE_TRACKS);
            emitScripterCC(1, GRID_CC_BANK_STEP, 0);
        } else if (choice === 2) {
            stepBank(VISIBLE_TRACKS);
            emitScripterCC(1, GRID_CC_BANK_STEP, 127);
        }
    } else if (param === PARAM_EXPR_SPLIT) {
        uiState.exprSplit = value >= 0.5;
        emitScripterCC(1, GRID_CC_EXPR_SPLIT, uiState.exprSplit ? 127 : 0);
    } else if (param === PARAM_SELECTED_PAD) {
        selectPad(value);
    } else if (param === PARAM_PAD_LEARN_CC) {
        setPadCustomMapping(uiState.selectedPad, value, undefined, undefined);
    } else if (param === PARAM_PAD_LEARN_CH) {
        setPadCustomMapping(uiState.selectedPad, undefined, value, undefined);
    } else if (param === PARAM_PAD_LEARN_PULSE && value >= 0.5) {
        emitLearnPulse(uiState.selectedPad);
    } else if (param === PARAM_CELL_STATE) {
        var cellVal = clamp(Math.round(value), 0, 3);
        uiState.cells[uiState.inspectScene - 1][uiState.inspectTrack - 1] = cellVal;
    } else if (param === PARAM_DUMP_HTML && value >= 0.5) {
        if (typeof Trace === "function") {
            Trace(renderHTML(uiState));
        }
    }
    refreshDOM();
}

function HandleMIDI(event) {
    event.send();

    if (typeof ControlChange !== "undefined" && event instanceof ControlChange) {
        applyMidiMessage(0xB0 | ((event.channel - 1) & 0x0F), event.number & 0x7F, event.value & 0x7F);
    } else if (typeof NoteOn !== "undefined" && event instanceof NoteOn) {
        applyMidiMessage(0x90 | ((event.channel - 1) & 0x0F), event.pitch & 0x7F, event.velocity & 0x7F);
    } else if (typeof NoteOff !== "undefined" && event instanceof NoteOff) {
        applyMidiMessage(0x80 | ((event.channel - 1) & 0x0F), event.pitch & 0x7F, 0);
    }
}

function ProcessMIDI() {
    if (typeof GetTimingInfo !== "function") return;
    var info = GetTimingInfo();
    if (wasPlaying !== !!info.playing) {
        uiState.playing = !!info.playing;
        refreshDOM();
    }
    wasPlaying = !!info.playing;
}

function Reset() {
    uiState = createDefaultState();
    refreshDOM();
}

/* Browser / WebView HUD API */
function mount(rootElement) {
    if (typeof document === "undefined" || !rootElement) return null;

    var styleId = "sentia-tiles-logic-scripter-ui-css";
    if (!document.getElementById(styleId)) {
        var style = document.createElement("style");
        style.id = styleId;
        style.textContent = renderCSS();
        document.head.appendChild(style);
    }

    mountedRoot = rootElement;
    if (!mountedRoot._tilesUiBound) {
        mountedRoot.addEventListener("click", function (ev) {
            var btn = ev.target && ev.target.closest ? ev.target.closest("[data-action]") : null;
            if (!btn) return;
            var action = btn.getAttribute("data-action");
            if (action === "set-page") {
                var p = parseInt(btn.getAttribute("data-page"), 10);
                if (!isNaN(p)) {
                    uiState.page = clamp(p, 0, 2);
                    emitScripterCC(1, GRID_CC_PAGE_SELECT, uiState.page);
                    refreshDOM();
                }
            } else if (action === "bank-prev") {
                stepBank(-VISIBLE_TRACKS);
                emitScripterCC(1, GRID_CC_BANK_STEP, 0);
                refreshDOM();
            } else if (action === "bank-next") {
                stepBank(VISIBLE_TRACKS);
                emitScripterCC(1, GRID_CC_BANK_STEP, 127);
                refreshDOM();
            } else if (action === "select-pad") {
                var pad = parseInt(btn.getAttribute("data-pad"), 10);
                if (!isNaN(pad)) {
                    selectPad(pad);
                    refreshDOM();
                }
            } else if (action === "send-learn-pulse") {
                emitLearnPulse(uiState.selectedPad);
                refreshDOM();
            } else if (action === "reset-pad-map") {
                resetPadCustomMapping(uiState.selectedPad);
                refreshDOM();
            }
        });

        mountedRoot.addEventListener("change", function (ev) {
            var el = ev.target;
            if (!el || !el.getAttribute) return;
            var field = el.getAttribute("data-field");
            if (!field) return;

            if (field === "pad-cc") {
                var ccVal = parseInt(el.value, 10);
                if (!isNaN(ccVal)) {
                    setPadCustomMapping(uiState.selectedPad, ccVal, undefined, undefined);
                    refreshDOM();
                }
            } else if (field === "pad-ch") {
                var chVal = parseInt(el.value, 10);
                if (!isNaN(chVal)) {
                    setPadCustomMapping(uiState.selectedPad, undefined, chVal, undefined);
                    refreshDOM();
                }
            } else if (field === "pad-axis") {
                setPadCustomMapping(uiState.selectedPad, undefined, undefined, el.value);
                refreshDOM();
            }
        });

        mountedRoot._tilesUiBound = true;
    }

    refreshDOM();
    return mountedRoot;
}

function unmount() {
    if (mountedRoot) {
        mountedRoot.innerHTML = "";
        mountedRoot = null;
    }
}

var LogicScripterUI = {
    getState: function () { return uiState; },
    getEffectivePadMapping: getEffectivePadMapping,
    selectPad: function (pad1) {
        selectPad(pad1);
        refreshDOM();
    },
    setPadCustomMapping: function (pad1, cc, channel, exprAxis) {
        setPadCustomMapping(pad1, cc, channel, exprAxis);
        refreshDOM();
    },
    resetPadCustomMapping: function (pad1) {
        resetPadCustomMapping(pad1);
        refreshDOM();
    },
    emitLearnPulse: function (pad1) {
        emitLearnPulse(pad1 !== undefined ? pad1 : uiState.selectedPad);
        refreshDOM();
    },
    setPage: function (page) {
        uiState.page = clamp(Math.round(page), 0, 2);
        refreshDOM();
    },
    setTrackOffset: function (offset) {
        setTrackOffset(offset);
        refreshDOM();
    },
    stepBank: function (delta) {
        stepBank(delta);
        refreshDOM();
    },
    applyMidiMessage: applyMidiMessage,
    renderCSS: renderCSS,
    renderMarkup: renderMarkup,
    renderHTML: renderHTML,
    mount: mount,
    unmount: unmount,
    reset: Reset
};

if (typeof module !== "undefined" && module.exports) {
    module.exports = {
        LogicScripterUI: LogicScripterUI,
        PluginParameters: PluginParameters,
        HandleMIDI: HandleMIDI,
        ParameterChanged: ParameterChanged,
        ProcessMIDI: ProcessMIDI,
        Reset: Reset
    };
}
