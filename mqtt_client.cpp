#include "mqtt_client.h"
#include <WiFi.h>
#include <PubSubClient.h>   // thu vien "PubSubClient" (Nick O'Leary)
#include <ArduinoJson.h>    // thu vien "ArduinoJson" v7 (Benoit Blanchon)
#include <sys/time.h>
#include <time.h>
#include "config.h"
#include "types.h"
#include "navigation.h"
#include "battery.h"
#include "wifi_manager.h"

namespace {

// Moc 2023-11-14: gio truoc moc nay nghia la chua dong bo
const time_t TIME_VALID_AFTER = 1700000000;

WiFiClient net;
PubSubClient mqtt(net);
void (*commandHook)() = nullptr;

String clientId;
String topicCommands;
String topicTelemetry;
String topicAvailability;
String topicAcks;
String topicTaskEvents;

// Task dang chay (backend gui taskId kem lenh START theo lich). 0 = khong co task.
uint32_t currentTaskId = 0;
// Task da xong nhung chua bao duoc len backend (mat ket noi / chua co gio) -> gui lai
uint32_t pendingTaskId = 0;
char pendingFinishedAt[32] = "";

bool triedConnect = false;
bool wasConnected = false;
uint32_t lastConnectTryMs = 0;
uint32_t lastTelemetryMs = 0;
uint32_t lastNoTimeWarnMs = 0;
RobotMode lastSentMode = MODE_IDLE;

// QoS 1 co the gui lai cung 1 lenh -> nho cac commandId gan day de bo qua
const uint8_t SEEN_LEN = 8;
String seenIds[SEEN_LEN];
uint8_t seenNext = 0;

String robotTopic(const char* suffix) {
  return String(MQTT_TOPIC_PREFIX "/robots/") + MQTT_ROBOT_ID + "/" + suffix;
}

// ------------------------------------------------------------
//  Thoi gian (backend dung java.time.Instant, UTC)
// ------------------------------------------------------------

bool timeValid() {
  return time(nullptr) > TIME_VALID_AFTER;
}

// vd 2026-10-31T08:15:30.123Z
bool formatNow(char* buf, size_t len) {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  if (tv.tv_sec <= TIME_VALID_AFTER) return false;
  struct tm t;
  gmtime_r(&tv.tv_sec, &t);
  size_t n = strftime(buf, len, "%Y-%m-%dT%H:%M:%S", &t);
  snprintf(buf + n, len - n, ".%03ldZ", (long)(tv.tv_usec / 1000));
  return true;
}

// So ngay tinh tu 1970-01-01 (thuat toan days_from_civil, lich Gregory)
int64_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const int yoe = y - era * 400;
  const int doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int64_t)era * 146097 + doe - 719468;
}

