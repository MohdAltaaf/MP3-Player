
#include <Arduino.h>
#include <math.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <Preferences.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "AudioTools.h"
#include "AudioTools/AudioCodecs/CodecMP3Helix.h"
#include "BluetoothA2DPSink.h"

constexpr int I2S_BCK = 26, I2S_LRCK = 25, I2S_DATA = 27;
constexpr int SD_CLK = 18, SD_MISO = 19, SD_MOSI = 23, SD_CS = 13;
constexpr int OLED_SDA = 21, OLED_SCL = 22;
constexpr int ENC_CLK = 32, ENC_DT = 33, ENC_SW = 14;
constexpr int BATTERY_ADC_PIN = 34; // ADC1; separate from the audio and SD pins.
constexpr float BATTERY_DIVIDER_RATIO = 2.0f; // 47k top / 47k bottom.
constexpr float BATTERY_CALIBRATION = 1.0f; // Actual voltage / reported voltage.
constexpr uint32_t BATTERY_SAMPLE_MS = 100;
constexpr uint8_t BATTERY_SAMPLES = 8;
constexpr int ENC_DIRECTION = 1; // Change to -1 if rotation is reversed.
constexpr int ENC_TRANSITIONS = 4; // Try 2 if two clicks are needed for one step.
constexpr int MAX_TRACKS = 100;
constexpr uint32_t HOLD_MS = 650;
constexpr uint32_t CLICK_GAP_MS = 300;
constexpr uint32_t UI_FRAME_MS = 40; // Up to 25 fps; only changed OLED ranges are sent.
constexpr uint32_t TRANSITION_MS = 200;
constexpr uint32_t VOLUME_POPUP_MS = 1300;
constexpr uint32_t VOLUME_TWEEN_MS = 140;
constexpr uint32_t VOLUME_SLIDE_MS = 180;
constexpr uint32_t VOLUME_SHAKE_MS = 280;
constexpr char BT_NAME[] = "ESP32_Music_Player";

enum PlayerMode : uint8_t { MODE_SD = 0, MODE_BT = 1 };
enum Screen : uint8_t { PLAYER, MENU, TRACK_LIST };

PlayerMode mode = MODE_SD;
Screen screen = PLAYER;
Preferences settings;
bool settingsOK = false;
Adafruit_SSD1306 oled(128, 64, &Wire, -1, 400000UL, 400000UL);
bool oledOK = false, sdOK = false, audioOK = false;
I2SStream audioOutput;
VolumeStream sdVolume(audioOutput);
BluetoothA2DPSink bluetooth(audioOutput);
audio_tools::MP3DecoderHelix *mp3 = nullptr;
EncodedAudioStream *decoded = nullptr;
StreamCopy copier(128); // Return to input/UI handling between small MP3 chunks.
File songFile;
String tracks[MAX_TRACKS];
int trackCount = 0, currentTrack = -1, selectedTrack = 0, menuIndex = 0;
bool paused = false;
int volumePercent = 35;
bool volumeDirty = false;
uint32_t lastVolumeChange = 0;
String notice;
uint32_t noticeUntil = 0;
bool displayDirty = true;
bool buttonDown = false;
struct ButtonState {
  bool lastRaw = HIGH, holdHandled = false;
  uint8_t clicks = 0;
  uint32_t changedAt = 0, pressedAt = 0, releasedAt = 0;
  Screen origin = PLAYER;
} button;
uint32_t lastInteraction = 0, volumeHighlightUntil = 0;
struct VolumePopupState {
  bool visible = false, shaking = false;
  float from = 35, to = 35;
  uint32_t openedAt = 0, touchedAt = 0, tweenAt = 0, shakenAt = 0;
  int shakeDirection = 1;
} volumePopup;
uint32_t transportUntil = 0;
int transportAction = 0; // -1 previous, 0 play/pause, +1 next.
uint8_t oledAddress = 0;
uint8_t transitionFrame[1024];
uint32_t transitionStarted = 0;
bool transitioning = false;
int transitionDirection = 1;
String btTitle, btArtist;
portMUX_TYPE metadataMux = portMUX_INITIALIZER_UNLOCKED;
char pendingTitle[160] = {}, pendingArtist[128] = {};
bool metadataChanged = false;
String marqueeText[2];
uint32_t marqueeStarted[2] = {};
int batteryPercent = -1; // Unknown until a plausible divider reading is available.
uint32_t batteryAdcMv = 0, batteryMv = 0;
float filteredBatteryMv = 0;

void drawScreen();
void hideVolumePopup();
bool startTrack(int index);
void changeTrack(int movement);

portMUX_TYPE encoderMux = portMUX_INITIALIZER_UNLOCKED;
volatile int encoderTicks = 0;
volatile uint8_t encoderPrevious = 0;
// DRAM table: safe to read from an IRAM interrupt handler.
DRAM_ATTR const int8_t encoderTable[16] = {
  0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0
};

void ARDUINO_ISR_ATTR encoderISR() {
  const uint8_t next = (digitalRead(ENC_CLK) << 1) | digitalRead(ENC_DT);
  portENTER_CRITICAL_ISR(&encoderMux);
  encoderTicks += encoderTable[(encoderPrevious << 2) | next];
  encoderPrevious = next;
  portEXIT_CRITICAL_ISR(&encoderMux);
}

void showNotice(const String &text) {
  notice = text;
  noticeUntil = millis() + 1600;
  volumeHighlightUntil = 0;
  hideVolumePopup();
  displayDirty = true;
  Serial.println(text);
}

String shortName(const String &path) {
  const int slash = path.lastIndexOf('/');
  return path.substring(slash + 1);
}

bool activeUntil(uint32_t deadline) {
  return deadline && int32_t(deadline - millis()) > 0;
}

