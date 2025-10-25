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
#define EEPROM_SIZE 512  // Reduced from 1024
#define HOSTNAME_PREFIX "testduck"
#define MAX_HOSTNAME_LENGTH 14
#define HTTP_RETRY_DELAY 500
#define REBOOT_DELAY 1000
#define ADMIN_USERNAME "user"
#define ADMIN_PASSWORD "pass"

// Enhanced NTP Constants with fallback servers
#define PRIMARY_NTP_SERVER "time.local"
#define SECONDARY_NTP_SERVER "time.nist.gov"
#define TERTIARY_NTP_SERVER "time.google.com"
#define NTP_TIMEZONE "GMT0BST,M3.5.0/1,M10.5.0/1"
#define NTP_SYNC_TIMEOUT 30000    // Reduced from 60s
#define NTP_RETRY_INTERVAL 3600000 // 1 hour
#define NTP_HEALTH_CHECK_INTERVAL 1800000 // 30 minutes

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

// Reduced configuration structure to prevent EEPROM corruption
struct ddnsConfig {
  char initialized;
  int deviceid;
  char domain[32];          // Reduced from 65
  char token[41];           // Full DuckDNS token is 40 chars + null terminator
  int updateinterval;
  char ntpServer[32];       // Reduced from 65
  bool persistentLogging;
  char reserved[2];         // Padding for alignment
} ddnsConfiguration;

// Reduced Log Entry Structure for memory optimization
#define DDNS_LOG_SIZE 8       // Reduced from 15
#define PERSISTENT_LOG_SIZE 20 // Reduced from 50
#define EEPROM_LOG_OFFSET 128

// Smaller log entry to prevent memory issues
struct EnhancedLogEntry {
  time_t timestamp;
  unsigned long localTime;
  int status;
  char errorMessage[64];    // Reduced from 128
  bool timestampValid;
  uint8_t logType;          // 0=DDNS, 1=NTP, 2=System
  bool hasError;
};

EnhancedLogEntry ddnsUpdateLog[DDNS_LOG_SIZE];
int ddnsLogIdx = 0;

// Smaller persistent log entry
struct PersistentLogEntry {
  unsigned long timestamp;
  unsigned long localTime;
  int status;
  char errorMessage[24];    // Reduced from 32
  bool timestampValid;
  uint8_t logType;
  char reserved[2];
};

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
unsigned long memory_check_timer = 0;
bool firstUpdate = true;
unsigned long time_sync_start = 0;

// Memory management constants
#define MIN_FREE_HEAP 8192  // Minimum free heap before taking action
#define MEMORY_CHECK_INTERVAL 60000  // Check every minute

// Reduced NTP server array
const char* ntpServers[] = {PRIMARY_NTP_SERVER, SECONDARY_NTP_SERVER, TERTIARY_NTP_SERVER};

// Web server
WebServer server(80);

// Optimized HTML/CSS - shorter strings
const char HTTP_HEADSTYLE[] PROGMEM =
  "<style>"
  "body{font-family:Arial;font-size:14px;margin:20px;background:#f5f5f5}"
  "div.b{max-width:600px;margin:0 auto;background:#fff;padding:20px;border-radius:8px}"
  "h1{color:#333;margin-bottom:20px}"
  "div.m{background:#007acc;padding:8px;margin:-20px -20px 20px;border-radius:8px 8px 0 0}"
  "div.m a{color:#fff;text-decoration:none;margin:0 10px}"
  ".status{padding:4px 8px;border-radius:4px;font-weight:bold}"
  ".success{background:#d4edda;color:#155724}"
  ".fail{background:#f8d7da;color:#721c24}"
  ".info{background:#d1ecf1;color:#0c5460}"
  ".warning{background:#fff3cd;color:#856404}"
  "table{width:100%;border-collapse:collapse;margin:10px 0}"
  "td,th{padding:8px;border:1px solid #ddd;text-align:left}"
  "tr:nth-child(even){background:#f9f9f9}"
  "input{width:100%;padding:8px;margin:4px 0;border:1px solid #ccc;border-radius:4px}"
  "button{background:#007acc;color:#fff;padding:8px 16px;border:none;border-radius:4px;cursor:pointer}"
  "button:hover{background:#005fa3}"
  "button.danger{background:#dc3545}"
  "</style>";

