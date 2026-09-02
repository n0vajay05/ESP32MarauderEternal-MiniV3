#include "SDInterface.h"
#include "lang_var.h"

namespace {
  bool removeTree(fs::FS& fs, const String& path, bool keep_root = false) {
    if (!fs.exists(path))
      return true;

    File node = fs.open(path);
    if (!node)
      return false;

    if (!node.isDirectory()) {
      node.close();
      return fs.remove(path);
    }

    File child = node.openNextFile();
    while (child) {
      String child_path = child.path();
      child.close();
      if (!removeTree(fs, child_path)) {
        node.close();
        return false;
      }
      child = node.openNextFile();
    }

    node.close();
    return keep_root || fs.rmdir(path);
  }

  String joinPath(const String& base, const String& child) {
    return base == "/" ? "/" + child : base + "/" + child;
  }

  void appendDirectoryFiles(LinkedList<String>* file_names,
                            const String& directory, const String& extension,
                            bool recursive, uint8_t depth = 0) {
    if (file_names == nullptr || depth > 8)
      return;

    File dir = SD.open(directory);
    if (!dir || !dir.isDirectory()) {
      if (dir)
        dir.close();
      return;
    }

    File entry = dir.openNextFile();
    while (entry) {
      String entry_path = entry.path();
      const String entry_name = marauder::storage::baseName(entry.name());
      const String expected_prefix =
          directory == "/" ? "/" : directory + "/";
      if (entry_path.length() == 0 ||
          !entry_path.startsWith(expected_prefix))
        entry_path = joinPath(directory, entry_name);
      const bool is_directory = entry.isDirectory();
      entry.close();

      if (is_directory) {
        if (recursive)
          appendDirectoryFiles(file_names, entry_path, extension, true,
                               depth + 1);
      }
      else if (extension.length() == 0 || entry_path.endsWith(extension)) {
        file_names->add(marauder::storage::relativePath(entry_path));
      }

      entry = dir.openNextFile();
    }
    dir.close();
  }

  bool copyTree(
    fs::FS& source,
    const String& source_path,
    fs::FS* destination,
    const String& destination_path,
    size_t& files_copied,
    size_t& bytes_copied,
    uint8_t& error
  ) {
    File source_node = source.open(source_path);
    if (!source_node) {
      error = 3;
      return false;
    }

    if (!source_node.isDirectory()) {
      if (destination) {
        File destination_file = destination->open(destination_path, FILE_WRITE);
        if (!destination_file) {
          source_node.close();
          error = 3;
          return false;
        }

        uint8_t buffer[512];
        while (source_node.available()) {
          size_t bytes_read = source_node.read(buffer, sizeof(buffer));
          if (bytes_read == 0 || destination_file.write(buffer, bytes_read) != bytes_read) {
            source_node.close();
            destination_file.close();
            error = 3;
            return false;
          }
          bytes_copied += bytes_read;
        }
        destination_file.close();
      }
      else
        bytes_copied += source_node.size();
      source_node.close();
      files_copied++;
      return true;
    }

    if (destination && destination_path != "/" &&
        !destination->exists(destination_path) && !destination->mkdir(destination_path)) {
      source_node.close();
      error = 3;
      return false;
    }

    File child = source_node.openNextFile();
    while (child) {
      String child_source_path = child.path();
      String child_name = child_source_path;
      if (child_name.startsWith(source_path))
        child_name.remove(0, source_path.length());
      while (child_name.startsWith("/"))
        child_name.remove(0, 1);
      child.close();

      if (!copyTree(
        source,
        child_source_path,
        destination,
        joinPath(destination_path, child_name),
        files_copied,
        bytes_copied,
        error
      )) {
        source_node.close();
        return false;
      }
      child = source_node.openNextFile();
    }

    source_node.close();
    return true;
  }
}

#ifdef HAS_C5_SD
  SDInterface::SDInterface(SPIClass* spi, int cs)
    : _spi(spi), _cs(cs) {}
#endif