// Nhan ca chuoi ISO-8601 UTC ("2026-10-31T08:15:30.123456Z") lan so giay epoch
bool parseInstant(JsonVariantConst v, time_t& out) {
  if (v.is<double>()) {
    out = (time_t)v.as<double>();
    return true;
  }
  const char* s = v.as<const char*>();
  if (!s) return false;
  int Y, M, D, h, m, sec;
  if (sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d", &Y, &M, &D, &h, &m, &sec) != 6) return false;
  out = (time_t)(daysFromCivil(Y, M, D) * 86400 + h * 3600 + m * 60 + sec);
  return true;
}

// ------------------------------------------------------------
//  Gui di
// ------------------------------------------------------------

void publishJson(const String& topic, JsonDocument& doc, bool retained) {
  char buf[256];
  size_t n = serializeJson(doc, buf, sizeof(buf));
  mqtt.publish(topic.c_str(), (const uint8_t*)buf, n, retained);
}

void publishTelemetry() {
  char sentAt[32];
  if (!formatNow(sentAt, sizeof(sentAt))) {
    uint32_t now = millis();
    if (now - lastNoTimeWarnMs >= 10000) {
      lastNoTimeWarnMs = now;
      Serial.println("[MQTT] Chua co gio NTP -> chua gui telemetry (kiem tra NTP_SERVER trong config.h)");
    }
    return;
  }

  RobotMode m = navGetMode();
  JsonDocument doc;
  doc["batteryPercent"] = batteryPercent();
  doc["status"] = m == MODE_IDLE ? "IDLE" : "WORKING";
  if (m == MODE_AUTO)   doc["mode"] = "AUTO";
  if (m == MODE_MANUAL) doc["mode"] = "MANUAL";
  doc["sentAt"] = sentAt;
  // Backend tu choi telemetry retained
  publishJson(topicTelemetry, doc, false);
}

void publishAck(const char* commandId, const char* result, const char* reason) {
  JsonDocument doc;
  doc["commandId"] = commandId;
  doc["result"] = result;
  if (reason) doc["reason"] = reason;
  char sentAt[32];
  if (formatNow(sentAt, sizeof(sentAt))) doc["sentAt"] = sentAt;
  publishJson(topicAcks, doc, false);
}

// Robot da dung -> task hien tai coi nhu xong, cho gui COMPLETED len backend
void finishCurrentTask() {
  if (currentTaskId == 0) return;
  pendingTaskId = currentTaskId;
  currentTaskId = 0;
  // Ghi lai thoi diem dung that; chua co gio thi lay luc gui duoc
  if (!formatNow(pendingFinishedAt, sizeof(pendingFinishedAt))) pendingFinishedAt[0] = '\0';
  Serial.printf("[MQTT] Task %u hoan thanh\n", pendingTaskId);
}

void publishPendingTask() {
  if (pendingTaskId == 0) return;
  if (!pendingFinishedAt[0] && !formatNow(pendingFinishedAt, sizeof(pendingFinishedAt))) return;
  JsonDocument doc;
  doc["taskId"] = pendingTaskId;
  doc["status"] = "COMPLETED";
  doc["finishedAt"] = pendingFinishedAt;
  publishJson(topicTaskEvents, doc, false);
  Serial.printf("[MQTT] Da bao task %u COMPLETED\n", pendingTaskId);
  pendingTaskId = 0;
  pendingFinishedAt[0] = '\0';
}

// ------------------------------------------------------------
//  Nhan lenh
// ------------------------------------------------------------

bool alreadySeen(const char* id) {
  for (const String& s : seenIds) {
    if (s == id) return true;
  }
  return false;
}

void remember(const char* id) {
  seenIds[seenNext] = id;
  seenNext = (seenNext + 1) % SEEN_LEN;
}

bool parseDirection(const char* dir, ManualCmd& out) {
  if (!strcmp(dir, "FORWARD"))  { out = MANUAL_FORWARD;  return true; }
  if (!strcmp(dir, "BACKWARD")) { out = MANUAL_BACKWARD; return true; }
  if (!strcmp(dir, "LEFT"))     { out = MANUAL_LEFT;     return true; }
  if (!strcmp(dir, "RIGHT"))    { out = MANUAL_RIGHT;    return true; }
  return false;
}

// Tra ve nullptr neu thanh cong, nguoc lai la ly do tu choi
const char* applyCommand(const char* action, const char* mode, const char* dir, uint32_t taskId) {
  if (!strcmp(action, "START") && taskId != 0) {
    finishCurrentTask();  // task cu chua duoc STOP -> dong lai truoc khi nhan task moi
    currentTaskId = taskId;
  }
  if (!strcmp(action, "START") || !strcmp(action, "SET_MODE")) {
    if (!strcmp(mode, "AUTO"))   { navStartAuto();           return nullptr; }
    if (!strcmp(mode, "MANUAL")) { navManual(MANUAL_STOP);   return nullptr; }
    return "invalid mode";
  }
  if (!strcmp(action, "MOVE")) {
    ManualCmd cmd;
    if (!parseDirection(dir, cmd)) return "invalid direction";
    navManual(cmd);
    return nullptr;
  }
  if (!strcmp(action, "STOP_MOVEMENT")) {
    // MANUAL: dung banh nhung giu che do; AUTO: dung han
    if (navGetMode() == MODE_MANUAL) navManual(MANUAL_STOP);
    else { navStop(); finishCurrentTask(); }
    return nullptr;
  }
  if (!strcmp(action, "STOP")) {
    navStop();
    finishCurrentTask();
    return nullptr;
  }
  if (!strcmp(action, "RETURN_TO_DOCK")) {
    navStop();
    finishCurrentTask();
    return "docking not supported, robot stopped";
  }
  return "unknown action";
}

void handleCommand(const uint8_t* payload, unsigned int len) {
  JsonDocument doc;
  if (deserializeJson(doc, payload, len)) {
    Serial.println("[MQTT] Lenh khong phai JSON hop le -> bo qua");
    return;
  }
  const char* id     = doc["commandId"] | "";
  const char* action = doc["action"] | "";
  const char* mode   = doc["mode"] | "";
  const char* dir    = doc["direction"] | "";
  uint32_t taskId    = doc["taskId"] | 0u;

  if (*id && alreadySeen(id)) return;

  time_t issuedAt, expiresAt;
  if (!timeValid() && parseInstant(doc["issuedAt"].as<JsonVariantConst>(), issuedAt)) {
    struct timeval tv = {issuedAt, 0};
    settimeofday(&tv, nullptr);
    Serial.println("[MQTT] Chua co NTP -> lay gio tu issuedAt cua lenh");
  }
  if (timeValid() && parseInstant(doc["expiresAt"].as<JsonVariantConst>(), expiresAt) && time(nullptr) > expiresAt) {
    Serial.printf("[MQTT] Lenh %s da het han -> bo qua\n", action);
    publishAck(id, "EXPIRED", nullptr);
    return;
  }
  if (*id) remember(id);

  Serial.printf("[MQTT] Lenh: %s %s%s\n", action, mode, dir);
  const char* err = applyCommand(action, mode, dir, taskId);
  if (err) Serial.printf("[MQTT] Tu choi lenh: %s\n", err);
  publishAck(id, err ? "REJECTED" : "APPLIED", err);

  if (commandHook) commandHook();
  lastTelemetryMs = 0;  // bao trang thai moi ngay o lan mqttUpdate() ke tiep
}

void onMessage(char* topic, uint8_t* payload, unsigned int len) {
  if (topicCommands == topic) handleCommand(payload, len);
}

// ------------------------------------------------------------
//  Ket noi
// ------------------------------------------------------------

void tryConnect(uint32_t now) {
  if (triedConnect && now - lastConnectTryMs < MQTT_RETRY_MS) return;
  triedConnect = true;
  lastConnectTryMs = now;

  // Tu mo TCP voi timeout ngan: neu de PubSubClient tu mo, loop() co the bi chan
  // vai giay -> robot dang chay AUTO se khong tranh vat can trong luc do.
  IPAddress ip;
  ip.fromString(MQTT_BROKER_IP);
  if (!net.connected() && !net.connect(ip, MQTT_BROKER_PORT, MQTT_CONNECT_TIMEOUT_MS)) {
    Serial.printf("[MQTT] Khong ket noi duoc broker %s:%d\n", MQTT_BROKER_IP, MQTT_BROKER_PORT);
    return;
  }

  // Last will: robot mat ket noi dot ngot -> broker tu bao "offline"
  if (!mqtt.connect(clientId.c_str(), nullptr, nullptr,
                    topicAvailability.c_str(), 1, true, "offline")) {
    Serial.printf("[MQTT] Broker tu choi ket noi (state=%d)\n", mqtt.state());
    net.stop();
    return;
  }

  mqtt.publish(topicAvailability.c_str(), "online", true);
  mqtt.subscribe(topicCommands.c_str(), 1);
  Serial.printf("[MQTT] Da ket noi broker, nghe lenh tai %s\n", topicCommands.c_str());
  lastTelemetryMs = 0;
}

}  // namespace

