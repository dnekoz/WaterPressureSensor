#include "Settings.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

#include "Config.h"

static const char *CONFIG_PATH = "/config.json";

Settings settings;

static char s_deviceId[16];

const char *deviceId() { return s_deviceId; }

// Копирует строку в поле фиксированной длины, всегда оставляя завершающий ноль.
static void copyField(char *dst, size_t size, const char *src) {
  strncpy(dst, src ? src : "", size - 1);
  dst[size - 1] = '\0';
}

// Перезаписывает поле, только если ключ реально есть в JSON: иначе оставляем
// значение по умолчанию (и не копируем строку саму в себя).
static void loadField(char *dst, size_t size, JsonVariantConst v) {
  if (v.is<const char *>()) copyField(dst, size, v.as<const char *>());
}

void settingsBegin() {
  snprintf(s_deviceId, sizeof(s_deviceId), "wps_%06x", ESP.getChipId());

  settingsDefaults(settings, s_deviceId);

  if (!LittleFS.begin()) {
    Serial.println(F("[cfg] LittleFS не смонтирована, форматирую"));
    LittleFS.format();
    if (!LittleFS.begin()) {
      Serial.println(F("[cfg] LittleFS недоступна, работаю на значениях по умолчанию"));
      return;
    }
  }

  File f = LittleFS.open(CONFIG_PATH, "r");
  if (!f) {
    Serial.println(F("[cfg] config.json нет, беру значения по умолчанию"));
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();

  if (err) {
    Serial.printf("[cfg] config.json повреждён (%s), беру значения по умолчанию\n", err.c_str());
    settingsDefaults(settings, s_deviceId);
    return;
  }

  // Каждое поле необязательно: старый config.json без новых ключей остаётся читаемым.
  loadField(settings.mqttHost, sizeof(settings.mqttHost), doc["mqttHost"]);
  settings.mqttPort = doc["mqttPort"] | settings.mqttPort;
  loadField(settings.mqttUser, sizeof(settings.mqttUser), doc["mqttUser"]);
  loadField(settings.mqttPass, sizeof(settings.mqttPass), doc["mqttPass"]);
  loadField(settings.mqttBase, sizeof(settings.mqttBase), doc["mqttBase"]);
  settings.mqttIntervalS = doc["mqttIntervalS"] | settings.mqttIntervalS;
  loadField(settings.haPrefix, sizeof(settings.haPrefix), doc["haPrefix"]);
  loadField(settings.deviceName, sizeof(settings.deviceName), doc["deviceName"]);

  settings.adcFullScaleV = doc["adcFullScaleV"] | settings.adcFullScaleV;
  settings.vZero         = doc["vZero"] | settings.vZero;
  settings.vFull         = doc["vFull"] | settings.vFull;
  settings.pFullMPa      = doc["pFullMPa"] | settings.pFullMPa;

  if (settingsSanitize(settings, s_deviceId)) {
    Serial.println(F("[cfg] в config.json были недопустимые значения, часть полей сброшена"));
  }
  Serial.println(F("[cfg] настройки загружены"));
}

bool settingsSave() {
  JsonDocument doc;

  doc["mqttHost"]      = settings.mqttHost;
  doc["mqttPort"]      = settings.mqttPort;
  doc["mqttUser"]      = settings.mqttUser;
  doc["mqttPass"]      = settings.mqttPass;
  doc["mqttBase"]      = settings.mqttBase;
  doc["mqttIntervalS"] = settings.mqttIntervalS;
  doc["haPrefix"]      = settings.haPrefix;
  doc["deviceName"]    = settings.deviceName;

  doc["adcFullScaleV"] = settings.adcFullScaleV;
  doc["vZero"]         = settings.vZero;
  doc["vFull"]         = settings.vFull;
  doc["pFullMPa"]      = settings.pFullMPa;

  File f = LittleFS.open(CONFIG_PATH, "w");
  if (!f) {
    Serial.println(F("[cfg] не смог открыть config.json на запись"));
    return false;
  }

  bool ok = serializeJson(doc, f) > 0;
  f.close();

  Serial.println(ok ? F("[cfg] настройки сохранены") : F("[cfg] запись config.json не удалась"));
  return ok;
}
