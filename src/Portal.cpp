#include "Portal.h"

#include <ESP8266WiFi.h>
#include <WiFiManager.h>

#include "Config.h"
#include "Display.h"
#include "Settings.h"

static WiFiManager wm;

static char s_apName[32];
static bool s_settingsChanged = false;
static unsigned long s_lastConnectedMs = 0;

// Поля портала. Это файловые статики, а не локальные переменные: фоновый
// веб-портал живёт всё время работы устройства и обращается к ним по указателю,
// который WiFiManager сохранил в addParameter(). Локальные объекты дали бы
// висячие указатели после выхода из portalBegin().
//
// Значения проставляются в portalBegin() через setValue(), потому что на момент
// статической инициализации настройки из LittleFS ещё не прочитаны.

static WiFiManagerParameter pMqttHead("<hr><h3>MQTT</h3>");
static WiFiManagerParameter pMqttHost("mqtt_host", "Адрес брокера (пусто = MQTT выключен)", "", 63);
static WiFiManagerParameter pMqttPort("mqtt_port", "Порт", "", 5, "type=\"number\" min=\"1\" max=\"65535\"");
static WiFiManagerParameter pMqttUser("mqtt_user", "Пользователь", "", 31);
static WiFiManagerParameter pMqttPass("mqtt_pass", "Пароль", "", 63, "type=\"password\"");
static WiFiManagerParameter pMqttBase("mqtt_base", "Базовый топик", "", 63);
static WiFiManagerParameter pMqttInt("mqtt_int", "Период публикации, с", "", 5, "type=\"number\" min=\"1\" max=\"3600\"");
static WiFiManagerParameter pDevName("dev_name", "Имя устройства в Home Assistant", "", 31);
static WiFiManagerParameter pHaPrefix("ha_prefix", "Префикс discovery (пусто = без Home Assistant)", "", 31);

static WiFiManagerParameter pCalHead("<hr><h3>Калибровка датчика</h3>");
static WiFiManagerParameter pAdcFs("adc_fs", "Напряжение на A0 при отсчёте 1023, В", "", 7, "type=\"number\" step=\"0.01\"");
static WiFiManagerParameter pVZero("v_zero", "Выход датчика при нулевом давлении, В", "", 7, "type=\"number\" step=\"0.01\"");
static WiFiManagerParameter pVFull("v_full", "Выход датчика при полной шкале, В", "", 7, "type=\"number\" step=\"0.01\"");
static WiFiManagerParameter pPFull("p_full", "Полная шкала датчика, МПа", "", 7, "type=\"number\" step=\"0.01\"");

const char *portalApName() { return s_apName; }

static void setFloatParam(WiFiManagerParameter &p, float value, int length) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%.2f", value);
  p.setValue(buf, length);
}

static void setIntParam(WiFiManagerParameter &p, long value, int length) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%ld", value);
  p.setValue(buf, length);
}

// Переносит текущие настройки в поля формы, чтобы портал показывал реальные
// значения (в том числе поправленные валидацией).
static void syncParamsFromSettings() {
  pMqttHost.setValue(settings.mqttHost, 63);
  setIntParam(pMqttPort, settings.mqttPort, 5);
  pMqttUser.setValue(settings.mqttUser, 31);
  pMqttPass.setValue(settings.mqttPass, 63);
  pMqttBase.setValue(settings.mqttBase, 63);
  setIntParam(pMqttInt, settings.mqttIntervalS, 5);
  pDevName.setValue(settings.deviceName, 31);
  pHaPrefix.setValue(settings.haPrefix, 31);

  setFloatParam(pAdcFs, settings.adcFullScaleV, 7);
  setFloatParam(pVZero, settings.vZero, 7);
  setFloatParam(pVFull, settings.vFull, 7);
  setFloatParam(pPFull, settings.pFullMPa, 7);
}

static void copyParam(char *dst, size_t size, const char *src) {
  strncpy(dst, src ? src : "", size - 1);
  dst[size - 1] = '\0';
}