int estimateBatteryPercent(float millivolts) {
  // Approximate UI profile for a conventional 3.7V nominal / 4.2V full cell.
  // Tune against your cell's discharge curve. Not for LiFePO4 or multi-cell packs.
  // The 0% display point is NOT a hardware undervoltage cutoff.
  const uint16_t voltage[] = {3300, 3500, 3600, 3700, 3750, 3800,
                             3850, 3900, 4000, 4100, 4200};
  const uint8_t percent[] = {0, 5, 10, 20, 35, 50, 60, 70, 85, 95, 100};
  if (millivolts <= voltage[0]) return 0;
  for (size_t i = 1; i < sizeof(voltage) / sizeof(voltage[0]); ++i) {
    if (millivolts <= voltage[i]) {
      const float fraction = (millivolts - voltage[i - 1]) /
                             float(voltage[i] - voltage[i - 1]);
      return int(percent[i - 1] + fraction * (percent[i] - percent[i - 1]) + 0.5f);
    }
  }
  return 100;
}

void updateBattery(bool immediate = false) {
  static uint32_t lastSample = 0, totalMv = 0;
  static uint8_t sampleCount = 0;
  const uint32_t now = millis();
  if (!immediate && now - lastSample < BATTERY_SAMPLE_MS) return;
  lastSample = now;
  // One conversion per visit: do not pause the MP3 pipeline for a sample burst.
  totalMv += analogReadMilliVolts(BATTERY_ADC_PIN);
  ++sampleCount;
  if (!immediate && sampleCount < BATTERY_SAMPLES) return;
  batteryAdcMv = (totalMv + sampleCount / 2) / sampleCount;
  totalMv = 0;
  sampleCount = 0;
  const float measured = batteryAdcMv * BATTERY_DIVIDER_RATIO * BATTERY_CALIBRATION;
  const int previous = batteryPercent;
  // With the bottom resistor fitted, a disconnected sense supply reads near 0V.
  // Reject an absent / implausible input instead of showing a fabricated percent.
  if (measured < 2500 || measured > 4350) {
    filteredBatteryMv = 0;
    batteryMv = 0;
    batteryPercent = -1;
  } else {
    filteredBatteryMv = filteredBatteryMv == 0 ? measured :
                        filteredBatteryMv + 0.15f * (measured - filteredBatteryMv);
    batteryMv = uint32_t(filteredBatteryMv + 0.5f);
    const int estimate = estimateBatteryPercent(filteredBatteryMv);
    // Two percentage points of hysteresis suppress flicker from playback load.
    if (batteryPercent < 0 || abs(estimate - batteryPercent) >= 2 ||
        estimate == 0 || estimate == 100) batteryPercent = estimate;
  }
  if (previous != batteryPercent) displayDirty = true;
}

void printBatteryStatus() {
  Serial.printf("Battery ADC: %lu mV | filtered battery: %lu mV | estimate: ",
                (unsigned long)batteryAdcMv, (unsigned long)batteryMv);
  if (batteryPercent < 0) Serial.println("unknown; check divider and sense supply");
  else Serial.printf("%d%% (voltage estimate)\n", batteryPercent);
}

// Decode UTF-8 and transliterate Vietnamese instead of displaying broken bytes.
String displayText(const String &input) {
  String result;
  result.reserve(input.length());
  for (size_t i = 0; i < input.length();) {
    uint32_t c = uint8_t(input[i++]);
    if (c >= 0xC2 && c <= 0xDF && i < input.length()) {
      c = ((c & 0x1F) << 6) | (uint8_t(input[i++]) & 0x3F);
    } else if (c >= 0xE0 && c <= 0xEF && i + 1 < input.length()) {
      const uint8_t a = input[i++], b = input[i++];
      c = ((c & 0x0F) << 12) | ((a & 0x3F) << 6) | (b & 0x3F);
    } else if (c >= 0xF0 && c <= 0xF4) {
      i = min(i + size_t(3), input.length());
      c = ' '; // Emoji have no glyph in the built-in font.
    }
    char letter = '?';
    if (c >= 32 && c <= 126) letter = char(c);
    else if (c >= 0x1EA0 && c <= 0x1EF9) {
      char base = c <= 0x1EB7 ? 'A' : c <= 0x1EC7 ? 'E' :
                  c <= 0x1ECB ? 'I' : c <= 0x1EE3 ? 'O' :
                  c <= 0x1EF1 ? 'U' : 'Y';
      letter = char(base + ((c & 1) ? 32 : 0));
    } else if (c >= 0x0300 && c <= 0x036F) continue; // Combining accent.
    else {
      switch (c) {
        case 0x00C0: case 0x00C1: case 0x00C2: case 0x00C3: case 0x0102: letter = 'A'; break;
        case 0x00E0: case 0x00E1: case 0x00E2: case 0x00E3: case 0x0103: letter = 'a'; break;
        case 0x00C8: case 0x00C9: case 0x00CA: letter = 'E'; break;
        case 0x00E8: case 0x00E9: case 0x00EA: letter = 'e'; break;
        case 0x00CC: case 0x00CD: case 0x0128: letter = 'I'; break;
        case 0x00EC: case 0x00ED: case 0x0129: letter = 'i'; break;
        case 0x00D2: case 0x00D3: case 0x00D4: case 0x00D5: case 0x01A0: letter = 'O'; break;
        case 0x00F2: case 0x00F3: case 0x00F4: case 0x00F5: case 0x01A1: letter = 'o'; break;
        case 0x00D9: case 0x00DA: case 0x0168: case 0x01AF: letter = 'U'; break;
        case 0x00F9: case 0x00FA: case 0x0169: case 0x01B0: letter = 'u'; break;
        case 0x00DD: letter = 'Y'; break;
        case 0x00FD: letter = 'y'; break;
        case 0x0110: letter = 'D'; break;
        case 0x0111: letter = 'd'; break;
        case 0x2018: case 0x2019: letter = '\''; break;
        case 0x201C: case 0x201D: letter = '"'; break;
        case 0x2013: case 0x2014: letter = '-'; break;
        default: if (c < 32) letter = ' '; break;
      }
    }
    result += letter;
  }
  return result;
}

String songTitle(int index) {
  if (index < 0 || index >= trackCount) return "Choose a song";
  String name = shortName(tracks[index]);
  name.remove(name.length() - 4); // Hide the .mp3 extension.
  return displayText(name);
}

