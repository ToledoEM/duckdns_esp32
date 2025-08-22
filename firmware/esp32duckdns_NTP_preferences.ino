#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h> // MODIFIED: Replaced EEPROM with Preferences
#include <WiFiManager.h>
#include <Ticker.h>
#include <time.h>
#include <sntp.h>

// NEW: Preferences object and namespaces
Preferences preferences;
const char* CONFIG_NAMESPACE = "ddns_config";
const char* LOG_NAMESPACE = "ddns_logs";

// Constants
#define HOSTNAME_PREFIX "myduckdns"
#define MAX_HOSTNAME_LENGTH 14
#define HTTP_RETRY_DELAY 500
#define REBOOT_DELAY 1000
#define ADMIN_USERNAME "admin"
#define ADMIN_PASSWORD "password"

// Enhanced NTP Constants with fallback servers
#define PRIMARY_NTP_SERVER "time.local"
#define SECONDARY_NTP_SERVER "2.uk.pool.ntp.org"
#define TERTIARY_NTP_SERVER "3.uk.pool.ntp.org"
#define NTP_TIMEZONE "GMT0BST,M3.5.0/1,M10.5.0/1"
#define NTP_SYNC_TIMEOUT 60000    // 60 seconds
#define NTP_RETRY_INTERVAL 3600000 // 1 hour
#define NTP_HEALTH_CHECK_INTERVAL 1800000 // 30 minutes
#define REBOOT_CHECK_INTERVAL 60000  // Check reboot time every minute

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

// Enhanced configuration structure
struct ddnsConfig {
  char initialized; // Still used as a marker, but within Preferences
  int deviceid;
  bool enableDailyReboot;
  int rebootHour;
  int rebootMinute;
  char domain[65];
  char token[37];
  int updateinterval;
  char ntpServer[65];
  char secondaryNtpServer[65];
  char tertiaryNtpServer[65];
  bool persistentLogging;
} ddnsConfiguration;

// Enhanced Log Entry Structure
#define DDNS_LOG_SIZE 15
#define PERSISTENT_LOG_SIZE 50

struct EnhancedLogEntry {
  time_t timestamp;
  unsigned long localTime;
  int status;
  String errorMessage;
  bool timestampValid;
  uint8_t logType; // 0=DDNS, 1=NTP, 2=System
};

// Persistent log structure (for binary storage)
struct PersistentLogEntry {
  unsigned long timestamp;
  unsigned long localTime;
  int status;
  char errorMessage[32];
  bool timestampValid;
  uint8_t logType;
  char reserved[2];
};

EnhancedLogEntry ddnsUpdateLog[DDNS_LOG_SIZE];
int ddnsLogIdx = 0;

// NTP Management
struct NtpStatus {
  bool synchronized;
  unsigned long lastSyncTime;
  unsigned long lastSyncAttempt;
  int syncAttempts;
  int currentServerIndex;
  bool healthCheckEnabled;
} ntpStatus;

// Global variables
unsigned long checkconnection_timer = 0;
unsigned long ddns_updatetimer = 0;
time_t ddns_lastupdatetime = 0;
unsigned long ddns_lastupdatestatus = 0;
unsigned long ntp_health_check_timer = 0;
bool firstUpdate = true;
unsigned long time_sync_start = 0;
const char* ntpServers[] = {PRIMARY_NTP_SERVER, SECONDARY_NTP_SERVER, TERTIARY_NTP_SERVER};

// Web server
WebServer server(80);

// Enhanced HTML/CSS with mobile responsiveness and better status indicators
const char HTTP_HEADSTYLE[] PROGMEM =
  "<style>"
  "body{text-align:center;font-family:verdana;font-size:90%;background:#f9f9f9;margin:0;padding:0}"
  "div.b{margin:0 auto;max-width:750px;background:#fff;border-radius:8px;box-shadow:0 2px 8px #aaa;padding:24px 16px;}"
  "h1{font-size:2em;margin:0 0 16px 0;}"
  "div.m{background:#0077c2;padding:10px;color:#fff;border-radius:4px 4px 0 0;margin-bottom:16px;}"
  "div.m a{color:#fff;text-decoration:none;padding:0 8px;}"
  "span.status-badge{display:inline-block;padding:0.2em 0.8em;border-radius:99px;font-weight:bold;}"
  "span.success{background:#c8f7c5;color:#15690b;}"
  "span.fail{background:#ffd8d8;color:#a00;}"
  "span.info{background:#e3f0fd;color:#16588a;}"
  "span.warning{background:#fff3cd;color:#856404;}"
  "span.ntp-status{display:inline-block;padding:0.3em 0.8em;border-radius:4px;margin:0 4px;}"
  "span.sync-ok{background:#d4edda;color:#155724;border:1px solid #c3e6cb;}"
  "span.sync-fail{background:#f8d7da;color:#721c24;border:1px solid #f5c6cb;}"
  "table{width:100%;margin:8px 0;border-collapse:collapse;font-size:90%;}"
  "td,th{padding:6px;text-align:left;border:1px solid #ddd;}"
  "tr:nth-child(even){background:#f5f5f5;}"
  "input[type=text],input[type=password]{width:100%;padding:8px;margin:4px 0;box-sizing:border-box;border:1px solid #ddd;border-radius:4px;}"
  "button{background:#0077c2;color:#fff;border:none;padding:10px 20px;border-radius:4px;cursor:pointer;margin:2px;}"
  "button:hover{background:#005fa3;}"
  "button.danger{background:#dc3545;}"
  "button.danger:hover{background:#c82333;}"
  ".log-type{font-size:0.8em;padding:0.1em 0.4em;border-radius:3px;margin-right:4px;}"
  ".log-ddns{background:#e3f0fd;color:#16588a;}"
  ".log-ntp{background:#fff3cd;color:#856404;}"
  ".log-system{background:#e2e3e5;color:#383d41;}"
  "@media(max-width:750px){div.b{padding:12px 8px;}}"
  "</style>";

