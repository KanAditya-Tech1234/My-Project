// =====================================================================
// Drone camera + GPS tracker
// Board : Waveshare ESP32-S3-SIM7670G-4G (LTE Cat-1, GNSS, camera port)
//
// Every interval it:
//   1. reads the GNSS position from the SIM7670G,
//   2. takes a JPEG photo with the camera (OV2640 / OV5640),
//   3. POSTs the photo to  <SERVER_BASE>/api/photo?trackerId=..&lat=..&lon=..
//   4. reads "intervalMs" from the server's reply and uses it as the delay.
//
// The interval is measured START-TO-START: if the website is set to 10 s,
// a new upload begins every 10 s no matter how long the previous upload
// took (as long as the upload itself fits inside those 10 s).
//
// Arduino IDE settings (ESP32 core 3.x by Espressif):
//   Board            : ESP32S3 Dev Module
//   USB CDC On Boot  : Enabled           (so Serial Monitor works over USB-C)
//   PSRAM            : QSPI PSRAM        (try "OPI PSRAM" if camera init fails)
//   Flash Size       : 16MB
//   Partition Scheme : 16M Flash (3MB APP/9.9MB FATFS)  (any 3MB+ app works)
//
// Board DIP switches (back side): CAM = ON, 4G = ON.
// =====================================================================

#include <Arduino.h>
#include "esp_camera.h"

// ================= USER CONFIG =================

const char* TRACKER_ID  = "CAM-01";

// Same Render service the EC200U tracker already uses. No trailing slash.
const char* SERVER_BASE = "https://tracker-drone.onrender.com";

const char* APN = "airtelgprs.com";      // change for Jio/Vi/BSNL SIMs

// Waveshare shipped two board revisions with different camera pins.
// Set to 1 or 2 (check the silkscreen / your purchase; V2 is newer).
#define BOARD_VERSION 2

// Fallback delay until the server tells us otherwise.
const unsigned long DEFAULT_INTERVAL_MS = 10000;

// Must match server.js's MIN/MAX_INTERVAL_MS.
const unsigned long MIN_INTERVAL_MS = 1000;
const unsigned long MAX_INTERVAL_MS = 300000;

// Photo size/quality. VGA (640x480) at quality 12 is usually 25–60 KB,
// which uploads in a few seconds over LTE Cat-1. Bigger frames = slower.
const framesize_t FRAME_SIZE   = FRAMESIZE_VGA;
const int         JPEG_QUALITY = 12;          // 0–63, lower = better quality, bigger file
const size_t      MAX_UPLOAD_BYTES = 150000;  // keep under the modem's HTTPDATA limit

// While testing indoors there is no GNSS fix. Set true to still put the
// device on the map using a fixed test position (photos upload either way).
const bool   SIMULATE_GPS_IF_NO_FIX = false;
const double TEST_LAT = 23.057984;
const double TEST_LON = 72.521018;

// ================= MODEM PINS =================

#define MODEM_RX_PIN   17     // ESP32-S3 RX  <- SIM7670G TX
#define MODEM_TX_PIN   18     // ESP32-S3 TX  -> SIM7670G RX
#define MODEM_BAUD     115200 // SIM7670G factory default
// UART speed used after start-up. At 115200 a 50 KB photo takes ~4.5 s
// just to cross the wire to the modem; at 921600 it takes ~0.6 s.
// Set to 0 to stay at MODEM_BAUD.
#define MODEM_FAST_BAUD 921600

HardwareSerial modem(1);
unsigned long modemBaud = MODEM_BAUD;

// ================= CAMERA PINS =================

#define PWDN_GPIO_NUM   -1
#define RESET_GPIO_NUM  -1
#define SIOD_GPIO_NUM   15
#define SIOC_GPIO_NUM   16
#define Y9_GPIO_NUM     14
#define Y8_GPIO_NUM     13
#define Y7_GPIO_NUM     12
#define Y6_GPIO_NUM     11
#define Y5_GPIO_NUM     10
#define Y4_GPIO_NUM      9
#define Y3_GPIO_NUM      8
#define Y2_GPIO_NUM      7
#if BOARD_VERSION == 1
  #define XCLK_GPIO_NUM  34
  #define VSYNC_GPIO_NUM 36
  #define HREF_GPIO_NUM  35
  #define PCLK_GPIO_NUM  37
