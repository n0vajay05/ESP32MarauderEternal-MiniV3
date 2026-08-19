#pragma once

#include "DroneRemoteID.h"

namespace DroneRemoteIDSpoofer {
bool selectTarget(const DroneRemoteID::CapturedDrone& target);
void run();
}