// Forward declarations
void enhancedLogAdd(int status, uint8_t logType, const String& errorMsg = "");
void updatePendingTimestamps();
void ddnsPreferencesWrite();

// Enhanced helper functions
bool isTimeExpired(unsigned long timer) {
  return (long)(millis() - timer) >= 0;
}

bool isTimeSynced() {
  time_t now = time(nullptr);
  // Extended valid time range and updated for current date
  return now > 1704067200 && now < 1956528000; // 2024-2032 range
}

bool isTimeReasonable() {
  time_t now = time(nullptr);
  return now > 1672531200; // At least 2023
}

String fmtTime(unsigned long sec) {
  if (sec == 0) return "Never";
  int m = sec / 60;
  int h = m / 60;
  int d = h / 24;
  h = h % 24;
  m = m % 60;
  int s = sec % 60;
  char buf[32];
  if (d > 0) {
    snprintf(buf, sizeof(buf), "%dd %02dh %02dm", d, h, m);
  } else {
    snprintf(buf, sizeof(buf), "%02dh %02dm %02ds", h, m, s);
  }
  return String(buf);
}

String getLogTypeStr(uint8_t type) {
  switch(type) {
    case 0: return "DDNS";
    case 1: return "NTP";
    case 2: return "SYS";
    default: return "UNK";
  }
}

String getLogTypeClass(uint8_t type) {
  switch(type) {
    case 0: return "log-ddns";
    case 1: return "log-ntp";
    case 2: return "log-system";
    default: return "log-system";
  }
}

// Enhanced NTP Management Functions
void initNtpStatus() {
  ntpStatus.synchronized = false;
  ntpStatus.lastSyncTime = 0;
  ntpStatus.lastSyncAttempt = 0;
  ntpStatus.syncAttempts = 0;
  ntpStatus.currentServerIndex = 0;
  ntpStatus.healthCheckEnabled = true;
}

bool attemptNtpSync() {
  ntpStatus.lastSyncAttempt = millis();
  ntpStatus.syncAttempts++;
  const char* server = (strlen(ddnsConfiguration.ntpServer) > 0) ?
                      ddnsConfiguration.ntpServer :
                      ntpServers[ntpStatus.currentServerIndex];
  #if defined SERIAL_ENABLED
  Serial.println("Attempting NTP sync with server: " + String(server));
  Serial.println("Sync attempt #" + String(ntpStatus.syncAttempts));
  #endif

  configTime(0, 0, server);
  setenv("TZ", NTP_TIMEZONE, 1);
  tzset();

  // Wait for sync with timeout
  unsigned long syncStart = millis();
  while (millis() - syncStart < NTP_SYNC_TIMEOUT) {
    if (isTimeSynced()) {
      ntpStatus.synchronized = true;
      ntpStatus.lastSyncTime = millis();
      time_t now = time(nullptr);

      #if defined SERIAL_ENABLED
      Serial.println("NTP sync successful! Time: " + String((unsigned long)now));
      #endif

      enhancedLogAdd(1, 1, "NTP sync successful with " + String(server));
      updatePendingTimestamps();
      return true;
    }
    delay(1000);
  }

  // Try next server on failure
  ntpStatus.currentServerIndex = (ntpStatus.currentServerIndex + 1) % 3;
  enhancedLogAdd(0, 1, "NTP sync timeout with " + String(server));

  #if defined SERIAL_ENABLED
  Serial.println("NTP sync failed with " + String(server));
  #endif

  return false;
}

void checkNtpHealth() {
  if (!ntpStatus.healthCheckEnabled) return;

  bool needsSync = false;
  // Check if we've never synced
  if (!ntpStatus.synchronized) {
    needsSync = true;
  }
  // Check if it's been too long since last sync
  else if (millis() - ntpStatus.lastSyncTime > NTP_RETRY_INTERVAL) {
    needsSync = true;
  }
  // Check if current time seems invalid
  else if (!isTimeSynced()) {
    needsSync = true;
    ntpStatus.synchronized = false;
  }

  if (needsSync && (millis() - ntpStatus.lastSyncAttempt > 60000)) { // Wait 1 min between attempts
    enhancedLogAdd(0, 1, "NTP health check triggered resync");
    attemptNtpSync();
  }
}