bool SDInterface::initSD() {
  #ifdef HAS_SD
    #ifdef KIT
      pinMode(SD_DET, INPUT);
      if (digitalRead(SD_DET) != LOW) {
        this->supported = false;
        return false;
      }
    #endif

    pinMode(SD_CS, OUTPUT);

    delay(10);
    #if (defined(MARAUDER_M5STICKC)) || (defined(HAS_CYD_TOUCH)) || (defined(MARAUDER_CARDPUTER)) || (defined(MARAUDER_CARDPUTER_ADV))
      /* Set up SPI SD Card using external pin header
      StickCPlus Header - SPI SD Card Reader
                  3v3   -   3v3
                  GND   -   GND
                   G0   -   CLK
              G36/G25   -   MISO
                  G26   -   MOSI
                        -   CS (jumper to SD Card GND Pin)
      */
      #if defined(MARAUDER_M5STICKC)
        enum { SPI_SCK = 0, SPI_MISO = 36, SPI_MOSI = 26 };
      #elif defined(HAS_CYD_TOUCH) || defined(MARAUDER_CARDPUTER) || defined(MARAUDER_CARDPUTER_ADV) || defined(HAS_SEPARATE_SD)
        enum { SPI_SCK = SD_SCK, SPI_MISO = SD_MISO, SPI_MOSI = SD_MOSI };
      #else
        enum { SPI_SCK = 0, SPI_MISO = 36, SPI_MOSI = 26 };
      #endif
      #if !defined(MARAUDER_CARDPUTER) && !defined(MARAUDER_CARDPUTER_ADV)
        this->spiExt = new SPIClass();
      #else
        this->spiExt = new SPIClass(FSPI);
      #endif
      Serial.println(F("Using external SPI configuration..."));
      this->spiExt->begin(SPI_SCK, SPI_MISO, SPI_MOSI, SD_CS);
      if (!SD.begin(SD_CS, *(this->spiExt))) {
    #elif defined(HAS_C5_SD)
      if (!SD.begin(SD_CS, *_spi)) {
    #else
      if (!SD.begin(SD_CS)) {
    #endif
      Serial.println(F("Failed to mount SD Card"));
      this->supported = false;
      return false;
    }
    else {
      this->supported = true;
      this->cardType = SD.cardType();

      this->cardSizeMB = SD.cardSize() / (1024 * 1024);
      this->card_sz = String(this->cardSizeMB);

      this->ensureStorageLayout();

      if (this->sd_files == nullptr)
        this->sd_files = new LinkedList<String>();
    
      return true;
  }

  #else
    return false;
  #endif
}

bool SDInterface::ensureStorageLayout() {
  #ifdef HAS_SD
    if (!this->supported)
      return false;

    const char* directories[] = {
      marauder::storage::CAPTURES_DIR,
      marauder::storage::LOGS_DIR,
      marauder::storage::GPS_DIR,
      marauder::storage::WARDRIVE_DIR,
      marauder::storage::LISTS_DIR,
      marauder::storage::EVIL_PORTAL_DIR,
      marauder::storage::EVIL_PORTAL_HTML_DIR,
      marauder::storage::CONFIG_DIR,
      marauder::storage::FIRMWARE_DIR,
      marauder::storage::SCRIPTS_DIR,
    };

    bool complete = true;
    for (const char* directory : directories) {
      if (!SD.exists(directory) && !SD.mkdir(directory)) {
        Serial.println(String(F("Could not create SD directory ")) +
                       directory);
        complete = false;
      }
    }
    return complete;
  #else
    return false;
  #endif
}

File SDInterface::getFile(String path) {
  if (this->supported) {
    File file = SD.open(path, FILE_READ);

    //if (file)
    return file;
  }
  return File();
}

bool SDInterface::removeFile(String file_path) {
  if (SD.remove(file_path))
    return true;
  else
    return false;
}

bool SDInterface::migrateSPIFFS(uint8_t operation, size_t& files_copied,
                                size_t& bytes_copied, uint8_t& error) {
  files_copied = bytes_copied = error = 0;

  if (!this->supported) {
    error = 1;
    return false;
  }

  const String backup_path = "/spiffs";
  File backup = SD.open(backup_path);
  bool valid_backup = backup && backup.isDirectory();
  backup.close();

  if (operation == 1) {
    if (!valid_backup) {
      error = 2;
      return false;
    }
    return copyTree(SD, backup_path, nullptr, "", files_copied,
                    bytes_copied, error);
  }

  if (operation == 2) {
    if (!valid_backup) {
      error = 2;
      return false;
    }

    const String rollback_path = "/spiffs.restore-rollback";
    if (!removeTree(SD, rollback_path)) {
      error = 3;
      return false;
    }

    size_t rollback_files = 0;
    size_t rollback_bytes = 0;
    uint8_t rollback_error = 0;
    if (!copyTree(SPIFFS, "/", &SD, rollback_path, rollback_files,
                  rollback_bytes, rollback_error)) {
      removeTree(SD, rollback_path);
      error = 3;
      return false;
    }

    bool cleared = removeTree(SPIFFS, "/", true);
    if (cleared && copyTree(SD, backup_path, &SPIFFS, "/", files_copied,
                            bytes_copied, error)) {
      removeTree(SD, rollback_path);
      return true;
    }

    removeTree(SPIFFS, "/", true);
    size_t recovered_files = 0;
    size_t recovered_bytes = 0;
    uint8_t recovery_error = 0;
    copyTree(SD, rollback_path, &SPIFFS, "/", recovered_files,
             recovered_bytes, recovery_error);
    removeTree(SD, rollback_path);
    error = 3;
    return false;
  }

  const String staging_path = "/spiffs.tmp";
  const String previous_path = "/spiffs.previous";

  if (!removeTree(SD, staging_path) || !removeTree(SD, previous_path)) {
    error = 3;
    return false;
  }

  if (!copyTree(SPIFFS, "/", &SD, staging_path, files_copied,
                bytes_copied, error)) {
    removeTree(SD, staging_path);
    return false;
  }

  if (SD.exists(backup_path) && !SD.rename(backup_path, previous_path)) {
    removeTree(SD, staging_path);
    error = 3;
    return false;
  }

  if (!SD.rename(staging_path, backup_path)) {
    if (SD.exists(previous_path))
      SD.rename(previous_path, backup_path);
    error = 3;
    return false;
  }

  removeTree(SD, previous_path);
  return true;
}

void SDInterface::listDirToLinkedList(LinkedList<String>* file_names,
                                      String str_dir, String ext,
                                      bool recursive) {
  if (this->supported)
    appendDirectoryFiles(file_names, str_dir, ext, recursive);
}

void SDInterface::listDir(String str_dir){
  if (this->supported) {
    File dir = SD.open(str_dir);
    while (true)
    {
      File entry = dir.openNextFile();
      if (! entry)
      {
        break;
      }
      //for (uint8_t i = 0; i < numTabs; i++)
      //{
      //  Serial.print('\t');
      //}
      Serial.print(entry.name());
      Serial.print("\t");
      Serial.println(entry.size());
      entry.close();
    }
  }
}

void SDInterface::runUpdate(String file_name) {
  if (file_name == "") {
    file_name = marauder::storage::DEFAULT_UPDATE;
    if (!SD.exists(file_name) && SD.exists("/update.bin"))
      file_name = "/update.bin";
  }

  #ifdef HAS_SCREEN
    display_obj.tft.setTextWrap(false);
    display_obj.tft.setFreeFont(NULL);
    display_obj.tft.setCursor(0, TFT_HEIGHT / 3);
    display_obj.tft.setTextSize(1);
    display_obj.tft.setTextColor(TFT_WHITE);
  
    display_obj.tft.println("Opening " + file_name + "...");
  #endif

  File updateBin = SD.open(file_name);

  if (updateBin) {
    if(updateBin.isDirectory()){
      #ifdef HAS_SCREEN
        display_obj.tft.setTextColor(TFT_RED);
        display_obj.tft.println(F(text_table2[0]));
      #endif
      Serial.print(F("Error, could not find \""));
      Serial.print(file_name);
      Serial.println(F("\""));
      #ifdef HAS_SCREEN
        display_obj.tft.setTextColor(TFT_WHITE);
      #endif
      updateBin.close();
      return;
    }

    size_t updateSize = updateBin.size();

    if (updateSize > 0) {
      if (!this->validateUpdate(updateBin)) {
        updateBin.close();
        return;
      }
      #ifdef HAS_SCREEN
        display_obj.tft.println(F(text_table2[1]));
      #endif
      Serial.println(F("Starting update over SD. Please wait..."));
      if (!this->performUpdate(updateBin, updateSize)) {
        updateBin.close();
        return;
      }
    }
    else {
      #ifdef HAS_SCREEN
        display_obj.tft.setTextColor(TFT_RED);
        display_obj.tft.println(F(text_table2[2]));
      #endif
      Serial.println(F("Error, file is empty"));
      #ifdef HAS_SCREEN
        display_obj.tft.setTextColor(TFT_WHITE);
      #endif
      updateBin.close();
      return;
    }

    updateBin.close();
    
      // whe finished remove the binary from sd card to indicate end of the process
    #ifdef HAS_SCREEN
      display_obj.tft.println(F(text_table2[3]));
    #endif
    // Update.end() validates the image and selects the partition written by
    // Update.begin(). Selecting "next" again here can choose a different slot.
    ESP.restart();
  }
  else {
    #ifdef HAS_SCREEN
      display_obj.tft.setTextColor(TFT_RED);
      display_obj.tft.println(F(text_table2[4]));
    #endif
    Serial.println("Could not load firmware image " + file_name);
    #ifdef HAS_SCREEN
      display_obj.tft.setTextColor(TFT_WHITE);
    #endif
  }
}

bool SDInterface::validateUpdate(File &updateBin) {
  constexpr size_t ESP_IMAGE_HEADER_SIZE = 24;
  constexpr uint8_t ESP_IMAGE_MAGIC = 0xe9;
  constexpr uint16_t ESP32_C5_IMAGE_ID = 0x17;
  constexpr size_t APPLICATION_PARTITION_SIZE = 0x3c0000;

  if (updateBin.size() < ESP_IMAGE_HEADER_SIZE ||
      updateBin.size() > APPLICATION_PARTITION_SIZE) {
    Serial.println(F("Rejected SD update: image size does not fit app partition"));
    return false;
  }

  uint8_t image_header[ESP_IMAGE_HEADER_SIZE];
  updateBin.seek(0);
  if (updateBin.read(image_header, sizeof(image_header)) !=
          static_cast<int>(sizeof(image_header)) ||
      image_header[0] != ESP_IMAGE_MAGIC ||
      (static_cast<uint16_t>(image_header[12]) |
       (static_cast<uint16_t>(image_header[13]) << 8)) !=
          ESP32_C5_IMAGE_ID) {
    updateBin.seek(0);
    Serial.println(F("Rejected SD update: invalid ESP32-C5 application header"));
    return false;
  }

  MarauderFirmware::MetadataScanner scanner;
  uint8_t buffer[512];
  updateBin.seek(0);

  while (updateBin.available() && !scanner.found()) {
    size_t bytes_read = updateBin.read(buffer, sizeof(buffer));
    for (size_t i = 0; i < bytes_read && !scanner.found(); i++)
      scanner.push(buffer[i]);
  }

  updateBin.seek(0);

  if (!scanner.found()) {
    #ifdef HAS_SCREEN
      display_obj.tft.setTextColor(TFT_RED);
      display_obj.tft.println(F("Rejected: invalid image"));
      display_obj.tft.setTextColor(TFT_WHITE);
    #endif
    Serial.println(F("Rejected SD update: Eternal firmware identity not found"));
    return false;
  }

  const MarauderFirmware::Metadata &candidate = scanner.metadata();
  const MarauderFirmware::Metadata &current = MarauderFirmware::currentMetadata();
  if (!MarauderFirmware::metadataMatches(candidate, current)) {
    #ifdef HAS_SCREEN
      display_obj.tft.setTextColor(TFT_RED);
      display_obj.tft.println(F("Rejected: wrong device"));
      display_obj.tft.setTextColor(TFT_WHITE);
    #endif
    Serial.print(F("Rejected SD update: expected "));
    Serial.print(current.hardware);
    Serial.print(F("/"));
    Serial.print(current.chip);
    Serial.print(F(", got "));
    Serial.print(candidate.hardware);
    Serial.print(F("/"));
    Serial.println(candidate.chip);
    return false;
  }

  Serial.print(F("Validated SD update for "));
  Serial.print(candidate.hardware);
  Serial.print(F("/"));
  Serial.print(candidate.chip);
  Serial.print(F(" version "));
  Serial.println(candidate.version);
  return true;
}

bool SDInterface::performUpdate(Stream &updateSource, size_t updateSize) {
  if (Update.begin(updateSize)) {   
    #ifdef HAS_SCREEN
      display_obj.tft.println(text_table2[5] + String(updateSize));
      display_obj.tft.println(F(text_table2[6]));
    #endif
    size_t written = Update.writeStream(updateSource);
    if (written == updateSize) {
      #ifdef HAS_SCREEN
        display_obj.tft.println(text_table2[7] + String(written) + text_table2[10]);
      #endif
      Serial.print(F("Written : "));
      Serial.print(written);
      Serial.println(F(" successfully"));
    }
    else {
      #ifdef HAS_SCREEN
        display_obj.tft.println(text_table2[8] + String(written) + "/" + String(updateSize) + text_table2[9]);
      #endif
      Serial.print(F("Written only : "));
      Serial.print(written);
      Serial.print(F("/"));
      Serial.print(updateSize);
      Serial.println(F(". Retry?"));
      Update.abort();
      return false;
    }
    if (Update.end()) {
      if (Update.isFinished()) {
        return true;
      }
      else {
        #ifdef HAS_SCREEN
          display_obj.tft.setTextColor(TFT_RED);
          display_obj.tft.println(text_table2[12]);
        #endif
        Serial.println(F("Update not finished? Something went wrong!"));
        #ifdef HAS_SCREEN
          display_obj.tft.setTextColor(TFT_WHITE);
        #endif
        return false;
      }
    }
    else {
      #ifdef HAS_SCREEN
        display_obj.tft.println(text_table2[13] + String(Update.getError()));
      #endif
      Serial.print(F("Error Occurred. Error #: "));
      Serial.println(Update.getError());
      return false;
    }

  }
  else
  {
    #ifdef HAS_SCREEN
      display_obj.tft.println(text_table2[14]);
    #endif
    Serial.println(F("Not enough space to begin OTA"));
    return false;
  }
}
