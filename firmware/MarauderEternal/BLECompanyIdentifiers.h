#pragma once

#include <Arduino.h>

namespace BLECompanyIdentifiers {

const char* lookup(uint16_t companyId);
size_t count();

}  // namespace BLECompanyIdentifiers