// --- Storage Functions (MODIFIED: Replaced EEPROM with Preferences) ---
void ddnsPreferencesInit() {
  preferences.begin(CONFIG_NAMESPACE, false);

  // Check if the configuration is initialized by looking for a key.
  if (preferences.getUChar("initialized", 0) != 0x10) {
    #if defined SERIAL_ENABLED
    Serial.println("Initializing persistent storage with default values");
    #endif

    // Write default values
    preferences.putUChar("initialized", 0x10);
    preferences.putInt("deviceid", 1);
    preferences.putString("domain", "your-domain");
    preferences.putString("token", "your-token");
    preferences.putInt("updateinterval", 10);
    preferences.putString("ntpServer", PRIMARY_NTP_SERVER);
    preferences.putString("ntpServer2", SECONDARY_NTP_SERVER);
    preferences.putString("ntpServer3", TERTIARY_NTP_SERVER);
    preferences.putBool("persistLog", false);
  }

  // Load configuration from Preferences into the global struct
  ddnsConfiguration.deviceid = preferences.getInt("deviceid", 1);
  strncpy(ddnsConfiguration.domain, preferences.getString("domain", "your-domain").c_str(), sizeof(ddnsConfiguration.domain) - 1);
  ddnsConfiguration.domain[sizeof(ddnsConfiguration.domain) - 1] = '\0';
  strncpy(ddnsConfiguration.token, preferences.getString("token", "your-token").c_str(), sizeof(ddnsConfiguration.token) - 1);
  ddnsConfiguration.token[sizeof(ddnsConfiguration.token) - 1] = '\0';
  ddnsConfiguration.updateinterval = preferences.getInt("updateinterval", 10);
  strncpy(ddnsConfiguration.ntpServer, preferences.getString("ntpServer", PRIMARY_NTP_SERVER).c_str(), sizeof(ddnsConfiguration.ntpServer) - 1);
  ddnsConfiguration.ntpServer[sizeof(ddnsConfiguration.ntpServer) - 1] = '\0';
  strncpy(ddnsConfiguration.secondaryNtpServer, preferences.getString("ntpServer2", SECONDARY_NTP_SERVER).c_str(), sizeof(ddnsConfiguration.secondaryNtpServer) - 1);
  ddnsConfiguration.secondaryNtpServer[sizeof(ddnsConfiguration.secondaryNtpServer) - 1] = '\0';
  strncpy(ddnsConfiguration.tertiaryNtpServer, preferences.getString("ntpServer3", TERTIARY_NTP_SERVER).c_str(), sizeof(ddnsConfiguration.tertiaryNtpServer) - 1);
  ddnsConfiguration.tertiaryNtpServer[sizeof(ddnsConfiguration.tertiaryNtpServer) - 1] = '\0';
  ddnsConfiguration.persistentLogging = preferences.getBool("persistLog", false);

  preferences.end();
}

void ddnsPreferencesWrite() {
  preferences.begin(CONFIG_NAMESPACE, false);

  preferences.putInt("deviceid", ddnsConfiguration.deviceid);
  preferences.putString("domain", ddnsConfiguration.domain);
  preferences.putString("token", ddnsConfiguration.token);
  preferences.putInt("updateinterval", ddnsConfiguration.updateinterval);
  preferences.putString("ntpServer", ddnsConfiguration.ntpServer);
  preferences.putString("ntpServer2", ddnsConfiguration.secondaryNtpServer);
  preferences.putString("ntpServer3", ddnsConfiguration.tertiaryNtpServer);
  preferences.putBool("persistLog", ddnsConfiguration.persistentLogging);
  preferences.putBool("dailyReboot", ddnsConfiguration.enableDailyReboot);
  preferences.putInt("rebootHour", ddnsConfiguration.rebootHour);
  preferences.putInt("rebootMinute", ddnsConfiguration.rebootMinute);

  preferences.end();
}

void saveToPersistentLog(const EnhancedLogEntry& entry) {
  preferences.begin(LOG_NAMESPACE, false);

  // Get the current log count to determine the next storage key
  int logCount = preferences.getInt("log_count", 0);

  // Prepare persistent entry
  PersistentLogEntry persistentEntry;
  persistentEntry.timestamp = entry.timestamp;
  persistentEntry.localTime = entry.localTime;
  persistentEntry.status = entry.status;
  strncpy(persistentEntry.errorMessage, entry.errorMessage.c_str(), 31);
  persistentEntry.errorMessage[31] = '\0';
  persistentEntry.timestampValid = entry.timestampValid;
  persistentEntry.logType = entry.logType;

  // Calculate position (circular buffer) and create the key
  int position = logCount % PERSISTENT_LOG_SIZE;
  String key = "log_" + String(position);

  // Write the log entry as a binary blob
  preferences.putBytes(key.c_str(), &persistentEntry, sizeof(PersistentLogEntry));

  // Add new reboot preferences
  ddnsConfiguration.enableDailyReboot = preferences.getBool("dailyReboot", false);
  ddnsConfiguration.rebootHour = preferences.getInt("rebootHour", 3);    // Default 3 AM
  ddnsConfiguration.rebootMinute = preferences.getInt("rebootMinute", 0);

  // Update and save the total log count
  logCount++;
  preferences.putInt("log_count", logCount);

  preferences.end();
}

void handleClearPersistentLogs() {
  if (!authenticate()) return;

  preferences.begin(LOG_NAMESPACE, false);
  preferences.clear(); // MODIFIED: A much simpler and more robust way to clear all logs
  preferences.end();

  enhancedLogAdd(1, 2, "Persistent logs cleared");

  server.sendHeader("Location", "/logs");
  server.send(302, "text/plain", "");
}

