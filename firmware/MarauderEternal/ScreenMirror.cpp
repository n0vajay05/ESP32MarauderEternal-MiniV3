#include "ScreenMirror.h"
#include "MirrorCanvas.h"
#include "MemoryGuard.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

namespace marauder { namespace mirror {
namespace {
constexpr uint32_t kLeaseMs = 4000;
constexpr uint32_t kBaud = 921600;
Canvas canvas;
bool external = false;
std::atomic<bool> streaming{false};
TaskHandle_t worker = nullptr;
StaticSemaphore_t mutexStorage;
SemaphoreHandle_t mutex = nullptr;
char queuedCommand[256]{};
bool queued = false;
uint32_t session = 0, sequence = 0, lastPoll = 0, packets = 0;
uint64_t captureUs = 0;
uint64_t pollUs = 0;
uint32_t maxPollUs = 0;
uint8_t pending[kPacketMax];
size_t pendingSize = 0;

class Capture : public TFT_Capture {
 public:
  void window(int32_t x, int32_t y, int32_t xe, int32_t ye) override {
    xSemaphoreTake(mutex, portMAX_DELAY); canvas.window(x, y, xe, ye); xSemaphoreGive(mutex);
  }
  void solid(uint16_t color, uint32_t count) override {
    xSemaphoreTake(mutex, portMAX_DELAY);
    int64_t start = esp_timer_get_time(); canvas.solid(color, count);
    captureUs += esp_timer_get_time() - start;
    xSemaphoreGive(mutex);
  }
  void pixels(const void* data, uint32_t count, bool swap) override {
    xSemaphoreTake(mutex, portMAX_DELAY);
    int64_t start = esp_timer_get_time(); canvas.pixels(data, count, swap);
    captureUs += esp_timer_get_time() - start;
    xSemaphoreGive(mutex);
  }
  void pixel(int32_t x, int32_t y, uint16_t color) override {
    xSemaphoreTake(mutex, portMAX_DELAY);
    int64_t start = esp_timer_get_time(); canvas.pixel(x, y, color);
    captureUs += esp_timer_get_time() - start;
    xSemaphoreGive(mutex);
  }
  void rotated(uint8_t value) override {
    xSemaphoreTake(mutex, portMAX_DELAY); canvas.rotation(value); xSemaphoreGive(mutex);
  }
} capture;

void resetSession() {
  pendingSize = 0;
  Serial.flush(); // At most one bounded packet remains in the TX buffer.
  Serial.updateBaudRate(115200);
  streaming = false;
}
bool hex32(const char* text, uint32_t& result) {
  if (strlen(text) != 8) return false;
  result = 0;
  for (unsigned i = 0; i < 8; ++i) {
    char c = text[i]; unsigned value;
    if (c >= '0' && c <= '9') value = c - '0';
    else if (c >= 'a' && c <= 'f') value = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') value = c - 'A' + 10;
    else return false;
    result = (result << 4) | value;
  }
  return true;
}
void status(const char* state, uint32_t requestSession) {
  Serial.printf("@MIRROR:{\"protocol\":1,\"status\":\"%s\",\"session\":%lu,\"baud\":%lu,"
                "\"width\":128,\"height\":128,\"bufferBytes\":%u,\"memory\":\"%s\","
                "\"heap\":%lu,\"captureUs\":%llu,\"packets\":%lu,"
                "\"pollUs\":%llu,\"maxPollUs\":%lu,\"stackFree\":%u,\"upMs\":%lu}\n",
      state, static_cast<unsigned long>(requestSession), static_cast<unsigned long>(kBaud),
      canvas.ready() ? unsigned(kBytes) : 0, external ? "psram" : "internal",
      static_cast<unsigned long>(ESP.getFreeHeap()), static_cast<unsigned long long>(captureUs),
      static_cast<unsigned long>(packets), static_cast<unsigned long long>(pollUs),
      static_cast<unsigned long>(maxPollUs), worker ? unsigned(uxTaskGetStackHighWaterMark(worker)) : 0,
      static_cast<unsigned long>(millis()));
}

void mirrorTask(void*) {
  char input[256]{}; size_t used = 0; bool overflow = false;
  while (true) {
    if (!streaming) {
      used = 0; overflow = false;
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      continue;
    }
    tick();
    if (!streaming) continue;
    unsigned budget = 128;
    while (budget-- && Serial.available()) {
      char value = Serial.read();
      if (value == '\r') continue;
      if (value == '\n') {
        input[used] = 0;
        if (!overflow && used) {
          if (!command(input, false)) {
            xSemaphoreTake(mutex, portMAX_DELAY);
            if (!queued) { memcpy(queuedCommand, input, used + 1); queued = true; }
            xSemaphoreGive(mutex);
          }
        }
        used = 0; overflow = false;
        if (!streaming) break;
      } else if (used + 1 < sizeof(input)) input[used++] = value;
      else overflow = true;
    }
    vTaskDelay(1);
  }
}
}

void begin(TFT_eSPI& display) {
  // Keeping a single shadow from boot is necessary because panel RAM readback
  // does not work on this board. It also lets a PC attach during any activity
  // without redrawing or stopping that activity. Prefer external PSRAM.
  if (!mutex) mutex = xSemaphoreCreateMutexStatic(&mutexStorage);
  if (!canvas.ready()) {
    auto* storage = static_cast<uint16_t*>(heap_caps_calloc(1, kBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    external = storage != nullptr;
    if (!storage && RuntimeMemoryGuard::instance().allow(kBytes))
      storage = static_cast<uint16_t*>(heap_caps_calloc(1, kBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    canvas.attach(storage);
  }
  if (canvas.ready()) { display.setCapture(&capture); canvas.rotation(display.getRotation()); }
  if (canvas.ready() && !worker && xTaskCreate(mirrorTask, "screen-mirror", 4096, nullptr, 1, &worker) != pdPASS)
    worker = nullptr;
}
bool active() { return streaming; }
String serialInput() {
  xSemaphoreTake(mutex, portMAX_DELAY);
  String result = queued ? String(queuedCommand) : String(); queued = false;
  xSemaphoreGive(mutex);
  return result;
}
void tick() {
  if (streaming && uint32_t(millis() - lastPoll) >= kLeaseMs) resetSession();
}
bool command(const char* input, bool storageActive) {
  if (strncmp(input, "mirror ", 7) && strcmp(input, "mirror")) return false;
  char action[12]{}, token[10]{}, acknowledgement[10]{}, extra[2]{};
  int count = sscanf(input, "mirror %11s %9s %9s %1s", action, token, acknowledgement, extra);
  uint32_t requested = 0, ack = 0;
  if (count == 1 && !strcmp(action, "status")) { status(canvas.ready() ? streaming ? "active" : "available" : "no-memory", session); return true; }
  if (count < 2 || !hex32(token, requested) || !requested) { status("invalid-request", 0); return true; }
  if (!strcmp(action, "start") && count == 2) {
    if (storageActive) { status("storage-busy", requested); return true; }
    if (!canvas.ready() || !worker) { status("no-memory", requested); return true; }
    if (streaming && session != requested) { status("busy", requested); return true; }
    session = requested; sequence = 0; pendingSize = 0; packets = 0;
    lastPoll = millis();
    xSemaphoreTake(mutex, portMAX_DELAY); canvas.invalidate(); queued = false; xSemaphoreGive(mutex);
    status("ready", requested); Serial.flush();
    Serial.updateBaudRate(kBaud); streaming = true;
    xTaskNotifyGive(worker);
    return true;
  }
  if (!streaming || session != requested) { status("session-expired", requested); return true; }
  if (!strcmp(action, "stop") && count == 2) { status("stopped", requested); resetSession(); return true; }
  if (strcmp(action, "poll") || count != 3 || !hex32(acknowledgement, ack)) { status("invalid-request", requested); return true; }
  lastPoll = millis();
  const int64_t pollStart = esp_timer_get_time();
  // Retransmit until acknowledged. A lost/corrupt packet never silently clears
  // a row, and a slow viewer cannot grow a queue on the Mini.
  if (!pendingSize || ack == sequence) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    pendingSize = packet(pending, canvas, canvas.takeRow(), session, ++sequence);
    xSemaphoreGive(mutex);
  }
  if (Serial.availableForWrite() >= int(pendingSize)) {
    Serial.write(pending, pendingSize); ++packets;
  }
  uint32_t elapsed = esp_timer_get_time() - pollStart;
  pollUs += elapsed; if (elapsed > maxPollUs) maxPollUs = elapsed;
  return true;
}

bool verifyCapture(TFT_eSPI& display) {
  if (!canvas.ready()) return false;
  display.setRotation(0); display.fillScreen(TFT_BLACK);
  display.fillRect(0, 0, 64, 64, TFT_RED); display.fillRect(64, 0, 64, 64, TFT_GREEN);
  display.fillRect(0, 64, 64, 64, TFT_BLUE); display.fillRect(64, 64, 64, 64, TFT_WHITE);
  display.drawFastVLine(2, 0, 128, TFT_WHITE); display.drawFastHLine(0, 3, 128, TFT_WHITE);
  display.drawPixel(5, 5, TFT_CYAN);
  TFT_eSprite sprite(&display); sprite.setColorDepth(16);
  if (!sprite.createSprite(16, 16)) return false;
  sprite.fillSprite(TFT_MAGENTA); sprite.pushSprite(80, 80);
  const bool swap = display.getSwapBytes();
  uint16_t colors[] = {TFT_RED, TFT_GREEN, TFT_BLUE, TFT_WHITE};
  display.setSwapBytes(true); display.pushImage(96, 80, 4, 1, colors); display.setSwapBytes(swap);
  xSemaphoreTake(mutex, portMAX_DELAY);
  bool ok = canvas.row(0)[0] == TFT_RED && canvas.row(0)[127] == TFT_GREEN &&
      canvas.row(127)[0] == TFT_BLUE && canvas.row(127)[127] == TFT_WHITE &&
      canvas.row(80)[2] == TFT_WHITE && canvas.row(3)[80] == TFT_WHITE &&
      canvas.row(5)[5] == TFT_CYAN && canvas.row(80)[80] == TFT_MAGENTA;
  for (unsigned i = 0; i < 4; ++i) ok &= canvas.row(80)[96 + i] == colors[i];
  xSemaphoreGive(mutex);
  display.setRotation(1); display.drawPixel(0, 0, TFT_YELLOW);
  display.setRotation(2); display.drawPixel(0, 0, TFT_CYAN);
  display.setRotation(3); display.drawPixel(0, 0, TFT_MAGENTA);
  xSemaphoreTake(mutex, portMAX_DELAY);
  ok &= canvas.row(0)[127] == TFT_YELLOW && canvas.row(127)[127] == TFT_CYAN && canvas.row(127)[0] == TFT_MAGENTA;
  xSemaphoreGive(mutex);
  display.setRotation(0);
  display.setTextColor(TFT_WHITE, TFT_BLACK); display.setTextSize(1);
  display.drawString("Mirror TEST", 6, 10, 2); display.drawString("12", 20, 42, 4);
  return ok;
}
} }
