#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <EEPROM.h>
#include <WiFiManager.h>
#include <Ticker.h>
#include <time.h>
#include <sntp.h>

// Constants
#define EEPROM_INITIALIZED_MARKER 0x10
#define EEPROM_SIZE 512
#define HOSTNAME_PREFIX "testduck"
#define MAX_HOSTNAME_LENGTH 14
#define HTTP_RETRY_DELAY 500
#define REBOOT_DELAY 1000
#define ADMIN_USERNAME "user"
#define ADMIN_PASSWORD "pass"

// NTP Constants for London, UK
#define DEFAULT_NTP_SERVER "time.local"
#define NTP_TIMEZONE "GMT0BST,M3.5.0/1,M10.5.0/1"

// Debug modes
#define DEBUGMODE_STATUSLED 1
#define DEBUGMODE_SERIAL 2
#define DEBUGMODE_BOTH 3
#define DEBUGMODE DEBUGMODE_BOTH
#if DEBUGMODE == DEBUGMODE_BOTH || DEBUGMODE == DEBUGMODE_SERIAL
#define SERIAL_ENABLED 1
#define WIFIMANAGERSETSTATUS_SERIALENABLED 1
#endif
#if DEBUGMODE == DEBUGMODE_BOTH || DEBUGMODE == DEBUGMODE_STATUSLED
#define WIFIMANAGERSETSTATUS_LEDENABLED 1
#endif

// Pin definitions and timers
#define DDNS_STATUSLED 2
#define WIFIMANAGERSETSTATUS_LED 2
#define WIFIMANAGERSETSTATUS_STARTAP 0
#define WIFIMANAGERSETSTATUS_CONNECTED 1
#define WIFIMANAGERSETSTATUS_TRYCONNECTION 2
#define CHECKCONNECTION_INTERVALL 1000
#define DDNS_ONERRORRETRIES 5
#define DDNS_ONERRORINTERVAL 1

Ticker wifiManagerStatusLedTicker;
Ticker ddnsStatusLedTicker;

// Configuration structure
struct ddnsConfig {
  char initialized;
  int deviceid;
  char domain[65];
  char token[37];
  int updateinterval;
  char ntpServer[65];
} ddnsConfiguration;

// Global variables
unsigned long checkconnection_timer = 0;
unsigned long ddns_updatetimer = 0;
unsigned long ddns_lastupdatetime = 0;
unsigned long ddns_lastupdatestatus = 0;
bool firstUpdate = true;
unsigned long time_sync_start = 0;

// Update Log Feature
#define DDNS_LOG_SIZE 5
struct UpdateLogEntry {
  unsigned long timestamp;
  int status;
  String errorMessage;
};
UpdateLogEntry ddnsUpdateLog[DDNS_LOG_SIZE];
int ddnsLogIdx = 0;

// Web server
WebServer server(80);

// Enhanced HTML/CSS with mobile responsiveness
const char HTTP_HEADSTYLE[] PROGMEM =
  "<style>"
  "body{text-align:center;font-family:verdana;font-size:90%;background:#f9f9f9;margin:0;padding:0}"
  "div.b{margin:0 auto;max-width:650px;background:#fff;border-radius:8px;box-shadow:0 2px 8px #aaa;padding:24px 16px;}"
  "h1{font-size:2em;margin:0 0 16px 0;}"
  "div.m{background:#0077c2;padding:10px;color:#fff;border-radius:4px 4px 0 0;margin-bottom:16px;}"
  "div.m a{color:#fff;text-decoration:none;padding:0 8px;}"
  "span.status-badge{display:inline-block;padding:0.2em 0.8em;border-radius:99px;font-weight:bold;}"
  "span.success{background:#c8f7c5;color:#15690b;}"
  "span.fail{background:#ffd8d8;color:#a00;}"
  "span.info{background:#e3f0fd;color:#16588a;}"
  "table{width:100%;margin:8px 0;border-collapse:collapse;font-size:90%;}"
  "td,th{padding:6px;text-align:left;border:1px solid #ddd;}"
  "tr:nth-child(even){background:#f5f5f5;}"
  "input[type=text],input[type=password]{width:100%;padding:8px;margin:4px 0;box-sizing:border-box;border:1px solid #ddd;border-radius:4px;}"
  "button{background:#0077c2;color:#fff;border:none;padding:10px 20px;border-radius:4px;cursor:pointer;}"
  "button:hover{background:#005fa3;}"
  "@media(max-width:650px){div.b{padding:12px 8px;}}"
  "</style>";