// --- Logging and Web UI Functions ---
void enhancedLogAdd(int status, uint8_t logType, const String& errorMsg) {
  if (ddnsLogIdx >= 0 && ddnsLogIdx < DDNS_LOG_SIZE) {
    time_t now = time(nullptr);
    unsigned long currentTime = millis();

    ddnsUpdateLog[ddnsLogIdx] = {
      .timestamp = now,
      .localTime = currentTime,
      .status = status,
      .errorMessage = errorMsg,
      .timestampValid = isTimeReasonable(),
      .logType = logType
    };
    ddnsLogIdx = (ddnsLogIdx + 1) % DDNS_LOG_SIZE;

    // Store in persistent memory if enabled
    if (ddnsConfiguration.persistentLogging) {
      saveToPersistentLog(ddnsUpdateLog[(ddnsLogIdx - 1 + DDNS_LOG_SIZE) % DDNS_LOG_SIZE]);
    }

    #if defined SERIAL_ENABLED
    Serial.println("Enhanced log entry added - Type: " + getLogTypeStr(logType) +
                   ", Status: " + String(status) +
                   ", Valid TS: " + String(isTimeReasonable()) +
                   ", Message: " + errorMsg);
    #endif
  }
}

void updatePendingTimestamps() {
  if (!isTimeSynced()) return;

  time_t currentRealTime = time(nullptr);
  unsigned long currentLocalTime = millis();
  int updatedCount = 0;

  for (int i = 0; i < DDNS_LOG_SIZE; i++) {
    if (!ddnsUpdateLog[i].timestampValid && ddnsUpdateLog[i].localTime > 0) {
      // Calculate approximate real timestamp based on time difference
      unsigned long timeDiff = currentLocalTime - ddnsUpdateLog[i].localTime;
      ddnsUpdateLog[i].timestamp = currentRealTime - (timeDiff / 1000);
      ddnsUpdateLog[i].timestampValid = false; // Mark as estimated, not actual
      updatedCount++;
    }
  }

  if (updatedCount > 0) {
    enhancedLogAdd(1, 1, "Updated " + String(updatedCount) + " pending timestamps");
    #if defined SERIAL_ENABLED
    Serial.println("Updated " + String(updatedCount) + " pending log timestamps");
    #endif
  }
}

String enhancedLogTableHTML() {
  String out = "";
  char time_str[30];
  bool hasEntries = false;

  for (int i = 0; i < DDNS_LOG_SIZE; i++) {
    int idx = (ddnsLogIdx + i) % DDNS_LOG_SIZE;
    if (ddnsUpdateLog[idx].timestamp == 0 && ddnsUpdateLog[idx].localTime == 0 &&
        ddnsUpdateLog[idx].status == 0 && ddnsUpdateLog[idx].errorMessage == "") continue;

    hasEntries = true;
    String timeDisplay;
    String timeClass = "";

    if (!ddnsUpdateLog[idx].timestampValid || ddnsUpdateLog[idx].timestamp < 1672531200) {
      if (ddnsUpdateLog[idx].localTime > 0) {
        timeDisplay = "~" + fmtTime((millis() - ddnsUpdateLog[idx].localTime) / 1000) + " ago";
        timeClass = " class='warning'";
      } else {
        timeDisplay = "Undetermined";
        timeClass = " class='fail'";
      }
    } else {
      struct tm timeinfo;
      localtime_r(&ddnsUpdateLog[idx].timestamp, &timeinfo);
      strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &timeinfo);
      timeDisplay = String(time_str);
      if (!ddnsUpdateLog[idx].timestampValid) {
        timeDisplay += " (est)";
        timeClass = " class='info'";
      }
    }

    String status = ddnsUpdateLog[idx].status ?
      "<span class='status-badge success'>Success</span>" :
      "<span class='status-badge fail'>Fail" +
      (ddnsUpdateLog[idx].errorMessage.length() ? ": " + ddnsUpdateLog[idx].errorMessage : "") + "</span>";

    String logType = "<span class='log-type " + getLogTypeClass(ddnsUpdateLog[idx].logType) + "'>" +
                     getLogTypeStr(ddnsUpdateLog[idx].logType) + "</span>";

    out += "<tr><td" + timeClass + ">" + timeDisplay + "</td><td>" + logType + status + "</td></tr>";
  }

  return hasEntries ? out : "<tr><td colspan='2'>No log entries yet</td></tr>";
}

String loadPersistentLogsHTML() {
  String out = "<table><tr><th>Time</th><th>Event</th></tr>";

  preferences.begin(LOG_NAMESPACE, true); // Read-only mode

  int logCount = preferences.getInt("log_count", 0);

  if (logCount <= 0) {
    out += "<tr><td colspan='2'>No persistent logs found</td></tr>";
  } else {
    int displayCount = min(logCount, PERSISTENT_LOG_SIZE);
    int startIdx = (logCount > PERSISTENT_LOG_SIZE) ? logCount % PERSISTENT_LOG_SIZE : 0;

    for (int i = 0; i < displayCount; i++) {
      int idx = (startIdx + i) % PERSISTENT_LOG_SIZE;
      String key = "log_" + String(idx);

      PersistentLogEntry entry;
      if (preferences.getBytes(key.c_str(), &entry, sizeof(PersistentLogEntry))) {
        if (entry.timestamp > 0) {
          String timeDisplay;
          if (entry.timestampValid && entry.timestamp > 1672531200) {
            struct tm timeinfo;
            localtime_r((time_t*)&entry.timestamp, &timeinfo);
            char time_str[30];
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &timeinfo);
            timeDisplay = String(time_str);
            if (!entry.timestampValid) {
              timeDisplay += " (est)";
            }
          } else {
            timeDisplay = "Timestamp invalid";
          }

          String status = entry.status ?
            "<span class='status-badge success'>Success</span>" :
            "<span class='status-badge fail'>Fail" +
            (strlen(entry.errorMessage) > 0 ? ": " + String(entry.errorMessage) : "") + "</span>";

          String logType = "<span class='log-type " + getLogTypeClass(entry.logType) + "'>" +
                           getLogTypeStr(entry.logType) + "</span>";

          out += "<tr><td>" + timeDisplay + "</td><td>" + logType + status + "</td></tr>";
        }
      }
    }
  }

  preferences.end();
  out += "</table>";
  return out;
}