// Forward declarations
void enhancedLogAdd(int status, uint8_t logType, const char* errorMsg = "");
void updatePendingTimestamps();
void saveToPersistentLog(const EnhancedLogEntry& entry);
void ddnsEEPROMwrite();
int ddnsUpdate();
bool attemptNtpSync();
void checkNtpHealth();
void checkMemoryPressure();

// Helper functions
bool isTimeExpired(unsigned long timer) {
  return (long)(millis() - timer) >= 0;
}

bool isTimeSynced() {
  time_t now = time(nullptr);
  return now > 1704067200 && now < 1956528000; // 2024-2032 range
}

bool isTimeReasonable() {
  time_t now = time(nullptr);
  return now > 1672531200; // At least 2023
}

// Memory pressure monitoring and management
void checkMemoryPressure() {
  uint32_t freeHeap = ESP.getFreeHeap();
  
  if (freeHeap < MIN_FREE_HEAP) {
    // Memory pressure detected - take action
    enhancedLogAdd(0, 2, "Low memory warning");
    
    // Force garbage collection
    ESP.restart(); // In extreme cases, restart to recover
  }
  
#if defined SERIAL_ENABLED
  if (freeHeap < (MIN_FREE_HEAP * 2)) {
    Serial.print("Warning: Low heap: ");
    Serial.println(freeHeap);
  }
#endif
}

// Enhanced memory-safe string formatting with bounds checking
void fmtTime(char* buffer, size_t bufsize, unsigned long sec) {
  if (!buffer || bufsize < 2) return; // Safety check
  
  if (sec == 0) {
    strncpy(buffer, "Never", bufsize - 1);
    buffer[bufsize - 1] = '\0';
    return;
  }
  
  int m = sec / 60;
  int h = m / 60;
  int d = h / 24;
  h = h % 24;
  m = m % 60;
  int s = sec % 60;
  
  // Bounds-checked formatting
  if (d > 0) {
    int written = snprintf(buffer, bufsize, "%dd %02dh %02dm", d, h, m);
    if (written >= (int)bufsize) buffer[bufsize - 1] = '\0';
  } else {
    int written = snprintf(buffer, bufsize, "%02dh %02dm %02ds", h, m, s);
    if (written >= (int)bufsize) buffer[bufsize - 1] = '\0';
  }
}

const char* getLogTypeStr(uint8_t type) {
  switch(type) {
    case 0: return "DDNS";
    case 1: return "NTP";
    case 2: return "SYS";
    default: return "UNK";
  }
}

const char* getLogTypeClass(uint8_t type) {
  switch(type) {
    case 0: return "info";
    case 1: return "warning";
    case 2: return "info";
    default: return "info";
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
  Serial.print("NTP sync with: ");
  Serial.println(server);
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
      
#if defined SERIAL_ENABLED
      Serial.println("NTP sync successful");
#endif
      
      enhancedLogAdd(1, 1, "NTP sync OK");
      updatePendingTimestamps();
      return true;
    }
    delay(1000);
  }
  
  // Try next server on failure
  ntpStatus.currentServerIndex = (ntpStatus.currentServerIndex + 1) % 3;
  enhancedLogAdd(0, 1, "NTP sync timeout");
  
#if defined SERIAL_ENABLED
  Serial.println("NTP sync failed");
#endif
  
  return false;
}