void textAt(int x, int y, const String &text, uint8_t size = 1,
            uint16_t color = SSD1306_WHITE) {
  oled.setTextSize(size);
  oled.setTextColor(color);
  oled.setCursor(x, y);
  oled.print(text);
}

void rightText(int right, int y, const String &text) {
  textAt(right - int(text.length()) * 6, y, text);
}

void marquee(const String &text, int x, int y, int width, uint8_t size,
              int slot, uint16_t color = SSD1306_WHITE) {
  if (marqueeText[slot] != text) {
    marqueeText[slot] = text;
    marqueeStarted[slot] = millis();
  }
  const int overflow = int(text.length()) * 6 * size - width;
  int offset = 0;
  if (overflow > 0) {
    const uint32_t leg = 1000 + uint32_t(overflow) * 30;
    const uint32_t phase = (millis() - marqueeStarted[slot]) % (leg * 2);
    const uint32_t travel = phase <= leg ? phase : leg * 2 - phase;
    offset = travel <= 1000 ? 0 : min(overflow, int((travel - 1000) / 30));
  }
  oled.setTextSize(size);
  oled.setTextColor(color);
  const int glyphWidth = 6 * size;
  for (size_t i = 0; i < text.length(); ++i) {
    const int px = x + int(i) * glyphWidth - offset;
    if (px >= x + width) break;
    if (px + glyphWidth <= x) continue;
    oled.setCursor(px, y);
    oled.write(text[i]);
  }
  // Mask partial glyphs at the viewport edges; list markers are drawn later.
  const uint16_t background = color == SSD1306_WHITE ? SSD1306_BLACK : SSD1306_WHITE;
  if (x > 0) oled.fillRect(0, y, x, size * 8, background);
  if (x + width < 128) oled.fillRect(x + width, y, 128 - x - width, size * 8, background);
}

int menuCount() { return mode == MODE_SD ? 5 : 4; }

String menuLabel(int index) {
  if (mode == MODE_SD) {
    const char *labels[] = {"Browse songs", "Previous track", "Next track", "Use Bluetooth", "Back"};
    return labels[index];
  }
  const char *labels[] = {"Previous track", "Next track", "Use SD card", "Back"};
  return labels[index];
}

// Tiny vector icons remain crisp on a monochrome 128x64 display.
void drawIcon(int kind, int x, int y, uint16_t ink = SSD1306_WHITE) {
  if (kind == 0) { // SD card
    oled.drawRoundRect(x, y, 8, 9, 1, ink);
    for (int i = 0; i < 3; ++i) oled.drawFastVLine(x + 2 + i * 2, y + 1, 3, ink);
  } else if (kind == 1) { // Bluetooth
    oled.drawFastVLine(x + 4, y, 10, ink);
    oled.drawLine(x + 4, y, x + 8, y + 3, ink);
    oled.drawLine(x + 8, y + 3, x, y + 8, ink);
    oled.drawLine(x, y + 2, x + 8, y + 7, ink);
    oled.drawLine(x + 8, y + 7, x + 4, y + 10, ink);
  } else if (kind == 2) { // Speaker
    oled.fillRect(x, y + 3, 3, 4, ink);
    oled.fillTriangle(x + 2, y + 3, x + 6, y, x + 6, y + 9, ink);
    oled.drawLine(x + 8, y + 2, x + 9, y + 4, ink);
    oled.drawLine(x + 9, y + 4, x + 8, y + 7, ink);
  } else if (kind == 3 || kind == 4) { // Previous / next
    const bool next = kind == 4;
    oled.fillTriangle(x + (next ? 1 : 9), y, x + (next ? 1 : 9), y + 8,
                      x + (next ? 8 : 2), y + 4, ink);
    oled.fillRect(x + (next ? 9 : 0), y, 2, 9, ink);
  } else if (kind == 5) { // Music note
    oled.drawFastVLine(x + 3, y + 1, 7, ink);
    oled.drawFastVLine(x + 8, y, 7, ink);
    oled.drawLine(x + 3, y + 1, x + 8, y, ink);
    oled.fillCircle(x + 1, y + 8, 2, ink);
    oled.fillCircle(x + 6, y + 7, 2, ink);
  } else if (kind == 6) { // Back
    oled.drawLine(x, y + 4, x + 4, y, ink);
    oled.drawLine(x, y + 4, x + 4, y + 8, ink);
    oled.drawFastHLine(x, y + 4, 10, ink);
  } else { // Menu
    for (int i = 0; i < 3; ++i) oled.drawFastHLine(x, y + i * 4, 10, ink);
  }
}

void drawBattery() {
  oled.drawRoundRect(108, 1, 18, 9, 1, SSD1306_WHITE);
  oled.fillRect(126, 3, 2, 5, SSD1306_WHITE);
  if (batteryPercent < 0) { textAt(114, 2, "?"); return; }
  const int bars = constrain((batteryPercent + 24) / 25, 0, 4);
  for (int i = 0; i < bars; ++i) oled.fillRect(110 + i * 4, 3, 2, 5, SSD1306_WHITE);
}

// Cubic ease-out, driven by millis(): animations never wait or delay audio.
float easeOut(uint32_t elapsed, uint32_t duration) {
  const float t = min(1.0f, float(elapsed) / duration);
  const float remaining = 1.0f - t;
  return 1.0f - remaining * remaining * remaining;
}

float animatedVolume(uint32_t now) {
  return volumePopup.from + (volumePopup.to - volumePopup.from) *
         easeOut(now - volumePopup.tweenAt, VOLUME_TWEEN_MS);
}

void hideVolumePopup() {
  volumePopup.visible = volumePopup.shaking = false;
}

