"use strict";

const assert = require("assert");
const path = require("path");

const mod = require(path.join(__dirname, "..", "scripter", "LogicScripterUI.js"));
const ui = mod.LogicScripterUI;

assert.ok(Array.isArray(mod.PluginParameters), "PluginParameters must exist");
assert.ok(mod.PluginParameters.length >= 6, "Expected Scripter parameters");

/* Verify HTML & CSS generation defaults */
ui.reset();
let html = ui.renderHTML();
assert.ok(html.includes("SENTIA TILES"), "Missing SENTIA TILES heading");
assert.ok(html.includes("Live Loops Bank Status"), "Missing Live Loops bank section");
assert.ok(html.includes("Current Mixer Page Mapping"), "Missing Mixer mapping section");
assert.ok(html.includes("Exo 2") && html.includes("Sora"), "Missing Sentia fixed fonts");
assert.ok(html.includes("#171d3a") && html.includes("#ff00ff"), "Missing Sentia palette colors");
assert.ok(html.includes("T1 – T5") && html.includes("Bank 1 (Ch 1–8)"), "Expected initial bank window");

/* Verify track bank stepping & cross-bank MCU alignment */
ui.stepBank(5);
assert.strictEqual(ui.getState().trackOffset, 5);
assert.strictEqual(ui.getState().mcuBank, 0);
ui.stepBank(5);
assert.strictEqual(ui.getState().trackOffset, 10);
assert.strictEqual(ui.getState().mcuBank, 1);

html = ui.renderHTML();
assert.ok(html.includes("T11 – T15"), "Expected updated track range after +5 steps");
assert.ok(html.includes("Bank 2 (Ch 9–16)"), "Expected updated MCU bank label");

/* Verify Live Loops MIDI state updates & Mixer page switch */
ui.applyMidiMessage(0xB0, 119, 2); /* CC 119 = Mixer */
assert.strictEqual(ui.getState().page, 2);

ui.applyMidiMessage(0xBF, 24, 1); /* Ch 16 CC 24 cell fire pad 1 */
assert.strictEqual(ui.getState().cells[0][0], 2);

ui.applyMidiMessage(0x90, 0x10, 127); /* MCU Mute slot 0 in active bank 1 (abs track 8) */
assert.strictEqual(ui.getState().strips[8].muted, true);

ui.applyMidiMessage(0x90, 0x56, 127); /* MCU Cycle on */
assert.strictEqual(ui.getState().cycleActive, true);

html = ui.renderHTML();
assert.ok(html.includes("is-playing"), "Expected playing cell state class in rendered HTML");
assert.ok(html.includes("Current Mixer Page Mapping (Mixer)"), "Expected active Mixer page header");

/* Verify 24-pad TILES hardware CSS grid & custom MIDI mapping */
assert.ok(html.includes("tiles-hw-grid-24"), "Expected 24-pad CSS grid class");
assert.ok(html.includes("TILES 24-Pad Hardware MIDI Mapper"), "Expected 24-pad mapper heading");
for (let p = 1; p <= 24; p++) {
    assert.ok(html.includes(`data-pad="${p}"`), `Expected pad ${p} in 24-pad grid`);
}

ui.selectPad(14);
assert.strictEqual(ui.getState().selectedPad, 14);
ui.setPadCustomMapping(14, 74, 15, "Tilt CC 22");
const m14 = ui.getEffectivePadMapping(14);
assert.strictEqual(m14.custom, true);
assert.strictEqual(m14.cc, 74);
assert.strictEqual(m14.channel, 15);
assert.strictEqual(m14.exprAxis, "Tilt CC 22");

/* Verify untouched Scripter HandleMIDI passthrough & Learn pulse emission */
let sentCount = 0;
let lastCcSent = null;
global.ControlChange = function () { this.channel = 1; this.number = 0; this.value = 0; };
global.ControlChange.prototype.send = function () {
    sentCount++;
    lastCcSent = { ch: this.channel, cc: this.number, val: this.value };
};
global.NoteOn = function () { this.channel = 1; this.pitch = 0; this.velocity = 0; };
global.NoteOn.prototype.send = function () { sentCount++; };
global.NoteOff = function () { this.channel = 1; this.pitch = 0; this.velocity = 0; };
global.NoteOff.prototype.send = function () { sentCount++; };

const ev = new global.NoteOn();
ev.channel = 1;
ev.pitch = 36;
ev.velocity = 127;
mod.HandleMIDI(ev);
assert.strictEqual(sentCount, 1, "HandleMIDI must forward incoming event untouched");
assert.strictEqual(ui.getState().cells[0][0], 2, "Grid note 36 velocity 127 sets cell (0,0) playing");

ui.emitLearnPulse(14);
assert.strictEqual(sentCount, 2, "emitLearnPulse must emit mapped ControlChange");
assert.deepStrictEqual(lastCcSent, { ch: 15, cc: 74, val: 127 });
assert.strictEqual(ui.getState().lastLearnPulsePad, 14);

ui.resetPadCustomMapping(14);
assert.strictEqual(ui.getEffectivePadMapping(14).custom, false);

console.log("All SENTIA TILES LogicScripterUI tests passed.");
