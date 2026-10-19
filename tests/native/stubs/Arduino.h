#pragma once
#include <cstdint>

constexpr int LOW = 0, HIGH = 1, INPUT = 0, INPUT_PULLUP = 1, INPUT_PULLDOWN = 2;
uint32_t millis();
int digitalRead(int pin);
void pinMode(int pin, int mode);
void delay(unsigned ms);