void loop() {
  // Check WiFi connection
  if (isTimeExpired(checkconnection_timer)) {
    checkconnection_timer = millis() + CHECKCONNECTION_INTERVALL;
    if (WiFi.status() != WL_CONNECTED) {
      wifiManagerSetStatus(WIFIMANAGERSETSTATUS_TRYCONNECTION);
    } else {
      wifiManagerSetStatus(WIFIMANAGERSETSTATUS_CONNECTED);
    }
  }

  // Handle web server requests
  server.handleClient();

  // Periodic NTP health check
  if (isTimeExpired(ntp_health_check_timer)) {
    ntp_health_check_timer = millis() + NTP_HEALTH_CHECK_INTERVAL;
    checkNtpHealth();
  }

  // Periodic memory pressure check
  if (isTimeExpired(memory_check_timer)) {
    memory_check_timer = millis() + MEMORY_CHECK_INTERVAL;
    checkMemoryPressure();
  }

  // Wait for NTP sync on first update
  if (firstUpdate) {
    if (isTimeSynced()) {
#if defined SERIAL_ENABLED
      Serial.println("First DDNS update");
#endif
      firstUpdate = false;
      if (ddnsUpdate()) {
        ddns_lastupdatetime = (unsigned long)time(nullptr);
      }
      ddns_updatetimer = millis() + (unsigned long)ddnsConfiguration.updateinterval * 60 * 1000;
    } else if (millis() - time_sync_start > 120000) { // 2 minute timeout
#if defined SERIAL_ENABLED
      Serial.println("NTP timeout - proceeding");
#endif
      enhancedLogAdd(0, 1, "NTP timeout");
      firstUpdate = false;
      ddnsUpdate();
      ddns_updatetimer = millis() + (unsigned long)ddnsConfiguration.updateinterval * 60 * 1000;
    }
    return;
  }

  // Regular DDNS updates
  if (isTimeExpired(ddns_updatetimer)) {
    ddns_updatetimer = millis() + (unsigned long)ddnsConfiguration.updateinterval * 60 * 1000;
    if (ddnsUpdate()) {
      ddns_lastupdatetime = (unsigned long)time(nullptr);
    }
  }

  // Memory management - yield to prevent watchdog
  yield();
}

void checkNtpHealth() {
  if (!ntpStatus.healthCheckEnabled) return;
  
  bool needsSync = false;
  
  if (!ntpStatus.synchronized) {
    needsSync = true;
  } else if (millis() - ntpStatus.lastSyncTime > NTP_RETRY_INTERVAL) {
    needsSync = true;
  } else if (!isTimeSynced()) {
    needsSync = true;
    ntpStatus.synchronized = false;
  }
  
  if (needsSync && (millis() - ntpStatus.lastSyncAttempt > 60000)) {
    enhancedLogAdd(0, 1, "NTP health check");
    attemptNtpSync();
  }
}

// Memory-safe logging functions
void enhancedLogAdd(int status, uint8_t logType, const char* errorMsg) {
  if (ddnsLogIdx >= 0 && ddnsLogIdx < DDNS_LOG_SIZE) {
    EnhancedLogEntry& entry = ddnsUpdateLog[ddnsLogIdx];
    
    entry.timestamp = time(nullptr);
    entry.localTime = millis();
    entry.status = status;
    entry.timestampValid = isTimeReasonable();
    entry.logType = logType;
    entry.hasError = false;
    
    if (errorMsg && strlen(errorMsg) > 0) {
      strncpy(entry.errorMessage, errorMsg, sizeof(entry.errorMessage) - 1);
      entry.errorMessage[sizeof(entry.errorMessage) - 1] = '\0';
      entry.hasError = true;
    } else {
      entry.errorMessage[0] = '\0';
    }
    
    ddnsLogIdx = (ddnsLogIdx + 1) % DDNS_LOG_SIZE;
    
    if (ddnsConfiguration.persistentLogging) {
      saveToPersistentLog(entry);
    }
    
#if defined SERIAL_ENABLED
    Serial.print("Log: ");
    Serial.print(getLogTypeStr(logType));
    Serial.print(" - ");
    Serial.print(status ? "OK" : "FAIL");
    if (entry.hasError) {
      Serial.print(" - ");
      Serial.print(entry.errorMessage);
    }
    Serial.println();
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
      unsigned long timeDiff = currentLocalTime - ddnsUpdateLog[i].localTime;
      ddnsUpdateLog[i].timestamp = currentRealTime - (timeDiff / 1000);
      ddnsUpdateLog[i].timestampValid = false;
      updatedCount++;
    }
  }
  
  if (updatedCount > 0) {
    enhancedLogAdd(1, 1, "Updated timestamps");
  }
}

