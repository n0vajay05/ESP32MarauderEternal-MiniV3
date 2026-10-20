#pragma once
#include <algorithm>
#include <cstring>
#include <string>

namespace marauder { namespace ota {
class Keyboard {
 public:
  explicit Keyboard(bool numeric = false) : numeric(numeric) {}
  bool numeric = false, actions = false;
  unsigned page = 0, selected = 0;
  std::string value;
  size_t maximum = 64;
  const char* keys() const {
    if (numeric) return "123456789.0";
    static const char* pages[] = {"abcdefghijklmnopqrstuvwxyz ", "ABCDEFGHIJKLMNOPQRSTUVWXYZ ",
                                 "0123456789 ", " !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~"};
    return pages[page % 4];
  }
  unsigned columns() const { return numeric ? 3 : 6; }
  unsigned actionCount() const { return numeric ? 3 : 4; }
  void move(int dx, int dy) {
    const unsigned count = strlen(keys()), cols = columns();
    if (actions) {
      if (dx) selected = (selected + actionCount() + dx) % actionCount();
      if (dy) { selected = std::min(count - 1, (dy < 0 ? ((count - 1) / cols) * cols : 0) + selected * cols / actionCount()); actions = false; }
      return;
    }
    if (dx) selected = (selected + count + dx) % count;
    if (dy < 0 && selected < cols) { selected = selected * actionCount() / cols; actions = true; }
    else if (dy > 0 && selected / cols == (count - 1) / cols) { selected = (selected % cols) * actionCount() / cols; actions = true; }
    else if (dy) selected = std::min(count - 1, unsigned(int(selected) + dy * int(cols)));
  }
  // 0: editing, 1: entered, -1: cancelled. Enter is always bottom right.
  int choose() {
    if (!actions) { if (value.size() < maximum) value += keys()[selected]; return 0; }
    if (selected + 1 == actionCount()) return 1;
    if (selected + 2 == actionCount()) return -1;
    if (selected + 3 == actionCount()) { if (!value.empty()) value.pop_back(); return 0; }
    page = (page + 1) % 4; selected = 0; actions = false; return 0;
  }
};
} }
