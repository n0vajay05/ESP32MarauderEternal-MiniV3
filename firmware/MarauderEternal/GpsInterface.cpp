#include "GpsInterface.h"
#include "DeviceClock.h"

#ifdef HAS_GPS

extern GpsInterface gps_obj;
extern DeviceClock device_clock_obj;

char nmeaBuffer[100];

MicroNMEA nmea(nmeaBuffer, sizeof(nmeaBuffer));

// Use a uniquely named UART object. The Arduino core already defines Serial2,
// and constructing another object with that symbol corrupts startup state.
static HardwareSerial gpsSerial(GPS_SERIAL_INDEX);

static const uint32_t GPS_BAUD_RATES[] = {
    115200, 9600, 38400, 57600, 19200, 4800};
static const uint8_t GPS_BAUD_RATE_COUNT =
    sizeof(GPS_BAUD_RATES) / sizeof(GPS_BAUD_RATES[0]);
static const uint32_t RECOVERY_BAUD_DWELL_MS = 1800;
static const uint32_t GPS_TRAFFIC_TIMEOUT_MS = 6000;
static const uint32_t GPS_FIX_TIMEOUT_MS = 3500;

void GpsInterface::begin() {
  this->gps_enabled = false;
  this->good_fix = false;
  this->gps_baud = 0;
  this->listening_baud = 0;
  this->last_sentence_ms = 0;
  this->last_fix_sentence_ms = 0;
  this->last_baud_switch_ms = 0;
  this->recovery_baud_index = 0;
  this->type_flag = GPSTYPE_NATIVE;
  this->disable_queue();
  nmea.setUnknownSentenceHandler(gps_nmea_notimp);

  // Start at the most common receiver baud and rotate in main(). Probing every
  // baud synchronously delayed boot by more than seven seconds when GPS was
  // absent or still cold-starting.
  this->recovery_baud_index = 1;
  this->listenAtBaud(GPS_BAUD_RATES[this->recovery_baud_index]);
  Serial.println(F("GPS: listening for checksum-valid NMEA without blocking boot"));
}

void GpsInterface::listenAtBaud(uint32_t baud) {
  gpsSerial.end();

  // Arduino HardwareSerial takes ESP RX first and ESP TX second. The board
  // constants are named for the GPS module pins: GPS_TX feeds ESP RX.
  gpsSerial.begin(baud, SERIAL_8N1, GPS_TX, GPS_RX);
  nmea.setBuffer(nmeaBuffer, sizeof(nmeaBuffer));
  this->listening_baud = baud;
  this->last_baud_switch_ms = millis();
}

//passthrough for other objects
void gps_nmea_notimp(MicroNMEA& nmea){
  gps_obj.enqueue(nmea);
}

