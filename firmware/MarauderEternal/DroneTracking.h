#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include "DroneRemoteID.h"

// Passive telemetry model. Encoding follows opendroneid-core-c (Apache-2.0),
// https://github.com/opendroneid/opendroneid-core-c. No radio or filesystem IO.
namespace drone_tracking {
constexpr unsigned kHistory = 10;
constexpr uint32_t kLostMs = 30000;
constexpr uint32_t kReuseMs = 120000;
constexpr float kUnknownAltitude = -1000.0f;
struct Point {
  int32_t latitudeE7 = 0, longitudeE7 = 0;
  float altitude = kUnknownAltitude;
  uint32_t observedMs = 0;
};
struct Record {
  char address[18] = {}, wifiAddress[18] = {}, bleAddress[18] = {};
  char id[21] = {}, operatorId[21] = {}, description[24] = {};
  double latitude = 0, longitude = 0, operatorLatitude = 0, operatorLongitude = 0;
  float altitude = kUnknownAltitude, height = kUnknownAltitude;
  float speed = 255, direction = 361, verticalSpeed = 63;
  float operatorAltitude = kUnknownAltitude;
  uint32_t firstSeen = 0, lastSeen = 0, locationSeen = 0, generation = 0;
  uint32_t messageCount = 0, rateStarted = 0, rateMessages = 0;
  uint32_t lastLost = 0, lastReacquired = 0, systemSeen = 0;
  uint32_t messageSeen[6] = {};
  uint32_t authenticationTimestamp = 0;
  uint8_t authenticationType = 0, authenticationPage = 0;
  uint8_t authenticationLastPage = 0, authenticationLength = 0;
  uint32_t locationRevision = 0, lostCount = 0, reacquiredCount = 0;
  float messageRate = 0;
  int8_t rssi = 0;
  uint8_t channel = 0, uaType = 0, idType = 0, status = 0;
  uint8_t messagesSeen = 0, methods = 0, rawMessageMask = 0;
  uint8_t rawMessages[DroneRemoteID::CAPTURED_MESSAGE_TYPES][25] = {};
  bool locationValid = false, operatorLocationValid = false, lost = false, pinned = false;
  Point history[kHistory];
  uint8_t historyCount = 0;
};
bool coordinates(double latitude, double longitude);
bool validMessage(const uint8_t* message, size_t length);
bool apply(Record& record, const uint8_t* message, size_t length, uint32_t now, bool countReception = true);
void lifecycle(Record& record, uint32_t now);
bool sameIdentity(const Record& a, const Record& b);
// Bounded store; caller protects it against radio/main-loop concurrency.
Record* observe(Record* records, unsigned& count, unsigned capacity,
                const char* address, int8_t rssi, uint8_t channel,
                uint8_t method, const uint8_t* message, size_t length,
                uint32_t now, uint32_t& nextGeneration);
double distanceMeters(double lat1, double lon1, double lat2, double lon2);
double bearingDegrees(double lat1, double lon1, double lat2, double lon2);
std::string csvField(const char* text);
std::string xmlText(const char* text);
std::string csvHeader();
std::string csvRow(const Record& record, uint32_t now);
std::string gpxPoint(const Point& point);
} // namespace drone_tracking
