import { BrowserWindow, app, dialog, ipcMain } from "electron";
import { basename, dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { usb } from "usb";
import { readFile, rename, writeFile } from "node:fs/promises";
import { randomUUID } from "node:crypto";
//#region src/main/usb.ts
var activeDevice = null;
var controlInterface = null;
var controlIn = null;
var controlOut = null;
var receiveBuffer = "";
var commandQueue = Promise.resolve();
async function listUsbDevices() {
	return (await usb.getDevices()).map((device) => ({
		vendorId: device.vendorId,
		productId: device.productId,
		productName: device.productName ?? void 0,
		manufacturerName: device.manufacturerName ?? void 0,
		serialNumber: device.serialNumber ?? void 0
	}));
}
async function connectUsbDevice(vendorId, productId) {
	await disconnectUsbDevice();
	const device = await usb.findDeviceByIds(vendorId, productId);
	if (!device) throw new Error(`USB device ${hex(vendorId)}:${hex(productId)} not found`);
	await device.open();
	const iface = device.configuration?.interfaces.find((i) => i.alternate.interfaceClass === 255 && i.alternate.interfaceName === "SENTIA TILES Control");
	if (!iface) {
		await device.close();
		throw new Error("SENTIA TILES Control interface not found");
	}
	const out = iface.alternate.endpoints.find((e) => e.direction === "out");
	const input = iface.alternate.endpoints.find((e) => e.direction === "in");
	if (!out || !input) {
		await device.close();
		throw new Error("TILES control endpoints not found");
	}
	await device.claimInterface(iface.interfaceNumber);
	activeDevice = device;
	controlInterface = iface.interfaceNumber;
	controlOut = out.endpointNumber;
	controlIn = input.endpointNumber;
	receiveBuffer = "";
	console.log("TILES ready:", {
		interface: controlInterface,
		outEndpoint: controlOut,
		inEndpoint: controlIn
	});
	return {
		connected: true,
		vendorId: device.vendorId,
		productId: device.productId,
		productName: device.productName,
		manufacturerName: device.manufacturerName
	};
}
async function disconnectUsbDevice() {
	if (activeDevice) try {
		if (controlInterface !== null) await activeDevice.releaseInterface(controlInterface);
		await activeDevice.close();
	} catch (error) {
		console.warn("USB disconnect error:", error);
	}
	activeDevice = null;
	controlInterface = null;
	controlIn = null;
	controlOut = null;
	receiveBuffer = "";
	return { connected: false };
}
async function readLine() {
	if (!activeDevice || controlIn === null) throw new Error("TILES is not connected");
	while (!receiveBuffer.includes("\n")) {
		const result = await activeDevice.transferIn(controlIn, 64);
		if (result.status !== "ok" || !result.data) throw new Error(`USB read failed: ${result.status}`);
		const data = new Uint8Array(result.data.buffer, result.data.byteOffset, result.data.byteLength);
		receiveBuffer += new TextDecoder().decode(data);
	}
	const index = receiveBuffer.indexOf("\n");
	const line = receiveBuffer.slice(0, index).replace(/\r$/, "");
	receiveBuffer = receiveBuffer.slice(index + 1);
	return line;
}
/**
* Public command function.
*
* Serializes all USB commands so multiple React components
* cannot send overlapping requests to the instrument.
*
* Each command waits for the previous command to finish.
*/
function sendTilesCommand(command, multiLine = false) {
	const result = commandQueue.then(() => sendTilesCommandNow(command, multiLine));
	commandQueue = result.then(() => void 0, () => void 0);
	return result;
}
/**
* Performs one complete USB request/response transaction.
*
* Called only from the command queue.
*/
async function sendTilesCommandNow(command, multiLine = false) {
	if (!activeDevice || controlOut === null) throw new Error("TILES is not connected");
	console.log(`→ TILES: ${command}`);
	const write = await activeDevice.transferOut(controlOut, new TextEncoder().encode(`${command}\n`));
	if (write.status !== "ok") throw new Error(`USB write failed: ${write.status}`);
	const lines = [];
	while (true) {
		const line = await readLine();
		console.log(`← TILES: ${line}`);
		if (multiLine && line === "OK") return lines;
		lines.push(line);
		if (!multiLine || line.startsWith("ERR ")) return lines;
	}
}
function hex(value) {
	return `0x${value.toString(16).padStart(4, "0")}`;
}
var tiles_settings_default = {
	_comment: "Mock catalog: base v0.3.6 plus 21 alpha.6.5 settings from sentiainstruments/TILES firmware/board-bringup at a52f515fffa0c025c4d7900096920cbb309bc8ea. Not measured against hardware. In-memory simulator; old versions remain selectable.",
	firmware: "alpha.6.5",
	settings: [
		{
			"id": 256,
			"key": "pedal.mode",
			"type": "enum",
			"values": ["sustain", "expression"],
			"default": "sustain",
			"value": "sustain",
			"persist": true
		},
		{
			"id": 257,
			"key": "pedal.polarity",
			"type": "enum",
			"values": ["normally_open", "normally_closed"],
			"default": "normally_open",
			"value": "normally_open",
			"persist": true
		},
		{
			"id": 258,
			"key": "pedal.sustain_style",
			"type": "enum",
			"values": ["synth", "hold"],
			"default": "synth",
			"value": "synth",
			"persist": true
		},
		{
			"id": 512,
			"key": "expression.mpe_enabled",
			"type": "bool",
			"default": true,
			"value": true,
			"persist": true
		},
		{
			"id": 513,
			"key": "expression.pitch_bend_sensitivity",
			"type": "float",
			"min": .001,
			"max": 1,
			"default": .065,
			"value": .065,
			"persist": true
		},
		{
			"id": 514,
			"key": "expression.aftertouch_sensitivity",
			"type": "uint",
			"min": 1,
			"max": 65535,
			"default": 900,
			"value": 900,
			"persist": true
		},
		{
			"id": 768,
			"key": "cv_gate.enabled",
			"type": "bool",
			"default": false,
			"value": false,
			"persist": false
		},
		{
			"id": 769,
			"key": "cv_gate.pitch.volts_per_semitone",
			"type": "float",
			"min": .001,
			"max": 1,
			"default": .083333,
			"value": .083333,
			"persist": true
		},
		{
			"id": 770,
			"key": "cv_gate.pitch.reference_note",
			"type": "int",
			"min": 0,
			"max": 127,
			"default": 0,
			"value": 0,
			"persist": true
		},
		{
			"id": 771,
			"key": "cv_gate.pitch.zero_trim_volts",
			"type": "float",
			"min": -2.5,
			"max": 2.5,
			"default": 0,
			"value": 0,
			"persist": true
		},
		{
			"id": 772,
			"key": "cv_gate.pitch.gain_trim",
			"type": "float",
			"min": .5,
			"max": 2,
			"default": 1,
			"value": 1,
			"persist": true
		},
		{
			"id": 773,
			"key": "cv_gate.pressure.full_scale_volts",
			"type": "float",
			"min": .1,
			"max": 10,
			"default": 10,
			"value": 10,
			"persist": true
		},
		{
			"id": 774,
			"key": "cv_gate.pressure.zero_trim_volts",
			"type": "float",
			"min": -2.5,
			"max": 2.5,
			"default": 0,
			"value": 0,
			"persist": true
		},
		{
			"id": 775,
			"key": "cv_gate.pressure.gain_trim",
			"type": "float",
			"min": .5,
			"max": 2,
			"default": 1,
			"value": 1,
			"persist": true
		},
		{
			"id": 1024,
			"key": "look.idle_baseline_percent",
			"type": "uint",
			"min": 0,
			"max": 100,
			"default": 50,
			"value": 50,
			"persist": true
		},
		{
			"id": 1029,
			"key": "look.echo_sustain_tint_percent",
			"type": "uint",
			"min": 0,
			"max": 100,
			"default": 35,
			"value": 35,
			"persist": true
		},
		{
			"id": 1030,
			"key": "look.echo_secondary_g_percent",
			"type": "uint",
			"min": 0,
			"max": 100,
			"default": 18,
			"value": 18,
			"persist": true
		},
		{
			"id": 1031,
			"key": "look.echo_secondary_b_percent",
			"type": "uint",
			"min": 0,
			"max": 100,
			"default": 12,
			"value": 12,
			"persist": true
		},
		{
			"id": 1032,
			"key": "look.echo_flash_ms",
			"type": "uint",
			"min": 0,
			"max": 2e3,
			"default": 180,
			"value": 180,
			"persist": true
		},
		{
			"id": 1280,
			"key": "midi.din_trs_type",
			"type": "enum",
			"values": ["a", "b"],
			"default": "a",
			"value": "a",
			"persist": true
		},
		{
			"id": 1536,
			"key": "features.melodic_harmonics",
			"type": "bool",
			"default": true,
			"value": true,
			"persist": true
		},
		{
			"id": 1537,
			"key": "features.harmonics.arm_ms",
			"type": "uint",
			"min": 0,
			"max": 1e3,
			"default": 150,
			"value": 150,
			"persist": true
		},
		{
			"id": 1538,
			"key": "features.harmonics.confirm_ms",
			"type": "uint",
			"min": 0,
			"max": 500,
			"default": 40,
			"value": 40,
			"persist": true
		},
		{
			"id": 1539,
			"key": "features.harmonics.press_depth",
			"type": "uint",
			"min": 1,
			"max": 1e3,
			"default": 128,
			"value": 128,
			"persist": true
		},
		{
			"id": 1792,
			"key": "color.root",
			"type": "color",
			"default": "660066",
			"value": "660066",
			"persist": true
		},
		{
			"id": 1793,
			"key": "color.third",
			"type": "color",
			"default": "none",
			"value": "none",
			"persist": true
		},
		{
			"id": 1794,
			"key": "color.fifth",
			"type": "color",
			"default": "240066",
			"value": "240066",
			"persist": true
		},
		{
			"id": 1795,
			"key": "color.note",
			"type": "color",
			"default": "363636",
			"value": "363636",
			"persist": true
		},
		{
			"id": 1796,
			"key": "color.accidental",
			"type": "color",
			"default": "000000",
			"value": "000000",
			"persist": true
		},
		{
			"id": 1797,
			"key": "color.custom_pads",
			"type": "bool",
			"default": false,
			"value": false,
			"persist": true
		},
		{
			"id": 1808,
			"key": "color.pad.01",
			"type": "color",
			"default": "none",
			"value": "none",
			"persist": true
		},
		{
			"id": 1809,
			"key": "color.pad.02",
			"type": "color",
			"default": "none",
			"value": "none",
			"persist": true
		},
		{
			"id": 1810,
			"key": "color.pad.03",
			"type": "color",
			"default": "none",
			"value": "none",
			"persist": true
		},
		{
			"id": 1811,
			"key": "color.pad.04",
			"type": "color",
			"default": "none",
			"value": "none",
			"persist": true
		},
		{
			"id": 1812,
			"key": "color.pad.05",
			"type": "color",
			"default": "none",
			"value": "none",
			"persist": true
		},
		{
			"id": 1813,
			"key": "color.pad.06",
			"type": "color",
			"default": "none",
			"value": "none",
			"persist": true
		},
		{
			"id": 1814,
			"key": "color.pad.07",
			"type": "color",
			"default": "none",
			"value": "none",
			"persist": true
		},
		{
			"id": 1815,
			"key": "color.pad.08",
			"type": "color",
			"default": "none",
			"value": "none",
			"persist": true
		},
		{
			"id": 1816,
			"key": "color.pad.09",
			"type": "color",
			"default": "none",
			"value": "none",
			"persist": true
		},
		{
			"id": 1817,
			"key": "color.pad.10",
			"type": "color",
			"default": "none",
			"value": "none",
			"persist": true
		},
		{
			"id": 1818,
			"key": "color.pad.11",
			"type": "color",
			"default": "none",
			"value": "none",
			"persist": true
		},
		{
			"id": 1819,
			"key": "color.pad.12",
			"type": "color",
			"default": "none",
			"value": "none",
			"persist": true
		},
		{
			"id": 1820,
			"key": "color.pad.13",
			"type": "color",
			"default": "none",
			"value": "none",
			"persist": true
		},
		{
			"id": 1821,
			"key": "color.pad.14",
			"type": "color",
			"default": "none",
			"value": "none",
			"persist": true
		},
		{
			"id": 1822,
			"key": "color.pad.15",
			"type": "color",
			"default": "none",
			"value": "none",
			"persist": true
		},
		{
			"id": 1823,
			"key": "color.pad.16",
			"type": "color",
			"default": "none",
			"value": "none",
			"persist": true
		},
		{
			"id": 1824,
			"key": "color.pad.17",
			"type": "color",
			"default": "none",
			"value": "none",
			"persist": true
		},
		{
			"id": 1825,
			"key": "color.pad.18",
			"type": "color",
			"default": "none",
			"value": "none",
			"persist": true
		},
		{
			"id": 1826,
			"key": "color.pad.19",
			"type": "color",
			"default": "none",
			"value": "none",
			"persist": true
		},
		{
			"id": 1827,
			"key": "color.pad.20",
			"type": "color",
			"default": "none",
			"value": "none",
			"persist": true
		},
		{
			"id": 1828,
			"key": "color.pad.21",
			"type": "color",
			"default": "none",
			"value": "none",
			"persist": true
		},
		{
			"id": 1829,
			"key": "color.pad.22",
			"type": "color",
			"default": "none",
			"value": "none",
			"persist": true
		},
		{
			"id": 1830,
			"key": "color.pad.23",
			"type": "color",
			"default": "none",
			"value": "none",
			"persist": true
		},
		{
			"id": 1831,
			"key": "color.pad.24",
			"type": "color",
			"default": "none",
			"value": "none",
			"persist": true
		}
	],
	legacyFirmware: "0.2.0",
	legacySettings: [
		{
			"id": 1025,
			"key": "look.natural_pad_percent",
			"type": "uint",
			"min": 0,
			"max": 100,
			"default": 21,
			"value": 21,
			"persist": true
		},
		{
			"id": 1026,
			"key": "look.root_pad_percent",
			"type": "uint",
			"min": 0,
			"max": 100,
			"default": 40,
			"value": 40,
			"persist": true
		},
		{
			"id": 1027,
			"key": "look.fifth_pad_percent",
			"type": "uint",
			"min": 0,
			"max": 100,
			"default": 40,
			"value": 40,
			"persist": true
		},
		{
			"id": 1028,
			"key": "look.fifth_red_tint_percent",
			"type": "uint",
			"min": 0,
			"max": 100,
			"default": 35,
			"value": 35,
			"persist": true
		}
	],
	alphaSettings: [
		{
			"id": 1033,
			"key": "look.pressure_hue_degrees",
			"type": "uint",
			"default": 40,
			"value": 40,
			"persist": true,
			"min": 0,
			"max": 90
		},
		{
			"id": 1034,
			"key": "look.bend_dim_percent",
			"type": "uint",
			"default": 50,
			"value": 50,
			"persist": true,
			"min": 0,
			"max": 90
		},
		{
			"id": 1035,
			"key": "look.led_intensity_percent",
			"type": "uint",
			"default": 100,
			"value": 100,
			"persist": true,
			"min": 0,
			"max": 100
		},
		{
			"id": 1036,
			"key": "look.play_effect",
			"type": "enum",
			"default": "off",
			"value": "off",
			"persist": true,
			"values": [
				"off",
				"afterglow",
				"ripple",
				"cross",
				"halo",
				"warmth",
				"prism_ripple",
				"touch_colour",
				"heatmap",
				"fireworks",
				"rise",
				"ink"
			]
		},
		{
			"id": 1281,
			"key": "midi.clock_lead_ms",
			"type": "uint",
			"default": 15,
			"value": 15,
			"persist": true,
			"min": 0,
			"max": 50
		},
		{
			"id": 1282,
			"key": "midi.parts_port",
			"type": "enum",
			"default": "parts",
			"value": "parts",
			"persist": true,
			"values": ["parts", "main"]
		},
		{
			"id": 2048,
			"key": "modes.duet",
			"type": "bool",
			"default": true,
			"value": true,
			"persist": true
		},
		{
			"id": 2049,
			"key": "modes.sequencer",
			"type": "bool",
			"default": true,
			"value": true,
			"persist": true
		},
		{
			"id": 2050,
			"key": "modes.drums",
			"type": "bool",
			"default": true,
			"value": true,
			"persist": true
		},
		{
			"id": 2051,
			"key": "modes.song",
			"type": "bool",
			"default": true,
			"value": true,
			"persist": true
		},
		{
			"id": 2052,
			"key": "modes.prism",
			"type": "bool",
			"default": true,
			"value": true,
			"persist": true
		},
		{
			"id": 2053,
			"key": "modes.bass",
			"type": "bool",
			"default": true,
			"value": true,
			"persist": true
		},
		{
			"id": 2054,
			"key": "modes.ableton",
			"type": "bool",
			"default": true,
			"value": true,
			"persist": true
		},
		{
			"id": 2055,
			"key": "modes.volca",
			"type": "bool",
			"default": true,
			"value": true,
			"persist": true
		},
		{
			"id": 2056,
			"key": "modes.bounce",
			"type": "bool",
			"default": true,
			"value": true,
			"persist": true
		},
		{
			"id": 2057,
			"key": "modes.cradle",
			"type": "bool",
			"default": true,
			"value": true,
			"persist": true
		},
		{
			"id": 2058,
			"key": "modes.sand",
			"type": "bool",
			"default": true,
			"value": true,
			"persist": true
		},
		{
			"id": 2059,
			"key": "modes.ripples",
			"type": "bool",
			"default": true,
			"value": true,
			"persist": true
		},
		{
			"id": 2060,
			"key": "modes.orbits",
			"type": "bool",
			"default": true,
			"value": true,
			"persist": true
		},
		{
			"id": 2304,
			"key": "layout.melodic",
			"type": "enum",
			"default": "rows",
			"value": "rows",
			"persist": true,
			"values": [
				"rows",
				"columns",
				"fourths",
				"keys",
				"fifths",
				"tonnetz",
				"quarter_tones"
			]
		},
		{
			"id": 2560,
			"key": "seq.lock",
			"type": "enum",
			"default": "off",
			"value": "off",
			"persist": true,
			"values": ["off", "bar"]
		}
	]
};
//#endregion
//#region src/main/mockUsb.ts
var mockDevice = {
	vendorId: 4617,
	productId: 1,
	productName: "SENTIA TILES (Simulator)",
	manufacturerName: "Sentia Instruments"
};
var VERSIONS = [
	"0.2.0",
	"0.3.6",
	"alpha.6.5"
];
var firmware = "alpha.6.5";
var settings = /* @__PURE__ */ new Map();
var scales = /* @__PURE__ */ new Map();
var toWire = (value) => typeof value === "boolean" ? value ? "1" : "0" : String(value);
function loadVersion(next) {
	firmware = next;
	const list = next === "alpha.6.5" ? [...tiles_settings_default.settings, ...tiles_settings_default.alphaSettings] : next === "0.3.6" ? tiles_settings_default.settings : [...tiles_settings_default.settings.filter((s) => !s.key.startsWith("color.")), ...tiles_settings_default.legacySettings];
	settings = new Map(list.map((s) => [s.key, {
		...s,
		value: toWire(s.default)
	}]));
	scales.clear();
}
loadVersion(firmware);
function schemaLine(setting) {
	const parts = [
		`id=${setting.id}`,
		`key=${setting.key}`,
		`type=${setting.type}`
	];
	if (setting.values) parts.push(`values=${setting.values.join("|")}`);
	if (setting.min !== void 0) parts.push(`min=${setting.min}`);
	if (setting.max !== void 0) parts.push(`max=${setting.max}`);
	parts.push(`default=${toWire(setting.default)}`);
	if (!setting.persist) parts.push("persist=0");
	return parts.join(" ");
}
function validate(setting, value) {
	if (setting.type === "bool") return ["0", "1"].includes(value) ? null : "ERR bad-value";
	if (setting.type === "enum") return setting.values?.includes(value) ? null : "ERR out-of-range";
	if (setting.type === "color") return /^(?:[0-9a-fA-F]{6}|none)$/.test(value) ? null : "ERR bad-value";
	if ([
		"int",
		"uint",
		"float"
	].includes(setting.type)) {
		if (!value || !/^[+-]?(?:\d+\.?\d*|\.\d+)$/.test(value)) return "ERR bad-value";
		const num = Number(value);
		if (!Number.isFinite(num) || setting.type !== "float" && !Number.isInteger(num)) return "ERR bad-value";
		if (setting.type === "uint" && num < 0) return "ERR out-of-range";
		if (setting.min !== void 0 && num < setting.min || setting.max !== void 0 && num > setting.max) return "ERR out-of-range";
		return null;
	}
	return "ERR bad-value";
}
var validToken = (s, allowEmpty = false) => allowEmpty && s.length === 0 || s.length > 0 && s.length <= 16 && /^[!-~]+$/.test(s);
function parseSlot(raw) {
	if (!/^[1-9]$/.test(raw ?? "")) return null;
	return Number(raw);
}
function scaleLine(scale) {
	let value = `${scale.slot} ${scale.name} ${scale.intervals.join(",")}`;
	if (scale.pack) value += ` pack=${scale.pack}`;
	if (scale.item) value += ` item=${scale.item}`;
	return `${value} version=${scale.version}`;
}
function putScale(parts) {
	const slot = parseSlot(parts[0]);
	if (slot === null) return "ERR bad-slot";
	const name = parts[1] ?? "";
	if (!validToken(name)) return "ERR bad-name";
	const rawIntervals = parts[2] ?? "";
	if (!/^\d+(,\d+)*$/.test(rawIntervals)) return "ERR bad-intervals";
	const intervals = rawIntervals.split(",").map(Number);
	if (!intervals.length || intervals.length > 12 || intervals[0] !== 0 || intervals.some((n, i) => n > 11 || i > 0 && n <= intervals[i - 1])) return "ERR bad-intervals";
	const fields = {};
	for (const field of parts.slice(3)) {
		const index = field.indexOf("=");
		const key = field.slice(0, index);
		if (index < 1 || ![
			"pack",
			"item",
			"version"
		].includes(key) || key in fields) return "ERR bad-field";
		fields[key] = field.slice(index + 1);
	}
	const pack = fields.pack ?? "";
	const item = fields.item ?? "";
	const version = fields.version === void 0 ? 0 : Number(fields.version);
	if (!validToken(pack, true) || !validToken(item, true)) return "ERR bad-name";
	if (fields.version !== void 0 && (!/^\d+$/.test(fields.version) || !Number.isInteger(version) || version > 65535)) return "ERR bad-field";
	scales.set(slot, {
		slot,
		name,
		intervals,
		pack,
		item,
		version
	});
	return "OK";
}
function handleScale(parts) {
	const action = parts[0];
	const slot = parseSlot(parts[1]);
	if (![
		"GET",
		"PUT",
		"DELETE"
	].includes(action ?? "")) return ["ERR unsupported-command"];
	if (slot === null) return ["ERR bad-slot"];
	if (action === "PUT") return [putScale(parts.slice(1))];
	if (action === "DELETE") {
		scales.delete(slot);
		return ["OK"];
	}
	const value = scales.get(slot);
	return value ? [scaleLine(value)] : ["ERR empty"];
}
/**
* An in-memory, firmware-versioned protocol simulator.
* Multiline responses omit the terminal OK because the real USB adapter
* consumes it before returning lines to the renderer.
*/
async function mockCommand(command, _multiLine = false) {
	const [action, key, ...rest] = command.trim().split(/\s+/);
	console.log("[SIM]", command);
	if (action === "SIM" && key === "VERSION") {
		if (!VERSIONS.includes(rest[0])) return ["ERR bad-value"];
		loadVersion(rest[0]);
		return ["OK"];
	}
	if (action === "INFO") return [
		"simulator=1",
		"unit=2",
		`firmware=${firmware}`,
		`settings=${settings.size}`,
		"power=usb_only",
		"power.led_ceiling_percent=50",
		"store.loaded=1",
		"store.pending=0",
		`content.storage=${firmware !== "0.2.0" ? "ok" : "none"}`,
		`content.scales=${scales.size}/9`,
		"drums.storage=mock"
	];
	if (action === "LIST") return [...settings.values()].map((s) => `${s.key}=${s.value}`);
	if (action === "SCHEMA") return [...settings.values()].map(schemaLine);
	if (action === "GET") {
		const setting = settings.get(key);
		return setting ? [setting.value] : ["ERR unknown-key"];
	}
	if (action === "SET") {
		const setting = settings.get(key);
		if (!setting) return ["ERR unknown-key"];
		const value = rest.join(" ");
		const error = validate(setting, value);
		if (error) return [error];
		setting.value = setting.type === "color" && value !== "none" ? value.toUpperCase() : value;
		return ["OK"];
	}
	if (action === "RESET") {
		if (key === "ALL") {
			for (const s of settings.values()) s.value = toWire(s.default);
			return ["OK"];
		}
		const setting = settings.get(key);
		if (!setting) return ["ERR unknown-key"];
		setting.value = toWire(setting.default);
		return ["OK"];
	}
	if (action === "SAVE") return ["OK"];
	if (action === "REBOOT" && key === "APP") {
		for (const s of settings.values()) if (!s.persist) s.value = toWire(s.default);
		return ["OK"];
	}
	if (action === "REBOOT" && key === "BOOTSEL") return ["ERR unsupported-command"];
	if (firmware !== "0.2.0") {
		if (action === "SCALES") return [...scales.values()].sort((a, b) => a.slot - b.slot).map(scaleLine);
		if (action === "SCALE") return handleScale([key, ...rest]);
		if (action === "CONTENT" && key === "LIST") return [...scales.values()].sort((a, b) => a.slot - b.slot).map((s) => {
			const bytes = 6 + s.intervals.length + s.name.length + s.pack.length + s.item.length;
			return `scale ${s.slot} bytes=${bytes} version=${s.version}${s.pack ? ` pack=${s.pack}` : ""}${s.item ? ` item=${s.item}` : ""}`;
		});
		if (action === "CONTENT" && key === "CLEAR") {
			scales.clear();
			return ["OK"];
		}
		if (action === "TEST" && key === "OFF") return ["OK"];
		if (action === "TEST" && key === "MODE" && [
			"melodic",
			"duet",
			"sequencer",
			"bass",
			"song",
			"ableton",
			"drums",
			"volca",
			"prism",
			"bounce",
			"ripples",
			"sand",
			"orbits",
			"cradle"
		].includes(rest[0])) return ["OK"];
	}
	return ["ERR unsupported-command"];
}
var record = (v) => typeof v === "object" && v !== null && !Array.isArray(v);
var text = (v, max) => typeof v === "string" && v.length > 0 && v.length <= max;
var limitedText = (v, max) => typeof v === "string" && v.length <= max;
function isValidScale(value) {
	if (!record(value) || typeof value.slot !== "number" || !Number.isInteger(value.slot) || Number(value.slot) < 1 || Number(value.slot) > 9 || !text(value.name, 16) || !/^[!-~]+$/.test(value.name) || !Array.isArray(value.intervals) || value.intervals.length < 1 || value.intervals.length > 12) return false;
	const intervals = value.intervals;
	return intervals.every((n, i) => Number.isInteger(n) && n >= 0 && n < 12 && (i === 0 ? n === 0 : n > intervals[i - 1]));
}
function validateProject(value) {
	if (!record(value) || value.format !== "sentia.tiles-project" || value.version !== 1 || !text(value.id, 100) || !text(value.name, 100) || !text(value.createdAt, 40) || !text(value.updatedAt, 40) || !text(value.selectedPresetId, 100) || !Array.isArray(value.presets) || value.presets.length < 1 || value.presets.length > 128) throw new Error("Invalid or unsupported .tilesproject file (requires format version 1).");
	const ids = /* @__PURE__ */ new Set();
	for (const preset of value.presets) {
		if (!record(preset) || !text(preset.id, 100) || !text(preset.name, 100) || !limitedText(preset.description, 1e3) || !record(preset.settings) || !Array.isArray(preset.scales) || preset.scales.length > 9 || !(preset.previewScaleSlot === null || typeof preset.previewScaleSlot === "number" && Number.isInteger(preset.previewScaleSlot) && Number(preset.previewScaleSlot) >= 1 && Number(preset.previewScaleSlot) <= 9)) throw new Error("Project contains an invalid preset.");
		if (ids.has(preset.id)) throw new Error("Duplicate preset IDs.");
		ids.add(preset.id);
		const settings = Object.entries(preset.settings);
		if (settings.length > 256 || !settings.every(([key, val]) => /^[a-z][a-z0-9_.]{0,99}$/.test(key) && typeof val === "string" && val.length <= 100 && !/[\r\n]/.test(val))) throw new Error("Preset has invalid settings.");
		if (!preset.scales.every(isValidScale) || new Set(preset.scales.map((s) => s.slot)).size !== preset.scales.length) throw new Error("Preset contains an invalid custom scale.");
	}
	if (!ids.has(value.selectedPresetId)) throw new Error("Project-selected preset does not exist.");
	return value;
}
function serializeProject(project) {
	validateProject(project);
	return JSON.stringify(project, null, 2) + "\n";
}
//#endregion
//#region src/main/projectFiles.ts
var MAX_BYTES = 2097152;
var currentFile = null;
function registerProjectIpc(getWindow) {
	ipcMain.handle("project:open", async () => {
		const parent = getWindow();
		const options = {
			title: "Open TILES Project",
			properties: ["openFile"],
			filters: [{
				name: "TILES Project",
				extensions: ["tiles", "tilesproject"]
			}]
		};
		const choice = parent ? await dialog.showOpenDialog(parent, options) : await dialog.showOpenDialog(options);
		const path = choice.filePaths[0];
		if (choice.canceled || !path) return null;
		if (!/\.(tiles|tilesproject)$/i.test(path)) throw new Error("Choose a .tiles or .tilesproject file.");
		const raw = await readFile(path, "utf8");
		if (Buffer.byteLength(raw, "utf8") > MAX_BYTES) throw new Error("Project exceeds 2 MB.");
		const project = validateProject(JSON.parse(raw));
		currentFile = path;
		return {
			project,
			fileName: basename(path)
		};
	});
	ipcMain.handle("project:save", async (_event, value, saveAs) => {
		const project = validateProject(value);
		const raw = serializeProject(project);
		if (Buffer.byteLength(raw, "utf8") > MAX_BYTES) throw new Error("Project exceeds 2 MB.");
		let path = saveAs ? null : currentFile;
		if (!path) {
			const parent = getWindow();
			const options = {
				title: "Save TILES Project",
				defaultPath: `${project.name.replace(/[^a-zA-Z0-9 _-]/g, "").trim() || "Untitled"}.tiles`,
				filters: [{
					name: "TILES Project",
					extensions: ["tiles"]
				}]
			};
			const choice = parent ? await dialog.showSaveDialog(parent, options) : await dialog.showSaveDialog(options);
			if (choice.canceled || !choice.filePath) return null;
			path = choice.filePath.toLowerCase().endsWith(".tiles") ? choice.filePath : `${choice.filePath}.tiles`;
		}
		const temporary = join(dirname(path), `.${basename(path)}.${randomUUID()}.tmp`);
		try {
			await writeFile(temporary, raw, {
				encoding: "utf8",
				flag: "wx"
			});
			await rename(temporary, path);
		} catch (error) {
			const { unlink } = await import("node:fs/promises");
			await unlink(temporary).catch(() => void 0);
			throw error;
		}
		currentFile = path;
		return { fileName: basename(path) };
	});
	ipcMain.handle("project:new", () => {
		currentFile = null;
		return true;
	});
}
//#endregion
//#region src/main/index.ts
var __dirname = dirname(fileURLToPath(import.meta.url));
var mainWindow = null;
var isMock = process.env.TILES_MOCK === "1";
function createWindow() {
	mainWindow = new BrowserWindow({
		width: 1200,
		height: 800,
		webPreferences: {
			preload: join(__dirname, "preload.mjs"),
			contextIsolation: true,
			nodeIntegration: false
		}
	});
	if (process.env.VITE_DEV_SERVER_URL) mainWindow.loadURL(process.env.VITE_DEV_SERVER_URL);
	else mainWindow.loadFile("dist/index.html");
}
async function sendUsbList() {
	const devices = await listUsbDevices();
	mainWindow?.webContents.send("usb:changed", devices);
}
app.whenReady().then(() => {
	registerProjectIpc(() => mainWindow);
	ipcMain.handle("usb:list", async () => {
		if (isMock) return [mockDevice];
		return listUsbDevices();
	});
	ipcMain.handle("usb:connect", async (_event, vendorId, productId) => {
		if (isMock) return {
			connected: true,
			...mockDevice
		};
		return connectUsbDevice(vendorId, productId);
	});
	ipcMain.handle("usb:disconnect", async () => {
		if (isMock) return { connected: false };
		return disconnectUsbDevice();
	});
	usb.addEventListener("connect", () => {
		sendUsbList();
	});
	usb.addEventListener("disconnect", () => {
		sendUsbList();
	});
	ipcMain.handle("usb:command", async (_event, command, multiLine = false) => {
		if (isMock) return mockCommand(command, multiLine);
		return sendTilesCommand(command, multiLine);
	});
	createWindow();
});
app.on("window-all-closed", () => {
	if (process.platform !== "darwin") app.quit();
});
app.on("activate", () => {
	if (BrowserWindow.getAllWindows().length === 0) createWindow();
});
//#endregion
export {};
