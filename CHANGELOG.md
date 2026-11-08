# Changelog - OneFamily

All notable changes to the OneFamily project are documented in this file.

## [v1.2.2-beta16] - Pre-release (2026-11-08)

### DualSense Adaptive Trigger Force Calibration (Issue #64)
- **Calibrated Motor Force for Simple Effect Modes**: Corrected trigger parameter encoding in `bt_driver.c` for Feedback (`0x01`), Weapon/Hair Trigger (`0x02`), and Vibration (`0x06`). Previously, preset values used zone indices (1-8) resulting in ~2-3% motor exertion.
- **Hair Trigger Preset (`0x02`)**: Calibrated start position (`0x20`), end position (`0x50`), and full motor force (`0xFF` / 100%) for an immediate, tactile break point.
- **Continuous Resistance Preset (`0x01`)**: Set start position (`0x00`) with medium force (`0x64` / 40%) for smooth resistance across the entire pull.
- **Trigger Vibration Preset (`0x06`)**: Configured frequency (`0x0C` / 12 Hz) and high amplitude (`0xC8` / 78%) for robust haptic feedback.
- **Active Motor Release on Trigger Off**: Changed Off command from `0x00` to `0x05`, actively releasing the trigger motor gearing to neutral.

### Companion App Performance & Input Responsiveness (Issue #65)
- **O2 Optimization**: Enabled `-O2` optimization in `build_app.ps1` for release builds of the Companion app.
- **RAM Backbuffer & Single VRAM Blit**: Re-architected `Scene2D` rendering to perform all pixel drawing, shape rendering, and alpha blending in cached heap system RAM (`ramBackBuffer`). Replaced per-pixel reads from uncached Write-Combined Direct Memory (Type 3) with a single sequential `memcpy` during `SubmitFlip()`, eliminating CPU pipeline stalls and boosting frame rates from 4-5 FPS to a solid 30-60 FPS.
- **FreeType Glyph Cache & Instant Text Measurement**: Implemented per-face character glyph rasterization caching (`CachedGlyph`) in `Scene2D`. Character bitmaps and metrics are cached upon first encounter, reducing subsequent character draws to lightweight memory blits and eliminating redundant FreeType rasterization. Centered text layout now uses `Scene2D::GetTextWidth()` without reloading glyphs each frame.
- **Hardware Input Batching (`scePadRead`)**: Replaced single-state polling (`scePadReadState`) with multi-sample batch reading (`scePadRead` up to 16 samples) in the main interaction loop and setup wizard. All buffered button events are accumulated across the frame interval, ensuring fast button taps (<80 ms) are never dropped.

---

## [v1.2.2-beta15] - Pre-release (2026-11-08)

### DualSense Bluetooth Input Freeze Resolution (Issues #60, #62)
- **Eliminated Feature Report 0x05 Stalling in Orbis Kernel `bthid`**: Removed the synchronous `ioctl(s_bt_fd, 0x8010622FUL)` Feature Report 0x05 query from `bt_driver_poll_step()`. On Orbis OS FW 13.x, requesting Feature Report 0x05 over the L2CAP control channel stalled the interrupt IN endpoint queue in `bthid`, preventing any subsequent input reports (Report `0x01` or `0x31`) from reaching the daemon and leaving the controller completely unresponsive on the "Who is using this controller?" user selection screen.
- **Native Extended Report Switch via Output Report 0x31**: Alligned switching to extended 78-byte Report `0x31` with the proven AnyPad-PS4 reference architecture. The DualSense autonomously switches to Report `0x31` upon receiving the first Output Report `0x31` (`out[41] = 0x02`, `out[44] = 0x02`), eliminating any need for control channel feature report transactions.
- **Pre-Calibrated Nominal IMU Scales**: Standardized motion sensor processing to utilize the nominal DualSense IMU scales pre-initialized in the driver (`gyro_scale = 2000 / 32768 * deg2rad`, `accel_scale = 1 / 8192`), ensuring immediate, drift-free gyroscope and accelerometer responsiveness with zero silicon delay.
- **Input Packet Telemetry**: Added diagnostic logging for the first 5 input reports received per device to track report ID and length, ensuring complete visibility into the Bluetooth stream.

---

## [v1.2.2-beta14] - Pre-release (2026-11-08)

### DualSense Bluetooth Universal Capacity & Safe-Gate (Issues #60, #62)
- **Universal Kernel Link Capacity**: Expanded `controller_capacity` support beyond FW 12.50 to FW 13.02 and FW 13.52, removing the hardcoded FW 12.50 signature check and dynamically discovering kernel `lock` and `unlock` offsets (`s_discovered_lock` / `s_discovered_unlock`).
- **Eliminated DualSense Login Screen Freeze**: Fixed an issue where DualSense controllers on FW 13.02/13.52 would connect and launch the user profile selection screen but stop responding to button inputs, caused by `bthid` dropping 78-byte reports when link capacity was left at 22 bytes.
- **Safe-Gate Invariant for Unmapped Firmwares**: On firmware versions where kernel link capacity cannot be dynamically patched (such as FW 9.00, 9.60, or 11.00), OneFamily strictly suppresses Feature Report 5 and 78-byte output reports. The DualSense operates stably in standard 10-byte Report `0x01` mode, ensuring all buttons, sticks, D-Pad, and PS button remain responsive without Bluetooth L2CAP packet drops.
- **Immediate Pairing Handshake & Output Heartbeat**: Applied link capacity patch immediately upon connection and reconnection, enabling initial handshake reports (`out[41] = 0x02`, `out[44] = 0x02`) to cleanly finish the blue pairing animation across all supported firmwares.