#else
  #define XCLK_GPIO_NUM  39
  #define VSYNC_GPIO_NUM 42
  #define HREF_GPIO_NUM  41
  #define PCLK_GPIO_NUM  46
#endif

// ================= STATE =================

// Delay actually used by loop(). Overwritten by the server's ack, so a
// change on the website reaches the drone after its next upload.
unsigned long currentIntervalMs = DEFAULT_INTERVAL_MS;

bool cameraReady = false;
int consecutiveFailures = 0;

struct GnssFix {
  bool   valid = false;
  double lat = 0;
  double lon = 0;
  double alt = 0;
  String utcDate;   // ddmmyy
  String utcTime;   // hhmmss(.s)
};

// =======================================================
// AT COMMAND HELPERS
// =======================================================

void flushModem() {
  delay(20);
  while (modem.available()) modem.read();
}

// Reads from the modem until `expected` (or "ERROR") appears or timeout.
String waitFor(const char* expected, unsigned long timeout) {
  String resp;
  unsigned long start = millis();
  while (millis() - start < timeout) {
    while (modem.available()) resp += (char)modem.read();
    if (resp.indexOf(expected) != -1 || resp.indexOf("ERROR") != -1) break;
    delay(1);
  }
  if (resp.length()) {
    Serial.print("<< ");
    Serial.println(resp);
  }
  return resp;
}

String sendATraw(const String& cmd, const char* expected = "OK", unsigned long timeout = 5000) {
  flushModem();
  modem.print(cmd);
  modem.print("\r\n");
  Serial.print(">> ");
  Serial.println(cmd);
  return waitFor(expected, timeout);
}

bool sendAT(const String& cmd, const char* expected = "OK", unsigned long timeout = 5000) {
  return sendATraw(cmd, expected, timeout).indexOf(expected) != -1;
}

// Waits up to `timeout` for the modem to answer a plain "AT".
bool modemResponds(unsigned long timeout) {
  unsigned long start = millis();
  while (millis() - start < timeout) {
    flushModem();
    modem.print("AT\r\n");
    String r = waitFor("OK", 500);
    if (r.indexOf("OK") != -1) return true;
  }
  return false;
}

// =======================================================
// TIME (IST string from GNSS UTC, or from the modem clock)
// =======================================================

// Howard Hinnant's civil-date algorithms — tiny and handle leap years.
long daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const long era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (long)doe - 719468;
}

void civilFromDays(long z, int& y, unsigned& m, unsigned& d) {
  z += 719468;
  const long era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  y = (int)yoe + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y += (m <= 2);
}

// Shifts a date/time by `offsetMin` minutes and formats it.
String formatShifted(int y, int mo, int d, int h, int mi, int s, long offsetMin) {
  long totalMin = daysFromCivil(y, mo, d) * 1440L + h * 60L + mi + offsetMin;
  long days = totalMin >= 0 ? totalMin / 1440 : (totalMin - 1439) / 1440;
  long rem = totalMin - days * 1440;
  int yy; unsigned mm, dd;
  civilFromDays(days, yy, mm, dd);
  char out[25];
  snprintf(out, sizeof(out), "%04d-%02u-%02u %02ld:%02ld:%02d",
           yy, mm, dd, rem / 60, rem % 60, s);
  return String(out);
}

const long IST_OFFSET_MIN = 330;

String istFromGnss(const GnssFix& fix) {
  if (fix.utcDate.length() < 6 || fix.utcTime.length() < 6) return "";
  int d  = fix.utcDate.substring(0, 2).toInt();
  int mo = fix.utcDate.substring(2, 4).toInt();
  int y  = fix.utcDate.substring(4, 6).toInt() + 2000;
  int h  = fix.utcTime.substring(0, 2).toInt();
  int mi = fix.utcTime.substring(2, 4).toInt();
  int s  = fix.utcTime.substring(4, 6).toInt();
  return formatShifted(y, mo, d, h, mi, s, IST_OFFSET_MIN);
}

