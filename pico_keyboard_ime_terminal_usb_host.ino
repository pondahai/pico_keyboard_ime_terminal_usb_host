// ==========================================================================
//
//   PicoType Keyboard - Standalone Chinese Input & Display Device
//   Version: 1.8.0 (Multi-Tab UI Refactor)
//   Milestone 3.1 Debugging Build (v2.2 - Tag ID Logging)
//
// ==========================================================================

// --- 包含所有必要的函式庫 ---
#include "picotype_data_optimized.h"
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <SPI.h>
#include <string.h>
#include <Arduino.h>

// --- USB Host (PIO-USB) ---
// 增大 CDC Host 緩衝區，Meshtastic 會同時送出大量 debug text + protobuf
#define CFG_TUH_CDC_RX_BUFSIZE 512
#define CFG_TUH_CDC_TX_BUFSIZE 512
#include "pio_usb.h"
#include "Adafruit_TinyUSB.h"
#include "hardware/watchdog.h"  // 用於 Core 1 死亡時自動重啟

// --- Protobuf 函式庫 ---
#include "pb.h"
#include "pb_encode.h"
#include "pb_decode.h"
#include "mesh.pb.h"
#include "channel.pb.h"

// ==========================================================================
// --- 硬體與 UI 配置 ---
// ==========================================================================
#define TFT_CS 17
#define TFT_RST 21
#define TFT_DC 20
#define TFT_BLK 22
const int DATA_OUT_PIN = 15;
const int LATCH_PIN = 14;
const int CLOCK_PIN = 26;
const int DATA_IN_PIN = 27;
// --- USB Host (PIO-USB) on GPIO 0 (D+) / GPIO 1 (D-) ---
#define HOST_PIN_DP 0   // USB D+ pin (D- = D+ + 1 automatically)

// USB Host 物件 (僅 Core 1 直接存取)
Adafruit_USBH_Host USBHost;
Adafruit_USBH_CDC  SerialHost;

// ==========================================================================
// --- 雙核安全 Ring Buffer ---
// Core 0 和 Core 1 之間的資料橋接，避免同時操作 TinyUSB 造成 crash
// TX: Core 0 寫入 → Core 1 讀出送到 USB
// RX: Core 1 從 USB 讀入 → Core 0 讀出處理
// ==========================================================================
#define RING_BUF_SIZE 1024

struct RingBuffer {
  volatile uint8_t buf[RING_BUF_SIZE];
  volatile uint16_t head;  // 寫入位置
  volatile uint16_t tail;  // 讀出位置

  void init() { head = 0; tail = 0; }

  uint16_t available() const {
    return (uint16_t)(head - tail);  // 利用 uint16_t 自然溢位
  }

  uint16_t free_space() const {
    return RING_BUF_SIZE - available();
  }

  bool write(uint8_t b) {
    if (free_space() == 0) return false;
    buf[head % RING_BUF_SIZE] = b;
    head++;
    return true;
  }

  size_t write(const uint8_t *data, size_t len) {
    size_t written = 0;
    while (written < len && free_space() > 0) {
      buf[head % RING_BUF_SIZE] = data[written++];
      head++;
    }
    return written;
  }

  int read() {
    if (available() == 0) return -1;
    uint8_t b = buf[tail % RING_BUF_SIZE];
    tail++;
    return b;
  }

  int peek() const {
    if (available() == 0) return -1;
    return buf[tail % RING_BUF_SIZE];
  }
};

RingBuffer usb_tx_ring;  // Core 0 寫入, Core 1 讀出
RingBuffer usb_rx_ring;  // Core 1 寫入, Core 0 讀出
volatile bool usb_cdc_mounted = false;  // Core 1 設定, Core 0 讀取
#define COLOR_BACKGROUND ILI9341_BLACK
#define COLOR_TEXT ILI9341_WHITE
#define COLOR_INPUT ILI9341_YELLOW
#define COLOR_CANDIDATE ILI9341_CYAN
#define COLOR_CYAN ILI9341_CYAN
#define COLOR_BORDER 0x39E7
#define COLOR_HINT ILI9341_GREEN
#define COLOR_OUTPUT ILI9341_WHITE
#define COLOR_STATUS_ONLINE ILI9341_GREEN
#define COLOR_STATUS_STALE ILI9341_YELLOW
#define COLOR_STATUS_OFFLINE 0x7BEF  // Dark Grey
const int FONT_HEIGHT = 16;
const int PADDING = 4;
const int MARGIN = 5;
const int LINE_SPACING = 4;
#define MAX_CHAR_BUFFER_SIZE 256
uint8_t g_char_bitmap_buffer[MAX_CHAR_BUFFER_SIZE];
#define PROTO_BUFFER_SIZE 512
uint8_t proto_tx_buffer[PROTO_BUFFER_SIZE];
uint8_t proto_rx_buffer[PROTO_BUFFER_SIZE];

// ==========================================================================
// --- 鍵盤按鍵定義與 Keymap ---
// ==========================================================================
#define KEY_NULL 0
#define KEY_SHIFT_R 202
#define KEY_ENTER 203
#define KEY_BACKSPACE 204
#define KEY_CTRL_L 205
#define KEY_TAB 206
#define KEY_ESC 207
#define KEY_CAPS_LOCK 208
#define KEY_UP_ARROW 209
#define KEY_DOWN_ARROW 210
#define KEY_LEFT_ARROW 211
#define KEY_RIGHT_ARROW 212
#define KEY_PGUP 213
#define KEY_PGDOWN 214
#define KEY_ALT 215

const char keymap_base[8][8] = {
  { '1', '3', '5', '7', '9', '-', KEY_TAB, KEY_BACKSPACE },
  { 'q', 'e', 't', 'u', 'o', '[', KEY_ESC, '\\' },
  { 'a', 'd', 'g', 'j', 'l', '\'', KEY_CTRL_L, KEY_CAPS_LOCK },
  { 'z', 'c', 'b', 'm', '.', KEY_SHIFT_R, KEY_DOWN_ARROW, KEY_NULL },
  { '2', '4', '6', '8', '0', '=', '`', KEY_NULL },
  { 'w', 'r', 'y', 'i', 'p', ']', KEY_UP_ARROW, KEY_PGUP },
  { 's', 'f', 'h', 'k', ';', KEY_ENTER, KEY_RIGHT_ARROW, KEY_PGDOWN },
  { 'x', 'v', 'n', ',', '/', ' ', KEY_LEFT_ARROW, KEY_ALT }
};
const char keymap_shifted[8][8] = {
  { '!', '#', '%', '&', '(', '_', '-', KEY_BACKSPACE },
  { 'Q', 'E', 'T', 'U', 'O', '{', KEY_ESC, '\\' },
  { 'A', 'D', 'G', 'J', 'L', '"', KEY_CTRL_L, KEY_CAPS_LOCK },
  { 'Z', 'C', 'B', 'M', '>', KEY_SHIFT_R, KEY_DOWN_ARROW, KEY_NULL },
  { '@', '$', '^', '*', ')', '+', '~', KEY_NULL },
  { 'W', 'R', 'Y', 'I', 'P', '}', KEY_UP_ARROW, KEY_PGUP },
  { 'S', 'F', 'H', 'K', ':', KEY_ENTER, KEY_RIGHT_ARROW, KEY_PGDOWN },
  { 'X', 'V', 'N', '<', '?', ' ', KEY_LEFT_ARROW, KEY_ALT }
};

// ==========================================================================
// --- 資料模型與 UI 狀態定義 ---
// ==========================================================================
// enum UIView { VIEW_MAIN_CHAT, VIEW_NODE_LIST, VIEW_NODE_DETAILS, VIEW_CHANNEL_LIST }; // [UI REFACTOR] Replaced by Tab system
struct MyInfo {
  uint32_t node_num;
  char pio_env[40];
  uint32_t reboot_count;
  // 新增欄位
  meshtastic_Config_LoRaConfig_RegionCode region;
  meshtastic_Config_LoRaConfig_ModemPreset modem_preset;
  uint16_t channel_num;
};
struct Node {
  uint32_t num;
  char long_name[40];
  char short_name[5];
  meshtastic_HardwareModel hw_model;
  uint32_t last_heard;
  float snr;
  bool has_position;
  int32_t latitude_i;
  int32_t longitude_i;
};
#define MAX_NODES 30
struct Channel {
  uint32_t index;
  meshtastic_ChannelSettings settings;
  meshtastic_Channel_Role role;
};
#define MAX_CHANNELS 8
Channel g_channels[MAX_CHANNELS];
int g_channel_count = 0;
int g_channel_list_selection = 0;
int g_selected_channel_index = 0; // 當前選擇的發送頻道 (預設為 0/Primary)

