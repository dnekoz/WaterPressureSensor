#include "Portal.h"

#include <ArduinoJson.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <Updater.h>
#include <LittleFS.h>
#include <memory>
#include <new>
#include "CaStore.h"
#include "Config.h"
#include "Display.h"
#include "Mqtt.h"
#include "Sensor.h"
#include "Settings.h"
#include "WebAssets.h"
#include "WifiConnection.h"
#include "WifiDiagnostics.h"

static ESP8266WebServer server(80);
static DNSServer dns;
static WifiConnection s_wifi;
static char s_apName[32];
static bool s_apActive = false;
static bool s_apOffered = false;
static bool s_settingsChanged = false;
static uint32_t s_disconnectedMs = 0;
static uint32_t s_apStartedMs = 0;
static uint32_t s_restartMs = 0;
static bool s_restartPending = false;
static bool s_wifiPending = false;
static uint32_t s_wifiApplyMs = 0;
static char s_wifiSsid[33];
static char s_wifiPass[65];
static char s_session[33] = "";
static uint32_t s_sessionMs = 0;
static uint32_t s_loginAttemptMs = 0;
static bool s_loginAttempted = false;
static bool s_loginPasswordVisible = false;
static bool s_uploadOk = false;
static bool s_uploadStarted = false;
static bool s_uploadFinished = false;
static String s_uploadError;
static std::unique_ptr<UpdaterClass> s_firmware;

static void noCache() {
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("X-Content-Type-Options", "nosniff");
}

static void jsonReply(int status, const JsonDocument &doc) {
  noCache();
  if (doc.overflowed()) { server.send(503, "application/json", "{\"error\":\"Недостаточно памяти\"}"); return; }
  String text;
  serializeJson(doc, text);
  server.send(status, "application/json; charset=utf-8", text);
}

static void reply(int status, const char *message, bool error = false) {
  JsonDocument doc;
  doc[error ? "error" : "message"] = message;
  jsonReply(status, doc);
}

static bool authenticated() {
  if (!s_session[0] || uint32_t(millis() - s_sessionMs) >= 3600000UL) return false;
  String cookies = server.header("Cookie");
  int start = 0;
  while (start < (int)cookies.length()) {
    int end = cookies.indexOf(';', start);
    if (end < 0) end = cookies.length();
    String cookie = cookies.substring(start, end);
    cookie.trim();
    if (cookie == String("wps_session=") + s_session) return true;
    start = end + 1;
  }
  return false;
}

static bool mutationHeader() { return server.header("X-WPS-Request") == "1"; }

static bool authorize(bool mutation = false) {
  if (!authenticated()) { reply(401, "Войдите, чтобы изменить настройки.", true); return false; }
  if (mutation && !mutationHeader()) { reply(403, "Откройте страницу устройства и повторите действие.", true); return false; }
  return true;
}

static bool readJson(JsonDocument &doc) {
  if (!server.hasArg("plain") || server.arg("plain").length() > 2048) {
    reply(400, "Некорректный размер запроса.", true); return false;
  }
  auto error = deserializeJson(doc, server.arg("plain"));
  if (error || !doc.is<JsonObject>()) { reply(400, "Ожидается JSON-объект.", true); return false; }
  return true;
}

static void redrawDisplay() {
  if (!portalDrawIfActive()) {
    displayReading(sensorRead(), portalWifiConnected(), mqttEnabled(), mqttConnected(),
                   mqttTlsActive());
  }
}

static void handleLoginDisplay() {
  // Окно ещё не авторизовано; пароль показывается только на физическом OLED.
  if (!mutationHeader()) { reply(403, "Откройте страницу устройства.", true); return; }
  JsonDocument doc;
  if (!readJson(doc)) return;
  if (!doc["visible"].is<bool>()) {
    reply(400, "Укажите, открыто ли окно входа.", true); return;
  }
  s_loginPasswordVisible = doc["visible"].as<bool>();
  // Обновляем экран до следующей попытки MQTT, которая может задержать loop().
  redrawDisplay();
  reply(200, "Экран обновлён.");
}

