let electron = require("electron");
//#region src/main/preload.ts
electron.contextBridge.exposeInMainWorld("tiles", { usb: {
	list: () => electron.ipcRenderer.invoke("usb:list"),
	connect: (vendorId, productId) => electron.ipcRenderer.invoke("usb:connect", vendorId, productId),
	disconnect: () => electron.ipcRenderer.invoke("usb:disconnect"),
	onChanged: (callback) => {
		const listener = (_event, devices) => {
			callback(devices);
		};
		electron.ipcRenderer.on("usb:changed", listener);
		return () => {
			electron.ipcRenderer.removeListener("usb:changed", listener);
		};
	},
	command: (command, multiLine = false) => electron.ipcRenderer.invoke("usb:command", command, multiLine)
} });
//#endregion
