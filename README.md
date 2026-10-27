<p align="center">
  <img src="app/assets/logo.png" alt="OneFamily Logo" width="220"/>
</p>

<h1 align="center">OneFamily</h1>
<p align="center">
  <b>Universal USB Controller Daemon & Companion App for PlayStation 4</b><br>
  <i>"No more console war - we are a family!"</i>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/PS4-GoldHEN%20Compatible-0070D1?style=for-the-badge&logo=playstation" alt="GoldHEN Compatible"/>
  <img src="https://img.shields.io/badge/Polling-250%20Hz%20(4ms)-brightgreen?style=for-the-badge" alt="250 Hz Polling"/>
  <img src="https://img.shields.io/badge/Platform-PlayStation%204-blue?style=for-the-badge" alt="Platform PS4"/>
  <img src="https://img.shields.io/badge/License-MIT-orange?style=for-the-badge" alt="License MIT"/>
</p>

---

## 🌟 Overview

**OneFamily** is a modern, high-performance background daemon and 1080p companion homebrew app for jailbroken PlayStation 4 consoles running **GoldHEN**.

It allows you to connect virtually any USB controller (Xbox 360, Xbox One, Xbox Series X|S, Nintendo Switch, DualShock 3, DualSense) and play across the entire PS4 operating system and all games with **zero input lag**, full analog precision, and rumble feedback.

---

## 📸 Screenshots

<p align="center">
  <b>PS4 Dashboard & Auto-Boot Notification</b><br>
  <img src="assets/screenshots/02_daemon_notification.jpg" width="85%" alt="OneFamily Daemon Notification on XMB"/>
</p>

<p align="center">
  <b>All-in-One Automated Setup Guide</b><br>
  <img src="assets/screenshots/03_setup_guide.jpg" width="85%" alt="Setup Guide"/>
</p>

<p align="center">
  <b>Settings & Configuration (Real-Time INI Reload)</b><br>
  <img src="assets/screenshots/04_settings_tab.jpg" width="85%" alt="Settings Tab"/>
</p>

<p align="center">
  <b>Hardware Input Tester & Drift Auto-Calibration</b><br>
  <img src="assets/screenshots/05_tester_calibration.jpg" width="85%" alt="Input Tester and Stick Calibration"/>
</p>

---

## ✨ Key Features

- **🎮 Native DualShock 4 Virtual Device:** Emulates an official Type 1 DualShock 4 via `libScePad`, ensuring 100% compatibility with all PS4 games, apps, and the system shell (XMB).
- **⚡ Ultra-Low Latency (250 Hz Polling):** Operates on an unthrottled 4 ms polling loop for competitive, instantaneous input response.
- **🚀 All-in-One Automated Setup:** The companion app handles everything on first launch:
  - Deploys the background daemon payload to `/data/payloads/onefamily.bin`.
  - Configures `/data/GoldHEN/payloads.ini` for automatic boot on system start (`[AutoRun]`).
  - Initializes `/data/GoldHEN/onefamily.ini` with optimal default settings.
- **🎯 Analog Stick Drift Auto-Calibration:** Measures resting stick noise across 180 frames (~3 seconds) and automatically calculates the minimal ideal deadzone to eliminate drift while preserving responsiveness.
- **📳 Dual Vibration / Rumble Support:** Full support for large (heavy) and small (light) rumble motors, including a dedicated hardware vibration tester in the companion app.
- **🔄 On-the-Fly USB Hotplugging:** Plug, unplug, or switch controllers anytime without needing to restart the game or console.
- **🕹️ Customizable Controls:**
  - Stick Deadzones: 0% up to 25%.
  - Trigger Activation Thresholds.
  - Swap Bumpers and Triggers (L1/R1 $\leftrightarrow$ L2/R2).
  - Nintendo Switch Button Layout: Positional (A=Circle, B=Cross) or By Letter.
  - Xbox Guide Button Mapping: Native PS Button or Touchpad Click.

---

## 🎮 Supported Controllers

| Controller Family | Connection | Vibration | Drift Calibration |
| :--- | :---: | :---: | :---: |
| **Microsoft Xbox Series X \| S** | USB | ✅ Yes | ✅ Yes |
| **Microsoft Xbox One / One S / One X** | USB | ✅ Yes | ✅ Yes |
| **Microsoft Xbox 360 (Official & Clones)** | USB | ✅ Yes | ✅ Yes |
| **Nintendo Switch Pro Controller** | USB | — | ✅ Yes |
| **Nintendo Switch Third-Party Controllers** | USB | — | ✅ Yes |
| **Sony PlayStation 5 DualSense / Edge** | USB | ✅ Yes | ✅ Yes |
| **Sony PlayStation 3 DualShock 3** | USB | ✅ Yes | ✅ Yes |

---

## 🚀 Quick Start / Installation

1. **Install the Companion App:**
   - Download `onefamily_companion.pkg` from the [Releases](../../releases) page.
   - Install it on your PS4 via GoldHEN Package Installer.

2. **First-Time Setup (All-in-One):**
   - Open **OneFamily Companion** from your PS4 dashboard.
   - The app will automatically install the payload and configure GoldHEN auto-boot.

3. **Reboot & Enjoy:**
   - Restart your console and enable GoldHEN.
   - A notification **"OneFamily Daemon Active"** will appear on the top-left of the screen.
   - Plug in your USB controller — it works instantly!

4. **Customize & Calibrate:**
   - Launch **OneFamily Companion** anytime to calibrate stick drift, test vibration, or adjust settings. Changes apply in real time!

---

## 🛠️ Building from Source

### Prerequisites
- [OpenOrbis PS4 Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain)
- LLVM Clang / LLD (`clang`, `ld.lld`)
- .NET 8 / 9 SDK (for `PkgTool.Core`)
- Python 3.x

### Build Commands (PowerShell)

```powershell
# 1. Build the OneFamily background daemon payload
.\build_daemon.ps1

# 2. Build and package the Companion App PKG
.\build_app.ps1
```

The compiled binaries will be output to:
- `bin/onefamily.bin` (Daemon payload)
- `bin/onefamily_companion.pkg` (PS4 Companion Package)

---

## 📜 License

This project is licensed under the **MIT License** - see the [LICENSE](LICENSE) file for details.
