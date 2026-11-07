#include <Arduino.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <driver/uart.h>
#include <esp_task_wdt.h>
#include <soc/gpio_struct.h>
#include <string.h>
#include "secrets.h"

const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;

// Panel TX -> 16, panel RX <- 17. Main TX -> 18, main RX <- 15.
static const int kPanelRxPin = 16;
static const int kPanelTxPin = 17;
static const int kMainRxPin = 18;
static const int kMainTxPin = 15;
// UART is receive-only. These pins are not wired to the dryer.
static const int kPanelUartTxPin = 21;
static const int kMainUartTxPin = 14;
static const uint32_t kBitUs = 208;
static const uint16_t kTelnetPort = 23;
static const uint32_t kBurstGapMs = 30;
static const uint32_t kFlashMs = 140;

HardwareSerial PanelSerial(1);
HardwareSerial MainSerial(2);
WiFiServer telnet(kTelnetPort);
WiFiClient client;
WebServer web(80);

bool serverStarted = false;
bool wifiStarted = false;
uint32_t flashUntil = 0;
int flashKind = 0;
int ledMode = -1;
char cmd[32];
size_t cmdLen = 0;

uint8_t lastMain[9];
bool hasLastMain = false;
bool panelRxInvert = false;
bool panelTxInvert = false;
bool mainRxInvert = false;
bool mainTxInvert = false;
uint32_t panelBaud = 4800;
uint32_t mainBaud = 4800;
volatile bool passThrough = true;

struct DryerCommand {
  const char* name;
  uint8_t len;
  uint8_t repeats;
  uint8_t frame[9];
};

const DryerCommand kCommands[] = {
    {"on", 8, 1, {0x00, 0xE9, 0xAF, 0xA9, 0xA9, 0xB9, 0xA9, 0xB9}},
    {"off", 8, 1, {0x00, 0xE9, 0xAF, 0xA9, 0xA9, 0xB9, 0xA9, 0xB9}},
    {"humidity", 8, 1, {0x00, 0xE9, 0xAF, 0xA9, 0xA9, 0xAF, 0xA9, 0xAF}},
    {"fan", 9, 1, {0x00, 0xE9, 0xAF, 0xA9, 0xE9, 0xA9, 0xE9, 0xE9, 0xF9}},
    {"sleep", 9, 1, {0x00, 0xE9, 0xAF, 0xA9, 0xB9, 0xA9, 0xE9, 0xAF, 0xF9}},
    {"timer", 9, 1, {0x00, 0xE9, 0xAF, 0xA9, 0xA9, 0xE9, 0xA9, 0xEF, 0xF9}},
};

struct Burst {
  const char* name;
  HardwareSerial* in;
  int flashKind;
  uint8_t buf[160];
  size_t len;
  uint32_t lastMs;
};

Burst panelBurst = {"panel", &PanelSerial, 2, {}, 0, 0};
Burst mainBurst = {"main", &MainSerial, 3, {}, 0, 0};

// The UART bytes are only a reading of the pulses. Web On replays the last
// panel On burst edge for edge.
struct PulseEdge {
  uint16_t dt;
  uint8_t level;
};
static const uint16_t kEdgeMax = 512;
static PulseEdge edgeSlots[2][kEdgeMax];
static volatile uint16_t edgeLen[2] = {0, 0};
static volatile uint8_t edgeWrite = 0;
static volatile uint8_t edgeReadySlot = 0;
static volatile uint32_t edgeReadyMs = 0;
static PulseEdge onWave[kEdgeMax];
static uint16_t onWaveLen = 0;
static volatile bool onWaveReady = false;
static volatile uint32_t blockPanelUntil = 0;
static volatile bool replayReq = false;
static volatile bool replayBusy = false;
static volatile bool replayDone = false;
static const PulseEdge* volatile replayEdges = nullptr;
static volatile uint16_t replayLen = 0;
static volatile uint8_t mainRaw[9];
static volatile uint32_t mainRawSeq = 0;
static char panelText[8] = "--";
static char mainHex[32] = "";
static Preferences waveStore;

// Measured panel On burst. Replaying these 54 edges toggles the fan.
static const PulseEdge kOnEdges[] = {
    {0, 0}, {15, 1}, {109, 0}, {4381, 1}, {2254, 0}, {249, 1},
    {252, 0}, {249, 1}, {251, 0}, {250, 1}, {752, 0}, {260, 1},
    {741, 0}, {275, 1}, {226, 0}, {250, 1}, {251, 0}, {250, 1},
    {250, 0}, {250, 1}, {251, 0}, {250, 1}, {251, 0}, {250, 1},
    {251, 0}, {249, 1}, {251, 0}, {250, 1}, {251, 0}, {250, 1},
    {251, 0}, {250, 1}, {251, 0}, {249, 1}, {252, 0}, {249, 1},
    {752, 0}, {250, 1}, {752, 0}, {249, 1}, {251, 0}, {275, 1},
    {226, 0}, {250, 1}, {252, 0}, {248, 1}, {752, 0}, {250, 1},
    {251, 0}, {250, 1}, {752, 0}, {249, 1}, {752, 0}, {250, 1},
};
static const uint16_t kOnEdgeCount = sizeof(kOnEdges) / sizeof(kOnEdges[0]);

// Panel Humidity. Frame 00 E9 AF A9 A9 AF A9 AF.
static const PulseEdge kHumidityEdges[] = {
    {0, 0}, {17, 1}, {108, 0}, {4381, 1}, {2254, 0}, {250, 1},
    {251, 0}, {250, 1}, {251, 0}, {249, 1}, {752, 0}, {262, 1},
    {740, 0}, {274, 1}, {226, 0}, {250, 1}, {251, 0}, {250, 1},
    {251, 0}, {250, 1}, {251, 0}, {250, 1}, {250, 0}, {250, 1},
    {251, 0}, {250, 1}, {251, 0}, {250, 1}, {250, 0}, {251, 1},
    {250, 0}, {250, 1}, {251, 0}, {250, 1}, {752, 0}, {250, 1},
    {250, 0}, {250, 1}, {752, 0}, {250, 1}, {251, 0}, {274, 1},
    {226, 0}, {250, 1}, {251, 0}, {250, 1}, {751, 0}, {250, 1},
    {752, 0}, {250, 1}, {251, 0}, {249, 1}, {252, 0}, {250, 1},
};
static const uint16_t kHumidityEdgeCount = sizeof(kHumidityEdges) / sizeof(kHumidityEdges[0]);

