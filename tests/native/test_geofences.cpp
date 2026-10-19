#include "Geofences.h"
#include "SavedWiFi.h"
#include <cassert>
#include <limits>
#include <iostream>

int main() {
  using namespace geofences;
  Config fences[kMax];
  Gate gate;
  assert(gate.update(fences, false, 0, 0) == State::Clear);
  fences[0].enabled = true; strcpy(fences[0].name, "Home");
  assert(gate.update(fences, false, 0, 0) == State::WaitingForFix);
  assert(gate.update(fences, true, 0, 0) == State::Inside);
  constexpr double degPerMile = 180.0 / (3.14159265358979323846 * 3958.7613);
  assert(gate.update(fences, true, 0, .103 * degPerMile) == State::Inside);
  assert(gate.update(fences, true, 0, .12 * degPerMile) == State::Clear);
  assert(gate.update(fences, true, 0, .103 * degPerMile) == State::Clear);
  assert(gate.update(fences, true, 0, .099 * degPerMile) == State::Inside);
  assert(gate.update(fences, false, 0, 2) == State::WaitingForFix);
  assert(gate.update(fences, true, 0, 2) == State::Clear);
  assert(gate.update(fences, true, 91, 0) == State::WaitingForFix);
  assert(std::isfinite(distanceMiles(90, 180, -90, 0)));
  assert(distanceMiles(0, 179.999, 0, -179.999) < .14);
  Config invalid = fences[0]; invalid.radiusMiles = std::numeric_limits<float>::quiet_NaN();
  assert(!valid(invalid));
  DynamicJsonDocument doc(8192);
  doc.createNestedArray("Settings");
  assert(migrate(doc)); assert(!migrate(doc));
  for (unsigned i = 0; i < kMax; ++i) {
    Config f = fences[0]; f.latitude = i * 10;
    assert(save(doc, i, f));
  }
  Config loaded;
  assert(load(doc, 4, loaded) && loaded.latitude == 40);
  assert(!save(doc, 5, fences[0])); assert(!save(doc, 0, invalid));
  assert(!remove(doc, 5)); assert(remove(doc, 2)); assert(!load(doc, 2, loaded));
  assert(load(doc, 4, loaded) && loaded.latitude == 40); // Slots never shift.
  assert(!doc.overflowed());
  std::cout << "Geofence boundaries, hysteresis, stale fix, validation and slots passed\n";
}