// Helper functions
bool isTimeExpired(unsigned long timer) {
  return (long)(millis() - timer) >= 0;
}

bool isTimeSynced() {
  time_t now = time(nullptr);
  // Valid time should be between 2023 and 2030 (reasonable range)
  return now > 1672531200 && now < 1893456000;
}

String fmtTime(unsigned long sec) {
  if (sec == 0) return "Never";
  int m = sec / 60;
  int h = m / 60;
  m = m % 60;
  int s = sec % 60;
  char buf[16];
  snprintf(buf, sizeof(buf), "%02dh %02dm %02ds", h, m, s);
  return String(buf);
}

void ddnsLogAdd(int status, const String& errorMsg = "") {
  if (ddnsLogIdx >= 0 && ddnsLogIdx < DDNS_LOG_SIZE) {
    time_t now = time(nullptr);
    // Use timestamp 0 if time is not synchronized yet (placeholder)
    // Valid time should be between 2023 and 2030 (reasonable range)
    unsigned long timestamp = (now > 1672531200 && now < 1893456000) ? (unsigned long)now : 0;
    ddnsUpdateLog[ddnsLogIdx] = { timestamp, status, errorMsg };
    ddnsLogIdx = (ddnsLogIdx + 1) % DDNS_LOG_SIZE;
#if defined SERIAL_ENABLED
    if (timestamp == 0) {
      Serial.println("Log entry added with placeholder timestamp (time not synced), raw time: " + String((unsigned long)now));
    } else {
      Serial.println("Log entry added with valid timestamp: " + String(timestamp));
    }
    Serial.println("Log index now at: " + String(ddnsLogIdx));
#endif
  }
}

String logTableHTML() {
  String out = "";
  char time_str[30];
  for (int i = 0; i < DDNS_LOG_SIZE; i++) {
    int idx = (ddnsLogIdx + i) % DDNS_LOG_SIZE;
    // Skip completely empty entries
    if (ddnsUpdateLog[idx].timestamp == 0 && ddnsUpdateLog[idx].status == 0 && ddnsUpdateLog[idx].errorMessage == "") continue;

    String timeDisplay;
    if (ddnsUpdateLog[idx].timestamp == 0) {
      // Show "Undetermined" for entries logged before time sync
      timeDisplay = "Undetermined";
    } else {
      // Format real timestamps
      struct tm timeinfo;
      localtime_r((time_t*)&ddnsUpdateLog[idx].timestamp, &timeinfo);
      strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &timeinfo);
      timeDisplay = String(time_str);
    }

    String status = ddnsUpdateLog[idx].status ? "<span class='status-badge success'>Success</span>" : "<span class='status-badge fail'>Fail" + (ddnsUpdateLog[idx].errorMessage.length() ? ": " + ddnsUpdateLog[idx].errorMessage : "") + "</span>";
    out += "<tr><td>" + timeDisplay + "</td><td>" + status + "</td></tr>";
  }
  return out.length() ? out : "<tr><td colspan='2'>No updates yet</td></tr>";
}

bool authenticate() {
  if (!server.authenticate(ADMIN_USERNAME, ADMIN_PASSWORD)) {
    server.requestAuthentication();
    return false;
  }
  return true;
}