// Panel Fan, pressed twice as the same frame 00 E9 AF A9 E9 A9 E9 E9 F9.
static const PulseEdge kFanEdges[] = {
    {0, 0}, {17, 1}, {108, 0}, {4381, 1}, {2254, 0}, {250, 1},
    {251, 0}, {250, 1}, {251, 0}, {250, 1}, {751, 0}, {267, 1},
    {735, 0}, {274, 1}, {226, 0}, {250, 1}, {251, 0}, {250, 1},
    {251, 0}, {250, 1}, {251, 0}, {249, 1}, {252, 0}, {249, 1},
    {251, 0}, {250, 1}, {251, 0}, {250, 1}, {251, 0}, {249, 1},
    {752, 0}, {250, 1}, {251, 0}, {250, 1}, {251, 0}, {249, 1},
    {251, 0}, {250, 1}, {752, 0}, {250, 1}, {251, 0}, {274, 1},
    {226, 0}, {250, 1}, {752, 0}, {250, 1}, {250, 0}, {250, 1},
    {251, 0}, {250, 1}, {752, 0}, {249, 1}, {251, 0}, {250, 1},
};
static const uint16_t kFanEdgeCount = sizeof(kFanEdges) / sizeof(kFanEdges[0]);

// Panel Sleep. Frame 00 E9 AF A9 B9 A9 E9 AF F9.
static const PulseEdge kSleepEdges[] = {
    {0, 0}, {21, 1}, {104, 0}, {4382, 1}, {2253, 0}, {250, 1},
    {251, 0}, {250, 1}, {251, 0}, {250, 1}, {751, 0}, {262, 1},
    {740, 0}, {275, 1}, {226, 0}, {250, 1}, {251, 0}, {249, 1},
    {252, 0}, {249, 1}, {251, 0}, {250, 1}, {251, 0}, {250, 1},
    {251, 0}, {249, 1}, {251, 0}, {250, 1}, {752, 0}, {250, 1},
    {251, 0}, {249, 1}, {252, 0}, {249, 1}, {251, 0}, {250, 1},
    {251, 0}, {250, 1}, {752, 0}, {249, 1}, {251, 0}, {275, 1},
    {226, 0}, {250, 1}, {752, 0}, {250, 1}, {751, 0}, {250, 1},
    {251, 0}, {250, 1}, {751, 0}, {250, 1}, {251, 0}, {250, 1},
};
static const uint16_t kSleepEdgeCount = sizeof(kSleepEdges) / sizeof(kSleepEdges[0]);

// Panel Timer. Frame 00 E9 AF A9 A9 E9 A9 EF F9. One press, one train.
static const PulseEdge kTimerEdges[] = {
    {0, 0}, {20, 1}, {106, 0}, {4381, 1}, {2254, 0}, {250, 1},
    {250, 0}, {250, 1}, {251, 0}, {250, 1}, {752, 0}, {261, 1},
    {740, 0}, {275, 1}, {226, 0}, {250, 1}, {251, 0}, {249, 1},
    {251, 0}, {250, 1}, {251, 0}, {250, 1}, {251, 0}, {250, 1},
    {251, 0}, {249, 1}, {251, 0}, {250, 1}, {251, 0}, {250, 1},
    {252, 0}, {249, 1}, {751, 0}, {250, 1}, {251, 0}, {250, 1},
    {251, 0}, {250, 1}, {751, 0}, {250, 1}, {251, 0}, {275, 1},
    {226, 0}, {250, 1}, {251, 0}, {250, 1}, {751, 0}, {250, 1},
    {752, 0}, {249, 1}, {752, 0}, {250, 1}, {251, 0}, {250, 1},
};
static const uint16_t kTimerEdgeCount = sizeof(kTimerEdges) / sizeof(kTimerEdges[0]);

void paintLed() {
  bool connected = client && client.connected();
  bool flashing = (int32_t)(flashUntil - millis()) > 0;
  int next = flashing ? flashKind : (connected ? 1 : 0);
  if (next == ledMode) {
    return;
  }
  ledMode = next;
  if (next == 4) {
    neopixelWrite(RGB_BUILTIN, 48, 0, 0);
  } else if (next == 3) {
    neopixelWrite(RGB_BUILTIN, 0, 32, 40);
  } else if (next == 2) {
    neopixelWrite(RGB_BUILTIN, 48, 32, 0);
  } else if (next == 1) {
    neopixelWrite(RGB_BUILTIN, 0, 40, 0);
  } else {
    neopixelWrite(RGB_BUILTIN, 0, 0, 40);
  }
}

void pulse(int kind) {
  flashKind = kind;
  flashUntil = millis() + kFlashMs;
  ledMode = -1;
  paintLed();
}

// Frames are kept from boot and dumped when Telnet connects.
char* logBuf = nullptr;
size_t logCap = 0;
size_t logHead = 0;
size_t logSize = 0;
bool replayOn = false;
size_t replayOff = 0;
size_t replayLeft = 0;

void printHelp();

void startLog() {
  if (psramFound()) {
    logCap = 256 * 1024;
    logBuf = (char*)ps_malloc(logCap);
  }
  if (!logBuf) {
    logCap = 24 * 1024;
    logBuf = (char*)malloc(logCap);
  }
  if (!logBuf) {
    logCap = 0;
  }
}

void appendLog(const char* line, size_t n) {
  if (!logBuf) {
    return;
  }
  for (size_t i = 0; i < n; i++) {
    if (logSize == logCap && replayOn && logHead == replayOff) {
      return;
    }
    logBuf[logHead] = line[i];
    logHead = (logHead + 1) % logCap;
    if (logSize < logCap) {
      logSize++;
    }
    if (replayOn) {
      replayLeft++;
    }
  }
}

void logLine(const char* line, size_t n) {
  Serial.write(line, n);
  appendLog(line, n);
  if (!replayOn && client && client.connected()) {
    client.write(line, n);
  }
}

void beginReplay() {
  if (!logBuf || logSize == 0) {
    client.println("replay empty");
    printHelp();
    return;
  }
  replayOff = (logHead + logCap - logSize) % logCap;
  replayLeft = logSize;
  replayOn = true;
  client.printf("replay %u bytes%s\n", (unsigned)logSize, logSize == logCap ? ", oldest dropped" : "");
}