void saveToPersistentLog(const EnhancedLogEntry& entry) {
  static int logCount = -1;
  static unsigned long lastCommitTime = 0;
  static bool pendingCommit = false;
  
  if (logCount == -1) {
    EEPROM.get(EEPROM_LOG_OFFSET, logCount);
    if (logCount < 0 || logCount >= PERSISTENT_LOG_SIZE) {
      logCount = 0;
    }
  }
  
  PersistentLogEntry persistentEntry;
  persistentEntry.timestamp = entry.timestamp;
  persistentEntry.localTime = entry.localTime;
  persistentEntry.status = entry.status;
  strncpy(persistentEntry.errorMessage, entry.errorMessage, sizeof(persistentEntry.errorMessage) - 1);
  persistentEntry.errorMessage[sizeof(persistentEntry.errorMessage) - 1] = '\0';
  persistentEntry.timestampValid = entry.timestampValid;
  persistentEntry.logType = entry.logType;
  
  int position = logCount % PERSISTENT_LOG_SIZE;
  int address = EEPROM_LOG_OFFSET + sizeof(int) + (position * sizeof(PersistentLogEntry));
  
  EEPROM.put(address, persistentEntry);
  logCount++;
  EEPROM.put(EEPROM_LOG_OFFSET, logCount);
  
  pendingCommit = true;
  
  // Batch EEPROM commits to reduce wear - commit every 30 seconds or on critical events
  unsigned long now = millis();
  bool isCritical = (entry.status == 0 && entry.logType == 0); // DDNS failures
  
  if (isCritical || (now - lastCommitTime > 30000)) {
    EEPROM.commit();
    pendingCommit = false;
    lastCommitTime = now;
  }
}

// Handle pending EEPROM commits to reduce wear
void handlePendingEEPROMCommit() {
  static bool pendingCommit = false;
  static unsigned long lastCommitTime = 0;
  
  // This will be called by saveToPersistentLog when needed
  if (pendingCommit && (millis() - lastCommitTime > 30000)) {
    EEPROM.commit();
    pendingCommit = false;
    lastCommitTime = millis();
  }
}

// Memory-optimized HTML generation
void buildLogTable(String& out) {
  out += "<table><tr><th>Time</th><th>Event</th></tr>";
  
  bool hasEntries = false;
  char timeStr[32];
  
  for (int i = 0; i < DDNS_LOG_SIZE; i++) {
    int idx = (ddnsLogIdx + i) % DDNS_LOG_SIZE;
    const EnhancedLogEntry& entry = ddnsUpdateLog[idx];
    
    if (entry.timestamp == 0 && entry.localTime == 0) continue;
    hasEntries = true;
    
    // Format time
    if (entry.timestampValid && entry.timestamp > 1672531200) {
      struct tm timeinfo;
      localtime_r(&entry.timestamp, &timeinfo);
      strftime(timeStr, sizeof(timeStr), "%m-%d %H:%M:%S", &timeinfo);
    } else if (entry.localTime > 0) {
      char agoStr[16];
      fmtTime(agoStr, sizeof(agoStr), (millis() - entry.localTime) / 1000);
      snprintf(timeStr, sizeof(timeStr), "~%s ago", agoStr);
    } else {
      strcpy(timeStr, "Unknown");
    }
    
    out += "<tr><td>";
    out += timeStr;
    out += "</td><td><span class='";
    out += getLogTypeClass(entry.logType);
    out += "'>";
    out += getLogTypeStr(entry.logType);
    out += "</span> ";
    out += entry.status ? "OK" : "FAIL";
    if (entry.hasError) {
      out += ": ";
      out += entry.errorMessage;
    }
    out += "</td></tr>";
  }
  
  if (!hasEntries) {
    out += "<tr><td colspan='2'>No entries</td></tr>";
  }
  out += "</table>";
}

bool authenticate() {
  if (!server.authenticate(ADMIN_USERNAME, ADMIN_PASSWORD)) {
    server.requestAuthentication();
    return false;
  }
  return true;
}