// +CCLK: "yy/MM/dd,hh:mm:ss±zz"  (zz = offset in quarter hours).
// Works whether the modem reports UTC (zz = 00) or local time.
String istFromModemClock() {
  String raw = sendATraw("AT+CCLK?", "OK", 2000);
  int q1 = raw.indexOf('"');
  int q2 = raw.indexOf('"', q1 + 1);
  if (q1 < 0 || q2 < q1 + 17) return "";
  String t = raw.substring(q1 + 1, q2);
  int y  = t.substring(0, 2).toInt() + 2000;
  if (y < 2024) return "";   // clock not synced from the network yet
  int mo = t.substring(3, 5).toInt();
  int d  = t.substring(6, 8).toInt();
  int h  = t.substring(9, 11).toInt();
  int mi = t.substring(12, 14).toInt();
  int s  = t.substring(15, 17).toInt();
  long zoneMin = 0;
  if (t.length() >= 20) {
    zoneMin = t.substring(18).toInt() * 15L;
    if (t[17] == '-') zoneMin = -zoneMin;
  }
  return formatShifted(y, mo, d, h, mi, s, IST_OFFSET_MIN - zoneMin);
}

// =======================================================
// GNSS
// =======================================================

// Accepts both decimal degrees and NMEA ddmm.mmmm — SIM7670G firmware
// versions differ in which one +CGNSSINFO reports.
double toDegrees(const String& s, bool isLat) {
  double v = s.toDouble();
  if ((isLat && v > 90.0) || (!isLat && v > 180.0)) {
    int deg = (int)(v / 100);
    v = deg + (v - deg * 100) / 60.0;
  }
  return v;
}

GnssFix readGnss() {
  GnssFix fix;
  String r = sendATraw("AT+CGNSSINFO", "OK", 3000);
  int p = r.indexOf("+CGNSSINFO:");
  if (p == -1) return fix;
  int eol = r.indexOf('\n', p);
  String line = r.substring(p + 11, eol == -1 ? r.length() : eol);
  line.trim();

  // Split on commas (keeping empty fields).
  String tok[24];
  int n = 0, start = 0;
  for (int i = 0; i <= (int)line.length() && n < 24; i++) {
    if (i == (int)line.length() || line[i] == ',') {
      tok[n] = line.substring(start, i);
      tok[n].trim();
      n++;
      start = i + 1;
    }
  }

  // Latitude is the field just before "N"/"S"; the rest follow it.
  for (int i = 1; i + 5 < n; i++) {
    if ((tok[i] == "N" || tok[i] == "S") && (tok[i + 2] == "E" || tok[i + 2] == "W")) {
      if (tok[i - 1].length() == 0 || tok[i + 1].length() == 0) return fix;
      fix.lat = toDegrees(tok[i - 1], true);
      fix.lon = toDegrees(tok[i + 1], false);
      if (tok[i] == "S") fix.lat = -fix.lat;
      if (tok[i + 2] == "W") fix.lon = -fix.lon;
      fix.utcDate = tok[i + 3];
      fix.utcTime = tok[i + 4];
      fix.alt = tok[i + 5].toDouble();
      fix.valid = !(fix.lat == 0 && fix.lon == 0);
      return fix;
    }
  }
  return fix;
}

// =======================================================
// CAMERA
// =======================================================