void pumpReplay() {
  if (!replayOn) {
    return;
  }
  if (!client || !client.connected()) {
    replayOn = false;
    return;
  }
  if (replayLeft == 0) {
    replayOn = false;
    client.println("--- live ---");
    printHelp();
    return;
  }
  uint8_t tmp[256];
  size_t n = replayLeft < sizeof(tmp) ? replayLeft : sizeof(tmp);
  for (size_t i = 0; i < n; i++) {
    tmp[i] = logBuf[(replayOff + i) % logCap];
  }
  size_t wrote = client.write(tmp, n);
  if (wrote == 0) {
    return;
  }
  replayOff = (replayOff + wrote) % logCap;
  replayLeft -= wrote;
}

bool samePrefix(const uint8_t* frame, const uint8_t* prefix, size_t n) {
  return memcmp(frame, prefix, n) == 0;
}

const char* frameName(const uint8_t* frame, size_t len) {
  for (size_t i = 0; i < sizeof(kCommands) / sizeof(kCommands[0]); i++) {
    size_t n = kCommands[i].len;
    if (len >= n && samePrefix(frame, kCommands[i].frame, n)) {
      return kCommands[i].name;
    }
  }
  static const uint8_t kStatus[] = {0x00, 0x89, 0xA8, 0xA9, 0xA9, 0x89, 0x29, 0xA1};
  if (len >= 8 && samePrefix(frame, kStatus, 8)) {
    return "status";
  }
  return "";
}

char decodeSeg7(uint8_t v) {
  switch (v & 0x7F) {
    case 0x3F: return '0';
    case 0x06: return '1';
    case 0x5B: return '2';
    case 0x4F: return '3';
    case 0x66: return '4';
    case 0x6D: return '5';
    case 0x7D: return '6';
    case 0x07: return '7';
    case 0x7F: return '8';
    case 0x6F: return '9';
    case 0x76: return 'H';
    case 0x30: return 'I';
    case 0x38: return 'L';
    case 0x79: return 'E';
    case 0x00: return ' ';
    default: return '?';
  }
}

int decodeSeg7Digit(uint8_t v) {
  char c = decodeSeg7(v);
  return (c >= '0' && c <= '9') ? (c - '0') : -1;
}

int decodeDisplayNumber(uint8_t d1, uint8_t d2) {
  if (d1 == 0x00 && d2 == 0x00) {
    return -1;
  }
  int hi = (d1 == 0x00) ? 0 : decodeSeg7Digit(d1);
  int lo = decodeSeg7Digit(d2);
  if (hi < 0 || lo < 0) {
    return -1;
  }
  return hi * 10 + lo;
}

void formatPanelDisplay(uint8_t d1, uint8_t d2, char* out, size_t outLen) {
  char c1 = decodeSeg7(d1);
  char c2 = decodeSeg7(d2);
  if (c1 == ' ' && c2 == ' ') {
    snprintf(out, outLen, "off");
  } else if (c1 == ' ') {
    snprintf(out, outLen, "%c", c2);
  } else {
    snprintf(out, outLen, "%c%c", c1, c2);
  }
}

bool decodeUartDisplay(const uint8_t* frame, size_t len, char* out, size_t outLen) {
  if (len < 10 || frame[0] != 0x00) {
    return false;
  }
  int8_t pat[80];
  size_t nb = 0;
  for (size_t i = 1; i < len && nb + 4 <= sizeof(pat); i++) {
    switch (frame[i]) {
      case 0xEF: pat[nb++] = 1; pat[nb++] = 1; break;
      case 0xE9: pat[nb++] = 0; pat[nb++] = 0; pat[nb++] = 1; break;
      case 0xB9: pat[nb++] = 0; pat[nb++] = 1; pat[nb++] = -1; break;
      case 0xAF: pat[nb++] = 1; pat[nb++] = 0; pat[nb++] = -1; break;
      case 0xA9: pat[nb++] = 0; pat[nb++] = 0; pat[nb++] = 0; pat[nb++] = -1; break;
      case 0xF9: pat[nb++] = 0; pat[nb++] = -1; break;
      default: return false;
    }
  }
  if (nb < 24) {
    return false;
  }
  static const uint8_t kKnown[] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07,
                                   0x7F, 0x6F, 0x00, 0x76, 0x30, 0x38, 0x79};
  auto matchByte = [&](size_t off, uint8_t* matched) -> bool {
    uint8_t found = 0;
    int count = 0;
    for (uint8_t code : kKnown) {
      bool ok = true;
      for (size_t b = 0; b < 8; b++) {
        int8_t want = (code >> (7 - b)) & 1;
        if (pat[off + b] != -1 && pat[off + b] != want) {
          ok = false;
          break;
        }
      }
      if (ok) {
        found = code;
        count++;
      }
    }
    if (count == 1) {
      *matched = found;
      return true;
    }
    return false;
  };
  uint8_t d1 = 0, d2 = 0;
  if (!matchByte(8, &d1) || !matchByte(16, &d2)) {
    return false;
  }
  formatPanelDisplay(d1, d2, out, outLen);
  return true;
}

void describeMainChange(const uint8_t* frame, size_t len, char* out, size_t outLen) {
  out[0] = '\0';
  static uint32_t seenSeq = 0;
  uint32_t seq = mainRawSeq;
  if (seq != 0 && seq != seenSeq) {
    uint8_t raw[9];
    for (int i = 0; i < 9; i++) {
      raw[i] = mainRaw[i];
    }
    seenSeq = seq;
    formatPanelDisplay(raw[1], raw[2], panelText, sizeof(panelText));
    snprintf(mainHex, sizeof(mainHex), "%02X %02X %02X %02X %02X %02X %02X %02X %02X",
             raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7], raw[8]);
    snprintf(out, outLen, " display=%s [%s]", panelText, mainHex);
    return;
  }
  char disp[8];
  if (decodeUartDisplay(frame, len, disp, sizeof(disp))) {
    snprintf(panelText, sizeof(panelText), "%s", disp);
    snprintf(out, outLen, " display=%s", panelText);
    return;
  }
  if (len != 9) {
    return;
  }
  if (!hasLastMain) {
    memcpy(lastMain, frame, 9);
    hasLastMain = true;
    snprintf(out, outLen, " first-status");
    return;
  }
  bool onlyTail = true;
  char diff[80];
  size_t used = 0;
  diff[0] = '\0';
  for (size_t i = 0; i < 8; i++) {
    if (frame[i] == lastMain[i]) {
      continue;
    }
    onlyTail = false;
    int wrote = snprintf(diff + used, sizeof(diff) - used, " b%u:%02X>%02X", (unsigned)i,
                         lastMain[i], frame[i]);
    if (wrote < 0 || used + (size_t)wrote >= sizeof(diff)) {
      break;
    }
    used += (size_t)wrote;
  }
  bool tailToggle = (frame[8] == 0x0E || frame[8] == 0x0F) &&
                    (lastMain[8] == 0x0E || lastMain[8] == 0x0F);
  memcpy(lastMain, frame, 9);
  if (onlyTail && tailToggle) {
    snprintf(out, outLen, " unchanged");
    return;
  }
  snprintf(out, outLen, "%s", diff[0] ? diff : " unchanged");
}