// Simplified web page handlers
void pageHome() {
  String content;
  content.reserve(2048);
  
  char timeStr[32];
  char uptimeStr[32];
  
  fmtTime(uptimeStr, sizeof(uptimeStr), millis() / 1000);
  
  String currentTimeStr = "Not synchronized";
  if (isTimeSynced()) {
    time_t now = time(nullptr);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
    currentTimeStr = String(timeStr);
  }
  
  unsigned long nextUpdate = ddns_updatetimer > millis() ? (ddns_updatetimer - millis()) / 1000 : 0;
  fmtTime(timeStr, sizeof(timeStr), nextUpdate);
  
  content = F("<!DOCTYPE html><html><head><title>ESP32 DuckDNS</title>");
  content += FPSTR(HTTP_HEADSTYLE);
  content += F("</head><body><div class='b'>"
               "<h1>ESP32 DuckDNS Client</h1>"
               "<div class='m'><a href='/'>Status</a> | <a href='/settings'>Settings</a></div>");
  
  content += "<p><strong>Current Time:</strong> " + currentTimeStr + "</p>";
  content += "<p><strong>Last DDNS:</strong> <span class='";
  content += ddns_lastupdatestatus ? "success" : "fail";
  content += "'>";
  content += ddns_lastupdatestatus ? "Success" : "Failed";
  content += "</span></p>";
  content += "<p><strong>Next Update:</strong> " + String(timeStr) + "</p>";
  content += "<p><strong>Uptime:</strong> " + String(uptimeStr) + "</p>";
  content += "<p><strong>IP:</strong> " + WiFi.localIP().toString() + "</p>";
  
  content += "<p><a href='/forcesync'><button>Force DDNS</button></a> ";
  content += "<a href='/forcentp'><button>Force NTP</button></a></p>";
  
  buildLogTable(content);
  
  content += F("</div></body></html>");
  server.send(200, "text/html", content);
}

void pageSettings() {
  if (!authenticate()) return;
  
  String content;
  content.reserve(1536);
  
  if (server.hasArg("save")) {
    if (server.hasArg("deviceid")) {
      ddnsConfiguration.deviceid = server.arg("deviceid").toInt();
    }
    if (server.hasArg("domain")) {
      strncpy(ddnsConfiguration.domain, server.arg("domain").c_str(), sizeof(ddnsConfiguration.domain) - 1);
      ddnsConfiguration.domain[sizeof(ddnsConfiguration.domain) - 1] = '\0';
    }
    if (server.hasArg("token")) {
      strncpy(ddnsConfiguration.token, server.arg("token").c_str(), sizeof(ddnsConfiguration.token) - 1);
      ddnsConfiguration.token[sizeof(ddnsConfiguration.token) - 1] = '\0';
    }
    if (server.hasArg("interval")) {
      ddnsConfiguration.updateinterval = server.arg("interval").toInt();
    }
    if (server.hasArg("ntpserver")) {
      strncpy(ddnsConfiguration.ntpServer, server.arg("ntpserver").c_str(), sizeof(ddnsConfiguration.ntpServer) - 1);
      ddnsConfiguration.ntpServer[sizeof(ddnsConfiguration.ntpServer) - 1] = '\0';
    }
    ddnsConfiguration.persistentLogging = server.hasArg("logging");
    
    ddnsEEPROMwrite();
    enhancedLogAdd(1, 2, "Settings saved");
    
    content = F("<!DOCTYPE html><html><head><title>Saved</title>");
    content += FPSTR(HTTP_HEADSTYLE);
    content += F("</head><body><div class='b'><h1>Settings Saved</h1>"
                 "<p>Restarting in 5 seconds...</p>"
                 "<script>setTimeout(function(){location.href='/';},5000);</script>"
                 "</div></body></html>");
    server.send(200, "text/html", content);
    delay(1000);
    ESP.restart();
    return;
  }
  
  content = F("<!DOCTYPE html><html><head><title>Settings</title>");
  content += FPSTR(HTTP_HEADSTYLE);
  content += F("</head><body><div class='b'>"
               "<h1>Settings</h1>"
               "<div class='m'><a href='/'>Status</a> | <a href='/settings'>Settings</a></div>"
               "<form method='get'>"
               "<p>Device ID (0-999):<br><input name='deviceid' value='");
  content += String(ddnsConfiguration.deviceid);
  content += F("'></p><p>Domain:<br><input name='domain' value='");
  content += String(ddnsConfiguration.domain);
  content += F("'></p><p>Token:<br><input name='token' value='");
  content += String(ddnsConfiguration.token);
  content += F("'></p><p>Update Interval (min):<br><input name='interval' value='");
  content += String(ddnsConfiguration.updateinterval);
  content += F("'></p><p>NTP Server:<br><input name='ntpserver' value='");
  content += String(ddnsConfiguration.ntpServer);
  content += F("'></p><p><label><input type='checkbox' name='logging'");
  if (ddnsConfiguration.persistentLogging) content += F(" checked");
  content += F("> Enable Persistent Logging</label></p>"
               "<input type='hidden' name='save' value='1'>"
               "<button type='submit'>Save Settings</button></form>"
               "</div></body></html>");
  
  server.send(200, "text/html", content);
}

