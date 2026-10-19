#include <Arduino.h>

// ================= USER CONFIG =================

const char* TRACKER_ID = "TRK-05";

const double START_LAT = 23.05798405211327;
const double START_LON = 72.52101804006736;

const double END_LAT = 23.2233;
const double END_LON = 72.6477;

const double ALTITUDE = 15.0;

const int TOTAL_STEPS = 30;
const unsigned long TX_INTERVAL_MS = 5000;   // fallback/default only — see currentIntervalMs

// Safety bounds so a bad/garbled server reply can never freeze the loop
// or make it hammer the server. Must match server.js's MIN/MAX_INTERVAL_MS.
const unsigned long MIN_INTERVAL_MS = 1000;
const unsigned long MAX_INTERVAL_MS = 300000;

const char* SERVER_URL = "https://tracker-drone.onrender.com/";
const char* APN = "airtelgprs.com";

// ================= MODEM PINS =================

#define EC200U_RX   18
#define EC200U_TX   17
#define PW_KEY      10
#define EC200U_BAUD 115200

HardwareSerial modemSerial(2);

int currentStep = 0;

// This is the delay actually used by loop(). It starts at the hardcoded
// default and gets overwritten by whatever the server's ack reports, so
// changing the delay on the website (or in config.json) takes effect on
// the very next transmission — no reflash, no reboot.
unsigned long currentIntervalMs = TX_INTERVAL_MS;

// =======================================================
// AT COMMAND HELPERS
// =======================================================

void flushModem() {
  delay(100);
  while (modemSerial.available())
    modemSerial.read();
}

String sendATraw(const char* cmd, unsigned long timeout = 5000) {

  flushModem();

  modemSerial.println(cmd);

  Serial.print(">> ");
  Serial.println(cmd);

  String resp = "";

  unsigned long start = millis();

  while (millis() - start < timeout) {

    while (modemSerial.available())
      resp += (char)modemSerial.read();

    if (resp.indexOf("OK") != -1 ||
        resp.indexOf("ERROR") != -1)
      break;
  }

  if (resp.length()) {
    Serial.print("<< ");
    Serial.println(resp);
  }

  return resp;
}

bool sendAT(const char* cmd,
            const char* expected = "OK",
            unsigned long timeout = 5000) {

  return sendATraw(cmd, timeout).indexOf(expected) != -1;
}

String waitFor(const char* expected,
               unsigned long timeout = 15000) {

  String resp = "";

  unsigned long start = millis();

  while (millis() - start < timeout) {

    while (modemSerial.available())
      resp += (char)modemSerial.read();

    if (resp.indexOf(expected) != -1)
      break;
  }

  if (resp.length()) {
    Serial.print("<< ");
    Serial.println(resp);
  }

  return resp;
}

// =======================================================
// GET MODEM TIME IN IST
// =======================================================

String getISTTimestamp() {

  String raw = sendATraw("AT+CCLK?", 3000);

  int q1 = raw.indexOf('"');
  int q2 = raw.lastIndexOf('"');

  if (q1 < 0 || q2 <= q1)
    return "1970-01-01 00:00:00";

  String ts = raw.substring(q1 + 1, q2);

  int year = ts.substring(0,2).toInt() + 2000;
  int month = ts.substring(3,5).toInt();
  int day = ts.substring(6,8).toInt();

  int hour = ts.substring(9,11).toInt();
  int minute = ts.substring(12,14).toInt();
  int second = ts.substring(15,17).toInt();

  // Convert UTC → IST

  minute += 30;

  if (minute >= 60) {
    minute -= 60;
    hour++;
  }

  hour += 5;

  if (hour >= 24) {
    hour -= 24;
    day++;
  }

  int daysInMonth[] =
  {31,28,31,30,31,30,31,31,30,31,30,31};

  if ((year % 4 == 0 && year % 100 != 0) ||
      (year % 400 == 0))
    daysInMonth[1] = 29;

  if (day > daysInMonth[month-1]) {

    day = 1;
    month++;

    if (month > 12) {
      month = 1;
      year++;
    }
  }

  char out[25];

  snprintf(out,
           sizeof(out),
           "%04d-%02d-%02d %02d:%02d:%02d",
           year,
           month,
           day,
           hour,
           minute,
           second);

  return String(out);
}

// =======================================================
// JSON PAYLOAD
// =======================================================