void GpsInterface::enqueue(MicroNMEA& nmea){
  std::string nmea_sentence = std::string(nmea.getSentence());

  if(nmea_sentence.length()){
    this->notimp_nmea_sentence = nmea_sentence.c_str();

    bool unparsed=1;
    bool enqueue=1;

    char system=nmea.getTalkerID();
    String msg_id=nmea.getMessageID();
    int length=nmea_sentence.length();

    if(length>0&&length<256){
      if(system){
        if(msg_id=="TXT"){
          if(length>8){
            std::string content=nmea_sentence.substr(7,std::string::npos);

            int tot_brk=content.find(',');
            int num_brk=content.find(',',tot_brk+1);
            int txt_brk=content.find(',',num_brk+1);
            int chk_brk=content.rfind('*');

            if(tot_brk!=std::string::npos && num_brk!=std::string::npos && txt_brk!=std::string::npos && chk_brk!=std::string::npos
                && chk_brk>txt_brk && txt_brk>num_brk && num_brk>tot_brk && tot_brk>=0){
              std::string type_str=content.substr(num_brk+1,txt_brk-num_brk-1);
              std::string text_str=content.substr(txt_brk+1,chk_brk-txt_brk-1);
              std::string checksum=content.substr(chk_brk+1,std::string::npos);

              int type=0;
              if(type_str.length()) type=atoi(type_str.c_str());

              if(text_str.length() && checksum.length()){
                String text=text_str.c_str();
                if(type>1){
                  char type_cstr[16];
                  snprintf(type_cstr, sizeof(type_cstr), "%02d ", type);
                  text=type_cstr+text;
                }

                if(this->queue_enabled_flag){
                  if(!this->text)
                    this->text=new LinkedList<String>;
                  #ifdef GPS_TEXT_MAXLINES
                    while(this->text->size()>=GPS_TEXT_MAXLINES)
                  #else
                    while(this->text->size()>=5)
                  #endif
                      this->text->shift();
                  this->text->add(text);
                  this->text_cycles = 1;
                }
                this->gps_text=text;

                if(this->gps_text=="") this->gps_text=text;
                unparsed=0;
              }
            }
          }
        }
      }
    }

    if(unparsed)
      this->notparsed_nmea_sentence = nmea_sentence.c_str();

    if(this->queue_enabled_flag){
      if(enqueue){
        nmea_sentence_t line = { unparsed, msg_id, nmea_sentence.c_str() };

        if(this->queue){
          #ifdef GPS_NMEA_MAXQUEUE
            if(this->queue->size()>=GPS_NMEA_MAXQUEUE)
          #else
            if(this->queue->size()>=30)
          #endif
              this->queue->shift();
        }
        else
           this->new_queue();

        this->queue->add(line);
      }
      else
        if(!this->queue)
          this->new_queue();
    }
    else
      this->flush_queue();
  }
  else
    if(!this->queue_enabled_flag)
      this->flush_queue();
}

void GpsInterface::enable_queue(){
  if(!this->queue)
    this->new_queue();
  if(!this->text)
    this->text=new LinkedList<String>;
  if(!this->text_in)
    this->text_in=new LinkedList<String>;
  this->flush_queue();
  this->queue_enabled_flag=1;
}

void GpsInterface::disable_queue(){
  this->queue_enabled_flag=0;
  this->flush_queue();
}

bool GpsInterface::queue_enabled(){
  return this->queue_enabled_flag;
}

LinkedList<nmea_sentence_t>* GpsInterface::get_queue(){
  return this->queue;
}

void GpsInterface::new_queue(){
  if(!this->queue)
    this->queue=new LinkedList<nmea_sentence_t>;
}

void GpsInterface::flush_queue(){
  this->flush_queue_nmea();
  this->flush_text();
}

void GpsInterface::flush_queue_nmea(){
  if(this->queue)
    this->queue->clear();
  else
    this->new_queue();
}

void GpsInterface::flush_text(){
  this->flush_queue_text();
  this->flush_queue_textin();
}

void GpsInterface::flush_queue_text(){
  this->text_cycles=0;

  if(this->text){
    this->text->clear();
  }
  else
    this->text=new LinkedList<String>;
}

void GpsInterface::flush_queue_textin(){
  if(this->text_in){
    this->text_in->clear();
  }
  else
    this->text_in=new LinkedList<String>;
}

void GpsInterface::sendSentence(const char* sentence){
  MicroNMEA::sendSentence(gpsSerial, sentence);
}

void GpsInterface::sendSentence(Stream &s, const char* sentence){
  MicroNMEA::sendSentence(s, sentence);
}

void GpsInterface::setType(String t){
  if(t == "native")
    this->type_flag=GPSTYPE_NATIVE;
  else if(t == "gps")
    this->type_flag=GPSTYPE_GPS;
  else if(t == "glonass")
    this->type_flag=GPSTYPE_GLONASS;
  else if(t == "galileo")
    this->type_flag=GPSTYPE_GALILEO;
  else if(t == "navic")
    this->type_flag=GPSTYPE_NAVIC;
  else if(t == "qzss")
    this->type_flag=GPSTYPE_QZSS;        
  else if(t == "beidou")
    this->type_flag=GPSTYPE_BEIDOU;
  else if(t == "beidou_bd")
    this->type_flag=GPSTYPE_BEIDOU_BD;    
  else
    this->type_flag=GPSTYPE_ALL;
}