void showVolumePopup(int previousVolume, bool limitHit, int direction) {
  const uint32_t now = millis();
  const bool continuing = volumePopup.visible && now - volumePopup.touchedAt < VOLUME_POPUP_MS;
  // Retarget from the current visual position, even when the knob turns quickly.
  volumePopup.from = continuing ? animatedVolume(now) : float(previousVolume);
  volumePopup.to = volumePercent;
  volumePopup.tweenAt = now;
  if (!continuing) {
    volumePopup.openedAt = now;
    volumePopup.shaking = false;
  }
  volumePopup.touchedAt = now;
  volumePopup.visible = true;
  if (limitHit) {
    // Let a shake finish before restarting it; rapid turns must not freeze it.
    if (!volumePopup.shaking || now - volumePopup.shakenAt >= VOLUME_SHAKE_MS) {
      volumePopup.shaking = true;
      volumePopup.shakenAt = now;
      volumePopup.shakeDirection = direction < 0 ? -1 : 1;
    }
  } else volumePopup.shaking = false;
  noticeUntil = 0;
  displayDirty = true;
}

void drawVolumeArc(int cx, int cy, float level) {
  // Cache both outlines once. No trigonometry in subsequent animation frames.
  static int8_t points[61][4];
  static bool ready = false;
  if (!ready) {
    for (int i = 0; i <= 60; ++i) {
      const float angle = PI - i * PI / 60.0f;
      points[i][0] = int8_t(lroundf(31 * cosf(angle)));
      points[i][1] = int8_t(-lroundf(31 * sinf(angle)));
      points[i][2] = int8_t(lroundf(26 * cosf(angle)));
      points[i][3] = int8_t(-lroundf(26 * sinf(angle)));
    }
    ready = true;
  }
  const int filled = constrain(int(level * 60 / 100 + 0.5f), 0, 60);
  for (int i = 0; i < 60; ++i) {
    const int ox = cx + points[i][0], oy = cy + points[i][1];
    const int ix = cx + points[i][2], iy = cy + points[i][3];
    const int nx = cx + points[i + 1][0], ny = cy + points[i + 1][1];
    const int jx = cx + points[i + 1][2], jy = cy + points[i + 1][3];
    if (i < filled) {
      oled.fillTriangle(ox, oy, nx, ny, ix, iy, SSD1306_WHITE);
      oled.fillTriangle(ix, iy, nx, ny, jx, jy, SSD1306_WHITE);
    }
    oled.drawLine(ox, oy, nx, ny, SSD1306_WHITE);
    oled.drawLine(ix, iy, jx, jy, SSD1306_WHITE);
  }
  for (int i : {0, 60}) oled.drawLine(cx + points[i][0], cy + points[i][1],
                                   cx + points[i][2], cy + points[i][3], SSD1306_WHITE);
}

void drawVolumePopup() {
  if (!volumePopup.visible) return;
  const uint32_t now = millis(), age = now - volumePopup.touchedAt;
  if (age >= VOLUME_POPUP_MS) { hideVolumePopup(); return; }
  int dy = int(48 * (1.0f - easeOut(now - volumePopup.openedAt, VOLUME_SLIDE_MS)));
  const uint32_t remaining = VOLUME_POPUP_MS - age;
  if (remaining < VOLUME_SLIDE_MS)
    dy = int(48 * (1.0f - easeOut(remaining, VOLUME_SLIDE_MS)));
  int dx = 0;
  if (volumePopup.shaking) {
    const uint32_t elapsed = now - volumePopup.shakenAt;
    if (elapsed >= VOLUME_SHAKE_MS) volumePopup.shaking = false;
    else {
      const int8_t shake[] = {3, -3, 2, -2, 1, -1, 0};
      dx = shake[elapsed / UI_FRAME_MS] * volumePopup.shakeDirection;
    }
  }
  // Dim the background with a checker pattern; retain the status bar above.
  for (int y = 14; y < 64; ++y)
    for (int x = (y & 1); x < 128; x += 2) oled.drawPixel(x, y, SSD1306_BLACK);
  oled.fillRoundRect(8 + dx, 15 + dy, 112, 48, 6, SSD1306_BLACK);
  oled.drawRoundRect(8 + dx, 15 + dy, 112, 48, 6, SSD1306_WHITE);
  drawVolumeArc(64 + dx, 51 + dy, animatedVolume(now));
  const String label = volumePercent == 0 ? "MUTE" : volumePercent == 100 ? "MAX" : "VOL";
  textAt(64 + dx - int(label.length()) * 3, 32 + dy, label);
  const String value = String(volumePercent);
  textAt(64 + dx - int(value.length()) * 6, 43 + dy, value, 2);
  textAt(28 + dx, 54 + dy, "0");
  textAt(84 + dx, 54 + dy, "100");
}

void setScreen(Screen target) {
  if (screen == target) return;
  if (oledOK) memcpy(transitionFrame, oled.getBuffer(), sizeof(transitionFrame));
  transitionDirection = target == PLAYER || (screen == TRACK_LIST && target == MENU) ? -1 : 1;
  transitioning = oledOK;
  transitionStarted = millis();
  screen = target;
  button.clicks = 0;
  hideVolumePopup();
  volumeHighlightUntil = 0;
  displayDirty = true;
}