bool initCamera() {
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer   = LEDC_TIMER_0;
  c.pin_d0 = Y2_GPIO_NUM;
  c.pin_d1 = Y3_GPIO_NUM;
  c.pin_d2 = Y4_GPIO_NUM;
  c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM;
  c.pin_d5 = Y7_GPIO_NUM;
  c.pin_d6 = Y8_GPIO_NUM;
  c.pin_d7 = Y9_GPIO_NUM;
  c.pin_xclk  = XCLK_GPIO_NUM;
  c.pin_pclk  = PCLK_GPIO_NUM;
  c.pin_vsync = VSYNC_GPIO_NUM;
  c.pin_href  = HREF_GPIO_NUM;
  c.pin_sccb_sda = SIOD_GPIO_NUM;
  c.pin_sccb_scl = SIOC_GPIO_NUM;
  c.pin_pwdn  = PWDN_GPIO_NUM;
  c.pin_reset = RESET_GPIO_NUM;
  c.xclk_freq_hz = 20000000;
  c.pixel_format = PIXFORMAT_JPEG;
  c.frame_size   = FRAME_SIZE;
  c.jpeg_quality = JPEG_QUALITY;

  if (psramFound()) {
    c.fb_location = CAMERA_FB_IN_PSRAM;
    c.fb_count    = 2;
    c.grab_mode   = CAMERA_GRAB_LATEST;   // always hand us the newest frame
  } else {
    Serial.println("WARNING: no PSRAM found - check the PSRAM setting in Tools menu");
    c.fb_location = CAMERA_FB_IN_DRAM;
    c.fb_count    = 1;
    c.grab_mode   = CAMERA_GRAB_WHEN_EMPTY;
    if (FRAME_SIZE > FRAMESIZE_QVGA) c.frame_size = FRAMESIZE_QVGA;
  }

  esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed (0x%x). Is the CAM DIP switch ON and BOARD_VERSION right?\n", err);
    return false;
  }
  Serial.println("Camera ready");
  return true;
}

// Returns a fresh JPEG frame (caller must esp_camera_fb_return it), or nullptr.
camera_fb_t* capturePhoto() {
  if (!cameraReady) return nullptr;

  // Throw away one frame so we don't upload a picture that was sitting
  // in the buffer since the previous cycle.
  camera_fb_t* fb = esp_camera_fb_get();
  if (fb) esp_camera_fb_return(fb);

  fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Camera capture failed");
    return nullptr;
  }

  if (fb->len > MAX_UPLOAD_BYTES) {
    // Too big for one upload — compress harder for this and later shots.
    Serial.printf("Photo %u bytes > limit, lowering quality\n", fb->len);
    esp_camera_fb_return(fb);
    sensor_t* s = esp_camera_sensor_get();
    if (s) s->set_quality(s, min(63, JPEG_QUALITY + 10));
    fb = esp_camera_fb_get();
    if (fb && fb->len > MAX_UPLOAD_BYTES) {
      esp_camera_fb_return(fb);
      return nullptr;
    }
  }
  return fb;
}

// =======================================================
// SERVER REPLY
// =======================================================

// Looks for "intervalMs":NUMBER in the server's ack body and, if it's a
// sane value, adopts it as the new transmission delay.
void applyIntervalFromResponse(const String& body) {
  int key = body.indexOf("\"intervalMs\"");
  if (key == -1) return;
  int colon = body.indexOf(':', key);
  if (colon == -1) return;
  long value = body.substring(colon + 1).toInt();
  if (value >= (long)MIN_INTERVAL_MS && value <= (long)MAX_INTERVAL_MS) {
    if ((unsigned long)value != currentIntervalMs) {
      Serial.printf("Delay updated by server -> %ld ms\n", value);
    }
    currentIntervalMs = (unsigned long)value;
  }
}

// =======================================================
// HTTP(S) POST via SIM7670G built-in HTTP stack
// =======================================================

