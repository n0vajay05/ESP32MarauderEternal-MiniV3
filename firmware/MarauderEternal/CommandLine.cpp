#include "CommandLine.h"

#include <errno.h>
#include <limits.h>
#ifdef HAS_SD
  #include "SdTransferPath.h"
  #include "mbedtls/sha256.h"
#endif

namespace {
  bool validTransactionId(const String& transaction_id) {
    if (transaction_id.length() == 0 || transaction_id.length() > 40)
      return false;

    for (size_t i = 0; i < transaction_id.length(); i++) {
      char c = transaction_id.charAt(i);
      if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') || c == '-' || c == '_' || c == '.'))
        return false;
    }
    return true;
  }

  void machineResult(
    const String& transaction_id,
    const char* command,
    const char* status,
    const char* code,
    size_t files = 0,
    size_t bytes = 0,
    bool rebooting = false
  ) {
    Serial.printf(
      "@MARAUDER:{\"protocol\":1,\"tx\":\"%s\",\"command\":\"%s\","
      "\"status\":\"%s\",\"code\":\"%s\",\"files\":%u,\"bytes\":%u,"
      "\"rebooting\":%s}\n",
      transaction_id.c_str(), command, status, code,
      static_cast<unsigned int>(files), static_cast<unsigned int>(bytes),
      rebooting ? "true" : "false"
    );
  }

  const char* storageErrorCode(uint8_t error, const char* fallback) {
    if (error == 1)
      return "SD_NOT_READY";
    if (error == 2)
      return "BACKUP_NOT_FOUND";
    return fallback;
  }

  #ifdef HAS_SD
  constexpr uint8_t SD_LIST_MAX_DEPTH = 8;

  String joinedSdPath(const String& directory, const String& name) {
    const String base_name = marauder::storage::baseName(name);
    return directory == "/" ? "/" + base_name
                              : directory + "/" + base_name;
  }

  String normalizedSdEntryPath(const String& directory, File& entry) {
    String path = entry.path();
    const String expected_prefix = directory == "/" ? "/" : directory + "/";
    if (path.length() == 0 || !path.startsWith(expected_prefix))
      path = joinedSdPath(directory, entry.name());
    return path;
  }

  void printHexString(const String& value) {
    static constexpr char hex[] = "0123456789abcdef";
    for (size_t index = 0; index < value.length(); ++index) {
      const uint8_t byte = static_cast<uint8_t>(value.charAt(index));
      Serial.write(hex[byte >> 4]);
      Serial.write(hex[byte & 0x0f]);
    }
  }

  void machineSdSummary(const String& transaction_id, const char* command,
                        const char* status, const char* code,
                        uint64_t files = 0, uint64_t bytes = 0) {
    Serial.printf(
        "@MARAUDER:{\"protocol\":1,\"tx\":\"%s\",\"command\":\"%s\","
        "\"status\":\"%s\",\"code\":\"%s\",\"files\":%llu,"
        "\"bytes\":%llu}\n",
        transaction_id.c_str(), command, status, code,
        static_cast<unsigned long long>(files),
        static_cast<unsigned long long>(bytes));
  }

  void machineSdFile(const String& transaction_id, const String& path,
                     uint64_t bytes, uint64_t modified) {
    Serial.printf(
        "@MARAUDER:{\"protocol\":1,\"tx\":\"%s\",\"command\":\"sdlist\","
        "\"status\":\"file\",\"code\":\"OK\",\"pathHex\":\"",
        transaction_id.c_str());
    printHexString(path);
    Serial.printf("\",\"bytes\":%llu,\"modified\":%llu}\n",
                  static_cast<unsigned long long>(bytes),
                  static_cast<unsigned long long>(modified));
  }

  bool streamSdDirectory(const String& transaction_id,
                         const String& directory, uint8_t depth,
                         uint64_t& files, uint64_t& bytes) {
    if (depth > SD_LIST_MAX_DEPTH)
      return false;

    File dir = SD.open(directory, FILE_READ);
    if (!dir || !dir.isDirectory()) {
      if (dir)
        dir.close();
      return false;
    }

    File entry = dir.openNextFile();
    while (entry) {
      const String path = normalizedSdEntryPath(directory, entry);
      const bool is_directory = entry.isDirectory();
      const uint64_t file_size = is_directory ? 0 : entry.size();
      const time_t last_write = entry.getLastWrite();
      const uint64_t modified = last_write > 0
                                    ? static_cast<uint64_t>(last_write) : 0;
      entry.close();

      if (is_directory) {
        if (!streamSdDirectory(transaction_id, path, depth + 1, files,
                               bytes)) {
          dir.close();
          return false;
        }
      }
      else {
        machineSdFile(transaction_id, path, file_size, modified);
        ++files;
        bytes += file_size;
      }
      entry = dir.openNextFile();
    }
    dir.close();
    return true;
  }

  void machineSdGetStarted(const String& transaction_id,
                           const String& path, uint64_t bytes) {
    Serial.printf(
        "@MARAUDER:{\"protocol\":1,\"tx\":\"%s\",\"command\":\"sdget\","
        "\"status\":\"started\",\"code\":\"OK\",\"pathHex\":\"",
        transaction_id.c_str());
    printHexString(path);
    Serial.printf("\",\"bytes\":%llu}\n",
                  static_cast<unsigned long long>(bytes));
    Serial.flush();
  }

  void machineSdGetFinished(const String& transaction_id,
                            const char* status, const char* code,
                            uint64_t bytes, const char* sha256 = nullptr) {
    Serial.printf(
        "@MARAUDER:{\"protocol\":1,\"tx\":\"%s\",\"command\":\"sdget\","
        "\"status\":\"%s\",\"code\":\"%s\",\"bytes\":%llu",
        transaction_id.c_str(), status, code,
        static_cast<unsigned long long>(bytes));
    if (sha256 != nullptr)
      Serial.printf(",\"sha256\":\"%s\"", sha256);
    Serial.println("}");
  }

  void machineSdPutResult(const String& transaction_id,
                          const char* status, const char* code,
                          const String* path, uint64_t bytes,
                          const char* sha256 = nullptr) {
    Serial.printf(
        "@MARAUDER:{\"protocol\":1,\"tx\":\"%s\",\"command\":\"sdput\","
        "\"status\":\"%s\",\"code\":\"%s\",\"bytes\":%llu",
        transaction_id.c_str(), status, code,
        static_cast<unsigned long long>(bytes));
    if (path != nullptr) {
      Serial.print(F(",\"pathHex\":\""));
      printHexString(*path);
      Serial.print('"');
    }
    if (sha256 != nullptr)
      Serial.printf(",\"sha256\":\"%s\"", sha256);
    Serial.println("}");
    Serial.flush();
  }

  void writeTransferPadding(uint64_t bytes) {
    uint8_t padding[128] = {};
    while (bytes > 0) {
      const size_t count = bytes > sizeof(padding) ? sizeof(padding)
                                                   : static_cast<size_t>(bytes);
      Serial.write(padding, count);
      bytes -= count;
      delay(0);
    }
  }
  #endif
}

// Brightness functions defined in esp32_marauder.ino
#if !defined(HAS_MINI_SCREEN) || defined(MARAUDER_MINI_V3)
  extern void brightnessCycle();
  extern void brightnessSave(uint8_t level);
  extern uint8_t getBrightnessLevel();
#endif

void CommandLine::RunSetup() {
  Serial.println(this->ascii_art);

  Serial.println(F("\n\n--------------------------------\n"));
  Serial.println(F("     ESP32 Marauder Eternal  \n"));
  Serial.println("            " + version_number + "\n");
  Serial.println(F("By: JustCallMeKoKo/n0vajay05\n"));
  Serial.println(F("--------------------------------\n\n"));
  
  Serial.print("> ");
}

bool CommandLine::sdSessionActive() const {
  return this->sd_session_active;
}

void CommandLine::showSdSessionStatus(const char* status,
                                      const String& detail, int progress) {
  #ifdef HAS_SCREEN
    display_obj.tft.fillScreen(TFT_BLACK);
    display_obj.tft.setFreeFont(NULL);
    display_obj.tft.setTextSize(1);
    display_obj.tft.setTextWrap(false);
    display_obj.tft.setTextColor(0x733F, TFT_BLACK);
    display_obj.tft.drawCentreString("USB SD FILES", TFT_WIDTH / 2, 6, 1);
    display_obj.tft.drawFastHLine(8, 20, TFT_WIDTH - 16, 0x31A6);

    display_obj.tft.setTextColor(TFT_WHITE, TFT_BLACK);
    display_obj.tft.drawCentreString(status, TFT_WIDTH / 2, 30, 2);

    String visible_detail = detail;
    if (visible_detail.length() > 20)
      visible_detail = "..." + visible_detail.substring(visible_detail.length() - 17);
    if (visible_detail.length() > 0) {
      display_obj.tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
      display_obj.tft.drawCentreString(visible_detail, TFT_WIDTH / 2, 52, 1);
    }

    if (progress >= 0) {
      progress = constrain(progress, 0, 100);
      const int16_t bar_x = 12;
      const int16_t bar_y = 69;
      const int16_t bar_width = TFT_WIDTH - 24;
      display_obj.tft.drawRect(bar_x, bar_y, bar_width, 12, 0x31A6);
      display_obj.tft.fillRect(bar_x + 2, bar_y + 2,
                               (bar_width - 4) * progress / 100, 8,
                               TFT_CYAN);
      display_obj.tft.setTextColor(TFT_WHITE, TFT_BLACK);
      display_obj.tft.drawCentreString(String(progress) + "%",
                                       TFT_WIDTH / 2, 85, 1);
    }

    display_obj.tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    display_obj.tft.drawCentreString("CONTROLS LOCKED",
                                     TFT_WIDTH / 2, 103, 1);
    #ifdef MARAUDER_MINI_V3
      display_obj.tft.setTextColor(TFT_GREEN, TFT_BLACK);
      display_obj.tft.drawCentreString("HOLD CENTER: EXIT",
                                       TFT_WIDTH / 2, 116, 1);
    #endif
  #else
    (void)status;
    (void)detail;
    (void)progress;
  #endif
}

void CommandLine::exitSdSession() {
  this->sd_session_active = false;
  #ifdef HAS_SCREEN
    display_obj.clearScreen();
    menu_function_obj.changeMenu(menu_function_obj.current_menu, true);
  #endif
}

String CommandLine::getSerialInput() {
  String input = "";

  if (Serial.available() > 0)
    input = Serial.readStringUntil('\n');

  input.trim();
  return input;
}

void CommandLine::main(uint32_t currentTime) {
  #if defined(MARAUDER_MINI_V3) && defined(HAS_BUTTONS)
    if (this->sd_session_active)
      c_btn.justPressed();
    if (this->sd_session_active && c_btn.isHeld()) {
      while (digitalRead(c_btn.getPin()) ==
             (c_btn.getPullup() ? LOW : HIGH))
        delay(10);
      this->exitSdSession();
      Serial.println(F("USB SD mode closed from device controls"));
    }
  #endif

  String input = this->getSerialInput();

  const bool sd_operation = input.startsWith("sdlist ") ||
                            input.startsWith("sdget ") ||
                            input.startsWith("sdput ");

  this->runCommand(input);

  if (this->sd_session_active && sd_operation)
    this->showSdSessionStatus("CONNECTED", "Waiting for PC");

  if (input != "")
    Serial.print("> ");
}

LinkedList<String> CommandLine::parseCommand(String input, const char* delim) {
  LinkedList<String> cmd_args;

  bool inQuote = false;
  bool inApostrophe = false;
  String buffer = "";

  for (int i = 0; i < input.length(); i++) {
    char c = input.charAt(i);

    if (c == '"') {
      // Check if the quote is within an apostrophe
      if (inApostrophe) {
        buffer += c;
      } else {
        inQuote = !inQuote;
      }
    } else if (c == '\'') {
      // Check if the apostrophe is within a quote
      if (inQuote) {
        buffer += c;
      } else {
        inApostrophe = !inApostrophe;
      }
    } else if (!inQuote && !inApostrophe && strchr(delim, c) != NULL) {
      cmd_args.add(buffer);
      buffer = "";
    } else {
      buffer += c;
    }
  }

  // Add the last argument
  if (!buffer.isEmpty()) {
    cmd_args.add(buffer);
  }

  return cmd_args;
}

int CommandLine::argSearch(LinkedList<String>* cmd_args_list, const char* key) {
  if (!cmd_args_list || !key)
    return -1;

  for (int i = 0; i < cmd_args_list->size(); i++) {
    if (strcmp(cmd_args_list->get(i).c_str(), key) == 0)
      return i;
  }

  return -1;
}

bool CommandLine::checkValueExists(LinkedList<String>* cmd_args_list, int index) {
  return cmd_args_list != nullptr && index >= 0 &&
         index < cmd_args_list->size() - 1;
}

bool CommandLine::argumentValue(LinkedList<String>* cmd_args_list,
                                int flag_index, const char* flag,
                                String& value) {
  if (!checkValueExists(cmd_args_list, flag_index)) {
    Serial.print(F("Missing value after "));
    Serial.println(flag == nullptr ? "option" : flag);
    return false;
  }
  value = cmd_args_list->get(flag_index + 1);
  if (value.length() == 0) {
    Serial.print(F("Empty value after "));
    Serial.println(flag == nullptr ? "option" : flag);
    return false;
  }
  return true;
}

bool CommandLine::integerValue(String value, int& result, const char* label) {
  value.trim();
  if (value.length() == 0) {
    Serial.print(F("Missing numeric value for "));
    Serial.println(label == nullptr ? "argument" : label);
    return false;
  }

  errno = 0;
  char* end = nullptr;
  const long parsed = strtol(value.c_str(), &end, 10);
  if (errno == ERANGE || end == value.c_str() || *end != '\0' ||
      parsed < INT_MIN || parsed > INT_MAX) {
    Serial.print(F("Invalid numeric value for "));
    Serial.print(label == nullptr ? "argument" : label);
    Serial.print(F(": "));
    Serial.println(value);
    return false;
  }

  result = static_cast<int>(parsed);
  return true;
}

bool CommandLine::inRange(int max, int index) {
  if ((index >= 0) && (index < max))
    return true;

  return false;
}