void drawList(bool songs) {
  const int count = songs ? trackCount : menuCount();
  const int selected = songs ? selectedTrack : menuIndex;
  drawIcon(songs ? 5 : 7, 1, 0);
  textAt(16, 1, songs ? "LIBRARY" : "MENU");
  rightText(127, 1, count ? String(selected + 1) + "/" + count : "0/0");
  oled.drawFastHLine(0, 11, 128, SSD1306_WHITE);
  if (!count) {
    textAt(2, 26, sdOK ? "No MP3 in SD root" : "SD not ready");
  } else {
    const int first = constrain(selected - 1, 0, max(0, count - 3));
    const int targetY = 14 + (selected - first) * 14;
    static Screen previousScreen = PLAYER;
    static int previousSelected = -1, fromY = 14, toY = 14;
    static uint32_t movedAt = 0;
    if (previousScreen != screen) {
      fromY = toY = targetY;
      previousSelected = selected;
      previousScreen = screen;
    } else if (previousSelected != selected) {
      fromY += int((toY - fromY) * easeOut(millis() - movedAt, 150));
      toY = targetY;
      previousSelected = selected;
      movedAt = millis();
    }
    for (int row = 0; row < 3 && first + row < count; ++row) {
      const int index = first + row, y = 14 + row * 14;
      const String label = songs ? songTitle(index) : menuLabel(index);
      if (index == selected) marquee(label, 17, y + 3, 94, 1, 1);
      else textAt(17, y + 3, label.substring(0, 15));
      const int action = index - (mode == MODE_SD ? 1 : 0);
      const int icon = songs || action < 0 ? 5 : action == 0 ? 3 : action == 1 ? 4 :
                       action == 2 ? (mode == MODE_SD ? 1 : 0) : 6;
      drawIcon(icon, 3, y + 2);
      if (songs && index == currentTrack) oled.fillCircle(115, y + 6, 1, SSD1306_WHITE);
    }
    // Invert the capsule after drawing: labels stay legible as it glides past them.
    const int focusY = fromY + int((toY - fromY) * easeOut(millis() - movedAt, 150));
    oled.fillRoundRect(0, focusY, 120, 13, 3, SSD1306_INVERSE);
    const int thumb = max(5, 40 * min(3, count) / count);
    const int thumbY = 14 + (count <= 1 ? 0 : selected * (40 - thumb) / (count - 1));
    oled.drawFastVLine(125, 14, 40, SSD1306_WHITE);
    oled.fillRect(124, thumbY, 3, thumb, SSD1306_WHITE);
  }
  textAt(3, 57, "Click:OK   Hold:Back");
}

void drawPlayer() {
  const bool connected = mode == MODE_BT && bluetooth.is_connected();
  const bool playing = audioOK && (mode == MODE_SD ? songFile && !paused :
                        connected && bluetooth.get_audio_state() == ESP_A2D_AUDIO_STATE_STARTED);
  drawIcon(mode == MODE_SD ? 0 : 1, 1, 0);
  textAt(14, 2, mode == MODE_SD ? "SD" : "BT");
  if (mode == MODE_BT) {
    if (connected) oled.fillCircle(32, 5, 2, SSD1306_WHITE);
    else oled.drawCircle(32, 5, 2, SSD1306_WHITE);
  }
  drawIcon(2, 57, 0);
  rightText(99, 2, String(volumePercent));
  if (!volumePercent) oled.drawLine(57, 0, 66, 9, SSD1306_WHITE);
  if (activeUntil(volumeHighlightUntil)) oled.fillRoundRect(53, 0, 49, 11, 3, SSD1306_INVERSE);
  drawBattery();
  oled.drawFastHLine(0, 12, 128, SSD1306_WHITE);
  String title, detail;
  if (!audioOK) { title = "Audio error"; detail = "Check Serial Monitor"; }
  else if (mode == MODE_BT) {
    title = connected ? (btTitle.length() ? btTitle : "Phone audio") : "Pair phone";
    detail = connected ? (btArtist.length() ? btArtist : playing ? "Bluetooth streaming" : "Paused / ready") : BT_NAME;
  } else if (!sdOK || !trackCount) {
    title = sdOK ? "No songs" : "No SD card";
    detail = "FAT32 / MP3 in root";
  } else {
    title = songTitle(currentTrack);
    detail = String(paused ? "PAUSED  " : "TRACK   ") + (currentTrack + 1) + " / " + trackCount;
  }
  marquee(title, 2, 19, 124, 2, 0);
  textAt(2, 37, detail.substring(0, 21));
  const bool feedback = activeUntil(transportUntil);
  drawIcon(3, 25, 49);
  drawIcon(4, 92, 49);
  if (feedback && transportAction) {
    const int x = transportAction < 0 ? 20 : 87;
    const int width = 21 + int(4 * (1.0f - easeOut(350 - (transportUntil - millis()), 350)));
    oled.drawRoundRect(x, 46, width, 15, 4, SSD1306_WHITE);
  }
  oled.fillCircle(64, 53, buttonDown ? 8 : 9, SSD1306_WHITE);
  if (playing) {
    oled.fillRect(61, 49, 2, 9, SSD1306_BLACK);
    oled.fillRect(66, 49, 2, 9, SSD1306_BLACK);
  } else oled.fillTriangle(62, 49, 62, 57, 68, 53, SSD1306_BLACK);
  // Pending click dots give feedback during the 300 ms multi-click window.
  for (uint8_t i = 0; i < button.clicks; ++i) oled.fillCircle(45 + i * 4, 53, 1, SSD1306_WHITE);
  if (mode == MODE_SD && songFile && songFile.size()) {
    const int progress = (uint64_t(songFile.position()) * 128) / songFile.size();
    oled.drawFastHLine(0, 63, progress, SSD1306_WHITE);
  }
}

// Partial SSD1306 updates keep title scrolling/volume feedback off the full-frame
// 1024-byte transfer path. Each range is independently addressed for safe retry.
void flushOLED() {
  static uint8_t sent[1024];
  static bool sentOnce = false;
  uint8_t *buffer = oled.getBuffer();
  for (int page = 0; page < 8; ++page) {
    int first = 0, last = 127;
    if (sentOnce) {
      while (first < 128 && sent[page * 128 + first] == buffer[page * 128 + first]) ++first;
      if (first == 128) continue;
      while (last > first && sent[page * 128 + last] == buffer[page * 128 + last]) --last;
    }
    Wire.beginTransmission(oledAddress);
    const uint8_t commands[] = {0x00, SSD1306_COLUMNADDR, uint8_t(first), uint8_t(last),
                              SSD1306_PAGEADDR, uint8_t(page), uint8_t(page)};
    Wire.write(commands, sizeof(commands));
    if (Wire.endTransmission() != 0) { sentOnce = false; return; }
    for (int column = first; column <= last; column += 31) {
      const int bytes = min(31, last - column + 1);
      Wire.beginTransmission(oledAddress);
      Wire.write(uint8_t(0x40));
      Wire.write(buffer + page * 128 + column, bytes);
      if (Wire.endTransmission() != 0) { sentOnce = false; return; }
    }
    memcpy(sent + page * 128 + first, buffer + page * 128 + first, last - first + 1);
  }
  sentOnce = true;
}