void keepPanelWave(bool storeOn, const uint8_t* frame, size_t len);

bool panelIdle(const uint8_t* frame, size_t len) {
  static const uint8_t head[] = {0x00, 0xE9, 0xAF, 0xA9, 0xA9};
  if (len < 6 || memcmp(frame, head, 5) != 0) {
    return false;
  }
  if (frame[5] != 0xE9 && frame[5] != 0xEB) {
    return false;
  }
  // Timer is 00 E9 AF A9 A9 E9 A9 EF F9. The idle hello ends in B9.
  if (len >= 8 && frame[7] != 0xB9 && frame[7] != 0x39) {
    return false;
  }
  return true;
}

void logFrame(const char* dir, const uint8_t* frame, size_t len) {
  char line[220];
  int n = snprintf(line, sizeof(line), "%s %lu", dir, millis());
  for (size_t i = 0; i < len && n > 0 && n < (int)sizeof(line) - 4; i++) {
    n += snprintf(line + n, sizeof(line) - n, " %02X", frame[i]);
  }
  const char* name = frameName(frame, len);
  if (strcmp(dir, "panel") == 0 && !panelIdle(frame, len)) {
    keepPanelWave(strcmp(name, "on") == 0, frame, len);
    // The panel repeats a step while it is waiting to see the new status.
    // The first train has already been copied. Hold the later ones off the main board.
    if (strcmp(name, "humidity") == 0 || strcmp(name, "fan") == 0 || strcmp(name, "sleep") == 0 ||
        strcmp(name, "timer") == 0) {
      blockPanelUntil = millis() + 2000;
    }
  }
  if (name[0] && n > 0 && n < (int)sizeof(line) - 16) {
    n += snprintf(line + n, sizeof(line) - n, " %s", name);
  }
  if (strcmp(dir, "main") == 0 && n > 0 && n < (int)sizeof(line) - 48) {
    char extra[80];
    describeMainChange(frame, len, extra, sizeof(extra));
    n += snprintf(line + n, sizeof(line) - n, "%s", extra);
  }
  if (n < 0 || n >= (int)sizeof(line) - 2) {
    return;
  }
  line[n++] = '\n';
  line[n] = '\0';
  logLine(line, n);
}

void publishBurst(Burst& burst) {
  size_t i = 0;
  while (i < burst.len && burst.buf[i] == 0xFF) {
    i++;
  }
  while (i < burst.len) {
    if (burst.buf[i] == 0xFF) {
      i++;
      continue;
    }
    size_t start = i;
    i++;
    while (i < burst.len && burst.buf[i] != 0x00 && burst.buf[i] != 0xFF) {
      i++;
    }
    size_t n = i - start;
    if (n >= 4) {
      logFrame(burst.name, burst.buf + start, n);
    }
  }
  burst.len = 0;
}

void takeBytes(Burst& burst) {
  while (burst.in->available() && burst.len < sizeof(burst.buf)) {
    uint8_t byte = burst.in->read();
    burst.buf[burst.len++] = byte;
    burst.lastMs = millis();
  }
  if (burst.len && millis() - burst.lastMs >= kBurstGapMs) {
    pulse(burst.flashKind);
    publishBurst(burst);
  }
}

const char* pol(bool inverted) {
  return inverted ? "inv" : "norm";
}

void describeMode() {
  client.printf("panel %lu rx %s tx %s, main %lu rx %s tx %s, pass %s\n", panelBaud, pol(panelRxInvert),
                pol(panelTxInvert), mainBaud, pol(mainRxInvert), pol(mainTxInvert),
                passThrough ? "on" : "off");
}

void applyLine(uart_port_t port, bool rxInvert, bool txInvert) {
  uint32_t mask = 0;
  if (rxInvert) {
    mask |= UART_SIGNAL_RXD_INV;
  }
  if (txInvert) {
    mask |= UART_SIGNAL_TXD_INV;
  }
  uart_set_line_inverse(port, mask);
}

void applyUarts() {
  PanelSerial.end();
  MainSerial.end();
  delay(10);
  PanelSerial.begin(panelBaud, SERIAL_8N1, kPanelRxPin, kPanelUartTxPin, false);
  MainSerial.begin(mainBaud, SERIAL_8N1, kMainRxPin, kMainUartTxPin, false);
  applyLine(UART_NUM_1, panelRxInvert, panelTxInvert);
  applyLine(UART_NUM_2, mainRxInvert, mainTxInvert);
  panelBurst.len = 0;
  mainBurst.len = 0;
  hasLastMain = false;
}

void printHelp() {
  client.println("panel TX->16  panel RX<-17  main TX->18  main RX<-15");
  describeMode();
  client.println("link: panel|main then 4800|9600|2400 and inv|norm");
  client.println("pass on copies each edge");
  client.println("tx inv|norm sets only a typed command, not the live copy");
  client.println("commands: on, off, humidity, fan, sleep, timer");
  client.println("led blue=idle green=telnet yellow=panel cyan=main red=command");
  client.println("telnet replays frames kept since boot, then live");
}

void holdOutputs() {
  pinMode(kPanelTxPin, OUTPUT);
  pinMode(kMainTxPin, OUTPUT);
  digitalWrite(kPanelTxPin, digitalRead(kMainRxPin));
  digitalWrite(kMainTxPin, digitalRead(kPanelRxPin));
}

