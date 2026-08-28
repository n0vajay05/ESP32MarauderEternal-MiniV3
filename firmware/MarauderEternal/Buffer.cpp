#include "Buffer.h"
#include "PcapHeader.h"
#include "lang_var.h"

Buffer::Buffer() {}

Buffer::~Buffer() {
  free(bufA);
  free(bufB);
}

bool Buffer::ensureAllocated() {
  if (bufA != nullptr && bufB != nullptr)
    return true;

  free(bufA);
  free(bufB);
  bufA = nullptr;
  bufB = nullptr;

  #ifdef HAS_PSRAM
    bufA = static_cast<uint8_t*>(ps_malloc(BUF_SIZE));
    bufB = static_cast<uint8_t*>(ps_malloc(BUF_SIZE));
  #else
    bufA = static_cast<uint8_t*>(malloc(BUF_SIZE));
    bufB = static_cast<uint8_t*>(malloc(BUF_SIZE));
  #endif

  if (bufA == nullptr || bufB == nullptr) {
    free(bufA);
    free(bufB);
    bufA = nullptr;
    bufB = nullptr;
    Serial.println(F("Capture buffers could not be allocated"));
    return false;
  }
  return true;
}

bool Buffer::createFile(const char* name, bool is_pcap, bool is_gpx) {
  int index = 0;
  const char* extension = is_pcap ? "pcap" : (is_gpx ? "gpx" : "log");
  do {
    fileName = "/" + String(name) + "_" + String(index++) + "." + extension;
  } while (fs->exists(fileName));

  Serial.println(fileName);
  file = fs->open(fileName, FILE_WRITE);
  if (!file) {
    Serial.println("Could not create capture file '" + fileName + "'");
    return false;
  }
  file.close();
  return true;
}

void Buffer::open(bool is_pcap) {
  portENTER_CRITICAL(&mux);
  bufSizeA = 0;
  bufSizeB = 0;
  useA = true;
  savingA = false;
  savingB = false;
  droppedRecords = 0;
  writing = true;
  portEXIT_CRITICAL(&mux);

  if (is_pcap) {
    uint8_t header[marauder::kPcapGlobalHeaderSize];
    marauder::makePcapGlobalHeader(SNAP_LEN, header);
    write(header, sizeof(header));
  }
}

String Buffer::getFileName() {
  return fileName;
}

void Buffer::openFile(const char* file_name, fs::FS* destination_fs,
                      bool serial_output, bool is_pcap, bool is_gpx,
                      bool force) {
  const bool save_pcap = settings_obj.loadSetting<bool>("SavePCAP");
  if (!save_pcap && !force) {
    portENTER_CRITICAL(&mux);
    writing = false;
    portEXIT_CRITICAL(&mux);
    fs = nullptr;
    serial = false;
    return;
  }

  fs = destination_fs;
  serial = serial_output;
  if (fs != nullptr && !createFile(file_name, is_pcap, is_gpx))
    fs = nullptr;

  if ((fs != nullptr || serial) && ensureAllocated())
    open(is_pcap);
  else {
    portENTER_CRITICAL(&mux);
    writing = false;
    portEXIT_CRITICAL(&mux);
  }
}

void Buffer::pcapOpen(const char* file_name, fs::FS* fs, bool serial) {
  openFile(file_name, fs, serial, true);
}

void Buffer::logOpen(const char* file_name, fs::FS* fs, bool serial,
                     bool force) {
  openFile(file_name, fs, serial, false, false, force);
}

void Buffer::gpxOpen(const char* file_name, fs::FS* fs, bool serial) {
  openFile(file_name, fs, serial, false, true);
}

bool Buffer::add(const uint8_t* data, uint32_t len, bool is_pcap) {
  if (data == nullptr || len == 0)
    return false;

  const uint32_t header_len = is_pcap ? 16 : 0;
  const uint32_t record_len = header_len + len;
  if (record_len > BUF_SIZE) {
    portENTER_CRITICAL(&mux);
    droppedRecords++;
    portEXIT_CRITICAL(&mux);
    return false;
  }

  uint8_t header[16];
  if (is_pcap) {
    uint32_t microseconds = micros();
    const uint32_t seconds = microseconds / 1000000;
    microseconds -= seconds * 1000000;
    const uint32_t values[4] = {seconds, microseconds, len, len};
    for (size_t value = 0; value < 4; value++) {
      header[value * 4] = values[value];
      header[value * 4 + 1] = values[value] >> 8;
      header[value * 4 + 2] = values[value] >> 16;
      header[value * 4 + 3] = values[value] >> 24;
    }
  }

  portENTER_CRITICAL(&mux);
  if (!writing || bufA == nullptr || bufB == nullptr) {
    portEXIT_CRITICAL(&mux);
    return false;
  }

  uint32_t* active_size = useA ? &bufSizeA : &bufSizeB;
  const bool other_available = useA ? (!savingB && bufSizeB == 0) :
                                      (!savingA && bufSizeA == 0);
  if (*active_size + record_len > BUF_SIZE && other_available) {
    useA = !useA;
    active_size = useA ? &bufSizeA : &bufSizeB;
  }

  if (*active_size + record_len > BUF_SIZE || (useA ? savingA : savingB)) {
    droppedRecords++;
    portEXIT_CRITICAL(&mux);
    return false;
  }

  uint8_t* destination = (useA ? bufA : bufB) + *active_size;
  if (header_len > 0) {
    memcpy(destination, header, header_len);
    destination += header_len;
  }
  memcpy(destination, data, len);
  *active_size += record_len;
  portEXIT_CRITICAL(&mux);
  return true;
}

