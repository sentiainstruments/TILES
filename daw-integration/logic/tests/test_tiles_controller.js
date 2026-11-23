"use strict";

const assert = require("assert");
const fs = require("fs");
const path = require("path");
const vm = require("vm");

const scriptPath = path.join(__dirname, "..", "scripter", "TILES_Controller.js");
const code = fs.readFileSync(scriptPath, "utf8");

const sentEvents = [];

function makeSandbox() {
    function ControlChange() {
        this.channel = 1;
        this.number = 0;
        this.value = 0;
    }
    ControlChange.prototype.send = function () {
        sentEvents.push({ type: "cc", channel: this.channel, number: this.number, value: this.value });
    };

    function NoteOn() {
        this.channel = 1;
        this.pitch = 0;
        this.velocity = 0;
    }
    NoteOn.prototype.send = function () {
        sentEvents.push({ type: "noteon", channel: this.channel, pitch: this.pitch, velocity: this.velocity });
    };

    function NoteOff() {
        this.channel = 1;
        this.pitch = 0;
        this.velocity = 0;
    }
    NoteOff.prototype.send = function () {
        sentEvents.push({ type: "noteoff", channel: this.channel, pitch: this.pitch, velocity: this.velocity });
    };

    function ChannelPressure() {
        this.channel = 1;
        this.value = 0;
    }
    ChannelPressure.prototype.send = function () {
        sentEvents.push({ type: "pressure", channel: this.channel, value: this.value });
    };

    function PitchBend() {
        this.channel = 1;
        this.value = 0;
    }
    PitchBend.prototype.send = function () {
        sentEvents.push({ type: "bend", channel: this.channel, value: this.value });
    };

    return {
        ControlChange,
        NoteOn,
        NoteOff,
        ChannelPressure,
        PitchBend,
        GetTimingInfo: () => ({ playing: false }),
        Math,
        Object
    };
}

const sandbox = makeSandbox();
vm.createContext(sandbox);
vm.runInContext(code, sandbox);

function hasCC(ch, cc, val) {
    return sentEvents.some((e) => e.type === "cc" && e.channel === ch && e.number === cc && e.value === val);
}

/* 1. Changing Active Mixer Bank (PARAM_MIXER_BANK = 13) -> updates track offset & LED color immediately. */
sentEvents.length = 0;
sandbox.ParameterChanged(13, 2); /* Group 3 (Tracks 11-15) -> offset 10, Amber color code 2 (ccVal 63) */
assert.ok(hasCC(1, 119, 2), "Expected Mixer page activation CC 119 = 2");
assert.ok(hasCC(1, 113, 2), "Expected Mixer Bank CC 113 = 2");
assert.ok(hasCC(1, 106, 10), "Expected Track Offset CC 106 = 10");
assert.ok(hasCC(1, 112, 2), "Expected Fader Group LED Color CC 112 = 2 (Amber)");
assert.ok(hasCC(16, 32, 63) && hasCC(16, 36, 63), "Expected per-fader color hint CCs 32..36");

/* 2. MCU Bank Right Note (0x2F) in HandleMIDI steps +8 tracks and updates fader group color. */
sentEvents.length = 0;
const mcuBankRight = new sandbox.NoteOn();
mcuBankRight.channel = 1;
mcuBankRight.pitch = 0x2F;
mcuBankRight.velocity = 127;
sandbox.HandleMIDI(mcuBankRight);
assert.ok(sentEvents.some((e) => e.type === "noteon" && e.pitch === 0x2F), "MCU note must pass untouched");
assert.ok(hasCC(1, 106, 18), "Track offset must advance from 10 to 18");
assert.ok(hasCC(1, 113, 3), "Active mixer bank must advance to Group 4 (18/5 = 3)");
assert.ok(hasCC(1, 112, 3), "Expected Gold LED color code 3 for Group 4");

/* 3. Fader Group LED Color override (PARAM_FADER_COLOR_MODE = 14) -> Magenta (6 -> code 5, ccVal 127). */
sentEvents.length = 0;
sandbox.ParameterChanged(14, 6);
assert.ok(hasCC(1, 112, 5), "Expected Magenta color override code 5");
assert.ok(hasCC(16, 32, 127), "Expected Magenta ccVal 127 on per-fader CC 32");

/* 4. MPE passthrough + expression split still functions cleanly. */
sentEvents.length = 0;
const mpeNote = new sandbox.NoteOn();
mpeNote.channel = 2;
mpeNote.pitch = 60;
mpeNote.velocity = 110;
sandbox.HandleMIDI(mpeNote);
assert.ok(sentEvents.some((e) => e.type === "noteon" && e.channel === 2 && e.pitch === 60 && e.velocity === 110));
assert.ok(hasCC(16, 20, 110), "Focused Strike CC 20 must emit velocity");
assert.ok(hasCC(2, 14, 110), "Per-voice Strike CC 14 must emit velocity");

