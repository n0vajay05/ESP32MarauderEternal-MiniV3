#include "DroneExport.h"
#include <cstring>
#include <algorithm>

namespace drone_tracking {
namespace {
constexpr const char* kGpxHeader = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<gpx version=\"1.1\" creator=\"Marauder Eternal\" xmlns=\"http://www.topografix.com/GPX/1/1\">\n";
bool write(File& file, const std::string& text) {
  if (!file || file.write(reinterpret_cast<const uint8_t*>(text.data()), text.size()) != text.size()) return false;
  file.flush();
  return !file.getWriteError();
}
}
String Export::trackPath(unsigned slot) const { return base_ + ".t" + String(slot); }
bool Export::append(const String& path, const std::string& text) {
  File file = fs_->open(path, FILE_APPEND);
  const bool ok = write(file, text);
  file.close();
  healthy_ &= ok;
  return ok;
}
bool Export::begin(fs::FS& filesystem, const String& base) {
  if (active() || !base.length()) return false;
  for (unsigned i = 0; i < 16; ++i)
    if (filesystem.exists(base + ".t" + String(i))) return false;
  if (filesystem.exists(base + ".csv") || filesystem.exists(base + ".gpx") ||
      filesystem.exists(base + ".part")) return false;
  fs_ = &filesystem; base_ = base; healthy_ = true;
  for (auto& track : tracks_) track = Track();
  File csv = fs_->open(base_ + ".csv", FILE_WRITE);
  bool ok = write(csv, csvHeader()); csv.close();
  File gpx = fs_->open(base_ + ".part", FILE_WRITE);
  ok &= write(gpx, kGpxHeader); gpx.close();
  healthy_ = ok;
  if (!ok) fs_ = nullptr;
  return ok;
}
bool Export::flushTrack(unsigned slot) {
  Track& track = tracks_[slot];
  if (!track.used || !track.points) { track = Track(); return true; }
  File source = fs_->open(trackPath(slot), FILE_READ);
  File output = fs_->open(base_ + ".part", FILE_APPEND);
  bool ok = source && output && write(output, "<trk><name>" + xmlText(track.id) +
      "</name><desc>Remote ID " + xmlText(track.address) + "</desc><trkseg>\n");
  uint8_t buffer[256];
  const size_t expected = source ? source.size() : 0;
  size_t copied = 0;
  while (ok && copied < expected) {
    const size_t count = source.read(buffer, std::min(sizeof(buffer), expected - copied));
    if (!count || output.write(buffer, count) != count) { ok = false; break; }
    copied += count;
    delay(0);
  }
  if (ok) ok = write(output, "</trkseg></trk>\n");
  source.close(); output.close();
  healthy_ &= ok;
  if (ok) { fs_->remove(trackPath(slot)); track = Track(); }
  return ok;
}
bool Export::sample(const Record& record, uint32_t now) {
  if (!active() || !healthy_) return false;
  unsigned slot = 16;
  for (unsigned i = 0; i < 16; ++i) {
    const Track& track = tracks_[i];
    const bool stable = (record.idType == 1 || record.idType == 2) && record.idType == track.idType;
    if (track.used && (track.generation == record.generation ||
        (stable && !strcmp(track.id, record.id)))) { slot = i; break; }
  }
  if (slot == 16) {
    for (unsigned i = 0; i < 16; ++i) if (!tracks_[i].used) { slot = i; break; }
    if (slot == 16) {
      slot = 0;
      for (unsigned i = 1; i < 16; ++i)
        if (uint32_t(now - tracks_[i].lastSeen) > uint32_t(now - tracks_[slot].lastSeen)) slot = i;
      if (!flushTrack(slot)) return false;
    }
    tracks_[slot].used = true;
  }
  Track& track = tracks_[slot];
  const bool newGeneration = track.generation != record.generation;
  track.generation = record.generation;
  strncpy(track.id, record.id, sizeof(track.id)); track.id[sizeof(track.id) - 1] = 0;
  strncpy(track.address, record.address, sizeof(track.address)); track.address[sizeof(track.address) - 1] = 0;
  track.idType = record.idType; track.lastSeen = record.lastSeen;
  if (track.sampled && uint32_t(now - track.lastSample) < 1000) return true;
  const bool stateChanged = track.messages != record.messageCount || track.lost != record.lostCount ||
                            track.reacquired != record.reacquiredCount;
  if (!track.sampled || stateChanged || newGeneration) {
    if (!append(base_ + ".csv", csvRow(record, now))) return false;
    track.lastSample = now; track.sampled = true;
    track.messages = record.messageCount; track.lost = record.lostCount;
    track.reacquired = record.reacquiredCount;
  }
  const bool fresh = record.locationValid && record.historyCount &&
                     uint32_t(now - record.locationSeen) < kLostMs;
  if (fresh && track.points && uint32_t(record.locationSeen - track.lastLocation) >= kLostMs)
    track.pendingBreak = true;
  if (fresh && (newGeneration || track.revision != record.locationRevision || track.pendingBreak)) {
    Point point = record.history[record.historyCount - 1];
    point.observedMs = record.locationSeen;
    // Finish each segment at a long reception gap, avoiding a fictitious line
    // between a lost aircraft and its later reacquisition.
    std::string line;
    if (track.pendingBreak) line = "</trkseg><trkseg>\n";
    line += gpxPoint(point);
    if (!append(trackPath(slot), line)) return false;
    track.revision = record.locationRevision;
    track.pendingBreak = false; ++track.points;
  }
  if (fresh) track.lastLocation = record.locationSeen;
  return true;
}
bool Export::finish() {
  if (!active()) return false;
  bool ok = healthy_;
  for (unsigned i = 0; i < 16 && ok; ++i) ok = flushTrack(i);
  if (ok) ok = append(base_ + ".part", "</gpx>\n");
  if (ok) ok = fs_->rename(base_ + ".part", base_ + ".gpx");
  healthy_ &= ok;
  fs_ = nullptr;
  return ok;
}
} // namespace drone_tracking
