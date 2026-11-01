# Changelog - OneFamily

All notable changes to the OneFamily project are documented in this file.

## [v1.2.1] - Hotfix Release (2026-11-01)
- **Zero-Panic Kernel Safety Guard**: Eliminated unmapped Ring 0 memory scanning across unknown firmware versions, completely preventing supervisor page faults and kernel panics on FW 13.52 and other unmapped firmwares.
- **Graceful Unsupported Firmware Fallback**: Safely skips the Bluetooth allowlist subsystem on unmapped firmwares while keeping USB controllers (Xbox, Switch, DualSense over USB, Generic HID) 100% active and stable.

---

## [v1.2.0] - Official Release (2026-11-01)

### DualSense (PS5) Bluetooth Wireless & USB Support
- **Ring 0 Kernel Allowlist Patch**: Native integration of the Orbis OS kernel allowlist patch, allowing PlayStation 5 DualSense and DualSense Edge controllers to pair wirelessly over Bluetooth without disabling standard DualShock 4 pads.
- **Hybrid Multi-Firmware Compatibility**:
  - *Verified Static Table (FW 12.50)*: Zero-overhead allowlist execution protected by 64-bit magic header validation (`0x56415741E5894855UL`) and synchronized `bthid_lock` / `bthid_unlock` mutexes.
  - *Safe Ring 0 AOB Dynamic Scanner*: Dynamic signature scanner for other firmwares (9.00, 11.00, 13.00, 13.52+) verifying target opcode patterns within tight safety bounds.
  - *Zero-Panic Safety Guarantee*: Memory write protection (CR0 WP) is strictly preserved unless all integrity checkpoints pass.
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