String GpsInterface::generateGXgga(){
  String msg_type="$"+this->generateType()+"GGA,";

  char timeStr[16];
  snprintf(timeStr, sizeof(timeStr), "%02u%02u%02u,",
           nmea.getHour(), nmea.getMinute(), nmea.getSecond());

  long lat = nmea.getLatitude();
  char latDir = lat < 0 ? 'S' : 'N';
  lat = abs(lat);
  char latStr[32];
  snprintf(latStr, sizeof(latStr), "%02ld%08.5f,", lat / 1000000, ((lat % 1000000)*60) / 1000000.0);

  long lon = nmea.getLongitude();
  char lonDir = lon < 0 ? 'W' : 'E';
  lon = abs(lon);
  char lonStr[32];
  snprintf(lonStr, sizeof(lonStr), "%03ld%08.5f,", lon / 1000000, ((lon % 1000000)*60) / 1000000.0);

  int fixQuality = nmea.isValid() ? 1 : 0;
  char fixStr[8];
  snprintf(fixStr, sizeof(fixStr), "%01d,", fixQuality);

  int numSatellites = nmea.getNumSatellites();
  char satStr[8];
  snprintf(satStr, sizeof(satStr), "%02d,", numSatellites);

  unsigned long hdop = nmea.getHDOP();
  char hdopStr[24];
  snprintf(hdopStr, sizeof(hdopStr), "%01.2f,", 2.5 * (((float)(hdop))/10));

  long altitude;
  if(!nmea.getAltitude(altitude)) altitude=0;
  char altStr[24];
  snprintf(altStr, sizeof(altStr), "%01.1f,", altitude/1000.0);

  String message = msg_type + timeStr + latStr + latDir + ',' + lonStr + lonDir +
                    ',' + fixStr + satStr + hdopStr + altStr + "M,,M,,";

  return message;
}

String GpsInterface::generateGXrmc(){
  String msg_type="$"+this->generateType()+"RMC,";

  char timeStr[16];
  snprintf(timeStr, sizeof(timeStr), "%02u%02u%02u,",
           nmea.getHour(), nmea.getMinute(), nmea.getSecond());

  char dateStr[16];
  snprintf(dateStr, sizeof(dateStr), "%02u%02u%02u,",
           nmea.getDay(), nmea.getMonth(),
           static_cast<unsigned int>(nmea.getYear() % 100));

  char status = nmea.isValid() ? 'A' : 'V';
  char mode = nmea.isValid() ? 'A' : 'N';

  long lat = nmea.getLatitude();
  char latDir = lat < 0 ? 'S' : 'N';
  lat = abs(lat);
  char latStr[32];
  snprintf(latStr, sizeof(latStr), "%02ld%08.5f,", lat / 1000000, ((lat % 1000000)*60) / 1000000.0);

  long lon = nmea.getLongitude();
  char lonDir = lon < 0 ? 'W' : 'E';
  lon = abs(lon);
  char lonStr[32];
  snprintf(lonStr, sizeof(lonStr), "%03ld%08.5f,", lon / 1000000, ((lon % 1000000)*60) / 1000000.0);

  char speedStr[24];
  snprintf(speedStr, sizeof(speedStr), "%01.1f,", nmea.getSpeed() / 1000.0);

  char courseStr[24];
  snprintf(courseStr, sizeof(courseStr), "%01.1f,", nmea.getCourse() / 1000.0);

  String message = msg_type + timeStr + status + ',' + latStr + latDir + ',' +
                    lonStr + lonDir + ',' + speedStr + courseStr + dateStr + ',' + ',' + mode;
  return message;
}

