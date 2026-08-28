#pragma once

#ifndef Buffer_h
#define Buffer_h

#include "Arduino.h"
#include "FS.h"
#include "settings.h"
#include "esp_wifi_types.h"
#include "configs.h"

extern Settings settings_obj;

class Buffer {
  public:
    Buffer();
    ~Buffer();
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    void pcapOpen(const char* file_name, fs::FS* fs, bool serial);
    void logOpen(const char* file_name, fs::FS* fs, bool serial,
                 bool force = false);
    void gpxOpen(const char* file_name, fs::FS* fs, bool serial);
    void append(wifi_promiscuous_pkt_t* packet, int len);
    void append(String log);
    void save();
    String getFileName();

  private:
    bool createFile(const char* name, bool is_pcap, bool is_gpx = false);
    bool ensureAllocated();
    void open(bool is_pcap);
    void openFile(const char* file_name, fs::FS* fs, bool serial,
                  bool is_pcap, bool is_gpx = false, bool force = false);
    bool add(const uint8_t* data, uint32_t len, bool is_pcap);
    bool write(const uint8_t* data, uint32_t len);
    bool saveFs(const uint8_t* data, uint32_t len);
    bool saveSerial(const uint8_t* data, uint32_t len);

    uint8_t* bufA = nullptr;
    uint8_t* bufB = nullptr;
    uint32_t bufSizeA = 0;
    uint32_t bufSizeB = 0;

    bool writing = false;
    bool useA = true;
    bool savingA = false;
    bool savingB = false;
    uint32_t droppedRecords = 0;
    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

    String fileName = "/0.pcap";
    File file;
    fs::FS* fs = nullptr;
    bool serial = false;
};

#endif