### Controller Remapping Engine Expansion (Issue #63)
- **Raised Maximum Remap Rules to 32**: Doubled `MAX_REMAP_RULES` from 16 to 32 per controller profile in `usb_driver.c`, allowing complex custom controller remapping configurations without truncating rules.
- **Per-Rule Diagnostic Logging**: Added detailed log output for each successfully applied button remap rule loaded from `/data/onefamily/controllers/<VID>_<PID>.ini`.

---

## [v1.2.2-beta13] - Pre-release (2026-11-08)

### Motion Sensors & Gyroscope Fixes (Issue #53)
- **Corrected IMU Sensor Mapping to PS4 ABI**: Fixed a fundamental field inversion in the Orbis OS `ScePadData` layout where angular velocity (gyroscope in rad/s) was being written into the accelerometer field and linear acceleration (in g) was being written into the angular velocity field.
- **Eliminated Uncontrollable Camera Spin / Gyroscope Drift**: Fixed the constant 57.3 deg/s (1.0 rad/s) rotational spin in games that read gyroscope rates, caused by games reading the static 1.0g gravity vector as angular velocity.
- **Neutral Motion Detection in Virtual Pad**: Updated `vpad.c` neutral detection to monitor angular velocity instead of linear acceleration, allowing the virtual pad to properly transition to 60 Hz idle state when resting.

### DualSense Bluetooth Robustness & Diagnostics (Issue #62)
- **Retrocompatible Bluetooth Packet Acceptance**: Enabled processing of Bluetooth DualSense reports when `report[1] == 0x1F` or when Report ID `0x01` / `0x31` is presented directly in `report[2]`, restoring compatibility across different Orbis kernel bluetooth stack states.
- **Support for 9-byte & 10-byte Simple Reports**: Added fallback handling for both 9-byte and 10-byte simple input reports with digital trigger mapping for L2/R2.
- **Rate-Limited Diagnostic Logging**: Added throttling for dropped packets and CRC mismatch diagnostics to facilitate debugging without flooding the daemon log.

---

## [v1.2.2-beta12] - Pre-release (2026-11-08)

### Motion Sensors & Controller Accuracy Fixes (Issue #53)
- **Orientation Quaternion for USB DS4 & Switch Pro**: Implemented real-time complementary orientation filter (`motion_fusion.h`) on USB DualShock 4 and Nintendo Switch Pro controllers, enabling full 3D tilt tracking in games that query the quaternion orientation (e.g. Gravity Rush).
- **Nintendo Switch Pro Multi-Sample IMU Averaging**: Decoded and averaged all valid motion samples in Switch Pro report `0x30` packets, and carried over previous motion samples during `0x21` subcommand replies to eliminate jitter and packet drops.
- **DualSense USB Touchpad Coordinate Scaling**: Aligned vertical capacitive touchpad coordinates from DualSense native resolution (`1920x1080`) to DualShock 4 range (`1920x942`), preventing out-of-bounds Y inputs.

---

## [v1.2.2-beta11] - Pre-release (2026-11-07)

### Controller Compatibility & Gameplay Fixes
- **User Account Selection Screen Restored**: Restored the native PlayStation 4 "Who is using this controller?" profile selection prompt whenever a USB or Bluetooth controller connects, allowing users to freely choose which account to assign the controller to.
- **DualSense Bluetooth Connection Stability**: Resolved an issue where DualSense wireless controllers could disconnect or become unresponsive shortly after connecting on Firmware 9.00 and other firmware versions, and restored full support for connecting up to 4 DualSense controllers simultaneously.
- **Logitech Cordless RumblePad 2 Vibration Stop Fix**: Added the dedicated stop command required by Logitech RumblePad 2 controllers so vibration stops immediately when a game ends a rumble effect.

---

## [v1.2.2-beta10] - Pre-release (2026-11-07)

### Controller Compatibility & Gameplay Fixes
- **User Account Binding & Unresponsive Input Fix**: Fixed an issue where connected controllers (including Bluetooth DualSense and USB PS3 controllers) could register on the console without responding to button presses or allowing user profile selection.
- **Official Nintendo Switch Pro & Third-Party Clones**: Restored reliable USB wake-up for official Nintendo Switch Pro controllers (preventing charging-only mode) while keeping third-party Switch Pro / PS4 multi-mode controllers (such as GXtrust Muta 542) stable without disconnections or stuck inputs.
- **GameSir Controllers Support (Nova 2 Lite & Cyclone 2)**: Fixed 2.4GHz wireless dongle and wired PS4 mode detection on GameSir Nova 2 Lite (preventing false keyboard detection) and resolved continuous vibration issues on GameSir Cyclone 2.
- **Logitech RumblePad 2 Stability**: Resolved self-vibration and USB disconnections on Logitech RumblePad 2 controllers.
- **DualSense Bluetooth Support on Firmware 12.00 - 13.50**: Improved Bluetooth controller registration across newer PlayStation 4 firmware versions and reduced repeated reconnection notifications after idle periods.
- **Motion Sensors & Gyroscope Support**: Added full motion sensor (gyroscope and accelerometer) translation for Nintendo Switch Pro controllers and improved gyroscope accuracy for DualShock 4 and DualSense controllers.
- **USB Headsets & Audio Dongles Coexistence**: Prevented USB audio headsets and wireless dongles (such as SteelSeries Arctis Nova Elite) from being repeatedly scanned by the controller driver.

### New Features
- **Custom Button Remapping for All Controllers**: Added support for custom button remapping via INI profiles (`/data/onefamily/controllers/VVVV_PPPP.ini`) across all supported controllers, including Xbox, Switch Pro, PlayStation, and Generic HID gamepads.

---

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