// Web page handlers
void pageHome() {
  String content;
  content.reserve(4096);

  String lastupdatestatus = ddns_lastupdatestatus ? "Success" : "Fail";
  String statusClass = ddns_lastupdatestatus ? "success" : "fail";
  
  // Better handling of time since last update
  unsigned long lastupdatetime = 0;
  if (ddns_lastupdatetime > 0 && isTimeSynced()) {
    time_t now = time(nullptr);
    if (now > ddns_lastupdatetime) {
      lastupdatetime = now - ddns_lastupdatetime;
    }
  }
  
  unsigned long nextupdatetime = ddns_updatetimer > millis() ? (ddns_updatetimer - millis()) / 1000 : 0;

  String ip = WiFi.isConnected() ? WiFi.localIP().toString() : "N/A";
  int rssi = WiFi.isConnected() ? WiFi.RSSI() : 0;
  String uptime = fmtTime(millis() / 1000);
  String fw = "v1.4 (" __DATE__ " " __TIME__ ")";

  // Current time display
  String currentTimeStr = "Time not synchronized";
  if (isTimeSynced()) {
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    char time_str[30];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &timeinfo);
    currentTimeStr = String(time_str);
  }

  content = F("<!DOCTYPE html><html lang='en'><head>"
              "<meta name='viewport' content='width=device-width, initial-scale=1'>"
              "<title>ESP32 Duck DNS Client</title>");
  content += FPSTR(HTTP_HEADSTYLE);
  content += F("</head><body><div class='b'>"
               "<h1>ESP32 DuckDNS Client</h1>"
               "<div class='m'><a href='/'>Status</a> | <a href='/settings'>Settings</a></div>"
               "<div class='p'><h2>Status</h2>");

  content += "<p>Current Time: <b>" + currentTimeStr + "</b></p>";

  content += "<p>Last update: <span class='status-badge " + statusClass + "'>" + lastupdatestatus + "</span><br/>"
             "Time since update: <b>" + fmtTime(lastupdatetime) + "</b><br/>"
             "Next update in: <b>" + fmtTime(nextupdatetime) + "</b></p>";

  // Force sync button
  content += "<p><a href='/forcesync'><button>Force Sync</button></a></p>";

  content += "<h3>Device Info</h3>"
             "<p>IP Address: <b>" + ip + "</b><br/>"
             "WiFi Signal: <b>" + String(rssi) + " dBm</b><br/>"
             "Uptime: <b>" + uptime + "</b><br/>"
             "Firmware: <b>" + fw + "</b></p>";

  content += "<h3>Update Log</h3>"
             "<table><tr><th>Time</th><th>Status</th></tr>"
             + logTableHTML() + "</table>";

  content += F("</div></div></body></html>");

  server.send(200, "text/html", content);
}

