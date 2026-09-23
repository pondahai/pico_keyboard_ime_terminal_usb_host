# Third-Party Notices

本專案的 `picotype_data_optimized.h` 是**衍生資料**,不是手寫的程式碼。
它由 [ime-charset-font-bitmap](https://github.com/pondahai/ime-charset-font-bitmap)
的建置管線產生,內容同時包含第三方的**字型點陣資料**與**注音碼表資料**。

散布本專案(含編譯後的韌體)時,以下聲明必須隨著一起散布。

---

## 1. 注音輸入法碼表 — McBopomofo(小麥注音輸入法)

| | |
|---|---|
| 上游 | https://github.com/openvanilla/McBopomofo |
| 授權 | MIT License |
| 著作權 | Copyright (c) 2011-2026 Mengjuei Hsieh et al. |

### 實際使用的檔案

| 檔案 | 內容 | 有無額外上游出處 |
|---|---|---|
| `Source/Data/BPMFBase.txt` | 單字注音對應 | 無 —— 適用 McBopomofo 的 MIT |
| `Source/Data/BPMFPunctuations.txt` | 標點符號對應 | 無 —— 適用 McBopomofo 的 MIT |

### 明確未使用的檔案

`Source/Data/BPMFMappings.txt`(2–6 字的多字詞庫)**未被使用**。

這點值得寫明:依 McBopomofo 的 `Source/Data/README.md`,該檔案
*"Originally simplified from tsi.src of libtabe (BSD Licensed) with modifications"*
——它帶有 libtabe 的 BSD 血統。

本專案的輸入法是**單字候選**而非詞庫,只取用了 `BPMFBase.txt` 與
`BPMFPunctuations.txt`,因此不涉及 libtabe,亦無 BSD 條款需要遵循。

### 資料在本專案中的形式

原始碼表為文字格式,經建置管線轉換為「索引 + 資料池」的二進位結構,
以 `zhuyin_idx_raw_opt[]` 與 `zhuyin_pool_opt[]` 兩個陣列存在於
`picotype_data_optimized.h`。此為格式轉換,內容仍為 McBopomofo 的衍生著作。

### MIT License 全文

```
MIT License

Copyright (c) 2011-2026 Mengjuei Hsieh et al.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

---

## 2. 中文字型點陣資料

`picotype_data_optimized.h` 內的 `font_map_raw_opt[]` 與
`font_bitmap_data_1bpp[]` 是由下列字型光柵化產生的點陣資料。

| 字型 | 授權 | 角色 |
|---|---|---|
| [Cubic 11](https://github.com/ACh-K/Cubic-11)(俐方體十一號)v1.430 | SIL OFL 1.1 | 主字型 |
| [Fusion Pixel 12px zh_hant](https://github.com/TakWolf/fusion-pixel-font) | SIL OFL 1.1 | fallback |

依 SIL OFL 1.1,散布衍生資料時須隨附授權文字。完整的字型出處、版本固定理由
與保留字型名稱說明,見上游 repo 的 `fonts/LICENSES/ATTRIBUTION.md`。

> **保留字型名稱(Reserved Font Name)**:Cubic 保留「Cubic」「俐方體」。
> 本專案的產出不得以這些名稱對外呈現為字型名,故衍生檔案採中性命名
> (`picotype_*`)。

---

## 3. Meshtastic protobuf(本 repo 有收錄)

本 repo **收錄了** 16 個由 Meshtastic 的 `.proto` 定義產生的檔案:

```
channel.pb.c/h        config.pb.c/h      device_ui.pb.c/h   mesh.pb.c/h
module_config.pb.c/h  portnums.pb.c/h    telemetry.pb.c/h   xmodem.pb.c/h
```

| 元件 | 上游 | 授權 |
|---|---|---|
| Meshtastic `.proto` 定義 | [meshtastic/protobufs](https://github.com/meshtastic/protobufs) | **GPL-3.0** |

**這就是本專案採用 GPL-3.0 的原因。** 上列產生檔是 GPL-3.0 `.proto` 的衍生著作,
既然收錄在 repo 內一併散布,整個專案即須以 GPL-3.0 散布。

(姊妹專案 [pico_keyboard_ime_terminal](https://github.com/pondahai/pico_keyboard_ime_terminal)
不收錄這些檔案、要求使用者自行產生,故其自身程式碼採 MIT。)

## 4. Nanopb

`pb.h`、`pb_common.*`、`pb_encode.*`、`pb_decode.*` 取自
[nanopb/nanopb](https://github.com/nanopb/nanopb),
授權為 **Zlib License**,著作權 Copyright (c) 2011 Petteri Aimonen。

Zlib 與 GPL-3.0 相容,這些檔案在本專案中維持其原授權。

## 5. Pico-PIO-USB / Adafruit TinyUSB

USB Host 功能依賴 [Pico-PIO-USB](https://github.com/sekigon-gonnoc/Pico-PIO-USB)
與 [Adafruit TinyUSB](https://github.com/adafruit/Adafruit_TinyUSB_Arduino),
兩者均為 MIT,以函式庫形式於編譯期引入,本 repo 未收錄其原始碼。
