#include "SavedWiFi.h"
#include <cassert>
#include <iostream>

int main() {
  using namespace saved_wifi;
  DynamicJsonDocument doc(8192);
  doc["Settings"][0]["name"] = "ClientSSID";
  doc["Settings"][0]["value"] = "legacy";
  doc["Settings"][1]["name"] = "ClientPW";
  doc["Settings"][1]["value"] = "password";
  doc["Settings"][2]["name"] = "CustomFeature";
  doc["Settings"][2]["value"] = 42;
  assert(migrate(doc)); assert(!migrate(doc));
  Profile p;
  assert(read(doc, 0, p) && strcmp(p.ssid, "legacy") == 0);
  assert(save(doc, "open", "") == Saved);
  assert(save(doc, "three", "password") == Saved);
  assert(save(doc, "four", "password") == Saved);
  assert(save(doc, "five", "password") == Saved);
  assert(save(doc, "six", "password") == Full);
  assert(profiles(doc).size() == 5);
  assert(save(doc, "legacy", "newpassword") == Updated);
  assert(read(doc, 0, p) && strcmp(p.password, "newpassword") == 0);
  assert(save(doc, "six", "password", 3) == Saved);
  assert(read(doc, 0, p) && strcmp(p.ssid, "six") == 0);
  assert(save(doc, "seven", "password", 5) == Error);
  assert(save(doc, "bad", "short") == Error);
  assert(save(doc, "", "password") == Error);
  assert(!valid("123456789012345678901234567890123", "password"));
  assert(valid("hex", "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"));
  assert(!valid("hex", "g123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"));
  assert(setting(doc, "CustomFeature")["value"] == 42);
  assert(!remove(doc, 5));
  while (profiles(doc).size()) assert(remove(doc, 0));
  assert(strcmp(setting(doc, "ClientSSID")["value"], "") == 0);
  const bool visible[] = {false, true, true, false, true};
  const int rssi[] = {-1, -70, -20, 0, -20};
  unsigned order[5]; rank(5, visible, rssi, order);
  const unsigned expected[] = {2, 4, 1, 0, 3};
  for (unsigned i = 0; i < 5; ++i) assert(order[i] == expected[i]);
  std::cout << "Saved Wi-Fi migration, limits, ordering and preservation passed\n";
}
