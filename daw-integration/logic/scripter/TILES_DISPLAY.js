/*
 * TILES DISPLAY for Apple Logic Pro (Scripter MIDI FX).
 *
 * Place before an instrument in a Software Instrument or Summing Track Stack.
 * All incoming MIDI and per-note MPE expression (member channels 2..16,
 * 14-bit PitchBend, ChannelPressure, CC 74 Slide, CC 64 Sustain) pass through
 * untouched via direct event.send().
 *
 * Route the parallel External Instrument leg in the Summing Stack to
 * "TILES Logic Display" (Channel All). The bridge routes Channel 1 to TILES
 * primary green LEDs and Channel 2 to secondary soft-red LEDs.
 */

var NeedsTimingInfo = true;

var PluginParameters = [
    {
        name: "VIEW (Light TILES Pads)",
        type: "checkbox",
        defaultValue: 0
    },
    {
        name: "Display Role",
        type: "menu",
        valueStrings: ["Primary (Green - Ch 1)", "Secondary (Soft Red - Ch 2)"],
        defaultValue: 0
    },
    {
        name: "Flush Active Notes",
        type: "momentary",
        defaultValue: 0
    }
];

var activeNotes = {};
var wasPlaying = false;
var viewArmed = false;
var roleChannel = 1;

function sendDisplayNote(isNoteOn, pitch, velocity, ch) {
    var msg = isNoteOn ? new NoteOn() : new NoteOff();
    msg.pitch = pitch;
    msg.velocity = isNoteOn ? velocity : 0;
    msg.channel = ch;
    msg.send();
}

function flushAllLitNotes() {
    for (var key in activeNotes) {
        if (Object.prototype.hasOwnProperty.call(activeNotes, key) && activeNotes[key] > 0) {
            var parts = key.split(":");
            var pitch = parseInt(parts[0], 10);
            var ch = parseInt(parts[1], 10);
            sendDisplayNote(false, pitch, 0, ch);
        }
    }
    activeNotes = {};

    var panic = new ControlChange();
    panic.number = 123;
    panic.value = 0;
    panic.channel = roleChannel;
    panic.send();
}

function ParameterChanged(param, value) {
    if (param === 0) {
        var nextArmed = value >= 0.5;
        if (viewArmed && !nextArmed) {
            flushAllLitNotes();
        }
        viewArmed = nextArmed;
    } else if (param === 1) {
        var nextCh = value >= 0.5 ? 2 : 1;
        if (nextCh !== roleChannel) {
            flushAllLitNotes();
            roleChannel = nextCh;
        }
    } else if (param === 2 && value >= 0.5) {
        flushAllLitNotes();
    }
}

function HandleMIDI(event) {
    /* Pass original event untouched first for zero-latency MPE fidelity. */
    event.send();

    if (!viewArmed) {
        return;
    }

    if (event instanceof NoteOn && event.velocity > 0) {
        var onKey = event.pitch + ":" + roleChannel;
        activeNotes[onKey] = (activeNotes[onKey] || 0) + 1;
        sendDisplayNote(true, event.pitch, event.velocity, roleChannel);
    } else if (event instanceof NoteOff || (event instanceof NoteOn && event.velocity === 0)) {
        var offKey = event.pitch + ":" + roleChannel;
        if (activeNotes[offKey] && activeNotes[offKey] > 0) {
            activeNotes[offKey] -= 1;
            if (activeNotes[offKey] <= 0) {
                delete activeNotes[offKey];
                sendDisplayNote(false, event.pitch, 0, roleChannel);
            }
        }
    }
}

function ProcessMIDI() {
    var info = GetTimingInfo();
    if (wasPlaying && !info.playing) {
        flushAllLitNotes();
    }
    wasPlaying = info.playing;
}

function Reset() {
    flushAllLitNotes();
}