/*bool CommandLine::apSelected() {
  for (int i = 0; i < access_points->size(); i++) {
    if (access_points->get(i).selected)
      return true;
  }

  return false;
}*/

bool CommandLine::hasSSIDs() {
  if (ssids->size() == 0)
    return false;

  return true;
}

void CommandLine::showCounts(int selected, int unselected) {
  Serial.print((String) selected + " selected");
  
  if (unselected != -1) 
    Serial.print(", " + (String) unselected + " unselected");
  
  Serial.println("");
}

String CommandLine::toLowerCase(String str) {
  String result = str;
  for (int i = 0; i < str.length(); i++) {
    int charValue = str.charAt(i);
    if (charValue >= 65 && charValue <= 90) { // ASCII codes for uppercase letters
      charValue += 32;
      result.setCharAt(i, char(charValue));
    }
  }
  return result;
}

void CommandLine::filterAccessPoints(String filter) {
  int count_selected = 0;
  int count_unselected = 0;

  // Split the filter string into individual filters
  LinkedList<String> filters;
  int start = 0;
  int end = filter.indexOf(" or ");
  while (end != -1) {
    filters.add(filter.substring(start, end));
    start = end + 4;
    end = filter.indexOf(" or ", start);
  }
  filters.add(filter.substring(start));

  // Loop over each access point and check if it matches any of the filters
  for (int i = 0; i < access_points->size(); i++) {
    AccessPoint access_point = access_points->get(i);
    bool matchesFilter = false;
    for (int j = 0; j < filters.size(); j++) {
      String f = toLowerCase(filters.get(j));
      if (f.substring(0, 7) == "equals ") {
        String ssidEquals = f.substring(7);
        if ((ssidEquals.charAt(0) == '\"' && ssidEquals.charAt(ssidEquals.length() - 1) == '\"' && ssidEquals.length() > 1) ||
            (ssidEquals.charAt(0) == '\'' && ssidEquals.charAt(ssidEquals.length() - 1) == '\'' && ssidEquals.length() > 1)) {
          ssidEquals = ssidEquals.substring(1, ssidEquals.length() - 1);
        }
        if (access_point.essid.equalsIgnoreCase(ssidEquals)) {
          matchesFilter = true;
          break;
        }
      } else if (f.substring(0, 9) == "contains ") {
        String ssidContains = f.substring(9);
        if ((ssidContains.charAt(0) == '\"' && ssidContains.charAt(ssidContains.length() - 1) == '\"' && ssidContains.length() > 1) ||
            (ssidContains.charAt(0) == '\'' && ssidContains.charAt(ssidContains.length() - 1) == '\'' && ssidContains.length() > 1)) {
          ssidContains = ssidContains.substring(1, ssidContains.length() - 1);
        }
        String essid = toLowerCase(access_point.essid);
        if (essid.indexOf(ssidContains) != -1) {
          matchesFilter = true;
          break;
        }
      }
    }
    // Toggles the selected state of the AP
    access_point.selected = matchesFilter;
    access_points->set(i, access_point);

    if (matchesFilter) {
      count_selected++;
    } else {
      count_unselected++;
    }
  }

  this->showCounts(count_selected, count_unselected);
}

void CommandLine::startScanFromCLI(int scan_mode, uint16_t color, const char* scan_name) {
  Serial.print(F("Starting"));
  Serial.print(scan_name);
  Serial.print(F(". Stop with "));
  Serial.println(STOPSCAN_CMD);
  #ifdef HAS_SCREEN
    display_obj.clearScreen();
    menu_function_obj.drawStatusBar();
  #endif
  wifi_scan_obj.StartScan(scan_mode, color);
}