static void handleLogin() {
  if (!mutationHeader()) { reply(403, "Откройте страницу устройства.", true); return; }
  if (s_loginAttempted && uint32_t(millis() - s_loginAttemptMs) < 3000) {
    server.sendHeader("Retry-After", "3");
    reply(429, "Подождите 3 секунды перед новой попыткой.", true); return;
  }
  JsonDocument doc;
  if (!readJson(doc)) return;
  s_loginAttempted = true;
  s_loginAttemptMs = millis();
  const char *password = doc["password"] | "";
  if (!settings.portalPass[0] || strcmp(password, settings.portalPass)) {
    reply(401, "Неверный пароль интерфейса.", true); return;
  }
  snprintf(s_session, sizeof(s_session), "%08lx%08lx%08lx%08lx",
           (unsigned long)ESP.random(), (unsigned long)ESP.random(),
           (unsigned long)ESP.random(), (unsigned long)ESP.random());
  s_sessionMs = millis();
  s_loginPasswordVisible = false;
  redrawDisplay();
  server.sendHeader("Set-Cookie", String("wps_session=") + s_session + "; Path=/; HttpOnly; SameSite=Strict; Max-Age=3600");
  reply(200, "Вы вошли в интерфейс.");
}

static void handleLogout() {
  if (!authorize(true)) return;
  s_session[0] = '\0';
  server.sendHeader("Set-Cookie", "wps_session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
  reply(200, "Вы вышли из интерфейса.");
}

static void handleReading() {
  char payload[320];
  if (!mqttStateJson(payload, sizeof(payload), sensorRead(), WiFi.RSSI(), ESP.getFreeHeap())) {
    reply(503, "Измерение пока недоступно.", true); return;
  }
  noCache();
  server.send(200, "application/json", payload);
}

static void handleStatus() {
  JsonDocument doc;
  char reading[320];
  if (!mqttStateJson(reading, sizeof(reading), sensorRead(), WiFi.RSSI(), ESP.getFreeHeap())) {
    reply(503, "Недостаточно памяти для показаний.", true); return;
  }
  doc["reading"] = serialized(reading);
  doc["name"] = settings.deviceName;
  doc["id"] = deviceId();
  doc["version"] = FW_VERSION;
  doc["sdk"] = ESP.getSdkVersion();
  doc["sdk_release"] = WPS_NONOS_SDK_RELEASE;
  doc["uptime_s"] = millis() / 1000;
  doc["max_bar"] = sensorMaxBar();
  doc["heap_block"] = ESP.getMaxFreeBlockSize();
  doc["authenticated"] = authenticated();
  doc["wifi"]["connected"] = s_wifi.connected();
  doc["wifi"]["ssid"] = s_wifi.ssid();
  doc["wifi"]["ip"] = WiFi.localIP().toString();
  doc["wifi"]["ap"] = s_apActive;
  doc["wifi"]["ap_name"] = s_apName;
  doc["wifi"]["ap_ip"] = WiFi.softAPIP().toString();
  doc["mqtt"]["enabled"] = mqttEnabled();
  doc["mqtt"]["connected"] = mqttConnected();
  doc["mqtt"]["tls"] = mqttTlsActive();
  doc["mqtt"]["mode"] = settings.mqttTlsMode;
  doc["mqtt"]["error"] = mqttLastError();
  doc["ca"]["present"] = caExists();
  doc["ca"]["size"] = caSize();
  doc["ca"]["error"] = caLastError();
  doc["tls_error"] = mqttTlsError();
  doc["storage_error"] = settingsLastError();
#if defined(WPS_WIFI_DIAGNOSTICS)
  doc["wifi_diagnostics"]["enabled"] = wifiDiagnosticsEnabled();
  doc["wifi_diagnostics"]["error"] = wifiDiagnosticsError();
#endif
  jsonReply(200, doc);
}

#if defined(WPS_WIFI_DIAGNOSTICS)
static void handleWifiDiagnostics() {
  if (!authorize()) return;
  File previous = LittleFS.open(wifiDiagnosticsPreviousLogPath(), "r");
  File current = LittleFS.open(wifiDiagnosticsLogPath(), "r");
  if (!previous && !current) {
    reply(503, "Журнал диагностики Wi-Fi пока недоступен.", true);
    return;
  }
  noCache();
  server.sendHeader("Content-Disposition", "attachment; filename=wifi-diagnostics.ndjson");
  server.setContentLength((previous ? previous.size() : 0) + (current ? current.size() : 0));
  server.send(200, "application/x-ndjson; charset=utf-8", "");
  if (previous) server.sendContent(previous, previous.size());
  if (current) server.sendContent(current, current.size());
}
#endif

static void handleGetSettings() {
  if (!authorize()) return;
  JsonDocument doc;
  settingsToJson(doc.to<JsonObject>(), settings);
  doc.remove("mqttPass");
  doc.remove("portalPass");
  doc.remove("haPrefixApplied");
  doc["mqttPasswordSet"] = settings.mqttPass[0] != '\0';
  doc["wifiSsid"] = s_wifi.ssid();
  jsonReply(200, doc);
}

static void handleSaveSettings() {
  if (!authorize(true)) return;
  JsonDocument doc;
  if (!readJson(doc)) return;
  Settings candidate = settings;
  char error[192];
  if (!settingsPatch(doc.as<JsonObjectConst>(), candidate, error, sizeof(error))) {
    Serial.printf("[CONFIG][ERROR] Настройки не сохранены: %s\n", error);
    reply(400, error, true); return;
  }
  bool passwordChanged = strcmp(candidate.portalPass, settings.portalPass) != 0;
  if (!settingsCommit(candidate)) { reply(507, settingsLastError(), true); return; }
  s_settingsChanged = true;
  if (passwordChanged) s_session[0] = '\0';
  reply(200, passwordChanged ? "Пароль изменён. Войдите с новым паролем. Пароль точки доступа обновится после перезагрузки." : "Настройки сохранены.");
}

static void handleWifi() {
  if (!authorize(true)) return;
  JsonDocument doc;
  if (!readJson(doc)) return;
  if (!doc["ssid"].is<const char *>() || (doc["password"].isNull() == false && !doc["password"].is<const char *>())) {
    reply(400, "Укажите имя сети и пароль.", true); return;
  }
  const char *ssid = doc["ssid"] | "";
  const char *pass = doc["password"] | "";
  if (doc["keepPassword"] == true) {
    if (strcmp(s_wifi.ssid(), ssid)) { reply(400, "Для новой сети укажите пароль.", true); return; }
    pass = s_wifi.password();
  }
  if (!strlen(ssid) || strlen(ssid) > 32 || strlen(pass) > 63 || (strlen(pass) > 0 && strlen(pass) < 8)) {
    reply(400, "Имя сети: 1–32 байта. Пароль: 8–63 символа или пустой для открытой сети.", true); return;
  }
  snprintf(s_wifiSsid, sizeof(s_wifiSsid), "%s", ssid);
  snprintf(s_wifiPass, sizeof(s_wifiPass), "%s", pass);
  s_wifiPending = true;
  s_wifiApplyMs = millis();
  reply(200, "Подключаемся к сети. Состояние обновится автоматически.");
}

static void handleScanStart() {
  if (!authorize(true)) return;
  if (WiFi.scanComplete() != WIFI_SCAN_RUNNING) {
    WiFi.scanDelete();
    WiFi.scanNetworks(true, true);
  }
  reply(202, "Ищем сети…");
}

static void handleScanResults() {
  if (!authorize()) return;
  JsonDocument doc;
  int count = WiFi.scanComplete();
  doc["running"] = count == WIFI_SCAN_RUNNING;
  JsonArray networks = doc["networks"].to<JsonArray>();
  for (int i = 0; i < count && i < 20; ++i) {
    JsonObject network = networks.add<JsonObject>();
    network["ssid"] = WiFi.SSID(i);
    network["rssi"] = WiFi.RSSI(i);
    network["secure"] = WiFi.encryptionType(i) != ENC_TYPE_NONE;
  }
  jsonReply(200, doc);
}

static void startAp() {
  if (s_apActive) return;
  WiFi.mode(WIFI_AP_STA);
  s_apActive = WiFi.softAP(s_apName, settings.portalPass);
  s_apStartedMs = millis();
  s_apOffered = true;
  if (s_apActive) {
    dns.start(53, "*", WiFi.softAPIP());
    Serial.printf("[wifi] AP %s, http://%s/\n", s_apName, WiFi.softAPIP().toString().c_str());
  } else {
    Serial.println(F("[WIFI][ERROR] Не удалось открыть точку доступа. Проверьте пароль (8–63 символа) и свободную память."));
  }
}

static void startNetworkServices(const char *hostname) {
  // При смене/потере интерфейса сетевые слушатели могут закрыться. Поднимаем
  // их заново и после получения нового IP, не полагаясь на старые сокеты.
  server.begin();
  MDNS.end();
  if (MDNS.begin(hostname)) MDNS.addService("http", "tcp", 80);
  else Serial.println(F("[WIFI][ERROR] mDNS не запущен. Используйте IP устройства."));
  if (s_apActive) {
    dns.stop();
    dns.start(53, "*", WiFi.softAPIP());
  }
}

static void handleCaUpload() {
  HTTPUpload &upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    s_uploadStarted = true;
    s_uploadFinished = false;
    s_uploadError = "";
    s_uploadOk = authenticated() && mutationHeader();
    if (s_uploadOk) s_uploadOk = caWriteBegin();
    else s_uploadError = "Войдите в интерфейс перед загрузкой.";
  } else if (upload.status == UPLOAD_FILE_WRITE && s_uploadOk) {
    s_uploadOk = caWriteChunk(upload.buf, upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END) {
    // Commit выполняется в основном обработчике, когда весь запрос завершён.
    s_uploadFinished = true;
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    caWriteAbort();
    s_uploadOk = false;
    s_uploadError = "Загрузка прервана. Предыдущий сертификат сохранён.";
  }
  sensorSample();
  yield();
}

static void handleCaDone() {
  if (!authorize(true)) { caWriteAbort(); s_uploadStarted = false; return; }
  bool ok = s_uploadStarted && s_uploadFinished && s_uploadOk && caWriteEnd();
  s_uploadStarted = false;
  if (!ok) {
    caWriteAbort();
    String error = s_uploadError.length() ? s_uploadError : String(caLastError());
    reply(400, error.length() ? error.c_str() : "Выберите PEM-сертификат.", true);
    return;
  }
  s_settingsChanged = true;
  reply(200, "CA-сертификат сохранён. MQTT переподключается.");
}

static void handleFirmwareUpload() {
  HTTPUpload &upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    s_uploadStarted = true;
    s_uploadFinished = false;
    s_uploadError = "";
    s_uploadOk = authenticated() && mutationHeader();
    if (s_uploadOk) {
      s_firmware.reset(new (std::nothrow) UpdaterClass());
      if (s_firmware) s_firmware->runAsync(true);
      uint32_t available = ESP.getFreeSketchSpace();
      s_uploadOk = s_firmware && available > 0x1000 && s_firmware->begin((available - 0x1000) & 0xFFFFF000, U_FLASH);
      if (!s_uploadOk) s_uploadError = "Не удалось начать обновление: недостаточно места или памяти.";
    } else s_uploadError = "Войдите в интерфейс перед обновлением.";
  } else if (upload.status == UPLOAD_FILE_WRITE && s_uploadOk) {
    s_uploadOk = s_firmware->write(upload.buf, upload.currentSize) == upload.currentSize;
    if (!s_uploadOk) s_uploadError = "Не удалось записать прошивку.";
  } else if (upload.status == UPLOAD_FILE_END) {
    s_uploadFinished = upload.totalSize > 0;
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    // Destructor освобождает буфер без commit. end(false) может подтвердить
    // полностью записанный файл даже в ветке отмены.
    s_firmware.reset();
    s_uploadOk = false;
    s_uploadError = "Загрузка прошивки прервана.";
  }
  sensorSample();
  yield();
}