void drawScreen() {
  if (!oledOK) return;
  oled.clearDisplay();
  if (screen == PLAYER) drawPlayer();
  else drawList(screen == TRACK_LIST);
  if (screen == PLAYER) drawVolumePopup();
  if (notice.length() && activeUntil(noticeUntil)) {
    oled.fillRoundRect(1, 46, 126, 17, 4, SSD1306_WHITE);
    textAt(4, 51, notice.substring(0, 20), 1, SSD1306_BLACK);
  }
  if (transitioning) {
    const uint32_t elapsed = millis() - transitionStarted;
    if (elapsed >= TRANSITION_MS) transitioning = false;
    else {
      uint8_t target[1024];
      uint8_t *buffer = oled.getBuffer();
      memcpy(target, buffer, sizeof(target));
      const int distance = int(128 * easeOut(elapsed, TRANSITION_MS));
      for (int page = 0; page < 8; ++page) {
        for (int x = 0; x < 128; ++x) {
          const int oldX = x + transitionDirection * distance;
          const int newX = x - transitionDirection * (128 - distance);
          buffer[page * 128 + x] = oldX >= 0 && oldX < 128 ?
              transitionFrame[page * 128 + oldX] : target[page * 128 + newX];
        }
      }
    }
  }
  flushOLED();
  displayDirty = false;
}

void onMetadata(uint8_t attribute, const uint8_t *value) {
  if (!value) return;
  portENTER_CRITICAL(&metadataMux);
  if (attribute == ESP_AVRC_MD_ATTR_TITLE) strlcpy(pendingTitle, (const char *)value, sizeof(pendingTitle));
  else if (attribute == ESP_AVRC_MD_ATTR_ARTIST) strlcpy(pendingArtist, (const char *)value, sizeof(pendingArtist));
  metadataChanged = true;
  portEXIT_CRITICAL(&metadataMux);
}

void onBTConnection(esp_a2d_connection_state_t state, void *context) {
  if (state != ESP_A2D_CONNECTION_STATE_DISCONNECTED) return;
  portENTER_CRITICAL(&metadataMux);
  pendingTitle[0] = pendingArtist[0] = 0;
  metadataChanged = true;
  portEXIT_CRITICAL(&metadataMux);
}

void updateMetadata() {
  char title[sizeof(pendingTitle)], artist[sizeof(pendingArtist)];
  portENTER_CRITICAL(&metadataMux);
  const bool changed = metadataChanged;
  if (changed) {
    memcpy(title, pendingTitle, sizeof(title));
    memcpy(artist, pendingArtist, sizeof(artist));
    metadataChanged = false;
  }
  portEXIT_CRITICAL(&metadataMux);
  if (changed) {
    btTitle = displayText(String(title));
    btArtist = displayText(String(artist));
    displayDirty = true;
  }
}

void setupOLED() {
  Wire.begin(OLED_SDA, OLED_SCL);
  uint8_t address = 0;
  for (uint8_t candidate : {uint8_t(0x3C), uint8_t(0x3D)}) {
    Wire.beginTransmission(candidate);
    if (Wire.endTransmission() == 0) { address = candidate; break; }
  }
  if (!address) {
    Serial.println("OLED not found at 0x3C/0x3D; continuing without display.");
    return;
  }
  oledAddress = address;
  oledOK = oled.begin(SSD1306_SWITCHCAPVCC, address, true, false);
  oled.setTextWrap(false);
  Wire.setClock(400000);
  Serial.printf("OLED 0x%02X: %s\n", address, oledOK ? "OK" : "FAILED");
}

void scanSD() {
  SPI.begin(SD_CLK, SD_MISO, SD_MOSI, SD_CS);
  sdOK = SD.begin(SD_CS, SPI, 4000000);
  if (!sdOK) { showNotice("SD mount failed"); return; }
  File root = SD.open("/");
  if (!root || !root.isDirectory()) {
    sdOK = false;
    showNotice("SD root failed");
    return;
  }
  bool limited = false;
  while (true) {
    File entry = root.openNextFile();
    if (!entry) break;
    String name = entry.name();
    String lower = name;
    lower.toLowerCase();
    if (!entry.isDirectory() && lower.endsWith(".mp3") && !shortName(name).startsWith(".")) {
      if (trackCount < MAX_TRACKS) {
        tracks[trackCount++] = name.startsWith("/") ? name : "/" + name;
      } else {
        limited = true;
      }
    }
    entry.close();
  }
  root.close();
  for (int i = 1; i < trackCount; ++i) {
    String key = tracks[i];
    int j = i - 1;
    while (j >= 0 && tracks[j].compareTo(key) > 0) {
      tracks[j + 1] = tracks[j];
      --j;
    }
    tracks[j + 1] = key;
  }
  Serial.printf("SD ready: %d MP3 file(s)%s\n", trackCount, limited ? " (list limited)" : "");
}

// Avoid feeding ID3v2 cover art / metadata into the MP3 decoder.
void skipID3(File &file) {
  uint8_t header[10];
  if (file.read(header, sizeof(header)) == sizeof(header) &&
      header[0] == 'I' && header[1] == 'D' && header[2] == '3' &&
      !(header[6] & 0x80) && !(header[7] & 0x80) &&
      !(header[8] & 0x80) && !(header[9] & 0x80)) {
    const uint32_t size = (uint32_t(header[6]) << 21) |
                          (uint32_t(header[7]) << 14) |
                          (uint32_t(header[8]) << 7) | header[9];
    const uint32_t footer = (header[3] == 4 && (header[5] & 0x10)) ? 10 : 0;
    if (uint64_t(size) + 10 + footer < file.size()) {
      file.seek(size + 10 + footer);
      return;
    }
  }
  file.seek(0);
}

