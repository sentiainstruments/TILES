# src/main/

Electron main process. Owns: USB device discovery/connect over the vendor
interface, the protocol client (request/response + streaming subscriptions
per `../../../shared/protocol/`), local preset/profile file storage, and
firmware update orchestration. Exposes a narrow IPC surface to the
renderer — the renderer asks for data/actions, it never opens the USB
device itself.