/* 5. MIDILogger System Console toggle (PARAM 15 -> CC 85) & Filter (PARAM 16 -> CC 86). */
sentEvents.length = 0;
sandbox.ParameterChanged(16, 1); /* MPE + Expression Only */
assert.ok(hasCC(1, 86, 1), "Expected CC 86 = 1 (MPE filter)");
sandbox.ParameterChanged(15, 1); /* Enable MIDILogger console */
assert.ok(hasCC(1, 85, 127), "Expected CC 85 = 127 when MIDILogger enabled");
sandbox.ParameterChanged(15, 0); /* Disable MIDILogger console */
assert.ok(hasCC(1, 85, 0), "Expected CC 85 = 0 when MIDILogger disabled");

const paramUpdates = [];

function makeTrackedSandbox() {
    const sb = makeSandbox();
    sb.SetParameter = (param, val) => {
        paramUpdates.push({ param, val });
    };
    return sb;
}

const sb2 = makeTrackedSandbox();
vm.createContext(sb2);
vm.runInContext(code, sb2);

/* 6. Bank Previous/Next stepping (PARAM 17/18) + SetParameter UI sync + clamping. */
sentEvents.length = 0;
paramUpdates.length = 0;
sb2.ParameterChanged(19, 1); /* PARAM_TRACK_COLOR_MODE = Logic Track Colors (Per-Channel) */
/* Bank 1 default palette: [2 (63), 4 (106), 0 (21), 5 (127), 1 (42)] */
assert.ok(hasCC(16, 32, 63), "Bank 1 Track 1 Amber (63)");
assert.ok(hasCC(16, 33, 106), "Bank 1 Track 2 Coral (106)");
assert.ok(hasCC(16, 34, 21), "Bank 1 Track 3 Periwinkle (21)");
assert.ok(hasCC(16, 35, 127), "Bank 1 Track 4 Magenta (127)");
assert.ok(hasCC(16, 36, 42), "Bank 1 Track 5 Teal (42)");

sentEvents.length = 0;
paramUpdates.length = 0;
sb2.ParameterChanged(18, 1); /* PARAM_BANK_NEXT -> Bank 2 (offset 5) */
assert.ok(hasCC(1, 113, 1), "Expected Mixer Bank CC 113 = 1 (Bank 2)");
assert.ok(hasCC(1, 106, 5), "Expected Track Offset CC 106 = 5");
/* Bank 2 default palette: [3 (85), 1 (42), 0 (21), 2 (63), 5 (127)] */
assert.ok(hasCC(16, 32, 85), "Bank 2 Track 6 Gold (85)");
assert.ok(hasCC(16, 33, 42), "Bank 2 Track 7 Teal (42)");
assert.ok(paramUpdates.some((u) => u.param === 13 && u.val === 1), "SetParameter synced PARAM_MIXER_BANK = 1");

/* Assign custom Logic track color to Bank 2 Ch 1 (Track 6 -> Coral = index 5 -> code 4 -> ccVal 106). */
sentEvents.length = 0;
sb2.ParameterChanged(20, 5);
assert.ok(hasCC(16, 32, 106), "Bank 2 Track 6 custom Coral override emits ccVal 106");

/* Step back to Bank 1 via PARAM_BANK_PREV (17), verify Bank 1 Track 1 stays Amber (63). */
sentEvents.length = 0;
sb2.ParameterChanged(17, 1);
assert.ok(hasCC(1, 113, 0), "Expected step back to Bank 1 (CC 113 = 0)");
assert.ok(hasCC(1, 106, 0), "Expected Track Offset CC 106 = 0");
assert.ok(hasCC(16, 32, 63), "Bank 1 Track 1 remains Amber (63)");

/* Step forward to Bank 2 again -> Track 6 custom Coral (106) persists and SetParameter recalls menu value 5. */
sentEvents.length = 0;
paramUpdates.length = 0;
sb2.ParameterChanged(18, 1);
assert.ok(hasCC(16, 32, 106), "Bank 2 Track 6 custom Coral persists across bank switch");
assert.ok(paramUpdates.some((u) => u.param === 20 && u.val === 5), "PARAM_BANK_CH_COLOR_BASE recalls Track 6 override");

/* Incoming Ch 1 CC 114 step (-5 tracks) + Ch 16 CC 33 track color hint from Logic Pro. */
sentEvents.length = 0;
const bankStepDown = new sb2.ControlChange();
bankStepDown.channel = 1;
bankStepDown.number = 114;
bankStepDown.value = 0;
sb2.HandleMIDI(bankStepDown);
assert.ok(hasCC(1, 106, 0), "CC 114 value < 64 steps back to offset 0");

sentEvents.length = 0;
paramUpdates.length = 0;
const track2ColorHint = new sb2.ControlChange();
track2ColorHint.channel = 16;
track2ColorHint.number = 33;
track2ColorHint.value = 85; /* Gold -> code 3 -> menu 4 */
sb2.HandleMIDI(track2ColorHint);
assert.ok(hasCC(16, 33, 85), "Incoming Ch 16 CC 33 updates Track 2 LED color to Gold (85)");
assert.ok(paramUpdates.some((u) => u.param === 21 && u.val === 4), "PARAM_BANK_CH_COLOR_BASE+1 synced to Gold (4)");

console.log("All SENTIA TILES TILES_Controller mixer bank sync tests passed.");