bool authenticate() {
  if (!server.authenticate(ADMIN_USERNAME, ADMIN_PASSWORD)) {
    server.requestAuthentication();
    return false;
  }
  return true;
}

// Enhanced web page handlers
void pageHome() {
  String content;
  content.reserve(6144);

  String lastupdatestatus = ddns_lastupdatestatus ? "Success" : "Fail";
  String statusClass = ddns_lastupdatestatus ? "success" : "fail";

  unsigned long lastupdatetime = 0;
  if (ddns_lastupdatetime > 0 && isTimeSynced()) {
    time_t now = time(nullptr);
    if (now > ddns_lastupdatetime) {
      lastupdatetime = now - ddns_lastupdatetime;
    }
  }

  unsigned long nextupdatetime = ddns_updatetimer > millis() ?
                               (ddns_updatetimer - millis()) / 1000 : 0;

  String ip = WiFi.isConnected() ? WiFi.localIP().toString() : "N/A";
  int rssi = WiFi.isConnected() ? WiFi.RSSI() : 0;
  String uptime = fmtTime(millis() / 1000);
  String fw = "v2.1 Preferences (" __DATE__ " " __TIME__ ")";

  String currentTimeStr = "Time not synchronized";
  String ntpStatusStr = "<span class='ntp-status sync-fail'>Not Synced</span>";

  if (isTimeSynced()) {
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    char time_str[30];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S %Z", &timeinfo);
    currentTimeStr = String(time_str);
    ntpStatusStr = "<span class='ntp-status sync-ok'>Synchronized</span>";
  }

  content = F("<!DOCTYPE html><html lang='en'><head>"
              "<meta name='viewport' content='width=device-width, initial-scale=1'>"
              "<title>ESP32 Duck DNS Client Enhanced</title>");
  content += FPSTR(HTTP_HEADSTYLE);
  content += F("</head><body><div class='b'>"
               "<h1>ESP32 DuckDNS Client Enhanced</h1>"
               "<div class='m'><a href='/'>Status</a> | <a href='/settings'>Settings</a> | <a href='/logs'>System Logs</a></div>"
               "<div class='p'><h2>Status</h2>");
  content += "<p>Current Time: <b>" + currentTimeStr + "</b> " + ntpStatusStr + "</p>";

  content += "<h3>NTP Status</h3><p>";
  content += "Sync Attempts: <b>" + String(ntpStatus.syncAttempts) + "</b><br/>";
  content += "Last Sync: <b>" + (ntpStatus.lastSyncTime > 0 ? fmtTime((millis() - ntpStatus.lastSyncTime) / 1000) + " ago" : "Never") + "</b><br/>";
  content += "Current Server: <b>" + String(ntpServers[ntpStatus.currentServerIndex]) + "</b>";
  content += "</p>";

  content += "<h3>DDNS Status</h3>";
  content += "<p>Last update: <span class='status-badge " + statusClass + "'>" + lastupdatestatus + "</span><br/>"
             "Time since update: <b>" + fmtTime(lastupdatetime) + "</b><br/>"
             "Next update in: <b>" + fmtTime(nextupdatetime) + "</b></p>";

  content += "<p><a href='/forcesync'><button>Force DDNS Sync</button></a> ";
  content += "<a href='/forcentp'><button>Force NTP Sync</button></a> ";
  content += "<a href='/clearlogs'><button class='danger'>Clear Logs</button></a></p>";

  content += "<h3>Device Info</h3>"
             "<p>IP Address: <b>" + ip + "</b><br/>"
             "WiFi Signal: <b>" + String(rssi) + " dBm</b><br/>"
             "Uptime: <b>" + uptime + "</b><br/>"
             "Firmware: <b>" + fw + "</b><br/>"
             "Persistent Logging: <b>"
             + String(ddnsConfiguration.persistentLogging ? "Enabled" : "Disabled") + "</b></p>";

  content += "<h3>Recent Activity Log</h3>"
             "<table><tr><th>Time</th><th>Event</th></tr>"
             + enhancedLogTableHTML() + "</table>";
  content += F("</div></div></body></html>");

  server.send(200, "text/html", content);
}

