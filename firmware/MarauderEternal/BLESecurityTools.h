#pragma once

#include "configs.h"
#include "WiFiScan.h"

namespace BLESecurityTools {

void selectTarget(const BleDevice& device);
bool hasTarget();
String selectedTargetLabel();
String deviceDisplayLabel(const BleDevice& device);
const char* manufacturerName(const BleDevice& device);

void showAdvertisedInfo();
void inspectTarget();
void runDeviceSpoof();

}  // namespace BLESecurityTools
