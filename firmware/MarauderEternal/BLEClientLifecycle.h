#pragma once

#include <stdint.h>

namespace marauder {

// A disconnect is asynchronous. Retain ownership on timeout or deletion
// failure so callers can retry without stopping the host or losing the slot.
template <typename Client, typename Clock, typename Wait, typename Destroy>
bool releaseDisconnectedClient(Client*& client, Clock now, Wait wait,
                               Destroy destroy, uint32_t timeoutMs = 2000) {
  if (client == nullptr) return true;
  client->setSelfDelete(false, false);
  if (client->isConnected()) {
    client->disconnect();
    const uint32_t started = now();
    while (client->isConnected() && uint32_t(now() - started) < timeoutMs)
      wait();
    if (client->isConnected()) return false;
  }
  if (!destroy(client)) return false;
  client = nullptr;
  return true;
}

}  // namespace marauder

#ifdef ARDUINO
#include <Arduino.h>
#include <NimBLEDevice.h>

namespace marauder {
inline bool releaseNimbleClient(NimBLEClient*& client) {
  // On a timeout the library owns deferred deletion. Remember only that
  // cleanup is pending, never a pointer that its callback can invalidate.
  static bool deferredCleanup = false;
  if (deferredCleanup) {
    while (NimBLEClient* disconnected = NimBLEDevice::getDisconnectedClient()) {
      const size_t before = NimBLEDevice::getCreatedClientCount();
      if (!NimBLEDevice::deleteClient(disconnected)) return false;
      // A pending connection cancellation can also defer deletion. Give its
      // callback a scheduling opportunity on the next attempt, without spinning.
      if (NimBLEDevice::getCreatedClientCount() >= before) return false;
    }
    if (NimBLEDevice::getCreatedClientCount() != 0) return false;
    deferredCleanup = false;
  }
  const bool released = releaseDisconnectedClient(
      client, []() { return millis(); }, []() { delay(10); },
      [](NimBLEClient* owned) { return NimBLEDevice::deleteClient(owned); });
  if (!released) {
    // deleteClient requests deletion on the disconnect event when the client
    // is still connected. The host must remain alive to process that event.
    NimBLEDevice::deleteClient(client);
    client = nullptr;
    deferredCleanup = true;
    Serial.println(F("BLE client cleanup pending; keeping host active"));
  }
  return released;
}
}  // namespace marauder
#endif
