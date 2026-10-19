#pragma once
#include <FS.h>
#include "DroneTracking.h"

namespace drone_tracking {
// SD/file writes occur in the foreground only. A bounded set of temporary
// tracks is streamed into a GPX staging file; completed files are renamed only
// after every write succeeds. Failed exports retain their recoverable parts.
class Export {
 public:
  bool begin(fs::FS& filesystem, const String& base);
  bool sample(const Record& record, uint32_t now);
  bool finish();
  bool active() const { return fs_ != nullptr; }
  bool healthy() const { return healthy_; }
  const String& base() const { return base_; }
 private:
  struct Track {
    bool used = false;
    uint32_t generation = 0, revision = 0, lastLocation = 0, lastSample = 0;
    uint32_t lastSeen = 0, messages = 0, lost = 0, reacquired = 0;
    bool sampled = false, pendingBreak = false;
    char id[21] = {}, address[18] = {};
    uint8_t idType = 0;
    uint32_t points = 0;
  } tracks_[16];
  fs::FS* fs_ = nullptr;
  String base_;
  bool healthy_ = true;
  bool flushTrack(unsigned slot);
  String trackPath(unsigned slot) const;
  bool append(const String& path, const std::string& text);
};
} // namespace drone_tracking