void dumpWave(const char* tag, const PulseEdge* edges, uint16_t n) {
  char line[180];
  int wrote = snprintf(line, sizeof(line), "wave %s %u\n", tag, n);
  if (wrote > 0) {
    logLine(line, (size_t)wrote);
  }
  size_t used = 0;
  for (uint16_t i = 0; i < n; i++) {
    int m = snprintf(line + used, sizeof(line) - used, "{%u,%u},", edges[i].dt, edges[i].level);
    if (m < 0 || used + (size_t)m + 2 >= sizeof(line)) {
      if (used > 0) {
        line[used++] = '\n';
        logLine(line, used);
      }
      used = 0;
      m = snprintf(line, sizeof(line), "{%u,%u},", edges[i].dt, edges[i].level);
      if (m < 0) {
        return;
      }
    }
    used += (size_t)m;
  }
  if (used > 0) {
    line[used++] = '\n';
    logLine(line, used);
  }
}

void saveOnWave() {
  if (!waveStore.begin("dryer", false)) {
    return;
  }
  waveStore.putUShort("len", onWaveLen);
  waveStore.putBytes("edges", onWave, onWaveLen * sizeof(PulseEdge));
  waveStore.end();
}

void loadOnWave() {
  if (!waveStore.begin("dryer", true)) {
    return;
  }
  uint16_t n = waveStore.getUShort("len", 0);
  if (n >= 16 && n <= kEdgeMax &&
      waveStore.getBytes("edges", onWave, n * sizeof(PulseEdge)) == n * sizeof(PulseEdge)) {
    onWaveLen = n;
    onWaveReady = true;
  }
  waveStore.end();
  if (!onWaveReady) {
    return;
  }
  char line[48];
  int wrote = snprintf(line, sizeof(line), "loaded on waveform, %u edges\n", onWaveLen);
  if (wrote > 0) {
    logLine(line, (size_t)wrote);
  }
}

void keepPanelWave(bool storeOn, const uint8_t* frame, size_t len) {
  if (replayBusy || !edgeReadySlot || millis() - edgeReadyMs > 1000) {
    return;
  }
  uint8_t slot = edgeReadySlot - 1;
  uint16_t n = edgeLen[slot];
  if (n < 16 || n > kEdgeMax) {
    return;
  }
  if (storeOn) {
    memcpy(onWave, edgeSlots[slot], n * sizeof(PulseEdge));
    onWaveLen = n;
    onWaveReady = true;
    saveOnWave();
  }
  char tag[40];
  if (storeOn) {
    snprintf(tag, sizeof(tag), "on");
  } else {
    size_t used = 0;
    for (size_t i = 0; i < len && used + 2 < sizeof(tag); i++) {
      int m = snprintf(tag + used, sizeof(tag) - used, "%02X", frame[i]);
      if (m < 0) {
        break;
      }
      used += (size_t)m;
    }
  }
  edgeReadySlot = 0;
  dumpWave(tag, storeOn ? onWave : edgeSlots[slot], n);
}

void IRAM_ATTR playOnWave(uint32_t mainOut) {
  const PulseEdge* edges = replayEdges;
  uint16_t n = replayLen;
  for (uint16_t i = 0; i < n; i++) {
    if (edges[i].dt) {
      delayMicroseconds(edges[i].dt);
    }
    if (edges[i].level) {
      GPIO.out_w1ts = mainOut;
    } else {
      GPIO.out_w1tc = mainOut;
    }
    if ((i & 0x3F) == 0) {
      esp_task_wdt_reset();
    }
  }
  GPIO.out_w1ts = mainOut;
}

void IRAM_ATTR mirrorLoop() {
  const uint32_t panelIn = 1u << kPanelRxPin;
  const uint32_t mainIn = 1u << kMainRxPin;
  const uint32_t panelOut = 1u << kPanelTxPin;
  const uint32_t mainOut = 1u << kMainTxPin;
  uint32_t spins = 0;
  uint32_t prevLevel = (GPIO.in & panelIn) ? 1 : 0;
  uint32_t prevUs = micros();
  uint16_t edgeCount = 0;
  uint32_t mainPrevLevel = (GPIO.in & mainIn) ? 1 : 0;
  uint32_t mainPrevUs = prevUs;
  uint8_t mainBytes[9];
  int8_t mainBitPos = -1;
  while (passThrough) {
    if (replayReq) {
      playOnWave(mainOut);
      replayReq = false;
      replayDone = true;
      esp_task_wdt_reset();
      prevLevel = (GPIO.in & panelIn) ? 1 : 0;
      prevUs = micros();
      edgeCount = 0;
      continue;
    }
    uint32_t in = GPIO.in;
    uint32_t panelLevel = (in & panelIn) ? 1 : 0;
    if (panelLevel != prevLevel) {
      uint32_t now = micros();
      uint32_t dt = now - prevUs;
      prevUs = now;
      prevLevel = panelLevel;
      if (dt > 6000) {
        if (edgeCount >= 16) {
          edgeLen[edgeWrite] = edgeCount;
          edgeReadySlot = edgeWrite + 1;
          edgeReadyMs = millis();
          edgeWrite ^= 1;
        }
        edgeCount = 0;
        dt = 0;
      }
      if (edgeCount < kEdgeMax) {
        edgeSlots[edgeWrite][edgeCount].dt = dt > 65535 ? 65535 : (uint16_t)dt;
        edgeSlots[edgeWrite][edgeCount].level = (uint8_t)panelLevel;
        edgeCount++;
      }
    } else if ((spins & 0xFF) == 0 && edgeCount >= 16 && micros() - prevUs > 6000) {
      edgeLen[edgeWrite] = edgeCount;
      edgeReadySlot = edgeWrite + 1;
      edgeReadyMs = millis();
      edgeWrite ^= 1;
      edgeCount = 0;
    }
    uint32_t mainLevel = (in & mainIn) ? 1 : 0;
    if (mainLevel != mainPrevLevel) {
      uint32_t now = micros();
      uint32_t dt = now - mainPrevUs;
      mainPrevUs = now;
      mainPrevLevel = mainLevel;
      if (mainLevel == 0) {
        if (dt > 1500 && dt < 3200) {
          memset(mainBytes, 0, sizeof(mainBytes));
          mainBitPos = 0;
        } else if (mainBitPos >= 0 && mainBitPos < 72 && dt > 120 && dt < 1200) {
          if (dt > 500) {
            mainBytes[mainBitPos >> 3] |= 1u << (7 - (mainBitPos & 7));
          }
          mainBitPos++;
          if (mainBitPos == 72) {
            uint8_t sum = 0x5A;
            for (int i = 0; i < 8; i++) {
              sum += mainBytes[i];
            }
            if (sum == mainBytes[8]) {
              for (int i = 0; i < 9; i++) {
                mainRaw[i] = mainBytes[i];
              }
              mainRawSeq++;
            }
            mainBitPos = -1;
          }
        } else {
          mainBitPos = -1;
        }
      }
    }
    bool blockPanel = (int32_t)(blockPanelUntil - millis()) > 0;
    if (blockPanel || (in & panelIn)) {
      GPIO.out_w1ts = mainOut;
    } else {
      GPIO.out_w1tc = mainOut;
    }
    if (in & mainIn) {
      GPIO.out_w1ts = panelOut;
    } else {
      GPIO.out_w1tc = panelOut;
    }
    if ((++spins & 0xFFFF) == 0) {
      esp_task_wdt_reset();
    }
  }
}

