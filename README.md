# Amir Mobasheraghdam — ESP32-C3 Idea Beacon

An interactive **Seeed Studio XIAO ESP32-C3** project with a **1.3" SH1106 OLED**, Wi-Fi captive portal, BLE advertising, guest idea submission, persistent storage and a local admin dashboard.

## What this version fixes

This repository is a cleaned-up and hardened revision of the original sketch.

- Removed the hard-coded public admin PIN.
- Uses a random 6-digit admin PIN generated on first boot and stored in ESP32 NVS.
- Uses a fresh random admin session token and exact cookie parsing.
- Adds CSRF protection to authenticated admin POST actions.
- Adds login throttling and a short lockout after repeated wrong PIN attempts.
- Validates delete indexes instead of trusting arbitrary form input.
- Validates guest, quote and author lengths on the device side.
- Makes the pending OLED idea queue drop the oldest item instead of silently losing the newest submission.
- Handles common Android, Apple and Windows captive-portal probe URLs.
- Avoids calling the DNS server when DNS startup failed.
- Keeps Wi-Fi startup before BLE for more reliable ESP32-C3 radio initialization.
- Preserves persistent quotes, messages, unread count and OLED settings.

## Hardware

- Seeed Studio XIAO ESP32-C3
- 1.3" 128x64 SH1106 OLED
- I2C connection using the board's normal SDA/SCL pins

For an SSD1306 display, replace the `U8G2_SH1106_128X64_NONAME_F_HW_I2C` constructor in the sketch with the commented SSD1306 alternative.

## Arduino IDE libraries

Install:

- **U8g2** by olikraus
- ESP32 board package by Espressif Systems

The ESP32 package provides:

- WiFi
- WebServer
- DNSServer
- Preferences
- BLE

## First boot

1. Flash `AMIR_IDEA_BEACON.ino`.
2. On the first boot, the OLED shows a randomly generated **6-digit ADMIN PIN** for about 12 seconds.
3. Save that PIN.
4. Connect to Wi-Fi:
   - SSID: `Amir-Message`
   - Password: none
5. Open `http://10.77.0.1/` if the captive portal does not open automatically.
6. Admin dashboard:
   - `http://10.77.0.1/admin`

The PIN is not stored in the public source code.

## Changing the admin PIN

Open the admin dashboard and use the **Sicherheit** section. The new PIN must be exactly 6 digits. Changing it invalidates the previous session.

## If you lose the PIN

The PIN lives in the ESP32 Preferences/NVS namespace `amir-beacon`.

The simplest recovery option is to erase the board's flash/NVS from Arduino IDE or `esptool`, then flash again. This also erases saved quotes/messages/settings and causes a new PIN to be generated.

## Main behavior

- OLED continuously scrolls German quotes.
- Guests can submit a name and idea.
- A new idea triggers an OLED alert and then scrolls on screen.
- Up to 20 messages are kept in persistent storage.
- Admin can:
  - change OLED speed and contrast
  - add/delete/reset quotes
  - view/delete/clear ideas
  - rotate to a new quote
  - change the admin PIN
  - log out

## Security scope

This device intentionally runs an **open local Wi-Fi access point** so visitors can join easily. Do not treat it as an Internet-facing application.

The admin dashboard is protected by a local PIN, per-boot session token, CSRF token and basic login throttling. Anyone with physical access to the device can still erase or reflash it.

## Files

- `AMIR_IDEA_BEACON.ino` — main firmware
- `.github/prompts/review-and-fix.prompt.md` — reusable AI code-review/fix prompt
- `.github/copilot-instructions.md` — project-specific review/build expectations
- `.gitignore` — common local/Arduino/PlatformIO exclusions

## Author

**Amir Mobasheraghdam**

Embedded Systems • Automation • ESP32 • IoT • PLC • Software Development