double interpolate(double a,
                   double b,
                   int step,
                   int total) {

  return a + (b - a) * step / (total - 1);
}

String buildJSON(double lat,
                 double lon,
                 double alt,
                 const String& ts) {

  char la[20];
  char lo[20];

  snprintf(la,sizeof(la),"%.6f",lat);
  snprintf(lo,sizeof(lo),"%.6f",lon);

  return "{"
         "\"TrackerId\":\"" + String(TRACKER_ID) + "\","
         "\"Latitude\":\"" + String(la) + "\","
         "\"Longitude\":\"" + String(lo) + "\","
         "\"Altitude\":\"" + String((int)alt) + "m\","
         "\"Timestamp\":\"" + ts + "\","
         "\"Maplink\":\"https://maps.google.com/?q="
         + String(la) + "," + String(lo) + "\""
         "}";
}

// =======================================================
// SSL CONFIGURATION
// =======================================================

void configureSSL() {

  Serial.println("Configuring SSL...");

  sendAT("AT+QSSLCFG=\"sslversion\",1,4");
  sendAT("AT+QSSLCFG=\"ciphersuite\",1,0XFFFF");
  sendAT("AT+QSSLCFG=\"seclevel\",1,0");
  sendAT("AT+QSSLCFG=\"ignorelocaltime\",1,1");
  sendAT("AT+QSSLCFG=\"negotiatetime\",1,60");
  sendAT("AT+QSSLCFG=\"sni\",1,1");

  sendAT("AT+QHTTPCFG=\"sslctxid\",1");
  sendAT("AT+QHTTPCFG=\"contextid\",1");
  sendAT("AT+QHTTPCFG=\"requestheader\",0");
  sendAT("AT+QHTTPCFG=\"responseheader\",0");
  sendAT("AT+QHTTPCFG=\"contenttype\",4");
}

// =======================================================
// HTTPS POST
// =======================================================

// Looks for "intervalMs":NUMBER in the server's ack body and, if it's a
// sane value, adopts it as the new transmission delay. Anything malformed
// or out of bounds is ignored and the previous interval keeps being used.
void applyIntervalFromResponse(const String& body) {

  int key = body.indexOf("\"intervalMs\"");
  if (key == -1) return;

  int colon = body.indexOf(':', key);
  if (colon == -1) return;

  int numEnd = colon + 1;
  while (numEnd < (int)body.length() &&
         (isDigit(body[numEnd]) || body[numEnd] == ' '))
    numEnd++;

  String numStr = body.substring(colon + 1, numEnd);
  numStr.trim();

  if (numStr.length() == 0) return;

  long value = numStr.toInt();

  if (value >= (long)MIN_INTERVAL_MS && value <= (long)MAX_INTERVAL_MS) {
    if ((unsigned long)value != currentIntervalMs) {
      Serial.print("Delay updated by server -> ");
      Serial.print(value);
      Serial.println(" ms");
    }
    currentIntervalMs = (unsigned long)value;
  }
}

bool httpsPost(const String& json) {

  String cmd =
      "AT+QHTTPURL=" +
      String(strlen(SERVER_URL)) +
      ",30";

  modemSerial.println(cmd);

  if (waitFor("CONNECT").indexOf("CONNECT") == -1)
    return false;

  modemSerial.print(SERVER_URL);

  if (waitFor("OK").indexOf("OK") == -1)
    return false;

  // Give the modem a brief moment to finish settling the URL/SSL context
  // before demanding it accept a POST. On most cycles this isn't needed,
  // but under marginal signal the modem can be slightly slower to respond
  // than usual, and this beats another whole failed upload.
  delay(300);

  cmd =
      "AT+QHTTPPOST=" +
      String(json.length()) +
      ",60,60";

  modemSerial.println(cmd);

  // Was 15s (waitFor's default). Bumped to 20s: an occasional SSL
  // renegotiation or momentary signal dip can push past 15s even when
  // the upload would otherwise have gone through fine.
  if (waitFor("CONNECT", 20000).indexOf("CONNECT") == -1) {
    resetHttpSession();
    return false;
  }

  modemSerial.print(json);

  String resp = waitFor("+QHTTPPOST:",60000);

  if (resp.indexOf("+QHTTPPOST:") == -1) {
    // No response at all to the POST itself — the session is likely
    // wedged. Reset it now so the *next* cycle starts clean instead of
    // inheriting whatever bad state caused this failure.
    resetHttpSession();
    return false;
  }

  String body = sendATraw("AT+QHTTPREAD=60",10000);
  applyIntervalFromResponse(body);

  return resp.indexOf("200")!=-1 ||
         resp.indexOf("201")!=-1;
}