String urlEncode(const String& s) {
  String out;
  const char* hex = "0123456789ABCDEF";
  for (size_t i = 0; i < s.length(); i++) {
    char ch = s[i];
    if (isalnum((unsigned char)ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
      out += ch;
    } else {
      out += '%';
      out += hex[(ch >> 4) & 0xF];
      out += hex[ch & 0xF];
    }
  }
  return out;
}

bool httpPost(const String& url, const char* contentType, const uint8_t* data, size_t len) {
  sendAT("AT+HTTPTERM", "OK", 2000);   // clear any half-finished session; error is fine

  if (!sendAT("AT+HTTPINIT", "OK", 5000)) return false;

  bool ok = false;
  do {
    if (!sendAT("AT+HTTPPARA=\"URL\",\"" + url + "\"")) break;
    if (!sendAT(String("AT+HTTPPARA=\"CONTENT\",\"") + contentType + "\"")) break;
    if (url.startsWith("https")) sendAT("AT+HTTPPARA=\"SSLCFG\",0");

    // Hand the body to the modem.
    flushModem();
    modem.printf("AT+HTTPDATA=%u,%u\r\n", (unsigned)len, 30000u);
    if (waitFor("DOWNLOAD", 5000).indexOf("DOWNLOAD") == -1) break;

    size_t sent = 0;
    while (sent < len) {
      size_t chunk = min((size_t)1024, len - sent);
      modem.write(data + sent, chunk);
      sent += chunk;
    }
    modem.flush();
    if (waitFor("OK", 30000).indexOf("OK") == -1) break;

    // Send the request. Render's free tier can take ~50 s to wake up.
    if (!sendAT("AT+HTTPACTION=1")) break;
    String act = waitFor("+HTTPACTION:", 90000);
    int p = act.indexOf("+HTTPACTION:");
    if (p == -1) { Serial.println("No HTTP response (timeout)"); break; }

    // +HTTPACTION: 1,<status>,<length>
    int c1 = act.indexOf(',', p);
    int c2 = act.indexOf(',', c1 + 1);
    int status = act.substring(c1 + 1, c2).toInt();
    long respLen = act.substring(c2 + 1).toInt();
    Serial.printf("HTTP status %d, %ld byte reply\n", status, respLen);
    if (status >= 700) Serial.println("(7xx = modem network/SSL error, not a server error)");

    if (respLen > 0) {
      String body = sendATraw("AT+HTTPREAD=0," + String(min(respLen, 1024L)), "+HTTPREAD: 0", 5000);
      applyIntervalFromResponse(body);
    }
    ok = (status >= 200 && status < 300);
  } while (false);

  sendAT("AT+HTTPTERM", "OK", 2000);
  return ok;
}

// =======================================================
// MODEM START-UP
// =======================================================

void connectData() {
  sendAT(String("AT+CGDCONT=1,\"IP\",\"") + APN + "\"");
  sendAT("AT+CGACT=1,1", "OK", 20000);
  sendATraw("AT+CGPADDR=1", "OK", 3000);
}

void initModem() {
  Serial.println("Waiting for SIM7670G (4G DIP switch must be ON)...");

  modem.setRxBufferSize(4096);
  modem.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
  bool found = modemResponds(30000);

#if MODEM_FAST_BAUD
  if (!found) {
    // A previous run may have left the modem at the fast baud rate.
    modem.updateBaudRate(MODEM_FAST_BAUD);
    found = modemResponds(3000);
    if (found) modemBaud = MODEM_FAST_BAUD;
  }
#endif

  if (!found) {
    Serial.println("ERROR: modem not responding. Check DIP switches, then restarting...");
    delay(5000);
    ESP.restart();
  }

#if MODEM_FAST_BAUD
  if (modemBaud != MODEM_FAST_BAUD) {
    sendAT("AT+IPR=" + String(MODEM_FAST_BAUD));
    delay(100);
    modem.updateBaudRate(MODEM_FAST_BAUD);
    if (modemResponds(3000)) {
      modemBaud = MODEM_FAST_BAUD;
    } else {
      Serial.println("Fast baud rate failed, staying at 115200");
      modem.updateBaudRate(MODEM_BAUD);
      modemResponds(3000);
    }
  }
#endif
  Serial.printf("Modem ready at %lu baud\n", modemBaud);

  sendAT("ATE0");
  sendAT("AT+CMEE=2");

  if (!sendAT("AT+CPIN?", "READY", 10000)) {
    Serial.println("SIM not ready! Check the SIM card. Restarting...");
    delay(5000);
    ESP.restart();
  }

  sendAT("AT+CTZU=1");   // take time from the network

  Serial.println("Waiting for network...");
  unsigned long start = millis();
  while (true) {
    String r = sendATraw("AT+CEREG?", "OK", 3000);
    String r2 = sendATraw("AT+CREG?", "OK", 3000);
    if (r.indexOf(",1") != -1 || r.indexOf(",5") != -1 ||
        r2.indexOf(",1") != -1 || r2.indexOf(",5") != -1) break;
    if (millis() - start > 180000) {
      Serial.println("No network after 3 minutes. Restarting...");
      ESP.restart();
    }
    delay(2000);
  }
  Serial.println("Network registered");

  connectData();

  // SSL context 0: TLS 1.2+, no certificate check, SNI on (Render needs SNI).
  sendAT("AT+CSSLCFG=\"sslversion\",0,4");
  sendAT("AT+CSSLCFG=\"authmode\",0,0");
  sendAT("AT+CSSLCFG=\"enableSNI\",0,1");

  // GNSS on. A cold start outdoors can take 30 s – a few minutes to get a fix.
  sendAT("AT+CGNSSPWR=1", "OK", 5000);
  waitFor("READY", 15000);

  Serial.println("Modem initialised");
}

// =======================================================
// SETUP / LOOP
// =======================================================

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\nDrone camera tracker (ESP32-S3 SIM7670G) starting");

  cameraReady = initCamera();
  initModem();
}