String GpsInterface::generateType(){
  String msg_type="";

  if(this->type_flag<8) //8=BeiDou in BD mode
    msg_type+='G';

  if(this->type_flag == GPSTYPE_NATIVE){ //type_flag=0
    char system=this->nav_system;
    if(system)
      msg_type+=system;
    else
      msg_type+='N';
  }
  else if(this->type_flag == GPSTYPE_GPS) //type_flag=2
    msg_type+='P';
  else if(this->type_flag == GPSTYPE_GLONASS) //type_flag=3
    msg_type+='L';
  else if(this->type_flag == GPSTYPE_GALILEO) //type_flag=4
    msg_type+='A';
  else if(this->type_flag == GPSTYPE_NAVIC) //type_flag=5
    msg_type+='I';
  else if(this->type_flag == GPSTYPE_QZSS) //type_flag=6
    msg_type+='Q';
  else if(this->type_flag == GPSTYPE_BEIDOU) //type_flag=7
    msg_type+='B';
  else if(this->type_flag == GPSTYPE_BEIDOU_BD){ //type_flag=8
    msg_type+='B';
    msg_type+='D';
  }
  else{ //type_flag=1=all ... also default if unset/wrong (obj default is type_flag=0=native)
    if(this->type_flag>=8) //catch uncaught first char, assume G if not already output
      msg_type+='G';
    msg_type+='N';
  }

  return msg_type;
}

// Thanks JosephHewitt
String GpsInterface::dt_string_from_gps(){
  //Return a datetime String using GPS data only.
  String datetime = "";
  if (nmea.isValid() && nmea.getYear() > 0){
    datetime += nmea.getYear();

    datetime += "-";

    uint8_t month = nmea.getMonth();
    if (month < 10)
      datetime += "0";
    datetime += month;

    datetime += "-";

    uint8_t day = nmea.getDay();
    if (day < 10)
      datetime += "0";
    datetime += day;

    datetime += " ";

    uint8_t hour = nmea.getHour();
    if (hour < 10)
      datetime += "0";
    datetime += hour;

    datetime += ":";

    uint8_t minute = nmea.getMinute();
    if (minute < 10)
      datetime += "0";
    datetime += minute;

    datetime += ":";
    
    uint8_t seconds = nmea.getSecond();
    if (seconds < 10)
      datetime += "0";
    datetime += seconds;
  }
  return datetime;
}

void GpsInterface::setGPSInfo() {
  String nmea_sentence = String(nmea.getSentence());
  if(nmea_sentence != "") this->nmea_sentence = nmea_sentence;

  this->good_fix = nmea.isValid();
  this->nav_system = nmea.getNavSystem();
  this->num_sats = nmea.getNumSatellites();

  this->datetime = this->dt_string_from_gps();

  if (nmea.isValid() && nmea.getYear() >= 2020) {
    const marauder::clock::UtcDateTime gps_time = {
        static_cast<uint16_t>(nmea.getYear()),
        static_cast<uint8_t>(nmea.getMonth()),
        static_cast<uint8_t>(nmea.getDay()),
        static_cast<uint8_t>(nmea.getHour()),
        static_cast<uint8_t>(nmea.getMinute()),
        static_cast<uint8_t>(nmea.getSecond()),
    };
    device_clock_obj.syncFromGps(gps_time);
  }

  const uint8_t hdop = nmea.getHDOP();
  this->accuracy = hdop == 255 ? 0.0f : 2.5f * (static_cast<float>(hdop) / 10.0f);

  // Invalid GGA/RMC sentences legitimately report the acquisition state and
  // satellite count, but their empty coordinates decode to MicroNMEA's
  // sentinel value. Preserve the last real position until a new fix arrives.
  if (!this->good_fix)
    return;

  this->lat_int = nmea.getLatitude();
  this->lon_int = nmea.getLongitude();

  this->lat = String((float)nmea.getLatitude()/1000000, 7);
  this->lon = String((float)nmea.getLongitude()/1000000, 7);
  long alt = 0;
  if (!nmea.getAltitude(alt)){
    alt = 0;
  }
  this->altf = (float)alt / 1000;

  //nmea.clear();
}

