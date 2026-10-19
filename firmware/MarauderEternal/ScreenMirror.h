#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>

namespace marauder { namespace mirror {
void begin(TFT_eSPI& display);
void tick();
bool active();
String serialInput();
bool verifyCapture(TFT_eSPI& display);
// Returns true for mirror commands, including malformed/unsupported requests.
bool command(const char* input, bool storageActive);
} }
