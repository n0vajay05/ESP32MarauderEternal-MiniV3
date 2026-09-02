#pragma once

#ifndef EvilPortal_h
#define EvilPortal_h

#include "ESPAsyncWebServer.h"
#include <AsyncTCP.h>
#include <DNSServer.h>
#include <WiFi.h>

#include "configs.h"
#include "settings.h"
#include "RsnCapabilities.h"
#ifdef HAS_SCREEN
  #include "Display.h"
  #include <LinkedList.h>
#endif
#include "SDInterface.h"
#include "Buffer.h"
#include "lang_var.h"

extern Settings settings_obj;
extern SDInterface sd_obj;
#ifdef HAS_SCREEN
  extern Display display_obj;
#endif
extern Buffer buffer_obj; 

#define WAITING 0
#define GOOD 1
#define BAD 2

#define SET_HTML_CMD "sethtml="
#define SET_AP_CMD "setap="
#define RESET_CMD "reset"
#define START_CMD "start"
#define ACK_CMD "ack"
#define MAX_AP_NAME_SIZE 33
#define WIFI_SCAN_EVIL_PORTAL 30

extern char apName[MAX_AP_NAME_SIZE];

#ifndef HAS_PSRAM
  extern char index_html[MAX_HTML_SIZE];
#else
  extern char* index_html;
#endif

struct ssid {
  String essid;
  uint8_t channel;
  uint8_t bssid[6];
  bool selected;
  bool persistent = false;
};

struct AccessPoint {
  String essid;
  uint8_t channel;
  uint8_t bssid[6];
  bool selected;
 // LinkedList<char>* beacon;
  char beacon[2];
  int8_t rssi;
  LinkedList<uint16_t>* stations = nullptr;
  uint16_t packets;
  uint8_t sec;
  bool wps;
  String man;
  bool has_msg_1;
  bool has_msg_2;
  bool has_msg_3;
  bool has_msg_4;
  PmfStatus pmf_status = PMF_STATUS_UNKNOWN;
  uint32_t last_seen_ms;
};

struct PortalCredential {
  uint32_t captured_at_ms;
  String ssid;
  String username;
  String password;
};

class CaptiveRequestHandler : public AsyncWebHandler {
public:
  CaptiveRequestHandler() {}
  virtual ~CaptiveRequestHandler() {}

  bool canHandle(AsyncWebServerRequest *request) { return true; }

  void handleRequest(AsyncWebServerRequest *request) {
    #ifdef HAS_PSRAM
      if (index_html == nullptr) {
        request->send(503, "text/plain", "Portal content is not loaded");
        return;
      }
      request->send(200, "text/html", index_html);
    #else
      request->send_P(200, "text/html", index_html);
    #endif
  }
};

class EvilPortal {

  private:
    bool runServer;
    bool name_received;
    bool password_received;

    String user_name;
    String password;

    bool has_html;
    int target_ap_index = -1;
    uint8_t target_ap_channel = 1;
    int session_credential_count = 0;

    DNSServer dnsServer;

    void (*resetFunction)(void) = 0;

    bool setHtml();
    bool setAP(LinkedList<ssid>* ssids, LinkedList<AccessPoint>* access_points);
    void setupServer();
    bool startPortal();
    bool startAP();
    void sendToDisplay(String msg);
    void loadCredentials();
    bool storeCredential(const String& username, const String& password);
    bool installHtml(const char* html, size_t length);

  public:
    String target_html_name = "index.html";
    uint8_t selected_html_index = 0;

    bool using_serial_html;
    bool has_ap;

    LinkedList<String>* html_files;
    LinkedList<PortalCredential>* captured_credentials;

    void cleanup();
    int getCredentialCount();
    String getCredentialDisplayLabel(int index);
    const PortalCredential* getCredential(int index);
    int getSessionCredentialCount();
    const PortalCredential* getSessionCredential(int index);
    uint8_t getConnectedClientCount();
    bool isRunning() const;
    bool clearCredentials();
    String get_user_name();
    String get_password();
    bool setAP(String essid);
    bool setAPFromConfig();
    void setTargetAP(int index, uint8_t channel);
    int getTargetAPIndex() const;
    uint8_t getTargetAPChannel() const;
    void setup();
    void refreshHtmlFiles();
    bool begin(LinkedList<ssid>* ssids, LinkedList<AccessPoint>* access_points);
    void main(uint8_t scan_mode);
    void setHtmlFromSerial();

};

#endif