void GpsInterface::handleCompletedSentence() {
  const char* sentence = nmea.getSentence();
  if (sentence == nullptr || sentence[0] != '$' ||
      !MicroNMEA::testChecksum(sentence)) {
    return;
  }

  const uint32_t now = millis();
  this->gps_enabled = true;
  this->last_sentence_ms = now;
  this->gps_baud = this->listening_baud;

  const char* message = nmea.getMessageID();
  if (message != nullptr &&
      (strcmp(message, "GGA") == 0 || strcmp(message, "RMC") == 0)) {
    this->last_fix_sentence_ms = now;
    this->setGPSInfo();
  }
}

float GpsInterface::getAccuracy() {
  return this->accuracy;
}

String GpsInterface::getLat() {
  return this->lat;
}

String GpsInterface::getLon() {
  return this->lon;
}

int32_t GpsInterface::getLatInt() {
  return this->lat_int;
}

int32_t GpsInterface::getLonInt() {
  return this->lon_int;
}

float GpsInterface::getAlt() {
  return this->altf;
}

String GpsInterface::getDatetime() {
  return this->datetime;
}

String GpsInterface::getNumSatsString() {
  return (String)num_sats;
}

int GpsInterface::getNumSats() {
  return num_sats;
}

bool GpsInterface::getFixStatus() {
  return this->good_fix;
}

String GpsInterface::getFixStatusAsString() {
  if (this->getFixStatus())
    return "Yes";
  else
    return "No";
}

bool GpsInterface::getGpsModuleStatus() {
  return this->gps_enabled;
}

uint32_t GpsInterface::getBaudRate() {
  return this->gps_baud;
}

uint32_t GpsInterface::getLastSentenceAgeMs() {
  if (this->last_sentence_ms == 0)
    return UINT32_MAX;
  return millis() - this->last_sentence_ms;
}

String GpsInterface::getText() {
  return this->gps_text;
}

int GpsInterface::getTextQueueSize() {
  if(this->queue_enabled_flag){
    bool exists=0;
    if(this->text){
      int size=this->text->size();
      if(size) return size;
      exists=1;
    }
    if(this->text_in){
      int size=this->text_in->size();
      if(size) return size;
      exists=1;
    }
    if(exists)
      return 0;
    else
      return -2;
  }
  else
    return -1;
}

String GpsInterface::getTextQueue(bool flush) {
  if(!this->queue_enabled_flag)
    return this->gps_text;

  if(!this->text)
    this->text=new LinkedList<String>;

  String result;
  for(int i=0;i<this->text->size();i++){
    const String line=this->text->get(i);
    if(line.length()==0)
      continue;
    if(result.length()>0)
      result+="\r\n";
    result+=line;
  }

  if(flush){
    this->text->clear();
    this->text_cycles=0;
  }
  return result.length()>0 ? result : this->gps_text;
}

String GpsInterface::getNmea() {
  return this->nmea_sentence;
}

String GpsInterface::getNmeaNotimp() {
  return this->notimp_nmea_sentence;
}

String GpsInterface::getNmeaNotparsed() {
  return this->notparsed_nmea_sentence;
}

void GpsInterface::main() {
  while (gpsSerial.available()) {
    const char c = gpsSerial.read();
    if (nmea.process(c))
      this->handleCompletedSentence();
  }

  const uint32_t now = millis();
  if (this->gps_enabled && this->last_sentence_ms != 0 &&
      now - this->last_sentence_ms > GPS_TRAFFIC_TIMEOUT_MS) {
    this->gps_enabled = false;
    this->good_fix = false;
    this->num_sats = 0;
    this->gps_baud = 0;
    this->last_baud_switch_ms = now;
  }
  if (this->good_fix && this->last_fix_sentence_ms != 0 &&
      now - this->last_fix_sentence_ms > GPS_FIX_TIMEOUT_MS) {
    this->good_fix = false;
  }

  if (!this->gps_enabled &&
      now - this->last_baud_switch_ms >= RECOVERY_BAUD_DWELL_MS) {
    this->recovery_baud_index =
        (this->recovery_baud_index + 1) % GPS_BAUD_RATE_COUNT;
    this->listenAtBaud(GPS_BAUD_RATES[this->recovery_baud_index]);
  }
}
#endif
