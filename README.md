# PicoType Keyboard

PicoType 是一台基於 Raspberry Pi Pico (RP2040) 的**獨立中文輸入與顯示裝置**，專為 Meshtastic mesh 網路而設計。內建注音輸入法、自製高效能字型渲染器，以及完整的分頁式 UI，可透過 USB Host 直接連接 Meshtastic 無線電模組進行收發訊息。

> 版本：v1.8.0（Multi-Tab UI Refactor）

## 功能特色

- **內建注音 IME**：以二分搜尋的字庫進行注音轉中文。
- **自製字型渲染器**：在 ILI9341 TFT 上繪製 UTF-8 文字與中文字形。
- **分頁式 UI**：聊天（Chat）、節點列表（Node List）、頻道列表（Channel List）、我的資訊（My Info）。
- **USB Host**：以 PIO-USB 讓 Pico 當 USB Host，透過 CDC 與 Meshtastic 模組通訊。
- **Meshtastic Protobuf**：使用 Nanopb 進行節點探索、文字訊息與設定。

## 硬體架構

採雙核心設計以兼顧反應速度與穩定性：

| 核心 | 職責 |
|------|------|
| **Core 0** | 主邏輯與 UI、8x8 鍵盤矩陣掃描、Protobuf 編解碼、IME 引擎、TFT 繪圖 |
| **Core 1** | 專責 PIO-USB Host stack（`Pico-PIO-USB` + `Adafruit_TinyUSB`），與 Core 0 透過 thread-safe RingBuffer 橋接資料 |

### 接腳配置

| 功能 | GPIO |
|------|------|
| TFT CS / RST / DC / BLK | 17 / 21 / 20 / 22 |
| 鍵盤 DATA_OUT / LATCH / CLOCK / DATA_IN | 15 / 14 / 26 / 27 |
| USB Host D+ / D- | GP0 / GP1（D- 自動為 D+ + 1） |

> 系統時脈鎖定 **120MHz**，為 USB 12Mbps 位元時序提供整數倍基準，降低軟體模擬 USB 的訊號抖動。

## 主要檔案

- `pico_keyboard_ime_terminal_usb_host.ino` — 主程式：UI 邏輯、鍵盤掃描、協定處理。
- `picotype_data_optimized.h` — 字型 bitmap 與 IME 字庫（存於 PROGMEM）。
- `*.pb.h` / `*.pb.c` — Nanopb 產生的 Meshtastic 協定檔。
- `pb_encode.*` / `pb_decode.*` — Nanopb 核心。
- `GEMINI.md` — 開發歷程與除錯紀錄（含 USB Host 當機問題的完整調查結論）。

## 環境需求

- Arduino IDE 或 PlatformIO
- Raspberry Pi Pico Arduino core（**Earle Philhower** 版本，建議）
- 函式庫：`Adafruit_ILI9341`、`Adafruit_GFX`、`Pico-PIO-USB`、`Adafruit_TinyUSB`
- Arduino IDE 設定：**Tools → USB Stack: Adafruit TinyUSB**（必要，否則 USB Host 無法運作）

## 編譯與燒錄

```bash
# 編譯
arduino-cli compile --fqbn rp2040:rp2040:pico pico_keyboard_ime_terminal_usb_host.ino

# 燒錄
arduino-cli upload -p <port> --fqbn rp2040:rp2040:pico pico_keyboard_ime_terminal_usb_host.ino
```

## 已知問題：USB Host 當機

USB Host 跑一段時間後可能整個卡死，由 watchdog 自動整機重啟恢復。根因為上游 **Pico-PIO-USB library bug [#192](https://github.com/sekigon-gonnoc/Pico-PIO-USB/issues/192)**（timer 被打斷時 `pio_usb_bus_receive_packet_and_handshake()` 進無限迴圈鎖死 core），**非本專案程式碼問題**。該缺陷在 library main 分支已修正。

**建議優先升級 Pico-PIO-USB 到最新版。** 完整調查與行動方案見 [`GEMINI.md`](GEMINI.md#usb-host-當機問題調查結論-2026-06-15)。

## 開發慣例

- **UI 更新**：以 `needs_redraw` flag 優化渲染效能。
- **記憶體管理**：字型與 IME 資料存於 PROGMEM 以符合 flash 容量。
- **錯誤處理**：watchdog 在 Core 1（USB）卡死時自動重啟裝置。

## 授權

**GPL-3.0**，見 [`LICENSE`](LICENSE)。

原因是本 repo 收錄了 16 個由 [meshtastic/protobufs](https://github.com/meshtastic/protobufs)
的 `.proto` 定義產生的檔案（`mesh.pb.*`、`channel.pb.*`、`config.pb.*` 等），
而該專案為 GPL-3.0，其產生檔是衍生著作。

> 姊妹專案 [pico_keyboard_ime_terminal](https://github.com/pondahai/pico_keyboard_ime_terminal)
> 不收錄這些檔案、要求使用者自行產生，故其自身程式碼採 MIT。

第三方成分的完整聲明見 [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)，摘要：

| 成分 | 上游 | 授權 |
| :--- | :--- | :--- |
| `*.pb.c` / `*.pb.h` | [meshtastic/protobufs](https://github.com/meshtastic/protobufs) | **GPL-3.0** |
| `pb_*.c` / `pb.h` | [nanopb](https://github.com/nanopb/nanopb) | Zlib，Copyright (c) 2011 Petteri Aimonen |
| 注音碼表（`zhuyin_*`） | [McBopomofo](https://github.com/openvanilla/McBopomofo) | MIT，Copyright (c) 2011-2026 Mengjuei Hsieh et al. |
| 中文字型（`font_*`） | Cubic 11 + Fusion Pixel 12px | SIL OFL 1.1 |

碼表使用的是 `BPMFBase.txt`（單字注音）與 `BPMFPunctuations.txt`（標點）。
帶有 libtabe（BSD）血統的多字詞庫 `BPMFMappings.txt` **未使用**。