// ==========================================================================
// --- 類別定義 (無變動) ---
// ==========================================================================
class FontRenderer {
public:
  FontRenderer(Adafruit_ILI9341& display)
    : _tft(display) {}
  void drawString(int16_t x, int16_t y, const char* text, uint16_t color, uint16_t bg = COLOR_BACKGROUND) {
    if (bg != COLOR_BACKGROUND) _tft.fillRect(x, y, getStringWidth(text), FONT_HEIGHT + 2, bg);
    int16_t cursor_x = x;
    for (int i = 0; text[i] != '\0';) {
      uint32_t unicode;
      int char_len = decodeUtf8(text + i, &unicode);
      if (char_len == 0) {
        i++;
        continue;
      }
      if (unicode < 128) {
        _tft.setCursor(cursor_x, y + FONT_HEIGHT - 12);
        _tft.setTextColor(color);
        _tft.print((char)unicode);
        cursor_x += 6;
      } else {
        const FontMapRecord_Opt* record = findChar(unicode);
        if (record) {
          drawChar(cursor_x, y, color, record, g_char_bitmap_buffer);
          cursor_x += pgm_read_byte(&record->x_advance);
        } else {
          _tft.drawRect(cursor_x + 2, y + 2, FONT_HEIGHT - 4, FONT_HEIGHT - 4, ILI9341_MAGENTA);
          cursor_x += FONT_HEIGHT / 2;
        }
      }
      i += char_len;
    }
  }
  int getStringWidth(const char* text) {
    int16_t total_width = 0;
    for (int i = 0; text[i] != '\0';) {
      uint32_t unicode;
      int char_len = decodeUtf8(text + i, &unicode);
      if (char_len == 0) {
        i++;
        continue;
      }
      if (unicode < 128) total_width += 6;
      else {
        const FontMapRecord_Opt* record = findChar(unicode);
        if (record) total_width += pgm_read_byte(&record->x_advance);
        else total_width += FONT_HEIGHT / 2;
      }
      i += char_len;
    }
    return total_width;
  }
  int decodeUtf8(const char* s, uint32_t* unicode) {
    if ((*s & 0x80) == 0) {
      *unicode = *s;
      return 1;
    }
    if ((*s & 0xE0) == 0xC0) {
      *unicode = ((uint32_t)(s[0] & 0x1F) << 6) | (s[1] & 0x3F);
      return 2;
    }
    if ((*s & 0xF0) == 0xE0) {
      *unicode = ((uint32_t)(s[0] & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
      return 3;
    }
    return 0;
  }
private:
  Adafruit_ILI9341& _tft;
  const FontMapRecord_Opt* findChar(uint32_t unicode) {
    int low = 0;
    int high = font_map_count_opt - 1;
    while (low <= high) {
      int mid = low + (high - low) / 2;
      uint32_t mid_unicode = pgm_read_dword(&font_map_opt[mid].unicode);
      if (mid_unicode == unicode) return &font_map_opt[mid];
      if (mid_unicode < unicode) low = mid + 1;
      else high = mid - 1;
    }
    return nullptr;
  }
  void drawChar(int16_t x, int16_t y, uint16_t color, const FontMapRecord_Opt* record, uint8_t* bitmap_buffer) {
    uint8_t w = pgm_read_byte(&record->width), h = pgm_read_byte(&record->height);
    int8_t x_off = pgm_read_byte(&record->x_offset), y_off = pgm_read_byte(&record->y_offset);
    uint32_t offset = pgm_read_dword(&record->offset);
    if (w == 0 || h == 0) return;
    size_t buffer_size = (size_t)w * h;
    if (buffer_size > MAX_CHAR_BUFFER_SIZE) return;
    memcpy_P(bitmap_buffer, font_bitmap_data_opt + offset, buffer_size);
    int16_t draw_x = x + x_off, draw_y = y + y_off;
    _tft.startWrite();
    for (int16_t j = 0; j < h; j++)
      for (int16_t i = 0; i < w; i++)
        if (bitmap_buffer[j * w + i] > 128) _tft.writePixel(draw_x + i, draw_y + j, color);
    _tft.endWrite();
  }
};
class ImeEngine {
public:
  ImeEngine() {}
  bool query(const char* inputCode, char* resultBuffer, size_t bufferSize) {
    char bopomofoBuffer[32] = { 0 };
    if (!mapKeyToBopomofo(inputCode, bopomofoBuffer, sizeof(bopomofoBuffer))) return false;
    char queryKey[12] = { 0 };
    formatBopomofoToKey(bopomofoBuffer, queryKey, sizeof(queryKey));
    const ImeIndexRecord_Opt* resultRecord = binarySearch(queryKey);
    if (resultRecord) {
      uint16_t offset = pgm_read_word(&resultRecord->data_offset);
      uint16_t length = pgm_read_word(&resultRecord->data_len);
      if (length >= bufferSize) length = bufferSize - 1;
      memcpy_P(resultBuffer, zhuyin_pool_opt + offset, length);
      resultBuffer[length] = '\0';
      return true;
    }
    return false;
  }
  static bool mapKeyToBopomofo(const char* input, char* output, size_t outputSize) {
    output[0] = '\0';
    for (int i = 0; input[i] != '\0'; ++i) {
      char tempBuf[4] = { 0 };
      if (keyToBopomofoChar(input[i], tempBuf)) {
        if (strlen(output) + strlen(tempBuf) < outputSize) strcat(output, tempBuf);
        else return false;
      }
    }
    return true;
  }
private:
  static int keyToBopomofoChar(char key, char* buf) {
    char lower_key = tolower(key); // <-- 新增此行，將傳入的 key 轉為小寫
    if (lower_key == '1') {
      strcpy(buf, "ㄅ");
      return 3;
    }
    if (lower_key == 'q') {
      strcpy(buf, "ㄆ");
      return 3;
    }
    if (lower_key == 'a') {
      strcpy(buf, "ㄇ");
      return 3;
    }
    if (lower_key == 'z') {
      strcpy(buf, "ㄈ");
      return 3;
    }
    if (lower_key == '2') {
      strcpy(buf, "ㄉ");
      return 3;
    }
    if (lower_key == 'w') {
      strcpy(buf, "ㄊ");
      return 3;
    }
    if (lower_key == 's') {
      strcpy(buf, "ㄋ");
      return 3;
    }
    if (lower_key == 'x') {
      strcpy(buf, "ㄌ");
      return 3;
    }
    if (lower_key == 'e') {
      strcpy(buf, "ㄍ");
      return 3;
    }
    if (lower_key == 'd') {
      strcpy(buf, "ㄎ");
      return 3;
    }
    if (lower_key == 'c') {
      strcpy(buf, "ㄏ");
      return 3;
    }
    if (lower_key == 'r') {
      strcpy(buf, "ㄐ");
      return 3;
    }
    if (lower_key == 'f') {
      strcpy(buf, "ㄑ");
      return 3;
    }
    if (lower_key == 'v') {
      strcpy(buf, "ㄒ");
      return 3;
    }
    if (lower_key == '5') {
      strcpy(buf, "ㄓ");
      return 3;
    }
    if (lower_key == 't') {
      strcpy(buf, "ㄔ");
      return 3;
    }
    if (lower_key == 'g') {
      strcpy(buf, "ㄕ");
      return 3;
    }
    if (lower_key == 'b') {
      strcpy(buf, "ㄖ");
      return 3;
    }
    if (lower_key == 'y') {
      strcpy(buf, "ㄗ");
      return 3;
    }
    if (lower_key == 'h') {
      strcpy(buf, "ㄘ");
      return 3;
    }
    if (lower_key == 'n') {
      strcpy(buf, "ㄙ");
      return 3;
    }
    if (lower_key == 'u') {
      strcpy(buf, "ㄧ");
      return 3;
    }
    if (lower_key == 'j') {
      strcpy(buf, "ㄨ");
      return 3;
    }
    if (lower_key == 'm') {
      strcpy(buf, "ㄩ");
      return 3;
    }
    if (lower_key == '8') {
      strcpy(buf, "ㄚ");
      return 3;
    }
    if (lower_key == 'i') {
      strcpy(buf, "ㄛ");
      return 3;
    }
    if (lower_key == 'k') {
      strcpy(buf, "ㄜ");
      return 3;
    }
    if (lower_key == ',') {
      strcpy(buf, "ㄝ");
      return 3;
    }
    if (lower_key == '9') {
      strcpy(buf, "ㄞ");
      return 3;
    }
    if (lower_key == 'o') {
      strcpy(buf, "ㄟ");
      return 3;
    }
    if (lower_key == 'l') {
      strcpy(buf, "ㄠ");
      return 3;
    }
    if (lower_key == '.') {
      strcpy(buf, "ㄡ");
      return 3;
    }
    if (lower_key == '0') {
      strcpy(buf, "ㄢ");
      return 3;
    }
    if (lower_key == 'p') {
      strcpy(buf, "ㄣ");
      return 3;
    }
    if (lower_key == ';') {
      strcpy(buf, "ㄤ");
      return 3;
    }
    if (lower_key == '/') {
      strcpy(buf, "ㄥ");
      return 3;
    }
    if (lower_key == '-') {
      strcpy(buf, "ㄦ");
      return 3;
    }
    if (lower_key == '6') {
      strcpy(buf, "ˊ");
      return 3;
    }
    if (lower_key == '3') {
      strcpy(buf, "ˇ");
      return 3;
    }
    if (lower_key == '4') {
      strcpy(buf, "ˋ");
      return 3;
    }
    if (lower_key == '7') {
      strcpy(buf, "˙");
      return 3;
    }
    return 0;
  }
  void formatBopomofoToKey(const char* bopomofo, char* key, size_t keySize) {
    char* p_key = key;
    const char* p_bopo = bopomofo;
    bool hasTone = false;
    while (*p_bopo != '\0' && (p_key - key) < (keySize - 1)) {
      if (memcmp(p_bopo, "ˊ", 3) == 0) {
        *p_key++ = '2';
        p_bopo += 3;
        hasTone = true;
      } else if (memcmp(p_bopo, "ˇ", 3) == 0) {
        *p_key++ = '3';
        p_bopo += 3;
        hasTone = true;
      } else if (memcmp(p_bopo, "ˋ", 3) == 0) {
        *p_key++ = '4';
        p_bopo += 3;
        hasTone = true;
      } else if (memcmp(p_bopo, "˙", 3) == 0) {
        *p_key++ = '5';
        p_bopo += 3;
        hasTone = true;
      } else {
        memcpy(p_key, p_bopo, 3);
        p_key += 3;
        p_bopo += 3;
      }
    }
    if (!hasTone && (p_key - key) < (keySize - 1)) { *p_key++ = '1'; }
    *p_key = '\0';
  }
  const ImeIndexRecord_Opt* binarySearch(const char* key) {
    int low = 0, high = zhuyin_idx_count_opt - 1;
    while (low <= high) {
      int mid = low + (high - low) / 2;
      char key_from_pool[12] = { 0 };
      uint16_t key_offset = pgm_read_word(&zhuyin_idx_opt[mid].key_offset);
      uint8_t key_len = pgm_read_byte(&zhuyin_idx_opt[mid].key_len);
      memcpy_P(key_from_pool, zhuyin_pool_opt + key_offset, key_len);
      int cmp = strcmp(key, key_from_pool);
      if (cmp == 0) return &zhuyin_idx_opt[mid];
      else if (cmp < 0) high = mid - 1;
      else low = mid + 1;
    }
    return nullptr;
  }
};

// ==========================================================================
// --- 全域物件與變數 ---
// ==========================================================================
Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC, TFT_RST);
MyInfo g_my_info;
Node g_node_db[MAX_NODES];
int g_node_count = 0;
int g_node_list_selection = 0;
ImeEngine ime;
FontRenderer* renderer = nullptr;
enum State { STATE_INPUT,
             STATE_SELECTION };
State currentState = STATE_INPUT;
String inputBuffer_keys = "", inputBuffer_bopomofo = "", candidate_string = "";
String editor_content = "--- PROTOBUF MODE ---\n";
int candidate_page = 0;
const int CANDIDATES_PER_PAGE = 9;
bool needs_redraw = true;
bool keyState[8][8], lastKeyState[8][8];
bool isShiftPressed = false, isAltPressed = false;
enum InputMode { MODE_LOWERCASE, MODE_UPPERCASE, MODE_BOPOMOFO };
InputMode g_input_mode = MODE_BOPOMOFO; // 預設開機為注音模式

bool isImeEnabled = true, isTerminalMode = false, isProtoMode = true;
String g_old_input_display = "", g_old_candidate_display = "", g_old_mode_hint = "";
bool g_device_responded = false;
bool g_got_my_info = false;
unsigned long g_last_handshake_time = 0, g_last_heartbeat_time = 0;
enum ConnectionPhase {
  PHASE_GETTING_ID,
  PHASE_POPULATING_NODELIST,
  PHASE_SYNCING_SELF_INFO,
  PHASE_CONNECTED
};
ConnectionPhase g_connection_phase = PHASE_GETTING_ID;
const unsigned long HANDSHAKE_RETRY_INTERVAL = 5000, HEARTBEAT_INTERVAL = 15000;
uint32_t g_target_node_num = 0xFFFFFFFF;
String g_target_node_name = "廣播";
uint32_t g_last_sent_packet_id = 0;
String g_last_sent_line = "";
uint32_t g_current_unix_time = 0;

// --- [FEATURE] 名稱編輯機制 (方向鍵選擇版) ---
enum MyInfoTabState { MY_INFO_BROWSING, MY_INFO_EDITING };
MyInfoTabState g_my_info_tab_state = MY_INFO_BROWSING; // 目前分頁的狀態 (瀏覽或編輯)
int g_my_info_selection = 0;                          // 在瀏覽模式下，目前選擇的項目 (0=長名稱, 1=短名稱)
const int MY_INFO_EDITABLE_FIELDS = 2;                // 可編輯的欄位總數
String g_edit_buffer = "";
String g_my_long_name = "PicoType UI";
String g_my_short_name = "Pico";
// --- [FEATURE] 結束 ---

// --- [UI REFACTOR] 分頁系統定義 ---
typedef void (*DrawFunction)();
typedef void (*InputHandler)(char);

struct Tab {
  const char* name;
  DrawFunction draw_func;
  InputHandler input_handler;
};

int g_current_tab_index = 0;
bool g_is_node_detail_view_active = false;  // 用於節點分頁內的子視圖狀態

const int TAB_BAR_HEIGHT = FONT_HEIGHT + PADDING * 2;

// 函式原型宣告
void drawMainChatContent();
void drawNodeListContent();
void drawChannelListContent();
void drawMyInfoContent(); // <-- 新增
void handleMainChatInput(char keyCode);
void handleNodeListInput(char keyCode);
void handleChannelListInput(char keyCode);
void handleMyInfoInput(char keyCode); // <-- 新增

// 分頁陣列
Tab g_tabs[] = {
  { "聊天", drawMainChatContent, handleMainChatInput },
  { "節點", drawNodeListContent, handleNodeListInput },
  { "頻道", drawChannelListContent, handleChannelListInput },
  { "本機", drawMyInfoContent, handleMyInfoInput } // <-- 新增此行
};
const int g_tab_count = sizeof(g_tabs) / sizeof(Tab);
// --- [UI REFACTOR] 結束 ---


// ==========================================================================
// --- 輔助函式 (大部分無變動) ---
// ==========================================================================
void addMyInfoToNodeDB();
void updateNodeInDB(const meshtastic_NodeInfo& node_info);
// 記憶體保護機制：修剪過長的編輯器內容
void trimEditorContent() {
  const int MAX_CONTENT_LENGTH = 2000;
  if (editor_content.length() > MAX_CONTENT_LENGTH) {
    int cutIndex = editor_content.indexOf('\n', editor_content.length() - MAX_CONTENT_LENGTH);
    if (cutIndex != -1) {
      editor_content = editor_content.substring(cutIndex + 1);
    } else {
      editor_content = editor_content.substring(editor_content.length() - MAX_CONTENT_LENGTH);
    }
  }
}
int getUtf8CharLength(const char* s) {
  if ((*s & 0x80) == 0) return 1;
  if ((*s & 0xE0) == 0xC0) return 2;
  if ((*s & 0xF0) == 0xE0) return 3;
  if ((*s & 0xF8) == 0xF0) return 4;
  return 1;
}
int countUtf8Chars(String& str) {
  int count = 0;
  for (int i = 0; i < str.length();) {
    i += getUtf8CharLength(str.c_str() + i);
    count++;
  }
  return count;
}
int getNthUtf8CharByteIndex(String& str, int n) {
  if (n < 0) return -1;
  if (n == 0) return 0;
  int current_char_count = 0;
  for (int i = 0; i < str.length();) {
    if (current_char_count == n) return i;
    i += getUtf8CharLength(str.c_str() + i);
    current_char_count++;
  }
  if (current_char_count == n) return str.length();
  return -1;
}

String getChannelRoleString(meshtastic_Channel_Role role) {
  switch (role) {
    case meshtastic_Channel_Role_DISABLED: return "停用";
    case meshtastic_Channel_Role_PRIMARY: return "主頻道";
    case meshtastic_Channel_Role_SECONDARY: return "副頻道";
    default: return "未知";
  }
}

String getRegionString(meshtastic_Config_LoRaConfig_RegionCode region) {
    switch (region) {
        case meshtastic_Config_LoRaConfig_RegionCode_US: return "US";
        case meshtastic_Config_LoRaConfig_RegionCode_EU_433: return "EU433";
        case meshtastic_Config_LoRaConfig_RegionCode_EU_868: return "EU868";
        case meshtastic_Config_LoRaConfig_RegionCode_CN: return "CN";
        case meshtastic_Config_LoRaConfig_RegionCode_JP: return "JP";
        case meshtastic_Config_LoRaConfig_RegionCode_ANZ: return "ANZ";
        case meshtastic_Config_LoRaConfig_RegionCode_KR: return "KR";
        case meshtastic_Config_LoRaConfig_RegionCode_TW: return "TW";
        case meshtastic_Config_LoRaConfig_RegionCode_RU: return "RU";
        case meshtastic_Config_LoRaConfig_RegionCode_IN: return "IN";
        case meshtastic_Config_LoRaConfig_RegionCode_NZ_865: return "NZ865";
        case meshtastic_Config_LoRaConfig_RegionCode_TH: return "TH";
        case meshtastic_Config_LoRaConfig_RegionCode_LORA_24: return "LoRa24";
        case meshtastic_Config_LoRaConfig_RegionCode_UA_433: return "UA433";
        case meshtastic_Config_LoRaConfig_RegionCode_UA_868: return "UA868";
        case meshtastic_Config_LoRaConfig_RegionCode_MY_433: return "MY433";
        case meshtastic_Config_LoRaConfig_RegionCode_MY_919: return "MY919";
        case meshtastic_Config_LoRaConfig_RegionCode_SG_923: return "SG923";
        case meshtastic_Config_LoRaConfig_RegionCode_PH_433: return "PH433";
        case meshtastic_Config_LoRaConfig_RegionCode_PH_868: return "PH868";
        case meshtastic_Config_LoRaConfig_RegionCode_PH_915: return "PH915";
        case meshtastic_Config_LoRaConfig_RegionCode_UNSET: return "Unset";
        default: return "Unknown";
    }
}

String getModemPresetString(meshtastic_Config_LoRaConfig_ModemPreset preset) {
    switch (preset) {
        case meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST: return "LongFast";
        case meshtastic_Config_LoRaConfig_ModemPreset_LONG_SLOW: return "LongSlow";
        case meshtastic_Config_LoRaConfig_ModemPreset_VERY_LONG_SLOW: return "VLongSlow"; // Deprecated
        case meshtastic_Config_LoRaConfig_ModemPreset_MEDIUM_SLOW: return "MedSlow";
        case meshtastic_Config_LoRaConfig_ModemPreset_MEDIUM_FAST: return "MedFast";
        case meshtastic_Config_LoRaConfig_ModemPreset_SHORT_SLOW: return "ShortSlow";
        case meshtastic_Config_LoRaConfig_ModemPreset_SHORT_FAST: return "ShortFast";
        case meshtastic_Config_LoRaConfig_ModemPreset_LONG_MODERATE: return "LongMod";
        case meshtastic_Config_LoRaConfig_ModemPreset_SHORT_TURBO: return "Turbo";
        default: return "Unknown";
    }
}
byte myShiftIn(uint8_t dataPin, uint8_t clockPin) {
  byte data = 0;
  for (int i = 0; i < 8; i++) {
    if (digitalRead(dataPin)) { data |= (1 << i); }
    digitalWrite(clockPin, HIGH);
    delayMicroseconds(1);
    digitalWrite(clockPin, LOW);
  }
  return data;
}
void updateKeyboardAndLeds_TwoPulse() {
  for (int row = 0; row < 8; row++) {
    byte rowScanData = (1 << row);
    byte ledData = (isImeEnabled ? 1 : 0) | ((isTerminalMode || isProtoMode) ? 2 : 0);
    digitalWrite(LATCH_PIN, LOW);
    shiftOut(DATA_OUT_PIN, CLOCK_PIN, MSBFIRST, ledData);
    shiftOut(DATA_OUT_PIN, CLOCK_PIN, MSBFIRST, rowScanData);
    digitalWrite(LATCH_PIN, HIGH);
    delayMicroseconds(5);
    digitalWrite(LATCH_PIN, LOW);
    delayMicroseconds(1);
    digitalWrite(LATCH_PIN, HIGH);
    byte colData = myShiftIn(DATA_IN_PIN, CLOCK_PIN);
    for (int col = 0; col < 8; col++) keyState[row][7 - col] = (colData & (1 << col));
  }
}
void print_hex_buffer(const uint8_t* buffer, size_t len, const char* prefix = "") {
  Serial.print(prefix);
  for (size_t i = 0; i < len; i++) {
    if (buffer[i] < 0x10) Serial.print("0");
    Serial.print(buffer[i], HEX);
    Serial.print(" ");
  }
  Serial.println();
}
const char* getHwModelString(meshtastic_HardwareModel model) {
  switch (model) {
    case meshtastic_HardwareModel_TLORA_V2: return "T-LoRa V2";
    case meshtastic_HardwareModel_TBEAM: return "T-Beam";
    case meshtastic_HardwareModel_HELTEC_V2_1: return "Heltec V2.1";
    case meshtastic_HardwareModel_HELTEC_V3: return "Heltec V3";
    case meshtastic_HardwareModel_RAK4631: return "RAK4631";
    case meshtastic_HardwareModel_T_ECHO: return "T-Echo";
    case meshtastic_HardwareModel_LILYGO_TBEAM_S3_CORE: return "T-Beam S3";
    case meshtastic_HardwareModel_PRIVATE_HW: return "PicoType";
    case meshtastic_HardwareModel_UNSET:
    default: return "Unknown";
  }
}

// 請用這個版本完整替換舊的 addMyInfoToNodeDB
void addMyInfoToNodeDB() {
  if (g_my_info.node_num == 0) return;

  // 檢查節點是否已存在
  for (int i = 0; i < g_node_count; i++) {
    if (g_node_db[i].num == g_my_info.node_num) {
      return; // 已存在，直接返回
    }
  }

  // 如果不存在，則新增一個
  if (g_node_count < MAX_NODES) {
    Node& selfNode = g_node_db[g_node_count];
    selfNode.num = g_my_info.node_num;

    // ====================== 關鍵修改 ======================
    // 不再使用硬編碼的 "PicoType UI (Self)"
    // 而是使用 g_my_long_name 的當前值。
    // 如果是第一次開機，它會是 "PicoType UI"，
    // 如果是 Alt-R 重設，它會是已經學習到的正確名稱！
    if (g_my_long_name.length() > 0) {
        strncpy(selfNode.long_name, g_my_long_name.c_str(), sizeof(selfNode.long_name) - 1);
    } else {
        strcpy(selfNode.long_name, "PicoType UI (Self)"); // 僅作為最終後備
    }
    // =======================================================
    
    strncpy(selfNode.short_name, g_my_short_name.c_str(), sizeof(selfNode.short_name) - 1);
    selfNode.hw_model = meshtastic_HardwareModel_PRIVATE_HW;
    selfNode.last_heard = g_current_unix_time > 0 ? g_current_unix_time : 1;
    selfNode.snr = 0;
    selfNode.has_position = false;
    g_node_count++;
    needs_redraw = true;
  }
}

String getNodeDisplayName(uint32_t node_num) {
  // 首先，在我們的節點資料庫中尋找這個 ID
  for (int i = 0; i < g_node_count; i++) {
    if (g_node_db[i].num == node_num) {
      // 找到了節點，開始判斷要顯示哪個名稱
      // 優先使用長名稱，如果它存在且不是 "N/A"
      if (strlen(g_node_db[i].long_name) > 0 && strcmp(g_node_db[i].long_name, "N/A") != 0) {
        return String(g_node_db[i].long_name);
      }
      // 其次使用短名稱，只要它不是我們自己產生的 ? 開頭的臨時名
      if (strlen(g_node_db[i].short_name) > 0 && g_node_db[i].short_name[0] != '?') {
        return String(g_node_db[i].short_name);
      }
      // 如果名稱都無效，則跳出迴圈，使用下面的後備邏輯
      break; 
    }
  }

  // 如果找不到節點，或者節點沒有有效名稱，則回傳一個統一格式的縮寫 ID
  char fallback_name[10];
  sprintf(fallback_name, "!%04lX", node_num & 0xFFFF);
  return String(fallback_name);
}

// ==========================================================================
// --- 資料庫與 Protobuf 處理 (無變動) ---
// ==========================================================================
// 請用這個最終版本完整替換現有的 updateNodeInDB 函式
void updateNodeInDB(const meshtastic_NodeInfo& node_info) {
  if (node_info.num == 0) return; // 忽略無效的節點ID

  int existing_node_index = -1;
  for (int i = 0; i < g_node_count; i++) {
    if (g_node_db[i].num == node_info.num) {
      existing_node_index = i;
      break;
    }
  }

  // --- 判斷是否為本機節點的更新 ---
  // 只有當我們已經知道自己是誰，並且收到的封包ID與自己相符時
  bool is_my_info_update = (g_my_info.node_num != 0 && node_info.num == g_my_info.node_num);

  if (existing_node_index != -1) {
    // --- 更新現有節點 ---
    Node& node = g_node_db[existing_node_index];

    if (node_info.has_user) {
      // -- 更新長名稱 --
      // 只有當網路名稱非空，且與目前資料庫中的名稱不同時才更新
      if (strlen(node_info.user.long_name) > 0 && strcmp(node.long_name, node_info.user.long_name) != 0) {
        strncpy(node.long_name, node_info.user.long_name, sizeof(node.long_name) - 1);
      }
      // -- 更新短名稱 -- (同樣邏輯)
      if (strlen(node_info.user.short_name) > 0 && strcmp(node.short_name, node_info.user.short_name) != 0) {
        strncpy(node.short_name, node_info.user.short_name, sizeof(node.short_name) - 1);
      }
    }
    
    // 如果這次更新的是本機節點，則額外同步 g_my_long_name
    if (is_my_info_update && node_info.has_user) {
        // ====================== 關鍵防禦邏輯 ======================
        // 只有當網路名稱非空，且與 g_my_long_name 不同時才更新
        if (strlen(node_info.user.long_name) > 0 && g_my_long_name != node_info.user.long_name) {
            g_my_long_name = node_info.user.long_name;
        }
        if (strlen(node_info.user.short_name) > 0 && g_my_short_name != node_info.user.short_name) {
            g_my_short_name = node_info.user.short_name;
        }
        // =========================================================
    }

    // 更新其他非名稱資訊
    node.last_heard = node_info.last_heard;
    node.snr = node_info.snr;
    node.hw_model = node_info.user.hw_model;
    if (node_info.has_position) {
      node.has_position = true;
      node.latitude_i = node_info.position.latitude_i;
      node.longitude_i = node_info.position.longitude_i;
    }

  } else {
    // --- 新增節點 ---
    if (g_node_count < MAX_NODES) {
      Node& newNode = g_node_db[g_node_count];
      newNode.num = node_info.num;

      if (node_info.has_user) {
        strncpy(newNode.long_name, node_info.user.long_name, sizeof(newNode.long_name) - 1);
        strncpy(newNode.short_name, node_info.user.short_name, sizeof(newNode.short_name) - 1);
      } else {
        strcpy(newNode.long_name, "N/A");
        char tempName[10];
        sprintf(tempName, "?%04lX", node_info.num & 0xFFFF);
        strcpy(newNode.short_name, tempName);
      }
      
      // 如果這個新加入的節點剛好是我們自己，也需要同步 g_my_long_name
      if (is_my_info_update && node_info.has_user) {
          if (strlen(node_info.user.long_name) > 0) {
              g_my_long_name = node_info.user.long_name;
          }
          if (strlen(node_info.user.short_name) > 0) {
              g_my_short_name = node_info.user.short_name;
          }
      }
      
      // 更新其他非名稱資訊
      newNode.last_heard = node_info.last_heard;
      newNode.snr = node_info.snr;
      newNode.hw_model = node_info.user.hw_model;
      if (node_info.has_position) {
        newNode.has_position = true;
        newNode.latitude_i = node_info.position.latitude_i;
        newNode.longitude_i = node_info.position.longitude_i;
      } else {
        newNode.has_position = false;
      }
      g_node_count++;
    }
  }

  checkAndFinalizeConnection();

  if (g_connection_phase != PHASE_CONNECTED) {
    // 檢查階段二的條件
    if (g_connection_phase == PHASE_POPULATING_NODELIST && g_node_count > 0) {
      Serial.println("[CONNECTION] Phase 2/3 Complete: Node list is populated.");
      g_connection_phase = PHASE_SYNCING_SELF_INFO; // << 進入下一階段
    }

    // 檢查階段三的條件
    bool longNameOK = (g_my_long_name != "PicoType UI" && g_my_long_name != "");
    bool shortNameOK = (g_my_short_name != "Pico" && g_my_short_name != "");
    if (g_connection_phase == PHASE_SYNCING_SELF_INFO && longNameOK && shortNameOK) {
      Serial.println("[CONNECTION] Phase 3/3 Complete: Self info synced!");
      g_connection_phase = PHASE_CONNECTED; // << 所有階段完成！
      
      g_device_responded = true;
      
      // 執行最終的連線成功動作
      editor_content = "[Sync Complete!]\n";
      needs_redraw = true;
      g_last_heartbeat_time = millis();
    }
  }  
}


// ==========================================================================
// --- USB CDC Serial 抽象層 ---
// Core 0 透過 Ring Buffer 與 Core 1 溝通，絕不直接碰 SerialHost
// ==========================================================================

inline bool meshSerial_available() {
  return usb_cdc_mounted && usb_rx_ring.available() > 0;
}

inline int meshSerial_read() {
  return usb_rx_ring.read();
}

inline size_t meshSerial_write(const uint8_t *buf, size_t len) {
  if (!usb_cdc_mounted) return 0;
  return usb_tx_ring.write(buf, len);
}

inline size_t meshSerial_write(uint8_t b) {
  if (!usb_cdc_mounted) return 0;
  return usb_tx_ring.write(b) ? 1 : 0;
}

inline size_t meshSerial_print(const String &s) {
  if (!usb_cdc_mounted) return 0;
  return usb_tx_ring.write((const uint8_t*)s.c_str(), s.length());
}

// ==========================================================================
// --- USB CDC Host Callbacks ---
// 這些 callback 在 Core 1 的 context 中被 TinyUSB 呼叫
// ==========================================================================
void tuh_cdc_mount_cb(uint8_t idx) {
  SerialHost.mount(idx);
  SerialHost.setBaudrate(115200);
  SerialHost.setDtrRts(true, false);
  usb_tx_ring.init();
  usb_rx_ring.init();
  usb_cdc_mounted = true;
  Serial.println("[USB] Meshtastic CDC device mounted.");
}

void tuh_cdc_umount_cb(uint8_t idx) {
  usb_cdc_mounted = false;
  SerialHost.umount(idx);
  Serial.println("[USB] Meshtastic CDC device disconnected.");
}

// ==========================================================================
// --- Protobuf 通訊函式 ---
// ==========================================================================
void sendProtoPacket(const meshtastic_ToRadio& payload) {
  pb_ostream_t stream = pb_ostream_from_buffer(proto_tx_buffer, sizeof(proto_tx_buffer));
  if (!pb_encode(&stream, meshtastic_ToRadio_fields, &payload)) {
    Serial.println("[TX-ERROR] Protobuf encoding failed!");
    return;
  }
  size_t message_len = stream.bytes_written;
  if (!usb_cdc_mounted) return;

  // 組合成一個完整封包寫入 TX ring buffer
  uint8_t header[4] = {
    0x94, 0xc3,
    (uint8_t)((message_len >> 8) & 0xFF),
    (uint8_t)(message_len & 0xFF)
  };
  usb_tx_ring.write(header, 4);
  usb_tx_ring.write(proto_tx_buffer, message_len);

  Serial.print("[TX] Sent packet, len=");
  Serial.println(message_len);
}
void initiateConnectionAttempt() {
  Serial.println("\n[CONNECTION] Initiating new connection attempt...");
  // 這是真正的「初始化」，只應執行一次
  g_node_count = 0;
  g_channel_count = 0;
  g_device_responded = false;
  g_got_my_info = false;
  g_my_info.region = meshtastic_Config_LoRaConfig_RegionCode_UNSET; // Reset region check

  g_connection_phase = PHASE_GETTING_ID;

  // 初始化後，立刻發送第一次握手請求
  sendProtoHandshake();
}
void sendWantConfigRequest() {
  editor_content += "[Connecting (1/3)...]\n"; // 提示目前階段
  needs_redraw = true;
  
  meshtastic_ToRadio payload = meshtastic_ToRadio_init_default;
  payload.which_payload_variant = meshtastic_ToRadio_want_config_id_tag;
  payload.want_config_id = 0x1A2B3C4D;
  sendProtoPacket(payload);
  Serial.println("[TX] Sent: WantConfigRequest");
  g_last_handshake_time = millis();
}



void sendProtoHandshake() {
  // 不再清空任何東西，不再顯示 Connecting
  editor_content += "[Connecting...]\n"; // 這一行可以保留，以便看到重試
  needs_redraw = true;
  
  meshtastic_ToRadio payload = meshtastic_ToRadio_init_default;
  payload.which_payload_variant = meshtastic_ToRadio_want_config_id_tag;
  payload.want_config_id = 0x1A2B3C4D;
  sendProtoPacket(payload);
  Serial.println("[TX] Handshake sent (Retry).");
  g_last_handshake_time = millis();
}

void checkAndFinalizeConnection() {
  // 如果已經顯示過 Connected!，就直接返回，避免重複執行
  if (g_device_responded) return;

  // --- 定義成功的四大條件 ---
  // 條件 1: 必須已經收到過 my_info 封包
  bool condition1_gotMyInfo = g_got_my_info;

  // 條件 2: 節點列表必須至少有一個節點 (通常是自己)
  bool condition2_nodeListPopulated = (g_node_count > 0);

  // 條件 3: 本機長名稱必須已經不再是預設值
  bool condition3_longNameSynced = (g_my_long_name != "PicoType UI");

  // 條件 4: 本機短名稱必須已經不再是預設值
  bool condition4_shortNameSynced = (g_my_short_name != "Pico");

  // 條件 5: 必須至少獲取到一個頻道資訊 (Primary Channel)
  bool condition5_gotChannels = (g_channel_count > 0);

  // 條件 6: 必須收到 LoRa Config (Region 不為 UNSET)
  bool condition6_gotLoRaConfig = (g_my_info.region != meshtastic_Config_LoRaConfig_RegionCode_UNSET);

  // --- 檢查所有條件是否都滿足 ---
  if (condition1_gotMyInfo && condition2_nodeListPopulated && condition3_longNameSynced && condition4_shortNameSynced && condition5_gotChannels && condition6_gotLoRaConfig) {
    Serial.println("\n[CONNECTION] Success! All sync conditions met.");

    // 將最終的成功旗標設為 true，這會停止 [Connecting...] 迴圈
    g_device_responded = true;

    // 更新 UI 顯示一個更有意義的成功訊息
    editor_content = "[Sync Complete!]\n";
    needs_redraw = true;

    // 成功後，立刻開始發送心跳包
    g_last_heartbeat_time = millis(); 
  }
}

void sendHeartbeat() {
  meshtastic_ToRadio payload = meshtastic_ToRadio_init_default;
  payload.which_payload_variant = meshtastic_ToRadio_heartbeat_tag;
  sendProtoPacket(payload);
  Serial.println("[TX] Heartbeat sent.");
  g_last_heartbeat_time = millis();
}
void sendOwnerInfo() {
  meshtastic_ToRadio payload = meshtastic_ToRadio_init_default;
  payload.which_payload_variant = meshtastic_ToRadio_packet_tag;
  meshtastic_MeshPacket* packet = &payload.packet;
  packet->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
  packet->to = g_my_info.node_num;
  packet->want_ack = false;
  meshtastic_Data* data = &packet->decoded;
  data->portnum = meshtastic_PortNum_NODEINFO_APP;
  meshtastic_User user = meshtastic_User_init_default;
  strcpy(user.id, "!0");
  strncpy(user.long_name, g_my_long_name.c_str(), sizeof(user.long_name) - 1);
  strncpy(user.short_name, g_my_short_name.c_str(), sizeof(user.short_name) - 1);  
  user.hw_model = meshtastic_HardwareModel_PRIVATE_HW;
  pb_ostream_t stream = pb_ostream_from_buffer(data->payload.bytes, sizeof(data->payload.bytes));
  if (pb_encode(&stream, meshtastic_User_fields, &user)) {
    data->payload.size = stream.bytes_written;
    sendProtoPacket(payload);
    Serial.println("[TX] SetOwner packet sent.");
  } else {
    Serial.println("[TX-ERROR] Failed to encode User for SetOwner.");
  }
}
void sendProtobufTextMessage() {
  int last_newline = editor_content.lastIndexOf('\n');
  String message_to_send = editor_content.substring(last_newline + 1);
  if (message_to_send.length() == 0) return;
  meshtastic_ToRadio payload = meshtastic_ToRadio_init_default;
  payload.which_payload_variant = meshtastic_ToRadio_packet_tag;
  meshtastic_MeshPacket* packet = &payload.packet;
  packet->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
  packet->to = g_target_node_num;
  packet->want_ack = true;
  uint32_t packet_id = millis();
  packet->id = packet_id;
  g_last_sent_packet_id = packet_id;
  packet->hop_limit = 3;
  packet->channel = g_selected_channel_index; // 設定發送頻道
  meshtastic_Data* data = &packet->decoded;
  data->portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
  size_t len = message_to_send.length();
  if (len >= sizeof(data->payload.bytes)) len = sizeof(data->payload.bytes) - 1;
  memcpy(data->payload.bytes, message_to_send.c_str(), len);
  data->payload.size = len;
  sendProtoPacket(payload);
  g_last_sent_line = message_to_send;
  editor_content += " [⋯]\n";
  trimEditorContent();
  needs_redraw = true;
}
void handleProtoRx() {
  static enum { WAITING_FOR_START_1,
                WAITING_FOR_START_2,
                READING_LEN_1,
                READING_LEN_2,
                READING_PAYLOAD } rx_state = WAITING_FOR_START_1;
  static uint16_t payload_len = 0;
  static uint16_t payload_idx = 0;
  static uint32_t bytes_discarded = 0;  // 追蹤丟棄的 debug text 量

  // 批次快速讀取，避免 RX 緩衝區堆積導致 Core 1 crash
  int max_bytes = 512;  // 每次 loop 最多處理 512 bytes，避免阻塞太久
  while (meshSerial_available() && max_bytes-- > 0) {
    uint8_t b = meshSerial_read();
    switch (rx_state) {
      case WAITING_FOR_START_1:
        if (b == 0x94) {
          rx_state = WAITING_FOR_START_2;
        } else {
          bytes_discarded++;  // 丟棄 debug console 文字，不印 hex
        }
        break;
      case WAITING_FOR_START_2:
        if (b == 0xc3) {
          if (bytes_discarded > 0) {
            Serial.print("[RX] Discarded ");
            Serial.print(bytes_discarded);
            Serial.println(" bytes of debug text before packet.");
            bytes_discarded = 0;
          }
          rx_state = READING_LEN_1;
        } else {
          bytes_discarded++;
          rx_state = WAITING_FOR_START_1;
        }
        break;
      case READING_LEN_1:
        payload_len = (uint16_t)b << 8;
        rx_state = READING_LEN_2;
        break;
      case READING_LEN_2:
        payload_len |= b;
        if (payload_len > 0 && payload_len <= PROTO_BUFFER_SIZE) {
          payload_idx = 0;
          rx_state = READING_PAYLOAD;
        } else {
          rx_state = WAITING_FOR_START_1;
        }
        break;
      case READING_PAYLOAD:
        if (payload_idx < PROTO_BUFFER_SIZE) { proto_rx_buffer[payload_idx++] = b; }
        if (payload_idx >= payload_len) {
          Serial.println("\n--- DECODING PACKET ---");
          print_hex_buffer(proto_rx_buffer, payload_len, "[RAW]: ");
          meshtastic_FromRadio payload = meshtastic_FromRadio_init_zero;
          pb_istream_t stream = pb_istream_from_buffer(proto_rx_buffer, payload_len);
          bool status = pb_decode(&stream, meshtastic_FromRadio_fields, &payload);
          if (status) {
            Serial.print("[DECODER] Success! Identified payload variant tag: ");
            Serial.println(payload.which_payload_variant);
            if (payload.which_payload_variant == meshtastic_FromRadio_channel_tag) { Serial.println("[DECODER] Tag matches channel_tag! SHOULD BE PROCESSING."); }
            switch (payload.which_payload_variant) {
              case meshtastic_FromRadio_my_info_tag:
  if (g_connection_phase == PHASE_GETTING_ID) {
    g_my_info.node_num = payload.my_info.my_node_num;
    Serial.print("[CONNECTION] Phase 1/3 Complete: Got Node ID 0x");
    Serial.println(g_my_info.node_num, HEX);
    g_connection_phase = PHASE_POPULATING_NODELIST; // << 進入下一階段
  }

                break;
              case meshtastic_FromRadio_node_info_tag:
                updateNodeInDB(payload.node_info);
                needs_redraw = true;
                break;
              case meshtastic_FromRadio_config_tag:
                 if (payload.config.which_payload_variant == meshtastic_Config_lora_tag) {
                     g_my_info.region = payload.config.payload_variant.lora.region;
                     g_my_info.modem_preset = payload.config.payload_variant.lora.modem_preset;
                     g_my_info.channel_num = payload.config.payload_variant.lora.channel_num;
                     needs_redraw = true;
                     Serial.println("[CONFIG] Updated LoRa Config: Region=" + String(g_my_info.region) + ", Preset=" + String(g_my_info.modem_preset));
                 }
                 break;
              case meshtastic_FromRadio_channel_tag:
                if (g_channel_count < MAX_CHANNELS) {
                  g_channels[g_channel_count].index = payload.channel.index;
                  g_channels[g_channel_count].settings = payload.channel.settings;
                  g_channels[g_channel_count].role = payload.channel.role;
                  g_channel_count++;
                  needs_redraw = true;
                }
                break;
              case meshtastic_FromRadio_packet_tag:
                {
                  meshtastic_MeshPacket* packet = &payload.packet;
                  if (packet->which_payload_variant == meshtastic_MeshPacket_decoded_tag && packet->decoded.portnum == meshtastic_PortNum_ROUTING_APP && packet->decoded.request_id == g_last_sent_packet_id && g_last_sent_packet_id != 0) {
                    meshtastic_Routing routing_payload = meshtastic_Routing_init_zero;
                    pb_istream_t route_stream = pb_istream_from_buffer(packet->decoded.payload.bytes, packet->decoded.payload.size);
                    if (pb_decode(&route_stream, meshtastic_Routing_fields, &routing_payload)) {
                      if (routing_payload.which_variant == meshtastic_Routing_error_reason_tag) {
                        String original_line = g_last_sent_line + " [⋯]";
                        String replaced_line = g_last_sent_line + " [×]"; //✗
                        editor_content.replace(original_line, replaced_line);
                        g_last_sent_packet_id = 0;
                        needs_redraw = true;
                      }
                    }
                  } else if (packet->decoded.reply_id == g_last_sent_packet_id && g_last_sent_packet_id != 0) {
                    String original_line = g_last_sent_line + " [⋯]";
                    String replaced_line = g_last_sent_line + " [○]"; // ✓
                    editor_content.replace(original_line, replaced_line);
                    g_last_sent_packet_id = 0;
                    needs_redraw = true;
                  }
                  if (packet->which_payload_variant == meshtastic_MeshPacket_decoded_tag && packet->decoded.portnum == meshtastic_PortNum_TEXT_MESSAGE_APP) {
                    bool node_exists = false;
                    for (int i = 0; i < g_node_count; i++) {
                      if (g_node_db[i].num == packet->from) {
                        node_exists = true;
                        break;
                      }
                    }
                    if (!node_exists) {
                      meshtastic_NodeInfo tempNodeInfo = meshtastic_NodeInfo_init_zero;
                      tempNodeInfo.num = packet->from;
                      tempNodeInfo.last_heard = g_current_unix_time;
                      tempNodeInfo.snr = packet->rx_snr;
                      updateNodeInDB(tempNodeInfo);
                    }
                    // String from_node_str = "0x" + String(packet->from, HEX);
                    // for (int i = 0; i < g_node_count; i++)
                    //   if (g_node_db[i].num == packet->from) {
                    //     from_node_str = g_node_db[i].short_name;
                    //     break;
                    //   }
                    // char text_buf[256] = { 0 };
                    // memcpy(text_buf, packet->decoded.payload.bytes, packet->decoded.payload.size);
                    // editor_content += "\n[" + from_node_str + "]: " + String(text_buf) + "\n";
                    String from_node_str = getNodeDisplayName(packet->from); // 直接呼叫新函式
                    char text_buf[256] = { 0 };
                    memcpy(text_buf, packet->decoded.payload.bytes, packet->decoded.payload.size);
                    editor_content += "\n[" + from_node_str + "]: " + String(text_buf) + "\n";
                    trimEditorContent();
                    
                    needs_redraw = true;
                  }
                }
                break;
              case meshtastic_FromRadio_config_complete_id_tag:
                editor_content += "[NodeDB sync complete]\n";
                needs_redraw = true;
                break;
              default: break;
            }
          } else {
            Serial.print("[DECODER] Failed: ");
            Serial.println(PB_GET_ERROR(&stream));
          }
          rx_state = WAITING_FOR_START_1;
        }
        break;
    }
  }
}
void sendTerminalLine() {
  int last_newline = editor_content.lastIndexOf('\n');
  String line_to_send = editor_content.substring(last_newline + 1);
  if (line_to_send.length() > 0) meshSerial_print(line_to_send);
  meshSerial_write('\n');
  editor_content += '\n';
  needs_redraw = true;
}

// ==========================================================================
// --- [UI REFACTOR] UI 繪圖函式 ---
// ==========================================================================

void drawTabBar() {
  tft.fillRect(0, 0, tft.width(), TAB_BAR_HEIGHT, 0x2104);  // 深色背景
  int current_x = MARGIN;

  for (int i = 0; i < g_tab_count; i++) {
    bool is_selected = (i == g_current_tab_index);
    uint16_t text_color = is_selected ? COLOR_INPUT : COLOR_TEXT;
    uint16_t bg_color = is_selected ? 0x4208 : 0x2104;

    int tab_width = renderer->getStringWidth(g_tabs[i].name) + PADDING * 2;

    tft.fillRect(current_x, 0, tab_width, TAB_BAR_HEIGHT, bg_color);
    renderer->drawString(current_x + PADDING, PADDING, g_tabs[i].name, text_color, bg_color);

    current_x += tab_width + MARGIN;
  }
  tft.drawFastHLine(0, TAB_BAR_HEIGHT - 1, tft.width(), COLOR_BORDER);
}

void drawMainChatContent() {
  int content_y_start = TAB_BAR_HEIGHT;

  String current_mode_hint = isProtoMode ? "[Proto]" : (isTerminalMode ? "[終端]" : "[編輯]");

  switch (g_input_mode) {
    case MODE_LOWERCASE:
      current_mode_hint += "[e]";
      break;
    case MODE_UPPERCASE:
      current_mode_hint += "[E]";
      break;
    case MODE_BOPOMOFO:
      current_mode_hint += "[注]";
      break;
  }

  if (current_mode_hint != g_old_mode_hint) {
    int old_width = renderer->getStringWidth(g_old_mode_hint.c_str());
    tft.fillRect(tft.width() - old_width - MARGIN, content_y_start + PADDING, old_width + MARGIN, FONT_HEIGHT, COLOR_BACKGROUND);
    renderer->drawString(tft.width() - renderer->getStringWidth(current_mode_hint.c_str()) - MARGIN, content_y_start + PADDING, current_mode_hint.c_str(), COLOR_HINT);
    g_old_mode_hint = current_mode_hint;
  }

  // String target_hint = "[To:" + g_target_node_name + "]";
  String target_name = (g_target_node_num == 0xFFFFFFFF) ? "廣播" : getNodeDisplayName(g_target_node_num);
  String target_hint = "[To:" + target_name + "]";  
  String current_input_display = target_hint + " " + (isImeEnabled ? inputBuffer_bopomofo : "");
  if (current_input_display != g_old_input_display) {
    tft.fillRect(MARGIN, content_y_start + PADDING, tft.width() - renderer->getStringWidth(current_mode_hint.c_str()) - MARGIN * 3, FONT_HEIGHT, COLOR_BACKGROUND);
    int target_width = renderer->getStringWidth(target_hint.c_str());
    renderer->drawString(MARGIN, content_y_start + PADDING, target_hint.c_str(), COLOR_CYAN);
    
    // 顯示當前頻道提示
    String ch_hint = "[Ch" + String(g_selected_channel_index) + "]";
    int ch_hint_width = renderer->getStringWidth(ch_hint.c_str());
    renderer->drawString(MARGIN + target_width + MARGIN, content_y_start + PADDING, ch_hint.c_str(), 0xFCA0); // Orange
    
    renderer->drawString(MARGIN + target_width + MARGIN + ch_hint_width + MARGIN, content_y_start + PADDING, (isImeEnabled ? inputBuffer_bopomofo : "").c_str(), COLOR_INPUT);
    g_old_input_display = current_input_display;
  }

  int y_pos = content_y_start + PADDING + FONT_HEIGHT + LINE_SPACING;
  tft.drawFastHLine(0, y_pos, tft.width(), COLOR_BORDER);
  y_pos += 1 + LINE_SPACING;
  String current_candidate_display = "候選: ";
  if (isImeEnabled && candidate_string.length() > 0) {
    int start_char_index = candidate_page * CANDIDATES_PER_PAGE;
    int byte_idx = getNthUtf8CharByteIndex(candidate_string, start_char_index);
    if (byte_idx != -1) {
      String page_string = candidate_string.substring(byte_idx);
      int chars_on_page = 0, current_byte_idx = 0;
      while (chars_on_page < CANDIDATES_PER_PAGE && current_byte_idx < page_string.length()) {
        int char_len = getUtf8CharLength(page_string.c_str() + current_byte_idx);
        if (currentState == STATE_SELECTION) current_candidate_display += String((chars_on_page + 1) % 10) + ":";
        current_candidate_display += page_string.substring(current_byte_idx, current_byte_idx + char_len) + " ";
        current_byte_idx += char_len;
        chars_on_page++;
      }
    }
  }
  if (current_candidate_display != g_old_candidate_display) {
    tft.fillRect(MARGIN, y_pos, tft.width() - MARGIN * 2, FONT_HEIGHT, COLOR_BACKGROUND);
    renderer->drawString(MARGIN, y_pos, current_candidate_display.c_str(), COLOR_CANDIDATE);
    g_old_candidate_display = current_candidate_display;
  }
  y_pos += FONT_HEIGHT + LINE_SPACING;
  tft.drawFastHLine(0, y_pos, tft.width(), COLOR_BORDER);
  int editor_area_y = y_pos + 1;
  tft.fillRect(0, editor_area_y, tft.width(), tft.height() - editor_area_y, COLOR_BACKGROUND);
  int last_line_break = 0, current_y = editor_area_y;
  int displayable_lines = (tft.height() - current_y) / (FONT_HEIGHT + LINE_SPACING);
  int line_count = 1;
  for (int i = 0; i < editor_content.length(); i++)
    if (editor_content[i] == '\n') line_count++;
  int start_line_num = max(0, line_count - displayable_lines);
  int current_line_num = 0;
  for (int i = 0; i < editor_content.length(); i++) {
    if (editor_content[i] == '\n') {
      if (current_line_num >= start_line_num) {
        String line = editor_content.substring(last_line_break, i);
        renderer->drawString(MARGIN, current_y, line.c_str(), COLOR_OUTPUT);
        current_y += FONT_HEIGHT + LINE_SPACING;
      }
      last_line_break = i + 1;
      current_line_num++;
    }
  }
  if (last_line_break < editor_content.length() && current_line_num >= start_line_num) {
    renderer->drawString(MARGIN, current_y, editor_content.substring(last_line_break).c_str(), COLOR_OUTPUT);
  }
}

void drawNodeDetailsContent() {
  int content_y_start = TAB_BAR_HEIGHT;
  tft.fillRect(0, content_y_start, tft.width(), tft.height() - content_y_start, COLOR_BACKGROUND);

  int node_idx = g_node_list_selection - 1;
  if (node_idx < 0 || node_idx >= g_node_count) {
    renderer->drawString(MARGIN, content_y_start + PADDING, "錯誤: 節點無效", COLOR_TEXT);
    return;
  }

  Node& node = g_node_db[node_idx];
  char buf[128], buf2[64];

  sprintf(buf, "%s (%s)", node.short_name, getHwModelString(node.hw_model));
  renderer->drawString(MARGIN, content_y_start + PADDING, buf, COLOR_HINT);
  const char* back_hint = "[←/ESC]返回";
  renderer->drawString(tft.width() - renderer->getStringWidth(back_hint) - MARGIN, content_y_start + PADDING, back_hint, COLOR_HINT);

  int y_pos = content_y_start + PADDING + FONT_HEIGHT + LINE_SPACING;
  tft.drawFastHLine(0, y_pos, tft.width(), COLOR_BORDER);
  y_pos += 1 + LINE_SPACING;

  sprintf(buf, "名稱: %s", node.long_name);
  renderer->drawString(MARGIN, y_pos, buf, COLOR_TEXT);
  y_pos += FONT_HEIGHT + LINE_SPACING;

  sprintf(buf, "ID:   !%08lX", node.num);
  renderer->drawString(MARGIN, y_pos, buf, COLOR_TEXT);
  y_pos += FONT_HEIGHT + LINE_SPACING;

  unsigned long time_since_heard = 0;
  if (g_current_unix_time > 0 && node.last_heard > 0) { time_since_heard = g_current_unix_time - node.last_heard; }
  sprintf(buf, "狀態: %lus ago", time_since_heard);
  renderer->drawString(MARGIN, y_pos, buf, COLOR_TEXT);

  sprintf(buf2, "SNR: %.1f dB", node.snr);
  renderer->drawString(tft.width() - renderer->getStringWidth(buf2) - MARGIN, y_pos, buf2, COLOR_TEXT);
  y_pos += FONT_HEIGHT + LINE_SPACING;

  y_pos += LINE_SPACING;
  tft.drawFastHLine(MARGIN, y_pos, tft.width() - MARGIN * 2, COLOR_BORDER);
  y_pos += LINE_SPACING;

  if (node.has_position) {
    float lat = node.latitude_i / 10000000.0f;
    float lon = node.longitude_i / 10000000.0f;
    sprintf(buf, "GPS: %.4f, %.4f", lat, lon);
    renderer->drawString(MARGIN, y_pos, buf, COLOR_TEXT);
  } else {
    renderer->drawString(MARGIN, y_pos, "GPS: N/A", COLOR_TEXT);
  }
}

void drawNodeListContent() {
  if (g_is_node_detail_view_active) {
    drawNodeDetailsContent();
    return;
  }

  int content_y_start = TAB_BAR_HEIGHT;
  const int ITEM_HEIGHT = FONT_HEIGHT + LINE_SPACING;

  tft.fillRect(0, content_y_start, tft.width(), tft.height() - content_y_start, COLOR_BACKGROUND);

  char buf[128];
  sprintf(buf, "節點列表 (%d) - [→]詳情 [Enter]選擇", g_node_count + 1);
  renderer->drawString(MARGIN, content_y_start + PADDING, buf, COLOR_HINT);

  int y_pos = content_y_start + PADDING + FONT_HEIGHT + LINE_SPACING;

  // --- 繪製廣播選項 (維持原樣) ---
  bool is_broadcast_selected = (g_node_list_selection == 0);
  uint16_t broadcast_bg_color = is_broadcast_selected ? COLOR_CYAN : COLOR_BACKGROUND;
  uint16_t broadcast_text_color = is_broadcast_selected ? COLOR_BACKGROUND : COLOR_CYAN;
  tft.fillRect(0, y_pos, tft.width(), ITEM_HEIGHT - 1, broadcast_bg_color);
  renderer->drawString(MARGIN + 12, y_pos, "廣播 (Broadcast All)", broadcast_text_color, broadcast_bg_color);
  y_pos += ITEM_HEIGHT;

  // --- 繪製節點列表 (新的佈局邏輯) ---
  int displayable_lines = (tft.height() - y_pos) / ITEM_HEIGHT;
  int start_index = 0;
  if (g_node_list_selection > 0 && (g_node_list_selection - 1) >= displayable_lines) {
    start_index = (g_node_list_selection - 1) - displayable_lines + 1;
  }
  
  // --- 定義欄位寬度和位置 ---
  const int SCREEN_WIDTH = tft.width();
  const int TIME_WIDTH = renderer->getStringWidth("99m ago") + MARGIN; // 最後通訊時間的寬度
  const int ID_WIDTH = renderer->getStringWidth("!FFFF") + MARGIN;   // 縮寫ID的寬度
  
  const int TIME_X = SCREEN_WIDTH - TIME_WIDTH;
  const int ID_X = TIME_X - ID_WIDTH;
  const int NAME_MAX_WIDTH = ID_X - (MARGIN + 12) - MARGIN; // 名稱可用的最大寬度

  for (int i = start_index; i < g_node_count; i++) {
    if (y_pos > tft.height() - ITEM_HEIGHT) break;
    Node& node = g_node_db[i];
    bool is_selected = ((i + 1) == g_node_list_selection);

    uint16_t bg_color = is_selected ? COLOR_TEXT : COLOR_BACKGROUND;
    uint16_t text_color = is_selected ? COLOR_BACKGROUND : COLOR_TEXT;

    tft.fillRect(0, y_pos, tft.width(), ITEM_HEIGHT - 1, bg_color);
    
    // 狀態燈
    uint16_t status_color = COLOR_STATUS_OFFLINE;
    unsigned long time_since_heard = 0;
    if (g_current_unix_time > 0 && node.last_heard > 0) { time_since_heard = g_current_unix_time - node.last_heard; }
    if (node.last_heard > 0) {
      if (time_since_heard < 5 * 60) status_color = COLOR_STATUS_ONLINE;
      else if (time_since_heard < 30 * 60) status_color = COLOR_STATUS_STALE;
    }
    tft.fillRect(MARGIN, y_pos + (ITEM_HEIGHT - 8) / 2, 8, 8, status_color);

    // --- 繪製名稱 (帶截斷邏輯) ---
    String display_name = getNodeDisplayName(node.num);
    // 如果名稱太長，就從後面開始逐字刪除，直到寬度符合為止
    while(renderer->getStringWidth(display_name.c_str()) > NAME_MAX_WIDTH && display_name.length() > 0) {
        int last_char_idx = getNthUtf8CharByteIndex(display_name, countUtf8Chars(display_name) - 1);
        if (last_char_idx != -1) display_name.remove(last_char_idx);
        else break; // 避免無限迴圈
    }
    renderer->drawString(MARGIN + 12, y_pos, display_name.c_str(), text_color, bg_color);
    
    // --- 繪製縮寫ID (靠右對齊) ---
    char short_id_buf[10];
    sprintf(short_id_buf, "!%04lX", node.num & 0xFFFF);
    renderer->drawString(ID_X, y_pos, short_id_buf, text_color, bg_color);

    // --- 繪製最後通訊時間 (靠右對齊) ---
    String relative_time_str = "N/A";
    if (node.last_heard > 0 && g_current_unix_time > 0) {
      if (time_since_heard < 60) relative_time_str = String(time_since_heard) + "s";
      else if (time_since_heard < 3600) relative_time_str = String(time_since_heard / 60) + "m";
      else relative_time_str = String(time_since_heard / 3600) + "h";
    }
    // 從最右邊往左計算繪製起點，以實現右對齊
    int time_draw_x = SCREEN_WIDTH - renderer->getStringWidth(relative_time_str.c_str()) - MARGIN;
    renderer->drawString(time_draw_x, y_pos, relative_time_str.c_str(), text_color, bg_color);

    y_pos += ITEM_HEIGHT;
  }
}

void drawChannelListContent() {
  int content_y_start = TAB_BAR_HEIGHT;
  tft.fillRect(0, content_y_start, tft.width(), tft.height() - content_y_start, COLOR_BACKGROUND);

  char buf[128];
  sprintf(buf, "頻道列表 (目前: %d) - [Enter]切換 [r]重整", g_selected_channel_index);
  renderer->drawString(MARGIN, content_y_start + PADDING, buf, COLOR_HINT);

  int y_pos = content_y_start + PADDING + FONT_HEIGHT + LINE_SPACING;

  tft.fillRect(0, y_pos, tft.width(), FONT_HEIGHT + 2, 0x18C3);
  renderer->drawString(MARGIN, y_pos, "Idx", COLOR_TEXT);
  renderer->drawString(MARGIN + 30, y_pos, "名稱", COLOR_TEXT);
  renderer->drawString(MARGIN + 90, y_pos, "模式", COLOR_TEXT);
  renderer->drawString(MARGIN + 150, y_pos, "角色", COLOR_TEXT);
  y_pos += FONT_HEIGHT + LINE_SPACING;

  for (int i = 0; i < g_channel_count; i++) {
    if (y_pos > tft.height() - FONT_HEIGHT) break;
    Channel& ch = g_channels[i];
    bool is_selected = (i == g_channel_list_selection);

    uint16_t bg_color = is_selected ? COLOR_TEXT : COLOR_BACKGROUND;
    uint16_t text_color = is_selected ? COLOR_BACKGROUND : COLOR_TEXT;

    tft.fillRect(0, y_pos, tft.width(), FONT_HEIGHT + LINE_SPACING - 1, bg_color);
    
    // 繪製選取標記與 Index
    sprintf(buf, "%s%d", (ch.index == g_selected_channel_index ? "*" : ""), ch.index);
    renderer->drawString(MARGIN, y_pos, buf, text_color, bg_color);

    String ch_name = String(ch.settings.name);
    if (ch.role == meshtastic_Channel_Role_PRIMARY) { ch_name += " (P)"; }
    renderer->drawString(MARGIN + 30, y_pos, ch_name.c_str(), text_color, bg_color);

    const char* mode_str = ch.settings.psk.size > 0 ? "私密" : "公共";
    renderer->drawString(MARGIN + 90, y_pos, mode_str, text_color, bg_color);

    renderer->drawString(MARGIN + 150, y_pos, getChannelRoleString(ch.role).c_str(), text_color, bg_color);

    char link_status[5] = "";
    if (ch.settings.uplink_enabled && ch.settings.downlink_enabled) strcpy(link_status, "↑↓");
    else if (ch.settings.uplink_enabled) strcpy(link_status, "↑");
    else if (ch.settings.downlink_enabled) strcpy(link_status, "↓");
    renderer->drawString(MARGIN + 220, y_pos, link_status, text_color, bg_color);

    y_pos += FONT_HEIGHT + LINE_SPACING;
  }
}

void drawMyInfoContent() {
  int content_y_start = TAB_BAR_HEIGHT;
  tft.fillRect(0, content_y_start, tft.width(), tft.height() - content_y_start, COLOR_BACKGROUND);
  char buf[128];
  int y_pos = content_y_start + PADDING;

  // 根據狀態是「瀏覽」還是「編輯」來繪製 UI
  if (g_my_info_tab_state == MY_INFO_BROWSING) {
    // --- 瀏覽模式 ---
    renderer->drawString(MARGIN, y_pos, "本機資訊 - [↑↓]選擇 [Enter]編輯", COLOR_HINT);
    y_pos += FONT_HEIGHT + LINE_SPACING;
    tft.drawFastHLine(0, y_pos, tft.width(), COLOR_BORDER);
    y_pos += 1 + LINE_SPACING;

    // --- 繪製可編輯欄位 (帶反白效果) ---
    // 長名稱
    bool is_long_name_selected = (g_my_info_selection == 0);
    uint16_t long_name_bg = is_long_name_selected ? COLOR_TEXT : COLOR_BACKGROUND;
    uint16_t long_name_fg = is_long_name_selected ? COLOR_BACKGROUND : COLOR_TEXT;
    tft.fillRect(0, y_pos, tft.width(), FONT_HEIGHT + LINE_SPACING - 1, long_name_bg);
    sprintf(buf, "長名稱:   %s", g_my_long_name.c_str());
    renderer->drawString(MARGIN, y_pos, buf, long_name_fg, long_name_bg);
    y_pos += FONT_HEIGHT + LINE_SPACING;

    // 短名稱
    bool is_short_name_selected = (g_my_info_selection == 1);
    uint16_t short_name_bg = is_short_name_selected ? COLOR_TEXT : COLOR_BACKGROUND;
    uint16_t short_name_fg = is_short_name_selected ? COLOR_BACKGROUND : COLOR_TEXT;
    tft.fillRect(0, y_pos, tft.width(), FONT_HEIGHT + LINE_SPACING - 1, short_name_bg);
    sprintf(buf, "短名稱:   %s", g_my_short_name.c_str());
    renderer->drawString(MARGIN, y_pos, buf, short_name_fg, short_name_bg);
    y_pos += FONT_HEIGHT + LINE_SPACING;

    // --- 繪製不可編輯的唯讀資訊 ---
    // 節點 ID
    if (g_my_info.node_num != 0) {
      sprintf(buf, "節點 ID:   !%08lX", g_my_info.node_num);
    } else {
      strcpy(buf, "節點 ID:   (等待同步)");
    }
    renderer->drawString(MARGIN, y_pos, buf, COLOR_TEXT);
    y_pos += FONT_HEIGHT + LINE_SPACING;

    // 連線狀態
    sprintf(buf, "USB: %s / Proto: %s", usb_cdc_mounted ? "掛載" : "未偵測", g_device_responded ? "已連線" : "未連線");
    renderer->drawString(MARGIN, y_pos, buf, g_device_responded ? COLOR_STATUS_ONLINE : (usb_cdc_mounted ? COLOR_STATUS_STALE : COLOR_STATUS_OFFLINE));
    y_pos += FONT_HEIGHT + LINE_SPACING;

    // 區域
    sprintf(buf, "區域: %s", getRegionString(g_my_info.region).c_str());
    renderer->drawString(MARGIN, y_pos, buf, COLOR_TEXT);
    y_pos += FONT_HEIGHT + LINE_SPACING;

    // 模式 (Modem Preset)
    sprintf(buf, "模式: %s", getModemPresetString(g_my_info.modem_preset).c_str());
    renderer->drawString(MARGIN, y_pos, buf, COLOR_TEXT);
    y_pos += FONT_HEIGHT + LINE_SPACING;

    // 頻率 Slot
    sprintf(buf, "頻率Slot: %d", g_my_info.channel_num);
    renderer->drawString(MARGIN, y_pos, buf, COLOR_TEXT);
    y_pos += FONT_HEIGHT + LINE_SPACING;

  } else { // g_my_info_tab_state == MY_INFO_EDITING
    // --- 編輯模式 ---
    String title = (g_my_info_selection == 0) ? "編輯長名稱" : "編輯短名稱";
    renderer->drawString(MARGIN, y_pos, title.c_str(), COLOR_HINT);
    String hint = "[Enter]儲存, [ESC]取消";
    renderer->drawString(tft.width() - renderer->getStringWidth(hint.c_str()) - MARGIN, y_pos, hint.c_str(), COLOR_HINT);
    y_pos += FONT_HEIGHT + LINE_SPACING;
    tft.drawFastHLine(0, y_pos, tft.width(), COLOR_BORDER);
    y_pos += 1 + PADDING;

    // 顯示正在輸入的文字，並加上一個模擬的游標
    String display_text = g_edit_buffer + "_";
    tft.fillRect(0, y_pos, tft.width(), FONT_HEIGHT + PADDING * 2, 0x2104); // 輸入框背景
    renderer->drawString(MARGIN, y_pos + PADDING, display_text.c_str(), COLOR_INPUT);
  }
}

void drawUI() {
  if (!needs_redraw) return;

  static int last_tab_index = -1;
  if (g_current_tab_index != last_tab_index) {
    tft.fillScreen(COLOR_BACKGROUND);
    g_old_input_display = "";
    g_old_candidate_display = "";
    g_old_mode_hint = "";
    last_tab_index = g_current_tab_index;
  }

  drawTabBar();
  g_tabs[g_current_tab_index].draw_func();

  needs_redraw = false;
}

// ==========================================================================
// --- [UI REFACTOR] 輸入處理 ---
// ==========================================================================
void handleSelection(int choice) {
  int actual_index = candidate_page * CANDIDATES_PER_PAGE + choice;
  int byte_idx = getNthUtf8CharByteIndex(candidate_string, actual_index);
  if (byte_idx != -1) {
    String selected_char = candidate_string.substring(byte_idx, byte_idx + getUtf8CharLength(candidate_string.c_str() + byte_idx));
    editor_content += selected_char;
    inputBuffer_keys = "";
    inputBuffer_bopomofo = "";
    candidate_string = "";
    currentState = STATE_INPUT;
    needs_redraw = true;
  }
}

void handleMainChatInput(char keyCode) {


  if (isImeEnabled) {
    if (currentState == STATE_INPUT) {
      if (keyCode == KEY_ESC) {
        inputBuffer_keys = "";
        inputBuffer_bopomofo = "";
        candidate_string = "";
      } else if ((keyCode >= 'a' && keyCode <= 'z') || (keyCode >= '0' && keyCode <= '9') || strchr(";,./-", keyCode)) {
        inputBuffer_keys += keyCode;
        char temp[32] = { 0 };
        ImeEngine::mapKeyToBopomofo(inputBuffer_keys.c_str(), temp, sizeof(temp));
        inputBuffer_bopomofo = temp;
        char result_buf[512] = { 0 };
        if (ime.query(inputBuffer_keys.c_str(), result_buf, sizeof(result_buf)) && strlen(result_buf) > 0) {
          candidate_string = result_buf;
          candidate_page = 0;
        } else {
          candidate_string = "";
        }
      } else if (keyCode == ' ' || keyCode == KEY_ENTER) {
        if (inputBuffer_keys.length() > 0 && candidate_string.length() > 0) {
          currentState = STATE_SELECTION;
          if (keyCode == ' ') handleSelection(0);
        } else if (inputBuffer_keys.length() == 0) {
          if (keyCode == KEY_ENTER) {
            if (isProtoMode) sendProtobufTextMessage();
            else if (isTerminalMode) sendTerminalLine();
            else editor_content += '\n';
          } else if (keyCode == ' ') editor_content += "　";
        }
      } else if (keyCode == KEY_BACKSPACE) {
        if (inputBuffer_keys.length() > 0) {
          inputBuffer_keys.remove(inputBuffer_keys.length() - 1);
          char temp[32] = { 0 };
          ImeEngine::mapKeyToBopomofo(inputBuffer_keys.c_str(), temp, sizeof(temp));
          inputBuffer_bopomofo = temp;
          char result_buf[512] = { 0 };
          if (inputBuffer_keys.length() > 0 && ime.query(inputBuffer_keys.c_str(), result_buf, sizeof(result_buf)) && strlen(result_buf) > 0) {
            candidate_string = result_buf;
            candidate_page = 0;
          } else {
            candidate_string = "";
          }
        } else if (editor_content.length() > 0) {
          int last_char_idx = getNthUtf8CharByteIndex(editor_content, countUtf8Chars(editor_content) - 1);
          if (last_char_idx != -1) editor_content.remove(last_char_idx);
        }
      }
    } else if (currentState == STATE_SELECTION) {
      int choice = -1;
      if (keyCode >= '1' && keyCode <= '9') choice = keyCode - '1';
      else if (keyCode == '0') choice = 9;
      if (choice != -1) handleSelection(choice);
      else if (keyCode == '.' || keyCode == KEY_RIGHT_ARROW) {
        if ((candidate_page + 1) * CANDIDATES_PER_PAGE < countUtf8Chars(candidate_string)) candidate_page++;
      } else if (keyCode == ',' || keyCode == KEY_LEFT_ARROW) {
        if (candidate_page > 0) candidate_page--;
      } else if (keyCode == KEY_BACKSPACE || keyCode == KEY_ESC) {
        currentState = STATE_INPUT;
      }
    }
  } else {  // IME is disabled
    if (keyCode == KEY_ESC) {
    } else {
      switch (keyCode) {
        case KEY_ENTER:
          if (isProtoMode) sendProtobufTextMessage();
          else if (isTerminalMode) sendTerminalLine();
          else editor_content += '\n';
          break;
        case KEY_BACKSPACE:
          if (editor_content.length() > 0) {
            int last_char_idx = getNthUtf8CharByteIndex(editor_content, countUtf8Chars(editor_content) - 1);
            if (last_char_idx != -1) editor_content.remove(last_char_idx);
          }
          break;
        default:
          if (keyCode > 0 && keyCode < 200) { editor_content += (char)keyCode; }
          break;
      }
    }
  }
  needs_redraw = true;
}

void handleNodeListInput(char keyCode) {
  if (g_is_node_detail_view_active) {
    if (keyCode == KEY_ESC || keyCode == KEY_LEFT_ARROW) {
      g_is_node_detail_view_active = false;
      needs_redraw = true;
    }
    return;
  }

  if (keyCode == KEY_UP_ARROW) {
    if (g_node_list_selection > 0) g_node_list_selection--;
  } else if (keyCode == KEY_DOWN_ARROW) {
    if (g_node_list_selection < g_node_count) g_node_list_selection++;
  } else if (keyCode == KEY_ENTER) {
    if (g_node_list_selection == 0) {
      g_target_node_num = 0xFFFFFFFF;
      g_target_node_name = "廣播";
      editor_content += "[切換至廣播模式]\n";
    } else {
      int node_idx = g_node_list_selection - 1;
      if (node_idx < g_node_count) {
        g_target_node_num = g_node_db[node_idx].num;
        // g_target_node_name = g_node_db[node_idx].short_name;
        // editor_content += "[開始與 " + g_target_node_name + " 的私訊]\n";
        g_target_node_name = getNodeDisplayName(g_target_node_num); // 更新全域目標名稱
        editor_content += "[開始與 " + g_target_node_name + " 的私訊]\n";
      }
    }
    g_current_tab_index = 0;  // Switch to chat tab
  } else if (keyCode == KEY_RIGHT_ARROW) {
    if (g_node_list_selection > 0) {
      int node_idx = g_node_list_selection - 1;
      if (node_idx < g_node_count) {
        g_is_node_detail_view_active = true;
      }
    }
  } else if (keyCode == KEY_ESC) {
    g_current_tab_index = 0;  // Switch to chat tab
  }
  needs_redraw = true;
}

void handleChannelListInput(char keyCode) {
  if (keyCode == KEY_UP_ARROW) {
    if (g_channel_list_selection > 0) g_channel_list_selection--;
  } else if (keyCode == KEY_DOWN_ARROW) {
    if (g_channel_list_selection < g_channel_count - 1) g_channel_list_selection++;
  } else if (keyCode == KEY_ESC) {
    g_current_tab_index = 0;
  } else if (keyCode == KEY_ENTER) {
    if (g_channel_list_selection < g_channel_count) {
      g_selected_channel_index = g_channels[g_channel_list_selection].index;
      String chName = g_channels[g_channel_list_selection].settings.name;
      if (chName.length() == 0) chName = "Default";
      editor_content += "[系統] 已切換至頻道 " + String(g_selected_channel_index) + ": " + chName + "\n";
      g_current_tab_index = 0;
    }
  } else if (keyCode == 'r' || keyCode == 'R') {
    // 手動重整頻道資訊
    editor_content += "[系統] 正在重新請求頻道資訊...\n";
    sendWantConfigRequest();
    // 切換回聊天頁面看進度，或者留在原地？留在原地比較好，但要提示
  }
  needs_redraw = true;
}

void handleMyInfoInput(char keyCode) {
  if (g_my_info_tab_state == MY_INFO_BROWSING) {
    // --- 瀏覽模式下的按鍵處理 ---
    switch (keyCode) {
      case KEY_UP_ARROW:
        g_my_info_selection = max(0, g_my_info_selection - 1);
        needs_redraw = true;
        break;
      case KEY_DOWN_ARROW:
        g_my_info_selection = min(MY_INFO_EDITABLE_FIELDS - 1, g_my_info_selection + 1);
        needs_redraw = true;
        break;
      case KEY_ENTER:
        // 根據目前選擇的項目，進入編輯模式
        g_my_info_tab_state = MY_INFO_EDITING;
        if (g_my_info_selection == 0) { // 編輯長名稱
          g_edit_buffer = g_my_long_name;
        } else { // 編輯短名稱
          g_edit_buffer = g_my_short_name;
        }
        isImeEnabled = false; // 強制使用英文模式
        needs_redraw = true;
        break;
      case 'r':
      case 'R':
        // 手動重整
        editor_content += "[系統] 正在重新請求本機資訊...\n";
        sendWantConfigRequest();
        break;
      case KEY_ESC:
        g_current_tab_index = 0; // 返回聊天室
        needs_redraw = true;
        break;
    }
  } else { // g_my_info_tab_state == MY_INFO_EDITING
    // --- 編輯模式下的按鍵處理 (與上一版相同) ---
    switch (keyCode) {
      case KEY_ENTER:
        // 儲存變更
        if (g_my_info_selection == 0) {
          g_my_long_name = g_edit_buffer;
        } else if (g_my_info_selection == 1) {
          g_my_short_name = g_edit_buffer;
        }
        sendOwnerInfo(); // 將新名稱廣播到網路上
        g_my_info_tab_state = MY_INFO_BROWSING; // 返回瀏覽模式
        needs_redraw = true;
        break;
      case KEY_ESC:
        // 取消編輯
        g_my_info_tab_state = MY_INFO_BROWSING;
        needs_redraw = true;
        break;
      case KEY_BACKSPACE:
        if (g_edit_buffer.length() > 0) {
          g_edit_buffer.remove(g_edit_buffer.length() - 1);
          needs_redraw = true;
        }
        break;
      default:
        // 輸入字元
        if (keyCode > 0 && keyCode < 200) {
          int max_len = (g_my_info_selection == 0) ? 39 : 4;
          if (g_edit_buffer.length() < max_len) {
            g_edit_buffer += (char)keyCode;
            needs_redraw = true;
          }
        }
        break;
    }
  }
}

void processKeyEvent(int r, int c, bool isPressed) {
  if (keymap_base[r][c] == KEY_SHIFT_R) {
    isShiftPressed = isPressed;
    return;
  }
  if (keymap_base[r][c] == KEY_ALT) {
    isAltPressed = isPressed;
    return;
  }
  if (!isPressed) return;

  if (keymap_base[r][c] == KEY_CAPS_LOCK) {
    switch (g_input_mode) {
      case MODE_LOWERCASE:
        g_input_mode = MODE_UPPERCASE;
        break;
      case MODE_UPPERCASE:
        g_input_mode = MODE_BOPOMOFO;
        break;
      case MODE_BOPOMOFO:
        g_input_mode = MODE_LOWERCASE;
        break;
    }
    // 同步 isImeEnabled 狀態
    isImeEnabled = (g_input_mode == MODE_BOPOMOFO);

    // 切換模式後，清空輸入緩衝區，避免狀態混亂
    inputBuffer_keys = "";
    inputBuffer_bopomofo = "";
    candidate_string = "";
    currentState = STATE_INPUT;
    needs_redraw = true;

    return; // 處理完畢，直接返回，不執行後續程式碼
  }

  // Shift + CapsLock is the most reliable way to get a single character
  char baseKeyCode = keymap_base[r][c];
  char shiftedKeyCode = keymap_shifted[r][c];
  char finalKeyCode = isShiftPressed ? shiftedKeyCode : baseKeyCode;
  if (g_input_mode == MODE_UPPERCASE && baseKeyCode >= 'a' && baseKeyCode <= 'z') {
    finalKeyCode = isShiftPressed ? baseKeyCode : shiftedKeyCode;
  }

  if (finalKeyCode == KEY_PGDOWN) {
    g_current_tab_index = (g_current_tab_index - 1 + g_tab_count) % g_tab_count;
    needs_redraw = true;
    return;
  }
  if (finalKeyCode == KEY_PGUP) {
    g_current_tab_index = (g_current_tab_index + 1) % g_tab_count;
    needs_redraw = true;
    return;
  }

  if (isAltPressed) {
    // 全域快捷鍵
    if (finalKeyCode == 't') {
      isTerminalMode = !isTerminalMode;
      isProtoMode = false;
      editor_content = isTerminalMode ? "--- TERMINAL MODE ---\n" : "--- EDITOR MODE ---\n";
      g_current_tab_index = 0;
      needs_redraw = true;
      return;
    }
    if (finalKeyCode == 'p') {
      isProtoMode = !isProtoMode;
      isTerminalMode = false;
      if (isProtoMode) {
        editor_content = "--- PROTOBUF MODE ---\n";
        g_device_responded = false;
        sendProtoHandshake();
      } else {
        editor_content = "--- EDITOR MODE ---\n";
      }
      g_current_tab_index = 0;
      needs_redraw = true;
      return;
    }
    if (finalKeyCode == 'r') {
      if (isProtoMode) {
        g_device_responded = false;
        g_node_count = 0;
        editor_content = "--- PROTOBUF MODE ---\n";
        initiateConnectionAttempt();
      }
      return;
    }
    if (finalKeyCode == 'b') {
      g_target_node_num = 0xFFFFFFFF;
      g_target_node_name = "廣播";
      g_current_tab_index = 0;
      editor_content += "[切換至廣播模式]\n";
      needs_redraw = true;
      return;
    }
  }

  // 將按鍵事件分派給當前分頁的處理函式
  g_tabs[g_current_tab_index].input_handler(finalKeyCode);
}

// ==========================================================================
// --- 主程式 Setup & Loop (優化版) ---
// ==========================================================================
void setup() {
  // [穩定性優化] 將系統時脈鎖定在 120MHz (12MHz 的整數倍)
  // 這能讓 PIO 模擬 USB 訊號時的波形完全對齊基準，大幅減少通訊錯誤
  set_sys_clock_khz(120000, true);

  Serial.begin(115200);
  // 初始化雙核共享緩衝區
  usb_tx_ring.init();
  usb_rx_ring.init();
  // USB Host 在 Core 1 的 setup1() 中初始化
  Serial.println("[USB] Waiting for Meshtastic USB device...");
  pinMode(TFT_BLK, OUTPUT);
  digitalWrite(TFT_BLK, HIGH);
  tft.begin();
  tft.setRotation(1);
  renderer = new FontRenderer(tft);
  tft.setTextWrap(false);
  tft.setTextSize(1);
  tft.fillScreen(COLOR_BACKGROUND);
  pinMode(DATA_OUT_PIN, OUTPUT);
  pinMode(LATCH_PIN, OUTPUT);
  pinMode(CLOCK_PIN, OUTPUT);
  pinMode(DATA_IN_PIN, INPUT);
  digitalWrite(LATCH_PIN, LOW);
  digitalWrite(CLOCK_PIN, LOW);
  updateKeyboardAndLeds_TwoPulse();
  memcpy(lastKeyState, keyState, sizeof(keyState));
  Serial.println("\n--- PicoType Keyboard Initialized (v1.8.0) ---");
  initiateConnectionAttempt();
  needs_redraw = true;
}

// ==========================================================================
// --- Core 1: USB Host Task ---
// PIO-USB Host stack 必須在 Core 1 上運行
// ==========================================================================
void setup1() {
  // [修正] 移除 while (!Serial)，避免在未連接 PC 時導致 Core 1 啟動阻塞，進而觸發 Core 0 的 Watchdog
  // while (!Serial) delay(10); 

  // 設定 PIO-USB 腳位
  pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
  pio_cfg.pin_dp = HOST_PIN_DP;

  // [穩定性優化] 改用 PIO 0 實例，這是最通用的設定
  USBHost.configure_pio_usb(0, &pio_cfg);

  // [穩定性優化] 提升 PIO 中斷優先級至最高 (0)
  // 防止 Core 0 進行頻繁的螢幕 SPI 繪圖時干擾到 USB 數據的即時接收
  irq_set_priority(PIO0_IRQ_0, 0); 
  irq_set_priority(PIO1_IRQ_0, 0);

  USBHost.begin(1);  // rhport = 1 (PIO-USB)
  Serial.println("[USB] Core 1 PIO-USB Host initialized on PIO 0.");
}

// Core 1 心跳計數器，讓 Core 0 偵測 Core 1 是否還活著
volatile unsigned long core1_heartbeat = 0;

void loop1() {
  USBHost.task();

  // --- Core 1 獨佔 USB 讀寫 ---
  if (!usb_cdc_mounted) {
    core1_heartbeat = millis();
    return;
  }

  // TX: 從 tx_ring 讀出，送到 USB（每次最多 64 bytes，配合 USB endpoint 大小）
  // 注意：不呼叫 SerialHost.flush()。flush() 在 USB core 上是阻塞等待，會卡住
  // loop1() 直到 TX 送完，期間打亂 PIO-USB 的 SOF 時序，是觸發 host 卡死的成因之一。
  // write() 已把資料寫入 CDC TX FIFO，後面緊接的 USBHost.task() 會自然驅動實際傳輸。
  int tx_count = 64;
  while (usb_tx_ring.available() > 0 && tx_count-- > 0) {
    int b = usb_tx_ring.read();
    if (b >= 0) SerialHost.write((uint8_t)b);
  }

  // 讓 USBHost.task() 再跑一次，處理剛送出的 TX 並接收新的 RX
  USBHost.task();

  // RX: 從 USB 讀入，寫到 rx_ring（每次最多 64 bytes）
  int rx_count = 64;
  while (SerialHost.available() > 0 && usb_rx_ring.free_space() > 0 && rx_count-- > 0) {
    int b = SerialHost.read();
    if (b >= 0) usb_rx_ring.write((uint8_t)b);
  }

  core1_heartbeat = millis();
}

// ==========================================================================
// --- Core 0: Main Loop ---
// ==========================================================================
void loop() {
  // --- USB CDC 連線狀態偵測 (透過 volatile flag，不直接碰 SerialHost) ---
  static bool usb_was_connected = false;
  bool usb_connected = usb_cdc_mounted;

  if (usb_connected && !usb_was_connected) {
    Serial.println("[USB] Device connected, starting handshake...");
    editor_content += "[USB] 偵測到裝置，開始握手...\n";
    delay(500);  // 等待 CDC 初始化穩定
    initiateConnectionAttempt();
    needs_redraw = true;
  }
  if (!usb_connected && usb_was_connected) {
    Serial.println("[USB] Device disconnected!");
    // 不設 PHASE_GETTING_ID，避免在 USB 未 mount 時反覆 retry
    // 等 USB 重新 mount 後由 connect 分支啟動握手
    editor_content += "[USB] 裝置已斷開\n";
    needs_redraw = true;
  }
  usb_was_connected = usb_connected;

  // --- USB 除錯：每 10 秒印一次狀態 ---
  static unsigned long last_usb_debug = 0;
  if (millis() - last_usb_debug > 10000) {
    last_usb_debug = millis();
    unsigned long core1_age = millis() - core1_heartbeat;
    Serial.print("[DBG] mounted=");
    Serial.print(usb_cdc_mounted);
    Serial.print(" rx=");
    Serial.print(usb_rx_ring.available());
    Serial.print(" tx=");
    Serial.print(usb_tx_ring.available());
    Serial.print(" phase=");
    Serial.print(g_connection_phase);
    Serial.print(" core1_age=");
    Serial.print(core1_age);
    Serial.println("ms");

    // Core 1 超過 3 秒沒更新 = PIO-USB 卡死 (已知 bug)
    // PIO state machine 卡死後軟重啟 Core 1 無法恢復 USB 列舉
    // 因此直接整機重啟，這是目前最可靠的恢復方式
    if (core1_age > 3000) {
      Serial.println("[FATAL] Core 1 stopped! Rebooting...");
      Serial.flush();
      delay(500);
      watchdog_reboot(0, 0, 0);
    }
  }

  // --- 原有邏輯 ---
  if (isProtoMode) {
    while (meshSerial_available()) { handleProtoRx(); }

    if (usb_cdc_mounted && g_connection_phase != PHASE_CONNECTED && (millis() - g_last_handshake_time > HANDSHAKE_RETRY_INTERVAL)) {
      switch (g_connection_phase) {
        case PHASE_GETTING_ID:
          Serial.println("[RETRY] Retrying to get node ID...");
          sendWantConfigRequest();
          break;
        
        case PHASE_POPULATING_NODELIST:
          Serial.println("[RETRY] Retrying to populate node list...");
          editor_content += "[Connecting (2/3)...]\n";
          sendWantConfigRequest(); // 繼續發送通用請求來獲取 NodeInfo
          break;

        case PHASE_SYNCING_SELF_INFO:
          Serial.println("[RETRY] Retrying to sync self name...");

          editor_content += "[Syncing Name...]\n"; // 更新提示文字
          sendWantConfigRequest(); 
          
          
          break;

        default:
          break; // 不應發生
      }
    }
    if (usb_cdc_mounted && g_connection_phase == PHASE_CONNECTED && (millis() - g_last_heartbeat_time > HEARTBEAT_INTERVAL)) {
       sendHeartbeat();
       }
  } else if (isTerminalMode && meshSerial_available()) {
    while (meshSerial_available()) {
      char incomingByte = meshSerial_read();
      if (incomingByte == '\b' || incomingByte == 127) {
        if (editor_content.length() > 0) {
          int last_char_idx = getNthUtf8CharByteIndex(editor_content, countUtf8Chars(editor_content) - 1);
          if (last_char_idx != -1) editor_content.remove(last_char_idx);
        }
      } else {
        editor_content += incomingByte;
      }
    }
    needs_redraw = true;
  }

  updateKeyboardAndLeds_TwoPulse();

  for (int r = 0; r < 8; r++)
    for (int c = 0; c < 8; c++)
      if (keyState[r][c] != lastKeyState[r][c])
        processKeyEvent(r, c, keyState[r][c]);

  memcpy(lastKeyState, keyState, sizeof(keyState));
  drawUI();
}