void mqttInit(void (*onCommand)()) {
  commandHook = onCommand;

  clientId          = String("robot-vacuum-") + MQTT_ROBOT_ID;
  topicCommands     = robotTopic("commands");
  topicTelemetry    = robotTopic("telemetry");
  topicAvailability = robotTopic("availability");
  topicAcks         = robotTopic("acknowledgements");
  topicTaskEvents   = robotTopic("task-events");

  // UTC, khong gio mua he. SNTP tu dong bo khi co WiFi.
  configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2);

  IPAddress ip;
  ip.fromString(MQTT_BROKER_IP);
  mqtt.setServer(ip, MQTT_BROKER_PORT);
  mqtt.setCallback(onMessage);
  mqtt.setBufferSize(512);   // mac dinh 256 byte, lenh JSON co the dai hon
  mqtt.setKeepAlive(MQTT_KEEPALIVE_S);
  mqtt.setSocketTimeout(2);
}

void mqttUpdate() {
  uint32_t now = millis();
  bool connected = mqtt.connected();

  if (wasConnected && !connected) {
    Serial.println("[MQTT] Mat ket noi broker");
    // Dang dieu khien tay tu xa ma mat ket noi -> dung, tranh robot chay mai khong ai dieu khien
    if (navGetMode() == MODE_MANUAL) navManual(MANUAL_STOP);
  }
  wasConnected = connected;

  if (!connected) {
    if (wifiConnected()) tryConnect(now);
    return;
  }

  mqtt.loop();
  publishPendingTask();

  RobotMode m = navGetMode();
  if (lastTelemetryMs == 0 || m != lastSentMode || now - lastTelemetryMs >= MQTT_TELEMETRY_INTERVAL_MS) {
    lastSentMode = m;
    lastTelemetryMs = now ? now : 1;
    publishTelemetry();
  }
}

bool mqttConnected() {
  return mqtt.connected();
}
