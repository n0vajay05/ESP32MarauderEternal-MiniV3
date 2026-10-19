#pragma once
#include <Arduino.h>

// Explicit serial diagnostics. Fixtures contain synthetic data only and never
// read, export, or replace the user's settings file.
void runWiFiChecks(const String& phase);
void runGeofenceChecks(const String& phase);

void runGpsTrackerChecks();
void runFoxChecks();
void runTrackerChecks();
void runMirrorChecks();