void driveMain(bool high) {
  if (high) {
    GPIO.out_w1ts = 1u << kMainTxPin;
  } else {
    GPIO.out_w1tc = 1u << kMainTxPin;
  }
  delayMicroseconds(kBitUs);
}

void bangFrame(const DryerCommand& cmd) {
  // tx inv flips the captured waveform. tx norm sends the bytes as read, idle high.
  bool flip = mainTxInvert;
  auto bit = [flip](int uartBit) { driveMain(flip ? !uartBit : uartBit); };
  driveMain(true);
  for (uint8_t copy = 0; copy < cmd.repeats; copy++) {
    for (uint8_t i = 0; i < cmd.len; i++) {
      bit(0);
      for (int b = 0; b < 8; b++) {
        bit((cmd.frame[i] >> b) & 1);
      }
      bit(1);
    }
  }
}

bool savedWave(const char* name, const PulseEdge** edges, uint16_t* count) {
  if (strcmp(name, "on") == 0 || strcmp(name, "off") == 0) {
    if (onWaveReady) {
      *edges = onWave;
      *count = onWaveLen;
    } else {
      *edges = kOnEdges;
      *count = kOnEdgeCount;
    }
    return true;
  }
  if (strcmp(name, "humidity") == 0) {
    *edges = kHumidityEdges;
    *count = kHumidityEdgeCount;
    return true;
  }
  if (strcmp(name, "fan") == 0) {
    *edges = kFanEdges;
    *count = kFanEdgeCount;
    return true;
  }
  if (strcmp(name, "sleep") == 0) {
    *edges = kSleepEdges;
    *count = kSleepEdgeCount;
    return true;
  }
  if (strcmp(name, "timer") == 0) {
    *edges = kTimerEdges;
    *count = kTimerEdgeCount;
    return true;
  }
  return false;
}

bool sendFrame(const DryerCommand& cmd) {
  const PulseEdge* edges = nullptr;
  uint16_t count = 0;
  if (savedWave(cmd.name, &edges, &count)) {
    replayEdges = edges;
    replayLen = count;
    replayBusy = true;
    replayDone = false;
    replayReq = true;
    uint32_t start = millis();
    while (!replayDone && millis() - start < 800) {
      delay(1);
    }
    replayBusy = false;
    if (!replayDone) {
      if (client && client.connected()) {
        client.println("replay did not run");
      }
      return false;
    }
    char line[64];
    int n = snprintf(line, sizeof(line), "telnet %lu %s replay %u edges\n", millis(), cmd.name, replayLen);
    if (n > 0) {
      logLine(line, (size_t)n);
    }
    pulse(4);
    return true;
  }
  bool resume = passThrough;
  passThrough = false;
  delay(40);
  bangFrame(cmd);
  delay(15);
  holdOutputs();
  passThrough = resume;
  char line[80];
  int n = snprintf(line, sizeof(line), "telnet %lu %s x%u:", millis(), cmd.name, cmd.repeats);
  for (uint8_t i = 0; n > 0 && i < cmd.len && n < (int)sizeof(line) - 4; i++) {
    n += snprintf(line + n, sizeof(line) - n, " %02X", cmd.frame[i]);
  }
  if (n < 0 || n >= (int)sizeof(line) - 2) {
    return true;
  }
  line[n++] = '\n';
  line[n] = '\0';
  logLine(line, n);
  pulse(4);
  return true;
}

int waitNextDisplayNumber(uint32_t afterSeq, uint32_t timeoutMs) {
  uint32_t start = millis();
  uint32_t lastSeq = afterSeq;
  while (millis() - start < timeoutMs) {
    takeBytes(panelBurst);
    takeBytes(mainBurst);
    pumpReplay();
    uint32_t seq = mainRawSeq;
    if (seq != lastSeq) {
      uint8_t d1 = mainRaw[1];
      uint8_t d2 = mainRaw[2];
      lastSeq = seq;
      int val = decodeDisplayNumber(d1, d2);
      if (val >= 0) {
        return val;
      }
    }
    delay(5);
  }
  return -1;
}

// Steps the Timer button until the panel's 2-digit display shows `targetHours` (0..24).
// The first press wakes the timer display; subsequent presses increment by 1 hour.
bool setTimerHours(int targetHours, int* finalHours = nullptr) {
  if (targetHours < 0 || targetHours > 24) {
    return false;
  }
  const DryerCommand* timerCmd = nullptr;
  for (size_t i = 0; i < sizeof(kCommands) / sizeof(kCommands[0]); i++) {
    if (strcmp(kCommands[i].name, "timer") == 0) {
      timerCmd = &kCommands[i];
      break;
    }
  }
  if (!timerCmd) {
    return false;
  }
  for (int step = 0; step < 28; step++) {
    uint32_t seqBefore = mainRawSeq;
    if (!sendFrame(*timerCmd)) {
      return false;
    }
    // Wait for at least one new main frame after the replay so we read the updated digit.
    delay(120);
    int shown = waitNextDisplayNumber(seqBefore + 1, 900);
    if (finalHours) {
      *finalHours = shown;
    }
    if (shown == targetHours) {
      char line[64];
      int n = snprintf(line, sizeof(line), "telnet %lu timer set to %02d\n", millis(), shown);
      if (n > 0) {
        logLine(line, (size_t)n);
      }
      return true;
    }
    delay(260);
  }
  return false;
}

