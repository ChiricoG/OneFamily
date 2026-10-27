# Changelog - OneFamily

All notable changes to the OneFamily project are documented in this file.

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