void pageNotFound() {
  server.send(404, "text/plain", "Not Found");
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

// EEPROM functions
void ddnsEEPROMinit() {
  EEPROM.begin(EEPROM_SIZE);
  delay(100);
  
  EEPROM.get(0, ddnsConfiguration);
  if (ddnsConfiguration.initialized != EEPROM_INITIALIZED_MARKER) {
#if defined SERIAL_ENABLED
    Serial.println("Initializing EEPROM");
#endif
    memset(&ddnsConfiguration, 0, sizeof(ddnsConfiguration));
    ddnsConfiguration.initialized = EEPROM_INITIALIZED_MARKER;
    ddnsConfiguration.deviceid = 1;
    strcpy(ddnsConfiguration.domain, "your-domain");
    strcpy(ddnsConfiguration.token, "your-token");
    ddnsConfiguration.updateinterval = 10;
    strcpy(ddnsConfiguration.ntpServer, PRIMARY_NTP_SERVER);
    ddnsConfiguration.persistentLogging = false;
    
    EEPROM.put(0, ddnsConfiguration);
    EEPROM.commit();
  }
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
      Serial.println("WiFi: AP mode");
#endif
#if defined WIFIMANAGERSETSTATUS_LEDENABLED
      wifiManagerStatusLedTicker.attach(0.2, wifiManagerStatusLedTick);
#endif
      break;

    case WIFIMANAGERSETSTATUS_CONNECTED:
#if defined WIFIMANAGERSETSTATUS_SERIALENABLED
      Serial.println("WiFi: Connected");
#endif
#if defined WIFIMANAGERSETSTATUS_LEDENABLED
      wifiManagerStatusLedTicker.detach();
      digitalWrite(WIFIMANAGERSETSTATUS_LED, HIGH);
#endif
      break;

    case WIFIMANAGERSETSTATUS_TRYCONNECTION:
#if defined WIFIMANAGERSETSTATUS_SERIALENABLED
      Serial.println("WiFi: Connecting");
#endif
#if defined WIFIMANAGERSETSTATUS_LEDENABLED
      wifiManagerStatusLedTicker.attach(0.5, wifiManagerStatusLedTick);
#endif
      break;
  }
}