void pageSettings() {
  if (!authenticate()) {
    return;
  }

  String content;
  content.reserve(4096);
  bool settingsChanged = false;

  if (server.hasArg("b")) {
    if (server.hasArg("n") && ddnsConfiguration.deviceid != server.arg("n").toInt()) {
      ddnsConfiguration.deviceid = server.arg("n").toInt();
      settingsChanged = true;
    }

    if (server.hasArg("d")) {
      String domain = server.arg("d");
      if (domain.length() + 1 <= sizeof(ddnsConfiguration.domain)) {
        strncpy(ddnsConfiguration.domain, domain.c_str(), sizeof(ddnsConfiguration.domain) - 1);
        ddnsConfiguration.domain[sizeof(ddnsConfiguration.domain) - 1] = '\0';
        settingsChanged = true;
      }
    }

    if (server.hasArg("t")) {
      String token = server.arg("t");
      if (token.length() + 1 <= sizeof(ddnsConfiguration.token)) {
        strncpy(ddnsConfiguration.token, token.c_str(), sizeof(ddnsConfiguration.token) - 1);
        ddnsConfiguration.token[sizeof(ddnsConfiguration.token) - 1] = '\0';
        settingsChanged = true;
      }
    }

    if (server.hasArg("u")) {
      ddnsConfiguration.updateinterval = server.arg("u").toInt();
      settingsChanged = true;
    }

    if (server.hasArg("ns")) {
      String ntpServer = server.arg("ns");
      if (ntpServer.length() + 1 <= sizeof(ddnsConfiguration.ntpServer)) {
        strncpy(ddnsConfiguration.ntpServer, ntpServer.c_str(), sizeof(ddnsConfiguration.ntpServer) - 1);
        ddnsConfiguration.ntpServer[sizeof(ddnsConfiguration.ntpServer) - 1] = '\0';
        settingsChanged = true;
      }
    }

    if (server.arg("r") == "1") {
      WiFiManager wifiManager;
      wifiManager.resetSettings();
    }

    if (settingsChanged) {
      ddnsEEPROMwrite();
    }

    content = F("<!DOCTYPE html><html lang='en'><head>"
                "<meta name='viewport' content='width=device-width, initial-scale=1'>"
                "<title>Settings Saved - ESP32 Duck DNS Client</title>");
    content += FPSTR(HTTP_HEADSTYLE);
    content += F("</head><body><div class='b'>"
                 "<h1>Settings Saved</h1>"
                 "<p>Device will restart in 10 seconds...</p>"
                 "<script>setTimeout(function(){window.location.href='/';},10000);</script>"
                 "</div></body></html>");

    server.send(200, "text/html", content);
    delay(1000);
    ESP.restart();
    return;
  }

  content = F("<!DOCTYPE html><html lang='en'><head>"
              "<meta name='viewport' content='width=device-width, initial-scale=1'>"
              "<title>Settings - ESP32 Duck DNS Client</title>");
  content += FPSTR(HTTP_HEADSTYLE);
  content += F("</head><body><div class='b'>"
               "<h1>ESP32 DuckDNS Client</h1>"
               "<div class='m'><a href='/'>Status</a> | <a href='/settings'>Settings</a></div>"
               "<h2>Settings</h2>");

  content += F("<form method='get' action='/settings' onsubmit='return validateForm()'>"
               "<p><label>Device ID (0-999):</label><br/>"
               "<input type='text' name='n' maxlength='3' value='");
  content += String(ddnsConfiguration.deviceid);
  content += F("'></p>"
               "<p><label>Domain:</label><br/>"
               "<input type='text' name='d' maxlength='64' value='");
  content += String(ddnsConfiguration.domain);
  content += F("'></p>"
               "<p><label>Token:</label><br/>"
               "<input type='text' name='t' maxlength='36' value='");
  content += String(ddnsConfiguration.token);
  content += F("'></p>"
               "<p><label>Update interval (minutes):</label><br/>"
               "<input type='text' name='u' maxlength='3' value='");
  content += String(ddnsConfiguration.updateinterval);
  content += F("'></p>"
               "<p><label>NTP Server:</label><br/>"
               "<input type='text' name='ns' maxlength='64' value='");
  content += String(ddnsConfiguration.ntpServer);
  content += F("'></p>"
               "<p><label><input type='checkbox' name='r' value='1'> Reset WiFi settings</label></p>"
               "<input type='hidden' name='b' value='1'>"
               "<button type='submit'>Save Settings</button></form>");

  content += F("<script>"
               "function validateForm() {"
               "var deviceId = document.getElementsByName('n')[0].value;"
               "var domain = document.getElementsByName('d')[0].value;"
               "var token = document.getElementsByName('t')[0].value;"
               "var interval = document.getElementsByName('u')[0].value;"
               "var ntpServer = document.getElementsByName('ns')[0].value;"
               "if(isNaN(deviceId) || deviceId < 0 || deviceId > 999) {"
               "alert('Device ID must be between 0 and 999');"
               "return false;}"
               "if(domain.length < 1 || domain.length > 64) {"
               "alert('Domain must be between 1 and 64 characters');"
               "return false;}"
               "if(token.length < 1 || token.length > 36) {"
               "alert('Token must be between 1 and 36 characters');"
               "return false;}"
               "if(isNaN(interval) || interval < 1 || interval > 99) {"
               "alert('Update interval must be between 1 and 99 minutes');"
               "return false;}"
               "if(ntpServer.length < 1 || ntpServer.length > 64) {"
               "alert('NTP Server must be between 1 and 64 characters');"
               "return false;}"
               "return true;}"
               "</script>");

  content += F("</div></body></html>");

  server.send(200, "text/html", content);
}