void pageSettings() {
  if (!authenticate()) {
    return;
  }

  String content;
  content.reserve(5120);
  bool settingsChanged = false;

  if (server.hasArg("b")) {
    if (server.hasArg("n") && ddnsConfiguration.deviceid != server.arg("n").toInt()) {
      ddnsConfiguration.deviceid = server.arg("n").toInt();
      settingsChanged = true;
    }

    if (server.hasArg("d")) {
      String domain = server.arg("d");
      if (domain.length() < sizeof(ddnsConfiguration.domain)) {
        strncpy(ddnsConfiguration.domain, domain.c_str(), sizeof(ddnsConfiguration.domain) - 1);
        ddnsConfiguration.domain[sizeof(ddnsConfiguration.domain) - 1] = '\0';
        settingsChanged = true;
      }
    }

    if (server.hasArg("t")) {
      String token = server.arg("t");
      if (token.length() < sizeof(ddnsConfiguration.token)) {
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
      if (ntpServer.length() < sizeof(ddnsConfiguration.ntpServer)) {
        strncpy(ddnsConfiguration.ntpServer, ntpServer.c_str(), sizeof(ddnsConfiguration.ntpServer) - 1);
        ddnsConfiguration.ntpServer[sizeof(ddnsConfiguration.ntpServer) - 1] = '\0';
        settingsChanged = true;
      }
    }

    // Additional NTP servers
    if (server.hasArg("ns2")) {
      String ntpServer2 = server.arg("ns2");
      if (ntpServer2.length() < sizeof(ddnsConfiguration.secondaryNtpServer)) {
        strncpy(ddnsConfiguration.secondaryNtpServer, ntpServer2.c_str(), sizeof(ddnsConfiguration.secondaryNtpServer) - 1);
        ddnsConfiguration.secondaryNtpServer[sizeof(ddnsConfiguration.secondaryNtpServer) - 1] = '\0';
        settingsChanged = true;
      }
    }

    if (server.hasArg("ns3")) {
      String ntpServer3 = server.arg("ns3");
      if (ntpServer3.length() < sizeof(ddnsConfiguration.tertiaryNtpServer)) {
        strncpy(ddnsConfiguration.tertiaryNtpServer, ntpServer3.c_str(), sizeof(ddnsConfiguration.tertiaryNtpServer) - 1);
        ddnsConfiguration.tertiaryNtpServer[sizeof(ddnsConfiguration.tertiaryNtpServer) - 1] = '\0';
        settingsChanged = true;
      }
    }

    if (server.hasArg("pl")) {
      bool persistentLogging = server.arg("pl") == "1";
      if (ddnsConfiguration.persistentLogging != persistentLogging) {
        ddnsConfiguration.persistentLogging = persistentLogging;
        settingsChanged = true;
      }
    }

    if (server.arg("r") == "1") {
      WiFiManager wifiManager;
      wifiManager.resetSettings();
      enhancedLogAdd(1, 2, "WiFi settings reset");
    }

    if (server.hasArg("dr")) {
      ddnsConfiguration.enableDailyReboot = server.arg("dr") == "1";
      settingsChanged = true;
    } else {
       ddnsConfiguration.enableDailyReboot = false;
       settingsChanged = true;
    }

    if (server.hasArg("rh")) {
       int hour = server.arg("rh").toInt();
       if (hour >= 0 && hour <= 23) {
       ddnsConfiguration.rebootHour = hour;
       settingsChanged = true;
      }
     }

      if (server.hasArg("rm")) {
          int minute = server.arg("rm").toInt();
            if (minute >= 0 && minute <= 59) {
                ddnsConfiguration.rebootMinute = minute;
                settingsChanged = true;
            }
        }

    if (settingsChanged) {
      ddnsPreferencesWrite(); // MODIFIED: Call the new save function
      enhancedLogAdd(1, 2, "Configuration updated");
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
               "<h1>ESP32 DuckDNS Client Enhanced</h1>"
               "<div class='m'><a href='/'>Status</a> | <a href='/settings'>Settings</a> | <a href='/logs'>System Logs</a></div>"
               "<h2>Settings</h2>");
  content += F("<form method='get' action='/settings' onsubmit='return validateForm()'>"
               "<h3>DDNS Configuration</h3>"
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
  content += F("'></p>");

  content += F("<h3>NTP Configuration</h3>"
               "<p><label>Primary NTP Server:</label><br/>"
               "<input type='text' name='ns' maxlength='64' value='");
  content += String(ddnsConfiguration.ntpServer);
  content += F("'></p>");
  content += F("<p><label>Secondary NTP Server:</label><br/>"
               "<input type='text' name='ns2' maxlength='64' value='");
  content += String(ddnsConfiguration.secondaryNtpServer);
  content += F("'></p>");
  content += F("<p><label>Tertiary NTP Server:</label><br/>"
               "<input type='text' name='ns3' maxlength='64' value='");
  content += String(ddnsConfiguration.tertiaryNtpServer);
  content += F("'></p>");


  content += F("<h3>System Options</h3>"
               "<p><label><input type='checkbox' name='pl' value='1'");
  if (ddnsConfiguration.persistentLogging) content += F(" checked");
  content += F("> Enable persistent logging</label></p>"
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
               "if(isNaN(interval) || interval < 1 || interval > 999) {"
               "alert('Update interval must be between 1 and 999 minutes');"
               "return false;}"
               "if(ntpServer.length < 1 || ntpServer.length > 64) {"
               "alert('Primary NTP Server must be between 1 and 64 characters');"
               "return false;}"
               "return true;}"
               "</script>");
  content += F("<h3>Scheduled Reboot</h3>"
                "<p><label><input type='checkbox' name='dr' value='1'");
  if (ddnsConfiguration.enableDailyReboot) content += F(" checked");
  content += F("> Enable daily reboot</label></p>"
                "<p><label>Reboot Time (24h format):</label><br/>"
                "<input type='number' name='rh' min='0' max='23' value='");
  content += String(ddnsConfiguration.rebootHour);
  content += F("' style='width:60px'> : "
                "<input type='number' name='rm' min='0' max='59' value='");
  content += String(ddnsConfiguration.rebootMinute);
  content += F("' style='width:60px'></p>");

  content += F("</div></body></html>");
  server.send(200, "text/html", content);
}

void pageLogs() {
  if (!authenticate()) {
    return;
  }
  String content;
  content.reserve(4096);
  content = F("<!DOCTYPE html><html lang='en'><head>"
              "<meta name='viewport' content='width=device-width, initial-scale=1'>"
              "<title>System Logs - ESP32 Duck DNS Client</title>");
  content += FPSTR(HTTP_HEADSTYLE);
  content += F("</head><body><div class='b'>"
               "<h1>ESP32 DuckDNS Client Enhanced</h1>"
               "<div class='m'><a href='/'>Status</a> | <a href='/settings'>Settings</a> | <a href='/logs'>System Logs</a></div>"
               "<h2>System Logs</h2>");

  content += "<p>Memory Buffer: <b>" + String(DDNS_LOG_SIZE) + " entries</b> | ";
  content += "Persistent Storage: <b>" + String(ddnsConfiguration.persistentLogging ? "Enabled" : "Disabled") + "</b></p>";
  content += "<h3>Current Session Logs</h3>"
             "<table><tr><th>Time</th><th>Event</th></tr>"
             + enhancedLogTableHTML() + "</table>";

  if (ddnsConfiguration.persistentLogging) {
    content += "<h3>Persistent Logs</h3>";
    content += loadPersistentLogsHTML();
  }

  content += "<p><a href='/clearlogs'><button class='danger'>Clear Session Logs</button></a>";
  if (ddnsConfiguration.persistentLogging) {
    content += " <a href='/clearpersistent'><button class='danger'>Clear Persistent Logs</button></a>";
  }
  content += "</p>";
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
    ddns_lastupdatetime = time(nullptr);
  }
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

void handleForceNtpSync() {
  attemptNtpSync();
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

void handleClearLogs() {
  if (!authenticate()) return;
  for (int i = 0; i < DDNS_LOG_SIZE; i++) {
    ddnsUpdateLog[i] = {0, 0, 0, "", false, 0};
  }
  ddnsLogIdx = 0;
  enhancedLogAdd(1, 2, "Session logs cleared");
  server.sendHeader("Location", "/logs");
  server.send(302, "text/plain", "");
}

void pageApiStatus() {
  String json;
  json.reserve(2048);
  json = "{";
  json += "\"status\":\"" + String(ddns_lastupdatestatus ? "success" : "fail") + "\",";
  json += "\"last_update\":" + String(ddns_lastupdatetime) + ",";
  json += "\"next_update\":" + String(ddns_updatetimer > millis() ? (ddns_updatetimer - millis()) / 1000 : 0) + ",";
  json += "\"ip\":\"" + (WiFi.isConnected() ? WiFi.localIP().toString() : "N/A") + "\",";
  json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  json += "\"uptime\":" + String(millis() / 1000) + ",";
  json += "\"time_synced\":" + String(isTimeSynced() ? "true" : "false") + ",";
  json += "\"ntp_attempts\":" + String(ntpStatus.syncAttempts) + ",";
  json += "\"ntp_last_sync\":" + String(ntpStatus.lastSyncTime) + ",";
  json += "\"persistent_logging\":" + String(ddnsConfiguration.persistentLogging ? "true" : "false") + ",";
  json += "\"log\":[";

  bool firstLog = true;
  for (int i = 0; i < DDNS_LOG_SIZE; i++) {
    int idx = (ddnsLogIdx + i) % DDNS_LOG_SIZE;
    if (ddnsUpdateLog[idx].timestamp == 0 && ddnsUpdateLog[idx].localTime == 0) continue;
    if (!firstLog) json += ",";
    json += "{\"time\":" + String(ddnsUpdateLog[idx].timestamp);
    json += ",\"local_time\":" + String(ddnsUpdateLog[idx].localTime);
    json += ",\"status\":\"" + String(ddnsUpdateLog[idx].status ? "success" : "fail") + "\"";
    json += ",\"type\":\"" + getLogTypeStr(ddnsUpdateLog[idx].logType) + "\"";
    json += ",\"timestamp_valid\":" + String(ddnsUpdateLog[idx].timestampValid ? "true" : "false");
    if (ddnsUpdateLog[idx].errorMessage.length() > 0) {
      json += ",\"error\":\"" + ddnsUpdateLog[idx].errorMessage + "\"";
    }
    json += "}";
    firstLog = false;
  }
  json += "]}";
  server.send(200, "application/json", json);
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

// Enhanced DDNS update function
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

    http.setTimeout(10000); // 10 second timeout
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
    if (isTimeReasonable()) {
      ddns_lastupdatetime = (unsigned long)now;
    }
    enhancedLogAdd(1, 0, "DDNS update successful");
    firstUpdate = false;
    #if defined SERIAL_ENABLED
    Serial.println("DDNS update successful");
    #endif
    http.end();
    return 1;
  } catch (const std::exception& e) {
    error_message = e.what();
    #if defined SERIAL_ENABLED
    Serial.println("DDNS update error: " + error_message);
    #endif
    ddnsStatusLedTicker.attach(0.5, ddnsStatusLedTick);
    ddns_lastupdatestatus = 0;
    enhancedLogAdd(0, 0, error_message);
  }

  http.end();
  return 0;
}

void checkScheduledReboot() {
    static unsigned long lastRebootCheck = 0;

    if (!ddnsConfiguration.enableDailyReboot || !isTimeSynced()) {
        return;
    }

    unsigned long currentMillis = millis();
    if (currentMillis - lastRebootCheck >= REBOOT_CHECK_INTERVAL) {
        lastRebootCheck = currentMillis;

        time_t now;
        struct tm timeinfo;
        time(&now);
        localtime_r(&now, &timeinfo);

        if (timeinfo.tm_hour == ddnsConfiguration.rebootHour &&
            timeinfo.tm_min == ddnsConfiguration.rebootMinute) {
            enhancedLogAdd(1, 2, "Scheduled daily reboot triggered");
            delay(1000);
            ESP.restart();
        }
    }
}


void setup() {
  #if defined SERIAL_ENABLED
  Serial.begin(115200);
  delay(500);
  Serial.println("\nESP32 DuckDNS Client Enhanced v2.1 (Preferences) starting...");
  #endif

  pinMode(WIFIMANAGERSETSTATUS_LED, OUTPUT);
  digitalWrite(WIFIMANAGERSETSTATUS_LED, HIGH);

  pinMode(DDNS_STATUSLED, OUTPUT);
  digitalWrite(DDNS_STATUSLED, HIGH);

  // Initialize variables
  ddns_lastupdatetime = 0;
  ddns_updatetimer = millis();
  ntp_health_check_timer = millis() + NTP_HEALTH_CHECK_INTERVAL;
  firstUpdate = true;

  initNtpStatus();

  for (int i = 0; i < DDNS_LOG_SIZE; i++) {
    ddnsUpdateLog[i] = {0, 0, 0, "", false, 0};
  }
  ddnsLogIdx = 0;

  ddnsPreferencesInit(); // MODIFIED: Initialize from Preferences

  char deviceidc[4];
  snprintf(deviceidc, sizeof(deviceidc), "%03d", ddnsConfiguration.deviceid);
  char hostname[MAX_HOSTNAME_LENGTH];
  snprintf(hostname, sizeof(hostname), "%s%s", HOSTNAME_PREFIX, deviceidc);

  WiFi.setHostname(hostname);
  WiFiManager wifiManager;
  wifiManager.setConfigPortalTimeout(180);
  wifiManager.setDebugOutput(SERIAL_ENABLED);

  wifiManagerSetStatus(WIFIMANAGERSETSTATUS_TRYCONNECTION);
  if (!wifiManager.autoConnect("ESP32-DuckDNS-Enhanced")) {
    enhancedLogAdd(0, 2, "WiFi connection failed");
    ESP.restart();
  }
  wifiManagerSetStatus(WIFIMANAGERSETSTATUS_CONNECTED);
  enhancedLogAdd(1, 2, "WiFi connected: " + WiFi.localIP().toString());

  #if defined SERIAL_ENABLED
  Serial.println("Initializing NTP service...");
  Serial.print("Primary NTP server: ");
  Serial.println(ddnsConfiguration.ntpServer);
  #endif

  time_sync_start = millis();
  attemptNtpSync();

  enhancedLogAdd(1, 2, "System startup");

  if (MDNS.begin(hostname)) {
    #if defined SERIAL_ENABLED
    Serial.println("MDNS responder started");
    #endif
    enhancedLogAdd(1, 2, "MDNS started");
  }

  server.on("/", pageHome);
  server.on("/settings", pageSettings);
  server.on("/logs", pageLogs);
  server.on("/api/status", pageApiStatus);
  server.on("/forcesync", handleForceSync);
  server.on("/forcentp", handleForceNtpSync);
  server.on("/clearlogs", handleClearLogs);
  server.on("/clearpersistent", handleClearPersistentLogs);
  server.onNotFound(pageNotFound);
  server.begin();

  enhancedLogAdd(1, 2, "Web server started on port 80");
  checkconnection_timer = millis();

  #if defined SERIAL_ENABLED
  Serial.println("Setup completed successfully!");
  #endif
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
  checkScheduledReboot();

  server.handleClient();

  if (isTimeExpired(ntp_health_check_timer)) {
    ntp_health_check_timer = millis() + NTP_HEALTH_CHECK_INTERVAL;
    checkNtpHealth();
  }

  if (firstUpdate) {
    if (isTimeSynced()) {
      #if defined SERIAL_ENABLED
      Serial.println("NTP time synchronized, performing first DDNS update");
      #endif
      firstUpdate = false;
      if (ddnsUpdate()) {
        ddns_lastupdatetime = (unsigned long)time(nullptr);
      }
      ddns_updatetimer = millis() + (unsigned long)ddnsConfiguration.updateinterval * 60 * 1000;
    } else if (millis() - time_sync_start > 120000) { // 2 minute timeout
      #if defined SERIAL_ENABLED
      Serial.println("NTP sync timeout - proceeding without time sync");
      #endif
      enhancedLogAdd(0, 1, "NTP initial sync timeout");
      firstUpdate = false;
      ddnsUpdate();
      ddns_updatetimer = millis() + (unsigned long)ddnsConfiguration.updateinterval * 60 * 1000;
    }
    return;
  }

  if (isTimeExpired(ddns_updatetimer)) {
    ddns_updatetimer = millis() + (unsigned long)ddnsConfiguration.updateinterval * 60 * 1000;
    if (ddnsUpdate()) {
      ddns_lastupdatetime = (unsigned long)time(nullptr);
    }
  }
}