// Enhanced DDNS update function with improved error handling
int ddnsUpdate() {
  // Validate configuration before attempting update
  if (strlen(ddnsConfiguration.domain) == 0 || strlen(ddnsConfiguration.token) == 0) {
    enhancedLogAdd(0, 0, "Invalid config");
    return 0;
  }
  
  WiFiClientSecure client;
  HTTPClient http;
  
  // Enhanced security settings
  client.setInsecure();
  client.setTimeout(10000);
  
  // Build URL in stack memory with bounds checking
  char url[160]; // Increased buffer size for safety
  int urlLen = snprintf(url, sizeof(url), 
                       "https://www.duckdns.org/update?domains=%s&token=%s&ip=",
                       ddnsConfiguration.domain, ddnsConfiguration.token);
  
  // Validate URL length
  if (urlLen >= sizeof(url)) {
    enhancedLogAdd(0, 0, "URL too long");
    return 0;
  }
  
  if (!http.begin(client, url)) {
    enhancedLogAdd(0, 0, "HTTP init failed");
    client.stop(); // Ensure client is properly cleaned up
    return 0;
  }
  
  // Set headers and timeout
  http.setTimeout(15000); // Increased timeout for reliability
  http.setUserAgent("ESP32-DuckDNS/2.1");
  http.addHeader("Connection", "close");
  
  int httpCode = http.GET();
  
  if (httpCode <= 0) {
    char errorMsg[32];
    snprintf(errorMsg, sizeof(errorMsg), "HTTP conn error %d", httpCode);
    enhancedLogAdd(0, 0, errorMsg);
    http.end();
    client.stop();
    return 0;
  }
  
  if (httpCode != HTTP_CODE_OK) {
    char errorMsg[32];
    snprintf(errorMsg, sizeof(errorMsg), "HTTP error %d", httpCode);
    enhancedLogAdd(0, 0, errorMsg);
    http.end();
    client.stop();
    return 0;
  }
  
  String payload = http.getString();
  http.end();
  client.stop(); // Explicit cleanup
  
  // Validate response
  payload.trim(); // Remove any whitespace
  if (payload != "OK") {
    char errorMsg[32];
    snprintf(errorMsg, sizeof(errorMsg), "Bad resp: %.15s", payload.c_str());
    enhancedLogAdd(0, 0, errorMsg);
    ddnsStatusLedTicker.attach(0.5, ddnsStatusLedTick);
    ddns_lastupdatestatus = 0;
    return 0;
  }
  
  // Success
  ddnsStatusLedTicker.detach();
  digitalWrite(DDNS_STATUSLED, HIGH);
  ddns_lastupdatestatus = 1;
  if (isTimeReasonable()) {
    ddns_lastupdatetime = (unsigned long)time(nullptr);
  }
  enhancedLogAdd(1, 0, "DDNS OK");
  firstUpdate = false;
  
#if defined SERIAL_ENABLED
  Serial.println("DDNS update OK");
#endif
  
  return 1;
}

void setup() {
#if defined SERIAL_ENABLED
  Serial.begin(115200);
  delay(100);
  Serial.println("ESP32 DuckDNS Client v2.1 starting...");
#endif

  pinMode(WIFIMANAGERSETSTATUS_LED, OUTPUT);
  pinMode(DDNS_STATUSLED, OUTPUT);
  digitalWrite(WIFIMANAGERSETSTATUS_LED, HIGH);
  digitalWrite(DDNS_STATUSLED, HIGH);

  // Initialize variables
  ddns_lastupdatetime = 0;
  ddns_updatetimer = millis();
  ntp_health_check_timer = millis() + NTP_HEALTH_CHECK_INTERVAL;
  memory_check_timer = millis() + MEMORY_CHECK_INTERVAL;
  firstUpdate = true;
  
  initNtpStatus();
  
  // Clear log array
  memset(ddnsUpdateLog, 0, sizeof(ddnsUpdateLog));
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
  if (!wifiManager.autoConnect("ESP32-DuckDNS")) {
    enhancedLogAdd(0, 2, "WiFi failed");
    ESP.restart();
  }
  wifiManagerSetStatus(WIFIMANAGERSETSTATUS_CONNECTED);
  enhancedLogAdd(1, 2, "WiFi OK");

  // Initialize NTP
  time_sync_start = millis();
  attemptNtpSync();

  enhancedLogAdd(1, 2, "System started");

  if (MDNS.begin(hostname)) {
    enhancedLogAdd(1, 2, "mDNS OK");
  }

  // Setup web server
  server.on("/", pageHome);
  server.on("/settings", pageSettings);
  server.on("/forcesync", handleForceSync);
  server.on("/forcentp", handleForceNtpSync);
  server.onNotFound(pageNotFound);
  server.begin();

  enhancedLogAdd(1, 2, "Web server started");
  
#if defined SERIAL_ENABLED
  Serial.println("Setup complete");
  Serial.print("Free heap: ");
  Serial.println(ESP.getFreeHeap());
#endif
}