// Releases the modem's internal HTTP(S) session/socket after a failed
// transaction. Cheap and safe to call even if there was nothing to clean
// up — it just makes sure the *next* AT+QHTTPURL starts from a known-good
// state instead of getting stuck behind whatever went wrong this time.
void resetHttpSession() {
  sendAT("AT+QHTTPSTOP", "OK", 5000);
}

// =======================================================
// POWER ON MODEM
// =======================================================

void powerOnModem() {

  Serial.println("Powering modem...");

  pinMode(PW_KEY, OUTPUT);

  digitalWrite(PW_KEY, LOW);
  delay(1500);
  digitalWrite(PW_KEY, HIGH);

  delay(5000);
}

// =======================================================
// INITIALIZE MODEM
// =======================================================

void initModem() {

  powerOnModem();

  bool modemReady = false;

  for (int i = 0; i < 15; i++) {

    if (sendAT("AT")) {
      modemReady = true;
      break;
    }

    delay(1000);
  }

  if (!modemReady) {
    Serial.println("ERROR: Modem not responding.");
    while (1);
  }

  Serial.println("Modem Ready");

  sendAT("ATE0");
  sendAT("AT+CMEE=2");

  if (!sendAT("AT+CPIN?", "READY")) {
    Serial.println("SIM not ready!");
    while (1);
  }

  // Automatic network time update
  sendAT("AT+CTZU=1");

  Serial.println("Waiting for network...");

  while (true) {

    String resp = sendATraw("AT+CREG?", 3000);

    if (resp.indexOf(",1") != -1 ||
        resp.indexOf(",5") != -1) {
      break;
    }

    delay(2000);
  }

  Serial.println("Network Registered");

  sendAT("AT+QIDEACT=1");

  delay(1000);

  String apnCmd =
      "AT+QICSGP=1,1,\"" +
      String(APN) +
      "\",\"\",\"\",0";

  sendAT(apnCmd.c_str());

  if (!sendAT("AT+QIACT=1", "OK", 20000)) {
    Serial.println("Failed to activate data.");
    while (1);
  }

  Serial.println("4G Connected");

  configureSSL();

  Serial.println("Initialization Complete");
}

// =======================================================
// SETUP
// =======================================================

void setup() {

  Serial.begin(115200);

  delay(1000);

  modemSerial.begin(
      EC200U_BAUD,
      SERIAL_8N1,
      EC200U_RX,
      EC200U_TX);

  initModem();

  Serial.println();
  Serial.println("GPS Tracker Started");
}

// =======================================================
// LOOP
// =======================================================

void loop() {

  // Interval is measured start-to-start, so the time spent uploading
  // counts towards it (10 s on the website = one upload every 10 s).
  unsigned long cycleStart = millis();

  double lat = interpolate(
      START_LAT,
      END_LAT,
      currentStep,
      TOTAL_STEPS);

  double lon = interpolate(
      START_LON,
      END_LON,
      currentStep,
      TOTAL_STEPS);

  String timestamp = getISTTimestamp();

  String payload =
      buildJSON(
          lat,
          lon,
          ALTITUDE,
          timestamp);

  Serial.println("--------------------------------");
  Serial.print("Step : ");
  Serial.println(currentStep + 1);

  Serial.print("Lat  : ");
  Serial.println(lat, 6);

  Serial.print("Lon  : ");
  Serial.println(lon, 6);

  Serial.print("Time : ");
  Serial.println(timestamp);

  bool ok = httpsPost(payload);

  if (!ok) {
    Serial.println("Upload Failed - retrying once...");
    delay(500);
    ok = httpsPost(payload);
  }

  if (ok)
    Serial.println("Upload Successful");
  else
    Serial.println("Upload Failed");

  currentStep++;

  if (currentStep >= TOTAL_STEPS) {
    currentStep = 0;
    Serial.println("Route Restarted");
  }

  unsigned long elapsed = millis() - cycleStart;

  if (elapsed < currentIntervalMs)
    delay(currentIntervalMs - elapsed);
  else {
    Serial.print("NOTE: cycle took ");
    Serial.print(elapsed);
    Serial.println(" ms, longer than the interval - sending again now");
  }
}