// Steps the Humidity button until the panel's 2-digit display shows `targetPercent` (40..70, step 5).
bool setHumidityPercent(int targetPercent, int* finalPercent = nullptr) {
  if (targetPercent < 40 || targetPercent > 70 || (targetPercent % 5) != 0) {
    return false;
  }
  const DryerCommand* humCmd = nullptr;
  for (size_t i = 0; i < sizeof(kCommands) / sizeof(kCommands[0]); i++) {
    if (strcmp(kCommands[i].name, "humidity") == 0) {
      humCmd = &kCommands[i];
      break;
    }
  }
  if (!humCmd) {
    return false;
  }
  for (int step = 0; step < 10; step++) {
    uint32_t seqBefore = mainRawSeq;
    if (!sendFrame(*humCmd)) {
      return false;
    }
    delay(120);
    int shown = waitNextDisplayNumber(seqBefore + 1, 900);
    if (finalPercent) {
      *finalPercent = shown;
    }
    if (shown == targetPercent) {
      char line[64];
      int n = snprintf(line, sizeof(line), "telnet %lu humidity set to %d%%\n", millis(), shown);
      if (n > 0) {
        logLine(line, (size_t)n);
      }
      return true;
    }
    delay(260);
  }
  return false;
}

void runCommand(char* text) {
  while (*text == ' ') {
    text++;
  }
  size_t len = strlen(text);
  while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\r')) {
    text[--len] = '\0';
  }
  if (len == 0) {
    return;
  }
  if (strcasecmp(text, "tx inv") == 0 || strcasecmp(text, "tx norm") == 0) {
    mainTxInvert = strcasecmp(text, "tx inv") == 0;
    applyUarts();
    describeMode();
    return;
  }
  if (strcasecmp(text, "pass") == 0 || strncasecmp(text, "pass ", 5) == 0) {
    if (strcasecmp(text, "pass off") == 0) {
      passThrough = false;
    } else if (strcasecmp(text, "pass on") == 0 || strcasecmp(text, "pass") == 0) {
      passThrough = true;
    } else {
      client.printf("unknown: %s\n", text);
      printHelp();
      return;
    }
    describeMode();
    return;
  }
  if (strncasecmp(text, "main", 4) == 0 || strncasecmp(text, "panel", 5) == 0) {
    bool isMain = strncasecmp(text, "main", 4) == 0;
    char* args = text + (isMain ? 4 : 5);
    bool* rxInv = isMain ? &mainRxInvert : &panelRxInvert;
    bool* txMatch = isMain ? &panelTxInvert : &mainTxInvert;
    uint32_t* baud = isMain ? &mainBaud : &panelBaud;
    bool changed = false;
    char* tok = strtok(args, " ");
    while (tok) {
      if (strcasecmp(tok, "inv") == 0) {
        *rxInv = true;
        *txMatch = true;
      } else if (strcasecmp(tok, "norm") == 0) {
        *rxInv = false;
        *txMatch = false;
      } else if (strcasecmp(tok, "4800") == 0 || strcasecmp(tok, "9600") == 0 ||
                 strcasecmp(tok, "2400") == 0) {
        *baud = (uint32_t)atoi(tok);
      } else {
        client.printf("unknown: %s\n", tok);
        printHelp();
        return;
      }
      changed = true;
      tok = strtok(nullptr, " ");
    }
    if (!changed) {
      printHelp();
      return;
    }
    applyUarts();
    describeMode();
    return;
  }
  if (strncasecmp(text, "humidity ", 9) == 0) {
    char* arg = text + 9;
    while (*arg == ' ') {
      arg++;
    }
    if (*arg >= '0' && *arg <= '9') {
      int target = atoi(arg);
      int got = -1;
      bool ok = setHumidityPercent(target, &got);
      if (client && client.connected()) {
        if (ok) {
          client.printf("humidity set to %d%%\n", got);
        } else {
          client.printf("humidity failed (40..70 step 5, last=%d)\n", got);
        }
      }
      return;
    }
  }
  if (strncasecmp(text, "timer ", 6) == 0) {
    char* arg = text + 6;
    while (*arg == ' ') {
      arg++;
    }
    if (*arg >= '0' && *arg <= '9') {
      int target = atoi(arg);
      int got = -1;
      bool ok = setTimerHours(target, &got);
      if (client && client.connected()) {
        if (ok) {
          client.printf("timer set to %02d\n", got);
        } else {
          client.printf("timer failed (last=%d)\n", got);
        }
      }
      return;
    }
  }
  for (size_t i = 0; i < sizeof(kCommands) / sizeof(kCommands[0]); i++) {
    if (strcasecmp(text, kCommands[i].name) == 0) {
      sendFrame(kCommands[i]);
      return;
    }
  }
  client.printf("unknown: %s\n", text);
  printHelp();
}

