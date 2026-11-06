# Changelog - OneFamily

All notable changes to the OneFamily project are documented in this file.

## [v1.2.2-beta9] - Pre-release (2026-11-06)

### Bluetooth Controller Support & Pairing
- **DualSense Bluetooth Pairing on FW 11.00 and FW 9.00**: Added native wireless pairing and connection support for DualSense and DualSense Edge controllers over Bluetooth on PlayStation 4 Firmware 11.00 and Firmware 9.00.
- **Enhanced Bluetooth Pairing Engine**: Implemented verified kernel allowlist tables and hardened pattern matching across all supported firmware versions (5.05 - 11.00).

---

## [v1.2.2-beta8] - Pre-release (2026-11-06)

### Controller Compatibility & Hardware Fixes
- **Logitech RumblePad 2 & Legacy Gamepads Native Support (Issue #41)**: Added native support for Logitech RumblePad 2 (`0x046D:0xC218`), Logitech Dual Action (`0x046D:0xC216`), and Logitech F310/F710 DirectInput mode. Adapted endpoint packet sizing (`wMaxPacketSize = 8`) and polling intervals (`bInterval`), implemented hardware quirk protection preventing microcontroller freezes, and aligned force feedback to the `hid-lg2ff` rumble protocol.
- **PSVR Coexistence & Sony USB Bus Scan Optimization (Issue #52)**: Added hardware filtering for non-gamepad Sony USB devices (PSVR headset / processor unit `0x054C:0x09AF`, PlayStation Cameras, and USB audio devices) preventing false Generic HID detection. Fixed bus scan abort logic, allowing PS3 DualShock 3 and other gamepads to connect simultaneously with PSVR hardware plugged in.
- **Official DualShock 4 Protection & Switch Pro Protocol Hardening (Issue #22)**: Hardened Sony manufacturer string detection against partial/truncated USB reads (`iManufacturer`), preventing official wired DualShock 4 controllers from being hijacked. Aligned Nintendo Switch Pro transfer buffers to 64 bytes (`wMaxPacketSize`) and enforced host kernel driver detachment (`p_sceUsbdDetachKernelDriver`) across all claimed controller interfaces.

### Companion App Features
- **Isolated Test Mode in Input Tester Tab**: Added an isolated Test Mode toggled via R3 (Right Stick Click). When active, Player slot switching via L2, R2, and analog sticks is locked, allowing users to freely test analog sticks, triggers, and full-range inputs without switching player tabs.

---

## [v1.2.2-beta7] - Pre-release (2026-11-05)

### Controller Compatibility & Gameplay Fixes
- **ShanWan & Betop Controllers Support**: Fixed connection dropouts and freezes on ShanWan (BSP-G6, ShanWan PS3/PC, Redragon G808) and Betop controllers. Fixed an issue where moving the left analog stick horizontally triggered accidental button presses and locked up the sticks.
- **PlayStation 3 (DualShock 3 / Sixaxis) USB Support**: Restored full wired USB support for original PS3 controllers, resolving the issue where the controller connected but buttons were unresponsive.
- **Wireless 2.4GHz Dongle & Xbox 360 Responsiveness**: Eliminated input lag and stuttering when playing with high-performance 2.4GHz wireless dongles (such as GuliKit KK3 Max 2.4GHz adapter and 8BitDo). Restored proper status LED illumination (Player 1) on Xbox 360 controllers.
- **Stuck Rumble Prevention**: Fixed an issue where the controller could occasionally keep vibrating indefinitely when vibration stopped. Stop commands are now safely repeated to guarantee motors shut off immediately.
- **Switch Pro Stick Direction (Y-Axis)**: Fixed inverted vertical analog stick movement on Nintendo Switch Pro and compatible controllers, ensuring pushing up and down moves in the correct direction.

### Companion App Fixes
- **Player Screen Fix in Input Tester**: Resolved an issue where two active players were displayed simultaneously for a single connected controller. Only active controllers and users are now displayed.
- **Vibration Hardware Test**: Fixed the vibration test feature (Square button) in the Input Tester tab, allowing physical motor testing across all controllers (including Xbox Series X) even when in-game vibration is disabled in general settings.

---

## [v1.2.2-beta6] - Pre-release (2026-11-05)

### Multi-DualSense & Controller Subsystem
- **Multi-DualSense Wireless Bluetooth Support**: Connect up to 4 DualSense controllers simultaneously (Players 1 to 4) over Bluetooth with independent slot binding.
- **DualSense Edge (0x0DF2) Bluetooth Support**: Native registration in kernel Bluetooth driver with optimized slot allocation.
- **Full Sensors, Touchpad & Heartbeat**: Real-time 6-axis gyroscope/accelerometer, 2-point capacitive touchpad coordinate translation, and periodic heartbeat output reports preventing idle sleep.

### USB Driver, Handshakes & Input Lag Fixes
- **Switch Pro Official Handshake & Clone Separation**: Distinguishes official Nintendo Switch Pro from third-party clones using USB string descriptors. Official controllers complete full USB lock with increased 150ms timeout; clones remain in passive mode without crash loops.
- **Zero Input Lag on 1000Hz / 500Hz Controllers**: FIFO hardware drain for high-polling USB controllers (GuliKit KK3 Max, PowerA, Xbox One) without button-state corruption (eliminating sticky buttons).
- **USB Audio Accessory & Headphone Filter**: Top-level application collection validation strictly enforces Gamepad/Joystick collections, ignoring USB headphones/headsets with volume keys (SteelSeries Arctis Nova, Corsair, HyperX).
- **Third-Party PS4 Mode Support**: Native USB translation for GameSir (G7 SE, Cyclone 2, Nova Lite) and third-party DS4 controllers.
- **60 Hz Idle Keepalive & 250 Hz Active Pacing**: Paced idle transmission to 60 Hz (~16.6ms) for frame-perfect input tracking in 60 FPS rhythm and fighting games (*Project DIVA Future Tone*, *Street Fighter*).
- **8BitDo Ultimate 2C Wireless Dongle**: Idle receiver state (controller off) ignored; dongle reboot debounce eliminates user selection screen trap.

### Companion App & In-App Updater
- **Stable-Only In-App Updater**: Companion app queries `/releases/latest` and filters pre-releases, notifying and downloading only official stable releases.
- **Multi-Controller Status & Tester**: Live 4-slot status display with automatic slot focus, battery indicators, and text geometry fixes.
- **System Requirements Clarification**: Minimum requirement documented as **GoldHEN >= 2.4b18.10** for ELF user-service stability and AutoRun support across all exploits (including VueAfterFree on FW 12.50).

---

## [v1.2.2] - Official Maintenance Release (2026-11-03)

### PS4 FW 5.05 - 7.55 Stability & Zero-Panic Kernel Guard
- **Legacy Firmware Kernel Panic Guard (`bt_driver.c`)**: Added explicit firmware boundary check (`s_current_fw_bcd < 0x0900`). On firmwares prior to 9.00 (e.g. FW 5.05, 6.72, 7.xx), the PS4 kernel binary text segment ends at ~4.5 MB (`~0x480000`), well before modern kernel addresses. The daemon cleanly and safely skips DualSense Bluetooth initialization on FW < 9.00 without touching `/dev/bt`, `kqueue`, or invoking Ring 0 allowlist scanning (`0x4E0000 - 0x660000`).
- **Ring 0 Defense-in-Depth (`patch_allowlist`)**: Added immediate kernel-mode validation abort (`args->request->fw_version < 0x0900 -> return EINVAL`) preventing any out-of-bounds kernel memory dereferencing or Trap 12 (#PF) panics.
- **Full USB Controller Support on FW 5.05**: All Xbox (Series X|S, One, 360), Nintendo Switch, Google Stadia, DualShock 3, ShanWan, Betop, 8BitDo, and Generic HID controllers operate flawlessly at full 250 Hz (4 ms) on FW 5.05 and Payload Guest / GoldHEN BinLoader.

### DirectInput & Generic HID Robustness
- **Elimination of Fragile Cancel-on-Circle State Machine (`usb_driver.c`)**: Completely removed `s_awaiting_user_pick`, `execute_picker_cancel_teardown`, and `s_replug_vid`. Physical DS4 controllers hand over full, uninterrupted control to the USB controller. Pressing Circle/B in games, menus, or single-user hot-swaps will never unexpectedly disconnect or unregister the controller.
- **Stale Mode Packet Filter (`generic_hid.c`)**: Added report length validation discarding residual packets from prior controller modes (e.g. 20-byte Xbox packets on an 8-byte DirectInput controller) when switching modes.

### Canonical User Data Path Fallback (`/user/data`)
- **Resilient Logging on FW 5.05 (`log.c`)**: Added automatic fallback to canonical `/user/data/onefamily/onefamily.log` when `/data/onefamily/onefamily.log` cannot be opened (resolving missing log files on systems where `/data` is not symlinked).
- **Configuration & Controller Profile Fallbacks (`config.c`, `generic_hid.c`, `main.c`)**: Added `/user/data/` fallback lookups for `onefamily.ini`, `/user/data/onefamily/controllers/*.ini`, IPC vibration (`onefamily.vibe`), and IPC mapping reload (`onefamily.reload`).

---

## [v1.2.1] - Official Release (2026-11-02)

### Bluetooth DualSense Multi-Firmware Architecture & FW 13.52 Support
- **Pre-verified Static Table for FW 13.52**: Integrated field-tested, native static allowlist table (`static_1352[12]`) enabling 0 ms instant wireless Bluetooth pairing for DualSense and DualSense Edge controllers on PS4 Firmware 13.52 without runtime scanning overhead.
- **Universal VID-First AOB Scanner**: Replaced generic assembly prologue anchors with an ultra-safe, kernel-bounded VID-First clustering scanner (`[0x4E0000, 0x660000)`). Completely eliminates false positives from generic x86-64 stack frames and prevents any risk of Page Faults (`#PF` / Trap 12).
- **Greedy Nearest-Pairing Engine**: Intelligently pairs each Thrustmaster Vendor ID with its companion Product ID located within the same function block, resolving ioctl `register_dualsense` failures and guaranteeing that both VID and PID are patched simultaneously.
- **Dynamic Multi-Firmware Fallback**: Automatic, universal compatibility for all other GoldHEN firmware versions (5.05 up to 11.00+) with automatic caching in Ring 0 for clean restore on shutdown.
- **Discovered Offsets Logging**: Automatically outputs discovered hex offsets and mutex positions to `/data/onefamily/onefamily.log` on newly encountered firmware versions.

### In-App Companion Updater Fix
- **HTTP 302 Large Response Header Support**: Configured `sceHttpSetResponseHeaderMaxSize` to 64 KB and expanded HTTP memory pool to 1 MB, resolving `SCE_HTTP_ERROR_HEADER_SIZE_LIMIT` (`0x80431050`) on GitHub release asset redirects to Azure/AWS S3.
- **Dedicated Diagnostic Logging**: Added persistent `/data/onefamily/updater.log` tracing network status, HTTP headers, redirect hops, and download progress.

---

## [v1.2.0] - Official Release (2026-11-01)

### DualSense (PS5) Bluetooth Wireless & USB Support
- **Ring 0 Kernel Allowlist Patch**: Native integration of the Orbis OS kernel allowlist patch, allowing PlayStation 5 DualSense and DualSense Edge controllers to pair wirelessly over Bluetooth without disabling standard DualShock 4 pads.
- **Zero-Panic Multi-Firmware Architecture**: Verified static table for FW 12.50 protected by 64-bit magic header validation (`0x56415741E5894855UL`) and synchronized `bthid_lock` / `bthid_unlock` mutexes. On unmapped firmwares (such as FW 13.52), gracefully skips Bluetooth without touching unverified kernel memory, guaranteeing 0 panics.
- **64-bit Syscall Architecture (`daemon_sys_call6`)**: Dedicated inline assembly register constraints guaranteeing clean 64-bit pointers and resolving `sys_kevent` `-EFAULT` (-14).
- **FreeBSD `sys_kexec` Argument Mapping (`struct PatchArgs`)**: Correctly maps the FreeBSD kernel `uap` structure in Ring 0 execution.
- **DualSense USB Streaming**: Added Standard HID Handshake (`GET_DESCRIPTOR 0x22`), removed disruptive `SET_IDLE`, aligned output reports strictly to 64 bytes, eliminated destructive EP0 fallbacks, and added early ClearHalt recovery.

### Dual-Slot Virtual Pad & Co-op Support
- **Dual-Slot Architecture**: Simultaneous connection support for two controllers (e.g. DualSense via Bluetooth + Xbox pad over USB).
- **Native User Selection (`sceUserServiceRequestSelectUser`)**: Prompts the official PS4 account picker when a new controller connects, assigning independent user profiles.
- **Seamless Handover & Clean Teardown**: Automatically powers off physical DS4s assigned to the same user slot and handles modal cancellations gracefully.

### Universal Generic HID Rumble
- **3-Tier Universal Rumble System**: Auto-detection, pre-configured INI presets (Logitech, ShanWan, DragonRise, GreenAsia, Betop, 8BitDo), and raw hex directives (`rumble_report = 05 L R 00 00`).
- **Non-Destructive Profile Upgrader**: Injects `[Rumble]` sections into existing user controller profiles without overwriting custom remappings.
- **Heartbeat Rumble Polling**: Reads motor offsets `[3..4]` with 100 ms keep-alive heartbeat.

### Companion App UI & Theme
- **Procedural PS4 XMB Animated Wave Background**: Multi-layered sinusoidal waves with floating particles rendered at solid 60 FPS via L1/L2 zero-stall RAM compositing cache.
- **PlayStation Blue Theme**: Restyled all UI elements, badges, icons, cards, and borders with PlayStation Blue accents (`#0070D1`, `#00A2FF`).
- **Resilient Installer**: Explicitly unlinks destination payload binaries before writing, preventing file lock conflicts.

---

## [v1.1.6] - Hotfix Release (2026-10-29)
- Hotfix maintenance release and stability updates for Xbox controller USB detection.

---

## [v1.0.0] - Initial Release (2026-10-27)

### Architecture & Engine
- **Native PS4 Daemon (`onefamily.bin`):** Transitioned to a freestanding background ELF daemon running at 250 Hz (4 ms loop) with direct system calls, bypassing user-space PRX injection limitations.
- **Virtual DualShock 4 Device:** Emulates an official Type 1 DualShock 4 device (`libScePad`), delivering full system-wide compatibility across the PS4 XMB shell and all games.
- **Kernel & Dynamic Link Resilience:** Dynamic resolution of system libraries (`libSceUsbd.sprx`, `libScePad.sprx`, `libSceMbus.sprx`) without PLT or libc crashes.

### Hardware & Controller Support
- **Multi-Brand Compatibility:** Out-of-the-box support for Xbox 360, Xbox One, Xbox Series X|S, Nintendo Switch, DualShock 3, and DualSense.
- **Rumble / Vibration:** Implemented hardware rumble packets for Xbox GIP and Xbox 360 protocols, including IPC synchronization with the companion app and OS remote setting polling.
- **Dynamic Hotplugging:** Real-time connect and disconnect handling without freezing games or requiring console reboots.

### Companion App (`onefamily_companion.pkg`)
- **1080p 60 FPS Interface:** Custom hardware-rendered UI with PlayStation/Xbox crossover aesthetics.
- **All-in-One Automated Installer:** Automatically extracts the daemon payload to `/data/payloads/onefamily.bin`, initializes `/data/GoldHEN/onefamily.ini`, and registers auto-boot in `/data/GoldHEN/payloads.ini`.
- **Hardware Input Tester:** Live visualization of analog sticks, deadzones, trigger levels, bumper states, and button presses.
- **Stick Drift Auto-Calibration:** 3-second noise analysis routine that recommends and saves the minimal optimal deadzone.
- **Vibration Feedback Tester:** Hardware test sequence for left (heavy) and right (light) motors.