void pageNotFound() {
  String content;
  content.reserve(1024);
  content = F("<!DOCTYPE html><html lang='en'><head>"
              "<meta name='viewport' content='width=device-width, initial-scale=1'>"
              "<title>404 - ESP32 Duck DNS Client</title>");
  content += FPSTR(HTTP_HEADSTYLE);
  content += F("</head><body><div class='b'>"
               "<h1>404 Not Found</h1>"
               "<p>The requested page was not found.</p>"
               "<p><a href='/'>Return to Home</a></p>"
               "</div></body></html>");
  server.send(404, "text/html", content);
}

void handleForceSync() {
  if (ddnsUpdate()) {
    ddns_lastupdatetime = (unsigned long)time(nullptr);
  }
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

void pageApiStatus() {
  String json;
  json.reserve(1024);
  json = "{";
  json += "\"status\":\"" + String(ddns_lastupdatestatus ? "success" : "fail") + "\",";
  json += "\"last_update\":" + String(ddns_lastupdatetime) + ",";
  json += "\"next_update\":" + String(ddns_updatetimer > millis() ? (ddns_updatetimer - millis()) / 1000 : 0) + ",";
  json += "\"ip\":\"" + (WiFi.isConnected() ? WiFi.localIP().toString() : "N/A") + "\",";
  json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  json += "\"uptime\":" + String(millis() / 1000) + ",";
  json += "\"time_synced\":" + String(isTimeSynced() ? "true" : "false") + ",";
  json += "\"log\":[";

  bool firstLog = true;
  for (int i = 0; i < DDNS_LOG_SIZE; i++) {
    int idx = (ddnsLogIdx + i) % DDNS_LOG_SIZE;
    if (ddnsUpdateLog[idx].timestamp == 0) continue;
    if (!firstLog) json += ",";
    json += "{\"time\":" + String(ddnsUpdateLog[idx].timestamp);
    json += ",\"status\":\"" + String(ddnsUpdateLog[idx].status ? "success" : "fail") + "\"";
    if (ddnsUpdateLog[idx].errorMessage.length() > 0) {
      json += ",\"error\":\"" + ddnsUpdateLog[idx].errorMessage + "\"";
    }
    json += "}";
    firstLog = false;
  }
  json += "]}";
  server.send(200, "application/json", json);
}

// EEPROM functions
void ddnsEEPROMinit() {
  EEPROM.begin(EEPROM_SIZE);
  delay(10);
  EEPROM.get(0, ddnsConfiguration);
  if (ddnsConfiguration.initialized != EEPROM_INITIALIZED_MARKER) {
#if defined SERIAL_ENABLED
    Serial.println("Initializing EEPROM with default values");
#endif
    ddnsConfiguration.initialized = EEPROM_INITIALIZED_MARKER;
    ddnsConfiguration.deviceid = 1;
    strncpy(ddnsConfiguration.domain, "your-domain", sizeof(ddnsConfiguration.domain) - 1);
    strncpy(ddnsConfiguration.token, "your-token", sizeof(ddnsConfiguration.token) - 1);
    ddnsConfiguration.updateinterval = 10;
    strncpy(ddnsConfiguration.ntpServer, DEFAULT_NTP_SERVER, sizeof(ddnsConfiguration.ntpServer) - 1);
    EEPROM.put(0, ddnsConfiguration);
    EEPROM.commit();
  }
}

void ddnsEEPROMread() {
  EEPROM.get(0, ddnsConfiguration);
}

void ddnsEEPROMwrite() {
  EEPROM.put(0, ddnsConfiguration);
  EEPROM.commit();
}

// LED status indicators
void wifiManagerStatusLedTick() {
  digitalWrite(WIFIMANAGERSETSTATUS_LED, !digitalRead(WIFIMANAGERSETSTATUS_LED));
}

void ddnsStatusLedTick() {
  digitalWrite(DDNS_STATUSLED, !digitalRead(DDNS_STATUSLED));
}

void wifiManagerSetStatus(int status) {
  switch (status) {
    case WIFIMANAGERSETSTATUS_STARTAP:
#if defined WIFIMANAGERSETSTATUS_SERIALENABLED
      Serial.println("WiFi: Configuration mode");
      Serial.println(WiFi.softAPIP());
#endif
#if defined WIFIMANAGERSETSTATUS_LEDENABLED
      wifiManagerStatusLedTicker.attach(0.2, wifiManagerStatusLedTick);
#endif
      break;

    case WIFIMANAGERSETSTATUS_CONNECTED:
#if defined WIFIMANAGERSETSTATUS_SERIALENABLED
      Serial.println("WiFi: Connected");
      Serial.println(WiFi.localIP());
#endif
#if defined WIFIMANAGERSETSTATUS_LEDENABLED
      wifiManagerStatusLedTicker.detach();
      digitalWrite(WIFIMANAGERSETSTATUS_LED, HIGH);
#endif
      break;

    case WIFIMANAGERSETSTATUS_TRYCONNECTION:
#if defined WIFIMANAGERSETSTATUS_SERIALENABLED
      Serial.println("WiFi: Attempting connection");
#endif
#if defined WIFIMANAGERSETSTATUS_LEDENABLED
      wifiManagerStatusLedTicker.attach(0.5, wifiManagerStatusLedTick);
#endif
      break;
  }
}

// DDNS update function
int ddnsUpdate() {
  WiFiClientSecure client;
  HTTPClient http;
  String error_message = "";

  try {
    client.setInsecure();

    String url = "https://www.duckdns.org/update?domains=";
    url += String(ddnsConfiguration.domain);
    url += "&token=" + String(ddnsConfiguration.token);
    url += "&ip=";

    if (!http.begin(client, url)) {
      throw std::runtime_error("Failed to initialize HTTP client");
    }

    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
      throw std::runtime_error(("HTTP error: " + String(httpCode)).c_str());
    }

    String payload = http.getString();
    if (payload != "OK") {
      throw std::runtime_error(("Invalid response: " + payload).c_str());
    }

    // Success
    ddnsStatusLedTicker.detach();
    digitalWrite(DDNS_STATUSLED, HIGH);
    ddns_lastupdatestatus = 1;
    time_t now = time(nullptr);
    if (now > 1672531200 && now < 1893456000) {
      ddns_lastupdatetime = (unsigned long)now;
    }
    ddnsLogAdd(1);
    firstUpdate = false;
#if defined SERIAL_ENABLED
    Serial.println("DDNS update successful");
#endif
    return 1;

  } catch (const std::exception& e) {
    error_message = e.what();
#if defined SERIAL_ENABLED
    Serial.println("DDNS update error: " + error_message);
#endif
    ddnsStatusLedTicker.attach(0.5, ddnsStatusLedTick);
    ddns_lastupdatestatus = 0;
    ddnsLogAdd(0, error_message);
  }

  http.end();
  return 0;
}