bool startTrack(int index) {
  if (!audioOK || !sdOK || !decoded || index < 0 || index >= trackCount) return false;
  copier.end();
  decoded->end();
  if (songFile) songFile.close();
  paused = true;
  currentTrack = -1;
  songFile = SD.open(tracks[index], FILE_READ);
  if (!songFile) { showNotice("Cannot open MP3"); return false; }
  skipID3(songFile);
  if (!decoded->begin()) {
    songFile.close();
    showNotice("MP3 decoder failed");
    return false;
  }
  sdVolume.setVolume(volumePercent / 100.0f);
  copier.begin(*decoded, songFile);
  currentTrack = index;
  if (screen != TRACK_LIST) selectedTrack = index;
  marqueeStarted[0] = millis();
  paused = false;
  displayDirty = true;
  Serial.print("Playing: ");
  Serial.println(tracks[index]);
  return true;
}

void applyVolume() {
  if (mode == MODE_BT) {
    bluetooth.set_volume((volumePercent * 127 + 50) / 100);
  } else {
    sdVolume.setVolume(volumePercent / 100.0f);
  }
  volumeDirty = true;
  lastVolumeChange = millis();
  volumeHighlightUntil = millis() + 1100;
  noticeUntil = 0;
  displayDirty = true;
}

// Reboot releases the previous Bluetooth/MP3 pipeline and its RAM completely.
void switchMode(uint8_t target) {
  if (target == mode) { setScreen(PLAYER); return; }
  if (!settingsOK) { showNotice("Settings unavailable"); return; }
  settings.putUChar("mode", target);
  settings.putUChar("volume", volumePercent);
  if (mode == MODE_SD && currentTrack >= 0) settings.putString("song", tracks[currentTrack]);
  showNotice(target == MODE_SD ? "Restart -> SD" : "Restart -> Bluetooth");
  drawScreen();
  delay(250);
  ESP.restart();
}

void togglePlayback() {
  if (mode == MODE_BT && audioOK) {
    if (!bluetooth.is_connected()) showNotice("Connect phone first");
    else if (bluetooth.get_audio_state() == ESP_A2D_AUDIO_STATE_STARTED) bluetooth.pause();
    else bluetooth.play();
  } else if (songFile && currentTrack >= 0) {
    paused = !paused;
  } else if (mode == MODE_SD && trackCount) {
    startTrack(selectedTrack);
  }
  displayDirty = true;
}

void changeTrack(int movement) {
  if (!movement || !audioOK) return;
  transportAction = movement > 0 ? 1 : -1;
  transportUntil = millis() + 350;
  if (mode == MODE_SD) {
    if (!trackCount) { showNotice("No songs on SD"); return; }
    startTrack(((currentTrack + movement) % trackCount + trackCount) % trackCount);
  } else if (bluetooth.is_connected()) {
    if (movement > 0) bluetooth.next();
    else bluetooth.previous();

  } else {
    showNotice("Connect phone first");
  }
  displayDirty = true;
}

void shortPress() {
  if (screen == MENU) {
    if (mode == MODE_SD && menuIndex == 0) {
      selectedTrack = currentTrack >= 0 ? currentTrack : 0;
      setScreen(TRACK_LIST);
    } else {
      const int action = menuIndex - (mode == MODE_SD ? 1 : 0);
      if (action == 0 || action == 1) {
        changeTrack(action == 0 ? -1 : 1);
        setScreen(PLAYER);
      } else if (action == 2) switchMode(mode == MODE_SD ? MODE_BT : MODE_SD);
      else setScreen(PLAYER);
    }
  } else if (screen == TRACK_LIST) {
    if (startTrack(selectedTrack)) setScreen(PLAYER);
  } else {
    togglePlayback();
  }
  volumeHighlightUntil = 0;
  displayDirty = true;
}

void dispatchClicks() {
  const uint8_t clicks = button.clicks;
  button.clicks = 0;
  if (screen != PLAYER || !clicks) return;
  if (clicks == 1) togglePlayback();
  else changeTrack(clicks == 2 ? 1 : -1);
  displayDirty = true;
}

void handleButton() {
  const uint32_t now = millis();
  const bool raw = digitalRead(ENC_SW);
  if (raw != button.lastRaw) { button.lastRaw = raw; button.changedAt = now; }
  // Wait for raw HIGH as well: a second press inside its debounce interval
  // must not accidentally dispatch the first click.
  if (!buttonDown && raw == HIGH && button.clicks && now - button.releasedAt >= CLICK_GAP_MS)
    dispatchClicks();
  if ((raw == LOW) != buttonDown && now - button.changedAt >= 25) {
    buttonDown = raw == LOW;
    lastInteraction = now;
    if (buttonDown) {
      hideVolumePopup();
      button.pressedAt = now;
      button.holdHandled = false;
      button.origin = screen;
    } else if (!button.holdHandled && button.origin == screen) {
      if (screen == PLAYER) {
        ++button.clicks;
        button.releasedAt = now;
        if (button.clicks >= 3) dispatchClicks();
      } else shortPress(); // Immediate confirmation in menus.
    }
    displayDirty = true;
  }
  if (buttonDown && raw == LOW && !button.holdHandled && now - button.pressedAt >= HOLD_MS) {
    button.holdHandled = true;
    button.clicks = 0;
    if (screen == TRACK_LIST) setScreen(MENU);
    else if (screen == MENU) setScreen(PLAYER);
    else { menuIndex = 0; setScreen(MENU); }
    lastInteraction = now;
  }
}

