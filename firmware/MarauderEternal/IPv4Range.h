#pragma once

#include <cstdint>

namespace marauder {

struct IPv4HostRange {
  uint32_t network;
  uint32_t broadcast;
  uint32_t first;
  uint32_t last;
  bool valid;
};

IPv4HostRange ipv4HostRange(uint32_t address, uint32_t subnetMask);
uint32_t nextIPv4Host(uint32_t current, const IPv4HostRange& range);
uint32_t previousIPv4Host(uint32_t current, uint32_t steps,
                          const IPv4HostRange& range);

// Enumerates the complete subnet independently of the gateway's address.
// Zero marks exhaustion and stays exhausted until reset().
class IPv4HostCursor {
 public:
  void reset(uint32_t localAddress, uint32_t subnetMask);
  uint32_t next();

 private:
  IPv4HostRange range_{};
  uint32_t local_ = 0;
  uint32_t current_ = 0;
  bool exhausted_ = true;
};

}  // namespace marauder