void Buffer::append(wifi_promiscuous_pkt_t* packet, int len) {
  // openFile() applies the SavePCAP policy once. Avoid a settings lookup from
  // every promiscuous callback, and allow explicitly forced diagnostic logs.
  if (packet != nullptr && len > 0)
    add(packet->payload, static_cast<uint32_t>(len), true);
}

void Buffer::append(String log) {
  add(reinterpret_cast<const uint8_t*>(log.c_str()), log.length(), false);
}

bool Buffer::write(const uint8_t* data, uint32_t len) {
  return add(data, len, false);
}

bool Buffer::saveFs(const uint8_t* data, uint32_t len) {
  file = fs->open(fileName, FILE_APPEND);
  if (!file) {
    Serial.println(text02 + fileName + "'");
    return false;
  }
  const bool success = file.write(data, len) == len;
  file.close();
  if (!success)
    Serial.println(F("Capture write was incomplete; buffered data retained"));
  return success;
}

bool Buffer::saveSerial(const uint8_t* data, uint32_t len) {
  const char* mark_begin = "[BUF/BEGIN]";
  const size_t mark_begin_len = strlen(mark_begin);
  const char* mark_close = "[BUF/CLOSE]";
  const size_t mark_close_len = strlen(mark_close);
  const size_t total = mark_begin_len + len + mark_close_len;

  uint8_t* output = static_cast<uint8_t*>(malloc(total));
  if (output == nullptr) {
    Serial.println(F("Could not allocate serial capture buffer"));
    return false;
  }

  uint8_t* cursor = output;
  memcpy(cursor, mark_begin, mark_begin_len);
  cursor += mark_begin_len;
  memcpy(cursor, data, len);
  cursor += len;
  memcpy(cursor, mark_close, mark_close_len);

  const bool success = Serial.write(output, total) == total;
  free(output);
  return success;
}

void Buffer::save() {
  uint8_t* data = nullptr;
  uint32_t length = 0;
  bool selectedA = false;

  portENTER_CRITICAL(&mux);
  if (bufA == nullptr || bufB == nullptr) {
    portEXIT_CRITICAL(&mux);
    return;
  }

  // Flush the inactive buffer first; it contains the oldest complete records.
  if (useA && bufSizeB > 0 && !savingB) {
    selectedA = false;
  } else if (!useA && bufSizeA > 0 && !savingA) {
    selectedA = true;
  } else if (useA && bufSizeA > 0 && !savingA &&
             bufSizeB == 0 && !savingB) {
    useA = false;
    selectedA = true;
  } else if (!useA && bufSizeB > 0 && !savingB &&
             bufSizeA == 0 && !savingA) {
    useA = true;
    selectedA = false;
  } else {
    portEXIT_CRITICAL(&mux);
    return;
  }

  if (selectedA) {
    savingA = true;
    data = bufA;
    length = bufSizeA;
  } else {
    savingB = true;
    data = bufB;
    length = bufSizeB;
  }
  portEXIT_CRITICAL(&mux);

  bool success = true;
  if (fs != nullptr)
    success = saveFs(data, length) && success;
  if (serial)
    success = saveSerial(data, length) && success;

  portENTER_CRITICAL(&mux);
  if (selectedA) {
    if (success)
      bufSizeA = 0;
    savingA = false;
  } else {
    if (success)
      bufSizeB = 0;
    savingB = false;
  }
  const uint32_t dropped = droppedRecords;
  droppedRecords = 0;
  portEXIT_CRITICAL(&mux);

  if (dropped > 0)
    Serial.printf("Capture buffer dropped %lu record(s)\n",
                  static_cast<unsigned long>(dropped));
}