static void handleFirmwareDone() {
  if (!authorize(true)) { s_firmware.reset(); s_uploadStarted = false; return; }
  bool ok = s_uploadStarted && s_uploadFinished && s_uploadOk && s_firmware && s_firmware->end(true);
  s_firmware.reset();
  s_uploadStarted = false;
  if (!ok) {
    reply(400, s_uploadError.length() ? s_uploadError.c_str() : "Неверный или неполный файл прошивки.", true);
    return;
  }
  reply(200, "Прошивка записана. Устройство перезагружается.");
  s_restartPending = true;
  s_restartMs = millis();
}

static void serveAsset(const uint8_t *data, size_t length, const char *type, const char *etag, bool html = false) {
  server.sendHeader("X-Content-Type-Options", "nosniff");
  server.sendHeader("Referrer-Policy", "no-referrer");
  server.sendHeader("X-Frame-Options", "DENY");
  server.sendHeader("Cache-Control", html ? "no-cache" : "public, max-age=86400");
  server.sendHeader("ETag", etag);
  if (server.header("If-None-Match") == etag) { server.send(304); return; }
  server.sendHeader("Content-Encoding", "gzip");
  server.send_P(200, type, (const char *)data, length);
}

void portalBegin() {
  snprintf(s_apName, sizeof(s_apName), "WaterSensor-%06x", ESP.getChipId());
  Serial.printf("[portal] пароль интерфейса и точки доступа: %s\n", settings.portalPass);
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  char hostname[32];
  snprintf(hostname, sizeof(hostname), "wps-%06x", ESP.getChipId());
  WiFi.hostname(hostname);
  s_wifi.begin(millis());
  if (!s_wifi.configured()) startAp();
  s_disconnectedMs = millis();
  server.collectHeaders("Cookie", "X-WPS-Request", "If-None-Match");
  server.on("/", HTTP_GET, [] { serveAsset(WEB_INDEX, sizeof(WEB_INDEX), "text/html; charset=utf-8", WEB_INDEX_ETAG, true); });
  server.on("/app.css", HTTP_GET, [] { serveAsset(WEB_CSS, sizeof(WEB_CSS), "text/css; charset=utf-8", WEB_CSS_ETAG); });
  server.on("/app.js", HTTP_GET, [] { serveAsset(WEB_JS, sizeof(WEB_JS), "application/javascript; charset=utf-8", WEB_JS_ETAG); });
  server.on("/reading", HTTP_GET, handleReading);
  server.on("/api/status", HTTP_GET, handleStatus);
#if defined(WPS_WIFI_DIAGNOSTICS)
  server.on("/api/diagnostics/wifi", HTTP_GET, handleWifiDiagnostics);
#endif
  server.on("/api/login", HTTP_POST, handleLogin);
  server.on("/api/login/display", HTTP_POST, handleLoginDisplay);
  server.on("/api/logout", HTTP_POST, handleLogout);
  server.on("/api/settings", HTTP_GET, handleGetSettings);
  server.on("/api/settings", HTTP_POST, handleSaveSettings);
  server.on("/api/wifi", HTTP_POST, handleWifi);
  server.on("/api/wifi/scan", HTTP_POST, handleScanStart);
  server.on("/api/wifi/scan", HTTP_GET, handleScanResults);
  server.on("/api/ca", HTTP_POST, handleCaDone, handleCaUpload);
  server.on("/api/ca/delete", HTTP_POST, [] {
    if (!authorize(true)) return;
    if (!caRemove()) { reply(500, caLastError(), true); return; }
    s_settingsChanged = true;
    reply(200, "Сертификат удалён. Для режима CA загрузите новый сертификат.");
  });
  server.on("/api/firmware", HTTP_POST, handleFirmwareDone, handleFirmwareUpload);
  server.on("/api/restart", HTTP_POST, [] {
    if (!authorize(true)) return;
    reply(200, "Устройство перезагружается.");
    s_restartPending = true;
    s_restartMs = millis();
  });
  server.onNotFound([] {
    if (server.method() != HTTP_GET || server.uri().startsWith("/api/")) {
      reply(404, "Страница не найдена.", true); return;
    }
    String section = "overview";
    if (server.uri() == "/wifi" || server.uri() == "/ca" || server.uri() == "/param") section = "connection";
    if (server.uri() == "/update" || server.uri() == "/info") section = "device";
    server.sendHeader("Location", String("http://") + server.client().localIP().toString() + "/#" + section);
    noCache();
    server.send(302, "text/plain", "");
  });
  startNetworkServices(hostname);
}