const char kPage[] = R"html(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Air dryer</title>
<style>
  body { font-family: sans-serif; margin: 2rem; background: #111; color: #eee; }
  .reading { font-size: 2.6rem; font-weight: 700; margin: 0.4rem 0 1.2rem; }
  .reading span { color: #6cf; }
  button { font-size: 1.3rem; margin: 0.4rem; padding: 0.9rem 1.3rem; }
  #status { margin-top: 1.4rem; min-height: 1.2em; }
  #raw { color: #888; font-family: monospace; font-size: 0.95rem; }
</style>
</head>
<body>
<h1>Air dryer</h1>
<div class="reading">Humidity / Display: <span id="disp">--</span></div>
<button onclick="send('on')">On</button>
<button onclick="send('off')">Off</button>
<button onclick="send('humidity')">Humidity</button>
<button onclick="send('fan')">Fan</button>
<button onclick="send('sleep')">Sleep</button>
<button onclick="send('timer')">Timer</button>
<div style="margin-top: 1rem;">
  <select id="humTarget" style="font-size: 1.2rem; padding: 0.5rem;">
    <option value="40">40%</option>
    <option value="45">45%</option>
    <option value="50" selected>50%</option>
    <option value="55">55%</option>
    <option value="60">60%</option>
    <option value="65">65%</option>
    <option value="70">70%</option>
  </select>
  <button onclick="setHumidity()">Set Humidity (%)</button>
</div>
<div style="margin-top: 0.6rem;">
  <input id="hours" type="number" min="0" max="24" value="2" style="font-size: 1.2rem; width: 4rem; padding: 0.5rem;">
  <button onclick="setTimer()">Set Timer (h)</button>
</div>
<p id="status"></p>
<p id="raw"></p>
<script>
function send(name) {
  const status = document.getElementById('status');
  status.textContent = 'sending ' + name + '...';
  fetch('/cmd?name=' + name).then(r => r.text()).then(t => { status.textContent = t; });
}
function setHumidity() {
  const p = document.getElementById('humTarget').value;
  const status = document.getElementById('status');
  status.textContent = 'setting humidity to ' + p + '%...';
  fetch('/humidity?percent=' + encodeURIComponent(p)).then(r => r.text()).then(t => { status.textContent = t; });
}
function setTimer() {
  const h = document.getElementById('hours').value;
  const status = document.getElementById('status');
  status.textContent = 'setting timer to ' + h + 'h...';
  fetch('/timer?hours=' + encodeURIComponent(h)).then(r => r.text()).then(t => { status.textContent = t; });
}
function poll() {
  fetch('/state').then(r => r.json()).then(s => {
    document.getElementById('disp').textContent =
      /^[0-9]+$/.test(s.display) ? s.display + '%' : s.display;
    document.getElementById('raw').textContent = s.raw ? 'main: ' + s.raw : '';
  }).catch(() => {});
}
poll();
setInterval(poll, 1000);
</script>
</body>
</html>
)html";

void handleRoot() {
  web.send(200, "text/html", kPage);
}

void handleState() {
  char body[128];
  snprintf(body, sizeof(body), "{\"display\":\"%s\",\"raw\":\"%s\"}", panelText, mainHex);
  web.send(200, "application/json", body);
}

void handleCmd() {
  String name = web.arg("name");
  for (size_t i = 0; i < sizeof(kCommands) / sizeof(kCommands[0]); i++) {
    if (name.equalsIgnoreCase(kCommands[i].name)) {
      if (sendFrame(kCommands[i])) {
        web.send(200, "text/plain", String("sent ") + kCommands[i].name);
      } else {
        web.send(200, "text/plain", "replay did not run");
      }
      return;
    }
  }
  web.send(404, "text/plain", "unknown");
}

void handleHumidity() {
  if (!web.hasArg("percent")) {
    web.send(400, "text/plain", "missing percent");
    return;
  }
  int target = web.arg("percent").toInt();
  int got = -1;
  if (setHumidityPercent(target, &got)) {
    char msg[32];
    snprintf(msg, sizeof(msg), "humidity set to %d%%", got);
    web.send(200, "text/plain", msg);
  } else {
    char msg[48];
    snprintf(msg, sizeof(msg), "humidity failed (40..70 step 5, last=%d)", got);
    web.send(200, "text/plain", msg);
  }
}

void handleTimer() {
  if (!web.hasArg("hours")) {
    web.send(400, "text/plain", "missing hours");
    return;
  }
  int target = web.arg("hours").toInt();
  int got = -1;
  if (setTimerHours(target, &got)) {
    char msg[32];
    snprintf(msg, sizeof(msg), "timer set to %02dh", got);
    web.send(200, "text/plain", msg);
  } else {
    char msg[48];
    snprintf(msg, sizeof(msg), "timer failed (last=%d)", got);
    web.send(200, "text/plain", msg);
  }
}

void serviceTask(void*) {
  for (;;) {
    takeBytes(panelBurst);
    takeBytes(mainBurst);
    pumpReplay();

    // Wi-Fi init blocks long enough to drop the dryer's power-on exchange.
    if (!wifiStarted && millis() > 3000) {
      WiFi.mode(WIFI_STA);
      WiFi.begin(ssid, password);
      wifiStarted = true;
      Serial.printf("WiFi starting, ssid=%s\n", ssid);
    }

    if (!serverStarted && wifiStarted && WiFi.status() == WL_CONNECTED) {
      telnet.begin();
      web.on("/", handleRoot);
      web.on("/state", handleState);
      web.on("/cmd", handleCmd);
      web.on("/humidity", handleHumidity);
      web.on("/timer", handleTimer);
      web.begin();
      serverStarted = true;
      Serial.printf("telnet %s %u\n", WiFi.localIP().toString().c_str(), kTelnetPort);
      Serial.printf("http://%s/\n", WiFi.localIP().toString().c_str());
    }

    if (telnet.hasClient()) {
      WiFiClient next = telnet.available();
      if (client) {
        client.stop();
      }
      client = next;
      client.setTimeout(20);
      beginReplay();
      cmdLen = 0;
      Serial.println("telnet client connected");
    }

    paintLed();

    if (client && client.connected()) {
      while (client.available()) {
        char c = client.read();
        if (c == '\r') {
          continue;
        }
        if (c == '\n') {
          cmd[cmdLen] = '\0';
          runCommand(cmd);
          cmdLen = 0;
          continue;
        }
        if (cmdLen + 1 < sizeof(cmd)) {
          cmd[cmdLen++] = c;
        }
      }
    }

    if (serverStarted) {
      web.handleClient();
    }
    vTaskDelay(1);
  }
}

void setup() {
  // The dryer beeps only if this link is already up during its power-on handshake.
  disableCore1WDT();
  pinMode(kPanelRxPin, INPUT);
  pinMode(kMainRxPin, INPUT);
  holdOutputs();
  applyUarts();
  holdOutputs();
  startLog();

  Serial.begin(115200);
  loadOnWave();
  Serial.println("ESP32-S3 bridge");
  Serial.printf("panel RX %d %s TX %d %s, main RX %d %s TX %d %s, %lu/%lu, pass on\n",
                kPanelRxPin, pol(panelRxInvert), kPanelTxPin, pol(panelTxInvert), kMainRxPin,
                pol(mainRxInvert), kMainTxPin, pol(mainTxInvert), panelBaud, mainBaud);
  paintLed();
  xTaskCreatePinnedToCore(serviceTask, "dryer", 16384, nullptr, 1, nullptr, 0);
}

void loop() {
  mirrorLoop();
  esp_task_wdt_reset();
}
