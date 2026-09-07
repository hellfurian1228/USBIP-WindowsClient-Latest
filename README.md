# USBIP-WindowsClient
<img width="256" height="256" alt="usbip" src="https://github.com/user-attachments/assets/3d66d928-e987-4ec4-a350-59341a83051b" />

[![Status](https://img.shields.io/badge/Status-Beta-orange.svg?style=flat-square)](https://github.com/hellfurian1228/USBIP-WindowsClient)
[![Donate](https://img.shields.io/badge/Donate-PayPal-blue.svg?style=flat-square&logo=paypal)](https://www.paypal.com/donate/?hosted_button_id=P3P43EXYJWRLC)

## ☕ Support the Project
If you find this tool useful and want to support continued development, I utilize subscription based software for code. Donations will go towards this. Thanks!

[**Donate via PayPal**](https://www.paypal.com/donate/?hosted_button_id=P3P43EXYJWRLC)

## Discord https://discord.gg/R2nfbS4K2

## USBIP-WindowsClient
A modern, Qt-based graphical user interface for the USB/IP protocol on Windows. This client allows you to easily connect to remote USB/IP servers, mount shared USB devices, and manage connections with a clean and intuitive interface.

## ✨ Features
Intuitive GUI: Easily scan hosts, list available remote USB devices, and attach/detach them with a single click.

Auto-Connect: Automatically reconnect to desired devices on startup.

System Tray Integration: Minimize the application to the system tray to keep it running in the background.

System Logger: Built-in real-time logger for monitoring connection status and troubleshooting.

Multi-Architecture Support: Fully compatible with both x64 and ARM64 Windows devices.

## 📦 Requirements
OS: Windows 10 (version 1903 or later) or Windows 11 (x64 / ARM64)

Drivers: USB/IP VHCI and UDE drivers installed (Test Signing mode enabled if using unsigned drivers)

Framework: Qt 6.11.1 or later

## 🛠️ Build Instructions
### 1. IDE Setup
*   Open the project folder in Visual Studio or VS Code.

### 2. Configuration
*   Configure the project using CMake.

### 3. Compilation
*   Build the ALL_BUILD target in Release configuration.

### 4. Packaging
*   Package the installer using CPack (NSIS generator).

## Updates

The installed client checks the latest GitHub release shortly after startup. When a newer
release is available, it displays the release notes and offers to download and launch the
Windows installer. The client exits before the installer replaces its files.

To publish an update, create a GitHub release in
`hellfurian1228/USBIP-WindowsClient` with a version tag such as `v1.0.5`, include the change
log in the release description, and attach the generated `USBIPClient_Installer.exe` installer. The application
version is defined by the `project(... VERSION ...)` value in `CMakeLists.txt`; CMake also generates the
Inno Setup version include used by `installer.iss`.

## Device telemetry

Live Device Telemetry reports the current imported-device state from the Windows VHCI driver
and displays the USB class retained during the most recent host scan. The current prebuilt
driver does not expose transfer counters or timing samples, so throughput and jitter are shown
as unavailable. Implementing those fields requires VHCI driver statistics or a user-mode
USB/IP transport that observes every transfer.

When the paired host provides telemetry, the client polls `http://<host>:3241/telemetry` once
per second, or uses the `telemetry_port` DNS-SD TXT value when advertised. The response must
contain a `devices` array with cumulative `bytes_to_client` and `bytes_from_client` counters,
plus optional `latency_us_average` and `jitter_us` values, keyed by `busid`. The client computes
throughput from counter deltas and shows `Unavailable` for unsupported, null, malformed, or
unreachable telemetry. Telemetry failures do not affect USB/IP connections.

## Diagnostic logs

The client creates a `Logs` folder beside the installed executable. Normal application and Qt
warning/error messages are appended to daily UTF-8 text files named
`USBIPClient_yyyyMMdd.txt`. If Windows terminates the process because of an unhandled exception,
the exception code is appended to `USBIPClient_Crash.txt`. The in-app logger remains a separate
view and clearing it does not delete the saved files.

## 🛠️ Tech Stack
*   **C++:** Native Windows client and USB/IP protocol handling.
*   **CMake:** Unified build system for native components.

