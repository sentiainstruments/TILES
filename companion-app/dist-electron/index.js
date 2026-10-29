import { BrowserWindow, app, ipcMain } from "electron";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { usb } from "usb";
//#region src/main/usb.ts
var activeDevice = null;
var controlInterface = null;
var controlIn = null;
var controlOut = null;
var receiveBuffer = "";
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
	} catch {}
	activeDevice = null;
	controlInterface = controlIn = controlOut = null;
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
async function sendTilesCommand(command, multiLine = false) {
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
//#endregion
//#region src/main/index.ts
var __dirname = dirname(fileURLToPath(import.meta.url));
var mainWindow = null;
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
	ipcMain.handle("usb:list", async () => {
		return listUsbDevices();
	});
	ipcMain.handle("usb:connect", async (_event, vendorId, productId) => {
		return connectUsbDevice(vendorId, productId);
	});
	ipcMain.handle("usb:disconnect", async () => {
		return disconnectUsbDevice();
	});
	usb.addEventListener("connect", () => {
		sendUsbList();
	});
	usb.addEventListener("disconnect", () => {
		sendUsbList();
	});
	ipcMain.handle("usb:command", async (_event, command, multiLine = false) => {
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
