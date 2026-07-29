# BraiVo: Talking Braille Calculator

BraiVo is an open-source, audio-assisted talking calculator designed to be accessible, featuring voice feedback and a tactile braille-based layout. It is powered by an ESP32 microcontroller, providing both visual output via an LCD and auditory feedback using the Talkie library.

## Features
- **Voice Response Mechanism:** Provides spoken feedback for key presses, mathematical expressions, results, and errors.
- **Advanced Math Parsing:** Handles basic arithmetic, fractions, square roots, percentages, and identifies repeating decimals.
- **S-D Toggle:** Easily switch between fraction and decimal representations of the result.
- **Tactile Keypad:** Uses a custom 4x6 matrix keypad mapped specifically for intuitive, braille-like navigation.
- **Battery Monitoring:** Integrated battery voltage monitoring to ensure reliable operation.

## Hardware Components
- **Microcontroller:** ESP32 (esp32dev)
- **Display:** 16x2 I2C LCD (LiquidCrystal_I2C)
- **Input:** 4x6 Matrix Keypad
- **Audio:** Speaker (driven by the Talkie voice synthesis library)

## Software Stack
- **Framework:** Arduino / PlatformIO
- **Dependencies:**
  - `marcoschwartz/LiquidCrystal_I2C`
  - `Keypad`
  - `arminjo/Talkie`

## Keypad Layout (4x6 Matrix)
The physical keypad is mapped as follows:
- **Row 1:** `[---]`, `[ 7 ]`, `[ 8 ]`, `[ 9 ]`, `[DEL]`, `[ AC ]`
- **Row 2:** `[ SQ]`, `[ 4 ]`, `[ 5 ]`, `[ 6 ]`, `[ / ]`, `[  - ]`
- **Row 3:** `[ SD]`, `[ 1 ]`, `[ 2 ]`, `[ 3 ]`, `[ * ]`, `[  + ]`
- **Row 4:** `[ ( ]`, `[ ) ]`, `[ 0 ]`, `[ . ]`, `[ % ]`, `[  = ]`

*(Note: SQ stands for Square Root, SD toggles fraction/decimal)*

## Getting Started
1. Open the project in [PlatformIO](https://platformio.org/).
2. Ensure the required libraries are installed (they are defined in `platformio.ini`).
3. Connect your ESP32 and required peripherals according to the provided schematic/wiring screenshots in the `screenshots/` directory.
4. Build and upload the firmware.

## Wiring & Hardware Guides
Refer to the `screenshots/` directory for visual guides on wiring the 4-pin tactile switch matrix and the ESP32 connections.