void setup() {
#if defined SERIAL_ENABLED
  Serial.begin(115200);
  delay(500);
  Serial.println("\nESP32 DuckDNS Client v1.4 starting...");
#endif

  pinMode(WIFIMANAGERSETSTATUS_LED, OUTPUT);
  digitalWrite(WIFIMANAGERSETSTATUS_LED, HIGH);

  pinMode(DDNS_STATUSLED, OUTPUT);
  digitalWrite(DDNS_STATUSLED, HIGH);

  ddns_lastupdatetime = 0;
  ddns_updatetimer = millis();
  firstUpdate = true;
  
  // Properly initialize the log array
  for (int i = 0; i < DDNS_LOG_SIZE; i++) {
    ddnsUpdateLog[i].timestamp = 0;
    ddnsUpdateLog[i].status = 0;
    ddnsUpdateLog[i].errorMessage = "";
  }
  ddnsLogIdx = 0;

  ddnsEEPROMinit();
  char deviceidc[4];
  snprintf(deviceidc, sizeof(deviceidc), "%03d", ddnsConfiguration.deviceid);
  char hostname[MAX_HOSTNAME_LENGTH];
  snprintf(hostname, sizeof(hostname), "%s%s", HOSTNAME_PREFIX, deviceidc);

  WiFi.setHostname(hostname);
  WiFiManager wifiManager;
  wifiManager.setConfigPortalTimeout(180);
  wifiManager.setDebugOutput(SERIAL_ENABLED);

  wifiManagerSetStatus(WIFIMANAGERSETSTATUS_TRYCONNECTION);
  if (!wifiManager.autoConnect("ESP32-DuckDNS-AP")) {
    ESP.restart();
  }
  wifiManagerSetStatus(WIFIMANAGERSETSTATUS_CONNECTED);

#if defined SERIAL_ENABLED
  Serial.println("Initializing NTP service...");
  Serial.print("Using NTP server: ");
  Serial.println(ddnsConfiguration.ntpServer);
#endif
  configTime(0, 0, ddnsConfiguration.ntpServer);
  setenv("TZ", NTP_TIMEZONE, 1);
  tzset();
  time_sync_start = millis();

  if (MDNS.begin(hostname)) {
#if defined SERIAL_ENABLED
    Serial.println("MDNS responder started");
#endif
  }

  server.on("/", pageHome);
  server.on("/settings", pageSettings);
  server.on("/api/status", pageApiStatus);
  server.on("/forcesync", handleForceSync);
  server.onNotFound(pageNotFound);
  server.begin();

  checkconnection_timer = millis();
}