void loop() {
  unsigned long cycleStart = millis();

  GnssFix fix = readGnss();
  if (!fix.valid && SIMULATE_GPS_IF_NO_FIX) {
    fix.valid = true;
    fix.lat = TEST_LAT;
    fix.lon = TEST_LON;
  }

  String ts = istFromGnss(fix);
  if (ts.length() == 0) ts = istFromModemClock();

  char la[16] = "", lo[16] = "";
  if (fix.valid) {
    snprintf(la, sizeof(la), "%.6f", fix.lat);
    snprintf(lo, sizeof(lo), "%.6f", fix.lon);
  }

  Serial.println("--------------------------------");
  Serial.printf("GNSS : %s\n", fix.valid ? (String(la) + ", " + lo).c_str() : "no fix yet");
  Serial.printf("Time : %s\n", ts.c_str());

  bool ok = false;
  camera_fb_t* fb = capturePhoto();

  if (fb) {
    Serial.printf("Photo: %u bytes\n", fb->len);
    String url = String(SERVER_BASE) + "/api/photo?trackerId=" + urlEncode(TRACKER_ID);
    if (fix.valid) {
      url += "&lat=" + String(la) + "&lon=" + String(lo) + "&alt=" + String((int)fix.alt);
    }
    if (ts.length()) url += "&ts=" + urlEncode(ts);
    ok = httpPost(url, "image/jpeg", fb->buf, fb->len);
    esp_camera_fb_return(fb);
  } else if (fix.valid) {
    // No camera frame — still report the position using the same JSON
    // format as the EC200U tracker.
    String json = String("{\"TrackerId\":\"") + TRACKER_ID +
                  "\",\"Latitude\":\"" + la + "\",\"Longitude\":\"" + lo +
                  "\",\"Altitude\":\"" + String((int)fix.alt) + "m\",\"Timestamp\":\"" + ts +
                  "\",\"Maplink\":\"https://maps.google.com/?q=" + la + "," + lo + "\"}";
    ok = httpPost(String(SERVER_BASE) + "/", "application/json",
                  (const uint8_t*)json.c_str(), json.length());
  } else {
    Serial.println("Nothing to send (no photo and no GPS fix)");
    ok = true;
  }

  Serial.println(ok ? "Upload successful" : "Upload failed");

  if (ok) {
    consecutiveFailures = 0;
  } else if (++consecutiveFailures == 3) {
    Serial.println("3 failures in a row - re-activating mobile data");
    connectData();
  } else if (consecutiveFailures >= 6) {
    Serial.println("6 failures in a row - restarting");
    ESP.restart();
  }

  // Start-to-start timing: wait only for what's left of the interval.
  unsigned long elapsed = millis() - cycleStart;
  if (elapsed < currentIntervalMs) {
    delay(currentIntervalMs - elapsed);
  } else {
    Serial.printf("NOTE: this cycle took %lu ms, longer than the %lu ms interval - "
                  "sending again immediately. Choose a longer delay or a smaller FRAME_SIZE.\n",
                  elapsed, currentIntervalMs);
  }
}
