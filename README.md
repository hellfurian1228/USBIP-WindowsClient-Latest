# USBIP-WindowsClient
<img width="256" height="256" alt="usbip" src="https://github.com/user-attachments/assets/3d66d928-e987-4ec4-a350-59341a83051b" />

[![Status](https://img.shields.io/badge/Status-Release-green.svg?style=flat-square)](https://github.com/hellfurian1228/USBIP-WindowsClient)
[![Donate](https://img.shields.io/badge/Donate-PayPal-blue.svg?style=flat-square&logo=paypal)](https://www.paypal.com/donate/?hosted_button_id=P3P43EXYJWRLC)

## USBIP-WindowsClient
A modern, Qt-based graphical user interface for the USB/IP protocol on Windows. This client allows you to easily connect to remote USB/IP servers, mount shared USB devices, and manage connections with a clean and intuitive interface.

## ☕ Support the Project
If you find this tool useful and want to support continued development, I utilize subscription based software for code. Donations will go towards this. Thanks!

[**Donate via PayPal**](https://www.paypal.com/donate/?hosted_button_id=P3P43EXYJWRLC)

## Discord https://discord.gg/R2nfbS4K2

## 📢 Project Update: The Future of USBIP-AndroidHost
This repository represents the final free, open-source version of USBIP-AndroidHost.

When I started this project, I stated that it would be a free solution, and I am honoring that promise. This existing open-source repository will always remain free and available to you here on GitHub.

I want to extend a massive thank you to everyone who has helped with testing, reporting bugs, and ensuring we could build a stable, functional, and truly free alternative to the other paid app out there (you know the one) with its predatory, hardware-locked licensing!

## 🚀 What's Next: Google Play Store Release
Moving forward, I will be continuing active development—adding new features, further optimizing the native C++ networking codebase, and refining the UI.

To help cover the costs of development tools, testing hardware, and the sheer amount of time I invest in maintaining this protocol, the next evolution of this app will be released as a fully approved, fixed-price app on the Google Play Store.

By purchasing the Play Store version, you will get:

Automatic Updates: Seamless background updates so you never have to manually install an APK again.

Dedicated Support: Priority troubleshooting for your specific hardware setups.

Exclusive New Features: Access to all future quality-of-life improvements, UI overhauls, and advanced networking features.

No Predatory Subscriptions: A flat, one-time fee for unlimited devices. No recurring charges, and no hardware-locked licenses.

Thank you for supporting this project and helping it grow from an experimental prototype into the low-latency networking tool it is today!

## ✨ Features
Intuitive GUI: Easily scan hosts, list available remote USB devices, and attach/detach them with a single click.

Auto-Connect: Automatically reconnect to desired devices on startup.

System Tray Integration: Minimize the application to the system tray to keep it running in the background.

Audio Relay Subsystem: Stream and receive audio over UDP to relay audio devices between systems.

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

## 🛠️ Tech Stack
*   **C++ (JNI):** High-performance native server daemon for protocol handling.
*   **CMake:** Unified build system for native components.

*Note: This is an early beta. Use it, break it, and report issues to help improve stability.*