void portalLoop() {
  if (s_apActive) dns.processNextRequest();
  server.handleClient();
  MDNS.update();
  uint32_t now = millis();
  if (s_restartPending && uint32_t(now - s_restartMs) > 1200) ESP.restart();
  if (s_wifiPending && uint32_t(now - s_wifiApplyMs) > 300) {
    s_wifiPending = false;
    startAp();
    s_wifi.connect(s_wifiSsid, s_wifiPass, now);
    memset(s_wifiPass, 0, sizeof(s_wifiPass));
    s_disconnectedMs = now;
  }
  s_wifi.loop(millis());
  now = millis();
  bool connected = s_wifi.connected();
  static bool wasConnected = false;
  if (connected != wasConnected) {
    if (connected) {
      char hostname[32];
      snprintf(hostname, sizeof(hostname), "wps-%06x", ESP.getChipId());
      startNetworkServices(hostname);
      Serial.printf("[WIFI][OK] Сеть: %s. Интерфейс: http://%s/\n", s_wifi.ssid(), WiFi.localIP().toString().c_str());
    } else Serial.println(F("[WIFI][LOST] Связь с роутером потеряна. Пробую подключиться снова."));
    wasConnected = connected;
  }
  if (connected) {
    s_disconnectedMs = now;
    s_apOffered = false;
  } else {
    if (!s_apOffered && uint32_t(now - s_disconnectedMs) >= 15000) startAp();
  }
  // startAp() above can yield while switching radio modes. Use a fresh clock:
  // the earlier `now` can precede s_apStartedMs and underflow the unsigned age.
  const uint32_t apNow = millis();
  if (s_apActive && WiFi.softAPgetStationNum() == 0 &&
      ((connected && uint32_t(apNow - s_apStartedMs) > 10000) ||
       (s_wifi.configured() && uint32_t(apNow - s_apStartedMs) > PORTAL_TIMEOUT_S * 1000UL))) {
    dns.stop();
    WiFi.softAPdisconnect(true);
    // softAPdisconnect(true) снимает только AP-бит. Если менеджер держит STA
    // выключенным на 200 мс, закрытие AP не должно включать его раньше срока.
    s_apActive = false;
  }
}

bool portalWifiConnected() { return s_wifi.connected(); }
const char *portalApName() { return s_apName; }
bool portalConsumeSettingsChanged() {
  bool changed = s_settingsChanged;
  s_settingsChanged = false;
  return changed;
}
bool portalDrawIfActive() {
  if (s_loginPasswordVisible) {
    displayLoginPassword(settings.portalPass);
    return true;
  }
  if (!s_apActive || portalWifiConnected()) return false;
  displayPortal(s_apName, settings.portalPass, WiFi.softAPIP().toString().c_str());
  return true;
}
