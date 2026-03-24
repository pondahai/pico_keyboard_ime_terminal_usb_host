# PicoType Keyboard - Project Overview

PicoType is a standalone Chinese input and display device designed for the Meshtastic mesh networking protocol. It is built on the Raspberry Pi Pico (RP2040) and features a integrated Chinese IME (Bopomofo), a custom high-performance font renderer, and a full tab-based UI.

## Architecture

The project employs a dual-core architecture to ensure responsiveness and stability:

*   **Core 0 (Main Logic & UI):**
    *   Handles the main application loop and UI state management.
    *   Processes keyboard matrix scanning (8x8 matrix).
    *   Manages the tab-based UI system (Chat, Node List, Channel List, My Info).
    *   Performs Protobuf encoding and decoding (Nanopb) for Meshtastic messages.
    *   Runs the `ImeEngine` for Bopomofo-to-Chinese character conversion.
    *   Uses a custom `FontRenderer` to draw UTF-8 text and Chinese glyphs on an ILI9341 TFT display.
*   **Core 1 (USB Host):**
    *   Dedicated to the PIO-USB Host stack (`Pico-PIO-USB` + `Adafruit_TinyUSB`).
    *   Manages the low-level USB CDC communication with connected Meshtastic devices.
    *   Uses a thread-safe `RingBuffer` to bridge data between the two cores.

## Key Technologies

*   **Processor:** RP2040 (Dual-core ARM Cortex-M0+).
*   **Display:** ILI9341 TFT (SPI) with custom font rendering logic.
*   **USB Host:** PIO-USB implementation allowing the Pico to act as a USB host for Meshtastic radios.
*   **Protocols:** Meshtastic Protobuf (Nanopb) for node discovery, text messaging, and configuration.
*   **IME:** Custom Bopomofo (Zhuyin) engine with a binary-searched character pool.

## Key Files

*   `pico_keyboard_ime_terminal_usb_host.ino`: The main entry point containing UI logic, keyboard scanning, and protocol handling.
*   `picotype_data_optimized.h`: A large header file containing hardcoded font bitmaps and the IME character database.
*   `mesh.pb.h`, `channel.pb.h`, etc.: Nanopb-generated headers for the Meshtastic protocol.
*   `pb_encode.h` / `pb_decode.h`: Nanopb core library files.

## Development Conventions

*   **UI Updates:** The UI uses a `needs_redraw` flag to optimize rendering performance.
*   **Memory Management:** Font and IME data are stored in `PROGMEM` to fit within the Pico's flash memory.
*   **Error Handling:** Includes a watchdog-based recovery system that reboots the device if Core 1 (USB) hangs.

## Building and Running

### Prerequisites
*   Arduino IDE or PlatformIO.
*   Raspberry Pi Pico Arduino core (Earle Philhower version recommended).
*   Libraries: `Adafruit_ILI9341`, `Adafruit_GFX`, `Pico-PIO-USB`, `Adafruit_TinyUSB`.

### Build Commands (TODO)
*   Compile: `arduino-cli compile --fqbn rp2040:rp2040:pico pico_keyboard_ime_terminal_usb_host.ino`
*   Upload: `arduino-cli upload -p <port> --fqbn rp2040:rp2040:pico pico_keyboard_ime_terminal_usb_host.ino`