void handleEncoder() {
  portENTER_CRITICAL(&encoderMux);
  const int steps = encoderTicks / ENC_TRANSITIONS;
  encoderTicks -= steps * ENC_TRANSITIONS;
  portEXIT_CRITICAL(&encoderMux);
  const int movement = steps * ENC_DIRECTION;
  if (!movement || buttonDown) return;
  const uint32_t now = millis();
  lastInteraction = now;
  if (screen == MENU) {
    const int count = menuCount();
    menuIndex = ((menuIndex + movement) % count + count) % count;
  } else if (screen == TRACK_LIST && trackCount) {
    selectedTrack = ((selectedTrack + movement) % trackCount + trackCount) % trackCount;
  } else if (screen == PLAYER && audioOK) {
    static uint32_t lastTurn = 0;
    const int increment = lastTurn && now - lastTurn < 65 ? 4 : 2;
    lastTurn = now;
    const int previousVolume = volumePercent;
    volumePercent = constrain(volumePercent + movement * increment, 0, 100);
    const bool limitHit = (previousVolume == 100 && movement > 0) ||
                          (previousVolume == 0 && movement < 0);
    if (volumePercent != previousVolume) applyVolume();
    showVolumePopup(previousVolume, limitHit, movement);
  }
  displayDirty = true;
}

void handleSerial() {
  while (Serial.available()) {
    const char key = Serial.read();
    if (key == 'b') switchMode(MODE_BT);
    else if (key == 's') switchMode(MODE_SD);
    else if (key == 'p') togglePlayback();
    else if (key == 'n') changeTrack(1);
    else if (key == 'v') changeTrack(-1);
    else if (key == 't') printBatteryStatus();
    else if (key == 'l') {
      Serial.printf("Mode: %s | volume: %d%% | SD tracks: %d\n",
                    mode == MODE_SD ? "SD" : "Bluetooth", volumePercent, trackCount);
      for (int i = 0; i < trackCount; ++i) Serial.println(tracks[i]);
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\nESP32 SD / Bluetooth player");
  Serial.println("Serial: s=SD, b=Bluetooth, p=play/pause, n=next, v=previous, l=list, t=battery");
  Serial.println("Encoder: turn=volume, 1/2/3 clicks=play-pause/next/previous, hold=menu");
  settingsOK = settings.begin("music-player", false);
  if (settingsOK) {
    mode = settings.getUChar("mode", MODE_SD) == MODE_BT ? MODE_BT : MODE_SD;
    volumePercent = constrain(int(settings.getUChar("volume", 35)), 0, 100);
  }
  pinMode(ENC_CLK, INPUT_PULLUP);
  pinMode(ENC_DT, INPUT_PULLUP);
  pinMode(ENC_SW, INPUT_PULLUP);
  encoderPrevious = (digitalRead(ENC_CLK) << 1) | digitalRead(ENC_DT);
  attachInterrupt(digitalPinToInterrupt(ENC_CLK), encoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_DT), encoderISR, CHANGE);
  pinMode(BATTERY_ADC_PIN, INPUT); // GPIO34 has no internal pull-up/down.
  analogReadResolution(12);
  analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);
  updateBattery(true); // ADC setup/calibration happens before audio starts.
  setupOLED();

  auto config = audioOutput.defaultConfig(TX_MODE);
  config.pin_bck = I2S_BCK;
  config.pin_ws = I2S_LRCK;
  config.pin_data = I2S_DATA;
  config.sample_rate = 44100;
  config.bits_per_sample = 16;
  config.channels = 2;
  // Enough queued PCM to cover occasional OLED refreshes.
  config.buffer_count = 8;
  config.buffer_size = 512;
  audioOK = audioOutput.begin(config);
  if (!audioOK) { showNotice("I2S init failed"); drawScreen(); return; }

  if (mode == MODE_BT) {
    bluetooth.set_auto_reconnect(false);
    bluetooth.set_avrc_metadata_attribute_mask(ESP_AVRC_MD_ATTR_TITLE | ESP_AVRC_MD_ATTR_ARTIST);
    bluetooth.set_avrc_metadata_callback(onMetadata);
    bluetooth.set_on_connection_state_changed(onBTConnection);
    bluetooth.start(BT_NAME);
    bluetooth.set_volume((volumePercent * 127 + 50) / 100);
    Serial.print("Bluetooth name: ");
    Serial.println(BT_NAME);
  } else {
    auto volumeConfig = sdVolume.defaultConfig();
    volumeConfig.sample_rate = 44100;
    volumeConfig.volume = volumePercent / 100.0f;
    sdVolume.begin(volumeConfig);
    mp3 = new audio_tools::MP3DecoderHelix();
    decoded = new EncodedAudioStream(&sdVolume, mp3);
    // The codec wrapper forwards format changes through VolumeStream to I2S.
    scanSD();
    if (trackCount) {
      int resume = 0;
      const String savedSong = settingsOK ? settings.getString("song", "") : "";
      for (int i = 0; i < trackCount; ++i) if (tracks[i] == savedSong) { resume = i; break; }
      startTrack(resume);
    }
  }
  drawScreen();
}

void loop() {
  handleButton();
  handleEncoder();
  handleSerial();
  updateMetadata();
  updateBattery();
  if (audioOK && mode == MODE_SD && songFile && !paused) {
    copier.copy();
    if (!songFile.available()) {
      const int next = (currentTrack + 1) % trackCount;
      startTrack(next);
    }
  } else {
    delay(1);
  }
  const uint32_t now = millis();
  static uint32_t lastBluetoothPoll = 0;
  if (audioOK && mode == MODE_BT && now - lastBluetoothPoll >= 250) {
    lastBluetoothPoll = now;
    // Reflect volume changes made on the phone in the OLED and saved setting.
    const int remotePercent = (bluetooth.get_volume() * 100 + 63) / 127;
    if (remotePercent != volumePercent) {
      volumePercent = remotePercent;
      volumeDirty = true;
      lastVolumeChange = now;
      volumeHighlightUntil = now + 900;
      displayDirty = true;
    }
  }
  if (volumeDirty && now - lastVolumeChange >= 2000) {
    if (settingsOK) settings.putUChar("volume", volumePercent);
    volumeDirty = false;
  }
  static uint32_t lastDraw = 0;
  if (screen == MENU && !buttonDown && now - lastInteraction >= 20000) {
    setScreen(PLAYER);
  }
  // Render up to 25 fps; the display retains unchanged pages/columns.
  if (now - lastDraw >= UI_FRAME_MS) {
    drawScreen();
    lastDraw = now;
  }
}