void loop() {
  if (isTimeExpired(checkconnection_timer)) {
    checkconnection_timer = millis() + CHECKCONNECTION_INTERVALL;
    if (WiFi.status() != WL_CONNECTED) {
      wifiManagerSetStatus(WIFIMANAGERSETSTATUS_TRYCONNECTION);
    } else {
      wifiManagerSetStatus(WIFIMANAGERSETSTATUS_CONNECTED);
    }
  }

  server.handleClient();

  // Wait for NTP time to synchronize before starting DDNS updates
  if (firstUpdate) {
    time_t now = time(nullptr);
    if (now > 1672531200 && now < 1893456000) { // Valid time range check (2023-2030)
#if defined SERIAL_ENABLED
      Serial.println("NTP time synchronized: " + String((unsigned long)now));
      Serial.println("Performing first DDNS update");
#endif
      // Log the NTP sync event
      ddnsLogAdd(1, "NTP time synchronized");
      firstUpdate = false;
      // Force an immediate update after successful sync
      ddnsUpdate();
      // Set the next update timer
      ddns_updatetimer = millis() + (unsigned long)ddnsConfiguration.updateinterval * 60 * 1000;
    } else if (millis() - time_sync_start > 30000) { // Increase timeout to 30 seconds
#if defined SERIAL_ENABLED
      Serial.println("NTP sync timeout - proceeding without time sync");
      Serial.print("Current NTP server: ");
      Serial.println(ddnsConfiguration.ntpServer);
      Serial.println("Raw time value: " + String((unsigned long)now));
#endif
      // Log the timeout event
      ddnsLogAdd(0, "NTP sync timeout");
      firstUpdate = false;
      // Still try DDNS update even without time sync
      ddnsUpdate();
      // Set timer for next update
      ddns_updatetimer = millis() + (unsigned long)ddnsConfiguration.updateinterval * 60 * 1000;
    }
    return; // Skip the rest of the loop until time is handled
  }

  // Regular DDNS updates
  if (isTimeExpired(ddns_updatetimer)) {
    ddns_updatetimer = millis() + (unsigned long)ddnsConfiguration.updateinterval * 60 * 1000;
    ddnsUpdate();
  }
}