void CommandLine::runCommand(String input) {
  if (input == "") return;

  const bool storage_protocol_command =
      input.startsWith("protocolinfo ") || input.startsWith("sdlist ") ||
      input.startsWith("sdget ") || input.startsWith("sdput ") ||
      input.startsWith("sdsession ");
  if(wifi_scan_obj.scanning() && wifi_scan_obj.currentScanMode == WIFI_SCAN_GPS_NMEA){
    if(input != STOPSCAN_CMD && !storage_protocol_command) return;
  }
  Serial.println("#" + input);

  LinkedList<String> cmd_args = this->parseCommand(input, " ");

  if (this->sd_session_active &&
      cmd_args.get(0) != PROTOCOL_INFO_CMD &&
      cmd_args.get(0) != SD_LIST_CMD && cmd_args.get(0) != SD_GET_CMD &&
      cmd_args.get(0) != SD_PUT_CMD && cmd_args.get(0) != SD_SESSION_CMD) {
    Serial.println(F("USB SD mode is active; close it before using other commands"));
    return;
  }
  
  //// Admin commands
  // Help
  if (cmd_args.get(0) == HELP_CMD) {
    Serial.println(HELP_HEAD);
    Serial.println(HELP_CH_CMD);
    Serial.println(HELP_SETTINGS_CMD);
    Serial.println(HELP_CLEARAP_CMD_A);
    Serial.println(HELP_REBOOT_CMD);
    Serial.println(HELP_UPDATE_CMD_A);
    Serial.println(HELP_LS_CMD);
    Serial.println(HELP_PROTOCOL_INFO_CMD);
    Serial.println(HELP_BACKUP_SPIFFS_CMD);
    Serial.println(HELP_BACKUP_STATUS_CMD);
    Serial.println(HELP_RESTORE_SPIFFS_CMD);
    Serial.println(HELP_SD_LIST_CMD);
    Serial.println(HELP_SD_GET_CMD);
    Serial.println(HELP_SD_PUT_CMD);
    Serial.println(HELP_SD_SESSION_CMD);
    Serial.println(HELP_LED_CMD);
    Serial.println(HELP_GPS_DATA_CMD);
    Serial.println(HELP_GPS_CMD);
    Serial.println(HELP_NMEA_CMD);
    Serial.println(HELP_GPS_POI_CMD);
    Serial.println(HELP_GPS_TRACKER_CMD);
    Serial.println(HELP_SCREENSHOT_CMD);
    
    // WiFi sniff/scan
    Serial.println(HELP_EVIL_PORTAL_CMD);
    Serial.println(HELP_KARMA_CMD);
    Serial.println(HELP_PACKET_COUNT_CMD);
    Serial.println(HELP_PING_CMD);
    Serial.println(HELP_ARP_SCAN_CMD);
    Serial.println(HELP_PORT_SCAN_CMD);
    Serial.println(HELP_SIGSTREN_CMD);
    Serial.println(HELP_SCAN_ALL_CMD);
    //Serial.println(HELP_SCANSTA_CMD);
    Serial.println(HELP_SNIFF_RAW_CMD);
    Serial.println(HELP_SNIFF_BEACON_CMD);
    Serial.println(HELP_SNIFF_PROBE_CMD);
    Serial.println(HELP_SNIFF_PWN_CMD);
    Serial.println(HELP_SNIFF_PINESCAN_CMD);
    Serial.println(HELP_SNIFF_MULTISSID_CMD);
    Serial.println(HELP_SNIFF_DEAUTH_CMD);
    Serial.println(HELP_SNIFF_PMKID_CMD);
    Serial.println(HELP_SNIFF_SAE_CMD);
    Serial.println(HELP_STOPSCAN_CMD);
    #ifdef HAS_GPS
      Serial.println(HELP_WARDRIVE_CMD);
      Serial.println(HELP_WARDRIVEPOI_CMD);
    #endif
    Serial.println(HELP_MAC_TRACK_CMD);
    
    // WiFi attack
    Serial.println(HELP_ATTACK_CMD);
    
    // WiFi Aux
    Serial.println(HELP_INFO_CMD);
    Serial.println(HELP_LIST_AP_CMD_A);
    Serial.println(HELP_LIST_AP_CMD_B);
    Serial.println(HELP_LIST_AP_CMD_C);
    Serial.println(HELP_LIST_AP_CMD_D);
    Serial.println(HELP_LIST_AP_CMD_E);
    Serial.println(HELP_LIST_AP_CMD_F);
    Serial.println(HELP_LIST_AP_CMD_G);
    Serial.println(HELP_SEL_CMD_A);
    Serial.println(HELP_SSID_CMD_A);
    Serial.println(HELP_SSID_CMD_B);
    Serial.println(HELP_SAVE_CMD);
    Serial.println(HELP_LOAD_CMD);
    Serial.println(HELP_JOIN_CMD);
    Serial.println(HELP_MAC_CMD_A);
    Serial.println(HELP_MAC_CMD_B);
    Serial.println(HELP_MAC_CMD_C);
    Serial.println(HELP_MAC_CMD_D);
    Serial.println(HELP_ADD_CMD_A);
    Serial.println(HELP_ADD_CMD_B);
    Serial.println(HELP_UPLOAD_CMD);

    // Bluetooth sniff/scan
    #ifdef HAS_BT
      Serial.println(HELP_BT_SNIFF_CMD);
      Serial.println(HELP_BT_SPAM_CMD);
      Serial.println(HELP_BT_FINDMY_CMD);
      Serial.println(HELP_BT_SPOOFAT_CMD);
      Serial.println(HELP_BT_SKIM_CMD);
    #endif
    Serial.println(HELP_BRIGHTNESS_CMD);
    Serial.println(HELP_FOOT);
    return;
  }

  // Re-render the current menu into a 16-bit shadow framebuffer and stream it
  // as RGB888. Direct controller RAM readback returned uniform data through
  // this Mini V3's shared TFT_MISO path, so it cannot provide a useful frame.
  if (cmd_args.get(0) == SCREENSHOT_CMD) {
    #ifdef MARAUDER_MINI_V3
      constexpr size_t screenshot_row_bytes = TFT_WIDTH * 3;
      uint8_t row[screenshot_row_bytes];
      TFT_eSprite frame(&display_obj.tft);
      frame.setColorDepth(16);

      if (!frame.createSprite(TFT_WIDTH, TFT_HEIGHT) ||
          !menu_function_obj.renderCurrentMenu(frame)) {
        Serial.println(F("Screenshot unavailable: framebuffer allocation failed"));
        return;
      }

      Serial.printf("SCREENSHOT-BEGIN RGB888 %u %u %u\n",
                    static_cast<unsigned>(TFT_WIDTH),
                    static_cast<unsigned>(TFT_HEIGHT),
                    static_cast<unsigned>(TFT_WIDTH * TFT_HEIGHT * 3));
      Serial.flush();

      for (int16_t y = 0; y < TFT_HEIGHT; ++y) {
        for (int16_t x = 0; x < TFT_WIDTH; ++x) {
          const uint16_t pixel = frame.readPixel(x, y);
          row[x * 3] = (pixel >> 8) & 0xF8;
          row[x * 3] |= row[x * 3] >> 5;
          row[x * 3 + 1] = (pixel >> 3) & 0xFC;
          row[x * 3 + 1] |= row[x * 3 + 1] >> 6;
          row[x * 3 + 2] = (pixel << 3) & 0xF8;
          row[x * 3 + 2] |= row[x * 3 + 2] >> 5;
        }
        Serial.write(row, screenshot_row_bytes);
      }

      Serial.flush();
      Serial.println();
      Serial.println(F("SCREENSHOT-END"));
    #else
      Serial.println(F("Screenshot unavailable: this target has no menu framebuffer"));
    #endif
  }
  // Stop Scan
  else if (cmd_args.get(0) == STOPSCAN_CMD) {
    int f_arg = this->argSearch(&cmd_args, "-f");
    
    uint8_t old_scan_mode=wifi_scan_obj.currentScanMode;

    if (f_arg != -1) {
      WiFi.disconnect(true);
      delay(100);
    }

    wifi_scan_obj.StartScan(WIFI_SCAN_OFF);

    if(old_scan_mode == WIFI_SCAN_GPS_NMEA)
      Serial.println(F("END OF NMEA STREAM"));
    else if(old_scan_mode == WIFI_SCAN_GPS_DATA)
      Serial.println(F("Stopping GPS data updates"));
    else
      Serial.println(F("Stopping WiFi tran/recv"));

    // If we don't do this, the text and button coordinates will be off
    #ifdef HAS_SCREEN
      display_obj.init();
      menu_function_obj.changeMenu(menu_function_obj.current_menu);
    #endif
  }
  else if (cmd_args.get(0) == GPS_DATA_CMD) {
    #ifdef HAS_GPS
      if (gps_obj.getGpsModuleStatus()) {
        Serial.print(F("Getting GPS Data. Stop with "));
        Serial.println((String)STOPSCAN_CMD);
        wifi_scan_obj.currentScanMode = WIFI_SCAN_GPS_DATA;
        #ifdef HAS_SCREEN
          menu_function_obj.changeMenu(&menu_function_obj.gpsInfoMenu);
        #endif
        wifi_scan_obj.StartScan(WIFI_SCAN_GPS_DATA, TFT_CYAN);
      }
    #endif
  }
  else if (cmd_args.get(0) == GPS_CMD) {
    #ifdef HAS_GPS
      if (gps_obj.getGpsModuleStatus()) {
        int get_arg = this->argSearch(&cmd_args, "-g");
        int track_arg = this->argSearch(&cmd_args, "-t");
        int nmea_arg = this->argSearch(&cmd_args, "-n");

        if (get_arg != -1) {
          String gps_info;
          if (!this->argumentValue(&cmd_args, get_arg, "-g", gps_info))
            return;

          if (gps_info == "fix")
            Serial.println("Fix: " + gps_obj.getFixStatusAsString());
          else if (gps_info == "sat")
            Serial.println("Sats: " + gps_obj.getNumSatsString());
          else if (gps_info == "lat")
            Serial.println("Lat: " + gps_obj.getLat());
          else if (gps_info == "lon")
            Serial.println("Lon: " + gps_obj.getLon());
          else if (gps_info == "alt")
            Serial.println("Alt: " + (String)gps_obj.getAlt());
          else if (gps_info == "accuracy")
            Serial.println("Accuracy: " + (String)gps_obj.getAccuracy());
          else if (gps_info == "date")
            Serial.println("Date/Time: " + gps_obj.getDatetime());
          else if (gps_info == "text"){
            Serial.println(gps_obj.getText());
          }
          else if (gps_info == "nmea"){
            int notparsed_arg = this->argSearch(&cmd_args, "-p");
            int notimp_arg = this->argSearch(&cmd_args, "-i");
            int recd_arg = this->argSearch(&cmd_args, "-r");
            if(notparsed_arg == -1 && notimp_arg == -1 && recd_arg == -1){
              gps_obj.sendSentence(Serial, gps_obj.generateGXgga().c_str());
              gps_obj.sendSentence(Serial, gps_obj.generateGXrmc().c_str());
            }
            else if(notparsed_arg == -1 && notimp_arg == -1)
              Serial.println(gps_obj.getNmea());
            else if(notparsed_arg == -1)
              Serial.println(gps_obj.getNmeaNotimp());
            else
              Serial.println(gps_obj.getNmeaNotparsed());
          }
          else
            Serial.println(F("You did not provide a valid argument"));
        }
        else if(nmea_arg != -1){
          String nmea_type;
          if (!this->argumentValue(&cmd_args, nmea_arg, "-n", nmea_type))
            return;

          if (nmea_type == "native" || nmea_type == "all" || nmea_type == "gps" || nmea_type == "glonass"
              || nmea_type == "galileo" || nmea_type == "navic" || nmea_type == "qzss" || nmea_type == "beidou"){
            if(nmea_type == "beidou"){
              int beidou_bd_arg = this->argSearch(&cmd_args, "-b");
              if(beidou_bd_arg != -1)
                nmea_type="beidou_bd";
            }
            gps_obj.setType(nmea_type);
            Serial.print(F("GPS Output Type Set To: "));
            Serial.println(nmea_type);
          }
        }
        else if (track_arg != -1) {
          wifi_scan_obj.currentScanMode = GPS_TRACKER;
          #ifdef HAS_SCREEN
            menu_function_obj.changeMenu(&menu_function_obj.gpsInfoMenu);
          #endif
          wifi_scan_obj.StartScan(GPS_TRACKER, TFT_CYAN);
        }
        else if(cmd_args.size()>1)
          Serial.println(F("You did not provide a valid flag"));
      }
    #endif
  }
  else if (cmd_args.get(0) == NMEA_CMD) {
    #ifdef HAS_GPS
      if (gps_obj.getGpsModuleStatus()) {
        #ifdef HAS_SCREEN
          menu_function_obj.changeMenu(&menu_function_obj.gpsInfoMenu);
        #endif
        wifi_scan_obj.currentScanMode = WIFI_SCAN_GPS_NMEA;
        wifi_scan_obj.StartScan(WIFI_SCAN_GPS_NMEA, TFT_CYAN);
      }
    #endif
  }
  // LED command
  else if (cmd_args.get(0) == LED_CMD) {
    #if defined(PIN) && defined(HAS_NEOPIXEL_LED) 
      int hex_arg = this->argSearch(&cmd_args, "-s");
      int pat_arg = this->argSearch(&cmd_args, "-p");
      if (hex_arg != -1) {
        String hexstring;
        if (!this->argumentValue(&cmd_args, hex_arg, "-s", hexstring))
          return;
        int number = (int)strtol(&hexstring[1], NULL, 16);
        int r = number >> 16;
        int g = number >> 8 & 0xFF;
        int b = number & 0xFF;
        //Serial.println(r);
        //Serial.println(g);
        //Serial.println(b);
        led_obj.setColor(r, g, b);
        led_obj.setMode(MODE_CUSTOM);
      }
      else if (pat_arg != -1) {
        String pat_name;
        if (!this->argumentValue(&cmd_args, pat_arg, "-p", pat_name))
          return;
        pat_name.toLowerCase();
        if (pat_name == "rainbow") {
          led_obj.setMode(MODE_RAINBOW);
        }
      }
    #endif
  }
  // ls command
  else if (cmd_args.get(0) == LS_CMD) {
    #ifdef HAS_SD
      if (cmd_args.size() > 1)
        sd_obj.listDir(cmd_args.get(1));
    #endif
  }

  else if (cmd_args.get(0) == PROTOCOL_INFO_CMD) {
    int machine_arg = this->argSearch(&cmd_args, "--machine");
    String transaction_id = machine_arg >= 0 && machine_arg + 1 < cmd_args.size()
      ? cmd_args.get(machine_arg + 1) : "";
    if (machine_arg >= 0 && !validTransactionId(transaction_id))
      machineResult(transaction_id, PROTOCOL_INFO_CMD, "error", "INVALID_TRANSACTION");
    else if (machine_arg >= 0) {
      #ifdef HAS_SD
        Serial.printf(
          "@MARAUDER:{\"protocol\":1,\"tx\":\"%s\",\"command\":\"protocolinfo\","
          "\"status\":\"success\",\"code\":\"OK\",\"firmware\":\"%s\","
          "\"capabilities\":[\"spiffs-backup\",\"spiffs-backup-status\","
          "\"spiffs-restore\",\"sd-list\",\"sd-download\",\"sd-upload\","
          "\"sd-session\"],"
          "\"backupPath\":\"/spiffs\"}\n",
          transaction_id.c_str(), version_number.c_str()
        );
      #else
        Serial.printf(
          "@MARAUDER:{\"protocol\":1,\"tx\":\"%s\",\"command\":\"protocolinfo\","
          "\"status\":\"success\",\"code\":\"OK\",\"firmware\":\"%s\","
          "\"capabilities\":[]}\n",
          transaction_id.c_str(), version_number.c_str()
        );
      #endif
    }
    else {
      #ifdef HAS_SD
        Serial.println(F("SPIFFS migration protocol 1; backup path: /spiffs"));
      #else
        Serial.println(F("SPIFFS migration unavailable: SD not supported"));
      #endif
    }
  }

  else if (cmd_args.get(0) == SD_SESSION_CMD) {
    const int machine_arg = this->argSearch(&cmd_args, "--machine");
    const String transaction_id =
        machine_arg >= 0 && machine_arg + 1 < cmd_args.size()
            ? cmd_args.get(machine_arg + 1) : "";
    const int state_arg = this->argSearch(&cmd_args, "--state");
    const String state =
        state_arg >= 0 && state_arg + 1 < cmd_args.size()
            ? cmd_args.get(state_arg + 1) : "";
    if (machine_arg < 0 || !validTransactionId(transaction_id)) {
      machineResult(transaction_id, SD_SESSION_CMD, "error",
                    "INVALID_TRANSACTION");
      return;
    }
    if (state != "begin" && state != "end") {
      machineResult(transaction_id, SD_SESSION_CMD, "error",
                    "INVALID_STATE");
      return;
    }

    #ifdef HAS_SD
      if (state == "begin") {
        if (!sd_obj.supported) {
          machineResult(transaction_id, SD_SESSION_CMD, "error",
                        "SD_NOT_READY");
          return;
        }
        if (wifi_scan_obj.scanning()) {
          machineResult(transaction_id, SD_SESSION_CMD, "error",
                        "DEVICE_BUSY");
          return;
        }
        this->sd_session_active = true;
        this->showSdSessionStatus("CONNECTED", "Waiting for PC");
        machineResult(transaction_id, SD_SESSION_CMD, "success", "OK");
      }
      else {
        machineResult(transaction_id, SD_SESSION_CMD, "success", "OK");
        this->exitSdSession();
      }
    #else
      machineResult(transaction_id, SD_SESSION_CMD, "error",
                    "SD_NOT_SUPPORTED");
    #endif
  }

  else if (cmd_args.get(0) == SD_LIST_CMD) {
    const int machine_arg = this->argSearch(&cmd_args, "--machine");
    const String transaction_id =
        machine_arg >= 0 && machine_arg + 1 < cmd_args.size()
            ? cmd_args.get(machine_arg + 1) : "";
    if (machine_arg < 0) {
      Serial.println(F("SD file listing requires --machine <transaction-id>"));
      return;
    }
    if (!validTransactionId(transaction_id)) {
      machineResult(transaction_id, SD_LIST_CMD, "error",
                    "INVALID_TRANSACTION");
      return;
    }

    #ifdef HAS_SD
      if (!sd_obj.supported) {
        machineSdSummary(transaction_id, SD_LIST_CMD, "error",
                         "SD_NOT_READY");
        return;
      }
      if (wifi_scan_obj.scanning()) {
        machineSdSummary(transaction_id, SD_LIST_CMD, "error",
                         "DEVICE_BUSY");
        return;
      }

      this->showSdSessionStatus("LISTING FILES", "Reading SD card");
      machineSdSummary(transaction_id, SD_LIST_CMD, "started", "OK");
      uint64_t files = 0;
      uint64_t bytes = 0;
      if (streamSdDirectory(transaction_id, "/", 0, files, bytes))
        machineSdSummary(transaction_id, SD_LIST_CMD, "success", "OK",
                         files, bytes);
      else
        machineSdSummary(transaction_id, SD_LIST_CMD, "error",
                         "LIST_FAILED", files, bytes);
    #else
      machineResult(transaction_id, SD_LIST_CMD, "error",
                    "SD_NOT_SUPPORTED");
    #endif
  }

  else if (cmd_args.get(0) == SD_GET_CMD) {
    const int machine_arg = this->argSearch(&cmd_args, "--machine");
    const String transaction_id =
        machine_arg >= 0 && machine_arg + 1 < cmd_args.size()
            ? cmd_args.get(machine_arg + 1) : "";
    if (machine_arg < 0) {
      Serial.println(F("SD download requires --machine <transaction-id>"));
      return;
    }
    if (!validTransactionId(transaction_id)) {
      machineResult(transaction_id, SD_GET_CMD, "error",
                    "INVALID_TRANSACTION");
      return;
    }

    const int path_arg = this->argSearch(&cmd_args, "--path-hex");
    if (path_arg < 0 || path_arg + 1 >= cmd_args.size()) {
      machineResult(transaction_id, SD_GET_CMD, "error", "INVALID_PATH");
      return;
    }

    #ifdef HAS_SD
      if (!sd_obj.supported) {
        machineSdSummary(transaction_id, SD_GET_CMD, "error",
                         "SD_NOT_READY");
        return;
      }
      if (wifi_scan_obj.scanning()) {
        machineSdSummary(transaction_id, SD_GET_CMD, "error",
                         "DEVICE_BUSY");
        return;
      }

      char decoded_path[SD_TRANSFER_MAX_PATH_BYTES] = {};
      if (!decodeSdPathHex(cmd_args.get(path_arg + 1).c_str(), decoded_path,
                           sizeof(decoded_path)) ||
          !isSafeSdFilePath(decoded_path)) {
        machineSdSummary(transaction_id, SD_GET_CMD, "error",
                         "INVALID_PATH");
        return;
      }

      File file = SD.open(decoded_path, FILE_READ);
      if (!file) {
        machineSdSummary(transaction_id, SD_GET_CMD, "error",
                         "PATH_NOT_FOUND");
        return;
      }
      if (file.isDirectory()) {
        file.close();
        machineSdSummary(transaction_id, SD_GET_CMD, "error",
                         "NOT_A_FILE");
        return;
      }

      mbedtls_sha256_context sha_context;
      mbedtls_sha256_init(&sha_context);
      if (mbedtls_sha256_starts(&sha_context, 0) != 0) {
        file.close();
        mbedtls_sha256_free(&sha_context);
        machineSdSummary(transaction_id, SD_GET_CMD, "error",
                         "HASH_INIT_FAILED");
        return;
      }

      const uint64_t file_size = file.size();
      const String path(decoded_path);
      this->showSdSessionStatus("DOWNLOADING", path, 0);
      machineSdGetStarted(transaction_id, path, file_size);

      uint8_t transfer_buffer[1024];
      uint64_t transferred = 0;
      int last_display_progress = -1;
      const char* transfer_error = nullptr;
      while (transferred < file_size) {
        const uint64_t remaining = file_size - transferred;
        const size_t requested = remaining > sizeof(transfer_buffer)
                                     ? sizeof(transfer_buffer)
                                     : static_cast<size_t>(remaining);
        const size_t read_count = file.read(transfer_buffer, requested);
        if (read_count == 0) {
          transfer_error = "FILE_READ_FAILED";
          break;
        }
        if (mbedtls_sha256_update(&sha_context, transfer_buffer,
                                  read_count) != 0) {
          transfer_error = "HASH_UPDATE_FAILED";
          break;
        }
        const size_t write_count = Serial.write(transfer_buffer, read_count);
        transferred += write_count;
        if (file_size > 0) {
          const int display_progress =
              static_cast<int>(transferred * 100ULL / file_size);
          if (display_progress >= last_display_progress + 5 ||
              display_progress == 100) {
            last_display_progress = display_progress;
            this->showSdSessionStatus("DOWNLOADING", path,
                                      display_progress);
          }
        }
        if (write_count != read_count) {
          transfer_error = "SERIAL_WRITE_FAILED";
          break;
        }
        delay(0);
      }
      file.close();

      if (transfer_error != nullptr && transferred < file_size)
        writeTransferPadding(file_size - transferred);

      uint8_t digest[32] = {};
      if (transfer_error == nullptr &&
          mbedtls_sha256_finish(&sha_context, digest) != 0)
        transfer_error = "HASH_FINISH_FAILED";
      mbedtls_sha256_free(&sha_context);

      Serial.flush();
      Serial.println();
      if (transfer_error == nullptr) {
        char digest_hex[65] = {};
        static constexpr char hex[] = "0123456789abcdef";
        for (size_t index = 0; index < sizeof(digest); ++index) {
          digest_hex[index * 2] = hex[digest[index] >> 4];
          digest_hex[index * 2 + 1] = hex[digest[index] & 0x0f];
        }
        machineSdGetFinished(transaction_id, "success", "OK",
                             transferred, digest_hex);
      }
      else {
        machineSdGetFinished(transaction_id, "error", transfer_error,
                             transferred);
      }
    #else
      machineResult(transaction_id, SD_GET_CMD, "error",
                    "SD_NOT_SUPPORTED");
    #endif
  }

  else if (cmd_args.get(0) == SD_PUT_CMD) {
    const int machine_arg = this->argSearch(&cmd_args, "--machine");
    const String transaction_id =
        machine_arg >= 0 && machine_arg + 1 < cmd_args.size()
            ? cmd_args.get(machine_arg + 1) : "";
    if (machine_arg < 0) {
      Serial.println(F("SD upload requires --machine <transaction-id>"));
      return;
    }
    if (!validTransactionId(transaction_id)) {
      machineResult(transaction_id, SD_PUT_CMD, "error",
                    "INVALID_TRANSACTION");
      return;
    }

    const int path_arg = this->argSearch(&cmd_args, "--path-hex");
    const int bytes_arg = this->argSearch(&cmd_args, "--bytes");
    const int digest_arg = this->argSearch(&cmd_args, "--sha256");
    if (path_arg < 0 || path_arg + 1 >= cmd_args.size()) {
      machineResult(transaction_id, SD_PUT_CMD, "error", "INVALID_PATH");
      return;
    }
    if (bytes_arg < 0 || bytes_arg + 1 >= cmd_args.size()) {
      machineResult(transaction_id, SD_PUT_CMD, "error", "INVALID_SIZE");
      return;
    }
    if (digest_arg < 0 || digest_arg + 1 >= cmd_args.size()) {
      machineResult(transaction_id, SD_PUT_CMD, "error", "INVALID_DIGEST");
      return;
    }

    #ifdef HAS_SD
      if (!sd_obj.supported) {
        machineSdSummary(transaction_id, SD_PUT_CMD, "error",
                         "SD_NOT_READY");
        return;
      }
      if (wifi_scan_obj.scanning()) {
        machineSdSummary(transaction_id, SD_PUT_CMD, "error",
                         "DEVICE_BUSY");
        return;
      }

      char decoded_path[SD_TRANSFER_MAX_PATH_BYTES] = {};
      if (!decodeSdPathHex(cmd_args.get(path_arg + 1).c_str(), decoded_path,
                           sizeof(decoded_path)) ||
          !isEvilPortalHtmlUploadPath(decoded_path)) {
        machineSdSummary(transaction_id, SD_PUT_CMD, "error",
                         "INVALID_UPLOAD_PATH");
        return;
      }

      errno = 0;
      char* size_end = nullptr;
      const String size_value = cmd_args.get(bytes_arg + 1);
      const unsigned long parsed_size =
          strtoul(size_value.c_str(), &size_end, 10);
      if (errno == ERANGE || size_end == size_value.c_str() ||
          *size_end != '\0' || parsed_size == 0 ||
          parsed_size >= MAX_HTML_SIZE) {
        machineSdSummary(transaction_id, SD_PUT_CMD, "error",
                         "INVALID_SIZE");
        return;
      }
      const size_t upload_size = static_cast<size_t>(parsed_size);

      String expected_digest = cmd_args.get(digest_arg + 1);
      if (!isSha256Hex(expected_digest.c_str())) {
        machineSdSummary(transaction_id, SD_PUT_CMD, "error",
                         "INVALID_DIGEST");
        return;
      }
      expected_digest.toLowerCase();

      const String destination(decoded_path);
      const bool overwrite = this->argSearch(&cmd_args, "--overwrite") >= 0;
      if (SD.exists(destination) && !overwrite) {
        machineSdPutResult(transaction_id, "error", "FILE_EXISTS",
                           &destination, 0);
        return;
      }
      if (!sd_obj.ensureStorageLayout()) {
        machineSdPutResult(transaction_id, "error", "DIRECTORY_FAILED",
                           &destination, 0);
        return;
      }

      const String staging = String(marauder::storage::EVIL_PORTAL_HTML_DIR) +
                             "/.upload-" + transaction_id + ".part";
      const String previous = String(marauder::storage::EVIL_PORTAL_HTML_DIR) +
                              "/.upload-" + transaction_id + ".bak";
      if ((SD.exists(staging) && !SD.remove(staging)) ||
          (SD.exists(previous) && !SD.remove(previous))) {
        machineSdPutResult(transaction_id, "error", "TEMP_CLEANUP_FAILED",
                           &destination, 0);
        return;
      }

      File output = SD.open(staging, FILE_WRITE);
      if (!output || output.isDirectory()) {
        if (output)
          output.close();
        machineSdPutResult(transaction_id, "error", "FILE_OPEN_FAILED",
                           &destination, 0);
        return;
      }

      mbedtls_sha256_context sha_context;
      mbedtls_sha256_init(&sha_context);
      if (mbedtls_sha256_starts(&sha_context, 0) != 0) {
        output.close();
        SD.remove(staging);
        mbedtls_sha256_free(&sha_context);
        machineSdPutResult(transaction_id, "error", "HASH_INIT_FAILED",
                           &destination, 0);
        return;
      }

      this->showSdSessionStatus("UPLOADING", destination, 0);
      machineSdPutResult(transaction_id, "ready", "OK", &destination,
                         upload_size);

      constexpr uint32_t upload_idle_timeout_ms = 10000;
      uint8_t transfer_buffer[1024];
      size_t transferred = 0;
      int last_display_progress = -1;
      uint32_t last_data_ms = millis();
      const char* transfer_error = nullptr;
      while (transferred < upload_size) {
        const int available = Serial.available();
        if (available <= 0) {
          if (millis() - last_data_ms >= upload_idle_timeout_ms) {
            transfer_error = "TRANSFER_TIMEOUT";
            break;
          }
          delay(1);
          continue;
        }

        const size_t remaining = upload_size - transferred;
        size_t requested = static_cast<size_t>(available);
        if (requested > sizeof(transfer_buffer))
          requested = sizeof(transfer_buffer);
        if (requested > remaining)
          requested = remaining;
        const size_t read_count = Serial.readBytes(transfer_buffer, requested);
        if (read_count == 0)
          continue;
        last_data_ms = millis();
        transferred += read_count;
        const int display_progress = static_cast<int>(
            static_cast<uint64_t>(transferred) * 100ULL / upload_size);
        if (display_progress >= last_display_progress + 5 ||
            display_progress == 100) {
          last_display_progress = display_progress;
          this->showSdSessionStatus("UPLOADING", destination,
                                    display_progress);
        }

        // Once a storage or hash error occurs, continue consuming the exact
        // payload length so raw HTML bytes never leak into the command parser.
        if (transfer_error != nullptr)
          continue;
        if (mbedtls_sha256_update(&sha_context, transfer_buffer,
                                  read_count) != 0) {
          transfer_error = "HASH_UPDATE_FAILED";
          continue;
        }
        if (output.write(transfer_buffer, read_count) != read_count)
          transfer_error = "FILE_WRITE_FAILED";
        delay(0);
      }

      if (transfer_error == nullptr)
        output.flush();
      output.close();

      uint8_t digest[32] = {};
      if (transfer_error == nullptr &&
          mbedtls_sha256_finish(&sha_context, digest) != 0)
        transfer_error = "HASH_FINISH_FAILED";
      mbedtls_sha256_free(&sha_context);

      char digest_hex[65] = {};
      if (transfer_error == nullptr) {
        static constexpr char hex[] = "0123456789abcdef";
        for (size_t index = 0; index < sizeof(digest); ++index) {
          digest_hex[index * 2] = hex[digest[index] >> 4];
          digest_hex[index * 2 + 1] = hex[digest[index] & 0x0f];
        }
        if (expected_digest != digest_hex)
          transfer_error = "HASH_MISMATCH";
      }

      if (transfer_error != nullptr) {
        SD.remove(staging);
        machineSdPutResult(transaction_id, "error", transfer_error,
                           &destination, transferred);
        return;
      }

      const bool replacing = SD.exists(destination);
      if (replacing && !SD.rename(destination, previous)) {
        SD.remove(staging);
        machineSdPutResult(transaction_id, "error", "BACKUP_FAILED",
                           &destination, transferred);
        return;
      }
      if (!SD.rename(staging, destination)) {
        if (replacing)
          SD.rename(previous, destination);
        SD.remove(staging);
        machineSdPutResult(transaction_id, "error", "COMMIT_FAILED",
                           &destination, transferred);
        return;
      }
      if (replacing && SD.exists(previous) && !SD.remove(previous))
        Serial.println(F("Could not remove completed SD upload rollback file"));

      evil_portal_obj.refreshHtmlFiles();
      machineSdPutResult(transaction_id, "success", "OK", &destination,
                         transferred, digest_hex);
    #else
      machineResult(transaction_id, SD_PUT_CMD, "error",
                    "SD_NOT_SUPPORTED");
    #endif
  }

  else if (cmd_args.get(0) == BACKUP_SPIFFS_CMD ||
           cmd_args.get(0) == BACKUP_STATUS_CMD ||
           cmd_args.get(0) == RESTORE_SPIFFS_CMD) {
    uint8_t operation = cmd_args.get(0) == BACKUP_SPIFFS_CMD ? 0 :
                        cmd_args.get(0) == BACKUP_STATUS_CMD ? 1 : 2;
    const char* command = operation == 0 ? BACKUP_SPIFFS_CMD :
                          operation == 1 ? BACKUP_STATUS_CMD : RESTORE_SPIFFS_CMD;
    int machine_arg = this->argSearch(&cmd_args, "--machine");
    String transaction_id = machine_arg >= 0 && machine_arg + 1 < cmd_args.size()
      ? cmd_args.get(machine_arg + 1) : "";
    bool machine = machine_arg >= 0;
    if (machine && !validTransactionId(transaction_id)) {
      machineResult(transaction_id, command, "error", "INVALID_TRANSACTION");
      return;
    }

    #ifdef HAS_SD
      size_t files = 0;
      size_t bytes = 0;
      uint8_t error = 0;
      if (machine && operation != 1)
        machineResult(transaction_id, command, "started", "OK");
      else if (!machine && operation != 1)
        Serial.printf("SPIFFS %s started\n", operation == 0 ? "backup" : "restore");

      bool success = sd_obj.migrateSPIFFS(operation, files, bytes, error);
      if (machine) {
        if (success)
          machineResult(transaction_id, command, "success", "OK", files,
                        bytes, operation == 2);
        else {
          const char* fallback = operation == 0 ? "BACKUP_FAILED" :
                                 operation == 1 ? "BACKUP_INSPECTION_FAILED" :
                                                  "RESTORE_FAILED";
          machineResult(transaction_id, command, "error",
                        storageErrorCode(error, fallback));
        }
      }
      else if (success) {
        const char* action = operation == 0 ? "backup complete" :
                             operation == 1 ? "backup status" : "restore complete";
        Serial.printf("SPIFFS %s: %u files, %u bytes%s\n", action,
                      static_cast<unsigned int>(files),
                      static_cast<unsigned int>(bytes),
                      operation == 2 ? "; rebooting" : "");
      }
      else {
        const char* fallback = operation == 0 ? "BACKUP_FAILED" :
                               operation == 1 ? "BACKUP_INSPECTION_FAILED" :
                                                "RESTORE_FAILED";
        Serial.printf("SPIFFS %s failed: %s\n",
                      operation == 0 ? "backup" :
                      operation == 1 ? "backup status" : "restore",
                      storageErrorCode(error, fallback));
      }

      if (success && operation == 2) {
        delay(1000);
        ESP.restart();
      }
    #else
      if (machine)
        machineResult(transaction_id, command, "error", "SD_NOT_SUPPORTED");
      else
        Serial.println(F("SD Card NOT Supported"));
    #endif
  }

  // Channel command
  else if (cmd_args.get(0) == CH_CMD) {
    // Search for channel set arg
    int ch_set = this->argSearch(&cmd_args, "-s");

    if (ch_set != -1) {
      String channel_value;
      if (!this->argumentValue(&cmd_args, ch_set, "-s", channel_value))
        return;
      int channel = 0;
      if (!this->integerValue(channel_value, channel, "channel"))
        return;
      if (channel < 1 || channel > 196) {
        Serial.println(F("Channel is outside the supported range"));
        return;
      }
      wifi_scan_obj.set_channel = channel;
      wifi_scan_obj.changeChannel();
      Serial.println(wifi_scan_obj.set_channel);
    }
    Serial.println(wifi_scan_obj.set_channel);
  }
  // Clear APs
  else if (cmd_args.get(0) == CLEARAP_CMD) {
    int ap_sw = this->argSearch(&cmd_args, "-a"); // APs
    int ss_sw = this->argSearch(&cmd_args, "-s"); // SSIDs
    int cl_sw = this->argSearch(&cmd_args, "-c"); // Stations

    if (ap_sw != -1) {
      #ifdef HAS_SCREEN
        menu_function_obj.changeMenu(&menu_function_obj.clearAPsMenu);
      #endif
      wifi_scan_obj.RunClearAPs();
    }

    if (ss_sw != -1) {
      #ifdef HAS_SCREEN
        menu_function_obj.changeMenu(&menu_function_obj.clearSSIDsMenu);
      #endif
      wifi_scan_obj.RunClearSSIDs();
    }

    if (cl_sw != -1) {
      #ifdef HAS_SCREEN
        menu_function_obj.changeMenu(&menu_function_obj.clearAPsMenu);
      #endif
      wifi_scan_obj.RunClearStations();
    }
  }

  else if (cmd_args.get(0) == UPLOAD_CMD) {
    #ifdef HAS_DIRECT_UPLOAD
      int dest_sw = this->argSearch(&cmd_args, "-d");
      String upload_dest_arg;
      if (!this->argumentValue(&cmd_args, dest_sw, "-d", upload_dest_arg))
        return;
      int upload_dest = -1;

      if (upload_dest_arg == "wdg")
        upload_dest = WDG_UPLOAD;
      else if (upload_dest_arg == "wigle")
        upload_dest = WIGLE_UPLOAD;
      else if (upload_dest_arg == "both")
        upload_dest = BOTH_UPLOAD;

      if (upload_dest > -1) {
        String ssid = settings_obj.loadSetting<String>("ClientSSID");
        String pw = settings_obj.loadSetting<String>("ClientPW");

        Serial.println("Connecting to " + ssid);

        if (!wifi_scan_obj.joinWiFi(ssid, pw, false)) {
          Serial.println("Failed to connected to " + ssid);
          return;
        }
        delay(1000);
        LinkedList<String> upload_files;
        sd_obj.listDirToLinkedList(&upload_files,
                                   marauder::storage::WARDRIVE_DIR);
        sd_obj.listDirToLinkedList(&upload_files, "/");
        for (int i = 0; i < upload_files.size(); i++) {
          if (marauder::storage::isWardriveUploadCandidate(
                  upload_files.get(i))) {
            Serial.println("Uploading " + upload_files.get(i) + "...");
            if (wifi_scan_obj.uploadFile(
                    marauder::storage::withLeadingSlash(upload_files.get(i)),
                    true, upload_dest)) {
              Serial.println("Upload OK");
            } else {
              Serial.println("Upload failed");
            }
          }
        }
        WiFi.disconnect(true);
        delay(100);
        wifi_scan_obj.StartScan(WIFI_SCAN_OFF, TFT_RED);

        Serial.println("Upload complete");
      }
    #else
      Serial.println("Direct upload not supported");
    #endif
  }

  else if (cmd_args.get(0) == SETTINGS_CMD) {
    int ss_sw = this->argSearch(&cmd_args, "-s"); // Set setting
    int re_sw = this->argSearch(&cmd_args, "-r"); // Reset setting
    int en_sw = this->argSearch(&cmd_args, "enable"); // enable setting
    int da_sw = this->argSearch(&cmd_args, "disable"); // disable setting

    if (re_sw != -1) {
      settings_obj.createDefaultSettings(SPIFFS);
      return;
    }

    if (ss_sw == -1) {
      settings_obj.printJsonSettings(settings_obj.getSettingsString());
    }
    else {
      bool result = false;
      String setting_name;
      if (!this->argumentValue(&cmd_args, ss_sw, "-s", setting_name))
        return;
      if (en_sw != -1)
        result = settings_obj.saveSetting<bool>(setting_name.c_str(), true);
      else if (da_sw != -1)
        result = settings_obj.saveSetting<bool>(setting_name.c_str(), false);
      else
        return;

      if (!result) {
        Serial.print(F("Could not successfully update setting \""));
        Serial.println(setting_name + "\"");
        return;
      }
    }
  }

  else if (cmd_args.get(0) == REBOOT_CMD)
    ESP.restart();

  //// WiFi/Bluetooth Scan/Attack commands
  if (!wifi_scan_obj.scanning()) {
    // Dump pcap/log to serial too, valid for all scan/attack commands
    wifi_scan_obj.save_serial = this->argSearch(&cmd_args, "-serial") != -1;

    // Signal strength scan
    if (cmd_args.get(0) == SIGSTREN_CMD) {
      int bt_sw = this->argSearch(&cmd_args, "-b");
      int wf_sw = this->argSearch(&cmd_args, "-w");
      if (wf_sw > -1) {
        String target_value;
        if (!this->argumentValue(&cmd_args, wf_sw, "-w", target_value))
          return;
        int targ_index = -1;
        if (!this->integerValue(target_value, targ_index, "AP index"))
          return;
        if (this->inRange(access_points->size(), targ_index)) {
          for (int i = 0; i < access_points->size(); i++) {
            AccessPoint access_point = access_points->get(i);
            access_point.selected = (i == targ_index);
            access_points->set(i, access_point);
          }
          this->startScanFromCLI(WIFI_SCAN_SIG_STREN, TFT_GREEN, "Fox Hunt");
        }
      }
      else if (bt_sw > -1) {
        String target_value;
        if (!this->argumentValue(&cmd_args, bt_sw, "-b", target_value))
          return;
        int targ_index = -1;
        if (!this->integerValue(target_value, targ_index, "BLE index"))
          return;
        if (this->inRange(ble_devices->size(), targ_index)) {
          for (int i = 0; i < ble_devices->size(); i++) {
            BleDevice ble_device = ble_devices->get(i);
            ble_device.selected = (i == targ_index);
            ble_devices->set(i, ble_device);
          }
          this->startScanFromCLI(BT_SCAN_FOX_HUNT, TFT_CYAN, "Bluetooth Fox Hunt");
        }
      }
    }
    // Packet count
    else if (cmd_args.get(0) == PACKET_COUNT_CMD) {
      this->startScanFromCLI(WIFI_SCAN_PACKET_RATE, TFT_ORANGE, "Packet Count Scan");
    }
    // Wardrive
    else if (cmd_args.get(0) == WARDRIVE_CMD) {
      #ifdef HAS_GPS
        if (gps_obj.getGpsModuleStatus()) {
          //int sta_sw = this->argSearch(&cmd_args, "-s");
          this->startScanFromCLI(WIFI_SCAN_WAR_DRIVE, TFT_GREEN, "Wardrive");
        }
      #else
        Serial.println(F("GPS not supported"));
      #endif
    }
    // Karma
    else if (cmd_args.get(0) == KARMA_CMD) {
      int pr_sw = this->argSearch(&cmd_args, "-p");

      if (pr_sw == -1) {
        return;
      }

      String probe_value;
      if (!this->argumentValue(&cmd_args, pr_sw, "-p", probe_value))
        return;
      int pr_index = -1;
      if (!this->integerValue(probe_value, pr_index, "probe SSID index"))
        return;

      if ((pr_index < 0) || (pr_index > probe_req_ssids->size() - 1)) {
        return;
      }

      if (evil_portal_obj.setAP(probe_req_ssids->get(pr_index).essid)) {
        this->startScanFromCLI(WIFI_SCAN_EVIL_PORTAL, TFT_ORANGE, "Karma Attack");
        /*Serial.println(STOPSCAN_CMD);
        #ifdef HAS_SCREEN
          display_obj.clearScreen();
          menu_function_obj.drawStatusBar();
        #endif
        wifi_scan_obj.StartScan(WIFI_SCAN_EVIL_PORTAL, TFT_ORANGE);*/
        wifi_scan_obj.setMac();
      }
      else {
        Serial.println(F("Unable to set AP ESSID"));
        return;
      }

    }
    // AP Scan
    else if (cmd_args.get(0) == EVIL_PORTAL_CMD) {
      int cmd_sw = this->argSearch(&cmd_args, "-c");
      int html_sw = this->argSearch(&cmd_args, "-w");

      if (cmd_sw != -1) {
        String et_command;
        if (!this->argumentValue(&cmd_args, cmd_sw, "-c", et_command))
          return;
        if (et_command == "start") {
          Serial.print(F("Starting Evil Portal. Stop with "));
          Serial.println(STOPSCAN_CMD);
          #ifdef HAS_SCREEN
            display_obj.clearScreen();
            menu_function_obj.drawStatusBar();
          #endif
          if (html_sw != -1) {
            String target_html_name;
            if (!this->argumentValue(&cmd_args, html_sw, "-w",
                                     target_html_name))
              return;
            evil_portal_obj.target_html_name = target_html_name;
            evil_portal_obj.using_serial_html = false;
            Serial.print(F("Set html file as "));
            Serial.println(evil_portal_obj.target_html_name);
          }
          //else {
          //  evil_portal_obj.target_html_name = "index.html";
          //}
          wifi_scan_obj.StartScan(WIFI_SCAN_EVIL_PORTAL, TFT_MAGENTA);
        }
        else if (et_command == "reset") {
          
        }
        else if (et_command == "ack") {
          
        }
        else if (et_command == "sethtml") {
          if (cmd_sw + 2 >= cmd_args.size()) {
            Serial.println(F("Missing HTML filename after sethtml"));
            return;
          }
          String target_html_name = cmd_args.get(cmd_sw + 2);
          evil_portal_obj.target_html_name = target_html_name;
          evil_portal_obj.using_serial_html = false;
          Serial.print(F("Set html file as "));
          Serial.println(evil_portal_obj.target_html_name);
        }
        else if (et_command == "sethtmlstr") {
          evil_portal_obj.setHtmlFromSerial();
        }
        else if (et_command == "setap") {
          if (cmd_sw + 2 >= cmd_args.size()) {
            Serial.println(F("Missing AP index after setap"));
            return;
          }
          int target_ap_index = -1;
          if (!this->integerValue(cmd_args.get(cmd_sw + 2), target_ap_index,
                                  "AP index"))
            return;
          if ((target_ap_index >= 0) && (target_ap_index < access_points->size())) {
            for (int index = 0; index < access_points->size(); index++) {
              AccessPoint access_point = access_points->get(index);
              access_point.selected = index == target_ap_index;
              access_points->set(index, access_point);
            }
            AccessPoint new_ap = access_points->get(target_ap_index);
            if (evil_portal_obj.setAP(new_ap.essid))
              evil_portal_obj.setTargetAP(target_ap_index, new_ap.channel);
          }
        }
      }
    }
    else if (cmd_args.get(0) == SCAN_ALL_CMD) {
      Serial.print(F("Scanning for APs and Stations. Stop with "));
      Serial.println(STOPSCAN_CMD);
      wifi_scan_obj.StartScan(WIFI_SCAN_AP_STA, TFT_MAGENTA);
    }
    // Raw sniff
    else if (cmd_args.get(0) == SNIFF_RAW_CMD)
      this->startScanFromCLI(WIFI_SCAN_RAW_CAPTURE, TFT_WHITE, "Raw sniff");
    // Beacon sniff
    else if (cmd_args.get(0) == SNIFF_BEACON_CMD) {
      this->startScanFromCLI(WIFI_SCAN_AP, TFT_MAGENTA, "Beacon sniff");
    }
    // SAE sniff
    else if (cmd_args.get(0) == SNIFF_SAE_CMD) {
      this->startScanFromCLI(WIFI_SCAN_SAE_COMMIT, TFT_MAGENTA, "Commit sniff");
    }
    // Probe sniff
    else if (cmd_args.get(0) == SNIFF_PROBE_CMD) {
      this->startScanFromCLI(WIFI_SCAN_PROBE, TFT_MAGENTA, "Probe sniff");
    }
    // Deauth sniff
    else if (cmd_args.get(0) == SNIFF_DEAUTH_CMD) {
      this->startScanFromCLI(WIFI_SCAN_DEAUTH, TFT_RED, "Deauth sniff");
    }
    // Pwn sniff
    else if (cmd_args.get(0) == SNIFF_PWN_CMD) {
      this->startScanFromCLI(WIFI_SCAN_PWN, TFT_MAGENTA, "Pwnagotchi sniff");
    }
    // PineScan sniff
    else if (cmd_args.get(0) == SNIFF_PINESCAN_CMD) {
      this->startScanFromCLI(WIFI_SCAN_PINESCAN, TFT_MAGENTA, "Pinescan sniff");
    }
    // MultiSSID sniff
    else if (cmd_args.get(0) == SNIFF_MULTISSID_CMD) {
      this->startScanFromCLI(WIFI_SCAN_MULTISSID, TFT_MAGENTA, "MultiSSID sniff");
    }
    // PMKID sniff
    else if (cmd_args.get(0) == SNIFF_PMKID_CMD) {
      int ch_sw = this->argSearch(&cmd_args, "-c");
      int d_sw = this->argSearch(&cmd_args, "-d"); // Deauth for pmkid
      int l_sw = this->argSearch(&cmd_args, "-l"); // Only run on list

      if (l_sw != -1) {
        if (!wifi_scan_obj.filterActive()) {
          Serial.println("You don't have any targets selected. Use " + (String)SEL_CMD);
          return;
        }
      }
      
      if (ch_sw != -1) {
        String channel_value;
        if (!this->argumentValue(&cmd_args, ch_sw, "-c", channel_value))
          return;
        int channel = 0;
        if (!this->integerValue(channel_value, channel, "channel"))
          return;
        if (channel < 1 || channel > 196) {
          Serial.println(F("Channel is outside the supported range"));
          return;
        }
        wifi_scan_obj.set_channel = channel;
        wifi_scan_obj.changeChannel();
        Serial.println("Set channel: " + (String)wifi_scan_obj.set_channel);
        
      }

      if (d_sw == -1) {
        Serial.println("Starting PMKID sniff on channel " + (String)wifi_scan_obj.set_channel + ". Stop with " + (String)STOPSCAN_CMD);
        wifi_scan_obj.StartScan(WIFI_SCAN_EAPOL, TFT_VIOLET);
      }
      else if ((d_sw != -1) && (l_sw != -1)) {
        Serial.println("Starting TARGETED PMKID sniff with deauthentication on channel " + (String)wifi_scan_obj.set_channel + ". Stop with " + (String)STOPSCAN_CMD);
        wifi_scan_obj.StartScan(WIFI_SCAN_ACTIVE_LIST_EAPOL, TFT_VIOLET);
      }
      else {
        Serial.println("Starting PMKID sniff with deauthentication on channel " + (String)wifi_scan_obj.set_channel + ". Stop with " + (String)STOPSCAN_CMD);
        wifi_scan_obj.StartScan(WIFI_SCAN_ACTIVE_EAPOL, TFT_VIOLET);
      }
    }    
    // MAC Tracking
    else if (cmd_args.get(0) == MAC_TRACK_CMD) {
      this->startScanFromCLI(WIFI_SCAN_DETECT_FOLLOW, TFT_MAGENTA, "MAC Tracker");
      /*Serial.println(STOPSCAN_CMD);
      #ifdef HAS_SCREEN
        display_obj.clearScreen();
        menu_function_obj.drawStatusBar();
      #endif
      wifi_scan_obj.StartScan(WIFI_SCAN_DETECT_FOLLOW, TFT_MAGENTA);*/
    }


  //// MAC Address commands    (Added by H4W9_4)
	// Generate random MAC for AP
    if (cmd_args.get(0) == MAC_CMD_A) {
      #ifdef HAS_SCREEN
        display_obj.clearScreen();
        menu_function_obj.drawStatusBar();
      #endif
      wifi_scan_obj.RunGenerateRandomMac(true);
    }

	  // Generate random MAC for STA
	  else if (cmd_args.get(0) == MAC_CMD_B) {
      //Serial.println("Setting STA MAC: " + macToString(this->sta_mac));
      #ifdef HAS_SCREEN
        display_obj.clearScreen();
        menu_function_obj.drawStatusBar();
      #endif
      wifi_scan_obj.RunGenerateRandomMac(false);
    }

	  // Clone MAC for AP
	  else if (cmd_args.get(0) == MAC_CMD_C) {
      int ap_sw = this->argSearch(&cmd_args, "-a"); // APs
      
      if (ap_sw == -1) {
        return;
      }

      String ap_value;
      if (!this->argumentValue(&cmd_args, ap_sw, "-a", ap_value))
        return;
      int ap_index = -1;
      if (!this->integerValue(ap_value, ap_index, "AP index"))
        return;

      if ((ap_index < 0) || (ap_index > access_points->size() - 1)) {
        return;
      }
      
      if (ap_sw != -1) {
        #ifdef HAS_SCREEN
          display_obj.clearScreen();
          menu_function_obj.drawStatusBar();
        #endif
        wifi_scan_obj.RunSetMac(access_points->get(ap_index).bssid, true);
      }
    }

    // Clone MAC for STA
	  else if (cmd_args.get(0) == MAC_CMD_D) {
      int cl_sw = this->argSearch(&cmd_args, "-s"); // Stations
            
      if (cl_sw == -1)
        return;

      String station_value;
      if (!this->argumentValue(&cmd_args, cl_sw, "-s", station_value))
        return;
      int sta_index = -1;
      if (!this->integerValue(station_value, sta_index, "station index"))
        return;

      if ((sta_index < 0) || (sta_index > stations->size() - 1)) {
        return;
      }

      if (cl_sw != -1) {
        #ifdef HAS_SCREEN
          display_obj.clearScreen();
          menu_function_obj.drawStatusBar();
        #endif
        wifi_scan_obj.RunSetMac(stations->get(sta_index).mac, false);
      }
    }
    //// End MAC Address commands    (Added by H4W9_4)

    
    //// WiFi attack commands
    // attack
    if (cmd_args.get(0) == ATTACK_CMD) {
      int attack_type_switch = this->argSearch(&cmd_args, "-t"); // Required
      int list_beacon_sw = this->argSearch(&cmd_args, "-l");
      int rand_beacon_sw = this->argSearch(&cmd_args, "-r");
      int ap_beacon_sw = this->argSearch(&cmd_args, "-a");
      int src_addr_sw = this->argSearch(&cmd_args, "-s");
      int dst_addr_sw = this->argSearch(&cmd_args, "-d");
      int targ_sw = this->argSearch(&cmd_args, "-c");
  
      if (attack_type_switch == -1)
        return;
      else {
        String attack_type;
        if (!this->argumentValue(&cmd_args, attack_type_switch, "-t",
                                 attack_type))
          return;
  
        // Branch on attack type
        if (attack_type == ATTACK_TYPE_DEAUTH) {
          // Default to broadcast
          if ((dst_addr_sw == -1) && (targ_sw == -1)) {
            Serial.println(F("Sending to broadcast..."));
            wifi_scan_obj.dst_mac = "ff:ff:ff:ff:ff:ff";
          }
          // Dest addr specified
          else if (dst_addr_sw != -1) {
            String destination_mac;
            if (!this->argumentValue(&cmd_args, dst_addr_sw, "-d",
                                     destination_mac))
              return;
            wifi_scan_obj.dst_mac = destination_mac;
            Serial.println("Sending to " + wifi_scan_obj.dst_mac + "...");
          }
          // Station list specified
          else if (targ_sw != -1)
            Serial.println(F("Sending to Station list"));

          // Source addr not specified
          if (src_addr_sw == -1) {
            if (!wifi_scan_obj.filterActive()) {
              Serial.println("You don't have any targets selected. Use " + (String)SEL_CMD);
              return;
            }
            #ifdef HAS_SCREEN
              display_obj.clearScreen();
              menu_function_obj.drawStatusBar();
            #endif
            Serial.println("Starting Deauthentication attack. Stop with " + (String)STOPSCAN_CMD);
            // Station list not specified
            if (targ_sw == -1)
              wifi_scan_obj.StartScan(WIFI_ATTACK_DEAUTH, TFT_RED);
            // Station list specified
            else
              wifi_scan_obj.StartScan(WIFI_ATTACK_DEAUTH_TARGETED, TFT_ORANGE);
          }
          // Source addr specified
          else {
            String src_mac_str;
            if (!this->argumentValue(&cmd_args, src_addr_sw, "-s",
                                     src_mac_str))
              return;
            if (sscanf(src_mac_str.c_str(),
                       "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx",
                       &wifi_scan_obj.src_mac[0], &wifi_scan_obj.src_mac[1],
                       &wifi_scan_obj.src_mac[2], &wifi_scan_obj.src_mac[3],
                       &wifi_scan_obj.src_mac[4], &wifi_scan_obj.src_mac[5]) != 6) {
              Serial.println(F("Source MAC must be XX:XX:XX:XX:XX:XX"));
              return;
            }

            #ifdef HAS_SCREEN
              display_obj.clearScreen();
              menu_function_obj.drawStatusBar();
            #endif
            Serial.println("Starting Manual Deauthentication attack. Stop with " + (String)STOPSCAN_CMD);
            wifi_scan_obj.StartScan(WIFI_ATTACK_DEAUTH_MANUAL, TFT_RED);            
          }
        }
        else if (attack_type == ATTACK_TYPE_BM) {
          // Attack all
          if (targ_sw == -1) {
            this->startScanFromCLI(WIFI_ATTACK_BAD_MSG, TFT_RED, "Bad Msg attack against all stations");
            /*Serial.prinln((String)STOPSCAN_CMD);
            #ifdef HAS_SCREEN
              display_obj.clearScreen();
              menu_function_obj.drawStatusBar();
            #endif
            wifi_scan_obj.StartScan(WIFI_ATTACK_BAD_MSG, TFT_RED);*/
          }
          // Target clients
          else {
            this->startScanFromCLI(WIFI_ATTACK_BAD_MSG_TARGETED, TFT_YELLOW, "targeted Bad Msg attack");
            /*Serial.println(STOPSCAN_CMD);
            #ifdef HAS_SCREEN
              display_obj.clearScreen();
              menu_function_obj.drawStatusBar();
            #endif
            wifi_scan_obj.StartScan(WIFI_ATTACK_BAD_MSG_TARGETED, TFT_YELLOW);*/
          }
        }
        else if (attack_type == ATTACK_TYPE_S) {
          // Attack all
          if (targ_sw == -1) {
            this->startScanFromCLI(WIFI_ATTACK_SLEEP, TFT_RED, "Sleep attack against all stations");
            /*Serial.println(STOPSCAN_CMD);
            #ifdef HAS_SCREEN
              display_obj.clearScreen();
              menu_function_obj.drawStatusBar();
            #endif
            wifi_scan_obj.StartScan(WIFI_ATTACK_SLEEP, TFT_RED);*/
          }
          // Target clients
          else {
            this->startScanFromCLI(WIFI_ATTACK_SLEEP_TARGETED, TFT_MAGENTA, "targeted Sleep attack");
            /*Serial.println(STOPSCAN_CMD);
            #ifdef HAS_SCREEN
              display_obj.clearScreen();
              menu_function_obj.drawStatusBar();
            #endif
            wifi_scan_obj.StartScan(WIFI_ATTACK_SLEEP_TARGETED, TFT_MAGENTA);*/
          }
        }
        else if (attack_type == ATTACK_TYPE_BEACON) {
          // spam by list
          if (list_beacon_sw != -1) {
            if (!this->hasSSIDs()) {
              Serial.println("You don't have any SSIDs in your list. Use " + (String)SSID_CMD);
              return;
            }
            #ifdef HAS_SCREEN
              display_obj.clearScreen();
              menu_function_obj.drawStatusBar();
            #endif
            Serial.println("Starting Beacon list spam. Stop with " + (String)STOPSCAN_CMD);
            wifi_scan_obj.StartScan(WIFI_ATTACK_BEACON_LIST, TFT_RED);
          }
          // spam with random
          else if (rand_beacon_sw != -1) {
            #ifdef HAS_SCREEN
              display_obj.clearScreen();
              menu_function_obj.drawStatusBar();
            #endif
            Serial.println("Starting random Beacon spam. Stop with " + (String)STOPSCAN_CMD);
            wifi_scan_obj.StartScan(WIFI_ATTACK_BEACON_SPAM, TFT_ORANGE);
          }
          // Spam from AP list
          else if (ap_beacon_sw != -1) {
            if (!wifi_scan_obj.filterActive()) {
              Serial.println("You don't have any targets selected. Use " + (String)SEL_CMD);
              return;
            }
            #ifdef HAS_SCREEN
              display_obj.clearScreen();
              menu_function_obj.drawStatusBar();
            #endif
            Serial.println("Starting Targeted AP Beacon spam. Stop with " + (String)STOPSCAN_CMD);
            wifi_scan_obj.StartScan(WIFI_ATTACK_AP_SPAM, TFT_MAGENTA);
          }
        }
        else if (attack_type == ATTACK_TYPE_PROBE) {
          if (!wifi_scan_obj.filterActive()) {
            Serial.println("You don't have any targets selected. Use " + (String)SEL_CMD);
            return;
          }
          this->startScanFromCLI(WIFI_ATTACK_AUTH, TFT_RED, "Probe spam");
        }
        else if (attack_type == ATTACK_TYPE_RR) {
          this->startScanFromCLI(WIFI_ATTACK_RICK_ROLL, TFT_YELLOW, "Rick Roll Beacon spam");
        }
        else if (attack_type == ATTACK_TYPE_FUNNY) {
          this->startScanFromCLI(WIFI_ATTACK_FUNNY_BEACON, TFT_CYAN, "Funny SSID Beacon spam");
        }
        else if (attack_type == ATTACK_TYPE_SAE) {
          this->startScanFromCLI(WIFI_ATTACK_SAE_COMMIT, TFT_CYAN, "SAE Commit spam");
        }
        else if (attack_type == ATTACK_TYPE_CSA) {
          this->startScanFromCLI(WIFI_ATTACK_CSA, TFT_CYAN, "Channel Switch Announcement attack");
        }
        else if (attack_type == ATTACK_TYPE_QUIET) {
          this->startScanFromCLI(WIFI_ATTACK_QUIET, TFT_CYAN, "Quite Time attack");
        }
        else {
          return;
        }
      }
    }

    //// Bluetooth scan/attack commands
    // Bluetooth scan
    if (cmd_args.get(0) == BT_SNIFF_CMD) {
      #ifdef HAS_BT
        int bt_type_sw = this->argSearch(&cmd_args, "-t");

        // Specifying type of bluetooth sniff
        if (bt_type_sw != -1) {
          String bt_type;
          if (!this->argumentValue(&cmd_args, bt_type_sw, "-t", bt_type))
            return;

          bt_type.toLowerCase();

          // Airtag sniff
          if (bt_type == "airtag") {
            this->startScanFromCLI(BT_SCAN_AIRTAG, TFT_WHITE, "Airtag sniff");
          }
          else if (bt_type == "flipper") {
            this->startScanFromCLI(BT_SCAN_FLIPPER, TFT_ORANGE, "Flipper sniff");
          }
          else if (bt_type == "flock") {
            this->startScanFromCLI(BT_SCAN_FLOCK, TFT_ORANGE, "Flock sniff");
          }
          else if (bt_type == "meta") {
            this->startScanFromCLI(BT_SCAN_RAYBAN, TFT_ORANGE, "Meta sniff");
          }
          else if (bt_type == "capture") {
            Serial.println("Starting BLE advertisement capture. Stop with " + (String)STOPSCAN_CMD);
            wifi_scan_obj.startBLEAdvertisementCapture();
          }
        }
        // General bluetooth sniff
        else {
          this->startScanFromCLI(BT_SCAN_ALL, TFT_GREEN, "Bluetooth scan");
        }
      #else
        Serial.println(F("Bluetooth not supported"));
      #endif
    }
    else if (cmd_args.get(0) == BT_SPOOFAT_CMD) {
      int at_sw = this->argSearch(&cmd_args, "-t");
      if (at_sw != -1) {
        #ifdef HAS_BT
          String target_value;
          if (!this->argumentValue(&cmd_args, at_sw, "-t", target_value))
            return;
          int target_mac = -1;
          if (!this->integerValue(target_value, target_mac, "AirTag index"))
            return;
          if (this->inRange(airtags->size(), target_mac)) {
            for (int i = 0; i < airtags->size(); i++) {
              AirTag at = airtags->get(i);
              if (i == target_mac)
                at.selected = true;
              else
                at.selected = false;
              airtags->set(i, at);
            }
            this->startScanFromCLI(BT_SPOOF_AIRTAG, TFT_WHITE, "Spoofing Airtag");
          }
          else {
            return;
          }
        #endif
      }
    }
    else if (cmd_args.get(0) == BT_FINDMY_CMD) {
      #ifdef HAS_NIMBLE_2
      int index_sw = this->argSearch(&cmd_args, "-t");

      if (index_sw != -1) {
        String target_value;
        if (!this->argumentValue(&cmd_args, index_sw, "-t", target_value))
          return;
        int targ_index = -1;
        if (!this->integerValue(target_value, targ_index, "AirTag index"))
          return;
        if (this->inRange(airtags->size(), targ_index)) {
          for (int x = 0; x < airtags->size(); x++) {
            AirTag new_atx = airtags->get(x);
            if (x != targ_index)
              new_atx.selected = false;
            else
              new_atx.selected = true;
            airtags->set(x, new_atx);
          }

          wifi_scan_obj.executeFindMySound(false);
        }
      }
      #endif
    }
    else if (cmd_args.get(0) == BT_SPAM_CMD) {
      int bt_type_sw = this->argSearch(&cmd_args, "-t");
      if (bt_type_sw != -1) {
        String bt_type;
        if (!this->argumentValue(&cmd_args, bt_type_sw, "-t", bt_type))
          return;

        #ifdef HAS_BT
          if (bt_type == "sourapple") {
            #ifdef HAS_BT
              this->startScanFromCLI(BT_ATTACK_SOUR_APPLE, TFT_GREEN, "Sour Apple attack");
            #endif
          }
          else if (bt_type == "applejuice") {
            #ifdef HAS_BT
              this->startScanFromCLI(BT_ATTACK_APPLE_JUICE, TFT_GREEN, "Apple Juice attack");
            #endif
          }
          else if (bt_type == "windows") {
            #ifdef HAS_BT
              this->startScanFromCLI(BT_ATTACK_SWIFTPAIR_SPAM, TFT_CYAN, "Swiftpair Spam attack");
            #endif
          }
          else if (bt_type == "samsung") {
            #ifdef HAS_BT
              this->startScanFromCLI(BT_ATTACK_SAMSUNG_SPAM, TFT_CYAN, "Samsung Spam attack");
            #endif
          }
          else if (bt_type == "google") {
            #ifdef HAS_BT
              this->startScanFromCLI(BT_ATTACK_GOOGLE_SPAM, TFT_CYAN, "Google Spam attack");
            #endif
          }
          else if (bt_type == "flipper") {
            #ifdef HAS_BT
              this->startScanFromCLI(BT_ATTACK_FLIPPER_SPAM, TFT_ORANGE, "Flipper Spam attack");
            #endif
          }
          else if (bt_type == "all") {
            #ifdef HAS_BT
              this->startScanFromCLI(BT_ATTACK_SPAM_ALL, TFT_MAGENTA, "BT Spam All attack");
            #endif
          }
        #else
          Serial.println(F("Bluetooth not supported"));
        #endif
      }
    }
    // Bluetooth CC Skimmer scan
    else if (cmd_args.get(0) == BT_SKIM_CMD) {
      #ifdef HAS_BT
        this->startScanFromCLI(BT_SCAN_SKIMMERS, TFT_MAGENTA, "Bluetooth CC Skimmer scan");
      #else
        Serial.println(F("Bluetooth not supported"));
      #endif
    }

    // Brightness command
    else if (cmd_args.get(0) == BRIGHTNESS_CMD) {
      #if !defined(HAS_MINI_SCREEN) || defined(MARAUDER_MINI_V3)
        int c_arg = this->argSearch(&cmd_args, "-c");
        int s_arg = this->argSearch(&cmd_args, "-s");
        if (c_arg != -1) {
          brightnessCycle();
        } else if (s_arg != -1) {
          String level_value;
          if (!this->argumentValue(&cmd_args, s_arg, "-s", level_value))
            return;
          int level = -1;
          if (!this->integerValue(level_value, level, "brightness"))
            return;
          if (level >= 0 && level < 10) {
            uint8_t lvl = static_cast<uint8_t>(level);
            brightnessSave(lvl);
            Serial.print(F("[Brightness] Set to level "));
            Serial.println(lvl);
          } else {
            Serial.println(F("Level must be 0-9"));
          }
        } else {
          Serial.print(F("[Brightness] Current level: "));
          Serial.println(getBrightnessLevel());
        }
      #endif
    }

    // Wardrive POI command
    else if (cmd_args.get(0) == WARDRIVEPOI_CMD) {
      if (wifi_scan_obj.currentScanMode == WIFI_SCAN_WAR_DRIVE ||
          wifi_scan_obj.currentScanMode == WIFI_SCAN_STATION_WAR_DRIVE) {
        if (cmd_args.size() > 1) {
          // Join remaining args as label
          String label = "";
          for (int i = 1; i < cmd_args.size(); i++) {
            if (i > 1) label += " ";
            label += cmd_args.get(i);
          }
          wifi_scan_obj.tagPOI(label.c_str());
        } else {
          wifi_scan_obj.tagPOI(nullptr);
        }
      } 
      //else {
      //  Serial.println(F("No active wardrive. Start wardrive first."));
      //}
    }

    // Update command
    if (cmd_args.get(0) == UPDATE_CMD) {
      int sd_sw = this->argSearch(&cmd_args, "-s"); // SD Update
      if (sd_sw != -1) {
        #ifdef HAS_SD
          if (!sd_obj.supported) {
            Serial.println(F("SD card is not connected."));
            return;
          }
          wifi_scan_obj.currentScanMode = OTA_UPDATE;
          sd_obj.runUpdate();
        #endif
      }
    }
  }

  if (wifi_scan_obj.wifi_connected) {
    // Ping Scan
    if (cmd_args.get(0) == PING_CMD) {
      this->startScanFromCLI(WIFI_PING_SCAN, TFT_GREEN, "Ping Scan");
    }

    #ifndef HAS_DUAL_BAND
      if (cmd_args.get(0) == ARP_SCAN_CMD) {
        this->startScanFromCLI(WIFI_ARP_SCAN, TFT_CYAN, "ARP Scan");
      }
    #endif

    // GPS POI
    if (cmd_args.get(0) == GPS_POI_CMD) {
      #ifdef HAS_GPS
        int start_sw = this->argSearch(&cmd_args, "-s");
        int mark_sw = this->argSearch(&cmd_args, "-m");
        int end_sw = this->argSearch(&cmd_args, "-e");

        if (start_sw != -1) {
          wifi_scan_obj.StartScan(GPS_POI, TFT_CYAN);
          wifi_scan_obj.currentScanMode = WIFI_SCAN_OFF;
          #ifdef HAS_SCREEN
            menu_function_obj.changeMenu(&menu_function_obj.gpsPOIMenu);
          #endif
        }
        else if (mark_sw != -1) {
          wifi_scan_obj.currentScanMode = GPS_POI;
          #ifdef HAS_SCREEN
            display_obj.tft.setCursor(0, TFT_HEIGHT / 2);
            display_obj.clearScreen();
          #endif
          if (wifi_scan_obj.RunGPSInfo(true, false, true)) {
            #ifdef HAS_SCREEN
              display_obj.showCenterText("POI Logged", TFT_HEIGHT / 2);
            #endif
          }
          else {
            #ifdef HAS_SCREEN
              display_obj.showCenterText("POI Log Failed", TFT_HEIGHT / 2);
            #endif
          }
          wifi_scan_obj.currentScanMode = WIFI_SCAN_OFF;
          delay(2000);
          //wifi_scan_obj.StartScan(WIFI_SCAN_OFF);
          #ifdef HAS_SCREEN
            menu_function_obj.changeMenu(&menu_function_obj.gpsPOIMenu);
          #endif
        }
        else if (end_sw != -1) {
          wifi_scan_obj.currentScanMode = GPS_POI;
          wifi_scan_obj.StartScan(WIFI_SCAN_OFF);
          #ifdef HAS_SCREEN
            menu_function_obj.changeMenu(menu_function_obj.gpsPOIMenu.parentMenu);
          #endif
        }
      #endif
    }

    // Port Scan
    if (cmd_args.get(0) == PORT_SCAN_CMD) {
      int all_sw = this->argSearch(&cmd_args, "-a");
      int ip_sw = this->argSearch(&cmd_args, "-t");
      int port_sw = this->argSearch(&cmd_args, "-s");

      // Check they specified ip index
      if (ip_sw != -1) {
        String ip_value;
        if (!this->argumentValue(&cmd_args, ip_sw, "-t", ip_value))
          return;
        int ip_index = -1;
        if (!this->integerValue(ip_value, ip_index, "IP index"))
          return;

        // Check provided index is in list
        if (this->inRange(ipList->size(), ip_index)) {

          // Full port scan
          if (all_sw != -1) {
            wifi_scan_obj.current_scan_ip = ipList->get(ip_index);
            String msg = "Selected: " + ipList->get(ip_index).toString();
            this->startScanFromCLI(WIFI_PORT_SCAN_ALL, TFT_BLUE, msg.c_str());
          }
        }
        else {
          return;
        }
      }
      else if (port_sw != -1) {
        String port_name;
        if (!this->argumentValue(&cmd_args, port_sw, "-s", port_name))
          return;
        port_name.toUpperCase();
        uint8_t target_mode = 0;
        if (port_name == "SSH")
          target_mode = WIFI_SCAN_SSH;
        else if (port_name == "TELNET")
          target_mode = WIFI_SCAN_TELNET;
        else if (port_name == "DNS")
          target_mode = WIFI_SCAN_DNS;
        else if (port_name == "HTTP")
          target_mode = WIFI_SCAN_HTTP;
        else if (port_name == "SMTP")
          target_mode = WIFI_SCAN_SMTP;
        else if (port_name == "HTTPS")
          target_mode = WIFI_SCAN_HTTPS;
        else if (port_name == "RDP")
          target_mode = WIFI_SCAN_RDP;

        if (target_mode != 0) {
          this->startScanFromCLI(target_mode, TFT_CYAN, "port scan");
        }
      }
    }
  }


  int count_selected = 0;
  //// WiFi aux commands
  // List access points
  if (cmd_args.get(0) == LIST_AP_CMD) {
    int ap_sw = this->argSearch(&cmd_args, "-a");
    int ss_sw = this->argSearch(&cmd_args, "-s");
    int cl_sw = this->argSearch(&cmd_args, "-c");
    int at_sw = this->argSearch(&cmd_args, "-t");
    int ip_sw = this->argSearch(&cmd_args, "-i");
    int pr_sw = this->argSearch(&cmd_args, "-p");
    int bt_sw = this->argSearch(&cmd_args, "-b");

    // List APs
    if (ap_sw != -1) {
      for (int i = 0; i < access_points->size(); i++) {
        AccessPoint access_point = access_points->get(i);
        if (access_point.selected) {
          Serial.println("[" + (String)i + "][CH:" + (String)access_point.channel + "] " + access_point.essid + " " + (String)access_point.rssi + " (selected)");
          count_selected += 1;
        } 
        else
          Serial.println("[" + (String)i + "][CH:" + (String)access_point.channel + "] " + access_point.essid + " " + (String)access_point.rssi);
      }
      this->showCounts(count_selected);
    }
    else if (bt_sw != -1) {
      for (int i = 0; i < ble_devices->size(); i++) {
        BleDevice ble_device = ble_devices->get(i);
        Serial.println("[" + (String)i + "][RSSI:" + (String)ble_device.rssi + "] " + ble_device.name);
        if (ble_device.advertisedServices.length())
          Serial.println("  Advertised services: " + ble_device.advertisedServices);
      }
    }
    // List IPs
    else if (ip_sw != -1) {
      for (int i = 0; i < ipList->size(); i++) {
        Serial.println("[" + (String)i + "] " + ipList->get(i).toString());
      }
    }
    // List Probes
    else if (pr_sw != -1) {
      for (int i = 0; i < probe_req_ssids->size(); i++) {
        Serial.println("[" + (String)i + "] " + probe_req_ssids->get(i).essid);
      }
    }
    // List SSIDs
    else if (ss_sw != -1) {
      for (int i = 0; i < ssids->size(); i++) {
        if (ssids->get(i).selected) {
          Serial.println("[" + (String)i + "] " + ssids->get(i).essid + " (selected)");
          count_selected += 1;
        } 
        else
          Serial.println("[" + (String)i + "] " + ssids->get(i).essid);
      }
      this->showCounts(count_selected);
    }
    // List Stations
    else if (cl_sw != -1) {
      char sta_mac[] = "00:00:00:00:00:00";
      for (int x = 0; x < access_points->size(); x++) {
        AccessPoint access_point = access_points->get(x);
        Serial.println("[" + (String)x + "] " + access_point.essid + " " + (String)access_point.rssi + ":");
        for (int i = 0; i < access_point.stations->size(); i++) {
          Station station = stations->get(access_point.stations->get(i));
          wifi_scan_obj.getMAC(sta_mac, station.mac, 0);
          if (station.selected) {
            Serial.print("  [" + (String)access_point.stations->get(i) + "] ");
            Serial.print(sta_mac);
            Serial.println(F(" (selected)"));
            count_selected += 1;
          }
          else {
            Serial.print("  [" + (String)access_point.stations->get(i) + "] ");
            Serial.println(sta_mac);
          }
        }
      }
      this->showCounts(count_selected);
    }
    // List airtags
    else if (at_sw != -1) {
      for (int i = 0; i < airtags->size(); i++) {
        Serial.println("[" + (String)i + "]MAC: " + airtags->get(i).mac);
      }
    }
  }
  else if (cmd_args.get(0) == INFO_CMD) {
    int ap_sw = this->argSearch(&cmd_args, "-a");

    if (ap_sw != -1) {
      String ap_value;
      if (!this->argumentValue(&cmd_args, ap_sw, "-a", ap_value))
        return;
      int filter_ap = -1;
      if (!this->integerValue(ap_value, filter_ap, "AP index"))
        return;
      if (!this->inRange(access_points->size(), filter_ap)) {
        Serial.println(F("AP index is out of range"));
        return;
      }
      wifi_scan_obj.RunAPInfo(filter_ap, false);
    }
    else {
      wifi_scan_obj.currentScanMode = SHOW_INFO;
      #ifdef HAS_SCREEN
        menu_function_obj.changeMenu(&menu_function_obj.infoMenu);
      #endif
      wifi_scan_obj.RunInfo();
    }
  }
  else if (cmd_args.get(0) == JOIN_CMD) {
    int ap_sw = this->argSearch(&cmd_args, "-a");
    int pw_sw = this->argSearch(&cmd_args, "-p");
    int s_sw  = this->argSearch(&cmd_args, "-s");

    if ((ap_sw != -1) && (pw_sw != -1)) {
      String ap_value;
      String password;
      if (!this->argumentValue(&cmd_args, ap_sw, "-a", ap_value) ||
          !this->argumentValue(&cmd_args, pw_sw, "-p", password))
        return;
      int index = -1;
      if (!this->integerValue(ap_value, index, "AP index"))
        return;
      if (!this->inRange(access_points->size(), index)) {
        Serial.println(F("AP index is out of range"));
        return;
      }
      AccessPoint access_point = access_points->get(index);
      Serial.println("Using SSID: " + (String)access_point.essid);
      //wifi_scan_obj.currentScanMode = LV_JOIN_WIFI;
      //wifi_scan_obj.StartScan(LV_JOIN_WIFI, TFT_YELLOW); 
      wifi_scan_obj.joinWiFi(access_point.essid, password, false);
      #ifdef HAS_SCREEN
        #ifdef HAS_MINI_KB
          menu_function_obj.changeMenu(menu_function_obj.current_menu);
        #endif
      #endif
    }
    else if (s_sw != -1) {
      String ssid = settings_obj.loadSetting<String>("ClientSSID");
      String pw = settings_obj.loadSetting<String>("ClientPW");

      if ((ssid != "") && (pw != "")) {
        wifi_scan_obj.joinWiFi(ssid, pw, false);
        #ifdef HAS_SCREEN
          menu_function_obj.changeMenu(menu_function_obj.current_menu);
        #endif
      }
      else {
        Serial.println(F("There are no saved WiFi credentials"));
      }
    }
    else {
      Serial.println(F("You did not provide the proper args"));
      return;
    }
  }
  // Select access points or stations
  else if (cmd_args.get(0) == SEL_CMD) {
    // Get switches
    int ap_sw = this->argSearch(&cmd_args, "-a");
    int ss_sw = this->argSearch(&cmd_args, "-s");
    int cl_sw = this->argSearch(&cmd_args, "-c");
    int filter_sw = this->argSearch(&cmd_args, "-f");

    count_selected = 0;
    int count_unselected = 0;
    // select Access points
    if (ap_sw != -1) {

      // If the filters parameter was specified
      if (filter_sw != -1) {
        String filter_ap;
        if (!this->argumentValue(&cmd_args, filter_sw, "-f", filter_ap))
          return;
        this->filterAccessPoints(filter_ap);
      } else {
        String ap_value;
        if (!this->argumentValue(&cmd_args, ap_sw, "-a", ap_value))
          return;
        // Get list of indices
        LinkedList<String> ap_index = this->parseCommand(ap_value, ",");

        // Select ALL APs
        if (ap_value == "all") {
          bool all_selected = access_points->size() > 0;
          for (int i = 0; i < access_points->size(); i++)
            all_selected = all_selected && access_points->get(i).selected;
          const bool select = !all_selected;
          for (int i = 0; i < access_points->size(); i++) {
            AccessPoint access_point = access_points->get(i);
            access_point.selected = select;
            access_points->set(i, access_point);
            select ? count_selected++ : count_unselected++;
          }
          this->showCounts(count_selected, count_unselected);
        }
        // Select specific APs
        else {
          // Mark APs as selected
          for (int i = 0; i < ap_index.size(); i++) {
            int index = -1;
            if (!this->integerValue(ap_index.get(i), index, "AP index"))
              continue;
            if (!this->inRange(access_points->size(), index)) {
              Serial.print(F("Index not in range: "));
              Serial.println(index);
              continue;
            }
            AccessPoint access_point = access_points->get(index);
            if (access_point.selected) {
              // Unselect "selected" ap
              AccessPoint new_ap = access_point;
              new_ap.selected = false;
              access_points->set(index, new_ap);
              count_unselected += 1;
            }
            else {
              // Select "unselected" ap
              AccessPoint new_ap = access_point;
              new_ap.selected = true;
              access_points->set(index, new_ap);
              count_selected += 1;
            }
          }
          this->showCounts(count_selected, count_unselected);
        }
      }
    }
    else if (cl_sw != -1) {
      String station_value;
      if (!this->argumentValue(&cmd_args, cl_sw, "-c", station_value))
        return;
      LinkedList<String> sta_index = this->parseCommand(station_value, ",");
      
      // Select all Stations
      if (station_value == "all") {
        bool all_selected = stations->size() > 0;
        for (int i = 0; i < stations->size(); i++)
          all_selected = all_selected && stations->get(i).selected;
        const bool select = !all_selected;
        for (int i = 0; i < stations->size(); i++) {
          Station station = stations->get(i);
          station.selected = select;
          stations->set(i, station);
          if (select && this->inRange(access_points->size(), station.ap)) {
            AccessPoint access_point = access_points->get(station.ap);
            access_point.selected = true;
            access_points->set(station.ap, access_point);
          }
          select ? count_selected++ : count_unselected++;
        }
        this->showCounts(count_selected, count_unselected);
      }
      // Select specific Stations
      else {
        // Mark Stations as selected
        for (int i = 0; i < sta_index.size(); i++) {
          int index = -1;
          if (!this->integerValue(sta_index.get(i), index, "station index"))
            continue;
          if (!this->inRange(stations->size(), index)) {
            Serial.print(F("Index not in range: "));
            Serial.println(index);
            continue;
          }
          Station station = stations->get(index);
          if (station.selected) {
            // Unselect "selected" ap
            Station new_sta = station;
            new_sta.selected = false;
            stations->set(index, new_sta);
            count_unselected += 1;
          }
          else {
            // Select "unselected" ap
            Station new_sta = station;
            new_sta.selected = true;
            stations->set(index, new_sta);
            if (this->inRange(access_points->size(), new_sta.ap)) {
              AccessPoint access_point = access_points->get(new_sta.ap);
              access_point.selected = true;
              access_points->set(new_sta.ap, access_point);
            }
            count_selected += 1;
          }
        }
        this->showCounts(count_selected, count_unselected);
      }
    }
    // select ssids
    else if (ss_sw != -1) {
      String ssid_value;
      if (!this->argumentValue(&cmd_args, ss_sw, "-s", ssid_value))
        return;
      // Get list of indices
      LinkedList<String> ss_index = this->parseCommand(ssid_value, ",");

      // Select ALL SSIDs
      if (ssid_value == "all") {
        bool all_selected = ssids->size() > 0;
        for (int i = 0; i < ssids->size(); i++)
          all_selected = all_selected && ssids->get(i).selected;
        const bool select = !all_selected;
        for (int i = 0; i < ssids->size(); i++) {
          ssid new_ssid = ssids->get(i);
          new_ssid.selected = select;
          ssids->set(i, new_ssid);
          select ? count_selected++ : count_unselected++;
        }
      }
      else {
      // Mark SSIDs as selected
        for (int i = 0; i < ss_index.size(); i++) {
          int index = -1;
          if (!this->integerValue(ss_index.get(i), index, "SSID index"))
            continue;
          if (!this->inRange(ssids->size(), index)) {
            Serial.print(F("Index not in range: "));
            Serial.println(index);
            continue;
          }
          if (ssids->get(index).selected) {
            ssid new_ssid = ssids->get(index);
            new_ssid.selected = false;
            ssids->set(index, new_ssid);
            count_unselected += 1;
          }
          else {
            ssid new_ssid = ssids->get(index);
            new_ssid.selected = true;
            ssids->set(index, new_ssid);
            count_selected += 1;
          }
        }
      }
      this->showCounts(count_selected, count_unselected);
    }
  }
  else if (cmd_args.get(0) == SAVE_CMD) {
    int ap_sw = this->argSearch(&cmd_args, "-a");
    int st_sw = this->argSearch(&cmd_args, "-s");

    if (ap_sw != -1) {
      #ifdef HAS_SCREEN
        menu_function_obj.changeMenu(&menu_function_obj.saveAPsMenu);
      #endif
      wifi_scan_obj.RunSaveAPList(true);
    }
    else if (st_sw != -1) {
      #ifdef HAS_SCREEN
        menu_function_obj.changeMenu(&menu_function_obj.saveSSIDsMenu);
      #endif
      wifi_scan_obj.RunSaveSSIDList(true);
    }
  }
  else if (cmd_args.get(0) == LOAD_CMD) {
    int ap_sw = this->argSearch(&cmd_args, "-a");
    int st_sw = this->argSearch(&cmd_args, "-s");

    if (ap_sw != -1) {
      #ifdef HAS_SCREEN
        menu_function_obj.changeMenu(&menu_function_obj.loadAPsMenu);
      #endif
      wifi_scan_obj.RunLoadAPList();
    }
    else if (st_sw != -1) {
      #ifdef HAS_SCREEN
        menu_function_obj.changeMenu(&menu_function_obj.loadSSIDsMenu);
      #endif
      wifi_scan_obj.RunLoadSSIDList();
    }
  }

  // Add AP or station manually
  else if (cmd_args.get(0) == ADD_CMD) {
    int ap_sw = this->argSearch(&cmd_args, "-a");
    int sta_sw = this->argSearch(&cmd_args, "-c");
    int bssid_sw = this->argSearch(&cmd_args, "-b");

    if (ap_sw != -1) {
      // add -a -b <mac> [-ch <channel>] [-e <ssid>]
      if (bssid_sw == -1 || !this->checkValueExists(&cmd_args, bssid_sw)) {
        Serial.println(F("BSSID required: add -a -b <mac>"));
        return;
      }

      String mac_str;
      if (!this->argumentValue(&cmd_args, bssid_sw, "-b", mac_str))
        return;
      uint8_t mac[6];
      if (sscanf(mac_str.c_str(), "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx",
             &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) != 6) {
        Serial.println(F("Format must be XX:XX:XX:XX:XX:XX"));
        return;
      }

      // Duplicate check
      for (int i = 0; i < access_points->size(); i++) {
        AccessPoint access_point = access_points->get(i);
        bool match = true;
        for (int x = 0; x < 6; x++) {
          if (mac[x] != access_point.bssid[x]) {
            match = false;
            break;
          }
        }
        if (match) {
          Serial.print(F("AP already exists at ["));
          Serial.print(i);
          Serial.print(F("]: "));
          Serial.println(access_point.essid);
          return;
        }
      }

      int ch_sw = this->argSearch(&cmd_args, "-ch");
      uint8_t channel = 1;
      if (ch_sw != -1) {
        String channel_value;
        if (!this->argumentValue(&cmd_args, ch_sw, "-ch", channel_value))
          return;
        int requested_channel = 0;
        if (!this->integerValue(channel_value, requested_channel, "channel"))
          return;
        if (requested_channel < 1 || requested_channel > 196) {
          Serial.println(F("Channel is outside the supported range"));
          return;
        }
        channel = static_cast<uint8_t>(requested_channel);
      }

      int essid_sw = this->argSearch(&cmd_args, "-e");
      String essid = mac_str;
      if (essid_sw != -1 &&
          !this->argumentValue(&cmd_args, essid_sw, "-e", essid))
        return;

      AccessPoint ap{};
      ap.essid = essid;
      ap.channel = channel;
      memcpy(ap.bssid, mac, 6);
      ap.selected = true;
      ap.stations = new LinkedList<uint16_t>();
      ap.beacon[0] = 0;
      ap.beacon[1] = 0;
      ap.rssi = -127;
      ap.packets = 0;
      ap.sec = 0;
      ap.wps = false;
      ap.man = "";
      ap.has_msg_1 = false;
      ap.has_msg_2 = false;
      ap.has_msg_3 = false;
      ap.has_msg_4 = false;
      ap.last_seen_ms = millis();

      access_points->add(ap);

      Serial.print(F("Added AP ["));
      Serial.print(access_points->size() - 1);
      Serial.print(F("][CH:"));
      Serial.print(channel);
      Serial.print(F("] "));
      Serial.print(essid);
      Serial.println(F(" (selected)"));
    }
    else if (sta_sw != -1) {
      // add -c -b <mac> -ap <ap_index>
      if (bssid_sw == -1 || !this->checkValueExists(&cmd_args, bssid_sw)) {
        Serial.println(F("MAC required: add -c -b <mac> -ap <index>"));
        return;
      }

      int ap_idx_sw = this->argSearch(&cmd_args, "-ap");
      if (ap_idx_sw == -1 || !this->checkValueExists(&cmd_args, ap_idx_sw)) {
        Serial.println(F("AP index required: add -c -b <mac> -ap <index>"));
        return;
      }

      String ap_value;
      if (!this->argumentValue(&cmd_args, ap_idx_sw, "-ap", ap_value))
        return;
      int ap_index = -1;
      if (!this->integerValue(ap_value, ap_index, "AP index"))
        return;
      if (!this->inRange(access_points->size(), ap_index)) {
        Serial.print(F("AP index not in range: "));
        Serial.println(ap_index);
        return;
      }

      String mac_str;
      if (!this->argumentValue(&cmd_args, bssid_sw, "-b", mac_str))
        return;
      uint8_t mac[6];
      if (sscanf(mac_str.c_str(), "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx",
             &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) != 6) {
        Serial.println(F("Invalid MAC address format: use XX:XX:XX:XX:XX:XX"));
        return;
      }

      // Duplicate check
      for (int i = 0; i < stations->size(); i++) {
        bool match = true;
        for (int x = 0; x < 6; x++) {
          if (mac[x] != stations->get(i).mac[x]) {
            match = false;
            break;
          }
        }
        if (match) {
          Serial.print(F("Station already exists at ["));
          Serial.print(i);
          Serial.println(F("]"));
          return;
        }
      }

      Station sta;
      memcpy(sta.mac, mac, 6);
      sta.selected = true;
      sta.packets = 0;
      sta.ap = ap_index;

      stations->add(sta);

      // Link station to AP
      AccessPoint ap = access_points->get(ap_index);
      ap.selected = true;
      ap.stations->add(stations->size() - 1);
      access_points->set(ap_index, ap);

      Serial.print(F("Added station ["));
      Serial.print(stations->size() - 1);
      Serial.print(F("] -> AP ["));
      Serial.print(ap_index);
      Serial.print(F("] "));
      Serial.print(ap.essid);
      Serial.println(F(" (selected)"));
    }
    else {
      Serial.println(F("Usage: add -a -b <mac> or add -c -b <mac> -ap <index>"));
    }
  }

  // SSID stuff
  else if (cmd_args.get(0) == SSID_CMD) {
    int add_sw = this->argSearch(&cmd_args, "-a");
    int gen_sw = this->argSearch(&cmd_args, "-g");
    int spc_sw = this->argSearch(&cmd_args, "-n");
    int rem_sw = this->argSearch(&cmd_args, "-r");

    // Add ssid
    if (add_sw != -1) {
      // Generate random
      if (gen_sw != -1) {
        String count_value;
        if (!this->argumentValue(&cmd_args, gen_sw, "-g", count_value))
          return;
        int gen_count = 0;
        if (!this->integerValue(count_value, gen_count, "SSID count"))
          return;
        if (gen_count < 1 || gen_count > 256) {
          Serial.println(F("SSID generation count must be 1-256"));
          return;
        }
        wifi_scan_obj.generateSSIDs(gen_count);
      }
      // Add specific
      else if (spc_sw != -1) {
        String essid;
        if (!this->argumentValue(&cmd_args, spc_sw, "-n", essid))
          return;
        wifi_scan_obj.addSSID(essid);
      }
      else {
        Serial.println(F("You did not specify how to add SSIDs"));
      }
    }
    // Remove SSID
    else if (rem_sw != -1) {
      String index_value;
      if (!this->argumentValue(&cmd_args, rem_sw, "-r", index_value))
        return;
      int index = -1;
      if (!this->integerValue(index_value, index, "SSID index"))
        return;
      if (!this->inRange(ssids->size(), index)) {
        Serial.print(F("Index not in range: "));
        Serial.println(index);
        return;
      }
      wifi_scan_obj.removeSSID(index);
    }
    else {
      Serial.println(F("You did not specify whether to add or remove SSIDs"));
      return;
    }
  }
}
