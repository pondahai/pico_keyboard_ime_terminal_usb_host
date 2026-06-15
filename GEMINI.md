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

## Stability Fix Progress (2026-03-24)

### Step 1: Physical Layer & Interrupt Optimization (Applied)
*   **Clock Alignment:** System clock forced to **120MHz**. This provides an exact integer multiple for the USB 12Mbps bit timing, drastically reducing signal jitter inherent in software-defined USB.
*   **IRQ Prioritization:** Manually set `PIO0_IRQ_0` and `PIO1_IRQ_0` to priority **0** (highest). This ensures that heavy SPI activity (TFT rendering) on Core 0 does not block critical USB sampling on Core 1.
*   **Status:** Successfully implemented and committed to Git.

### Step 2: Connection Stability & Diagnostics (Current Status)
*   **Core 1 Boot Fix:** Removed `while (!Serial)` in `setup1()` that caused Core 1 to hang if no PC monitor was attached, which previously triggered a system-wide Watchdog reboot every 3 seconds.
*   **Diagnostic UI:** Added detailed connection status to the "My Info" tab:
    *   `USB: 掛載/未偵測`: Indicates physical USB layer status.
    *   `Proto: 已連線/未連線`: Indicates Meshtastic Protobuf handshake status.
*   **PIO Optimization:** Switched `configure_pio_usb` instance from `1` to `0` for better compatibility with existing libraries.
*   **Current Issue:** `[DBG] mounted=0` indicates no USB hardware detected by PIO-USB stack.

### Pending Tasks (Hardware Verification)
1.  [ ] **Wiring Check:** Verify USB D+ is on GP0 and D- is on GP1. Try swapping GP0 and GP1 wires.
2.  [ ] **Power Supply:** Ensure the Pico's VBUS (Pin 40) is providing 5V to the Meshtastic module's USB port.
3.  [ ] **Software Flag:** Confirm "Tools -> USB Stack: Adafruit TinyUSB" is selected in Arduino IDE during compilation.
4.  [ ] **Module Check:** Verify the Meshtastic module is powered on and its USB mode is standard Serial/CDC.

## USB Host 當機問題調查結論 (2026-06-15)

### 症狀
USB Host 一開始正常，**跑一段時間後整個卡死**，只能靠 watchdog 整機重啟恢復。

### 確定根因：Pico-PIO-USB library bug（非本專案程式碼問題）
對應上游 issue [#192](https://github.com/sekigon-gonnoc/Pico-PIO-USB/issues/192)：
*   `pio_usb_bus_receive_packet_and_handshake()` 在 timer 被打斷時（**別的中斷跑太久，或寫 flash**）沒有 timeout 保護，會**無限讀取資料、鎖死整個 core**。
*   內部 `idx` 為 `int16_t` 且只檢查上界，溢位變負數後**寫壞 buffer 前面的記憶體**。
*   本專案程式碼**沒有任何 flash 寫入**（`reboot_count` 欄位未落地），故觸發源是「別的中斷跑太久」——最可能是 Core 0 的 TFT SPI 重繪打斷 USB timer。

### 為什麼只能整機重啟
issue #192 證實：軟復原 `tuh_deinit/init`、`pio_usb_host_stop/restart` 會 panic 或 hang，**整機 watchdog 重啟是目前唯一可靠的恢復方式**。現有重啟設計是對的，需保留。

### 好消息：上游 main 分支已修
比對現行 [pio_usb.c](https://github.com/sekigon-gonnoc/Pico-PIO-USB/blob/main/src/pio_usb.c)，#192 的兩個缺陷都已補上：
*   已加 `get_time_us_32() - start > 7` µs timeout。
*   已加 `if (idx < rx_buf_len)` 邊界保護。
本專案疑似 pin 在還沒這些修正的舊版（無法在開發機定位到已安裝的 Pico-PIO-USB，需另行確認版本）。

### 行動方案（依優先序）
1.  **[治本] 升級 Pico-PIO-USB 到最新版**：Arduino IDE → 程式庫管理員 → 搜 `Pico PIO USB` → 更新。升級後實測「跑多久才會死」。若最新 release 仍缺修正，從 GitHub main 安裝。
2.  **[已套用] 移除 `loop1()` 的 `SerialHost.flush()`**：flush 在 USB core 上是阻塞呼叫，會打亂 PIO-USB 的 SOF 時序，是觸發卡死的成因之一。write() + 緊接的 task() 已足夠驅動 TX。
3.  **[保留] watchdog 整機重啟當安全網**：即使升級，timeout 為「每收 byte 重置」，timer 嚴重被打斷仍可能 stall。
4.  **[次要緩解] 降低 Core 0 對 USB timer 的干擾**：減少 `drawUI()` 重繪頻率／避免長時間 blocking SPI。
5.  **[備案] 若升級後仍當**：自行 vendor 一份 library，加更強的硬性 iteration 上限。

### 相關 issue
*   [#192](https://github.com/sekigon-gonnoc/Pico-PIO-USB/issues/192) — 無限迴圈 + 記憶體損壞（根因）
*   [#203](https://github.com/sekigon-gonnoc/Pico-PIO-USB/issues/203) — RP2040/RP2350 隨機 stall/crash（換 RP2350 無法解）
*   [#197](https://github.com/sekigon-gonnoc/Pico-PIO-USB/issues/197) — `pio_usb_host_frame()` 掛起
*   [#21](https://github.com/sekigon-gonnoc/Pico-PIO-USB/issues/21) — usb host stops working after some seconds
*   [pico-feedback #394](https://github.com/raspberrypi/pico-feedback/issues/394) — RP2040 矽晶級 DATA0/DATA1 PID 復原缺陷

## Building and Running

### Prerequisites
*   Arduino IDE or PlatformIO.
*   Raspberry Pi Pico Arduino core (Earle Philhower version recommended).
*   Libraries: `Adafruit_ILI9341`, `Adafruit_GFX`, `Pico-PIO-USB`, `Adafruit_TinyUSB`.

### Build Commands (TODO)
*   Compile: `arduino-cli compile --fqbn rp2040:rp2040:pico pico_keyboard_ime_terminal_usb_host.ino`
*   Upload: `arduino-cli upload -p <port> --fqbn rp2040:rp2040:pico pico_keyboard_ime_terminal_usb_host.ino`
