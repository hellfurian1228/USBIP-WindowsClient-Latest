# USBIP-WindowsClient
<img width="256" height="256" alt="usbip" src="https://github.com/user-attachments/assets/3d66d928-e987-4ec4-a350-59341a83051b" />

[![Status](https://img.shields.io/badge/Status-Release-green.svg?style=flat-square)](https://github.com/hellfurian1228/USBIP-WindowsClient)
[![Donate](https://img.shields.io/badge/Donate-PayPal-blue.svg?style=flat-square&logo=paypal)](https://www.paypal.com/donate/?hosted_button_id=P3P43EXYJWRLC)

https://mizukos.itch.io/usbip

## USBIP-WindowsClient
A modern, Qt-based graphical user interface for the USB/IP protocol on Windows. This client allows you to easily connect to remote USB/IP servers, mount shared USB devices, and manage connections with a clean and intuitive interface.

## 📢 Project Update: The Future of the USB/IP Client
This repository represents the final free, open-source version of the USB/IP Windows Client.

When I started this project, I stated that it would be a free solution, and I am honoring that promise. This existing open-source repository will always remain free and available to you on GitHub.

I want to extend a massive thank you to everyone who has helped with testing, reporting bugs, and ensuring we could build a stable, functional, and truly free alternative to the other paid apps out there with their predatory, hardware-locked licensing!

## 🚀 What's Next: Steam Release
Moving forward, I will be continuing active development—adding new features, further optimizing the native C++ networking codebase, and refining the UI.

To help cover the costs of development tools, testing hardware, and the sheer amount of time I invest in maintaining and expanding this protocol, the next evolution of this client app will be released as a fixed-price application on Steam.

By purchasing the Steam version, you will get:

Automatic Updates: Seamless background updates via Steam so you never have to manually download installers again.

Dedicated Support: Priority troubleshooting for your specific hardware setups, drivers, and USB peripherals.

Exclusive New Features: Access to all future quality-of-life improvements, UI overhauls, and advanced networking features.

No Predatory Subscriptions: A flat, one-time fee for unlimited use. No recurring charges, and no hardware-locked licenses.

Thank you for supporting this project and helping it grow from an experimental prototype into the robust, low-latency networking tool it is today!

## ☕ Support the Project
If you find this tool useful and want to support continued development, I utilize subscription based software for code. Donations will go towards this. Thanks!

[**Donate via PayPal**](https://www.paypal.com/donate/?hosted_button_id=P3P43EXYJWRLC)

[**Donate via Ko-Fi**](https://ko-fi.com/mizukos)

## Discord https://discord.gg/R2nfbS4K2

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

## 📜 Credits & Acknowledgements

This project is built upon the foundational work of the global open-source community:

*   **Takahiro Hirofuchi & the NAIST Research Team:** The original architects of the USB/IP protocol and researchers at the Nara Institute of Science and Technology.
*   **The Linux Kernel Community:** For maintaining and improving the core USB/IP drivers within the mainline kernel.
*   **cezanne (GitHub):** The creator of the `usbip-win` project, which successfully ported the Virtual Host Controller Interface (VHCI) to Windows.
*   **USBIP-Win2 Community:** For the ongoing development of modern Windows drivers and clients that this host is designed to communicate with.