// Забирает значения из формы. Пустые и бессмысленные значения приводит в
// порядок settingsSanitize() — кроме haPrefix, пустота которого осмысленна.
static void readParamsIntoSettings() {
  copyParam(settings.mqttHost, sizeof(settings.mqttHost), pMqttHost.getValue());
  settings.mqttPort = (uint16_t)atoi(pMqttPort.getValue());
  copyParam(settings.mqttUser, sizeof(settings.mqttUser), pMqttUser.getValue());
  copyParam(settings.mqttPass, sizeof(settings.mqttPass), pMqttPass.getValue());
  copyParam(settings.mqttBase, sizeof(settings.mqttBase), pMqttBase.getValue());
  settings.mqttIntervalS = (uint16_t)atoi(pMqttInt.getValue());
  copyParam(settings.deviceName, sizeof(settings.deviceName), pDevName.getValue());
  copyParam(settings.haPrefix, sizeof(settings.haPrefix), pHaPrefix.getValue());

  settings.adcFullScaleV = atof(pAdcFs.getValue());
  settings.vZero         = atof(pVZero.getValue());
  settings.vFull         = atof(pVFull.getValue());
  settings.pFullMPa      = atof(pPFull.getValue());
}

static void onSaveParams() {
  readParamsIntoSettings();
  settingsSanitize(settings, deviceId());
  syncParamsFromSettings();
  settingsSave();
  s_settingsChanged = true;
  Serial.println(F("[portal] настройки с портала применены"));
}

static void onApMode(WiFiManager *mgr) {
  Serial.printf("[portal] режим настройки: AP %s, http://%s/\n", s_apName,
                WiFi.softAPIP().toString().c_str());
  displayPortal(s_apName, PORTAL_AP_PASSWORD, WiFi.softAPIP().toString().c_str());
}

void portalBegin() {
  snprintf(s_apName, sizeof(s_apName), "WaterSensor-%06x", ESP.getChipId());

  syncParamsFromSettings();

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);

  wm.addParameter(&pMqttHead);
  wm.addParameter(&pMqttHost);
  wm.addParameter(&pMqttPort);
  wm.addParameter(&pMqttUser);
  wm.addParameter(&pMqttPass);
  wm.addParameter(&pMqttBase);
  wm.addParameter(&pMqttInt);
  wm.addParameter(&pDevName);
  wm.addParameter(&pHaPrefix);
  wm.addParameter(&pCalHead);
  wm.addParameter(&pAdcFs);
  wm.addParameter(&pVZero);
  wm.addParameter(&pVFull);
  wm.addParameter(&pPFull);

  wm.setTitle(F("Water Pressure Sensor"));

  // "param" выносит настройки на отдельную страницу Setup. Так их можно менять
  // потом по IP устройства, не трогая страницу WiFi и не рискуя затереть
  // сохранённые учётные данные сети.
  std::vector<const char *> menu = {"wifi", "param", "info", "sep", "restart", "exit"};
  wm.setMenu(menu);

  wm.setSaveParamsCallback(onSaveParams);
  wm.setSaveConfigCallback(onSaveParams);
  wm.setAPCallback(onApMode);

  char hostname[32];
  snprintf(hostname, sizeof(hostname), "wps-%06x", ESP.getChipId());
  wm.setHostname(hostname);

  // Ключевая правка против прежнего поведения: без таймаута устройство висело
  // в портале вечно и переставало измерять, если WiFi пропал.
  wm.setConfigPortalTimeout(PORTAL_TIMEOUT_S);
  wm.setBreakAfterConfig(true);

  displayMessage("WiFi...", "");
  bool connected = wm.autoConnect(s_apName, PORTAL_AP_PASSWORD);

  if (connected) {
    Serial.printf("[wifi] подключено к %s, IP %s\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    displayMessage("WiFi OK", WiFi.localIP().toString().c_str());
  } else {
    Serial.println(F("[wifi] не подключено, работаю офлайн"));
    // Строки для OLED только ASCII: у FreeSans9pt7b нет кириллических глифов.
    displayMessage("WiFi failed", "offline mode");
  }

  // Портал остаётся доступен по IP устройства всё время работы: MQTT и
  // калибровку можно править без перепрошивки и без сброса настроек.
  wm.startWebPortal();

  s_lastConnectedMs = millis();
}

void portalLoop() {
  wm.process();

  if (WiFi.status() == WL_CONNECTED) {
    s_lastConnectedMs = millis();
    return;
  }

  if (WIFI_LOST_RESTART_MIN > 0 &&
      millis() - s_lastConnectedMs > (unsigned long)WIFI_LOST_RESTART_MIN * 60000UL) {
    Serial.println(F("[wifi] связи нет слишком долго, перезагружаюсь"));
    ESP.restart();
  }
}

bool portalWifiConnected() { return WiFi.status() == WL_CONNECTED; }

bool portalConsumeSettingsChanged() {
  bool changed = s_settingsChanged;
  s_settingsChanged = false;
  return changed;
}
